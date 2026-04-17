# Write Optimization Phase 3: Concurrency

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace hbtrie_node write locks with adaptive spinlocks and implement two-phase writes to enable linear write scaling beyond 4 threads. Expected result: ~250-350K ops/sec single-threaded, ~300-500K ops/sec with 4 threads and linear scaling.

**Architecture:** Spinlocks replace per-node `pthread_mutex_t` with a 1-byte `_Atomic uint8_t` that spins 128 times before yielding. Two-phase writes split `hbtrie_insert` into `hbtrie_reserve` (under lock: walk trie, find/allocate target, mark dirty) and `hbtrie_commit` (outside lock: write value, update version chain). Hash-based LRU lookup was implemented in Phase 2 Task 4.

**Tech Stack:** C11 atomics, spinlocks, MVCC seqlocks

**Spec:** `docs/superpowers/specs/2026-04-16-write-optimization-design.md` — Phase 3

**Prerequisites:** Phase 1 (atomic refcounts) and Phase 2 must be complete.

---

## File Structure

| File | Responsibility |
|------|---------------|
| `src/Util/threadding.h` | `spinlock_t` type, `spinlock_lock`, `spinlock_unlock`, `spinlock_init`, `cpu_relax` |
| `src/HBTrie/hbtrie.h` | `spinlock_t write_lock` in `hbtrie_node_t`, `hbtrie_reservation_t`, new API declarations |
| `src/HBTrie/hbtrie.c` | `spinlock_lock`/`unlock` replacements, `hbtrie_reserve`, `hbtrie_commit` implementations |
| `src/Database/database.c` | `database_put_sync` using reserve+commit instead of insert |

---

### Task 1: Replace hbtrie_node Write Lock with Spinlock

**Files:**
- Modify: `src/Util/threadding.h`
- Modify: `src/HBTrie/hbtrie.h`
- Modify: `src/HBTrie/hbtrie.c`

- [ ] **Step 1: Add spinlock type to threadding.h**

Open `src/Util/threadding.h`. Add the spinlock type and functions after the existing lock abstractions:

```c
/* Adaptive spinlock - spins up to 128 iterations, then yields */
typedef struct {
    _Atomic uint8_t state;  /* 0=unlocked, 1=exclusive */
} spinlock_t;

static inline void spinlock_init(spinlock_t* l) {
    atomic_store(&l->state, 0);
}

static inline void spinlock_lock(spinlock_t* l) {
    int spins = 0;
    while (__atomic_exchange_n(&l->state, 1, __ATOMIC_ACQUIRE)) {
        spins++;
        if (spins > 128) {
            sched_yield();
            spins = 0;
        }
#if defined(__x86_64__) || defined(__i386__)
        __builtin_ia32_pause();
#elif defined(__aarch64__)
        __asm__ __volatile__("yield");
#else
        /* No-op on other architectures */
#endif
    }
}

static inline void spinlock_unlock(spinlock_t* l) {
    __atomic_store_n(&l->state, 0, __ATOMIC_RELEASE);
}

static inline void spinlock_destroy(spinlock_t* l) {
    (void)l; /* No-op — nothing to destroy */
}
```

- [ ] **Step 2: Replace `PLATFORMLOCKTYPE(write_lock)` with `spinlock_t` in hbtrie_node_t**

Open `src/HBTrie/hbtrie.h`. Find `hbtrie_node_t` struct (lines 42-54). Replace:

```c
PLATFORMLOCKTYPE(write_lock);      // Writer mutual exclusion
```

With:

```c
spinlock_t write_lock;             // Writer mutual exclusion (adaptive spinlock)
```

- [ ] **Step 3: Replace `platform_lock`/`platform_unlock` with spinlock calls in hbtrie.c**

Open `src/HBTrie/hbtrie.c`. Replace all occurrences of:

```c
platform_lock(&current->write_lock)
```

With:

```c
spinlock_lock(&current->write_lock)
```

And all occurrences of:

```c
platform_unlock(&current->write_lock)
```

With:

```c
spinlock_unlock(&current->write_lock)
```

Also replace `platform_lock_init(&node->write_lock)` with `spinlock_init(&node->write_lock)` in `hbtrie_node_create`.

Replace `platform_lock_destroy(&node->write_lock)` with `spinlock_destroy(&node->write_lock)` in `hbtrie_node_destroy` (if it exists — spinlock_destroy is a no-op).

- [ ] **Step 4: Update `hbtrie_combined_t` allocation in hbtrie_node_create**

Since we added `hbtrie_combined_t` in Phase 1, ensure `spinlock_init` is called on the combined allocation's `write_lock`. The `hbtrie_node_create` function should already be initializing `node->write_lock` — just make sure it uses `spinlock_init` instead of `platform_lock_init`.

- [ ] **Step 5: Build and run tests**

Run: `cd /home/victor/Workspace/src/github.com/vijayee/WaveDB/build && cmake -DCMAKE_BUILD_TYPE=Release .. && make -j$(nproc) && ctest --output-on-failure`

Expected: All tests pass. Spinlocks are functionally identical to mutexes for correctness — they just spin instead of sleeping.

- [ ] **Step 6: Run concurrent benchmark**

Run: `./build/tests/benchmark/benchmark_database 2>&1`

Verify improved write scaling at 4, 8, 16, 32 threads. Expected: ~5-10% write improvement at low thread counts, more at higher counts.

- [ ] **Step 7: Run stress test with 32+ threads**

Run the concurrent benchmark with 32 threads and verify no livelocks or deadlocks. Spinlocks should yield after 128 spins.

- [ ] **Step 8: Run valgrind leak check**

Run: `valgrind --leak-check=full --error-exitcode=1 ./build/tests/test_database 2>&1 | tail -20`

Expected: Zero leaks.

- [ ] **Step 9: Commit**

```bash
git add src/Util/threadding.h src/HBTrie/hbtrie.h src/HBTrie/hbtrie.c
git commit -m "feat: replace pthread mutex with adaptive spinlock for hbtrie_node write lock

Spinlocks spin 128 times before yielding, avoiding syscall overhead
for the common uncontended case. Reduces hbtrie_node_t size by 39
bytes per node (40-byte mutex → 1-byte spinlock). Lock crabbing
benefits most — each level descent goes from syscall to spin.
~5-8% single-threaded, ~10% concurrent improvement expected."
```

---

### Task 2: Two-Phase Write

**Files:**
- Modify: `src/HBTrie/hbtrie.h` (add `hbtrie_reservation_t`, new API)
- Modify: `src/HBTrie/hbtrie.c` (implement `hbtrie_reserve`, `hbtrie_commit`)
- Modify: `src/Database/database.c` (use reserve+commit in `database_put_sync`)

This is the most complex and highest-risk optimization. It changes the fundamental write path to hold the write lock only during trie descent/reservation, not during value writing.

- [ ] **Step 1: Define `hbtrie_reservation_t` in hbtrie.h**

Open `src/HBTrie/hbtrie.h`. Add the reservation struct and API declarations:

```c
typedef struct {
    hbtrie_node_t* target_node;     /* Node where insertion happens */
    bnode_entry_t* target_entry;    /* Entry being modified (NULL for new entry) */
    int insert_position;            /* Position in bnode for new entry */
    uint8_t needs_split;            /* Whether bnode needs splitting */
    uint8_t entry_type;             /* VALUE or CHILD */
    path_t* path;                   /* Path being inserted (referenced) */
    identifier_t* value;            /* Value being inserted (referenced) */
    transaction_id_t txn_id;        /* Transaction ID */
    uint32_t btree_node_size;       /* Btree node size for splits */
    uint8_t chunk_size;             /* Chunk size for identifier ops */
    hbtrie_t* trie;                 /* Back-reference for commit */
    int shard;                      /* Shard index for lock release */
} hbtrie_reservation_t;

/* Two-phase write API */
hbtrie_reservation_t* hbtrie_reserve(hbtrie_t* trie, path_t* path,
                                      identifier_t* value, transaction_id_t txn_id);
int hbtrie_commit(hbtrie_reservation_t* res);
void hbtrie_reservation_destroy(hbtrie_reservation_t* res);
```

- [ ] **Step 2: Implement `hbtrie_reserve` in hbtrie.c**

This function replaces the first part of `hbtrie_insert` — it walks the trie under the write lock, finds the target node and position, and returns a reservation. The key difference: it does NOT write the value or update version chains.

The implementation should:
1. Acquire the shard write lock: `spinlock_lock(&db->write_locks[shard])`
2. Walk the trie following each identifier in the path
3. Find or create the target node
4. Determine the insertion position in the bnode
5. Mark the target node as dirty (`is_dirty = 1`, `seq` set to odd)
6. Reference the path and value for later use
7. Store all reservation info in `hbtrie_reservation_t`
8. Release the shard write lock: `spinlock_unlock(&db->write_locks[shard])`

The reservation holds references (via `refcounter_reference`) to the path and value, ensuring they survive until commit.

- [ ] **Step 3: Implement `hbtrie_commit` in hbtrie.c**

This function replaces the second part of `hbtrie_insert` — it writes the value and updates version chains WITHOUT holding the write lock.

The implementation should:
1. If `needs_split`: perform the bnode split (which may trigger cascade splits)
2. Insert the value into the target bnode at `insert_position`
3. Update the version chain for MVCC
4. Set `seq` to even (stable) on all modified nodes to signal readers
5. Dereference the path and value
6. Free the reservation

This is safe because:
- The reserved positions are marked with odd `seq` (readers retry)
- Other writers to the same shard are excluded by the per-shard lock during reserve
- The commit only modifies the reserved position, not other nodes

- [ ] **Step 4: Implement `hbtrie_reservation_destroy` in hbtrie.c**

Clean up the reservation if `hbtrie_commit` fails or if we need to abort:

```c
void hbtrie_reservation_destroy(hbtrie_reservation_t* res) {
    if (res == NULL) return;
    if (res->path != NULL) path_destroy(res->path);
    if (res->value != NULL) identifier_destroy(res->value);
    // Mark any dirty nodes as stable (even seq) to unblock readers
    if (res->target_node != NULL) {
        atomic_store(&res->target_node->seq, atomic_load(&res->target_node->seq) + 1);
    }
    free(res);
}
```

- [ ] **Step 5: Update `database_put_sync` to use reserve+commit**

Open `src/Database/database.c`. Find `database_put_sync`. Replace the `hbtrie_insert` call with:

```c
// Phase 1: reserve (under write lock)
hbtrie_reservation_t* res = hbtrie_reserve(db->trie, path, value, txn->txn_id);
if (res == NULL) {
    // Reservation failed — fall back to regular insert
    platform_lock(&db->write_locks[shard]);
    result = hbtrie_insert(db->trie, path, value, txn->txn_id);
    platform_unlock(&db->write_locks[shard]);
} else {
    // Phase 2: commit (outside write lock)
    result = hbtrie_commit(res);
    if (result != 0) {
        hbtrie_reservation_destroy(res);
    }
}
```

Keep the existing `hbtrie_insert` path as a fallback for now — this allows A/B testing and regression checking.

- [ ] **Step 6: Build and run tests**

Run: `cd /home/victor/Workspace/src/github.com/vijayee/WaveDB/build && cmake -DCMAKE_BUILD_TYPE=Release .. && make -j$(nproc) && ctest --output-on-failure`

Expected: All tests pass. `hbtrie_reserve` + `hbtrie_commit` must produce identical results to `hbtrie_insert`.

- [ ] **Step 7: Run concurrent benchmark**

Run: `./build/tests/benchmark/benchmark_database 2>&1`

Expected: Linear write scaling beyond 4 threads. Write throughput should increase with thread count rather than degrading.

- [ ] **Step 8: Run valgrind leak check**

Run: `valgrind --leak-check=full --error-exitcode=1 ./build/tests/test_hbtrie 2>&1 | tail -20`

Expected: Zero leaks. Reservation cleanup on error paths must not leak.

- [ ] **Step 9: Add reserve+commit vs insert comparison test**

Add a test that:
1. Inserts N key-value pairs using `hbtrie_insert`
2. Verifies all reads return correct values
3. Destroys the trie
4. Creates a new trie
5. Inserts the same N key-value pairs using `hbtrie_reserve` + `hbtrie_commit`
6. Verifies all reads return the same values as step 2

- [ ] **Step 10: Add concurrent writer stress test**

Add a test that creates 16 threads, each writing 10,000 unique key-value pairs using `database_put_sync`. Verify that all writes succeeded and reads return correct values. This tests the two-phase write under concurrent contention.

- [ ] **Step 11: Commit**

```bash
git add src/HBTrie/hbtrie.h src/HBTrie/hbtrie.c src/Database/database.c
git commit -m "feat: implement two-phase write (reserve + commit)

Split hbtrie_insert into hbtrie_reserve (under write lock) and
hbtrie_commit (outside write lock). Reserve walks the trie and
finds the target position. Commit writes the value and updates
version chains without holding the lock. This reduces write lock
hold time dramatically, enabling linear write scaling beyond 4
threads. Falls back to hbtrie_insert on reservation failure.
2-3x single-threaded improvement, enables linear scaling."
```

---

### Task 3: Phase 3 Benchmark Comparison

- [ ] **Step 1: Run sync benchmark**

Run: `./build/tests/benchmark/benchmark_database_sync 2>&1`

Record put/get/delete/mixed throughput. Compare with Phase 2 results.

Expected: Put throughput ~250-350K ops/sec (from ~125K in Phase 2).

- [ ] **Step 2: Run concurrent benchmark**

Run: `./build/tests/benchmark/benchmark_database 2>&1`

Record write throughput at 4, 8, 16, 32 threads. Expected: linear scaling, ~300-500K ops/sec at 4 threads, continuing to scale at 16-32 threads.

- [ ] **Step 3: Update analysis document**

Update `docs/write-optimization-analysis.md` with actual benchmark results from all three phases. Add a comparison table showing:

| Phase | ST Put | 4T Write | 16T Write | 32T Write |
|-------|--------|----------|-----------|-----------|
| Baseline | ~63K | ~82K | ~70K | ~61K |
| Phase 1 | ? | ? | ? | ? |
| Phase 2 | ? | ? | ? | ? |
| Phase 3 | ? | ? | ? | ? |

- [ ] **Step 4: Record results and commit**

```bash
git add .benchmarks/ docs/write-optimization-analysis.md
git commit -m "bench: update benchmark results after Phase 3 optimizations"
```