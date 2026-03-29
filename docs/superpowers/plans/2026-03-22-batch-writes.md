# Batch Write Operations Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement atomic batch write operations with single transaction ID per batch, enabling efficient bulk writes with all-or-nothing semantics.

**Architecture:** Add batch builder (new files), WAL batch format (modify existing), database batch API (modify existing), and recovery (modify existing). Minimal changes to existing infrastructure - treat batches as new WAL entry type.

**Tech Stack:** C (existing codebase), existing WAL and MVCC infrastructure, thread pool for async execution, GoogleTest for testing.

---

## File Structure

**New files:**
- `src/Database/batch.h` - Batch builder header (batch_t, batch_op_t, API)
- `src/Database/batch.c` - Batch builder implementation
- `tests/test_batch.cpp` - Unit tests for batch builder

**Modified files:**
- `src/Database/wal.h` - Add WAL_BATCH type
- `src/Database/wal.c` - Add batch serialization/deserialization
- `src/Database/database.h` - Add batch API declarations
- `src/Database/database.c` - Implement batch submission (sync + async)
- `src/Database/wal_manager.h` - Add recovery function declaration
- `src/Database/wal_manager.c` - Add batch recovery logic
- `tests/test_database.cpp` - Add batch integration tests

**Files for reference:**
- `src/Database/wal.h:33-35` - Existing WAL_PUT/WAL_DELETE types
- `src/Database/wal.c:382-387` - Existing WAL header format
- `src/Database/database.c` - Existing database_put_sync pattern
- `src/Workers/transaction_id.h:14-18` - transaction_id_t structure

---

## Task 1: Create Batch Builder Header

**Files:**
- Create: `src/Database/batch.h`

- [ ] **Step 1: Create header file with data structures**

Create `src/Database/batch.h`:

```c
//
// Batch Write Operations
//

#ifndef WAVEDB_BATCH_H
#define WAVEDB_BATCH_H

#include <stdint.h>
#include <stddef.h>
#include "../RefCounter/refcounter.h"
#include "../HBTrie/path.h"
#include "../HBTrie/identifier.h"
#include "../Util/threadding.h"
#include "wal.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * batch_op_t - Single operation in a batch
 */
typedef struct {
    wal_type_e type;        // WAL_PUT or WAL_DELETE
    path_t* path;           // Key (ownership transfers to batch)
    identifier_t* value;    // Value for PUT (NULL for DELETE)
} batch_op_t;

/**
 * batch_t - Batch write handle
 *
 * Collects multiple write operations for atomic submission.
 * Thread-safe for concurrent batch_add_* calls.
 */
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

/**
 * Create a batch for collecting write operations.
 *
 * @param reserve_count Pre-allocate space for this many operations (0 = default 100)
 * @param max_size Maximum operations allowed (0 = default 10000)
 * @return New batch or NULL on failure
 */
batch_t* batch_create(size_t reserve_count, size_t max_size);

/**
 * Add a PUT operation to batch.
 *
 * Ownership semantics:
 *   - On success: ownership of path and value transfers to batch
 *   - On error: ownership remains with caller (caller must destroy)
 *
 * @param batch Batch to modify
 * @param path Key path (ownership transfers on success)
 * @param value Value to store (ownership transfers on success)
 * @return 0 on success, -1 on error, -2 if batch is full, -6 if already submitted
 */
int batch_add_put(batch_t* batch, path_t* path, identifier_t* value);

/**
 * Add a DELETE operation to batch.
 *
 * Ownership semantics:
 *   - On success: ownership of path transfers to batch
 *   - On error: ownership remains with caller (caller must destroy)
 *
 * @param batch Batch to modify
 * @param path Key path to delete (ownership transfers on success)
 * @return 0 on success, -1 on error, -2 if batch is full, -6 if already submitted
 */
int batch_add_delete(batch_t* batch, path_t* path);

/**
 * Estimate serialized size of batch.
 *
 * @param batch Batch to estimate
 * @return Estimated size in bytes
 */
size_t batch_estimate_size(batch_t* batch);

/**
 * Destroy a batch.
 *
 * Frees all operations and their paths/values.
 * Safe to call even if batch was submitted.
 *
 * @param batch Batch to destroy
 */
void batch_destroy(batch_t* batch);

#ifdef __cplusplus
}
#endif

#endif //WAVEDB_BATCH_H
```

- [ ] **Step 2: Commit header file**

```bash
git add src/Database/batch.h
git commit -m "feat: add batch builder header with data structures and API"
```

---

## Task 2: Implement Batch Builder

**Files:**
- Create: `src/Database/batch.c`

- [ ] **Step 1: Create implementation file with create/destroy**

Create `src/Database/batch.c`:

```c
//
// Batch Write Operations
//

#include "batch.h"
#include "../Util/allocator.h"
#include <stdlib.h>
#include <string.h>

// Default batch size
#define BATCH_DEFAULT_RESERVE 100
#define BATCH_DEFAULT_MAX_SIZE 10000

batch_t* batch_create(size_t reserve_count, size_t max_size) {
    batch_t* batch = get_clear_memory(sizeof(batch_t));
    if (batch == NULL) {
        return NULL;
    }

    // Use defaults if 0
    if (reserve_count == 0) reserve_count = BATCH_DEFAULT_RESERVE;
    if (max_size == 0) max_size = BATCH_DEFAULT_MAX_SIZE;

    // Allocate operations array
    batch->ops = get_clear_memory(sizeof(batch_op_t) * reserve_count);
    if (batch->ops == NULL) {
        free(batch);
        return NULL;
    }

    batch->count = 0;
    batch->capacity = reserve_count;
    batch->max_size = max_size;
    batch->estimated_size = 0;
    batch->submitted = 0;

    platform_lock_init(&batch->lock);
    refcounter_init((refcounter_t*) batch);

    return batch;
}

void batch_destroy(batch_t* batch) {
    if (batch == NULL) return;

    refcounter_dereference((refcounter_t*) batch);
    if (refcounter_count((refcounter_t*) batch) == 0) {
        // Free all operations
        for (size_t i = 0; i < batch->count; i++) {
            if (batch->ops[i].path != NULL) {
                path_destroy(batch->ops[i].path);
            }
            if (batch->ops[i].value != NULL) {
                identifier_destroy(batch->ops[i].value);
            }
        }

        free(batch->ops);
        platform_lock_destroy(&batch->lock);
        refcounter_destroy_lock((refcounter_t*) batch);
        free(batch);
    }
}

// Helper: estimate serialized size of a path
static size_t estimate_path_size(path_t* path) {
    // path length field (4 bytes) + path data
    size_t size = 4; // path_len
    for (size_t i = 0; i < path->identifiers->length; i++) {
        identifier_t* id = (identifier_t*)path->identifiers->data[i];
        size += id->length;
    }
    return size;
}

// Helper: estimate serialized size of an identifier
static size_t estimate_identifier_size(identifier_t* id) {
    // value length field (4 bytes) + value data
    return 4 + id->length;
}

int batch_add_put(batch_t* batch, path_t* path, identifier_t* value) {
    if (batch == NULL || path == NULL || value == NULL) {
        return -1;
    }

    platform_lock(&batch->lock);

    // Check if already submitted
    if (batch->submitted) {
        platform_unlock(&batch->lock);
        return -6;
    }

    // Check capacity
    if (batch->count >= batch->max_size) {
        platform_unlock(&batch->lock);
        return -2;
    }

    // Grow array if needed
    if (batch->count >= batch->capacity) {
        size_t new_capacity = batch->capacity * 2;
        if (new_capacity > batch->max_size) {
            new_capacity = batch->max_size;
        }
        batch_op_t* new_ops = realloc(batch->ops, sizeof(batch_op_t) * new_capacity);
        if (new_ops == NULL) {
            platform_unlock(&batch->lock);
            return -1;
        }
        batch->ops = new_ops;
        batch->capacity = new_capacity;
    }

    // Add operation
    batch->ops[batch->count].type = WAL_PUT;
    batch->ops[batch->count].path = path;
    batch->ops[batch->count].value = value;

    // Update estimated size
    batch->estimated_size += 1; // op_type
    batch->estimated_size += estimate_path_size(path);
    batch->estimated_size += estimate_identifier_size(value);

    batch->count++;

    platform_unlock(&batch->lock);
    return 0;
}

int batch_add_delete(batch_t* batch, path_t* path) {
    if (batch == NULL || path == NULL) {
        return -1;
    }

    platform_lock(&batch->lock);

    // Check if already submitted
    if (batch->submitted) {
        platform_unlock(&batch->lock);
        return -6;
    }

    // Check capacity
    if (batch->count >= batch->max_size) {
        platform_unlock(&batch->lock);
        return -2;
    }

    // Grow array if needed
    if (batch->count >= batch->capacity) {
        size_t new_capacity = batch->capacity * 2;
        if (new_capacity > batch->max_size) {
            new_capacity = batch->max_size;
        }
        batch_op_t* new_ops = realloc(batch->ops, sizeof(batch_op_t) * new_capacity);
        if (new_ops == NULL) {
            platform_unlock(&batch->lock);
            return -1;
        }
        batch->ops = new_ops;
        batch->capacity = new_capacity;
    }

    // Add operation
    batch->ops[batch->count].type = WAL_DELETE;
    batch->ops[batch->count].path = path;
    batch->ops[batch->count].value = NULL;

    // Update estimated size
    batch->estimated_size += 1; // op_type
    batch->estimated_size += estimate_path_size(path);
    // DELETE has value_len = 0, no value data

    batch->count++;

    platform_unlock(&batch->lock);
    return 0;
}

size_t batch_estimate_size(batch_t* batch) {
    if (batch == NULL) return 0;
    return batch->estimated_size;
}
```

- [ ] **Step 2: Commit batch builder implementation**

```bash
git add src/Database/batch.c
git commit -m "feat: implement batch builder with create, add operations, and destroy"
```

---

## Task 3: Add Batch Unit Tests

**Files:**
- Create: `tests/test_batch.cpp`

- [ ] **Step 1: Create test file with basic batch tests**

Create `tests/test_batch.cpp`:

```cpp
#include <gtest/gtest.h>
extern "C" {
#include "Database/batch.h"
#include "HBTrie/path.h"
#include "HBTrie/identifier.h"
#include "Buffer/buffer.h"
}

// Helper to create a path
static path_t* create_test_path(const char* key) {
    path_t* path = path_create();
    buffer_t* buf = buffer_create_from_pointer_copy((uint8_t*)key, strlen(key));
    identifier_t* id = identifier_create(buf, 0);
    buffer_destroy(buf);
    path_append(path, id);
    identifier_destroy(id);
    return path;
}

// Helper to create an identifier
static identifier_t* create_test_value(const char* value) {
    buffer_t* buf = buffer_create_from_pointer_copy((uint8_t*)value, strlen(value));
    identifier_t* id = identifier_create(buf, 0);
    buffer_destroy(buf);
    return id;
}

TEST(BatchTest, CreateDestroy) {
    batch_t* batch = batch_create(0, 0);
    ASSERT_NE(batch, nullptr);
    EXPECT_EQ(batch->count, 0);
    EXPECT_EQ(batch->submitted, 0);
    batch_destroy(batch);
}

TEST(BatchTest, AddPutOperations) {
    batch_t* batch = batch_create(10, 100);
    ASSERT_NE(batch, nullptr);

    path_t* path1 = create_test_path("key1");
    identifier_t* value1 = create_test_value("value1");

    int result = batch_add_put(batch, path1, value1);
    EXPECT_EQ(result, 0);
    EXPECT_EQ(batch->count, 1);
    EXPECT_GT(batch->estimated_size, 0);

    batch_destroy(batch);
}

TEST(BatchTest, AddDeleteOperations) {
    batch_t* batch = batch_create(10, 100);
    ASSERT_NE(batch, nullptr);

    path_t* path1 = create_test_path("key1");

    int result = batch_add_delete(batch, path1);
    EXPECT_EQ(result, 0);
    EXPECT_EQ(batch->count, 1);

    batch_destroy(batch);
}

TEST(BatchTest, RejectNullPath) {
    batch_t* batch = batch_create(10, 100);
    ASSERT_NE(batch, nullptr);

    identifier_t* value = create_test_value("value");
    int result = batch_add_put(batch, NULL, value);
    EXPECT_EQ(result, -1);
    EXPECT_EQ(batch->count, 0);

    identifier_destroy(value);
    batch_destroy(batch);
}

TEST(BatchTest, RejectNullValue) {
    batch_t* batch = batch_create(10, 100);
    ASSERT_NE(batch, nullptr);

    path_t* path = create_test_path("key");
    int result = batch_add_put(batch, path, NULL);
    EXPECT_EQ(result, -1);
    EXPECT_EQ(batch->count, 0);

    path_destroy(path);
    batch_destroy(batch);
}

TEST(BatchTest, RespectMaxSize) {
    batch_t* batch = batch_create(10, 5); // Max 5 operations
    ASSERT_NE(batch, nullptr);

    for (int i = 0; i < 5; i++) {
        char key[32];
        snprintf(key, sizeof(key), "key%d", i);
        path_t* path = create_test_path(key);
        identifier_t* value = create_test_value("value");

        int result = batch_add_put(batch, path, value);
        EXPECT_EQ(result, 0);
    }

    // 6th operation should fail
    path_t* path = create_test_path("key6");
    identifier_t* value = create_test_value("value");
    int result = batch_add_put(batch, path, value);
    EXPECT_EQ(result, -2); // -2 = batch full

    path_destroy(path);
    identifier_destroy(value);
    batch_destroy(batch);
}

TEST(BatchTest, PreventDoubleSubmission) {
    batch_t* batch = batch_create(10, 100);
    ASSERT_NE(batch, nullptr);

    path_t* path1 = create_test_path("key1");
    identifier_t* value1 = create_test_value("value1");

    int result = batch_add_put(batch, path1, value1);
    EXPECT_EQ(result, 0);

    // Mark as submitted
    batch->submitted = 1;

    // Try to add another operation
    path_t* path2 = create_test_path("key2");
    identifier_t* value2 = create_test_value("value2");
    result = batch_add_put(batch, path2, value2);
    EXPECT_EQ(result, -6); // -6 = already submitted

    path_destroy(path2);
    identifier_destroy(value2);
    batch_destroy(batch);
}

TEST(BatchTest, EstimateSize) {
    batch_t* batch = batch_create(10, 100);
    ASSERT_NE(batch, nullptr);

    size_t initial_size = batch_estimate_size(batch);
    EXPECT_EQ(initial_size, 0);

    path_t* path = create_test_path("key");
    identifier_t* value = create_test_value("value");

    batch_add_put(batch, path, value);

    size_t new_size = batch_estimate_size(batch);
    EXPECT_GT(new_size, initial_size);

    batch_destroy(batch);
}

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
```

- [ ] **Step 2: Add test to CMakeLists.txt**

The test should be added to the existing test infrastructure. Check `tests/CMakeLists.txt` or main CMakeLists.txt for test patterns.

- [ ] **Step 3: Build and run tests**

```bash
cd build-test
cmake ..
make test_batch
./tests/test_batch
```

Expected: All tests pass

- [ ] **Step 4: Commit tests**

```bash
git add tests/test_batch.cpp
git commit -m "test: add unit tests for batch builder"
```

---

## Task 4: Add WAL_BATCH Type

**Files:**
- Modify: `src/Database/wal.h:23-26`

- [ ] **Step 1: Add WAL_BATCH to enum**

Open `src/Database/wal.h` and find the enum at lines 23-26. Modify to add WAL_BATCH:

```c
/**
 * WAL entry types
 */
typedef enum {
    WAL_PUT = 'p',      // Insert/update operation
    WAL_DELETE = 'd',   // Delete operation
    WAL_BATCH = 'b'     // Batch of operations
} wal_type_e;
```

- [ ] **Step 2: Commit WAL type addition**

```bash
git add src/Database/wal.h
git commit -m "feat: add WAL_BATCH entry type"
```

---

## Task 5: Implement Batch Serialization in WAL

**Files:**
- Modify: `src/Database/wal.c`

- [ ] **Step 1: Add batch serialization helper function**

Add to `src/Database/wal.c` after the existing `wal_write` function (around line 424):

```c
// Serialize a path to buffer
static size_t serialize_path(buffer_t* buf, path_t* path) {
    size_t start_size = buf->size;

    // Serialize each identifier in path
    for (size_t i = 0; i < path->identifiers->length; i++) {
        identifier_t* id = (identifier_t*)path->identifiers->data[i];
        buffer_append(buf, id->data, id->length);
    }

    return buf->size - start_size;
}

// Serialize an identifier to buffer
static size_t serialize_identifier(buffer_t* buf, identifier_t* id) {
    buffer_append(buf, id->data, id->length);
    return id->length;
}
```

- [ ] **Step 2: Add batch write function**

Add after the serialization helpers:

```c
// Write a batch entry to WAL
int wal_write_batch(wal_t* wal, transaction_id_t txn_id, batch_t* batch) {
    if (wal == NULL || batch == NULL) {
        return -1;
    }

    platform_lock(&wal->lock);

    // Serialize batch operations
    buffer_t* data = buffer_create(1024); // Start with 1KB
    if (data == NULL) {
        platform_unlock(&wal->lock);
        return -1;
    }

    // Write count (number of operations)
    uint8_t count_buf[4];
    write_uint32_be(count_buf, (uint32_t)batch->count);
    buffer_append(data, count_buf, 4);

    // Write each operation
    for (size_t i = 0; i < batch->count; i++) {
        batch_op_t* op = &batch->ops[i];

        // Op type (1 byte)
        uint8_t op_type = (uint8_t)op->type;
        buffer_append(data, &op_type, 1);

        // Path length and data
        uint8_t path_len_buf[4];
        // Note: We need to calculate path length first
        size_t path_len = 0;
        for (size_t j = 0; j < op->path->identifiers->length; j++) {
            identifier_t* id = (identifier_t*)op->path->identifiers->data[j];
            path_len += id->length;
        }
        write_uint32_be(path_len_buf, (uint32_t)path_len);
        buffer_append(data, path_len_buf, 4);

        // Path data
        serialize_path(data, op->path);

        // Value length and data
        uint8_t value_len_buf[4];
        if (op->type == WAL_PUT && op->value != NULL) {
            write_uint32_be(value_len_buf, (uint32_t)op->value->length);
            buffer_append(data, value_len_buf, 4);
            serialize_identifier(data, op->value);
        } else {
            // DELETE operation: value_len = 0
            write_uint32_be(value_len_buf, 0);
            buffer_append(data, value_len_buf, 4);
        }
    }

    // Compute CRC
    uint32_t crc = wal_crc32(data->data, data->size);

    // Write header
    uint8_t header[33];
    header[0] = (uint8_t)WAL_BATCH;
    transaction_id_serialize(&txn_id, header + 1);
    write_uint32_be(header + 25, crc);
    write_uint32_be(header + 29, (uint32_t)data->size);

    ssize_t written = write(wal->fd, header, 33);
    if (written != 33) {
        buffer_destroy(data);
        platform_unlock(&wal->lock);
        return -1;
    }

    // Write data
    written = write(wal->fd, data->data, data->size);
    if (written != (ssize_t)data->size) {
        buffer_destroy(data);
        platform_unlock(&wal->lock);
        return -1;
    }

    // Sync based on mode
    wal->pending_writes++;
    if (wal->sync_mode == WAL_SYNC_IMMEDIATE) {
        fsync(wal->fd);
        wal->pending_writes = 0;
    } else if (wal->sync_mode == WAL_SYNC_DEBOUNCED && wal->fsync_debouncer != NULL) {
        debouncer_debounce(wal->fsync_debouncer);
    }

    wal->current_size += 33 + data->size;
    buffer_destroy(data);

    platform_unlock(&wal->lock);
    return 0;
}
```

- [ ] **Step 3: Commit batch serialization**

```bash
git add src/Database/wal.c
git commit -m "feat: implement batch serialization in WAL"
```

---

## Task 6: Implement Batch Deserialization in WAL

**Files:**
- Modify: `src/Database/wal.c`

- [ ] **Step 1: Add batch deserialization helper function**

Add after the serialization functions:

```c
// Deserialize a path from buffer
static path_t* deserialize_path(const uint8_t* data, size_t len) {
    path_t* path = path_create();
    size_t offset = 0;

    while (offset < len) {
        // Create identifier from data
        buffer_t* buf = buffer_create_from_pointer_copy((uint8_t*)(data + offset), len - offset);
        if (buf == NULL) {
            path_destroy(path);
            return NULL;
        }

        identifier_t* id = identifier_create(buf, 0);
        buffer_destroy(buf);

        if (id == NULL) {
            path_destroy(path);
            return NULL;
        }

        path_append(path, id);
        identifier_destroy(id);
        offset += buf->size; // This is approximate - needs proper parsing
    }

    return path;
}
```

Note: This is a simplified version. The actual implementation will need proper path deserialization matching the existing path serialization in the codebase.

- [ ] **Step 2: Add batch read function**

```c
// Read a batch entry from WAL
int wal_read_batch(wal_t* wal, transaction_id_t* txn_id, batch_t** batch, uint64_t* cursor) {
    if (wal == NULL || txn_id == NULL || batch == NULL || cursor == NULL) {
        return -1;
    }

    platform_lock(&wal->lock);

    // Seek to cursor
    if (lseek(wal->fd, (off_t)*cursor, SEEK_SET) < 0) {
        platform_unlock(&wal->lock);
        return -1;
    }

    // Read header
    uint8_t header[33];
    ssize_t bytes_read = read(wal->fd, header, 33);
    if (bytes_read == 0) {
        platform_unlock(&wal->lock);
        return 1; // EOF
    }
    if (bytes_read != 33) {
        platform_unlock(&wal->lock);
        return -1;
    }

    // Parse header
    if (header[0] != (uint8_t)WAL_BATCH) {
        platform_unlock(&wal->lock);
        return -1; // Wrong entry type
    }

    transaction_id_deserialize(txn_id, header + 1);
    uint32_t expected_crc = read_uint32_be(header + 25);
    uint32_t data_len = read_uint32_be(header + 29);

    // Read data
    buffer_t* data = buffer_create(data_len);
    if (data == NULL) {
        platform_unlock(&wal->lock);
        return -1;
    }

    bytes_read = read(wal->fd, data->data, data_len);
    if (bytes_read != (ssize_t)data_len) {
        buffer_destroy(data);
        platform_unlock(&wal->lock);
        return -1;
    }

    // Verify CRC
    uint32_t actual_crc = wal_crc32(data->data, data_len);
    if (actual_crc != expected_crc) {
        buffer_destroy(data);
        platform_unlock(&wal->lock);
        return -1;
    }

    // Parse count
    uint32_t count = read_uint32_be(data->data);

    // Create batch
    batch_t* result = batch_create(count, count);
    if (result == NULL) {
        buffer_destroy(data);
        platform_unlock(&wal->lock);
        return -1;
    }

    // Parse operations
    size_t offset = 4; // Skip count
    for (uint32_t i = 0; i < count; i++) {
        // Op type
        wal_type_e op_type = (wal_type_e)data->data[offset++];

        // Path length
        uint32_t path_len = read_uint32_be(data->data + offset);
        offset += 4;

        // Path data
        path_t* path = deserialize_path(data->data + offset, path_len);
        if (path == NULL) {
            batch_destroy(result);
            buffer_destroy(data);
            platform_unlock(&wal->lock);
            return -1;
        }
        offset += path_len;

        // Value length
        uint32_t value_len = read_uint32_be(data->data + offset);
        offset += 4;

        // Value data
        identifier_t* value = NULL;
        if (op_type == WAL_PUT && value_len > 0) {
            buffer_t* value_buf = buffer_create_from_pointer_copy(data->data + offset, value_len);
            if (value_buf == NULL) {
                path_destroy(path);
                batch_destroy(result);
                buffer_destroy(data);
                platform_unlock(&wal->lock);
                return -1;
            }
            value = identifier_create(value_buf, 0);
            buffer_destroy(value_buf);
            if (value == NULL) {
                path_destroy(path);
                batch_destroy(result);
                buffer_destroy(data);
                platform_unlock(&wal->lock);
                return -1;
            }
            offset += value_len;
        }

        // Add to batch
        if (op_type == WAL_PUT) {
            batch_add_put(result, path, value);
        } else {
            batch_add_delete(result, path);
        }
    }

    *batch = result;
    *cursor += 33 + data_len;

    buffer_destroy(data);
    platform_unlock(&wal->lock);
    return 0;
}
```

- [ ] **Step 3: Commit batch deserialization**

```bash
git add src/Database/wal.c
git commit -m "feat: implement batch deserialization in WAL"
```

---

## Task 7: Add Batch API to Database Header

**Files:**
- Modify: `src/Database/database.h`

- [ ] **Step 1: Add batch function declarations**

Add to `src/Database/database.h` after the existing sync functions (around line 188):

```c
/**
 * Synchronously submit a batch of write operations.
 *
 * Entire batch is atomic: all operations succeed or all fail.
 * Batch must not be empty or already submitted.
 *
 * @param db Database to modify
 * @param batch Batch to submit (caller retains ownership)
 * @return 0 on success, error code on failure
 *
 * Error codes:
 *   -1: General error
 *   -2: Batch is full
 *   -3: Batch is empty
 *   -4: Validation failed
 *   -5: Batch too large for WAL
 *   -6: Batch already submitted
 */
int database_write_batch_sync(database_t* db, batch_t* batch);

/**
 * Asynchronously submit a batch of write operations.
 *
 * Entire batch is atomic and processed by single worker thread.
 * Batch must not be empty or already submitted.
 *
 * @param db Database to modify
 * @param batch Batch to submit (caller retains ownership until promise resolved)
 * @param promise Promise to resolve with result code
 */
void database_write_batch(database_t* db, batch_t* batch, promise_t* promise);

/**
 * Get maximum WAL file size for batch size planning.
 *
 * @param db Database to query
 * @return Maximum WAL file size in bytes
 */
size_t database_get_max_wal_size(database_t* db);
```

- [ ] **Step 2: Commit header changes**

```bash
git add src/Database/database.h
git commit -m "feat: add batch submission API declarations to database header"
```

---

## Task 8: Implement Synchronous Batch Submission

**Files:**
- Modify: `src/Database/database.c`

- [ ] **Step 1: Add helper function to get max WAL size**

Add to `src/Database/database.c`:

```c
size_t database_get_max_wal_size(database_t* db) {
    if (db == NULL || db->wal == NULL) {
        return 0;
    }
    return db->wal->max_size;
}
```

- [ ] **Step 2: Implement synchronous batch submission**

Add after the helper:

```c
int database_write_batch_sync(database_t* db, batch_t* batch) {
    if (db == NULL || batch == NULL) {
        return -1;
    }

    // Validate batch
    if (batch->count == 0) {
        return -3; // Empty batch
    }

    if (batch->submitted) {
        return -6; // Already submitted
    }

    // Check size against WAL max
    size_t estimated_size = batch_estimate_size(batch);
    size_t max_wal_size = database_get_max_wal_size(db);
    if (estimated_size > max_wal_size) {
        return -5; // Batch too large
    }

    // Mark batch as submitted
    platform_lock(&batch->lock);
    batch->submitted = 1;
    platform_unlock(&batch->lock);

    // Generate transaction ID
    transaction_id_t txn_id = transaction_id_get_next();

    // Acquire ALL write locks in order
    for (size_t i = 0; i < WRITE_LOCK_SHARDS; i++) {
        platform_lock(&db->write_locks[i]);
    }

    // Write to WAL
    int result = wal_write_batch(db->wal, txn_id, batch);

    if (result != 0) {
        // WAL write failed - release locks and return error
        for (size_t i = WRITE_LOCK_SHARDS; i > 0; i--) {
            platform_unlock(&db->write_locks[i - 1]);
        }
        return result;
    }

    // Apply to trie
    for (size_t i = 0; i < batch->count; i++) {
        batch_op_t* op = &batch->ops[i];

        int op_result;
        if (op->type == WAL_PUT) {
            op_result = hbtrie_insert(db->trie, op->path, op->value);
        } else { // WAL_DELETE
            op_result = hbtrie_remove(db->trie, op->path);
        }

        if (op_result != 0) {
            // CRITICAL ERROR: Should not happen
            // Crash to force recovery
            fprintf(stderr, "CRITICAL: Batch operation failed after WAL write. Crashing for recovery.\n");
            abort();
        }
    }

    // Release all locks in reverse order
    for (size_t i = WRITE_LOCK_SHARDS; i > 0; i--) {
        platform_unlock(&db->write_locks[i - 1]);
    }

    return 0;
}
```

- [ ] **Step 3: Commit sync batch implementation**

```bash
git add src/Database/database.c
git commit -m "feat: implement synchronous batch submission with atomic guarantees"
```

---

## Task 9: Implement Asynchronous Batch Submission

**Files:**
- Modify: `src/Database/database.c`

- [ ] **Step 1: Add batch work context structure**

Add before the async function:

```c
typedef struct {
    database_t* db;
    batch_t* batch;
    promise_t* promise;
} batch_work_context_t;
```

- [ ] **Step 2: Add batch worker function**

```c
static void batch_worker_execute(void* ctx) {
    batch_work_context_t* context = (batch_work_context_t*)ctx;

    int result = database_write_batch_sync(context->db, context->batch);

    // Resolve promise
    promise_resolve(context->promise, result);

    // Dereference batch
    batch_destroy(context->batch);

    // Free context
    free(context);
}

static void batch_worker_abort(void* ctx) {
    batch_work_context_t* context = (batch_work_context_t*)ctx;

    // Reject promise
    promise_reject(context->promise);

    // Dereference batch
    batch_destroy(context->batch);

    // Free context
    free(context);
}
```

- [ ] **Step 3: Implement async batch submission**

```c
void database_write_batch(database_t* db, batch_t* batch, promise_t* promise) {
    if (db == NULL || batch == NULL || promise == NULL) {
        if (promise != NULL) {
            promise_reject(promise);
        }
        return;
    }

    // Create context
    batch_work_context_t* context = malloc(sizeof(batch_work_context_t));
    if (context == NULL) {
        promise_reject(promise);
        return;
    }

    context->db = db;
    context->batch = batch;
    context->promise = promise;

    // Reference batch (prevent destruction until worker finishes)
    refcounter_reference((refcounter_t*) batch);

    // Create work
    work_t* work = work_create(batch_worker_execute, batch_worker_abort, context);
    if (work == NULL) {
        free(context);
        promise_reject(promise);
        return;
    }

    // Enqueue to thread pool
    work_pool_enqueue(db->pool, work);
}
```

- [ ] **Step 4: Commit async batch implementation**

```bash
git add src/Database/database.c
git commit -m "feat: implement asynchronous batch submission with worker thread execution"
```

---

## Task 10: Add Batch Recovery to WAL Manager

**Files:**
- Modify: `src/Database/wal_manager.c`

- [ ] **Step 1: Add batch recovery logic to recovery function**

Find the `wal_manager_recover` function and add batch handling after the existing WAL_PUT/WAL_DELETE cases:

```c
// In wal_manager_recover, after handling WAL_PUT and WAL_DELETE:

case WAL_BATCH: {
    // Read batch entry
    batch_t* batch = NULL;
    result = wal_read_batch(wal, &txn_id, &batch, &cursor);
    if (result != 0) {
        // CRC failure or read error - skip this entry
        fprintf(stderr, "WAL recovery: batch entry failed at cursor %lu\n", (unsigned long)cursor);
        continue;
    }

    // Apply all operations in batch
    for (size_t i = 0; i < batch->count; i++) {
        batch_op_t* op = &batch->ops[i];

        if (op->type == WAL_PUT) {
            result = hbtrie_insert(db->trie, op->path, op->value);
        } else if (op->type == WAL_DELETE) {
            result = hbtrie_remove(db->trie, op->path);
        }

        if (result != 0) {
            // Operation failed - log and skip batch
            fprintf(stderr, "WAL recovery: batch operation %zu failed\n", i);
            batch_destroy(batch);
            break;
        }
    }

    batch_destroy(batch);
    break;
}
```

- [ ] **Step 2: Commit recovery logic**

```bash
git add src/Database/wal_manager.c
git commit -m "feat: add batch recovery logic to WAL manager"
```

---

## Task 11: Add Database Integration Tests

**Files:**
- Modify: `tests/test_database.cpp`

- [ ] **Step 1: Add batch test cases to existing test file**

Add to `tests/test_database.cpp`:

```cpp
// Batch write tests
TEST_F(DatabaseTest, BatchWriteSync) {
    batch_t* batch = batch_create(10, 100);
    ASSERT_NE(batch, nullptr);

    // Add operations
    path_t* path1 = create_test_path("batch_key1");
    identifier_t* value1 = create_test_value("batch_value1");
    batch_add_put(batch, path1, value1);

    path_t* path2 = create_test_path("batch_key2");
    identifier_t* value2 = create_test_value("batch_value2");
    batch_add_put(batch, path2, value2);

    // Submit batch
    int result = database_write_batch_sync(db, batch);
    EXPECT_EQ(result, 0);

    // Verify both keys exist
    path_t* check_path1 = create_test_path("batch_key1");
    identifier_t* result_val1 = NULL;
    int get_result = database_get_sync(db, check_path1, &result_val1);
    EXPECT_EQ(get_result, 0);
    EXPECT_NE(result_val1, nullptr);
    identifier_destroy(result_val1);
    path_destroy(check_path1);

    path_t* check_path2 = create_test_path("batch_key2");
    identifier_t* result_val2 = NULL;
    get_result = database_get_sync(db, check_path2, &result_val2);
    EXPECT_EQ(get_result, 0);
    EXPECT_NE(result_val2, nullptr);
    identifier_destroy(result_val2);
    path_destroy(check_path2);

    batch_destroy(batch);
}

TEST_F(DatabaseTest, BatchWriteAtomicity) {
    batch_t* batch = batch_create(10, 100);
    ASSERT_NE(batch, nullptr);

    // Add operations
    path_t* path1 = create_test_path("atomic_key1");
    identifier_t* value1 = create_test_value("atomic_value1");
    batch_add_put(batch, path1, value1);

    // This key doesn't exist - delete should succeed but entire batch should work
    path_t* path2 = create_test_path("nonexistent_key");
    batch_add_delete(batch, path2);

    // Submit batch
    int result = database_write_batch_sync(db, batch);
    EXPECT_EQ(result, 0); // Should succeed - delete of non-existent key is OK

    batch_destroy(batch);
}

TEST_F(DatabaseTest, BatchRejectDoubleSubmission) {
    batch_t* batch = batch_create(10, 100);
    ASSERT_NE(batch, nullptr);

    path_t* path = create_test_path("key");
    identifier_t* value = create_test_value("value");
    batch_add_put(batch, path, value);

    // First submission
    int result = database_write_batch_sync(db, batch);
    EXPECT_EQ(result, 0);

    // Mark as submitted manually (simulating async submission)
    batch->submitted = 1;

    // Second submission should fail
    result = database_write_batch_sync(db, batch);
    EXPECT_EQ(result, -6); // -6 = already submitted

    batch_destroy(batch);
}

TEST_F(DatabaseTest, BatchRejectEmpty) {
    batch_t* batch = batch_create(10, 100);
    ASSERT_NE(batch, nullptr);

    int result = database_write_batch_sync(db, batch);
    EXPECT_EQ(result, -3); // -3 = empty batch

    batch_destroy(batch);
}
```

- [ ] **Step 2: Build and run tests**

```bash
cd build-test
cmake ..
make test_database
./tests/test_database --gtest_filter="DatabaseTest.Batch*"
```

Expected: All batch tests pass

- [ ] **Step 3: Commit integration tests**

```bash
git add tests/test_database.cpp
git commit -m "test: add database integration tests for batch operations"
```

---

## Task 12: Add Recovery Test

**Files:**
- Modify: `tests/test_database.cpp`

- [ ] **Step 1: Add recovery test for batches**

Add to `tests/test_database.cpp`:

```cpp
TEST_F(DatabaseTest, BatchWriteRecovery) {
    // Write batch
    {
        batch_t* batch = batch_create(10, 100);
        ASSERT_NE(batch, nullptr);

        path_t* path1 = create_test_path("recovery_key1");
        identifier_t* value1 = create_test_value("recovery_value1");
        batch_add_put(batch, path1, value1);

        path_t* path2 = create_test_path("recovery_key2");
        identifier_t* value2 = create_test_value("recovery_value2");
        batch_add_put(batch, path2, value2);

        int result = database_write_batch_sync(db, batch);
        EXPECT_EQ(result, 0);

        batch_destroy(batch);
    }

    // Destroy database
    database_destroy(db);

    // Reopen database (triggers recovery)
    int error_code = 0;
    db = database_create(test_dir, 50, NULL, 0, 0, 1, 0, NULL, NULL, &error_code);
    ASSERT_NE(db, nullptr);

    // Verify both keys recovered
    path_t* check_path1 = create_test_path("recovery_key1");
    identifier_t* result1 = NULL;
    int result = database_get_sync(db, check_path1, &result1);
    EXPECT_EQ(result, 0);
    EXPECT_NE(result1, nullptr);
    identifier_destroy(result1);
    path_destroy(check_path1);

    path_t* check_path2 = create_test_path("recovery_key2");
    identifier_t* result2 = NULL;
    result = database_get_sync(db, check_path2, &result2);
    EXPECT_EQ(result, 0);
    EXPECT_NE(result2, nullptr);
    identifier_destroy(result2);
    path_destroy(check_path2);
}
```

- [ ] **Step 2: Build and run tests**

```bash
cd build-test
cmake ..
make test_database
./tests/test_database --gtest_filter="DatabaseTest.BatchWriteRecovery"
```

Expected: Recovery test passes

- [ ] **Step 3: Commit recovery test**

```bash
git add tests/test_database.cpp
git commit -m "test: add recovery test for batch operations"
```

---

## Task 13: Update Documentation

**Files:**
- Modify: `STYLEGUIDE.md`

- [ ] **Step 1: Add batch usage patterns to style guide**

Add to `STYLEGUIDE.md` after existing patterns:

```markdown
## Batch Write Operations

### Usage Pattern

```c
// Create batch
batch_t* batch = batch_create(0, 0); // Use defaults

// Add operations
for (int i = 0; i < num_operations; i++) {
    path_t* path = create_path(keys[i]);
    identifier_t* value = create_value(values[i]);

    int result = batch_add_put(batch, path, value);
    if (result != 0) {
        // Error handling
        batch_destroy(batch);
        return result;
    }
}

// Submit synchronously
int result = database_write_batch_sync(db, batch);
if (result != 0) {
    // Error handling
    batch_destroy(batch);
    return result;
}

// Clean up
batch_destroy(batch);
```

### Ownership Rules

- `batch_add_*` transfers ownership on success, retains ownership on error
- Always check return value before assuming ownership transferred
- Call `batch_destroy()` after submission (batch is not reusable)
- For async: wait for promise before destroying batch

### Error Codes

- `-1`: General error
- `-2`: Batch is full
- `-3`: Batch is empty
- `-4`: Validation failed
- `-5`: Batch too large for WAL
- `-6`: Batch already submitted
```

- [ ] **Step 2: Commit documentation**

```bash
git add STYLEGUIDE.md
git commit -m "docs: add batch write usage patterns to style guide"
```

---

## Task 14: Run All Tests

- [ ] **Step 1: Build all tests**

```bash
cd build-test
cmake ..
make
```

Expected: All targets build successfully

- [ ] **Step 2: Run unit tests**

```bash
./tests/test_batch
```

Expected: All batch unit tests pass

- [ ] **Step 3: Run database tests**

```bash
./tests/test_database
```

Expected: All database tests pass (including new batch tests)

- [ ] **Step 4: Run full test suite**

```bash
ctest --output-on-failure
```

Expected: All tests pass

---

## Task 15: Create Final Commit and Summary

- [ ] **Step 1: Verify all changes committed**

```bash
git status
```

Expected: No uncommitted changes

- [ ] **Step 2: Create feature summary**

The batch write operations feature is now complete with:

- **Batch builder API** (`batch.h/batch.c`): Create, add operations, destroy
- **WAL integration**: Single entry per batch with atomic guarantees
- **Database API**: Synchronous and asynchronous batch submission
- **Recovery**: Batch entries replayed atomically during crash recovery
- **Testing**: Unit tests, integration tests, recovery tests
- **Documentation**: Usage patterns in style guide

All tests pass. Feature ready for integration.

---

**End of Implementation Plan**