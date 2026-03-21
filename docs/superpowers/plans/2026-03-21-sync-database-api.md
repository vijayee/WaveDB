# Synchronous Database API Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add synchronous versions of core database operations (put, get, delete) that execute in the caller thread without using the work pool.

**Architecture:** Inline implementations that duplicate async logic without context/promise wrappers. Three new functions in database.c, declarations in database.h, single-threaded benchmarks in benchmark_database_sync.cpp.

**Tech Stack:** C (existing codebase), GoogleTest for benchmarking

---

## File Structure

### Modified Files

**src/Database/database.h** - Add three function declarations
**src/Database/database.c** - Add three function implementations
**CMakeLists.txt** - Add benchmark target

### Created Files

**tests/benchmark/benchmark_database_sync.cpp** - Single-threaded benchmarks

---

## Task 1: Add Synchronous API Declarations

**Files:**
- Modify: `src/Database/database.h` (after line 156, before `#ifdef __cplusplus`)

- [ ] **Step 1: Add database_put_sync declaration**

Add after `database_count` declaration (around line 157):

```c
/**
 * Synchronously insert a value.
 *
 * @param db       Database to modify
 * @param path     Path key (takes ownership of reference)
 * @param value    Value to store (takes ownership of reference)
 * @return 0 on success, -1 on error
 */
int database_put_sync(database_t* db, path_t* path, identifier_t* value);
```

- [ ] **Step 2: Add database_get_sync declaration**

Add immediately after:

```c
/**
 * Synchronously get a value.
 *
 * Checks LRU cache first, then trie.
 *
 * @param db       Database to query
 * @param path     Path key to find (takes ownership, always consumed)
 * @param result   Output: found value (caller must destroy) or NULL if not found
 *                 Caller should NOT initialize *result before calling
 * @return 0 on success (result found), -1 on error, -2 on not found
 */
int database_get_sync(database_t* db, path_t* path, identifier_t** result);
```

- [ ] **Step 3: Add database_delete_sync declaration**

Add immediately after:

```c
/**
 * Synchronously delete a value.
 *
 * @param db       Database to modify
 * @param path     Path key to delete (takes ownership)
 * @return 0 on success, -1 on error
 */
int database_delete_sync(database_t* db, path_t* path);
```

- [ ] **Step 4: Verify compilation**

Run: `cd build-test && make database`
Expected: Compiles successfully with new declarations

- [ ] **Step 5: Commit**

```bash
git add src/Database/database.h
git commit -m "feat: add synchronous database API declarations

Co-Authored-By: Claude Sonnet 4.6 <noreply@anthropic.com>"
```

---

## Task 2: Implement database_put_sync

**Files:**
- Modify: `src/Database/database.c` (after database_count function, before closing brace)

- [ ] **Step 1: Add database_put_sync implementation**

Add at end of file before final closing brace:

```c

// ============================================================================
// Synchronous API Implementation
// ============================================================================

int database_put_sync(database_t* db, path_t* path, identifier_t* value) {
    // Validation (same as async)
    if (db == NULL || path == NULL || value == NULL) {
        if (path) path_destroy(path);
        if (value) identifier_destroy(value);
        return -1;
    }

    // Begin MVCC transaction
    txn_desc_t* txn = tx_manager_begin(db->tx_manager);
    if (txn == NULL) {
        path_destroy(path);
        identifier_destroy(value);
        return -1;
    }

    // Write to thread-local WAL
    buffer_t* entry = encode_put_entry(path, value);
    if (entry != NULL) {
        thread_wal_t* twal = get_thread_wal(db->wal_manager);
        if (twal != NULL) {
            int result = thread_wal_write(twal, txn->txn_id, WAL_PUT, entry);
            if (result != 0) {
                log_warn("Failed to write to thread-local WAL");
            }
        }
        buffer_destroy(entry);
    }

    // Acquire sharded write lock
    size_t shard = get_write_lock_shard(path);
    platform_lock(&db->write_locks[shard]);

    // Apply to trie with MVCC
    hbtrie_insert_mvcc(db->trie, path, value, txn->txn_id);

    // Release write lock
    platform_unlock(&db->write_locks[shard]);

    // Commit transaction
    tx_manager_commit(db->tx_manager, txn);

    // Update LRU cache
    path_t* copied_path = path_copy(path);
    identifier_t* value_ref = REFERENCE(value, identifier_t);
    identifier_t* ejected = database_lru_cache_put(db->lru, copied_path, value_ref);
    if (ejected) {
        identifier_destroy(ejected);
    }

    // Cleanup
    path_destroy(path);
    identifier_destroy(value);
    txn_desc_destroy(txn);

    return 0;
}
```

- [ ] **Step 2: Verify compilation**

Run: `cd build-test && make database`
Expected: Compiles successfully

- [ ] **Step 3: Commit**

```bash
git add src/Database/database.c
git commit -m "feat: implement database_put_sync

Co-Authored-By: Claude Sonnet 4.6 <noreply@anthropic.com>"
```

---

## Task 3: Implement database_get_sync

**Files:**
- Modify: `src/Database/database.c` (after database_put_sync)

- [ ] **Step 1: Add database_get_sync implementation**

Add after `database_put_sync`:

```c

int database_get_sync(database_t* db, path_t* path, identifier_t** result) {
    // Initialize output
    if (result == NULL) {
        if (path) path_destroy(path);
        return -1;
    }
    *result = NULL;

    // Validation
    if (db == NULL || path == NULL) {
        if (path) path_destroy(path);
        return -1;
    }

    // Check LRU cache first
    identifier_t* value = database_lru_cache_get(db->lru, path);
    if (value != NULL) {
        path_destroy(path);
        *result = value;
        return 0;
    }

    // Get last committed transaction ID (lock-free read)
    transaction_id_t read_txn_id = tx_manager_get_last_committed(db->tx_manager);

    // Look up in trie with MVCC (lock-free!)
    value = hbtrie_find_mvcc(db->trie, path, read_txn_id);

    // Add to LRU cache if found
    if (value != NULL) {
        path_t* copied_path = path_copy(path);
        identifier_t* cached = REFERENCE(value, identifier_t);
        database_lru_cache_put(db->lru, copied_path, cached);
    }

    path_destroy(path);

    if (value != NULL) {
        *result = value;
        return 0;
    } else {
        return -2;  // Not found
    }
}
```

- [ ] **Step 2: Verify compilation**

Run: `cd build-test && make database`
Expected: Compiles successfully

- [ ] **Step 3: Commit**

```bash
git add src/Database/database.c
git commit -m "feat: implement database_get_sync

Co-Authored-By: Claude Sonnet 4.6 <noreply@anthropic.com>"
```

---

## Task 4: Implement database_delete_sync

**Files:**
- Modify: `src/Database/database.c` (after database_get_sync)

- [ ] **Step 1: Add database_delete_sync implementation**

Add after `database_get_sync`:

```c

int database_delete_sync(database_t* db, path_t* path) {
    // Validation
    if (db == NULL || path == NULL) {
        if (path) path_destroy(path);
        return -1;
    }

    // Begin transaction
    txn_desc_t* txn = tx_manager_begin(db->tx_manager);
    if (txn == NULL) {
        path_destroy(path);
        return -1;
    }

    // Write to thread-local WAL
    buffer_t* entry = encode_delete_entry(path);
    if (entry != NULL) {
        thread_wal_t* twal = get_thread_wal(db->wal_manager);
        if (twal != NULL) {
            int result = thread_wal_write(twal, txn->txn_id, WAL_DELETE, entry);
            if (result != 0) {
                log_warn("Failed to write to thread-local WAL");
            }
        }
        buffer_destroy(entry);
    }

    // Acquire sharded write lock
    size_t shard = get_write_lock_shard(path);
    platform_lock(&db->write_locks[shard]);

    // Remove from trie with MVCC (creates tombstone)
    identifier_t* removed = hbtrie_delete_mvcc(db->trie, path, txn->txn_id);

    // Release write lock
    platform_unlock(&db->write_locks[shard]);

    // Commit transaction
    tx_manager_commit(db->tx_manager, txn);
    txn_desc_destroy(txn);

    // Remove from LRU cache
    database_lru_cache_delete(db->lru, path);

    path_destroy(path);

    if (removed) {
        identifier_destroy(removed);
    }

    return 0;
}
```

- [ ] **Step 2: Verify compilation**

Run: `cd build-test && make database`
Expected: Compiles successfully

- [ ] **Step 3: Commit**

```bash
git add src/Database/database.c
git commit -m "feat: implement database_delete_sync

Co-Authored-By: Claude Sonnet 4.6 <noreply@anthropic.com>"
```

---

## Task 5: Create Synchronous Database Benchmark

**Files:**
- Create: `tests/benchmark/benchmark_database_sync.cpp`

- [ ] **Step 1: Create benchmark file with single-threaded benchmarks**

Create `tests/benchmark/benchmark_database_sync.cpp` with this complete content:

```cpp
//
// Synchronous Database Benchmarks
// Performance comparison: sync vs async API
//

#include "benchmark_base.h"
#include "../../src/Database/database.h"
#include "../../src/Buffer/buffer.h"
#include "../../src/Workers/transaction_id.h"
#include "../../src/Util/allocator.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#ifdef __cplusplus
extern "C" {
#endif

// Test context for sync benchmarks (no work pool or timing wheel needed)
typedef struct {
    database_t* db;
    char test_dir[256];
    uint64_t counter;
} sync_benchmark_ctx_t;

// Initialize sync benchmark database
static void sync_benchmark_init(sync_benchmark_ctx_t* ctx, const char* test_name) {
    snprintf(ctx->test_dir, sizeof(ctx->test_dir), "/tmp/sync_db_bench_%s_%d", test_name, getpid());
    mkdir(ctx->test_dir, 0755);

    // Configure WAL with large file size for benchmarks
    wal_config_t wal_config;
    wal_config.sync_mode = WAL_SYNC_ASYNC;  // Use ASYNC for performance testing
    wal_config.debounce_ms = WAL_DEFAULT_DEBOUNCE_MS;
    wal_config.idle_threshold_ms = WAL_DEFAULT_IDLE_THRESHOLD_MS;
    wal_config.compact_interval_ms = WAL_DEFAULT_COMPACT_INTERVAL_MS;
    wal_config.max_file_size = 100 * 1024 * 1024;  // 100MB

    int error = 0;
    ctx->db = database_create(ctx->test_dir, 50, &wal_config, 0, 4096, 0, 0, NULL, NULL, &error);
    ctx->counter = 0;

    if (error != 0 || ctx->db == NULL) {
        fprintf(stderr, "Failed to create database: error_code=%d\n", error);
        exit(1);
    }
}

// Cleanup sync benchmark database
static void sync_benchmark_cleanup(sync_benchmark_ctx_t* ctx) {
    if (ctx->db) {
        database_destroy(ctx->db);
    }

    // Remove test directory
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", ctx->test_dir);
    system(cmd);
}

// Generate test key path
static path_t* generate_test_path(uint64_t counter) {
    char key[64];
    snprintf(key, sizeof(key), "key_%lu", counter);

    identifier_t* id = identifier_create(strlen(key));
    memcpy(id->data, key, strlen(key));

    path_t* path = path_create();
    path_append(path, id);

    identifier_destroy(id);
    return path;
}

// Generate test value
static identifier_t* generate_test_value(uint64_t counter) {
    char value[128];
    snprintf(value, sizeof(value), "value_%lu_test_data_for_database", counter);

    identifier_t* id = identifier_create(strlen(value));
    memcpy(id->data, value, strlen(value));

    return id;
}

// Benchmark: Put operation
static void benchmark_sync_put(void* user_data, uint64_t iterations) {
    sync_benchmark_ctx_t* ctx = (sync_benchmark_ctx_t*)user_data;

    for (uint64_t i = 0; i < iterations; i++) {
        path_t* path = generate_test_path(ctx->counter);
        identifier_t* value = generate_test_value(ctx->counter++);

        int result = database_put_sync(ctx->db, path, value);

        if (result != 0) {
            fprintf(stderr, "Put failed at iteration %lu\n", i);
        }
    }
}

// Benchmark: Get operation
static void benchmark_sync_get(void* user_data, uint64_t iterations) {
    sync_benchmark_ctx_t* ctx = (sync_benchmark_ctx_t*)user_data;

    for (uint64_t i = 0; i < iterations; i++) {
        path_t* path = generate_test_path(i % ctx->counter);  // Get existing keys
        identifier_t* result = NULL;

        int rc = database_get_sync(ctx->db, path, &result);

        if (rc == 0 && result != NULL) {
            identifier_destroy(result);
        }
    }
}

// Benchmark: Mixed put/get operations
static void benchmark_sync_mixed(void* user_data, uint64_t iterations) {
    sync_benchmark_ctx_t* ctx = (sync_benchmark_ctx_t*)user_data;

    for (uint64_t i = 0; i < iterations; i++) {
        if (i % 2 == 0) {
            // Put operation
            path_t* path = generate_test_path(ctx->counter);
            identifier_t* value = generate_test_value(ctx->counter++);

            int result = database_put_sync(ctx->db, path, value);

            if (result != 0) {
                fprintf(stderr, "Put failed at iteration %lu\n", i);
            }
        } else {
            // Get operation
            path_t* path = generate_test_path(i % ctx->counter);
            identifier_t* result = NULL;

            int rc = database_get_sync(ctx->db, path, &result);

            if (rc == 0 && result != NULL) {
                identifier_destroy(result);
            }
        }
    }
}

// Benchmark: Delete operation
static void benchmark_sync_delete(void* user_data, uint64_t iterations) {
    sync_benchmark_ctx_t* ctx = (sync_benchmark_ctx_t*)user_data;

    // First insert keys
    for (uint64_t i = 0; i < iterations; i++) {
        path_t* path = generate_test_path(i);
        identifier_t* value = generate_test_value(i);
        database_put_sync(ctx->db, path, value);
    }

    // Then delete them
    for (uint64_t i = 0; i < iterations; i++) {
        path_t* path = generate_test_path(i);

        int result = database_delete_sync(ctx->db, path);

        if (result != 0) {
            fprintf(stderr, "Delete failed at iteration %lu\n", i);
        }
    }
}

#ifdef __cplusplus
}
#endif

// Run all benchmarks
void run_sync_benchmarks(void) {
    printf("========================================\n");
    printf("Synchronous Database Benchmarks\n");
    printf("========================================\n\n");

    sync_benchmark_ctx_t ctx;

    // Single-threaded: Put
    printf("Running Sync Put benchmark...\n");
    sync_benchmark_init(&ctx, "put");
    benchmark_metrics_t put_metrics = benchmark_run(
        "Sync Put",
        benchmark_sync_put,
        &ctx,
        10,      // warmup
        10000    // measurement iterations
    );
    benchmark_print_results(&put_metrics);
    benchmark_save_json(".benchmarks/sync_put.json", &put_metrics);
    sync_benchmark_cleanup(&ctx);

    // Single-threaded: Get
    printf("\nRunning Sync Get benchmark...\n");
    sync_benchmark_init(&ctx, "get");
    // Pre-populate with keys
    for (int i = 0; i < 10000; i++) {
        path_t* path = generate_test_path(i);
        identifier_t* value = generate_test_value(i);
        database_put_sync(ctx.db, path, value);
    }
    ctx.counter = 10000;  // Track how many keys exist

    benchmark_metrics_t get_metrics = benchmark_run(
        "Sync Get",
        benchmark_sync_get,
        &ctx,
        10,
        10000
    );
    benchmark_print_results(&get_metrics);
    benchmark_save_json(".benchmarks/sync_get.json", &get_metrics);
    sync_benchmark_cleanup(&ctx);

    // Single-threaded: Mixed
    printf("\nRunning Sync Mixed benchmark...\n");
    sync_benchmark_init(&ctx, "mixed");
    benchmark_metrics_t mixed_metrics = benchmark_run(
        "Sync Mixed",
        benchmark_sync_mixed,
        &ctx,
        10,
        10000
    );
    benchmark_print_results(&mixed_metrics);
    benchmark_save_json(".benchmarks/sync_mixed.json", &mixed_metrics);
    sync_benchmark_cleanup(&ctx);

    // Single-threaded: Delete
    printf("\nRunning Sync Delete benchmark...\n");
    sync_benchmark_init(&ctx, "delete");
    benchmark_metrics_t delete_metrics = benchmark_run(
        "Sync Delete",
        benchmark_sync_delete,
        &ctx,
        10,
        10000
    );
    benchmark_print_results(&delete_metrics);
    benchmark_save_json(".benchmarks/sync_delete.json", &delete_metrics);
    sync_benchmark_cleanup(&ctx);

    printf("========================================\n\n");
}

int main(int argc, char** argv) {
    // Initialize transaction ID system
    transaction_id_init();

    // Create benchmark directory if it doesn't exist
    system("mkdir -p .benchmarks");

    run_sync_benchmarks();

    return 0;
}
```

- [ ] **Step 2: Commit**

```bash
git add tests/benchmark/benchmark_database_sync.cpp
git commit -m "feat: add synchronous database benchmarks (single-threaded)

Co-Authored-By: Claude Sonnet 4.6 <noreply@anthropic.com>"
```

---

## Task 6: Add Benchmark to Build System

**Files:**
- Modify: `CMakeLists.txt` (after line 245, after benchmark_database)

- [ ] **Step 1: Add benchmark_database_sync to CMakeLists.txt**

After the `benchmark_database` target (around line 245), add:

```cmake

# Sync Database Benchmark
add_executable(benchmark_database_sync tests/benchmark/benchmark_database_sync.cpp)
target_link_libraries(benchmark_database_sync benchmark_base wavedb Threads::Threads)
```

- [ ] **Step 2: Regenerate build files**

Run: `cd build-test && cmake ..`
Expected: CMake configuration succeeds

- [ ] **Step 3: Verify compilation**

Run: `cd build-test && make benchmark_database_sync`
Expected: Compiles successfully

- [ ] **Step 4: Commit**

```bash
git add CMakeLists.txt
git commit -m "build: add benchmark_database_sync to build system

Co-Authored-By: Claude Sonnet 4.6 <noreply@anthropic.com>"
```

---

## Task 7: Run Benchmarks and Verify

**Files:**
- Run: benchmark_database_sync executable

- [ ] **Step 1: Run synchronous benchmarks**

Run: `cd build-test && ./tests/benchmark/benchmark_database_sync`
Expected: Benchmarks run successfully, output performance metrics

- [ ] **Step 2: Verify no memory leaks**

Run: `cd build-test && valgrind --leak-check=full ./tests/benchmark/benchmark_database_sync 2>&1 | grep "definitely lost"`
Expected: "definitely lost: 0 bytes in 0 blocks"

- [ ] **Step 3: Compare sync vs async performance**

Review benchmark output:
- Single-threaded sync should be within ±10% of async (from existing benchmarks)
- No errors in any benchmark

- [ ] **Step 4: Commit benchmark results**

```bash
git add .benchmarks/sync_*.json
git commit -m "chore: add synchronous database benchmark results

Co-Authored-By: Claude Sonnet 4.6 <noreply@anthropic.com>"
```

---

## Success Criteria

- ✅ All three synchronous functions compile without errors
- ✅ Sync functions follow same patterns as async (MVCC, WAL, LRU)
- ✅ Benchmarks compile and run successfully
- ✅ No memory leaks in valgrind
- ✅ Performance within ±10% of async API
- ✅ No errors in benchmark runs

---

## Implementation Notes

1. **Code reuse**: Sync functions intentionally duplicate logic from async. Future refactoring could extract common helpers.

2. **Thread safety**: Uses same MVCC and sharded write locks as async. Safe for concurrent use.

3. **Error handling**: Maps async promise rejection to sync error codes (-1 for error, -2 for not found).

4. **Performance**: No work pool overhead makes sync faster for small operations, but blocking behavior limits throughput for bulk operations.

5. **Benchmarks**: Single-threaded only. Concurrent benchmarks require `benchmark_run_concurrent` function which doesn't exist in benchmark_base.