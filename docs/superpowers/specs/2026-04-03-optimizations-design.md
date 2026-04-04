# WaveDB Performance Optimizations Design

**Date:** 2026-04-03
**Status:** Draft
**Priority:** Throughput (ops/sec)

## Overview

This document specifies a phased implementation of performance optimizations for WaveDB, targeting throughput improvements while maintaining correctness and cross-platform compatibility.

## Goals

- Single-threaded insert: ~100K ops/s → ~150K ops/s (50% improvement)
- Concurrent reads (8 cores): ~200K ops/s → ~800K ops/s (4x improvement)
- Fragment find: Maintain O(log n) instead of degrading to O(n)
- P99 latency: <10ms → <5ms (50% reduction)

## Non-Goals

- API changes or breaking changes to bindings
- Platform-specific builds (all optimizations must be portable)
- Optimizations that sacrifice correctness for speed

---

## Phase 1: Quick Wins (High ROI, Low Risk)

### 1.1 Fragment Re-sorting Fix

**Location:** `src/Storage/section.c:192-196`

**Problem:** After partial allocation from a fragment, the fragment remains in its original position even though its size changed. This breaks the size-sorted invariant, degrading `fragment_list_find_fit` from O(log n) toward O(n) over time.

**Solution:** Re-sort fragments after modification.

```c
static void fragment_list_re_sort(fragment_list_t* list, size_t index) {
    fragment_t* frag = list->fragments.data[index];
    vec_erase(&list->fragments, index);
    fragment_list_insert_sorted(list, frag);
}
```

**Files to modify:**
- `src/Storage/section.c` - Add re-sort after fragment modification

**Testing:**
- Existing unit tests should pass
- New test: verify fragment list remains sorted after repeated allocations

---

### 1.2 Memory Pool for Chunks

**Location:** `src/HBTrie/chunk.c`

**Problem:** Every chunk creation involves two heap allocations (`chunk_t` + `buffer_t`). This is a hot path in insert/find operations.

**Solution:** Use existing memory pool for `chunk_t` structures.

```c
extern memory_pool_t* g_small_pool;  // 64-byte class

chunk_t* chunk_create(const void* data, size_t chunk_size) {
    chunk_t* chunk = memory_pool_alloc(g_small_pool);
    if (!chunk) {
        chunk = get_clear_memory(sizeof(chunk_t));  // Fallback
    }
    // ... rest unchanged
}
```

**Files to modify:**
- `src/HBTrie/chunk.c` - Use memory pool
- `src/HBTrie/chunk.h` - Add pool declaration if needed

**Testing:**
- Existing unit tests should pass
- Benchmark: measure allocation improvement

---

### 1.3 Memory Pool for Identifiers

**Location:** `src/HBTrie/identifier.c`

**Problem:** Same as chunks - frequent heap allocations.

**Solution:** Use memory pool for `identifier_t` structures.

```c
extern memory_pool_t* g_medium_pool;  // 256-byte class

identifier_t* identifier_create(...) {
    identifier_t* id = memory_pool_alloc(g_medium_pool);
    if (!id) {
        id = get_clear_memory(sizeof(identifier_t));  // Fallback
    }
    // ... rest unchanged
}
```

**Files to modify:**
- `src/HBTrie/identifier.c` - Use memory pool
- `src/HBTrie/identifier.h` - Add pool declaration if needed

---

## Phase 2: Lock Contention (High Impact)

### 2.1 Fine-Grained HBTrie Locking

**Location:** `src/HBTrie/hbtrie.c`

**Problem:** Global `trie->lock` serializes all trie operations. Per-node locks exist but aren't used.

**Solution:** Use per-node locks with hand-over-hand locking during traversal.

```c
int hbtrie_insert(hbtrie_t* trie, path_t* path, identifier_t* value) {
    hbtrie_node_t* current = trie->root;
    hbtrie_node_t* parent = NULL;

    // Acquire root lock
    platform_lock(&current->lock);

    while (traversing) {
        // Find next node
        hbtrie_node_t* child = ...;

        if (child) {
            // Acquire child lock before releasing parent
            platform_lock(&child->lock);
            if (parent) platform_unlock(&parent->lock);
            current = child;
        } else {
            // Insert at current node
            // ...
            break;
        }
    }

    platform_unlock(&current->lock);
    return 0;
}
```

**Files to modify:**
- `src/HBTrie/hbtrie.c` - Implement hand-over-hand locking
- `src/HBTrie/hbtrie.h` - Document locking contract

**Testing:**
- Existing unit tests should pass
- Stress test: concurrent insert/find operations
- Benchmark: measure throughput improvement under concurrency

---

### 2.2 Thread-Local Memory Pool Cache

**Location:** `src/Util/memory_pool.c`

**Problem:** Global lock per size class creates contention under high allocation rates.

**Solution:** Thread-local free lists with global pool as backing.

```c
#define TLS_CACHE_SIZE 64

typedef struct {
    void* cache[TLS_CACHE_SIZE];
    size_t count;
} tls_pool_cache_t;

static __thread tls_pool_cache_t tls_small;
static __thread tls_pool_cache_t tls_medium;
static __thread tls_pool_cache_t tls_large;

void* memory_pool_alloc(memory_pool_t* pool) {
    // Check thread-local cache first (lock-free)
    if (pool->size_class == 64 && tls_small.count > 0) {
        return tls_small.cache[--tls_small.count];
    }
    if (pool->size_class == 256 && tls_medium.count > 0) {
        return tls_medium.cache[--tls_medium.count];
    }
    if (pool->size_class == 1024 && tls_large.count > 0) {
        return tls_large.cache[--tls_large.count];
    }

    // Fallback to global pool
    return memory_pool_global_alloc(pool);
}

void memory_pool_free(memory_pool_t* pool, void* ptr) {
    // Return to thread-local cache if space
    // Otherwise return to global pool
}
```

**Files to modify:**
- `src/Util/memory_pool.c` - Add TLS cache
- `src/Util/memory_pool.h` - Update declarations

**Testing:**
- Existing unit tests should pass
- Stress test: multi-threaded allocation patterns
- Benchmark: measure allocation throughput

---

### 2.3 Sharded Transaction Manager Locks

**Location:** `src/HBTrie/mvcc.c`

**Problem:** Computing min active transaction ID is O(n) with global lock.

**Solution:** Maintain min transaction ID with atomic updates, or use sharded locks.

```c
// Option A: Atomic min tracking
static atomic_uint64_t min_active_txn_id;

void transaction_manager_add(transaction_manager_t* mgr, transaction_t* txn) {
    // Add to active list
    // Update min if this transaction is smaller
    uint64_t expected = atomic_load(&mgr->min_active_txn_id);
    while (transaction_id_compare(&txn->id, &expected) < 0) {
        if (atomic_compare_exchange_weak(&mgr->min_active_txn_id, &expected, txn->id.id)) {
            break;
        }
    }
}

// Option B: Sharded locks for active transaction list
#define TXN_SHARDS 16
typedef struct {
    PLATFORMLOCKTYPE(lock);
    vec_t active_txns;
} txn_shard_t;

txn_shard_t shards[TXN_SHARDS];
```

**Files to modify:**
- `src/HBTrie/mvcc.c` - Implement sharded locks or atomic min

**Testing:**
- Existing unit tests should pass
- Stress test: many concurrent transactions

---

## Phase 3: Algorithmic Optimizations

### 3.1 SIMD Binary Search

**Location:** `src/HBTrie/bnode.c`

**Problem:** Binary search branch misprediction on small nodes hurts performance.

**Solution:** SIMD-accelerated comparison with runtime dispatch and scalar fallback.

```c
// Scalar fallback (always compiled)
static size_t bnode_search_scalar(bnode_t* node, chunk_t* key) {
    size_t lo = 0, hi = node->entries.length;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int cmp = chunk_compare(node->entries.data[mid]->key, key);
        if (cmp < 0) lo = mid + 1;
        else if (cmp > 0) hi = mid;
        else return mid;
    }
    return lo;
}

// AVX2 version (x86_64 with AVX2)
#if defined(__x86_64__)
#include <immintrin.h>

static size_t bnode_search_avx2(bnode_t* node, chunk_t* key) {
    // Compare multiple entries at once using AVX2
    // For nodes with <16 entries, linear SIMD scan
    // For larger nodes, SIMD-accelerated binary search
    __m256i key_vec = _mm256_loadu_si256((const __m256i*)key->data);
    // ... SIMD comparison logic
}
#endif

// NEON version (ARM64)
#if defined(__aarch64__)
#include <arm_neon.h>

static size_t bnode_search_neon(bnode_t* node, chunk_t* key) {
    // NEON comparison
}
#endif

// Runtime dispatch
static size_t (*bnode_search_func)(bnode_t*, chunk_t*) = NULL;

static void init_bnode_search_dispatcher(void) {
    #if defined(__x86_64__)
    if (__builtin_cpu_supports("avx2")) {
        bnode_search_func = bnode_search_avx2;
        return;
    }
    #endif
    #if defined(__aarch64__)
    bnode_search_func = bnode_search_neon;
    return;
    #endif
    bnode_search_func = bnode_search_scalar;
}

size_t bnode_search(bnode_t* node, chunk_t* key) {
    if (bnode_search_func == NULL) init_bnode_search_dispatcher();
    return bnode_search_func(node, key);
}
```

**Files to modify:**
- `src/HBTrie/bnode.c` - Add SIMD variants
- `src/HBTrie/bnode.h` - Update declarations
- `CMakeLists.txt` - Add compiler flags for SIMD

**Testing:**
- Unit tests with scalar-only builds
- Unit tests with SIMD builds
- Benchmark: compare scalar vs SIMD performance

---

### 3.2 xxHash for Path Hashing

**Location:** `src/Database/database_lru.c`

**Problem:** Byte-by-byte hashing is slow for long paths.

**Solution:** Use xxHash3, which has SIMD-accelerated variants and excellent performance.

```c
#define XXH_STATIC_LINKING_ONLY
#define XXH_IMPLEMENTATION
#include "xxhash.h"

static size_t hash_path(const path_t* path) {
    XXH3_state_t state;
    XXH3_64bits_reset(&state);

    for (size_t i = 0; i < path->identifiers.length; i++) {
        identifier_t* id = path->identifiers.data[i];
        for (size_t j = 0; j < id->chunks.length; j++) {
            chunk_t* chunk = id->chunks.data[j];
            XXH3_64bits_update(&state, chunk->data->data, chunk->data->size);
        }
    }

    return (size_t)XXH3_64bits_digest(&state);
}
```

**Files to add/modify:**
- `src/Util/xxhash.h` - Add xxHash header-only library
- `src/Database/database_lru.c` - Use xxHash
- `CMakeLists.txt` - Update if needed

**Testing:**
- Existing unit tests should pass
- Benchmark: compare old hash vs xxHash

---

### 3.3 Version Chain Fast-Path

**Location:** `src/HBTrie/bnode.c:276-307`

**Problem:** MVCC version chain traversal is O(n) in number of versions.

**Solution:** Optimize for the common case where readers want the latest committed version.

```c
version_entry_t* version_entry_find_visible(version_entry_t* versions,
                                             transaction_id_t read_txn_id) {
    // Fast path: most reads want the latest committed version
    if (versions != NULL) {
        // Check if latest version is visible
        if (transaction_id_compare(&versions->txn_id, &read_txn_id) <= 0 &&
            !versions->is_deleted) {
            return versions;  // Fast path hit
        }
    }

    // Slow path: traverse chain
    version_entry_t* current = versions;
    while (current != NULL) {
        if (transaction_id_compare(&current->txn_id, &read_txn_id) <= 0 &&
            !current->is_deleted) {
            return current;
        }
        current = current->next;
    }
    return NULL;
}
```

**Files to modify:**
- `src/HBTrie/bnode.c` - Add fast-path check

**Testing:**
- Existing MVCC tests should pass
- Benchmark: measure fast-path hit rate

---

## Phase 4: Data Structure Improvements (Lower Priority)

### 4.1 Fragment List Improvements

**Problem:** O(n) insert/remove due to memmove.

**Solutions (choose one):**
- Balanced tree (red-black or AVL) for O(log n) operations
- Size-segregated free lists (binned allocator pattern)
- Hybrid: keep sorted array for small lists, tree for large

**Priority:** Only implement if Phase 1-3 show good results and benchmarks indicate fragment list is still a bottleneck.

---

### 4.2 Transaction Manager Min-Heap

**Problem:** O(n) scan for min active transaction.

**Solution:** Maintain min-heap of active transactions.

**Priority:** Only if transaction manager shows up in profiles.

---

## Testing Strategy

### After Each Phase

```bash
# C unit tests
cmake --build build -j$(nproc)
ctest --test-dir build -j$(nproc) --output-on-failure

# Dart tests
cd bindings/dart
cp ../../build/libwavedb.so.0.1.0 ./libwavedb.so
LD_LIBRARY_PATH=$(pwd) dart test

# NodeJS tests
cd bindings/nodejs
npm test
```

### Benchmark Verification

```bash
# Database benchmark
./build/tests/benchmark/benchmark_database

# Compare before/after metrics
```

---

## Rollback Plan

Each phase is a separate commit. If a phase causes issues:

1. Revert the specific commit
2. Re-run tests to verify rollback
3. Rebuild bindings with previous working version

---

## File Summary

| Phase | Files Modified | Files Added |
|-------|----------------|-------------|
| 1.1 | `src/Storage/section.c` | - |
| 1.2 | `src/HBTrie/chunk.c`, `chunk.h` | - |
| 1.3 | `src/HBTrie/identifier.c`, `identifier.h` | - |
| 2.1 | `src/HBTrie/hbtrie.c`, `hbtrie.h` | - |
| 2.2 | `src/Util/memory_pool.c`, `memory_pool.h` | - |
| 2.3 | `src/HBTrie/mvcc.c` | - |
| 3.1 | `src/HBTrie/bnode.c`, `bnode.h` | - |
| 3.2 | `src/Database/database_lru.c` | `src/Util/xxhash.h` |
| 3.3 | `src/HBTrie/bnode.c` | - |
| 4.1 | TBD based on profiling | - |
| 4.2 | TBD based on profiling | - |

---

## Success Criteria

| Metric | Current | Target | Phase |
|--------|---------|--------|-------|
| Single-threaded insert | ~100K ops/s | ~150K ops/s | Phase 1 |
| Concurrent reads (8 cores) | ~200K ops/s | ~800K ops/s | Phase 2 |
| Fragment find | O(n) degraded | O(log n) stable | Phase 1 |
| P99 latency | <10ms | <5ms | Phase 2+3 |
| All tests passing | Yes | Yes | All phases |