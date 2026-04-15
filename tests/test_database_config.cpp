#include <gtest/gtest.h>
#include <cstdio>
#include <unistd.h>
#include <sys/stat.h>
#include <cstring>
extern "C" {
#include "Database/database_config.h"
#include "Database/database.h"
#include "Workers/pool.h"
#include "Time/wheel.h"
}

// Test: database_config_default creates valid config
TEST(DatabaseConfig, DefaultCreatesValidConfig) {
    database_config_t* config = database_config_default();
    ASSERT_NE(config, nullptr);

    // Check defaults
    EXPECT_EQ(config->chunk_size, 4);
    EXPECT_EQ(config->btree_node_size, 4096u);
    EXPECT_EQ(config->enable_persist, 1);
    EXPECT_EQ(config->lru_memory_mb, 50u);
    EXPECT_EQ(config->storage_cache_size, 1024u);
    EXPECT_EQ(config->worker_threads, 4u);
    EXPECT_EQ(config->timer_resolution_ms, 10u);
    EXPECT_EQ(config->external_pool, nullptr);
    EXPECT_EQ(config->external_wheel, nullptr);

    database_config_destroy(config);
}

// Test: database_config_copy creates exact copy
TEST(DatabaseConfig, CopyCreatesExactCopy) {
    database_config_t* config = database_config_default();
    ASSERT_NE(config, nullptr);

    // Modify some values
    config->lru_memory_mb = 100;
    config->worker_threads = 8;

    database_config_t* copy = database_config_copy(config);
    ASSERT_NE(copy, nullptr);

    EXPECT_EQ(copy->lru_memory_mb, 100u);
    EXPECT_EQ(copy->worker_threads, 8u);
    EXPECT_EQ(copy->chunk_size, config->chunk_size);

    database_config_destroy(config);
    database_config_destroy(copy);
}

// Test: database_config_destroy handles NULL
TEST(DatabaseConfig, DestroyHandlesNull) {
    // Should not crash
    database_config_destroy(nullptr);
}

// Test: database_config_copy handles NULL
TEST(DatabaseConfig, CopyHandlesNull) {
    database_config_t* copy = database_config_copy(nullptr);
    EXPECT_EQ(copy, nullptr);
}

// Test: WAL config defaults are reasonable
TEST(DatabaseConfig, WalConfigDefaults) {
    database_config_t* config = database_config_default();
    ASSERT_NE(config, nullptr);

    EXPECT_EQ(config->wal_config.sync_mode, WAL_SYNC_IMMEDIATE);
    EXPECT_GT(config->wal_config.debounce_ms, 0u);
    EXPECT_GT(config->wal_config.max_file_size, 0u);

    database_config_destroy(config);
}

// Test: Save and load preserves values
TEST(DatabaseConfig, SaveAndLoadPreservesValues) {
    // Create temp directory
    char temp_dir[] = "/tmp/wavedb_config_test_XXXXXX";
    mkdtemp(temp_dir);

    // Create config with custom values
    database_config_t* original = database_config_default();
    original->lru_memory_mb = 100;
    original->worker_threads = 8;
    original->wal_config.sync_mode = WAL_SYNC_DEBOUNCED;
    original->wal_config.debounce_ms = 200;

    // Save
    int result = database_config_save(temp_dir, original);
    ASSERT_EQ(result, 0);

    // Load
    database_config_t* loaded = database_config_load(temp_dir);
    ASSERT_NE(loaded, nullptr);

    // Verify
    EXPECT_EQ(loaded->lru_memory_mb, 100u);
    EXPECT_EQ(loaded->worker_threads, 8u);
    EXPECT_EQ(loaded->wal_config.sync_mode, WAL_SYNC_DEBOUNCED);
    EXPECT_EQ(loaded->wal_config.debounce_ms, 200u);

    // Verify immutable settings preserved
    EXPECT_EQ(loaded->chunk_size, original->chunk_size);
    EXPECT_EQ(loaded->btree_node_size, original->btree_node_size);
    EXPECT_EQ(loaded->enable_persist, original->enable_persist);

    database_config_destroy(original);
    database_config_destroy(loaded);

    // Cleanup
    char config_path[256];
    snprintf(config_path, sizeof(config_path), "%s/.config", temp_dir);
    unlink(config_path);
    rmdir(temp_dir);
}

// Test: Load returns NULL for non-existent config
TEST(DatabaseConfig, LoadNonExistentReturnsNull) {
    database_config_t* config = database_config_load("/nonexistent/path/12345");
    EXPECT_EQ(config, nullptr);
}

// Test: Merge uses saved for immutable, passed for mutable
TEST(DatabaseConfig, MergeRules) {
    database_config_t* saved = database_config_default();
    saved->chunk_size = 8;  // Different immutable
    saved->lru_memory_mb = 50;  // Original mutable

    database_config_t* passed = database_config_default();
    passed->chunk_size = 4;  // Different immutable
    passed->lru_memory_mb = 100;  // New mutable

    database_config_t* merged = database_config_merge(saved, passed);
    ASSERT_NE(merged, nullptr);

    // Immutable: use saved
    EXPECT_EQ(merged->chunk_size, 8u);

    // Mutable: use passed
    EXPECT_EQ(merged->lru_memory_mb, 100u);

    database_config_destroy(saved);
    database_config_destroy(passed);
    database_config_destroy(merged);
}

// Test: Merge handles NULL saved
TEST(DatabaseConfig, MergeNullSaved) {
    database_config_t* passed = database_config_default();
    passed->lru_memory_mb = 100;

    database_config_t* merged = database_config_merge(nullptr, passed);
    ASSERT_NE(merged, nullptr);
    EXPECT_EQ(merged->lru_memory_mb, 100u);

    database_config_destroy(passed);
    database_config_destroy(merged);
}

// Test: Merge handles NULL passed
TEST(DatabaseConfig, MergeNullPassed) {
    database_config_t* saved = database_config_default();
    saved->lru_memory_mb = 100;

    database_config_t* merged = database_config_merge(saved, nullptr);
    ASSERT_NE(merged, nullptr);
    EXPECT_EQ(merged->lru_memory_mb, 100u);

    database_config_destroy(saved);
    database_config_destroy(merged);
}

// Test: Merge handles both NULL
TEST(DatabaseConfig, MergeBothNull) {
    database_config_t* merged = database_config_merge(nullptr, nullptr);
    ASSERT_NE(merged, nullptr);

    // Should return defaults
    EXPECT_EQ(merged->chunk_size, DATABASE_CONFIG_DEFAULT_CHUNK_SIZE);
    EXPECT_EQ(merged->lru_memory_mb, DATABASE_CONFIG_DEFAULT_LRU_MEMORY_MB);

    database_config_destroy(merged);
}

// Test: Create database with config
TEST(DatabaseConfig, CreateWithConfig) {
    char temp_dir[] = "/tmp/wavedb_config_test_XXXXXX";
    mkdtemp(temp_dir);

    database_config_t* config = database_config_default();
    config->lru_memory_mb = 10;
    config->worker_threads = 2;

    int error = 0;
    database_t* db = database_create_with_config(temp_dir, config, &error);
    ASSERT_NE(db, nullptr);
    EXPECT_EQ(error, 0);

    // Verify owns_pool is true (created own)
    EXPECT_TRUE(db->owns_pool);
    EXPECT_TRUE(db->owns_wheel);

    database_destroy(db);
    database_config_destroy(config);

    // Cleanup
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", temp_dir);
    system(cmd);
}

// Test: Reopen preserves immutable settings
TEST(DatabaseConfig, ReopenPreservesImmutable) {
    char temp_dir[] = "/tmp/wavedb_config_test_XXXXXX";
    mkdtemp(temp_dir);

    // Create with chunk_size=8
    database_config_t* config1 = database_config_default();
    config1->chunk_size = 8;
    config1->worker_threads = 2;

    int error = 0;
    database_t* db1 = database_create_with_config(temp_dir, config1, &error);
    ASSERT_NE(db1, nullptr);
    database_destroy(db1);
    database_config_destroy(config1);

    // Reopen with chunk_size=4 (should be ignored)
    database_config_t* config2 = database_config_default();
    config2->chunk_size = 4;  // Different - should be ignored
    config2->lru_memory_mb = 100;  // Mutable - should change
    config2->worker_threads = 2;

    database_t* db2 = database_create_with_config(temp_dir, config2, &error);
    ASSERT_NE(db2, nullptr);

    // Verify immutable setting preserved
    EXPECT_EQ(db2->chunk_size, 8u);  // Original value preserved

    database_destroy(db2);
    database_config_destroy(config2);

    // Cleanup
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", temp_dir);
    system(cmd);
}

// Test: External pool/wheel not owned
TEST(DatabaseConfig, ExternalResourcesNotOwned) {
    char temp_dir[] = "/tmp/wavedb_config_test_XXXXXX";
    mkdtemp(temp_dir);

    // Create external resources
    work_pool_t* pool = work_pool_create(2);
    hierarchical_timing_wheel_t* wheel = hierarchical_timing_wheel_create(10, pool);

    database_config_t* config = database_config_default();
    config->external_pool = pool;
    config->external_wheel = wheel;

    int error = 0;
    database_t* db = database_create_with_config(temp_dir, config, &error);
    ASSERT_NE(db, nullptr);

    // Verify not owned
    EXPECT_FALSE(db->owns_pool);
    EXPECT_FALSE(db->owns_wheel);

    database_destroy(db);
    database_config_destroy(config);

    // Stop wheel and pool before cleanup
    if (wheel) {
        hierarchical_timing_wheel_stop(wheel);
    }
    if (pool) {
        work_pool_shutdown(pool);
        work_pool_join_all(pool);
    }
    hierarchical_timing_wheel_destroy(wheel);
    work_pool_destroy(pool);

    // Cleanup
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", temp_dir);
    system(cmd);
}