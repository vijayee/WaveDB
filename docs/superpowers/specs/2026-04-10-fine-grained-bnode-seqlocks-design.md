# Fine-Grained Per-Bnode Seqlocks for HBTrie

**Date:** 2026-04-10
**Branch:** experimental-locks

## Context

WaveDB's HBTrie has a single global `trie->lock` (a `pthread_mutex_t`) that serializes all concurrent writers, regardless of which key they're writing to. This is the dominant write bottleneck. The database layer has 64 sharded write locks, but they're redundant — `hbtrie_insert_mvcc` immediately acquires `trie->lock`, making all writes globally serial.

Meanwhile, MVCC reads (`hbtrie_find_mvcc`) are already declared lock-free but lack formal memory ordering guarantees. The multi-level B+tree we implemented gives us natural granularity: each `bnode_t` is a natural lock domain.

The non-MVCC API (`hbtrie_insert`, `hbtrie_find`, `hbtrie_remove`) is only used by visualization examples and never by the database layer. The `_mvcc` suffix on the actual API is semantically misleading.

## Goal

Replace the global `trie->lock` with per-bnode sequence locks, enabling concurrent writes to different bnodes while providing provably safe lock-free reads. Remove the non-MVCC API and rename the MVCC API to be the primary API.

## Design

### Commit 1: API Cleanup — Remove Non-MVCC Functions and Rename

**Remove:**
- `hbtrie_insert(trie, path, value)` from `hbtrie.h` and `hbtrie.c`
- `hbtrie_find(trie, path)` from `hbtrie.h` and `hbtrie.c`
- `hbtrie_remove(trie, path)` from `hbtrie.h` and `hbtrie.c`

**Rename:**
- `hbtrie_insert_mvcc` → `hbtrie_insert`
- `hbtrie_find_mvcc` → `hbtrie_find`
- `hbtrie_delete_mvcc` → `hbtrie_delete`

**Update all callers:**
- `src/Database/database.c`: all `_mvcc` calls drop the suffix
- `src/HBTrie/hbtrie.c`: internal references updated
- `src/HBTrie/hbtrie.h`: declarations updated
- Visualization examples (`example_viz.c`, `example_complex_viz.c`): remove or stub out — they used the non-MVCC API and will be revisited later

**Verification:** `test_database`, `test_hbtrie`, `test_bnode` all pass. Node.js and Dart binding tests pass.

### Commit 2+: Per-Bnode Seqlock Implementation

#### Data Structure Changes

**`src/HBTrie/bnode.h`** — `bnode_t`:

```c
typedef struct bnode_t {
    refcounter_t refcounter;        // MUST be first
    _Atomic(uint64_t) seq;           // Seqlock: even=stable, odd=writing
    PLATFORMLOCKTYPE(write_lock);    // Writer mutual exclusion (was: unused `lock`)
    uint32_t node_size;
    uint16_t level;                  // 1=leaf, >1=internal
    vec_t(bnode_entry_t) entries;
} bnode_t;
```

The existing unused `PLATFORMLOCKTYPE(lock)` field is renamed to `write_lock`. The new `_Atomic(uint64_t) seq` field is added after `refcounter` (before `write_lock`) to keep it on a separate cache line from the write lock, reducing false sharing between readers and writers.

**`src/HBTrie/hbtrie.h`** — `hbtrie_t`:

```c
typedef struct hbtrie_t {
    refcounter_t refcounter;
    _Atomic(hbtrie_node_t*) root;   // Atomic root pointer (was: plain pointer)
    uint8_t chunk_size;
    uint32_t btree_node_size;
} hbtrie_t;
```

Remove `PLATFORMLOCKTYPE(lock)` from `hbtrie_t`. The root pointer becomes atomic.

**`src/HBTrie/hbtrie.h`** — `hbtrie_node_t`:

```c
typedef struct hbtrie_node_t {
    refcounter_t refcounter;
    PLATFORMLOCKTYPE(write_lock);   // For root btree pointer updates (was: unused `lock`)
    _Atomic(uint64_t) seq;           // Seqlock for btree/btree_height updates
    bnode_t* btree;
    uint16_t btree_height;
    struct sections_t* storage;
    size_t section_id;
    size_t block_index;
    uint8_t is_loaded;
    uint8_t is_dirty;
} hbtrie_node_t;
```

The existing unused `lock` is renamed to `write_lock`. A `seq` field is added for protecting `btree` and `btree_height` updates (when the root bnode of an hbtrie_node's B+tree splits).

#### Writer Protocol

**Insert path (`hbtrie_insert`):**

1. **Descent:** Optimistic read using seqlocks (same as reader protocol). Record `bnode_path` for split propagation.
2. **Leaf modification:** Acquire `leaf->write_lock`. Increment `leaf->seq` (odd = writing). Perform the mutation. If no split needed: increment `leaf->seq` (even), release `leaf->write_lock`.
3. **Split propagation:** The leaf seq is set to even (stable) after the split completes — entries are now split between left and right siblings, both consistent. Then release leaf lock. Acquire `parent->write_lock`, increment parent seq (odd), insert split key + right sibling, check if parent needs split. If parent doesn't split: increment parent seq (even), release parent lock. If parent splits: set parent seq even, release parent lock, continue upward to grandparent.
   - **Reader safety during split propagation:** After the leaf split but before the parent update, a reader descending through the parent will not find entries that moved to the right sibling. This is safe because the parent's seqlock will be odd during the update, causing readers to retry the parent. Once the parent update completes and seq becomes even, readers will see both left and right children.
4. **Root update in hbtrie_node:** When `hbtrie_node->btree` root splits, acquire `hbtrie_node->write_lock`, increment `hbtrie_node->seq`, update `btree` and `btree_height`, increment `hbtrie_node->seq`, release `hbtrie_node->write_lock`.
5. **Trie root update:** Use `atomic_store(&trie->root, new_root, memory_order_release)`.

**Delete path (`hbtrie_delete`):**

Simpler — only the leaf bnode needs a write lock. Acquire `leaf->write_lock`, increment seq, update version chain (add tombstone), increment seq, release lock. No splits can occur.

**The global `trie->lock` is completely removed** from insert and delete paths.

#### Reader Protocol

**`hbtrie_find` (the renamed `hbtrie_find_mvcc`):**

```c
// Descent through internal bnodes (optimistic)
static bnode_t* bnode_descend_optimistic(bnode_t* root, chunk_t* key) {
    bnode_t* current = root;
    while (current->level > 1) {
        uint64_t s1, s2;
        bnode_t* child;
        do {
            s1 = atomic_load(&current->seq);
            if (s1 & 1) { cpu_relax(); continue; }
            size_t idx;
            bnode_entry_t* ie = bnode_find(current, key, &idx);
            // Determine child: if exact match and is_bnode_child, follow child_bnode;
            // if exact match and !is_bnode_child, follow child (hbtrie_node);
            // if no match, follow left neighbor's child_bnode or entries.data[0].child_bnode
            s2 = atomic_load(&current->seq);
        } while (s1 != s2);
        current = child;
    }
    return current;
}
```

At each bnode, the reader:
1. Loads `seq` (s1)
2. If odd, a writer is active — spin/retry
3. Reads bnode data (entries, child pointers)
4. Loads `seq` again (s2)
5. If s1 != s2, data changed during read — retry this bnode

For `hbtrie_node_t` transitions (following `entry->child`):
- Read `hbtrie_node->seq` around reading `hbtrie_node->btree` to get the current root bnode

For `trie->root`:
- Use `atomic_load(&trie->root, memory_order_acquire)`

#### `hbtrie_node_t` Root Btree Protection

When a btree root inside an `hbtrie_node_t` splits:
1. Acquire `hbtrie_node->write_lock`
2. Increment `hbtrie_node->seq` (odd)
3. Update `hbtrie_node->btree` and `hbtrie_node->btree_height`
4. Increment `hbtrie_node->seq` (even)
5. Release `hbtrie_node->write_lock`

Readers access `hbtrie_node->btree` using the same seqlock pattern as bnodes.

#### `cpu_relax` Helper

Already defined in `src/Util/threadding.h` — platform-specific spin-loop hint (`pause` on x86, `yield` on ARM, noop fallback). Seqlock retry loops use `cpu_relax()` instead of bare busy-wait.

#### Split Propagation and Deadlock Safety

Split propagation acquires locks bottom-up (leaf → parent → grandparent). This ordering is consistent and deadlock-free because:
- Descent is optimistic (no locks held during descent)
- Writers only acquire locks after completing descent
- The `bnode_path` records the exact descent path, ensuring consistent lock ordering

If a writer needs to re-descend after a failed optimistic read (because a bnode changed), it restarts the entire descent — it does not hold any locks during re-descent.

#### Non-MVCC Functions

The non-MVCC functions are removed in Commit 1. No changes needed.

#### GC Path (`hbtrie_gc`)

`hbtrie_gc` currently traverses all bnodes without locks. Under the new scheme, it uses the same optimistic read protocol (seqlock validation at each bnode). Since GC only reads version chains and doesn't mutate bnode structure, it's a reader.

However, GC *does* mutate version chains (pruning old versions). This mutation happens on leaf `bnode_entry_t` objects inside leaf bnodes. The GC must acquire `leaf->write_lock` and use the seqlock protocol when modifying version chains.

**Tombstone entry removal (bug fix):**

Currently, when GC prunes a version chain and the only remaining version is a tombstone (`is_deleted=1`), the `bnode_entry_t` stays in the bnode indefinitely. The existing code only downgrades to legacy mode when the single remaining version is *not* deleted (line 2218: `!entry->versions->is_deleted`). This causes:
- Memory leak — every delete permanently occupies a `bnode_entry_t` + `version_entry_t`
- Bnode bloat — entries vectors grow with tombstones, causing unnecessary splits
- Read degradation — binary search scans over dead entries

Fix: After `version_entry_gc`, if the only remaining version is a tombstone whose `txn_id < min_active_txn_id` (visible to all active transactions), remove the `bnode_entry_t` entirely using `bnode_remove_at`. This requires:
1. The leaf bnode's `write_lock` must be held (GC already needs this for version chain mutations)
2. The seqlock protocol: increment seq (odd), remove entry, increment seq (even)
3. After removal, if the entry's `child` was an `hbtrie_node_t` (for intermediate-level entries), that subtree is orphaned and should be destroyed. For leaf-level entries, there is no child subtree.

This also means GC must track the entry's index within the leaf bnode so it can call `bnode_remove_at` after pruning. The current iteration (`for (size_t i = 0; i < bn->entries.length; i++)`) already provides this index, but removal during iteration requires iterating backwards (from `length-1` to `0`) to avoid invalidating subsequent indices.

## Files Modified

1. **`src/HBTrie/bnode.h`** — Add `_Atomic(uint64_t) seq` to `bnode_t`, rename `lock` to `write_lock`
2. **`src/HBTrie/bnode.c`** — Initialize `seq` to 0 in `bnode_create`/`bnode_create_with_level`, update lock init/destroy
3. **`src/HBTrie/hbtrie.h`** — Add `seq` and rename `lock` to `write_lock` in `hbtrie_node_t`, make `root` atomic in `hbtrie_t`, remove `lock` from `hbtrie_t`
4. **`src/HBTrie/hbtrie.c`** — Remove non-MVCC functions, rename MVCC functions, implement seqlock read/write protocol, remove `trie->lock` acquisition from insert/delete paths, implement `bnode_descend_optimistic`, update split propagation for per-bnode locking
5. **`src/Database/database.c`** — Update all `hbtrie_*_mvcc` calls to drop `_mvcc` suffix
6. **`example_viz.c`, `example_complex_viz.c`** — Remove or stub (used non-MVCC API)

## Verification

1. **Commit 1 verification:**
   - `cd build && cmake .. -DBUILD_BENCHMARKS=ON -DBUILD_TESTS=ON && make -j$(nproc)`
   - `./test_database` — all 16 tests pass
   - `./test_hbtrie`, `./test_bnode` — pass
   - Node.js: `ASAN_OPTIONS=detect_leaks=0 npm test` — 60 pass
   - Dart: `dart test` — 75 pass

2. **Seqlock verification:**
   - All above tests pass again
   - New stress test: concurrent writers (8+ threads) to different keys, verify no data corruption
   - New stress test: concurrent readers + writers on overlapping keys, verify readers see consistent state
   - New stress test: concurrent inserts triggering bnode splits, verify split propagation is safe under concurrency
   - Benchmark: `benchmark_database` — concurrent write throughput should scale with thread count (not flat)

3. **Performance comparison:**
   - Baseline: `experimental-locks` branch before seqlock changes
   - Target: single-threaded performance within 5%, concurrent write throughput 4-8x improvement at 16+ threads