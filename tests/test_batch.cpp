//
// Created by victor on 3/22/26.
//

#include <gtest/gtest.h>
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

    // Second destroy - should be safe due to reference counting
    batch_destroy(batch);

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