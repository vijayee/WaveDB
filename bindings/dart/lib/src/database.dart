// lib/src/database.dart
import 'dart:async';
import 'dart:convert';
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

  /// Number of worker threads (default: 4)
  final int? workerThreads;

  /// WAL sync mode: 'immediate', 'debounced', 'async' (default: 'debounced')
  final String? walSyncMode;

  /// Debounce window for fsync in ms (default: 250)
  final int? walDebounceMs;

  /// Max WAL file size before sealing (default: 131072)
  final int? walMaxFileSize;

  /// Run in sync-only mode (no concurrency control, no worker pool).
  /// When true, worker threads are forced to 0.
  final bool? syncOnly;

  const WaveDBConfig({
    this.chunkSize,
    this.btreeNodeSize,
    this.enablePersist,
    this.lruMemoryMb,
    this.lruShards,
    this.bnodeCacheMemoryMb,
    this.bnodeCacheShards,
    this.workerThreads,
    this.walSyncMode,
    this.walDebounceMs,
    this.walMaxFileSize,
    this.syncOnly,
  });
}

/// Encryption configuration for WaveDB
class WaveDBEncryption {
  /// Encryption type: 'symmetric' or 'asymmetric'
  final String type;

  /// Symmetric key (32 bytes for AES-256), required for symmetric encryption
  final Uint8List? key;

  /// Asymmetric public key (DER-encoded), required for asymmetric encryption
  final Uint8List? publicKey;

  /// Asymmetric private key (DER-encoded), optional for write-only mode
  final Uint8List? privateKey;

  WaveDBEncryption.symmetric({required Uint8List key})
      : type = 'symmetric', this.key = key, publicKey = null, privateKey = null;

  WaveDBEncryption.asymmetric({required Uint8List publicKey, Uint8List? privateKey})
      : type = 'asymmetric', this.publicKey = publicKey, this.privateKey = privateKey, key = null;
}

/// Return code from C raw sync get operations indicating "not found"
const int _kNotFoundCode = -2;

/// Operation type for async dispatch
enum _AsyncOpType { put, get, delete, batch }

/// WaveDB database instance
///
/// Provides both async and sync methods for database operations.
/// Async methods use the C worker pool via promise_t and
/// NativeCallable.listener to bridge C thread callbacks back
/// to the Dart isolate.
class WaveDB implements Finalizable {
  Pointer<database_t>? _db;
  final String _path;
  final String _delimiter;
  final bool _syncOnly;
  bool _isClosed = false;

  // NativeFinalizer to ensure database_destroy is called if the Dart object
  // is GC'd without an explicit close(). This prevents native resource leaks.
  static final NativeFinalizer _finalizer = NativeFinalizer(
    WaveDBNative.databaseDestroyNative.cast(),
  );

  // NativeCallable listeners for C promise callbacks.
  // These must persist for the lifetime of the database since C holds pointers to them.
  static NativeCallable<Void Function(Pointer<Void>, Pointer<Void>)>? _resolveCallable;
  static NativeCallable<Void Function(Pointer<Void>, Pointer<async_error_t>)>? _rejectCallable;

  // Active completers keyed by request id
  static int _nextRequestId = 1;
  static final Map<int, _PendingOp> _pending = {};

  // Unique instance id to scope pending ops
  final int _instanceId;
  static int _nextInstanceId = 1;

  WaveDB(String path, {String delimiter = '/', WaveDBConfig? config, WaveDBEncryption? encryption})
      : _path = path,
        _delimiter = delimiter,
        _syncOnly = config?.syncOnly ?? false,
        _instanceId = _nextInstanceId++ {
    if (delimiter.length != 1) {
      throw ArgumentError('Delimiter must be a single character, got: "$delimiter"');
    }
    if (encryption != null) {
      _db = _createEncrypted(path, encryption, config);
    } else if (config != null) {
      _db = WaveDBNative.databaseCreateWithConfig(
        path,
        chunkSize: config.chunkSize,
        btreeNodeSize: config.btreeNodeSize,
        enablePersist: config.enablePersist,
        lruMemoryMb: config.lruMemoryMb,
        lruShards: config.lruShards,
        bnodeCacheMemoryMb: config.bnodeCacheMemoryMb,
        bnodeCacheShards: config.bnodeCacheShards,
        workerThreads: config.workerThreads,
        walSyncMode: config.walSyncMode,
        walDebounceMs: config.walDebounceMs,
        walMaxFileSize: config.walMaxFileSize,
        syncOnly: config.syncOnly,
      );
    } else {
      _db = WaveDBNative.databaseCreate(path);
    }
    if (_db == nullptr) {
      throw WaveDBException.ioError('database_create', 'Failed to create database at $path');
    }

    _finalizer.attach(this, _db!.cast(), detach: this);
    _ensureCallbacksRegistered();
  }

  /// Create an encrypted database using the FFI encrypted config API
  static Pointer<database_t> _createEncrypted(String path, WaveDBEncryption encryption, WaveDBConfig? config) {
    // Map encryption type string to C enum value
    final encryptionType = {'symmetric': 1, 'asymmetric': 2}[encryption.type];
    if (encryptionType == null) {
      throw ArgumentError('Invalid encryption type: ${encryption.type}. Must be "symmetric" or "asymmetric"');
    }

    // Allocate native key pointers as needed
    Pointer<Uint8>? symKeyPtr;
    Pointer<Uint8>? asymPubKeyPtr;
    Pointer<Uint8>? asymPrivKeyPtr;
    int? symKeyLen;
    int? asymPubKeyLen;
    int? asymPrivKeyLen;

    try {
      if (encryption.key != null) {
        symKeyLen = encryption.key!.length;
        symKeyPtr = calloc<Uint8>(symKeyLen);
        symKeyPtr!.asTypedList(symKeyLen).setAll(0, encryption.key!);
      }
      if (encryption.publicKey != null) {
        asymPubKeyLen = encryption.publicKey!.length;
        asymPubKeyPtr = calloc<Uint8>(asymPubKeyLen);
        asymPubKeyPtr!.asTypedList(asymPubKeyLen).setAll(0, encryption.publicKey!);
      }
      if (encryption.privateKey != null) {
        asymPrivKeyLen = encryption.privateKey!.length;
        asymPrivKeyPtr = calloc<Uint8>(asymPrivKeyLen);
        asymPrivKeyPtr!.asTypedList(asymPrivKeyLen).setAll(0, encryption.privateKey!);
      }

      return WaveDBNative.databaseCreateEncrypted(
        path,
        encryptionType: encryptionType,
        symmetricKey: symKeyPtr,
        symmetricKeyLength: symKeyLen,
        asymmetricPublicKey: asymPubKeyPtr,
        asymmetricPublicKeyLength: asymPubKeyLen,
        asymmetricPrivateKey: asymPrivKeyPtr,
        asymmetricPrivateKeyLength: asymPrivKeyLen,
        chunkSize: config?.chunkSize,
        btreeNodeSize: config?.btreeNodeSize,
        enablePersist: config?.enablePersist,
        lruMemoryMb: config?.lruMemoryMb,
        lruShards: config?.lruShards,
        bnodeCacheMemoryMb: config?.bnodeCacheMemoryMb,
        bnodeCacheShards: config?.bnodeCacheShards,
        workerThreads: config?.workerThreads,
        walSyncMode: config?.walSyncMode,
        walDebounceMs: config?.walDebounceMs,
        walMaxFileSize: config?.walMaxFileSize,
        syncOnly: config?.syncOnly,
      );
    } finally {
      if (symKeyPtr != null) calloc.free(symKeyPtr);
      if (asymPubKeyPtr != null) calloc.free(asymPubKeyPtr);
      if (asymPrivKeyPtr != null) calloc.free(asymPrivKeyPtr);
    }
  }

  /// Ensure the NativeCallable.listener callbacks are registered.
  /// These are created once and reused across all async operations.
  static void _ensureCallbacksRegistered() {
    if (_resolveCallable != null && _rejectCallable != null) return;

    if (_resolveCallable == null) {
      _resolveCallable = NativeCallable<Void Function(Pointer<Void>, Pointer<Void>)>.listener(
        _cResolveCallback,
      );
    }
    if (_rejectCallable == null) {
      _rejectCallable = NativeCallable<Void Function(Pointer<Void>, Pointer<async_error_t>)>.listener(
        _cRejectCallback,
      );
    }
  }

  /// C resolve callback — called from the C worker pool thread.
  /// NativeCallable.listener marshals this to the Dart isolate's event loop.
  static void _cResolveCallback(Pointer<Void> ctx, Pointer<Void> payload) {
    final requestId = ctx.address;
    final pending = _pending.remove(requestId);

    if (pending == null) {
      // Orphaned resolve — no matching request (e.g., close() cancelled the op
      // but C callback still fired). We can't free identifier_t* payloads
      // correctly without knowing the op type (identifierDestroy vs malloc.free),
      // so we accept a small leak for this rare race.
      return;
    }

    try {
      if (pending.completer.isCompleted) return;

      switch (pending.type) {
        case _AsyncOpType.get:
          final idPtr = payload.cast<identifier_t>();
          if (idPtr == nullptr) {
            pending.completer.complete(null);
          } else {
            try {
              pending.completer.complete(IdentifierConverter.fromNative(idPtr));
            } catch (e) {
              pending.completer.completeError(e);
            } finally {
              WaveDBNative.identifierReference(idPtr);
              WaveDBNative.identifierDestroy(idPtr);
            }
          }
          break;
        case _AsyncOpType.put:
        case _AsyncOpType.delete:
        case _AsyncOpType.batch:
          if (payload != nullptr) {
            malloc.free(payload);
          }
          pending.completer.complete(null);
          break;
      }
    } catch (e) {
      if (!pending.completer.isCompleted) {
        pending.completer.completeError(e);
      }
    } finally {
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

    // Always destroy the C error object
    String errorMsg = 'Operation failed';
    if (error != nullptr) {
      final msgPtr = WaveDBNative.errorGetMessage(error);
      if (msgPtr != nullptr) {
        errorMsg = msgPtr.toDartString();
      }
      WaveDBNative.errorDestroy(error);
    }

    if (pending == null) return;

    try {
      if (!pending.completer.isCompleted) {
        pending.completer.completeError(WaveDBException.ioError('async', errorMsg));
      }
    } finally {
      WaveDBNative.promiseDestroy(pending.promisePtr);
      if (pending.batchPtr != null) {
        WaveDBNative.batchDestroy(pending.batchPtr!);
      }
    }
  }

  /// Dispatch an async operation via the C promise/pool infrastructure
  Future<T> _dispatchAsync<T>(_AsyncOpType type, void Function(Pointer<promise_t>) dispatch, [Pointer<batch_t>? batchPtr]) {
    final completer = Completer<T>();
    final requestId = _nextRequestId++;

    final ctxPtr = Pointer<Void>.fromAddress(requestId);
    final promisePtr = WaveDBNative.promiseCreate(
      _resolveCallable!.nativeFunction,
      _rejectCallable!.nativeFunction,
      ctxPtr,
    );

    if (promisePtr == nullptr) {
        completer.completeError(WaveDBException.ioError('async', 'Failed to create promise'));
      return completer.future;
    }

    _pending[requestId] = _PendingOp(_instanceId, type, completer, promisePtr, batchPtr);

    // Dispatch to the C worker pool
    try {
      dispatch(promisePtr);
    } catch (e) {
      _pending.remove(requestId);
      WaveDBNative.promiseDestroy(promisePtr);
      if (batchPtr != null) {
        WaveDBNative.batchDestroy(batchPtr);
      }
      completer.completeError(e);
    }

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

  /// Convert a key string to native UTF-8 and return (pointer, byte length).
  /// Uses utf8.encode to get the correct byte length (not UTF-16 code units).
  (Pointer<Utf8>, int) _keyToNative(String keyStr) {
    final keyPtr = keyStr.toNativeUtf8();
    final keyByteLen = utf8.encode(keyStr).length;
    return (keyPtr, keyByteLen);
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
    if (_syncOnly) {
      putSync(key, value);
      return;
    }
    if (value == null) {
      throw ArgumentError('Value is required for put operation');
    }
    final keyStr = _keyToString(key);
    final (keyPtr, keyByteLen) = _keyToNative(keyStr);
    final valueBytes = IdentifierConverter.toBytes(value);
    final valuePtr = calloc<Uint8>(valueBytes.length);
    valuePtr.asTypedList(valueBytes.length).setAll(0, valueBytes);

    try {
      return _dispatchAsync<void>(_AsyncOpType.put, (promise) {
        final rc = WaveDBNative.databasePutRaw(
          _db!, keyPtr.cast(), keyByteLen, _delimiterCodeUnit,
          valuePtr, valueBytes.length, promise,
        );
        if (rc != 0) {
          throw WaveDBException.ioError('put_raw', 'return code: $rc');
        }
      });
    } finally {
      // database_put_raw copies data internally before dispatching
      calloc.free(keyPtr);
      calloc.free(valuePtr);
    }
  }

  /// Retrieve a value asynchronously via the C worker pool
  Future<dynamic> get(dynamic key) async {
    _checkClosed();
    if (_syncOnly) {
      return getSync(key);
    }
    final keyStr = _keyToString(key);
    final (keyPtr, keyByteLen) = _keyToNative(keyStr);

    try {
      return _dispatchAsync<dynamic>(_AsyncOpType.get, (promise) {
        final rc = WaveDBNative.databaseGetRaw(
          _db!, keyPtr.cast(), keyByteLen, _delimiterCodeUnit, promise,
        );
        if (rc != 0) {
          throw WaveDBException.ioError('get_raw', 'return code: $rc');
        }
      });
    } finally {
      // database_get_raw copies key internally before dispatching
      calloc.free(keyPtr);
    }
  }

  /// Delete a value asynchronously via the C worker pool
  Future<void> del(dynamic key) async {
    _checkClosed();
    if (_syncOnly) {
      delSync(key);
      return;
    }
    final keyStr = _keyToString(key);
    final (keyPtr, keyByteLen) = _keyToNative(keyStr);

    try {
      return _dispatchAsync<void>(_AsyncOpType.delete, (promise) {
        final rc = WaveDBNative.databaseDeleteRaw(
          _db!, keyPtr.cast(), keyByteLen, _delimiterCodeUnit, promise,
        );
        if (rc != 0) {
          throw WaveDBException.ioError('delete_raw', 'return code: $rc');
        }
      });
    } finally {
      // database_delete_raw copies key internally before dispatching
      calloc.free(keyPtr);
    }
  }

  /// Execute multiple operations atomically via the C worker pool
  Future<void> batch(List<Map<String, dynamic>> operations) async {
    _checkClosed();
    if (operations.isEmpty) return;
    if (_syncOnly) {
      batchSync(operations);
      return;
    }

    final opsPtr = calloc<RawOp>(operations.length);
    final keyPtrs = <Pointer<Uint8>>[];
    final valuePtrs = <Pointer<Uint8>>[];

    try {
      for (int i = 0; i < operations.length; i++) {
        final op = operations[i];
        final type = op['type'] as String?;
        final key = op['key'];

        if (type == null || key == null) {
          throw ArgumentError('Operation must have type and key');
        }

        final keyStr = _keyToString(key);
        final (keyPtr, keyByteLen) = _keyToNative(keyStr);
        keyPtrs.add(keyPtr.cast());
        opsPtr[i].key = keyPtr.cast();
        opsPtr[i].keyLen = keyByteLen;

        if (type == 'put') {
          final value = op['value'];
          if (value == null) {
            throw ArgumentError('Put operation must have value');
          }
          opsPtr[i].type = 0;
          final valueBytes = IdentifierConverter.toBytes(value);
          final valuePtr = calloc<Uint8>(valueBytes.length);
          valuePtr.asTypedList(valueBytes.length).setAll(0, valueBytes);
          valuePtrs.add(valuePtr);
          opsPtr[i].value = valuePtr;
          opsPtr[i].valueLen = valueBytes.length;
        } else if (type == 'del') {
          opsPtr[i].type = 1;
          opsPtr[i].value = nullptr;
          opsPtr[i].valueLen = 0;
        } else {
          throw ArgumentError('Operation type must be "put" or "del"');
        }
      }

      return _dispatchAsync<void>(_AsyncOpType.batch, (promise) {
        final rc = WaveDBNative.databaseBatchRaw(
          _db!, _delimiterCodeUnit, opsPtr, operations.length, promise,
        );
        if (rc != 0) {
          throw WaveDBException.ioError('batch_raw', 'return code: $rc');
        }
      });
    } finally {
      // database_batch_raw copies all data internally before dispatching
      for (final p in keyPtrs) calloc.free(p);
      for (final p in valuePtrs) calloc.free(p);
      calloc.free(opsPtr);
    }
  }

  /// Store a nested object as flattened paths via the C worker pool
  Future<void> putObject(dynamic key, Map<String, dynamic> obj) async {
    _checkClosed();
    final operations = ObjectOps.flattenObject(key, obj, _delimiter);
    if (_syncOnly) {
      batchSync(operations);
      return;
    }
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
    final (keyPtr, keyByteLen) = _keyToNative(keyStr);
    final valueBytes = IdentifierConverter.toBytes(value);
    final valuePtr = calloc<Uint8>(valueBytes.length);
    try {
      valuePtr.asTypedList(valueBytes.length).setAll(0, valueBytes);
      final rc = WaveDBNative.databasePutSyncRaw(
        _db!, keyPtr.cast(), keyByteLen, _delimiterCodeUnit,
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
    final (keyPtr, keyByteLen) = _keyToNative(keyStr);
    final valueOutPtr = calloc<Pointer<Uint8>>();
    final valueLenPtr = calloc<Size>();

    try {
      final rc = WaveDBNative.databaseGetSyncRaw(
        _db!, keyPtr.cast(), keyByteLen, _delimiterCodeUnit,
        valueOutPtr, valueLenPtr,
      );

      if (rc == _kNotFoundCode) return null;
      if (rc != 0) throw WaveDBException.ioError('get', 'return code: $rc');

      final valuePtr = valueOutPtr.value;
      final valueLen = valueLenPtr.value;
      if (valuePtr == nullptr || valueLen == 0) return null;

      try {
        // databaseGetSyncRaw returns exact-length data, no chunk padding
        final bytes = valuePtr.asTypedList(valueLen);
        final bytesUint8 = Uint8List.fromList(bytes);
        return IdentifierConverter.isPrintableASCII(bytesUint8)
            ? utf8.decode(bytesUint8, allowMalformed: true)
            : bytesUint8;
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
    final (keyPtr, keyByteLen) = _keyToNative(keyStr);

    try {
      final rc = WaveDBNative.databaseDeleteSyncRaw(
        _db!, keyPtr.cast(), keyByteLen, _delimiterCodeUnit,
      );
      if (rc != 0) {
        throw WaveDBException.ioError('delete', 'return code: $rc');
      }
    } finally {
      calloc.free(keyPtr);
    }
  }

  void _batchSyncInternal(List<Map<String, dynamic>> operations) {
    if (operations.isEmpty) return;

    final opsPtr = calloc<RawOp>(operations.length);
    final keyPtrs = <Pointer<Uint8>>[];
    final valuePtrs = <Pointer<Uint8>>[];

    try {
      for (int i = 0; i < operations.length; i++) {
        final op = operations[i];
        final type = op['type'] as String?;
        final key = op['key'];

        if (type == null || key == null) {
          throw ArgumentError('Operation must have type and key');
        }

        final keyStr = _keyToString(key);
        final (keyPtr, keyByteLen) = _keyToNative(keyStr);
        keyPtrs.add(keyPtr.cast());
        opsPtr[i].key = keyPtr.cast();
        opsPtr[i].keyLen = keyByteLen;

        if (type == 'put') {
          final value = op['value'];
          if (value == null) {
            throw ArgumentError('Put operation must have value');
          }
          opsPtr[i].type = 0;
          final valueBytes = IdentifierConverter.toBytes(value);
          final valuePtr = calloc<Uint8>(valueBytes.length);
          valuePtr.asTypedList(valueBytes.length).setAll(0, valueBytes);
          valuePtrs.add(valuePtr);
          opsPtr[i].value = valuePtr;
          opsPtr[i].valueLen = valueBytes.length;
        } else if (type == 'del') {
          opsPtr[i].type = 1;
          opsPtr[i].value = nullptr;
          opsPtr[i].valueLen = 0;
        } else {
          throw ArgumentError('Operation type must be "put" or "del"');
        }
      }

      final rc = WaveDBNative.databaseBatchSyncRaw(
        _db!, _delimiterCodeUnit, opsPtr, operations.length,
      );
      if (rc != 0) {
        throw WaveDBException.ioError('batch', 'return code: $rc');
      }
    } finally {
      for (final p in keyPtrs) calloc.free(p);
      for (final p in valuePtrs) calloc.free(p);
      calloc.free(opsPtr);
    }
  }

  void _putObjectSyncInternal(dynamic key, Map<String, dynamic> obj) {
    final operations = ObjectOps.flattenObject(key, obj, _delimiter);
    _batchSyncInternal(operations);
  }

  Map<String, dynamic>? _getObjectSyncInternal(dynamic key) {
    final keyStr = _keyToString(key);
    final (prefixPtr, keyByteLen) = _keyToNative(keyStr);
    final resultsPtr = calloc<Pointer<RawResult>>();
    final countPtr = calloc<Size>();

    try {
      final rc = WaveDBNative.databaseScanSyncRaw(
        _db!, prefixPtr.cast(), keyByteLen, _delimiterCodeUnit,
        resultsPtr, countPtr,
      );
      if (rc != 0) throw WaveDBException.ioError('scan', 'return code: $rc');

      final results = resultsPtr.value;
      final count = countPtr.value;

      if (count == 0 || results == nullptr) return null;

      final basePath = key is List
          ? key.map((e) => e.toString()).toList()
          : keyStr.split(_delimiter).where((s) => s.isNotEmpty).toList();

      final entries = <MapEntry<List<String>, dynamic>>[];

      try {
        for (int i = 0; i < count; i++) {
          final rawKey = results[i].key;
          final keyLen = results[i].keyLen;
          final rawVal = results[i].value;
          final valLen = results[i].valueLen;

          if (rawKey == nullptr) continue;
          final keyData = rawKey.asTypedList(keyLen);
          final keyStrResult = utf8.decode(keyData, allowMalformed: true)
              .replaceAll('\x00', '');
          final pathParts = keyStrResult.split(_delimiter)
              .where((s) => s.isNotEmpty)
              .toList();

          dynamic valueResult;
          if (rawVal != nullptr && valLen > 0) {
            // databaseScanSyncRaw returns exact-length data, no chunk padding
            final valUint8 = Uint8List.fromList(rawVal.asTypedList(valLen));
            valueResult = IdentifierConverter.isPrintableASCII(valUint8)
                ? utf8.decode(valUint8, allowMalformed: true)
                : valUint8;
          } else {
            valueResult = '';
          }

          entries.add(MapEntry(pathParts, valueResult));
        }
      } finally {
        WaveDBNative.databaseRawResultsFree(results, count);
      }

      return ObjectOps.reconstructObject(entries, basePath);
    } finally {
      calloc.free(prefixPtr);
      calloc.free(resultsPtr);
      calloc.free(countPtr);
    }
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
    if (reverse) {
      throw UnimplementedError('Reverse scanning is not yet supported');
    }

    return WaveDBIterator(
      db: _db!,
      delimiter: _delimiter,
      startPath: start,
      endPath: end,
      reverse: reverse,
      keys: keys,
      values: values,
      isClosed: () => _isClosed,
    ).stream;
  }

  // ============================================================
  // LIFECYCLE
  // ============================================================

  /// Close the database
  ///
  /// Destroys the database. The C database_destroy internally stops the
  /// timing wheel, shuts down the worker pool, and joins all worker threads,
  /// so pending async operations will complete or abort before teardown.
  void close() {
    if (_db != null && !_isClosed) {
      _finalizer.detach(this);

      // Cancel only this instance's pending async operations
      final keysToRemove = <int>[];
      for (final entry in _pending.entries) {
        if (entry.value.instanceId == _instanceId) {
          if (!entry.value.completer.isCompleted) {
            WaveDBNative.promiseDestroy(entry.value.promisePtr);
            if (entry.value.batchPtr != null) {
              WaveDBNative.batchDestroy(entry.value.batchPtr!);
            }
            entry.value.completer.completeError(WaveDBException.databaseClosed());
          }
          keysToRemove.add(entry.key);
        }
      }
      for (final key in keysToRemove) {
        _pending.remove(key);
      }

      WaveDBNative.databaseDestroy(_db!);
      _db = null;
      _isClosed = true;
    }
  }
}

/// Pending async operation context
class _PendingOp {
  final int instanceId;
  final _AsyncOpType type;
  final Completer completer;
  final Pointer<promise_t> promisePtr;
  final Pointer<batch_t>? batchPtr;

  _PendingOp(this.instanceId, this.type, this.completer, this.promisePtr, [this.batchPtr]);
}