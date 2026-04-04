# Performance Optimizations Phase 2: Profiling-Guided Improvements

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Eliminate performance bottlenecks identified through profiling: getenv in hot path, LRU cache lock contention, and memory allocation overhead.

**Architecture:** 
- Remove getenv call from chunk_compare hot path (3.68% overhead)
- Implement sharded LRU cache to eliminate read lock contention (6.73% overhead)
- Add buffer_t and path_t to memory pool to reduce allocation overhead (11.23% overhead)

**Tech Stack:** C (native library), pthreads, atomic operations

---

## Profiling Summary

Performance bottlenecks identified via `perf record`:

| Hotspot | Overhead | Impact |
|---------|----------|--------|
| Memory allocation (_int_malloc, _int_free, calloc) | 11.23% | High |
| Lock contention (pthread_mutex_lock/unlock) | 6.73% | Medium-High |
| getenv in chunk_compare | 3.68% | High (avoidable) |
| Comparison functions | 4.75% | Medium |
| Logging overhead | ~1-2% | Low |

---

## Task 1: Remove getenv from Hot Path

**Files:**
- Modify: `src/HBTrie/chunk.c`

**Problem:** `getenv("WAVEDB_DEBUG_CHUNK")` is called on every chunk comparison. This traverses the environment variables list each call, causing 3.68% CPU overhead.

**Solution:** Cache the debug flag at program startup, check a static variable instead.

- [ ] **Step 1: Add static debug flag to chunk.c**

At the top of `src/HBTrie/chunk.c` (after the includes), add:

```c
// Cached debug flag - initialized once on first use
static int debug_chunk_flag = -1;  // -1 = not initialized, 0 = off, 1 = on

// Check and cache the debug flag
static int get_debug_chunk_flag(void) {
    if (debug_chunk_flag == -1) {
        debug_chunk_flag = getenv("WAVEDB_DEBUG_CHUNK") ? 1 : 0;
    }
    return debug_chunk_flag;
}
```

- [ ] **Step 2: Modify chunk_compare to use cached flag**

Replace the `chunk_compare` function (lines 67-98):

```c
int chunk_compare(chunk_t* a, chunk_t* b) {
  if (a == NULL && b == NULL) return 0;
  if (a == NULL) return -1;
  if (b == NULL) return 1;

  // Compare chunk data byte by byte
  size_t size_a = a->data->size;
  size_t size_b = b->data->size;
  size_t min_size = size_a < size_b ? size_a : size_b;

  // Debug logging for WAL recovery (cached flag - no getenv per call)
  if (get_debug_chunk_flag()) {
    fprintf(stderr, "CHUNK_COMPARE: size_a=%zu, size_b=%zu\n", size_a, size_b);
    fprintf(stderr, "  Chunk A: ");
    for (size_t i = 0; i < size_a && i < 8; i++) {
      fprintf(stderr, "%02x ", a->data->data[i]);
    }
    fprintf(stderr, "\n  Chunk B: ");
    for (size_t i = 0; i < size_b && i < 8; i++) {
      fprintf(stderr, "%02x ", b->data->data[i]);
    }
    fprintf(stderr, "\n");
  }

  int cmp = memcmp(a->data->data, b->data->data, min_size);
  if (cmp != 0) return cmp;

  // If equal up to min_size, shorter chunk is "less"
  if (size_a < size_b) return -1;
  if (size_a > size_b) return 1;
  return 0;
}
```

- [ ] **Step 3: Build and verify**

```bash
cd /home/victor/Workspace/src/github.com/vijayee/WaveDB/build
cmake --build . -j$(nproc)
```

- [ ] **Step 4: Run C tests**

```bash
ctest --test-dir build -j$(nproc) --output-on-failure
```

Expected: All tests pass.

- [ ] **Step 5: Commit**

```bash
git add src/HBTrie/chunk.c
git commit -m "perf(chunk): cache debug flag to remove getenv from hot path"
```

---

## Task 2: Implement Sharded LRU Cache

**Files:**
- Modify: `src/Database/database_lru.c`
- Modify: `src/Database/database_lru.h`
- Modify: `src/Database/database.c`

**Problem:** The LRU cache uses a single global lock (`platform_lock(&lru->lock)`) for all operations. Read operations acquire this lock even though the underlying HBTrie reads are lock-free via MVCC.

**Solution:** Shard the cache into multiple independent locks, similar to the existing write lock sharding pattern. This reduces contention by factor of N (where N = number of shards).

- [ ] **Step 1: Define shard count in database_lru.h**

In `src/Database/database_lru.h`, add after the includes:

```c
// Number of LRU cache shards - should match NUM_WRITE_LOCK_SHARDS
#define NUM_LRU_SHARDS 16
```

- [ ] **Step 2: Redefine database_lru_cache_t structure**

Replace the `database_lru_cache_t` typedef in `src/Database/database_lru.h`:

```c
// Single shard of the LRU cache
typedef struct database_lru_shard {
    hashmap_t cache;              // Hashmap: path_t* -> database_lru_node_t*
    database_lru_node_t* first;   // Most recently used
    database_lru_node_t* last;    // Least recently used (eviction candidate)
    size_t current_memory;        // Current memory usage in bytes
    size_t max_memory;            // Max memory for this shard
    size_t entry_count;           // Number of entries in this shard
    PLATFORMLOCKTYPE lock;        // Lock for this shard only
} database_lru_shard_t;

// Sharded LRU cache
typedef struct database_lru_cache {
    database_lru_shard_t shards[NUM_LRU_SHARDS];
    size_t total_max_memory;      // Total memory budget across all shards
} database_lru_cache_t;
```

- [ ] **Step 3: Update database_lru_cache_create**

Replace `database_lru_cache_create` in `src/Database/database_lru.c`:

```c
database_lru_cache_t* database_lru_cache_create(size_t max_memory_bytes) {
    database_lru_cache_t* lru = get_clear_memory(sizeof(database_lru_cache_t));
    if (lru == NULL) return NULL;

    lru->total_max_memory = max_memory_bytes;
    size_t per_shard_memory = (max_memory_bytes == 0) ?
        (DATABASE_DEFAULT_LRU_MEMORY_MB * 1024 * 1024) / NUM_LRU_SHARDS :
        max_memory_bytes / NUM_LRU_SHARDS;

    for (int i = 0; i < NUM_LRU_SHARDS; i++) {
        database_lru_shard_t* shard = &lru->shards[i];
        
        hashmap_init(&shard->cache, (size_t (*)(const path_t*))hash_path, 
                     (int (*)(const path_t*, const path_t*))compare_path);
        hashmap_set_key_alloc_funcs(&shard->cache,
            (path_t* (*)(const path_t*))dup_path,
            (void (*)(path_t*))free_path);
        
        shard->first = NULL;
        shard->last = NULL;
        shard->current_memory = 0;
        shard->max_memory = per_shard_memory;
        shard->entry_count = 0;
        
        platform_lock_init(&shard->lock);
    }

    return lru;
}
```

- [ ] **Step 4: Add helper to get shard from path**

Add after `free_path` function:

```c
// Get shard index for a path (consistent hashing)
static size_t get_shard_index(const path_t* path) {
    if (path == NULL) return 0;
    
    // Use the hash to distribute paths across shards
    size_t hash = hash_path(path);
    return hash % NUM_LRU_SHARDS;
}
```

- [ ] **Step 5: Update database_lru_cache_destroy**

```c
void database_lru_cache_destroy(database_lru_cache_t* lru) {
    if (lru == NULL) return;

    for (int i = 0; i < NUM_LRU_SHARDS; i++) {
        database_lru_shard_t* shard = &lru->shards[i];
        
        platform_lock(&shard->lock);
        
        // Free all nodes in this shard
        database_lru_node_t* node = shard->first;
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
        
        hashmap_cleanup(&shard->cache);
        platform_unlock(&shard->lock);
        platform_lock_destroy(&shard->lock);
    }
    
    free(lru);
}
```

- [ ] **Step 6: Update database_lru_cache_get**

```c
identifier_t* database_lru_cache_get(database_lru_cache_t* lru, path_t* path) {
    if (lru == NULL || path == NULL) {
        return NULL;
    }

    size_t shard_idx = get_shard_index(path);
    database_lru_shard_t* shard = &lru->shards[shard_idx];

    platform_lock(&shard->lock);

    database_lru_node_t* node = hashmap_get(&shard->cache, path);
    if (node == NULL) {
        platform_unlock(&shard->lock);
        return NULL;
    }

    // Move to front (most recently used)
    lru_move_to_front(shard, node);

    // Return reference to value
    identifier_t* value = (identifier_t*)refcounter_reference((refcounter_t*)node->value);

    platform_unlock(&shard->lock);
    return value;
}
```

- [ ] **Step 7: Update lru_move_to_front (now takes shard parameter)**

```c
// Move node to front of LRU list (most recently used)
static void lru_move_to_front(database_lru_shard_t* shard, database_lru_node_t* node) {
    if (node == shard->first) {
        return; // Already at front
    }

    // Remove from current position
    if (node->previous != NULL) {
        node->previous->next = node->next;
    }
    if (node->next != NULL) {
        node->next->previous = node->previous;
    }
    if (node == shard->last) {
        shard->last = node->previous;
    }

    // Insert at front
    node->previous = NULL;
    node->next = shard->first;
    if (shard->first != NULL) {
        shard->first->previous = node;
    }
    shard->first = node;

    if (shard->last == NULL) {
        shard->last = node;
    }
}
```

- [ ] **Step 8: Update lru_evict (now takes shard parameter)**

```c
// Evict least recently used entry from shard
static identifier_t* lru_evict(database_lru_shard_t* shard) {
    if (shard->last == NULL) {
        return NULL;
    }

    database_lru_node_t* node = shard->last;

    // Remove from hashmap
    hashmap_remove(&shard->cache, node->path);

    // Remove from list
    if (node->previous != NULL) {
        node->previous->next = NULL;
    }
    if (node == shard->first) {
        shard->first = NULL;
    }
    shard->last = node->previous;

    // Update memory tracking
    shard->current_memory -= node->memory_size;
    shard->entry_count--;

    // Get value to return (caller must destroy)
    identifier_t* value = node->value;
    node->value = NULL;

    // Free path and node
    path_destroy(node->path);
    free(node);

    return value;
}
```

- [ ] **Step 9: Update database_lru_cache_put**

```c
identifier_t* database_lru_cache_put(database_lru_cache_t* lru, path_t* path, identifier_t* value) {
    if (lru == NULL || path == NULL) {
        if (path != NULL) path_destroy(path);
        if (value != NULL) identifier_destroy(value);
        return NULL;
    }

    size_t shard_idx = get_shard_index(path);
    database_lru_shard_t* shard = &lru->shards[shard_idx];

    platform_lock(&shard->lock);

    // Check if already exists
    database_lru_node_t* existing = hashmap_get(&shard->cache, path);
    identifier_t* ejected = NULL;

    if (existing != NULL) {
        // Update existing entry
        identifier_t* old_value = existing->value;
        existing->value = value;

        // Update memory tracking
        size_t old_memory = existing->memory_size;
        existing->memory_size = calculate_entry_memory(path, value);
        shard->current_memory += (existing->memory_size - old_memory);

        path_destroy(path); // We don't need the new path, keep the old one
        lru_move_to_front(shard, existing);
        platform_unlock(&shard->lock);
        return old_value; // Caller must destroy old value
    }

    // Check if we need to evict (memory-based)
    size_t entry_memory = calculate_entry_memory(path, value);
    while (shard->current_memory + entry_memory > shard->max_memory && shard->last != NULL) {
        identifier_t* evicted = lru_evict(shard);
        if (evicted != NULL) {
            identifier_destroy(evicted);
        }
    }

    // Create new node
    database_lru_node_t* node = lru_node_create(path, value);
    if (node == NULL) {
        platform_unlock(&shard->lock);
        return ejected;
    }

    // Add to hashmap
    int result = hashmap_put(&shard->cache, node->path, node);
    if (result != 0) {
        lru_node_destroy(node);
        platform_unlock(&shard->lock);
        return ejected;
    }

    // Add to front of list
    node->next = shard->first;
    if (shard->first != NULL) {
        shard->first->previous = node;
    }
    shard->first = node;
    if (shard->last == NULL) {
        shard->last = node;
    }

    // Update memory tracking
    shard->current_memory += node->memory_size;
    shard->entry_count++;

    platform_unlock(&shard->lock);
    return ejected;
}
```

- [ ] **Step 10: Update database_lru_cache_delete**

```c
void database_lru_cache_delete(database_lru_cache_t* lru, path_t* path) {
    if (lru == NULL || path == NULL) {
        return;
    }

    size_t shard_idx = get_shard_index(path);
    database_lru_shard_t* shard = &lru->shards[shard_idx];

    platform_lock(&shard->lock);

    database_lru_node_t* node = hashmap_get(&shard->cache, path);
    if (node == NULL) {
        platform_unlock(&shard->lock);
        return;
    }

    // Remove from hashmap
    hashmap_remove(&shard->cache, path);

    // Remove from list
    if (node->previous != NULL) {
        node->previous->next = node->next;
    }
    if (node->next != NULL) {
        node->next->previous = node->previous;
    }
    if (node == shard->first) {
        shard->first = node->next;
    }
    if (node == shard->last) {
        shard->last = node->previous;
    }

    // Update memory tracking
    shard->current_memory -= node->memory_size;
    shard->entry_count--;

    // Free node
    lru_node_destroy(node);

    platform_unlock(&shard->lock);
}
```

- [ ] **Step 11: Update database_lru_cache_contains**

```c
uint8_t database_lru_cache_contains(database_lru_cache_t* lru, path_t* path) {
    if (lru == NULL || path == NULL) {
        return 0;
    }

    size_t shard_idx = get_shard_index(path);
    database_lru_shard_t* shard = &lru->shards[shard_idx];

    platform_lock(&shard->lock);
    database_lru_node_t* node = hashmap_get(&shard->cache, path);
    platform_unlock(&shard->lock);

    return node != NULL ? 1 : 0;
}
```

- [ ] **Step 12: Update database_lru_cache_clear**

```c
void database_lru_cache_clear(database_lru_cache_t* lru) {
    if (lru == NULL) return;

    for (int i = 0; i < NUM_LRU_SHARDS; i++) {
        database_lru_shard_t* shard = &lru->shards[i];
        
        platform_lock(&shard->lock);

        // Free all nodes
        database_lru_node_t* node = shard->first;
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

        // Clear hashmap
        hashmap_clear(&shard->cache);

        shard->first = NULL;
        shard->last = NULL;
        shard->current_memory = 0;
        shard->entry_count = 0;

        platform_unlock(&shard->lock);
    }
}
```

- [ ] **Step 13: Update database_lru_cache_size**

```c
size_t database_lru_cache_size(database_lru_cache_t* lru) {
    if (lru == NULL) return 0;

    size_t total = 0;
    for (int i = 0; i < NUM_LRU_SHARDS; i++) {
        platform_lock(&lru->shards[i].lock);
        total += lru->shards[i].entry_count;
        platform_unlock(&lru->shards[i].lock);
    }
    return total;
}
```

- [ ] **Step 14: Build and verify**

```bash
cd /home/victor/Workspace/src/github.com/vijayee/WaveDB/build
cmake --build . -j$(nproc)
```

- [ ] **Step 15: Run C tests**

```bash
ctest --test-dir build -j$(nproc) --output-on-failure
```

- [ ] **Step 16: Commit**

```bash
git add src/Database/database_lru.h src/Database/database_lru.c
git commit -m "perf(lru): implement sharded LRU cache to reduce lock contention"
```

---

## Task 3: Add buffer_t to Memory Pool

**Files:**
- Modify: `src/Buffer/buffer.c`

**Problem:** `buffer_t` allocations use `get_clear_memory()` which calls `calloc()` directly. Buffers are frequently created/destroyed in hot paths.

**Solution:** Use the memory pool for buffer_t allocations (typically 24-32 bytes).

- [ ] **Step 1: Add memory pool include in buffer.c**

At the top of `src/Buffer/buffer.c`, add after existing includes:

```c
#include "../Util/memory_pool.h"
```

- [ ] **Step 2: Modify buffer_create to use memory pool**

Replace `buffer_create` function (lines 13-19):

```c
buffer_t* buffer_create(size_t size) {
  buffer_t* buf = (buffer_t*)memory_pool_alloc(sizeof(buffer_t));
  if (buf == NULL) {
    buf = get_clear_memory(sizeof(buffer_t));
  }
  buf->data = get_clear_memory(size);
  buf->size = size;
  refcounter_init((refcounter_t*) buf);
  return buf;
}
```

- [ ] **Step 3: Modify buffer_destroy to use memory pool free**

Replace `buffer_destroy` function (lines 57-66):

```c
void buffer_destroy(buffer_t* buf) {
  refcounter_dereference((refcounter_t*)buf);
  if (refcounter_count((refcounter_t*)buf) == 0) {
    free(buf->data);
    refcounter_destroy_lock((refcounter_t*)buf);
    memory_pool_free(buf, sizeof(buffer_t));
  }
}
```

- [ ] **Step 4: Modify buffer_create_from_pointer_copy**

Replace (lines 21-28):

```c
buffer_t* buffer_create_from_pointer_copy(uint8_t* data, size_t size) {
  buffer_t* buf = (buffer_t*)memory_pool_alloc(sizeof(buffer_t));
  if (buf == NULL) {
    buf = get_clear_memory(sizeof(buffer_t));
  }
  buf->size = size;
  buf->data = get_memory(size);
  buffer_copy_from_pointer(buf, data, size);
  refcounter_init((refcounter_t*) buf);
  return buf;
}
```

- [ ] **Step 5: Modify buffer_create_from_existing_memory**

Replace (lines 30-36):

```c
buffer_t* buffer_create_from_existing_memory(uint8_t* data, size_t size) {
  buffer_t* buf = (buffer_t*)memory_pool_alloc(sizeof(buffer_t));
  if (buf == NULL) {
    buf = get_clear_memory(sizeof(buffer_t));
  }
  buf->data = data;
  buf->size = size;
  refcounter_init((refcounter_t*) buf);
  return buf;
}
```

- [ ] **Step 6: Build and verify**

```bash
cd /home/victor/Workspace/src/github.com/vijayee/WaveDB/build
cmake --build . -j$(nproc)
```

- [ ] **Step 7: Run C tests**

```bash
ctest --test-dir build -j$(nproc) --output-on-failure
```

- [ ] **Step 8: Commit**

```bash
git add src/Buffer/buffer.c
git commit -m "perf(buffer): use memory pool for buffer_t allocations"
```

---

## Task 4: Add path_t to Memory Pool

**Files:**
- Modify: `src/HBTrie/path.c`

**Problem:** `path_t` allocations use `get_clear_memory()` directly. Paths are created frequently in hot paths.

**Solution:** Use the memory pool for path_t allocations (typically 32-48 bytes).

- [ ] **Step 1: Add memory pool include in path.c**

At the top of `src/HBTrie/path.c`, add after existing includes:

```c
#include "../Util/memory_pool.h"
```

- [ ] **Step 2: Modify path_create to use memory pool**

Replace `path_create` function (lines 10-15):

```c
path_t* path_create(void) {
  path_t* path = (path_t*)memory_pool_alloc(sizeof(path_t));
  if (path == NULL) {
    path = get_clear_memory(sizeof(path_t));
  }
  vec_init(&path->identifiers);
  refcounter_init((refcounter_t*)path);
  return path;
}
```

- [ ] **Step 3: Modify path_destroy to use memory pool free**

Replace `path_destroy` function (lines 28-44):

```c
void path_destroy(path_t* path) {
  if (path == NULL) return;

  refcounter_dereference((refcounter_t*)path);
  if (refcounter_count((refcounter_t*)path) == 0) {
    // Destroy all identifiers
    identifier_t* id;
    int i;
    vec_foreach(&path->identifiers, id, i) {
      identifier_destroy(id);
    }
    vec_deinit(&path->identifiers);

    refcounter_destroy_lock((refcounter_t*)path);
    memory_pool_free(path, sizeof(path_t));
  }
}
```

- [ ] **Step 4: Build and verify**

```bash
cd /home/victor/Workspace/src/github.com/vijayee/WaveDB/build
cmake --build . -j$(nproc)
```

- [ ] **Step 5: Run C tests**

```bash
ctest --test-dir build -j$(nproc) --output-on-failure
```

- [ ] **Step 6: Commit**

```bash
git add src/HBTrie/path.c
git commit -m "perf(path): use memory pool for path_t allocations"
```

---

## Task 5: Run Full Test Suite

- [ ] **Step 1: Run all C tests**

```bash
ctest --test-dir build -j$(nproc) --output-on-failure
```

Expected: All tests pass.

- [ ] **Step 2: Run benchmarks**

```bash
./build/tests/benchmark/benchmark_database
```

Record throughput numbers for comparison.

- [ ] **Step 3: Build Dart bindings**

```bash
cd /home/victor/Workspace/src/github.com/vijayee/WaveDB/bindings/dart
cp ../../build/libwavedb.so.0.1.0 ./libwavedb.so
```

- [ ] **Step 4: Run Dart tests**

```bash
cd /home/victor/Workspace/src/github.com/vijayee/WaveDB/bindings/dart
LD_LIBRARY_PATH=$(pwd) dart test
```

Expected: All tests pass.

- [ ] **Step 5: Build NodeJS bindings**

```bash
cd /home/victor/Workspace/src/github.com/vijayee/WaveDB/bindings/nodejs
npm run build
```

- [ ] **Step 6: Run NodeJS tests**

```bash
cd /home/victor/Workspace/src/github.com/vijayee/WaveDB/bindings/nodejs
npm test
```

Expected: All tests pass.

---

## Task 6: Update README with New Benchmark Results

- [ ] **Step 1: Run final benchmarks**

```bash
./build/tests/benchmark/benchmark_database
```

- [ ] **Step 2: Update README.md performance section**

Update the performance tables in README.md with new throughput numbers.

- [ ] **Step 3: Commit**

```bash
git add README.md
git commit -m "docs: update benchmark results after phase 2 optimizations"
```

---

## Summary

**Optimizations implemented:**

| Task | Impact | Files |
|------|--------|-------|
| Remove getenv from chunk_compare | +3-5% throughput | `src/HBTrie/chunk.c` |
| Sharded LRU cache | +1-2% read throughput, better concurrency | `src/Database/database_lru.{c,h}` |
| Memory pool for buffer_t | +1-2% throughput | `src/Buffer/buffer.c` |
| Memory pool for path_t | +0.5-1% throughput | `src/HBTrie/path.c` |

**Expected total improvement:** 6-10% throughput improvement, significantly better concurrent read scaling.

**Lock contention reduction:** The sharded LRU cache reduces contention from a single global lock to 16 independent locks, improving concurrent read throughput by up to 16x under high contention.