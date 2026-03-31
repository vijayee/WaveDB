/**
 * Test MVCC version chain creation during recovery
 */

#include <gtest/gtest.h>
#include <string.h>
extern "C" {
#include "HBTrie/hbtrie.h"
#include "HBTrie/path.h"
#include "HBTrie/identifier.h"
#include "HBTrie/chunk.h"
#include "Buffer/buffer.h"
#include "Workers/transaction_id.h"
#include "Util/allocator.h"
}

class MVCCRecoveryTest : public ::testing::Test {
protected:
    hbtrie_t* trie;
    uint8_t chunk_size = 4;

    void SetUp() override {
        transaction_id_init();
        trie = hbtrie_create(chunk_size, 4096);
        ASSERT_NE(trie, nullptr);
    }

    void TearDown() override {
        if (trie) {
            hbtrie_destroy(trie);
        }
    }

    identifier_t* create_identifier(const char* str) {
        size_t len = strlen(str);
        buffer_t* buf = buffer_create_from_pointer_copy((uint8_t*)str, len);
        EXPECT_NE(buf, nullptr);

        identifier_t* id = identifier_create(buf, chunk_size);
        EXPECT_NE(id, nullptr);

        buffer_destroy(buf);
        return id;
    }

    std::string identifier_to_string(identifier_t* id) {
        if (!id) return "";

        std::string result;
        for (int i = 0; i < id->chunks.length; i++) {
            chunk_t* chunk = id->chunks.data[i];
            size_t len = chunk->data->size;
            result.append((char*)chunk->data->data, len);
        }
        return result;
    }
};

TEST_F(MVCCRecoveryTest, VersionChainCreation) {
    // Create path for "key0"
    path_t* path = path_create();
    ASSERT_NE(path, nullptr);

    identifier_t* key_id = create_identifier("key0");
    path_append(path, key_id);

    // Create two values
    identifier_t* value1 = create_identifier("value0");
    identifier_t* value2 = create_identifier("value0b");

    // Get two transaction IDs
    transaction_id_t txn1 = transaction_id_get_next();
    transaction_id_t txn2 = transaction_id_get_next();

    // First insert (should create new entry)
    int result1 = hbtrie_insert_mvcc(trie, path, value1, txn1);
    EXPECT_EQ(result1, 0);

    // Second insert (should upgrade to version chain)
    int result2 = hbtrie_insert_mvcc(trie, path, value2, txn2);
    EXPECT_EQ(result2, 0);

    // Read with txn2 (should get value2)
    identifier_t* found = hbtrie_find_mvcc(trie, path, txn2);
    ASSERT_NE(found, nullptr);

    std::string result = identifier_to_string(found);
    EXPECT_EQ(result, "value0b") << "Expected 'value0b' but got '" << result << "'";

    // Read with txn1 (should get value1)
    identifier_t* found1 = hbtrie_find_mvcc(trie, path, txn1);
    ASSERT_NE(found1, nullptr);

    std::string result1_str = identifier_to_string(found1);
    EXPECT_EQ(result1_str, "value0") << "Expected 'value0' but got '" << result1_str << "'";

    // Cleanup
    path_destroy(path);
    identifier_destroy(key_id);
    identifier_destroy(value1);
    identifier_destroy(value2);
}

TEST_F(MVCCRecoveryTest, MultipleVersionChain) {
    // Create path for "key1"
    path_t* path = path_create();
    ASSERT_NE(path, nullptr);

    identifier_t* key_id = create_identifier("key1");
    path_append(path, key_id);

    // Create three values
    identifier_t* value1 = create_identifier("v1");
    identifier_t* value2 = create_identifier("v2");
    identifier_t* value3 = create_identifier("v3");

    // Get three transaction IDs
    transaction_id_t txn1 = transaction_id_get_next();
    transaction_id_t txn2 = transaction_id_get_next();
    transaction_id_t txn3 = transaction_id_get_next();

    // Insert three versions
    EXPECT_EQ(hbtrie_insert_mvcc(trie, path, value1, txn1), 0);
    EXPECT_EQ(hbtrie_insert_mvcc(trie, path, value2, txn2), 0);
    EXPECT_EQ(hbtrie_insert_mvcc(trie, path, value3, txn3), 0);

    // Read with each transaction ID
    identifier_t* found1 = hbtrie_find_mvcc(trie, path, txn1);
    ASSERT_NE(found1, nullptr);
    EXPECT_EQ(identifier_to_string(found1), "v1");

    identifier_t* found2 = hbtrie_find_mvcc(trie, path, txn2);
    ASSERT_NE(found2, nullptr);
    EXPECT_EQ(identifier_to_string(found2), "v2");

    identifier_t* found3 = hbtrie_find_mvcc(trie, path, txn3);
    ASSERT_NE(found3, nullptr);
    EXPECT_EQ(identifier_to_string(found3), "v3");

    // Cleanup
    path_destroy(path);
    identifier_destroy(key_id);
    identifier_destroy(value1);
    identifier_destroy(value2);
    identifier_destroy(value3);
}

TEST_F(MVCCRecoveryTest, SameTransactionMultipleWrites) {
    // This test simulates what happens during WAL recovery:
    // Multiple writes to the same key in the same trie

    // Create path for "key2"
    path_t* path = path_create();
    ASSERT_NE(path, nullptr);

    identifier_t* key_id = create_identifier("key2");
    path_append(path, key_id);

    // Create two values
    identifier_t* value1 = create_identifier("first");
    identifier_t* value2 = create_identifier("second");

    // Get transaction IDs (simulating WAL replay)
    transaction_id_t txn1 = transaction_id_get_next();
    transaction_id_t txn2 = transaction_id_get_next();

    // Simulate WAL replay: insert both values
    printf("Inserting value1 with txn1\n");
    EXPECT_EQ(hbtrie_insert_mvcc(trie, path, value1, txn1), 0);

    printf("Inserting value2 with txn2\n");
    EXPECT_EQ(hbtrie_insert_mvcc(trie, path, value2, txn2), 0);

    // Read with latest transaction (should get latest value)
    printf("Reading with txn2\n");
    identifier_t* found = hbtrie_find_mvcc(trie, path, txn2);
    ASSERT_NE(found, nullptr);

    std::string result = identifier_to_string(found);
    printf("Got: '%s'\n", result.c_str());
    EXPECT_EQ(result, "second") << "Expected 'second' but got '" << result << "'";

    // Cleanup
    path_destroy(path);
    identifier_destroy(key_id);
    identifier_destroy(value1);
    identifier_destroy(value2);
}