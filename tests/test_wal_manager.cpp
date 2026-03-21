//
// Created by victor on 3/21/26.
//

#include <gtest/gtest.h>
#include "Database/wal_manager.h"
#include "Buffer/buffer.h"
#include "Workers/transaction_id.h"
#include <pthread.h>
#include <unistd.h>
#include <cstring>

class WalManagerTest : public ::testing::Test {
protected:
    char temp_dir[256];
    wal_manager_t* manager;
    wal_config_t config;

    void SetUp() override {
        // Create temporary directory using mkdtemp
        strcpy(temp_dir, "/tmp/wal_test_XXXXXX");
        ASSERT_NE(mkdtemp(temp_dir), nullptr) << "Failed to create temp dir";

        // Initialize transaction ID generator
        transaction_id_init();

        // Initialize default config
        config.sync_mode = WAL_SYNC_IMMEDIATE;
        config.debounce_ms = WAL_DEFAULT_DEBOUNCE_MS;
        config.idle_threshold_ms = WAL_DEFAULT_IDLE_THRESHOLD_MS;
        config.compact_interval_ms = WAL_DEFAULT_COMPACT_INTERVAL_MS;
        config.max_file_size = WAL_DEFAULT_MAX_FILE_SIZE;

        manager = nullptr;
    }

    void TearDown() override {
        if (manager) {
            wal_manager_destroy(manager);
        }
        // Remove temporary directory
        char cmd[512];
        snprintf(cmd, sizeof(cmd), "rm -rf %s", temp_dir);
        system(cmd);
    }
};

TEST_F(WalManagerTest, CreateManager) {
    int error = 0;
    manager = wal_manager_create(temp_dir, &config, &error);
    ASSERT_NE(manager, nullptr);
    EXPECT_EQ(error, 0);
}

TEST_F(WalManagerTest, GetThreadWal) {
    int error = 0;
    manager = wal_manager_create(temp_dir, &config, &error);
    ASSERT_NE(manager, nullptr);
    EXPECT_EQ(error, 0);

    thread_wal_t* twal = get_thread_wal(manager);
    ASSERT_NE(twal, nullptr);
    EXPECT_GT(twal->fd, 0);  // Valid file descriptor
}

TEST_F(WalManagerTest, ThreadWalFilePath) {
    int error = 0;
    manager = wal_manager_create(temp_dir, &config, &error);
    ASSERT_NE(manager, nullptr);

    thread_wal_t* twal = get_thread_wal(manager);
    ASSERT_NE(twal, nullptr);
    ASSERT_NE(twal->file_path, nullptr);

    // File path should contain "thread_" and ".wal"
    EXPECT_NE(strstr(twal->file_path, "thread_"), nullptr);
    EXPECT_NE(strstr(twal->file_path, ".wal"), nullptr);
}

TEST_F(WalManagerTest, MultipleThreadWals) {
    int error = 0;
    manager = wal_manager_create(temp_dir, &config, &error);
    ASSERT_NE(manager, nullptr);

    // First call should create thread-local WAL
    thread_wal_t* twal1 = get_thread_wal(manager);
    ASSERT_NE(twal1, nullptr);

    // Second call should return same thread-local WAL
    thread_wal_t* twal2 = get_thread_wal(manager);
    ASSERT_NE(twal2, nullptr);
    EXPECT_EQ(twal1, twal2);
}
