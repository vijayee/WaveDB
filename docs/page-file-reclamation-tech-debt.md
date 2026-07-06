# Page-File Write Amplification: Sub-Block Packing (fixed 0.1.12) + Reclamation (tech debt)

**Date:** 2026-07-06
**Status:** Mechanism 1 (initial-load structural bloat) **FIXED in 0.1.12** by sub-block
packing. Mechanism 2 (overwrite CoW bloat) **deferred** — reclamation/vacuum is
unimplemented; this doc records the full scope, reproduction, and proposed fix so a
future release can pick it up.

## TL;DR

WaveDB's page file (`data.wdbp`) exhibited two independent write-amplification
mechanisms that together inflated a ~5K-episode Hippo corpus to 4.69 GB
(~960 KB/episode, ~1000x over logical content):

1. **Initial-load structural bloat** — every bnode, even a compact ~18-200 byte
   one, consumed a full 4 KB block. Fixed in 0.1.12 by packing single-block
   bnodes into shared blocks (61x reduction; see `src/Storage/page_file.c`
   `page_file_write_node`).
2. **Overwrite CoW bloat** — every update (CoW) writes a new bnode to a fresh
   block and marks the old offset stale, but **stale regions are never
   reclaimed**. The reclamation function exists but has zero callers; no
   page-file vacuum/compaction pass exists. Deferred.

The two are independent: a fresh load (one write per key) only hits mechanism 1;
an update workload hits mechanism 2 on top. The 0.1.12 fix makes fresh loads
compact; mechanism 2 must be addressed before long-running update workloads
(retrieval-count bumps, LTP phase transitions, consolidation) can run without
unbounded file growth.

## Mechanism 1 — Initial-load structural bloat (FIXED 0.1.12)

### Root cause

`page_file_write_node` (`src/Storage/page_file.c:260`) serializes a bnode via
`bnode_serialize_v3` (`src/Storage/node_serializer.c:763`), which writes
**compact** data — 1-byte magic + 2-byte level + 2-byte entry count + entries,
via a dynamic buffer; `*len = sb.size` is the actual used byte count, **no
padding to `node_size`**. A near-empty leaf bnode is ~18-200 bytes on the wire.

Despite that, the block-advance condition forced one full 4 KB block per bnode:

```c
// page_file.c:388 (BEFORE the 0.1.12 fix)
if (remaining_space <= INDEX_BLK_META_SIZE || written >= total_len) {
    // write IndexBlkMeta at block tail, then advance:
    pf->cur_bid++;
    pf->cur_offset = 0;
}
```

`written >= total_len` is TRUE at the end of *every* `page_file_write_node`
call (the write loop only exits when `written == total_len`), so the advance
fired on every bnode. One bnode = one 4096-byte block, ~3.9 KB wasted.

This compounded with `chunk_size=4` (default): `hbtrie_insert`
(`src/HBTrie/hbtrie.c:2262`) creates one `hbtrie_node` + root bnode **per 4-byte
chunk** (not per delimiter-separated path component). A 44-byte key with
`chunk_size=4` spans ~12 chunks → 12 bnodes → 12 blocks ≈ 48 KB/key. Increasing
`btree_node_size` did not help (confirmed: `node=65536` = same 24.5 KB/key) —
confirming the bnode is compact on the wire and the floor is the per-block
allocation, not the B+tree node capacity.

### Fix (0.1.12)

Change the advance condition so single-block bnodes pack into the remaining
block space; only advance when the block is actually full OR a multi-block
bnode finishes (its chain needs a terminating `IndexBlkMeta`):

```c
// page_file.c:388 (AFTER the 0.1.12 fix)
if (remaining_space <= INDEX_BLK_META_SIZE ||
    (written >= total_len && bids_count > 1)) {
```

This is read-safe: `page_file_read_node` (`src/Storage/page_file.c:418`) reads
single-block bnodes by `disk_offset + 4-byte size prefix` from the block buffer
and never consults `IndexBlkMeta`; only multi-block bnodes (> ~4 KB) follow
`IndexBlkMeta.next_bid` chains. The data area `[block_start, block_start +
block_size - 16)` is never written into by bnode payloads (the write path
reserves the trailing 16 bytes), so packing multiple bnodes into one block does
not collide with the per-block `IndexBlkMeta` slot. After a multi-block bnode
the code still advances to a fresh block so no two multi-block chains share a
tail block (one `IndexBlkMeta` per block can only terminate one chain).

### Result

`chunk_size=4` (default) divergent-prefix keys: 24.5 KB/key → **0.4 KB/key**
(61x). All prefix scans unchanged. Multi-block bnodes (values > 4 KB) round-trip
correctly. Reopen + NUL-free scan gate holds. Zero test-suite regressions
(the 3 `test_encryption` failures and all `*_async` failures are pre-existing
on 0.1.11 — environmental, unrelated to this change).

## Mechanism 2 — Overwrite CoW bloat (DEFERRED)

### Root cause

WaveDB is copy-on-write: updating a bnode writes it to a **new** file offset and
marks the old offset stale (`database.c:610` CoW, `database.c:624`
`page_file_mark_stale` → `stale_region_add`). Stale regions are tracked in the
`stale_region_mgr` but **never reclaimed**:

- `page_file_get_reusable_blocks` (`src/Storage/page_file.c:640`) — **zero
  callers** (dead code). Intended to hand out previously-stale blocks for reuse.
- `page_file_alloc_block` (`src/Storage/page_file.c:225`) — dead code; reserves
  a full block via `pf->cur_offset = pf->block_size` but is never called by the
  live write path.
- `bnode_cache_flush_dirty` (`src/BTree/bnode_cache.c:492`) — dead code (zero
  callers).
- `stale_region_clear` (`src/Storage/stale_region.h`) — header comment says
  "used after compaction," but **compaction was never implemented**. The
  `compact_wal_files` path only handles `.wal` files, never `data.wdbp`.

`page_file_open` (`src/Storage/page_file.c:210-219`) repositions `cur_bid` /
`cur_offset` to EOF on reopen → append-only forever. `ftruncate` is only used to
**grow** the file. No page-file vacuum/compaction pass exists, so stale CoW
regions accumulate for the life of the file.

### Symptom

Overwriting the same keys (same paths, new values) grows the file ~linearly with
the number of overwrite passes — old CoW copies are never reused. MVCC
`tx_manager_gc` only prunes in-memory version chains, never page-file blocks;
GC is never auto-scheduled (only fires inside `database_snapshot`).

### Reproduction

```python
# _probe_reclaim.py — scratch, not committed
import shutil, tempfile
from pathlib import Path
import wavedb
base = Path(tempfile.mkdtemp(prefix="hippo_reclaim_"))
db = wavedb.WaveDB(str(base))
N = 500
keys = [f"memory/spo/ep_{i:06d}/has_entity/entity_0" for i in range(N)]
db.put_sync(keys[0], b"seed")
for i, k in enumerate(keys):
    db.put_sync(k, f"v{i}".encode())
db.close()
print(f"after initial write of {N} keys: "
      f"{(base/'data.wdbp').stat().st_size/1024/1024:.2f} MB")
for rep in range(1, 6):
    db = wavedb.WaveDB(str(base))
    for i, k in enumerate(keys):
        db.put_sync(k, f"overwrite-{rep}-{i}".encode())
    db.close()
    print(f"after overwrite pass {rep}: "
          f"{(base/'data.wdbp').stat().st_size/1024/1024:.2f} MB")
shutil.rmtree(base)
```

Observed (0.1.11, before sub-block packing): initial 500 keys = 10 MB, then
+10 MB per overwrite pass → 60 MB after 5 overwrites (122.8 KB/key). The growth
is ~linear in overwrite count; reclamation is dead. (With the 0.1.12 sub-block
packing fix the *absolute* sizes drop ~60x, but the *linear growth with
overwrites* persists — mechanism 2 is orthogonal to mechanism 1.)

### Proposed fix (two options, not yet implemented)

**Option A — Reuse-during-write (interleaved allocation).** Wire
`page_file_get_reusable_blocks` / `stale_region_get_reusable` into
`page_file_write_node` so a new bnode first tries to allocate from a stale
region of sufficient size before falling back to the append cursor. This keeps
the file from growing during updates without an offline pass. **Risk: high.**
It changes *where* bnodes get written (not just block-advance timing), so it
must be correct under concurrent writes, reopen, and MVCC visibility (a stale
region can only be reused once no live read txn can see the old bnode —
otherwise a read could see a reused block mid-overwrite). Needs a free-space
buddy/slab allocator over stale regions and coordination with the tx manager's
GC of version chains.

**Option B — Vacuum/compaction pass (coordinated rewrite).** Implement the
stubbed `stale_region_clear` "used after compaction" path: a periodic or
on-demand pass that, at a safe point (no in-flight writes; quiescent txns),
walks every live bnode from the root and rewrites it to a **fresh** `data.wdbp`,
dropping all stale regions, then swaps the files and rewrites the superblock.
**Risk: medium.** Simpler to reason about than interleaved allocation (one
coordinated rewrite vs. mutating the allocation path), but needs a safe point,
doubles disk briefly during compaction, and must correctly remap every
`disk_offset` / `child_disk_offset` in the rewritten trie. `page_file_open`'s
EOF-reposition logic must be reset to the compacted size.

### Recommendation

Start with **Option B (vacuum/compaction pass)** as a manually-triggered or
snapshot-triggered operation — it is easier to verify correct (deterministic
rewrite, testable by comparing live-key sets before/after) and does not risk
read-during-overwrite corruption. Option A can follow if update throughput
demands it. Both must be gated on MVCC quiescence (no read txn can reference a
block being reclaimed).

### Why it was not needed for the 0.1.12 corpora

The Hippo reload-compaction (writing a fresh DB from the existing extracted
triples) writes each key exactly **once** — no overwrites, no CoW garbage — so
mechanism 2 does not arise. The 0.1.12 sub-block packing fix alone takes the
DialogSum corpus from 4.69 GB to ~60 MB. Reclamation becomes relevant only once
the Ponder Engine runs an update workload in production.

## References

- `src/Storage/page_file.c:260` — `page_file_write_node` (live write path; 0.1.12 fix here)
- `src/Storage/page_file.c:388` — the block-advance condition (the 0.1.12 change)
- `src/Storage/page_file.c:418` — `page_file_read_node` (read path; sub-block-safe)
- `src/Storage/page_file.c:225` — `page_file_alloc_block` (dead code)
- `src/Storage/page_file.c:640` — `page_file_get_reusable_blocks` (dead code, zero callers)
- `src/Storage/stale_region.h` — `stale_region_get_reusable` / `stale_region_clear` (clear stubbed for compaction)
- `src/Database/database.c:574` — `database_flush_dirty_bnodes` (CoW at :610, mark_stale at :624)
- `src/HBTrie/hbtrie.c:2262` — `hbtrie_insert` (one hbtrie_node per chunk)
- `src/Storage/node_serializer.c:763` — `bnode_serialize_v3` (compact serialization)