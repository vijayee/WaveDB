# Trie Cycle Investigation — Known Bug

**Status:** Mitigated (cycle guard prevents hang/OOM). Root cause not yet fixed.
**Date:** 2026-07-13
**Related commit:** `6d02324` (fix(iterator): bounds-check tombstones + cycle guard)

## Summary

A trie — by construction — is acyclic. But under specific conditions (N=2000+
vectors with IVF rebuild's delete+put pattern), the HBTrie develops a 2-node
cycle in `trie_child` pointers: node A's entry has `trie_child = B`, and node
B's entry has `trie_child = A`. This causes `database_scan_next` to descend
infinitely (pushing the same two nodes repeatedly), growing the iterator stack
to 2500+ frames and consuming all memory.

## How It Manifests

1. **Insert phase** (2000 vectors, each a 3-op batch): completes normally,
   ~20 seconds.
2. **Train + rebuild** (k-means + ~3920-op batch of deletes + puts): completes
   normally, ~2 seconds.
3. **Search phase** (50 recall queries): the first ~8 queries complete in
   ~0.001s each. Then query 9's cluster scan hangs — CPU at 97%, no progress,
   RSS growing slowly.

### Without the cycle guard (commit before 6d02324)

The iterator stack grows to 2500+ frames via `push_frame`. Each frame
references an `hbtrie_node_t` (incrementing its refcount). The `realloc` in
`push_frame` copies the growing stack. RSS climbs to 45GB, filling RAM + swap,
crashing the machine.

### With the cycle guard (commit 6d02324)

`push_frame` caps stack depth at 200. When exceeded, it returns -1. The scan
returns an error, the caller handles it, and the test continues. RSS stays at
~25MB. All 23 vector tests pass (IVF recall@10: 0.92, SLSH: 0.914).

## The Cycle Structure

Confirmed via runtime diagnostics (node address dump in `push_frame`):

```
Stack[11]  node A (0x...3f20)  entry_index=4  trie frame
Stack[12]  node B (0x...4b20)  entry_index=4  trie frame
Stack[13]  node A (0x...3f20)  entry_index=4  trie frame  ← cycle
Stack[14]  node A (0x...3f20)  entry_index=4  trie frame
... (repeats 188+ times until guard fires at depth 200)
```

- Node A's bnode entry at index 4 has `has_value=1` and `trie_child = B`
- Node B's bnode entry at index 4 has `has_value=1` and `trie_child = A`
- Both nodes are valid `hbtrie_node_t` objects (the scan can read their
  entries without crashing) — the pointers aren't garbage, just wrong
- The two nodes are ~3KB apart in memory (same memory pool allocation batch)

The legitimate descent path is stack[0]–stack[10] (11 levels: root → vec →
test → cluster/centroid → cid → id, with multi-level B+tree frames). The
cycle starts at stack[11], which is the leaf level of the trie.

## Root Cause Hypothesis

The cycle is **not created by any trie_child assignment path**. This was
verified by instrumenting every assignment in `hbtrie_insert_unsafe`:

| Assignment path | Check | Result |
|----------------|-------|--------|
| `entry->trie_child = child` (create, `hbtrie_node_create`) | New node is ancestor? Has entries? | Didn't fire — new nodes are fresh, 0 entries |
| `entry->trie_child = entry->child` (upgrade) | `entry->child` is ancestor? | Didn't fire |
| `entry->trie_child = loaded` (lazy-load from disk) | Reached at all? | Didn't fire — `child_disk_offset` is 0 in sync_only mode |
| `entry->child = child` (create, `hbtrie_node_create`) | New node is ancestor? Has entries? | Didn't fire |
| `bnode_entry_lazy_load_trie_child` (function-based lazy-load) | Reached at all? | Didn't fire |
| Memory pool aliasing | New node has 0 entries? | Didn't fire — no aliasing |
| `hbtrie_node_copy` (copy path) | Called at all? | Not called (vector layer uses `open_separate`, not subtree) |

Since no assignment path creates the cycle, the corruption comes from
**memory safety bugs** — specifically, the pre-existing ASAN heap-use-after-free
in `push_frame`'s `realloc`:

### The ASAN use-after-free (partially mitigated)

`push_frame` grows the iterator stack via `realloc`. When `realloc` moves the
allocation, all cached pointers into the old stack (`frame`, `btree`, `entry`)
become stale. The scan code was holding `frame` across `push_frame` calls and
reading `frame->entry_index` after the realloc — a heap-use-after-free that
ASAN flagged.

The fix in commit `6d02324` re-fetches `frame` after `push_frame`:
```c
frame = &iter->stack[iter->stack_depth - 2];
```

But this only fixes the `frame` pointer. Other stale pointers (`btree`,
`entry`, `bnode_path`) may still corrupt memory when written through. During
the rebuild's high-volume delete+put pattern (3920 ops in one batch), the
iterator is created/destroyed repeatedly, and the realloc corruption can
write to freed memory that's been reallocated to trie nodes — corrupting
their `trie_child` fields.

## Strategies Tried

### 1. Tombstone bounds check (FIXED — commit 6d02324)
**Problem:** `database_scan_next` only checked `within_bounds` for entries
with visible values. Tombstoned entries (deleted versions) were skipped
without bounds checking, so scans traversed past the upper bound into sibling
keyspaces, pushing `trie_child` frames without popping.

**Fix:** Check `within_bounds` for tombstoned entries too. If past the upper
bound, stop the scan.

**Result:** Eliminated the OOM from unbounded stack growth (4500+ frames →
200 frames). But the cycle still forms.

### 2. Cycle guard in `push_frame` (MITIGATED — commit 6d02324)
**Fix:** Cap stack depth at 200 (far above any real trie depth of ~10 for
6000 keys). When exceeded, return -1.

**Result:** Prevents hang/OOM. All tests pass. But the underlying corruption
remains.

### 3. Runtime cycle detection at assignment points (INCONCLUSIVE)
Added cycle checks at every `trie_child`/`child` assignment in
`hbtrie_insert_unsafe`, comparing the assigned node against the descent path
(ancestors). Also checked for memory pool aliasing (new node has entries).

**Result:** No checks fired. The cycle is not created by any single
assignment — it's created by memory corruption writing to the wrong node.

### 4. Seek failure investigation (RULED OUT)
Added bail diagnostics to `seek_to_start_path`. The seek never bails — it
correctly positions the cursor at the lower bound. The slowness was from the
cycle, not from a failed seek walking from the leftmost key.

### 5. Lazy-load investigation (RULED OUT)
Added prints to all lazy-load paths (inline + `bnode_entry_lazy_load_trie_child`).
None fire — `child_disk_offset` is always 0 in sync_only mode (fresh database,
no disk persistence).

### 6. Memory pool aliasing investigation (RULED OUT)
After every `hbtrie_node_create`, checked if the returned node has a non-empty
btree (indicating the pool recycled a live pointer). Never fired — newly
created nodes always have 0 entries.

## Path to Fixing

### Option A: Fix the ASAN use-after-free (root cause)
The `push_frame` realloc invalidates cached pointers. The `frame` re-fetch
mitigates the most obvious case, but a full audit is needed:

1. **Run the full test suite under ASAN** to find the exact use-after-free:
   ```bash
   cmake -B build-asan -DCMAKE_BUILD_TYPE=Debug -DBUILD_WITH_ASAN=ON
   cmake --build build-asan --target test_vector
   ./build-asan/test_vector --gtest_filter='*IVFRecallGate*'
   ```
   ASAN will report the exact stale-pointer access with a stack trace.

2. **Audit all `push_frame` callers** for stale pointers after the call:
   - `database_scan_next`: `frame`, `btree`, `entry`, `bnode_path`
   - `seek_to_start_path`: `frame`, `cur_bn`
   - `seek_to_rightmost`: `frame`
   - Any function that holds a pointer into `iter->stack` across `push_frame`

3. **Re-fetch all stale pointers** after every `push_frame`/`push_bnode_frame`
   call, not just `frame`.

4. **Alternatively**, switch the iterator stack from `realloc`-grown to a
   **fixed-size array** (e.g., 64 entries — far above any real trie depth).
   This eliminates `realloc` entirely, eliminating the use-after-free class.
   The cycle guard already caps at 200, so a fixed array of 200 would work.

### Option B: Keep the cycle guard, file the ASAN bug separately
The cycle guard (commit `6d02324`) prevents the hang/OOM. All tests pass. The
ASAN use-after-free is a pre-existing memory safety bug that predates the
vector layer work and should be tracked as a separate tech-debt item.

**Recommendation:** Option A (fixed-size array) is the cleanest fix — it
eliminates the realloc entirely, which is the root cause of both the ASAN
bug and the cycle. The cycle guard can remain as a safety net.

## Reproduction

```bash
cmake --build build --target test_vector -j4
./build/test_vector --gtest_filter='VectorLayerTest.IVFRecallGate'
```

With the cycle guard in place, this passes (recall@10: 0.92, ~2s). Without
the guard, it hangs and OOMs the machine.

The cycle only manifests at scale (N=2000 with rebuild's delete+put pattern).
Smaller tests (N=20, like `IVFTrainRebuild`) don't trigger it — the trie is
too shallow for the realloc corruption to affect the right memory.

## Related Files

- `src/Database/database_iterator.c` — `push_frame` (cycle guard),
  `database_scan_next` (tombstone bounds check, frame re-fetch)
- `src/HBTrie/hbtrie.c` — `hbtrie_insert_unsafe` (trie_child assignments,
  all checked and ruled out)
- `src/HBTrie/bnode.c` — `bnode_split` (trie_child copy/clear during split)
- `src/Database/database.c` — `database_scan_range_sync_raw` (100K hard cap)