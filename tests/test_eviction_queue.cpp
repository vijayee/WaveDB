#include <gtest/gtest.h>
#include "Database/eviction_queue.h"
#include <thread>
#include <vector>

TEST(EvictionQueueTest, PushAndDrain) {
    eviction_queue_t queue;
    eviction_queue_init(&queue);

    EXPECT_EQ(eviction_queue_push(&queue, 42), 0);
    EXPECT_EQ(eviction_queue_push(&queue, 100), 0);

    uint64_t out[4];
    size_t n = eviction_queue_drain(&queue, out, 4);
    EXPECT_EQ(n, 2u);
    EXPECT_EQ(out[0], 42u);
    EXPECT_EQ(out[1], 100u);
}

TEST(EvictionQueueTest, OverflowReturnsError) {
    eviction_queue_t queue;
    eviction_queue_init(&queue);

    for (uint64_t i = 0; i < EVICTION_QUEUE_CAPACITY; i++) {
        EXPECT_EQ(eviction_queue_push(&queue, i + 1), 0);
    }
    EXPECT_EQ(eviction_queue_push(&queue, 999), -1);
}

TEST(EvictionQueueTest, DrainEmpty) {
    eviction_queue_t queue;
    eviction_queue_init(&queue);

    uint64_t out[4];
    size_t n = eviction_queue_drain(&queue, out, 4);
    EXPECT_EQ(n, 0u);
}

TEST(EvictionQueueTest, PartialDrain) {
    eviction_queue_t queue;
    eviction_queue_init(&queue);

    for (uint64_t i = 0; i < 8; i++) {
        eviction_queue_push(&queue, i + 1);
    }

    uint64_t out[3];
    size_t n = eviction_queue_drain(&queue, out, 3);
    EXPECT_EQ(n, 3u);
    EXPECT_EQ(out[0], 1u);
    EXPECT_EQ(out[1], 2u);
    EXPECT_EQ(out[2], 3u);

    uint64_t out2[8];
    n = eviction_queue_drain(&queue, out2, 8);
    EXPECT_EQ(n, 5u);
    EXPECT_EQ(out2[0], 4u);
}

TEST(EvictionQueueTest, ConcurrentPushDrain) {
    eviction_queue_t queue;
    eviction_queue_init(&queue);

    const int N = 1000;
    std::thread pusher([&]() {
        for (uint64_t i = 1; i <= N; i++) {
            while (eviction_queue_push(&queue, i) != 0) {
                // spin until space available
            }
        }
    });

    std::vector<uint64_t> collected;
    std::thread drainer([&]() {
        while (collected.size() < (size_t)N) {
            uint64_t out[16];
            size_t n = eviction_queue_drain(&queue, out, 16);
            for (size_t i = 0; i < n; i++) {
                collected.push_back(out[i]);
            }
        }
    });

    pusher.join();
    drainer.join();

    EXPECT_EQ(collected.size(), (size_t)N);
}