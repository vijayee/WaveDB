# Concurrent Hashmap Core Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement a concurrent hashmap with lock-free reads and striped write locks.

**Architecture:** Thread-safe hashmap using C11 atomics for lock-free reads and per-stripe mutex locks for writes. Each stripe is an independent hashmap segment, reducing write contention.

**Tech Stack:** C11 atomics, existing hashmap as reference, platform threading primitives

**Spec Reference:** `docs/superpowers/specs/2026-04-05-lockfree-lru-design.md` lines 29-131

---

### Task 1: Create Header File with Data Structures

**Files:**
- Create: `src/Util/concurrent_hashmap.h`

- [ ] **Step 1: Create header guard and includes**

```c
//
// Concurrent Hashmap - Lock-free reads with striped write locks
//

#ifndef WAVEDB_CONCURRENT_HASHMAP_H
#define WAVEDB_CONCURRENT_HASHMAP_H

#include <stdint.h>
#include <stddef.h>
#include <stdatomic.h>
#include "../Util/threadding.h"

#ifdef __cplusplus
extern "C" {
#endif

// Forward declarations
typedef struct chash_entry_t chash_entry_t;
typedef struct chash_stripe_t chash_stripe_t;
typedef struct concurrent_hashmap_t concurrent_hashmap_t;

// Function pointer types
typedef size_t (*chash_hash_fn)(const void* key);
typedef int (*chash_compare_fn)(const void* a, const void* b);
typedef void* (*chash_key_dup_fn)(const void* key);
typedef void (*chash_key_free_fn)(void* key);

#endif // WAVEDB_CONCURRENT_HASHMAP_H
```

- [ ] **Step 2: Define entry structure**

```c
// Hashmap entry - lives in collision chain
struct chash_entry_t {
    void* key;                    // Key (owned by entry)
    _Atomic void* value;          // Value (atomic for lock-free reads)
    chash_entry_t* next;          // Chain for collision resolution
    uint32_t hash;                // Cached hash value
    _Atomic uint8_t tombstone;    // 1 if deleted (for lock-free removal)
};
```

- [ ] **Step 3: Define stripe structure**

```c
// Per-stripe hashmap segment with its own lock
struct chash_stripe_t {
    PLATFORMLOCKTYPE(lock);       // Lock for this stripe only
    chash_entry_t** buckets;      // Bucket array
    size_t bucket_count;          // Number of buckets (power of 2)
    size_t entry_count;           // Entries in this stripe
    size_t tombstone_count;       // Deleted entries awaiting cleanup
};
```

- [ ] **Step 4: Define top-level hashmap structure**

```c
// Concurrent hashmap with striped locks
struct concurrent_hashmap_t {
    chash_stripe_t* stripes;      // Array of stripes
    size_t num_stripes;           // Number of stripes (power of 2)
    size_t stripe_mask;           // Mask for stripe selection (num_stripes - 1)
    
    chash_hash_fn hash_fn;
    chash_compare_fn compare_fn;
    chash_key_dup_fn key_dup_fn;
    chash_key_free_fn key_free_fn;
    
    size_t initial_bucket_count;  // Initial buckets per stripe
    float load_factor;            // Resize threshold
    
    _Atomic size_t total_entries;  // Approximate total entries
};
```

- [ ] **Step 5: Define API functions**

```c
/**
 * Create a concurrent hashmap.
 *
 * @param num_stripes Number of stripes (0 for auto-scale, must be power of 2)
 * @param initial_bucket_count Initial buckets per stripe
 * @param load_factor Resize threshold (0.0-1.0)
 * @param hash_fn Hash function
 * @param compare_fn Key comparison function
 * @param key_dup_fn Key duplication (NULL for pointer storage)
 * @param key_free_fn Key free function (NULL for no free)
 * @return New hashmap or NULL on failure
 */
concurrent_hashmap_t* concurrent_hashmap_create(
    size_t num_stripes,
    size_t initial_bucket_count,
    float load_factor,
    chash_hash_fn hash_fn,
    chash_compare_fn compare_fn,
    chash_key_dup_fn key_dup_fn,
    chash_key_free_fn key_free_fn
);

/**
 * Destroy a concurrent hashmap.
 *
 * @param map Hashmap to destroy
 */
void concurrent_hashmap_destroy(concurrent_hashmap_t* map);

/**
 * Get a value from the hashmap (lock-free).
 *
 * @param map Hashmap to query
 * @param key Key to look up
 * @return Value if found, NULL if not found
 */
void* concurrent_hashmap_get(concurrent_hashmap_t* map, const void* key);

/**
 * Check if a key exists (lock-free).
 *
 * @param map Hashmap to query
 * @param key Key to check
 * @return 1 if exists, 0 if not
 */
uint8_t concurrent_hashmap_contains(concurrent_hashmap_t* map, const void* key);

/**
 * Put a value into the hashmap.
 *
 * @param map Hashmap to update
 * @param key Key (ownership depends on key_dup_fn)
 * @param value Value to store
 * @return Old value if key existed (caller must free), NULL otherwise
 */
void* concurrent_hashmap_put(concurrent_hashmap_t* map, void* key, void* value);

/**
 * Put a value only if key does not exist.
 *
 * @param map Hashmap to update
 * @param key Key (ownership depends on key_dup_fn)
 * @param value Value to store
 * @return Existing value if key exists, NULL if inserted
 */
void* concurrent_hashmap_put_if_absent(concurrent_hashmap_t* map, void* key, void* value);

/**
 * Remove a value from the hashmap.
 *
 * @param map Hashmap to update
 * @param key Key to remove
 * @return Value if found (caller must free), NULL otherwise
 */
void* concurrent_hashmap_remove(concurrent_hashmap_t* map, const void* key);

/**
 * Get approximate entry count.
 *
 * @param map Hashmap to query
 * @return Approximate number of entries
 */
size_t concurrent_hashmap_size(concurrent_hashmap_t* map);
```

- [ ] **Step 6: Add closing extern C block**

```c
#ifdef __cplusplus
}
#endif
```

- [ ] **Step 7: Commit**

```bash
git add src/Util/concurrent_hashmap.h
git commit -m "feat(concurrent-hashmap): add header with data structures and API"
```

---

### Task 2: Implement Core Functions (Create/Destroy)

**Files:**
- Create: `src/Util/concurrent_hashmap.c`
- Modify: `src/Util/CMakeLists.txt` (add new source file)

- [ ] **Step 1: Create implementation file with includes**

```c
//
// Concurrent Hashmap Implementation
//

#include "concurrent_hashmap.h"
#include "allocator.h"
#include <stdlib.h>
#include <string.h>

// Default number of stripes (power of 2)
#define DEFAULT_NUM_STRIPES 64
#define DEFAULT_BUCKET_COUNT 16
#define DEFAULT_LOAD_FACTOR 0.75f
```

- [ ] **Step 2: Implement helper functions**

```c
// Round up to next power of 2
static size_t next_power_of_2(size_t n) {
    n--;
    n |= n >> 1;
    n |= n >> 2;
    n |= n >> 4;
    n |= n >> 8;
    n |= n >> 16;
    n |= n >> 32;
    n++;
    return n;
}

// Get stripe index from hash
static inline size_t get_stripe_index(concurrent_hashmap_t* map, size_t hash) {
    return hash & map->stripe_mask;
}

// Get bucket index from hash
static inline size_t get_bucket_index(size_t hash, size_t bucket_count) {
    return hash % bucket_count;
}
```

- [ ] **Step 3: Implement concurrent_hashmap_create**

```c
concurrent_hashmap_t* concurrent_hashmap_create(
    size_t num_stripes,
    size_t initial_bucket_count,
    float load_factor,
    chash_hash_fn hash_fn,
    chash_compare_fn compare_fn,
    chash_key_dup_fn key_dup_fn,
    chash_key_free_fn key_free_fn
) {
    if (hash_fn == NULL || compare_fn == NULL) {
        return NULL;
    }

    // Auto-scale stripes based on CPU cores
    if (num_stripes == 0) {
        int cores = platform_core_count();
        num_stripes = (size_t)(cores * 4);  // 4x cores
        if (num_stripes < 16) num_stripes = 16;
        if (num_stripes > 256) num_stripes = 256;
    }
    
    // Ensure power of 2
    num_stripes = next_power_of_2(num_stripes);
    
    if (initial_bucket_count == 0) {
        initial_bucket_count = DEFAULT_BUCKET_COUNT;
    }
    initial_bucket_count = next_power_of_2(initial_bucket_count);
    
    if (load_factor <= 0.0f || load_factor > 1.0f) {
        load_factor = DEFAULT_LOAD_FACTOR;
    }

    concurrent_hashmap_t* map = get_clear_memory(sizeof(concurrent_hashmap_t));
    if (map == NULL) return NULL;

    map->stripes = get_clear_memory(num_stripes * sizeof(chash_stripe_t));
    if (map->stripes == NULL) {
        free(map);
        return NULL;
    }

    map->num_stripes = num_stripes;
    map->stripe_mask = num_stripes - 1;
    map->hash_fn = hash_fn;
    map->compare_fn = compare_fn;
    map->key_dup_fn = key_dup_fn;
    map->key_free_fn = key_free_fn;
    map->initial_bucket_count = initial_bucket_count;
    map->load_factor = load_factor;
    atomic_init(&map->total_entries, 0);

    // Initialize each stripe
    for (size_t i = 0; i < num_stripes; i++) {
        chash_stripe_t* stripe = &map->stripes[i];
        
        stripe->buckets = get_clear_memory(initial_bucket_count * sizeof(chash_entry_t*));
        if (stripe->buckets == NULL) {
            // Cleanup on failure
            for (size_t j = 0; j < i; j++) {
                platform_lock_destroy(&map->stripes[j].lock);
                free(map->stripes[j].buckets);
            }
            free(map->stripes);
            free(map);
            return NULL;
        }
        
        stripe->bucket_count = initial_bucket_count;
        stripe->entry_count = 0;
        stripe->tombstone_count = 0;
        platform_lock_init(&stripe->lock);
    }

    return map;
}
```

- [ ] **Step 4: Implement entry cleanup helper**

```c
// Free an entry
static void free_entry(concurrent_hashmap_t* map, chash_entry_t* entry) {
    if (entry == NULL) return;
    
    if (map->key_free_fn && entry->key) {
        map->key_free_fn(entry->key);
    }
    // Note: value is caller's responsibility
    free(entry);
}
```

- [ ] **Step 5: Implement concurrent_hashmap_destroy**

```c
void concurrent_hashmap_destroy(concurrent_hashmap_t* map) {
    if (map == NULL) return;

    // Free all entries in all stripes
    for (size_t i = 0; i < map->num_stripes; i++) {
        chash_stripe_t* stripe = &map->stripes[i];
        
        platform_lock(&stripe->lock);
        
        // Free all entries in buckets
        for (size_t b = 0; b < stripe->bucket_count; b++) {
            chash_entry_t* entry = stripe->buckets[b];
            while (entry != NULL) {
                chash_entry_t* next = entry->next;
                free_entry(map, entry);
                entry = next;
            }
        }
        
        free(stripe->buckets);
        platform_unlock(&stripe->lock);
        platform_lock_destroy(&stripe->lock);
    }

    free(map->stripes);
    free(map);
}
```

- [ ] **Step 6: Update CMakeLists.txt**

Add `src/Util/concurrent_hashmap.c` to the source files list in `src/Util/CMakeLists.txt`.

- [ ] **Step 7: Commit**

```bash
git add src/Util/concurrent_hashmap.c src/Util/CMakeLists.txt
git commit -m "feat(concurrent-hashmap): implement create/destroy functions"
```

---

### Task 3: Implement Lock-Free Read Operations

**Files:**
- Modify: `src/Util/concurrent_hashmap.c`

- [ ] **Step 1: Implement concurrent_hashmap_get (lock-free)**

```c
void* concurrent_hashmap_get(concurrent_hashmap_t* map, const void* key) {
    if (map == NULL || key == NULL) {
        return NULL;
    }

    size_t hash = map->hash_fn(key);
    size_t stripe_idx = get_stripe_index(map, hash);
    chash_stripe_t* stripe = &map->stripes[stripe_idx];
    
    // NO LOCK - lock-free read
    size_t bucket_idx = get_bucket_index(hash, stripe->bucket_count);
    chash_entry_t* entry = atomic_load(&stripe->buckets[bucket_idx]);
    
    // Traverse chain looking for key
    while (entry != NULL) {
        // Skip tombstones
        if (!atomic_load(&entry->tombstone) &&
            entry->hash == hash &&
            map->compare_fn(entry->key, key) == 0) {
            // Found - return value atomically
            return atomic_load(&entry->value);
        }
        entry = entry->next;
    }
    
    return NULL;
}
```

- [ ] **Step 2: Implement concurrent_hashmap_contains (lock-free)**

```c
uint8_t concurrent_hashmap_contains(concurrent_hashmap_t* map, const void* key) {
    if (map == NULL || key == NULL) {
        return 0;
    }

    size_t hash = map->hash_fn(key);
    size_t stripe_idx = get_stripe_index(map, hash);
    chash_stripe_t* stripe = &map->stripes[stripe_idx];
    
    // NO LOCK - lock-free check
    size_t bucket_idx = get_bucket_index(hash, stripe->bucket_count);
    chash_entry_t* entry = atomic_load(&stripe->buckets[bucket_idx]);
    
    while (entry != NULL) {
        if (!atomic_load(&entry->tombstone) &&
            entry->hash == hash &&
            map->compare_fn(entry->key, key) == 0) {
            return 1;
        }
        entry = entry->next;
    }
    
    return 0;
}
```

- [ ] **Step 3: Commit**

```bash
git add src/Util/concurrent_hashmap.c
git commit -m "feat(concurrent-hashmap): implement lock-free get and contains"
```

---

### Task 4: Write Unit Tests for Lock-Free Reads

**Files:**
- Create: `tests/test_concurrent_hashmap.cpp`

- [ ] **Step 1: Create test file with includes and test fixture**

```cpp
//
// Unit tests for concurrent hashmap
//

#include <gtest/gtest.h>
#include <cstring>
#include <thread>
#include <vector>
#include <atomic>
extern "C" {
#include "Util/concurrent_hashmap.h"
}

// Simple string key helpers
static size_t hash_string(const void* key) {
    const char* str = (const char*)key;
    size_t hash = 5381;
    int c;
    while ((c = *str++)) {
        hash = ((hash << 5) + hash) + c;
    }
    return hash;
}

static int compare_string(const void* a, const void* b) {
    return strcmp((const char*)a, (const char*)b);
}

static void* dup_string(const void* key) {
    const char* str = (const char*)key;
    return strdup(str);
}

static void free_string(void* key) {
    free(key);
}

class ConcurrentHashmapTest : public ::testing::Test {
protected:
    concurrent_hashmap_t* map;
    
    void SetUp() override {
        map = concurrent_hashmap_create(
            16,     // 16 stripes
            8,      // 8 initial buckets per stripe
            0.75f,  // load factor
            hash_string,
            compare_string,
            dup_string,
            free_string
        );
        ASSERT_NE(map, nullptr);
    }
    
    void TearDown() override {
        if (map) {
            concurrent_hashmap_destroy(map);
            map = nullptr;
        }
    }
};
```

- [ ] **Step 2: Write basic put/get test**

```cpp
TEST_F(ConcurrentHashmapTest, PutGet) {
    char* key = strdup("test_key");
    char* value = strdup("test_value");
    
    // Put
    void* old = concurrent_hashmap_put(map, key, value);
    EXPECT_EQ(old, nullptr);
    
    // Get
    void* result = concurrent_hashmap_get(map, "test_key");
    EXPECT_EQ(result, value);
    
    // Cleanup (map owns the key copy)
    free(key);
}
```

- [ ] **Step 3: Write contains test**

```cpp
TEST_F(ConcurrentHashmapTest, Contains) {
    char* key = strdup("test_key");
    char* value = strdup("test_value");
    
    // Not contains initially
    EXPECT_EQ(concurrent_hashmap_contains(map, "test_key"), 0);
    
    // Put
    concurrent_hashmap_put(map, key, value);
    
    // Contains after put
    EXPECT_EQ(concurrent_hashmap_contains(map, "test_key"), 1);
    
    free(key);
}
```

- [ ] **Step 4: Write concurrent read test**

```cpp
TEST_F(ConcurrentHashmapTest, ConcurrentReads) {
    // Pre-populate with some entries
    for (int i = 0; i < 100; i++) {
        char key[32];
        snprintf(key, sizeof(key), "key%d", i);
        char* value = strdup(key);
        concurrent_hashmap_put(map, strdup(key), value);
    }
    
    // Concurrent reads from multiple threads
    std::atomic<int> success_count(0);
    std::vector<std::thread> threads;
    
    for (int t = 0; t < 8; t++) {
        threads.emplace_back([&, t]() {
            for (int i = 0; i < 100; i++) {
                char key[32];
                snprintf(key, sizeof(key), "key%d", i);
                void* value = concurrent_hashmap_get(map, key);
                if (value != nullptr) {
                    success_count++;
                }
            }
        });
    }
    
    for (auto& thread : threads) {
        thread.join();
    }
    
    // All reads should succeed (800 total: 8 threads * 100 keys)
    EXPECT_EQ(success_count.load(), 800);
}
```

- [ ] **Step 5: Write size test**

```cpp
TEST_F(ConcurrentHashmapTest, Size) {
    EXPECT_EQ(concurrent_hashmap_size(map), 0u);
    
    for (int i = 0; i < 10; i++) {
        char key[32];
        snprintf(key, sizeof(key), "key%d", i);
        concurrent_hashmap_put(map, strdup(key), strdup("value"));
    }
    
    EXPECT_EQ(concurrent_hashmap_size(map), 10u);
}
```

- [ ] **Step 6: Add main()**

```cpp
int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
```

- [ ] **Step 7: Update tests CMakeLists.txt**

Add `test_concurrent_hashmap.cpp` to the test files in `tests/CMakeLists.txt`.

- [ ] **Step 8: Build and run tests**

```bash
cd build && cmake .. && make test_concurrent_hashmap
./tests/test_concurrent_hashmap
```

Expected: All tests pass

- [ ] **Step 9: Commit**

```bash
git add tests/test_concurrent_hashmap.cpp tests/CMakeLists.txt
git commit -m "test(concurrent-hashmap): add unit tests for lock-free reads"
```

---

### Task 5: Implement Write Operations with Striped Locks

**Files:**
- Modify: `src/Util/concurrent_hashmap.c`

- [ ] **Step 1: Implement find_or_create_entry helper**

```c
// Find existing entry or create new one (must hold stripe lock)
static chash_entry_t* find_or_create_entry(
    concurrent_hashmap_t* map,
    chash_stripe_t* stripe,
    const void* key,
    size_t hash,
    int create_new,
    chash_entry_t*** prev_ptr
) {
    size_t bucket_idx = get_bucket_index(hash, stripe->bucket_count);
    chash_entry_t** prev = &stripe->buckets[bucket_idx];
    chash_entry_t* entry = *prev;
    
    // Look for existing entry
    while (entry != NULL) {
        if (!atomic_load(&entry->tombstone) &&
            entry->hash == hash &&
            map->compare_fn(entry->key, key) == 0) {
            if (prev_ptr) *prev_ptr = prev;
            return entry;
        }
        prev = &entry->next;
        entry = *prev;
    }
    
    if (!create_new) {
        if (prev_ptr) *prev_ptr = prev;
        return NULL;
    }
    
    // Create new entry
    chash_entry_t* new_entry = get_clear_memory(sizeof(chash_entry_t));
    if (new_entry == NULL) {
        if (prev_ptr) *prev_ptr = NULL;
        return NULL;
    }
    
    // Initialize entry
    if (map->key_dup_fn) {
        new_entry->key = map->key_dup_fn(key);
    } else {
        new_entry->key = (void*)key;
    }
    atomic_init(&new_entry->value, NULL);
    new_entry->next = NULL;
    new_entry->hash = (uint32_t)hash;
    atomic_init(&new_entry->tombstone, 0);
    
    // Link into bucket
    *prev = new_entry;
    if (prev_ptr) *prev_ptr = prev;
    
    stripe->entry_count++;
    atomic_fetch_add(&map->total_entries, 1);
    
    return new_entry;
}
```

- [ ] **Step 2: Implement concurrent_hashmap_put**

```c
void* concurrent_hashmap_put(concurrent_hashmap_t* map, void* key, void* value) {
    if (map == NULL || key == NULL) {
        return NULL;
    }

    size_t hash = map->hash_fn(key);
    size_t stripe_idx = get_stripe_index(map, hash);
    chash_stripe_t* stripe = &map->stripes[stripe_idx];
    
    platform_lock(&stripe->lock);
    
    chash_entry_t** prev_ptr = NULL;
    chash_entry_t* entry = find_or_create_entry(map, stripe, key, hash, 1, &prev_ptr);
    
    if (entry == NULL) {
        platform_unlock(&stripe->lock);
        // Key duplication failed
        if (map->key_free_fn && value) {
            // Free value since we're not storing it
        }
        return NULL;
    }
    
    // Swap value atomically
    void* old_value = atomic_exchange(&entry->value, value);
    
    // If key was duplicated and we already had it, free the duplicate
    if (map->key_dup_fn && map->key_free_fn) {
        // The key passed in was duplicated; if entry already existed,
        // we need to free our duplicate since entry->key is already set
        // Actually, find_or_create_entry handles this - if entry exists,
        // it doesn't duplicate the key again
        if (prev_ptr && *prev_ptr == entry->next) {
            // New entry was created, but we need to check
            // The key is already stored in entry->key
        }
    }
    
    // Free the input key if we're not managing keys
    if (!map->key_dup_fn && map->key_free_fn) {
        map->key_free_fn(key);
    }
    
    platform_unlock(&stripe->lock);
    return old_value;
}
```

- [ ] **Step 3: Implement concurrent_hashmap_put_if_absent**

```c
void* concurrent_hashmap_put_if_absent(concurrent_hashmap_t* map, void* key, void* value) {
    if (map == NULL || key == NULL) {
        return NULL;
    }

    size_t hash = map->hash_fn(key);
    size_t stripe_idx = get_stripe_index(map, hash);
    chash_stripe_t* stripe = &map->stripes[stripe_idx];
    
    platform_lock(&stripe->lock);
    
    chash_entry_t* entry = find_or_create_entry(map, stripe, key, hash, 0, NULL);
    
    if (entry != NULL) {
        // Key exists, return existing value
        void* existing = atomic_load(&entry->value);
        platform_unlock(&stripe->lock);
        
        // Free the key if we're managing keys
        if (map->key_free_fn && !map->key_dup_fn) {
            map->key_free_fn(key);
        }
        
        return existing;
    }
    
    // Key doesn't exist, create it
    entry = find_or_create_entry(map, stripe, key, hash, 1, NULL);
    if (entry == NULL) {
        platform_unlock(&stripe->lock);
        return NULL;
    }
    
    // Set value atomically
    void* old_value = atomic_exchange(&entry->value, value);
    (void)old_value;  // Should be NULL for new entry
    
    platform_unlock(&stripe->lock);
    return NULL;  // Inserted, no previous value
}
```

- [ ] **Step 4: Implement concurrent_hashmap_remove**

```c
void* concurrent_hashmap_remove(concurrent_hashmap_t* map, const void* key) {
    if (map == NULL || key == NULL) {
        return NULL;
    }

    size_t hash = map->hash_fn(key);
    size_t stripe_idx = get_stripe_index(map, hash);
    chash_stripe_t* stripe = &map->stripes[stripe_idx];
    
    platform_lock(&stripe->lock);
    
    size_t bucket_idx = get_bucket_index(hash, stripe->bucket_count);
    chash_entry_t** prev = &stripe->buckets[bucket_idx];
    chash_entry_t* entry = *prev;
    
    // Find entry
    while (entry != NULL) {
        if (!atomic_load(&entry->tombstone) &&
            entry->hash == hash &&
            map->compare_fn(entry->key, key) == 0) {
            break;
        }
        prev = &entry->next;
        entry = *prev;
    }
    
    if (entry == NULL) {
        platform_unlock(&stripe->lock);
        return NULL;
    }
    
    // Mark as tombstone (lock-free reads will skip)
    atomic_store(&entry->tombstone, 1);
    
    // Get value
    void* value = atomic_load(&entry->value);
    
    // Update counts
    stripe->entry_count--;
    stripe->tombstone_count++;
    atomic_fetch_sub(&map->total_entries, 1);
    
    platform_unlock(&stripe->lock);
    return value;
}
```

- [ ] **Step 5: Implement concurrent_hashmap_size**

```c
size_t concurrent_hashmap_size(concurrent_hashmap_t* map) {
    if (map == NULL) return 0;
    return atomic_load(&map->total_entries);
}
```

- [ ] **Step 6: Commit**

```bash
git add src/Util/concurrent_hashmap.c
git commit -m "feat(concurrent-hashmap): implement write operations with striped locks"
```

---

### Task 6: Add Write Operation Tests

**Files:**
- Modify: `tests/test_concurrent_hashmap.cpp`

- [ ] **Step 1: Add replace test**

```cpp
TEST_F(ConcurrentHashmapTest, Replace) {
    char* key = strdup("test_key");
    char* value1 = strdup("value1");
    char* value2 = strdup("value2");
    
    // Put first value
    void* old1 = concurrent_hashmap_put(map, key, value1);
    EXPECT_EQ(old1, nullptr);
    
    // Put second value (replace)
    key = strdup("test_key");  // Duplicate since map owns first key
    void* old2 = concurrent_hashmap_put(map, key, value2);
    EXPECT_EQ(old2, value1);
    
    // Get should return new value
    void* result = concurrent_hashmap_get(map, "test_key");
    EXPECT_EQ(result, value2);
    
    free(old2);  // Clean up old value
}
```

- [ ] **Step 2: Add put_if_absent test**

```cpp
TEST_F(ConcurrentHashmapTest, PutIfAbsent) {
    char* key1 = strdup("test_key");
    char* value1 = strdup("value1");
    
    // First put should succeed
    void* result1 = concurrent_hashmap_put_if_absent(map, key1, value1);
    EXPECT_EQ(result1, nullptr);
    
    // Second put should return existing
    char* key2 = strdup("test_key");
    char* value2 = strdup("value2");
    void* result2 = concurrent_hashmap_put_if_absent(map, key2, value2);
    EXPECT_EQ(result2, value1);
    
    // Clean up key2 since it wasn't stored
    free(key2);
    free(value2);
    
    // Get should return first value
    void* value = concurrent_hashmap_get(map, "test_key");
    EXPECT_EQ(value, value1);
}
```

- [ ] **Step 3: Add remove test**

```cpp
TEST_F(ConcurrentHashmapTest, Remove) {
    char* key = strdup("test_key");
    char* value = strdup("value");
    
    // Put
    concurrent_hashmap_put(map, key, value);
    EXPECT_EQ(concurrent_hashmap_contains(map, "test_key"), 1);
    
    // Remove
    void* removed = concurrent_hashmap_remove(map, "test_key");
    EXPECT_EQ(removed, value);
    EXPECT_EQ(concurrent_hashmap_contains(map, "test_key"), 0);
    EXPECT_EQ(concurrent_hashmap_size(map), 0u);
    
    free(removed);
}
```

- [ ] **Step 4: Add concurrent write test**

```cpp
TEST_F(ConcurrentHashmapTest, ConcurrentWrites) {
    std::vector<std::thread> threads;
    std::atomic<int> success_count(0);
    
    // Each thread writes to different keys
    for (int t = 0; t < 8; t++) {
        threads.emplace_back([&, t]() {
            for (int i = 0; i < 100; i++) {
                char key[32];
                snprintf(key, sizeof(key), "thread%d_key%d", t, i);
                char* value = strdup(key);
                void* old = concurrent_hashmap_put(map, strdup(key), value);
                if (old == nullptr) {
                    success_count++;
                }
            }
        });
    }
    
    for (auto& thread : threads) {
        thread.join();
    }
    
    // All puts should succeed (no duplicates)
    EXPECT_EQ(success_count.load(), 800);
    EXPECT_EQ(concurrent_hashmap_size(map), 800u);
}
```

- [ ] **Step 5: Add concurrent read-write mix test**

```cpp
TEST_F(ConcurrentHashmapTest, ConcurrentReadWrite) {
    // Pre-populate
    for (int i = 0; i < 100; i++) {
        char key[32];
        snprintf(key, sizeof(key), "key%d", i);
        concurrent_hashmap_put(map, strdup(key), strdup("init"));
    }
    
    std::atomic<int> read_count(0);
    std::atomic<int> write_count(0);
    std::vector<std::thread> threads;
    
    // Reader threads
    for (int t = 0; t < 4; t++) {
        threads.emplace_back([&]() {
            for (int i = 0; i < 100; i++) {
                char key[32];
                snprintf(key, sizeof(key), "key%d", i);
                if (concurrent_hashmap_get(map, key) != nullptr) {
                    read_count++;
                }
            }
        });
    }
    
    // Writer threads
    for (int t = 0; t < 4; t++) {
        threads.emplace_back([&, t]() {
            for (int i = 0; i < 100; i++) {
                char key[32];
                snprintf(key, sizeof(key), "key%d", i);
                char value[32];
                snprintf(value, sizeof(value), "thread%d", t);
                concurrent_hashmap_put(map, strdup(key), strdup(value));
                write_count++;
            }
        });
    }
    
    for (auto& thread : threads) {
        thread.join();
    }
    
    // All operations should complete without crashes
    EXPECT_EQ(read_count.load(), 400);
    EXPECT_EQ(write_count.load(), 400);
}
```

- [ ] **Step 6: Build and run tests**

```bash
cd build && make test_concurrent_hashmap
./tests/test_concurrent_hashmap
```

Expected: All tests pass

- [ ] **Step 7: Commit**

```bash
git add tests/test_concurrent_hashmap.cpp
git commit -m "test(concurrent-hashmap): add tests for write operations"
```

---

## Success Criteria for Phase 1

1. All unit tests pass
2. Lock-free reads work correctly under concurrent access
3. Striped write locks allow concurrent writes to different stripes
4. No memory leaks (valgrind/ASAN clean)
5. Thread sanitization passes (TSAN clean)