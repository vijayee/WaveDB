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
///   const char* path,
///   uint32_t chunk_size,
///   void* options,
///   uint32_t btree_node_size,
///   uint32_t wal_enabled,
///   uint32_t snapshot_enabled,
///   void* callback,
///   int32_t* error_code
/// )
typedef DatabaseCreateC = Pointer<database_t> Function(
  Pointer<Utf8> path,
  Uint32 chunk_size,
  Pointer<Void> options,
  Uint32 btree_node_size,
  Uint32 wal_enabled,
  Uint32 snapshot_enabled,
  Pointer<Void> callback,
  Pointer<Int32> error_code,
);

/// Dart signature for database_create
typedef DatabaseCreate = Pointer<database_t> Function(
  Pointer<Utf8> path,
  int chunk_size,
  Pointer<Void> options,
  int btree_node_size,
  int wal_enabled,
  int snapshot_enabled,
  Pointer<Void> callback,
  Pointer<Int32> error_code,
);

/// C signature: void database_destroy(database_t* db)
typedef DatabaseDestroyC = Void Function(Pointer<database_t> db);

/// Dart signature for database_destroy
typedef DatabaseDestroy = void Function(Pointer<database_t> db);

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

// ============================================================
// C TYPEDEFS - Identifier Operations
// ============================================================

/// C signature: identifier_t* identifier_create(
///   const uint8_t* data,
///   uint32_t length
/// )
typedef IdentifierCreateC = Pointer<identifier_t> Function(
  Pointer<Uint8> data,
  Uint32 length,
);

/// Dart signature for identifier_create
typedef IdentifierCreate = Pointer<identifier_t> Function(
  Pointer<Uint8> data,
  int length,
);

/// C signature: void identifier_destroy(identifier_t* id)
typedef IdentifierDestroyC = Void Function(Pointer<identifier_t> id);

/// Dart signature for identifier_destroy
typedef IdentifierDestroy = void Function(Pointer<identifier_t> id);

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

  // Synchronous operations
  static late final DatabasePutSync _databasePutSync = WaveDBLibrary.load()
      .lookupFunction<DatabasePutSyncC, DatabasePutSync>('database_put_sync');

  static late final DatabaseGetSync _databaseGetSync = WaveDBLibrary.load()
      .lookupFunction<DatabaseGetSyncC, DatabaseGetSync>('database_get_sync');

  static late final DatabaseDeleteSync _databaseDeleteSync = WaveDBLibrary.load()
      .lookupFunction<DatabaseDeleteSyncC, DatabaseDeleteSync>('database_delete_sync');

  // Path operations
  static late final PathCreate _pathCreate = WaveDBLibrary.load()
      .lookupFunction<PathCreateC, PathCreate>('path_create');

  static late final PathAppend _pathAppend = WaveDBLibrary.load()
      .lookupFunction<PathAppendC, PathAppend>('path_append');

  static late final PathDestroy _pathDestroy = WaveDBLibrary.load()
      .lookupFunction<PathDestroyC, PathDestroy>('path_destroy');

  // Identifier operations
  static late final IdentifierCreate _identifierCreate = WaveDBLibrary.load()
      .lookupFunction<IdentifierCreateC, IdentifierCreate>('identifier_create');

  static late final IdentifierDestroy _identifierDestroy = WaveDBLibrary.load()
      .lookupFunction<IdentifierDestroyC, IdentifierDestroy>('identifier_destroy');

  // Iterator operations
  static late final DatabaseScanStart _databaseScanStart = WaveDBLibrary.load()
      .lookupFunction<DatabaseScanStartC, DatabaseScanStart>('database_scan_start');

  static late final DatabaseScanNext _databaseScanNext = WaveDBLibrary.load()
      .lookupFunction<DatabaseScanNextC, DatabaseScanNext>('database_scan_next');

  static late final DatabaseScanEnd _databaseScanEnd = WaveDBLibrary.load()
      .lookupFunction<DatabaseScanEndC, DatabaseScanEnd>('database_scan_end');

  // ============================================================
  // PUBLIC API - Database Lifecycle
  // ============================================================

  /// Create a new database instance
  ///
  /// [path] - Filesystem path for the database
  /// [chunkSize] - Chunk size (0 = default)
  /// [btreeNodeSize] - B-tree node size (0 = default)
  /// [walEnabled] - Enable write-ahead logging (0 = disabled)
  /// [snapshotEnabled] - Enable snapshots (1 = enabled)
  ///
  /// Returns a pointer to the database handle.
  /// Throws [WaveDBException] if creation fails.
  static Pointer<database_t> databaseCreate(
    String path, {
    int chunkSize = 0,
    int btreeNodeSize = 0,
    int walEnabled = 0,
    int snapshotEnabled = 1,
  }) {
    final pathPtr = path.toNativeUtf8();
    final errorPtr = calloc<Int32>();

    try {
      final db = _databaseCreate(
        pathPtr.cast(),
        chunkSize,
        nullptr,
        btreeNodeSize,
        walEnabled,
        snapshotEnabled,
        nullptr,
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

  // ============================================================
  // PUBLIC API - Identifier Operations
  // ============================================================

  /// Create an identifier from raw data
  ///
  /// [data] - Pointer to the data bytes
  /// [length] - Length of the data
  ///
  /// Returns a pointer to the identifier handle.
  /// The caller is responsible for destroying the identifier.
  static Pointer<identifier_t> identifierCreate(
    Pointer<Uint8> data,
    int length,
  ) {
    return _identifierCreate(data, length);
  }

  /// Destroy an identifier and free all associated resources
  ///
  /// [id] - Identifier handle to destroy
  static void identifierDestroy(Pointer<identifier_t> id) {
    _identifierDestroy(id);
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
}