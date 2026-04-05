# Michael-Scott Lock-Free Queue Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement a lock-free queue based on the Michael-Scott algorithm for LRU ordering.

**Architecture:** Lock-free enqueue (tail) and dequeue (head) using CAS operations. Uses dummy/sentinel node approach to simplify empty queue handling.

**Tech Stack:** C11 atomics, memory pool for node allocation

**Prerequisite:** Phase 2 completed

**Spec Reference:** `docs/superpowers/specs/2026-04-05-lockfree-lru-design.md` lines 139-146, 277-328

---

### Task 1: Create Header with Data Structures

**Files:**
- Create: `src/Util/ms_queue.h`

- [ ] **Step 1: Create header with guard and includes**

```c
//
// Michael-Scott Lock-Free Queue
//
// Based on: https://www.cs.rochester.edu/u/scott/papers/1996_PODC_queues.pdf
//

#ifndef WAVEDB_MS_QUEUE_H
#define WAVEDB_MS_QUEUE_H

#include <stdint.h>
#include <stddef.h>
#include <stdatomic.h>

#ifdef __cplusplus
extern "C" {
#endif

// Forward declarations
typedef struct ms_queue_node_t ms_queue_node_t;
typedef struct ms_queue_t ms_queue_t;

#endif // WAVEDB_MS_QUEUE_H
```

- [ ] **Step 2: Define queue node structure**

```c
// Queue node - generic container for lock-free queue
struct ms_queue_node_t {
    _Atomic(void*) data;              // Payload (NULL for holes/removed)
    _Atomic(struct ms_queue_node_t*) next;  // Next node in queue
};
```

- [ ] **Step 3: Define queue structure**

```c
// Michael-Scott lock-free queue
struct ms_queue_t {
    _Atomic(ms_queue_node_t*) head;   // LRU end (for dequeue)
    _Atomic(ms_queue_node_t*) tail;   // MRU end (for enqueue)
    _Atomic(size_t) size;             // Approximate count (including holes)
};
```

- [ ] **Step 4: Define API functions**

```c
/**
 * Initialize a queue.
 *
 * @param queue Queue to initialize
 * @return 0 on success, -1 on failure
 */
int ms_queue_init(ms_queue_t* queue);

/**
 * Destroy a queue.
 *
 * @param queue Queue to destroy
 * @param free_data Function to free data payloads (NULL for no free)
 */
void ms_queue_destroy(ms_queue_t* queue, void (*free_data)(void*));

/**
 * Enqueue an item (lock-free).
 *
 * @param queue Queue to update
 * @param node Pre-allocated node (caller owns allocation)
 * @return 0 on success
 */
int ms_queue_enqueue(ms_queue_t* queue, ms_queue_node_t* node);

/**
 * Dequeue an item (lock-free).
 *
 * @param queue Queue to update
 * @return Node if available, NULL if empty
 */
ms_queue_node_t* ms_queue_dequeue(ms_queue_t* queue);

/**
 * Get approximate size.
 *
 * @param queue Queue to query
 * @return Approximate number of items
 */
size_t ms_queue_size(ms_queue_t* queue);

/**
 * Check if queue is empty.
 *
 * @param queue Queue to query
 * @return 1 if empty, 0 otherwise
 */
int ms_queue_is_empty(ms_queue_t* queue);
```

- [ ] **Step 5: Commit**

```bash
git add src/Util/ms_queue.h
git commit -m "feat(ms-queue): add header with data structures and API"
```

---

### Task 2: Implement Core Queue Operations

**Files:**
- Create: `src/Util/ms_queue.c`
- Modify: `src/Util/CMakeLists.txt`

- [ ] **Step 1: Create implementation file with includes**

```c
//
// Michael-Scott Lock-Free Queue Implementation
//

#include "ms_queue.h"
#include "allocator.h"
#include <stdlib.h>

// Dummy/sentinel node for empty queue
#define DUMMY_NODE ((ms_queue_node_t*)1)
```

- [ ] **Step 2: Implement ms_queue_init**

```c
int ms_queue_init(ms_queue_t* queue) {
    if (queue == NULL) return -1;
    
    // Create dummy node for empty queue
    ms_queue_node_t* dummy = get_clear_memory(sizeof(ms_queue_node_t));
    if (dummy == NULL) return -1;
    
    atomic_init(&dummy->data, NULL);
    atomic_init(&dummy->next, NULL);
    
    atomic_init(&queue->head, dummy);
    atomic_init(&queue->tail, dummy);
    atomic_init(&queue->size, 0);
    
    return 0;
}
```

- [ ] **Step 3: Implement ms_queue_destroy**

```c
void ms_queue_destroy(ms_queue_t* queue, void (*free_data)(void*)) {
    if (queue == NULL) return;
    
    // Drain all nodes
    ms_queue_node_t* node = atomic_load(&queue->head);
    while (node != NULL) {
        ms_queue_node_t* next = atomic_load(&node->next);
        
        if (free_data != NULL) {
            void* data = atomic_load(&node->data);
            if (data != NULL) {
                free_data(data);
            }
        }
        
        free(node);
        node = next;
    }
    
    atomic_init(&queue->head, NULL);
    atomic_init(&queue->tail, NULL);
    atomic_init(&queue->size, 0);
}
```

- [ ] **Step 4: Implement ms_queue_enqueue (lock-free)**

```c
int ms_queue_enqueue(ms_queue_t* queue, ms_queue_node_t* node) {
    if (queue == NULL || node == NULL) return -1;
    
    // Initialize node
    atomic_store(&node->next, NULL);
    
    while (1) {
        ms_queue_node_t* tail = atomic_load(&queue->tail);
        ms_queue_node_t* next = atomic_load(&tail->next);
        
        // Check if tail is still consistent
        if (tail == atomic_load(&queue->tail)) {
            if (next == NULL) {
                // Tail is pointing to last node, try to link new node
                if (atomic_compare_exchange_weak(&tail->next, &next, node)) {
                    // Successfully linked, try to advance tail
                    atomic_compare_exchange_weak(&queue->tail, &tail, node);
                    atomic_fetch_add(&queue->size, 1);
                    return 0;
                }
            } else {
                // Tail not pointing to last node, advance it
                atomic_compare_exchange_weak(&queue->tail, &tail, next);
            }
        }
    }
}
```

- [ ] **Step 5: Implement ms_queue_dequeue (lock-free)**

```c
ms_queue_node_t* ms_queue_dequeue(ms_queue_t* queue) {
    if (queue == NULL) return NULL;
    
    while (1) {
        ms_queue_node_t* head = atomic_load(&queue->head);
        ms_queue_node_t* tail = atomic_load(&queue->tail);
        ms_queue_node_t* next = atomic_load(&head->next);
        
        // Check if head is still consistent
        if (head == atomic_load(&queue->head)) {
            if (head == tail) {
                // Queue might be empty
                if (next == NULL) {
                    // Queue is empty
                    return NULL;
                }
                // Tail is falling behind, advance it
                atomic_compare_exchange_weak(&queue->tail, &tail, next);
            } else {
                // Read value before CAS (ABA problem prevention)
                ms_queue_node_t* node = next;
                
                // Try to swing head to next node
                if (atomic_compare_exchange_weak(&queue->head, &head, node)) {
                    // Successfully dequeued
                    atomic_fetch_sub(&queue->size, 1);
                    
                    // Return the OLD head (dummy), not the new value
                    // Caller is responsible for freeing old head
                    // The dequeued value is in node->data
                    return head;
                }
            }
        }
    }
}
```

- [ ] **Step 6: Implement size and empty helpers**

```c
size_t ms_queue_size(ms_queue_t* queue) {
    if (queue == NULL) return 0;
    return atomic_load(&queue->size);
}

int ms_queue_is_empty(ms_queue_t* queue) {
    if (queue == NULL) return 1;
    
    ms_queue_node_t* head = atomic_load(&queue->head);
    ms_queue_node_t* tail = atomic_load(&queue->tail);
    
    return head == tail && atomic_load(&head->next) == NULL;
}
```

- [ ] **Step 7: Update CMakeLists.txt**

Add `src/Util/ms_queue.c` to source files.

- [ ] **Step 8: Commit**

```bash
git add src/Util/ms_queue.c src/Util/CMakeLists.txt
git commit -m "feat(ms-queue): implement Michael-Scott lock-free queue"
```

---

### Task 3: Write Unit Tests

**Files:**
- Create: `tests/test_ms_queue.cpp`

- [ ] **Step 1: Create test file**

```cpp
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
    // The value is in the NEXT node after dequeue
    
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
        threads.emplace_back([&]() {
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

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
```

- [ ] **Step 2: Update tests CMakeLists.txt**

Add `test_ms_queue.cpp` to test files.

- [ ] **Step 3: Build and run tests**

```bash
cd build && cmake .. && make test_ms_queue
./tests/test_ms_queue
```

Expected: All tests pass

- [ ] **Step 4: Commit**

```bash
git add tests/test_ms_queue.cpp tests/CMakeLists.txt
git commit -m "test(ms-queue): add unit tests for lock-free queue"
```

---

### Task 4: Add Memory Pool Integration

**Files:**
- Modify: `src/Util/ms_queue.h`
- Modify: `src/Util/ms_queue.c`

- [ ] **Step 1: Add memory pool allocation option**

Add function for allocating queue nodes from memory pool:

```c
// In ms_queue.h after API declarations

/**
 * Allocate a queue node from the memory pool.
 *
 * @param data Initial payload
 * @return New node or NULL on failure
 */
ms_queue_node_t* ms_queue_node_alloc(void* data);

/**
 * Free a queue node to the memory pool.
 *
 * @param node Node to free
 */
void ms_queue_node_free(ms_queue_node_t* node);
```

- [ ] **Step 2: Implement memory pool allocation**

```c
// In ms_queue.c, add include
#include "memory_pool.h"

ms_queue_node_t* ms_queue_node_alloc(void* data) {
    ms_queue_node_t* node = memory_pool_alloc(sizeof(ms_queue_node_t));
    if (node == NULL) return NULL;
    
    atomic_init(&node->data, data);
    atomic_init(&node->next, NULL);
    
    return node;
}

void ms_queue_node_free(ms_queue_node_t* node) {
    if (node != NULL) {
        memory_pool_free(node, sizeof(ms_queue_node_t));
    }
}
```

- [ ] **Step 3: Add test using memory pool**

```cpp
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
```

- [ ] **Step 4: Build and run tests**

```bash
cd build && make test_ms_queue
./tests/test_ms_queue
```

Expected: All tests pass

- [ ] **Step 5: Commit**

```bash
git add src/Util/ms_queue.h src/Util/ms_queue.c tests/test_ms_queue.cpp
git commit -m "feat(ms-queue): add memory pool allocation for nodes"
```

---

## Success Criteria for Phase 3

1. All unit tests pass
2. Lock-free enqueue works correctly under concurrent access
3. Lock-free dequeue works correctly under concurrent access
4. Memory pool allocation works
5. No memory leaks (valgrind/ASAN clean)
6. Thread sanitization passes (TSAN clean)