//
// test_section_gc.cpp - Tests for section storage GC deallocation and persistence
//

#include <gtest/gtest.h>
#include <string>
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
}

class SectionGCTest : public ::testing::Test {
protected:
    void SetUp() override {
        test_dir = "/tmp/wavedb_section_gc_" + std::to_string(getpid()) + "_" + std::to_string(test_counter++);
        mkdir(test_dir.c_str(), 0700);

        pool = work_pool_create(platform_core_count());
        work_pool_launch(pool);

        wheel = hierarchical_timing_wheel_create(8, pool);
        hierarchical_timing_wheel_run(wheel);
    }

    void TearDown() override {
        if (db) {
            database_destroy(db);
            db = nullptr;
        }

        if (wheel) {
            hierarchical_timing_wheel_wait_for_idle_signal(wheel);
            hierarchical_timing_wheel_stop(wheel);
        }

        if (pool) {
            work_pool_shutdown(pool);
            work_pool_join_all(pool);
            work_pool_destroy(pool);
            pool = nullptr;
        }

        if (wheel) {
            hierarchical_timing_wheel_destroy(wheel);
            wheel = nullptr;
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

    void expect_identifier_eq(identifier_t* id, const char* expected) {
        ASSERT_NE(id, nullptr);
        buffer_t* buf = identifier_to_buffer(id);
        ASSERT_NE(buf, nullptr);
        EXPECT_EQ(memcmp(buf->data, expected, strlen(expected)), 0);
        buffer_destroy(buf);
    }

    database_t* db = nullptr;
    work_pool_t* pool = nullptr;
    hierarchical_timing_wheel_t* wheel = nullptr;
    std::string test_dir;
    static int test_counter;
};

int SectionGCTest::test_counter = 0;

// ============================================================================
// Test 1: Persistence round-trip with section storage
// Insert data, snapshot to sections, destroy, reload, verify data intact.
// ============================================================================
TEST_F(SectionGCTest, PersistAndReload) {
    int error = 0;

    // Create database with section storage enabled
    database_config_t* config = database_config_default();
    config->enable_persist = 1;
    config->lru_memory_mb = 10;
    config->external_pool = pool;
    config->external_wheel = wheel;

    db = database_create_with_config(test_dir.c_str(), config, &error);
    database_config_destroy(config);
    ASSERT_NE(db, nullptr) << "Failed to create database, error=" << error;
    ASSERT_EQ(error, 0);

    // Insert data — database_put_sync CONSUMEs both path and value references
    path_t* path = make_path({"users", "alice"});
    identifier_t* value = make_value("engineer");
    EXPECT_EQ(database_put_sync(db, path, value), 0);

    // Snapshot to sections
    EXPECT_EQ(database_snapshot(db), 0);

    // Verify data before destroy
    path_t* read_path = make_path({"users", "alice"});
    identifier_t* result = nullptr;
    EXPECT_EQ(database_get_sync(db, read_path, &result), 0);
    expect_identifier_eq(result, "engineer");
    identifier_destroy(result);

    // Destroy and reload
    database_destroy(db);
    db = nullptr;

    // Wait for pending operations
    hierarchical_timing_wheel_wait_for_idle_signal(wheel);

    // Reload database from disk
    config = database_config_default();
    config->enable_persist = 1;
    config->lru_memory_mb = 10;
    config->external_pool = pool;
    config->external_wheel = wheel;

    db = database_create_with_config(test_dir.c_str(), config, &error);
    database_config_destroy(config);
    ASSERT_NE(db, nullptr) << "Failed to reload database, error=" << error;

    // Verify data persists after reload
    read_path = make_path({"users", "alice"});
    result = nullptr;
    EXPECT_EQ(database_get_sync(db, read_path, &result), 0);
    expect_identifier_eq(result, "engineer");
    identifier_destroy(result);
}

// ============================================================================
// Test 2: Multiple keys persistence round-trip
// Insert multiple keys, snapshot, reload, verify all intact.
// ============================================================================
TEST_F(SectionGCTest, MultipleKeysPersistAndReload) {
    int error = 0;

    database_config_t* config = database_config_default();
    config->enable_persist = 1;
    config->lru_memory_mb = 10;
    config->external_pool = pool;
    config->external_wheel = wheel;

    db = database_create_with_config(test_dir.c_str(), config, &error);
    database_config_destroy(config);
    ASSERT_NE(db, nullptr) << "Failed to create database, error=" << error;

    // Insert multiple keys — put_sync CONSUMEs path and value
    const char* keys[] = {"users/alice", "users/bob", "users/charlie", "data/1", "data/2"};
    const char* values[] = {"engineer", "designer", "manager", "hello", "world"};
    const int count = 5;

    for (int i = 0; i < count; i++) {
        path_t* path = make_path({"test", keys[i]});
        identifier_t* value = make_value(values[i]);
        EXPECT_EQ(database_put_sync(db, path, value), 0);
    }

    // Snapshot to sections
    EXPECT_EQ(database_snapshot(db), 0);

    // Verify all keys before destroy
    for (int i = 0; i < count; i++) {
        path_t* path = make_path({"test", keys[i]});
        identifier_t* result = nullptr;
        EXPECT_EQ(database_get_sync(db, path, &result), 0);
        expect_identifier_eq(result, values[i]);
        identifier_destroy(result);
    }

    // Destroy and reload
    database_destroy(db);
    db = nullptr;
    hierarchical_timing_wheel_wait_for_idle_signal(wheel);

    config = database_config_default();
    config->enable_persist = 1;
    config->lru_memory_mb = 10;
    config->external_pool = pool;
    config->external_wheel = wheel;

    db = database_create_with_config(test_dir.c_str(), config, &error);
    database_config_destroy(config);
    ASSERT_NE(db, nullptr) << "Failed to reload database, error=" << error;

    // Verify all keys after reload
    for (int i = 0; i < count; i++) {
        path_t* path = make_path({"test", keys[i]});
        identifier_t* result = nullptr;
        EXPECT_EQ(database_get_sync(db, path, &result), 0);
        expect_identifier_eq(result, values[i]);
        identifier_destroy(result);
    }
}

// ============================================================================
// Test 3: Delete and GC deallocation
// Insert data, snapshot, delete, snapshot again (triggers GC),
// verify deleted data is gone.
// ============================================================================
TEST_F(SectionGCTest, DeleteAndGC) {
    int error = 0;

    database_config_t* config = database_config_default();
    config->enable_persist = 1;
    config->lru_memory_mb = 10;
    config->external_pool = pool;
    config->external_wheel = wheel;

    db = database_create_with_config(test_dir.c_str(), config, &error);
    database_config_destroy(config);
    ASSERT_NE(db, nullptr) << "Failed to create database, error=" << error;

    // Insert data
    path_t* path = make_path({"users", "alice"});
    identifier_t* value = make_value("engineer");
    EXPECT_EQ(database_put_sync(db, path, value), 0);

    // First snapshot writes to sections
    EXPECT_EQ(database_snapshot(db), 0);

    // Verify data exists
    path_t* read_path = make_path({"users", "alice"});
    identifier_t* result = nullptr;
    EXPECT_EQ(database_get_sync(db, read_path, &result), 0);
    expect_identifier_eq(result, "engineer");
    identifier_destroy(result);

    // Delete the key
    path_t* del_path = make_path({"users", "alice"});
    EXPECT_EQ(database_delete_sync(db, del_path), 0);

    // Snapshot again — triggers GC which deallocates from sections
    EXPECT_EQ(database_snapshot(db), 0);

    // Verify data is gone
    read_path = make_path({"users", "alice"});
    result = nullptr;
    int rc = database_get_sync(db, read_path, &result);
    EXPECT_NE(rc, 0);  // Should not find deleted key
}

// ============================================================================
// Test 4: In-memory database still works (CBOR fallback)
// Verify that enable_persist=0 still uses monolithic CBOR snapshot.
// ============================================================================
TEST_F(SectionGCTest, InMemoryFallbackWorks) {
    int error = 0;

    // Create in-memory database (no section storage)
    db = database_create(test_dir.c_str(), 10, NULL, 4, 4096, 0, 0, pool, wheel, &error);
    ASSERT_NE(db, nullptr) << "Failed to create database, error=" << error;
    ASSERT_EQ(error, 0);

    // Insert data — put_sync CONSUMEs path and value
    path_t* path = make_path({"test", "key"});
    identifier_t* value = make_value("value123");
    EXPECT_EQ(database_put_sync(db, path, value), 0);

    // Snapshot (CBOR path)
    EXPECT_EQ(database_snapshot(db), 0);

    // Verify data
    path_t* read_path = make_path({"test", "key"});
    identifier_t* result = nullptr;
    EXPECT_EQ(database_get_sync(db, read_path, &result), 0);
    expect_identifier_eq(result, "value123");
    identifier_destroy(result);
}

// ============================================================================
// Test 5: Overwrite value and GC old version
// Insert, snapshot, overwrite with new value, snapshot (GC old version),
// verify only new value visible.
// ============================================================================
TEST_F(SectionGCTest, OverwriteAndGC) {
    int error = 0;

    database_config_t* config = database_config_default();
    config->enable_persist = 1;
    config->lru_memory_mb = 10;
    config->external_pool = pool;
    config->external_wheel = wheel;

    db = database_create_with_config(test_dir.c_str(), config, &error);
    database_config_destroy(config);
    ASSERT_NE(db, nullptr) << "Failed to create database, error=" << error;

    // Insert initial value
    path_t* path = make_path({"config", "setting"});
    identifier_t* value = make_value("old_value");
    EXPECT_EQ(database_put_sync(db, path, value), 0);

    // First snapshot
    EXPECT_EQ(database_snapshot(db), 0);

    // Overwrite with new value
    path = make_path({"config", "setting"});
    value = make_value("new_value");
    EXPECT_EQ(database_put_sync(db, path, value), 0);

    // Second snapshot triggers GC of old version
    EXPECT_EQ(database_snapshot(db), 0);

    // Verify only new value is visible
    path_t* read_path = make_path({"config", "setting"});
    identifier_t* result = nullptr;
    EXPECT_EQ(database_get_sync(db, read_path, &result), 0);
    expect_identifier_eq(result, "new_value");
    identifier_destroy(result);
}

// ============================================================================
// Test 6: Persistence round-trip with deep trie structure
// Test that bnode trees with internal nodes serialize/deserialize correctly.
// ============================================================================
TEST_F(SectionGCTest, DeepTriePersistAndReload) {
    int error = 0;

    database_config_t* config = database_config_default();
    config->enable_persist = 1;
    config->lru_memory_mb = 10;
    config->external_pool = pool;
    config->external_wheel = wheel;

    db = database_create_with_config(test_dir.c_str(), config, &error);
    database_config_destroy(config);
    ASSERT_NE(db, nullptr) << "Failed to create database, error=" << error;

    // Insert enough keys to force trie splits (chunk_size=4, so keys with
    // same 4-byte prefix go into same hbtrie_node)
    const int num_keys = 50;
    for (int i = 0; i < num_keys; i++) {
        char key[32];
        snprintf(key, sizeof(key), "key%04d", i);
        char val[32];
        snprintf(val, sizeof(val), "val%04d", i);

        path_t* path = make_path({"data", key});
        identifier_t* value = make_value(val);
        EXPECT_EQ(database_put_sync(db, path, value), 0);
    }

    // Snapshot to sections
    EXPECT_EQ(database_snapshot(db), 0);

    // Verify a sample of keys
    for (int i = 0; i < num_keys; i += 5) {
        char key[32];
        snprintf(key, sizeof(key), "key%04d", i);
        char val[32];
        snprintf(val, sizeof(val), "val%04d", i);

        path_t* path = make_path({"data", key});
        identifier_t* result = nullptr;
        EXPECT_EQ(database_get_sync(db, path, &result), 0);
        expect_identifier_eq(result, val);
        identifier_destroy(result);
    }

    // Destroy and reload
    database_destroy(db);
    db = nullptr;
    hierarchical_timing_wheel_wait_for_idle_signal(wheel);

    config = database_config_default();
    config->enable_persist = 1;
    config->lru_memory_mb = 10;
    config->external_pool = pool;
    config->external_wheel = wheel;

    db = database_create_with_config(test_dir.c_str(), config, &error);
    database_config_destroy(config);
    ASSERT_NE(db, nullptr) << "Failed to reload database, error=" << error;

    // Verify all keys after reload
    for (int i = 0; i < num_keys; i++) {
        char key[32];
        snprintf(key, sizeof(key), "key%04d", i);
        char val[32];
        snprintf(val, sizeof(val), "val%04d", i);

        path_t* path = make_path({"data", key});
        identifier_t* result = nullptr;
        EXPECT_EQ(database_get_sync(db, path, &result), 0);
        expect_identifier_eq(result, val);
        identifier_destroy(result);
    }
}

// ============================================================================
// Test 7: Auto-persist on destroy (no explicit snapshot needed)
// Test that database_destroy automatically persists the trie, so data
// survives across destroy/create cycles without calling database_snapshot.
// ============================================================================
TEST_F(SectionGCTest, AutoPersistOnDestroy) {
    int error = 0;

    database_config_t* config = database_config_default();
    config->enable_persist = 1;
    config->lru_memory_mb = 10;
    config->external_pool = pool;
    config->external_wheel = wheel;

    db = database_create_with_config(test_dir.c_str(), config, &error);
    database_config_destroy(config);
    ASSERT_NE(db, nullptr) << "Failed to create database, error=" << error;

    // Insert data
    path_t* path = make_path({"users", "alice"});
    identifier_t* value = make_value("engineer");
    EXPECT_EQ(database_put_sync(db, path, value), 0);

    path = make_path({"users", "bob"});
    value = make_value("designer");
    EXPECT_EQ(database_put_sync(db, path, value), 0);

    // Do NOT call database_snapshot — rely on auto-persist in database_destroy
    database_destroy(db);
    db = nullptr;
    hierarchical_timing_wheel_wait_for_idle_signal(wheel);

    // Reload database from disk
    config = database_config_default();
    config->enable_persist = 1;
    config->lru_memory_mb = 10;
    config->external_pool = pool;
    config->external_wheel = wheel;

    db = database_create_with_config(test_dir.c_str(), config, &error);
    database_config_destroy(config);
    ASSERT_NE(db, nullptr) << "Failed to reload database, error=" << error;

    // Verify data persists after reload
    path = make_path({"users", "alice"});
    identifier_t* result = nullptr;
    EXPECT_EQ(database_get_sync(db, path, &result), 0);
    expect_identifier_eq(result, "engineer");
    identifier_destroy(result);

    path = make_path({"users", "bob"});
    result = nullptr;
    EXPECT_EQ(database_get_sync(db, path, &result), 0);
    expect_identifier_eq(result, "designer");
    identifier_destroy(result);
}