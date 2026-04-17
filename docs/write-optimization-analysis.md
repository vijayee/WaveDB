# Write Optimization Analysis

**Date:** 2026-04-16  
**Branch:** write-optimization  
**Baseline:** Put ~63K ops/sec (sync), Get ~743K ops/sec (sync)

## Profiling Results

`perf record` of `benchmark_database_sync` (10K puts), self-time breakdown:

| Category | % Total | Key Functions |
|----------|---------|---------------|
| malloc/free | ~21% | `_int_malloc`, `_int_free`, `__libc_calloc` |
| WAL I/O | ~18% | `writev`, `write`, `fstat` syscalls, `wal_crc32` |
| refcount | ~13% | `refcounter_init`, `refcounter_dereference`, `refcounter_count` |
| CBOR encoding | ~13% | `cbor_typeof`, `cbor_serialize_alloc`, `cbor_decref` |
| path/identifier ops | ~11% | `identifier_create`, `identifier_destroy`, `path_create`, `hash_path` |
| mutex locking | ~8% | `pthread_mutex_lock`, `pthread_mutex_unlock` |
| hashmap | ~7% | `hashmap_hash_default`, `hashmap_entry_find` |
| logging | ~7% | `__vfprintf_internal` (debug/WAL logging) |
| hbtrie traversal | ~6% | `hbtrie_insert`, `bnode_search`, `inline_key_compare` |
| memcpy/memcmp | ~4% | `__memcmp_avx2_movbe`, `memcpy` |
| LRU cache | <1% | `database_lru_cache_put` |

## Why Writes Are Slow: Root Cause Analysis

The write path in `database_put_sync` holds a sharded mutex for the entire `hbtrie_insert` call, including all memory allocations, B+tree splits, and lock crabbing. Reads use lock-free seqlocks. This asymmetry means:

1. **Mutex held too long** — Every write to the same shard serializes for the full duration of trie traversal + mutation + allocation + potential split cascade
2. **CBOR serialization per write** — Every WAL entry is CBOR-encoded, adding parsing overhead
3. **Per-object refcount** — Every new node, version entry, and chunk share does an atomic CAS loop
4. **Per-operation WAL write** — `writev()` syscall per operation, no batching at the I/O level
5. **Deep path copy for LRU** — Every put allocates a full copy of the path for cache insertion

## YottaDB Comparison

YottaDB (GT.M) achieves high write throughput through techniques WaveDB doesn't use:

### Two-Phase Commit with Minimal Crit Hold Time
YottaDB's critical section only does phase 1: mark buffers dirty, increment transaction number. Phase 2 (actual buffer content updates) happens *outside* crit. This reduces crit hold time by ~70%.

### Optimistic Concurrency with Restarts
Readers proceed without locks. At commit time, all read buffers are validated against cycle counters. If validation fails, the transaction restarts (up to 32 times). WaveDB already uses MVCC for reads but still uses per-node write locks.

### Buffer-Level Latching
YottaDB uses fine-grained three-state latches (clear/set/conflict) on individual buffers rather than region-level mutexes. Readers use interlocked increment on `read_in_progress` — no mutex at all.

### Twin Buffers
When crit needs to update a buffer being written to disk, it creates a "twin" buffer rather than waiting. The old buffer is invalidated and the new one carries the update.

### Adaptive Mutex
YottaDB monitors contention ratio and switches between spinlocks and pthread mutexes at runtime. Under low contention, spinlocks are faster (no syscall). Under high contention, pthread mutexes avoid wasting CPU.

### Fixed-Format Journal Entries
YottaDB uses fixed-format binary journal entries with direct field offsets instead of CBOR. This eliminates parsing overhead entirely.

### Circular Journal Buffer with Batched Writes
Journal entries accumulate in a circular in-memory buffer and are flushed in batches via `writev()` with multiple iovecs, reducing syscall overhead.

## Recommendations (Priority Order)

### Priority 1: Two-Phase Write (HIGH IMPACT, HIGH EFFORT)

Move trie modifications outside the write mutex. The write mutex (`db->write_locks[shard]`) is held for the entire `hbtrie_insert` call.

**YottaDB's approach**: Only hold crit for phase 1 (reservation/mark dirty), then do actual mutations outside crit.

**WaveDB adaptation**:
- Phase 1 (under lock): Walk the trie, find/allocate target nodes, mark them dirty, reserve positions. Fast because we only need to reserve, not write values.
- Phase 2 (outside lock): Write actual values, update version chains.

**Expected impact**: 2-3x write throughput improvement.

**Files to modify**: `src/Database/database.c`, `src/HBTrie/hbtrie.c`, `src/HBTrie/hbtrie.h`

### Priority 2: Binary WAL Format (MEDIUM-HIGH IMPACT, MEDIUM EFFORT)

Replace CBOR serialization of WAL entries with a fixed binary format:

```
[type:1B][txn_id:24B][crc32:4B][path_len:4B][path:variable][value_len:4B][value:variable]
```

The WAL header is already fixed-format. Only the payload (path + value) is CBOR-encoded. Encoding the path as `[num_identifiers:2B][id_len:2B][id_data:variable]...` and the value as `[len:4B][data:variable]` eliminates CBOR parsing.

**Expected impact**: ~15% write improvement (eliminates ~13% CBOR overhead).

**Files to modify**: `src/Database/wal.c`, `src/Database/wal_manager.c`, `src/Database/database.c`

### Priority 3: Compile Out Debug Logging (LOW-MEDIUM IMPACT, LOW EFFORT)

~7% of write time is in `__vfprintf_internal` from `log_info`/`log_debug` calls on every write. These should be compiled out in release builds or use level-gated macros that short-circuit without calling `vfprintf`.

**Expected impact**: ~7% improvement, trivially.

**Files to modify**: `src/Util/log.h`, all files with `log_info`/`log_debug` calls in hot paths.

### Priority 4: Batch WAL writev (MEDIUM IMPACT, MEDIUM EFFORT)

Currently every `database_put_sync` calls `thread_wal_write` which does `writev()` per operation. Accumulate 4-8 entries before calling `writev()` with an iovec array.

**YottaDB's approach**: Circular in-memory journal buffer (`jnl_buff`) with `dskaddr`/`freeaddr`/`fsync_dskaddr` tracking positions. `jnl_sub_qio_start()` handles batched writes.

**WaveDB adaptation**: Add a per-thread WAL buffer that accumulates entries. Flush when:
- Buffer is full (e.g., 4KB)
- `WAL_SYNC_IMMEDIATE` mode
- `database_destroy()` or `database_snapshot()`
- Batch boundary in `database_write_batch_sync`

**Expected impact**: ~10-15% write improvement (reduces syscall overhead from ~14% of write time).

**Files to modify**: `src/Database/wal.c`, `src/Database/wal_manager.c`, `src/Database/database.c`

### Priority 5: Eliminate Path Deep Copy for LRU (MEDIUM IMPACT, MEDIUM EFFORT)

`path_copy()` in `database_lru_cache_put` allocates a full copy of every identifier and chunk in the path. Instead, store the path in the LRU by reference (increment refcount) and destroy it on eviction.

**Current flow**: `path_copy(path)` → deep alloc → `database_lru_cache_put()` → on eviction: `path_destroy()`

**Proposed flow**: `refcounter_reference(&path->refcounter)` → store in LRU → on eviction: `path_destroy()` (just decrements refcount)

**Expected impact**: ~10% improvement (eliminates deep copy allocation/deallocation).

**Files to modify**: `src/Database/database.c`, `src/Database/database_lru.c`, `src/HBTrie/path.h`

### Priority 6: Replace hbtrie_node Write Lock with Spinlock (MEDIUM IMPACT, MEDIUM EFFORT)

`hbtrie_node_t->write_lock` is a `pthread_mutex_t`. Each level of trie traversal acquires/releases it (lock crabbing). Under contention, this means multiple syscall-level lock operations per write.

Replace with a spinlock or adaptive latch:
```c
typedef struct {
    _Atomic uint8_t state;  // 0=unlocked, 1=exclusive
} spinlock_t;

static inline void spinlock_lock(spinlock_t* l) {
    while (__atomic_exchange_n(&l->state, 1, __ATOMIC_ACQUIRE)) {
        #if defined(__x86_64__) || defined(__i386__)
        __builtin_ia32_pause();
        #elif defined(__aarch64__)
        __asm__ __volatile__("yield");
        #endif
    }
}

static inline void spinlock_unlock(spinlock_t* l) {
    __atomic_store_n(&l->state, 0, __ATOMIC_RELEASE);
}
```

YottaDB uses `MUTEX_HARD_SPIN_COUNT=128` before falling back to semaphores.

**Expected impact**: ~5-8% improvement (reduces mutex overhead from ~8% to near-zero).

**Files to modify**: `src/HBTrie/hbtrie.h`, `src/HBTrie/hbtrie.c`, `src/Util/platform.h`

### Priority 7: Skip Refcount for Single-Owner Objects (MEDIUM IMPACT, MEDIUM EFFORT)

~13% of write time is in `refcounter_init`, `refcounter_dereference`, and `refcounter_count`. Many objects are created with a single owner and only acquire shared ownership later. During creation, atomic initialization is wasted:

```c
// Current: always atomic init
refcounter_init(&node->refcounter);  // atomic CAS loop

// Proposed: for single-owner creation, just set count = 1
node->refcounter.count = 1;  // plain write, no atomic op
// First shared ownership transfer does the first atomic increment
```

Also: `hbtrie_node_create` always creates a `bnode_t` alongside it. Pre-allocate them as a combined pool object since they're always created and destroyed together.

**Expected impact**: ~5-8% improvement.

**Files to modify**: `src/RefCounter/refcounter.h`, `src/RefCounter/refcounter.c`, `src/HBTrie/hbtrie.c`, `src/Util/memory_pool.h`

### Priority 8: Pre-allocate hbtrie_node + bnode as Combined Pool Object (LOW IMPACT, LOW EFFORT)

`hbtrie_node_create` allocates `hbtrie_node_t` and `bnode_t` separately via `memory_pool_alloc`. Since they're always created and destroyed together, allocate them as a single contiguous block:

```c
typedef struct {
    hbtrie_node_t node;
    bnode_t bnode;
} hbtrie_combined_t;
```

This reduces 2 allocations to 1 and improves cache locality.

**Expected impact**: ~5% improvement.

**Files to modify**: `src/HBTrie/hbtrie.c`, `src/Util/memory_pool.h`

## Implementation Order

Quick wins first, then architectural changes:

1. **Compile out debug logging** — 1 line macro change, ~7% gain
2. **Skip refcount for single-owner creation** — Small change to refcounter.h, ~5-8% gain
3. **Pre-allocate hbtrie_node + bnode** — Small change, ~5% gain
4. **Binary WAL format** — Medium effort, ~15% gain
5. **Batch WAL writev** — Medium effort, ~10-15% gain
6. **Eliminate path deep copy for LRU** — Medium effort, ~10% gain
7. **Replace write_lock with spinlock** — Medium effort, ~5-8% gain
8. **Two-phase write** — High effort, 2-3x gain

Items 1-3 can be done in a day. Items 4-6 take 2-3 days each. Item 8 is a 1-2 week project.

## Cumulative Impact Estimate

| After | Estimated Put Throughput |
|-------|------------------------|
| Baseline | ~63K ops/sec |
| Items 1-3 (quick wins) | ~80K ops/sec |
| + Item 4 (binary WAL) | ~92K ops/sec |
| + Item 5 (batch WAL) | ~105K ops/sec |
| + Item 6 (path ref) | ~115K ops/sec |
| + Item 7 (spinlock) | ~125K ops/sec |
| + Item 8 (two-phase write) | ~250-350K ops/sec |

These are rough estimates. Actual gains depend on interaction effects — some improvements overlap (reducing mutex time matters less if the mutex is already less contended after other fixes).