# Write Optimization Design

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Improve WaveDB write throughput from ~63K ops/sec (single-threaded) and ~82K ops/sec (4-thread) baseline to an estimated ~250-350K ops/sec (single-threaded) and ~300-500K ops/sec (4-thread) with linear scaling.

**Architecture:** Three phases of optimization — quick wins (logging, atomic refcounts, pool allocation), WAL + LRU improvements (binary format, batch writev, refcounted paths), and concurrency improvements (spinlocks, hash-based LRU, two-phase write). Each phase is independently shippable with measurable benchmark improvements.

**Tech Stack:** C11 (atomic primitives), pthreads, Linux syscalls (writev, fdatasync)

**Baseline benchmarks:** See `.benchmarks/sync_put.json`, `.benchmarks/sync_get.json`, `.benchmarks/sync_mixed.json` and `docs/write-optimization-analysis.md` for detailed profiling data.

---

## Phase 1: Quick Wins

### 1A — Compile-Out Debug Logging

**Current state:** `log_debug()` and `log_trace()` are always compiled in. `log_log()` checks level at runtime, but the function call and argument evaluation still happen. Under single-threaded profiling, `__vfprintf_internal` accounts for ~7% of write time.

**Change:** Add compile-time level filtering to `src/Util/log.h`:

```c
#define LOG_LEVEL_TRACE 0
#define LOG_LEVEL_DEBUG 1
#define LOG_LEVEL_INFO  2
#define LOG_LEVEL_WARN  3
#define LOG_LEVEL_ERROR 4
#define LOG_LEVEL_FATAL 5

#ifndef LOG_COMPILE_LEVEL
#define LOG_COMPILE_LEVEL LOG_LEVEL_INFO
#endif

#if LOG_COMPILE_LEVEL <= LOG_LEVEL_TRACE
#define log_trace(...) log_log(LOG_TRACE, __FILE__, __LINE__, __VA_ARGS__)
#else
#define log_trace(...) ((void)0)
#endif

#if LOG_COMPILE_LEVEL <= LOG_LEVEL_DEBUG
#define log_debug(...) log_log(LOG_DEBUG, __FILE__, __LINE__, __VA_ARGS__)
#else
#define log_debug(...) ((void)0)
#endif

#if LOG_COMPILE_LEVEL <= LOG_LEVEL_INFO
#define log_info(...) log_log(LOG_INFO, __FILE__, __LINE__, __VA_ARGS__)
#else
#define log_info(...) ((void)0)
#endif

// log_warn, log_error, log_fatal always compiled in (they're rare and important)
#define log_warn(...)  log_log(LOG_WARN, __FILE__, __LINE__, __VA_ARGS__)
#define log_error(...) log_log(LOG_ERROR, __FILE__, __LINE__, __VA_ARGS__)
#define log_fatal(...) log_log(LOG_FATAL, __FILE__, __LINE__, __VA_ARGS__)
```

Default `LOG_COMPILE_LEVEL` is `LOG_LEVEL_INFO`, which eliminates all `log_trace` and `log_debug` calls from the binary. Hot-path files (database.c, hbtrie.c, wal.c, wal_manager.c) call `log_debug` frequently.

Build systems can override with `-DLOG_COMPILE_LEVEL=0` for debug builds.

**Impact:** ~7% single-threaded write improvement. Also eliminates lock contention from `log_log`'s user-settable lock callback on the write path.

**Files:** `src/Util/log.h`

**Testing:** Existing unit tests pass. Debug builds use `-DLOG_COMPILE_LEVEL=0` to keep trace/debug output.

### 1B — Make REFCOUNTER_ATOMIC the Default

**Current state:** `refcounter_t` has two modes: `REFCOUNTER_ATOMIC` (lock-free, using C11 `_Atomic`) and non-atomic (mutex per object). Non-atomic is the default. Every `refcounter_reference` and `refcounter_dereference` in non-atomic mode acquires/releases a `pthread_mutex_t` — this accounts for 13% of single-threaded write time and 9% of concurrent write time.

**Change:** Remove the `#ifdef REFCOUNTER_ATOMIC` conditional. Make the atomic implementation the only implementation:

```c
typedef struct refcounter_t {
    _Atomic uint_fast16_t count;
    _Atomic uint_fast8_t yield;
} refcounter_t;
```

Remove:
- The `PLATFORMLOCKTYPE(lock)` field
- The non-atomic code paths in `refcounter_init`, `refcounter_reference`, `refcounter_dereference`, `refcounter_yield`, `refcounter_consume`, `refcounter_count`
- The `refcounter_destroy_lock` function (no-op in atomic mode, now unnecessary)

Keep:
- The `REFERENCE`, `DEREFERENCE`, `CONSUME` macros — they still work, just calling the atomic versions
- The `yield` mechanism for ownership transfer — still valuable for the CONSUME pattern

`refcounter_init` becomes a single `atomic_store(&rc->count, 1)` + `atomic_store(&rc->yield, 0)`. This makes Priority 7 from the original analysis (skip refcount for single-owner) unnecessary — atomic init is already cheap.

The `refcounter_t` shrinks from 48 bytes (mutex + padding) to 4 bytes (two atomic fields). This reduces memory footprint for every refcounted object (path_t, identifier_t, hbtrie_node_t, etc.).

**Impact:** ~13% single-threaded, ~9% concurrent write improvement. Reduces refcounter_t from 48 bytes to 4 bytes per object.

**Files:** `src/RefCounter/refcounter.h`, `src/RefCounter/refcounter.c`

**Testing:** Existing unit tests pass. The atomic and non-atomic paths had identical semantics (they were already tested interchangeably via the `#ifdef`). Removing the conditional doesn't change behavior.

### 1C — Combined Pool Allocation for hbtrie_node + bnode

**Current state:** `hbtrie_node_create` allocates `hbtrie_node_t` and `bnode_t` separately via `memory_pool_alloc`. Since they're always created and destroyed together, this is 2 allocations + 2 frees per trie node creation.

**Change:** Define a combined struct and allocate once:

```c
typedef struct {
    hbtrie_node_t node;
    bnode_t bnode;
} hbtrie_combined_t;
```

`hbtrie_node_create` allocates `hbtrie_combined_t` via `memory_pool_alloc(sizeof(hbtrie_combined_t))`. The `bnode` pointer in `hbtrie_node_t` points to `&combined->bnode`. `hbtrie_node_destroy` frees the combined allocation.

Since `hbtrie_combined_t` is likely ~120 bytes (hbtrie_node_t ~80 + bnode_t ~40), it fits in the memory pool's LARGE class (257-1024 bytes). If the pool is exhausted, it falls back to `malloc` as before.

`hbtrie_node_t` needs a new field or a different approach to find the combined allocation for destruction. Two options:

1. **Embed a pointer**: Add `hbtrie_combined_t* combined` to `hbtrie_node_t`, set to `&combined` on creation, used for freeing.
2. **Container macro**: Use `container_of` to go from `hbtrie_node_t*` to `hbtrie_combined_t*`:

```c
#define hbtrie_combined_from_node(node_ptr) \
    container_of(node_ptr, hbtrie_combined_t, node)
```

Option 2 is preferred — no extra pointer, uses the existing `container_of` pattern.

**Impact:** ~5% improvement. Reduces allocation overhead and improves cache locality (node and bnode are adjacent in memory).

**Files:** `src/HBTrie/hbtrie.h`, `src/HBTrie/hbtrie.c`, `src/Util/memory_pool.h` (add `container_of` macro if not present)

**Testing:** Existing hbtrie tests pass. `hbtrie_node_create` / `hbtrie_node_destroy` are well-tested.

---

## Phase 2: WAL + LRU

### 2A — Binary WAL Format

**Current state:** Every WAL entry CBOR-encodes the path and value. CBOR parsing accounts for ~12-13% of write time in both single-threaded and concurrent profiles. The WAL header is already fixed-format (33 bytes: type + txn_id + crc32 + data_len), but the payload is CBOR-encoded.

**Change:** Replace CBOR payload encoding with a fixed binary format. All multi-byte integers use **big-endian (network byte order)** for cross-platform portability — a database created on a little-endian x86 machine must be readable on a big-endian ARM server.

**Binary payload format:**

```
Header (33 bytes, unchanged):
  [type:1B][txn_id:24B][crc32:4B][data_len:4B]

Payload (new binary format, replaces CBOR):
  [path_count:2B BE]           — number of path identifiers
  [path_len:4B BE]             — total path byte length (for validation)
  For each identifier:
    [id_len:2B BE]             — byte length of this identifier
    [id_data:id_len bytes]      — raw bytes (no padding, no CBOR overhead)
  [value_len:4B BE]            — byte length of value
  [value:value_len bytes]       — raw value bytes
```

**Endianness:** All multi-byte integers are stored big-endian. On little-endian machines, `htobe16`/`htobe32` convert on write and `be16toh`/`be32toh` convert on read. These are single instructions on modern CPUs (`bswap`). On big-endian machines, these functions are no-ops.

A new file `src/Util/endian.h` provides portable wrappers:

```c
#ifndef WAVEDB_ENDIAN_H
#define WAVEDB_ENDIAN_H

#include <stdint.h>
#include <arpa/inet.h>  // or <sys/endian.h> on some platforms

// 16-bit
static inline uint16_t write_be16(uint8_t* buf, uint16_t val) {
    buf[0] = (val >> 8) & 0xFF;
    buf[1] = val & 0xFF;
    return val;
}

static inline uint16_t read_be16(const uint8_t* buf) {
    return ((uint16_t)buf[0] << 8) | buf[1];
}

// 32-bit
static inline uint32_t write_be32(uint8_t* buf, uint32_t val) {
    buf[0] = (val >> 24) & 0xFF;
    buf[1] = (val >> 16) & 0xFF;
    buf[2] = (val >> 8) & 0xFF;
    buf[3] = val & 0xFF;
    return val;
}

static inline uint32_t read_be32(const uint8_t* buf) {
    return ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16) |
           ((uint32_t)buf[2] << 8) | buf[3];
}

#endif // WAVEDB_ENDIAN_H
```

This avoids platform-specific header issues — the byte-by-byte approach works everywhere.

**Migration:** The WAL format version is stored in the WAL manifest (`thread_wal_manifest_t`). On open, if the version indicates CBOR, decode with the old path. On write, always use the new binary format. After compaction, all WALs are in the new format.

**CRC:** The existing `crc32` in the header covers the entire entry (header + payload). For the binary format, CRC is computed over the raw encoded bytes — same as now, just over binary payload instead of CBOR.

**Impact:** ~15% write improvement (eliminates CBOR overhead).

**Files:** `src/Database/wal.c`, `src/Database/wal_manager.c`, `src/Database/database.c` (encode_put_entry), `src/Database/wal.h` (add format version), `src/Util/endian.h` (new)

**Testing:** WAL round-trip tests: write entries in binary format, read them back, verify path and value match. Migration test: open a CBOR-format WAL, read entries, write new entries in binary format.

### 2B — Batch WAL writev with One-Shot Timer

**Current state:** Every `database_put_sync` calls `thread_wal_write` which acquires `twal->lock`, does CRC + `writev()`, and calls `debouncer_debounce()` — which cancels and reschedules the fsync timer on every write (2 lock acquisitions per write on `wheel->lock`).

**Change:** Replace the debouncer with a per-thread WAL write buffer and a one-shot timer:

1. **Per-thread write buffer**: `thread_wal_t` gets a pre-allocated buffer for accumulating entries:
```c
typedef struct {
    // ... existing fields ...
    uint8_t entry_buf[4096];      // pre-allocated entry buffer
    size_t entry_buf_used;         // bytes used in entry_buf
    uint8_t batch_count;           // entries accumulated in current batch
    uint8_t batch_size;            // max entries before flush (default: 4)
    uint64_t first_entry_time_ms;  // timestamp of first entry in batch
    int timer_active;             // 1 if one-shot timer is pending
} thread_wal_t;
```

2. **Accumulation (minimal locking)**: `thread_wal_write` copies the entry into the thread-local buffer. Since `thread_wal_t` is per-thread, only one write thread accesses the buffer at a time for normal writes. However, the timer callback also accesses `batch_count`, so the timer callback acquires `twal->lock` before reading it. The `twal->lock` is only acquired by the write thread during buffer-full flush, and by the timer callback during timeout flush — both are infrequent compared to per-write accumulation.

3. **Flush triggers**:
   - **Buffer full**: `batch_count >= batch_size` (4 entries by default) → flush immediately
   - **Debounce timeout**: When buffer transitions from empty to non-empty, set a one-shot timer for `debounce_ms`. Timer callback acquires `twal->lock`, checks `batch_count > 0`, and flushes if entries are buffered. **No cancel_timer calls** — if the buffer was already flushed when the timer fires, the callback is a no-op.
   - **IMMEDIATE mode**: `batch_size = 1`, flush after every write
   - **`database_destroy()` / `database_snapshot()`**: Flush all buffers before proceeding

4. **Timer lifecycle**:
   ```c
   void wal_flush_timer_callback(void* ctx) {
       thread_wal_t* twal = (thread_wal_t*)ctx;
       platform_lock(&twal->lock);
       if (twal->batch_count > 0) {
           thread_wal_flush_locked(twal);
       }
       twal->timer_active = 0;
       platform_unlock(&twal->lock);
   }

   void thread_wal_write(thread_wal_t* twal, ...) {
       // Copy entry to thread-local buffer (no lock)
       memcpy(twal->entry_buf + twal->entry_buf_used, entry, entry_size);
       twal->entry_buf_used += entry_size;
       twal->batch_count++;

       if (twal->batch_count >= twal->batch_size) {
           // Buffer full — flush immediately
           platform_lock(&twal->lock);
           thread_wal_flush_locked(twal);
           platform_unlock(&twal->lock);
       } else if (twal->batch_count == 1) {
           // First entry — start one-shot timer
           twal->timer_active = 1;
           hierarchical_timing_wheel_set_timer(twal->wheel, twal,
               wal_flush_timer_callback, NULL,
               (timer_duration_t){.milliseconds = twal->debounce_ms});
       }
       // else: accumulating, timer already set
   }
   ```

5. **Remove debouncer dependency**: The `debouncer_t` and its `debouncer_debounce` / `debouncer_flush` calls are removed from the WAL write path. The `debouncer_destroy` in cleanup becomes `timer_active` check + cancel if needed.

**Impact:** ~10-15% write improvement. Eliminates `wheel->lock` contention (was 2% under concurrent profile). Reduces `writev` syscalls from 1-per-write to 1-per-4-writes.

**Files:** `src/Database/wal_manager.c`, `src/Database/wal_manager.h`, `src/Database/database.c`, `src/Time/debouncer.c` (remove from WAL path), `src/Time/debouncer.h`

**Testing:** Existing WAL tests. Add test for buffer accumulation: write 3 entries, verify they're not flushed yet, write 4th entry, verify all 4 are flushed. Add test for timer flush: write 1 entry, wait for debounce_ms, verify it's flushed. Add test for IMMEDIATE mode: verify flush after every write.

### 2C — Eliminate Path Deep Copy for LRU

**Current state:** `database_lru_cache_put` calls `path_copy()` for the hashmap key, deep-copying every identifier and chunk. Under concurrent load, `path_compare` + `identifier_compare` account for 4% of CPU time.

**Change:** Use reference counting instead of deep copy. `path_t` already has a `refcounter_t` as its first member.

1. **Add `path_reference()` function**:
```c
path_t* path_reference(path_t* path) {
    if (path == NULL) return NULL;
    refcounter_reference(&path->refcounter);
    return path;
}
```

2. **LRU cache stores refcounted path pointers**: Instead of `dup_path` (which calls `path_copy`), the hashmap key allocation function calls `path_reference`. On eviction, `path_destroy()` decrements the refcount. If the path is still referenced elsewhere (e.g., by the caller), it stays alive.

3. **Add hash-based comparison**: Store `hash_path()` result alongside the key in the LRU node:
```c
typedef struct {
    path_t* key;        // refcounted path (no deep copy)
    uint64_t key_hash;  // hash_path() result, compared first
    identifier_t* value;
    size_t entry_size;  // cached, not recomputed per put
} database_lru_node_t;
```

On lookup, compare `key_hash` first (O(1) integer comparison). Only call `path_compare()` on hash collisions. This is effective because `hash_path()` uses xxhash with good distribution — collisions should be rare.

4. **Cache entry size**: Instead of calling `calculate_entry_memory()` on every put (which walks the entire path + value tree), compute the size once at insert time and store it in `database_lru_node_t.entry_size`.

**Impact:** ~10% single-threaded (eliminates deep copy), ~4% concurrent (reduces path comparison overhead).

**Files:** `src/Database/database_lru.c`, `src/Database/database_lru.h`, `src/HBTrie/path.h`, `src/HBTrie/path.c`, `src/Util/concurrent_hashmap.c`

**Testing:** Existing LRU tests. Add test for refcounted path: put into LRU, verify path refcount incremented. Destroy original path, verify LRU entry still valid. Evict entry, verify refcount decremented and path freed.

---

## Phase 3: Concurrency

### 3A — Replace hbtrie_node Write Lock with Spinlock

**Current state:** Each `hbtrie_node_t` has a `pthread_mutex_t write_lock` (40 bytes). Lock crabbing acquires child lock before releasing parent lock. Under contention, `pthread_mutex_lock` costs ~4-6% of write time (syscall + kernel context switch when contested).

**Change:** Replace `pthread_mutex_t write_lock` with an adaptive spinlock (1 byte):

```c
typedef struct {
    _Atomic uint8_t state;  // 0=unlocked, 1=exclusive
} spinlock_t;

static inline void spinlock_lock(spinlock_t* l) {
    int spins = 0;
    while (__atomic_exchange_n(&l->state, 1, __ATOMIC_ACQUIRE)) {
        spins++;
        if (spins > 128) {
            sched_yield();
            spins = 0;
        }
        cpu_relax();  // __builtin_ia32_pause() on x86, yield on ARM
    }
}

static inline void spinlock_unlock(spinlock_t* l) {
    __atomic_store_n(&l->state, 0, __ATOMIC_RELEASE);
}
```

Spin for 128 iterations, then yield. This avoids syscall overhead for the common case (uncontended or briefly contended). The `cpu_relax()` intrinsic reduces power consumption and pipeline stalls during spinning.

`hbtrie_node_t` shrinks by 39 bytes per node (40-byte mutex → 1-byte spinlock). For a trie with millions of nodes, this saves significant memory.

**`seq` field integration:** The existing `atomic_fetch_add(&current->seq, 1)` pattern for optimistic reads stays unchanged. Spinlocks protect writes; `seq` fields protect reads. They're complementary.

**Impact:** ~5-8% single-threaded, ~10% concurrent. Lock crabbing benefits most — each level descent goes from `pthread_mutex_lock` syscall to a spinlock that's typically uncontended.

**Files:** `src/HBTrie/hbtrie.h` (change `pthread_mutex_t write_lock` to `spinlock_t write_lock`), `src/HBTrie/hbtrie.c` (change `platform_lock`/`platform_unlock` to `spinlock_lock`/`spinlock_unlock`), `src/Util/platform.h` (add `spinlock_t`, `spinlock_lock`, `spinlock_unlock`, `cpu_relax`)

**Testing:** Existing hbtrie tests. Concurrent benchmark should show improved write scaling. Stress test with 32+ threads to verify spinlock doesn't livelock.

### 3B — Hash-Based LRU Lookup

**Current state:** LRU cache lookups use `path_compare()` for every entry in a hashmap bucket. Under concurrent load, this accounts for 4% of CPU time.

**Change:** This is already covered by Phase 2C — the `key_hash` field stored in `database_lru_node_t` enables hash-first comparison. The hashmap's comparison function is updated to:

```c
int lru_key_compare(const void* a, const void* b) {
    const database_lru_node_t* node_a = *(const database_lru_node_t**)a;
    const database_lru_node_t* node_b = *(const database_lru_node_t**)b;

    // Fast path: compare hashes first
    if (node_a->key_hash != node_b->key_hash) return 1;

    // Slow path: hash collision, compare paths
    return path_compare(node_a->key, node_b->key);
}
```

**Impact:** ~2-4% concurrent improvement.

**Note:** This is implemented as part of Phase 2C. Listed here for completeness in the optimization roadmap.

### 3C — Two-Phase Write

**Current state:** `database_put_sync` holds `write_locks[shard]` for the entire `hbtrie_insert` call — trie traversal, node allocation, bnode insertion, potential split cascade, version chain update. Under concurrent load, write throughput degrades past 4 threads (82K → 61K at 32 threads) because threads contend on the per-shard mutex for the full mutation duration.

**Change:** Split the write into reservation and commit phases:

**Phase 1 (under lock) — `hbtrie_reserve`:**
- Walk the trie, following chunks at each level
- Find or allocate the target node and insertion position
- Mark nodes as dirty (set `seq` to odd)
- Return a `hbtrie_reservation_t` containing the target node, insertion position, and version chain pointer
- Release the write lock

**Phase 2 (outside lock) — `hbtrie_commit`:**
- Write the actual value into the reserved position
- Update the version chain
- Set `seq` to even (stable) to signal readers
- Update the LRU cache

```c
typedef struct {
    hbtrie_node_t* target_node;
    bnode_entry_t* target_entry;
    int insert_position;    // where in the bnode to insert
    uint8_t needs_split;    // whether the bnode needs splitting
    uint8_t new_entry_type; // VALUE or CHILD
} hbtrie_reservation_t;

// New API:
hbtrie_reservation_t* hbtrie_reserve(hbtrie_t* trie, path_t* path, transaction_id_t txn_id);
int hbtrie_commit(hbtrie_t* trie, hbtrie_reservation_t* res, identifier_t* value, transaction_id_t txn_id);
void hbtrie_reservation_destroy(hbtrie_reservation_t* res);
```

**Optimistic read validation:** Readers already use MVCC seqlocks (`seq` field). After `hbtrie_commit`, the writer sets `seq` to even. If a reader sees an odd `seq`, it retries. This existing mechanism handles the race between `hbtrie_reserve` (sets `seq` odd) and concurrent readers.

**Split handling:** If `hbtrie_reserve` detects that a bnode needs splitting, it reserves space for the split cascade. `hbtrie_commit` performs the split outside the lock. This is safe because:
1. The reserved position is marked with an odd `seq` (readers will retry)
2. Other writers to the same shard are excluded by the per-shard lock during reserve
3. The split only affects the reserved node's subtree

**Impact:** 2-3x single-threaded write throughput improvement. Enables linear write scaling beyond 4 threads. The write lock hold time drops from the full mutation duration (including allocations and splits) to just the reservation walk.

**Files:** `src/Database/database.c`, `src/HBTrie/hbtrie.c`, `src/HBTrie/hbtrie.h`

**Testing:** Existing hbtrie tests (reserve + commit must produce identical results to insert). Concurrent benchmark should show linear scaling to 32+ threads. Stress test with concurrent writers to different paths (different shards) and same-path writers (same shard contention).

---

## Implementation Order

| Phase | Item | Effort | Cumulative ST Throughput |
|-------|------|--------|--------------------------|
| 1 | Compile-out logging | 1 hour | ~68K ops/sec |
| 1 | Atomic refcounts | 2-3 hours | ~80K ops/sec |
| 1 | Combined pool allocation | 1-2 hours | ~85K ops/sec |
| 2 | Binary WAL format | 1 day | ~100K ops/sec |
| 2 | Batch WAL writev + one-shot timer | 1 day | ~115K ops/sec |
| 2 | Refcounted LRU paths + hash lookup | 1 day | ~125K ops/sec |
| 3 | Spinlocks | half day | ~135K ops/sec |
| 3 | Two-phase write | 1-2 weeks | ~250-350K ops/sec |

**Concurrent (4 threads):**

| Phase | Cumulative Write Throughput |
|-------|------------------------------|
| Baseline | ~82K ops/sec (scales poorly) |
| After Phase 1 | ~100K ops/sec |
| After Phase 2 | ~150K ops/sec |
| After Phase 3 | ~300-500K ops/sec (linear scaling) |

---

## Testing Strategy

Each phase must pass:
1. All existing unit tests (`ctest --output-on-failure`)
2. The concurrent benchmark (`benchmark_database`) showing improvement over baseline
3. **No memory leaks** — run `valgrind --leak-check=full` on `test_database`, `test_hbtrie`, `test_bnode`, and `benchmark_database_quick` after each phase. Fix any leaks before proceeding.

New tests are added only for new behavior:
- Phase 1A (logging): Verify `log_debug`/`log_trace` calls are eliminated from binary with `LOG_COMPILE_LEVEL=INFO`. Run valgrind to confirm no leaks.
- Phase 1B (atomic refcounts): Run valgrind on all test suites. Pay special attention to reference count correctness — atomic operations must not leak or double-free.
- Phase 1C (combined pool): Run valgrind on `test_hbtrie` and `test_bnode`. Verify combined allocation is freed correctly on node destroy.
- Phase 2A (binary WAL): Binary WAL round-trip test, CBOR-to-binary migration test. Run valgrind on WAL read/write path.
- Phase 2B (batch writev): Buffer accumulation test, timer flush test, IMMEDIATE mode test. Run valgrind to verify no leaks in timer callback, no leaks on buffer flush, no double-frees on timer-no-op path.
- Phase 2C (refcounted LRU paths): Refcounted path lifecycle test, hash collision test. Run valgrind to verify path refcounts are balanced (no leaks from LRU eviction, no double-frees when path is both in LRU and held by caller).
- Phase 3A (spinlocks): Spinlock stress test with 32+ threads. Run valgrind on concurrent benchmark to verify no leaks.
- Phase 3C (two-phase write): Reserve + commit produces identical results to insert. Run valgrind on `test_hbtrie` with reserve/commit path. Verify reservation cleanup on error paths (no leaks if `hbtrie_commit` fails after `hbtrie_reserve`).

---

## Risks and Mitigations

| Risk | Mitigation |
|------|-----------|
| Binary WAL breaks backward compatibility | WAL version in manifest; old format readable, new format written |
| Atomic refcounts change memory layout | All code uses `REFERENCE`/`DEREFERENCE` macros; no direct field access |
| Two-phase write introduces race conditions | Existing MVCC seqlocks handle read-write races; stress test with concurrent readers and writers |
| Spinlocks waste CPU under high contention | Adaptive: spin 128 times, then yield. Monitor with concurrent benchmark at 32+ threads |
| One-shot timer fires after buffer already flushed | Timer callback checks `batch_count > 0` before flushing; idempotent no-op |
| Endianness bugs in binary WAL | Round-trip tests on both little-endian and big-endian (CI can emulate with `bswap` verification) |