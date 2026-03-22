# Batch Write Operations Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement atomic batch write operations with single transaction ID per batch, enabling efficient bulk writes with all-or-nothing semantics.

**Architecture:** Add batch.h/batch.c for batch builder, modify WAL to support WAL_BATCH entry type, extend database API with synchronous and asynchronous batch operations, update recovery to handle batch entries.

**Tech Stack:** C (existing codebase), GoogleTest for unit tests, existing WAL and MVCC infrastructure, CBOR serialization.

---

## File Structure

**New files:**
- `src/Database/batch.h` - Batch API header
- `src/Database/batch.c` - Batch implementation
- `tests/test_batch.cpp` - Batch unit tests

**Modified files:**
- `src/Database/wal.h` - Add WAL_BATCH enum
- `src/Database/wal.c` - Add batch serialization functions
- `src/Database/database.h` - Add batch API declarations
- `src/Database/database.c` - Implement batch submission
- `src/Database/wal_manager.c` - Add batch recovery
- `tests/test_database.cpp` - Add batch integration tests
- `CMakeLists.txt` - Add batch.c to build

**Verified:** Database has `write_locks[WRITE_LOCK_SHARDS]` (64 sharded locks) at `database.h:43`

---

## Phase 1: Batch Builder Foundation

### Task 1.1: Create batch.h header with data structures

**Files:**
- Create: `src/Database/batch.h`

- [ ] **Step 1: Create batch.h header**

Create `src/Database/batch.h` with complete data structures and API declarations (see spec lines 29-62 for exact struct definitions).

- [ ] **Step 2: Verify header compiles**

Run: `cd build-test && make wavedb`

Expected: Compiles successfully

- [ ] **Step 3: Commit header**

```bash
git add src/Database/batch.h
git commit -m "feat: add batch.h header with data structures and API"
```

---

### Task 1.2: Implement batch_create and batch_destroy

**Files:**
- Create: `src/Database/batch.c`
- Modify: `CMakeLists.txt`
- Create: `tests/test_batch.cpp`

- [ ] **Step 1: Write failing tests for batch_create/batch_destroy**

Create `tests/test_batch.cpp` with tests for:
- Create with reserve_count
- Create with 0 (default capacity)
- Destroy with operations
- Destroy NULL batch

- [ ] **Step 2: Run tests to verify they fail**

Run: `cd build-test && ./test_batch --gtest_filter=BatchTest.Create*`

Expected: FAIL (undefined references)

- [ ] **Step 3: Implement batch_create and batch_destroy**

Implement in `src/Database/batch.c`:
- Allocate batch_t
- Initialize lock and refcounter
- Allocate ops array with capacity
- Set default max_size = 10000
- Destroy frees all operations and array

- [ ] **Step 4: Add batch.c to CMakeLists.txt**

Add `src/Database/batch.c` to wavedb library sources.

- [ ] **Step 5: Run tests to verify they pass**

Run: `cd build-test && make && ./test_batch --gtest_filter=BatchTest.Create*`

Expected: PASS

- [ ] **Step 6: Commit implementation**

```bash
git add src/Database/batch.c tests/test_batch.cpp CMakeLists.txt
git commit -m "feat: implement batch_create and batch_destroy"
```

---

### Task 1.3: Implement batch_add_put and batch_add_delete with validation

**Files:**
- Modify: `src/Database/batch.c`
- Modify: `tests/test_batch.cpp`

- [ ] **Step 1: Write tests for batch_add_put**

Add tests for:
- Adding PUT operation (success case)
- Adding DELETE operation (success case)
- Adding with NULL batch (error -1)
- Adding with NULL path (error -1)
- Adding with NULL value for PUT (error -1)
- Adding when batch is full (error -2)
- Adding when batch is submitted (error -6)
- Thread-safe concurrent additions

- [ ] **Step 2: Run tests to verify they fail**

Run: `cd build-test && ./test_batch --gtest_filter=BatchTest.AddPut*`

Expected: FAIL (undefined references)

- [ ] **Step 3: Implement batch_add_put and batch_add_delete with full validation**

Implement in `src/Database/batch.c`:
1. Lock batch
2. Check submitted flag (return -6 if already submitted)
3. Check capacity (return -2 if full)
4. Check NULL inputs (return -1)
5. Grow array if needed
6. Add operation
7. Unlock
8. Return 0 on success

**Important:** On error, ownership remains with caller (caller must destroy path/value)

- [ ] **Step 4: Run tests to verify they pass**

Run: `cd build-test && make && ./test_batch --gtest_filter=BatchTest.AddPut*`

Expected: PASS

- [ ] **Step 5: Commit implementation**

```bash
git add src/Database/batch.c tests/test_batch.cpp
git commit -m "feat: implement batch_add_put/delete with validation"
```

---

### Task 1.4: Implement batch size estimation using CBOR

**Files:**
- Modify: `src/Database/batch.c`
- Modify: `tests/test_batch.cpp`
- Reference: `src/HBTrie/path.h`, `src/HBTrie/identifier.h`

- [ ] **Step 1: Check CBOR serialization functions exist**

Verify: `path_to_cbor`, `cbor_to_path`, `identifier_to_cbor`, `cbor_to_identifier` exist in `src/HBTrie/`

Expected: Functions exist and are usable

- [ ] **Step 2: Write test for batch_estimate_size**

Add test that:
- Creates batch with operations
- Estimates size
- Verifies size > 0 and increases with more operations

- [ ] **Step 3: Implement batch_estimate_size**

Implement in `src/Database/batch.c`:
```c
size_t batch_estimate_size(batch_t* batch) {
    if (batch == NULL) return 0;

    // Header: 33 bytes (same as single WAL entry)
    size_t size = 33;

    // Count field: 4 bytes
    size += 4;

    // Each operation
    for (size_t i = 0; i < batch->count; i++) {
        // op_type: 1 byte
        size += 1;

        // Serialize path to get size
        buffer_t* path_buf = path_to_cbor(batch->ops[i].path);
        size += 4 + buffer_size(path_buf); // path_len + data
        buffer_destroy(path_buf);

        // If PUT, serialize value
        if (batch->ops[i].type == WAL_PUT && batch->ops[i].value != NULL) {
            buffer_t* value_buf = identifier_to_cbor(batch->ops[i].value);
            size += 4 + buffer_size(value_buf); // value_len + data
            buffer_destroy(value_buf);
        }
    }

    return size;
}
```

- [ ] **Step 4: Update batch_add_* to update estimated_size**

Add to `batch_add_put` and `batch_add_delete`:
```c
// Update estimated_size
size_t path_size = 0;
buffer_t* path_buf = path_to_cbor(path);
if (path_buf) {
    path_size = buffer_size(path_buf);
    buffer_destroy(path_buf);
}

size_t value_size = 0;
if (value != NULL) {
    buffer_t* value_buf = identifier_to_cbor(value);
    if (value_buf) {
        value_size = buffer_size(value_buf);
        buffer_destroy(value_buf);
    }
}

batch->estimated_size += 1 + 4 + path_size + 4 + value_size;
```

- [ ] **Step 5: Run tests to verify they pass**

Run: `cd build-test && make && ./test_batch --gtest_filter=BatchTest.EstimateSize`

Expected: PASS

- [ ] **Step 6: Commit implementation**

```bash
git add src/Database/batch.c tests/test_batch.cpp
git commit -m "feat: implement accurate batch size estimation using CBOR"
```

---

## Phase 2: WAL Batch Format

### Task 2.1: Add WAL_BATCH type

**Files:**
- Modify: `src/Database/wal.h`

- [ ] **Step 1: Add WAL_BATCH to enum**

Edit `src/Database/wal.h`:
```c
typedef enum {
    WAL_PUT = 'p',
    WAL_DELETE = 'd',
    WAL_BATCH = 'b'
} wal_type_e;
```

- [ ] **Step 2: Commit type addition**

```bash
git add src/Database/wal.h
git commit -m "feat: add WAL_BATCH type to wal_type_e enum"
```

---

### Task 2.2: Implement batch serialization

**Files:**
- Modify: `src/Database/wal.c`
- Modify: `tests/test_batch.cpp`

- [ ] **Step 1: Write test for batch serialization**

Add to `tests/test_batch.cpp`:
- Create batch with operations
- Serialize batch
- Deserialize batch
- Verify all operations match

- [ ] **Step 2: Implement serialize_batch helper**

Add to `src/Database/wal.c`:
```c
static buffer_t* serialize_batch(batch_t* batch) {
    // Calculate size
    size_t size = batch_estimate_size(batch);
    buffer_t* buf = buffer_create(size);

    // Write count
    uint8_t count_bytes[4];
    write_uint32_be(count_bytes, (uint32_t)batch->count);
    buffer_append(buf, count_bytes, 4);

    // Write each operation
    for (size_t i = 0; i < batch->count; i++) {
        // op_type
        buffer_append_byte(buf, (uint8_t)batch->ops[i].type);

        // path
        buffer_t* path_buf = path_to_cbor(batch->ops[i].path);
        uint8_t path_len_bytes[4];
        write_uint32_be(path_len_bytes, (uint32_t)buffer_size(path_buf));
        buffer_append(buf, path_len_bytes, 4);
        buffer_append(buf, buffer_data(path_buf), buffer_size(path_buf));
        buffer_destroy(path_buf);

        // value
        if (batch->ops[i].type == WAL_PUT) {
            buffer_t* value_buf = identifier_to_cbor(batch->ops[i].value);
            uint8_t value_len_bytes[4];
            write_uint32_be(value_len_bytes, (uint32_t)buffer_size(value_buf));
            buffer_append(buf, value_len_bytes, 4);
            buffer_append(buf, buffer_data(value_buf), buffer_size(value_buf));
            buffer_destroy(value_buf);
        }
    }

    return buf;
}
```

- [ ] **Step 3: Implement deserialize_batch helper**

Add to `src/Database/wal.c`:
```c
static int deserialize_batch(buffer_t* data, batch_op_t** ops, size_t* count) {
    // Read count
    uint32_t op_count = read_uint32_be(buffer_data(data));

    // Allocate operations
    *ops = malloc(op_count * sizeof(batch_op_t));
    if (*ops == NULL) return -1;

    size_t offset = 4; // Skip count

    for (uint32_t i = 0; i < op_count; i++) {
        // Read op_type
        uint8_t op_type = buffer_data(data)[offset++];

        // Read path
        uint32_t path_len = read_uint32_be(buffer_data(data) + offset);
        offset += 4;

        buffer_t* path_buf = buffer_create_from_pointer_copy(
            buffer_data(data) + offset, path_len);
        offset += path_len;

        (*ops)[i].path = cbor_to_path(path_buf);
        buffer_destroy(path_buf);

        // Read value if PUT
        if (op_type == WAL_PUT) {
            uint32_t value_len = read_uint32_be(buffer_data(data) + offset);
            offset += 4;

            buffer_t* value_buf = buffer_create_from_pointer_copy(
                buffer_data(data) + offset, value_len);
            offset += value_len;

            (*ops)[i].value = cbor_to_identifier(value_buf);
            buffer_destroy(value_buf);
        } else {
            (*ops)[i].value = NULL;
        }

        (*ops)[i].type = (wal_type_e)op_type;
    }

    *count = op_count;
    return 0;
}
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `cd build-test && make && ./test_batch --gtest_filter=BatchTest.SerializeDeserialize*`

Expected: PASS

- [ ] **Step 5: Commit implementation**

```bash
git add src/Database/wal.c tests/test_batch.cpp
git commit -m "feat: implement batch serialization using existing CBOR functions"
```

---

## Phase 3: Database API - Sync Batch

### Task 3.1: Add batch API declarations

**Files:**
- Modify: `src/Database/database.h`

- [ ] **Step 1: Add function declarations**

Add to `src/Database/database.h` after existing API:
```c
/**
 * Submit batch synchronously.
 *
 * @param db Database to modify
 * @param batch Batch to submit
 * @return 0 on success, error code on failure
 */
int database_write_batch_sync(database_t* db, batch_t* batch);

/**
 * Submit batch asynchronously.
 *
 * @param db Database to modify
 * @param batch Batch to submit
 * @param promise Promise to resolve with result
 */
void database_write_batch(database_t* db, batch_t* batch, promise_t* promise);
```

- [ ] **Step 2: Commit header changes**

```bash
git add src/Database/database.h
git commit -m "feat: add batch API declarations"
```

---

### Task 3.2: Implement database_write_batch_sync

**Files:**
- Modify: `src/Database/database.c`
- Modify: `tests/test_database.cpp`

- [ ] **Step 1: Write integration tests**

Add to `tests/test_database.cpp`:
- Write batch sync basic
- Write batch sync empty batch (error -3)
- Write batch sync too large (error -5)
- Write batch sync double submit (error -6)
- Write batch sync concurrent batches

- [ ] **Step 2: Run tests to verify they fail**

Run: `cd build-test && ./test_database --gtest_filter=DatabaseTest.WriteBatchSync*`

Expected: FAIL (undefined reference)

- [ ] **Step 3: Implement database_write_batch_sync**

Implement in `src/Database/database.c`:
```c
int database_write_batch_sync(database_t* db, batch_t* batch) {
    if (db == NULL || batch == NULL) return -1;
    if (batch->count == 0) return -3;
    if (batch->submitted) return -6;

    // Check size
    size_t size = batch_estimate_size(batch);
    if (size > db->wal_manager->config.max_file_size) return -5;

    // Generate transaction ID
    transaction_id_t txn_id = transaction_id_get_next();

    // Mark as submitted
    platform_lock(&batch->lock);
    batch->submitted = 1;
    platform_unlock(&batch->lock);

    // Acquire all write locks
    for (size_t i = 0; i < WRITE_LOCK_SHARDS; i++) {
        platform_lock(&db->write_locks[i]);
    }

    // Serialize batch
    buffer_t* data = serialize_batch(batch);

    // Write to WAL
    int result = wal_write(db->wal, txn_id, WAL_BATCH, data);
    buffer_destroy(data);

    if (result != 0) {
        // Release locks
        for (size_t i = WRITE_LOCK_SHARDS; i > 0; i--) {
            platform_unlock(&db->write_locks[i - 1]);
        }
        return result;
    }

    // Apply to trie
    for (size_t i = 0; i < batch->count; i++) {
        int op_result;
        if (batch->ops[i].type == WAL_PUT) {
            op_result = hbtrie_insert(db->trie, batch->ops[i].path, batch->ops[i].value);
        } else {
            op_result = hbtrie_delete(db->trie, batch->ops[i].path);
        }

        if (op_result != 0) {
            // CRITICAL: Crash to force recovery
            fprintf(stderr, "CRITICAL: Batch apply failed, crashing for recovery\n");
            abort();
        }
    }

    // Release locks
    for (size_t i = WRITE_LOCK_SHARDS; i > 0; i--) {
        platform_unlock(&db->write_locks[i - 1]);
    }

    return 0;
}
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `cd build-test && make && ./test_database --gtest_filter=DatabaseTest.WriteBatchSync*`

Expected: PASS

- [ ] **Step 5: Commit implementation**

```bash
git add src/Database/database.c tests/test_database.cpp
git commit -m "feat: implement database_write_batch_sync with all validations"
```

---

## Phase 4: Async Support

### Task 4.1: Implement async batch execution

**Files:**
- Modify: `src/Database/database.c`

- [ ] **Step 1: Implement database_write_batch async**

Add to `src/Database/database.c`:
```c
typedef struct {
    database_t* db;
    batch_t* batch;
    promise_t* promise;
} batch_work_t;

static void batch_execute_work(void* ctx) {
    batch_work_t* work = (batch_work_t*)ctx;

    refcounter_reference((refcounter_t*)work->batch);
    int result = database_write_batch_sync(work->db, work->batch);
    promise_resolve(work->promise, &result, sizeof(int));
    refcounter_dereference((refcounter_t*)work->batch);

    free(work);
}

void database_write_batch(database_t* db, batch_t* batch, promise_t* promise) {
    if (db == NULL || batch == NULL || promise == NULL) {
        int error = -1;
        if (promise) promise_resolve(promise, &error, sizeof(int));
        return;
    }

    batch_work_t* work = malloc(sizeof(batch_work_t));
    if (work == NULL) {
        int error = -1;
        promise_resolve(promise, &error, sizeof(int));
        return;
    }

    work->db = db;
    work->batch = batch;
    work->promise = promise;

    refcounter_reference((refcounter_t*)batch);
    work_t* task = work_create(batch_execute_work, NULL, work);
    work_pool_enqueue(db->pool, task);
}
```

- [ ] **Step 2: Commit async implementation**

```bash
git add src/Database/database.c
git commit -m "feat: implement database_write_batch async version"
```

---

## Phase 5: Recovery

### Task 5.1: Add batch recovery logic

**Files:**
- Modify: `src/Database/wal_manager.c`

- [ ] **Step 1: Add WAL_BATCH case in recovery**

Edit recovery function in `src/Database/wal_manager.c`:
```c
case WAL_BATCH: {
    // Deserialize batch
    batch_op_t* ops = NULL;
    size_t op_count = 0;

    if (deserialize_batch(data, &ops, &op_count) != 0) {
        fprintf(stderr, "ERROR: Failed to deserialize batch\n");
        break;
    }

    // Apply each operation
    for (size_t i = 0; i < op_count; i++) {
        if (ops[i].type == WAL_PUT) {
            hbtrie_insert(trie, ops[i].path, ops[i].value);
        } else {
            hbtrie_delete(trie, ops[i].path);
        }

        // Clean up
        path_destroy(ops[i].path);
        if (ops[i].value) identifier_destroy(ops[i].value);
    }

    free(ops);
    break;
}
```

- [ ] **Step 2: Commit recovery implementation**

```bash
git add src/Database/wal_manager.c
git commit -m "feat: implement WAL_BATCH recovery logic"
```

---

## Phase 6: Testing and Documentation

### Task 6.1: Add missing test cases

**Files:**
- Modify: `tests/test_batch.cpp`
- Modify: `tests/test_database.cpp`

- [ ] **Step 1: Add validation tests**

Add to `tests/test_batch.cpp`:
- Test NULL batch (returns -1)
- Test NULL path (returns -1)
- Test NULL value for PUT (returns -1)
- Test full batch (returns -2)
- Test submitted batch (returns -6)

- [ ] **Step 2: Add thread safety test**

Add concurrent additions test:
- Create batch
- Spawn multiple threads
- Each thread adds operations
- Verify all operations present

- [ ] **Step 3: Add performance benchmark**

Add to `tests/test_database.cpp`:
- Benchmark: 1000 individual puts vs batch of 1000
- Compare throughput

- [ ] **Step 4: Add CRC32 validation test**

Add to recovery tests:
- Corrupt batch data
- Verify recovery skips corrupted batch
- Verify log shows error

- [ ] **Step 5: Run all tests**

Run: `cd build-test && make && ctest`

Expected: All tests pass

- [ ] **Step 6: Commit tests**

```bash
git add tests/test_batch.cpp tests/test_database.cpp
git commit -m "test: add comprehensive batch tests (validation, thread safety, performance, CRC32)"
```

---

### Task 6.2: Update documentation

**Files:**
- Modify: `STYLEGUIDE.md`

- [ ] **Step 1: Add batch API patterns to STYLEGUIDE.md**

Add section with examples for:
- Creating and submitting batches
- Error handling
- Async batch submission

- [ ] **Step 2: Commit documentation**

```bash
git add STYLEGUIDE.md
git commit -m "docs: add batch API usage patterns to STYLEGUIDE"
```

---

### Task 6.3: Final testing

- [ ] **Step 1: Run full test suite**

Run: `cd build-test && ctest --output-on-failure`

Expected: All tests pass

- [ ] **Step 2: Run valgrind memory check**

Run: `cd build-test && valgrind --leak-check=full ./test_batch`

Expected: No memory leaks

- [ ] **Step 3: Create final commit**

```bash
git add -A
git commit -m "feat: complete batch write operations implementation

All tests passing, no memory leaks, documentation complete."
```

---

## Summary

This implementation plan covers all spec requirements:

- **Phase 1:** Batch builder with validation and size estimation
- **Phase 2:** WAL batch format using existing CBOR serialization
- **Phase 3:** Database sync API with full validation
- **Phase 4:** Async batch execution with reference counting
- **Phase 5:** Batch recovery logic
- **Phase 6:** Comprehensive testing and documentation

All critical issues from spec review addressed:
1. ✅ Uses existing CBOR serialization functions
2. ✅ Accurate size estimation using serialized sizes
3. ✅ Phase 2 completed before Phase 3
4. ✅ Missing test cases added
5. ✅ Full validation implemented
6. ✅ Write locks verified (database.h:43)
7. ✅ Max_batch_size in batch_t (default 10000)
8. ✅ Complete implementation before tests run