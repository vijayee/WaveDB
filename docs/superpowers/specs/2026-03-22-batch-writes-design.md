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
    batch_op_t* ops;            // Dynamic array of operations
    size_t count;               // Current operation count
    size_t capacity;            // Array capacity
    size_t max_size;           // Maximum allowed operations
    size_t estimated_size;     // Running total of estimated serialized size
    uint8_t submitted;        // 0 = not submitted, 1 = submitted
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
- `estimated_size` tracks running total for size enforcement
- `submitted` prevents double-submission
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
 * Ownership semantics:
 *   - On success: ownership of path and value transfers to batch
 *   - On error: ownership remains with caller (caller must destroy)
 *
 * Validation performed:
 *   1. Check batch is not full (count < max_size)
 *   2. Check batch is not already submitted
 *   3. Check path is not NULL
 *   4. Check value is not NULL
 *   5. Validate path can be serialized
 *   6. Validate value can be serialized
 *   7. Update estimated_size
 *
 * @param batch Batch to modify
 * @param path Key path (ownership transfers on success)
 * @param value Value to store (ownership transfers on success)
 * @return 0 on success, -1 on error, -2 if batch is full
 */
int batch_add_put(batch_t* batch, path_t* path, identifier_t* value);

/**
 * Add a DELETE operation to batch.
 *
 * Ownership semantics:
 *   - On success: ownership of path transfers to batch
 *   - On error: ownership remains with caller (caller must destroy)
 *
 * Validation performed:
 *   1. Check batch is not full
 *   2. Check batch is not already submitted
 *   3. Check path is not NULL
 *   4. Validate path can be serialized
 *   5. Update estimated_size
 *
 * @param batch Batch to modify
 * @param path Key path to delete (ownership transfers on success)
 * @return 0 on success, -1 on error, -2 if batch is full
 */
int batch_add_delete(batch_t* batch, path_t* path);

/**
 * Estimate serialized size of batch.
 *
 * Useful for checking against WAL max_size before submission.
 *
 * @param batch Batch to estimate
 * @return Estimated size in bytes
 */
size_t batch_estimate_size(batch_t* batch);

/**
 * Submit batch synchronously.
 *
 * Blocks until entire batch is written to WAL and applied to database.
 * On any error: entire batch is rolled back, no partial writes.
 *
 * After submission:
 *   - Batch is marked as submitted (cannot add more operations)
 *   - Caller should destroy the batch after checking result
 *   - Batch is NOT reusable
 *
 * Batch must not be empty.
 * Batch size must not exceed WAL max_file_size.
 * Batch must not already be submitted.
 *
 * @param db Database to modify
 * @param batch Batch to submit (caller retains ownership until destroyed)
 * @return 0 on success, error code on failure
 *
 * Error codes:
 *   -1: General error
 *   -2: Batch is full
 *   -3: Batch is empty
 *   -4: Path/value validation failed
 *   -5: Batch too large for WAL
 *   -6: Batch already submitted
 */
int database_write_batch_sync(database_t* db, batch_t* batch);

/**
 * Submit batch asynchronously.
 *
 * Entire batch is processed by single worker thread (not split across threads).
 * On any error: entire batch is rolled back, no partial writes.
 *
 * After submission:
 *   - Batch is marked as submitted (cannot add more operations)
 *   - Worker references batch before starting work
 *   - Caller should destroy batch after promise resolves
 *   - Batch is NOT reusable
 *
 * Async reference counting flow:
 *   - Caller creates batch (refcount = 1)
 *   - Caller calls database_write_batch (refcount still 1)
 *   - Worker thread references batch (refcount = 2)
 *   - Worker executes batch, resolves promise
 *   - Worker dereferences batch (refcount = 1)
 *   - Caller destroys batch (refcount = 0, freed)
 *
 * Batch must not be empty.
 * Batch size must not exceed WAL max_file_size.
 * Batch must not already be submitted.
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
 * Safe to call even if batch was submitted.
 *
 * @param batch Batch to destroy
 */
void batch_destroy(batch_t* batch);
```

### Error Handling

**Batch building errors:**
- `batch_add_*` returns error immediately if validation fails
- On error: ownership remains with caller (caller must destroy path/value)
- Batch remains valid, caller can retry or destroy

**Batch submission errors:**
- Sync version returns error code
- Async version resolves promise with error code
- On any error during submission: entire batch rolled back, no partial writes
- Error recovery documented in Execution Flow section

**Return codes:**
- `0`: Success
- `-1`: General error
- `-2`: Batch is full
- `-3`: Batch is empty
- `-4`: Path/value validation failed
- `-5`: Batch too large for WAL
- `-6`: Batch already submitted

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
[Header: 33 bytes - SAME AS EXISTING FORMAT]
  - type (1 byte): WAL_BATCH
  - txn_id (24 bytes): transaction_id_t serialized (single ID for entire batch)
  - crc32 (4 bytes): CRC32 of data section
  - data_len (4 bytes): total data section length in bytes

[Data section: data_len bytes]
  - count (4 bytes): number of operations in batch
  - operations[]: serialized operations, one after another
    For each operation:
      - op_type (1 byte): WAL_PUT or WAL_DELETE
      - path_len (4 bytes): path byte length
      - path_data (path_len bytes): serialized path
      - value_len (4 bytes): value byte length (0 for DELETE)
      - value_data (value_len bytes): serialized value (absent for DELETE)
```

**Key points:**
- Header format is IDENTICAL to existing WAL_PUT/WAL_DELETE format
- `data_len` field allows WAL reader to allocate buffer correctly
- `count` is the first field in data section, not in header
- Single CRC32 for entire data section (atomic integrity)
- Batch must fit in single WAL file (enforced by size estimation)

**Serialization flow:**
1. Generate single `transaction_id_t` for entire batch
2. Serialize count and all operations into `buffer_t*`
3. Compute CRC32 of serialized data
4. Write header + data to WAL as single entry (using existing `wal_write` pattern)
5. WAL rotation if needed (same as single-op writes)

**Size enforcement:**
- `batch_add_*` updates `estimated_size` on each operation
- If `estimated_size` approaches `wal_max_size`, return error early
- Before submission: check `estimated_size` against `wal_max_size`, error if too large
- Caller can check size with `batch_estimate_size()` before submitting

---

## Execution Flow

### Synchronous Batch (database_write_batch_sync)

```
1. Validate batch (not empty, not full, not already submitted)
2. Check estimated_size against WAL max_size → error if too large
3. Generate single transaction_id_t for entire batch
4. Mark batch as submitted (prevent double-submission)
5. Serialize all operations into buffer_t
6. Compute CRC32 of buffer
7. Acquire ALL write locks (all 64 shards) IN ORDER
   - Acquire locks[0], locks[1], ..., locks[63] to prevent deadlock
8. Write single WAL_BATCH entry (may rotate WAL)
9. If WAL write fails:
   - Release all locks
   - Return error (no data written, nothing to rollback)
10. Apply all operations to database trie (in-memory)
11. If any operation fails:
    - THIS IS A CRITICAL ERROR - should not happen
    - Log error and crash the process
    - Recovery will replay the batch from WAL
    - Ensures consistency: either fully applied or not at all
12. Release all write locks (in reverse order: locks[63] down to locks[0])
13. Return success
```

### Asynchronous Batch (database_write_batch)

```
1. Validate batch (not empty, not full, not already submitted)
2. Reference batch: refcounter_reference(&batch->refcounter)
3. Create work_t with batch context
4. Enqueue to thread pool
5. Worker thread picks up work
6. Worker executes steps 2-11 from sync version
7. Resolve promise with result code
8. Dereference batch: refcounter_dereference(&batch->refcounter)
```

**Critical error handling (step 11):**
- **Before WAL write fails:** Return error, no recovery needed
- **After WAL write succeeds, before trie apply:** Process MUST crash
  - Batch is in WAL (durable) but not in trie (in-memory)
  - Cannot safely continue without risking inconsistency
  - Crash forces recovery to replay from WAL
  - Recovery will apply the entire batch atomically
- **No intermediate states:** Either batch fully applied or not at all

**Key points:**
- Steps 5-10 are WAL + in-memory updates (fast, single-threaded)
- Single transaction ID ties everything together
- Lock acquisition order prevents deadlocks
- Process crash on trie-apply failure ensures consistency via recovery

---

## Thread Safety & Concurrency

### Batch Builder Thread Safety

```c
// batch_t is thread-safe for concurrent batch_add_* calls
int batch_add_put(batch_t* batch, path_t* path, identifier_t* value) {
    platform_lock(&batch->lock);

    // Check if already submitted
    if (batch->submitted) {
        platform_unlock(&batch->lock);
        return -6;  // Batch already submitted
    }

    // Check capacity
    if (batch->count >= batch->max_size) {
        platform_unlock(&batch->lock);
        return -2;  // Batch is full
    }

    // Validate and add operation
    // ... update estimated_size ...
    // ... add to ops array ...

    platform_unlock(&batch->lock);
    return 0;
}
```

### Database Write Lock Integration

**Current:** Database has 64 sharded write locks for concurrent single-op writes.

**Batch writes:** Acquire ALL write locks (all 64 shards) IN ORDER to prevent deadlock.

**Lock acquisition order:**
```c
// MUST acquire locks in consistent order
for (size_t i = 0; i < WRITE_LOCK_SHARDS; i++) {
    platform_lock(&db->write_locks[i]);
}

// ... perform batch operations ...

// Release in REVERSE order
for (size_t i = WRITE_LOCK_SHARDS; i > 0; i--) {
    platform_unlock(&db->write_locks[i - 1]);
}
```

**Rationale:**
- Consistent lock ordering prevents deadlock with concurrent operations
- All batches use same order (ascending)
- All single operations use same order (acquire one lock, but in ascending order)
- Ensures consistency across entire batch
- Blocks all concurrent writes during batch execution

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
   a. Read header (type, txn_id, crc32, data_len)
   b. Read data section (data_len bytes)
   c. Validate CRC32 of data section
   d. If CRC32 fails:
      - Log error with transaction ID
      - Skip this batch (don't apply any operations)
      - Continue to next WAL entry
   e. Deserialize count from data section
   f. For each operation in batch:
      - Deserialize op_type, path, value
      - Apply to trie (same as single-op replay)
   g. If any operation fails:
      - Log error
      - Skip this batch (don't mark as applied)
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
- Data section format allows correct buffer allocation

---

## Implementation Order

### Phase 1: Batch Builder
- Add `batch_op_t` and `batch_t` structs to new `src/Database/batch.h/c`
- Implement `batch_create`, `batch_add_put`, `batch_add_delete`, `batch_destroy`
- Implement `batch_estimate_size` helper
- Add `max_batch_size` to database configuration
- Add `submitted` field tracking to prevent double-submission
- Add `estimated_size` field tracking for size enforcement
- Unit tests for batch builder (including size estimation and submission tracking)

### Phase 2: WAL Batch Format
- Add `WAL_BATCH` type to `wal_type_e` in `src/Database/wal.h`
- Implement batch serialization in `wal.c`: helper function to serialize batch
- Implement batch deserialization in `wal.c`: helper function to deserialize batch
- Update `wal_write` to handle batch type (use existing header format)
- Update `wal_read` to handle batch type (read data_len, then deserialize)
- Unit tests for WAL batch serialization (including size limits)

### Phase 3: Database API
- Implement `database_write_batch_sync` in `src/Database/database.c`
- Add write lock acquisition (all shards in order to prevent deadlock)
- Integrate with WAL batch write
- Apply operations to trie
- Handle critical error (crash on trie-apply failure)
- Unit tests for sync batch (including size validation and lock ordering)

### Phase 4: Async Support
- Implement `database_write_batch` async version in `src/Database/database.c`
- Create work context for batch execution
- Integrate with thread pool
- Implement reference counting flow (reference before enqueue, dereference after resolve)
- Resolve promise with result
- Unit tests for async batch (including reference counting)

### Phase 5: Recovery
- Update recovery in `src/Database/wal_manager.c` to handle `WAL_BATCH`
- Deserialize and replay batch operations
- MVCC integration (check transaction ID)
- Handle CRC32 failures (skip batch)
- Integration tests for crash recovery (including interrupted batches)

### Phase 6: Documentation & Testing
- Update STYLEGUIDE.md with batch API patterns
- Add examples to project documentation
- Performance benchmarks
- Edge case testing (size limits, double-submission, concurrent batches)

---

## Testing Strategy

### Unit Tests

**Batch builder tests:**
- Create/destroy batch
- Add operations until full
- Error on oversized batch (estimated_size)
- Error on empty batch submission
- Error on double-submission
- Size estimation accuracy
- Thread-safe concurrent additions

**WAL serialization tests:**
- Serialize empty batch
- Serialize batch with various operations
- Deserialize and verify all operations
- CRC32 validation (corrupted data)
- Size limit enforcement (reject if data_len > wal_max_size)

**Database batch tests:**
- Submit batch, verify all operations applied
- Batch with one failure: process crash (recovery replays)
- Batch exceeds WAL max_size: error returned before WAL write
- Concurrent batches (different databases)
- Concurrent batches (same database, serialized by locks)
- Double-submission prevention
- Lock acquisition order (no deadlocks)

### Integration Tests

**Recovery tests:**
- Write batch, simulate crash, recover
- Batch with partial write (corrupted): recovery skips
- Duplicate batch (same txn_id): recovery skips
- Multiple batches: all recovered in order
- Interrupted batch (process crash during trie apply): recovery replays

**Performance tests:**
- Batch of 1000 puts vs 1000 individual puts
- Measure WAL write throughput
- Measure database lock contention
- Compare sync vs async batch performance

### Edge Cases

- Batch with 0 operations → error (-3)
- Batch with max_batch_size operations → success
- Batch with max_batch_size + 1 operations → error (-2)
- Mixed PUT and DELETE in same batch
- DELETE of non-existent key in batch
- PUT then DELETE of same key in same batch
- Batch size exceeds WAL max_size → error (-5)
- Double-submission → error (-6)

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
| Write locking | Acquire all shards in order | Prevents deadlock, ensures consistency |
| Recovery CRC | Single CRC for entire batch | Atomic batch = atomic integrity check |
| Oversized batch handling | Reject with error before WAL write | Simple, caller handles splitting |
| Error recovery | Crash on trie-apply failure | Forces recovery to replay, ensures consistency |
| Batch reusability | Not reusable after submission | Clear ownership, simpler implementation |
| Size estimation | Track running total | Enable early rejection of oversized batches |

---

## Future Enhancements

Not in initial implementation, but considered for future:

1. **Per-key write locking** - Only acquire shards for keys in batch
2. **Batch size estimation API** - Expose `batch_estimate_size()` for caller optimization
3. **Batch reads** - Read operations in batch (visibility semantics TBD)
4. **Batch cancellation** - Cancel async batch before execution
5. **Batch progress callbacks** - Progress reporting for large batches

---

## Questions for Implementation

**All questions have been resolved in this spec:**

1. ~~Should `batch_add_*` validate paths and values immediately, or defer to submission?~~ **RESOLVED: Validate on add (fail-fast, easier debugging)**

2. ~~Should batches be reusable after submission?~~ **RESOLVED: No, caller should destroy after submit (clear ownership)**

3. ~~Default `max_batch_size` value?~~ **RESOLVED: 10,000 operations (reasonable default, caller can increase)**

4. ~~Should we expose WAL max_size to help callers size batches?~~ **RESOLVED: Yes, add `size_t database_get_max_wal_size(database_t* db)`**