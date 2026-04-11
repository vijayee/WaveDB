//
// Test for identifier_t using Google Test
//

#include <gtest/gtest.h>
#include "HBTrie/identifier.h"
#include "Buffer/buffer.h"

class IdentifierTest : public ::testing::Test {
protected:
    void SetUp() override {
    }

    void TearDown() override {
    }
};

TEST_F(IdentifierTest, Create) {
    const char* data = "hello world";
    size_t len = strlen(data);

    buffer_t* buf = buffer_create_from_pointer_copy((uint8_t*)data, len);
    ASSERT_NE(buf, nullptr);

    identifier_t* id = identifier_create(buf, 4);
    ASSERT_NE(id, nullptr);
    EXPECT_EQ(id->length, len);

    // Calculate expected chunks
    size_t expected_chunks = (len - 1) / 4 + 1;
    EXPECT_EQ(identifier_chunk_count(id), expected_chunks);

    buffer_destroy(buf);
    identifier_destroy(id);
}

TEST_F(IdentifierTest, ChunkCalculation) {
    struct {
        size_t length;
        size_t chunk_size;
        size_t expected_chunks;
    } tests[] = {
        {1, 4, 1},
        {4, 4, 1},
        {5, 4, 2},
        {8, 4, 2},
        {9, 4, 3},
        {0, 4, 1},
        {16, 8, 2},
    };

    for (const auto& test : tests) {
        size_t nchunk = identifier_calc_nchunk(test.length, test.chunk_size);
        EXPECT_EQ(nchunk, test.expected_chunks)
            << "nchunk(" << test.length << ", " << test.chunk_size << ")";
    }
}

TEST_F(IdentifierTest, Compare) {
    buffer_t* buf_a = buffer_create_from_pointer_copy((uint8_t*)"hello", 5);
    buffer_t* buf_b = buffer_create_from_pointer_copy((uint8_t*)"hello", 5);
    buffer_t* buf_c = buffer_create_from_pointer_copy((uint8_t*)"world", 5);

    ASSERT_NE(buf_a, nullptr);
    ASSERT_NE(buf_b, nullptr);
    ASSERT_NE(buf_c, nullptr);

    identifier_t* a = identifier_create(buf_a, 4);
    identifier_t* b = identifier_create(buf_b, 4);
    identifier_t* c = identifier_create(buf_c, 4);

    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    ASSERT_NE(c, nullptr);

    EXPECT_EQ(identifier_compare(a, b), 0);
    EXPECT_LT(identifier_compare(a, c), 0);
    EXPECT_GT(identifier_compare(c, a), 0);

    buffer_destroy(buf_a);
    buffer_destroy(buf_b);
    buffer_destroy(buf_c);
    identifier_destroy(a);
    identifier_destroy(b);
    identifier_destroy(c);
}

TEST_F(IdentifierTest, ToBuffer) {
    const char* data = "hello";
    size_t len = strlen(data);

    buffer_t* buf = buffer_create_from_pointer_copy((uint8_t*)data, len);
    ASSERT_NE(buf, nullptr);

    identifier_t* id = identifier_create(buf, 4);
    ASSERT_NE(id, nullptr);

    buffer_t* result = identifier_to_buffer(id);
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result->size, len);
    EXPECT_EQ(memcmp(result->data, data, len), 0);

    buffer_destroy(buf);
    buffer_destroy(result);
    identifier_destroy(id);
}

TEST_F(IdentifierTest, CreateEmpty) {
    identifier_t* id = identifier_create_empty(4);
    ASSERT_NE(id, nullptr);
    EXPECT_EQ(id->length, 0u);
    EXPECT_EQ(identifier_chunk_count(id), 1u);  // One empty chunk

    identifier_destroy(id);
}

TEST_F(IdentifierTest, ChunkAccess) {
    const char* data = "abcdefghij";  // 10 bytes
    buffer_t* buf = buffer_create_from_pointer_copy((uint8_t*)data, 10);
    ASSERT_NE(buf, nullptr);

    identifier_t* id = identifier_create(buf, 4);
    ASSERT_NE(id, nullptr);

    // Should have 3 chunks: "abcd", "efgh", "ij\0\0"
    EXPECT_EQ(identifier_chunk_count(id), 3u);

    chunk_t* chunk0 = identifier_get_chunk(id, 0);
    chunk_t* chunk1 = identifier_get_chunk(id, 1);
    chunk_t* chunk2 = identifier_get_chunk(id, 2);

    ASSERT_NE(chunk0, nullptr);
    ASSERT_NE(chunk1, nullptr);
    ASSERT_NE(chunk2, nullptr);

    // Verify content
    EXPECT_EQ(memcmp(chunk0->data, "abcd", 4), 0);
    EXPECT_EQ(memcmp(chunk1->data, "efgh", 4), 0);
    // Last chunk: "ij" + 2 nulls
    EXPECT_EQ(chunk2->data[0], 'i');
    EXPECT_EQ(chunk2->data[1], 'j');

    buffer_destroy(buf);
    identifier_destroy(id);
}

TEST_F(IdentifierTest, ReferenceCounting) {
    buffer_t* buf = buffer_create_from_pointer_copy((uint8_t*)"test", 4);
    ASSERT_NE(buf, nullptr);

    identifier_t* id = identifier_create(buf, 4);
    ASSERT_NE(id, nullptr);

    EXPECT_EQ(refcounter_count((refcounter_t*)id), 1u);

    identifier_t* ref = (identifier_t*)refcounter_reference((refcounter_t*)id);
    EXPECT_EQ(ref, id);
    EXPECT_EQ(refcounter_count((refcounter_t*)id), 2u);

    refcounter_dereference((refcounter_t*)id);
    EXPECT_EQ(refcounter_count((refcounter_t*)id), 1u);

    buffer_destroy(buf);
    identifier_destroy(id);
}