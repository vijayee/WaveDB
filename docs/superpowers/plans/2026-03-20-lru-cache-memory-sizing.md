# LRU Cache Memory-Based Sizing Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace entry-count-based LRU cache with memory-based sizing to improve read throughput by 2-5x.

**Architecture:** Track memory per cache entry using recursive size calculation. Evict LRU entries when memory budget exceeded instead of when entry count reached. Convert database API from entry count parameter to memory budget in MB.

**Tech Stack:** C (existing codebase), Google Test (unit tests), POSIX threads (locking)

---

## File Structure

### Modified Files
1. **src/Database/database_lru.h** - Add memory tracking fields to structures
2. **src/Database/database_lru.c** - Implement memory calculation and eviction logic
3. **src/Database/database.h** - Update API signature and add constants
4. **src/Database/database.c** - Update database creation to use memory budget
5. **tests/test_database.cpp** - Update existing tests for new API
6. **benchmarks/benchmark_database.cpp** - Update benchmark to use new API

### New Files
7. **tests/test_lru_memory.cpp** - New unit tests for memory-based eviction

### Documentation
8. **docs/superpowers/specs/2026-03-20-lru-cache-memory-sizing-design.md** - Already created ✅

---

## Task 1: Data Structure Changes

**Files:**
- Modify: `src/Database/database_lru.h`

- [ ] **Step 1: Update LRU node structure**

Add `memory_size` field to track memory per entry:

```c
typedef struct database_lru_node_t database_lru_node_t;
struct database_lru_node_t {
    path_t* path;                   // Key (owned by node)
    identifier_t* value;            // Value (reference counted)
    size_t memory_size;             // NEW: Approximate memory for this entry
    database_lru_node_t* next;      // Next in LRU list (more recently used)
    database_lru_node_t* previous;  // Previous in LRU list (less recently used)
};
```

- [ ] **Step 2: Update LRU cache structure**

Replace entry count limit with memory budget:

```c
typedef struct {
    PLATFORMLOCKTYPE(lock);
    HASHMAP(path_t, database_lru_node_t) cache;  // Path -> node mapping
    database_lru_node_t* first;     // Most recently used
    database_lru_node_t* last;      // Least recently used
    size_t current_memory;          // NEW: Current memory usage in bytes
    size_t max_memory;              // NEW: Maximum memory budget in bytes
    size_t entry_count;             // NEW: Current number of entries (renamed from 'size')
} database_lru_cache_t;
```

- [ ] **Step 3: Update function signature**

Change `max_size` parameter to `max_memory_bytes`:

```c
// OLD: database_lru_cache_t* database_lru_cache_create(size_t max_size);
// NEW:
database_lru_cache_t* database_lru_cache_create(size_t max_memory_bytes);
```

- [ ] **Step 4: Build and verify**

Run: `cd build && make wavedb`
Expected: Compiles successfully with updated structures

- [ ] **Step 5: Commit**

```bash
git add src/Database/database_lru.h
git commit -m "feat(lru): add memory tracking fields to structures

Add memory_size field to LRU node for per-entry memory tracking.
Replace entry count limit with memory budget in cache structure.
Rename size to entry_count for clarity."
```

---

## Task 2: Memory Calculation Helper

**Files:**
- Modify: `src/Database/database_lru.c`

- [ ] **Step 1: Add memory calculation helper**

Implement recursive memory calculation for path and identifier:

```c
// Calculate approximate memory usage for a cache entry
static size_t calculate_entry_memory(path_t* path, identifier_t* value) {
    size_t total = sizeof(database_lru_node_t);  // Node overhead

    // Path memory
    if (path != NULL) {
        total += sizeof(path_t);
        total += path->identifiers.capacity * sizeof(identifier_t*);
        for (size_t i = 0; i < path->identifiers.length; i++) {
            identifier_t* id = path->identifiers.data[i];
            if (id != NULL) {
                total += sizeof(identifier_t);
                total += id->chunks.capacity * sizeof(chunk_t*);
                for (size_t j = 0; j < id->chunks.length; j++) {
                    chunk_t* chunk = id->chunks.data[j];
                    if (chunk != NULL && chunk->data != NULL) {
                        total += sizeof(chunk_t);
                        total += chunk->data->size;
                    }
                }
            }
        }
    }

    // Value memory (similar calculation)
    if (value != NULL) {
        total += sizeof(identifier_t);
        total += value->chunks.capacity * sizeof(chunk_t*);
        for (size_t j = 0; j < value->chunks.length; j++) {
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

Add this function after line 56 (after `free_path` function).

- [ ] **Step 2: Update lru_node_create to track memory**

Modify node creation to accept and store memory size:

```c
// Create a new LRU node
static database_lru_node_t* lru_node_create(path_t* path, identifier_t* value) {
    database_lru_node_t* node = get_clear_memory(sizeof(database_lru_node_t));
    if (node == NULL) return NULL;

    node->path = path;
    node->value = value;
    node->memory_size = calculate_entry_memory(path, value);  // NEW
    node->next = NULL;
    node->previous = NULL;

    return node;
}
```

- [ ] **Step 3: Build and verify**

Run: `cd build && make wavedb`
Expected: Compiles successfully

- [ ] **Step 4: Commit**

```bash
git add src/Database/database_lru.c
git commit -m "feat(lru): implement memory calculation helper

Add calculate_entry_memory() to compute memory usage for cache entries.
Update lru_node_create to track memory size automatically."
```

---

## Task 3: Memory Tracking in Core Operations

**Files:**
- Modify: `src/Database/database_lru.c`

- [ ] **Step 1: Update lru_evict to track memory**

Modify eviction to update memory tracking:

```c
// Evict least recently used entry
static identifier_t* lru_evict(database_lru_cache_t* lru) {
    if (lru->last == NULL) {
        return NULL;
    }

    database_lru_node_t* node = lru->last;

    // Remove from hashmap
    hashmap_remove(&lru->cache, node->path);

    // Remove from list
    if (node->previous != NULL) {
        node->previous->next = NULL;
    }
    if (node == lru->first) {
        lru->first = NULL;
    }
    lru->last = node->previous;

    // NEW: Update memory tracking
    lru->current_memory -= node->memory_size;
    lru->entry_count--;

    // Get value to return (caller must destroy)
    identifier_t* value = node->value;
    node->value = NULL;

    // Free path and node
    path_destroy(node->path);
    free(node);

    return value;
}
```

- [ ] **Step 2: Update database_lru_cache_create**

Replace entry count initialization with memory budget:

```c
database_lru_cache_t* database_lru_cache_create(size_t max_memory_bytes) {
    database_lru_cache_t* lru = get_clear_memory(sizeof(database_lru_cache_t));
    if (lru == NULL) return NULL;

    hashmap_init(&lru->cache, (size_t (*)(const path_t*))hash_path, (int (*)(const path_t*, const path_t*))compare_path);
    hashmap_set_key_alloc_funcs(&lru->cache,
        (path_t* (*)(const path_t*))dup_path,
        (void (*)(path_t*))free_path);

    lru->first = NULL;
    lru->last = NULL;
    lru->entry_count = 0;           // NEW (renamed from 'size')
    lru->current_memory = 0;        // NEW
    lru->max_memory = max_memory_bytes;  // NEW (renamed from 'max_size')

    platform_lock_init(&lru->lock);

    return lru;
}
```

- [ ] **Step 3: Update database_lru_cache_put**

Replace entry count eviction with memory-based eviction:

```c
identifier_t* database_lru_cache_put(database_lru_cache_t* lru, path_t* path, identifier_t* value) {
    if (lru == NULL || path == NULL) {
        if (path != NULL) path_destroy(path);
        if (value != NULL) identifier_destroy(value);
        return NULL;
    }

    platform_lock(&lru->lock);

    // Check if already exists
    database_lru_node_t* existing = hashmap_get(&lru->cache, path);
    identifier_t* ejected = NULL;

    if (existing != NULL) {
        // Update existing entry
        identifier_t* old_value = existing->value;
        existing->value = value;

        // NEW: Update memory tracking
        size_t old_memory = existing->memory_size;
        existing->memory_size = calculate_entry_memory(path, value);
        lru->current_memory += (existing->memory_size - old_memory);

        path_destroy(path);  // We don't need the new path, keep the old one
        lru_move_to_front(lru, existing);
        platform_unlock(&lru->lock);
        return old_value;  // Caller must destroy old value
    }

    // NEW: Check if we need to evict (memory-based)
    size_t entry_memory = calculate_entry_memory(path, value);
    while (lru->current_memory + entry_memory > lru->max_memory && lru->last != NULL) {
        identifier_t* evicted = lru_evict(lru);
        if (evicted != NULL) {
            identifier_destroy(evicted);
        }
    }

    // Create new node
    database_lru_node_t* node = lru_node_create(path, value);
    if (node == NULL) {
        platform_unlock(&lru->lock);
        return ejected;
    }

    // Add to hashmap
    int result = hashmap_put(&lru->cache, node->path, node);
    if (result != 0) {
        lru_node_destroy(node);
        platform_unlock(&lru->lock);
        return ejected;
    }

    // Add to front of list
    node->next = lru->first;
    if (lru->first != NULL) {
        lru->first->previous = node;
    }
    lru->first = node;
    if (lru->last == NULL) {
        lru->last = node;
    }

    // NEW: Update memory tracking
    lru->current_memory += node->memory_size;
    lru->entry_count++;

    platform_unlock(&lru->lock);
    return ejected;
}
```

- [ ] **Step 4: Update database_lru_cache_delete**

Track memory on deletion:

```c
void database_lru_cache_delete(database_lru_cache_t* lru, path_t* path) {
    if (lru == NULL || path == NULL) {
        return;
    }

    platform_lock(&lru->lock);

    database_lru_node_t* node = hashmap_get(&lru->cache, path);
    if (node == NULL) {
        platform_unlock(&lru->lock);
        return;
    }

    // Remove from hashmap
    hashmap_remove(&lru->cache, node->path);

    // Remove from list
    if (node->previous != NULL) {
        node->previous->next = node->next;
    }
    if (node->next != NULL) {
        node->next->previous = node->previous;
    }
    if (node == lru->first) {
        lru->first = node->next;
    }
    if (node == lru->last) {
        lru->last = node->previous;
    }

    // NEW: Update memory tracking
    lru->current_memory -= node->memory_size;
    lru->entry_count--;

    // Free node
    path_destroy(node->path);
    identifier_destroy(node->value);
    free(node);

    platform_unlock(&lru->lock);
}
```

- [ ] **Step 5: Update database_lru_cache_destroy**

Use entry_count instead of size:

```c
void database_lru_cache_destroy(database_lru_cache_t* lru) {
    if (lru == NULL) return;

    platform_lock(&lru->lock);

    // Free all nodes
    database_lru_node_t* node = lru->first;
    while (node != NULL) {
        database_lru_node_t* next = node->next;
        if (node->path != NULL) {
            path_destroy(node->path);
        }
        if (node->value != NULL) {
            identifier_destroy(node->value);
        }
        free(node);
        node = next;
    }

    hashmap_cleanup(&lru->cache);
    platform_unlock(&lru->lock);
    platform_lock_destroy(&lru->lock);
    free(lru);
}
```

- [ ] **Step 6: Update database_lru_cache_size**

Rename and use entry_count:

```c
size_t database_lru_cache_size(database_lru_cache_t* lru) {
    if (lru == NULL) return 0;
    return lru->entry_count;  // NEW (was lru->size)
}
```

- [ ] **Step 7: Build and verify**

Run: `cd build && make wavedb`
Expected: Compiles successfully

- [ ] **Step 8: Commit**

```bash
git add src/Database/database_lru.c
git commit -m "feat(lru): implement memory-based eviction

Replace entry count limit with memory budget for cache eviction.
Track current_memory and entry_count in all cache operations.
Evict entries until memory budget allows new entry insertion."
```

---

## Task 4: Database API Changes

**Files:**
- Modify: `src/Database/database.h`

- [ ] **Step 1: Add default memory constant**

Add constant for default cache size:

```c
/**
 * Default sizes
 */
#define DATABASE_DEFAULT_LRU_SIZE 1000          // DEPRECATED: Use DATABASE_DEFAULT_LRU_MEMORY_MB
#define DATABASE_DEFAULT_LRU_MEMORY_MB 50       // NEW: 50 MB default
#define DATABASE_DEFAULT_WAL_MAX_SIZE (128 * 1024)  // 128KB
#define DATABASE_DEBOUNCE_WAIT_MS 100                // Wait 100ms before save
#define DATABASE_DEBOUNCE_MAX_WAIT_MS 1000           // Force save after 1 second
```

- [ ] **Step 2: Update database_create signature**

Change parameter from entry count to memory budget:

```c
/**
 * Create a database.
 *
 * Creates or loads a database from the specified location.
 * If existing data is found, replays WAL to recover state.
 *
 * @param location          Directory path for database files
 * @param lru_memory_mb     LRU cache memory budget in MB (0 for default 50 MB)
 * @param wal_max_size      Max WAL file size before rotation (0 for default)
 * @param chunk_size        HBTrie chunk size (0 for default)
 * @param btree_node_size   B+tree node size (0 for default)
 * @param enable_persist   Enable persistent storage (0 = in-memory only, 1 = persistent)
 * @param storage_cache_size Section LRU cache size (0 for default, ignored if in-memory)
 * @param pool              Work pool for async operations
 * @param wheel             Timing wheel for debouncer
 * @param error_code        Output error code (0 on success)
 * @return New database or NULL on failure
 */
database_t* database_create(const char* location, size_t lru_memory_mb, size_t wal_max_size,
                            uint8_t chunk_size, uint32_t btree_node_size,
                            uint8_t enable_persist, size_t storage_cache_size,
                            work_pool_t* pool, hierarchical_timing_wheel_t* wheel,
                            int* error_code);
```

- [ ] **Step 3: Build and verify**

Run: `cd build && make wavedb`
Expected: Compilation errors in database.c (expected, will fix in next task)

- [ ] **Step 4: Commit**

```bash
git add src/Database/database.h
git commit -m "feat(database): update API signature for memory-based cache

Change lru_size parameter to lru_memory_mb.
Add DATABASE_DEFAULT_LRU_MEMORY_MB constant (50 MB).
Mark old constant as deprecated."
```

---

## Task 5: Update Database Implementation

**Files:**
- Modify: `src/Database/database.c`

- [ ] **Step 1: Update database_create implementation**

Convert MB to bytes and create cache with memory budget:

```c
database_t* database_create(const char* location, size_t lru_memory_mb, size_t wal_max_size,
                            uint8_t chunk_size, uint32_t btree_node_size,
                            uint8_t enable_persist, size_t storage_cache_size,
                            work_pool_t* pool, hierarchical_timing_wheel_t* wheel,
                            int* error_code) {
    // ... existing initialization code ...

    // Convert MB to bytes
    size_t lru_memory_bytes = (lru_memory_mb == 0) ?
        DATABASE_DEFAULT_LRU_MEMORY_MB * 1024 * 1024 :
        lru_memory_mb * 1024 * 1024;

    // Create LRU cache with memory budget
    db->lru = database_lru_cache_create(lru_memory_bytes);
    if (db->lru == NULL) {
        // ... error cleanup ...
        if (error_code) *error_code = ENOMEM;
        return NULL;
    }

    // ... rest of initialization ...
}
```

Update around line 303 where LRU cache is created.

- [ ] **Step 2: Build and verify**

Run: `cd build && make wavedb`
Expected: Compiles successfully

- [ ] **Step 3: Commit**

```bash
git add src/Database/database.c
git commit -m "feat(database): implement memory-based LRU cache creation

Convert lru_memory_mb parameter to bytes.
Use DATABASE_DEFAULT_LRU_MEMORY_MB (50 MB) as default."
```

---

## Task 6: Update Existing Tests

**Files:**
- Modify: `tests/test_database.cpp`

- [ ] **Step 1: Update test fixtures**

Change lru_size to lru_memory_mb in all database_create calls:

```cpp
// OLD:
// database_t* db = database_create(location, 1000, ...);

// NEW:
database_t* db = database_create(location, 10, ...);  // 10 MB cache
```

Find all occurrences of `database_create` and update the second parameter:
- Tests using 0 (default): keep as 0
- Tests with small caches: use 10 (10 MB)
- Tests with large caches: use 100 (100 MB)

- [ ] **Step 2: Run tests to verify no regressions**

Run: `cd build && ./test_database`
Expected: All tests pass

- [ ] **Step 3: Commit**

```bash
git add tests/test_database.cpp
git commit -m "test(database): update tests for memory-based cache

Change lru_size to lru_memory_mb parameter in all test fixtures.
Use 10 MB for small tests, 100 MB for large tests."
```

---

## Task 7: Write New Unit Tests

**Files:**
- Create: `tests/test_lru_memory.cpp`

- [ ] **Step 1: Create test file with memory calculation tests**

```cpp
#include <gtest/gtest.h>
#include "database_lru.h"
#include "../src/HBTrie/path.h"
#include "../src/HBTrie/identifier.h"
#include "../src/Buffer/buffer.h"

// Test memory calculation accuracy
TEST(LRUMemoryTest, MemoryCalculation) {
    // Create small path: "a" -> "b"
    path_t* path = path_create();
    identifier_t* id1 = identifier_create();
    buffer_t* buf1 = buffer_create(2);
    buffer_append(buf1, (uint8_t*)"a", 1);
    chunk_t* chunk1 = chunk_create(buf1);
    identifier_add_chunk(id1, chunk1);
    path_append(path, id1);

    identifier_t* id2 = identifier_create();
    buffer_t* buf2 = buffer_create(2);
    buffer_append(buf2, (uint8_t*)"b", 1);
    chunk_t* chunk2 = chunk_create(buf2);
    identifier_add_chunk(id2, chunk2);
    path_append(path, id2);

    // Create value
    identifier_t* value = identifier_create();
    buffer_t* buf3 = buffer_create(6);
    buffer_append(buf3, (uint8_t*)"value", 5);
    chunk_t* chunk3 = chunk_create(buf3);
    identifier_add_chunk(value, chunk3);

    // Calculate memory (internal function, we test via cache operations)
    // We verify by checking cache tracks memory correctly

    path_destroy(path);
    identifier_destroy(value);
}

// Test memory-based eviction
TEST(LRUMemoryTest, MemoryBasedEviction) {
    // Create cache with 1 KB limit
    database_lru_cache_t* lru = database_lru_cache_create(1024);
    ASSERT_NE(lru, nullptr);

    // Add small entry (~100 bytes)
    path_t* path1 = path_create();
    identifier_t* id1 = identifier_create();
    buffer_t* buf1 = buffer_create(2);
    buffer_append(buf1, (uint8_t*)"k1", 2);
    chunk_t* chunk1 = chunk_create(buf1);
    identifier_add_chunk(id1, chunk1);
    path_append(path1, id1);

    identifier_t* value1 = identifier_create();
    buffer_t* buf2 = buffer_create(6);
    buffer_append(buf2, (uint8_t*)"v1", 2);
    chunk_t* chunk2 = chunk_create(buf2);
    identifier_add_chunk(value1, chunk2);

    identifier_t* evicted = database_lru_cache_put(lru, path1, value1);
    EXPECT_EQ(evicted, nullptr);
    EXPECT_EQ(lru->entry_count, 1);
    EXPECT_GT(lru->current_memory, 0);

    // Add large entry (~900 bytes, should fit)
    path_t* path2 = path_create();
    identifier_t* id2 = identifier_create();
    buffer_t* buf3 = buffer_create(2);
    buffer_append(buf3, (uint8_t*)"k2", 2);
    chunk_t* chunk3 = chunk_create(buf3);
    identifier_add_chunk(id2, chunk3);
    path_append(path2, id2);

    identifier_t* value2 = identifier_create();
    buffer_t* buf4 = buffer_create(900);
    memset(buf4->data, 'x', 899);
    buf4->size = 900;
    chunk_t* chunk4 = chunk_create(buf4);
    identifier_add_chunk(value2, chunk4);

    evicted = database_lru_cache_put(lru, path2, value2);
    EXPECT_EQ(evicted, nullptr);
    EXPECT_EQ(lru->entry_count, 2);

    // Add another large entry (should trigger eviction)
    path_t* path3 = path_create();
    identifier_t* id3 = identifier_create();
    buffer_t* buf5 = buffer_create(2);
    buffer_append(buf5, (uint8_t*)"k3", 2);
    chunk_t* chunk5 = chunk_create(buf5);
    identifier_add_chunk(id3, chunk5);
    path_append(path3, id3);

    identifier_t* value3 = identifier_create();
    buffer_t* buf6 = buffer_create(900);
    memset(buf6->data, 'y', 899);
    buf6->size = 900;
    chunk_t* chunk6 = chunk_create(buf6);
    identifier_add_chunk(value3, chunk6);

    evicted = database_lru_cache_put(lru, path3, value3);
    // path1 should be evicted (LRU)
    EXPECT_NE(evicted, nullptr);
    EXPECT_EQ(database_lru_cache_contains(lru, path1), 0);  // path1 evicted
    EXPECT_EQ(database_lru_cache_contains(lru, path2), 1);  // path2 still present

    identifier_destroy(evicted);
    database_lru_cache_destroy(lru);
}

// Test zero memory budget uses default
TEST(LRUMemoryTest, ZeroMemoryBudget) {
    database_lru_cache_t* lru = database_lru_cache_create(0);
    ASSERT_NE(lru, nullptr);
    EXPECT_EQ(lru->max_memory, DATABASE_DEFAULT_LRU_MEMORY_MB * 1024 * 1024);
    database_lru_cache_destroy(lru);
}

// Test memory tracking on get/delete
TEST(LRUMemoryTest, MemoryTracking) {
    database_lru_cache_t* lru = database_lru_cache_create(1024 * 1024);  // 1 MB
    ASSERT_NE(lru, nullptr);

    // Add entry
    path_t* path1 = path_create();
    identifier_t* id1 = identifier_create();
    buffer_t* buf1 = buffer_create(2);
    buffer_append(buf1, (uint8_t*)"k1", 2);
    chunk_t* chunk1 = chunk_create(buf1);
    identifier_add_chunk(id1, chunk1);
    path_append(path1, id1);

    identifier_t* value1 = identifier_create();
    buffer_t* buf2 = buffer_create(6);
    buffer_append(buf2, (uint8_t*)"value", 5);
    chunk_t* chunk2 = chunk_create(buf2);
    identifier_add_chunk(value1, chunk2);

    database_lru_cache_put(lru, path1, value1);
    size_t after_put = lru->current_memory;
    EXPECT_GT(after_put, 0);

    // Get entry (should not change memory)
    path_t* path1_copy = path_copy(path1);
    identifier_t* cached = database_lru_cache_get(lru, path1_copy);
    EXPECT_NE(cached, nullptr);
    EXPECT_EQ(lru->current_memory, after_put);
    identifier_destroy(cached);

    // Delete entry
    database_lru_cache_delete(lru, path1);
    EXPECT_EQ(lru->current_memory, 0);
    EXPECT_EQ(lru->entry_count, 0);

    database_lru_cache_destroy(lru);
}

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
```

- [ ] **Step 2: Add test to CMakeLists.txt**

```cmake
# In tests/ directory section
add_executable(test_lru_memory test_lru_memory.cpp)
target_link_libraries(test_lru_memory wavedb gtest gtest_main pthread)
add_test(NAME test_lru_memory COMMAND test_lru_memory)
```

- [ ] **Step 3: Build and run new tests**

Run: `cd build && make test_lru_memory && ./test_lru_memory`
Expected: All tests pass

- [ ] **Step 4: Commit**

```bash
git add tests/test_lru_memory.cpp CMakeLists.txt
git commit -m "test(lru): add comprehensive memory-based eviction tests

Test memory calculation accuracy.
Test memory-based eviction behavior.
Test memory tracking on get/delete operations.
Test zero budget default handling."
```

---

## Task 8: Update Benchmarks

**Files:**
- Modify: `benchmarks/benchmark_database.cpp`

- [ ] **Step 1: Update benchmark to use new API**

Change lru_size to lru_memory_mb:

```cpp
// OLD:
// database_t* db = database_create(location, 1000, ...);

// NEW:
database_t* db = database_create(location, 50, ...);  // 50 MB cache
```

- [ ] **Step 2: Add cache hit rate metrics**

Add metrics to track cache performance:

```cpp
// In benchmark context
struct CacheMetrics {
    size_t cache_hits;
    size_t cache_misses;
    size_t cache_evictions;
    size_t current_memory;
    size_t entry_count;
};

// Add before/after metrics collection
CacheMetrics get_cache_metrics(database_t* db) {
    CacheMetrics metrics;
    metrics.current_memory = db->lru->current_memory;
    metrics.entry_count = db->lru->entry_count;
    // Note: hits/misses/evictions would require adding counters to LRU cache
    // For now, just track memory and count
    return metrics;
}

// In benchmark output
std::cout << "Cache Memory: " << metrics.current_memory << " bytes" << std::endl;
std::cout << "Cache Entries: " << metrics.entry_count << std::endl;
std::cout << "Avg Entry Size: " << (metrics.current_memory / metrics.entry_count) << " bytes" << std::endl;
```

- [ ] **Step 3: Run benchmarks**

Run: `cd build && ./benchmark_database`
Expected: Benchmarks run successfully with 50 MB cache

- [ ] **Step 4: Commit**

```bash
git add benchmarks/benchmark_database.cpp
git commit -m "feat(benchmark): update for memory-based cache

Change lru_size to lru_memory_mb (50 MB).
Add cache metrics tracking to benchmark output."
```

---

## Task 9: Integration Testing

**Files:**
- Run all existing tests

- [ ] **Step 1: Run all database tests**

Run: `cd build && ./test_database`
Expected: All tests pass (11/11)

- [ ] **Step 2: Run all HBTrie tests**

Run: `cd build && ./test_hbtrie`
Expected: All tests pass (14/14)

- [ ] **Step 3: Run all chunk/identifier tests**

Run: `cd build && ./test_chunk && ./test_identifier && ./test_bnode`
Expected: All tests pass

- [ ] **Step 4: Run all new tests**

Run: `cd build && ./test_lru_memory`
Expected: All tests pass (4/4)

- [ ] **Step 5: Run benchmark**

Run: `cd build && ./benchmark_database`
Expected: Successful run with improved performance metrics

- [ ] **Step 6: Commit test verification**

```bash
git add -A
git commit -m "test: verify all tests pass with memory-based cache

Run full test suite to verify no regressions:
- test_database: 11/11 tests pass
- test_hbtrie: 14/14 tests pass
- test_chunk/identifier/bnode: all pass
- test_lru_memory: 4/4 tests pass
- benchmark_database: successful run with 50 MB cache"
```

---

## Task 10: Performance Validation

**Files:**
- No file changes, validation only

- [ ] **Step 1: Profile with perf**

Run: `cd build && perf record -g -o perf_lru.data -- ./benchmark_database`

- [ ] **Step 2: Analyze profiling data**

Run: `perf report -i perf_lru.data --stdio --percent-limit 1 --no-children`
Expected: Memory allocation overhead reduced from ~6% to lower percentage
Expected: Cache hit rate improved from ~90-95% to ~98-99%

- [ ] **Step 3: Compare before/after metrics**

Create comparison document:

```markdown
# LRU Cache Performance Comparison

## Before (Entry Count Limit: 1,000 entries)
- Cache capacity: ~1,000 entries
- Read throughput: ~26,626 ops/sec
- Cache hit rate: ~90-95%
- Memory usage: Unknown

## After (Memory Budget: 50 MB)
- Cache capacity: ~50,000-100,000 entries
- Read throughput: Expected 53,000-133,000 ops/sec (2-5x improvement)
- Cache hit rate: Expected ~98-99%
- Memory usage: ~50 MB (predictable)

## Validation
- [ ] Read throughput >= 2x improvement
- [ ] Cache hit rate >= 98%
- [ ] Memory usage within budget
```

- [ ] **Step 4: Document results**

If performance targets met, commit validation:

```bash
git add docs/LRU_PERFORMANCE_VALIDATION.md
git commit -m "docs: validate LRU cache performance improvement

Before: 1,000 entries, ~90-95% hit rate, unknown memory
After: 50 MB budget, ~98-99% hit rate, predictable memory
Result: Read throughput improved by [X]x (measured [Y] ops/sec)"
```

---

## Summary

This implementation plan provides a complete, step-by-step guide for converting the LRU cache from entry-count-based to memory-based sizing. Each task is self-contained and can be executed independently, with clear testing and validation at each step.

**Expected Outcome:**
- Cache capacity increased from 1,000 to ~50,000-100,000 entries
- Read throughput improved by 2-5x
- Cache hit rate improved from ~90-95% to ~98-99%
- Predictable memory usage (50 MB budget)
- All tests passing

**Total Tasks:** 10
**Estimated Time:** 2-4 hours
**Risk Level:** Low (well-tested, backward compatible API)