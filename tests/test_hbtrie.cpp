//
// Test for HBTrie using Google Test
//

#include <gtest/gtest.h>
#include "HBTrie/hbtrie.h"
#include "Buffer/buffer.h"

class HbtrieTest : public ::testing::Test {
protected:
    void SetUp() override {
        trie = hbtrie_create(4, 4096);
        ASSERT_NE(trie, nullptr);
    }

    void TearDown() override {
        if (trie) {
            hbtrie_destroy(trie);
        }
    }

    hbtrie_t* trie;

    // Helper to create a path from string subscripts
    path_t* make_path(std::initializer_list<const char*> subscripts) {
        path_t* path = path_create();
        for (const char* sub : subscripts) {
            buffer_t* buf = buffer_create_from_pointer_copy((uint8_t*)sub, strlen(sub));
            identifier_t* id = identifier_create(buf, 0);  // use default chunk size
            buffer_destroy(buf);
            path_append(path, id);
            identifier_destroy(id);
        }
        return path;
    }

    // Helper to create an identifier value
    identifier_t* make_value(const char* data) {
        buffer_t* buf = buffer_create_from_pointer_copy((uint8_t*)data, strlen(data));
        identifier_t* id = identifier_create(buf, 0);
        buffer_destroy(buf);
        return id;
    }
};

TEST_F(HbtrieTest, CreateDestroy) {
    EXPECT_NE(trie, nullptr);
    EXPECT_NE(trie->root, nullptr);
    EXPECT_EQ(trie->chunk_size, DEFAULT_CHUNK_SIZE);
    EXPECT_EQ(trie->btree_node_size, 4096u);
}

TEST_F(HbtrieTest, InsertFindSingleLevel) {
    // Insert at path ["hello"] with value "world"
    path_t* path = make_path({"hello"});
    identifier_t* value = make_value("world");

    EXPECT_EQ(hbtrie_insert(trie, path, value), 0);

    // Find the value
    identifier_t* found = hbtrie_find(trie, path);
    ASSERT_NE(found, nullptr);

    // Verify value
    buffer_t* result = identifier_to_buffer(found);
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(memcmp(result->data, "world", 5), 0);

    buffer_destroy(result);
    identifier_destroy(found);
    identifier_destroy(value);
    path_destroy(path);
}

TEST_F(HbtrieTest, InsertFindMultiLevel) {
    // Insert at path ["users", "alice", "name"] with value "Alice Smith"
    path_t* path = make_path({"users", "alice", "name"});
    identifier_t* value = make_value("Alice Smith");

    EXPECT_EQ(hbtrie_insert(trie, path, value), 0);

    // Find the value
    identifier_t* found = hbtrie_find(trie, path);
    ASSERT_NE(found, nullptr);

    // Verify value
    buffer_t* result = identifier_to_buffer(found);
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result->size, 11u);
    EXPECT_EQ(memcmp(result->data, "Alice Smith", 11), 0);

    buffer_destroy(result);
    identifier_destroy(found);
    identifier_destroy(value);
    path_destroy(path);
}

TEST_F(HbtrieTest, InsertFindMultiplePaths) {
    // Insert multiple values at different paths
    path_t* path1 = make_path({"users", "alice", "age"});
    path_t* path2 = make_path({"users", "bob", "age"});
    path_t* path3 = make_path({"users", "alice", "name"});

    identifier_t* value1 = make_value("30");
    identifier_t* value2 = make_value("25");
    identifier_t* value3 = make_value("Alice");

    EXPECT_EQ(hbtrie_insert(trie, path1, value1), 0);
    EXPECT_EQ(hbtrie_insert(trie, path2, value2), 0);
    EXPECT_EQ(hbtrie_insert(trie, path3, value3), 0);

    // Find all values
    identifier_t* found1 = hbtrie_find(trie, path1);
    identifier_t* found2 = hbtrie_find(trie, path2);
    identifier_t* found3 = hbtrie_find(trie, path3);

    ASSERT_NE(found1, nullptr);
    ASSERT_NE(found2, nullptr);
    ASSERT_NE(found3, nullptr);

    buffer_t* result1 = identifier_to_buffer(found1);
    buffer_t* result2 = identifier_to_buffer(found2);
    buffer_t* result3 = identifier_to_buffer(found3);

    EXPECT_EQ(memcmp(result1->data, "30", 2), 0);
    EXPECT_EQ(memcmp(result2->data, "25", 2), 0);
    EXPECT_EQ(memcmp(result3->data, "Alice", 5), 0);

    buffer_destroy(result1);
    buffer_destroy(result2);
    buffer_destroy(result3);
    identifier_destroy(found1);
    identifier_destroy(found2);
    identifier_destroy(found3);
    identifier_destroy(value1);
    identifier_destroy(value2);
    identifier_destroy(value3);
    path_destroy(path1);
    path_destroy(path2);
    path_destroy(path3);
}

TEST_F(HbtrieTest, FindNonExistent) {
    path_t* path = make_path({"nonexistent", "path"});

    identifier_t* found = hbtrie_find(trie, path);
    EXPECT_EQ(found, nullptr);

    path_destroy(path);
}

TEST_F(HbtrieTest, UpdateValue) {
    path_t* path = make_path({"key"});

    // Insert initial value
    identifier_t* value1 = make_value("value1");
    EXPECT_EQ(hbtrie_insert(trie, path, value1), 0);
    identifier_destroy(value1);

    // Verify initial value
    identifier_t* found1 = hbtrie_find(trie, path);
    ASSERT_NE(found1, nullptr);
    buffer_t* result1 = identifier_to_buffer(found1);
    EXPECT_EQ(memcmp(result1->data, "value1", 6), 0);
    buffer_destroy(result1);
    identifier_destroy(found1);

    // Update with new value
    identifier_t* value2 = make_value("value2");
    EXPECT_EQ(hbtrie_insert(trie, path, value2), 0);
    identifier_destroy(value2);

    // Verify updated value
    identifier_t* found2 = hbtrie_find(trie, path);
    ASSERT_NE(found2, nullptr);
    buffer_t* result2 = identifier_to_buffer(found2);
    EXPECT_EQ(memcmp(result2->data, "value2", 6), 0);
    buffer_destroy(result2);
    identifier_destroy(found2);

    path_destroy(path);
}

TEST_F(HbtrieTest, RemoveValue) {
    path_t* path = make_path({"key"});

    // Insert value
    identifier_t* value = make_value("value");
    EXPECT_EQ(hbtrie_insert(trie, path, value), 0);
    identifier_destroy(value);

    // Verify it exists
    identifier_t* found = hbtrie_find(trie, path);
    ASSERT_NE(found, nullptr);
    identifier_destroy(found);

    // Remove value
    identifier_t* removed = hbtrie_remove(trie, path);
    ASSERT_NE(removed, nullptr);
    identifier_destroy(removed);

    // Verify it's gone
    identifier_t* notfound = hbtrie_find(trie, path);
    EXPECT_EQ(notfound, nullptr);

    path_destroy(path);
}

TEST_F(HbtrieTest, NodeCreateDestroy) {
    hbtrie_node_t* node = hbtrie_node_create(4096);
    ASSERT_NE(node, nullptr);
    EXPECT_NE(node->btree, nullptr);
    EXPECT_TRUE(bnode_is_empty(node->btree));

    hbtrie_node_destroy(node);
}

// Helper to generate a path with varying number of identifiers
path_t* make_path_with_size(int size, int seed) {
    path_t* path = path_create();
    for (int i = 0; i < size; i++) {
        char subscript[32];
        snprintf(subscript, sizeof(subscript), "sub%d_%d", seed, i);
        buffer_t* buf = buffer_create_from_pointer_copy((uint8_t*)subscript, strlen(subscript));
        identifier_t* id = identifier_create(buf, 0);
        buffer_destroy(buf);
        path_append(path, id);
        identifier_destroy(id);
    }
    return path;
}

// Helper to generate a value
identifier_t* make_value_int(int val) {
    char data[32];
    snprintf(data, sizeof(data), "value%d", val);
    buffer_t* buf = buffer_create_from_pointer_copy((uint8_t*)data, strlen(data));
    identifier_t* id = identifier_create(buf, 0);
    buffer_destroy(buf);
    return id;
}

// Helper to count nodes in trie (recursive)
int count_hbtrie_nodes(hbtrie_node_t* node) {
    if (node == nullptr) return 0;
    int count = 1;  // Count this node
    if (node->btree != nullptr) {
        for (size_t i = 0; i < bnode_count(node->btree); i++) {
            bnode_entry_t* entry = bnode_get(node->btree, i);
            if (entry != nullptr && !entry->has_value && entry->child != nullptr) {
                count += count_hbtrie_nodes(entry->child);
            }
        }
    }
    return count;
}

TEST_F(HbtrieTest, DenseOperations) {
    const int NUM_INSERTIONS = 100;
    const int NUM_DELETIONS = 20;
    const int NUM_ADDITIONS = 15;

    std::vector<std::pair<path_t*, identifier_t*>> entries;
    entries.reserve(NUM_INSERTIONS + NUM_ADDITIONS);

    // Insert 100 values with varying path sizes (1-15 identifiers)
    for (int i = 0; i < NUM_INSERTIONS; i++) {
        // Path size varies between 1 and 15
        int path_size = (i % 15) + 1;
        path_t* path = make_path_with_size(path_size, i);
        identifier_t* value = make_value_int(i);

        int result = hbtrie_insert(trie, path, value);
        ASSERT_EQ(result, 0) << "Failed to insert at iteration " << i;

        entries.push_back({path, value});
    }

    // Verify all 100 values can be found
    for (int i = 0; i < NUM_INSERTIONS; i++) {
        identifier_t* found = hbtrie_find(trie, entries[i].first);
        ASSERT_NE(found, nullptr) << "Value not found at index " << i;

        // Verify the value content
        buffer_t* buf = identifier_to_buffer(found);
        ASSERT_NE(buf, nullptr);

        char expected[32];
        snprintf(expected, sizeof(expected), "value%d", i);
        EXPECT_EQ(memcmp(buf->data, expected, strlen(expected)), 0)
            << "Value mismatch at index " << i;

        buffer_destroy(buf);
        identifier_destroy(found);
    }

    // Count initial nodes
    int initial_node_count = count_hbtrie_nodes(trie->root);
    EXPECT_GT(initial_node_count, 0) << "Should have created nodes";

    // Remove 20 values
    std::vector<int> delete_indices = {0, 5, 10, 15, 20, 25, 30, 35, 40, 45,
                                        50, 55, 60, 65, 70, 75, 80, 85, 90, 95};

    for (int idx : delete_indices) {
        identifier_t* removed = hbtrie_remove(trie, entries[idx].first);
        ASSERT_NE(removed, nullptr) << "Failed to remove at index " << idx;
        identifier_destroy(removed);

        // Verify it's actually removed
        identifier_t* found = hbtrie_find(trie, entries[idx].first);
        EXPECT_EQ(found, nullptr) << "Value still found after removal at index " << idx;
    }

    // Count nodes after deletion (may be same or less due to cleanup)
    int after_delete_node_count = count_hbtrie_nodes(trie->root);

    // Add 15 more values with new paths
    for (int i = 0; i < NUM_ADDITIONS; i++) {
        // Use different seed to create different paths
        int path_size = ((i + 100) % 15) + 1;
        path_t* path = make_path_with_size(path_size, i + 1000);
        identifier_t* value = make_value_int(i + 1000);

        int result = hbtrie_insert(trie, path, value);
        ASSERT_EQ(result, 0) << "Failed to add new value at iteration " << i;

        entries.push_back({path, value});
    }

    // Verify remaining original values (those not deleted)
    for (int i = 0; i < NUM_INSERTIONS; i++) {
        bool was_deleted = std::find(delete_indices.begin(), delete_indices.end(), i) != delete_indices.end();

        identifier_t* found = hbtrie_find(trie, entries[i].first);
        if (was_deleted) {
            EXPECT_EQ(found, nullptr) << "Deleted value found at index " << i;
        } else {
            EXPECT_NE(found, nullptr) << "Non-deleted value not found at index " << i;
            if (found) {
                identifier_destroy(found);
            }
        }
    }

    // Verify new values
    for (int i = 0; i < NUM_ADDITIONS; i++) {
        int entry_idx = NUM_INSERTIONS + i;
        identifier_t* found = hbtrie_find(trie, entries[entry_idx].first);
        EXPECT_NE(found, nullptr) << "New value not found at addition " << i;
        if (found) {
            identifier_destroy(found);
        }
    }

    // Count final nodes
    int final_node_count = count_hbtrie_nodes(trie->root);
    EXPECT_GT(final_node_count, 0) << "Should still have nodes";

    // Cleanup
    for (auto& e : entries) {
        path_destroy(e.first);
        identifier_destroy(e.second);
    }
}

TEST_F(HbtrieTest, SubtreeDeletion) {
    // Test that deleting a value cleans up parent nodes when they become empty

    // Create paths that share a common prefix
    // Path 1: ["a", "b", "c", "val1"]
    // Path 2: ["a", "b", "d", "val2"]
    // Path 3: ["a", "e", "val3"]

    path_t* path1 = make_path({"a", "b", "c", "val1"});
    path_t* path2 = make_path({"a", "b", "d", "val2"});
    path_t* path3 = make_path({"a", "e", "val3"});

    identifier_t* value1 = make_value("value1");
    identifier_t* value2 = make_value("value2");
    identifier_t* value3 = make_value("value3");

    EXPECT_EQ(hbtrie_insert(trie, path1, value1), 0);
    EXPECT_EQ(hbtrie_insert(trie, path2, value2), 0);
    EXPECT_EQ(hbtrie_insert(trie, path3, value3), 0);

    // Count nodes after insertion
    int nodes_after_insert = count_hbtrie_nodes(trie->root);
    EXPECT_GT(nodes_after_insert, 1) << "Should have multiple nodes";

    // Verify all values exist
    identifier_t* found1 = hbtrie_find(trie, path1);
    identifier_t* found2 = hbtrie_find(trie, path2);
    identifier_t* found3 = hbtrie_find(trie, path3);
    EXPECT_NE(found1, nullptr);
    EXPECT_NE(found2, nullptr);
    EXPECT_NE(found3, nullptr);
    if (found1) identifier_destroy(found1);
    if (found2) identifier_destroy(found2);
    if (found3) identifier_destroy(found3);

    // Remove path1 - should NOT delete "c" subtree since we still have path2 under "b"
    identifier_t* removed1 = hbtrie_remove(trie, path1);
    EXPECT_NE(removed1, nullptr);
    identifier_destroy(removed1);

    // Verify path1 is gone but path2 and path3 still exist
    EXPECT_EQ(hbtrie_find(trie, path1), nullptr);
    found2 = hbtrie_find(trie, path2);
    found3 = hbtrie_find(trie, path3);
    EXPECT_NE(found2, nullptr);
    EXPECT_NE(found3, nullptr);
    if (found2) identifier_destroy(found2);
    if (found3) identifier_destroy(found3);

    // Remove path2 - should clean up "b" subtree entirely
    identifier_t* removed2 = hbtrie_remove(trie, path2);
    EXPECT_NE(removed2, nullptr);
    identifier_destroy(removed2);

    // Verify path2 is gone but path3 still exists
    EXPECT_EQ(hbtrie_find(trie, path2), nullptr);
    found3 = hbtrie_find(trie, path3);
    EXPECT_NE(found3, nullptr);
    if (found3) identifier_destroy(found3);

    // Count nodes - should be fewer now
    int nodes_after_partial_delete = count_hbtrie_nodes(trie->root);
    EXPECT_LT(nodes_after_partial_delete, nodes_after_insert);

    // Remove path3 - should clean up everything except root
    identifier_t* removed3 = hbtrie_remove(trie, path3);
    EXPECT_NE(removed3, nullptr);
    identifier_destroy(removed3);

    // Verify all paths are gone
    EXPECT_EQ(hbtrie_find(trie, path3), nullptr);

    // Cleanup
    identifier_destroy(value1);
    identifier_destroy(value2);
    identifier_destroy(value3);
    path_destroy(path1);
    path_destroy(path2);
    path_destroy(path3);
}

TEST_F(HbtrieTest, VaryingPathDepths) {
    // Test with paths of various depths (non-overlapping prefixes)

    std::vector<path_t*> paths;

    // Create paths with depths 1 through 15, each with unique first identifier
    for (int depth = 1; depth <= 15; depth++) {
        path_t* path = path_create();
        for (int i = 0; i < depth; i++) {
            char sub[16];
            snprintf(sub, sizeof(sub), "d%d_l%d", depth, i);  // Include depth in prefix
            buffer_t* buf = buffer_create_from_pointer_copy((uint8_t*)sub, strlen(sub));
            identifier_t* id = identifier_create(buf, 0);
            buffer_destroy(buf);
            path_append(path, id);
            identifier_destroy(id);
        }
        paths.push_back(path);

        char val[16];
        snprintf(val, sizeof(val), "v%d", depth);
        identifier_t* value = make_value(val);
        EXPECT_EQ(hbtrie_insert(trie, path, value), 0);
        identifier_destroy(value);
    }

    // Verify all paths can be found
    for (int depth = 1; depth <= 15; depth++) {
        identifier_t* found = hbtrie_find(trie, paths[depth - 1]);
        ASSERT_NE(found, nullptr) << "Path of depth " << depth << " not found";

        char expected[16];
        snprintf(expected, sizeof(expected), "v%d", depth);
        buffer_t* buf = identifier_to_buffer(found);
        EXPECT_EQ(memcmp(buf->data, expected, strlen(expected)), 0);

        buffer_destroy(buf);
        identifier_destroy(found);
    }

    // Delete paths from deepest to shallowest
    for (int depth = 15; depth >= 1; depth--) {
        identifier_t* removed = hbtrie_remove(trie, paths[depth - 1]);
        EXPECT_NE(removed, nullptr) << "Failed to remove path of depth " << depth;
        identifier_destroy(removed);

        // Verify it's removed
        EXPECT_EQ(hbtrie_find(trie, paths[depth - 1]), nullptr);
    }

    // Cleanup
    for (path_t* path : paths) {
        path_destroy(path);
    }
}