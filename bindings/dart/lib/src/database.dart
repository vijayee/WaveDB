// lib/src/database.dart
import 'dart:async';
import 'dart:ffi';
import 'dart:typed_data';
import 'package:ffi/ffi.dart';
import 'native/types.dart';
import 'native/wavedb_bindings.dart';
import 'path.dart';
import 'identifier.dart';
import 'exceptions.dart';
import 'iterator.dart';
import 'object_ops.dart';

/// Configuration options for WaveDB
class WaveDBConfig {
  /// HBTrie chunk size in bytes (default: 4)
  final int? chunkSize;

  /// B+tree node size in bytes (default: 4096)
  final int? btreeNodeSize;

  /// Enable persistent storage (default: true)
  final bool? enablePersist;

  /// LRU cache size in megabytes (default: 50)
  final int? lruMemoryMb;

  /// LRU cache shard count, 0 for auto-scale (default: 64)
  final int? lruShards;

  /// Bnode cache size in megabytes (default: 128)
  final int? bnodeCacheMemoryMb;

  /// Bnode cache shard count (default: 4)
  final int? bnodeCacheShards;

  /// Section cache size (default: 1024)
  final int? storageCacheSize;

  /// Number of worker threads (default: 4)
  final int? workerThreads;

  /// WAL sync mode: 'immediate', 'debounced', 'async' (default: 'debounced')
  final String? walSyncMode;

  /// Debounce window for fsync in ms (default: 250)
  final int? walDebounceMs;

  /// Max WAL file size before sealing (default: 131072)
  final int? walMaxFileSize;

  const WaveDBConfig({
    this.chunkSize,
    this.btreeNodeSize,
    this.enablePersist,
    this.lruMemoryMb,
    this.lruShards,
    this.bnodeCacheMemoryMb,
    this.bnodeCacheShards,
    this.storageCacheSize,
    this.workerThreads,
    this.walSyncMode,
    this.walDebounceMs,
    this.walMaxFileSize,
  });
}

/// Operation type for async dispatch
enum _AsyncOpType { put, get, delete, batch }

/// WaveDB database instance
///
/// Provides both async and sync methods for database operations.
/// Async methods use the C worker pool via promise_t and
/// NativeCallable.listener to bridge C thread callbacks back
/// to the Dart isolate.
class WaveDB {
  Pointer<database_t>? _db;
  final String _path;
  final String _delimiter;
  bool _isClosed = false;

  // NativeCallable listeners for C promise callbacks.
  // These must persist for the lifetime of the database since C holds pointers to them.
  static NativeCallable<Void Function(Pointer<Void>, Pointer<Void>)>? _resolveCallable;
  static NativeCallable<Void Function(Pointer<Void>, Pointer<async_error_t>)>? _rejectCallable;

  // Active completers keyed by request id
  static int _nextRequestId = 1;
  static final Map<int, _PendingOp> _pending = {};
  static int _pendingCount = 0;

  WaveDB(String path, {String delimiter = '/', WaveDBConfig? config})
      : _path = path,
        _delimiter = delimiter {
    if (config != null) {
      _db = WaveDBNative.databaseCreateWithConfig(
        path,
        chunkSize: config.chunkSize,
        btreeNodeSize: config.btreeNodeSize,
        enablePersist: config.enablePersist,
        lruMemoryMb: config.lruMemoryMb,
        lruShards: config.lruShards,
        bnodeCacheMemoryMb: config.bnodeCacheMemoryMb,
        bnodeCacheShards: config.bnodeCacheShards,
        storageCacheSize: config.storageCacheSize,
        workerThreads: config.workerThreads,
        walSyncMode: config.walSyncMode,
        walDebounceMs: config.walDebounceMs,
        walMaxFileSize: config.walMaxFileSize,
      );
    } else {
      _db = WaveDBNative.databaseCreate(path);
    }
    if (_db == nullptr) {
      throw WaveDBException.ioError('database_create', 'Failed to create database at $path');
    }

    _ensureCallbacksRegistered();
  }

  /// Ensure the NativeCallable.listener callbacks are registered.
  /// These are created once and reused across all async operations.
  static void _ensureCallbacksRegistered() {
    if (_resolveCallable != null) return;

    _resolveCallable = NativeCallable<Void Function(Pointer<Void>, Pointer<Void>)>.listener(
      _cResolveCallback,
    );
    _rejectCallable = NativeCallable<Void Function(Pointer<Void>, Pointer<async_error_t>)>.listener(
      _cRejectCallback,
    );
  }

  /// C resolve callback — called from the C worker pool thread.
  /// NativeCallable.listener marshals this to the Dart isolate's event loop.
  static void _cResolveCallback(Pointer<Void> ctx, Pointer<Void> payload) {
    final requestId = ctx.address;
    final pending = _pending.remove(requestId);
    if (pending == null || pending.completer.isCompleted) return;

    _pendingCount--;

    try {
      switch (pending.type) {
        case _AsyncOpType.get:
          final idPtr = payload.cast<identifier_t>();
          if (idPtr == nullptr) {
            pending.completer.complete(null);
          } else {
            try {
              pending.completer.complete(IdentifierConverter.fromNative(idPtr));
            } finally {
              // The value was CONSUME'd before being passed to the callback
              // (yield=1). REFERENCE consumes the yield (protecting the value
              // from other threads), then identifierDestroy decrements the
              // count to release this reference.
              WaveDBNative.identifierReference(idPtr);
              WaveDBNative.identifierDestroy(idPtr);
            }
          }
          break;
        case _AsyncOpType.put:
        case _AsyncOpType.delete:
        case _AsyncOpType.batch:
          // Free int* result from C resolve callback (allocated by C malloc)
          if (payload != nullptr) {
            malloc.free(payload);
          }
          pending.completer.complete(null);
          break;
      }
    } finally {
      // Clean up C-side allocations
      WaveDBNative.promiseDestroy(pending.promisePtr);
      if (pending.batchPtr != null) {
        WaveDBNative.batchDestroy(pending.batchPtr!);
      }
    }
  }

  /// C reject callback — called from the C worker pool thread.
  static void _cRejectCallback(Pointer<Void> ctx, Pointer<async_error_t> error) {
    final requestId = ctx.address;
    final pending = _pending.remove(requestId);
    if (pending == null || pending.completer.isCompleted) return;

    _pendingCount--;

    String errorMsg = 'Operation failed';
    // We can't easily read the error message from the struct without knowing layout,
    // so we just destroy it and report a generic error.
    WaveDBNative.errorDestroy(error);

    // Clean up C-side allocations
    WaveDBNative.promiseDestroy(pending.promisePtr);
    if (pending.batchPtr != null) {
      WaveDBNative.batchDestroy(pending.batchPtr!);
    }

    pending.completer.completeError(WaveDBException.ioError('async', errorMsg));
  }

  /// Dispatch an async operation via the C promise/pool infrastructure
  Future<T> _dispatchAsync<T>(_AsyncOpType type, void Function(Pointer<promise_t>) dispatch, [Pointer<batch_t>? batchPtr]) {
    final completer = Completer<T>();
    final requestId = _nextRequestId++;
    _pendingCount++;

    final ctxPtr = Pointer<Void>.fromAddress(requestId);
    final promisePtr = WaveDBNative.promiseCreate(
      _resolveCallable!.nativeFunction,
      _rejectCallable!.nativeFunction,
      ctxPtr,
    );

    if (promisePtr == nullptr) {
      _pendingCount--;
      completer.completeError(WaveDBException.ioError('async', 'Failed to create promise'));
      return completer.future;
    }

    _pending[requestId] = _PendingOp(type, completer, promisePtr, batchPtr);

    // Dispatch to the C worker pool
    dispatch(promisePtr);

    return completer.future;
  }

  bool get isClosed => _isClosed;
  String get path => _path;
  String get delimiter => _delimiter;

  int get _delimiterCodeUnit => _delimiter.codeUnitAt(0);

  /// Convert a key (String or List) to a delimiter-joined string
  String _keyToString(dynamic key) {
    if (key is String) return key;
    if (key is List) return key.map((e) => e.toString()).join(_delimiter);
    return key.toString();
  }

  void _checkClosed() {
    if (_isClosed || _db == null) {
      throw WaveDBException.databaseClosed();
    }
  }

  // ============================================================
  // ASYNC OPERATIONS
  // ============================================================

  /// Store a value asynchronously via the C worker pool
  Future<void> put(dynamic key, dynamic value) async {
    _checkClosed();
    if (value == null) {
      throw ArgumentError('Value is required for put operation');
    }
    final path = PathConverter.toNative(key, _delimiter);
    final id = IdentifierConverter.toNative(value);

    return _dispatchAsync<void>(_AsyncOpType.put, (promise) {
      WaveDBNative.databasePutAsync(_db!, path, id, promise);
    });
  }

  /// Retrieve a value asynchronously via the C worker pool
  Future<dynamic> get(dynamic key) async {
    _checkClosed();
    final path = PathConverter.toNative(key, _delimiter);

    return _dispatchAsync<dynamic>(_AsyncOpType.get, (promise) {
      WaveDBNative.databaseGetAsync(_db!, path, promise);
    });
  }

  /// Delete a value asynchronously via the C worker pool
  Future<void> del(dynamic key) async {
    _checkClosed();
    final path = PathConverter.toNative(key, _delimiter);

    return _dispatchAsync<void>(_AsyncOpType.delete, (promise) {
      WaveDBNative.databaseDeleteAsync(_db!, path, promise);
    });
  }

  /// Execute multiple operations atomically via the C worker pool
  Future<void> batch(List<Map<String, dynamic>> operations) async {
    _checkClosed();
    final batchPtr = WaveDBNative.batchCreate(operations.length);
    if (batchPtr == nullptr) {
      throw WaveDBException.ioError('batch', 'Failed to create batch');
    }

    try {
      for (final op in operations) {
        final type = op['type'] as String?;
        final key = op['key'];

        if (type == null || key == null) {
          WaveDBNative.batchDestroy(batchPtr);
          throw ArgumentError('Operation must have type and key');
        }

        final path = PathConverter.toNative(key, _delimiter);
        int rc;

        if (type == 'put') {
          final value = op['value'];
          if (value == null) {
            WaveDBNative.pathDestroy(path);
            WaveDBNative.batchDestroy(batchPtr);
            throw ArgumentError('Put operation must have value');
          }
          final id = IdentifierConverter.toNative(value);
          rc = WaveDBNative.batchAddPut(batchPtr, path, id);
          if (rc != 0) {
            WaveDBNative.pathDestroy(path);
            WaveDBNative.identifierDestroy(id);
            WaveDBNative.batchDestroy(batchPtr);
            throw WaveDBException.ioError('batch_add_put', 'return code: $rc');
          }
        } else if (type == 'del') {
          rc = WaveDBNative.batchAddDelete(batchPtr, path);
          if (rc != 0) {
            WaveDBNative.pathDestroy(path);
            WaveDBNative.batchDestroy(batchPtr);
            throw WaveDBException.ioError('batch_add_delete', 'return code: $rc');
          }
        } else {
          WaveDBNative.pathDestroy(path);
          WaveDBNative.batchDestroy(batchPtr);
          throw ArgumentError('Operation type must be "put" or "del"');
        }
      }
    } catch (e) {
      // If batchAddPut/Delete failed and threw, we already cleaned up
      rethrow;
    }

    return _dispatchAsync<void>(_AsyncOpType.batch, (promise) {
      WaveDBNative.databaseWriteBatchAsync(_db!, batchPtr, promise);
    }, batchPtr);
  }

  /// Store a nested object as flattened paths via the C worker pool
  Future<void> putObject(dynamic key, Map<String, dynamic> obj) async {
    _checkClosed();
    final operations = ObjectOps.flattenObject(key, obj, _delimiter);
    return batch(operations);
  }

  /// Retrieve a nested object from paths
  ///
  /// NOTE: This runs synchronously on the current isolate since
  /// scan operations don't have async C API equivalents.
  Future<Map<String, dynamic>?> getObject(dynamic key) async {
    _checkClosed();
    return _getObjectSyncInternal(key);
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
    final keyStr = _keyToString(key);
    final keyPtr = keyStr.toNativeUtf8();
    final valueBytes = IdentifierConverter.toBytes(value);
    final valuePtr = calloc<Uint8>(valueBytes.length);
    try {
      valuePtr.asTypedList(valueBytes.length).setAll(0, valueBytes);
      final rc = WaveDBNative.databasePutSyncRaw(
        _db!, keyPtr.cast(), keyStr.length, _delimiterCodeUnit,
        valuePtr, valueBytes.length,
      );
      if (rc != 0) {
        throw WaveDBException.ioError('put', 'return code: $rc');
      }
    } finally {
      calloc.free(keyPtr);
      calloc.free(valuePtr);
    }
  }

  dynamic _getSyncInternal(dynamic key) {
    final keyStr = _keyToString(key);
    final keyPtr = keyStr.toNativeUtf8();
    final valueOutPtr = calloc<Pointer<Uint8>>();
    final valueLenPtr = calloc<Size>();

    try {
      final rc = WaveDBNative.databaseGetSyncRaw(
        _db!, keyPtr.cast(), keyStr.length, _delimiterCodeUnit,
        valueOutPtr, valueLenPtr,
      );

      if (rc == -2) return null;
      if (rc != 0) throw WaveDBException.ioError('get', 'return code: $rc');

      final valuePtr = valueOutPtr.value;
      final valueLen = valueLenPtr.value;
      if (valuePtr == nullptr || valueLen == 0) return null;

      try {
        final bytes = Uint8List.fromList(valuePtr.asTypedList(valueLen));
        return IdentifierConverter.isPrintableASCII(bytes)
            ? String.fromCharCodes(bytes)
            : bytes;
      } finally {
        WaveDBNative.databaseRawValueFree(valuePtr);
      }
    } finally {
      calloc.free(keyPtr);
      calloc.free(valueOutPtr);
      calloc.free(valueLenPtr);
    }
  }

  void _delSyncInternal(dynamic key) {
    final keyStr = _keyToString(key);
    final keyPtr = keyStr.toNativeUtf8();

    try {
      final rc = WaveDBNative.databaseDeleteSyncRaw(
        _db!, keyPtr.cast(), keyStr.length, _delimiterCodeUnit,
      );
      if (rc != 0) {
        throw WaveDBException.ioError('delete', 'return code: $rc');
      }
    } finally {
      calloc.free(keyPtr);
    }
  }

  void _batchSyncInternal(List<Map<String, dynamic>> operations) {
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
    final startPath = key != null ? PathConverter.toNative(key, _delimiter) : nullptr;

    final iter = WaveDBNative.databaseScanStart(_db!, startPath, nullptr);

    if (iter == nullptr) {
      return null;
    }

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

          if (path == nullptr || value == nullptr) {
            calloc.free(pathPtr);
            calloc.free(valuePtr);
            break;
          }

          try {
            final pathParts = PathConverter.fromNative(path, _delimiter, asArray: true) as List<String>;
            final valueResult = IdentifierConverter.fromNative(value);

            final strippedPathParts = pathParts.map((p) {
              var result = p.trimRight();
              result = result.replaceAll('\x00', '');
              return result;
            }).toList();

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

    return ObjectOps.reconstructObject(entries, basePath);
  }

  // ============================================================
  // STREAMING
  // ============================================================

  /// Create a read stream
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
  ///
  /// Waits for pending async operations to complete, then destroys the database.
  void close() {
    if (_db != null && !_isClosed) {
      // Wait for pending async operations to drain
      // The C database_destroy will also wait for the worker pool to finish
      final maxWaitMs = 5000;
      var waitedMs = 0;
      while (_pendingCount > 0 && waitedMs < maxWaitMs) {
        // Yield to allow callbacks to fire
        // (In practice, NativeCallable.listener callbacks fire on the event loop)
        waitedMs++;
      }

      WaveDBNative.databaseDestroy(_db!);
      _db = null;
      _isClosed = true;
    }
  }
}

/// Pending async operation context
class _PendingOp {
  final _AsyncOpType type;
  final Completer completer;
  final Pointer<promise_t> promisePtr;
  final Pointer<batch_t>? batchPtr;

  _PendingOp(this.type, this.completer, this.promisePtr, [this.batchPtr]);
}