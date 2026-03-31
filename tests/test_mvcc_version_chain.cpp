/**
 * Minimal test for MVCC version chain creation
 */

#include <gtest/gtest.h>
#include "HBTrie/hbtrie.h"
#include "HBTrie/path.h"
#include "HBTrie/identifier.h"
#include "Workers/transaction_id.h"

TEST(MVCCVersionChain, TwoWritesSameKey) {
    // Initialize transaction ID generator
    transaction_id_init();

    // Create HBTrie
    uint8_t chunk_size = 4;
    uint32_t btree_node_size = 4096;
    hbtrie_t* trie = hbtrie_create(chunk_size, btree_node_size);
    ASSERT_NE(trie, nullptr);

    // Get two transaction IDs
    transaction_id_t txn1 = transaction_id_get_next();
    transaction_id_t txn2 = transaction_id_get_next();

    // Create path for "key0"
    path_t* path = path_create();
    ASSERT_NE(path, nullptr);

    // Create identifier "key0"
    std::string key_str = "key0";
    identifier_t* id = identifier_create(key_str.length(), chunk_size);
    ASSERT_NE(id, nullptr);
    memcpy(id->chunks.data[0]->data, key_str.data(), key_str.length());
    path_append(path, id);

    // Create two values
    std::string val1 = "value0";
    std::string val2 = "value0b";

    identifier_t* value1 = identifier_create(val1.length(), chunk_size);
    ASSERT_NE(value1, nullptr);
    memcpy(value1->chunks.data[0]->data, val1.data(), val1.length());

    identifier_t* value2 = identifier_create(val2.length(), chunk_size);
    ASSERT_NE(value2, nullptr);
    memcpy(value2->chunks.data[0]->data, val2.data(), val2.length());

    // First insert
    EXPECT_EQ(hbtrie_insert_mvcc(trie, path, value1, txn1), 0);

    // Second insert (should upgrade to version chain)
    EXPECT_EQ(hbtrie_insert_mvcc(trie, path, value2, txn2), 0);

    // Read with txn2 (should get value2)
    identifier_t* found = hbtrie_find_mvcc(trie, path, txn2);
    ASSERT_NE(found, nullptr);

    // Extract string
    std::string found_str((char*)found->chunks.data[0]->data, found->length);

    EXPECT_EQ(found_str, val2) << "Expected to find '" << val2 << "' but got '" << found_str << "'";

    // Cleanup
    path_destroy(path);
    identifier_destroy(id);
    identifier_destroy(value1);
    identifier_destroy(value2);
    hbtrie_destroy(trie);
}

TEST(MVCCVersionChain, ThreeWritesSameKey) {
    transaction_id_init();

    uint8_t chunk_size = 4;
    hbtrie_t* trie = hbtrie_create(chunk_size, 4096);
    ASSERT_NE(trie, nullptr);

    transaction_id_t txn1 = transaction_id_get_next();
    transaction_id_t txn2 = transaction_id_get_next();
    transaction_id_t txn3 = transaction_id_get_next();

    path_t* path = path_create();
    std::string key = "testkey";
    identifier_t* id = identifier_create(key.length(), chunk_size);
    memcpy(id->chunks.data[0]->data, key.data(), key.length());
    path_append(path, id);

    std::string val1 = "val1";
    std::string val2 = "val2";
    std::string val3 = "val3";

    identifier_t* value1 = identifier_create(val1.length(), chunk_size);
    memcpy(value1->chunks.data[0]->data, val1.data(), val1.length());

    identifier_t* value2 = identifier_create(val2.length(), chunk_size);
    memcpy(value2->chunks.data[0]->data, val2.data(), val2.length());

    identifier_t* value3 = identifier_create(val3.length(), chunk_size);
    memcpy(value3->chunks.data[0]->data, val3.data(), val3.length());

    EXPECT_EQ(hbtrie_insert_mvcc(trie, path, value1, txn1), 0);
    EXPECT_EQ(hbtrie_insert_mvcc(trie, path, value2, txn2), 0);
    EXPECT_EQ(hbtrie_insert_mvcc(trie, path, value3, txn3), 0);

    // Each transaction should see its corresponding value
    identifier_t* found1 = hbtrie_find_mvcc(trie, path, txn1);
    ASSERT_NE(found1, nullptr);
    std::string str1((char*)found1->chunks.data[0]->data, found1->length);
    EXPECT_EQ(str1, val1);

    identifier_t* found2 = hbtrie_find_mvcc(trie, path, txn2);
    ASSERT_NE(found2, nullptr);
    std::string str2((char*)found2->chunks.data[0]->data, found2->length);
    EXPECT_EQ(str2, val2);

    identifier_t* found3 = hbtrie_find_mvcc(trie, path, txn3);
    ASSERT_NE(found3, nullptr);
    std::string str3((char*)found3->chunks.data[0]->data, found3->length);
    EXPECT_EQ(str3, val3);

    path_destroy(path);
    identifier_destroy(id);
    identifier_destroy(value1);
    identifier_destroy(value2);
    identifier_destroy(value3);
    hbtrie_destroy(trie);
}