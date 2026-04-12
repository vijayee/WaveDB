// lib/src/native/wavedb_bindings.dart
import 'dart:ffi';

import 'package:ffi/ffi.dart';

import 'types.dart';
import 'wavedb_library.dart';
import '../exceptions.dart';

// ============================================================
// C TYPEDEFS - Database Lifecycle
// ============================================================

/// C signature: database_t* database_create(
///   const char* location,
///   size_t lru_memory_mb,
///   wal_config_t* wal_config,
///   uint8_t chunk_size,
///   uint32_t btree_node_size,
///   uint8_t enable_persist,
///   size_t storage_cache_size,
///   work_pool_t* pool,
///   hierarchical_timing_wheel_t* wheel,
///   int* error_code
/// )
typedef DatabaseCreateC = Pointer<database_t> Function(
  Pointer<Utf8> location,
  UintPtr lru_memory_mb,
  Pointer<Void> wal_config,
  Uint8 chunk_size,
  Uint32 btree_node_size,
  Uint8 enable_persist,
  UintPtr storage_cache_size,
  Pointer<Void> pool,
  Pointer<Void> wheel,
  Pointer<Int32> error_code,
);

/// Dart signature for database_create
typedef DatabaseCreate = Pointer<database_t> Function(
  Pointer<Utf8> location,
  int lru_memory_mb,
  Pointer<Void> wal_config,
  int chunk_size,
  int btree_node_size,
  int enable_persist,
  int storage_cache_size,
  Pointer<Void> pool,
  Pointer<Void> wheel,
  Pointer<Int32> error_code,
);

/// C signature: void database_destroy(database_t* db)
typedef DatabaseDestroyC = Void Function(Pointer<database_t> db);

/// Dart signature for database_destroy
typedef DatabaseDestroy = void Function(Pointer<database_t> db);

// ============================================================
// C TYPEDEFS - Database Configuration
// ============================================================

/// C signature: database_config_t* database_config_default()
typedef DatabaseConfigDefaultC = Pointer<database_config_t> Function();

/// Dart signature for database_config_default
typedef DatabaseConfigDefault = Pointer<database_config_t> Function();

/// C signature: database_config_t* database_config_copy(const database_config_t* config)
typedef DatabaseConfigCopyC = Pointer<database_config_t> Function(
  Pointer<database_config_t> config,
);

/// Dart signature for database_config_copy
typedef DatabaseConfigCopy = Pointer<database_config_t> Function(
  Pointer<database_config_t> config,
);

/// C signature: void database_config_destroy(database_config_t* config)
typedef DatabaseConfigDestroyC = Void Function(Pointer<database_config_t> config);

/// Dart signature for database_config_destroy
typedef DatabaseConfigDestroy = void Function(Pointer<database_config_t> config);

/// C signature: database_t* database_create_with_config(
///   const char* location,
///   database_config_t* config,
///   int* error_code
/// )
typedef DatabaseCreateWithConfigC = Pointer<database_t> Function(
  Pointer<Utf8> location,
  Pointer<database_config_t> config,
  Pointer<Int32> error_code,
);

/// Dart signature for database_create_with_config
typedef DatabaseCreateWithConfig = Pointer<database_t> Function(
  Pointer<Utf8> location,
  Pointer<database_config_t> config,
  Pointer<Int32> error_code,
);

// ============================================================
// C TYPEDEFS - Synchronous Operations
// ============================================================

/// C signature: int32_t database_put_sync(
///   database_t* db,
///   path_t* path,
///   identifier_t* value
/// )
typedef DatabasePutSyncC = Int32 Function(
  Pointer<database_t> db,
  Pointer<path_t> path,
  Pointer<identifier_t> value,
);

/// Dart signature for database_put_sync
typedef DatabasePutSync = int Function(
  Pointer<database_t> db,
  Pointer<path_t> path,
  Pointer<identifier_t> value,
);

/// C signature: int32_t database_get_sync(
///   database_t* db,
///   path_t* path,
///   identifier_t** result
/// )
typedef DatabaseGetSyncC = Int32 Function(
  Pointer<database_t> db,
  Pointer<path_t> path,
  Pointer<Pointer<identifier_t>> result,
);

/// Dart signature for database_get_sync
typedef DatabaseGetSync = int Function(
  Pointer<database_t> db,
  Pointer<path_t> path,
  Pointer<Pointer<identifier_t>> result,
);

/// C signature: int32_t database_delete_sync(
///   database_t* db,
///   path_t* path
/// )
typedef DatabaseDeleteSyncC = Int32 Function(
  Pointer<database_t> db,
  Pointer<path_t> path,
);

/// Dart signature for database_delete_sync
typedef DatabaseDeleteSync = int Function(
  Pointer<database_t> db,
  Pointer<path_t> path,
);

// ============================================================
// C TYPEDEFS - Async Database Operations
// ============================================================

/// C signature: void database_put(
///   database_t* db,
///   path_t* path,
///   identifier_t* value,
///   promise_t* promise
/// )
typedef DatabasePutAsyncC = Void Function(
  Pointer<database_t> db,
  Pointer<path_t> path,
  Pointer<identifier_t> value,
  Pointer<promise_t> promise,
);

/// Dart signature for database_put (async)
typedef DatabasePutAsync = void Function(
  Pointer<database_t> db,
  Pointer<path_t> path,
  Pointer<identifier_t> value,
  Pointer<promise_t> promise,
);

/// C signature: void database_get(
///   database_t* db,
///   path_t* path,
///   promise_t* promise
/// )
typedef DatabaseGetAsyncC = Void Function(
  Pointer<database_t> db,
  Pointer<path_t> path,
  Pointer<promise_t> promise,
);

/// Dart signature for database_get (async)
typedef DatabaseGetAsync = void Function(
  Pointer<database_t> db,
  Pointer<path_t> path,
  Pointer<promise_t> promise,
);

/// C signature: void database_delete(
///   database_t* db,
///   path_t* path,
///   promise_t* promise
/// )
typedef DatabaseDeleteAsyncC = Void Function(
  Pointer<database_t> db,
  Pointer<path_t> path,
  Pointer<promise_t> promise,
);

/// Dart signature for database_delete (async)
typedef DatabaseDeleteAsync = void Function(
  Pointer<database_t> db,
  Pointer<path_t> path,
  Pointer<promise_t> promise,
);

// ============================================================
// C TYPEDEFS - Batch Operations
// ============================================================

/// C signature: batch_t* batch_create(size_t reserve_count)
typedef BatchCreateC = Pointer<batch_t> Function(UintPtr reserve_count);

/// Dart signature for batch_create
typedef BatchCreate = Pointer<batch_t> Function(int reserve_count);

/// C signature: int batch_add_put(
///   batch_t* batch,
///   path_t* path,
///   identifier_t* value
/// )
typedef BatchAddPutC = Int32 Function(
  Pointer<batch_t> batch,
  Pointer<path_t> path,
  Pointer<identifier_t> value,
);

/// Dart signature for batch_add_put
typedef BatchAddPut = int Function(
  Pointer<batch_t> batch,
  Pointer<path_t> path,
  Pointer<identifier_t> value,
);

/// C signature: int batch_add_delete(
///   batch_t* batch,
///   path_t* path
/// )
typedef BatchAddDeleteC = Int32 Function(
  Pointer<batch_t> batch,
  Pointer<path_t> path,
);

/// Dart signature for batch_add_delete
typedef BatchAddDelete = int Function(
  Pointer<batch_t> batch,
  Pointer<path_t> path,
);

/// C signature: void batch_destroy(batch_t* batch)
typedef BatchDestroyC = Void Function(Pointer<batch_t> batch);

/// Dart signature for batch_destroy
typedef BatchDestroy = void Function(Pointer<batch_t> batch);

/// C signature: void database_write_batch(
///   database_t* db,
///   batch_t* batch,
///   promise_t* promise
/// )
typedef DatabaseWriteBatchAsyncC = Void Function(
  Pointer<database_t> db,
  Pointer<batch_t> batch,
  Pointer<promise_t> promise,
);

/// Dart signature for database_write_batch (async)
typedef DatabaseWriteBatchAsync = void Function(
  Pointer<database_t> db,
  Pointer<batch_t> batch,
  Pointer<promise_t> promise,
);

// ============================================================
// C TYPEDEFS - Path Operations
// ============================================================

/// C signature: path_t* path_create()
typedef PathCreateC = Pointer<path_t> Function();

/// Dart signature for path_create
typedef PathCreate = Pointer<path_t> Function();

/// C signature: int32_t path_append(
///   path_t* path,
///   identifier_t* identifier
/// )
typedef PathAppendC = Int32 Function(
  Pointer<path_t> path,
  Pointer<identifier_t> identifier,
);

/// Dart signature for path_append
typedef PathAppend = int Function(
  Pointer<path_t> path,
  Pointer<identifier_t> identifier,
);

/// C signature: void path_destroy(path_t* path)
typedef PathDestroyC = Void Function(Pointer<path_t> path);

/// Dart signature for path_destroy
typedef PathDestroy = void Function(Pointer<path_t> path);

/// C signature: size_t path_length(path_t* path)
typedef PathLengthC = UintPtr Function(Pointer<path_t> path);

/// Dart signature for path_length
typedef PathLength = int Function(Pointer<path_t> path);

/// C signature: identifier_t* path_get(path_t* path, size_t index)
typedef PathGetC = Pointer<identifier_t> Function(
  Pointer<path_t> path,
  UintPtr index,
);

/// Dart signature for path_get
typedef PathGet = Pointer<identifier_t> Function(
  Pointer<path_t> path,
  int index,
);

// ============================================================
// C TYPEDEFS - Buffer Operations (must come before Identifier)
// ============================================================

/// C signature: buffer_t* buffer_create(size_t size)
typedef BufferCreateC = Pointer<buffer_t> Function(UintPtr size);

/// Dart signature for buffer_create
typedef BufferCreate = Pointer<buffer_t> Function(int size);

/// C signature: buffer_t* buffer_create_from_pointer_copy(uint8_t* data, size_t size)
typedef BufferCreateFromPointerCopyC = Pointer<buffer_t> Function(
  Pointer<Uint8> data,
  UintPtr size,
);

/// Dart signature for buffer_create_from_pointer_copy
typedef BufferCreateFromPointerCopy = Pointer<buffer_t> Function(
  Pointer<Uint8> data,
  int size,
);

/// C signature: void buffer_destroy(buffer_t* buf)
typedef BufferDestroyC = Void Function(Pointer<buffer_t> buf);

/// Dart signature for buffer_destroy
typedef BufferDestroy = void Function(Pointer<buffer_t> buf);

// ============================================================
// C TYPEDEFS - Identifier Operations
// ============================================================

/// C signature: identifier_t* identifier_create(buffer_t* buf, size_t chunk_size)
typedef IdentifierCreateC = Pointer<identifier_t> Function(
  Pointer<buffer_t> buf,
  UintPtr chunk_size,
);

/// Dart signature for identifier_create
typedef IdentifierCreate = Pointer<identifier_t> Function(
  Pointer<buffer_t> buf,
  int chunk_size,
);

/// C signature: void identifier_destroy(identifier_t* id)
typedef IdentifierDestroyC = Void Function(Pointer<identifier_t> id);

/// Dart signature for identifier_destroy
typedef IdentifierDestroy = void Function(Pointer<identifier_t> id);

/// C signature: buffer_t* identifier_to_buffer(identifier_t* id)
typedef IdentifierToBufferC = Pointer<buffer_t> Function(
  Pointer<identifier_t> id,
);

/// Dart signature for identifier_to_buffer
typedef IdentifierToBuffer = Pointer<buffer_t> Function(
  Pointer<identifier_t> id,
);

// ============================================================
// C TYPEDEFS - Iterator Operations
// ============================================================

/// C signature: database_iterator_t* database_scan_start(
///   database_t* db,
///   path_t* start_path,
///   path_t* end_path
/// )
typedef DatabaseScanStartC = Pointer<database_iterator_t> Function(
  Pointer<database_t> db,
  Pointer<path_t> start_path,
  Pointer<path_t> end_path,
);

/// Dart signature for database_scan_start
typedef DatabaseScanStart = Pointer<database_iterator_t> Function(
  Pointer<database_t> db,
  Pointer<path_t> start_path,
  Pointer<path_t> end_path,
);

/// C signature: int32_t database_scan_next(
///   database_iterator_t* iter,
///   path_t** out_path,
///   identifier_t** out_value
/// )
typedef DatabaseScanNextC = Int32 Function(
  Pointer<database_iterator_t> iter,
  Pointer<Pointer<path_t>> out_path,
  Pointer<Pointer<identifier_t>> out_value,
);

/// Dart signature for database_scan_next
typedef DatabaseScanNext = int Function(
  Pointer<database_iterator_t> iter,
  Pointer<Pointer<path_t>> out_path,
  Pointer<Pointer<identifier_t>> out_value,
);

/// C signature: void database_scan_end(database_iterator_t* iter)
typedef DatabaseScanEndC = Void Function(Pointer<database_iterator_t> iter);

/// Dart signature for database_scan_end
typedef DatabaseScanEnd = void Function(Pointer<database_iterator_t> iter);

// ============================================================
// C TYPEDEFS - GraphQL Layer
// ============================================================

/// C signature: graphql_layer_t* graphql_layer_create(
///   const char* path,
///   const graphql_layer_config_t* config
/// )
typedef GraphQLLayerCreateC = Pointer<graphql_layer_t> Function(
  Pointer<Utf8> path,
  Pointer<graphql_layer_config_t> config,
);

/// Dart signature for graphql_layer_create
typedef GraphQLLayerCreate = Pointer<graphql_layer_t> Function(
  Pointer<Utf8> path,
  Pointer<graphql_layer_config_t> config,
);

/// C signature: void graphql_layer_destroy(graphql_layer_t* layer)
typedef GraphQLLayerDestroyC = Void Function(Pointer<graphql_layer_t> layer);

/// Dart signature for graphql_layer_destroy
typedef GraphQLLayerDestroy = void Function(Pointer<graphql_layer_t> layer);

/// C signature: graphql_layer_config_t* graphql_layer_config_default()
typedef GraphQLLayerConfigDefaultC = Pointer<graphql_layer_config_t> Function();

/// Dart signature for graphql_layer_config_default
typedef GraphQLLayerConfigDefault = Pointer<graphql_layer_config_t> Function();

/// C signature: void graphql_layer_config_destroy(graphql_layer_config_t* config)
typedef GraphQLLayerConfigDestroyC = Void Function(Pointer<graphql_layer_config_t> config);

/// Dart signature for graphql_layer_config_destroy
typedef GraphQLLayerConfigDestroy = void Function(Pointer<graphql_layer_config_t> config);

/// C signature: int graphql_schema_parse(graphql_layer_t* layer, const char* sdl)
typedef GraphQLSchemaParseC = Int32 Function(
  Pointer<graphql_layer_t> layer,
  Pointer<Utf8> sdl,
);

/// Dart signature for graphql_schema_parse
typedef GraphQLSchemaParse = int Function(
  Pointer<graphql_layer_t> layer,
  Pointer<Utf8> sdl,
);

/// C signature: graphql_result_t* graphql_query_sync(graphql_layer_t* layer, const char* query)
typedef GraphQLQuerySyncC = Pointer<graphql_result_t> Function(
  Pointer<graphql_layer_t> layer,
  Pointer<Utf8> query,
);

/// Dart signature for graphql_query_sync
typedef GraphQLQuerySync = Pointer<graphql_result_t> Function(
  Pointer<graphql_layer_t> layer,
  Pointer<Utf8> query,
);

/// C signature: graphql_result_t* graphql_mutate_sync(graphql_layer_t* layer, const char* mutation)
typedef GraphQLMutateSyncC = Pointer<graphql_result_t> Function(
  Pointer<graphql_layer_t> layer,
  Pointer<Utf8> mutation,
);

/// Dart signature for graphql_mutate_sync
typedef GraphQLMutateSync = Pointer<graphql_result_t> Function(
  Pointer<graphql_layer_t> layer,
  Pointer<Utf8> mutation,
);

/// C signature: void graphql_result_destroy(graphql_result_t* result)
typedef GraphQLResultDestroyC = Void Function(Pointer<graphql_result_t> result);

/// Dart signature for graphql_result_destroy
typedef GraphQLResultDestroy = void Function(Pointer<graphql_result_t> result);

/// C signature: const char* graphql_result_to_json(graphql_result_t* result)
typedef GraphQLResultToJsonC = Pointer<Utf8> Function(Pointer<graphql_result_t> result);

/// Dart signature for graphql_result_to_json
typedef GraphQLResultToJson = Pointer<Utf8> Function(Pointer<graphql_result_t> result);

// ============================================================
// C TYPEDEFS - Async GraphQL
// ============================================================

/// C signature: void graphql_query(
///   graphql_layer_t* layer,
///   const char* query,
///   promise_t* promise,
///   void* user_data
/// )
typedef GraphQLQueryAsyncC = Void Function(
  Pointer<graphql_layer_t> layer,
  Pointer<Utf8> query,
  Pointer<promise_t> promise,
  Pointer<Void> user_data,
);

/// Dart signature for graphql_query (async)
typedef GraphQLQueryAsync = void Function(
  Pointer<graphql_layer_t> layer,
  Pointer<Utf8> query,
  Pointer<promise_t> promise,
  Pointer<Void> user_data,
);

/// C signature: void graphql_mutate(
///   graphql_layer_t* layer,
///   const char* mutation,
///   promise_t* promise,
///   void* user_data
/// )
typedef GraphQLMutateAsyncC = Void Function(
  Pointer<graphql_layer_t> layer,
  Pointer<Utf8> mutation,
  Pointer<promise_t> promise,
  Pointer<Void> user_data,
);

/// Dart signature for graphql_mutate (async)
typedef GraphQLMutateAsync = void Function(
  Pointer<graphql_layer_t> layer,
  Pointer<Utf8> mutation,
  Pointer<promise_t> promise,
  Pointer<Void> user_data,
);

// ============================================================
// C TYPEDEFS - Promise
// ============================================================

/// C signature: promise_t* promise_create(
///   void (*resolve)(void*, void*),
///   void (*reject)(void*, async_error_t*),
///   void* ctx
/// )
typedef PromiseCreateC = Pointer<promise_t> Function(
  Pointer<NativeFunction<Void Function(Pointer<Void>, Pointer<Void>)>>,
  Pointer<NativeFunction<Void Function(Pointer<Void>, Pointer<async_error_t>)>>,
  Pointer<Void>,
);

/// Dart signature for promise_create
typedef PromiseCreate = Pointer<promise_t> Function(
  Pointer<NativeFunction<Void Function(Pointer<Void>, Pointer<Void>)>>,
  Pointer<NativeFunction<Void Function(Pointer<Void>, Pointer<async_error_t>)>>,
  Pointer<Void>,
);

/// C signature: void promise_destroy(promise_t* promise)
typedef PromiseDestroyC = Void Function(Pointer<promise_t> promise);

/// Dart signature for promise_destroy
typedef PromiseDestroy = void Function(Pointer<promise_t> promise);

/// C signature: void error_destroy(async_error_t* error)
typedef ErrorDestroyC = Void Function(Pointer<async_error_t> error);

/// Dart signature for error_destroy
typedef ErrorDestroy = void Function(Pointer<async_error_t> error);

// ============================================================
// WAVEDB NATIVE - FFI Bindings Wrapper
// ============================================================

/// FFI bindings wrapper for WaveDB native functions
///
/// This class provides Dart wrappers for all WaveDB C functions.
/// Function pointers are lazily loaded on first access.
class WaveDBNative {
  // Private constructor to prevent instantiation
  WaveDBNative._();

  // ============================================================
  // LAZY-LOADED FUNCTION POINTERS
  // ============================================================

  // Database lifecycle functions
  static late final DatabaseCreate _databaseCreate = WaveDBLibrary.load()
      .lookupFunction<DatabaseCreateC, DatabaseCreate>('database_create');

  static late final DatabaseDestroy _databaseDestroy = WaveDBLibrary.load()
      .lookupFunction<DatabaseDestroyC, DatabaseDestroy>('database_destroy');

  // Configuration functions
  static late final DatabaseConfigDefault _databaseConfigDefault = WaveDBLibrary.load()
      .lookupFunction<DatabaseConfigDefaultC, DatabaseConfigDefault>('database_config_default');

  static late final DatabaseConfigCopy _databaseConfigCopy = WaveDBLibrary.load()
      .lookupFunction<DatabaseConfigCopyC, DatabaseConfigCopy>('database_config_copy');

  static late final DatabaseConfigDestroy _databaseConfigDestroy = WaveDBLibrary.load()
      .lookupFunction<DatabaseConfigDestroyC, DatabaseConfigDestroy>('database_config_destroy');

  static late final DatabaseCreateWithConfig _databaseCreateWithConfig = WaveDBLibrary.load()
      .lookupFunction<DatabaseCreateWithConfigC, DatabaseCreateWithConfig>('database_create_with_config');

  // Synchronous operations
  static late final DatabasePutSync _databasePutSync = WaveDBLibrary.load()
      .lookupFunction<DatabasePutSyncC, DatabasePutSync>('database_put_sync');

  static late final DatabaseGetSync _databaseGetSync = WaveDBLibrary.load()
      .lookupFunction<DatabaseGetSyncC, DatabaseGetSync>('database_get_sync');

  static late final DatabaseDeleteSync _databaseDeleteSync = WaveDBLibrary.load()
      .lookupFunction<DatabaseDeleteSyncC, DatabaseDeleteSync>('database_delete_sync');

  // Async database operations
  static late final DatabasePutAsync _databasePutAsync = WaveDBLibrary.load()
      .lookupFunction<DatabasePutAsyncC, DatabasePutAsync>('database_put');

  static late final DatabaseGetAsync _databaseGetAsync = WaveDBLibrary.load()
      .lookupFunction<DatabaseGetAsyncC, DatabaseGetAsync>('database_get');

  static late final DatabaseDeleteAsync _databaseDeleteAsync = WaveDBLibrary.load()
      .lookupFunction<DatabaseDeleteAsyncC, DatabaseDeleteAsync>('database_delete');

  // Batch operations
  static late final BatchCreate _batchCreate = WaveDBLibrary.load()
      .lookupFunction<BatchCreateC, BatchCreate>('batch_create');

  static late final BatchAddPut _batchAddPut = WaveDBLibrary.load()
      .lookupFunction<BatchAddPutC, BatchAddPut>('batch_add_put');

  static late final BatchAddDelete _batchAddDelete = WaveDBLibrary.load()
      .lookupFunction<BatchAddDeleteC, BatchAddDelete>('batch_add_delete');

  static late final BatchDestroy _batchDestroy = WaveDBLibrary.load()
      .lookupFunction<BatchDestroyC, BatchDestroy>('batch_destroy');

  static late final DatabaseWriteBatchAsync _databaseWriteBatchAsync = WaveDBLibrary.load()
      .lookupFunction<DatabaseWriteBatchAsyncC, DatabaseWriteBatchAsync>('database_write_batch');

  // Path operations
  static late final PathCreate _pathCreate = WaveDBLibrary.load()
      .lookupFunction<PathCreateC, PathCreate>('path_create');

  static late final PathAppend _pathAppend = WaveDBLibrary.load()
      .lookupFunction<PathAppendC, PathAppend>('path_append');

  static late final PathDestroy _pathDestroy = WaveDBLibrary.load()
      .lookupFunction<PathDestroyC, PathDestroy>('path_destroy');

  static late final PathLength _pathLength = WaveDBLibrary.load()
      .lookupFunction<PathLengthC, PathLength>('path_length');

  static late final PathGet _pathGet = WaveDBLibrary.load()
      .lookupFunction<PathGetC, PathGet>('path_get');

  // Buffer operations
  static late final BufferCreateFromPointerCopy _bufferCreateFromPointerCopy =
      WaveDBLibrary.load()
          .lookupFunction<BufferCreateFromPointerCopyC, BufferCreateFromPointerCopy>(
              'buffer_create_from_pointer_copy');

  static late final BufferDestroy _bufferDestroy = WaveDBLibrary.load()
      .lookupFunction<BufferDestroyC, BufferDestroy>('buffer_destroy');

  // Identifier operations
  static late final IdentifierCreate _identifierCreate = WaveDBLibrary.load()
      .lookupFunction<IdentifierCreateC, IdentifierCreate>('identifier_create');

  static late final IdentifierDestroy _identifierDestroy = WaveDBLibrary.load()
      .lookupFunction<IdentifierDestroyC, IdentifierDestroy>('identifier_destroy');

  static late final IdentifierToBuffer _identifierToBuffer = WaveDBLibrary.load()
      .lookupFunction<IdentifierToBufferC, IdentifierToBuffer>('identifier_to_buffer');

  // Iterator operations
  static late final DatabaseScanStart _databaseScanStart = WaveDBLibrary.load()
      .lookupFunction<DatabaseScanStartC, DatabaseScanStart>('database_scan_start');

  static late final DatabaseScanNext _databaseScanNext = WaveDBLibrary.load()
      .lookupFunction<DatabaseScanNextC, DatabaseScanNext>('database_scan_next');

  static late final DatabaseScanEnd _databaseScanEnd = WaveDBLibrary.load()
      .lookupFunction<DatabaseScanEndC, DatabaseScanEnd>('database_scan_end');

  // GraphQL layer operations
  static late final GraphQLLayerCreate _graphQLLayerCreate = WaveDBLibrary.load()
      .lookupFunction<GraphQLLayerCreateC, GraphQLLayerCreate>('graphql_layer_create');

  static late final GraphQLLayerDestroy _graphQLLayerDestroy = WaveDBLibrary.load()
      .lookupFunction<GraphQLLayerDestroyC, GraphQLLayerDestroy>('graphql_layer_destroy');

  static late final GraphQLLayerConfigDefault _graphQLLayerConfigDefault = WaveDBLibrary.load()
      .lookupFunction<GraphQLLayerConfigDefaultC, GraphQLLayerConfigDefault>('graphql_layer_config_default');

  static late final GraphQLLayerConfigDestroy _graphQLLayerConfigDestroy = WaveDBLibrary.load()
      .lookupFunction<GraphQLLayerConfigDestroyC, GraphQLLayerConfigDestroy>('graphql_layer_config_destroy');

  static late final GraphQLSchemaParse _graphQLSchemaParse = WaveDBLibrary.load()
      .lookupFunction<GraphQLSchemaParseC, GraphQLSchemaParse>('graphql_schema_parse');

  static late final GraphQLQuerySync _graphQLQuerySync = WaveDBLibrary.load()
      .lookupFunction<GraphQLQuerySyncC, GraphQLQuerySync>('graphql_query_sync');

  static late final GraphQLMutateSync _graphQLMutateSync = WaveDBLibrary.load()
      .lookupFunction<GraphQLMutateSyncC, GraphQLMutateSync>('graphql_mutate_sync');

  static late final GraphQLResultDestroy _graphQLResultDestroy = WaveDBLibrary.load()
      .lookupFunction<GraphQLResultDestroyC, GraphQLResultDestroy>('graphql_result_destroy');

  static late final GraphQLResultToJson _graphQLResultToJson = WaveDBLibrary.load()
      .lookupFunction<GraphQLResultToJsonC, GraphQLResultToJson>('graphql_result_to_json');

  // Async GraphQL operations
  static late final GraphQLQueryAsync _graphQLQueryAsync = WaveDBLibrary.load()
      .lookupFunction<GraphQLQueryAsyncC, GraphQLQueryAsync>('graphql_query');

  static late final GraphQLMutateAsync _graphQLMutateAsync = WaveDBLibrary.load()
      .lookupFunction<GraphQLMutateAsyncC, GraphQLMutateAsync>('graphql_mutate');

  // Promise operations
  static late final PromiseCreate _promiseCreate = WaveDBLibrary.load()
      .lookupFunction<PromiseCreateC, PromiseCreate>('promise_create');

  static late final PromiseDestroy _promiseDestroy = WaveDBLibrary.load()
      .lookupFunction<PromiseDestroyC, PromiseDestroy>('promise_destroy');

  static late final ErrorDestroy _errorDestroy = WaveDBLibrary.load()
      .lookupFunction<ErrorDestroyC, ErrorDestroy>('error_destroy');

  // ============================================================
  // PUBLIC API - Database Lifecycle
  // ============================================================

  /// Create a new database instance
  ///
  /// [path] - Filesystem path for the database
  /// [lruMemoryMb] - LRU cache memory budget in MB (0 = default 50 MB)
  /// [chunkSize] - HBTrie chunk size (0 = default)
  /// [btreeNodeSize] - B-tree node size (0 = default)
  /// [enablePersist] - Enable persistent storage (0 = in-memory only, 1 = persistent)
  /// [storageCacheSize] - Section LRU cache size (0 = default)
  ///
  /// Returns a pointer to the database handle.
  /// Throws [WaveDBException] if creation fails.
  static Pointer<database_t> databaseCreate(
    String path, {
    int lruMemoryMb = 0,
    int chunkSize = 0,
    int btreeNodeSize = 0,
    int enablePersist = 1,
    int storageCacheSize = 0,
  }) {
    final pathPtr = path.toNativeUtf8();
    final errorPtr = calloc<Int32>();

    try {
      final db = _databaseCreate(
        pathPtr.cast(),
        lruMemoryMb,
        nullptr, // wal_config
        chunkSize,
        btreeNodeSize,
        enablePersist,
        storageCacheSize,
        nullptr, // pool
        nullptr, // wheel
        errorPtr,
      );

      if (db == nullptr) {
        final errorCode = errorPtr.value;
        throw WaveDBException.ioError(
          'Failed to create database',
          'Error code: $errorCode',
        );
      }

      return db;
    } finally {
      calloc.free(pathPtr);
      calloc.free(errorPtr);
    }
  }

  /// Create a database with full configuration
  ///
  /// [path] - Filesystem path for the database
  /// [config] - Configuration options (all optional)
  ///   - chunkSize: HBTrie chunk size (default: 4)
  ///   - btreeNodeSize: B+tree node size (default: 4096)
  ///   - enablePersist: Enable persistence (default: true)
  ///   - lruMemoryMb: LRU cache size in MB (default: 50)
  ///   - lruShards: LRU cache shard count, 0 for auto (default: 64)
  ///   - storageCacheSize: Section cache size (default: 1024)
  ///   - workerThreads: Number of worker threads (default: 4)
  ///   - walSyncMode: WAL sync mode: 'immediate', 'debounced', 'async' (default: 'debounced')
  ///   - walDebounceMs: Debounce window for fsync (default: 100)
  ///   - walMaxFileSize: Max WAL file size (default: 131072)
  ///
  /// Returns a pointer to the database handle.
  /// Throws [WaveDBException] if creation fails.
  static Pointer<database_t> databaseCreateWithConfig(
    String path, {
    int? chunkSize,
    int? btreeNodeSize,
    bool? enablePersist,
    int? lruMemoryMb,
    int? lruShards,
    int? storageCacheSize,
    int? workerThreads,
    String? walSyncMode,
    int? walDebounceMs,
    int? walMaxFileSize,
  }) {
    final pathPtr = path.toNativeUtf8();
    final errorPtr = calloc<Int32>();

    try {
      // Get default config
      final configPtr = _databaseConfigDefault();
      if (configPtr == nullptr) {
        throw WaveDBException.ioError('database_config_default', 'Failed to create default config');
      }

      // Apply overrides (note: we can't directly modify the struct from Dart,
      // so we use the legacy database_create for now)
      // TODO: Add native config setters or use struct layout

      final db = _databaseCreateWithConfig(
        pathPtr.cast(),
        configPtr,
        errorPtr,
      );

      _databaseConfigDestroy(configPtr);

      if (db == nullptr) {
        final errorCode = errorPtr.value;
        throw WaveDBException.ioError(
          'database_create_with_config',
          'Error code: $errorCode',
        );
      }

      return db;
    } finally {
      calloc.free(pathPtr);
      calloc.free(errorPtr);
    }
  }

  /// Destroy a database instance and free all associated resources
  ///
  /// [db] - Pointer to the database handle
  static void databaseDestroy(Pointer<database_t> db) {
    _databaseDestroy(db);
  }

  // ============================================================
  // PUBLIC API - Synchronous Operations
  // ============================================================

  /// Synchronously put a value at the given path
  ///
  /// [db] - Database handle
  /// [path] - Path handle (created with pathCreate and pathAppend)
  /// [value] - Value handle (created with identifierCreate)
  ///
  /// Returns 0 on success, non-zero on failure.
  static int databasePutSync(
    Pointer<database_t> db,
    Pointer<path_t> path,
    Pointer<identifier_t> value,
  ) {
    return _databasePutSync(db, path, value);
  }

  /// Synchronously get a value at the given path
  ///
  /// [db] - Database handle
  /// [path] - Path handle
  /// [result] - Output pointer for the result identifier
  ///
  /// Returns 0 on success, non-zero on failure.
  /// The caller is responsible for destroying the result identifier.
  static int databaseGetSync(
    Pointer<database_t> db,
    Pointer<path_t> path,
    Pointer<Pointer<identifier_t>> result,
  ) {
    return _databaseGetSync(db, path, result);
  }

  /// Synchronously delete a value at the given path
  ///
  /// [db] - Database handle
  /// [path] - Path handle
  ///
  /// Returns 0 on success, non-zero on failure.
  static int databaseDeleteSync(
    Pointer<database_t> db,
    Pointer<path_t> path,
  ) {
    return _databaseDeleteSync(db, path);
  }

  // ============================================================
  // PUBLIC API - Async Database Operations
  // ============================================================

  /// Asynchronously put a value at the given path (dispatches to C worker pool)
  static void databasePutAsync(
    Pointer<database_t> db,
    Pointer<path_t> path,
    Pointer<identifier_t> value,
    Pointer<promise_t> promise,
  ) {
    _databasePutAsync(db, path, value, promise);
  }

  /// Asynchronously get a value at the given path (dispatches to C worker pool)
  static void databaseGetAsync(
    Pointer<database_t> db,
    Pointer<path_t> path,
    Pointer<promise_t> promise,
  ) {
    _databaseGetAsync(db, path, promise);
  }

  /// Asynchronously delete a value at the given path (dispatches to C worker pool)
  static void databaseDeleteAsync(
    Pointer<database_t> db,
    Pointer<path_t> path,
    Pointer<promise_t> promise,
  ) {
    _databaseDeleteAsync(db, path, promise);
  }

  // ============================================================
  // PUBLIC API - Batch Operations
  // ============================================================

  /// Create a write batch
  static Pointer<batch_t> batchCreate([int reserveCount = 0]) {
    return _batchCreate(reserveCount);
  }

  /// Add a PUT operation to a batch (ownership transfers on success)
  static int batchAddPut(
    Pointer<batch_t> batch,
    Pointer<path_t> path,
    Pointer<identifier_t> value,
  ) {
    return _batchAddPut(batch, path, value);
  }

  /// Add a DELETE operation to a batch (ownership transfers on success)
  static int batchAddDelete(
    Pointer<batch_t> batch,
    Pointer<path_t> path,
  ) {
    return _batchAddDelete(batch, path);
  }

  /// Destroy a batch
  static void batchDestroy(Pointer<batch_t> batch) {
    _batchDestroy(batch);
  }

  /// Asynchronously submit a write batch (dispatches to C worker pool)
  static void databaseWriteBatchAsync(
    Pointer<database_t> db,
    Pointer<batch_t> batch,
    Pointer<promise_t> promise,
  ) {
    _databaseWriteBatchAsync(db, batch, promise);
  }

  // ============================================================
  // PUBLIC API - Path Operations
  // ============================================================

  /// Create a new empty path
  ///
  /// Returns a pointer to the path handle.
  /// The caller is responsible for destroying the path.
  static Pointer<path_t> pathCreate() {
    return _pathCreate();
  }

  /// Append an identifier to a path
  ///
  /// [path] - Path handle
  /// [identifier] - Identifier handle to append
  ///
  /// Returns 0 on success, non-zero on failure.
  /// Note: The path takes ownership of the identifier.
  static int pathAppend(
    Pointer<path_t> path,
    Pointer<identifier_t> identifier,
  ) {
    return _pathAppend(path, identifier);
  }

  /// Destroy a path and free all associated resources
  ///
  /// [path] - Path handle to destroy
  static void pathDestroy(Pointer<path_t> path) {
    _pathDestroy(path);
  }

  /// Get the number of identifiers in a path
  ///
  /// [path] - Path handle
  ///
  /// Returns the number of identifiers in the path.
  static int pathLength(Pointer<path_t> path) {
    return _pathLength(path);
  }

  /// Get an identifier at a specific index in the path
  ///
  /// [path] - Path handle
  /// [index] - Index of the identifier to retrieve
  ///
  /// Returns a pointer to the identifier handle.
  /// Note: The returned identifier is owned by the path and should not be destroyed.
  static Pointer<identifier_t> pathGet(Pointer<path_t> path, int index) {
    return _pathGet(path, index);
  }

  // ============================================================
  // PUBLIC API - Buffer Operations
  // ============================================================

  /// Create a buffer from raw data (copies the data)
  ///
  /// [data] - Pointer to the data bytes
  /// [size] - Size of the data
  ///
  /// Returns a pointer to the buffer handle.
  /// The caller is responsible for destroying the buffer using bufferDestroy.
  static Pointer<buffer_t> bufferCreateFromPointerCopy(
    Pointer<Uint8> data,
    int size,
  ) {
    return _bufferCreateFromPointerCopy(data, size);
  }

  /// Destroy a buffer and free all associated resources
  ///
  /// [buf] - Buffer handle to destroy
  static void bufferDestroy(Pointer<buffer_t> buf) {
    _bufferDestroy(buf);
  }

  // ============================================================
  // PUBLIC API - Identifier Operations
  // ============================================================

  /// Create an identifier from raw data
  ///
  /// [data] - Pointer to the data bytes
  /// [length] - Length of the data
  /// [chunkSize] - Chunk size (0 = default 4 bytes)
  ///
  /// Returns a pointer to the identifier handle.
  /// The caller is responsible for destroying the identifier.
  static Pointer<identifier_t> identifierCreate(
    Pointer<Uint8> data,
    int length, [
    int chunkSize = 0,
  ]) {
    // Create a buffer from the data
    final buffer = _bufferCreateFromPointerCopy(data, length);
    if (buffer == nullptr) {
      return nullptr;
    }

    try {
      // Create the identifier from the buffer
      return _identifierCreate(buffer, chunkSize);
    } finally {
      // identifier_create takes ownership of the buffer data, but we still need
      // to destroy the buffer struct itself
      _bufferDestroy(buffer);
    }
  }

  /// Destroy an identifier and free all associated resources
  ///
  /// [id] - Identifier handle to destroy
  static void identifierDestroy(Pointer<identifier_t> id) {
    _identifierDestroy(id);
  }

  /// Convert an identifier to a buffer containing the original data
  ///
  /// [id] - Identifier handle
  ///
  /// Returns a pointer to the buffer handle.
  /// The caller is responsible for destroying the buffer using bufferDestroy.
  static Pointer<buffer_t> identifierToBuffer(Pointer<identifier_t> id) {
    return _identifierToBuffer(id);
  }

  // ============================================================
  // PUBLIC API - Iterator Operations
  // ============================================================

  /// Start a database scan (iteration)
  ///
  /// [db] - Database handle
  /// [startPath] - Optional start path (nullptr for beginning)
  /// [endPath] - Optional end path (nullptr for no upper bound)
  ///
  /// Returns a pointer to the iterator handle.
  /// The caller is responsible for calling databaseScanEnd.
  static Pointer<database_iterator_t> databaseScanStart(
    Pointer<database_t> db,
    Pointer<path_t>? startPath,
    Pointer<path_t>? endPath,
  ) {
    return _databaseScanStart(
      db,
      startPath ?? nullptr,
      endPath ?? nullptr,
    );
  }

  /// Get the next entry from an iterator
  ///
  /// [iter] - Iterator handle
  /// [outPath] - Output pointer for the path
  /// [outValue] - Output pointer for the value
  ///
  /// Returns 0 on success with data, non-zero when iteration is complete.
  /// The caller is responsible for destroying outPath and outValue.
  static int databaseScanNext(
    Pointer<database_iterator_t> iter,
    Pointer<Pointer<path_t>> outPath,
    Pointer<Pointer<identifier_t>> outValue,
  ) {
    return _databaseScanNext(iter, outPath, outValue);
  }

  /// End a database scan and free the iterator
  ///
  /// [iter] - Iterator handle to destroy
  static void databaseScanEnd(Pointer<database_iterator_t> iter) {
    _databaseScanEnd(iter);
  }

  // ============================================================
  // PUBLIC API - GraphQL Layer
  // ============================================================

  /// Create a GraphQL layer with its own database
  ///
  /// [path] - Database storage path (null for in-memory)
  /// [config] - Configuration (nullptr for defaults)
  ///
  /// Returns a pointer to the GraphQL layer handle.
  static Pointer<graphql_layer_t> graphQLLayerCreate(
    String? path, {
    Pointer<graphql_layer_config_t>? config,
  }) {
    final pathPtr = path != null ? path.toNativeUtf8() : nullptr;
    try {
      final layer = _graphQLLayerCreate(
        pathPtr.cast(),
        config ?? nullptr,
      );
      if (layer == nullptr) {
        throw WaveDBException.ioError('graphql_layer_create', 'Failed to create GraphQL layer');
      }
      return layer;
    } finally {
      if (pathPtr != nullptr) {
        calloc.free(pathPtr);
      }
    }
  }

  /// Destroy a GraphQL layer and free all resources
  static void graphQLLayerDestroy(Pointer<graphql_layer_t> layer) {
    _graphQLLayerDestroy(layer);
  }

  /// Get default GraphQL layer configuration
  static Pointer<graphql_layer_config_t> graphQLLayerConfigDefault() {
    return _graphQLLayerConfigDefault();
  }

  /// Destroy a GraphQL layer configuration
  static void graphQLLayerConfigDestroy(Pointer<graphql_layer_config_t> config) {
    _graphQLLayerConfigDestroy(config);
  }

  /// Parse a GraphQL schema definition (SDL)
  ///
  /// Returns 0 on success, non-zero on error.
  static int graphQLSchemaParse(
    Pointer<graphql_layer_t> layer,
    String sdl,
  ) {
    final sdlPtr = sdl.toNativeUtf8();
    try {
      return _graphQLSchemaParse(layer, sdlPtr.cast());
    } finally {
      calloc.free(sdlPtr);
    }
  }

  /// Execute a GraphQL query synchronously
  ///
  /// Returns a pointer to the result. Caller must destroy with graphQLResultDestroy.
  static Pointer<graphql_result_t> graphQLQuerySync(
    Pointer<graphql_layer_t> layer,
    String query,
  ) {
    final queryPtr = query.toNativeUtf8();
    try {
      return _graphQLQuerySync(layer, queryPtr.cast());
    } finally {
      calloc.free(queryPtr);
    }
  }

  /// Execute a GraphQL mutation synchronously
  ///
  /// Returns a pointer to the result. Caller must destroy with graphQLResultDestroy.
  static Pointer<graphql_result_t> graphQLMutateSync(
    Pointer<graphql_layer_t> layer,
    String mutation,
  ) {
    final mutationPtr = mutation.toNativeUtf8();
    try {
      return _graphQLMutateSync(layer, mutationPtr.cast());
    } finally {
      calloc.free(mutationPtr);
    }
  }

  /// Destroy a GraphQL result
  static void graphQLResultDestroy(Pointer<graphql_result_t> result) {
    _graphQLResultDestroy(result);
  }

  /// Convert a GraphQL result to a JSON string
  ///
  /// Returns a pointer to a null-terminated UTF-8 string.
  /// Caller must call malloc.free() on the returned pointer.
  static Pointer<Utf8> graphQLResultToJson(Pointer<graphql_result_t> result) {
    return _graphQLResultToJson(result);
  }

  // ============================================================
  // PUBLIC API - Async GraphQL
  // ============================================================

  /// Execute a GraphQL query asynchronously via the C worker pool
  ///
  /// The caller provides a promise with resolve/reject callbacks.
  static void graphQLQueryAsync(
    Pointer<graphql_layer_t> layer,
    String query,
    Pointer<promise_t> promise,
  ) {
    final queryPtr = query.toNativeUtf8();
    try {
      _graphQLQueryAsync(layer, queryPtr.cast(), promise, nullptr);
    } finally {
      calloc.free(queryPtr);
    }
  }

  /// Execute a GraphQL mutation asynchronously via the C worker pool
  ///
  /// The caller provides a promise with resolve/reject callbacks.
  static void graphQLMutateAsync(
    Pointer<graphql_layer_t> layer,
    String mutation,
    Pointer<promise_t> promise,
  ) {
    final mutationPtr = mutation.toNativeUtf8();
    try {
      _graphQLMutateAsync(layer, mutationPtr.cast(), promise, nullptr);
    } finally {
      calloc.free(mutationPtr);
    }
  }

  // ============================================================
  // PUBLIC API - Promise
  // ============================================================

  /// Create a C promise with the given resolve/reject callbacks and context
  static Pointer<promise_t> promiseCreate(
    Pointer<NativeFunction<Void Function(Pointer<Void>, Pointer<Void>)>> resolve,
    Pointer<NativeFunction<Void Function(Pointer<Void>, Pointer<async_error_t>)>> reject,
    Pointer<Void> ctx,
  ) {
    return _promiseCreate(resolve, reject, ctx);
  }

  /// Destroy a C promise
  static void promiseDestroy(Pointer<promise_t> promise) {
    _promiseDestroy(promise);
  }

  /// Destroy a C async error
  static void errorDestroy(Pointer<async_error_t> error) {
    _errorDestroy(error);
  }
}