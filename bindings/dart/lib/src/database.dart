// lib/src/database.dart
import 'dart:async';
import 'dart:ffi';
import 'package:ffi/ffi.dart';
import 'native/types.dart';
import 'native/wavedb_bindings.dart';
import 'path.dart';
import 'identifier.dart';
import 'exceptions.dart';
import 'iterator.dart';
import 'object_ops.dart';

/// WaveDB database instance
///
/// Provides both async and sync methods for database operations.
/// Async methods currently execute synchronously (Dart FFI limitation).
class WaveDB {
  Pointer<database_t>? _db;
  final String _path;
  final String _delimiter;
  bool _isClosed = false;

  /// Create or open a database at [path]
  ///
  /// [path] - Directory path for the database
  /// [delimiter] - Path delimiter for string keys (default: '/')
  WaveDB(String path, {String delimiter = '/'})
      : _path = path,
        _delimiter = delimiter {
    _db = WaveDBNative.databaseCreate(path);
    if (_db == nullptr) {
      throw WaveDBException.ioError('database_create', 'Failed to create database at $path');
    }
  }

  /// Check if database is closed
  bool get isClosed => _isClosed;

  /// Get the database path
  String get path => _path;

  /// Get the delimiter
  String get delimiter => _delimiter;

  void _checkClosed() {
    if (_isClosed || _db == null) {
      throw WaveDBException.databaseClosed();
    }
  }

  // ============================================================
  // ASYNC OPERATIONS
  // ============================================================

  /// Store a value asynchronously
  ///
  /// [key] - Key path (string with delimiter or list of identifiers)
  /// [value] - Value to store (string or Uint8List)
  Future<void> put(dynamic key, dynamic value) async {
    _checkClosed();
    if (value == null) {
      throw ArgumentError('Value is required for put operation');
    }
    return _runSync(() => _putSyncInternal(key, value));
  }

  /// Retrieve a value asynchronously
  ///
  /// [key] - Key path (string with delimiter or list of identifiers)
  /// Returns the value as String or Uint8List, or null if not found
  Future<dynamic> get(dynamic key) async {
    _checkClosed();
    return _runSync(() => _getSyncInternal(key));
  }

  /// Delete a value asynchronously
  ///
  /// [key] - Key path (string with delimiter or list of identifiers)
  Future<void> del(dynamic key) async {
    _checkClosed();
    return _runSync(() => _delSyncInternal(key));
  }

  /// Execute multiple operations atomically
  ///
  /// [operations] - List of operations
  /// Each operation: {'type': 'put'|'del', 'key': ..., 'value': ...}
  Future<void> batch(List<Map<String, dynamic>> operations) async {
    _checkClosed();
    return _runSync(() => _batchSyncInternal(operations));
  }

  /// Store a nested object as flattened paths
  ///
  /// [key] - Base key path (optional)
  /// [obj] - Object to flatten and store
  Future<void> putObject(dynamic key, Map<String, dynamic> obj) async {
    _checkClosed();
    return _runSync(() => _putObjectSyncInternal(key, obj));
  }

  /// Retrieve a nested object from paths
  ///
  /// [key] - Base key path to retrieve
  /// Returns the reconstructed object
  Future<Map<String, dynamic>?> getObject(dynamic key) async {
    _checkClosed();
    return _runSync(() => _getObjectSyncInternal(key));
  }

  // ============================================================
  // SYNC OPERATIONS
  // ============================================================

  /// Store a value synchronously (blocks current isolate)
  void putSync(dynamic key, dynamic value) {
    _checkClosed();
    _putSyncInternal(key, value);
  }

  /// Retrieve a value synchronously (blocks current isolate)
  dynamic getSync(dynamic key) {
    _checkClosed();
    return _getSyncInternal(key);
  }

  /// Delete a value synchronously (blocks current isolate)
  void delSync(dynamic key) {
    _checkClosed();
    _delSyncInternal(key);
  }

  /// Execute multiple operations synchronously (blocks current isolate)
  void batchSync(List<Map<String, dynamic>> operations) {
    _checkClosed();
    _batchSyncInternal(operations);
  }

  /// Store a nested object synchronously (blocks current isolate)
  void putObjectSync(dynamic key, Map<String, dynamic> obj) {
    _checkClosed();
    _putObjectSyncInternal(key, obj);
  }

  /// Retrieve a nested object synchronously (blocks current isolate)
  Map<String, dynamic>? getObjectSync(dynamic key) {
    _checkClosed();
    return _getObjectSyncInternal(key);
  }

  // ============================================================
  // INTERNAL SYNC IMPLEMENTATIONS
  // ============================================================

  void _putSyncInternal(dynamic key, dynamic value) {
    final path = PathConverter.toNative(key, _delimiter);
    final id = IdentifierConverter.toNative(value);

    // database_put_sync takes ownership of both path and value
    // They will be destroyed internally - DO NOT call destroy after
    final rc = WaveDBNative.databasePutSync(_db!, path, id);
    if (rc != 0) {
      throw WaveDBException.ioError('put', 'return code: $rc');
    }
  }

  dynamic _getSyncInternal(dynamic key) {
    final path = PathConverter.toNative(key, _delimiter);
    final resultPtr = calloc<Pointer<identifier_t>>();

    try {
      // database_get_sync takes ownership of path
      final rc = WaveDBNative.databaseGetSync(_db!, path, resultPtr);

      if (rc == -2) {
        // Not found
        return null;
      }

      if (rc != 0) {
        throw WaveDBException.ioError('get', 'return code: $rc');
      }

      final result = resultPtr.value;
      if (result == nullptr) {
        return null;
      }

      try {
        return IdentifierConverter.fromNative(result);
      } finally {
        // result must be destroyed by caller
        WaveDBNative.identifierDestroy(result);
      }
    } finally {
      calloc.free(resultPtr);
    }
  }

  void _delSyncInternal(dynamic key) {
    final path = PathConverter.toNative(key, _delimiter);

    // database_delete_sync takes ownership of path
    // It will be destroyed internally - DO NOT call destroy after
    final rc = WaveDBNative.databaseDeleteSync(_db!, path);
    if (rc != 0) {
      throw WaveDBException.ioError('delete', 'return code: $rc');
    }
  }

  void _batchSyncInternal(List<Map<String, dynamic>> operations) {
    // TODO: Implement atomic batch when C API supports transactions
    // Currently operations are executed individually - failures may leave partial changes
    for (final op in operations) {
      final type = op['type'] as String?;
      final key = op['key'];

      if (type == null || key == null) {
        throw ArgumentError('Operation must have type and key');
      }

      if (type == 'put') {
        final value = op['value'];
        if (value == null) {
          throw ArgumentError('Put operation must have value');
        }
        _putSyncInternal(key, value);
      } else if (type == 'del') {
        _delSyncInternal(key);
      } else {
        throw ArgumentError('Operation type must be "put" or "del"');
      }
    }
  }

  void _putObjectSyncInternal(dynamic key, Map<String, dynamic> obj) {
    final operations = ObjectOps.flattenObject(key, obj, _delimiter);
    _batchSyncInternal(operations);
  }

  Map<String, dynamic>? _getObjectSyncInternal(dynamic key) {
    // Create start path from key
    final startPath = key != null ? PathConverter.toNative(key, _delimiter) : nullptr;

    // Start scan
    final iter = WaveDBNative.databaseScanStart(_db!, startPath, nullptr);

    // Note: databaseScanStart takes ownership of startPath, so we don't free it here

    if (iter == nullptr) {
      return null;
    }

    // Collect entries and reconstruct object
    final entries = <MapEntry<List<String>, dynamic>>[];
    final basePath = key != null
        ? (key is List ? key.map((e) => e.toString()).toList() : key.toString().split(_delimiter).where((s) => s.isNotEmpty).toList())
        : <String>[];

    try {
      while (true) {
        final pathPtr = calloc<Pointer<path_t>>();
        final valuePtr = calloc<Pointer<identifier_t>>();

        try {
          final rc = WaveDBNative.databaseScanNext(iter, pathPtr, valuePtr);
          if (rc != 0) break;

          final path = pathPtr.value;
          final value = valuePtr.value;

          // Both path and value must be valid
          if (path == nullptr || value == nullptr) {
            calloc.free(pathPtr);
            calloc.free(valuePtr);
            break;
          }

          try {
            // Convert path to list of strings
            final pathParts = PathConverter.fromNative(path, _delimiter, asArray: true) as List<String>;
            final valueResult = IdentifierConverter.fromNative(value);

            // Strip trailing whitespace/nulls/padding from path parts
            // (The iterator reconstructs identifiers from chunks without knowing original length)
            final strippedPathParts = pathParts.map((p) {
              var result = p.trimRight();
              result = result.replaceAll('\x00', '');
              return result;
            }).toList();

            // Filter: only include entries that are under the base path
            if (strippedPathParts.length >= basePath.length) {
              bool matches = true;
              for (int i = 0; i < basePath.length && matches; i++) {
                if (strippedPathParts[i] != basePath[i]) {
                  matches = false;
                }
              }
              if (matches) {
                entries.add(MapEntry(strippedPathParts, valueResult));
              }
            }
          } finally {
            WaveDBNative.pathDestroy(path);
            WaveDBNative.identifierDestroy(value);
          }
        } finally {
          calloc.free(pathPtr);
          calloc.free(valuePtr);
        }
      }
    } finally {
      WaveDBNative.databaseScanEnd(iter);
    }

    // Reconstruct object from entries
    return ObjectOps.reconstructObject(entries, basePath);
  }

  // ============================================================
  // STREAMING
  // ============================================================

  /// Create a read stream
  ///
  /// [start] - Start path (inclusive, optional)
  /// [end] - End path (exclusive, optional)
  /// [reverse] - Scan in reverse order (default: false)
  /// [keys] - Include keys in results (default: true)
  /// [values] - Include values in results (default: true)
  ///
  /// NOTE: This method will throw NOT_SUPPORTED if the scan API is not available.
  /// The database_scan_* functions are not yet implemented in the C API.
  Stream<KeyValue> createReadStream({
    dynamic start,
    dynamic end,
    bool reverse = false,
    bool keys = true,
    bool values = true,
  }) {
    _checkClosed();

    return WaveDBIterator(
      db: _db!,
      delimiter: _delimiter,
      startPath: start,
      endPath: end,
      reverse: reverse,
      keys: keys,
      values: values,
    ).stream;
  }

  // ============================================================
  // LIFECYCLE
  // ============================================================

  /// Close the database
  void close() {
    if (_db != null && !_isClosed) {
      WaveDBNative.databaseDestroy(_db!);
      _db = null;
      _isClosed = true;
    }
  }

  /// Execute operation synchronously
  /// TODO: Use Isolate.run for true async when Dart FFI supports it
  Future<T> _runSync<T>(T Function() operation) async {
    return operation();
  }
}
