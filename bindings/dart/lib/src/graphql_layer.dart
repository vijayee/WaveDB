// lib/src/graphql_layer.dart
import 'dart:ffi';
import 'dart:convert';
import 'dart:async';
import 'package:ffi/ffi.dart';
import 'native/types.dart';
import 'native/wavedb_bindings.dart';

/// Configuration options for the GraphQL layer
class GraphQLLayerConfig {
  /// Database storage path (null for in-memory)
  final String? path;

  /// Enable persistent storage (default: true)
  final bool enablePersist;

  /// HBTrie chunk size in bytes (default: 4)
  final int? chunkSize;

  /// Number of worker threads (default: 4)
  final int? workerThreads;

  const GraphQLLayerConfig({
    this.path,
    this.enablePersist = true,
    this.chunkSize,
    this.workerThreads,
  });
}

/// Structured GraphQL error with message and optional path
class GraphQLError {
  final String message;
  final String? path;
  const GraphQLError({required this.message, this.path});

  @override
  String toString() => path != null ? '$message (path: $path)' : message;
}

/// GraphQL query/mutation result
class GraphQLResult {
  /// Whether the operation was successful
  final bool success;

  /// The data returned by the operation
  final dynamic data;

  /// Structured errors, if any
  final List<GraphQLError> errors;

  const GraphQLResult({
    required this.success,
    this.data,
    this.errors = const [],
  });

  @override
  String toString() => jsonEncode({'success': success, 'data': data, 'errors': errors.map((e) => {'message': e.message, 'path': e.path}).toList()});
}

/// GraphQL layer for WaveDB
///
/// Provides schema definition, query execution, and mutation operations
/// on top of WaveDB's hierarchical key-value store.
///
/// Async methods use the C worker pool and promise infrastructure,
/// with NativeCallable.listener bridging callbacks back to the Dart isolate.
class GraphQLLayer {
  Pointer<graphql_layer_t>? _layer;
  bool _closed = false;

  // NativeCallable listeners for C promise callbacks.
  // These must persist for the lifetime of the layer since C holds pointers to them.
  static NativeCallable<Void Function(Pointer<Void>, Pointer<Void>)>? _resolveCallable;
  static NativeCallable<Void Function(Pointer<Void>, Pointer<async_error_t>)>? _rejectCallable;

  // Active completers keyed by request id
  static int _nextRequestId = 1;
  static final Map<int, _GraphQLPendingOp> _pending = {};

  // Unique instance id to scope pending ops
  final int _instanceId;
  static int _nextInstanceId = 1;

  GraphQLLayer() : _instanceId = _nextInstanceId++;

  /// Create a new GraphQL layer with the given configuration
  ///
  /// Note: only [GraphQLLayerConfig.path] is currently used. Other config
  /// fields (enablePersist, chunkSize, workerThreads) are not yet supported
  /// by the C API's config interface and will be silently ignored.
  void create(GraphQLLayerConfig config) {
    if (_layer != null && !_closed) {
      WaveDBNative.graphQLLayerDestroy(_layer!);
    }

    final configPtr = WaveDBNative.graphQLLayerConfigDefault();
    try {
      _layer = WaveDBNative.graphQLLayerCreate(config.path, config: configPtr);
    } finally {
      WaveDBNative.graphQLLayerConfigDestroy(configPtr);
    }

    if (_layer == nullptr) {
      throw GraphQLLayerException('Failed to create GraphQL layer');
    }

    _ensureCallbacksRegistered();
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

    if (pending == null) return;

    try {
      if (pending.completer.isCompleted) return;

      final resultPtr = payload.cast<graphql_result_t>();
      final result = _convertResultFromPointer(resultPtr);
      pending.completer.complete(result);
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
        pending.completer.completeError(GraphQLLayerException(errorMsg));
      }
    } finally {
      WaveDBNative.promiseDestroy(pending.promisePtr);
    }
  }

  /// Convert a C graphql_result_t pointer to a Dart GraphQLResult
  static GraphQLResult _convertResultFromPointer(Pointer<graphql_result_t> resultPtr) {
    if (resultPtr == nullptr) {
      return const GraphQLResult(success: false, data: null, errors: [GraphQLError(message: 'Execution failed')]);
    }

    try {
      final jsonPtr = WaveDBNative.graphQLResultToJson(resultPtr);
      if (jsonPtr == nullptr) {
        return const GraphQLResult(success: false, data: null, errors: [GraphQLError(message: 'Failed to serialize result')]);
      }

      try {
        final jsonString = jsonPtr.toDartString();
        final parsed = jsonDecode(jsonString) as Map<String, dynamic>;
        return _parseResult(parsed);
      } finally {
        malloc.free(jsonPtr.cast());
      }
    } finally {
      WaveDBNative.graphQLResultDestroy(resultPtr);
    }
  }

  /// Parse a GraphQL schema definition (SDL)
  void parseSchema(String sdl) {
    _checkOpen();
    final errorPtr = calloc<Pointer<Utf8>>();
    try {
      final rc = WaveDBNative.graphQLSchemaParse(_layer!, sdl, errorOut: errorPtr);
      if (rc != 0) {
        final msg = errorPtr.value != nullptr
            ? errorPtr.value.toDartString()
            : 'Failed to parse schema (error code: $rc)';
        throw GraphQLLayerException(msg);
      }
    } finally {
      if (errorPtr.value != nullptr) {
        malloc.free(errorPtr.value);
      }
      malloc.free(errorPtr);
    }
  }

  /// Execute a GraphQL query synchronously
  GraphQLResult querySync(String query) {
    _checkOpen();
    final result = WaveDBNative.graphQLQuerySync(_layer!, query);
    if (result == nullptr) {
      return const GraphQLResult(success: false, data: null, errors: [GraphQLError(message: 'Query execution failed')]);
    }
    try {
      return _convertResultFromJson(result);
    } finally {
      WaveDBNative.graphQLResultDestroy(result);
    }
  }

  /// Execute a GraphQL mutation synchronously
  GraphQLResult mutateSync(String mutation) {
    _checkOpen();
    final result = WaveDBNative.graphQLMutateSync(_layer!, mutation);
    if (result == nullptr) {
      return const GraphQLResult(success: false, data: null, errors: [GraphQLError(message: 'Mutation execution failed')]);
    }
    try {
      return _convertResultFromJson(result);
    } finally {
      WaveDBNative.graphQLResultDestroy(result);
    }
  }

  /// Execute a GraphQL query asynchronously via the C worker pool
  ///
  /// Creates a C promise_t with Dart resolve/reject callbacks,
  /// then dispatches to graphql_query(). When the C worker completes,
  /// the callback fires on the Dart isolate's event loop and the
  /// Completer is resolved.
  Future<GraphQLResult> query(String query) async {
    _checkOpen();
    return _dispatchAsync(query, _OperationType.query);
  }

  /// Execute a GraphQL mutation asynchronously via the C worker pool
  Future<GraphQLResult> mutate(String mutation) async {
    _checkOpen();
    return _dispatchAsync(mutation, _OperationType.mutate);
  }

  /// Dispatch an async GraphQL operation via the C promise/pool infrastructure
  Future<GraphQLResult> _dispatchAsync(String input, _OperationType op) {
    final completer = Completer<GraphQLResult>();
    final requestId = _nextRequestId++;

    // Create a C promise_t with our NativeCallable listener callbacks
    // The ctx pointer encodes the request ID so we can look up the completer
    final ctxPtr = Pointer<Void>.fromAddress(requestId);
    final promisePtr = WaveDBNative.promiseCreate(
      _resolveCallable!.nativeFunction,
      _rejectCallable!.nativeFunction,
      ctxPtr,
    );

    if (promisePtr == nullptr) {
      completer.completeError(GraphQLLayerException('Failed to create promise'));
      return completer.future;
    }

    _pending[requestId] = _GraphQLPendingOp(_instanceId, completer, promisePtr);

    // Dispatch to the C worker pool
    if (op == _OperationType.query) {
      WaveDBNative.graphQLQueryAsync(_layer!, input, promisePtr);
    } else {
      WaveDBNative.graphQLMutateAsync(_layer!, input, promisePtr);
    }

    return completer.future;
  }

  /// Close the GraphQL layer and release resources
  void close() {
    if (!_closed && _layer != null) {
      // Cancel only this instance's pending async operations
      final keysToRemove = <int>[];
      for (final entry in _pending.entries) {
        if (entry.value.instanceId == _instanceId) {
          if (!entry.value.completer.isCompleted) {
            WaveDBNative.promiseDestroy(entry.value.promisePtr);
            entry.value.completer.completeError(GraphQLLayerException('GraphQL layer is closed'));
          }
          keysToRemove.add(entry.key);
        }
      }
      for (final key in keysToRemove) {
        _pending.remove(key);
      }

      WaveDBNative.graphQLLayerDestroy(_layer!);
      _layer = null;
      _closed = true;
    }
  }

  void _checkOpen() {
    if (_closed || _layer == null) {
      throw GraphQLLayerException('GraphQL layer is closed');
    }
  }

  /// Convert a C result to Dart by using the JSON serialization (sync path)
  GraphQLResult _convertResultFromJson(Pointer<graphql_result_t> resultPtr) {
    final jsonPtr = WaveDBNative.graphQLResultToJson(resultPtr);
    if (jsonPtr == nullptr) {
      return const GraphQLResult(success: false, data: null, errors: [GraphQLError(message: 'Failed to serialize result')]);
    }

    try {
      final jsonString = jsonPtr.toDartString();
      final parsed = jsonDecode(jsonString) as Map<String, dynamic>;
      return _parseResult(parsed);
    } finally {
      malloc.free(jsonPtr.cast());
    }
  }

  /// Parse a decoded JSON map into a GraphQLResult
  static GraphQLResult _parseResult(Map<String, dynamic> parsed) {
    return GraphQLResult(
      success: parsed['success'] as bool? ?? false,
      data: parsed['data'],
      errors: (parsed['errors'] as List<dynamic>?)
              ?.map((e) => GraphQLError(
                  message: (e is Map ? (e['message'] as String? ?? '') : e.toString()),
                  path: e is Map ? e['path'] as String? : null))
              .toList() ??
          const [],
    );
  }
}

/// Operation type for async dispatch
enum _OperationType { query, mutate }

/// Pending async GraphQL operation holding completer and C promise pointer
class _GraphQLPendingOp {
  final int instanceId;
  final Completer<GraphQLResult> completer;
  final Pointer<promise_t> promisePtr;
  _GraphQLPendingOp(this.instanceId, this.completer, this.promisePtr);
}

/// Exception thrown by GraphQL layer operations
class GraphQLLayerException implements Exception {
  final String message;
  const GraphQLLayerException(this.message);

  @override
  String toString() => 'GraphQLLayerException: $message';
}