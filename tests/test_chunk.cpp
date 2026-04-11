//
// Test for chunk_t using Google Test
//

#include <gtest/gtest.h>
#include "HBTrie/chunk.h"

class ChunkTest : public ::testing::Test {
protected:
    void SetUp() override {
    }

    void TearDown() override {
    }
};

TEST_F(ChunkTest, Create) {
    const char* data = "test";
    chunk_t* chunk = chunk_create(data, 4);
    ASSERT_NE(chunk, nullptr);
    ASSERT_EQ(chunk->size, 4u);
    EXPECT_EQ(memcmp(chunk->data, "test", 4), 0);
    chunk_destroy(chunk);
}

TEST_F(ChunkTest, CreateEmpty) {
    chunk_t* chunk = chunk_create_empty(4);
    ASSERT_NE(chunk, nullptr);
    ASSERT_EQ(chunk->size, 4u);
    // Should be zero-initialized
    for (size_t i = 0; i < 4; i++) {
        EXPECT_EQ(chunk->data[i], 0);
    }
    chunk_destroy(chunk);
}

TEST_F(ChunkTest, Compare) {
    chunk_t* a = chunk_create("abcd", 4);
    chunk_t* b = chunk_create("abcd", 4);
    chunk_t* c = chunk_create("abce", 4);

    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    ASSERT_NE(c, nullptr);

    EXPECT_EQ(chunk_compare(a, b), 0);
    EXPECT_LT(chunk_compare(a, c), 0);
    EXPECT_GT(chunk_compare(c, a), 0);

    chunk_destroy(a);
    chunk_destroy(b);
    chunk_destroy(c);
}

TEST_F(ChunkTest, CompareDifferentSizes) {
    chunk_t* a = chunk_create("abc", 4);
    chunk_t* b = chunk_create("abcd", 4);

    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);

    // Both chunks are size 4, so comparison should work
    // 'abc\0' vs 'abcd'
    EXPECT_LT(chunk_compare(a, b), 0);

    chunk_destroy(a);
    chunk_destroy(b);
}

TEST_F(ChunkTest, DataAccess) {
    chunk_t* chunk = chunk_create("hello", 5);
    ASSERT_NE(chunk, nullptr);

    void* data = chunk_data(chunk);
    ASSERT_NE(data, nullptr);

    const void* const_data = chunk_data_const(chunk);
    ASSERT_NE(const_data, nullptr);

    EXPECT_EQ(memcmp(data, "hello", 5), 0);

    chunk_destroy(chunk);
}

TEST_F(ChunkTest, ShareAndDestroy) {
    chunk_t* original = chunk_create("test", 4);
    ASSERT_NE(original, nullptr);

    chunk_t* shared = chunk_share(original);
    ASSERT_NE(shared, nullptr);

    // Shared should be the same pointer (refcounted)
    EXPECT_EQ(shared, original);

    // Data should be accessible from both
    EXPECT_EQ(memcmp(original->data, "test", 4), 0);
    EXPECT_EQ(memcmp(shared->data, "test", 4), 0);

    // Destroy shared - should just decrement refcount
    chunk_destroy(shared);

    // Original should still be valid
    EXPECT_EQ(memcmp(original->data, "test", 4), 0);

    chunk_destroy(original);
}