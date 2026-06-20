"""cffi native library loader for libwavedb."""
from __future__ import annotations

import ctypes.util
import os
import sys
from pathlib import Path

from cffi import FFI


ffi = FFI()

# ---- Types ----
# All WaveDB structs are declared opaque via forward declarations
# (`typedef struct foo_s foo_t;`). We only ever hold pointers to them
# from Python. cffi ABI mode (ffi.dlopen) has no C compiler available,
# so struct layouts cannot be inferred from the shared library.
#
# IMPORTANT: cffi distinguishes between "partial" structs
# (`typedef struct { ...; } foo_t;`) and "opaque" forward-declared
# structs (`typedef struct foo_s foo_t;`). Partial structs CANNOT be
# used in function signatures — accessing lib.foo where foo takes or
# returns a partial struct raises VerificationMissing. Forward-declared
# opaque structs work fine in function signatures, so we use that form
# exclusively for all opaque WaveDB types.
ffi.cdef("""
typedef struct refcounter_s refcounter_t;
typedef struct database_s database_t;
typedef struct database_config_s database_config_t;
typedef struct encrypted_database_config_s encrypted_database_config_t;
typedef struct promise_s promise_t;
typedef struct async_error_s async_error_t;
typedef struct database_iterator_s database_iterator_t;
typedef struct path_s path_t;
typedef struct identifier_s identifier_t;
typedef struct database_subtree_s database_subtree_t;
typedef struct graph_layer_config_s graph_layer_config_t;
typedef struct graph_layer_s graph_layer_t;
typedef struct graph_query_s graph_query_t;
typedef struct graph_result_s graph_result_t;
typedef struct graphql_layer_config_s graphql_layer_config_t;
typedef struct graphql_layer_s graphql_layer_t;
typedef struct graphql_result_s graphql_result_t;

typedef struct {
    const char* key;
    size_t key_len;
    const uint8_t* value;
    size_t value_len;
    int type;
} raw_op_t;

typedef struct {
    char* key;
    size_t key_len;
    uint8_t* value;
    size_t value_len;
} raw_result_t;
""")

# Identifier accessors + refcounter primitives used to decode the
# `identifier_t*` payload that `database_get_raw` resolves with. The
# payload arrives with yield=1 (the C worker called CONSUME before
# promise_resolve), so we must REFERENCE then identifier_destroy to
# decrement the count to zero and actually free the object.
ffi.cdef("""
uint8_t* identifier_get_data_copy(const identifier_t* id, size_t* out_len);
void identifier_destroy(identifier_t* id);
void* refcounter_reference(void* refcounter);
""")

# ---- Database lifecycle ----
# NOTE: database_create takes typed pointers (wal_config_t*, work_pool_t*,
# hierarchical_timing_wheel_t*) in the real header; we declare them as
# void* here because we never call database_create from Python (we use
# database_create_with_config) and the underlying struct types are opaque.
ffi.cdef("""
/* unused in v1 — Python uses database_create_with_config; kept for future direct-pool/wheel callers */
database_t* database_create(const char* location, size_t lru_memory_mb,
    void* wal_config, uint8_t chunk_size, uint32_t btree_node_size,
    uint8_t enable_persist, void* pool, void* wheel, int* error_code);
database_t* database_create_with_config(const char* location,
    database_config_t* config, int* error_code);
database_t* database_create_encrypted(const char* location,
    encrypted_database_config_t* config, int* error_code);
void database_destroy(database_t* db);

database_config_t* database_config_default(void);
database_config_t* database_config_copy(const database_config_t* config);
void database_config_destroy(database_config_t* config);
void database_config_set_chunk_size(database_config_t*, uint8_t);
void database_config_set_btree_node_size(database_config_t*, uint32_t);
void database_config_set_enable_persist(database_config_t*, uint8_t);
void database_config_set_lru_memory_mb(database_config_t*, size_t);
void database_config_set_lru_shards(database_config_t*, uint16_t);
void database_config_set_wal_sync_mode(database_config_t*, uint8_t);
void database_config_set_wal_debounce_ms(database_config_t*, uint64_t);
void database_config_set_worker_threads(database_config_t*, uint8_t);
void database_config_set_sync_only(database_config_t*, uint8_t);

encrypted_database_config_t* encrypted_database_config_default(void);
void encrypted_database_config_destroy(encrypted_database_config_t*);
void encrypted_database_config_set_type(encrypted_database_config_t*, int);
void encrypted_database_config_set_symmetric_key(encrypted_database_config_t*, const uint8_t*, size_t);
void encrypted_database_config_set_asymmetric_private_key(encrypted_database_config_t*, const uint8_t*, size_t);
void encrypted_database_config_set_asymmetric_public_key(encrypted_database_config_t*, const uint8_t*, size_t);
""")

# ---- Sync raw API ----
ffi.cdef("""
int database_put_sync_raw(database_t* db,
    const char* key, size_t key_len, char delimiter,
    const uint8_t* value, size_t value_len);
int database_get_sync_raw(database_t* db,
    const char* key, size_t key_len, char delimiter,
    uint8_t** value_out, size_t* value_len_out);
int database_delete_sync_raw(database_t* db,
    const char* key, size_t key_len, char delimiter);
void database_raw_value_free(uint8_t* value);
int database_batch_sync_raw(database_t* db, char delimiter,
    const raw_op_t* ops, size_t count);
int database_scan_sync_raw(database_t* db,
    const char* prefix, size_t prefix_len, char delimiter,
    raw_result_t** results, size_t* count);
int database_scan_range_sync_raw(database_t* db,
    const char* start_prefix, size_t start_prefix_len,
    const char* end_prefix, size_t end_prefix_len,
    char delimiter,
    raw_result_t** results, size_t* count);
void database_raw_results_free(raw_result_t* results, size_t count);
""")

# ---- Async raw API ----
ffi.cdef("""
int database_put_raw(database_t* db,
    const char* key, size_t key_len, char delimiter,
    const uint8_t* value, size_t value_len, promise_t* promise);
int database_get_raw(database_t* db,
    const char* key, size_t key_len, char delimiter, promise_t* promise);
int database_delete_raw(database_t* db,
    const char* key, size_t key_len, char delimiter, promise_t* promise);
int database_batch_raw(database_t* db, char delimiter,
    const raw_op_t* ops, size_t count, promise_t* promise);
""")

# ---- Promise + error ----
ffi.cdef("""
typedef void (*promise_resolve_cb)(void* ctx, void* payload);
typedef void (*promise_reject_cb)(void* ctx, async_error_t* error);
promise_t* promise_create(promise_resolve_cb resolve, promise_reject_cb reject, void* ctx);
void promise_destroy(promise_t* promise);
void promise_resolve(promise_t* promise, void* payload);
void promise_reject(promise_t* promise, async_error_t* error);
const char* error_get_message(async_error_t* error);
void error_destroy(async_error_t* error);
""")

# ---- Iterator ----
# Declared for completeness; v1 iterator bindings use the string-based
# database_scan_range_sync_raw above. This block parses fine even though
# path_t/identifier_t are opaque (we only hold pointers).
ffi.cdef("""
/* unused in v1 — Task 10 iterator uses database_scan_range_sync_raw (string-based); these path-typed iterators are kept for future migration */
database_iterator_t* database_scan_start(database_t* db,
    path_t* start_path, path_t* end_path);
int database_scan_next(database_iterator_t* iter,
    path_t** out_path, identifier_t** out_value);
void database_scan_end(database_iterator_t* iter);
""")

# ---- Subtree ----
# NOTE: header signatures are database_subtree_open(db, prefix, delimiter)
# and database_subtree_close(subtree) — no prefix_len, and the destroy
# function is named _close. The plan's _destroy / prefix_len variants do
# not match the actual header.
ffi.cdef("""
database_subtree_t* database_subtree_open(database_t* db,
    const char* prefix, char delimiter);
int database_subtree_delete_prefix(database_t* db,
    const char* prefix, char delimiter);
void database_subtree_close(database_subtree_t* subtree);
int database_subtree_put_sync_raw(database_subtree_t* st,
    const char* key, size_t key_len, char delimiter,
    const uint8_t* value, size_t value_len);
int database_subtree_get_sync_raw(database_subtree_t* st,
    const char* key, size_t key_len, char delimiter,
    uint8_t** value_out, size_t* value_len_out);
int database_subtree_delete_sync_raw(database_subtree_t* st,
    const char* key, size_t key_len, char delimiter);
int database_subtree_put_raw(database_subtree_t* st,
    const char* key, size_t key_len, char delimiter,
    const uint8_t* value, size_t value_len, promise_t* promise);
int database_subtree_get_raw(database_subtree_t* st,
    const char* key, size_t key_len, char delimiter, promise_t* promise);
int database_subtree_delete_raw(database_subtree_t* st,
    const char* key, size_t key_len, char delimiter, promise_t* promise);
""")

# ---- Graph ----
# NOTE: graph_layer_create takes (path, config, subtree, error_code),
# not (path, db) as the plan stated. config may be NULL for defaults.
ffi.cdef("""
graph_layer_t* graph_layer_create(const char* path,
    graph_layer_config_t* config, database_subtree_t* subtree,
    int* error_code);
void graph_layer_destroy(graph_layer_t* layer);
int graph_insert_sync(graph_layer_t* layer, const char* s, const char* p, const char* o);
graph_query_t* graph_query_create(graph_layer_t* layer);
void graph_query_destroy(graph_query_t* q);
int graph_query_vertex(graph_query_t* q, const char* id);
int graph_query_out(graph_query_t* q, const char* predicate);
int graph_query_in(graph_query_t* q, const char* predicate);
int graph_query_has(graph_query_t* q, const char* predicate, const char* value);
int graph_query_intersect(graph_query_t* q, graph_query_t* left, graph_query_t* right);
int graph_query_union(graph_query_t* q, graph_query_t* left, graph_query_t* right);
int graph_query_limit(graph_query_t* q, size_t limit);
graph_result_t* graph_query_execute_sync(graph_query_t* q);
size_t graph_result_count(graph_result_t* r);
const char* const* graph_result_vertices(graph_result_t* r);
void graph_result_destroy(graph_result_t* r);
""")

# ---- GraphQL ----
# NOTE: graphql_layer_create takes (path, config, subtree, error_code),
# not (path, db). graphql_query_sync takes only (layer, query) — no
# error_out parameter. Result accessors will be filled in when the
# GraphQLLayer task needs them.
ffi.cdef("""
graphql_layer_t* graphql_layer_create(const char* path,
    const graphql_layer_config_t* config, database_subtree_t* subtree,
    int* error_code);
void graphql_layer_destroy(graphql_layer_t* layer);
int graphql_schema_parse(graphql_layer_t* layer, const char* sdl, char** error_out);
graphql_result_t* graphql_query_sync(graphql_layer_t* layer, const char* query);
void graphql_result_destroy(graphql_result_t* r);
/* Result serialization. graphql_result_to_json returns a malloc'd buffer
   in standard GraphQL response format: {"data": <node>, "errors": [...]}.
   Caller must free() the returned string. The only field-level accessor
   exposed by the C API is this JSON serializer — the Node and Dart
   bindings both use it to materialize results, so we do the same. */
const char* graphql_result_to_json(graphql_result_t* result);
/* libc free for the JSON buffer returned by graphql_result_to_json. */
void free(void* ptr);
""")


def _find_library() -> str:
    env_path = os.environ.get("WAVEDB_LIB_PATH")
    if env_path and Path(env_path).exists():
        return env_path
    try:
        from importlib.resources import files
        pkg_dir = Path(files("wavedb._lib"))
        for name in ("libwavedb.so", "libwavedb.dylib", "wavedb.dll"):
            p = pkg_dir / name
            if p.exists():
                return str(p)
    except Exception:
        pass
    found = ctypes.util.find_library("wavedb")
    if found:
        return found
    raise RuntimeError(
        "could not locate libwavedb. Set WAVEDB_LIB_PATH or install the wavedb package."
    )


lib = ffi.dlopen(_find_library())
