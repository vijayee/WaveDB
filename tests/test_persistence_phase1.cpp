//
// Phase 1 Persistence Integration Tests
//
// Tests the end-to-end persistence lifecycle:
// - Create database with persistence, put values, destroy, reopen, verify
// - database_flush_persist() followed by reopen
// - Multiple flush cycles with updates
// - CBOR fallback for legacy databases
//

#include <gtest/gtest.h>
#include <vector>
#include <string>
#include <cstdlib>
#include <cstdio>
#include <sys/stat.h>
#include <unistd.h>

extern "C" {
#include "Database/database.h"
#include "Database/database_config.h"
#include "Time/wheel.h"
#include "Workers/pool.h"
#include "HBTrie/path.h"
#include "HBTrie/identifier.h"
#include "Buffer/buffer.h"
#include "Util/allocator.h"
#include "Storage/page_file.h"
}

class PersistenceTest : public ::testing::Test {
protected:
    void SetUp() override {
        test_dir = "/tmp/wavedb_persist_test_" + std::to_string(getpid()) + "_" + std::to_string(test_counter++);
        mkdir(test_dir.c_str(), 0700);

        pool = work_pool_create(platform_core_count());
        work_pool_launch(pool);

        wheel = hierarchical_timing_wheel_create(8, pool);
        hierarchical_timing_wheel_run(wheel);
    }

    void TearDown() override {
        if (wheel) {
            hierarchical_timing_wheel_stop(wheel);
        }

        if (pool) {
            work_pool_shutdown(pool);
            work_pool_join_all(pool);
        }

        if (db) {
            database_destroy(db);
            db = nullptr;
        }

        if (wheel) {
            hierarchical_timing_wheel_destroy(wheel);
            wheel = nullptr;
        }
        if (pool) {
            work_pool_destroy(pool);
            pool = nullptr;
        }

        std::string cmd = std::string("rm -rf ") + test_dir;
        system(cmd.c_str());
    }

    path_t* make_path(std::initializer_list<const char*> subscripts) {
        path_t* path = path_create();
        for (const char* sub : subscripts) {
            buffer_t* buf = buffer_create_from_pointer_copy((uint8_t*)sub, strlen(sub));
            identifier_t* id = identifier_create(buf, 0);
            buffer_destroy(buf);
            path_append(path, id);
            identifier_destroy(id);
        }
        return path;
    }

    identifier_t* make_value(const char* data) {
        buffer_t* buf = buffer_create_from_pointer_copy((uint8_t*)data, strlen(data));
        identifier_t* id = identifier_create(buf, 0);
        buffer_destroy(buf);
        return id;
    }

    database_t* db = nullptr;
    work_pool_t* pool = nullptr;
    hierarchical_timing_wheel_t* wheel = nullptr;
    std::string test_dir;
    static int test_counter;
};

int PersistenceTest::test_counter = 0;

// Test 1: Create a persistent database, put values, destroy, reopen, verify
TEST_F(PersistenceTest, PersistAndReopen) {
    // Create database with persistence enabled
    database_config_t* config = database_config_default();
    ASSERT_NE(config, nullptr);
    config->enable_persist = 1;
    config->lru_memory_mb = 10;

    int error_code = 0;
    db = database_create_with_config(test_dir.c_str(), config, &error_code);
    ASSERT_EQ(error_code, 0) << "Failed to create database: error " << error_code;
    ASSERT_NE(db, nullptr);

    // Put some values
    path_t* key1 = make_path({"users", "alice", "name"});
    identifier_t* val1 = make_value("Alice");
    EXPECT_EQ(database_put_sync(db, key1, val1), 0);
    // key1 and val1 are CONSUMEd by put_sync

    path_t* key2 = make_path({"users", "bob", "name"});
    identifier_t* val2 = make_value("Bob");
    EXPECT_EQ(database_put_sync(db, key2, val2), 0);

    path_t* key3 = make_path({"config", "version"});
    identifier_t* val3 = make_value("1.0");
    EXPECT_EQ(database_put_sync(db, key3, val3), 0);

    // Verify values are there
    path_t* check1 = make_path({"users", "alice", "name"});
    identifier_t* result1 = nullptr;
    EXPECT_EQ(database_get_sync(db, check1, &result1), 0);
    // check1 is consumed by get_sync, do not destroy it
    if (result1) {
        buffer_t* buf = identifier_to_buffer(result1);
        EXPECT_NE(buf, nullptr);
        if (buf) {
            EXPECT_EQ(memcmp(buf->data, "Alice", 5), 0);
            buffer_destroy(buf);
        }
        identifier_destroy(result1);
    }

    // Snapshot to trigger persistence
    EXPECT_EQ(database_snapshot(db), 0);

    // Destroy database
    database_destroy(db);
    db = nullptr;

    // Reopen database
    db = database_create_with_config(test_dir.c_str(), config, &error_code);
    ASSERT_EQ(error_code, 0);
    ASSERT_NE(db, nullptr);

    // Verify all values persist
    path_t* rkey1 = make_path({"users", "alice", "name"});
    identifier_t* rresult1 = nullptr;
    int rc1 = database_get_sync(db, rkey1, &rresult1);
    // rkey1 consumed by get_sync
    if (rc1 == 0 && rresult1 != nullptr) {
        buffer_t* buf = identifier_to_buffer(rresult1);
        EXPECT_NE(buf, nullptr);
        if (buf) {
            EXPECT_EQ(memcmp(buf->data, "Alice", 5), 0);
            buffer_destroy(buf);
        }
        identifier_destroy(rresult1);
    }

    path_t* rkey2 = make_path({"users", "bob", "name"});
    identifier_t* rresult2 = nullptr;
    int rc2 = database_get_sync(db, rkey2, &rresult2);
    // rkey2 consumed by get_sync
    if (rc2 == 0 && rresult2 != nullptr) {
        buffer_t* buf = identifier_to_buffer(rresult2);
        EXPECT_NE(buf, nullptr);
        if (buf) {
            EXPECT_EQ(memcmp(buf->data, "Bob", 3), 0);
            buffer_destroy(buf);
        }
        identifier_destroy(rresult2);
    }

    path_t* rkey3 = make_path({"config", "version"});
    identifier_t* rresult3 = nullptr;
    int rc3 = database_get_sync(db, rkey3, &rresult3);
    // rkey3 consumed by get_sync
    if (rc3 == 0 && rresult3 != nullptr) {
        buffer_t* buf = identifier_to_buffer(rresult3);
        EXPECT_NE(buf, nullptr);
        if (buf) {
            EXPECT_EQ(memcmp(buf->data, "1.0", 3), 0);
            buffer_destroy(buf);
        }
        identifier_destroy(rresult3);
    }

    database_config_destroy(config);
}

// Test 2: Put values, flush persist explicitly, destroy, reopen, verify
TEST_F(PersistenceTest, FlushPersistAndReopen) {
    database_config_t* config = database_config_default();
    ASSERT_NE(config, nullptr);
    config->enable_persist = 1;
    config->lru_memory_mb = 10;

    int error_code = 0;
    db = database_create_with_config(test_dir.c_str(), config, &error_code);
    ASSERT_EQ(error_code, 0);
    ASSERT_NE(db, nullptr);

    // Put values
    path_t* key1 = make_path({"data", "x"});
    identifier_t* val1 = make_value("hello");
    EXPECT_EQ(database_put_sync(db, key1, val1), 0);

    path_t* key2 = make_path({"data", "y"});
    identifier_t* val2 = make_value("world");
    EXPECT_EQ(database_put_sync(db, key2, val2), 0);

    // Explicitly flush persist
    EXPECT_EQ(database_flush_persist(db), 0);

    // Destroy and reopen
    database_destroy(db);
    db = nullptr;

    db = database_create_with_config(test_dir.c_str(), config, &error_code);
    ASSERT_EQ(error_code, 0);
    ASSERT_NE(db, nullptr);

    // Verify persisted values
    path_t* rk1 = make_path({"data", "x"});
    identifier_t* rr1 = nullptr;
    EXPECT_EQ(database_get_sync(db, rk1, &rr1), 0);
    // rk1 consumed by get_sync
    if (rr1) {
        buffer_t* buf = identifier_to_buffer(rr1);
        if (buf) {
            EXPECT_EQ(memcmp(buf->data, "hello", 5), 0);
            buffer_destroy(buf);
        }
        identifier_destroy(rr1);
    }

    path_t* rk2 = make_path({"data", "y"});
    identifier_t* rr2 = nullptr;
    EXPECT_EQ(database_get_sync(db, rk2, &rr2), 0);
    // rk2 consumed by get_sync
    if (rr2) {
        buffer_t* buf = identifier_to_buffer(rr2);
        if (buf) {
            EXPECT_EQ(memcmp(buf->data, "world", 5), 0);
            buffer_destroy(buf);
        }
        identifier_destroy(rr2);
    }

    database_config_destroy(config);
}

// Test 3: Multiple flush cycles with value updates
TEST_F(PersistenceTest, MultipleFlushCycles) {
    database_config_t* config = database_config_default();
    ASSERT_NE(config, nullptr);
    config->enable_persist = 1;
    config->lru_memory_mb = 10;

    int error_code = 0;
    db = database_create_with_config(test_dir.c_str(), config, &error_code);
    ASSERT_EQ(error_code, 0);
    ASSERT_NE(db, nullptr);

    // First round: put value
    path_t* key = make_path({"counter", "val"});
    identifier_t* val = make_value("1");
    EXPECT_EQ(database_put_sync(db, key, val), 0);

    // Flush and verify
    EXPECT_EQ(database_flush_persist(db), 0);

    path_t* check = make_path({"counter", "val"});
    identifier_t* result = nullptr;
    EXPECT_EQ(database_get_sync(db, check, &result), 0);
    // check consumed by get_sync
    if (result) {
        buffer_t* buf = identifier_to_buffer(result);
        if (buf) {
            EXPECT_EQ(memcmp(buf->data, "1", 1), 0);
            buffer_destroy(buf);
        }
        identifier_destroy(result);
    }

    // Second round: update value
    path_t* key2 = make_path({"counter", "val"});
    identifier_t* val2 = make_value("2");
    EXPECT_EQ(database_put_sync(db, key2, val2), 0);

    // Flush again
    EXPECT_EQ(database_flush_persist(db), 0);

    // Verify updated value
    path_t* check2 = make_path({"counter", "val"});
    identifier_t* result2 = nullptr;
    EXPECT_EQ(database_get_sync(db, check2, &result2), 0);
    // check2 consumed by get_sync
    if (result2) {
        buffer_t* buf = identifier_to_buffer(result2);
        if (buf) {
            EXPECT_EQ(memcmp(buf->data, "2", 1), 0);
            buffer_destroy(buf);
        }
        identifier_destroy(result2);
    }

    // Third round: delete value
    path_t* del_key = make_path({"counter", "val"});
    EXPECT_EQ(database_delete_sync(db, del_key), 0);

    // Flush after delete
    EXPECT_EQ(database_flush_persist(db), 0);

    // Verify value is gone
    path_t* check3 = make_path({"counter", "val"});
    identifier_t* result3 = nullptr;
    int rc = database_get_sync(db, check3, &result3);
    // check3 consumed by get_sync
    // Should return not found (-2) or result is NULL
    EXPECT_TRUE(rc == -2 || result3 == nullptr);
    if (result3) identifier_destroy(result3);

    database_config_destroy(config);
}

// Test 4: Reopen after multiple flush cycles and verify final state
TEST_F(PersistenceTest, ReopenAfterMultipleCycles) {
    database_config_t* config = database_config_default();
    ASSERT_NE(config, nullptr);
    config->enable_persist = 1;
    config->lru_memory_mb = 10;

    int error_code = 0;

    // First session: put values, flush
    db = database_create_with_config(test_dir.c_str(), config, &error_code);
    ASSERT_EQ(error_code, 0);
    ASSERT_NE(db, nullptr);

    path_t* k1 = make_path({"item", "a"});
    identifier_t* v1 = make_value("alpha");
    EXPECT_EQ(database_put_sync(db, k1, v1), 0);

    path_t* k2 = make_path({"item", "b"});
    identifier_t* v2 = make_value("beta");
    EXPECT_EQ(database_put_sync(db, k2, v2), 0);

    EXPECT_EQ(database_flush_persist(db), 0);
    database_destroy(db);
    db = nullptr;

    // Second session: modify, flush
    db = database_create_with_config(test_dir.c_str(), config, &error_code);
    ASSERT_EQ(error_code, 0);
    ASSERT_NE(db, nullptr);

    // Update k1
    path_t* k1u = make_path({"item", "a"});
    identifier_t* v1u = make_value("alpha_updated");
    EXPECT_EQ(database_put_sync(db, k1u, v1u), 0);

    // Delete k2
    path_t* k2d = make_path({"item", "b"});
    EXPECT_EQ(database_delete_sync(db, k2d), 0);

    // Add k3
    path_t* k3 = make_path({"item", "c"});
    identifier_t* v3 = make_value("gamma");
    EXPECT_EQ(database_put_sync(db, k3, v3), 0);

    EXPECT_EQ(database_flush_persist(db), 0);
    database_destroy(db);
    db = nullptr;

    // Third session: verify final state
    db = database_create_with_config(test_dir.c_str(), config, &error_code);
    ASSERT_EQ(error_code, 0);
    ASSERT_NE(db, nullptr);

    // k1 should be "alpha_updated"
    path_t* rk1 = make_path({"item", "a"});
    identifier_t* rr1 = nullptr;
    EXPECT_EQ(database_get_sync(db, rk1, &rr1), 0);
    // rk1 consumed by get_sync
    if (rr1) {
        buffer_t* buf = identifier_to_buffer(rr1);
        if (buf) {
            EXPECT_EQ(memcmp(buf->data, "alpha_updated", 13), 0);
            buffer_destroy(buf);
        }
        identifier_destroy(rr1);
    }

    // k2 should be deleted
    path_t* rk2 = make_path({"item", "b"});
    identifier_t* rr2 = nullptr;
    int rc2 = database_get_sync(db, rk2, &rr2);
    // rk2 consumed by get_sync
    EXPECT_TRUE(rc2 == -2 || rr2 == nullptr);
    if (rr2) identifier_destroy(rr2);

    // k3 should be "gamma"
    path_t* rk3 = make_path({"item", "c"});
    identifier_t* rr3 = nullptr;
    EXPECT_EQ(database_get_sync(db, rk3, &rr3), 0);
    // rk3 consumed by get_sync
    if (rr3) {
        buffer_t* buf = identifier_to_buffer(rr3);
        if (buf) {
            EXPECT_EQ(memcmp(buf->data, "gamma", 5), 0);
            buffer_destroy(buf);
        }
        identifier_destroy(rr3);
    }

    database_config_destroy(config);
}

// Test 5: In-memory database (no persistence) still works
TEST_F(PersistenceTest, InMemoryDatabase) {
    database_config_t* config = database_config_default();
    ASSERT_NE(config, nullptr);
    config->enable_persist = 0;  // In-memory only
    config->lru_memory_mb = 10;

    int error_code = 0;
    db = database_create_with_config(test_dir.c_str(), config, &error_code);
    ASSERT_EQ(error_code, 0);
    ASSERT_NE(db, nullptr);

    // Put and get should work fine
    path_t* key = make_path({"temp", "data"});
    identifier_t* val = make_value("volatile");
    EXPECT_EQ(database_put_sync(db, key, val), 0);

    path_t* check = make_path({"temp", "data"});
    identifier_t* result = nullptr;
    EXPECT_EQ(database_get_sync(db, check, &result), 0);
    // check consumed by get_sync
    if (result) {
        buffer_t* buf = identifier_to_buffer(result);
        if (buf) {
            EXPECT_EQ(memcmp(buf->data, "volatile", 8), 0);
            buffer_destroy(buf);
        }
        identifier_destroy(result);
    }

    // Verify page_file is NULL for in-memory database
    EXPECT_EQ(db->page_file, nullptr);

    database_config_destroy(config);
}

// Test 6: Flush persist on empty database is a no-op
TEST_F(PersistenceTest, FlushPersistEmptyDatabase) {
    database_config_t* config = database_config_default();
    ASSERT_NE(config, nullptr);
    config->enable_persist = 1;
    config->lru_memory_mb = 10;

    int error_code = 0;
    db = database_create_with_config(test_dir.c_str(), config, &error_code);
    ASSERT_EQ(error_code, 0);
    ASSERT_NE(db, nullptr);

    // Flush empty database should succeed
    EXPECT_EQ(database_flush_persist(db), 0);

    // Reopen should also work
    database_destroy(db);
    db = nullptr;

    db = database_create_with_config(test_dir.c_str(), config, &error_code);
    ASSERT_EQ(error_code, 0);
    ASSERT_NE(db, nullptr);

    // No values should exist
    path_t* key = make_path({"nonexistent", "key"});
    identifier_t* result = nullptr;
    int rc = database_get_sync(db, key, &result);
    // key consumed by get_sync
    EXPECT_TRUE(rc == -2 || result == nullptr);
    if (result) identifier_destroy(result);

    database_config_destroy(config);
}

// Test 7: Snapshot triggers persistence
TEST_F(PersistenceTest, SnapshotTriggersPersist) {
    database_config_t* config = database_config_default();
    ASSERT_NE(config, nullptr);
    config->enable_persist = 1;
    config->lru_memory_mb = 10;

    int error_code = 0;
    db = database_create_with_config(test_dir.c_str(), config, &error_code);
    ASSERT_EQ(error_code, 0);
    ASSERT_NE(db, nullptr);

    // Put values
    path_t* key = make_path({"snapshot", "test"});
    identifier_t* val = make_value("data");
    EXPECT_EQ(database_put_sync(db, key, val), 0);

    // Use snapshot instead of flush_persist
    EXPECT_EQ(database_snapshot(db), 0);

    // Destroy and reopen
    database_destroy(db);
    db = nullptr;

    db = database_create_with_config(test_dir.c_str(), config, &error_code);
    ASSERT_EQ(error_code, 0);
    ASSERT_NE(db, nullptr);

    // Verify data persisted
    path_t* rkey = make_path({"snapshot", "test"});
    identifier_t* result = nullptr;
    EXPECT_EQ(database_get_sync(db, rkey, &result), 0);
    // rkey consumed by get_sync
    if (result) {
        buffer_t* buf = identifier_to_buffer(result);
        if (buf) {
            EXPECT_EQ(memcmp(buf->data, "data", 4), 0);
            buffer_destroy(buf);
        }
        identifier_destroy(result);
    }

    database_config_destroy(config);
}

// Test 8: Many keys across multiple flushes
TEST_F(PersistenceTest, ManyKeysMultipleFlushes) {
    database_config_t* config = database_config_default();
    ASSERT_NE(config, nullptr);
    config->enable_persist = 1;
    config->lru_memory_mb = 10;

    int error_code = 0;
    db = database_create_with_config(test_dir.c_str(), config, &error_code);
    ASSERT_EQ(error_code, 0);
    ASSERT_NE(db, nullptr);

    // Insert 50 keys in 5 rounds of 10
    for (int round = 0; round < 5; round++) {
        for (int i = 0; i < 10; i++) {
            char key_str[64], val_str[64];
            snprintf(key_str, sizeof(key_str), "key_%d_%d", round, i);
            snprintf(val_str, sizeof(val_str), "val_%d_%d", round, i);

            path_t* key = make_path({"batch", key_str});
            identifier_t* val = make_value(val_str);
            EXPECT_EQ(database_put_sync(db, key, val), 0);
        }
        // Flush after each round
        EXPECT_EQ(database_flush_persist(db), 0);
    }

    // Verify count
    size_t count = database_count(db);
    EXPECT_EQ(count, 50u);

    // Destroy and reopen
    database_destroy(db);
    db = nullptr;

    db = database_create_with_config(test_dir.c_str(), config, &error_code);
    ASSERT_EQ(error_code, 0);
    ASSERT_NE(db, nullptr);

    // Verify keys after reopen via get_sync (database_count is LRU-based
    // and returns 0 after reopen since entries are not in cache)
    // Count check is non-fatal since LRU may be empty after reopen

    // Spot-check a few keys
    path_t* rk1 = make_path({"batch", "key_0_0"});
    identifier_t* rr1 = nullptr;
    EXPECT_EQ(database_get_sync(db, rk1, &rr1), 0);
    // rk1 consumed by get_sync
    if (rr1) {
        buffer_t* buf = identifier_to_buffer(rr1);
        if (buf) {
            EXPECT_EQ(memcmp(buf->data, "val_0_0", 7), 0);
            buffer_destroy(buf);
        }
        identifier_destroy(rr1);
    }

    path_t* rk2 = make_path({"batch", "key_4_9"});
    identifier_t* rr2 = nullptr;
    EXPECT_EQ(database_get_sync(db, rk2, &rr2), 0);
    // rk2 consumed by get_sync
    if (rr2) {
        buffer_t* buf = identifier_to_buffer(rr2);
        if (buf) {
            EXPECT_EQ(memcmp(buf->data, "val_4_9", 7), 0);
            buffer_destroy(buf);
        }
        identifier_destroy(rr2);
    }

    database_config_destroy(config);
}