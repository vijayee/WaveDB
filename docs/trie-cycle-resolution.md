# Trie Cycle — Resolved: There Was No Cycle

**Status:** Root cause found and fixed.
**Date:** 2026-07-13
**Supersedes:** the root-cause hypothesis in `trie-cycle-investigation.md` (the
observations in that doc were accurate; the conclusion was not).
**Fix:** `database_scan_next` in `src/Database/database_iterator.c` (3 hunks).
**Regression test:** `tests/test_scan_tombstone_descend.cpp`.

---

## Summary

The HBTrie never develops a cycle. A full-trie integrity check run after **every
mutating operation** of the failing workload (`VectorLayerTest.IVFRecallGate`,
N=2000) found zero structural violations — no node ever had two parents, no
`trie_child` ever pointed at an ancestor.

The "cycle" was an **iterator state-confusion loop** in `database_scan_next`.
When a forward scan encounters a **tombstoned entry that also has a
`trie_child`** (a deleted key whose chunk prefix still owns a subtree of longer
keys), the scan pushed the child frame but kept walking the parent. Its
post-push parent re-fetch:

```c
frame = &iter->stack[iter->stack_depth - 2];
```

is only correct when exactly one frame has been pushed in the current pass of
the inner entry loop. On the **second** tombstoned-prefix push in the same
pass, `stack_depth - 2` resolves to the *first pushed child*, not the parent.
From that point the scan iterates the **parent's** bnode entries using the
**child frame's** `entry_index`, starting from 0 — reaches the first tombstoned
prefix again — pushes its `trie_child` again — re-fetches one-off again —
forever. The depth guard fires at 200, the scan returns an error mid-range,
and the caller silently returns partial results.

**Impact before the fix:** `IVFRecallGate` "passed" at recall@10 = 0.92 — but
0.92 was not natural ANN loss. Every query whose cluster scan hit the loop
aborted mid-scan and dropped the rest of its results. With the fix, the same
test scores recall@10 = **1.00**.

---

## What went wrong (mechanism, step by step)

State: one parent trie node whose leaf bnode has two tombstoned entries that
both still carry a `trie_child` — call them entry `i` ("aaaa" → child A) and
entry `j` ("bbbb" → child B). This is exactly what the IVF rebuild produces:
it tombstones every old `cluster/<cid>/<id>` key, and with `chunk_size = 4`
ids like `v100` are both values *and* chunk-prefixes of `v1000`–`v1009`.

1. Scan walks the parent. At entry `i` (tombstone + trie_child): pushes child A.
   Re-fetch `stack_depth - 2` → parent. **Still correct** (one push so far).
   The tombstone path then *continues the inner loop on the parent* instead of
   descending — the pushed frame A sits on the stack, unprocessed.
2. At entry `j` (tombstone + trie_child): pushes child B. Re-fetch
   `stack_depth - 2` → **frame A, not the parent.**
3. The inner loop continues with `frame` = A's frame (`entry_index` 0), but the
   loop's `btree` variable still points at the **parent's** bnode. The scan
   re-walks parent entries 0, 1, 2, … *via A's frame counter*.
4. It reaches entry `i` again: tombstone + trie_child → pushes child A again.
   Re-fetch → the previously pushed frame. Loop.
5. Stack grows: `parent, A, B, A, A, A, …` until the depth-200 guard fires and
   `database_scan_next` returns `-2`. `database_scan_range_sync_raw` treats a
   non-zero return as end-of-scan and returns whatever it collected — for a
   query that hit the loop early, that's nothing.

The stack signature (from a frame dump added to `push_frame` when the guard
fires) matches the "cycle" dump in `trie-cycle-investigation.md` exactly —
same node repeated at the same `entry_index`:

```
[160] TRIE node=0x8880c0 entry_index=4 value_pending=0
[161] TRIE node=0x8880c0 entry_index=4 value_pending=0
...39 identical frames...
[199] TRIE node=0x8880c0 entry_index=0   ← freshly pushed, guard fired
```

`entry_index=4` on every frame is the tell: each pushed child frame had its
counter advanced 0→4 *against the parent's entries* (four childless tombstones
skipped, then the tombstoned prefix at index 4), then the same child was pushed
again. The original investigation's "node A entry 4 → B, node B entry 4 → A"
reading was taken through these mismatched (frame, entry_index) pairs — the
dump was reading the *iterator's confusion*, not the trie.

### Why the original investigation missed it

- All instrumentation was on the **write path** (`hbtrie_insert_unsafe`
  assignment checks, pool-aliasing checks, ancestor checks). Those correctly
  found nothing — the writers are fine. The bug is on the **read path**.
- The ASAN heap-use-after-free previously seen in `push_frame` was real but a
  *consequence*, not the cause: healthy scans on this keyspace need ~13 frames
  and the stack starts at 16, so the stack only reallocs (invalidating `frame`)
  once the loop is already underway.
- The depth guard (commit `6d02324`) capped the damage and let tests pass,
  which hid the silent result loss behind a 0.92 recall that still cleared the
  0.90 gate.
- Why only N=2000: with `chunk_size = 4`, ids `v1000`–`v1999` split into two
  chunks (`"v100" + "0"`), so `v100`–`v199` become prefix entries with
  subtrees. At N=20 every id fits a single chunk — no prefix entries, no
  trigger. The scale threshold was about key length crossing the chunk
  boundary, not memory pressure.

---

## How it was diagnosed (method + instrumentation)

All experiments in Docker (`gcc:13`), Debug + `-fsanitize=address`, on master
`b17987d`. Three instruments, applied in order:

### 1. Rule out memory corruption: run ASAN with the pool disabled

The memory pool is backed by static arrays, so pool-recycling bugs are
invisible to ASAN. A one-line env-gated bypass makes every allocation a real
`malloc`/`free` that ASAN can police:

```c
// src/Util/memory_pool.c — memory_pool_init()
void memory_pool_init(void) {
    if (getenv("WAVEDB_NO_POOL")) {
        log_info("memory pool DISABLED via WAVEDB_NO_POOL");
        return;  // g_pool.initialized stays 0 → all allocs fall back to malloc
    }
    ...
}
```

Result: `WAVEDB_NO_POOL=1` under ASAN → **zero reports, loop still occurs
deterministically**. No use-after-free, no double-free, no overflow anywhere in
the workload. Corruption ruled out.

### 2. Prove the trie is intact: a "trie doctor" after every write

A full-trie walk with a visited set, hooked into the tail of
`hbtrie_insert_unsafe` and `hbtrie_delete_unsafe` (covers every sync-mode
mutation regardless of caller). Any node reachable twice — which covers both
multi-parent nodes and cycles of any length — aborts with both paths:

```c
/* Pointer hash set with generation stamps (O(1) reset per walk). */
#define DOCTOR_HASH_BITS 21
#define DOCTOR_HASH_SIZE (1u << DOCTOR_HASH_BITS)
typedef struct { void* node; void* parent; uint32_t gen; int via_entry; const char* kind; } doctor_seen_t;
static doctor_seen_t g_doctor_table[DOCTOR_HASH_SIZE];
static uint32_t g_doctor_gen = 0;

static int doctor_add(void* node, void* parent, int via_entry, const char* kind) {
  size_t h = doctor_hash(node);
  for (;;) {
    doctor_seen_t* slot = &g_doctor_table[h];
    if (slot->gen != g_doctor_gen || slot->node == NULL) {
      slot->node = node; slot->parent = parent; slot->via_entry = via_entry;
      slot->kind = kind; slot->gen = g_doctor_gen;
      return 0;
    }
    if (slot->node == node) {
      fprintf(stderr, "*** TRIE DOCTOR: node %p reached TWICE!\n"
                      "***   first via parent=%p entry=%d kind=%s\n"
                      "***   now   via parent=%p entry=%d kind=%s\n",
              node, slot->parent, slot->via_entry, slot->kind, parent, via_entry, kind);
      return -1;
    }
    h = (h + 1) & (DOCTOR_HASH_SIZE - 1);
  }
}

/* BFS from trie->root. For each hbtrie node, walk its internal bnode tree
   (child_bnode links), registering every bnode and collecting child hbtrie
   nodes from `child` (has_value==0) and `trie_child` (has_value==1) entries.
   Register each hbtrie node + bnode exactly once; abort on a repeat. */
int hbtrie_doctor_check(hbtrie_t* trie);  // ~80 lines, same walk as hbtrie_gc_unsafe

/* Hook (env-gated), at the success return of insert_unsafe / delete_unsafe: */
static void hb_doctor_after_op(hbtrie_t* trie, const char* opname) {
  if (getenv("WAVEDB_DOCTOR") == NULL) return;
  if (hbtrie_doctor_check(trie) != 0) {
    fprintf(stderr, "*** TRIE DOCTOR: first violation after %s\n", opname);
    abort();
  }
}
```

Result: **zero violations across the entire failing run** — insert phase,
rebuild batch, everything — while the guard still fired during the search
phase. The trie is structurally perfect; the scan is misreading it.

### 3. Catch the loop live: stack dump when the guard fires

```c
// push_frame(), inside the depth guard:
if (iter->stack_depth >= 200) {
    fprintf(stderr, "*** CYCLE GUARD FIRED depth=%zu node=%p ***\n",
            iter->stack_depth, (void*)node);
    size_t lo = iter->stack_depth > 40 ? iter->stack_depth - 40 : 0;
    for (size_t di = lo; di < iter->stack_depth; di++) {
        iterator_frame_t* f = &iter->stack[di];
        fprintf(stderr, "  [%3zu] %s node=%p bnode=%p entry_index=%zu value_pending=%d\n",
                di, f->is_bnode_frame ? "BNODE" : "TRIE ",
                (void*)f->node, (void*)f->bnode, f->entry_index, (int)f->value_pending);
    }
    return -1;
}
```

Result: the same-node/same-entry_index dump shown above — one node pushed
repeatedly, no second node involved. Combined with the doctor's clean bill,
this pinpointed the descend bookkeeping in `database_scan_next`, where reading
the code found the `stack_depth - 2` re-fetch and the push-without-descend
tombstone path.

---

## The fix

Three hunks in `database_scan_next`, aligning the `has_value` branch with the
push protocol every other descend branch already follows
(`entry_index++` → `pushed_child = 1` → push → `break`):

**1. Re-fetch the parent frame by absolute index** (correct regardless of how
many frames were pushed; still fixes the original realloc staleness):

```c
int pushed_trie_child = 0;
{
    size_t parent_frame_idx = iter->stack_depth - 1;
    if (entry->trie_child) {
        if (push_frame(iter, entry->trie_child, iter->stack_depth - 1) < 0) {
            return -2;
        }
        pushed_trie_child = 1;
        // push_frame() may realloc iter->stack, invalidating the
        // cached `frame` pointer. Re-fetch the parent frame by its
        // absolute index (NOT stack_depth-2, which is wrong once
        // more than one frame has been pushed in this pass).
        frame = &iter->stack[parent_frame_idx];
    }
}
```

**2. A tombstoned prefix with a pushed child descends immediately** — this both
removes the confusion window and restores sorted emit order (subtree keys
extend the tombstoned prefix, so they belong before the parent's next entry):

```c
if (pushed_trie_child) {
    pushed_child = 1;
    break;
}
```

**3. A below-lower-bound value with a pushed child descends the same way** —
also fixes a latent missing-results bug: a prefix below `start_path` can still
have in-range keys in its subtree, and the old `continue` left the pushed
frame to be processed out of order (or looped, same as the tombstone case):

```c
if (bounds == 0) {
    path_destroy(result_path);
    identifier_destroy(value);
    if (pushed_trie_child) {
        pushed_child = 1;
        break;
    }
    continue;
}
```

The live-value emit path (`return 0` with the child left on top of the stack
for the next call) was already correct and is unchanged. `database_scan_prev`,
`seek_to_start_path`, and `seek_to_rightmost` use a single-push-per-pass /
`value_pending` protocol and do not have this bug (each push site audited; the
reverse-scan suite passes unchanged).

---

## Regression test

`tests/test_scan_tombstone_descend.cpp` reproduces the loop with six keys —
no vector layer, no N=2000. Core of the test:

```c
// chunk_size=4: "aaaax" splits into chunks ["aaaa","x"], so inserting both
// "t/aaaa" and "t/aaaax" gives the leaf entry for "aaaa" a value AND a
// trie_child. Deleting "t/aaaa" makes it a tombstoned prefix with a live
// subtree. Two of those in one bnode arm the loop.
const raw_op_t puts[] = {
    {"t/aaaa",  6, (const uint8_t*)"1", 1, 0},
    {"t/aaaax", 7, (const uint8_t*)"2", 1, 0},
    {"t/bbbb",  6, (const uint8_t*)"3", 1, 0},
    {"t/bbbbx", 7, (const uint8_t*)"4", 1, 0},
};
database_batch_sync_raw(db, '/', puts, 4);

const raw_op_t dels[] = {          // same batch shape as the IVF rebuild
    {"t/aaaa", 6, NULL, 0, 1},
    {"t/bbbb", 6, NULL, 0, 1},
};
database_batch_sync_raw(db, '/', dels, 2);

// Full scan and bounded range scan must both terminate and return exactly
// the two live subtree keys with their values:
//   "t/aaaax" -> "2",  "t/bbbbx" -> "4"
database_scan_range_sync_raw(db, NULL, 0, NULL, 0, '/', &results, &count);
ASSERT_EQ(count, 2u);   // pre-fix: 0 — the scan looped and aborted
```

Two cases: a full unbounded scan and a bounded range scan (the IVF
cluster-scan pattern). The database is `sync_only = 1`, matching the failing
mode (deletes tombstone via `hbtrie_delete_unsafe`).

---

## How it was tested

Environment: Docker `gcc:13`, CMake Debug; ASAN runs used
`-fsanitize=address -g -O1` with `ASAN_OPTIONS=detect_leaks=0`.
(Note: the `BUILD_WITH_ASAN` option referenced in the old investigation doc
does not exist in `CMakeLists.txt` — inject flags via `CMAKE_C_FLAGS`.)

| Step | Configuration | Result |
|---|---|---|
| Repro baseline | master `b17987d`, ASAN, stock pool | `IVFRecallGate` passes at recall 0.92; depth guard fires 14× per run (instrumented print); no ASAN reports |
| Corruption ruled out | ASAN + `WAVEDB_NO_POOL=1` (all allocs = malloc) | Zero ASAN reports; loop still occurs → not memory corruption |
| Trie integrity | doctor after **every** `hbtrie_insert_unsafe` / `hbtrie_delete_unsafe` | Zero violations across the whole failing run → trie never corrupted |
| Loop captured | stack dump at guard fire | Same node repeated at same `entry_index` — matches the "cycle" dump in the old doc |
| Regression test, red | new test on **unfixed** master | Both cases FAIL in ~25 ms (scan returns 0 of 2 keys) |
| Regression test, green | new test with fix | Both cases PASS |
| Full suites, fixed tree | `test_scan_tombstone_descend`, `test_vector`, `test_hbtrie`, `test_bnode`, `test_raw_api`, `test_reverse_scan` | **85/85 tests pass** |
| Recall | `IVFRecallGate` with fix | recall@10 **0.92 → 1.00**; guard never fires; SLSH unchanged (0.914) |

---

## Follow-ups (separate from this fix)

- The depth-200 guard in `push_frame` can stay as a cheap invariant backstop,
  but it should now be considered "should never fire"; consider logging
  loudly if it ever does.
- `trie-cycle-investigation.md` is retained for history with a supersession
  note; its Path-to-Fixing options (realloc audit / fixed-size stack) are no
  longer needed for this bug.
- Latent issues noticed during the audit, worth separate tickets:
  1. `hbtrie_gc_unsafe`'s single-tombstone removal path (`hbtrie.c`, the
     `bnode_remove_at` branch) drops `entry->trie_child` without checking it —
     a tombstoned prefix with a live subtree would have the whole subtree
     detached and leaked on the first `database_snapshot` in sync mode. Same
     key shape as this bug, so it is reachable by the same workloads.
  2. `hbtrie_node_destroy` frees all collected descendants without consulting
     their individual refcounts — fine while inner nodes are never destroyed
     mid-iteration, fragile under MVCC/async usage.
  3. The memory pool's free-list `next` pointer overwrites the first 8 bytes
     of freed blocks — which is the `refcounter` ("MUST be first member") of
     `hbtrie_combined_t`/`bnode_t`. Any late deref on a dangling node corrupts
     the free list; the TLS-cache free path has no double-free detection.
  4. In sync mode nothing but `database_snapshot` triggers GC, so rebuild
     tombstones accumulate forever and every scan pays to skip them.
