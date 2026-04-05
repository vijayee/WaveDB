# Lock-Free LRU Cache Design

**Date:** 2026-04-05
**Author:** Claude
**Status:** Pending Review
**Goal:** Implement a high-throughput lock-free LRU cache based on eBay's algorithm, achieving ~1M+ lookups/sec through lock-free reads and atomic CAS operations.

---

## Executive Summary

Replace the current sharded LRU cache with a lock-free implementation based on eBay's design and the Michael-Scott lock-free queue algorithm. The new design eliminates mutex contention on the read path, enabling true concurrent access without blocking.

**Key Changes:**
- Lock-free reads using atomic CAS operations
- Lock-free LRU queue using Michael-Scott algorithm
- "Holes" mechanism for deferred cleanup
- Concurrent hashmap with striped write locks
- Memory pool integration for node allocation

**Expected Impact:**
- Read throughput: 2-10x improvement (lock-free reads)
- Write throughput: Maintained (striped locks, similar to current)
- Memory usage: Predictable with memory pool
- Scalability: Linear with core count (no read contention)

---

## Phase 1: Concurrent Hashmap

**Goal:** Create a thread-safe hashmap with lock-free reads and fine-grained write locks.

### Data Structure

```c
// src/Util/concurrent_hashmap.h

typedef struct chash_entry_t {
    void* key;                    // Key (owned by entry)
    _Atomic void* value;          // Value (atomic for lock-free reads)
    struct chash_entry_t* next;   // Chain for collision resolution
    uint32_t hash;                // Cached hash value
    _Atomic uint8_t tombstone;    // 1 if deleted (for lock-free removal)
} chash_entry_t;

typedef struct {
    PLATFORMLOCKTYPE(lock);       // Lock for this stripe only
    chash_entry_t** buckets;      // Bucket array
    size_t bucket_count;          // Number of buckets
    size_t entry_count;           // Entries in this stripe
    size_t tombstone_count;       // Deleted entries awaiting cleanup
} chash_stripe_t;

typedef struct {
    chash_stripe_t* stripes;      // Array of stripes
    size_t num_stripes;           // Number of stripes (power of 2)
    size_t stripe_mask;           // Mask for stripe selection
    
    chash_hash_fn hash_fn;
    chash_compare_fn compare_fn;
    chash_key_dup_fn key_dup_fn;
    chash_key_free_fn key_free_fn;
    
    size_t initial_bucket_count;
    float load_factor;
    
    _Atomic size_t total_entries;
} concurrent_hashmap_t;
```

### Operations

| Operation | Lock | Notes |
|-----------|------|-------|
| `concurrent_hashmap_get()` | None | Lock-free read via atomic pointer load |
| `concurrent_hashmap_contains()` | None | Lock-free check |
| `concurrent_hashmap_put()` | Stripe lock | Brief lock, striped for low contention |
| `concurrent_hashmap_put_if_absent()` | Stripe lock | Atomic insertion check |
| `concurrent_hashmap_remove()` | Stripe lock | Brief lock, marks as tombstone |

### Read Operation (Lock-Free)

```c
void* concurrent_hashmap_get(concurrent_hashmap_t* map, const void* key) {
    size_t hash = map->hash_fn(key);
    size_t stripe_idx = hash & map->stripe_mask;
    chash_stripe_t* stripe = &map->stripes[stripe_idx];
    
    size_t bucket_idx = hash % stripe->bucket_count;
    chash_entry_t* entry = atomic_load(&stripe->buckets[bucket_idx]);
    
    while (entry != NULL) {
        if (!atomic_load(&entry->tombstone) &&
            entry->hash == hash &&
            map->compare_fn(entry->key, key) == 0) {
            return atomic_load(&entry->value);
        }
        entry = entry->next;
    }
    
    return NULL;
}
```

### Write Operation (Striped Lock)

```c
void* concurrent_hashmap_put(concurrent_hashmap_t* map, const void* key, void* value) {
    size_t hash = map->hash_fn(key);
    size_t stripe_idx = hash & map->stripe_mask;
    chash_stripe_t* stripe = &map->stripes[stripe_idx];
    
    platform_lock(&stripe->lock);
    
    // ... find or create entry ...
    // ... check for resize ...
    
    platform_unlock(&stripe->lock);
    return old_value;
}
```

### Files

| File | Purpose |
|------|---------|
| `src/Util/concurrent_hashmap.h` | Header with data structures and API |
| `src/Util/concurrent_hashmap.c` | Implementation |
| `tests/test_concurrent_hashmap.cpp` | Unit tests |

---

## Phase 2: Lock-Free LRU Cache

**Goal:** Build the eBay-style LRU cache on top of the concurrent hashmap, using Michael-Scott lock-free queue for LRU ordering.

### Key Concepts

**1. Michael-Scott Lock-Free Queue**

Based on the [Michael-Scott paper](https://cs.rochester.edu/~scott/papers/1996_PODC_queues.pdf), the algorithm provides:
- Lock-free enqueue (add at tail)
- Lock-free dequeue (remove from head)
- CAS-based pointer updates
- Linearizable operations

**2. LRU Queue Semantics**

```
Head (LRU) ──► [node] ──► [node] ──► [node] ──► Tail (MRU)
              ^                                  ^
              |                                  |
           Evict here                        Insert here
```

**3. Hole Mechanism**

When an entry is re-accessed:
1. Create new node at tail (MRU position)
2. CAS the entry's `node` pointer to point to new node
3. Mark old node as hole (`entry = NULL`)
4. Background purge removes holes from queue

This avoids the need to remove nodes from the middle of the queue (which would require locks).

### Data Structure

```c
// src/Database/lockfree_lru.h

// LRU node - lives in the lock-free queue
typedef struct lru_node_t {
    _Atomic(struct lru_entry_t*) entry;  // NULL = hole (marked for cleanup)
    _Atomic(struct lru_node_t*) next;    // Next in queue (toward MRU)
} lru_node_t;

// LRU entry - lives in the concurrent hashmap
typedef struct lru_entry_t {
    refcounter_t refcounter;             // MUST be first
    path_t* path;                         // Key (immutable)
    identifier_t* value;                 // Value (reference counted)
    _Atomic(lru_node_t*) node;           // Current position in LRU queue
    size_t memory_size;                  // Memory footprint
} lru_entry_t;

// Lock-free LRU queue (Michael-Scott)
typedef struct {
    _Atomic(lru_node_t*) head;           // LRU end (for dequeue)
    _Atomic(lru_node_t*) tail;           // MRU end (for enqueue)
    _Atomic(size_t) node_count;          // Approximate count (including holes)
    _Atomic(size_t) hole_count;          // Holes pending cleanup
} lru_queue_t;

// Per-shard LRU state
typedef struct {
    concurrent_hashmap_t map;            // path_t* -> lru_entry_t*
    lru_queue_t queue;                    // Lock-free LRU queue
    
    _Atomic size_t current_memory;
    _Atomic size_t entry_count;
    size_t max_memory;
    
    _Atomic uint8_t purging;             // Purge in progress flag
} lockfree_lru_shard_t;

// Top-level cache
typedef struct {
    lockfree_lru_shard_t* shards;
    uint16_t num_shards;
    size_t total_max_memory;
} lockfree_lru_cache_t;
```

### Operations

| Operation | Locks | Notes |
|-----------|-------|-------|
| `lockfree_lru_cache_get()` | None | Lock-free: hashmap read + CAS loop + queue enqueue |
| `lockfree_lru_cache_put()` | None* | Lock-free putIfAbsent + lock-free queue enqueue |
| `lockfree_lru_cache_delete()` | Stripe lock | Brief lock for hashmap removal |
| `lockfree_lru_cache_purge()` | Atomic flag | Single purge thread, doesn't block cache ops |
| `lockfree_lru_cache_evict()` | None | Lock-free queue dequeue |

*Note: `put` uses the concurrent hashmap's `put_if_absent` which has a stripe lock, but this is brief and striped for low contention.

### Get Operation (Lock-Free)

```c
identifier_t* lockfree_lru_cache_get(lockfree_lru_cache_t* lru, path_t* path) {
    // 1. Find shard (hash-based, no lock)
    size_t shard_idx = get_shard_index(lru, path);
    lockfree_lru_shard_t* shard = &lru->shards[shard_idx];
    
    // 2. Lookup entry in concurrent hashmap (lock-free)
    lru_entry_t* entry = concurrent_hashmap_get(&shard->map, path);
    if (entry == NULL) return NULL;  // Cache miss
    
    // 3. Read current node pointer atomically
    lru_node_t* current_node = atomic_load(&entry->node);
    if (current_node == NULL) return NULL;  // Entry being purged
    
    // 4. Create new node for MRU position
    lru_node_t* new_node = memory_pool_alloc(sizeof(lru_node_t));
    if (new_node == NULL) {
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
            memory_pool_free(new_node, sizeof(lru_node_t));
            return NULL;  // Entry was purged
        }
    }
    
    // 6. Mark old node as hole (lock-free)
    atomic_store(&current_node->entry, NULL);
    atomic_fetch_add(&shard->queue.hole_count, 1);
    
    // 7. Enqueue new node at tail (MRU) - LOCK-FREE
    lru_enqueue(&shard->queue, new_node);
    
    // 8. Return reference-counted value
    return (identifier_t*)refcounter_reference((refcounter_t*)entry->value);
}
```

### Michael-Scott Queue Operations

**Enqueue (Lock-Free):**
```c
void lru_enqueue(lru_queue_t* queue, lru_node_t* node) {
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

**Dequeue for Eviction (Lock-Free):**
```c
lru_node_t* lru_dequeue(lru_queue_t* queue) {
    while (1) {
        lru_node_t* head = atomic_load(&queue->head);
        lru_node_t* tail = atomic_load(&queue->tail);
        lru_node_t* next = atomic_load(&head->next);
        
        if (head == atomic_load(&queue->head)) {
            if (head == tail) {
                if (next == NULL) return NULL;  // Queue empty
                atomic_compare_exchange_weak(&queue->tail, &tail, next);
            } else {
                lru_entry_t* entry = atomic_load(&next->entry);
                if (entry != NULL) {
                    // Non-hole entry, try to claim
                    if (atomic_compare_exchange_strong(&queue->head, &head, next)) {
                        return head;  // Return the dummy/old head
                    }
                } else {
                    // Hole, advance past it
                    atomic_compare_exchange_weak(&queue->head, &head, next);
                    atomic_fetch_sub(&queue->hole_count, 1);
                }
            }
        }
    }
}
```

### Eviction Logic

```c
static int evict_lru_entry(lockfree_lru_shard_t* shard) {
    // Find first non-hole entry from head
    while (atomic_load(&shard->queue.hole_count) > 0 || 
           atomic_load(&shard->queue.node_count) > 0) {
        
        lru_node_t* old_head = lru_dequeue(&shard->queue);
        if (old_head == NULL) return 0;  // Queue empty
        
        // Try to find a non-hole entry
        lru_node_t* head = atomic_load(&shard->queue.head);
        lru_node_t* next = atomic_load(&head->next);
        
        if (next == NULL) return 0;
        
        lru_entry_t* entry = atomic_load(&next->entry);
        
        if (entry != NULL) {
            // Claim this entry for eviction
            if (atomic_compare_exchange_strong(&next->entry, &entry, NULL)) {
                // Remove from hashmap
                concurrent_hashmap_remove(&shard->map, entry->path);
                
                // Update memory tracking
                atomic_fetch_sub(&shard->current_memory, entry->memory_size);
                atomic_fetch_sub(&shard->entry_count, 1);
                
                // Free entry
                path_destroy(entry->path);
                identifier_destroy(entry->value);
                memory_pool_free(entry, sizeof(lru_entry_t));
                
                // Mark node as hole (will be cleaned by purge)
                atomic_fetch_add(&shard->queue.hole_count, 1);
                
                return 1;  // Eviction successful
            }
        }
    }
    
    return 0;  // No entries to evict
}
```

### Purge Operation

```c
void lockfree_lru_cache_purge(lockfree_lru_cache_t* lru) {
    for (size_t i = 0; i < lru->num_shards; i++) {
        lockfree_lru_shard_t* shard = &lru->shards[i];
        
        // Try to claim purge ownership
        uint8_t expected = 0;
        if (!atomic_compare_exchange_strong(&shard->purging, &expected, 1)) {
            continue;  // Another thread is purging
        }
        
        // Drain holes from head of queue
        size_t holes_drained = 0;
        while (atomic_load(&shard->queue.hole_count) > 0 && 
               holes_drained < MAX_PURGE_BATCH) {
            
            lru_node_t* head = atomic_load(&shard->queue.head);
            lru_node_t* next = atomic_load(&head->next);
            
            if (next == NULL) break;
            
            lru_entry_t* entry = atomic_load(&next->entry);
            if (entry == NULL) {
                // This is a hole, advance head
                if (atomic_compare_exchange_strong(&shard->queue.head, &head, next)) {
                    memory_pool_free(head, sizeof(lru_node_t));
                    atomic_fetch_sub(&shard->queue.hole_count, 1);
                    holes_drained++;
                }
            } else {
                break;  // Non-hole entry, stop draining
            }
        }
        
        atomic_store(&shard->purging, 0);
    }
}
```

### Files

| File | Purpose |
|------|---------|
| `src/Database/lockfree_lru.h` | Header with data structures and API |
| `src/Database/lockfree_lru.c` | Implementation |
| `tests/test_lockfree_lru.cpp` | Unit tests |

---

## Phase 3: Integration

**Goal:** Replace existing LRU with lock-free implementation and validate performance.

### Database Integration

```c
// In database.h
typedef struct database_t {
    // ... existing fields ...
    
    // LRU cache
    lockfree_lru_cache_t* lru;  // New lock-free implementation
    
    // ... existing fields ...
} database_t;
```

### API Compatibility

The new API maintains compatibility with the existing LRU interface:

```c
// Old API (sharded LRU)
identifier_t* database_lru_cache_get(database_lru_cache_t* lru, path_t* path);
identifier_t* database_lru_cache_put(database_lru_cache_t* lru, path_t* path, identifier_t* value);
void database_lru_cache_delete(database_lru_cache_t* lru, path_t* path);

// New API (lock-free LRU) - same signatures
identifier_t* lockfree_lru_cache_get(lockfree_lru_cache_t* lru, path_t* path);
identifier_t* lockfree_lru_cache_put(lockfree_lru_cache_t* lru, path_t* path, identifier_t* value);
void lockfree_lru_cache_delete(lockfree_lru_cache_t* lru, path_t* path);
```

### Memory Pool Integration

Both `lru_node_t` (~24 bytes) and `lru_entry_t` (~56 bytes) fit in the **small** memory pool class (64 bytes):

```c
// Node allocation
lru_node_t* node = memory_pool_alloc(sizeof(lru_node_t));  // Fits in SMALL pool

// Entry allocation  
lru_entry_t* entry = memory_pool_alloc(sizeof(lru_entry_t));  // Fits in SMALL pool

// Deallocation
memory_pool_free(node, sizeof(lru_node_t));
memory_pool_free(entry, sizeof(lru_entry_t));
```

---

## Testing Strategy

### Phase 1: Concurrent Hashmap Tests

```cpp
TEST(ConcurrentHashmapTest, PutGet);
TEST(ConcurrentHashmapTest, PutIfAbsent);
TEST(ConcurrentHashmapTest, Remove);
TEST(ConcurrentHashmapTest, ConcurrentReads);
TEST(ConcurrentHashmapTest, ConcurrentWrites);
TEST(ConcurrentHashmapTest, ConcurrentReadWrite);
TEST(ConcurrentHashmapTest, HighContention);
TEST(ConcurrentHashmapTest, Resize);
TEST(ConcurrentHashmapTest, TombstoneCleanup);
```

### Phase 2: Lock-Free LRU Tests

```cpp
TEST(LockfreeLRUTest, PutGet);
TEST(LockfreeLRUTest, PutIfAbsent);
TEST(LockfreeLRUTest, Eviction);
TEST(LockfreeLRUTest, LRUOrdering);
TEST(LockfreeLRUTest, ApproximateLRU);
TEST(LockfreeLRUTest, HoleCreation);
TEST(LockfreeLRUTest, HolePurge);
TEST(LockfreeLRUTest, ConcurrentGets);
TEST(LockfreeLRUTest, ConcurrentPutGets);
TEST(LockfreeLRUTest, ConcurrentPuts);
TEST(LockfreeLRUTest, ConcurrentPurge);
TEST(LockfreeLRUTest, MemoryTracking);
TEST(LockfreeLRUTest, HighContention);
TEST(LockfreeLRUTest, LongRunning);
TEST(LockfreeLRUTest, BenchmarkVsSharded);
```

### Phase 3: Integration Tests

```cpp
TEST(DatabaseTest, LockfreeLRUIntegration);
TEST(DatabaseTest, ThroughputBenchmark);
TEST(DatabaseTest, LatencyBenchmark);
TEST(DatabaseTest, ConcurrentAccessStress);
TEST(DatabaseTest, MemoryPoolUtilization);
```

---

## Performance Expectations

### Before (Sharded LRU)

| Metric | Value |
|--------|-------|
| Read throughput | ~26,000 ops/sec (with lock contention) |
| Write throughput | Similar to read |
| Lock contention | Per-shard mutex, still significant under high concurrency |
| Memory tracking | Accurate per-entry |

### After (Lock-Free LRU)

| Metric | Value |
|--------|-------|
| Read throughput | ~100,000-1,000,000 ops/sec (lock-free) |
| Write throughput | Similar to before (striped locks, brief) |
| Lock contention | None for reads, striped for writes |
| Memory tracking | Atomic counters, approximate but accurate |

### Expected Improvement

- **Read throughput**: 4-40x improvement (lock-free vs sharded locks)
- **Write throughput**: Maintained or slight improvement
- **Scalability**: Linear with core count (no read contention)
- **Memory overhead**: Similar (memory pool reduces fragmentation)

---

## Risks and Mitigations

| Risk | Mitigation |
|------|------------|
| ABA problem in CAS loops | Use version counters or double-width CAS |
| Memory pool exhaustion | Fallback to malloc, monitor pool utilization |
| Hole backlog during high traffic | Throttle purge frequency, monitor hole count |
| CAS starvation on highly contended keys | Exponential backoff, sharding reduces contention |
| Integration issues with existing code | Maintain API compatibility, comprehensive tests |

---

## Implementation Order

1. **Phase 1: Concurrent Hashmap** (2-3 days)
   - Implement data structures
   - Implement lock-free reads
   - Implement striped writes
   - Write unit tests
   - Benchmark standalone

2. **Phase 2: Lock-Free LRU** (3-4 days)
   - Implement Michael-Scott queue
   - Implement lock-free LRU operations
   - Implement hole mechanism
   - Write unit tests
   - Benchmark standalone

3. **Phase 3: Integration** (1-2 days)
   - Replace database_lru with lockfree_lru
   - Update database.c
   - Integration tests
   - Performance benchmarks
   - Compare before/after

---

## References

- [Michael-Scott Lock-Free Queue Paper](https://cs.rochester.edu/~scott/papers/1996_PODC_queues.pdf)
- [eBay High-Throughput Thread-Safe LRU Caching](https://innovation.ebayinc.com/stories/high-throughput-thread-safe-lru-caching/)
- [Java ConcurrentLinkedQueue](https://docs.oracle.com/javase/8/docs/api/java/util/concurrent/ConcurrentLinkedQueue.html)

---

## Success Criteria

1. **Correctness**
   - All unit tests pass
   - All integration tests pass
   - No memory leaks (valgrind/ASAN)
   - Thread sanitization passes (TSAN)

2. **Performance**
   - Read throughput ≥ 4x baseline
   - Write throughput ≥ baseline
   - No lock contention on reads (perf profiling)
   - Linear scaling up to 16 threads

3. **Maintainability**
   - Clear API documentation
   - Comprehensive test coverage
   - Performance monitoring hooks