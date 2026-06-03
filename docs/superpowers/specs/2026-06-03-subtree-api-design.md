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

The subtree is passed as an **optional parameter** to the existing layer create functions, not in the config. Configs contain configuration values only. If the subtree parameter is provided, the layer uses it (the subtree already has a reference to its underlying database). If not, business as usual — the layer creates its own database.

**GraphQL layer** — config stays unchanged. New optional parameter added to create:
```c
// graphql_layer_config_t — UNCHANGED, no subtree fields
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
} graphql_layer_config_t;

// Updated create signature — subtree is optional, NULL means create own database
graphql_layer_t* graphql_layer_create(const char* path,
                                       const graphql_layer_config_t* config,
                                       database_subtree_t* subtree);  // NEW optional param
```

When `subtree` is non-NULL, the layer uses it for all database operations and ignores `path`, `chunk_size`, `btree_node_size`, `lru_memory_mb`, `lru_shards`, `worker_threads`, `enable_persist`, and `wal_config` (managed by the shared database). The `delimiter` is still read from config.

**Graph layer** — new config struct (needed regardless of subtree — currently takes raw `database_config_t*`). Create signature gets optional subtree parameter:
```c
// NEW — graph_layer_config_t (replaces direct database_config_t* param)
typedef struct graph_layer_config_t {
    const char* path;               // Database storage path (NULL = in-memory)
    database_config_t* db_config;  // Database configuration
} graph_layer_config_t;

// Updated create signature — subtree is optional, NULL means create own database
graph_layer_t* graph_layer_create(const char* path,
                                   graph_layer_config_t* config,
                                   database_subtree_t* subtree);  // NEW optional param
```

When `subtree` is non-NULL, the layer uses it and ignores `path` and `db_config`.

**Backward compatibility:** Since both `subtree` parameters are optional (NULL = old behavior), existing code that calls these functions without the subtree argument continues to work. In languages with default arguments or optional parameters (JavaScript, Dart), the subtree can be omitted. In C, callers pass `NULL`.

**Layer internals:** When a layer is created with a subtree, it stores the subtree handle and uses `database_subtree_*` functions for all operations instead of `database_*` functions. The `database_subtree_get_pool()` accessor replaces direct `db->pool` access. On `*_layer_destroy`, the layer calls `database_subtree_close` to release the subtree handle (but does NOT destroy the underlying database — that's the application's responsibility).

### Layer Integration Pattern

**Before** (each layer owns its own database):
```c
// graphql_layer_create:
database_t* db = database_create_with_config(db_path, db_config, &error_code);
layer->db = db;
// ... use database_put_sync(db, path, value)
```

**After** (subtree as optional parameter — simple and backward compatible):
```c
// Application code creates one database, then opens subtrees for each layer
database_t* db = database_create_with_config(db_path, db_config, &error_code);
database_subtree_t* gql_subtree = database_subtree_open(db, "layer/graphql", '/');
database_subtree_t* graph_subtree = database_subtree_open(db, "layer/graphs/graph1", '/');

// GraphQL layer — pass subtree as optional parameter, NULL for old behavior
graphql_layer_t* gql = graphql_layer_create(NULL, &gql_config, gql_subtree);

// Graph layer — same pattern, new config struct
graph_layer_config_t graph_config = {0};
graph_config.path = NULL;
graph_config.db_config = NULL;
graph_layer_t* graph = graph_layer_create(NULL, &graph_config, graph_subtree);

// Inside each layer, all database operations use the subtree API:
// Path "Users/1/name" is automatically prefixed to "layer/graphql/Users/1/name"

// On reopen — recreate database, reopen subtrees, pass to layers
database_t* db = database_create_with_config(db_path, config, &error_code);
database_subtree_t* gql_subtree = database_subtree_open(db, "layer/graphql", '/');
graphql_layer_t* gql = graphql_layer_create(NULL, &gql_config, gql_subtree);
// graphql_schema_load works unchanged — sees __meta/layer at its own root

// Delete a layer's data:
database_subtree_delete(db, "layer/graphql", '/');

// Business as usual — no subtree, layer creates its own database
graphql_layer_t* standalone = graphql_layer_create("/path/to/db", &config, NULL);
```

### Files to Create/Modify

**New files:**
- `src/Database/database_subtree.h` — public API header
- `src/Database/database_subtree.c` — implementation
- `bindings/nodejs/src/subtree.cc` — Node.js Subtree class wrapping database_subtree_t
- `bindings/nodejs/src/subtree.h` — Node.js Subtree class header
- `bindings/nodejs/test/subtree.test.js` — Node.js Subtree API tests
- `bindings/dart/lib/src/subtree.dart` — Dart Subtree class
- `bindings/dart/test/subtree_test.dart` — Dart Subtree API tests

**Modified files:**
- `src/Layers/graphql/graphql_types.h` — add `database_subtree_t* subtree` field to `graphql_layer_t`
- `src/Layers/graphql/graphql_schema.h` — update `graphql_layer_create` signature to add optional `database_subtree_t* subtree` parameter
- `src/Layers/graphql/graphql_schema.c` — when subtree is provided, use it instead of creating a database
- `src/Layers/graph/graph.h` — create `graph_layer_config_t` struct; update `graph_layer_create` signature to add optional `database_subtree_t* subtree` parameter
- `src/Layers/graph/graph_internal.h` — add `database_subtree_t*` field to `graph_layer_t`
- `src/Layers/graph/graph.c` — when config has a subtree, use it instead of creating a database
- `bindings/nodejs/src/graph_layer.cc` — accept optional `subtree` in constructor options
- `bindings/nodejs/src/graphql_layer.cc` — accept optional `subtree` in constructor options
- `bindings/nodejs/src/database.cc` — add `openSubtree()` and `deleteSubtree()` methods
- `bindings/dart/lib/src/native/wavedb_bindings.dart` — add FFI typedefs for all subtree functions, update GraphLayerCreate
- `bindings/dart/lib/src/native/types.dart` — add `database_subtree_t` opaque type, `graph_layer_config_t` struct
- `bindings/dart/lib/src/database.dart` — add `openSubtree()` and `deleteSubtree()` methods
- `bindings/dart/lib/src/graph_layer.dart` — add `GraphLayerConfig` class with `subtree` field
- `bindings/dart/lib/src/graphql_layer.dart` — add `subtree` field to `GraphQLLayerConfig`
- `bindings/dart/lib/wavedb.dart` — export `Subtree` and `GraphLayerConfig`
- `bindings/nodejs/test/graph.test.js` — add subtree-based creation tests
- `bindings/dart/test/graph_layer_test.dart` — add subtree-based creation tests

### README Updates

Three READMEs document the layer creation API and must be updated to cover the new subtree feature:

**Root `README.md`:**
- Add a "Subtree API" section documenting `database_subtree_open`, `database_subtree_close`, and `database_subtree_delete` with C code examples
- Update the Graph Schema Layer section to show `graph_layer_config_t` usage (new struct) and the `subtree` field
- Update the GraphQL Schema Layer section to show the new `subtree` field in `graphql_layer_config_t`
- Add an example showing how to create a database, open two subtrees, and pass them to layer constructors

**`bindings/nodejs/README.md`:**
- Add `WaveDB.openSubtree(prefix, delimiter)` method documentation
- Add `WaveDB.deleteSubtree(prefix, delimiter)` method documentation
- Document the Subtree class and its methods (put, get, del, batch, scan, etc.)
- Update `new GraphLayer(options)` to document the `subtree` option
- Update `new GraphQLLayer(path, options)` to document the `subtree` option
- Add an example showing shared database with subtrees for both layers

**`bindings/dart/README.md`:**
- Add `WaveDB.openSubtree(prefix, delimiter)` method documentation
- Add `WaveDB.deleteSubtree(prefix, delimiter)` method documentation
- Document the Subtree class and its methods
- Update `GraphQLLayerConfig` to document the `subtree` field
- Document the new `GraphLayerConfig` class (doesn't exist today in Dart)
- Add an example showing shared database with subtrees for both layers

### Error Handling

- `database_subtree_open` returns `NULL` on allocation failure
- `database_subtree_delete` returns `0` on success, non-zero on error
- All CRUD operations return the same error codes as their `database_t` counterparts
- Scan operations return `NULL` iterators or negative counts on failure

### Binding Updates

The Node.js and Dart bindings must be updated to expose the subtree config and new API.

#### C API Changes That Affect Bindings

| Change | Impact |
|--------|--------|
| New `database_subtree_t` type and all `database_subtree_*` functions | New FFI bindings needed |
| New `graph_layer_config_t` struct (replaces direct `database_config_t*` param) | Changes `graph_layer_create` signature |
| `graphql_layer_create` gains optional `database_subtree_t* subtree` param | New optional parameter |
| `graph_layer_create` gains optional `database_subtree_t* subtree` param | New optional parameter |
| `database_subtree_open(db, prefix, delimiter)` | New function |
| `database_subtree_close(subtree)` | New function |
| `database_subtree_delete(db, prefix, delimiter)` | New function |

#### Node.js Binding Updates

**Files to modify:**
- `bindings/nodejs/src/graph_layer.cc` — Update `GraphLayer` constructor to accept optional `subtree` parameter. When provided, pass it to `graph_layer_create`. If not provided, behave as before (backward compatible).
- `bindings/nodejs/src/graphql_layer.cc` — Update `GraphQLLayer` constructor to accept optional `subtree` parameter.
- `bindings/nodejs/src/database.cc` — Add `WaveDB.openSubtree(prefix, delimiter)` method that returns a `Subtree` wrapper object. Add `WaveDB.deleteSubtree(prefix, delimiter)` method.
- New `bindings/nodejs/src/subtree.cc` and `subtree.h` — `Subtree` class wrapping `database_subtree_t*`. Exposes all `database_subtree_*` CRUD, batch, scan, and snapshot methods.
- `bindings/nodejs/test/graph.test.js` — Add tests for subtree-based GraphLayer creation
- `bindings/nodejs/test/subtree.test.js` — New test file for Subtree API

**Node.js API surface:**

```js
// Create a subtree from an existing database
const subtree = db.openSubtree('layer/graphql', '/');

// Pass subtree to layer as optional parameter (NULL = old behavior)
const graph = new GraphLayer(null, { subtree });
const gql = new GraphQLLayer(null, { subtree });

// Business as usual — no subtree, layer creates its own database
const standalone = new GraphLayer('/path/to/db');

// Subtree CRUD (mirrors database API)
await subtree.put(path, value);
const result = await subtree.get(path);
await subtree.delete(path);
const results = subtree.scanSync(prefix, delimiter);
subtree.close();

// Delete all data under a subtree prefix
db.deleteSubtree('layer/graphql', '/');
```

#### Dart Binding Updates

**Files to modify:**
- `bindings/dart/lib/src/native/wavedb_bindings.dart` — Add FFI typedefs for `database_subtree_open`, `database_subtree_close`, `database_subtree_delete`, and all `database_subtree_*` CRUD/scan functions. Update `GraphLayerCreate` typedef to use `graph_layer_config_t*` instead of `database_config_t*`.
- `bindings/dart/lib/src/native/types.dart` — Add `database_subtree_t` opaque type. Add `graph_layer_config_t` struct definition for FFI.
- `bindings/dart/lib/src/database.dart` — Add `openSubtree(String prefix, String delimiter)` method to `WaveDB` class. Add `deleteSubtree(String prefix, String delimiter)` method.
- `bindings/dart/lib/src/graph_layer.dart` — Add `subtree` optional parameter to `GraphLayer` constructor. Add `GraphLayerConfig` class.
- `bindings/dart/lib/src/graphql_layer.dart` — Add `subtree` optional parameter to `GraphQLLayer.create()`.
- New `bindings/dart/lib/src/subtree.dart` — `Subtree` class wrapping `database_subtree_t*`. Exposes CRUD, batch, scan, and snapshot methods.
- `bindings/dart/lib/wavedb.dart` — Export new `Subtree` class and `GraphLayerConfig`.
- `bindings/dart/test/graph_layer_test.dart` — Add tests for subtree-based creation
- `bindings/dart/test/subtree_test.dart` — New test file for Subtree API

**Dart API surface:**

```dart
// Create a subtree from an existing database
final subtree = db.openSubtree('layer/graphql', '/');
await subtree.put(path, value);
final result = await subtree.get(path);
await subtree.delete(path);
subtree.close();

// Delete all data under a subtree prefix
db.deleteSubtree('layer/graphql', '/');

// Pass subtree to layer as optional parameter
final graph = GraphLayer(config: GraphLayerConfig(), subtree: subtree);
final gql = GraphQLLayer(config: GraphQLLayerConfig(), subtree: subtree);

// Business as usual — no subtree, layer creates its own database
final standalone = GraphLayer('/path/to/db');
```

#### Backward Compatibility

Both bindings must remain backward compatible:
- If `subtree` is not provided (NULL/undefined/null), layers create their own `database_t` as before
- `graph_layer_create` with `database_config_t*` must still work (we'll add a compatibility wrapper or update the binding to construct a `graph_layer_config_t` internally)
- The `Subtree` class is additive — no existing API changes required

### Thread Safety

`database_subtree_t` is thread-safe to the same extent as `database_t` — the underlying database handles MVCC and locking. The subtree handle itself is immutable after creation (prefix doesn't change), so no additional locking is needed on the subtree struct.

### Reference Counting

`database_subtree_t` follows the project's standard refcounting pattern:
- `refcounter_t refcounter` as the first member
- `refcounter_init(&subtree->refcounter, (destructor)database_subtree_destroy)`
- `database_subtree_close` calls `refcounter_dereference`
- Actual cleanup happens when count reaches 0
- The underlying `database_t` is NOT dereferenced on subtree close — the application manages the database lifecycle separately