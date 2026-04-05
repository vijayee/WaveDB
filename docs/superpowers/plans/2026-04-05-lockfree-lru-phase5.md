# LRU Eviction and Purge Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement eviction logic and background purge for the lock-free LRU cache.

**Architecture:** Eviction walks from head (LRU end), purging removes holes. Both are lock-free operations using CAS.

**Tech Stack:** C11 atomics, memory pool

**Prerequisites:** Phase 4 completed

**Spec Reference:** `docs/superpowers/specs/2026-04-05-lockfree-lru-design.md` lines 333-415

---

### Task 1: Implement Delete Operation

**Files:**
- Modify: `src/Database/lockfree_lru.c`

- [ ] **Step 1: Implement lockfree_lru_cache_delete**

```c
void lockfree_lru_cache_delete(lockfree_lru_cache_t* lru, path_t* path) {
    if (lru == NULL || path == NULL) {
        return;
    }
    
    size_t shard_idx = get_shard_index(lru, path);
    lockfree_lru_shard_t* shard = &lru->shards[shard_idx];
    
    // Remove from hashmap (returns the entry)
    lru_entry_t* entry = concurrent_hashmap_remove(shard->map, path);
    if (entry == NULL) {
        return;
    }
    
    // Mark node as hole (lock-free)
    lru_node_t* node = atomic_load(&entry->node);
    if (node != NULL) {
        atomic_store(&node->entry, NULL);
        atomic_fetch_add(&shard->queue.hole_count, 1);
    }
    
    // Update memory tracking
    atomic_fetch_sub(&shard->current_memory, entry->memory_size);
    atomic_fetch_sub(&shard->entry_count, 1);
    
    // Dereference value
    if (entry->value != NULL) {
        identifier_destroy(entry->value);
    }
    
    // Free path (we own it now from removal)
    if (entry->path != NULL) {
        path_destroy(entry->path);
    }
    
    // Free entry
    memory_pool_free(entry, sizeof(lru_entry_t));
    
    // Free the input path since we took ownership
    path_destroy(path);
}
```

- [ ] **Step 2: Commit**

```bash
git add src/Database/lockfree_lru.c
git commit -m "feat(lockfree-lru): implement delete operation with hole marking"
```

---

### Task 2: Implement Eviction Logic

**Files:**
- Modify: `src/Database/lockfree_lru.c`

- [ ] **Step 1: Add eviction helper**

```c
// Maximum entries to check during eviction before giving up
#define MAX_EVICTION_SCAN 100

// Evict least recently used entry
static int evict_lru_entry(lockfree_lru_shard_t* shard) {
    int scanned = 0;
    
    while (atomic_load(&shard->queue.node_count) > 0 && scanned < MAX_EVICTION_SCAN) {
        lru_node_t* old_head = lru_dequeue(&shard->queue);
        if (old_head == NULL) {
            return 0;  // Queue empty
        }
        
        // Get the next node (the actual entry holder)
        lru_node_t* next = atomic_load(&old_head->next);
        if (next == NULL) {
            // Put back the dummy
            lru_enqueue(&shard->queue, old_head);
            return 0;
        }
        
        lru_entry_t* entry = atomic_load(&next->entry);
        
        if (entry != NULL) {
            // Try to claim this entry for eviction
            if (atomic_compare_exchange_strong(&next->entry, &entry, NULL)) {
                // Successfully claimed, now remove from hashmap
                concurrent_hashmap_remove(shard->map, entry->path);
                
                // Update memory tracking
                atomic_fetch_sub(&shard->current_memory, entry->memory_size);
                atomic_fetch_sub(&shard->entry_count, 1);
                
                // Free entry resources
                if (entry->path != NULL) {
                    path_destroy(entry->path);
                }
                if (entry->value != NULL) {
                    identifier_destroy(entry->value);
                }
                memory_pool_free(entry, sizeof(lru_entry_t));
                
                // Free old head (was dummy)
                memory_pool_free(old_head, sizeof(lru_node_t));
                
                // Mark next as hole (it's now removed from queue)
                // No need - we dequeued it
                
                return 1;  // Eviction successful
            }
        }
        
        // Entry was NULL (hole) or CAS failed, free old head and continue
        memory_pool_free(old_head, sizeof(lru_node_t));
        atomic_fetch_sub(&shard->queue.hole_count, 1);
        scanned++;
    }
    
    return 0;  // No entries evicted
}
```

- [ ] **Step 2: Add memory check in put**

Modify `lockfree_lru_cache_put` to check memory and evict if needed:

```c
// In lockfree_lru_cache_put, after creating entry but before inserting:

// Check memory budget and evict if needed
while (atomic_load(&shard->current_memory) + entry->memory_size > shard->max_memory) {
    if (!evict_lru_entry(shard)) {
        // Can't evict more, cache is full
        break;
    }
}
```

- [ ] **Step 3: Commit**

```bash
git add src/Database/lockfree_lru.c
git commit -m "feat(lockfree-lru): implement eviction logic"
```

---

### Task 3: Implement Purge Operation

**Files:**
- Modify: `src/Database/lockfree_lru.h`
- Modify: `src/Database/lockfree_lru.c`

- [ ] **Step 1: Add purge function declaration to header**

```c
/**
 * Purge holes from the queue (background cleanup).
 *
 * @param lru Cache to purge
 * @param max_batch Maximum holes to purge in one call (0 for unlimited)
 * @return Number of holes purged
 */
size_t lockfree_lru_cache_purge(lockfree_lru_cache_t* lru, size_t max_batch);
```

- [ ] **Step 2: Implement purge function**

```c
// Maximum holes to purge per call
#define MAX_PURGE_BATCH 100

size_t lockfree_lru_cache_purge(lockfree_lru_cache_t* lru, size_t max_batch) {
    if (lru == NULL) return 0;
    
    if (max_batch == 0) {
        max_batch = MAX_PURGE_BATCH;
    }
    
    size_t total_purged = 0;
    
    for (size_t i = 0; i < lru->num_shards; i++) {
        lockfree_lru_shard_t* shard = &lru->shards[i];
        
        // Try to claim purge ownership
        uint8_t expected = 0;
        if (!atomic_compare_exchange_strong(&shard->purging, &expected, 1)) {
            continue;  // Another thread is purging this shard
        }
        
        // Drain holes from head of queue
        size_t holes_purged = 0;
        while (atomic_load(&shard->queue.hole_count) > 0 && holes_purged < max_batch) {
            lru_node_t* head = atomic_load(&shard->queue.head);
            lru_node_t* next = atomic_load(&head->next);
            
            if (next == NULL) {
                break;  // Queue empty (only dummy left)
            }
            
            lru_entry_t* entry = atomic_load(&next->entry);
            if (entry == NULL) {
                // This is a hole, advance head
                if (atomic_compare_exchange_strong(&shard->queue.head, &head, next)) {
                    memory_pool_free(head, sizeof(lru_node_t));
                    atomic_fetch_sub(&shard->queue.hole_count, 1);
                    holes_purged++;
                    total_purged++;
                }
            } else {
                break;  // Non-hole entry, stop draining
            }
        }
        
        atomic_store(&shard->purging, 0);
    }
    
    return total_purged;
}
```

- [ ] **Step 3: Commit**

```bash
git add src/Database/lockfree_lru.h src/Database/lockfree_lru.c
git commit -m "feat(lockfree-lru): implement purge operation for hole cleanup"
```

---

### Task 4: Add Eviction and Purge Tests

**Files:**
- Modify: `tests/test_lockfree_lru.cpp`

- [ ] **Step 1: Add eviction test**

```cpp
TEST_F(LockfreeLRUTest, Eviction) {
    // Create cache with small memory limit
    lockfree_lru_cache_destroy(lru);
    lru = lockfree_lru_cache_create(1024, 0);  // 1 KB limit
    ASSERT_NE(lru, nullptr);
    
    // Add entries until eviction occurs
    int evicted_count = 0;
    for (int i = 0; i < 100; i++) {
        char key[32];
        snprintf(key, sizeof(key), "key%d", i);
        path_t* path = make_simple_path(key);
        identifier_t* value = make_simple_value("value_data_for_testing_eviction");
        
        identifier_t* evicted = lockfree_lru_cache_put(lru, path, value);
        if (evicted != nullptr) {
            identifier_destroy(evicted);
            evicted_count++;
        }
    }
    
    // Some entries should have been evicted
    EXPECT_GT(evicted_count, 0);
    
    // Memory should be within budget
    size_t memory = lockfree_lru_cache_memory(lru);
    EXPECT_LE(memory, 1024u);
}
```

- [ ] **Step 2: Add delete and purge test**

```cpp
TEST_F(LockfreeLRUTest, DeleteAndPurge) {
    // Add several entries
    for (int i = 0; i < 10; i++) {
        char key[32];
        snprintf(key, sizeof(key), "key%d", i);
        path_t* path = make_simple_path(key);
        identifier_t* value = make_simple_value("value");
        lockfree_lru_cache_put(lru, path, value);
    }
    
    EXPECT_EQ(lockfree_lru_cache_size(lru), 10u);
    
    // Delete half
    for (int i = 0; i < 5; i++) {
        char key[32];
        snprintf(key, sizeof(key), "key%d", i);
        path_t* path = make_simple_path(key);
        lockfree_lru_cache_delete(lru, path);
    }
    
    EXPECT_EQ(lockfree_lru_cache_size(lru), 5u);
    
    // Purge holes
    size_t purged = lockfree_lru_cache_purge(lru, 100);
    EXPECT_GT(purged, 0u);
    
    // Remaining entries still accessible
    for (int i = 5; i < 10; i++) {
        char key[32];
        snprintf(key, sizeof(key), "key%d", i);
        path_t* path = make_simple_path(key);
        identifier_t* cached = lockfree_lru_cache_get(lru, path);
        EXPECT_NE(cached, nullptr) << "Key " << key << " should still exist";
        if (cached) identifier_destroy(cached);
        path_destroy(path);
    }
}
```

- [ ] **Step 3: Add memory tracking test**

```cpp
TEST_F(LockfreeLRUTest, MemoryTracking) {
    size_t initial_memory = lockfree_lru_cache_memory(lru);
    EXPECT_EQ(initial_memory, 0u);
    
    // Add entry
    path_t* path = make_simple_path("key1");
    identifier_t* value = make_simple_value("value");
    lockfree_lru_cache_put(lru, path, value);
    
    size_t after_put = lockfree_lru_cache_memory(lru);
    EXPECT_GT(after_put, 0u);
    
    // Get entry (should not change memory)
    path_t* path_copy = path_copy(path);
    identifier_t* cached = lockfree_lru_cache_get(lru, path_copy);
    EXPECT_EQ(lockfree_lru_cache_memory(lru), after_put);
    if (cached) identifier_destroy(cached);
    
    // Delete entry
    path_t* path_del = make_simple_path("key1");
    lockfree_lru_cache_delete(lru, path_del);
    EXPECT_EQ(lockfree_lru_cache_memory(lru), 0u);
}
```

- [ ] **Step 4: Build and run tests**

```bash
cd build && make test_lockfree_lru
./tests/test_lockfree_lru
```

Expected: All tests pass

- [ ] **Step 5: Commit**

```bash
git add tests/test_lockfree_lru.cpp
git commit -m "test(lockfree-lru): add eviction, delete, and purge tests"
```

---

### Task 5: Add Concurrent Access Tests

**Files:**
- Modify: `tests/test_lockfree_lru.cpp`

- [ ] **Step 1: Add concurrent get test**

```cpp
TEST_F(LockfreeLRUTest, ConcurrentGets) {
    // Pre-populate cache
    for (int i = 0; i < 100; i++) {
        char key[32];
        snprintf(key, sizeof(key), "key%d", i);
        path_t* path = make_simple_path(key);
        identifier_t* value = make_simple_value("value");
        lockfree_lru_cache_put(lru, path, value);
    }
    
    std::atomic<int> success_count(0);
    std::vector<std::thread> threads;
    
    // Concurrent reads from multiple threads
    for (int t = 0; t < 8; t++) {
        threads.emplace_back([&]() {
            for (int i = 0; i < 100; i++) {
                char key[32];
                snprintf(key, sizeof(key), "key%d", i);
                path_t* path = make_simple_path(key);
                identifier_t* cached = lockfree_lru_cache_get(lru, path);
                if (cached != nullptr) {
                    success_count++;
                    identifier_destroy(cached);
                }
                path_destroy(path);
            }
        });
    }
    
    for (auto& thread : threads) {
        thread.join();
    }
    
    // All reads should succeed (800 total)
    EXPECT_EQ(success_count.load(), 800);
}
```

- [ ] **Step 2: Add concurrent put-get mix test**

```cpp
TEST_F(LockfreeLRUTest, ConcurrentPutGet) {
    std::atomic<int> put_count(0);
    std::atomic<int> get_count(0);
    std::vector<std::thread> threads;
    
    // 4 producer threads
    for (int t = 0; t < 4; t++) {
        threads.emplace_back([&, t]() {
            for (int i = 0; i < 50; i++) {
                char key[32];
                snprintf(key, sizeof(key), "thread%d_key%d", t, i);
                path_t* path = make_simple_path(key);
                identifier_t* value = make_simple_value("value");
                lockfree_lru_cache_put(lru, path, value);
                put_count++;
            }
        });
    }
    
    // 4 consumer threads
    for (int t = 0; t < 4; t++) {
        threads.emplace_back([&, t]() {
            for (int i = 0; i < 50; i++) {
                char key[32];
                snprintf(key, sizeof(key), "thread%d_key%d", t, i);
                path_t* path = make_simple_path(key);
                identifier_t* cached = lockfree_lru_cache_get(lru, path);
                if (cached != nullptr) {
                    get_count++;
                    identifier_destroy(cached);
                }
                path_destroy(path);
            }
        });
    }
    
    for (auto& thread : threads) {
        thread.join();
    }
    
    // All puts should complete
    EXPECT_EQ(put_count.load(), 200);
    // Gets may or may not find entries (timing dependent)
    EXPECT_GE(get_count.load(), 0);
}
```

- [ ] **Step 3: Add stress test**

```cpp
TEST_F(LockfreeLRUTest, StressTest) {
    const int iterations = 1000;
    std::atomic<int> operations(0);
    std::vector<std::thread> threads;
    
    // Mixed operations from multiple threads
    for (int t = 0; t < 8; t++) {
        threads.emplace_back([&, t]() {
            for (int i = 0; i < iterations; i++) {
                char key[32];
                snprintf(key, sizeof(key), "stress_key%d", i % 100);  // 100 unique keys
                
                int op = (i + t) % 3;  // Rotate between get, put, delete
                path_t* path = make_simple_path(key);
                
                if (op == 0) {
                    identifier_t* cached = lockfree_lru_cache_get(lru, path);
                    if (cached) identifier_destroy(cached);
                    path_destroy(path);
                } else if (op == 1) {
                    identifier_t* value = make_simple_value("stress_value");
                    lockfree_lru_cache_put(lru, path, value);
                } else {
                    lockfree_lru_cache_delete(lru, path);
                }
                
                operations++;
            }
        });
    }
    
    for (auto& thread : threads) {
        thread.join();
    }
    
    // All operations should complete without crashes
    EXPECT_EQ(operations.load(), 8 * iterations);
    
    // Purge any accumulated holes
    lockfree_lru_cache_purge(lru, 1000);
}
```

- [ ] **Step 4: Build and run tests**

```bash
cd build && make test_lockfree_lru
./tests/test_lockfree_lru
```

Expected: All tests pass, no crashes or hangs

- [ ] **Step 5: Commit**

```bash
git add tests/test_lockfree_lru.cpp
git commit -m "test(lockfree-lru): add concurrent access tests"
```

---

## Success Criteria for Phase 5

1. All unit tests pass
2. Eviction works correctly under memory pressure
3. Delete marks nodes as holes
4. Purge cleans up holes
5. Concurrent operations work without crashes
6. No memory leaks (valgrind/ASAN clean)
7. Thread sanitization passes (TSAN clean)