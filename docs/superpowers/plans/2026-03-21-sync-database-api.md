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

Create `tests/benchmark/benchmark_database_sync.cpp` with complete content (see plan document for full code - approximately 300 lines including setup, 4 benchmark functions, and main runner)

Key sections:
1. Includes and context struct
2. Helper functions: init, cleanup, generate_test_path, generate_test_value
3. Benchmark functions: put, get, mixed, delete
4. Main runner: run_sync_benchmarks() function
5. main() function

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