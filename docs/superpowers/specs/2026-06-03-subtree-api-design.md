# Subtree API Design

## Problem

The GraphQL and Graph schema layers each create their own `database_t` instance, which means each owns its own `hbtrie_t` root. They cannot share data or coexist in the same database because:

1. Two `database_t` instances cannot share a path on disk
2. Data paths collide at the trie root (e.g., GraphQL's `Users/1/name` vs Graph's `spo/...`)
3. The HBTrie has no namespace or subtree concept

## Goal

Allow multiple schema layers to share a single `database_t` (single persistence, single WAL, single MVCC) while being isolated in different subtrees. A layer attached to a subtree should see its namespace as its own root — paths like `__meta/layer` work unchanged. Layers need three lifecycle operations: create (open), reopen, and delete a subtree.

## Design

### New Type: `database_subtree_t`

A subtree wraps an existing `database_t` and a path prefix. All operations prepend the prefix before delegating to the underlying database. The layer never sees the prefix. The prefix can be a multi-level path like `"layer/graphs/graph1"` with the user's chosen delimiter.

```c
typedef struct database_subtree {
    refcounter_t refcounter;     // standard refcounting (first member per project convention)
    database_t* db;               // shared underlying database
    char* prefix;                 // namespace path, e.g. "layer/graphs/graph1"
    size_t prefix_len;            // length of prefix string
    char delimiter;               // path delimiter, e.g. '/' or '.' — used to join prefix with keys
} database_subtree_t;
```

### Lifecycle

```c
// Open or create a subtree within an existing database.
// prefix: namespace path, e.g. "layer/graphs/graph1" — can be multi-level
// delimiter: path delimiter character, e.g. '/' or '.' — used to join prefix with keys
// If the subtree namespace doesn't exist yet, it's implicitly created on first write.
// The prefix becomes the root namespace for all operations through this handle.
database_subtree_t* database_subtree_open(database_t* db, const char* prefix, char delimiter);

// Close a subtree handle — decrements refcount, frees the handle.
// Does NOT delete the subtree's data. Data persists on disk.
void database_subtree_close(database_subtree_t* subtree);

// Delete all data under a prefix from the database.
// prefix: namespace path, e.g. "layer/graphs/graph1"
// delimiter: must match the delimiter used when opening the subtree
// Scans for all keys starting with "prefix{delimiter}" and removes them.
// Can be called without an open subtree handle (operates on database directly).
// Returns 0 on success, non-zero on error.
int database_subtree_delete(database_t* db, const char* prefix, char delimiter);
```

`database_subtree_open` handles both create and reopen, similar to how `database_create_with_config` handles both cases. On reopen, the layer's schema data (`__meta/layer`, `__gschema/types`) is already under the prefix, so the layer's existing load logic works unchanged.

### Path Translation

For **path-based APIs**, the implementation prepends the prefix as additional `identifier_t` components in the `path_t`. The delimiter is used to split the prefix into path components:

- Subtree opened with `database_subtree_open(db, "layer/graphs/graph1", '/')`
- Layer calls: `database_subtree_put_sync(subtree, path("Users/1/name"), value)`
- Implementation calls: `database_put_sync(db, path("layer/graphs/graph1/Users/1/name"), value)`

For **raw (string) APIs**, the implementation prepends `"prefix{delimiter}"` to the key string:

- Subtree opened with `database_subtree_open(db, "layer/graphs/graph1", '/')`
- Layer calls: `database_subtree_put_sync_raw(subtree, "__meta/layer", 12, '/', value, len)`
- Implementation calls: `database_put_sync_raw(db, "layer/graphs/graph1/__meta/layer", 27, '/', value, len)`

### CRUD Operations

Each function mirrors the `database_t` API with `database_subtree_` prefix:

```c
// --- Path-based sync CRUD ---
int database_subtree_put_sync(database_subtree_t* st, path_t* path, identifier_t* value);
int database_subtree_get_sync(database_subtree_t* st, path_t* path, identifier_t** result);
int database_subtree_delete_sync(database_subtree_t* st, path_t* path);
int64_t database_subtree_increment_sync(database_subtree_t* st, path_t* path, int64_t delta);

// --- Path-based async CRUD ---
void database_subtree_put(database_subtree_t* st, path_t* path, identifier_t* value, promise_t* promise);
void database_subtree_get(database_subtree_t* st, path_t* path, promise_t* promise);
void database_subtree_delete(database_subtree_t* st, path_t* path, promise_t* promise);

// --- Raw (byte key/value) sync CRUD ---
int database_subtree_put_sync_raw(database_subtree_t* st, const char* key, size_t key_len, char delimiter, const uint8_t* value, size_t value_len);
int database_subtree_get_sync_raw(database_subtree_t* st, const char* key, size_t key_len, char delimiter, uint8_t** value_out, size_t* value_len_out);
int database_subtree_delete_sync_raw(database_subtree_t* st, const char* key, size_t key_len, char delimiter);

// --- Raw async CRUD ---
int database_subtree_put_raw(database_subtree_t* st, const char* key, size_t key_len, char delimiter, const uint8_t* value, size_t value_len, promise_t* promise);
int database_subtree_get_raw(database_subtree_t* st, const char* key, size_t key_len, char delimiter, promise_t* promise);
int database_subtree_delete_raw(database_subtree_t* st, const char* key, size_t key_len, char delimiter, promise_t* promise);
```

### Batch Operations

Each key in the batch gets the prefix prepended:

```c
int database_subtree_write_batch_sync(database_subtree_t* st, batch_t* batch);
void database_subtree_write_batch(database_subtree_t* st, batch_t* batch, promise_t* promise);
int database_subtree_batch_sync_raw(database_subtree_t* st, char delimiter, const raw_op_t* ops, size_t count);
int database_subtree_batch_raw(database_subtree_t* st, char delimiter, const raw_op_t* ops, size_t count, promise_t* promise);
```

For `batch_t` operations: each path in the batch is prefixed before delegating to `database_write_batch_sync`. For raw batch operations: each key string is prefixed before delegating to `database_batch_sync_raw`.

### Scan/Iterator Operations

Scans are **scoped to the subtree** — the implementation prepends the prefix to the scan range and strips it from results. A subtree scan cannot accidentally return data outside its namespace.

```c
// Iterator-based scan — start/end paths are prefixed, results have prefix stripped
database_iterator_t* database_subtree_scan_start(database_subtree_t* st, path_t* start_path, path_t* end_path);
database_iterator_t* database_subtree_scan_range(database_subtree_t* st, const char* start, const char* end);

// Raw scan — prefix is prepended to search prefix, results have prefix stripped
int database_subtree_scan_sync_raw(database_subtree_t* st, const char* prefix, size_t prefix_len, char delimiter, raw_result_t** results, size_t* count);
int database_subtree_scan_range_sync_raw(database_subtree_t* st, const char* start_prefix, size_t start_len, const char* end_prefix, size_t end_len, char delimiter, raw_result_t** results, size_t* count);

// Note: database_scan_next and database_scan_end remain unchanged — they operate on iterator, not database
```

### Snapshot and Introspection

```c
// Delegates to underlying database
int database_subtree_snapshot(database_subtree_t* st);
int database_subtree_flush_dirty_bnodes(database_subtree_t* st);

// Counts entries under the subtree's prefix
size_t database_subtree_count(database_subtree_t* st);

// Accessors — replaces direct field access on database_t
database_t* database_subtree_get_db(database_subtree_t* st);
work_pool_t* database_subtree_get_pool(database_subtree_t* st);
```

### Delete Implementation

`database_subtree_delete` uses the existing `database_scan_sync_raw` to find all keys under the prefix, then deletes each one using `database_delete_sync_raw` (or uses `database_batch_sync_raw` for efficiency):

```
1. Construct scan prefix: "prefix{delimiter}" (e.g. "layer/graphql/")
2. Scan database for all keys starting with that prefix
3. Delete each key (or batch-delete for efficiency)
4. Return 0 on success
```

This is a synchronous operation. For large subtrees, a future optimization could add an async variant.

### Layer Config Changes

The subtree field goes in each layer's config struct, not in `database_config_t`. A subtree is a layer concern (which namespace the layer operates in), not a database concern (how the database is configured).

**GraphQL layer** — extend existing `graphql_layer_config_t`:
```c
typedef struct graphql_layer_config_t {
    const char* path;
    uint8_t chunk_size;
    uint32_t btree_node_size;
    size_t lru_memory_mb;
    uint16_t lru_shards;
    uint8_t worker_threads;
    uint8_t enable_persist;
    wal_config_t wal_config;
    char delimiter;
    database_subtree_t* subtree;    // NEW: if set, layer uses this subtree instead of creating a database
} graphql_layer_config_t;
```

When `subtree` is non-NULL, `graphql_layer_create` uses the subtree for all database operations and ignores `path`, `chunk_size`, `btree_node_size`, `lru_memory_mb`, `lru_shards`, `worker_threads`, `enable_persist`, and `wal_config` (these are managed by the shared database). The `delimiter` is still read from the config since it's a layer-level concern.

**Graph layer** — create a new `graph_layer_config_t` (doesn't exist today):
```c
typedef struct graph_layer_config_t {
    const char* path;               // Database storage path (NULL = in-memory), ignored if subtree is set
    database_config_t* db_config;   // Database configuration, ignored if subtree is set
    database_subtree_t* subtree;    // NEW: if set, layer uses this subtree instead of creating a database
} graph_layer_config_t;
```

The graph layer currently takes `database_config_t*` directly in `graph_layer_create`. The new config struct wraps this and adds the subtree field. When `subtree` is non-NULL, `graph_layer_create` uses the subtree and ignores `path` and `db_config`.

**Updated create signatures:**
```c
// GraphQL — signature unchanged, config now has optional subtree field
graphql_layer_t* graphql_layer_create(const char* path,
                                       const graphql_layer_config_t* config);

// Graph — signature changes to accept new config struct
graph_layer_t* graph_layer_create(const char* path, graph_layer_config_t* config);
```

**Layer internals:** When a layer is created with a subtree, it stores the subtree handle and uses `database_subtree_*` functions for all operations instead of `database_*` functions. The `database_subtree_get_pool()` accessor replaces direct `db->pool` access.

### Layer Integration Pattern

**Before** (each layer owns its own database):
```c
// graphql_layer_create:
database_t* db = database_create_with_config(db_path, db_config, &error_code);
layer->db = db;
// ... use database_put_sync(db, path, value)
```

**After** (subtree passed through layer config):
```c
// Application code creates one database and opens subtrees with arbitrary paths:
database_t* db = database_create_with_config(db_path, db_config, &error_code);
database_subtree_t* graphql_subtree = database_subtree_open(db, "layer/graphql", '/');
database_subtree_t* graph_subtree = database_subtree_open(db, "layer/graphs/graph1", '/');

// GraphQL layer — subtree goes in config
graphql_layer_config_t gql_config = {0};
gql_config.subtree = graphql_subtree;
gql_config.delimiter = '/';
graphql_layer_t* gql = graphql_layer_create(NULL, &gql_config);
// path is NULL because the subtree already has a database; config path is ignored

// Graph layer — new config struct with subtree
graph_layer_config_t graph_config = {0};
graph_config.subtree = graph_subtree;
graph_layer_t* graph = graph_layer_create(NULL, &graph_config);

// Inside each layer, all database operations use the subtree API:
database_subtree_put_sync(layer->subtree, path, value);
// Path "Users/1/name" is automatically prefixed to "layer/graphql/Users/1/name"

// On reopen:
database_t* db = database_create_with_config(db_path, config, &error_code);
database_subtree_t* subtree = database_subtree_open(db, "layer/graphql", '/');
gql_config.subtree = subtree;
graphql_layer_t* gql = graphql_layer_create(NULL, &gql_config);
// graphql_schema_load works unchanged — sees __meta/layer at its own root

// Delete a layer's data:
database_subtree_delete(db, "layer/graphql", '/');
```

### Files to Create/Modify

**New files:**
- `src/Database/database_subtree.h` — public API header
- `src/Database/database_subtree.c` — implementation

**Modified files:**
- `src/Layers/graphql/graphql_types.h` — add `database_subtree_t*` field to `graphql_layer_config_t` and `graphql_layer_t`
- `src/Layers/graphql/graphql_schema.h` — update `graphql_layer_create` signature to accept subtree from config
- `src/Layers/graphql/graphql_schema.c` — when config has a subtree, use it instead of creating a database
- `src/Layers/graph/graph.h` — create `graph_layer_config_t` struct with `database_subtree_t*` field; update `graph_layer_create` signature
- `src/Layers/graph/graph_internal.h` — add `database_subtree_t*` field to `graph_layer_t`
- `src/Layers/graph/graph.c` — when config has a subtree, use it instead of creating a database

### Error Handling

- `database_subtree_open` returns `NULL` on allocation failure
- `database_subtree_delete` returns `0` on success, non-zero on error
- All CRUD operations return the same error codes as their `database_t` counterparts
- Scan operations return `NULL` iterators or negative counts on failure

### Thread Safety

`database_subtree_t` is thread-safe to the same extent as `database_t` — the underlying database handles MVCC and locking. The subtree handle itself is immutable after creation (prefix doesn't change), so no additional locking is needed on the subtree struct.

### Reference Counting

`database_subtree_t` follows the project's standard refcounting pattern:
- `refcounter_t refcounter` as the first member
- `refcounter_init(&subtree->refcounter, (destructor)database_subtree_destroy)`
- `database_subtree_close` calls `refcounter_dereference`
- Actual cleanup happens when count reaches 0
- The underlying `database_t` is NOT dereferenced on subtree close — the application manages the database lifecycle separately