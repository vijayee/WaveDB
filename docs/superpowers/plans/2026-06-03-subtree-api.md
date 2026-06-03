# Subtree API Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement `database_subtree_t` — a prefix-scoped wrapper around `database_t` that allows multiple schema layers to share a single database while operating in isolated subtrees.

**Architecture:** A `database_subtree_t` wraps a `database_t*` and a prefix string. All operations prepend the prefix before delegating. Layers receive a subtree as an optional parameter to their create functions. If provided, they use the subtree instead of creating their own database. Bindings (Node.js, Dart) expose the subtree API and the optional subtree parameter.

**Tech Stack:** C (core), C++/N-API (Node.js bindings), Dart FFI (Dart bindings), following existing WaveDB patterns (refcounting, `get_clear_memory`, `path_create_from_raw`).

---

## Task Dependency Graph

```
Task 1 (core struct + lifecycle)
  └── Task 2 (sync CRUD)
        └── Task 3 (async CRUD)
              └── Task 4 (batch + scan)
                    └── Task 5 (snapshot + introspection)
Task 6 (delete subtree) — independent of Tasks 2-5
Task 7 (graph_layer_config_t) — independent of Tasks 1-6
Task 8 (graphql layer subtree) — depends on Task 1
Task 9 (graph layer subtree) — depends on Tasks 1, 7
Task 10 (C tests) — depends on Tasks 1-6
Task 11 (Node.js bindings) — depends on Tasks 1-9
Task 12 (Dart bindings) — depends on Tasks 1-9
Task 13 (README updates) — depends on Tasks 1-12
```

---

### Task 1: Core struct, lifecycle, and path translation helpers

**Files:**
- Create: `src/Database/database_subtree.h`
- Create: `src/Database/database_subtree.c`

- [ ] **Step 1: Create the header file**

Create `src/Database/database_subtree.h` with the struct definition, lifecycle functions, and path translation helpers:

```c
#ifndef WAVEDB_DATABASE_SUBTREE_H
#define WAVEDB_DATABASE_SUBTREE_H

#include "Database/database.h"
#include "HBTrie/path.h"

typedef struct database_subtree {
    refcounter_t refcounter;     // MUST be first member
    database_t* db;               // shared underlying database
    char* prefix;                 // namespace path, e.g. "layer/graphs/graph1"
    size_t prefix_len;            // length of prefix string
    char delimiter;               // path delimiter, e.g. '/' or '.'
    uint8_t chunk_size;           // copied from db->chunk_size for path_create_from_raw calls
} database_subtree_t;
```

Note: `chunk_size` is not in the design spec but is needed because `path_create_from_raw` requires a `chunk_size` parameter. It's copied from `db->chunk_size` at subtree creation time.

// Lifecycle
database_subtree_t* database_subtree_open(database_t* db, const char* prefix, char delimiter);
void database_subtree_close(database_subtree_t* subtree);
int database_subtree_delete(database_t* db, const char* prefix, char delimiter);

// Accessors
database_t* database_subtree_get_db(database_subtree_t* st);
work_pool_t* database_subtree_get_pool(database_subtree_t* st);

// Internal path translation helpers (declared here for testing)
path_t* database_subtree_prepend_path(database_subtree_t* st, path_t* path);
char* database_subtree_prepend_key(database_subtree_t* st, const char* key, size_t key_len, size_t* out_len);

#endif // WAVEDB_DATABASE_SUBTREE_H
```

- [ ] **Step 2: Implement lifecycle functions and path helpers**

Create `src/Database/database_subtree.c`. Start with lifecycle (`open`, `close`) and the two path translation helpers. These are the foundation that all other functions build on.

`database_subtree_open`: allocate with `get_clear_memory`, `strdup` the prefix, copy `delimiter` and `chunk_size` from `db`, `refcounter_init`.

`database_subtree_close`: call `refcounter_dereference`. In the destructor (called when count reaches 0), free `prefix` and the struct. Do NOT destroy or dereference the underlying `db`.

`database_subtree_prepend_path`: Create a new `path_t`, build prefix components using `path_create_from_raw` on `st->prefix`, then append the original path's identifiers. Return the new path (caller owns it).

`database_subtree_prepend_key`: Allocate a new string of size `st->prefix_len + 1 + key_len`, copy prefix, delimiter, then key. Set `*out_len`.

- [ ] **Step 3: Add the new source file to the build system**

Update `CMakeLists.txt` (or whatever build system WaveDB uses) to include `src/Database/database_subtree.c` in the build. Check how other `src/Database/*.c` files are registered and follow the same pattern.

- [ ] **Step 4: Write a minimal lifecycle test**

Create `tests/test_subtree.c` with a test that:
1. Creates an in-memory database with `database_create_with_config(NULL, config, &error)`
2. Opens a subtree with `database_subtree_open(db, "layer/graphql", '/')`
3. Verifies the subtree struct fields (prefix, delimiter, chunk_size, db pointer)
4. Closes the subtree with `database_subtree_close`
5. Destroys the database

- [ ] **Step 5: Build and run the lifecycle test**

Run: `cmake --build build && ./build/test_subtree`
Expected: All assertions pass.

- [ ] **Step 6: Commit**

```bash
git add src/Database/database_subtree.h src/Database/database_subtree.c tests/test_subtree.c
git commit -m "feat: add database_subtree_t struct, lifecycle, and path helpers"
```

---

### Task 2: Sync CRUD operations

**Files:**
- Modify: `src/Database/database_subtree.h` (add sync CRUD declarations)
- Modify: `src/Database/database_subtree.c` (implement sync CRUD)

- [ ] **Step 1: Write failing tests for sync CRUD**

Add tests to `tests/test_subtree.c` that:
1. Open a subtree, put a value with `database_subtree_put_sync`, get it with `database_subtree_get_sync`, verify the value matches
2. Put values under different subtrees (e.g., "graphql" and "graph"), verify each subtree only sees its own data
3. Delete a value with `database_subtree_delete_sync`, verify get returns not-found
4. Test `database_subtree_increment_sync` by incrementing a counter and verifying the result
5. Test that the raw database sees the prefixed key (put via subtree at "Users/1/name", verify raw database has "graphql/Users/1/name")

These tests will fail because the functions don't exist yet.

- [ ] **Step 2: Run tests to verify they fail**

Run: `cmake --build build && ./build/test_subtree`
Expected: Compilation errors — functions not declared.

- [ ] **Step 3: Add sync CRUD declarations to the header**

Add to `src/Database/database_subtree.h`:

```c
// Path-based sync CRUD
int database_subtree_put_sync(database_subtree_t* st, path_t* path, identifier_t* value);
int database_subtree_get_sync(database_subtree_t* st, path_t* path, identifier_t** result);
int database_subtree_delete_sync(database_subtree_t* st, path_t* path);
int64_t database_subtree_increment_sync(database_subtree_t* st, path_t* path, int64_t delta);

// Raw (byte) sync CRUD
int database_subtree_put_sync_raw(database_subtree_t* st, const char* key, size_t key_len, char delimiter, const uint8_t* value, size_t value_len);
int database_subtree_get_sync_raw(database_subtree_t* st, const char* key, size_t key_len, char delimiter, uint8_t** value_out, size_t* value_len_out);
int database_subtree_delete_sync_raw(database_subtree_t* st, const char* key, size_t key_len, char delimiter);
```

- [ ] **Step 4: Implement sync CRUD functions**

Each function follows the same pattern:
- For path-based: call `database_subtree_prepend_path` to create a prefixed path, then call the corresponding `database_*_sync` function, then destroy the prefixed path.
- For raw-based: call `database_subtree_prepend_key` to create a prefixed key, then call the corresponding `database_*_sync_raw` function, then free the prefixed key.

Key implementation notes:
- `database_subtree_put_sync` and `database_subtree_get_sync` transfer ownership of the prefixed path to the underlying function (which destroys it). The original path is NOT destroyed — the caller still owns it.
- `database_subtree_increment_sync` delegates to `database_increment_sync` with the prefixed path.
- For `database_subtree_get_sync_raw`: the `value_out` and `value_len_out` are passed through directly — the value bytes don't need prefix stripping.

- [ ] **Step 5: Run tests to verify they pass**

Run: `cmake --build build && ./build/test_subtree`
Expected: All sync CRUD tests pass.

- [ ] **Step 6: Commit**

```bash
git add src/Database/database_subtree.h src/Database/database_subtree.c tests/test_subtree.c
git commit -m "feat: add sync CRUD operations to database_subtree_t"
```

---

### Task 3: Async CRUD operations

**Files:**
- Modify: `src/Database/database_subtree.h` (add async CRUD declarations)
- Modify: `src/Database/database_subtree.c` (implement async CRUD)

- [ ] **Step 1: Write failing tests for async CRUD**

Add tests to `tests/test_subtree.c` that:
1. Use `database_subtree_put` (async) with a promise, verify the value is stored
2. Use `database_subtree_get` (async) with a promise, verify the result matches
3. Use `database_subtree_delete` (async) with a promise, verify the value is deleted

- [ ] **Step 2: Run tests to verify they fail**

Run: `cmake --build build && ./build/test_subtree`
Expected: Compilation errors.

- [ ] **Step 3: Add async CRUD declarations to the header**

Add to `src/Database/database_subtree.h`:

```c
// Path-based async CRUD
void database_subtree_put(database_subtree_t* st, path_t* path, identifier_t* value, promise_t* promise);
void database_subtree_get(database_subtree_t* st, path_t* path, promise_t* promise);
void database_subtree_delete(database_subtree_t* st, path_t* path, promise_t* promise);

// Raw async CRUD
int database_subtree_put_raw(database_subtree_t* st, const char* key, size_t key_len, char delimiter, const uint8_t* value, size_t value_len, promise_t* promise);
int database_subtree_get_raw(database_subtree_t* st, const char* key, size_t key_len, char delimiter, promise_t* promise);
int database_subtree_delete_raw(database_subtree_t* st, const char* key, size_t key_len, char delimiter, promise_t* promise);
```

- [ ] **Step 4: Implement async CRUD functions**

Each async function follows the pattern from `database_put`/`database_get`/`database_delete` in `database.c`:
1. Validate args, resolve promise on failure
2. For sync_only mode: call the sync variant and resolve the promise
3. Allocate context struct with `get_clear_memory`
4. Fill ctx: `st->db`, prefixed path (from `database_subtree_prepend_path`), value, promise
5. Create work with `work_create(worker_fn, abort_fn, ctx)`
6. `refcounter_yield((refcounter_t*) work)`
7. `work_pool_enqueue(database_subtree_get_pool(st), work)`

The worker functions call the underlying `database_put`/`database_get`/`database_delete` (NOT the sync variants). This is because the async workers already handle MVCC, WAL, and locking internally. The prefixed path is what gets passed to the underlying async function.

For raw async variants: build the prefixed key string with `database_subtree_prepend_key`, then delegate to the corresponding `database_*_raw` function.

- [ ] **Step 5: Run tests to verify they pass**

Run: `cmake --build build && ./build/test_subtree`
Expected: All async CRUD tests pass.

- [ ] **Step 6: Commit**

```bash
git add src/Database/database_subtree.h src/Database/database_subtree.c tests/test_subtree.c
git commit -m "feat: add async CRUD operations to database_subtree_t"
```

---

### Task 4: Batch and scan operations

**Files:**
- Modify: `src/Database/database_subtree.h` (add batch and scan declarations)
- Modify: `src/Database/database_subtree.c` (implement batch and scan)

- [ ] **Step 1: Write failing tests for batch and scan**

Add tests to `tests/test_subtree.c` that:
1. Write 5 key-value pairs via `database_subtree_batch_sync_raw`, verify all exist via get
2. Scan with `database_subtree_scan_sync_raw` using a prefix, verify results are scoped to the subtree (no keys from other subtrees)
3. Verify that scan results have the subtree prefix stripped from keys
4. Write via subtree at "graphql/Users/1/name", then raw database scan for "graphql/" should find it, but subtree scan for "Users/" should return "Users/1/name" (prefix stripped)

- [ ] **Step 2: Run tests to verify they fail**

Run: `cmake --build build && ./build/test_subtree`
Expected: Compilation errors.

- [ ] **Step 3: Add batch and scan declarations to the header**

```c
// Batch
int database_subtree_write_batch_sync(database_subtree_t* st, batch_t* batch);
void database_subtree_write_batch(database_subtree_t* st, batch_t* batch, promise_t* promise);
int database_subtree_batch_sync_raw(database_subtree_t* st, char delimiter, const raw_op_t* ops, size_t count);
int database_subtree_batch_raw(database_subtree_t* st, char delimiter, const raw_op_t* ops, size_t count, promise_t* promise);

// Scan/Iterator
database_iterator_t* database_subtree_scan_start(database_subtree_t* st, path_t* start_path, path_t* end_path);
database_iterator_t* database_subtree_scan_range(database_subtree_t* st, const char* start, const char* end);
int database_subtree_scan_sync_raw(database_subtree_t* st, const char* prefix, size_t prefix_len, char delimiter, raw_result_t** results, size_t* count);
int database_subtree_scan_range_sync_raw(database_subtree_t* st, const char* start_prefix, size_t start_len, const char* end_prefix, size_t end_len, char delimiter, raw_result_t** results, size_t* count);
```

- [ ] **Step 4: Implement batch operations**

For `database_subtree_write_batch_sync`: iterate over the batch entries, prefix each path/key, and delegate to `database_write_batch_sync` with a new batch containing the prefixed entries. The batch API in WaveDB uses `batch_t` which contains `batch_entry_t` items — each entry has a `path_t*` and `identifier_t*`. Create a new batch, prefix each entry's path, and call `database_write_batch_sync(db, new_batch)`.

For `database_subtree_batch_sync_raw`: prefix each key in the `raw_op_t` array using `database_subtree_prepend_key`, then delegate to `database_batch_sync_raw`.

- [ ] **Step 5: Implement scan operations**

For `database_subtree_scan_start`: prepend the prefix to `start_path` and `end_path` using `database_subtree_prepend_path`, then call `database_scan_start(st->db, prefixed_start, prefixed_end)`. The returned iterator walks the underlying database, so results will include the prefix in paths. The caller can strip the prefix if needed, but this is not required by the spec (the spec says "results have prefix stripped" — implement this by documenting that `database_scan_next` returns paths WITH the prefix, and the raw scan variants strip it).

For `database_subtree_scan_sync_raw`: call `database_scan_sync_raw` with the combined prefix (st->prefix + "/" + user_prefix). Then iterate over results and strip the subtree prefix from each key. Stripping: if the key starts with `st->prefix + st->delimiter`, remove that prefix portion. Allocate a new `raw_result_t` array with stripped keys.

For `database_subtree_scan_range_sync_raw`: same pattern but with start and end prefixes.

For `database_subtree_scan_range`: prepend subtree prefix to start and end strings, call `database_scan_range`, return the iterator.

- [ ] **Step 6: Run tests to verify they pass**

Run: `cmake --build build && ./build/test_subtree`
Expected: All batch and scan tests pass.

- [ ] **Step 7: Commit**

```bash
git add src/Database/database_subtree.h src/Database/database_subtree.c tests/test_subtree.c
git commit -m "feat: add batch and scan operations to database_subtree_t"
```

---

### Task 5: Snapshot and introspection

**Files:**
- Modify: `src/Database/database_subtree.h` (add snapshot/introspection declarations)
- Modify: `src/Database/database_subtree.c` (implement snapshot/introspection)

- [ ] **Step 1: Add snapshot and introspection declarations to the header**

```c
// Snapshot and flush — delegates to underlying database
int database_subtree_snapshot(database_subtree_t* st);
int database_subtree_flush_dirty_bnodes(database_subtree_t* st);

// Count entries under the subtree's prefix
size_t database_subtree_count(database_subtree_t* st);
```

- [ ] **Step 2: Implement snapshot and flush**

These are simple delegations:
- `database_subtree_snapshot(st)` → `database_snapshot(st->db)`
- `database_subtree_flush_dirty_bnodes(st)` → `database_flush_dirty_bnodes(st->db)`

- [ ] **Step 3: Implement count**

`database_subtree_count(st)` → `database_scan_sync_raw(st->db, st->prefix, st->prefix_len, st->delimiter, &results, &count)` then free results and return `count`. This scans all keys under the subtree prefix and counts them.

- [ ] **Step 4: Write tests for snapshot, flush, and count**

Add tests to `tests/test_subtree.c`:
1. Put 3 values via subtree, call `database_subtree_count`, verify count is 3
2. Call `database_subtree_snapshot`, verify no error

- [ ] **Step 5: Run tests**

Run: `cmake --build build && ./build/test_subtree`
Expected: All tests pass.

- [ ] **Step 6: Commit**

```bash
git add src/Database/database_subtree.h src/Database/database_subtree.c tests/test_subtree.c
git commit -m "feat: add snapshot, flush, and count to database_subtree_t"
```

---

### Task 6: Delete subtree

**Files:**
- Modify: `src/Database/database_subtree.c` (implement `database_subtree_delete`)

- [ ] **Step 1: Write failing test for subtree deletion**

Add tests to `tests/test_subtree.c`:
1. Put 5 values under subtree "graphql", verify they exist
2. Call `database_subtree_delete(db, "graphql", '/')`, verify all 5 values are gone
3. Verify that values under a different subtree (e.g., "graph") are NOT deleted
4. Verify that the database itself is still usable after deletion

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build && ./build/test_subtree`
Expected: Function not declared or not working.

- [ ] **Step 3: Implement database_subtree_delete**

```c
int database_subtree_delete(database_t* db, const char* prefix, char delimiter) {
    if (!db || !prefix) return -1;

    // Build the scan prefix: "prefix{delimiter}"
    size_t prefix_len = strlen(prefix);
    size_t scan_prefix_len = prefix_len + 1;  // prefix + delimiter
    char* scan_prefix = malloc(scan_prefix_len + 1);
    if (!scan_prefix) return -1;
    memcpy(scan_prefix, prefix, prefix_len);
    scan_prefix[prefix_len] = delimiter;
    scan_prefix[scan_prefix_len] = '\0';

    // Scan for all keys under the prefix
    raw_result_t* results = NULL;
    size_t count = 0;
    int rc = database_scan_sync_raw(db, scan_prefix, scan_prefix_len, delimiter, &results, &count);
    free(scan_prefix);

    if (rc != 0) return rc;

    // Delete each key
    for (size_t i = 0; i < count; i++) {
        database_delete_sync_raw(db, results[i].key, results[i].key_len, delimiter);
    }

    database_raw_results_free(results, count);
    return 0;
}
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `cmake --build build && ./build/test_subtree`
Expected: All deletion tests pass.

- [ ] **Step 5: Commit**

```bash
git add src/Database/database_subtree.c tests/test_subtree.c
git commit -m "feat: implement database_subtree_delete"
```

---

### Task 7: Create graph_layer_config_t

**Files:**
- Modify: `src/Layers/graph/graph.h` (add `graph_layer_config_t` and update `graph_layer_create` signature)
- Modify: `src/Layers/graph/graph.c` (update `graph_layer_create` to use new config)
- Modify: `src/Layers/graph/graph_internal.h` (add `database_subtree_t*` field to `graph_layer_t`)

This task creates the new `graph_layer_config_t` struct that the graph layer currently lacks. It also adds the `database_subtree_t*` field to `graph_layer_t`. This must be done before Task 9 (graph layer subtree integration).

- [ ] **Step 1: Add graph_layer_config_t to graph.h**

Add before `graph_layer_create`:

```c
typedef struct graph_layer_config_t {
    const char* path;               // Database storage path (NULL = in-memory)
    database_config_t* db_config;  // Database configuration (NULL = defaults)
} graph_layer_config_t;
```

Update `graph_layer_create` signature to accept the new config and optional subtree:

```c
graph_layer_t* graph_layer_create(const char* path,
                                   graph_layer_config_t* config,
                                   database_subtree_t* subtree);
```

Add `#include "Database/database_subtree.h"` to the includes.

- [ ] **Step 2: Add database_subtree_t* field to graph_layer_t**

In `src/Layers/graph/graph_internal.h`, add a `database_subtree_t* subtree` field to `graph_layer_t`:

```c
struct graph_layer_t {
    database_t* db;
    database_subtree_t* subtree;   // Non-NULL if using a subtree
    graph_schema_t* schema;
    vec_t(morphism_entry_t) morphisms;
    graph_stats_t* stats;
    int stats_computed;
    PLATFORMRWLOCKTYPE(lock);
};
```

- [ ] **Step 3: Update graph_layer_create implementation**

In `src/Layers/graph/graph.c`, update `graph_layer_create`:

When `subtree` is non-NULL: set `layer->db = database_subtree_get_db(subtree)` and `layer->subtree = subtree`. Skip database creation. When `subtree` is NULL: use `config->db_config` (or `config->path`) to create the database, same as before.

When `config` is NULL: create a `graph_layer_config_t` with defaults (equivalent to current NULL config behavior).

- [ ] **Step 4: Update all callers of graph_layer_create**

Search for all existing calls to `graph_layer_create` and add `NULL` as the third parameter (subtree). This includes:
- `bindings/nodejs/src/graph_layer.cc`
- `bindings/dart/lib/src/graph_layer.dart`
- `tests/test_graph_set.c`
- Any other callers found via grep

Each caller gets `NULL` as the third argument, preserving backward compatibility.

- [ ] **Step 5: Update graph_layer_destroy**

In `graph_layer_destroy`: if `layer->subtree` is non-NULL, call `database_subtree_close(layer->subtree)`. Only call `database_destroy(layer->db)` if `layer->subtree` is NULL (the layer owns the database only if it created it).

- [ ] **Step 6: Build and run existing tests**

Run: `cmake --build build && ctest --test-dir build`
Expected: All existing tests still pass (backward compatibility).

- [ ] **Step 7: Commit**

```bash
git add src/Layers/graph/graph.h src/Layers/graph/graph_internal.h src/Layers/graph/graph.c bindings/nodejs/src/graph_layer.cc bindings/dart/lib/src/graph_layer.dart bindings/dart/lib/src/native/wavedb_bindings.dart
git commit -m "feat: add graph_layer_config_t and update graph_layer_create for subtree support"
```

---

### Task 8: Update GraphQL layer for subtree support

**Files:**
- Modify: `src/Layers/graphql/graphql_schema.h` (update `graphql_layer_create` signature)
- Modify: `src/Layers/graphql/graphql_schema.c` (implement subtree path in `graphql_layer_create`)
- Modify: `src/Layers/graphql/graphql_types.h` (add `database_subtree_t*` field to `graphql_layer_t`)

- [ ] **Step 1: Add subtree field to graphql_layer_t**

In `src/Layers/graphql/graphql_types.h`, add `database_subtree_t* subtree` to `graphql_layer_t`:

```c
struct graphql_layer_t {
    refcounter_t refcounter;
    database_t* db;
    database_subtree_t* subtree;   // Non-NULL if using a subtree
    graphql_type_registry_t* registry;
    work_pool_t* pool;
    char delimiter;
    char* version;
    char* db_path;
    bool owns_db_path;
    char* query_type;
    char* mutation_type;
};
```

- [ ] **Step 2: Update graphql_layer_create signature**

In `src/Layers/graphql/graphql_schema.h`, update:

```c
graphql_layer_t* graphql_layer_create(const char* path,
                                       const graphql_layer_config_t* config,
                                       database_subtree_t* subtree);
```

Add `#include "Database/database_subtree.h"`.

- [ ] **Step 3: Update graphql_layer_create implementation**

In `src/Layers/graphql/graphql_schema.c`, update `graphql_layer_create`:

When `subtree` is non-NULL:
- Set `layer->db = database_subtree_get_db(subtree)`
- Set `layer->subtree = subtree`
- Set `layer->pool = database_subtree_get_pool(subtree)`
- Skip database creation (don't call `database_create_with_config`)
- Set `layer->db_path = NULL` and `layer->owns_db_path = false`

When `subtree` is NULL: existing behavior unchanged (create database from config).

Then update all internal database operations to use `database_subtree_*` when `layer->subtree` is non-NULL. This includes:
- `db_put_string` / `db_get_string` — these use `database_put_sync` / `database_get_sync`. Add a wrapper that checks `layer->subtree` and delegates to the subtree variant.
- All other `database_*_sync` calls in `graphql_schema.c` and `graphql_resolve.c` — wrap in conditional: if `layer->subtree` use `database_subtree_*`, else use `database_*`.

For the `db->pool` access in `graphql_resolve.c` (line 318): replace with `database_subtree_get_pool(layer->subtree)` when subtree is set, otherwise `layer->db->pool`.

- [ ] **Step 4: Update graphql_layer_destroy**

In `graphql_layer_destroy`: if `layer->subtree` is non-NULL, call `database_subtree_close(layer->subtree)`. Only call `database_destroy(layer->db)` if `layer->subtree` is NULL.

- [ ] **Step 5: Update all callers of graphql_layer_create**

Search for all existing calls and add `NULL` as the third parameter. This includes:
- `bindings/nodejs/src/graphql_layer.cc`
- `bindings/dart/lib/src/graphql_layer.dart`
- Any test files

- [ ] **Step 6: Build and run existing tests**

Run: `cmake --build build && ctest --test-dir build`
Expected: All existing tests still pass.

- [ ] **Step 7: Commit**

```bash
git add src/Layers/graphql/graphql_types.h src/Layers/graphql/graphql_schema.h src/Layers/graphql/graphql_schema.c bindings/nodejs/src/graphql_layer.cc bindings/dart/lib/src/graphql_layer.dart
git commit -m "feat: add subtree support to graphql_layer_create"
```

---

### Task 9: Update Graph layer for subtree support

**Files:**
- Modify: `src/Layers/graph/graph.c` (use subtree in all database operations)
- Modify: `src/Layers/graph/graph_ops.c` (use subtree in scan operations)
- Modify: `src/Layers/graph/graph_schema_parser.c` (use subtree in schema load/save)

This task depends on Task 7 (graph_layer_config_t) being complete.

- [ ] **Step 1: Update graph_layer_create to use subtree**

In `src/Layers/graph/graph.c`, update `graph_layer_create`:
When `subtree` is non-NULL:
- Set `layer->db = database_subtree_get_db(subtree)`
- Set `layer->subtree = subtree`
- Skip database creation

When `subtree` is NULL: existing behavior (use `config->db_config` or create default database).

- [ ] **Step 2: Update all database operations in graph.c to use subtree**

In `graph.c`, find all `database_*_sync`, `database_*_sync_raw`, `database_*` (async), and `db->pool` calls. For each:
- If `layer->subtree` is set, use the corresponding `database_subtree_*` function
- If `layer->subtree` is NULL, use the existing `database_*` function

For `db->pool` access: replace with `database_subtree_get_pool(layer->subtree)` when subtree is set, otherwise `layer->db->pool`.

- [ ] **Step 3: Update graph_ops.c**

In `src/Layers/graph/graph_ops.c`, update all database operations the same way. This file handles scan operations, so `database_scan_sync_raw` and `database_scan_range_sync_raw` calls need `database_subtree_scan_sync_raw` equivalents.

- [ ] **Step 4: Update graph_schema_parser.c**

In `src/Layers/graph/graph_schema_parser.c`, update `db_get_string`/`db_put_string` style calls. These use the raw API, so they need `database_subtree_get_sync_raw`/`database_subtree_put_sync_raw` equivalents.

- [ ] **Step 5: Build and run existing tests**

Run: `cmake --build build && ctest --test-dir build`
Expected: All existing tests still pass.

- [ ] **Step 6: Commit**

```bash
git add src/Layers/graph/graph.c src/Layers/graph/graph_ops.c src/Layers/graph/graph_schema_parser.c
git commit -m "feat: use database_subtree_t in graph layer operations"
```

---

### Task 10: Comprehensive C tests

**Files:**
- Modify: `tests/test_subtree.c` (expand with comprehensive tests)

- [ ] **Step 1: Add cross-subtree isolation tests**

Tests that verify two subtrees on the same database are fully isolated:
1. Create one database, open two subtrees ("graphql" and "graph")
2. Put data into each subtree
3. Verify each subtree only sees its own data via get and scan
4. Verify `database_subtree_count` returns correct count per subtree

- [ ] **Step 2: Add reopen test**

Tests that verify subtree data persists across reopen:
1. Create database, open subtree, put data, close subtree, destroy database
2. Reopen database, reopen subtree with same prefix, verify data is still there

- [ ] **Step 3: Add delete test**

Tests that verify `database_subtree_delete` works:
1. Put data into two subtrees
2. Delete one subtree's data
3. Verify the deleted subtree returns empty on scan
4. Verify the other subtree is unaffected

- [ ] **Step 4: Add layer integration test**

Test that creates a GraphQL layer with a subtree:
1. Create database, open subtree, create graphql layer with subtree
2. Parse a schema, verify it stores data under the subtree prefix
3. Destroy the layer (should close subtree but not destroy database)
4. Reopen with same subtree, verify schema data persists

- [ ] **Step 5: Run all tests**

Run: `cmake --build build && ./build/test_subtree`
Expected: All tests pass.

- [ ] **Step 6: Commit**

```bash
git add tests/test_subtree.c
git commit -m "test: add comprehensive subtree isolation, reopen, and delete tests"
```

---

### Task 11: Node.js bindings

**Files:**
- Create: `bindings/nodejs/src/subtree.cc`
- Create: `bindings/nodejs/src/subtree.h`
- Modify: `bindings/nodejs/src/database.cc` (add `openSubtree`, `deleteSubtree` methods)
- Modify: `bindings/nodejs/src/graph_layer.cc` (add `subtree` option)
- Modify: `bindings/nodejs/src/graphql_layer.cc` (add `subtree` option)
- Modify: `bindings/nodejs/test/graph.test.js` (add subtree-based GraphLayer creation test)
- Create: `bindings/nodejs/test/subtree.test.js`
- Modify: `bindings/nodejs/binding.gyp` (add subtree.cc to sources)

- [ ] **Step 1: Create Subtree class header and implementation**

`bindings/nodejs/src/subtree.h`: Napi::ObjectWrap subclass wrapping `database_subtree_t*`.

`bindings/nodejs/src/subtree.cc`: Implement all CRUD, batch, scan, snapshot methods as Napi methods. Each method:
- Gets the `database_subtree_t*` from the wrapper
- Converts Napi args to C types
- Calls the corresponding `database_subtree_*` function
- Converts results back to Napi types

Key methods: `Put`, `Get`, `Del`, `PutSync`, `GetSync`, `DelSync`, `Batch`, `BatchSync`, `ScanRange`, `ScanSyncRaw`, `Close`, `Count`.

- [ ] **Step 2: Add openSubtree and deleteSubtree to WaveDB class**

In `bindings/nodejs/src/database.cc`, add two methods:
- `OpenSubtree(prefix, delimiter)` → creates a `database_subtree_t*` via `database_subtree_open`, wraps in `Subtree` Napi object
- `DeleteSubtree(prefix, delimiter)` → calls `database_subtree_delete`

- [ ] **Step 3: Update GraphLayer constructor**

In `bindings/nodejs/src/graph_layer.cc`, add an optional `subtree` option to the constructor. If provided, call `graph_layer_create(path, config, subtree_ptr)` instead of `graph_layer_create(path, config, NULL)`.

- [ ] **Step 4: Update GraphQLLayer constructor**

In `bindings/nodejs/src/graphql_layer.cc`, add an optional `subtree` option. If provided, call `graphql_layer_create(path, config, subtree_ptr)`.

- [ ] **Step 5: Update binding.gyp**

Add `src/subtree.cc` to the sources list.

- [ ] **Step 6: Write Node.js test file**

Create `bindings/nodejs/test/subtree.test.js` with tests:
- Open a subtree from a WaveDB instance
- Put/get/delete via subtree
- Verify cross-subtree isolation
- Delete subtree data

- [ ] **Step 7: Build and run Node.js tests**

Run: `cd bindings/nodejs && npm run build && npm test`
Expected: All Node.js tests pass.

- [ ] **Step 8: Commit**

```bash
git add bindings/nodejs/src/subtree.cc bindings/nodejs/src/subtree.h bindings/nodejs/src/database.cc bindings/nodejs/src/graph_layer.cc bindings/nodejs/src/graphql_layer.cc bindings/nodejs/test/subtree.test.js bindings/nodejs/binding.gyp
git commit -m "feat: add Node.js bindings for database_subtree_t"
```

---

### Task 12: Dart bindings

**Files:**
- Create: `bindings/dart/lib/src/subtree.dart`
- Modify: `bindings/dart/lib/src/native/wavedb_bindings.dart` (add FFI typedefs)
- Modify: `bindings/dart/lib/src/native/types.dart` (add `database_subtree_t` opaque type, `graph_layer_config_t`)
- Modify: `bindings/dart/lib/src/database.dart` (add `openSubtree`, `deleteSubtree`)
- Modify: `bindings/dart/lib/src/graph_layer.dart` (add `GraphLayerConfig`, `subtree` param)
- Modify: `bindings/dart/lib/src/graphql_layer.dart` (add `subtree` param)
- Modify: `bindings/dart/lib/wavedb.dart` (export `Subtree`, `GraphLayerConfig`)
- Modify: `bindings/dart/test/graph_layer_test.dart` (add subtree-based creation test)
- Create: `bindings/dart/test/subtree_test.dart`

- [ ] **Step 1: Add FFI typedefs and types**

In `bindings/dart/lib/src/native/wavedb_bindings.dart`, add FFI typedefs for all `database_subtree_*` functions.

In `bindings/dart/lib/src/native/types.dart`, add `class DatabaseSubtree extends Opaque {}` and the `GraphLayerConfig` struct.

- [ ] **Step 2: Create Subtree Dart class**

`bindings/dart/lib/src/subtree.dart`: Wraps `database_subtree_t*` via FFI. Implements CRUD, batch, scan, snapshot methods by calling the FFI bindings.

- [ ] **Step 3: Add openSubtree and deleteSubtree to WaveDB class**

In `bindings/dart/lib/src/database.dart`:
- `openSubtree(String prefix, String delimiter)` → calls `database_subtree_open` via FFI, returns `Subtree` object
- `deleteSubtree(String prefix, String delimiter)` → calls `database_subtree_delete` via FFI

- [ ] **Step 4: Update GraphLayer class**

In `bindings/dart/lib/src/graph_layer.dart`:
- Add `GraphLayerConfig` class with `path` and `dbConfig` fields
- Update `GraphLayer` constructor to accept optional `subtree` parameter
- Update FFI call to `graph_layer_create` with new signature

- [ ] **Step 5: Update GraphQLLayer class**

In `bindings/dart/lib/src/graphql_layer.dart`:
- Update `GraphQLLayerConfig` to accept optional `subtree` parameter
- Update `create()` method to pass subtree to `graphql_layer_create`

- [ ] **Step 6: Write Dart test file**

Create `bindings/dart/test/subtree_test.dart` with tests matching the Node.js test file.

- [ ] **Step 7: Build and run Dart tests**

Run: `cd bindings/dart && dart test`
Expected: All Dart tests pass.

- [ ] **Step 8: Commit**

```bash
git add bindings/dart/lib/src/subtree.dart bindings/dart/lib/src/native/wavedb_bindings.dart bindings/dart/lib/src/native/types.dart bindings/dart/lib/src/database.dart bindings/dart/lib/src/graph_layer.dart bindings/dart/lib/src/graphql_layer.dart bindings/dart/lib/wavedb.dart bindings/dart/test/subtree_test.dart
git commit -m "feat: add Dart bindings for database_subtree_t"
```

---

### Task 13: README updates

**Files:**
- Modify: `README.md`
- Modify: `bindings/nodejs/README.md`
- Modify: `bindings/dart/README.md`

- [ ] **Step 1: Add Subtree API section to root README**

Add a "Subtree API" section to `README.md` after the database operations section, documenting:
- `database_subtree_open(db, prefix, delimiter)` with a C code example
- `database_subtree_close(subtree)`
- `database_subtree_delete(db, prefix, delimiter)`
- CRUD, batch, and scan operations on subtrees
- How to use subtrees with layers (pass as optional parameter)

- [ ] **Step 2: Update GraphQL and Graph layer sections in root README**

Update the layer creation examples to show the subtree parameter:
```c
// With subtree
database_subtree_t* subtree = database_subtree_open(db, "layer/graphql", '/');
graphql_layer_t* gql = graphql_layer_create(NULL, &config, subtree);

// Without subtree (existing behavior)
graphql_layer_t* gql = graphql_layer_create("/path/to/db", &config, NULL);
```

- [ ] **Step 3: Update Node.js README**

In `bindings/nodejs/README.md`:
- Add `WaveDB.openSubtree(prefix, delimiter)` method documentation
- Add `WaveDB.deleteSubtree(prefix, delimiter)` method documentation
- Document the Subtree class and its methods
- Update `new GraphLayer(options)` to document the `subtree` option
- Update `new GraphQLLayer(path, options)` to document the `subtree` option

- [ ] **Step 4: Update Dart README**

In `bindings/dart/README.md`:
- Add `WaveDB.openSubtree(prefix, delimiter)` method documentation
- Add `WaveDB.deleteSubtree(prefix, delimiter)` method documentation
- Document the Subtree class and its methods
- Document the new `GraphLayerConfig` class
- Update `GraphQLLayer.create()` to document the `subtree` parameter

- [ ] **Step 5: Commit**

```bash
git add README.md bindings/nodejs/README.md bindings/dart/README.md
git commit -m "docs: add subtree API documentation to all READMEs"
```