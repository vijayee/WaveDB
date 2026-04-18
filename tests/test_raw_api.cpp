#include <gtest/gtest.h>
extern "C" {
#include "HBTrie/identifier.h"
#include "HBTrie/chunk.h"
#include "Buffer/buffer.h"
}

TEST(RawIdentifierTest, CreateFromRawBasic) {
    const uint8_t data[] = "hello";
    identifier_t* id = identifier_create_from_raw(data, 5, 0);
    ASSERT_NE(id, nullptr);
    EXPECT_EQ(id->length, 5u);
    EXPECT_EQ(identifier_chunk_count(id), 2u);

    size_t len;
    uint8_t* out = identifier_get_data_copy(id, &len);
    ASSERT_NE(out, nullptr);
    EXPECT_EQ(len, 5u);
    EXPECT_EQ(memcmp(out, "hello", 5), 0);
    free(out);
    identifier_destroy(id);
}

TEST(RawIdentifierTest, CreateFromRawEmpty) {
    identifier_t* id = identifier_create_from_raw(NULL, 0, 0);
    ASSERT_NE(id, nullptr);
    EXPECT_EQ(id->length, 0u);
    EXPECT_EQ(identifier_chunk_count(id), 0u);
    identifier_destroy(id);
}

TEST(RawIdentifierTest, CreateFromRawSingleChunk) {
    const uint8_t data[] = "abc";
    identifier_t* id = identifier_create_from_raw(data, 3, 4);
    ASSERT_NE(id, nullptr);
    EXPECT_EQ(id->length, 3u);
    EXPECT_EQ(identifier_chunk_count(id), 1u);
    size_t len;
    uint8_t* out = identifier_get_data_copy(id, &len);
    EXPECT_EQ(len, 3u);
    EXPECT_EQ(memcmp(out, "abc", 3), 0);
    free(out);
    identifier_destroy(id);
}

TEST(RawIdentifierTest, CreateFromRawMatchesIdentifierCreate) {
    const uint8_t data[] = "test_value_1234";
    size_t data_len = 15;
    buffer_t* buf = buffer_create_from_pointer_copy((uint8_t*)data, data_len);
    identifier_t* id_old = identifier_create(buf, 0);
    buffer_destroy(buf);
    identifier_t* id_new = identifier_create_from_raw(data, data_len, 0);
    ASSERT_NE(id_old, nullptr);
    ASSERT_NE(id_new, nullptr);
    EXPECT_EQ(id_old->length, id_new->length);
    EXPECT_EQ(identifier_chunk_count(id_old), identifier_chunk_count(id_new));
    EXPECT_EQ(identifier_compare(id_old, id_new), 0);
    identifier_destroy(id_old);
    identifier_destroy(id_new);
}

TEST(RawIdentifierTest, GetDataCopyMultiChunk) {
    const uint8_t data[] = "0123456789";
    identifier_t* id = identifier_create_from_raw(data, 10, 4);
    ASSERT_NE(id, nullptr);
    size_t len;
    uint8_t* out = identifier_get_data_copy(id, &len);
    EXPECT_EQ(len, 10u);
    EXPECT_EQ(memcmp(out, "0123456789", 10), 0);
    free(out);
    identifier_destroy(id);
}