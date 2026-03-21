# Synchronous Database API Design Spec

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add synchronous versions of core database operations (put, get, delete) that execute in the caller thread without using the work pool.

**Architecture:** Direct execution functions that call internal implementation (`_database_put`, `_database_get`, `_database_delete`) synchronously, matching async error paths and ownership semantics.

**Tech Stack:** C (existing codebase), GoogleTest for benchmarking

---

## API Design

### `database_put_sync`

**Declaration:**
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

**Implementation:**
- Validate parameters (same as async)
- Create context on stack (no heap allocation)
- Call `_database_put` directly
- Return 0 on success, -1 on error

**Ownership:** Takes ownership of `path` and `value` (destroys them internally like async)

### `database_get_sync`

**Declaration:**
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

**Implementation:**
- Validate parameters
- Create context on stack
- Call `_database_get` directly
- Extract result from promise resolution
- Return appropriate error code

**Ownership:** Takes ownership of `path`, transfers ownership of `*result` to caller

### `database_delete_sync`

**Declaration:**
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

**Implementation:**
- Validate parameters
- Create context on stack
- Call `_database_delete` directly
- Return 0 on success, -1 on error

**Ownership:** Takes ownership of `path`

---

## Implementation Details

### Implementation Approach: Inline Logic

Since internal async functions (`_database_put`, `_database_get`, `_database_delete`) free their context and use promise callbacks, synchronous versions cannot reuse them directly. Instead, sync functions duplicate the core logic inline (matching async implementations).

**Why not reuse internal functions?**
1. Internal functions call `free(ctx)` - stack allocation causes double-free
2. Internal functions use promise callbacks - sync needs direct value return
3. Creating heap context for sync adds unnecessary overhead

**Code sharing strategy:**
- Sync functions duplicate core logic (put/delete ~30 lines each, get ~25 lines)
- Common validation extracted to helper if future refactoring needed
- Maintain behavioral parity through tests

### `database_put_sync` Implementation

```c
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

### `database_get_sync` Implementation

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

### `database_delete_sync` Implementation

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

### Error Codes

**Common error codes across all sync functions:**

- `0` - Success
- `-1` - Error (parameter validation failed, transaction failed to start, internal error)
- `-2` - Not found (only for `database_get_sync`)

**Error conditions mapped from async:**

| Async Behavior | Sync Error Code |
|----------------|------------------|
| NULL db/path/value parameters | -1 (after cleanup) |
| Transaction begin fails | -1 |
| Value not found in get | -2 |
| WAL write fails (logged, continues) | 0 (success, logged warning) |
| Success | 0 |

### Thread Safety

**Sync and async operations can interleave safely:**

- Both use same MVCC transaction manager
- Both use same sharded write locks
- Both use same lock-free reads
- No additional synchronization needed

**Concurrency characteristics:**

- Multiple sync operations from different threads: Safe (sharded write locks)
- Sync + async operations concurrently: Safe (same locking/MVCC)
- Sync operation in one thread, async in another: Safe

---

## Benchmarks

### File: `tests/benchmark/benchmark_database_sync.cpp`

Mirror existing `benchmark_database.cpp` structure with synchronous calls.

#### Single-Threaded Benchmarks

1. **Put**: Insert 10,000 key-value pairs
2. **Get**: Retrieve 10,000 keys
3. **Batch Put**: Insert 10,000 pairs with periodic snapshot
4. **Mixed**: 50% puts, 50% gets
5. **Delete**: Delete 10,000 keys

#### Concurrent Benchmarks

Test synchronous API under concurrent load:
- 1, 2, 4, 8, 16 threads
- Each thread performs independent operations
- Measure throughput scaling

#### Comparison Benchmarks

Side-by-side sync vs async:
- Same workloads
- Same thread counts
- Compare throughput and latency

### Benchmark Harness

Create sync-specific context:

```cpp
typedef struct {
    database_t* db;
    char test_dir[256];
    // No work pool or timing wheel needed for sync
} sync_benchmark_ctx_t;
```

### Benchmark Metrics

**Primary metrics:**

1. **Throughput**: Operations per second (ops/sec)
   - Higher is better
   - Measure steady-state throughput after warmup

2. **Latency**: Time per operation (microseconds)
   - Report P50, P95, P99 percentiles
   - Lower is better

3. **Memory overhead**: Bytes allocated per operation
   - Lower is better
   - Use valgrind/heap profiling

4. **Scalability**: Throughput vs thread count
   - Linear scaling is ideal
   - Measure 1, 2, 4, 8, 16 threads

**Benchmark methodology:**

1. **Warmup**: Run 10 iterations before measurement
2. **Measurement**: Run 10,000 operations for single-threaded
3. **Concurrent**: Run 10,000 operations per thread
4. **Repeat**: 3 runs, report median

**Success criteria:**

- Sync single-threaded throughput: ±10% of async single-threaded
- Sync concurrent throughput: Similar scaling ratio to async
- Sync latency P99: <10ms (same as async)
- Memory overhead: <5% difference from async
- No memory leaks (verified by valgrind/ASAN)

---

## Files

### Modified

**src/Database/database.h**
- Add declarations for `database_put_sync`, `database_get_sync`, `database_delete_sync`

**src/Database/database.c**
- Implement three synchronous functions

### Created

**tests/benchmark/benchmark_database_sync.cpp**
- Synchronous API benchmarks
- Comparison with async API

---

## Success Criteria

- ✅ All three synchronous functions implemented
- ✅ Error handling matches async versions
- ✅ Ownership semantics identical to async
- ✅ Unit tests pass (existing tests, sync should work)
- ✅ Benchmarks compile and run
- ✅ Performance comparison shows sync overhead vs async
- ✅ No memory leaks in synchronous operations

---

## Trade-offs

### Advantages

- **Simpler API**: For scripts, tools, single-threaded apps
- **Lower overhead**: No work pool enqueue/dequeue overhead
- **Easier debugging**: Synchronous call stack easier to trace
- **Flexibility**: Developers choose sync/async per use case

### Limitations

- **Blocking**: Caller thread waits for completion
- **No cancellation**: Can't abort in-progress operations
- **Thread pool bypass**: Different threading characteristics
- **No pipelining**: Each operation blocks until complete

---

## Implementation Notes

1. **Code duplication**: Sync functions duplicate ~30 lines of logic from internal async functions. This is intentional - sharing would require refactoring async internals.

2. **Thread safety**: Same MVCC guarantees as async - sharded write locks, lock-free reads. Safe to interleave sync and async operations.

3. **WAL integration**: Synchronous writes still go through WAL (thread-local, no lock contention).

4. **No work pool**: Bypasses async infrastructure entirely - simpler execution path, less overhead for small operations.

5. **Testing**: All existing database tests should pass with sync functions (same behavior, just blocking).