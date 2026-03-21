//
// Created by victor on 3/21/26.
//

#include <gtest/gtest.h>
#include "Database/wal_manager.h"
#include "Buffer/buffer.h"
#include "Workers/transaction_id.h"
#include "Util/path_join.h"
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#include <sys/stat.h>

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

TEST_F(WalManagerTest, WriteToThreadWal) {
    int error = 0;
    manager = wal_manager_create(temp_dir, &config, &error);
    ASSERT_NE(manager, nullptr);

    thread_wal_t* twal = get_thread_wal(manager);
    ASSERT_NE(twal, nullptr);

    // Create test data
    const char* test_data = "Hello, WAL!";
    buffer_t* data = buffer_create_from_pointer_copy((uint8_t*)test_data, strlen(test_data));
    ASSERT_NE(data, nullptr);

    // Generate transaction ID
    transaction_id_t txn_id = transaction_id_get_next();

    // Write to thread-local WAL
    int result = thread_wal_write(twal, txn_id, WAL_PUT, data);
    EXPECT_EQ(result, 0);

    buffer_destroy(data);
}

TEST_F(WalManagerTest, ReadManifest) {
    int error = 0;
    manager = wal_manager_create(temp_dir, &config, &error);
    ASSERT_NE(manager, nullptr);

    thread_wal_t* twal = get_thread_wal(manager);
    ASSERT_NE(twal, nullptr);

    // Write entry
    const char* test_data = "test";
    buffer_t* data = buffer_create_from_pointer_copy((uint8_t*)test_data, 4);
    ASSERT_NE(data, nullptr);
    transaction_id_t txn_id = transaction_id_get_next();
    thread_wal_write(twal, txn_id, WAL_PUT, data);

    // Read manifest while manager is still active
    manifest_entry_t* entries = nullptr;
    size_t count = 0;
    int result = read_manifest(manager, &entries, &count);
    EXPECT_EQ(result, 0);
    EXPECT_GT(count, (size_t)0);  // At least one entry (the initial ACTIVE entry for thread WAL)

    // Verify entry
    if (count > 0) {
        EXPECT_EQ(entries[0].status, WAL_FILE_ACTIVE);
    }

    // Cleanup
    free(entries);
    buffer_destroy(data);
}

TEST_F(WalManagerTest, RecoverFromMultipleThreads) {
    int error = 0;
    manager = wal_manager_create(temp_dir, &config, &error);
    ASSERT_NE(manager, nullptr);

    // Write entries from thread 1
    thread_wal_t* twal1 = get_thread_wal(manager);
    const char* data1_str = "thread1";
    buffer_t* data1 = buffer_create_from_pointer_copy((uint8_t*)data1_str, strlen(data1_str));
    ASSERT_NE(data1, nullptr);
    transaction_id_t txn1 = transaction_id_get_next();
    thread_wal_write(twal1, txn1, WAL_PUT, data1);

    // Simulate another thread (would normally use pthread_create)
    // For testing, we'll manually create a second WAL
    uint64_t thread_id_2 = (uint64_t)pthread_self() + 1;
    thread_wal_t* twal2 = create_thread_wal(manager, thread_id_2);
    const char* data2_str = "thread2";
    buffer_t* data2 = buffer_create_from_pointer_copy((uint8_t*)data2_str, strlen(data2_str));
    ASSERT_NE(data2, nullptr);
    transaction_id_t txn2 = transaction_id_get_next();
    thread_wal_write(twal2, txn2, WAL_PUT, data2);

    // Close manager
    wal_manager_destroy(manager);
    manager = nullptr;

    // Reopen and recover
    manager = wal_manager_create(temp_dir, &config, &error);
    ASSERT_NE(manager, nullptr);

    // Recovery should read both files
    int recover_result = wal_manager_recover(manager, nullptr);
    EXPECT_EQ(recover_result, 0);

    // Cleanup
    buffer_destroy(data1);
    buffer_destroy(data2);
}

TEST_F(WalManagerTest, MigrateFromLegacyWal) {
    // Create legacy WAL file
    char* legacy_wal_path = path_join(temp_dir, "current.wal");
    int legacy_fd = open(legacy_wal_path, O_WRONLY | O_CREAT, 0644);
    ASSERT_GE(legacy_fd, 0);

    // Write some legacy entries
    // Note: For this test, we'll create an empty legacy file
    // A full migration test would use the existing wal_write() to create test data

    close(legacy_fd);
    free(legacy_wal_path);

    // Load with migration
    wal_recovery_options_t options;
    memset(&options, 0, sizeof(options));
    options.force_legacy = 0;
    options.force_migration = 0;
    options.rollback_on_failure = 1;
    options.keep_backup = 1;

    int error = 0;
    manager = wal_manager_load_with_options(temp_dir, &config, &options, &error);
    ASSERT_NE(manager, nullptr);
    EXPECT_EQ(error, 0);

    // Verify migration happened
    char* manifest_path = path_join(temp_dir, "manifest.dat");
    EXPECT_EQ(access(manifest_path, F_OK), 0);  // Manifest exists
    free(manifest_path);

    // Verify thread WAL was created
    thread_wal_t* twal = get_thread_wal(manager);
    ASSERT_NE(twal, nullptr);
}
