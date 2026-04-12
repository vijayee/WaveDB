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

/// GraphQL query/mutation result
class GraphQLResult {
  /// Whether the operation was successful
  final bool success;

  /// The data returned by the operation
  final dynamic data;

  /// Error messages, if any
  final List<String> errors;

  const GraphQLResult({
    required this.success,
    this.data,
    this.errors = const [],
  });

  @override
  String toString() => jsonEncode({'success': success, 'data': data, 'errors': errors});
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
  static final Map<int, Completer<GraphQLResult>> _pending = {};

  GraphQLLayer();

  /// Create a new GraphQL layer with the given configuration
  void create(GraphQLLayerConfig config) {
    if (_layer != null && !_closed) {
      WaveDBNative.graphQLLayerDestroy(_layer!);
    }

    _layer = WaveDBNative.graphQLLayerCreate(config.path, config: null);

    if (_layer == nullptr) {
      throw GraphQLLayerException('Failed to create GraphQL layer');
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
    final completer = _pending.remove(requestId);
    if (completer == null || completer.isCompleted) return;

    final resultPtr = payload.cast<graphql_result_t>();
    final result = _convertResultFromPointer(resultPtr);
    completer.complete(result);
  }

  /// C reject callback — called from the C worker pool thread.
  static void _cRejectCallback(Pointer<Void> ctx, Pointer<async_error_t> error) {
    final requestId = ctx.address;
    final completer = _pending.remove(requestId);
    if (completer == null || completer.isCompleted) return;

    WaveDBNative.errorDestroy(error);
    completer.completeError(GraphQLLayerException('Operation failed'));
  }

  /// Convert a C graphql_result_t pointer to a Dart GraphQLResult
  static GraphQLResult _convertResultFromPointer(Pointer<graphql_result_t> resultPtr) {
    if (resultPtr == nullptr) {
      return const GraphQLResult(success: false, data: null, errors: ['Execution failed']);
    }

    try {
      final jsonPtr = WaveDBNative.graphQLResultToJson(resultPtr);
      if (jsonPtr == nullptr) {
        return const GraphQLResult(success: false, data: null, errors: ['Failed to serialize result']);
      }

      try {
        final jsonString = jsonPtr.toDartString();
        final parsed = jsonDecode(jsonString) as Map<String, dynamic>;
        return GraphQLResult(
          success: parsed['success'] as bool? ?? false,
          data: parsed['data'],
          errors: (parsed['errors'] as List<dynamic>?)
                  ?.map((e) => e is String ? e : e.toString())
                  .toList() ??
              const [],
        );
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
    final rc = WaveDBNative.graphQLSchemaParse(_layer!, sdl);
    if (rc != 0) {
      throw GraphQLLayerException('Failed to parse schema (error code: $rc)');
    }
  }

  /// Execute a GraphQL query synchronously
  GraphQLResult querySync(String query) {
    _checkOpen();
    final result = WaveDBNative.graphQLQuerySync(_layer!, query);
    if (result == nullptr) {
      return const GraphQLResult(success: false, data: null, errors: ['Query execution failed']);
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
      return const GraphQLResult(success: false, data: null, errors: ['Mutation execution failed']);
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
    _pending[requestId] = completer;

    // Create a C promise_t with our NativeCallable listener callbacks
    // The ctx pointer encodes the request ID so we can look up the completer
    final ctxPtr = Pointer<Void>.fromAddress(requestId);
    final promisePtr = WaveDBNative.promiseCreate(
      _resolveCallable!.nativeFunction,
      _rejectCallable!.nativeFunction,
      ctxPtr,
    );

    if (promisePtr == nullptr) {
      _pending.remove(requestId);
      completer.completeError(GraphQLLayerException('Failed to create promise'));
      return completer.future;
    }

    // Dispatch to the C worker pool
    if (op == _OperationType.query) {
      WaveDBNative.graphQLQueryAsync(_layer!, input, promisePtr);
    } else {
      WaveDBNative.graphQLMutateAsync(_layer!, input, promisePtr);
    }

    // The promise is now owned by the C async system; it will be destroyed
    // by the worker callbacks. We just wait for the completer.
    return completer.future;
  }

  /// Close the GraphQL layer and release resources
  void close() {
    if (!_closed && _layer != null) {
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
      return const GraphQLResult(success: false, data: null, errors: ['Failed to serialize result']);
    }

    try {
      final jsonString = jsonPtr.toDartString();
      final parsed = jsonDecode(jsonString) as Map<String, dynamic>;
      return GraphQLResult(
        success: parsed['success'] as bool? ?? false,
        data: parsed['data'],
        errors: (parsed['errors'] as List<dynamic>?)
                ?.map((e) => e is String ? e : e.toString())
                .toList() ??
            const [],
      );
    } finally {
      malloc.free(jsonPtr.cast());
    }
  }
}

/// Operation type for async dispatch
enum _OperationType { query, mutate }

/// Exception thrown by GraphQL layer operations
class GraphQLLayerException implements Exception {
  final String message;
  const GraphQLLayerException(this.message);

  @override
  String toString() => 'GraphQLLayerException: $message';
}