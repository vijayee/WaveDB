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

A subtree wraps an existing `database_t` and a path prefix. All operations prepend the prefix before delegating to the underlying database. The layer never sees the prefix.

```c
typedef struct database_subtree {
    refcounter_t refcounter;     // standard refcounting (first member per project convention)
    database_t* db;               // shared underlying database
    char* prefix;                 // namespace, e.g. "graphql" or "graph"
    char delimiter;               // path delimiter, copied from db config
} database_subtree_t;
```

### Lifecycle

```c
// Open or create a subtree within an existing database.
// If the subtree namespace doesn't exist yet, it's implicitly created on first write.
// The prefix becomes the root namespace for all operations through this handle.
database_subtree_t* database_subtree_open(database_t* db, const char* prefix);

// Close a subtree handle — decrements refcount, frees the handle.
// Does NOT delete the subtree's data. Data persists on disk.
void database_subtree_close(database_subtree_t* subtree);

// Delete all data under a prefix from the database.
// Scans for all keys starting with "prefix/" and removes them.
// Can be called without an open subtree handle (operates on database directly).
// Returns 0 on success, non-zero on error.
int database_subtree_delete(database_t* db, const char* prefix);
```

`database_subtree_open` handles both create and reopen, similar to how `database_create_with_config` handles both cases. On reopen, the layer's schema data (`__meta/layer`, `__gschema/types`) is already under the prefix, so the layer's existing load logic works unchanged.

### Path Translation

For **path-based APIs**, the implementation prepends the prefix as the first `identifier_t` in the `path_t`:

- Layer calls: `database_subtree_put_sync(subtree, path("Users/1/name"), value)`
- Implementation calls: `database_put_sync(db, path("graphql/Users/1/name"), value)`

For **raw (string) APIs**, the implementation prepends `"prefix/"` to the key string:

- Layer calls: `database_subtree_put_sync_raw(subtree, "__meta/layer", 12, '/', value, len)`
- Implementation calls: `database_put_sync_raw(db, "graphql/__meta/layer", 20, '/', value, len)`

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
1. Scan database for all keys starting with "prefix/"
2. Delete each key (or batch-delete for efficiency)
3. Return 0 on success
```

This is a synchronous operation. For large subtrees, a future optimization could add an async variant.

### Layer Integration Pattern

**Before** (each layer owns its own database):
```c
// graphql_layer_create:
database_t* db = database_create_with_config(db_path, db_config, &error_code);
layer->db = db;
// ... use database_put_sync(db, path, value)
```

**After** (layers receive a subtree of a shared database):
```c
// Application code creates one database and opens subtrees:
database_t* db = database_create_with_config(db_path, db_config, &error_code);
database_subtree_t* graphql_subtree = database_subtree_open(db, "graphql");
database_subtree_t* graph_subtree = database_subtree_open(db, "graph");

// Layer creation receives the subtree instead of creating its own database:
graphql_layer_t* gql = graphql_layer_create_with_subtree(graphql_subtree, config);
graph_layer_t* graph = graph_layer_create_with_subtree(graph_subtree, config);

// Inside the layer, all database operations use the subtree API:
database_subtree_put_sync(layer->subtree, path, value);
// Path "Users/1/name" is automatically prefixed to "graphql/Users/1/name"

// On reopen:
database_t* db = database_create_with_config(db_path, config, &error_code);
database_subtree_t* subtree = database_subtree_open(db, "graphql");
// graphql_schema_load(subtree) works unchanged — sees __meta/layer at its own root

// Delete a layer's data:
database_subtree_delete(db, "graphql");
```

### Files to Create/Modify

**New files:**
- `src/Database/database_subtree.h` — public API header
- `src/Database/database_subtree.c` — implementation

**Modified files:**
- `src/Layers/graphql/graphql_schema.h` — add `graphql_layer_create_with_subtree`
- `src/Layers/graphql/graphql_schema.c` — implement subtree-based creation
- `src/Layers/graphql/graphql_types.h` — add `database_subtree_t*` field to `graphql_layer_t`
- `src/Layers/graph/graph.h` — add `graph_layer_create_with_subtree`
- `src/Layers/graph/graph.c` — implement subtree-based creation
- `src/Layers/graph/graph_internal.h` — add `database_subtree_t*` field to `graph_layer_t`

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