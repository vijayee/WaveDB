//
// Test for bnode_t using Google Test
//

#include <gtest/gtest.h>
#include "HBTrie/bnode.h"
#include "HBTrie/chunk.h"
#include "HBTrie/identifier.h"

class BnodeTest : public ::testing::Test {
protected:
    void SetUp() override {
    }

    void TearDown() override {
    }
};

TEST_F(BnodeTest, Create) {
    bnode_t* node = bnode_create(4096);
    ASSERT_NE(node, nullptr);
    EXPECT_EQ(node->node_size, 4096u);
    EXPECT_TRUE(bnode_is_empty(node));

    bnode_destroy(node);
}

TEST_F(BnodeTest, InsertFind) {
    bnode_t* node = bnode_create(4096);
    ASSERT_NE(node, nullptr);

    // Create some chunks as keys
    chunk_t* key_a = chunk_create("aaa", 3);
    chunk_t* key_b = chunk_create("bbb", 3);
    chunk_t* key_c = chunk_create("ccc", 3);

    ASSERT_NE(key_a, nullptr);
    ASSERT_NE(key_b, nullptr);
    ASSERT_NE(key_c, nullptr);

    // Create entries - bnode takes ownership of the chunks
    bnode_entry_t entry_a = { .key = key_a, .child = nullptr, .has_value = 0 };
    bnode_entry_t entry_b = { .key = key_b, .child = nullptr, .has_value = 0 };
    bnode_entry_t entry_c = { .key = key_c, .child = nullptr, .has_value = 0 };

    // Insert entries
    EXPECT_EQ(bnode_insert(node, &entry_a), 0);
    EXPECT_EQ(bnode_insert(node, &entry_b), 0);
    EXPECT_EQ(bnode_insert(node, &entry_c), 0);

    // Verify entries are sorted
    EXPECT_EQ(bnode_count(node), 3u);

    // Find entries - create temporary chunks for searching
    chunk_t* search_b = chunk_create("bbb", 3);
    chunk_t* search_a = chunk_create("aaa", 3);
    chunk_t* search_c = chunk_create("ccc", 3);

    size_t index;
    bnode_entry_t* found = bnode_find(node, search_b, &index);
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(index, 1u);

    found = bnode_find(node, search_a, &index);
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(index, 0u);

    found = bnode_find(node, search_c, &index);
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(index, 2u);

    // Cleanup - bnode_destroy will free the keys that were inserted
    chunk_destroy(search_a);
    chunk_destroy(search_b);
    chunk_destroy(search_c);
    bnode_destroy(node);
}

TEST_F(BnodeTest, Remove) {
    bnode_t* node = bnode_create(4096);
    ASSERT_NE(node, nullptr);

    chunk_t* key_a = chunk_create("aaa", 3);
    chunk_t* key_b = chunk_create("bbb", 3);

    ASSERT_NE(key_a, nullptr);
    ASSERT_NE(key_b, nullptr);

    bnode_entry_t entry_a = { .key = key_a, .child = nullptr, .has_value = 0 };
    bnode_entry_t entry_b = { .key = key_b, .child = nullptr, .has_value = 0 };

    bnode_insert(node, &entry_a);
    bnode_insert(node, &entry_b);

    EXPECT_EQ(bnode_count(node), 2u);

    // Create search key for removal
    chunk_t* search_a = chunk_create("aaa", 3);

    // Remove first entry
    bnode_entry_t removed = bnode_remove(node, search_a);
    EXPECT_EQ(bnode_count(node), 1u);

    // Try to find removed entry
    chunk_t* search_again = chunk_create("aaa", 3);
    size_t index;
    bnode_entry_t* found = bnode_find(node, search_again, &index);
    EXPECT_EQ(found, nullptr);

    // Cleanup - removed entry's key needs to be freed
    if (removed.key != nullptr) {
        chunk_destroy(removed.key);
    }
    chunk_destroy(search_a);
    chunk_destroy(search_again);
    bnode_destroy(node);
}

TEST_F(BnodeTest, SortedInsertion) {
    bnode_t* node = bnode_create(4096);
    ASSERT_NE(node, nullptr);

    // Insert in reverse order to verify sorting
    chunk_t* key_c = chunk_create("ccc", 3);
    chunk_t* key_a = chunk_create("aaa", 3);
    chunk_t* key_b = chunk_create("bbb", 3);

    bnode_entry_t entry_c = { .key = key_c, .child = nullptr, .has_value = 0 };
    bnode_entry_t entry_a = { .key = key_a, .child = nullptr, .has_value = 0 };
    bnode_entry_t entry_b = { .key = key_b, .child = nullptr, .has_value = 0 };

    bnode_insert(node, &entry_c);
    bnode_insert(node, &entry_a);
    bnode_insert(node, &entry_b);

    // Verify entries are sorted (a, b, c)
    chunk_t* search_a = chunk_create("aaa", 3);
    chunk_t* search_b = chunk_create("bbb", 3);
    chunk_t* search_c = chunk_create("ccc", 3);

    size_t index;
    bnode_find(node, search_a, &index);
    EXPECT_EQ(index, 0u);

    bnode_find(node, search_b, &index);
    EXPECT_EQ(index, 1u);

    bnode_find(node, search_c, &index);
    EXPECT_EQ(index, 2u);

    chunk_destroy(search_a);
    chunk_destroy(search_b);
    chunk_destroy(search_c);
    bnode_destroy(node);
}