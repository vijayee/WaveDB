# Lock-Free LRU Core Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement the core lock-free LRU cache using the concurrent hashmap and Michael-Scott queue.

**Architecture:** LRU entries stored in concurrent hashmap, ordered by Michael-Scott queue. "Holes" mechanism allows lock-free promotion without removing nodes from queue middle.

**Tech Stack:** C11 atomics, concurrent hashmap, MS queue, memory pool

**Prerequisites:** Phases 1-3 completed

**Spec Reference:** `docs/superpowers/specs/2026-04-05-lockfree-lru-design.md` lines 133-272

---

### Task 1: Create Header File with Data Structures

**Files:**
- Create: `src/Database/lockfree_lru.h`

- [ ] **Step 1: Create header with includes**

```c
//
// Lock-Free LRU Cache
//
// Based on eBay's high-throughput LRU design with Michael-Scott lock-free queue
//

#ifndef WAVEDB_LOCKFREE_LRU_H
#define WAVEDB_LOCKFREE_LRU_H

#include <stdint.h>
#include <stddef.h>
#include <stdatomic.h>
#include "../RefCounter/refcounter.h"
#include "../HBTrie/path.h"
#include "../HBTrie/identifier.h"

#ifdef __cplusplus
extern "C" {
#endif

// Forward declarations
typedef struct lru_node_t lru_node_t;
typedef struct lru_entry_t lru_entry_t;
typedef struct lru_queue_t lru_queue_t;
typedef struct lockfree_lru_shard_t lockfree_lru_shard_t;
typedef struct lockfree_lru_cache_t lockfree_lru_cache_t;

#endif // WAVEDB_LOCKFREE_LRU_H
```

- [ ] **Step 2: Define LRU node structure**

```c
// LRU node - lives in the lock-free queue
// Head side = LRU, Tail side = MRU
struct lru_node_t {
    _Atomic(lru_entry_t*) entry;    // NULL = hole (marked for cleanup)
    _Atomic(lru_node_t*) next;       // Next in queue (toward MRU)
};
```

- [ ] **Step 3: Define LRU entry structure**

```c
// LRU entry - lives in the concurrent hashmap
struct lru_entry_t {
    refcounter_t refcounter;         // MUST be first
    path_t* path;                     // Key (immutable)
    identifier_t* value;              // Value (reference counted)
    _Atomic(lru_node_t*) node;       // Current position in LRU queue
    size_t memory_size;               // Memory footprint
};
```

- [ ] **Step 4: Define LRU queue structure**

```c
// Lock-free LRU queue (Michael-Scott)
struct lru_queue_t {
    _Atomic(lru_node_t*) head;       // LRU end (for eviction)
    _Atomic(lru_node_t*) tail;       // MRU end (for insertion)
    _Atomic(size_t) node_count;      // Approximate count (including holes)
    _Atomic(size_t) hole_count;      // Holes pending cleanup
};
```

- [ ] **Step 5: Define shard structure**

```c
// Per-shard LRU state
struct lockfree_lru_shard_t {
    struct concurrent_hashmap_t* map; // path_t* -> lru_entry_t*
    lru_queue_t queue;                // Lock-free LRU queue
    
    _Atomic(size_t) current_memory;
    _Atomic(size_t) entry_count;
    size_t max_memory;
    
    _Atomic(uint8_t) purging;         // Purge in progress flag
};
```

- [ ] **Step 6: Define top-level cache structure**

```c
// Top-level cache
struct lockfree_lru_cache_t {
    lockfree_lru_shard_t* shards;
    uint16_t num_shards;
    size_t total_max_memory;
};
```

- [ ] **Step 7: Define API functions**

```c
/**
 * Create a lock-free LRU cache.
 *
 * @param max_memory_bytes Maximum memory budget (0 for default)
 * @param num_shards Number of shards (0 for auto-scale)
 * @return New cache or NULL on failure
 */
lockfree_lru_cache_t* lockfree_lru_cache_create(size_t max_memory_bytes, uint16_t num_shards);

/**
 * Destroy an LRU cache.
 *
 * @param lru Cache to destroy
 */
void lockfree_lru_cache_destroy(lockfree_lru_cache_t* lru);

/**
 * Get a value from the cache (lock-free).
 *
 * Updates position to most recently used if found.
 *
 * @param lru Cache to query
 * @param path Path to look up
 * @return Value if found (reference counted), NULL if not
 */
identifier_t* lockfree_lru_cache_get(lockfree_lru_cache_t* lru, path_t* path);

/**
 * Put a value into the cache.
 *
 * @param lru Cache to update
 * @param path Path key (takes ownership)
 * @param value Value to store (takes ownership)
 * @return Old value if replaced (caller must destroy), NULL otherwise
 */
identifier_t* lockfree_lru_cache_put(lockfree_lru_cache_t* lru, path_t* path, identifier_t* value);

/**
 * Delete a value from the cache.
 *
 * @param lru Cache to update
 * @param path Path to delete
 */
void lockfree_lru_cache_delete(lockfree_lru_cache_t* lru, path_t* path);

/**
 * Get approximate entry count.
 *
 * @param lru Cache to query
 * @return Approximate number of entries
 */
size_t lockfree_lru_cache_size(lockfree_lru_cache_t* lru);

/**
 * Get approximate memory usage.
 *
 * @param lru Cache to query
 * @return Approximate memory usage in bytes
 */
size_t lockfree_lru_cache_memory(lockfree_lru_cache_t* lru);
```

- [ ] **Step 8: Commit**

```bash
git add src/Database/lockfree_lru.h
git commit -m "feat(lockfree-lru): add header with data structures and API"
```

---

### Task 2: Implement LRU Queue Operations

**Files:**
- Create: `src/Database/lockfree_lru.c`
- Modify: `src/Database/CMakeLists.txt`

- [ ] **Step 1: Create implementation file with includes**

```c
//
// Lock-Free LRU Cache Implementation
//

#include "lockfree_lru.h"
#include "Util/concurrent_hashmap.h"
#include "Util/allocator.h"
#include "Util/memory_pool.h"
#include <stdlib.h>
#include <string.h>

// Default memory limit
#define DEFAULT_MAX_MEMORY_MB 50
```

- [ ] **Step 2: Implement queue init/destroy helpers**

```c
// Initialize LRU queue
static void lru_queue_init(lru_queue_t* queue) {
    // Create dummy node for empty queue
    lru_node_t* dummy = memory_pool_alloc(sizeof(lru_node_t));
    atomic_init(&dummy->entry, NULL);
    atomic_init(&dummy->next, NULL);
    
    atomic_init(&queue->head, dummy);
    atomic_init(&queue->tail, dummy);
    atomic_init(&queue->node_count, 0);
    atomic_init(&queue->hole_count, 0);
}

// Destroy LRU queue
static void lru_queue_destroy(lru_queue_t* queue) {
    lru_node_t* node = atomic_load(&queue->head);
    while (node != NULL) {
        lru_node_t* next = atomic_load(&node->next);
        memory_pool_free(node, sizeof(lru_node_t));
        node = next;
    }
}
```

- [ ] **Step 3: Implement lock-free enqueue**

```c
// Enqueue a node at tail (MRU position) - lock-free
static void lru_enqueue(lru_queue_t* queue, lru_node_t* node) {
    atomic_store(&node->next, NULL);
    
    while (1) {
        lru_node_t* tail = atomic_load(&queue->tail);
        lru_node_t* next = atomic_load(&tail->next);
        
        if (tail == atomic_load(&queue->tail)) {
            if (next == NULL) {
                if (atomic_compare_exchange_weak(&tail->next, &next, node)) {
                    atomic_compare_exchange_weak(&queue->tail, &tail, node);
                    atomic_fetch_add(&queue->node_count, 1);
                    return;
                }
            } else {
                atomic_compare_exchange_weak(&queue->tail, &tail, next);
            }
        }
    }
}
```

- [ ] **Step 4: Implement lock-free dequeue (for holes)**

```c
// Dequeue from head (LRU position) - lock-free
// Returns the old head (to be freed), or NULL if empty
static lru_node_t* lru_dequeue(lru_queue_t* queue) {
    while (1) {
        lru_node_t* head = atomic_load(&queue->head);
        lru_node_t* tail = atomic_load(&queue->tail);
        lru_node_t* next = atomic_load(&head->next);
        
        if (head == atomic_load(&queue->head)) {
            if (head == tail) {
                if (next == NULL) {
                    return NULL;  // Queue empty
                }
                atomic_compare_exchange_weak(&queue->tail, &tail, next);
            } else {
                if (atomic_compare_exchange_weak(&queue->head, &head, next)) {
                    atomic_fetch_sub(&queue->node_count, 1);
                    return head;  // Return old head
                }
            }
        }
    }
}
```

- [ ] **Step 5: Commit**

```bash
git add src/Database/lockfree_lru.c
git commit -m "feat(lockfree-lru): implement LRU queue operations"
```

---

### Task 3: Implement Create/Destroy

**Files:**
- Modify: `src/Database/lockfree_lru.c`

- [ ] **Step 1: Add hashmap callbacks**

```c
// Hash function for path_t*
static size_t hash_path(const void* key) {
    const path_t* path = (const path_t*)key;
    if (path == NULL) return 0;
    
    size_t hash = 0;
    size_t len = path_length((path_t*)path);
    
    for (size_t i = 0; i < len; i++) {
        identifier_t* id = path_get((path_t*)path, i);
        if (id != NULL) {
            for (int j = 0; j < id->chunks.length; j++) {
                chunk_t* chunk = id->chunks.data[j];
                if (chunk != NULL && chunk->data != NULL) {
                    for (size_t k = 0; k < chunk->data->size; k++) {
                        hash = hash * 31 + chunk->data->data[k];
                    }
                }
            }
        }
    }
    
    return hash;
}

// Compare function for path_t*
static int compare_path(const void* a, const void* b) {
    return path_compare((path_t*)a, (path_t*)b);
}

// Duplicate path for hashmap key
static void* dup_path(const void* key) {
    return path_copy((path_t*)key);
}

// Free path key
static void free_path(void* key) {
    path_destroy((path_t*)key);
}
```

- [ ] **Step 2: Calculate memory for entry**

```c
// Calculate approximate memory usage for a cache entry
static size_t calculate_entry_memory(path_t* path, identifier_t* value) {
    size_t total = sizeof(lru_entry_t) + sizeof(lru_node_t);
    
    // Path memory
    if (path != NULL) {
        total += sizeof(path_t);
        total += (size_t)path->identifiers.capacity * sizeof(identifier_t*);
        for (int i = 0; i < path->identifiers.length; i++) {
            identifier_t* id = path->identifiers.data[i];
            if (id != NULL) {
                total += sizeof(identifier_t);
                total += (size_t)id->chunks.capacity * sizeof(chunk_t*);
                for (int j = 0; j < id->chunks.length; j++) {
                    chunk_t* chunk = id->chunks.data[j];
                    if (chunk != NULL && chunk->data != NULL) {
                        total += sizeof(chunk_t);
                        total += chunk->data->size;
                    }
                }
            }
        }
    }
    
    // Value memory
    if (value != NULL) {
        total += sizeof(identifier_t);
        total += (size_t)value->chunks.capacity * sizeof(chunk_t*);
        for (int j = 0; j < value->chunks.length; j++) {
            chunk_t* chunk = value->chunks.data[j];
            if (chunk != NULL && chunk->data != NULL) {
                total += sizeof(chunk_t);
                total += chunk->data->size;
            }
        }
    }
    
    return total;
}
```

- [ ] **Step 3: Implement lockfree_lru_cache_create**

```c
lockfree_lru_cache_t* lockfree_lru_cache_create(size_t max_memory_bytes, uint16_t num_shards) {
    // Auto-scale shard count
    if (num_shards == 0) {
        int cores = platform_core_count();
        num_shards = (uint16_t)(cores * 4);
        if (num_shards < 64) num_shards = 64;
        if (num_shards > 256) num_shards = 256;
    }
    
    lockfree_lru_cache_t* lru = get_clear_memory(sizeof(lockfree_lru_cache_t));
    if (lru == NULL) return NULL;
    
    lru->shards = get_clear_memory(num_shards * sizeof(lockfree_lru_shard_t));
    if (lru->shards == NULL) {
        free(lru);
        return NULL;
    }
    
    lru->num_shards = num_shards;
    
    size_t total_memory = (max_memory_bytes == 0) ?
        DEFAULT_MAX_MEMORY_MB * 1024 * 1024 :
        max_memory_bytes;
    lru->total_max_memory = total_memory;
    
    size_t shard_memory = total_memory / num_shards;
    
    // Initialize each shard
    for (size_t i = 0; i < num_shards; i++) {
        lockfree_lru_shard_t* shard = &lru->shards[i];
        
        // Create concurrent hashmap for this shard
        shard->map = concurrent_hashmap_create(
            1,      // Single stripe per shard (we shard at LRU level)
            16,     // Initial buckets
            0.75f,  // Load factor
            hash_path,
            compare_path,
            dup_path,
            free_path
        );
        
        if (shard->map == NULL) {
            // Cleanup on failure
            for (size_t j = 0; j < i; j++) {
                concurrent_hashmap_destroy(lru->shards[j].map);
                lru_queue_destroy(&lru->shards[j].queue);
            }
            free(lru->shards);
            free(lru);
            return NULL;
        }
        
        // Initialize LRU queue
        lru_queue_init(&shard->queue);
        
        atomic_init(&shard->current_memory, 0);
        atomic_init(&shard->entry_count, 0);
        shard->max_memory = shard_memory;
        atomic_init(&shard->purging, 0);
    }
    
    return lru;
}
```

- [ ] **Step 4: Implement lockfree_lru_cache_destroy**

```c
void lockfree_lru_cache_destroy(lockfree_lru_cache_t* lru) {
    if (lru == NULL) return;
    
    for (size_t i = 0; i < lru->num_shards; i++) {
        lockfree_lru_shard_t* shard = &lru->shards[i];
        
        // Free all entries in hashmap
        // Note: We need to iterate and free manually
        // For now, just destroy the hashmap
        concurrent_hashmap_destroy(shard->map);
        
        // Destroy queue
        lru_queue_destroy(&shard->queue);
    }
    
    free(lru->shards);
    free(lru);
}
```

- [ ] **Step 5: Update CMakeLists.txt**

Add `src/Database/lockfree_lru.c` to source files.

- [ ] **Step 6: Commit**

```bash
git add src/Database/lockfree_lru.c src/Database/CMakeLists.txt
git commit -m "feat(lockfree-lru): implement create/destroy"
```

---

### Task 4: Implement Lock-Free Get Operation

**Files:**
- Modify: `src/Database/lockfree_lru.c`

- [ ] **Step 1: Add get_shard_index helper**

```c
// Get shard index for a path
static size_t get_shard_index(const lockfree_lru_cache_t* lru, const path_t* path) {
    size_t hash = hash_path(path);
    return hash % lru->num_shards;
}
```

- [ ] **Step 2: Implement lockfree_lru_cache_get**

```c
identifier_t* lockfree_lru_cache_get(lockfree_lru_cache_t* lru, path_t* path) {
    if (lru == NULL || path == NULL) {
        return NULL;
    }
    
    // 1. Find shard
    size_t shard_idx = get_shard_index(lru, path);
    lockfree_lru_shard_t* shard = &lru->shards[shard_idx];
    
    // 2. Lookup entry in concurrent hashmap (lock-free)
    lru_entry_t* entry = concurrent_hashmap_get(shard->map, path);
    if (entry == NULL) {
        return NULL;  // Cache miss
    }
    
    // 3. Read current node pointer atomically
    lru_node_t* current_node = atomic_load(&entry->node);
    if (current_node == NULL) {
        return NULL;  // Entry being purged
    }
    
    // 4. Create new node for MRU position
    lru_node_t* new_node = memory_pool_alloc(sizeof(lru_node_t));
    if (new_node == NULL) {
        // Fall back to returning value without promotion
        return (identifier_t*)refcounter_reference((refcounter_t*)entry->value);
    }
    
    atomic_init(&new_node->entry, entry);
    atomic_init(&new_node->next, NULL);
    
    // 5. CAS loop to atomically update entry->node
    lru_node_t* expected = current_node;
    while (expected != NULL) {
        if (atomic_compare_exchange_weak(&entry->node, &expected, new_node)) {
            break;  // Success
        }
        if (expected == NULL) {
            // Entry was purged
            memory_pool_free(new_node, sizeof(lru_node_t));
            return NULL;
        }
    }
    
    // 6. Mark old node as hole (lock-free)
    atomic_store(&current_node->entry, NULL);
    atomic_fetch_add(&shard->queue.hole_count, 1);
    
    // 7. Enqueue new node at tail (MRU)
    lru_enqueue(&shard->queue, new_node);
    
    // 8. Return reference-counted value
    return (identifier_t*)refcounter_reference((refcounter_t*)entry->value);
}
```

- [ ] **Step 3: Commit**

```bash
git add src/Database/lockfree_lru.c
git commit -m "feat(lockfree-lru): implement lock-free get with hole mechanism"
```

---

### Task 5: Implement Put Operation

**Files:**
- Modify: `src/Database/lockfree_lru.c`

- [ ] **Step 1: Implement lockfree_lru_cache_put**

```c
identifier_t* lockfree_lru_cache_put(lockfree_lru_cache_t* lru, path_t* path, identifier_t* value) {
    if (lru == NULL || path == NULL) {
        if (path != NULL) path_destroy(path);
        if (value != NULL) identifier_destroy(value);
        return NULL;
    }
    
    size_t shard_idx = get_shard_index(lru, path);
    lockfree_lru_shard_t* shard = &lru->shards[shard_idx];
    
    // Create entry
    lru_entry_t* entry = memory_pool_alloc(sizeof(lru_entry_t));
    if (entry == NULL) {
        path_destroy(path);
        identifier_destroy(value);
        return NULL;
    }
    
    // Initialize entry
    entry->path = path;
    entry->value = value;
    entry->memory_size = calculate_entry_memory(path, value);
    atomic_init(&entry->node, NULL);
    refcounter_init((refcounter_t*)entry);
    
    // Create LRU node
    lru_node_t* node = memory_pool_alloc(sizeof(lru_node_t));
    if (node == NULL) {
        memory_pool_free(entry, sizeof(lru_entry_t));
        path_destroy(path);
        identifier_destroy(value);
        return NULL;
    }
    
    atomic_init(&node->entry, entry);
    atomic_init(&node->next, NULL);
    atomic_store(&entry->node, node);
    
    // Try to put in hashmap
    lru_entry_t* existing = concurrent_hashmap_put_if_absent(shard->map, path, entry);
    
    if (existing != NULL) {
        // Entry already exists, free our new entry and use existing
        memory_pool_free(node, sizeof(lru_node_t));
        memory_pool_free(entry, sizeof(lru_entry_t));
        path_destroy(path);
        
        // Update existing entry's value
        // TODO: Handle value replacement
        return NULL;
    }
    
    // Successfully inserted, add to LRU queue
    lru_enqueue(&shard->queue, node);
    atomic_fetch_add(&shard->current_memory, entry->memory_size);
    atomic_fetch_add(&shard->entry_count, 1);
    
    return NULL;  // No old value for new entry
}
```

- [ ] **Step 2: Commit**

```bash
git add src/Database/lockfree_lru.c
git commit -m "feat(lockfree-lru): implement put operation"
```

---

### Task 6: Write Unit Tests

**Files:**
- Create: `tests/test_lockfree_lru.cpp`

- [ ] **Step 1: Create test file with fixtures**

```cpp
//
// Unit tests for lock-free LRU cache
//

#include <gtest/gtest.h>
#include <cstring>
#include <cstdio>
extern "C" {
#include "Database/lockfree_lru.h"
#include "HBTrie/path.h"
#include "HBTrie/identifier.h"
#include "Buffer/buffer.h"
}

// Helper to create a simple path
static path_t* make_simple_path(const char* key) {
    path_t* path = path_create();
    buffer_t* buf = buffer_create_from_pointer_copy((uint8_t*)key, strlen(key));
    identifier_t* id = identifier_create(buf, 0);
    buffer_destroy(buf);
    path_append(path, id);
    identifier_destroy(id);
    return path;
}

// Helper to create a simple value
static identifier_t* make_simple_value(const char* data) {
    buffer_t* buf = buffer_create_from_pointer_copy((uint8_t*)data, strlen(data));
    identifier_t* id = identifier_create(buf, 0);
    buffer_destroy(buf);
    return id;
}

class LockfreeLRUTest : public ::testing::Test {
protected:
    lockfree_lru_cache_t* lru;
    
    void SetUp() override {
        lru = lockfree_lru_cache_create(1024 * 1024, 0);  // 1 MB
        ASSERT_NE(lru, nullptr);
    }
    
    void TearDown() override {
        if (lru) {
            lockfree_lru_cache_destroy(lru);
            lru = nullptr;
        }
    }
};
```

- [ ] **Step 2: Write basic put/get test**

```cpp
TEST_F(LockfreeLRUTest, PutGet) {
    path_t* path = make_simple_path("key1");
    identifier_t* value = make_simple_value("value1");
    
    // Put
    identifier_t* old = lockfree_lru_cache_put(lru, path, value);
    EXPECT_EQ(old, nullptr);
    
    // Get
    path_t* path_copy = path_copy(path);
    identifier_t* cached = lockfree_lru_cache_get(lru, path_copy);
    EXPECT_NE(cached, nullptr);
    
    // Cleanup
    identifier_destroy(cached);
}
```

- [ ] **Step 3: Write cache miss test**

```cpp
TEST_F(LockfreeLRUTest, CacheMiss) {
    path_t* path = make_simple_path("nonexistent");
    identifier_t* cached = lockfree_lru_cache_get(lru, path);
    EXPECT_EQ(cached, nullptr);
    path_destroy(path);
}
```

- [ ] **Step 4: Write size test**

```cpp
TEST_F(LockfreeLRUTest, Size) {
    EXPECT_EQ(lockfree_lru_cache_size(lru), 0u);
    
    for (int i = 0; i < 10; i++) {
        char key[32];
        snprintf(key, sizeof(key), "key%d", i);
        path_t* path = make_simple_path(key);
        identifier_t* value = make_simple_value("value");
        lockfree_lru_cache_put(lru, path, value);
    }
    
    EXPECT_EQ(lockfree_lru_cache_size(lru), 10u);
}
```

- [ ] **Step 5: Build and run tests**

```bash
cd build && cmake .. && make test_lockfree_lru
./tests/test_lockfree_lru
```

Expected: Tests compile and pass

- [ ] **Step 6: Commit**

```bash
git add tests/test_lockfree_lru.cpp tests/CMakeLists.txt
git commit -m "test(lockfree-lru): add basic unit tests"
```

---

## Success Criteria for Phase 4

1. All unit tests pass
2. Lock-free get works correctly
3. Put creates entries and adds to queue
4. Memory pool used for allocations
5. No memory leaks (valgrind/ASAN clean)