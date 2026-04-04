#include <gtest/gtest.h>
extern "C" {
#include "Database/database_config.h"
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