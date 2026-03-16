//
// Test for transaction_id_t using Google Test
//

#include <gtest/gtest.h>
#include <pthread.h>
#include <vector>
#include <algorithm>
#include "Workers/transaction_id.h"

class TransactionIdTest : public ::testing::Test {
protected:
    void SetUp() override {
        transaction_id_init();
    }

    void TearDown() override {
    }
};

TEST_F(TransactionIdTest, GenerateUniqueness) {
    // Generate multiple transaction IDs and verify uniqueness
    std::vector<transaction_id_t> ids;
    const int count = 1000;

    for (int i = 0; i < count; i++) {
        transaction_id_t id = transaction_id_get_next();
        ids.push_back(id);
    }

    // Check that all IDs are unique
    for (int i = 0; i < count; i++) {
        for (int j = i + 1; j < count; j++) {
            EXPECT_NE(transaction_id_compare(&ids[i], &ids[j]), 0);
        }
    }
}

TEST_F(TransactionIdTest, MonotonicallyIncreasing) {
    // Generate multiple transaction IDs and verify they're monotonically increasing
    transaction_id_t prev = transaction_id_get_next();

    for (int i = 0; i < 100; i++) {
        transaction_id_t curr = transaction_id_get_next();
        EXPECT_GT(transaction_id_compare(&curr, &prev), 0);
        prev = curr;
    }
}

TEST_F(TransactionIdTest, SerializeDeserialize) {
    // Generate a transaction ID
    transaction_id_t original = transaction_id_get_next();

    // Serialize
    uint8_t buffer[24];
    transaction_id_serialize(&original, buffer);

    // Deserialize
    transaction_id_t deserialized;
    transaction_id_deserialize(&deserialized, buffer);

    // Compare
    EXPECT_EQ(transaction_id_compare(&original, &deserialized), 0);
    EXPECT_EQ(original.time, deserialized.time);
    EXPECT_EQ(original.nanos, deserialized.nanos);
    EXPECT_EQ(original.count, deserialized.count);
}

TEST_F(TransactionIdTest, SerializeDeserializeZeroValues) {
    // Test with all zeros
    transaction_id_t zero = {0, 0, 0};

    uint8_t buffer[24];
    transaction_id_serialize(&zero, buffer);

    transaction_id_t deserialized;
    transaction_id_deserialize(&deserialized, buffer);

    EXPECT_EQ(transaction_id_compare(&zero, &deserialized), 0);
}

TEST_F(TransactionIdTest, SerializeDeserializeMaxValues) {
    // Test with maximum values
    transaction_id_t max_id = {UINT64_MAX, UINT64_MAX, UINT64_MAX};

    uint8_t buffer[24];
    transaction_id_serialize(&max_id, buffer);

    transaction_id_t deserialized;
    transaction_id_deserialize(&deserialized, buffer);

    EXPECT_EQ(transaction_id_compare(&max_id, &deserialized), 0);
}

TEST_F(TransactionIdTest, ComparisonOperators) {
    transaction_id_t a = {100, 0, 0};
    transaction_id_t b = {200, 0, 0};
    transaction_id_t c = {100, 500, 0};
    transaction_id_t d = {100, 500, 10};

    // Test time comparison
    EXPECT_EQ(transaction_id_compare(&a, &b), -1);
    EXPECT_EQ(transaction_id_compare(&b, &a), 1);

    // Test nanos comparison
    EXPECT_EQ(transaction_id_compare(&a, &c), -1);
    EXPECT_EQ(transaction_id_compare(&c, &a), 1);

    // Test count comparison
    EXPECT_EQ(transaction_id_compare(&c, &d), -1);
    EXPECT_EQ(transaction_id_compare(&d, &c), 1);

    // Test equality
    EXPECT_EQ(transaction_id_compare(&a, &a), 0);
}

TEST_F(TransactionIdTest, ComparisonPriority) {
    // Time has highest priority, then nanos, then count
    transaction_id_t a = {100, 100, 100};
    transaction_id_t b = {200, 0, 0};

    // b.time > a.time, so b > a despite lower nanos and count
    EXPECT_EQ(transaction_id_compare(&a, &b), -1);

    transaction_id_t c = {100, 200, 0};
    transaction_id_t d = {100, 300, 0};

    // d.nanos > c.nanos, so d > c
    EXPECT_EQ(transaction_id_compare(&c, &d), -1);

    transaction_id_t e = {100, 100, 50};
    transaction_id_t f = {100, 100, 100};

    // f.count > e.count, so f > e
    EXPECT_EQ(transaction_id_compare(&e, &f), -1);
}

TEST_F(TransactionIdTest, CountIncrement) {
    // Generate multiple IDs quickly - they should have different counts
    // even if they have the same timestamp
    std::vector<transaction_id_t> ids;

    for (int i = 0; i < 10; i++) {
        ids.push_back(transaction_id_get_next());
    }

    // At least some should have the same time (generated within same second)
    // but all should be unique due to count or nanos
    bool same_time_found = false;
    for (size_t i = 0; i < ids.size(); i++) {
        for (size_t j = i + 1; j < ids.size(); j++) {
            if (ids[i].time == ids[j].time && ids[i].nanos == ids[j].nanos) {
                same_time_found = true;
                // Count must be different
                EXPECT_NE(ids[i].count, ids[j].count);
            }
        }
    }
}

// Thread safety test
struct ThreadArg {
    std::vector<transaction_id_t>* ids;
    int count;
};

void* generate_ids(void* arg) {
    ThreadArg* ta = (ThreadArg*)arg;
    for (int i = 0; i < ta->count; i++) {
        ta->ids->push_back(transaction_id_get_next());
    }
    return nullptr;
}

TEST_F(TransactionIdTest, ThreadSafety) {
    const int num_threads = 4;
    const int ids_per_thread = 100;

    std::vector<std::vector<transaction_id_t>> thread_ids(num_threads);
    std::vector<pthread_t> threads(num_threads);
    std::vector<ThreadArg> args(num_threads);

    // Create threads
    for (int i = 0; i < num_threads; i++) {
        thread_ids[i].reserve(ids_per_thread);
        args[i].ids = &thread_ids[i];
        args[i].count = ids_per_thread;
        pthread_create(&threads[i], nullptr, generate_ids, &args[i]);
    }

    // Wait for threads
    for (int i = 0; i < num_threads; i++) {
        pthread_join(threads[i], nullptr);
    }

    // Collect all IDs
    std::vector<transaction_id_t> all_ids;
    for (int i = 0; i < num_threads; i++) {
        all_ids.insert(all_ids.end(), thread_ids[i].begin(), thread_ids[i].end());
    }

    // Check that all IDs are unique
    for (size_t i = 0; i < all_ids.size(); i++) {
        for (size_t j = i + 1; j < all_ids.size(); j++) {
            EXPECT_NE(transaction_id_compare(&all_ids[i], &all_ids[j]), 0);
        }
    }
}

TEST_F(TransactionIdTest, RealisticUsage) {
    // Simulate realistic usage pattern
    transaction_id_t ids[5];

    for (int i = 0; i < 5; i++) {
        ids[i] = transaction_id_get_next();
    }

    // All should be monotonically increasing
    for (int i = 0; i < 4; i++) {
        EXPECT_GT(transaction_id_compare(&ids[i + 1], &ids[i]), 0);
    }

    // Serialize and deserialize
    for (int i = 0; i < 5; i++) {
        uint8_t buffer[24];
        transaction_id_serialize(&ids[i], buffer);

        transaction_id_t deserialized;
        transaction_id_deserialize(&deserialized, buffer);

        EXPECT_EQ(transaction_id_compare(&ids[i], &deserialized), 0);
    }
}