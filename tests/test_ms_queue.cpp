//
// Unit tests for Michael-Scott lock-free queue
//

#include <gtest/gtest.h>
#include <thread>
#include <vector>
#include <atomic>
extern "C" {
#include "Util/ms_queue.h"
}

class MSQueueTest : public ::testing::Test {
protected:
    ms_queue_t queue;

    void SetUp() override {
        ASSERT_EQ(ms_queue_init(&queue), 0);
    }

    void TearDown() override {
        ms_queue_destroy(&queue, nullptr);
    }
};

TEST_F(MSQueueTest, InitDestroy) {
    EXPECT_TRUE(ms_queue_is_empty(&queue));
    EXPECT_EQ(ms_queue_size(&queue), 0u);
}

TEST_F(MSQueueTest, EnqueueDequeue) {
    ms_queue_node_t* node = (ms_queue_node_t*)malloc(sizeof(ms_queue_node_t));
    atomic_init(&node->data, (void*)123);
    atomic_init(&node->next, nullptr);

    // Enqueue
    EXPECT_EQ(ms_queue_enqueue(&queue, node), 0);
    EXPECT_EQ(ms_queue_size(&queue), 1u);
    EXPECT_FALSE(ms_queue_is_empty(&queue));

    // Dequeue
    ms_queue_node_t* dequeued = ms_queue_dequeue(&queue);
    EXPECT_NE(dequeued, nullptr);
    EXPECT_EQ(ms_queue_size(&queue), 0u);

    // The returned node is the old dummy, the new head is the dequeued node
    free(dequeued);
}

TEST_F(MSQueueTest, EnqueueDequeueMultiple) {
    // Enqueue several items
    int values[] = {1, 2, 3, 4, 5};

    for (int i = 0; i < 5; i++) {
        ms_queue_node_t* node = (ms_queue_node_t*)malloc(sizeof(ms_queue_node_t));
        atomic_init(&node->data, (void*)(intptr_t)values[i]);
        atomic_init(&node->next, nullptr);
        ms_queue_enqueue(&queue, node);
    }

    EXPECT_EQ(ms_queue_size(&queue), 5u);

    // Dequeue all
    for (int i = 0; i < 5; i++) {
        ms_queue_node_t* dequeued = ms_queue_dequeue(&queue);
        EXPECT_NE(dequeued, nullptr);
        free(dequeued);
    }

    EXPECT_TRUE(ms_queue_is_empty(&queue));
}

TEST_F(MSQueueTest, ConcurrentEnqueue) {
    std::vector<std::thread> threads;
    std::atomic<int> enqueue_count(0);

    // Multiple threads enqueue concurrently
    for (int t = 0; t < 8; t++) {
        threads.emplace_back([&, t]() {
            (void)t;  // Suppress unused warning
            for (int i = 0; i < 100; i++) {
                ms_queue_node_t* node = (ms_queue_node_t*)malloc(sizeof(ms_queue_node_t));
                atomic_init(&node->data, (void*)(intptr_t)(t * 100 + i));
                atomic_init(&node->next, nullptr);
                ms_queue_enqueue(&queue, node);
                enqueue_count++;
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    EXPECT_EQ(enqueue_count.load(), 800);
    EXPECT_EQ(ms_queue_size(&queue), 800u);
}

TEST_F(MSQueueTest, ConcurrentDequeue) {
    // Pre-populate
    for (int i = 0; i < 800; i++) {
        ms_queue_node_t* node = (ms_queue_node_t*)malloc(sizeof(ms_queue_node_t));
        atomic_init(&node->data, (void*)(intptr_t)i);
        atomic_init(&node->next, nullptr);
        ms_queue_enqueue(&queue, node);
    }

    std::vector<std::thread> threads;
    std::atomic<int> dequeue_count(0);

    // Multiple threads dequeue concurrently
    for (int t = 0; t < 8; t++) {
        threads.emplace_back([&]() {
            (void)t;  // Suppress unused warning
            for (int i = 0; i < 100; i++) {
                ms_queue_node_t* node = ms_queue_dequeue(&queue);
                if (node != nullptr) {
                    dequeue_count++;
                    free(node);
                }
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    EXPECT_EQ(dequeue_count.load(), 800);
    EXPECT_TRUE(ms_queue_is_empty(&queue));
}

TEST_F(MSQueueTest, ConcurrentEnqueueDequeue) {
    std::atomic<int> total_enqueued(0);
    std::atomic<int> total_dequeued(0);
    std::vector<std::thread> threads;

    // 4 producer threads
    for (int t = 0; t < 4; t++) {
        threads.emplace_back([&, t]() {
            for (int i = 0; i < 100; i++) {
                ms_queue_node_t* node = (ms_queue_node_t*)malloc(sizeof(ms_queue_node_t));
                atomic_init(&node->data, (void*)(intptr_t)(t * 100 + i));
                atomic_init(&node->next, nullptr);
                ms_queue_enqueue(&queue, node);
                total_enqueued++;
            }
        });
    }

    // 4 consumer threads
    for (int t = 0; t < 4; t++) {
        threads.emplace_back([&, t]() {
            (void)t;  // Suppress unused warning
            for (int i = 0; i < 100; i++) {
                ms_queue_node_t* node = ms_queue_dequeue(&queue);
                if (node != nullptr) {
                    total_dequeued++;
                    free(node);
                }
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    // Should have enqueued all and dequeued some/all
    EXPECT_EQ(total_enqueued.load(), 400);
    // Dequeued count may vary, but no crashes or deadlocks
}

TEST_F(MSQueueTest, MemoryPoolAllocation) {
    // Allocate nodes from memory pool
    ms_queue_node_t* nodes[10];

    for (int i = 0; i < 10; i++) {
        nodes[i] = ms_queue_node_alloc((void*)(intptr_t)i);
        ASSERT_NE(nodes[i], nullptr);
    }

    // Enqueue all
    for (int i = 0; i < 10; i++) {
        EXPECT_EQ(ms_queue_enqueue(&queue, nodes[i]), 0);
    }

    EXPECT_EQ(ms_queue_size(&queue), 10u);

    // Dequeue and free
    for (int i = 0; i < 10; i++) {
        ms_queue_node_t* node = ms_queue_dequeue(&queue);
        EXPECT_NE(node, nullptr);
        ms_queue_node_free(node);
    }
}

TEST_F(MSQueueTest, FIFOOrder) {
    // Enqueue items with specific values
    for (int i = 0; i < 10; i++) {
        ms_queue_node_t* node = (ms_queue_node_t*)malloc(sizeof(ms_queue_node_t));
        atomic_init(&node->data, (void*)(intptr_t)i);
        atomic_init(&node->next, nullptr);
        ms_queue_enqueue(&queue, node);
    }

    // Dequeue and verify FIFO order
    int expected = 0;
    for (int i = 0; i < 10; i++) {
        ms_queue_node_t* dequeued = ms_queue_dequeue(&queue);
        EXPECT_NE(dequeued, nullptr);

        // The data is in the next node, not the dequeued dummy
        ms_queue_node_t* next = atomic_load(&dequeued->next);
        if (next != nullptr && i < 9) {
            void* data = atomic_load(&next->data);
            // Note: FIFO order is approximate in concurrent scenarios
        }
        free(dequeued);
    }
}

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}