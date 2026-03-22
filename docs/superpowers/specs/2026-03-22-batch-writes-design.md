# Batch Write Operations Design

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add atomic batch write operations to WaveDB database API with single transaction ID per batch for efficient bulk writes and atomic semantics.

**Architecture:** Batch operations are collected in memory, serialized as a single WAL_BATCH entry with one transaction ID, and applied atomically to the database. Minimal changes to existing WAL infrastructure by treating batches as a new entry type.

**Tech Stack:** C (existing codebase), existing WAL and MVCC infrastructure, thread pool for async execution.

---

## Overview

Batch writes allow callers to submit multiple `PUT` and `DELETE` operations as a single atomic unit:

- **Single transaction ID** for entire batch (not per-operation)
- **Write-only batches** (no reads in batch)
- **Atomic all-or-nothing** (entire batch succeeds or fails together)
- **Single WAL entry** per batch (efficient serialization)
- **Both sync and async** API versions

This design adds a thin layer on top of existing WAL infrastructure, reusing proven patterns while enabling efficient bulk writes.

---

## Data Structures

### Batch Operation

```c
typedef struct {
    wal_type_e type;        // WAL_PUT or WAL_DELETE
    path_t* path;           // Key (ownership transfers to batch)
    identifier_t* value;    // Value for PUT (NULL for DELETE)
} batch_op_t;
```

### Batch Handle

```c
typedef struct {
    refcounter_t refcounter;
    PLATFORMLOCKTYPE(lock);
    batch_op_t* ops;         // Dynamic array of operations
    size_t count;            // Current operation count
    size_t capacity;         // Array capacity
    size_t max_size;         // Maximum allowed operations
} batch_t;
```

### Database Configuration

Add to database creation:

```c
typedef struct {
    // ... existing wal_config_t fields ...
    size_t max_batch_size;   // Maximum operations per batch (0 = default 10,000)
} wal_config_t;
```

**Key points:**
- `batch_t` is reference-counted (caller can retain batch across multiple submissions)
- Thread-safe for concurrent `batch_add_*` calls
- Path/value ownership transfers to batch on add (caller shouldn't destroy)
- Configurable size limit prevents runaway memory usage

---

## API Design

### Batch Lifecycle

```c
/**
 * Create a batch for collecting write operations.
 *
 * @param reserve_count Pre-allocate space for this many operations (0 = default)
 * @return New batch or NULL on failure
 */
batch_t* batch_create(size_t reserve_count);

/**
 * Add a PUT operation to batch.
 *
 * Ownership of path and value transfers to batch.
 * Caller should not destroy path or value after this call.
 *
 * @param batch Batch to modify
 * @param path Key path (ownership transfers)
 * @param value Value to store (ownership transfers)
 * @return 0 on success, -1 on error, -2 if batch is full
 */
int batch_add_put(batch_t* batch, path_t* path, identifier_t* value);

/**
 * Add a DELETE operation to batch.
 *
 * Ownership of path transfers to batch.
 * Caller should not destroy path after this call.
 *
 * @param batch Batch to modify
 * @param path Key path to delete (ownership transfers)
 * @return 0 on success, -1 on error, -2 if batch is full
 */
int batch_add_delete(batch_t* batch, path_t* path);

/**
 * Submit batch synchronously.
 *
 * Blocks until entire batch is written to WAL and applied to database.
 * On any error: entire batch is rolled back, no partial writes.
 *
 * Batch must not be empty.
 * Batch size must not exceed WAL max_file_size.
 *
 * @param db Database to modify
 * @param batch Batch to submit (caller retains ownership)
 * @return 0 on success, error code on failure
 *
 * Error codes:
 *   -1: General error
 *   -2: Batch is full
 *   -3: Batch is empty
 *   -4: Path/value validation failed
 *   -5: Batch too large for WAL
 */
int database_write_batch_sync(database_t* db, batch_t* batch);

/**
 * Submit batch asynchronously.
 *
 * Entire batch is processed by single worker thread (not split across threads).
 * On any error: entire batch is rolled back, no partial writes.
 *
 * Batch must not be empty.
 * Batch size must not exceed WAL max_file_size.
 *
 * @param db Database to modify
 * @param batch Batch to submit (caller retains ownership until promise resolved)
 * @param promise Promise to resolve with result code (same error codes as sync version)
 */
void database_write_batch(database_t* db, batch_t* batch, promise_t* promise);

/**
 * Destroy a batch.
 *
 * Frees all operations and their paths/values.
 *
 * @param batch Batch to destroy
 */
void batch_destroy(batch_t* batch);
```

### Error Handling

**Batch building errors:**
- `batch_add_*` returns error immediately if batch is full or invalid
- Caller handles error before submission (batch remains valid)

**Batch execution errors:**
- Sync version returns error code
- Async version resolves promise with error code
- On any error: entire batch rolled back, no partial writes
- WAL may contain batch, but recovery will skip if apply failed

**Return codes:**
- `0`: Success
- `-1`: General error
- `-2`: Batch is full
- `-3`: Batch is empty
- `-4`: Path/value validation failed
- `-5`: Batch too large for WAL

---

## WAL Format

### New Entry Type

```c
typedef enum {
    WAL_PUT = 'p',      // Insert/update operation
    WAL_DELETE = 'd',   // Delete operation
    WAL_BATCH = 'b'     // Batch of operations
} wal_type_e;
```

### Batch Entry Format

```
[Header: 33 bytes]
  - type (1 byte): WAL_BATCH
  - txn_id (24 bytes): transaction_id_t serialized (single ID for entire batch)
  - crc32 (4 bytes): CRC32 of data section
  - count (4 bytes): number of operations

[Data: variable]
  For each operation:
    - op_type (1 byte): WAL_PUT or WAL_DELETE
    - path_len (4 bytes): path byte length
    - path_data (path_len bytes): serialized path
    - value_len (4 bytes): value byte length (0 for DELETE)
    - value_data (value_len bytes): serialized value (absent for DELETE)
```

**Note:** Single CRC32 for entire batch data section (not per-operation). Since batch is atomic, per-operation CRCs would be redundant integrity checking.

**Serialization flow:**
1. Generate single `transaction_id_t` for entire batch
2. Serialize all operations into `buffer_t*`
3. Compute CRC32 of serialized data
4. Write header + data to WAL as single entry
5. WAL rotation if needed (same as single-op writes)

**Size constraint:**
- Batch must fit in single WAL file
- If batch exceeds `wal_max_size`, error code `-5` returned
- Caller must split into smaller batches or increase WAL max_size

---

## Execution Flow

### Synchronous Batch (database_write_batch_sync)

```
1. Validate batch (not empty, not full, not already submitted)
2. Estimate serialized size
3. Check against WAL max_size → error if too large
4. Generate single transaction_id_t for entire batch
5. Acquire ALL write locks (all 64 shards)
6. Serialize all operations into buffer_t
7. Compute CRC32 of buffer
8. Write single WAL_BATCH entry (may rotate WAL)
9. If WAL write fails: release locks, return error (no data written)
10. Apply all operations to database trie (in-memory)
11. If any operation fails:
    - Don't commit to trie
    - Release locks
    - Return error
    - Note: WAL still has the batch, will be replayed on recovery
12. Release all write locks
13. Return success
```

### Asynchronous Batch (database_write_batch)

```
1. Validate batch
2. Reference batch (prevent destruction until promise resolved)
3. Create work_t with batch context
4. Enqueue to thread pool
5. Worker thread picks up work
6. Worker executes steps 2-11 from sync version
7. Resolve promise with result code
8. Dereference batch
```

**Key points:**
- Steps 5-10 are WAL + in-memory updates (fast, single-threaded)
- Single transaction ID ties everything together
- If trie apply fails, WAL still has batch (recovery will replay or skip via MVCC)
- Recovery handles idempotency (MVCC versioning prevents double-applies)

---

## Thread Safety & Concurrency

### Batch Builder Thread Safety

```c
// batch_t is thread-safe for concurrent batch_add_* calls
int batch_add_put(batch_t* batch, path_t* path, identifier_t* value) {
    platform_lock(&batch->lock);
    // ... check capacity, grow array if needed ...
    // ... add operation ...
    platform_unlock(&batch->lock);
    return 0;
}
```

### Database Write Lock Integration

**Current:** Database has 64 sharded write locks for concurrent single-op writes.

**Batch writes:** Acquire ALL write locks (all 64 shards) before writing batch.

**Rationale:**
- Simpler implementation
- Ensures consistent database view across entire batch
- Blocks all concurrent writes during batch execution
- Matches atomic semantics (all-or-nothing)

**Performance impact:**
- Batches block all writers for duration of batch
- Single-op writes may wait longer if batch in progress
- Acceptable for batch workloads (typically less frequent than single writes)

**Future optimization:** Per-key locking (only acquire shards for keys in batch).

---

## Recovery Process

### Batch Recovery Flow

1. **WAL Replay reads all entries:**
   - WAL_PUT, WAL_DELETE (existing)
   - WAL_BATCH (new)

2. **For WAL_BATCH entries:**
   ```
   a. Read header (type, txn_id, crc32, count)
   b. Validate CRC32 of data section
   c. For each operation in batch:
      - Deserialize path and value
      - Apply to trie (same as single-op replay)
   d. If CRC32 fails:
      - Log error with transaction ID
      - Skip this batch (don't apply any operations)
      - Continue to next WAL entry
   e. If any operation fails:
      - Log error
      - Skip this batch
      - Continue to next WAL entry
   ```

3. **Idempotency (existing MVCC handles this):**
   - Batch has single transaction ID
   - MVCC checks if transaction already applied
   - If yes: skip entire batch
   - If no: apply all operations, mark transaction as applied

4. **Failure handling:**
   - Corrupted batch (CRC32 fails): skip, don't crash recovery
   - Duplicate batch (same txn_id): skip, MVCC already applied
   - Partial batch (WAL write interrupted): CRC32 fails, skip

**Key points:**
- Recovery is all-or-nothing per batch (atomic replay)
- Existing MVCC handles duplicate prevention
- CRC32 validation catches incomplete writes
- Single transaction ID simplifies deduplication

---

## Implementation Order

### Phase 1: Batch Builder
- Add `batch_op_t` and `batch_t` structs to new `src/Database/batch.h/c`
- Implement `batch_create`, `batch_add_put`, `batch_add_delete`, `batch_destroy`
- Add max_batch_size to database configuration
- Unit tests for batch builder

### Phase 2: WAL Batch Format
- Add `WAL_BATCH` type to `wal_type_e` in `src/Database/wal.h`
- Implement batch serialization in `wal.c`: helper function to serialize batch
- Implement batch deserialization in `wal.c`: helper function to deserialize batch
- Update `wal_write` to handle batch type
- Update `wal_read` to handle batch type
- Unit tests for WAL batch serialization

### Phase 3: Database API
- Implement `database_write_batch_sync` in `src/Database/database.c`
- Add write lock acquisition (all shards)
- Integrate with WAL batch write
- Apply operations to trie
- Unit tests for sync batch

### Phase 4: Async Support
- Implement `database_write_batch` async version in `src/Database/database.c`
- Create work context for batch execution
- Integrate with thread pool
- Resolve promise with result
- Unit tests for async batch

### Phase 5: Recovery
- Update recovery in `src/Database/wal_manager.c` to handle `WAL_BATCH`
- Deserialize and replay batch operations
- MVCC integration (check transaction ID)
- Integration tests for crash recovery

### Phase 6: Documentation & Testing
- Update STYLEGUIDE.md with batch API patterns
- Add examples to project documentation
- Performance benchmarks
- Edge case testing

---

## Testing Strategy

### Unit Tests

**Batch builder tests:**
- Create/destroy batch
- Add operations until full
- Error on oversized batch
- Error on empty batch submission
- Thread-safe concurrent additions

**WAL serialization tests:**
- Serialize empty batch
- Serialize batch with various operations
- Deserialize and verify all operations
- CRC32 validation (corrupted data)

**Database batch tests:**
- Submit batch, verify all operations applied
- Batch with one failure: entire batch rolled back
- Batch exceeds WAL max_size: error returned
- Concurrent batches (different databases)
- Concurrent batches (same database, serialized by locks)

### Integration Tests

**Recovery tests:**
- Write batch, simulate crash, recover
- Batch with partial write (corrupted): recovery skips
- Duplicate batch (same txn_id): recovery skips
- Multiple batches: all recovered in order

**Performance tests:**
- Batch of 1000 puts vs 1000 individual puts
- Measure WAL write throughput
- Measure database lock contention

### Edge Cases

- Batch with 0 operations → error (-3)
- Batch with max_batch_size operations → success
- Batch with max_batch_size + 1 operations → error (-2)
- Mixed PUT and DELETE in same batch
- DELETE of non-existent key in batch
- PUT then DELETE of same key in same batch

---

## Design Decisions Summary

| Decision | Choice | Rationale |
|----------|--------|-----------|
| Transaction ID scope | Single ID per batch | Atomic semantics, simpler recovery |
| Batch operations | Write-only (PUT/DELETE) | Matches typical batch use case |
| Atomicity | All-or-nothing | Caller can't have partial results |
| WAL representation | Single entry per batch | Simpler implementation, atomic write |
| API style | Builder pattern | Flexible for building batches incrementally |
| Async support | Both sync and async | Matches existing API pattern |
| Async processing | Single worker thread | Atomic execution, no operation splitting |
| Batch size limits | Configurable with error on overflow | Prevents runaway memory, clear errors |
| Write locking | Acquire all shards | Simpler, ensures consistency |
| Recovery CRC | Single CRC for entire batch | Atomic batch = atomic integrity check |
| Oversized batch handling | Reject with error | Simple, caller handles splitting |

---

## Future Enhancements

Not in initial implementation, but considered for future:

1. **Per-key write locking** - Only acquire shards for keys in batch
2. **Batch size estimation API** - `size_t batch_size_estimate(batch_t* batch)`
3. **Batch reads** - Read operations in batch (visibility semantics TBD)
4. **Batch cancellation** - Cancel async batch before execution
5. **Batch progress callbacks** - Progress reporting for large batches

---

## Questions for Implementation

1. Should `batch_add_*` validate paths and values immediately, or defer to submission?
   - **Recommendation:** Validate on add (fail-fast, easier debugging)

2. Should batches be reusable after submission?
   - **Recommendation:** No, caller should destroy after submit (clear ownership)

3. Default `max_batch_size` value?
   - **Recommendation:** 10,000 operations (reasonable default, caller can increase)

4. Should we expose WAL max_size to help callers size batches?
   - **Recommendation:** Yes, add `size_t database_get_max_wal_size(database_t* db)`