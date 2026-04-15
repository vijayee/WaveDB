//
// Test for HBTrie using Google Test
//

#include <gtest/gtest.h>
#include "HBTrie/hbtrie.h"
#include "HBTrie/mvcc.h"
#include "Buffer/buffer.h"
#include "Workers/transaction_id.h"

class HbtrieTest : public ::testing::Test {
protected:
    void SetUp() override {
        transaction_id_init();
        trie = hbtrie_create(4, 4096);
        ASSERT_NE(trie, nullptr);
    }

    void TearDown() override {
        if (trie) {
            hbtrie_destroy(trie);
        }
    }

    hbtrie_t* trie;

    transaction_id_t next_txn_id() {
        return transaction_id_get_next();
    }

    transaction_id_t read_txn_id() {
        return transaction_id_get_next();
    }

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

    EXPECT_EQ(hbtrie_insert(trie, path, value, next_txn_id()), 0);

    // Find the value
    identifier_t* found = hbtrie_find(trie, path, read_txn_id());
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

    EXPECT_EQ(hbtrie_insert(trie, path, value, next_txn_id()), 0);

    // Find the value
    identifier_t* found = hbtrie_find(trie, path, read_txn_id());
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

    EXPECT_EQ(hbtrie_insert(trie, path1, value1, next_txn_id()), 0);
    EXPECT_EQ(hbtrie_insert(trie, path2, value2, next_txn_id()), 0);
    EXPECT_EQ(hbtrie_insert(trie, path3, value3, next_txn_id()), 0);

    // Find all values
    identifier_t* found1 = hbtrie_find(trie, path1, read_txn_id());
    identifier_t* found2 = hbtrie_find(trie, path2, read_txn_id());
    identifier_t* found3 = hbtrie_find(trie, path3, read_txn_id());

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

    identifier_t* found = hbtrie_find(trie, path, read_txn_id());
    EXPECT_EQ(found, nullptr);

    path_destroy(path);
}

TEST_F(HbtrieTest, UpdateValue) {
    path_t* path = make_path({"key"});

    // Insert initial value
    identifier_t* value1 = make_value("value1");
    EXPECT_EQ(hbtrie_insert(trie, path, value1, next_txn_id()), 0);
    identifier_destroy(value1);

    // Verify initial value
    identifier_t* found1 = hbtrie_find(trie, path, read_txn_id());
    ASSERT_NE(found1, nullptr);
    buffer_t* result1 = identifier_to_buffer(found1);
    EXPECT_EQ(memcmp(result1->data, "value1", 6), 0);
    buffer_destroy(result1);
    identifier_destroy(found1);

    // Update with new value
    identifier_t* value2 = make_value("value2");
    EXPECT_EQ(hbtrie_insert(trie, path, value2, next_txn_id()), 0);
    identifier_destroy(value2);

    // Verify updated value
    identifier_t* found2 = hbtrie_find(trie, path, read_txn_id());
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
    EXPECT_EQ(hbtrie_insert(trie, path, value, next_txn_id()), 0);
    identifier_destroy(value);

    // Verify it exists
    identifier_t* found = hbtrie_find(trie, path, read_txn_id());
    ASSERT_NE(found, nullptr);
    identifier_destroy(found);

    // Remove value
    identifier_t* removed = hbtrie_delete(trie, path, next_txn_id());
    ASSERT_NE(removed, nullptr);
    identifier_destroy(removed);

    // Verify it's gone
    identifier_t* notfound = hbtrie_find(trie, path, read_txn_id());
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

        int result = hbtrie_insert(trie, path, value, next_txn_id());
        ASSERT_EQ(result, 0) << "Failed to insert at iteration " << i;

        entries.push_back({path, value});
    }

    // Verify all 100 values can be found
    for (int i = 0; i < NUM_INSERTIONS; i++) {
        identifier_t* found = hbtrie_find(trie, entries[i].first, read_txn_id());
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

    // Note: With MVCC, deletes add tombstones rather than physically removing entries,
    // so node counts after deletion will differ from the old non-MVCC behavior.

    // Remove 20 values
    std::vector<int> delete_indices = {0, 5, 10, 15, 20, 25, 30, 35, 40, 45,
                                        50, 55, 60, 65, 70, 75, 80, 85, 90, 95};

    for (int idx : delete_indices) {
        identifier_t* removed = hbtrie_delete(trie, entries[idx].first, next_txn_id());
        ASSERT_NE(removed, nullptr) << "Failed to remove at index " << idx;
        identifier_destroy(removed);

        // Verify it's actually removed
        identifier_t* found = hbtrie_find(trie, entries[idx].first, read_txn_id());
        EXPECT_EQ(found, nullptr) << "Value still found after removal at index " << idx;
    }

    // Add 15 more values with new paths
    for (int i = 0; i < NUM_ADDITIONS; i++) {
        // Use different seed to create different paths
        int path_size = ((i + 100) % 15) + 1;
        path_t* path = make_path_with_size(path_size, i + 1000);
        identifier_t* value = make_value_int(i + 1000);

        int result = hbtrie_insert(trie, path, value, next_txn_id());
        ASSERT_EQ(result, 0) << "Failed to add new value at iteration " << i;

        entries.push_back({path, value});
    }

    // Verify remaining original values (those not deleted)
    for (int i = 0; i < NUM_INSERTIONS; i++) {
        bool was_deleted = std::find(delete_indices.begin(), delete_indices.end(), i) != delete_indices.end();

        identifier_t* found = hbtrie_find(trie, entries[i].first, read_txn_id());
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
        identifier_t* found = hbtrie_find(trie, entries[entry_idx].first, read_txn_id());
        EXPECT_NE(found, nullptr) << "New value not found at addition " << i;
        if (found) {
            identifier_destroy(found);
        }
    }

    // Cleanup
    for (auto& e : entries) {
        path_destroy(e.first);
        identifier_destroy(e.second);
    }
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
        EXPECT_EQ(hbtrie_insert(trie, path, value, next_txn_id()), 0);
        identifier_destroy(value);
    }

    // Verify all paths can be found
    for (int depth = 1; depth <= 15; depth++) {
        identifier_t* found = hbtrie_find(trie, paths[depth - 1], read_txn_id());
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
        identifier_t* removed = hbtrie_delete(trie, paths[depth - 1], next_txn_id());
        EXPECT_NE(removed, nullptr) << "Failed to remove path of depth " << depth;
        identifier_destroy(removed);

        // Verify it's removed
        EXPECT_EQ(hbtrie_find(trie, paths[depth - 1], read_txn_id()), nullptr);
    }

    // Cleanup
    for (path_t* path : paths) {
        path_destroy(path);
    }
}


// Test that B+tree splits propagate correctly within hbtrie_nodes
TEST_F(HbtrieTest, SplitPropagationWithSmallNodeSize) {
    // Create trie with small node size to trigger splits
    // Node size of 128 bytes should allow only ~4 entries per node
    hbtrie_t* small_trie = hbtrie_create(4, 128);
    ASSERT_NE(small_trie, nullptr);

    // Insert enough values to cause multiple splits
    const int NUM_KEYS = 50;
    std::vector<std::pair<path_t*, identifier_t*>> entries;

    for (int i = 0; i < NUM_KEYS; i++) {
        char key[16];
        snprintf(key, sizeof(key), "key%04d", i);
        path_t* path = make_path({key});
        identifier_t* value = make_value(key);

        EXPECT_EQ(hbtrie_insert(small_trie, path, value, next_txn_id()), 0)
            << "Failed to insert key " << i;

        entries.push_back({path, value});
    }

    // Verify all values are still retrievable after splits
    for (int i = 0; i < NUM_KEYS; i++) {
        char key[16];
        snprintf(key, sizeof(key), "key%04d", i);
        path_t* path = make_path({key});

        identifier_t* found = hbtrie_find(small_trie, path, read_txn_id());
        EXPECT_NE(found, nullptr) << "Value not found after splits: " << key;

        if (found) {
            buffer_t* buf = identifier_to_buffer(found);
            EXPECT_NE(buf, nullptr);
            if (buf) {
                EXPECT_EQ(memcmp(buf->data, key, strlen(key)), 0)
                    << "Value mismatch for key " << key;
                buffer_destroy(buf);
            }
            identifier_destroy(found);
        }

        path_destroy(path);
    }

    // Cleanup
    for (auto& e : entries) {
        path_destroy(e.first);
        identifier_destroy(e.second);
    }

    hbtrie_destroy(small_trie);
}

TEST_F(HbtrieTest, MultiLevelSplitPropagation) {
    // Create trie with paths that share prefixes to test multi-level splits
    hbtrie_t* small_trie = hbtrie_create(4, 128);
    ASSERT_NE(small_trie, nullptr);

    // Insert values with shared prefix to force deep tree with many entries at same level
    // Paths like ["a", "b", "c", "val00"], ["a", "b", "c", "val01"], etc.
    for (int i = 0; i < 30; i++) {
        char val[16];
        snprintf(val, sizeof(val), "val%02d", i);
        path_t* path = make_path({"a", "b", "c", val});
        identifier_t* value = make_value(val);

        EXPECT_EQ(hbtrie_insert(small_trie, path, value, next_txn_id()), 0)
            << "Failed to insert at index " << i;

        path_destroy(path);
        identifier_destroy(value);
    }

    // Verify all values can still be found
    for (int i = 0; i < 30; i++) {
        char val[16];
        snprintf(val, sizeof(val), "val%02d", i);
        path_t* path = make_path({"a", "b", "c", val});

        identifier_t* found = hbtrie_find(small_trie, path, read_txn_id());
        EXPECT_NE(found, nullptr) << "Value not found at index " << i;

        if (found) {
            identifier_destroy(found);
        }

        path_destroy(path);
    }

    hbtrie_destroy(small_trie);
}

TEST_F(HbtrieTest, CborRoundTripSingleLevel) {
    // Insert values then serialize/deserialize and verify
    path_t* p1 = make_path({"hello"});
    identifier_t* v1 = make_value("world");
    ASSERT_EQ(hbtrie_insert(trie, p1, v1, next_txn_id()), 0);
    path_destroy(p1);
    identifier_destroy(v1);

    path_t* p2 = make_path({"foo", "bar"});
    identifier_t* v2 = make_value("baz");
    ASSERT_EQ(hbtrie_insert(trie, p2, v2, next_txn_id()), 0);
    path_destroy(p2);
    identifier_destroy(v2);

    // Serialize
    cbor_item_t* cbor = hbtrie_to_cbor(trie);
    ASSERT_NE(cbor, nullptr);

    // Deserialize
    hbtrie_t* trie2 = cbor_to_hbtrie(cbor);
    ASSERT_NE(trie2, nullptr);

    // Verify values
    path_t* find1 = make_path({"hello"});
    identifier_t* found1 = hbtrie_find(trie2, find1, read_txn_id());
    EXPECT_NE(found1, nullptr);
    if (found1) {
        buffer_t* buf1 = identifier_to_buffer(found1);
        EXPECT_NE(buf1, nullptr);
        if (buf1) {
            EXPECT_EQ(memcmp(buf1->data, "world", 5), 0);
            buffer_destroy(buf1);
        }
        identifier_destroy(found1);
    }
    path_destroy(find1);

    path_t* find2 = make_path({"foo", "bar"});
    identifier_t* found2 = hbtrie_find(trie2, find2, read_txn_id());
    EXPECT_NE(found2, nullptr);
    if (found2) {
        identifier_destroy(found2);
    }
    path_destroy(find2);

    cbor_decref(&cbor);
    hbtrie_destroy(trie2);
}

TEST_F(HbtrieTest, CborRoundTripMultiLevelBtree) {
    // Create trie with small node size to force multi-level B+tree splits
    hbtrie_t* small_trie = hbtrie_create(4, 128);
    ASSERT_NE(small_trie, nullptr);

    // Insert enough single-subscript paths to force root btree split
    // Use short keys to pack more entries into the root btree
    for (int i = 0; i < 60; i++) {
        char key[8];
        snprintf(key, sizeof(key), "k%d", i);
        path_t* path = make_path({key});
        identifier_t* value = make_value(key);
        EXPECT_EQ(hbtrie_insert(small_trie, path, value, next_txn_id()), 0);
        path_destroy(path);
        identifier_destroy(value);
    }

    // Check if multi-level B+tree was created (may or may not be depending on node fill)
    int original_height = small_trie->root->btree_height;

    // Serialize
    cbor_item_t* cbor = hbtrie_to_cbor(small_trie);
    ASSERT_NE(cbor, nullptr);

    // Deserialize
    hbtrie_t* trie2 = cbor_to_hbtrie(cbor);
    ASSERT_NE(trie2, nullptr);

    // Verify btree_height preserved
    EXPECT_EQ(trie2->root->btree_height, original_height);

    // Verify all values can be found
    for (int i = 0; i < 60; i++) {
        char key[8];
        snprintf(key, sizeof(key), "k%d", i);
        path_t* path = make_path({key});

        identifier_t* found = hbtrie_find(trie2, path, read_txn_id());
        EXPECT_NE(found, nullptr) << "Value not found at index " << i;

        if (found) {
            buffer_t* buf = identifier_to_buffer(found);
            EXPECT_NE(buf, nullptr);
            if (buf) {
                EXPECT_EQ(memcmp(buf->data, key, strlen(key)), 0)
                    << "Value mismatch at index " << i << ": expected '"
                    << key << "', got '" << std::string((char*)buf->data, buf->size) << "'";
                buffer_destroy(buf);
            }
            identifier_destroy(found);
        }

        path_destroy(path);
    }

    cbor_decref(&cbor);
    hbtrie_destroy(trie2);
    hbtrie_destroy(small_trie);
}
