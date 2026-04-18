#include <gtest/gtest.h>
extern "C" {
#include "HBTrie/identifier.h"
#include "HBTrie/chunk.h"
#include "HBTrie/path.h"
#include "Buffer/buffer.h"
#include "Database/database.h"
#include "Workers/work.h"
#include "Workers/pool.h"
#include "Workers/promise.h"
#include "Workers/error.h"
}

TEST(RawIdentifierTest, CreateFromRawBasic) {
    const uint8_t data[] = "hello";
    identifier_t* id = identifier_create_from_raw(data, 5, 0);
    ASSERT_NE(id, nullptr);
    EXPECT_EQ(id->length, 5u);
    EXPECT_EQ(identifier_chunk_count(id), 2u);

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
    const uint8_t data[] = "test_value_1234";
    size_t data_len = 15;
    buffer_t* buf = buffer_create_from_pointer_copy((uint8_t*)data, data_len);
    identifier_t* id_old = identifier_create(buf, 0);
    buffer_destroy(buf);
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

// --- Database-level raw sync API tests ---

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

// --- Async raw API tests ---

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
    result = nullptr;
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
    promise_destroy(promise);
}

// --- Batch raw API tests ---

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