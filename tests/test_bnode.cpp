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

TEST_F(BnodeTest, NeedsSplitMinimumEntries) {
    // Test that nodes with less than 4 entries don't need split
    bnode_t* node = bnode_create(128);  // Small node to trigger size limit

    // Add 3 entries - should NOT need split (need at least 4)
    for (int i = 0; i < 3; i++) {
        char key[4];
        snprintf(key, sizeof(key), "k%02d", i);
        chunk_t* chunk = chunk_create(key, 3);
        bnode_entry_t entry = {.key = chunk, .has_value = 0};
        bnode_insert(node, &entry);
    }

    // Less than 4 entries - should not need split
    EXPECT_EQ(bnode_needs_split(node, 4), 0);

    // Add 4th entry
    chunk_t* chunk4 = chunk_create("k03", 3);
    bnode_entry_t entry4 = {.key = chunk4, .has_value = 0};
    bnode_insert(node, &entry4);

    // Now has 4 entries, but size might still be under limit
    // The split check depends on size, not just entry count

    bnode_destroy(node);
}

TEST_F(BnodeTest, SplitMinimumEntries) {
    // Verify split fails with less than 4 entries
    bnode_t* node = bnode_create(128);

    // Add only 3 entries
    for (int i = 0; i < 3; i++) {
        char key[4];
        snprintf(key, sizeof(key), "k%02d", i);
        chunk_t* chunk = chunk_create(key, 3);
        bnode_entry_t entry = {.key = chunk, .has_value = 0};
        bnode_insert(node, &entry);
    }

    bnode_t* right = nullptr;
    chunk_t* split_key = nullptr;

    // Should fail - need at least 4 entries
    EXPECT_EQ(bnode_split(node, &right, &split_key), -1);
    EXPECT_EQ(right, nullptr);
    EXPECT_EQ(split_key, nullptr);

    bnode_destroy(node);
}

TEST_F(BnodeTest, SplitDistributesEvenly) {
    bnode_t* node = bnode_create(128);

    // Add 8 entries to ensure split works
    for (int i = 0; i < 8; i++) {
        char key[4];
        snprintf(key, sizeof(key), "k%02d", i);
        chunk_t* chunk = chunk_create(key, 3);
        bnode_entry_t entry = {.key = chunk, .has_value = 0};
        bnode_insert(node, &entry);
    }

    EXPECT_EQ(bnode_count(node), 8u);

    bnode_t* right = nullptr;
    chunk_t* split_key = nullptr;

    EXPECT_EQ(bnode_split(node, &right, &split_key), 0);
    EXPECT_NE(right, nullptr);
    EXPECT_NE(split_key, nullptr);

    // Each side should have at least 2 entries
    EXPECT_GE(bnode_count(node), 2u);
    EXPECT_GE(bnode_count(right), 2u);

    // Total entries should be preserved
    EXPECT_EQ(bnode_count(node) + bnode_count(right), 8u);

    // Verify split_key is valid
    EXPECT_NE(split_key->data, nullptr);

    chunk_destroy(split_key);
    bnode_destroy(right);
    bnode_destroy(node);
}

TEST_F(BnodeTest, SplitKeyOwnership) {
    bnode_t* node = bnode_create(128);

    // Add 6 entries
    for (int i = 0; i < 6; i++) {
        char key[4];
        snprintf(key, sizeof(key), "k%02d", i);
        chunk_t* chunk = chunk_create(key, 3);
        bnode_entry_t entry = {.key = chunk, .has_value = 0};
        bnode_insert(node, &entry);
    }

    bnode_t* right = nullptr;
    chunk_t* split_key = nullptr;

    EXPECT_EQ(bnode_split(node, &right, &split_key), 0);

    // split_key should be a COPY, not a reference to existing key
    // We should be able to destroy it independently
    chunk_destroy(split_key);

    // Nodes should still be valid
    EXPECT_GT(bnode_count(node), 0u);
    EXPECT_GT(bnode_count(right), 0u);

    bnode_destroy(right);
    bnode_destroy(node);
}

TEST_F(BnodeTest, GetMinKey) {
    bnode_t* node = bnode_create(128);

    // Empty node
    EXPECT_EQ(bnode_get_min_key(node), nullptr);

    // Add entries in non-sorted order
    chunk_t* chunk_c = chunk_create("k02", 3);
    bnode_entry_t entry_c = {.key = chunk_c, .has_value = 0};
    bnode_insert(node, &entry_c);

    chunk_t* chunk_a = chunk_create("k00", 3);
    bnode_entry_t entry_a = {.key = chunk_a, .has_value = 0};
    bnode_insert(node, &entry_a);

    // First key should be "k00" (sorted order)
    chunk_t* min_key = bnode_get_min_key(node);
    ASSERT_NE(min_key, nullptr);
    EXPECT_EQ(chunk_compare(min_key, chunk_a), 0);

    bnode_destroy(node);
}