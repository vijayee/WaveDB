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
    try {
      final id = IdentifierConverter.toNative(value);
      try {
        final rc = WaveDBNative.databasePutSync(_db!, path, id);
        if (rc != 0) {
          throw WaveDBException.ioError('put', 'return code: $rc');
        }
      } finally {
        // databasePutSync takes ownership of id
        WaveDBNative.identifierDestroy(id);
      }
    } finally {
      // databasePutSync takes ownership of path
      WaveDBNative.pathDestroy(path);
    }
  }

  dynamic _getSyncInternal(dynamic key) {
    final path = PathConverter.toNative(key, _delimiter);
    final resultPtr = calloc<Pointer<identifier_t>>();

    try {
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
        WaveDBNative.identifierDestroy(result);
      }
    } finally {
      calloc.free(resultPtr);
      // databaseGetSync takes ownership of path
      WaveDBNative.pathDestroy(path);
    }
  }

  void _delSyncInternal(dynamic key) {
    final path = PathConverter.toNative(key, _delimiter);
    try {
      final rc = WaveDBNative.databaseDeleteSync(_db!, path);
      if (rc != 0) {
        throw WaveDBException.ioError('delete', 'return code: $rc');
      }
    } finally {
      // databaseDeleteSync takes ownership of path
      WaveDBNative.pathDestroy(path);
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
    // TODO: Requires database_scan API
    throw WaveDBException.notSupported('getObject');
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
