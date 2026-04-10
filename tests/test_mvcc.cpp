//
// Test for MVCC Transaction Manager using Google Test
//

#include <gtest/gtest.h>
#include "HBTrie/hbtrie.h"
#include "HBTrie/mvcc.h"
#include "Buffer/buffer.h"
#include <vector>
#include <thread>
#include <chrono>

class MvccTest : public ::testing::Test {
public:
    static void SetUpTestSuite() {
        transaction_id_init();
    }

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
            identifier_t* id = identifier_create(buf, 0);
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

    // Helper to verify value content
    void verify_value(identifier_t* found, const char* expected) {
        ASSERT_NE(found, nullptr);
        buffer_t* result = identifier_to_buffer(found);
        ASSERT_NE(result, nullptr);
        EXPECT_EQ(result->size, strlen(expected));
        EXPECT_EQ(memcmp(result->data, expected, strlen(expected)), 0);
        buffer_destroy(result);
    }
};

// ============================================================================
// Basic MVCC Operations
// ============================================================================

TEST_F(MvccTest, BasicInsertRead) {
    // Test 1: Insert with transaction and read back

    // Insert value using MVCC
    path_t* path = make_path({"test", "key"});
    identifier_t* value = make_value("test_value");

    // Get current transaction ID
    transaction_id_t txn_id = transaction_id_get_next();

    EXPECT_EQ(hbtrie_insert(trie, path, value, txn_id), 0);

    // Read with same transaction ID
    identifier_t* found = hbtrie_find(trie, path, txn_id);
    verify_value(found, "test_value");

    identifier_destroy(found);
    identifier_destroy(value);
    path_destroy(path);
}

TEST_F(MvccTest, VersionChainBasic) {
    // Test 2: Multiple versions of same key

    path_t* path = make_path({"versioned", "key"});

    // Insert version 1
    identifier_t* value1 = make_value("version1");
    transaction_id_t txn1 = transaction_id_get_next();
    EXPECT_EQ(hbtrie_insert(trie, path, value1, txn1), 0);
    identifier_destroy(value1);

    // Insert version 2
    identifier_t* value2 = make_value("version2");
    transaction_id_t txn2 = transaction_id_get_next();
    EXPECT_EQ(hbtrie_insert(trie, path, value2, txn2), 0);
    identifier_destroy(value2);

    // Insert version 3
    identifier_t* value3 = make_value("version3");
    transaction_id_t txn3 = transaction_id_get_next();
    EXPECT_EQ(hbtrie_insert(trie, path, value3, txn3), 0);
    identifier_destroy(value3);

    // Read with txn1 - should see version1
    identifier_t* found1 = hbtrie_find(trie, path, txn1);
    verify_value(found1, "version1");
    identifier_destroy(found1);

    // Read with txn2 - should see version2
    identifier_t* found2 = hbtrie_find(trie, path, txn2);
    verify_value(found2, "version2");
    identifier_destroy(found2);

    // Read with txn3 - should see version3
    identifier_t* found3 = hbtrie_find(trie, path, txn3);
    verify_value(found3, "version3");
    identifier_destroy(found3);

    path_destroy(path);
}

TEST_F(MvccTest, SnapshotIsolation) {
    // Test 3: Read-your-writes consistency

    path_t* path = make_path({"snapshot", "test"});

    // Insert first version
    identifier_t* value1 = make_value("first");
    transaction_id_t txn1 = transaction_id_get_next();
    EXPECT_EQ(hbtrie_insert(trie, path, value1, txn1), 0);
    identifier_destroy(value1);

    // Record transaction ID before second write
    transaction_id_t read_txn = txn1;

    // Insert second version
    identifier_t* value2 = make_value("second");
    transaction_id_t txn2 = transaction_id_get_next();
    EXPECT_EQ(hbtrie_insert(trie, path, value2, txn2), 0);
    identifier_destroy(value2);

    // Insert third version
    identifier_t* value3 = make_value("third");
    transaction_id_t txn3 = transaction_id_get_next();
    EXPECT_EQ(hbtrie_insert(trie, path, value3, txn3), 0);
    identifier_destroy(value3);

    // Read with read_txn (before second write) - should see "first"
    identifier_t* found_old = hbtrie_find(trie, path, read_txn);
    verify_value(found_old, "first");
    identifier_destroy(found_old);

    // Read with latest txn - should see "third"
    identifier_t* found_latest = hbtrie_find(trie, path, txn3);
    verify_value(found_latest, "third");
    identifier_destroy(found_latest);

    path_destroy(path);
}

TEST_F(MvccTest, LegacyCompatibility) {
    // Test 4: Legacy entries (has_versions=0) work correctly

    path_t* path = make_path({"legacy", "key"});

    // Insert with transaction ID
    identifier_t* value1 = make_value("legacy_value");
    transaction_id_t txn1 = transaction_id_get_next();
    EXPECT_EQ(hbtrie_insert(trie, path, value1, txn1), 0);
    identifier_destroy(value1);

    // Should be readable with any transaction ID
    transaction_id_t read_txn1 = transaction_id_get_next();
    identifier_t* found1 = hbtrie_find(trie, path, read_txn1);
    verify_value(found1, "legacy_value");
    identifier_destroy(found1);

    // Now insert MVCC version
    identifier_t* value2 = make_value("mvcc_value");
    transaction_id_t txn2 = transaction_id_get_next();
    EXPECT_EQ(hbtrie_insert(trie, path, value2, txn2), 0);
    identifier_destroy(value2);

    // Old transaction should still see legacy value
    identifier_t* found_old = hbtrie_find(trie, path, txn1);
    verify_value(found_old, "legacy_value");
    identifier_destroy(found_old);

    // New transaction should see MVCC value
    identifier_t* found_new = hbtrie_find(trie, path, txn2);
    verify_value(found_new, "mvcc_value");
    identifier_destroy(found_new);

    path_destroy(path);
}

TEST_F(MvccTest, MultipleKeys) {
    // Test 5: Multiple independent keys

    std::vector<path_t*> paths;
    std::vector<identifier_t*> values;
    std::vector<transaction_id_t> txns;

    for (int i = 0; i < 10; i++) {
        char key[32];
        snprintf(key, sizeof(key), "key%d", i);
        char val[32];
        snprintf(val, sizeof(val), "value%d", i);

        path_t* path = make_path({"multi", key});
        identifier_t* value = make_value(val);
        transaction_id_t txn = transaction_id_get_next();

        EXPECT_EQ(hbtrie_insert(trie, path, value, txn), 0);

        paths.push_back(path);
        values.push_back(value);
        txns.push_back(txn);

        identifier_destroy(value);
    }

    // Verify all keys
    for (int i = 0; i < 10; i++) {
        char val[32];
        snprintf(val, sizeof(val), "value%d", i);

        identifier_t* found = hbtrie_find(trie, paths[i], txns[i]);
        verify_value(found, val);
        identifier_destroy(found);
    }

    // Cleanup
    for (path_t* path : paths) {
        path_destroy(path);
    }
}

TEST_F(MvccTest, NonexistentKey) {
    // Test 6: Read non-existent key returns NULL

    path_t* path = make_path({"nonexistent", "key"});
    transaction_id_t txn = transaction_id_get_next();

    identifier_t* found = hbtrie_find(trie, path, txn);
    EXPECT_EQ(found, nullptr);

    path_destroy(path);
}

// ============================================================================
// Version Chain Management
// ============================================================================

TEST_F(MvccTest, VersionChainVisibility) {
    // Test 7: Older transactions don't see newer versions

    path_t* path = make_path({"visibility", "test"});

    // Insert version 1
    identifier_t* value1 = make_value("v1");
    transaction_id_t txn1 = transaction_id_get_next();
    hbtrie_insert(trie, path, value1, txn1);
    identifier_destroy(value1);

    // Insert version 2
    identifier_t* value2 = make_value("v2");
    transaction_id_t txn2 = transaction_id_get_next();
    hbtrie_insert(trie, path, value2, txn2);
    identifier_destroy(value2);

    // Insert version 3
    identifier_t* value3 = make_value("v3");
    transaction_id_t txn3 = transaction_id_get_next();
    hbtrie_insert(trie, path, value3, txn3);
    identifier_destroy(value3);

    // Verify visibility rules

    // txn1 sees v1
    identifier_t* f1 = hbtrie_find(trie, path, txn1);
    verify_value(f1, "v1");
    identifier_destroy(f1);

    // txn2 sees v2 (not v1 or v3)
    identifier_t* f2 = hbtrie_find(trie, path, txn2);
    verify_value(f2, "v2");
    identifier_destroy(f2);

    // txn3 sees v3
    identifier_t* f3 = hbtrie_find(trie, path, txn3);
    verify_value(f3, "v3");
    identifier_destroy(f3);

    // Transaction older than all writes sees nothing
    transaction_id_t old_txn = {0, 0, 0};
    identifier_t* f_old = hbtrie_find(trie, path, old_txn);
    EXPECT_EQ(f_old, nullptr);

    path_destroy(path);
}

TEST_F(MvccTest, ConcurrentWrites) {
    // Test 8: Simulated concurrent writes (sequential for proof-of-concept)

    path_t* path = make_path({"concurrent", "key"});

    std::vector<transaction_id_t> txns;

    // Perform 100 writes
    for (int i = 0; i < 100; i++) {
        char val[32];
        snprintf(val, sizeof(val), "value%d", i);

        identifier_t* value = make_value(val);
        transaction_id_t txn = transaction_id_get_next();
        hbtrie_insert(trie, path, value, txn);

        txns.push_back(txn);
        identifier_destroy(value);
    }

    // Verify random versions are visible
    for (int check : {0, 25, 50, 75, 99}) {
        char val[32];
        snprintf(val, sizeof(val), "value%d", check);

        identifier_t* found = hbtrie_find(trie, path, txns[check]);
        verify_value(found, val);
        identifier_destroy(found);
    }

    path_destroy(path);
}

// ============================================================================
// Transaction Manager Tests
// ============================================================================

class TxManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        trie = hbtrie_create(4, 4096);
        ASSERT_NE(trie, nullptr) << "Failed to create trie";

        // Create work pool and timing wheel (minimal for testing)
        pool = work_pool_create(1);  // Single thread for testing
        ASSERT_NE(pool, nullptr) << "Failed to create work pool";

        wheel = hierarchical_timing_wheel_create(1000, pool);
        ASSERT_NE(wheel, nullptr) << "Failed to create timing wheel";

        tx_manager = tx_manager_create(trie, pool, wheel, 100);
        ASSERT_NE(tx_manager, nullptr) << "Failed to create transaction manager";
    }

    void TearDown() override {
        if (tx_manager) {
            tx_manager_destroy(tx_manager);
        }
        if (wheel) {
            hierarchical_timing_wheel_destroy(wheel);
        }
        if (pool) {
            work_pool_destroy(pool);
        }
        if (trie) {
            hbtrie_destroy(trie);
        }
    }

    hbtrie_t* trie;
    tx_manager_t* tx_manager;
    work_pool_t* pool;
    hierarchical_timing_wheel_t* wheel;

    path_t* make_path(std::initializer_list<const char*> subscripts) {
        path_t* path = path_create();
        for (const char* sub : subscripts) {
            buffer_t* buf = buffer_create_from_pointer_copy((uint8_t*)sub, strlen(sub));
            identifier_t* id = identifier_create(buf, 0);
            buffer_destroy(buf);
            path_append(path, id);
            identifier_destroy(id);
        }
        return path;
    }

    identifier_t* make_value(const char* data) {
        buffer_t* buf = buffer_create_from_pointer_copy((uint8_t*)data, strlen(data));
        identifier_t* id = identifier_create(buf, 0);
        buffer_destroy(buf);
        return id;
    }
};

TEST_F(TxManagerTest, BeginCommit) {
    // Test 9: Basic transaction begin/commit

    txn_desc_t* txn = tx_manager_begin(tx_manager);
    ASSERT_NE(txn, nullptr);
    EXPECT_EQ(txn->state, TXN_ACTIVE);

    int result = tx_manager_commit(tx_manager, txn);
    EXPECT_EQ(result, 0);
    EXPECT_EQ(txn->state, TXN_COMMITTED);

    txn_desc_destroy(txn);
}

TEST_F(TxManagerTest, BeginAbort) {
    // Test 10: Transaction abort

    txn_desc_t* txn = tx_manager_begin(tx_manager);
    ASSERT_NE(txn, nullptr);
    EXPECT_EQ(txn->state, TXN_ACTIVE);

    int result = tx_manager_abort(tx_manager, txn);
    EXPECT_EQ(result, 0);
    EXPECT_EQ(txn->state, TXN_ABORTED);

    txn_desc_destroy(txn);
}

TEST_F(TxManagerTest, TransactionIDs) {
    // Test 11: Transaction IDs are monotonically increasing

    txn_desc_t* txn1 = tx_manager_begin(tx_manager);
    txn_desc_t* txn2 = tx_manager_begin(tx_manager);
    txn_desc_t* txn3 = tx_manager_begin(tx_manager);

    EXPECT_LT(transaction_id_compare(&txn1->txn_id, &txn2->txn_id), 0);
    EXPECT_LT(transaction_id_compare(&txn2->txn_id, &txn3->txn_id), 0);

    tx_manager_commit(tx_manager, txn1);
    tx_manager_commit(tx_manager, txn2);
    tx_manager_commit(tx_manager, txn3);

    txn_desc_destroy(txn1);
    txn_desc_destroy(txn2);
    txn_desc_destroy(txn3);
}

TEST_F(TxManagerTest, LastCommitted) {
    // Test 12: Last committed transaction ID tracking

    transaction_id_t initial = tx_manager_get_last_committed(tx_manager);

    txn_desc_t* txn1 = tx_manager_begin(tx_manager);
    tx_manager_commit(tx_manager, txn1);

    transaction_id_t after1 = tx_manager_get_last_committed(tx_manager);
    EXPECT_GE(transaction_id_compare(&after1, &initial), 0);

    txn_desc_t* txn2 = tx_manager_begin(tx_manager);
    tx_manager_commit(tx_manager, txn2);

    transaction_id_t after2 = tx_manager_get_last_committed(tx_manager);
    EXPECT_GE(transaction_id_compare(&after2, &after1), 0);

    txn_desc_destroy(txn1);
    txn_desc_destroy(txn2);
}

TEST_F(TxManagerTest, MinActive) {
    // Test 13: Minimum active transaction tracking

    txn_desc_t* txn1 = tx_manager_begin(tx_manager);
    txn_desc_t* txn2 = tx_manager_begin(tx_manager);
    txn_desc_t* txn3 = tx_manager_begin(tx_manager);

    transaction_id_t min_active = tx_manager_get_min_active(tx_manager);
    EXPECT_EQ(transaction_id_compare(&min_active, &txn1->txn_id), 0);

    tx_manager_commit(tx_manager, txn1);

    min_active = tx_manager_get_min_active(tx_manager);
    EXPECT_EQ(transaction_id_compare(&min_active, &txn2->txn_id), 0);

    tx_manager_commit(tx_manager, txn2);
    tx_manager_commit(tx_manager, txn3);

    // After all committed, min_active should equal last_committed
    transaction_id_t last_committed = tx_manager_get_last_committed(tx_manager);
    min_active = tx_manager_get_min_active(tx_manager);
    EXPECT_EQ(transaction_id_compare(&min_active, &last_committed), 0);

    txn_desc_destroy(txn1);
    txn_desc_destroy(txn2);
    txn_desc_destroy(txn3);
}

TEST_F(TxManagerTest, IntegrationWithHBTrie) {
    // Test 14: Integration test with HBTrie operations

    path_t* path1 = make_path({"test", "key1"});
    path_t* path2 = make_path({"test", "key2"});

    // Transaction 1: Insert key1
    txn_desc_t* txn1 = tx_manager_begin(tx_manager);
    identifier_t* value1 = make_value("value1");
    EXPECT_EQ(hbtrie_insert(trie, path1, value1, txn1->txn_id), 0);
    tx_manager_commit(tx_manager, txn1);
    identifier_destroy(value1);

    // Transaction 2: Insert key2
    txn_desc_t* txn2 = tx_manager_begin(tx_manager);
    identifier_t* value2 = make_value("value2");
    EXPECT_EQ(hbtrie_insert(trie, path2, value2, txn2->txn_id), 0);
    tx_manager_commit(tx_manager, txn2);
    identifier_destroy(value2);

    // Read with last committed transaction ID
    transaction_id_t last_committed = tx_manager_get_last_committed(tx_manager);
    identifier_t* found1 = hbtrie_find(trie, path1, last_committed);
    identifier_t* found2 = hbtrie_find(trie, path2, last_committed);

    EXPECT_NE(found1, nullptr);
    EXPECT_NE(found2, nullptr);

    identifier_destroy(found1);
    identifier_destroy(found2);
    txn_desc_destroy(txn1);
    txn_desc_destroy(txn2);
    path_destroy(path1);
    path_destroy(path2);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}