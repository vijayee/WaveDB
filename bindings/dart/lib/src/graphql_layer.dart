// lib/src/graphql_layer.dart
import 'dart:ffi';
import 'dart:convert';
import 'package:ffi/ffi.dart';
import 'native/types.dart';
import 'native/wavedb_bindings.dart';
import 'exceptions.dart';

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
class GraphQLLayer {
  Pointer<graphql_layer_t>? _layer;
  bool _closed = false;

  GraphQLLayer();

  /// Create a new GraphQL layer with the given configuration
  void create(GraphQLLayerConfig config) {
    if (_layer != null && !_closed) {
      WaveDBNative.graphQLLayerDestroy(_layer!);
    }

    // Create the layer — uses default config when config is null
    _layer = WaveDBNative.graphQLLayerCreate(config.path, config: null);

    if (_layer == nullptr) {
      throw GraphQLLayerException('Failed to create GraphQL layer');
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

  /// Convert a C result to Dart by using the JSON serialization
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

/// Exception thrown by GraphQL layer operations
class GraphQLLayerException implements Exception {
  final String message;
  const GraphQLLayerException(this.message);

  @override
  String toString() => 'GraphQLLayerException: $message';
}