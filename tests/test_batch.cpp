//
// Created by victor on 3/22/26.
//

#include <gtest/gtest.h>
#include <thread>
#include <atomic>
#include <vector>
#include <cstring>
#include "Database/batch.h"
#include "HBTrie/path.h"
#include "HBTrie/identifier.h"
#include "HBTrie/chunk.h"

class BatchTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Setup code if needed
    }

    void TearDown() override {
        // Cleanup code if needed
    }
};

TEST_F(BatchTest, CreateWithReserveCount) {
    // Create batch with specific reserve count
    size_t reserve_count = 100;
    batch_t* batch = batch_create(reserve_count);

    ASSERT_NE(batch, nullptr);
    EXPECT_EQ(batch->count, 0);
    EXPECT_EQ(batch->capacity, reserve_count);
    EXPECT_EQ(batch->max_size, BATCH_DEFAULT_MAX_SIZE);
    EXPECT_EQ(batch->estimated_size, 0);
    EXPECT_EQ(batch->submitted, 0);
    EXPECT_NE(batch->ops, nullptr);

    batch_destroy(batch);
}

TEST_F(BatchTest, CreateWithZero) {
    // Create batch with 0 (should use default capacity)
    batch_t* batch = batch_create(0);

    ASSERT_NE(batch, nullptr);
    EXPECT_EQ(batch->count, 0);
    EXPECT_GT(batch->capacity, 0);  // Should have default capacity
    EXPECT_EQ(batch->max_size, BATCH_DEFAULT_MAX_SIZE);
    EXPECT_EQ(batch->estimated_size, 0);
    EXPECT_EQ(batch->submitted, 0);
    EXPECT_NE(batch->ops, nullptr);

    batch_destroy(batch);
}

TEST_F(BatchTest, DestroyWithOperations) {
    // Create batch
    batch_t* batch = batch_create(10);
    ASSERT_NE(batch, nullptr);

    // Create test path and value using helper functions
    buffer_t* buf1 = buffer_create_from_pointer_copy((uint8_t*)"test", 4);
    ASSERT_NE(buf1, nullptr);
    identifier_t* value = identifier_create(buf1, 0);
    ASSERT_NE(value, nullptr);
    buffer_destroy(buf1);

    buffer_t* buf2 = buffer_create_from_pointer_copy((uint8_t*)"key", 3);
    ASSERT_NE(buf2, nullptr);
    identifier_t* id = identifier_create(buf2, 0);
    ASSERT_NE(id, nullptr);
    buffer_destroy(buf2);

    path_t* path = path_create();
    ASSERT_NE(path, nullptr);
    path_append(path, id);
    identifier_destroy(id);

    // Manually add operation to batch for testing destroy
    // Note: batch_add_put and batch_add_delete will be implemented in next task
    // For now, we manually populate to test destroy
    batch->ops[0].type = WAL_PUT;
    batch->ops[0].path = path;
    batch->ops[0].value = value;
    batch->count = 1;

    // Destroy batch - should clean up path and value
    batch_destroy(batch);

    // If batch_destroy worked correctly, path and value are freed
    // Test passes if no crash/double-free occurs
    EXPECT_TRUE(true);
}

TEST_F(BatchTest, DestroyNullBatch) {
    // Destroy NULL batch - should not crash
    batch_destroy(nullptr);
    EXPECT_TRUE(true);  // If we get here, the test passed
}

TEST_F(BatchTest, MultipleDestroyCalls) {
    // Create batch
    batch_t* batch = batch_create(10);
    ASSERT_NE(batch, nullptr);

    // First destroy
    batch_destroy(batch);

    // Second destroy on NULL is safe (batch_destroy handles NULL)
    // Note: Calling batch_destroy twice on the same pointer is a use-after-free,
    // not a reference counting feature. To properly test reference counting,
    // use batch_reference() before the second destroy.
    batch_destroy(nullptr);

    EXPECT_TRUE(true);  // If we get here, the test passed
}

TEST_F(BatchTest, ReferenceCounting) {
    // Create batch
    batch_t* batch = batch_create(10);
    ASSERT_NE(batch, nullptr);

    // Get initial reference count
    EXPECT_EQ(refcounter_count((refcounter_t*)batch), 1);

    // Dereference (but don't destroy yet)
    refcounter_dereference((refcounter_t*)batch);
    EXPECT_EQ(refcounter_count((refcounter_t*)batch), 0);

    // Destroy should actually free the batch now
    batch_destroy(batch);
    // batch is now freed

    EXPECT_TRUE(true);  // If we get here, the test passed
}

TEST_F(BatchTest, CapacityGreaterThanReserveCount) {
    // Create batch with small reserve count
    batch_t* batch = batch_create(5);
    ASSERT_NE(batch, nullptr);
    EXPECT_EQ(batch->capacity, 5);

    batch_destroy(batch);
}

TEST_F(BatchTest, LargeReserveCount) {
    // Create batch with large reserve count
    batch_t* batch = batch_create(10000);
    ASSERT_NE(batch, nullptr);
    EXPECT_EQ(batch->capacity, 10000);

    batch_destroy(batch);
}

// Helper function to create a test path
static path_t* create_test_path(const char* key) {
    buffer_t* buf = buffer_create_from_pointer_copy((uint8_t*)key, strlen(key));
    if (buf == nullptr) return nullptr;

    identifier_t* id = identifier_create(buf, 0);
    buffer_destroy(buf);
    if (id == nullptr) return nullptr;

    path_t* path = path_create();
    if (path == nullptr) {
        identifier_destroy(id);
        return nullptr;
    }

    if (path_append(path, id) != 0) {
        identifier_destroy(id);
        path_destroy(path);
        return nullptr;
    }

    identifier_destroy(id);
    return path;
}

// Helper function to create a test value
static identifier_t* create_test_value(const char* value) {
    buffer_t* buf = buffer_create_from_pointer_copy((uint8_t*)value, strlen(value));
    if (buf == nullptr) return nullptr;

    identifier_t* id = identifier_create(buf, 0);
    buffer_destroy(buf);
    return id;
}

TEST_F(BatchTest, AddPutSuccess) {
    // Create batch
    batch_t* batch = batch_create(10);
    ASSERT_NE(batch, nullptr);

    // Create test path and value
    path_t* path = create_test_path("test_key");
    ASSERT_NE(path, nullptr);

    identifier_t* value = create_test_value("test_value");
    ASSERT_NE(value, nullptr);

    // Add PUT operation
    int result = batch_add_put(batch, path, value);
    EXPECT_EQ(result, 0);
    EXPECT_EQ(batch->count, 1);
    EXPECT_EQ(batch->ops[0].type, WAL_PUT);
    EXPECT_EQ(batch->ops[0].path, path);
    EXPECT_EQ(batch->ops[0].value, value);
    EXPECT_GT(batch->estimated_size, 0);

    // Destroy batch (will also destroy path and value)
    batch_destroy(batch);
}

TEST_F(BatchTest, AddDeleteSuccess) {
    // Create batch
    batch_t* batch = batch_create(10);
    ASSERT_NE(batch, nullptr);

    // Create test path
    path_t* path = create_test_path("test_key");
    ASSERT_NE(path, nullptr);

    // Add DELETE operation
    int result = batch_add_delete(batch, path);
    EXPECT_EQ(result, 0);
    EXPECT_EQ(batch->count, 1);
    EXPECT_EQ(batch->ops[0].type, WAL_DELETE);
    EXPECT_EQ(batch->ops[0].path, path);
    EXPECT_EQ(batch->ops[0].value, nullptr);
    EXPECT_GT(batch->estimated_size, 0);

    // Destroy batch
    batch_destroy(batch);
}

TEST_F(BatchTest, AddPutNullBatch) {
    // Create test path and value
    path_t* path = create_test_path("test_key");
    ASSERT_NE(path, nullptr);

    identifier_t* value = create_test_value("test_value");
    ASSERT_NE(value, nullptr);

    // Try to add to NULL batch
    int result = batch_add_put(nullptr, path, value);
    EXPECT_EQ(result, -1);

    // Ownership remains with caller, so we must destroy
    path_destroy(path);
    identifier_destroy(value);
}

TEST_F(BatchTest, AddPutNullPath) {
    // Create batch
    batch_t* batch = batch_create(10);
    ASSERT_NE(batch, nullptr);

    // Create test value
    identifier_t* value = create_test_value("test_value");
    ASSERT_NE(value, nullptr);

    // Try to add with NULL path
    int result = batch_add_put(batch, nullptr, value);
    EXPECT_EQ(result, -1);
    EXPECT_EQ(batch->count, 0);

    // Ownership remains with caller, so we must destroy
    identifier_destroy(value);
    batch_destroy(batch);
}

TEST_F(BatchTest, AddPutNullValue) {
    // Create batch
    batch_t* batch = batch_create(10);
    ASSERT_NE(batch, nullptr);

    // Create test path
    path_t* path = create_test_path("test_key");
    ASSERT_NE(path, nullptr);

    // Try to add with NULL value
    int result = batch_add_put(batch, path, nullptr);
    EXPECT_EQ(result, -1);
    EXPECT_EQ(batch->count, 0);

    // Ownership remains with caller, so we must destroy
    path_destroy(path);
    batch_destroy(batch);
}

TEST_F(BatchTest, AddDeleteNullBatch) {
    // Create test path
    path_t* path = create_test_path("test_key");
    ASSERT_NE(path, nullptr);

    // Try to add to NULL batch
    int result = batch_add_delete(nullptr, path);
    EXPECT_EQ(result, -1);

    // Ownership remains with caller, so we must destroy
    path_destroy(path);
}

TEST_F(BatchTest, AddDeleteNullPath) {
    // Create batch
    batch_t* batch = batch_create(10);
    ASSERT_NE(batch, nullptr);

    // Try to add with NULL path
    int result = batch_add_delete(batch, nullptr);
    EXPECT_EQ(result, -1);
    EXPECT_EQ(batch->count, 0);

    batch_destroy(batch);
}

TEST_F(BatchTest, AddWhenBatchFull) {
    // Create batch with small max size
    batch_t* batch = batch_create(2);
    ASSERT_NE(batch, nullptr);
    batch->max_size = 2; // Set a small max_size for testing

    // Add two operations
    path_t* path1 = create_test_path("key1");
    ASSERT_NE(path1, nullptr);
    identifier_t* value1 = create_test_value("value1");
    ASSERT_NE(value1, nullptr);

    EXPECT_EQ(batch_add_put(batch, path1, value1), 0);
    EXPECT_EQ(batch->count, 1);

    path_t* path2 = create_test_path("key2");
    ASSERT_NE(path2, nullptr);
    identifier_t* value2 = create_test_value("value2");
    ASSERT_NE(value2, nullptr);

    EXPECT_EQ(batch_add_put(batch, path2, value2), 0);
    EXPECT_EQ(batch->count, 2);

    // Try to add third operation - should fail
    path_t* path3 = create_test_path("key3");
    ASSERT_NE(path3, nullptr);
    identifier_t* value3 = create_test_value("value3");
    ASSERT_NE(value3, nullptr);

    int result = batch_add_put(batch, path3, value3);
    EXPECT_EQ(result, -2);
    EXPECT_EQ(batch->count, 2); // Count unchanged

    // Ownership remains with caller on error
    path_destroy(path3);
    identifier_destroy(value3);

    batch_destroy(batch);
}

TEST_F(BatchTest, AddWhenBatchSubmitted) {
    // Create batch
    batch_t* batch = batch_create(10);
    ASSERT_NE(batch, nullptr);

    // Mark batch as submitted
    batch->submitted = 1;

    // Create test path and value
    path_t* path = create_test_path("test_key");
    ASSERT_NE(path, nullptr);

    identifier_t* value = create_test_value("test_value");
    ASSERT_NE(value, nullptr);

    // Try to add operation - should fail
    int result = batch_add_put(batch, path, value);
    EXPECT_EQ(result, -6);
    EXPECT_EQ(batch->count, 0);

    // Ownership remains with caller on error
    path_destroy(path);
    identifier_destroy(value);

    batch_destroy(batch);
}

TEST_F(BatchTest, ConcurrentAddOperations) {
    // Create batch
    batch_t* batch = batch_create(100);
    ASSERT_NE(batch, nullptr);

    // Number of concurrent threads
    const int num_threads = 10;
    const int ops_per_thread = 100;
    std::vector<std::thread> threads;
    std::atomic<int> success_count{0};

    // Launch threads
    for (int t = 0; t < num_threads; t++) {
        threads.emplace_back([&, t]() {
            for (int i = 0; i < ops_per_thread; i++) {
                char key[32];
                snprintf(key, sizeof(key), "key_%d_%d", t, i);

                path_t* path = create_test_path(key);
                if (path == nullptr) continue;

                identifier_t* value = create_test_value("value");
                if (value == nullptr) {
                    path_destroy(path);
                    continue;
                }

                int result = batch_add_put(batch, path, value);
                if (result == 0) {
                    success_count++;
                } else {
                    // Ownership remains with caller on error
                    path_destroy(path);
                    identifier_destroy(value);
                }
            }
        });
    }

    // Wait for all threads
    for (auto& thread : threads) {
        thread.join();
    }

    // Verify all operations succeeded
    EXPECT_EQ(batch->count, static_cast<size_t>(num_threads * ops_per_thread));
    EXPECT_EQ(success_count.load(), num_threads * ops_per_thread);

    batch_destroy(batch);
}

TEST_F(BatchTest, MixedPutAndDelete) {
    // Create batch
    batch_t* batch = batch_create(10);
    ASSERT_NE(batch, nullptr);

    // Add PUT operation
    path_t* path1 = create_test_path("key1");
    ASSERT_NE(path1, nullptr);
    identifier_t* value1 = create_test_value("value1");
    ASSERT_NE(value1, nullptr);

    EXPECT_EQ(batch_add_put(batch, path1, value1), 0);
    EXPECT_EQ(batch->count, 1);

    // Add DELETE operation
    path_t* path2 = create_test_path("key2");
    ASSERT_NE(path2, nullptr);

    EXPECT_EQ(batch_add_delete(batch, path2), 0);
    EXPECT_EQ(batch->count, 2);

    // Verify operations
    EXPECT_EQ(batch->ops[0].type, WAL_PUT);
    EXPECT_EQ(batch->ops[0].path, path1);
    EXPECT_EQ(batch->ops[0].value, value1);

    EXPECT_EQ(batch->ops[1].type, WAL_DELETE);
    EXPECT_EQ(batch->ops[1].path, path2);
    EXPECT_EQ(batch->ops[1].value, nullptr);

    batch_destroy(batch);
}

TEST_F(BatchTest, EstimateSize) {
    // Create batch
    batch_t* batch = batch_create(10);
    ASSERT_NE(batch, nullptr);

    // Initially size should be 0
    EXPECT_EQ(batch_estimate_size(batch), 0);

    // Add operation
    path_t* path1 = create_test_path("key1");
    ASSERT_NE(path1, nullptr);
    identifier_t* value1 = create_test_value("value1");
    ASSERT_NE(value1, nullptr);

    EXPECT_EQ(batch_add_put(batch, path1, value1), 0);

    // Size should be > 0
    size_t size1 = batch_estimate_size(batch);
    EXPECT_GT(size1, 0);

    // Add another operation
    path_t* path2 = create_test_path("key2");
    ASSERT_NE(path2, nullptr);
    identifier_t* value2 = create_test_value("value2");
    ASSERT_NE(value2, nullptr);

    EXPECT_EQ(batch_add_put(batch, path2, value2), 0);

    // Size should increase
    size_t size2 = batch_estimate_size(batch);
    EXPECT_GT(size2, size1);

    batch_destroy(batch);
}

TEST_F(BatchTest, ArrayGrowth) {
    // Create batch with small capacity
    batch_t* batch = batch_create(2);
    ASSERT_NE(batch, nullptr);
    EXPECT_EQ(batch->capacity, 2);

    // Add operations beyond initial capacity
    for (int i = 0; i < 10; i++) {
        char key[32];
        snprintf(key, sizeof(key), "key_%d", i);

        path_t* path = create_test_path(key);
        ASSERT_NE(path, nullptr);

        identifier_t* value = create_test_value("value");
        ASSERT_NE(value, nullptr);

        EXPECT_EQ(batch_add_put(batch, path, value), 0);
        EXPECT_EQ(batch->count, static_cast<size_t>(i + 1));

        // Capacity should grow as needed
        EXPECT_GE(batch->capacity, batch->count);
    }

    // Verify all operations added
    EXPECT_EQ(batch->count, 10);

    batch_destroy(batch);
}

TEST_F(BatchTest, SizeEstimationAccuracy) {
    // Create batch
    batch_t* batch = batch_create(10);
    ASSERT_NE(batch, nullptr);

    // Create paths and values with known sizes
    const char* key_data = "test_key";
    const char* value_data = "test_value";

    path_t* path = create_test_path(key_data);
    ASSERT_NE(path, nullptr);

    identifier_t* value = create_test_value(value_data);
    ASSERT_NE(value, nullptr);

    // Add PUT operation
    EXPECT_EQ(batch_add_put(batch, path, value), 0);

    // Get estimated size
    size_t estimated_size = batch_estimate_size(batch);
    EXPECT_GT(estimated_size, 0);

    // Size should include:
    // - 1 byte for WAL entry type
    // - CBOR serialized path (array of identifiers)
    // - CBOR serialized value (identifier)
    // - 8 bytes for CRC32 + data_len header

    // Estimate should be reasonable (not too small, not too large)
    // For a single operation with short key/value, expect ~50-200 bytes
    EXPECT_GT(estimated_size, 20);
    EXPECT_LT(estimated_size, 500);

    batch_destroy(batch);
}

TEST_F(BatchTest, SizeIncreasesWithOperations) {
    // Create batch
    batch_t* batch = batch_create(10);
    ASSERT_NE(batch, nullptr);

    // Add first operation
    path_t* path1 = create_test_path("key1");
    ASSERT_NE(path1, nullptr);
    identifier_t* value1 = create_test_value("value1");
    ASSERT_NE(value1, nullptr);

    EXPECT_EQ(batch_add_put(batch, path1, value1), 0);
    size_t size1 = batch_estimate_size(batch);
    EXPECT_GT(size1, 0);

    // Add second operation with longer key/value
    path_t* path2 = create_test_path("longer_key_name");
    ASSERT_NE(path2, nullptr);
    identifier_t* value2 = create_test_value("longer_value_data");
    ASSERT_NE(value2, nullptr);

    EXPECT_EQ(batch_add_put(batch, path2, value2), 0);
    size_t size2 = batch_estimate_size(batch);

    // Size should increase with additional operation
    EXPECT_GT(size2, size1);

    // Add third operation
    path_t* path3 = create_test_path("key3");
    ASSERT_NE(path3, nullptr);
    identifier_t* value3 = create_test_value("value3");
    ASSERT_NE(value3, nullptr);

    EXPECT_EQ(batch_add_put(batch, path3, value3), 0);
    size_t size3 = batch_estimate_size(batch);

    // Size should continue increasing
    EXPECT_GT(size3, size2);

    batch_destroy(batch);
}

TEST_F(BatchTest, PutVsDeleteSizeDifference) {
    // Create two batches
    batch_t* batch_put = batch_create(10);
    ASSERT_NE(batch_put, nullptr);

    batch_t* batch_delete = batch_create(10);
    ASSERT_NE(batch_delete, nullptr);

    // Create same path for both
    const char* key = "test_key";

    path_t* path_put = create_test_path(key);
    ASSERT_NE(path_put, nullptr);
    identifier_t* value = create_test_value("test_value_data");
    ASSERT_NE(value, nullptr);

    path_t* path_delete = create_test_path(key);
    ASSERT_NE(path_delete, nullptr);

    // Add PUT operation
    EXPECT_EQ(batch_add_put(batch_put, path_put, value), 0);
    size_t put_size = batch_estimate_size(batch_put);

    // Add DELETE operation
    EXPECT_EQ(batch_add_delete(batch_delete, path_delete), 0);
    size_t delete_size = batch_estimate_size(batch_delete);

    // PUT should be larger than DELETE (includes value)
    EXPECT_GT(put_size, delete_size);

    // Difference should be roughly the size of the value CBOR serialization
    size_t difference = put_size - delete_size;
    EXPECT_GT(difference, 10);  // Should be at least the value data

    batch_destroy(batch_put);
    batch_destroy(batch_delete);
}

TEST_F(BatchTest, SizeEstimationWithLargeValue) {
    // Create batch
    batch_t* batch = batch_create(10);
    ASSERT_NE(batch, nullptr);

    // Create path with short key
    path_t* path = create_test_path("key");
    ASSERT_NE(path, nullptr);

    // Create value with large data (1KB)
    const size_t large_size = 1024;
    char* large_value = new char[large_size];
    memset(large_value, 'X', large_size - 1);
    large_value[large_size - 1] = '\0';

    buffer_t* buf = buffer_create_from_pointer_copy((uint8_t*)large_value, large_size - 1);
    delete[] large_value;
    ASSERT_NE(buf, nullptr);

    identifier_t* value = identifier_create(buf, 0);
    buffer_destroy(buf);
    ASSERT_NE(value, nullptr);

    // Add PUT operation
    EXPECT_EQ(batch_add_put(batch, path, value), 0);

    // Size should be significantly larger due to large value
    size_t estimated_size = batch_estimate_size(batch);
    EXPECT_GT(estimated_size, large_size);  // Should be at least the data size
    EXPECT_GT(estimated_size, 1000);  // Should be well over 1KB

    batch_destroy(batch);
}

TEST_F(BatchTest, SizeEstimationMultiplePaths) {
    // Create batch
    batch_t* batch = batch_create(10);
    ASSERT_NE(batch, nullptr);

    // Create path with multiple subscripts
    path_t* path = path_create();
    ASSERT_NE(path, nullptr);

    // Add multiple identifiers to the path
    for (int i = 0; i < 5; i++) {
        char component[32];
        snprintf(component, sizeof(component), "component_%d", i);

        buffer_t* buf = buffer_create_from_pointer_copy((uint8_t*)component, strlen(component));
        ASSERT_NE(buf, nullptr);

        identifier_t* id = identifier_create(buf, 0);
        buffer_destroy(buf);
        ASSERT_NE(id, nullptr);

        EXPECT_EQ(path_append(path, id), 0);
        identifier_destroy(id);
    }

    // Create value
    identifier_t* value = create_test_value("test_value");
    ASSERT_NE(value, nullptr);

    // Add PUT operation
    EXPECT_EQ(batch_add_put(batch, path, value), 0);

    // Size should account for all path components
    size_t estimated_size = batch_estimate_size(batch);
    EXPECT_GT(estimated_size, 70);  // Should be substantial with 5 path components

    batch_destroy(batch);
}
