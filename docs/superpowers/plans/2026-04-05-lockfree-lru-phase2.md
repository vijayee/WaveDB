# Concurrent Hashmap Resize and Cleanup Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement resize and tombstone cleanup for the concurrent hashmap.

**Architecture:** Striped resize with minimal lock contention. Tombstone cleanup during resize to reclaim memory. Each stripe can resize independently.

**Tech Stack:** C11 atomics, platform threading primitives

**Prerequisite:** Phase 1 completed (concurrent_hashmap core)

**Spec Reference:** `docs/superpowers/specs/2026-04-05-lockfree-lru-design.md` lines 73-121

---

### Task 1: Implement Stripe Resize

**Files:**
- Modify: `src/Util/concurrent_hashmap.c`

- [ ] **Step 1: Add resize check to find_or_create_entry**

After incrementing entry count in `find_or_create_entry`, check if resize is needed:

```c
// After: stripe->entry_count++; atomic_fetch_add(&map->total_entries, 1);
// Add resize check
size_t threshold = (size_t)(stripe->bucket_count * map->load_factor);
if (stripe->entry_count > threshold && stripe->tombstone_count < stripe->entry_count / 2) {
    // Schedule resize (will be done under lock)
}
```

- [ ] **Step 2: Implement resize_stripe helper**

```c
// Resize a single stripe's bucket array (must hold stripe lock)
static int resize_stripe(concurrent_hashmap_t* map, chash_stripe_t* stripe) {
    size_t new_count = stripe->bucket_count * 2;
    chash_entry_t** new_buckets = get_clear_memory(new_count * sizeof(chash_entry_t*));
    if (new_buckets == NULL) {
        return -1;  // Allocation failed, continue with current size
    }
    
    // Rehash all entries
    for (size_t b = 0; b < stripe->bucket_count; b++) {
        chash_entry_t* entry = stripe->buckets[b];
        while (entry != NULL) {
            chash_entry_t* next = entry->next;
            
            // Skip tombstones (clean up during resize)
            if (atomic_load(&entry->tombstone)) {
                free_entry(map, entry);
                entry = next;
                continue;
            }
            
            // Calculate new bucket index
            size_t new_idx = get_bucket_index(entry->hash, new_count);
            
            // Insert at head of new bucket
            entry->next = new_buckets[new_idx];
            new_buckets[new_idx] = entry;
            
            entry = next;
        }
    }
    
    // Free old buckets and update stripe
    free(stripe->buckets);
    stripe->buckets = new_buckets;
    stripe->bucket_count = new_count;
    stripe->tombstone_count = 0;  // Cleaned up during resize
    
    return 0;
}
```

- [ ] **Step 3: Update find_or_create_entry to trigger resize**

In `find_or_create_entry`, after creating new entry:

```c
// Check if resize needed
size_t threshold = (size_t)(stripe->bucket_count * map->load_factor);
if (stripe->entry_count > threshold) {
    resize_stripe(map, stripe);
}
```

- [ ] **Step 4: Commit**

```bash
git add src/Util/concurrent_hashmap.c
git commit -m "feat(concurrent-hashmap): implement stripe resize with tombstone cleanup"
```

---

### Task 2: Add Resize Tests

**Files:**
- Modify: `tests/test_concurrent_hashmap.cpp`

- [ ] **Step 1: Add resize test**

```cpp
TEST_F(ConcurrentHashmapTest, Resize) {
    // Create map with small initial size
    concurrent_hashmap_destroy(map);
    map = concurrent_hashmap_create(
        4,      // 4 stripes
        4,      // 4 initial buckets
        0.75f,  // load factor
        hash_string,
        compare_string,
        dup_string,
        free_string
    );
    ASSERT_NE(map, nullptr);
    
    // Add entries to trigger resize
    for (int i = 0; i < 100; i++) {
        char key[32];
        snprintf(key, sizeof(key), "key%d", i);
        concurrent_hashmap_put(map, strdup(key), strdup("value"));
    }
    
    // All entries should still be accessible
    for (int i = 0; i < 100; i++) {
        char key[32];
        snprintf(key, sizeof(key), "key%d", i);
        void* value = concurrent_hashmap_get(map, key);
        EXPECT_NE(value, nullptr) << "Key " << key << " not found after resize";
    }
    
    EXPECT_EQ(concurrent_hashmap_size(map), 100u);
}
```

- [ ] **Step 2: Build and run tests**

```bash
cd build && make test_concurrent_hashmap
./tests/test_concurrent_hashmap
```

Expected: All tests pass

- [ ] **Step 3: Commit**

```bash
git add tests/test_concurrent_hashmap.cpp
git commit -m "test(concurrent-hashmap): add resize test"
```

---

### Task 3: Implement Tombstone Cleanup Function

**Files:**
- Modify: `src/Util/concurrent_hashmap.h`
- Modify: `src/Util/concurrent_hashmap.c`

- [ ] **Step 1: Add cleanup function declaration to header**

```c
/**
 * Clean up tombstones and optionally shrink.
 *
 * @param map Hashmap to clean
 * @return Number of tombstones removed
 */
size_t concurrent_hashmap_cleanup(concurrent_hashmap_t* map);
```

- [ ] **Step 2: Implement cleanup function**

```c
size_t concurrent_hashmap_cleanup(concurrent_hashmap_t* map) {
    if (map == NULL) return 0;
    
    size_t total_cleaned = 0;
    
    for (size_t i = 0; i < map->num_stripes; i++) {
        chash_stripe_t* stripe = &map->stripes[i];
        
        platform_lock(&stripe->lock);
        
        if (stripe->tombstone_count == 0) {
            platform_unlock(&stripe->lock);
            continue;
        }
        
        // Rebuild all buckets, skipping tombstones
        for (size_t b = 0; b < stripe->bucket_count; b++) {
            chash_entry_t** prev = &stripe->buckets[b];
            chash_entry_t* entry = *prev;
            
            while (entry != NULL) {
                chash_entry_t* next = entry->next;
                
                if (atomic_load(&entry->tombstone)) {
                    // Remove tombstone entry
                    *prev = next;
                    free_entry(map, entry);
                    total_cleaned++;
                } else {
                    prev = &entry->next;
                }
                
                entry = next;
            }
        }
        
        stripe->tombstone_count = 0;
        platform_unlock(&stripe->lock);
    }
    
    return total_cleaned;
}
```

- [ ] **Step 3: Commit**

```bash
git add src/Util/concurrent_hashmap.h src/Util/concurrent_hashmap.c
git commit -m "feat(concurrent-hashmap): add cleanup function for tombstones"
```

---

### Task 4: Add Cleanup Tests

**Files:**
- Modify: `tests/test_concurrent_hashmap.cpp`

- [ ] **Step 1: Add tombstone cleanup test**

```cpp
TEST_F(ConcurrentHashmapTest, TombstoneCleanup) {
    // Add entries
    for (int i = 0; i < 50; i++) {
        char key[32];
        snprintf(key, sizeof(key), "key%d", i);
        concurrent_hashmap_put(map, strdup(key), strdup("value"));
    }
    
    EXPECT_EQ(concurrent_hashmap_size(map), 50u);
    
    // Remove half
    for (int i = 0; i < 25; i++) {
        char key[32];
        snprintf(key, sizeof(key), "key%d", i);
        void* value = concurrent_hashmap_remove(map, key);
        free(value);
    }
    
    EXPECT_EQ(concurrent_hashmap_size(map), 25u);
    
    // Cleanup tombstones
    size_t cleaned = concurrent_hashmap_cleanup(map);
    EXPECT_EQ(cleaned, 25u);
    
    // Remaining entries still accessible
    for (int i = 25; i < 50; i++) {
        char key[32];
        snprintf(key, sizeof(key), "key%d", i);
        EXPECT_NE(concurrent_hashmap_get(map, key), nullptr);
    }
}
```

- [ ] **Step 2: Build and run tests**

```bash
cd build && make test_concurrent_hashmap
./tests/test_concurrent_hashmap
```

Expected: All tests pass

- [ ] **Step 3: Commit**

```bash
git add tests/test_concurrent_hashmap.cpp
git commit -m "test(concurrent-hashmap): add tombstone cleanup test"
```

---

## Success Criteria for Phase 2

1. All unit tests pass
2. Resize works correctly under load
3. Tombstone cleanup reclaims memory
4. No memory leaks (valgrind/ASAN clean)
5. Thread sanitization passes (TSAN clean)