# Raw Binding API — Phase 1: C Core Functions

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add C API functions that accept raw byte buffers instead of pre-constructed `path_t*`/`identifier_t*`, eliminating the per-operation allocation cascade in the Node.js and Dart bindings.

**Architecture:** New `identifier_create_from_raw` and `path_create_from_raw` skip the transient `buffer_t` intermediate. New `database_*_sync_raw` and `database_*_raw` (async) functions accept raw key/value bytes and delimiter, constructing the path/identifier hierarchy internally. New `database_batch_sync_raw`/`database_batch_raw` and `database_scan_sync_raw` handle bulk operations.

**Tech Stack:** C (core), GoogleTest for testing

**Spec:** `docs/superpowers/specs/2026-04-18-raw-binding-api-design.md`

---

## Files

### Create
- `tests/test_raw_api.cpp` — Unit tests for all raw API functions

### Modify
- `src/HBTrie/identifier.h` — Add `identifier_create_from_raw`, `identifier_get_data_copy`
- `src/HBTrie/identifier.c` — Implement new functions
- `src/HBTrie/path.h` — Add `path_create_from_raw`
- `src/HBTrie/path.c` — Implement new function
- `src/Database/database.h` — Add `raw_op_t`, `raw_result_t`, all raw function declarations
- `src/Database/database.c` — Implement sync raw, async raw, batch raw, scan raw functions
- `CMakeLists.txt` — Add `test_raw_api` test target

---

## Task 1: identifier_create_from_raw

**Files:**
- Modify: `src/HBTrie/identifier.h`
- Modify: `src/HBTrie/identifier.c`
- Test: `tests/test_raw_api.cpp`

- [ ] **Step 1: Write the failing test**

Add to `tests/test_raw_api.cpp`:

```cpp
#include <gtest/gtest.h>
extern "C" {
#include "HBTrie/identifier.h"
#include "HBTrie/chunk.h"
}

TEST(RawIdentifierTest, CreateFromRawBasic) {
    const uint8_t data[] = "hello";
    identifier_t* id = identifier_create_from_raw(data, 5, 0);
    ASSERT_NE(id, nullptr);
    EXPECT_EQ(id->length, 5u);
    // With chunk_size=4, "hello" (5 bytes) → 2 chunks
    EXPECT_EQ(identifier_chunk_count(id), 2u);

    // Verify data via identifier_get_data_copy
    size_t len;
    uint8_t* out = identifier_get_data_copy(id, &len);
    ASSERT_NE(out, nullptr);
    EXPECT_EQ(len, 5u);
    EXPECT_EQ(memcmp(out, "hello", 5), 0);
    free(out);

    identifier_destroy(id);
}

TEST(RawIdentifierTest, CreateFromRawEmpty) {
    identifier_t* id = identifier_create_from_raw(NULL, 0, 0);
    ASSERT_NE(id, nullptr);
    EXPECT_EQ(id->length, 0u);
    EXPECT_EQ(identifier_chunk_count(id), 0u);
    identifier_destroy(id);
}

TEST(RawIdentifierTest, CreateFromRawSingleChunk) {
    const uint8_t data[] = "abc";
    identifier_t* id = identifier_create_from_raw(data, 3, 4);
    ASSERT_NE(id, nullptr);
    EXPECT_EQ(id->length, 3u);
    EXPECT_EQ(identifier_chunk_count(id), 1u);

    size_t len;
    uint8_t* out = identifier_get_data_copy(id, &len);
    EXPECT_EQ(len, 3u);
    EXPECT_EQ(memcmp(out, "abc", 3), 0);
    free(out);

    identifier_destroy(id);
}

TEST(RawIdentifierTest, CreateFromRawMatchesIdentifierCreate) {
    // Same data through both paths should produce identical results
    const uint8_t data[] = "test_value_1234";
    size_t data_len = 15;

    // Old path: buffer_t → identifier_create
    buffer_t* buf = buffer_create_from_pointer_copy((uint8_t*)data, data_len);
    identifier_t* id_old = identifier_create(buf, 0);
    buffer_destroy(buf);

    // New path: identifier_create_from_raw
    identifier_t* id_new = identifier_create_from_raw(data, data_len, 0);

    ASSERT_NE(id_old, nullptr);
    ASSERT_NE(id_new, nullptr);
    EXPECT_EQ(id_old->length, id_new->length);
    EXPECT_EQ(identifier_chunk_count(id_old), identifier_chunk_count(id_new));
    EXPECT_EQ(identifier_compare(id_old, id_new), 0);

    identifier_destroy(id_old);
    identifier_destroy(id_new);
}

TEST(RawIdentifierTest, GetDataCopyMultiChunk) {
    // 10 bytes with chunk_size=4 → 3 chunks (4+4+2)
    const uint8_t data[] = "0123456789";
    identifier_t* id = identifier_create_from_raw(data, 10, 4);
    ASSERT_NE(id, nullptr);

    size_t len;
    uint8_t* out = identifier_get_data_copy(id, &len);
    EXPECT_EQ(len, 10u);
    EXPECT_EQ(memcmp(out, "0123456789", 10), 0);
    free(out);

    identifier_destroy(id);
}
```

- [ ] **Step 2: Add test target to CMakeLists.txt**

In the `BUILD_TESTS` block of `CMakeLists.txt`, add:

```cmake
add_executable(test_raw_api tests/test_raw_api.cpp)
target_link_libraries(test_raw_api wavedb gtest gtest_main)
add_test(NAME test_raw_api COMMAND test_raw_api)
```

- [ ] **Step 3: Build and verify tests fail**

Run: `cmake --build build --target test_raw_api 2>&1 | tail -5`
Expected: Compilation error — `identifier_create_from_raw` and `identifier_get_data_copy` not declared.

- [ ] **Step 4: Add declarations to identifier.h**

Add after the existing `identifier_create` declaration:

```c
/**
 * Create identifier directly from raw bytes, skipping buffer_t.
 * Eliminates 2 transient allocations (buffer_t struct + buffer data).
 *
 * @param data       Raw byte data (copied internally)
 * @param len        Length of data
 * @param chunk_size Chunk size (0 = DEFAULT_CHUNK_SIZE)
 * @return New identifier, or NULL on error
 */
identifier_t* identifier_create_from_raw(const uint8_t* data, size_t len, size_t chunk_size);

/**
 * Get raw byte data from identifier as contiguous buffer.
 * Caller MUST free the returned pointer with free().
 * Always returns a new allocation for consistent ownership.
 *
 * @param id       Identifier to read
 * @param out_len  Output: length of data
 * @return Malloc'd buffer containing identifier data, or NULL on error
 */
uint8_t* identifier_get_data_copy(const identifier_t* id, size_t* out_len);
```

- [ ] **Step 5: Implement identifier_create_from_raw**

In `identifier.c`, add:

```c
identifier_t* identifier_create_from_raw(const uint8_t* data, size_t len, size_t chunk_size) {
    if (chunk_size == 0) chunk_size = DEFAULT_CHUNK_SIZE;

    identifier_t* id = memory_pool_alloc(sizeof(identifier_t));
    if (!id) {
        id = get_clear_memory(sizeof(identifier_t));
        if (!id) return NULL;
    } else {
        memset(id, 0, sizeof(identifier_t));
    }

    vec_init(&id->chunks);
    id->length = len;
    id->chunk_size = chunk_size;

    if (len == 0 || data == NULL) {
        refcounter_init((refcounter_t*)id);
        return id;
    }

    size_t nchunk = identifier_calc_nchunk(len, chunk_size);
    vec_reserve(&id->chunks, nchunk);

    for (size_t i = 0; i < nchunk; i++) {
        size_t offset = i * chunk_size;
        size_t remaining = len - offset;
        size_t this_chunk_size = remaining < chunk_size ? remaining : chunk_size;

        chunk_t* chunk = chunk_create_empty(chunk_size);
        if (!chunk) {
            vec_foreach(&id->chunks, chunk_t*, ch) {
                chunk_destroy(ch);
            }
            vec_deinit(&id->chunks);
            memory_pool_free(id, sizeof(identifier_t));
            return NULL;
        }
        memcpy(chunk->data, data + offset, this_chunk_size);
        chunk->size = this_chunk_size;
        vec_push(&id->chunks, chunk);
    }

    refcounter_init((refcounter_t*)id);
    return id;
}
```

- [ ] **Step 6: Implement identifier_get_data_copy**

In `identifier.c`, add:

```c
uint8_t* identifier_get_data_copy(const identifier_t* id, size_t* out_len) {
    if (!id || !out_len) return NULL;

    *out_len = id->length;
    if (id->length == 0) {
        return malloc(1); // Return non-NULL for 0-length
    }

    uint8_t* result = malloc(id->length);
    if (!result) return NULL;

    size_t pos = 0;
    for (size_t i = 0; i < (size_t)id->chunks.length; i++) {
        chunk_t* chunk = id->chunks.data[i];
        size_t copy_len = chunk->size;
        if (pos + copy_len > id->length) {
            copy_len = id->length - pos;
        }
        memcpy(result + pos, chunk_data_const(chunk), copy_len);
        pos += copy_len;
    }

    return result;
}
```

- [ ] **Step 7: Build and run tests**

Run: `cmake --build build --target test_raw_api && ./build/test_raw_api`
Expected: All `RawIdentifierTest` tests PASS.

- [ ] **Step 8: Commit**

```
feat: add identifier_create_from_raw and identifier_get_data_copy
```

---

## Task 2: path_create_from_raw

**Files:**
- Modify: `src/HBTrie/path.h`
- Modify: `src/HBTrie/path.c`
- Modify: `tests/test_raw_api.cpp`

- [ ] **Step 1: Write the failing test**

Add to `tests/test_raw_api.cpp`:

```cpp
extern "C" {
#include "HBTrie/path.h"
}

TEST(RawPathTest, CreateFromRawBasic) {
    path_t* path = path_create_from_raw("users/alice/name", 16, '/', 0);
    ASSERT_NE(path, nullptr);
    EXPECT_EQ(path_length(path), 3u);
    path_destroy(path);
}

TEST(RawPathTest, CreateFromRawSingleSegment) {
    path_t* path = path_create_from_raw("simplekey", 9, '/', 0);
    ASSERT_NE(path, nullptr);
    EXPECT_EQ(path_length(path), 1u);
    path_destroy(path);
}

TEST(RawPathTest, CreateFromRawEmptySegments) {
    // Consecutive delimiters produce empty segments (skipped)
    path_t* path = path_create_from_raw("a//b", 4, '/', 0);
    ASSERT_NE(path, nullptr);
    EXPECT_EQ(path_length(path), 2u); // "a" and "b"
    path_destroy(path);
}

TEST(RawPathTest, CreateFromRawTrailingDelimiter) {
    path_t* path = path_create_from_raw("users/alice/", 12, '/', 0);
    ASSERT_NE(path, nullptr);
    EXPECT_EQ(path_length(path), 2u); // "users" and "alice"
    path_destroy(path);
}

TEST(RawPathTest, CreateFromRawNull) {
    path_t* path = path_create_from_raw(NULL, 0, '/', 0);
    ASSERT_NE(path, nullptr);
    EXPECT_EQ(path_length(path), 0u);
    path_destroy(path);
}

TEST(RawPathTest, CreateFromRawRoundTrip) {
    path_t* path = path_create_from_raw("users/alice/name", 16, '/', 0);
    ASSERT_NE(path, nullptr);

    // Verify each segment's data via identifier_get_data_copy
    size_t len;
    identifier_t* id0 = path_get(path, 0);
    uint8_t* seg0 = identifier_get_data_copy(id0, &len);
    EXPECT_EQ(len, 5u);
    EXPECT_EQ(memcmp(seg0, "users", 5), 0);
    free(seg0);

    identifier_t* id1 = path_get(path, 1);
    uint8_t* seg1 = identifier_get_data_copy(id1, &len);
    EXPECT_EQ(len, 5u);
    EXPECT_EQ(memcmp(seg1, "alice", 5), 0);
    free(seg1);

    identifier_t* id2 = path_get(path, 2);
    uint8_t* seg2 = identifier_get_data_copy(id2, &len);
    EXPECT_EQ(len, 4u);
    EXPECT_EQ(memcmp(seg2, "name", 4), 0);
    free(seg2);

    path_destroy(path);
}
```

- [ ] **Step 2: Build and verify tests fail**

Run: `cmake --build build --target test_raw_api 2>&1 | tail -5`
Expected: Compilation error — `path_create_from_raw` not declared.

- [ ] **Step 3: Add declaration to path.h**

Add after the existing `path_create_from_identifier` declaration:

```c
/**
 * Create path by parsing a raw key string with delimiter.
 * Uses identifier_create_from_raw internally (no buffer_t overhead).
 *
 * @param key        Key string (e.g. "users/alice/name")
 * @param key_len    Length of key string
 * @param delimiter  Path segment delimiter
 * @param chunk_size Chunk size for identifiers (0 = DEFAULT_CHUNK_SIZE)
 * @return New path, or NULL on error
 */
path_t* path_create_from_raw(const char* key, size_t key_len, char delimiter, size_t chunk_size);
```

- [ ] **Step 4: Implement path_create_from_raw**

In `path.c`, add:

```c
path_t* path_create_from_raw(const char* key, size_t key_len, char delimiter, size_t chunk_size) {
    path_t* path = path_create();
    if (!path) return NULL;

    if (chunk_size == 0) chunk_size = DEFAULT_CHUNK_SIZE;

    if (!key || key_len == 0) return path;

    size_t start = 0;
    for (size_t i = 0; i <= key_len; i++) {
        if (i == key_len || key[i] == delimiter) {
            size_t seg_len = i - start;
            if (seg_len == 0) {
                start = i + 1;
                continue;
            }

            identifier_t* id = identifier_create_from_raw(
                (const uint8_t*)(key + start), seg_len, chunk_size);
            if (!id) {
                path_destroy(path);
                return NULL;
            }

            path_append(path, id);
            identifier_destroy(id);
            start = i + 1;
        }
    }

    return path;
}
```

- [ ] **Step 5: Build and run tests**

Run: `cmake --build build --target test_raw_api && ./build/test_raw_api`
Expected: All `RawPathTest` tests PASS, plus existing `RawIdentifierTest` still PASS.

- [ ] **Step 6: Commit**

```
feat: add path_create_from_raw for delimiter-parsed key construction
```

---

## Task 3: Sync Raw Single-Key Functions

**Files:**
- Modify: `src/Database/database.h`
- Modify: `src/Database/database.c`
- Modify: `tests/test_raw_api.cpp`

- [ ] **Step 1: Write the failing tests**

Add to `tests/test_raw_api.cpp` (add the necessary extern "C" includes at top if not already present):

```cpp
extern "C" {
#include "Database/database.h"
}

class RawSyncTest : public ::testing::Test {
protected:
    database_t* db;
    char test_dir[256];

    void SetUp() override {
        snprintf(test_dir, sizeof(test_dir), "/tmp/wavedb_raw_test_%d", getpid());
        database_config_t* config = database_config_default();
        config->enable_persist = 0;
        db = database_create_with_config(test_dir, config, NULL);
        database_config_destroy(config);
        ASSERT_NE(db, nullptr);
    }

    void TearDown() override {
        if (db) database_destroy(db);
        rmdir(test_dir);
    }
};

TEST_F(RawSyncTest, PutAndGetSync) {
    int rc = database_put_sync_raw(db, "users/alice", 11, '/', (const uint8_t*)"hello", 5);
    EXPECT_EQ(rc, 0);

    uint8_t* value = NULL;
    size_t value_len = 0;
    rc = database_get_sync_raw(db, "users/alice", 11, '/', &value, &value_len);
    EXPECT_EQ(rc, 0);
    ASSERT_NE(value, nullptr);
    EXPECT_EQ(value_len, 5u);
    EXPECT_EQ(memcmp(value, "hello", 5), 0);
    database_raw_value_free(value);
}

TEST_F(RawSyncTest, GetNotFound) {
    uint8_t* value = NULL;
    size_t value_len = 0;
    int rc = database_get_sync_raw(db, "nonexistent", 11, '/', &value, &value_len);
    EXPECT_EQ(rc, -2);
    EXPECT_EQ(value, nullptr);
}

TEST_F(RawSyncTest, DeleteSync) {
    database_put_sync_raw(db, "users/alice", 11, '/', (const uint8_t*)"hello", 5);

    int rc = database_delete_sync_raw(db, "users/alice", 11, '/');
    EXPECT_EQ(rc, 0);

    uint8_t* value = NULL;
    size_t value_len = 0;
    rc = database_get_sync_raw(db, "users/alice", 11, '/', &value, &value_len);
    EXPECT_EQ(rc, -2);
}

TEST_F(RawSyncTest, PutNullKey) {
    int rc = database_put_sync_raw(db, NULL, 0, '/', (const uint8_t*)"val", 3);
    EXPECT_EQ(rc, -1);
}

TEST_F(RawSyncTest, PutAndGetVariousLengths) {
    // Short key, long value
    int rc = database_put_sync_raw(db, "k", 1, '/', (const uint8_t*)"long_value_here", 15);
    EXPECT_EQ(rc, 0);

    uint8_t* value = NULL;
    size_t value_len = 0;
    rc = database_get_sync_raw(db, "k", 1, '/', &value, &value_len);
    EXPECT_EQ(rc, 0);
    EXPECT_EQ(value_len, 15u);
    EXPECT_EQ(memcmp(value, "long_value_here", 15), 0);
    database_raw_value_free(value);
}

TEST_F(RawSyncTest, RawMatchesOriginalAPI) {
    // Put via raw API, get via original API
    database_put_sync_raw(db, "users/bob", 9, '/', (const uint8_t*)"raw_val", 7);

    path_t* path = path_create_from_raw("users/bob", 9, '/', 0);
    identifier_t* result = NULL;
    int rc = database_get_sync(db, path, &result);
    EXPECT_EQ(rc, 0);
    ASSERT_NE(result, nullptr);
    size_t len;
    uint8_t* data = identifier_get_data_copy(result, &len);
    EXPECT_EQ(len, 7u);
    EXPECT_EQ(memcmp(data, "raw_val", 7), 0);
    free(data);
    identifier_destroy(result);

    // Put via original API, get via raw API
    path_t* path2 = path_create_from_raw("users/carol", 11, '/', 0);
    buffer_t* buf = buffer_create_from_pointer_copy((uint8_t*)"orig_val", 8);
    identifier_t* val = identifier_create(buf, 0);
    buffer_destroy(buf);
    database_put_sync(db, path2, val);

    uint8_t* value = NULL;
    size_t value_len = 0;
    rc = database_get_sync_raw(db, "users/carol", 11, '/', &value, &value_len);
    EXPECT_EQ(rc, 0);
    EXPECT_EQ(value_len, 8u);
    EXPECT_EQ(memcmp(value, "orig_val", 8), 0);
    database_raw_value_free(value);
}
```

Note: The test needs to link `Threads::Threads` — update CMakeLists.txt:

```cmake
add_executable(test_raw_api tests/test_raw_api.cpp)
target_link_libraries(test_raw_api wavedb gtest gtest_main Threads::Threads)
add_test(NAME test_raw_api COMMAND test_raw_api)
```

- [ ] **Step 2: Build and verify tests fail**

Run: `cmake --build build --target test_raw_api 2>&1 | tail -5`
Expected: Compilation error — `database_put_sync_raw`, `database_get_sync_raw`, `database_delete_sync_raw`, `database_raw_value_free` not declared.

- [ ] **Step 3: Add declarations to database.h**

Add the `raw_op_t` and `raw_result_t` structs and sync raw function declarations:

```c
/* --- Raw API types --- */

typedef struct {
    const char* key;
    size_t key_len;
    const uint8_t* value;
    size_t value_len;
    int type;   /* 0 = put, 1 = delete */
} raw_op_t;

typedef struct {
    char* key;
    size_t key_len;
    uint8_t* value;
    size_t value_len;
} raw_result_t;

/* --- Sync raw functions --- */

int database_put_sync_raw(database_t* db,
    const char* key, size_t key_len, char delimiter,
    const uint8_t* value, size_t value_len);

int database_get_sync_raw(database_t* db,
    const char* key, size_t key_len, char delimiter,
    uint8_t** value_out, size_t* value_len_out);

int database_delete_sync_raw(database_t* db,
    const char* key, size_t key_len, char delimiter);

void database_raw_value_free(uint8_t* value);
```

- [ ] **Step 4: Implement sync raw functions in database.c**

Add to `database.c` (include path.h if not already included):

```c
int database_put_sync_raw(database_t* db,
    const char* key, size_t key_len, char delimiter,
    const uint8_t* value, size_t value_len) {
    if (!db || !key || key_len == 0 || !value) return -1;

    path_t* path = path_create_from_raw(key, key_len, delimiter, db->trie->chunk_size);
    if (!path) return -1;

    identifier_t* id = identifier_create_from_raw(value, value_len, db->trie->chunk_size);
    if (!id) { path_destroy(path); return -1; }

    return database_put_sync(db, path, id);
}

int database_get_sync_raw(database_t* db,
    const char* key, size_t key_len, char delimiter,
    uint8_t** value_out, size_t* value_len_out) {
    if (!db || !key || key_len == 0 || !value_out || !value_len_out) return -1;

    path_t* path = path_create_from_raw(key, key_len, delimiter, db->trie->chunk_size);
    if (!path) return -1;

    identifier_t* result = NULL;
    int rc = database_get_sync(db, path, &result);

    if (rc == 0 && result) {
        *value_out = identifier_get_data_copy(result, value_len_out);
        identifier_destroy(result);
        if (!*value_out) return -1;
        return 0;
    }

    *value_out = NULL;
    *value_len_out = 0;
    return rc;
}

int database_delete_sync_raw(database_t* db,
    const char* key, size_t key_len, char delimiter) {
    if (!db || !key || key_len == 0) return -1;

    path_t* path = path_create_from_raw(key, key_len, delimiter, db->trie->chunk_size);
    if (!path) return -1;

    return database_delete_sync(db, path);
}

void database_raw_value_free(uint8_t* value) {
    free(value);
}
```

- [ ] **Step 5: Build and run tests**

Run: `cmake --build build --target test_raw_api && ./build/test_raw_api`
Expected: All `RawSyncTest` tests PASS, plus all earlier tests still PASS.

- [ ] **Step 6: Run full test suite**

Run: `cd build && ctest --output-on-failure`
Expected: All tests PASS (no regressions).

- [ ] **Step 7: Commit**

```
feat: add sync raw API (put/get/delete_sync_raw, raw_value_free)
```

---

## Task 4: Async Raw Single-Key Functions

**Files:**
- Modify: `src/Database/database.h`
- Modify: `src/Database/database.c`
- Modify: `tests/test_raw_api.cpp`

- [ ] **Step 1: Write the failing test**

Add to `tests/test_raw_api.cpp`:

```cpp
extern "C" {
#include "Workers/work.h"
#include "Workers/work_pool.h"
#include "Workers/promise.h"
#include "Util/error.h"
}

class RawAsyncTest : public ::testing::Test {
protected:
    database_t* db;
    char test_dir[256];

    void SetUp() override {
        snprintf(test_dir, sizeof(test_dir), "/tmp/wavedb_raw_async_%d", getpid());
        database_config_t* config = database_config_default();
        config->enable_persist = 0;
        db = database_create_with_config(test_dir, config, NULL);
        database_config_destroy(config);
        ASSERT_NE(db, nullptr);
    }

    void TearDown() override {
        if (db) database_destroy(db);
        rmdir(test_dir);
    }

    static void resolve_cb(void* ctx, void* payload) {
        identifier_t** out = static_cast<identifier_t**>(ctx);
        if (payload) {
            *out = REFERENCE((identifier_t*)payload, identifier_t);
        } else {
            *out = nullptr;
        }
    }

    static void reject_cb(void* ctx, async_error_t* error) {
        identifier_t** out = static_cast<identifier_t**>(ctx);
        *out = nullptr;
        error_destroy(error);
    }
};

TEST_F(RawAsyncTest, PutAndGetRaw) {
    identifier_t* result = nullptr;
    promise_t* promise = promise_create(
        (void (*)(void*, void*))resolve_cb,
        (void (*)(void*, async_error_t*))reject_cb,
        &result);
    ASSERT_NE(promise, nullptr);

    int rc = database_put_raw(db, "users/alice", 11, '/',
                              (const uint8_t*)"hello", 5, promise);
    EXPECT_EQ(rc, 0);

    // Wait for async completion
    usleep(50000);

    // Now get it
    promise_t* get_promise = promise_create(
        (void (*)(void*, void*))resolve_cb,
        (void (*)(void*, async_error_t*))reject_cb,
        &result);
    rc = database_get_raw(db, "users/alice", 11, '/', get_promise);
    EXPECT_EQ(rc, 0);

    usleep(50000);
    if (result) {
        size_t len;
        uint8_t* data = identifier_get_data_copy(result, &len);
        EXPECT_EQ(len, 5u);
        EXPECT_EQ(memcmp(data, "hello", 5), 0);
        free(data);
        identifier_destroy(result);
    }

    promise_destroy(get_promise);
}
```

- [ ] **Step 2: Build and verify tests fail**

Run: `cmake --build build --target test_raw_api 2>&1 | tail -5`
Expected: Compilation error — `database_put_raw`, `database_get_raw` not declared.

- [ ] **Step 3: Add declarations to database.h**

```c
/* --- Async raw functions --- */

int database_put_raw(database_t* db,
    const char* key, size_t key_len, char delimiter,
    const uint8_t* value, size_t value_len,
    promise_t* promise);

int database_get_raw(database_t* db,
    const char* key, size_t key_len, char delimiter,
    promise_t* promise);

int database_delete_raw(database_t* db,
    const char* key, size_t key_len, char delimiter,
    promise_t* promise);
```

- [ ] **Step 4: Implement async raw functions in database.c**

Add the raw async context struct and worker functions:

```c
typedef struct {
    database_t* db;
    char* key_buf;
    size_t key_len;
    char delimiter;
    uint8_t* value_buf;
    size_t value_len;
    promise_t* promise;
    int op_type;   /* 0=put, 1=get, 2=delete */
} raw_async_ctx_t;

static void _raw_put_worker(void* ctx_ptr) {
    raw_async_ctx_t* ctx = (raw_async_ctx_t*)ctx_ptr;
    path_t* path = path_create_from_raw(ctx->key_buf, ctx->key_len,
                                         ctx->delimiter,
                                         ctx->db->trie->chunk_size);
    if (!path) {
        async_error_t* err = error_create("Failed to create path from raw key");
        promise_reject(ctx->promise, err);
        free(ctx->key_buf);
        free(ctx->value_buf);
        free(ctx);
        return;
    }

    identifier_t* value = identifier_create_from_raw(
        ctx->value_buf, ctx->value_len, ctx->db->trie->chunk_size);
    if (!value) {
        path_destroy(path);
        async_error_t* err = error_create("Failed to create value from raw data");
        promise_reject(ctx->promise, err);
        free(ctx->key_buf);
        free(ctx->value_buf);
        free(ctx);
        return;
    }

    free(ctx->key_buf);
    free(ctx->value_buf);

    // Build a standard put context and delegate to existing worker
    database_put_ctx_t* put_ctx = get_clear_memory(sizeof(database_put_ctx_t));
    put_ctx->db = ctx->db;
    put_ctx->path = path;
    put_ctx->value = value;
    put_ctx->promise = ctx->promise;
    free(ctx);

    _database_put(put_ctx);
}

static void abort_raw_put(void* ctx_ptr) {
    raw_async_ctx_t* ctx = (raw_async_ctx_t*)ctx_ptr;
    free(ctx->key_buf);
    free(ctx->value_buf);
    free(ctx);
}

static void _raw_get_worker(void* ctx_ptr) {
    raw_async_ctx_t* ctx = (raw_async_ctx_t*)ctx_ptr;
    path_t* path = path_create_from_raw(ctx->key_buf, ctx->key_len,
                                         ctx->delimiter,
                                         ctx->db->trie->chunk_size);
    free(ctx->key_buf);

    if (!path) {
        async_error_t* err = error_create("Failed to create path from raw key");
        promise_reject(ctx->promise, err);
        free(ctx);
        return;
    }

    database_get_ctx_t* get_ctx = get_clear_memory(sizeof(database_get_ctx_t));
    get_ctx->db = ctx->db;
    get_ctx->path = path;
    get_ctx->promise = ctx->promise;
    free(ctx);

    _database_get(get_ctx);
}

static void abort_raw_get(void* ctx_ptr) {
    raw_async_ctx_t* ctx = (raw_async_ctx_t*)ctx_ptr;
    free(ctx->key_buf);
    free(ctx);
}

static void _raw_delete_worker(void* ctx_ptr) {
    raw_async_ctx_t* ctx = (raw_async_ctx_t*)ctx_ptr;
    path_t* path = path_create_from_raw(ctx->key_buf, ctx->key_len,
                                         ctx->delimiter,
                                         ctx->db->trie->chunk_size);
    free(ctx->key_buf);

    if (!path) {
        async_error_t* err = error_create("Failed to create path from raw key");
        promise_reject(ctx->promise, err);
        free(ctx);
        return;
    }

    database_delete_ctx_t* del_ctx = get_clear_memory(sizeof(database_delete_ctx_t));
    del_ctx->db = ctx->db;
    del_ctx->path = path;
    del_ctx->promise = ctx->promise;
    free(ctx);

    _database_delete(del_ctx);
}

static void abort_raw_delete(void* ctx_ptr) {
    raw_async_ctx_t* ctx = (raw_async_ctx_t*)ctx_ptr;
    free(ctx->key_buf);
    free(ctx);
}

int database_put_raw(database_t* db,
    const char* key, size_t key_len, char delimiter,
    const uint8_t* value, size_t value_len,
    promise_t* promise) {
    if (!db || !key || key_len == 0 || !value || !promise) {
        if (promise) promise_resolve(promise, NULL);
        return -1;
    }

    raw_async_ctx_t* ctx = calloc(1, sizeof(raw_async_ctx_t));
    if (!ctx) { promise_resolve(promise, NULL); return -1; }

    ctx->db = db;
    ctx->key_len = key_len;
    ctx->key_buf = malloc(key_len);
    if (!ctx->key_buf) { free(ctx); promise_resolve(promise, NULL); return -1; }
    memcpy(ctx->key_buf, key, key_len);

    ctx->delimiter = delimiter;
    ctx->value_len = value_len;
    ctx->value_buf = malloc(value_len);
    if (!ctx->value_buf) { free(ctx->key_buf); free(ctx); promise_resolve(promise, NULL); return -1; }
    memcpy(ctx->value_buf, value, value_len);

    ctx->promise = promise;
    ctx->op_type = 0;

    work_t* work = work_create(_raw_put_worker, abort_raw_put, ctx);
    if (!work) {
        free(ctx->value_buf);
        free(ctx->key_buf);
        free(ctx);
        promise_resolve(promise, NULL);
        return -1;
    }

    refcounter_yield((refcounter_t*)work);
    work_pool_enqueue(db->pool, work);
    return 0;
}

int database_get_raw(database_t* db,
    const char* key, size_t key_len, char delimiter,
    promise_t* promise) {
    if (!db || !key || key_len == 0 || !promise) {
        if (promise) promise_resolve(promise, NULL);
        return -1;
    }

    raw_async_ctx_t* ctx = calloc(1, sizeof(raw_async_ctx_t));
    if (!ctx) { promise_resolve(promise, NULL); return -1; }

    ctx->db = db;
    ctx->key_len = key_len;
    ctx->key_buf = malloc(key_len);
    if (!ctx->key_buf) { free(ctx); promise_resolve(promise, NULL); return -1; }
    memcpy(ctx->key_buf, key, key_len);

    ctx->delimiter = delimiter;
    ctx->promise = promise;
    ctx->op_type = 1;

    work_t* work = work_create(_raw_get_worker, abort_raw_get, ctx);
    if (!work) {
        free(ctx->key_buf);
        free(ctx);
        promise_resolve(promise, NULL);
        return -1;
    }

    refcounter_yield((refcounter_t*)work);
    work_pool_enqueue(db->pool, work);
    return 0;
}

int database_delete_raw(database_t* db,
    const char* key, size_t key_len, char delimiter,
    promise_t* promise) {
    if (!db || !key || key_len == 0 || !promise) {
        if (promise) promise_resolve(promise, NULL);
        return -1;
    }

    raw_async_ctx_t* ctx = calloc(1, sizeof(raw_async_ctx_t));
    if (!ctx) { promise_resolve(promise, NULL); return -1; }

    ctx->db = db;
    ctx->key_len = key_len;
    ctx->key_buf = malloc(key_len);
    if (!ctx->key_buf) { free(ctx); promise_resolve(promise, NULL); return -1; }
    memcpy(ctx->key_buf, key, key_len);

    ctx->delimiter = delimiter;
    ctx->promise = promise;
    ctx->op_type = 2;

    work_t* work = work_create(_raw_delete_worker, abort_raw_delete, ctx);
    if (!work) {
        free(ctx->key_buf);
        free(ctx);
        promise_resolve(promise, NULL);
        return -1;
    }

    refcounter_yield((refcounter_t*)work);
    work_pool_enqueue(db->pool, work);
    return 0;
}
```

- [ ] **Step 5: Build and run tests**

Run: `cmake --build build --target test_raw_api && ./build/test_raw_api`
Expected: `RawAsyncTest` tests PASS, all earlier tests still PASS.

- [ ] **Step 6: Run full test suite**

Run: `cd build && ctest --output-on-failure`
Expected: All tests PASS.

- [ ] **Step 7: Commit**

```
feat: add async raw API (put/get/delete_raw)
```

---

## Task 5: Batch Raw Functions

**Files:**
- Modify: `src/Database/database.h`
- Modify: `src/Database/database.c`
- Modify: `tests/test_raw_api.cpp`

- [ ] **Step 1: Write the failing test**

Add to `tests/test_raw_api.cpp`:

```cpp
TEST_F(RawSyncTest, BatchSyncRaw) {
    raw_op_t ops[3];
    ops[0].key = "users/alice";
    ops[0].key_len = 11;
    ops[0].value = (const uint8_t*)"alice_val";
    ops[0].value_len = 9;
    ops[0].type = 0;

    ops[1].key = "users/bob";
    ops[1].key_len = 9;
    ops[1].value = (const uint8_t*)"bob_val";
    ops[1].value_len = 7;
    ops[1].type = 0;

    ops[2].key = "users/carol";
    ops[2].key_len = 11;
    ops[2].value = (const uint8_t*)"carol_val";
    ops[2].value_len = 9;
    ops[2].type = 0;

    int rc = database_batch_sync_raw(db, '/', ops, 3);
    EXPECT_EQ(rc, 0);

    // Verify all three entries
    uint8_t* val = NULL;
    size_t vlen = 0;

    rc = database_get_sync_raw(db, "users/alice", 11, '/', &val, &vlen);
    EXPECT_EQ(rc, 0);
    EXPECT_EQ(vlen, 9u);
    EXPECT_EQ(memcmp(val, "alice_val", 9), 0);
    database_raw_value_free(val);

    rc = database_get_sync_raw(db, "users/bob", 9, '/', &val, &vlen);
    EXPECT_EQ(rc, 0);
    EXPECT_EQ(vlen, 7u);
    database_raw_value_free(val);

    rc = database_get_sync_raw(db, "users/carol", 11, '/', &val, &vlen);
    EXPECT_EQ(rc, 0);
    EXPECT_EQ(vlen, 9u);
    database_raw_value_free(val);
}

TEST_F(RawSyncTest, BatchSyncRawWithDelete) {
    // Put, then delete via batch
    database_put_sync_raw(db, "users/alice", 11, '/', (const uint8_t*)"val", 3);

    raw_op_t ops[1];
    ops[0].key = "users/alice";
    ops[0].key_len = 11;
    ops[0].value = NULL;
    ops[0].value_len = 0;
    ops[0].type = 1;  // delete

    int rc = database_batch_sync_raw(db, '/', ops, 1);
    EXPECT_EQ(rc, 0);

    uint8_t* val = NULL;
    size_t vlen = 0;
    rc = database_get_sync_raw(db, "users/alice", 11, '/', &val, &vlen);
    EXPECT_EQ(rc, -2);
}
```

- [ ] **Step 2: Build and verify tests fail**

Expected: `database_batch_sync_raw` not declared.

- [ ] **Step 3: Add declaration to database.h**

```c
/* --- Batch raw functions --- */

int database_batch_sync_raw(database_t* db, char delimiter,
    const raw_op_t* ops, size_t count);

int database_batch_raw(database_t* db, char delimiter,
    const raw_op_t* ops, size_t count,
    promise_t* promise);
```

- [ ] **Step 4: Implement batch raw functions in database.c**

```c
int database_batch_sync_raw(database_t* db, char delimiter,
    const raw_op_t* ops, size_t count) {
    if (!db || !ops || count == 0) return -1;

    batch_t* batch = batch_create(count);
    if (!batch) return -1;

    for (size_t i = 0; i < count; i++) {
        path_t* path = path_create_from_raw(ops[i].key, ops[i].key_len,
                                             delimiter,
                                             db->trie->chunk_size);
        if (!path) { batch_destroy(batch); return -1; }

        if (ops[i].type == 0) {
            if (!ops[i].value) {
                path_destroy(path);
                batch_destroy(batch);
                return -1;
            }
            identifier_t* value = identifier_create_from_raw(
                ops[i].value, ops[i].value_len,
                db->trie->chunk_size);
            if (!value) { path_destroy(path); batch_destroy(batch); return -1; }

            int rc = batch_add_put(batch, path, value);
            if (rc != 0) {
                path_destroy(path);
                identifier_destroy(value);
                batch_destroy(batch);
                return -1;
            }
        } else {
            int rc = batch_add_delete(batch, path);
            if (rc != 0) {
                path_destroy(path);
                batch_destroy(batch);
                return -1;
            }
        }
    }

    return database_write_batch_sync(db, batch);
}

int database_batch_raw(database_t* db, char delimiter,
    const raw_op_t* ops, size_t count,
    promise_t* promise) {
    if (!db || !ops || count == 0 || !promise) return -1;

    batch_t* batch = batch_create(count);
    if (!batch) return -1;

    for (size_t i = 0; i < count; i++) {
        path_t* path = path_create_from_raw(ops[i].key, ops[i].key_len,
                                             delimiter,
                                             db->trie->chunk_size);
        if (!path) { batch_destroy(batch); return -1; }

        if (ops[i].type == 0) {
            if (!ops[i].value) {
                path_destroy(path);
                batch_destroy(batch);
                return -1;
            }
            identifier_t* value = identifier_create_from_raw(
                ops[i].value, ops[i].value_len,
                db->trie->chunk_size);
            if (!value) { path_destroy(path); batch_destroy(batch); return -1; }

            int rc = batch_add_put(batch, path, value);
            if (rc != 0) {
                path_destroy(path);
                identifier_destroy(value);
                batch_destroy(batch);
                return -1;
            }
        } else {
            int rc = batch_add_delete(batch, path);
            if (rc != 0) {
                path_destroy(path);
                batch_destroy(batch);
                return -1;
            }
        }
    }

    database_write_batch(db, batch, promise);
    return 0;
}
```

- [ ] **Step 5: Build and run tests**

Run: `cmake --build build --target test_raw_api && ./build/test_raw_api`
Expected: All batch tests PASS.

- [ ] **Step 6: Run full test suite**

Run: `cd build && ctest --output-on-failure`
Expected: All tests PASS.

- [ ] **Step 7: Commit**

```
feat: add batch raw API (database_batch_sync_raw, database_batch_raw)
```

---

## Task 6: Scan Raw Function

**Files:**
- Modify: `src/Database/database.h`
- Modify: `src/Database/database.c`
- Modify: `tests/test_raw_api.cpp`

- [ ] **Step 1: Write the failing test**

Add to `tests/test_raw_api.cpp`:

```cpp
TEST_F(RawSyncTest, ScanSyncRaw) {
    // Insert several entries under "users/"
    database_put_sync_raw(db, "users/alice", 11, '/', (const uint8_t*)"alice_val", 9);
    database_put_sync_raw(db, "users/bob", 9, '/', (const uint8_t*)"bob_val", 7);
    database_put_sync_raw(db, "users/carol", 11, '/', (const uint8_t*)"carol_val", 9);

    raw_result_t* results = NULL;
    size_t count = 0;
    int rc = database_scan_sync_raw(db, "users", 5, '/', &results, &count);
    EXPECT_EQ(rc, 0);
    EXPECT_GE(count, 3u);

    // Verify we can find our entries in the results
    bool found_alice = false, found_bob = false, found_carol = false;
    for (size_t i = 0; i < count; i++) {
        if (results[i].value_len == 9 && memcmp(results[i].value, "alice_val", 9) == 0) found_alice = true;
        if (results[i].value_len == 7 && memcmp(results[i].value, "bob_val", 7) == 0) found_bob = true;
        if (results[i].value_len == 9 && memcmp(results[i].value, "carol_val", 9) == 0) found_carol = true;
    }
    EXPECT_TRUE(found_alice);
    EXPECT_TRUE(found_bob);
    EXPECT_TRUE(found_carol);

    database_raw_results_free(results, count);
}

TEST_F(RawSyncTest, ScanSyncRawEmpty) {
    raw_result_t* results = NULL;
    size_t count = 0;
    int rc = database_scan_sync_raw(db, "nonexistent", 11, '/', &results, &count);
    EXPECT_EQ(rc, 0);
    EXPECT_EQ(count, 0u);
}
```

- [ ] **Step 2: Build and verify tests fail**

Expected: `database_scan_sync_raw` and `database_raw_results_free` not declared.

- [ ] **Step 3: Add declaration to database.h**

```c
/* --- Scan raw functions --- */

int database_scan_sync_raw(database_t* db,
    const char* prefix, size_t prefix_len, char delimiter,
    raw_result_t** results, size_t* count);

void database_raw_results_free(raw_result_t* results, size_t count);
```

- [ ] **Step 4: Implement scan raw function in database.c**

```c
int database_scan_sync_raw(database_t* db,
    const char* prefix, size_t prefix_len, char delimiter,
    raw_result_t** results, size_t* count) {
    if (!db || !results || !count) return -1;

    *results = NULL;
    *count = 0;

    path_t* start_path = NULL;
    if (prefix && prefix_len > 0) {
        start_path = path_create_from_raw(prefix, prefix_len, delimiter,
                                           db->trie->chunk_size);
        if (!start_path) return -1;
    }

    database_iterator_t* iter = database_scan_start(db, start_path, NULL);
    if (!iter) return 0;

    size_t capacity = 64;
    size_t n = 0;
    raw_result_t* out = malloc(capacity * sizeof(raw_result_t));
    if (!out) { database_scan_end(iter); return -1; }

    while (true) {
        path_t* out_path = NULL;
        identifier_t* out_value = NULL;
        int rc = database_scan_next(iter, &out_path, &out_value);
        if (rc != 0) break;

        if (n >= capacity) {
            capacity *= 2;
            raw_result_t* new_out = realloc(out, capacity * sizeof(raw_result_t));
            if (!new_out) {
                for (size_t j = 0; j < n; j++) {
                    free(out[j].key);
                    free(out[j].value);
                }
                free(out);
                path_destroy(out_path);
                identifier_destroy(out_value);
                database_scan_end(iter);
                return -1;
            }
            out = new_out;
        }

        // Assemble key string from path identifiers
        size_t key_total = 0;
        for (size_t i = 0; i < (size_t)out_path->identifiers.length; i++) {
            identifier_t* id = out_path->identifiers.data[i];
            key_total += id->length;
            if (i > 0) key_total++;
        }

        out[n].key = malloc(key_total + 1);
        if (!out[n].key) {
            for (size_t j = 0; j < n; j++) {
                free(out[j].key);
                free(out[j].value);
            }
            free(out);
            path_destroy(out_path);
            identifier_destroy(out_value);
            database_scan_end(iter);
            return -1;
        }

        size_t pos = 0;
        for (size_t i = 0; i < (size_t)out_path->identifiers.length; i++) {
            identifier_t* id = out_path->identifiers.data[i];
            if (i > 0) out[n].key[pos++] = delimiter;
            // Copy raw bytes from each identifier's chunks
            for (size_t j = 0; j < (size_t)id->chunks.length; j++) {
                chunk_t* chunk = id->chunks.data[j];
                size_t copy_len = chunk->size;
                if (pos + copy_len > key_total) copy_len = key_total - pos;
                memcpy(out[n].key + pos, chunk_data_const(chunk), copy_len);
                pos += copy_len;
            }
        }
        out[n].key[pos] = '\0';
        out[n].key_len = pos;

        // Copy value
        out[n].value = identifier_get_data_copy(out_value, &out[n].value_len);

        path_destroy(out_path);
        identifier_destroy(out_value);
        n++;
    }

    database_scan_end(iter);
    *results = out;
    *count = n;
    return 0;
}

void database_raw_results_free(raw_result_t* results, size_t count) {
    if (!results) return;
    for (size_t i = 0; i < count; i++) {
        free(results[i].key);
        free(results[i].value);
    }
    free(results);
}
```

- [ ] **Step 5: Build and run tests**

Run: `cmake --build build --target test_raw_api && ./build/test_raw_api`
Expected: All scan tests PASS.

- [ ] **Step 6: Run full test suite**

Run: `cd build && ctest --output-on-failure`
Expected: All tests PASS.

- [ ] **Step 7: Commit**

```
feat: add scan raw API (database_scan_sync_raw, database_raw_results_free)
```

---

## Task 7: C Benchmark

**Files:**
- Create: `tests/benchmark/benchmark_database_sync_raw.cpp`

- [ ] **Step 1: Create raw API benchmark**

Create `tests/benchmark/benchmark_database_sync_raw.cpp` modeled on `benchmark_database_sync.cpp`, testing `database_put_sync_raw`, `database_get_sync_raw`, `database_delete_sync_raw` with the same methodology (10K iterations, p50/p95/p99 latencies, JSON output).

Add to CMakeLists.txt as a benchmark target (not a test target).

- [ ] **Step 2: Run benchmark and compare**

Run the raw API benchmark and compare results against the existing `benchmark_database_sync` baseline. Expected: raw API throughput within 10-20% of C baseline for put/delete, near parity for get.

- [ ] **Step 3: Commit**

```
test: add benchmark for raw sync API
```

---

## Task 8: Run Valgrind and Full Validation

**Files:**
- No new files

- [ ] **Step 1: Run all tests under valgrind**

Run: `cd build && valgrind --leak-check=full ./test_raw_api 2>&1 | grep -E "(ERROR SUMMARY|definitely lost|indirectly lost)"`
Expected: 0 bytes lost, 0 errors.

- [ ] **Step 2: Run full test suite**

Run: `cd build && ctest --output-on-failure`
Expected: All tests PASS.

- [ ] **Step 3: Commit any fixes if needed**

If valgrind found leaks, fix them and commit:
```
fix: resolve memory leaks in raw API paths
```

---

## Summary

Phase 1 delivers the complete C core raw API:

| Function | Type | Status |
|----------|------|--------|
| `identifier_create_from_raw` | Internal | Task 1 |
| `identifier_get_data_copy` | Internal | Task 1 |
| `path_create_from_raw` | Internal | Task 2 |
| `database_put_sync_raw` | Sync single-key | Task 3 |
| `database_get_sync_raw` | Sync single-key | Task 3 |
| `database_delete_sync_raw` | Sync single-key | Task 3 |
| `database_raw_value_free` | Utility | Task 3 |
| `database_put_raw` | Async single-key | Task 4 |
| `database_get_raw` | Async single-key | Task 4 |
| `database_delete_raw` | Async single-key | Task 4 |
| `database_batch_sync_raw` | Sync batch | Task 5 |
| `database_batch_raw` | Async batch | Task 5 |
| `database_scan_sync_raw` | Sync scan | Task 6 |
| `database_raw_results_free` | Utility | Task 6 |

Phase 2 (Node.js binding) and Phase 3 (Dart binding) will be written as separate plan files once Phase 1 is complete and validated.