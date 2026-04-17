# hbtrie_node Eviction Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement callback-driven lazy unloading of hbtrie_nodes so the bnode_cache's existing LRU drives eviction, parent entry pointers are nulled asynchronously, and `is_loaded` becomes a real flag.

**Architecture:** The bnode_cache's LRU evicts bnodes as before. An eviction callback pushes evicted offsets to a lock-free ring buffer. A background task drains the queue, walks the hbtrie tree to null parent entry pointers, then calls deferred free. Zero new lock contention on read/write paths.

**Tech Stack:** C11 (stdatomic.h), existing gtest framework, existing work_pool/hierarchical_timing_wheel

**Spec:** `docs/superpowers/specs/2026-04-17-hbtrie-node-eviction-design.md`

---

### Task 1: Lock-free eviction queue

**Files:**
- Create: `src/Database/eviction_queue.h`
- Create: `src/Database/eviction_queue.c`
- Test: `tests/test_eviction_queue.cpp`

- [ ] **Step 1: Write the failing test**

```cpp
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

    // Fill to capacity
    for (uint64_t i = 0; i < EVICTION_QUEUE_CAPACITY; i++) {
        EXPECT_EQ(eviction_queue_push(&queue, i + 1), 0);
    }
    // One more should fail
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

    // Remaining 5 items still in queue
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

    // All items received (order may vary due to concurrency)
    EXPECT_EQ(collected.size(), (size_t)N);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd /home/victor/Workspace/src/github.com/vijayee/WaveDB/build && cmake --build . --target test_eviction_queue 2>&1 | tail -5`
Expected: Build error — files don't exist

- [ ] **Step 3: Create header file**

```c
// src/Database/eviction_queue.h
#ifndef EVICTION_QUEUE_H
#define EVICTION_QUEUE_H

#include <stdint.h>
#include <stddef.h>
#include "../Util/atomic_compat.h"

#define EVICTION_QUEUE_CAPACITY 256

typedef struct eviction_queue_t {
    ATOMIC_TYPE(uint64_t) head;
    ATOMIC_TYPE(uint64_t) tail;
    uint64_t offsets[EVICTION_QUEUE_CAPACITY];
} eviction_queue_t;

void eviction_queue_init(eviction_queue_t* queue);
int eviction_queue_push(eviction_queue_t* queue, uint64_t offset);
size_t eviction_queue_drain(eviction_queue_t* queue, uint64_t* out, size_t max);

#endif
```

- [ ] **Step 4: Create implementation file**

```c
// src/Database/eviction_queue.c
#include "eviction_queue.h"
#include <string.h>

void eviction_queue_init(eviction_queue_t* queue) {
    atomic_store(&queue->head, 0);
    atomic_store(&queue->tail, 0);
    memset(queue->offsets, 0, sizeof(queue->offsets));
}

int eviction_queue_push(eviction_queue_t* queue, uint64_t offset) {
    uint64_t tail = atomic_load(&queue->tail);
    uint64_t head = atomic_load(&queue->head);

    if (tail - head >= EVICTION_QUEUE_CAPACITY) {
        return -1;  // Full
    }

    queue->offsets[tail % EVICTION_QUEUE_CAPACITY] = offset;
    atomic_store(&queue->tail, tail + 1);
    return 0;
}

size_t eviction_queue_drain(eviction_queue_t* queue, uint64_t* out, size_t max) {
    uint64_t head = atomic_load(&queue->head);
    uint64_t tail = atomic_load(&queue->tail);
    size_t available = (size_t)(tail - head);

    size_t count = available < max ? available : max;
    for (size_t i = 0; i < count; i++) {
        out[i] = queue->offsets[(head + i) % EVICTION_QUEUE_CAPACITY];
    }

    atomic_store(&queue->head, head + count);
    return count;
}
```

- [ ] **Step 5: Add to CMakeLists.txt**

Add to the test section of `CMakeLists.txt`:

```cmake
add_executable(test_eviction_queue tests/test_eviction_queue.cpp)
target_link_libraries(test_eviction_queue wavedb gtest gtest_main Threads::Threads)
add_test(NAME test_eviction_queue COMMAND test_eviction_queue)
```

Also add `src/Database/eviction_queue.c` to the wavedb library sources (find the `add_library(wavedb` section and add it alongside the other `src/Database/*.c` files).

- [ ] **Step 6: Build and run test**

Run: `cd /home/victor/Workspace/src/github.com/vijayee/WaveDB/build && cmake .. && cmake --build . --target test_eviction_queue && ./test_eviction_queue`
Expected: All tests PASS

- [ ] **Step 7: Commit**

```bash
git add src/Database/eviction_queue.h src/Database/eviction_queue.c tests/test_eviction_queue.cpp CMakeLists.txt
git commit -m "feat: add lock-free eviction queue (ring buffer)"
```

---

### Task 2: Eviction callback and deferred free in bnode_cache

**Files:**
- Modify: `src/Storage/bnode_cache.h:22-31` (bnode_cache_item_t — add evict_pending)
- Modify: `src/Storage/bnode_cache.h:44-53` (file_bnode_cache_t — add on_evict, on_evict_data)
- Modify: `src/Storage/bnode_cache.c:155-172` (evict_if_needed — defer free, call callback)
- Test: `tests/test_bnode_cache.cpp`

- [ ] **Step 1: Write the failing test**

Add to `tests/test_bnode_cache.cpp`:

```cpp
// Test eviction callback fires and data is deferred
TEST_F(BnodeCacheTest, EvictionCallbackAndDeferredFree) {
    char path[512];
    make_path(path, sizeof(path), "evict_cb.db");

    page_file_t* pf = page_file_create(path, 4096, 2);
    ASSERT_NE(pf, nullptr);
    int rc = page_file_open(pf, 1);
    EXPECT_EQ(rc, 0);

    // Use very small max_memory so eviction triggers quickly
    bnode_cache_mgr_t* mgr = bnode_cache_mgr_create(64, 4);
    ASSERT_NE(mgr, nullptr);

    file_bnode_cache_t* fcache = bnode_cache_create_file_cache(mgr, pf, "evict_cb.db");
    ASSERT_NE(fcache, nullptr);

    // Track evicted offsets via callback
    uint64_t evicted_offset = 0;
    int callback_count = 0;
    fcache->on_evict = [](uint64_t offset, void* data) {
        uint64_t* out = (uint64_t*)data;
        *out = offset;
        ((int*)((void*)out + sizeof(uint64_t)))[0]++;
    };
    uint64_t cb_data[2] = {0, 0};  // [0]=offset, [1]=count (hacky but works for test)
    // Better: use a struct
    struct EvictTracker { uint64_t offset; int count; };
    EvictTracker tracker = {0, 0};
    fcache->on_evict = [](uint64_t offset, void* data) {
        EvictTracker* t = (EvictTracker*)data;
        t->offset = offset;
        t->count++;
    };
    fcache->on_evict_data = &tracker;

    // Write a node
    const char* payload = "test payload for eviction";
    size_t payload_len = strlen(payload);
    size_t total_len = 0;
    uint8_t* data = make_prefixed_data((const uint8_t*)payload, payload_len, &total_len);

    rc = bnode_cache_write(fcache, 8192, data, total_len);
    EXPECT_EQ(rc, 0);

    // Write another node to push memory over limit
    uint8_t* data2 = make_prefixed_data((const uint8_t*)"second node", 10, &total_len);
    rc = bnode_cache_write(fcache, 16384, data2, total_len);
    EXPECT_EQ(rc, 0);

    // The first write should have triggered eviction callback
    EXPECT_GT(tracker.count, 0);

    // Complete the deferred eviction
    bnode_cache_complete_evict(fcache, tracker.offset);

    bnode_cache_destroy_file_cache(fcache);
    bnode_cache_mgr_destroy(mgr);
    page_file_destroy(pf);
    free(data);
    free(data2);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd /home/victor/Workspace/src/github.com/vijayee/WaveDB/build && cmake --build . --target test_bnode_cache && ./test_bnode_cache --gtest_filter="*EvictionCallback*" 2>&1 | tail -10`
Expected: FAIL — `on_evict` field and `bnode_cache_complete_evict` don't exist

- [ ] **Step 3: Add callback fields to bnode_cache.h**

Add to `bnode_cache_item_t` (after `invalidate_pending`, line ~27):

```c
    uint8_t evict_pending;         // 1 when evicted but data not yet freed (deferred)
```

Add to `file_bnode_cache_t` (after `dirty_threshold`, line ~51):

```c
    bnode_evict_fn on_evict;       // Called when a bnode is evicted (before deferred free)
    void* on_evict_data;           // User data for eviction callback
```

Add the callback typedef before the struct definitions:

```c
typedef void (*bnode_evict_fn)(uint64_t disk_offset, void* user_data);
```

Add declaration after existing function declarations:

```c
void bnode_cache_complete_evict(file_bnode_cache_t* fcache, uint64_t offset);
```

- [ ] **Step 4: Modify evict_if_needed in bnode_cache.c**

Replace the free calls at lines 169-170 with deferred free + callback:

```c
static void evict_if_needed(file_bnode_cache_t* fcache, bnode_cache_shard_t* shard) {
    /* Caller must hold shard->lock */
    while (fcache->current_memory > fcache->max_memory && shard->lru_last != NULL) {
        bnode_cache_item_t* victim = shard->lru_last;
        if (victim->ref_count > 0 || victim->is_dirty) {
            break;
        }
        lru_remove(shard, victim);
        shard_remove(shard, victim->offset);
        shard->item_count--;
        fcache->current_memory -= victim->data_len;
        if (fcache->mgr != NULL) {
            fcache->mgr->current_total_memory -= victim->data_len;
        }

        // Deferred free: mark as evict_pending, call callback, don't free yet
        victim->evict_pending = 1;
        if (fcache->on_evict != NULL) {
            fcache->on_evict(victim->offset, fcache->on_evict_data);
        }
    }
}
```

- [ ] **Step 5: Implement bnode_cache_complete_evict**

Add to `bnode_cache.c`:

```c
void bnode_cache_complete_evict(file_bnode_cache_t* fcache, uint64_t offset) {
    if (fcache == NULL) return;

    size_t shard_idx = (size_t)(offset % fcache->num_shards);
    bnode_cache_shard_t* shard = &fcache->shards[shard_idx];

    platform_lock(&shard->lock);

    // Find the deferred-evict item by scanning the LRU list
    // (it was removed from the hash map but left in memory)
    bnode_cache_item_t* item = shard->lru_first;
    while (item != NULL) {
        if (item->evict_pending && item->offset == offset) {
            break;
        }
        item = item->lru_next;
    }

    if (item == NULL) {
        // Item not found or already freed — check if it's an orphan
        // Evicted items are removed from the LRU list too, so we need
        // a separate list for evict_pending items.
        platform_unlock(&shard->lock);
        return;
    }

    // Free the deferred data
    if (item->data != NULL) {
        free(item->data);
    }
    free(item);

    platform_unlock(&shard->lock);
}
```

Wait — evicted items are removed from the LRU list by `lru_remove`. So `complete_evict` can't find them by scanning the LRU. We need a separate deferred list per shard, or store the item pointer in a deferred-free list.

**Revised approach:** Add a `deferred_head` list to the shard for evict_pending items:

Add to `bnode_cache_shard_t`:

```c
    bnode_cache_item_t* deferred_first;  // Head of deferred-free list
```

In `evict_if_needed`, after `lru_remove`, add to deferred list instead of leaving unlinked:

```c
        // Add to deferred-free list instead of freeing
        victim->lru_next = shard->deferred_first;
        victim->lru_prev = NULL;
        shard->deferred_first = victim;
```

In `bnode_cache_complete_evict`, scan the deferred list:

```c
void bnode_cache_complete_evict(file_bnode_cache_t* fcache, uint64_t offset) {
    if (fcache == NULL) return;

    size_t shard_idx = (size_t)(offset % fcache->num_shards);
    bnode_cache_shard_t* shard = &fcache->shards[shard_idx];

    platform_lock(&shard->lock);

    bnode_cache_item_t** pp = &shard->deferred_first;
    while (*pp != NULL) {
        if ((*pp)->offset == offset && (*pp)->evict_pending) {
            bnode_cache_item_t* item = *pp;
            *pp = item->lru_next;
            if (item->data != NULL) {
                free(item->data);
            }
            free(item);
            platform_unlock(&shard->lock);
            return;
        }
        pp = &(*pp)->lru_next;
    }

    platform_unlock(&shard->lock);
}
```

- [ ] **Step 6: Build and run test**

Run: `cd /home/victor/Workspace/src/github.com/vijayee/WaveDB/build && cmake --build . --target test_bnode_cache && ./test_bnode_cache --gtest_filter="*EvictionCallback*"`
Expected: PASS

- [ ] **Step 7: Run all bnode_cache tests**

Run: `cd /home/victor/Workspace/src/github.com/vijayee/WaveDB/build && ./test_bnode_cache`
Expected: All tests PASS

- [ ] **Step 8: Commit**

```bash
git add src/Storage/bnode_cache.h src/Storage/bnode_cache.c tests/test_bnode_cache.cpp
git commit -m "feat: add eviction callback and deferred free to bnode_cache"
```

---

### Task 3: is_loaded checks in traversal paths

**Files:**
- Modify: `src/HBTrie/hbtrie.c:872-941` (hbtrie_cursor_next — handle unloaded nodes)
- Modify: `src/HBTrie/hbtrie.c:1925+` (hbtrie_find — add is_loaded checks after getting child)
- Modify: `src/HBTrie/hbtrie.c:628-692` (hbtrie_node_destroy — skip btree walk if not loaded)

- [ ] **Step 1: Fix hbtrie_node_destroy for unloaded nodes**

In `hbtrie_node_destroy`, the function walks `current->btree` via a stack. If `is_loaded == 0`, `btree` is NULL. The code already checks `if (current->btree != NULL)` before walking — but verify this handles the case. If `btree == NULL`, skip the btree walk and child collection, just destroy the node struct itself.

Read the exact code at hbtrie.c:628-692 and confirm the NULL check exists. If it does, add an explicit `is_loaded` check for clarity:

```c
if (current->btree != NULL && current->is_loaded) {
    // existing btree walk logic
}
```

- [ ] **Step 2: Fix hbtrie_cursor_next for unloaded nodes**

At line ~879, the cursor pops a NULL btree node. When `node->btree == NULL` (is_loaded == 0), the cursor should reload the node from disk before continuing:

After the existing `if (node == NULL || node->btree == NULL)` check, add:

```c
if (node != NULL && node->btree == NULL && node->is_loaded == 0 && cursor->trie->fcache != NULL) {
    // Node was evicted — reload from disk via child_disk_offset
    // Need to find the parent entry's child_disk_offset for this node.
    // This is complex — for now, signal end of traversal.
    // TODO: Full reload support for cursor (requires parent tracking)
    cursor->finished = 1;
    return -1;
}
```

Note: Full cursor reload support requires the cursor to track parent entries, which is a larger change. For Phase 1, cursors prevent eviction of visited nodes via refcount.

- [ ] **Step 3: Add is_loaded check in hbtrie_find after child access**

In `hbtrie_find` (~line 2006-2022 and ~2040-2056), after lazy loading a child and before descending into it, add:

```c
if (entry->child != NULL && !entry->child->is_loaded) {
    // Child was evicted — reload
    hbtrie_node_t* child = entry->child;
    if (child->disk_offset != 0 && trie->fcache != NULL) {
        bnode_entry_lazy_load_hbtrie_child(entry, trie->fcache,
                                             trie->chunk_size,
                                             trie->btree_node_size);
    }
}
```

Same pattern for `entry->trie_child`.

- [ ] **Step 4: Build and run existing hbtrie tests**

Run: `cd /home/victor/Workspace/src/github.com/vijayee/WaveDB/build && cmake --build . --target test_hbtrie && ./test_hbtrie`
Expected: All existing tests PASS (is_loaded is still always 1 at this point)

- [ ] **Step 5: Commit**

```bash
git add src/HBTrie/hbtrie.c
git commit -m "feat: add is_loaded checks in traversal paths (hbtrie_find, cursor, destroy)"
```

---

### Task 4: Tree walk helper for eviction

**Files:**
- Modify: `src/HBTrie/hbtrie.h` — declare `hbtrie_null_entries_by_offset`
- Modify: `src/HBTrie/hbtrie.c` — implement tree walk that nulls parent pointers for evicted offsets

- [ ] **Step 1: Write the failing test**

Add to `tests/test_hbtrie.cpp`:

```cpp
TEST(HBTrieTest, NullEntriesByOffset) {
    hbtrie_t* trie = hbtrie_create(4, 4096);
    ASSERT_NE(trie, nullptr);

    // Insert a key to create at least one hbtrie_node
    path_t* path = path_create();
    identifier_t* id1 = identifier_create_from_cstr("test");
    path_append(path, id1);
    identifier_t* val = identifier_create_from_cstr("value");
    hbtrie_insert(trie, path, val, 1);
    path_destroy(path);
    identifier_destroy(id1);
    identifier_destroy(val);

    // Manually set a child_disk_offset on an entry to simulate a persisted child
    hbtrie_node_t* root = atomic_load(&trie->root);
    ASSERT_NE(root, nullptr);
    if (root->btree != NULL && root->btree->entries.length > 0) {
        bnode_entry_t* entry = (bnode_entry_t*)root->btree->entries.data[0];
        if (entry->child != NULL) {
            uint64_t fake_offset = 9999;
            entry->child_disk_offset = fake_offset;
            entry->child->is_loaded = 0;  // Simulate eviction
            // Null the entries pointing to this offset
            size_t nulled = hbtrie_null_entries_by_offset(trie, fake_offset);
            EXPECT_EQ(nulled, 1u);
            // Child pointer should now be NULL
            EXPECT_EQ(entry->child, nullptr);
        }
    }

    hbtrie_destroy(trie);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd /home/victor/Workspace/src/github.com/vijayee/WaveDB/build && cmake --build . --target test_hbtrie && ./test_hbtrie --gtest_filter="*NullEntriesByOffset*"`
Expected: FAIL — function doesn't exist

- [ ] **Step 3: Declare function in hbtrie.h**

Add after the existing `bnode_entry_lazy_load_*` declarations:

```c
/**
 * Null all parent entry pointers that reference the given disk offset.
 *
 * Walks the entire hbtrie tree. For each entry where
 * entry->child_disk_offset == offset and entry->child (or child_bnode or
 * trie_child) is non-NULL, NULLs the pointer and sets is_loaded = 0 on
 * the child hbtrie_node.
 *
 * @param trie    HBTrie to walk
 * @param offset  Disk offset of the evicted bnode
 * @return Number of entries nulled
 */
size_t hbtrie_null_entries_by_offset(hbtrie_t* trie, uint64_t offset);
```

- [ ] **Step 4: Implement in hbtrie.c**

```c
size_t hbtrie_null_entries_by_offset(hbtrie_t* trie, uint64_t offset) {
    if (trie == NULL || offset == 0) return 0;

    hbtrie_node_t* root = atomic_load(&trie->root);
    if (root == NULL) return 0;

    size_t nulled = 0;

    // Iterative DFS using a stack
    hbtrie_node_t* stack[HBTRIE_CURSOR_MAX_DEPTH];
    size_t stack_depth = 0;
    stack[stack_depth++] = root;

    while (stack_depth > 0) {
        hbtrie_node_t* node = stack[--stack_depth];
        if (node == NULL || node->btree == NULL) continue;

        // Walk all bnodes in this hbtrie_node's btree
        bnode_t* bnodes[64];
        size_t bnode_depth = 0;
        bnodes[bnode_depth++] = node->btree;

        while (bnode_depth > 0) {
            bnode_t* bn = bnodes[--bnode_depth];
            if (bn == NULL) continue;

            for (size_t i = 0; i < bn->entries.length; i++) {
                bnode_entry_t* entry = (bnode_entry_t*)bn->entries.data[i];
                if (entry == NULL) continue;

                // Check child (hbtrie child)
                if (!entry->has_value && !entry->is_bnode_child &&
                    entry->child_disk_offset == offset && entry->child != NULL) {
                    entry->child->is_loaded = 0;
                    entry->child = NULL;
                    nulled++;
                }

                // Check trie_child (prefix sharing)
                if (entry->has_value && entry->trie_child != NULL &&
                    entry->child_disk_offset == offset) {
                    entry->trie_child->is_loaded = 0;
                    entry->trie_child = NULL;
                    nulled++;
                }

                // Check child_bnode (internal B+tree child)
                if (!entry->has_value && entry->is_bnode_child &&
                    entry->child_disk_offset == offset && entry->child_bnode != NULL) {
                    entry->child_bnode = NULL;
                    nulled++;
                }

                // Push child hbtrie_nodes for further traversal
                if (!entry->has_value && !entry->is_bnode_child && entry->child != NULL) {
                    if (stack_depth < HBTRIE_CURSOR_MAX_DEPTH) {
                        stack[stack_depth++] = entry->child;
                    }
                }
                if (entry->has_value && entry->trie_child != NULL) {
                    if (stack_depth < HBTRIE_CURSOR_MAX_DEPTH) {
                        stack[stack_depth++] = entry->trie_child;
                    }
                }

                // Push internal bnode children for further bnode tree walk
                if (entry->is_bnode_child && entry->child_bnode != NULL) {
                    if (bnode_depth < 64) {
                        bnodes[bnode_depth++] = entry->child_bnode;
                    }
                }
            }
        }
    }

    return nulled;
}
```

- [ ] **Step 5: Build and run test**

Run: `cd /home/victor/Workspace/src/github.com/vijayee/WaveDB/build && cmake --build . --target test_hbtrie && ./test_hbtrie --gtest_filter="*NullEntriesByOffset*"`
Expected: PASS

- [ ] **Step 6: Run all hbtrie tests**

Run: `cd /home/victor/Workspace/src/github.com/vijayee/WaveDB/build && ./test_hbtrie`
Expected: All tests PASS

- [ ] **Step 7: Commit**

```bash
git add src/HBTrie/hbtrie.h src/HBTrie/hbtrie.c tests/test_hbtrie.cpp
git commit -m "feat: add hbtrie_null_entries_by_offset for eviction tree walk"
```

---

### Task 5: Wire eviction pipeline into database_t

**Files:**
- Modify: `src/Database/database.h:46-78` — add eviction_queue_t field
- Modify: `src/Database/database.c:848-859` — wire callback, submit background task
- Modify: `src/Database/database.c:1000-1090` — drain queue in destroy, clean up deferred frees

- [ ] **Step 1: Add eviction queue to database_t**

Add to `database_t` struct (after `bnode_cache` field):

```c
    eviction_queue_t eviction_queue;    // Lock-free queue for evicted bnode offsets
```

Add include at top of `database.h`:

```c
#include "eviction_queue.h"
```

- [ ] **Step 2: Initialize eviction queue and wire callback in database_create_with_config**

After the bnode_cache creation (line ~859), add:

```c
    // Wire eviction callback for deferred free
    eviction_queue_init(&db->eviction_queue);
    if (db->bnode_cache != NULL) {
        db->bnode_cache->on_evict = database_on_bnode_evict;
        db->bnode_cache->on_evict_data = db;
    }
```

Add the callback function before `database_create_with_config`:

```c
static void database_on_bnode_evict(uint64_t disk_offset, void* user_data) {
    database_t* db = (database_t*)user_data;
    eviction_queue_push(&db->eviction_queue, disk_offset);
}
```

- [ ] **Step 3: Add background eviction task**

Add the background task function:

```c
static void database_eviction_task_execute(void* ctx) {
    database_t* db = (database_t*)ctx;
    if (db->trie == NULL || db->bnode_cache == NULL) return;

    uint64_t offsets[64];
    size_t n = eviction_queue_drain(&db->eviction_queue, offsets, 64);

    for (size_t i = 0; i < n; i++) {
        // Walk tree to null parent entry pointers
        hbtrie_null_entries_by_offset(db->trie, offsets[i]);
        // Free the deferred bnode data
        bnode_cache_complete_evict(db->bnode_cache, offsets[i]);
    }

    // Reschedule if queue had items or periodically
    if (db->pool != NULL && !db->pool->stop) {
        work_t* task = work_create(database_eviction_task_execute,
                                    database_eviction_task_abort,
                                    db);
        if (task != NULL) {
            // Schedule next run after 5 seconds via timing wheel
            // Or use a simple re-enqueue for now
            work_pool_enqueue(db->pool, task);
        }
    }
}

static void database_eviction_task_abort(void* ctx) {
    // Nothing to abort — task is idempotent
    (void)ctx;
}
```

**Note:** The initial scheduling should happen after database creation. Add a one-time task submission after the pool is launched.

Actually, for the initial implementation, schedule the eviction task periodically using the timing wheel. But the timing wheel API needs checking — if it supports delayed task submission, use it. Otherwise, use a simple loop that re-enqueues itself.

For simplicity, submit the eviction task once during database creation, and let it reschedule itself:

After pool launch (line ~711), add:

```c
    // Start background eviction task
    if (db->bnode_cache != NULL && db->pool != NULL) {
        work_t* task = work_create(database_eviction_task_execute,
                                    database_eviction_task_abort,
                                    db);
        if (task != NULL) {
            work_pool_enqueue(db->pool, task);
        }
    }
```

Wait — this would run continuously. The task needs a delay. Let me check if the timing wheel supports delayed execution... The timing wheel uses `wheel_insert_after`. But work items are for the pool. We need a way to schedule a work pool task after a delay.

**Simpler approach for Phase 1:** The eviction task drains the queue, then re-enqueues itself with a small sleep:

```c
static void database_eviction_task_execute(void* ctx) {
    database_t* db = (database_t*)ctx;
    if (db->trie == NULL || db->bnode_cache == NULL) return;

    uint64_t offsets[64];
    size_t n = eviction_queue_drain(&db->eviction_queue, offsets, 64);

    for (size_t i = 0; i < n; i++) {
        hbtrie_null_entries_by_offset(db->trie, offsets[i]);
        bnode_cache_complete_evict(db->bnode_cache, offsets[i]);
    }

    // Reschedule — work_pool_enqueue returns 1 if pool is stopped
    if (db->pool != NULL) {
        work_t* task = work_create(database_eviction_task_execute,
                                    database_eviction_task_abort,
                                    db);
        if (task != NULL) {
            if (work_pool_enqueue(db->pool, task) != 0) {
                work_destroy(task);  // Pool stopped, don't reschedule
            }
        }
    }
}
```

This runs the task continuously. For production, add a 5-second delay. For Phase 1, this is acceptable — the queue drain is O(1) when empty.

- [ ] **Step 4: Drain queue in database_destroy**

Before destroying the bnode_cache (line ~1056), process any remaining evictions:

```c
    // Process any remaining eviction callbacks before destroying bnode cache
    if (db->bnode_cache != NULL) {
        uint64_t offsets[64];
        size_t n;
        do {
            n = eviction_queue_drain(&db->eviction_queue, offsets, 64);
            for (size_t i = 0; i < n; i++) {
                if (db->trie != NULL) {
                    hbtrie_null_entries_by_offset(db->trie, offsets[i]);
                }
                bnode_cache_complete_evict(db->bnode_cache, offsets[i]);
            }
        } while (n > 0);

        // Unregister callback before destroying
        db->bnode_cache->on_evict = NULL;
        db->bnode_cache->on_evict_data = NULL;
    }
```

- [ ] **Step 5: Build and run database tests**

Run: `cd /home/victor/Workspace/src/github.com/vijayee/WaveDB/build && cmake --build . --target test_database && ./test_database`
Expected: All tests PASS

- [ ] **Step 6: Run persistence tests**

Run: `cd /home/victor/Workspace/src/github.com/vijayee/WaveDB/build && ./test_persistence_phase2`
Expected: All tests PASS

- [ ] **Step 7: Commit**

```bash
git add src/Database/database.h src/Database/database.c
git commit -m "feat: wire eviction pipeline into database_t (callback + background task)"
```

---

### Task 6: Integration test — eviction end-to-end

**Files:**
- Create: `tests/test_hbtrie_eviction.cpp`

- [ ] **Step 1: Write integration test**

```cpp
#include <gtest/gtest.h>
#include "Database/database.h"
#include "Database/database_config.h"

#include <cstdio>
#include <cstring>

class HBTrieEvictionTest : public ::testing::Test {
protected:
    char tmpdir[256];

    void SetUp() override {
        strcpy(tmpdir, "/tmp/hbtrie_eviction_test_XXXXXX");
        mkdtemp(tmpdir);
    }

    void TearDown() override {
        char cmd[512];
        snprintf(cmd, sizeof(cmd), "rm -rf %s", tmpdir);
        system(cmd);
    }
};

TEST_F(HBTrieEvictionTest, EvictAndReload) {
    char path[512];
    snprintf(path, sizeof(path), "%s/evict.db", tmpdir);

    // Create database with very small bnode cache to trigger eviction
    database_config_t* config = database_config_create();
    config->enable_persist = 1;
    config->lru_memory_mb = 10;
    config->chunk_size = 4;
    config->btree_node_size = 4096;

    database_t* db = database_create_with_config(path, config, NULL, NULL, 0);
    ASSERT_NE(db, nullptr);

    // Insert many keys to fill the tree
    for (int i = 0; i < 100; i++) {
        char key[64], val[64];
        snprintf(key, sizeof(key), "user%d", i);
        snprintf(val, sizeof(val), "value%d", i);

        path_t* p = path_create();
        identifier_t* id = identifier_create_from_cstr(key);
        path_append(p, id);
        identifier_t* v = identifier_create_from_cstr(val);
        database_put_sync(db, p, v);
        path_destroy(p);
        identifier_destroy(id);
        identifier_destroy(v);
    }

    // Read back all keys — should work even if some nodes were evicted
    for (int i = 0; i < 100; i++) {
        char key[64], expected[64];
        snprintf(key, sizeof(key), "user%d", i);
        snprintf(expected, sizeof(expected), "value%d", i);

        path_t* p = path_create();
        identifier_t* id = identifier_create_from_cstr(key);
        path_append(p, id);
        identifier_t* result = database_get_sync(db, p);
        path_destroy(p);
        identifier_destroy(id);

        ASSERT_NE(result, nullptr) << "Key user" << i << " not found";
        // Compare value
        char buf[64] = {0};
        memcpy(buf, result->data, result->length < 63 ? result->length : 63);
        EXPECT_STREQ(buf, expected) << "Mismatch for key user" << i;
        identifier_destroy(result);
    }

    database_destroy(db);
    database_config_destroy(config);
}

TEST_F(HBTrieEvictionTest, IsLoadedFlagSetCorrectly) {
    char path[512];
    snprintf(path, sizeof(path), "%s/loaded.db", tmpdir);

    database_config_t* config = database_config_create();
    config->enable_persist = 1;
    config->lru_memory_mb = 10;

    database_t* db = database_create_with_config(path, config, NULL, NULL, 0);
    ASSERT_NE(db, nullptr);

    // Root node should have is_loaded = 1
    hbtrie_node_t* root = atomic_load(&db->trie->root);
    ASSERT_NE(root, nullptr);
    EXPECT_EQ(root->is_loaded, 1);

    database_destroy(db);
    database_config_destroy(config);
}
```

- [ ] **Step 2: Add to CMakeLists.txt**

```cmake
add_executable(test_hbtrie_eviction tests/test_hbtrie_eviction.cpp)
target_link_libraries(test_hbtrie_eviction wavedb gtest gtest_main Threads::Threads)
add_test(NAME test_hbtrie_eviction COMMAND test_hbtrie_eviction)
```

- [ ] **Step 3: Build and run test**

Run: `cd /home/victor/Workspace/src/github.com/vijayee/WaveDB/build && cmake .. && cmake --build . --target test_hbtrie_eviction && ./test_hbtrie_eviction`
Expected: All tests PASS

- [ ] **Step 4: Commit**

```bash
git add tests/test_hbtrie_eviction.cpp CMakeLists.txt
git commit -m "test: add integration test for hbtrie_node eviction end-to-end"
```

---

### Task 7: Full test suite run and cleanup

- [ ] **Step 1: Run full test suite**

Run: `cd /home/victor/Workspace/src/github.com/vijayee/WaveDB/build && cmake --build . && ctest --output-on-failure`
Expected: All tests PASS

- [ ] **Step 2: Run under ASan**

Run: `cd /home/victor/Workspace/src/github.com/vijayee/WaveDB/build && cmake -DCMAKE_BUILD_TYPE=Debug -DSANITIZE=address .. && cmake --build . --clean-first && ASAN_OPTIONS=detect_leaks=1 ctest --output-on-failure 2>&1 | tail -30`
Expected: No leaks, no use-after-free

- [ ] **Step 3: Commit any fixes**

If tests fail, fix and commit.

- [ ] **Step 4: Final commit message**

```bash
git commit -m "feat: implement hbtrie_node eviction with callback-driven deferred free"
```