// lib/src/subtree.dart
import 'dart:async';
import 'dart:convert';
import 'dart:ffi';
import 'dart:typed_data';
import 'package:ffi/ffi.dart';
import 'native/types.dart';
import 'native/wavedb_bindings.dart';
import 'identifier.dart';
import 'exceptions.dart';

/// Return code from C raw sync get operations indicating "not found"
const int _kNotFoundCode = -2;

/// Operation type for async subtree dispatch
enum _SubtreeAsyncOpType { put, get, delete }

/// Pending async operation context for Subtree
class _SubtreePendingOp {
  final int instanceId;
  final _SubtreeAsyncOpType type;
  final Completer completer;
  final Pointer<promise_t> promisePtr;

  _SubtreePendingOp(this.instanceId, this.type, this.completer, this.promisePtr);
}

/// A scoped view into a WaveDB database with prefix isolation.
///
/// All operations on a subtree automatically prepend the prefix to keys,
/// providing namespace isolation. The subtree holds a reference on the
/// database, keeping it alive until all subtrees are closed.
/// Closing a subtree releases that reference.
///
/// Subtrees are created via [WaveDB.openSubtree] and support both
/// sync and async operations scoped to the prefix namespace.
class Subtree implements Finalizable {
  Pointer<database_subtree_t>? _st;
  final String _delimiter;
  final int _delimiterCode;
  bool _closed = false;
  final int _instanceId;
  static int _nextInstanceId = 1;

  // NativeCallable listeners for C promise callbacks (resolve/reject).
  static NativeCallable<Void Function(Pointer<Void>, Pointer<Void>)>? _resolveCallable;
  static NativeCallable<Void Function(Pointer<Void>, Pointer<async_error_t>)>? _rejectCallable;

  // Active completers keyed by request id
  static int _nextRequestId = 1;
  static final Map<int, _SubtreePendingOp> _pending = {};

  /// NativeFinalizer to ensure database_subtree_close is called if the Dart
  /// object is GC'd without an explicit close(). This prevents native resource leaks.
  static final NativeFinalizer _finalizer = NativeFinalizer(
    WaveDBNative.databaseSubtreeCloseNative.cast(),
  );

  /// Internal constructor used by [WaveDB.openSubtree].
  /// Do not call directly; use [WaveDB.openSubtree] instead.
  Subtree.internal(this._st, this._delimiter)
      : _delimiterCode = _delimiter.codeUnitAt(0),
        _instanceId = _nextInstanceId++ {
    _ensureCallbacksRegistered();
  }

  /// Attach the finalizer when created via WaveDB.openSubtree
  /// Internal method — do not call directly.
  void attachFinalizer() {
    if (_st != null) {
      _finalizer.attach(this, _st!.cast(), detach: this);
    }
  }

  /// Expose the native pointer for passing to GraphLayer/GraphQLLayer constructors.
  Pointer<database_subtree_t> get nativePtr => _st ?? nullptr;

  bool get isClosed => _closed;
  String get delimiter => _delimiter;

  void _checkClosed() {
    if (_closed || _st == null) {
      throw WaveDBException.ioError('subtree', 'Subtree is closed');
    }
  }

  /// Convert a key (String or List) to a delimiter-joined string
  String _keyToString(dynamic key) {
    if (key is String) return key;
    if (key is List) return key.map((e) => e.toString()).join(_delimiter);
    return key.toString();
  }

  /// Convert a key string to native UTF-8 and return (pointer, byte length).
  (Pointer<Utf8>, int) _keyToNative(String keyStr) {
    final encoded = utf8.encode(keyStr);
    final keyPtr = calloc<Uint8>(encoded.length);
    keyPtr.asTypedList(encoded.length).setAll(0, encoded);
    return (keyPtr.cast(), encoded.length);
  }

  // ============================================================
  // ASYNC OPERATIONS
  // ============================================================

  /// Store a value asynchronously via the C worker pool.
  Future<void> put(dynamic key, dynamic value) async {
    _checkClosed();
    if (value == null) {
      throw ArgumentError('Value is required for put operation');
    }
    final keyStr = _keyToString(key);
    final (keyPtr, keyByteLen) = _keyToNative(keyStr);
    final valueBytes = IdentifierConverter.toBytes(value);
    final valuePtr = calloc<Uint8>(valueBytes.length);
    valuePtr.asTypedList(valueBytes.length).setAll(0, valueBytes);

    try {
      return _dispatchAsync<void>(_SubtreeAsyncOpType.put, (promise) {
        final rc = WaveDBNative.databaseSubtreePutRaw(
          _st!, keyPtr.cast(), keyByteLen, _delimiterCode,
          valuePtr, valueBytes.length, promise,
        );
        if (rc != 0) {
          throw WaveDBException.ioError('subtree_put_raw', 'return code: $rc');
        }
      });
    } finally {
      calloc.free(keyPtr);
      calloc.free(valuePtr);
    }
  }

  /// Retrieve a value asynchronously via the C worker pool.
  ///
  /// Returns null if the key is not found.
  Future<dynamic> get(dynamic key) async {
    _checkClosed();
    final keyStr = _keyToString(key);
    final (keyPtr, keyByteLen) = _keyToNative(keyStr);

    try {
      return _dispatchAsync<dynamic>(_SubtreeAsyncOpType.get, (promise) {
        final rc = WaveDBNative.databaseSubtreeGetRaw(
          _st!, keyPtr.cast(), keyByteLen, _delimiterCode, promise,
        );
        if (rc != 0) {
          throw WaveDBException.ioError('subtree_get_raw', 'return code: $rc');
        }
      });
    } finally {
      calloc.free(keyPtr);
    }
  }

  /// Delete a value asynchronously via the C worker pool.
  Future<void> del(dynamic key) async {
    _checkClosed();
    final keyStr = _keyToString(key);
    final (keyPtr, keyByteLen) = _keyToNative(keyStr);

    try {
      return _dispatchAsync<void>(_SubtreeAsyncOpType.delete, (promise) {
        final rc = WaveDBNative.databaseSubtreeDeleteRaw(
          _st!, keyPtr.cast(), keyByteLen, _delimiterCode, promise,
        );
        if (rc != 0) {
          throw WaveDBException.ioError('subtree_delete_raw', 'return code: $rc');
        }
      });
    } finally {
      calloc.free(keyPtr);
    }
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
  ///
  /// Returns null if the key is not found.
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

  /// Scan entries under a prefix within the subtree namespace.
  ///
  /// Returns a list of maps with 'key' and 'value' entries.
  List<Map<String, dynamic>> scanSyncRaw(String prefix) {
    _checkClosed();
    return _scanSyncRawInternal(prefix);
  }

  /// Count the number of entries under the subtree prefix.
  int count() {
    _checkClosed();
    return WaveDBNative.databaseSubtreeCount(_st!);
  }

  /// Snapshot the subtree's underlying database.
  void snapshot() {
    _checkClosed();
    final rc = WaveDBNative.databaseSubtreeSnapshot(_st!);
    if (rc != 0) {
      throw WaveDBException.ioError('snapshot', 'return code: $rc');
    }
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
      final rc = WaveDBNative.databaseSubtreePutSyncRaw(
        _st!, keyPtr.cast(), keyByteLen, _delimiterCode,
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
      final rc = WaveDBNative.databaseSubtreeGetSyncRaw(
        _st!, keyPtr.cast(), keyByteLen, _delimiterCode,
        valueOutPtr, valueLenPtr,
      );

      if (rc == _kNotFoundCode) return null;
      if (rc != 0) throw WaveDBException.ioError('get', 'return code: $rc');

      final valuePtr = valueOutPtr.value;
      final valueLen = valueLenPtr.value;
      if (valuePtr == nullptr || valueLen == 0) return null;

      try {
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
      final rc = WaveDBNative.databaseSubtreeDeleteSyncRaw(
        _st!, keyPtr.cast(), keyByteLen, _delimiterCode,
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

      final rc = WaveDBNative.databaseSubtreeBatchSyncRaw(
        _st!, _delimiterCode, opsPtr, operations.length,
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

  List<Map<String, dynamic>> _scanSyncRawInternal(String prefix) {
    final (prefixPtr, prefixByteLen) = _keyToNative(prefix);
    final resultsPtr = calloc<Pointer<RawResult>>();
    final countPtr = calloc<Size>();

    try {
      final rc = WaveDBNative.databaseSubtreeScanSyncRaw(
        _st!, prefixPtr.cast(), prefixByteLen, _delimiterCode,
        resultsPtr, countPtr,
      );
      if (rc != 0) throw WaveDBException.ioError('scan', 'return code: $rc');

      final results = resultsPtr.value;
      final count = countPtr.value;

      if (count == 0 || results == nullptr) return [];

      final entries = <Map<String, dynamic>>[];

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

          dynamic valueResult;
          if (rawVal != nullptr && valLen > 0) {
            final valUint8 = Uint8List.fromList(rawVal.asTypedList(valLen));
            valueResult = IdentifierConverter.isPrintableASCII(valUint8)
                ? utf8.decode(valUint8, allowMalformed: true)
                : valUint8;
          } else {
            valueResult = '';
          }

          entries.add({'key': keyStrResult, 'value': valueResult});
        }
      } finally {
        WaveDBNative.databaseRawResultsFree(results, count);
      }

      return entries;
    } finally {
      calloc.free(prefixPtr);
      calloc.free(resultsPtr);
      calloc.free(countPtr);
    }
  }

  // ============================================================
  // ASYNC INFRASTRUCTURE
  // ============================================================

  /// Ensure the NativeCallable.listener callbacks are registered.
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
  static void _cResolveCallback(Pointer<Void> ctx, Pointer<Void> payload) {
    final requestId = ctx.address;
    final pending = _pending.remove(requestId);

    if (pending == null) {
      // Orphaned resolve — no matching request
      return;
    }

    try {
      if (pending.completer.isCompleted) return;

      switch (pending.type) {
        case _SubtreeAsyncOpType.get:
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
        case _SubtreeAsyncOpType.put:
        case _SubtreeAsyncOpType.delete:
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
    }
  }

  /// C reject callback — called from the C worker pool thread.
  static void _cRejectCallback(Pointer<Void> ctx, Pointer<async_error_t> error) {
    final requestId = ctx.address;
    final pending = _pending.remove(requestId);

    String errorMsg = 'Subtree operation failed';
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
    }
  }

  /// Dispatch an async operation via the C promise/pool infrastructure.
  Future<T> _dispatchAsync<T>(_SubtreeAsyncOpType type, void Function(Pointer<promise_t>) dispatch) {
    _checkClosed();
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

    _pending[requestId] = _SubtreePendingOp(_instanceId, type, completer, promisePtr);

    try {
      dispatch(promisePtr);
    } catch (e) {
      _pending.remove(requestId);
      WaveDBNative.promiseDestroy(promisePtr);
      completer.completeError(e);
    }

    return completer.future;
  }

  // ============================================================
  // LIFECYCLE
  // ============================================================

  /// Close the subtree and release native resources.
  ///
  /// Does NOT destroy or dereference the underlying database.
  /// Cancels any pending async operations for this instance.
  void close() {
    if (_closed || _st == null) return;

    // Cancel pending async operations for this instance
    final keysToRemove = <int>[];
    for (final entry in _pending.entries) {
      if (entry.value.instanceId == _instanceId) {
        if (!entry.value.completer.isCompleted) {
          WaveDBNative.promiseDestroy(entry.value.promisePtr);
          entry.value.completer.completeError(WaveDBException.databaseClosed());
        }
        keysToRemove.add(entry.key);
      }
    }
    for (final key in keysToRemove) {
      _pending.remove(key);
    }

    _finalizer.detach(this);
    WaveDBNative.databaseSubtreeClose(_st!);
    _st = null;
    _closed = true;
  }
}