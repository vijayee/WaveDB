#include <gtest/gtest.h>
#include "Storage/node_serializer.h"
#include "HBTrie/bnode.h"
#include "HBTrie/chunk.h"
#include "HBTrie/identifier.h"
#include "Buffer/buffer.h"
#include <cstring>

class V3SerializerTest : public ::testing::Test {
protected:
    uint8_t chunk_size = 4;
    uint32_t btree_node_size = 4096;

    chunk_t* make_chunk(const char* data) {
        return chunk_create((const uint8_t*)data, chunk_size);
    }

    identifier_t* make_ident(const char* data) {
        size_t len = strlen(data);
        buffer_t* buf = buffer_create(len);
        memcpy(buf->data, data, len);
        buf->size = len;
        identifier_t* ident = identifier_create(buf, chunk_size);
        buffer_destroy(buf);
        return ident;
    }
};

// Test 1: Serialize/deserialize a leaf bnode with value entries
TEST_F(V3SerializerTest, LeafNodeWithValueRoundTrip) {
    bnode_t* node = bnode_create_with_level(btree_node_size, 1);
    ASSERT_NE(node, nullptr);

    // Add entry with value
    bnode_entry_t entry1;
    memset(&entry1, 0, sizeof(entry1));
    chunk_t* key1 = make_chunk("abcd");
    bnode_entry_set_key(&entry1, key1);
    entry1.has_value = 1;
    entry1.value = make_ident("value1");
    bnode_insert(node, &entry1);
    chunk_destroy(key1);

    // Add another entry with value
    bnode_entry_t entry2;
    memset(&entry2, 0, sizeof(entry2));
    chunk_t* key2 = make_chunk("efgh");
    bnode_entry_set_key(&entry2, key2);
    entry2.has_value = 1;
    entry2.value = make_ident("value2");
    bnode_insert(node, &entry2);
    chunk_destroy(key2);

    // Serialize with V3
    uint8_t* buf = nullptr;
    size_t len = 0;
    int rc = bnode_serialize_v3(node, chunk_size, &buf, &len);
    EXPECT_EQ(rc, 0);
    EXPECT_GT(len, 0u);
    EXPECT_NE(buf, nullptr);

    // Verify magic byte
    EXPECT_EQ(buf[0], 0xB5);

    // Deserialize
    node_location_t* locations = nullptr;
    size_t num_locations = 0;
    bnode_t* result = bnode_deserialize_v3(buf, len, chunk_size, btree_node_size,
                                            &locations, &num_locations);
    ASSERT_NE(result, nullptr);
    uint16_t result_level = result->level;
    EXPECT_EQ(result_level, 1u);
    EXPECT_EQ(result->entries.length, 2u);

    // Verify value entries have child_disk_offset = 0 (no children)
    EXPECT_EQ(locations[0].offset, 0u);
    EXPECT_EQ(locations[1].offset, 0u);

    // Verify the values survived round-trip
    bnode_entry_t* r_entry1 = bnode_find(result, make_chunk("abcd"), nullptr);
    ASSERT_NE(r_entry1, nullptr);
    EXPECT_EQ(r_entry1->has_value, 1);

    free(locations);
    free(buf);
    bnode_destroy(result);
    bnode_destroy(node);
}

// Test 2: Serialize/deserialize internal bnode with child_disk_offset
TEST_F(V3SerializerTest, InternalNodeWithChildOffsetRoundTrip) {
    bnode_t* node = bnode_create_with_level(btree_node_size, 2);
    ASSERT_NE(node, nullptr);

    // Add entry with child bnode offset
    bnode_entry_t entry1;
    memset(&entry1, 0, sizeof(entry1));
    chunk_t* key1 = make_chunk("abcd");
    bnode_entry_set_key(&entry1, key1);
    entry1.is_bnode_child = 1;
    entry1.child_disk_offset = 4096;  // Points to child bnode at offset 4096
    bnode_insert(node, &entry1);
    chunk_destroy(key1);

    // Add entry with hbtrie_node child offset
    bnode_entry_t entry2;
    memset(&entry2, 0, sizeof(entry2));
    chunk_t* key2 = make_chunk("efgh");
    bnode_entry_set_key(&entry2, key2);
    entry2.child_disk_offset = 8192;  // Points to child hbtrie_node at offset 8192
    bnode_insert(node, &entry2);
    chunk_destroy(key2);

    // Serialize with V3
    uint8_t* buf = nullptr;
    size_t len = 0;
    int rc = bnode_serialize_v3(node, chunk_size, &buf, &len);
    EXPECT_EQ(rc, 0);

    // Deserialize
    node_location_t* locations = nullptr;
    size_t num_locations = 0;
    bnode_t* result = bnode_deserialize_v3(buf, len, chunk_size, btree_node_size,
                                            &locations, &num_locations);
    ASSERT_NE(result, nullptr);
    uint16_t result_level2 = result->level;
    EXPECT_EQ(result_level2, 2u);
    EXPECT_EQ(result->entries.length, 2u);

    // Verify child_disk_offset values preserved
    EXPECT_EQ(locations[0].offset, 4096u);  // bnode_child offset
    EXPECT_EQ(locations[1].offset, 8192u);  // hbtrie_node offset

    // Verify entries have child_disk_offset set, pointers NULL (lazy)
    bnode_entry_t* r_entry1 = bnode_find(result, make_chunk("abcd"), nullptr);
    ASSERT_NE(r_entry1, nullptr);
    EXPECT_EQ(r_entry1->is_bnode_child, 1);
    EXPECT_EQ(r_entry1->child_disk_offset, 4096u);
    EXPECT_EQ(r_entry1->child_bnode, nullptr);  // Lazy: not loaded

    bnode_entry_t* r_entry2 = bnode_find(result, make_chunk("efgh"), nullptr);
    ASSERT_NE(r_entry2, nullptr);
    EXPECT_EQ(r_entry2->child_disk_offset, 8192u);
    EXPECT_EQ(r_entry2->child, nullptr);  // Lazy: not loaded

    free(locations);
    free(buf);
    bnode_destroy(result);
    bnode_destroy(node);
}

// Test 3: V3 deserializer rejects V2 data (magic mismatch)
TEST_F(V3SerializerTest, V3RejectsV2Data) {
    bnode_t* node = bnode_create_with_level(btree_node_size, 1);
    bnode_entry_t entry;
    memset(&entry, 0, sizeof(entry));
    chunk_t* key = make_chunk("abcd");
    bnode_entry_set_key(&entry, key);
    entry.has_value = 1;
    entry.value = make_ident("val");
    bnode_insert(node, &entry);
    chunk_destroy(key);

    uint8_t* buf = nullptr;
    size_t len = 0;
    bnode_serialize(node, chunk_size, &buf, &len);  // V2 serialize
    EXPECT_EQ(buf[0], 0xB4);  // V2 magic

    // V3 deserializer should reject V2 data
    node_location_t* locations = nullptr;
    size_t num_locations = 0;
    bnode_t* result = bnode_deserialize_v3(buf, len, chunk_size, btree_node_size,
                                            &locations, &num_locations);
    EXPECT_EQ(result, nullptr);  // Should fail: wrong magic

    free(buf);
    bnode_destroy(node);
}

// Test 4: Zero-offset children (new node, not yet persisted)
TEST_F(V3SerializerTest, ZeroOffsetChildren) {
    bnode_t* node = bnode_create_with_level(btree_node_size, 2);
    ASSERT_NE(node, nullptr);

    bnode_entry_t entry;
    memset(&entry, 0, sizeof(entry));
    chunk_t* key = make_chunk("abcd");
    bnode_entry_set_key(&entry, key);
    entry.is_bnode_child = 1;
    entry.child_disk_offset = 0;  // Not yet persisted
    bnode_insert(node, &entry);
    chunk_destroy(key);

    uint8_t* buf = nullptr;
    size_t len = 0;
    bnode_serialize_v3(node, chunk_size, &buf, &len);
    EXPECT_EQ(buf[0], 0xB5);

    node_location_t* locations = nullptr;
    size_t num_locations = 0;
    bnode_t* result = bnode_deserialize_v3(buf, len, chunk_size, btree_node_size,
                                            &locations, &num_locations);
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(locations[0].offset, 0u);

    free(locations);
    free(buf);
    bnode_destroy(result);
    bnode_destroy(node);
}