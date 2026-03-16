//
// Test for variable-size section storage with transaction IDs
//

#include <gtest/gtest.h>
#include <fcntl.h>
#include <unistd.h>
#include "Storage/section.h"
#include "Workers/transaction_id.h"

class SectionVariableTest : public ::testing::Test {
protected:
    char* test_dir;
    char* data_path;
    char* meta_path;

    void SetUp() override {
        // Initialize transaction ID generator
        transaction_id_init();

        // Create temporary directory
        test_dir = strdup("/tmp/section_test_XXXXXX");
        mkdtemp(test_dir);

        data_path = path_join(test_dir, "data");
        meta_path = path_join(test_dir, "meta");
        mkdir_p(data_path);
        mkdir_p(meta_path);
    }

    void TearDown() override {
        // Clean up
        char cmd[512];
        snprintf(cmd, sizeof(cmd), "rm -rf %s", test_dir);
        system(cmd);
        free(test_dir);
        free(data_path);
        free(meta_path);
    }

    // Helper to create a path
    char* path_join(const char* dir, const char* file) {
        size_t len = strlen(dir) + strlen(file) + 2;
        char* path = (char*)malloc(len);
        snprintf(path, len, "%s/%s", dir, file);
        return path;
    }

    // Helper to create directory
    int mkdir_p(const char* path) {
        char tmp[512];
        snprintf(tmp, sizeof(tmp), "%s", path);
        for (char* p = tmp + 1; *p; p++) {
            if (*p == '/') {
                *p = 0;
                mkdir(tmp, 0755);
                *p = '/';
            }
        }
        return mkdir(tmp, 0755);
    }
};

TEST_F(SectionVariableTest, WriteReadVariableSize) {
    // Create section with 1MB size
    section_t* section = section_create(data_path, meta_path, 1024 * 1024, 0);
    ASSERT_NE(section, nullptr);

    // Test different sizes
    const char* test_data1 = "Hello";
    const char* test_data2 = "This is a longer test string for variable-size storage";
    const char* test_data3 = "Tiny";

    buffer_t* buf1 = buffer_create_from_existing_memory((uint8_t*)strdup(test_data1), strlen(test_data1));
    buffer_t* buf2 = buffer_create_from_existing_memory((uint8_t*)strdup(test_data2), strlen(test_data2));
    buffer_t* buf3 = buffer_create_from_existing_memory((uint8_t*)strdup(test_data3), strlen(test_data3));

    // Write with transaction IDs
    transaction_id_t txn_id1 = transaction_id_get_next();
    transaction_id_t txn_id2 = transaction_id_get_next();
    transaction_id_t txn_id3 = transaction_id_get_next();

    size_t offset1, offset2, offset3;
    uint8_t full;

    int result = section_write(section, txn_id1, buf1, &offset1, &full);
    ASSERT_EQ(result, 0);
    EXPECT_FALSE(full);

    result = section_write(section, txn_id2, buf2, &offset2, &full);
    ASSERT_EQ(result, 0);
    EXPECT_FALSE(full);

    result = section_write(section, txn_id3, buf3, &offset3, &full);
    ASSERT_EQ(result, 0);
    EXPECT_FALSE(full);

    // Read back and verify
    transaction_id_t read_txn_id;
    buffer_t* read_buf;

    result = section_read(section, offset1, &read_txn_id, &read_buf);
    ASSERT_EQ(result, 0);
    EXPECT_EQ(transaction_id_compare(&read_txn_id, &txn_id1), 0);
    EXPECT_EQ(read_buf->size, strlen(test_data1));
    EXPECT_EQ(memcmp(read_buf->data, test_data1, strlen(test_data1)), 0);
    buffer_destroy(read_buf);

    result = section_read(section, offset2, &read_txn_id, &read_buf);
    ASSERT_EQ(result, 0);
    EXPECT_EQ(transaction_id_compare(&read_txn_id, &txn_id2), 0);
    EXPECT_EQ(read_buf->size, strlen(test_data2));
    EXPECT_EQ(memcmp(read_buf->data, test_data2, strlen(test_data2)), 0);
    buffer_destroy(read_buf);

    result = section_read(section, offset3, &read_txn_id, &read_buf);
    ASSERT_EQ(result, 0);
    EXPECT_EQ(transaction_id_compare(&read_txn_id, &txn_id3), 0);
    EXPECT_EQ(read_buf->size, strlen(test_data3));
    EXPECT_EQ(memcmp(read_buf->data, test_data3, strlen(test_data3)), 0);
    buffer_destroy(read_buf);

    buffer_destroy(buf1);
    buffer_destroy(buf2);
    buffer_destroy(buf3);
    section_destroy(section);
}

TEST_F(SectionVariableTest, TransactionIDPreservation) {
    section_t* section = section_create(data_path, meta_path, 1024 * 1024, 0);
    ASSERT_NE(section, nullptr);

    // Generate transaction IDs
    transaction_id_t txn_ids[5];
    size_t offsets[5];

    for (int i = 0; i < 5; i++) {
        txn_ids[i] = transaction_id_get_next();

        char data[32];
        snprintf(data, sizeof(data), "Data block %d", i);
        buffer_t* buf = buffer_create_from_existing_memory((uint8_t*)strdup(data), strlen(data));

        uint8_t full;
        int result = section_write(section, txn_ids[i], buf, &offsets[i], &full);
        ASSERT_EQ(result, 0);

        buffer_destroy(buf);
    }

    // Verify transaction IDs are preserved
    for (int i = 0; i < 5; i++) {
        transaction_id_t read_txn_id;
        buffer_t* read_buf;

        int result = section_read(section, offsets[i], &read_txn_id, &read_buf);
        ASSERT_EQ(result, 0);
        EXPECT_EQ(transaction_id_compare(&read_txn_id, &txn_ids[i]), 0);

        buffer_destroy(read_buf);
    }

    section_destroy(section);
}

TEST_F(SectionVariableTest, FragmentManagement) {
    section_t* section = section_create(data_path, meta_path, 1024, 0);  // Small section for testing
    ASSERT_NE(section, nullptr);

    // Write multiple small blocks
    transaction_id_t txn_id;
    buffer_t* buf;
    size_t offset;
    uint8_t full;

    for (int i = 0; i < 10; i++) {
        txn_id = transaction_id_get_next();
        buf = buffer_create_from_existing_memory((uint8_t*)strdup("X"), 1);

        int result = section_write(section, txn_id, buf, &offset, &full);
        ASSERT_EQ(result, 0);
        buffer_destroy(buf);
    }

    // Deallocate some blocks (need to know exact offset, which is 33 bytes per record: 24 + 8 + 1)
    // The first 10 records would be at offsets: 0, 33, 66, 99, 132, 165, 198, 231, 264, 297
    int result = section_deallocate(section, 33, 1);  // Deallocate second record
    ASSERT_EQ(result, 0);

    result = section_deallocate(section, 99, 1);  // Deallocate fourth record
    ASSERT_EQ(result, 0);

    // Write new data - should reuse fragmented space
    txn_id = transaction_id_get_next();
    buf = buffer_create_from_existing_memory((uint8_t*)strdup("Y"), 1);
    result = section_write(section, txn_id, buf, &offset, &full);
    ASSERT_EQ(result, 0);

    // Verify the new data was written to a fragmented space
    // (Either at offset 33 or 99, depending on first-fit algorithm)
    EXPECT_TRUE(offset == 33 || offset == 99);

    buffer_destroy(buf);
    section_destroy(section);
}

TEST_F(SectionVariableTest, LargeData) {
    section_t* section = section_create(data_path, meta_path, 1024 * 1024, 0);
    ASSERT_NE(section, nullptr);

    // Write a large buffer (10KB)
    const size_t large_size = 10 * 1024;
    uint8_t* large_data = (uint8_t*)malloc(large_size);
    memset(large_data, 0xAB, large_size);

    buffer_t* buf = buffer_create_from_existing_memory(large_data, large_size);
    transaction_id_t txn_id = transaction_id_get_next();

    size_t offset;
    uint8_t full;
    int result = section_write(section, txn_id, buf, &offset, &full);
    ASSERT_EQ(result, 0);
    EXPECT_FALSE(full);

    // Read back
    transaction_id_t read_txn_id;
    buffer_t* read_buf;
    result = section_read(section, offset, &read_txn_id, &read_buf);
    ASSERT_EQ(result, 0);
    EXPECT_EQ(transaction_id_compare(&read_txn_id, &txn_id), 0);
    EXPECT_EQ(read_buf->size, large_size);
    EXPECT_EQ(memcmp(read_buf->data, large_data, large_size), 0);

    buffer_destroy(buf);
    buffer_destroy(read_buf);
    section_destroy(section);
}

TEST_F(SectionVariableTest, FullSection) {
    // Create a very small section (100 bytes)
    section_t* section = section_create(data_path, meta_path, 100, 0);
    ASSERT_NE(section, nullptr);

    // Try to write more than the section can hold
    buffer_t* buf = buffer_create_from_existing_memory((uint8_t*)strdup("test"), 4);
    transaction_id_t txn_id = transaction_id_get_next();

    size_t offset;
    uint8_t full;

    // First write should succeed (24 + 8 + 4 = 36 bytes used)
    int result = section_write(section, txn_id, buf, &offset, &full);
    ASSERT_EQ(result, 0);
    EXPECT_FALSE(full);

    // Second write should succeed (72 bytes used)
    txn_id = transaction_id_get_next();
    result = section_write(section, txn_id, buf, &offset, &full);
    ASSERT_EQ(result, 0);

    // Third write should fail (would exceed 100 bytes)
    txn_id = transaction_id_get_next();
    result = section_write(section, txn_id, buf, &offset, &full);
    EXPECT_NE(result, 0);  // Should return error

    buffer_destroy(buf);
    section_destroy(section);
}

TEST_F(SectionVariableTest, Persistence) {
    const char* test_data = "Persistent data test";

    // Create section and write data
    section_t* section = section_create(data_path, meta_path, 1024 * 1024, 0);
    ASSERT_NE(section, nullptr);

    transaction_id_t txn_id = transaction_id_get_next();
    buffer_t* buf = buffer_create_from_existing_memory((uint8_t*)strdup(test_data), strlen(test_data));

    size_t offset;
    uint8_t full;
    int result = section_write(section, txn_id, buf, &offset, &full);
    ASSERT_EQ(result, 0);

    buffer_destroy(buf);
    section_destroy(section);

    // Reload section
    section = section_create(data_path, meta_path, 1024 * 1024, 0);
    ASSERT_NE(section, nullptr);

    // Read back and verify
    transaction_id_t read_txn_id;
    buffer_t* read_buf;
    result = section_read(section, offset, &read_txn_id, &read_buf);
    ASSERT_EQ(result, 0);
    EXPECT_EQ(transaction_id_compare(&read_txn_id, &txn_id), 0);
    EXPECT_EQ(read_buf->size, strlen(test_data));
    EXPECT_EQ(memcmp(read_buf->data, test_data, strlen(test_data)), 0);

    buffer_destroy(read_buf);
    section_destroy(section);
}