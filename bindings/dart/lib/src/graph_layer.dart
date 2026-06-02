// lib/src/graph_layer.dart
import 'dart:ffi';
import 'dart:io';
import 'dart:async';
import 'package:ffi/ffi.dart';
import 'native/types.dart';
import 'native/wavedb_bindings.dart';

GraphQuery g([GraphLayer? layer]) => GraphQuery(layer ?? _defaultGraph);
GraphLayer? _defaultGraph;

/// Operation type for async graph dispatch
enum _GraphAsyncOpType { insert, delete }

/// Pending async operation context for graph layer
class _GraphPendingOp {
  final int instanceId;
  final _GraphAsyncOpType type;
  final Completer completer;
  final Pointer<promise_t> promisePtr;

  _GraphPendingOp(this.instanceId, this.type, this.completer, this.promisePtr);
}

/// Graph layer for WaveDB
///
/// Provides triple insert/delete and DSL query execution on top of
/// WaveDB's hierarchical key-value store.
class GraphLayer implements Finalizable {
  Pointer<graph_layer_t>? _layer;
  bool _closed = false;
  String? _tempDir;

  static final NativeFinalizer _finalizer = NativeFinalizer(
    WaveDBNative.graphLayerDestroyNative.cast(),
  );

  // NativeCallable listeners for C promise callbacks.
  static NativeCallable<Void Function(Pointer<Void>, Pointer<Void>)>? _resolveCallable;
  static NativeCallable<Void Function(Pointer<Void>, Pointer<async_error_t>)>? _rejectCallable;

  // Active completers keyed by request id
  static int _nextRequestId = 1;
  static final Map<int, _GraphPendingOp> _pending = {};

  // Unique instance id to scope pending ops
  final int _instanceId;
  static int _nextInstanceId = 1;

  GraphLayer([String? path]) : _instanceId = _nextInstanceId++ {
    String actualPath;
    if (path != null) {
      actualPath = path;
    } else {
      _tempDir = Directory.systemTemp.createTempSync('wavedb_graph_').path;
      actualPath = _tempDir!;
    }
    _layer = WaveDBNative.graphLayerCreate(actualPath);
    _finalizer.attach(this, _layer!.cast(), detach: this);
    if (_defaultGraph == null) _defaultGraph = this;

    _ensureCallbacksRegistered();
  }

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

  /// C resolve callback — called from C worker pool thread.
  /// NativeCallable.listener marshals this to the Dart isolate's event loop.
  static void _cResolveCallback(Pointer<Void> ctx, Pointer<Void> payload) {
    final requestId = ctx.address;
    final pending = _pending.remove(requestId);

    if (pending == null) return;

    try {
      if (pending.completer.isCompleted) return;

      switch (pending.type) {
        case _GraphAsyncOpType.insert:
        case _GraphAsyncOpType.delete:
          // payload is &tc->result — points into freed triple_work_ctx_t,
          // do NOT free it. The C worker already freed the context.
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

  /// C reject callback — called from C worker pool thread.
  static void _cRejectCallback(Pointer<Void> ctx, Pointer<async_error_t> error) {
    final requestId = ctx.address;
    final pending = _pending.remove(requestId);

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
        pending.completer.completeError(GraphLayerException(errorMsg));
      }
    } finally {
      WaveDBNative.promiseDestroy(pending.promisePtr);
    }
  }

  /// Dispatch an async operation via the C promise/pool infrastructure
  Future<T> _dispatchAsync<T>(_GraphAsyncOpType type, void Function(Pointer<promise_t>) dispatch) {
    final completer = Completer<T>();
    final requestId = _nextRequestId++;

    final ctxPtr = Pointer<Void>.fromAddress(requestId);
    final promisePtr = WaveDBNative.promiseCreate(
      _resolveCallable!.nativeFunction,
      _rejectCallable!.nativeFunction,
      ctxPtr,
    );

    if (promisePtr == nullptr) {
      completer.completeError(GraphLayerException('Failed to create async promise'));
      return completer.future;
    }

    _pending[requestId] = _GraphPendingOp(_instanceId, type, completer, promisePtr);

    try {
      dispatch(promisePtr);
    } catch (e) {
      _pending.remove(requestId);
      WaveDBNative.promiseDestroy(promisePtr);
      completer.completeError(e);
    }

    return completer.future;
  }

  // ── Async operations ──

  /// Insert a triple asynchronously (dispatches to C worker pool).
  Future<void> insert(String s, String p, String o) {
    _checkOpen();
    return _dispatchAsync<void>(_GraphAsyncOpType.insert, (promise) {
      WaveDBNative.graphInsertAsync(_layer!, s, p, o, promise);
    });
  }

  /// Delete a triple asynchronously (dispatches to C worker pool).
  Future<void> del(String s, String p, String o) {
    _checkOpen();
    return _dispatchAsync<void>(_GraphAsyncOpType.delete, (promise) {
      WaveDBNative.graphDeleteAsync(_layer!, s, p, o, promise);
    });
  }

  /// Execute a DSL query asynchronously.
  /// Note: query is currently synchronous (runs on main isolate) because
  /// the C layer's async query API requires an opaque graph_query_t pointer
  /// that cannot be constructed from Dart. A future C API addition of
  /// graph_parse_execute_async would enable true async queries.
  Future<List<String>> query(Object dsl) async {
    return exec(dsl);
  }

  // ── Sync operations ──

  /// Execute a DSL query and return results as a list of strings.
  List<String> exec(Object dsl) {
    _checkOpen();
    final dslStr = dsl is GraphQuery ? dsl._toDSL() : dsl as String;
    final resultPtr = WaveDBNative.graphParseExecute(_layer!, dslStr);
    if (resultPtr == nullptr) return const [];

    try {
      final count = WaveDBNative.graphResultCount(resultPtr);
      if (count == 0) return const [];

      final verts = WaveDBNative.graphResultVertices(resultPtr);
      final results = <String>[];
      for (int i = 0; i < count; i++) {
        final strPtr = verts.elementAt(i).value;
        if (strPtr != nullptr) {
          results.add(strPtr.toDartString());
        }
      }
      return results;
    } finally {
      WaveDBNative.graphResultDestroy(resultPtr);
    }
  }

  /// Execute a DSL query and return the count only.
  int count(Object dsl) {
    _checkOpen();
    final dslStr = dsl is GraphQuery ? dsl._toDSL() : dsl as String;
    return WaveDBNative.graphParseCount(_layer!, dslStr);
  }

  /// Insert a triple synchronously.
  void insertSync(String s, String p, String o) {
    _checkOpen();
    final rc = WaveDBNative.graphInsertSync(_layer!, s, p, o);
    if (rc != 0) {
      throw GraphLayerException('Insert failed ($rc)');
    }
  }

  /// Delete a triple synchronously.
  void deleteSync(String s, String p, String o) {
    _checkOpen();
    final rc = WaveDBNative.graphDeleteSync(_layer!, s, p, o);
    if (rc != 0) {
      throw GraphLayerException('Delete failed ($rc)');
    }
  }

  /// Parse a graph schema definition.
  void parseSchema(String sdl) {
    _checkOpen();
    WaveDBNative.graphSchemaParse(_layer!, sdl);
  }

  /// Define a named morphism (reusable traversal path).
  void defineMorphism(String name, String dsl) {
    _checkOpen();
    WaveDBNative.graphMorphismDefine(_layer!, name, dsl);
  }

  /// Close the graph layer and release C resources.
  void close() {
    if (!_closed && _layer != null) {
      if (_defaultGraph == this) _defaultGraph = null;

      // Cancel this instance's pending async operations
      final keysToRemove = <int>[];
      for (final entry in _pending.entries) {
        if (entry.value.instanceId == _instanceId) {
          if (!entry.value.completer.isCompleted) {
            WaveDBNative.promiseDestroy(entry.value.promisePtr);
            entry.value.completer.completeError(StateError('GraphLayer is closed'));
          }
          keysToRemove.add(entry.key);
        }
      }
      for (final key in keysToRemove) {
        _pending.remove(key);
      }

      _finalizer.detach(this);
      WaveDBNative.graphLayerDestroy(_layer!);
      _layer = null;
      _closed = true;
    }
    // Clean up temp directory if we created one
    if (_tempDir != null) {
      try {
        Directory(_tempDir!).deleteSync(recursive: true);
      } catch (_) {
        // Ignore cleanup errors
      }
      _tempDir = null;
    }
  }

  void _checkOpen() {
    if (_closed || _layer == null) {
      throw StateError('GraphLayer is closed');
    }
  }
}

/// Exception thrown by graph layer operations
class GraphLayerException implements Exception {
  final String message;
  const GraphLayerException(this.message);

  @override
  String toString() => 'GraphLayerException: $message';
}

/// Query builder for graph traversals
class GraphQuery {
  GraphLayer? _layer;
  final List<String> _steps = [];

  GraphQuery([GraphLayer? layer]) : _layer = layer;

  GraphQuery V(String id) { _steps.add('V("$id")'); return this; }
  GraphQuery Out(String pred) { _steps.add('Out("$pred")'); return this; }
  GraphQuery In(String pred) { _steps.add('In("$pred")'); return this; }
  GraphQuery Has(String pred, String val) { _steps.add('Has("$pred","$val")'); return this; }
  GraphQuery And(GraphQuery sub) { _steps.add('And(${sub._toDSL()})'); return this; }
  GraphQuery Or(GraphQuery sub) { _steps.add('Or(${sub._toDSL()})'); return this; }
  GraphQuery Not(GraphQuery sub) { _steps.add('Not(${sub._toDSL()})'); return this; }
  GraphQuery Difference(GraphQuery sub) { _steps.add('Difference(${sub._toDSL()})'); return this; }
  GraphQuery Follow(String name) { _steps.add('Follow("$name")'); return this; }
  GraphQuery Limit(int n) { _steps.add('Limit($n)'); return this; }
  /// Appends Count() — use with [GraphLayer.count], not [All].
  GraphQuery Count() { _steps.add('Count()'); return this; }

  String _toDSL() => 'g.${_steps.join('.')}';

  /// Execute the query and return results as a list of strings.
  List<String> All() {
    if (_layer == null) throw StateError('Query not bound to a GraphLayer');
    return _layer!.exec(this);
  }

  @override
  String toString() => _toDSL();
}