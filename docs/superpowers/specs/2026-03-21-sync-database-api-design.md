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
 * @param path     Path key to find (takes ownership)
 * @param result   Output: found value (caller must destroy) or NULL if not found
 * @return 0 on success, -1 on error, -2 on not found
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

### Direct Execution Pattern

Synchronous functions bypass work pool entirely:

```c
int database_put_sync(database_t* db, path_t* path, identifier_t* value) {
    // Same validation as async
    if (db == NULL || path == NULL || value == NULL) {
        if (path) path_destroy(path);
        if (value) identifier_destroy(value);
        return -1;
    }

    // Stack-allocated context (no heap)
    database_put_ctx_t ctx = {
        .db = db,
        .path = path,
        .value = value,
        .promise = NULL
    };

    // Direct execution (no work pool)
    _database_put(&ctx);

    return 0;
}
```

### Promise Handling for `database_get_sync`

The internal `_database_get` resolves a promise, but sync version needs to return the value:

```c
int database_get_sync(database_t* db, path_t* path, identifier_t** result) {
    if (db == NULL || path == NULL || result == NULL) {
        if (path) path_destroy(path);
        return -1;
    }

    // Create promise to capture result
    promise_t promise;
    identifier_t* value = NULL;
    // ... promise setup that stores value ...

    database_get_ctx_t ctx = {
        .db = db,
        .path = path,
        .promise = &promise
    };

    _database_get(&ctx);

    *result = value;
    return (value != NULL) ? 0 : -2;
}
```

### Error Path Consistency

Match async error paths exactly:
- Same validation order
- Same cleanup logic
- Same error codes where applicable

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

1. **Promise for get**: `_database_get` resolves promise with result. Sync version needs special handling to extract the value.

2. **Stack allocation**: Context structs allocated on stack (no heap overhead for sync).

3. **Thread safety**: Same MVCC guarantees as async - sharded write locks, lock-free reads.

4. **WAL integration**: Synchronous writes still go through WAL (thread-local, no lock contention).

5. **No work pool**: Bypasses async infrastructure entirely - simpler execution path.