# Page-File Reclamation: Vacuum/Compaction Pass

**Date:** 2026-07-07
**Status:** Design — supersedes the "Deferred" section of `docs/page-file-reclamation-tech-debt.md` (mechanism 2).
**Scope:** Implement the deferred reclamation pass (Option B from the tech-debt doc) plus revive `stale_region` persistence so reclamation is effective across process restarts.

## TL;DR

WaveDB is copy-on-write: every update writes bnodes to a fresh file offset and marks the old offset stale. Stale regions are tracked but never reclaimed, so update workloads grow the page file ~linearly with overwrite count. Several reclamation primitives (`page_file_get_reusable_blocks`, `stale_region_serialize/deserialize`, `bnode_cache_flush_dirty`) are dead code with zero callers. This spec defines a coordinated **vacuum pass** that walks the live trie, rewrites every live bnode to a fresh `data.wdbp.vacuum.tmp`, atomically renames it over `data.wdbp`, and persists the (now empty) stale region manager in the new file's superblock.

Three trigger surfaces (manual API, snapshot threshold, background worker) feed the same core. Writers and readers **block** on a condition variable during the vacuum window rather than bouncing with `-EBUSY`; the only `-EBUSY` returns are open-cursor refusal and drain-timeout abort.

## Background

See `docs/page-file-reclamation-tech-debt.md` for the full reproduction, root-cause analysis, and the 0.1.12 sub-block packing fix that addressed mechanism 1 (initial-load structural bloat). Mechanism 2 (overwrite CoW bloat) is what this spec implements.

**Confirmed dead code (zero callers as of 2026-07-07):**
- `page_file_get_reusable_blocks` (`src/Storage/page_file.c:658`)
- `page_file_alloc_block` (`src/Storage/page_file.c:229`)
- `stale_region_serialize` / `stale_region_deserialize` (`src/Storage/stale_region.c:144`, `:166`) — **stale regions are NOT persisted across reopen**; on every reopen the stale mgr resets to empty and previously-tracked stale bytes become permanently orphaned. Fixing this is in scope.
- `bnode_cache_flush_dirty` (`src/Storage/bnode_cache.c:492`)

**Confirmed reader behavior:** `bnode_cache.c:341` calls `page_file_read_node(offset)` directly, and cursors cache `child_disk_offset` values (`database_iterator.c:311`, `:505`). Readers **do** dereference raw disk offsets, so the vacuum window must exclude concurrent readers.

## Architecture

### Trigger surfaces

Three trigger surfaces feed one core `page_file_vacuum()`:

1. **Manual API** — `database_vacuum(db)` synchronous call. Always available regardless of `vacuum_mode`. Exposed in Node.js / Dart / Python bindings as `vacuum()`.
2. **Snapshot threshold** — `database_snapshot()` checks `page_file_stale_ratio()` ≥ `vacuum_config.stale_threshold` and runs vacuum instead of plain `database_flush_dirty_bnodes` when crossed. Gated by `vacuum_mode` having `STRICT` or `ADAPTIVE`.
3. **Background worker** — a timer task scheduled via the existing work_pool / timing wheel fires every `vacuum_config.background_interval_ms`; if stale ratio ≥ threshold and mode allows, runs vacuum.

### Modes

```c
typedef enum {
    VACUUM_MODE_MANUAL_ONLY = 0,
    VACUUM_MODE_STRICT      = 1,
    VACUUM_MODE_ADAPTIVE    = 2,
} vacuum_mode_t;
```

- `MANUAL_ONLY` — only `database_vacuum()` calls. Snapshot threshold + background worker are no-ops.
- `STRICT` — snapshot threshold + background worker both active; run when threshold crossed regardless of system load.
- `ADAPTIVE` — same as STRICT but the background worker skips a tick when `work_pool_queue_len > adaptive_busy_threshold` (don't compete with the writer). Snapshot-triggered vacuum still runs in ADAPTIVE (caller already paid for the snapshot).

Default: `STRICT`, with `stale_threshold=0.30`. Users opt down to `MANUAL_ONLY` or up to `ADAPTIVE`.

### Configuration

Extend `database_config_t` with `vacuum_config_t`:

```c
typedef struct {
    vacuum_mode_t mode;                    // default STRICT
    double         stale_threshold;         // 0.0-1.0; default 0.30
    uint64_t       min_file_size_bytes;     // don't vacuum below this; default 64 MB
    uint64_t       min_stale_bytes;         // don't vacuum if less stale; default 16 MB
    uint32_t       background_interval_ms;  // default 60000 (0 = disabled)
    uint32_t       drain_timeout_ms;       // halt→drain wait; default 5000
    uint32_t       cursor_close_wait_ms;   // auto-trigger wait for cursors to close; default 60000
    uint32_t       max_runtime_ms;          // hard cap on a single vacuum; default 30000 (0 = no cap)
    uint32_t       writer_block_timeout_ms; // 0 = block forever; default 0
    uint32_t       adaptive_busy_threshold; // queue length to skip adaptive tick; default 32
} vacuum_config_t;
```

Persisted in the existing CBOR config map (`database_config_save`). Missing map → defaults. Backward-compatible.

### Core algorithm: bottom-up post-order rewrite

The rewrite is a single post-order walk. The key insight: a parent bnode's serialized bytes contain its children's `child_disk_offset` values, so a parent can only be serialized **after** all its children are written and their new offsets known. The post-order walk guarantees this.

**Step-by-step:**

1. **Quiesce** (see "Quiescence protocol" below).
2. **Open** `data.wdbp.vacuum.tmp` via a fresh `page_file_t*` with the same `block_size`, `num_superblocks`, and `encryption_t*` as the source.
3. **Walk** the live trie from `db->trie->root`, post-order (leaves → interior → root). For each `bnode_t` reachable via in-memory `bnode_entry_t.child` pointers:
   a. Serialize via `bnode_serialize_v3` (same path as `database_flush_dirty_bnodes`).
   b. `page_file_write_node` to the new file; record `old_disk_offset → new_disk_offset` in a remap hashmap (`offset_remap_t`, simple open-addressing hashmap keyed by `uint64_t`).
   c. Patch the **parent's** in-memory `bnode_entry_t.child_disk_offset = new_offset` (the parent hasn't been serialized yet — it will be when we visit it).
   d. Update `bnode->disk_offset = new_offset` in memory.
4. **Write superblock** to the new file with the new root offset and current `transaction_id_t`. Persist the (empty) stale_region_mgr via the new superblock extension (see "Stale region persistence" below).
5. **fsync** the new file.
6. **Atomic publish** under `db->lock`:
   a. Close the old `page_file_t`'s fd.
   b. `rename("data.wdbp.vacuum.tmp", "data.wdbp")` (atomic on POSIX; on Windows, replace via `MoveFileExA(..., MOVEFILE_REPLACE_EXISTING)`).
   c. Open the new file's fd in the existing `page_file_t`.
   d. Reset `pf->cur_bid` / `pf->cur_offset` to the new EOF.
   e. Replace `pf->stale_mgr` with the new (empty) one.
7. **Resume** — broadcast `db->vacuum_cvar`, clear `db->vacuum_in_progress`.

**Encryption:** `page_file_write_node` already encrypts transparently via `pf->encryption`. The new page_file gets the same `encryption_t*` pointer; values are re-encrypted with the same key on write. No special handling.

**Multi-block bnodes:** `page_file_write_node` already handles multi-block chains. No special case in vacuum.

### Quiescence protocol: halt-block-resume

The vacuum must not run while a reader can dereference an old `disk_offset`. The protocol:

1. **Halt new work** — set `db->vacuum_in_progress = 1` (atomic). New work that would touch the trie blocks:
   - `work_pool_enqueue` paths (async put/delete/graph ops)
   - `database_put_sync` / `database_get_sync` / `database_delete_sync`
   - `tx_manager_begin_read`
   - `database_iterator_create`
   
   All of these check `vacuum_in_progress`; if set, they `platform_lock(&db->vacuum_writer_lock)` + `platform_condition_wait(db->vacuum_cvar, ..., writer_block_timeout_ms)`. Default `writer_block_timeout_ms = 0` (wait forever); if nonzero and timeout hits, return `-ETIMEDOUT`.

2. **Cursor handling** — if `db->open_cursor_count > 0` when vacuum is requested:
   - **Manual API** (`database_vacuum()`): return `-EBUSY` immediately. A sync caller doesn't want to block on an unknown-duration cursor close; they re-call after closing cursors.
   - **Snapshot threshold / background worker**: do **not** skip the tick. Wait on `db->cursor_cvar` up to `cursor_close_wait_ms` (default 60000 = 60s). When the last cursor closes (cursor destroy broadcasts `cursor_cvar`), wake up and proceed to step 3 immediately — vacuum fires within milliseconds of cursor close, not on the next tick. If `cursor_close_wait_ms` elapses with cursors still open, give up this tick and reschedule normally.
   - **Race window**: between "count drops to 0, broadcast" and "vacuum wakes, sets `vacuum_in_progress=1`", a new cursor could squeeze in. Mitigate with a wait loop: `while (open_cursor_count > 0) condition_wait(cursor_cvar, ..., cursor_close_wait_ms)`. Once the loop exits, hold `cursor_count_mutex` while setting `vacuum_in_progress = 1`, which blocks new cursor creation before the race window reopens.
   
   This avoids the "indefinitely blocked" failure mode: as long as the user eventually closes their cursors, vacuum fires promptly. The tick interval is only the fallback for cursors that stay open beyond `cursor_close_wait_ms`.

3. **Drain in-flight (bounded)** — wait up to `drain_timeout_ms` (default 5000) for:
   - `work_pool` queue empty (`work_pool_wait_for_idle_signal`)
   - `eviction_in_flight == 0` (existing atomic counter pattern at `database.c:1373`)
   - All active MVCC read txns closed
   
   If timeout exceeds: **abort**. Do not force-kill in-flight work — that would corrupt the in-memory trie. Clear `vacuum_in_progress`, broadcast condvar, return `-EBUSY`. Blocked writers resume; the vacuum just didn't happen. Caller can retry.

4. **GC version chains** — call `tx_manager_gc(db->tx_manager)` to prune unreachable version chains. Anything still referenced by a live read txn stays in the trie and gets copied to the new file as live data. No special pinning logic — GC is authoritative for what's reachable.

5. **Do the rewrite** (post-order, above).

6. **Atomic publish** (above) — under `db->lock`, patch in-memory offsets, swap fd, reset pf state, broadcast condvar, clear flag.

For `sync_only` mode: no work pool, no tx_manager, no concurrent readers. Steps 1-4 collapse; vacuum is effectively single-threaded.

### Stale region persistence

Currently `stale_region_serialize` / `stale_region_deserialize` are dead code — the stale mgr is in-memory only and resets to empty on every reopen. Fix as part of this work:

**Approach:** store the serialized stale_region blob in the superblock's own block, in the bytes after the fixed 72-byte `page_superblock_t` layout. The superblock occupies a full 4096-byte block but only uses 72 bytes; bytes [72, block_size) are unused today. Extend `page_superblock_t` with two fields:

```c
uint64_t stale_region_offset;  // byte offset within the superblock's block where stale_region blob starts (0 = none)
uint64_t stale_region_size;    // byte length of the stale_region blob
```

These two fields are appended to the fixed superblock layout (after `crc32`, before padding) and covered by the CRC. On `page_file_write_superblock`:

1. Serialize `pf->stale_mgr` via `stale_region_serialize` → `(blob, blob_len)`.
2. If `blob_len + 72 + 16 ≤ block_size` (fits in the superblock's slack space): set `stale_region_offset = 72 + 16`, `stale_region_size = blob_len`, pwrite the blob to `slot * block_size + stale_region_offset`.
3. If too large (rare — would need ~4000 bytes of stale regions, i.e. ~250 merged stale regions): fall back to writing the blob to a fresh block at EOF, set `stale_region_offset` to that block's byte offset, `stale_region_size` accordingly. (The blob is just `count + total + array of {offset,length}` — 16 + 16N bytes — so the inline case handles up to ~250 regions, which covers normal operation.)

On `page_file_open` (existing file path): read the latest valid superblock, then `pread(pf->fd, blob, stale_region_size, slot * block_size + stale_region_offset)` and `stale_region_deserialize`. Replace `pf->stale_mgr` with the result. On parse error (CRC mismatch on the blob, or implausible count): start with an empty mgr and log a warning — better to lose stale tracking than to crash.

**Backward compatibility:** existing files written before this change have `stale_region_offset = 0` and `stale_region_size = 0` (the bytes are zero-padded). `page_file_open` treats `0` as "no persisted stale regions" — mgr starts empty (current behavior; no regression). New files always populate the fields.

**Vacuum interaction:** the new file's stale mgr starts empty (we just dropped all stale regions), so the persisted form is the trivial "count=0" record.

### File lifecycle

```
data.wdbp                        ← live file
data.wdbp.vacuum.tmp              ← transient; exists only during a vacuum pass
```

**Crash recovery on `page_file_open`:** if `data.wdbp.vacuum.tmp` exists, delete it (orphan from a previous crashed vacuum). This runs before the existing file-size check.

## Components

### New / modified files

| File | Change |
|---|---|
| `src/Storage/page_file.h` | Add `page_file_vacuum()`; add `stale_region_offset` to `page_superblock_t`; declare `page_file_open_vacuum_tmp()` helper. |
| `src/Storage/page_file.c` | Implement `page_file_vacuum()` core (or split into `page_file_vacuum.c`? — see "Open question" below). Persist stale mgr in superblock. Cleanup `*.vacuum.tmp` on open. |
| `src/Util/offset_remap.h` / `.c` | New: simple open-addressing hashmap `uint64_t → uint64_t` for old→new offset remap. |
| `src/Database/database_config.h` | Add `vacuum_config_t`, `vacuum_mode_t`. |
| `src/Database/database_config.c` | Persist/parse `vacuum_config` in CBOR config map. |
| `src/Database/database.h` | Add `database_vacuum()`, `vacuum_in_progress`/`vacuum_cvar`/`vacuum_writer_lock`/`open_cursor_count` to `database_t`. |
| `src/Database/database.c` | Implement `database_vacuum()`; snapshot threshold hook in `database_snapshot()`; background vacuum task spawn/stop in create/destroy; block writers in `put_sync`/`get_sync`/etc; cursor open/close tracking. |
| `src/Database/database_iterator.c` | Register/unregister cursors in `db->open_cursor_count` (atomic). |
| `bindings/nodejs/` | Expose `vacuum()` method on Database. |
| `bindings/dart/` | Expose `vacuum()` method on WaveDB. |
| `bindings/python/` | Expose `vacuum()` method on WaveDB. |
| `tests/` | New unit tests (see Testing). |
| `benchmarks/benchmark_vacuum.cpp` | New: perf microbenchmark. |

### Open question: where does the core rewrite live?

The post-order walk + remap logic touches `page_file_t`, `hbtrie_t`, `bnode_t`, `bnode_cache`. Options:

- **A:** in `page_file.c` — `page_file_vacuum(pf, root, ...)` takes the trie root and does everything. Pro: keeps storage code together. Con: page_file now depends on hbtrie/bnode (currently it doesn't).
- **B:** in `database.c` — `database_vacuum()` orchestrates the walk and calls `page_file_write_node` directly. Pro: database.c already has `database_flush_dirty_bnodes` with the same pattern; reuse. Con: a bit of layering bleed (database knows about serialization internals).
- **C:** new `src/Storage/vacuum.c` — dedicated module. Pro: clean separation, easy to test in isolation. Con: another file to maintain.

**Recommendation: B** — mirror the existing `database_flush_dirty_bnodes` pattern; it's already in the right layer. `page_file_vacuum()` only adds the *file-level* operations (open tmp, swap, persist stale mgr); the trie walk stays in database.c.

## Data flow

```
database_vacuum(db) / snapshot threshold / background tick
        │
        ├─ check open_cursor_count → if > 0, return -EBUSY (manual) / skip (auto)
        ├─ set vacuum_in_progress=1  ── writers block on condvar ──────┐
        ├─ drain work_pool + eviction_in_flight + read txns          │
        │   (bounded by drain_timeout_ms; abort -EBUSY if exceeded)  │ blocked
        ├─ tx_manager_gc                                              │ writers
        ├─ walk trie post-order:                                       │
        │   for each bnode:                                            │
        │     serialize → write to vacuum.tmp → remap old→new         │
        │     patch parent's child_disk_offset in memory               │
        ├─ write superblock (+ empty stale mgr) to vacuum.tmp         │
        ├─ fsync vacuum.tmp                                            │
        ├─ atomic publish under db->lock:                              │
        │   close old fd → rename vacuum.tmp → data.wdbp → open new   │
        │   reset pf->cur_bid/cur_offset → swap stale_mgr             │
        ├─ clear vacuum_in_progress=0                                  │
        └─ broadcast vacuum_cvar ──────────────────────────────────-──┘
                                                                       │
                                                            writers resume
```

## Error handling

| Failure | Behavior | State after |
|---|---|---|
| Open cursor at vacuum start | Manual: `-EBUSY` (caller retries). Auto: wait `cursor_close_wait_ms` for cursors to close; if they do, proceed; if timeout, skip tick. | Unchanged until cursors close. |
| Drain timeout | Abort. `-EBUSY` returned. | Unchanged. Writers resume. |
| `-EIO` mid-rewrite | Abort. Delete `vacuum.tmp`. | Old file intact. `-EIO` returned. |
| Crash before rename | Old file intact. `vacuum.tmp` orphaned. | Next `page_file_open` deletes `vacuum.tmp`. |
| Crash after rename, before sb write | **Prevented**: superblock written + fsync'd before rename. New file is consistent post-rename. | Reopen reads new file. |
| `writer_block_timeout_ms` exceeded | Blocked writer returns `-ETIMEDOUT`. | Vacuum continues; other writers stay blocked. |
| `max_runtime_ms` exceeded | Abort rewrite. Delete `vacuum.tmp`. `-ETIMEDOUT`. | Old file intact. |
| Vacuum under sync_only mode | Steps 1-4 collapse (no work_pool/tx_manager). Single-threaded. | Same as above. |

## Testing

### Unit tests

- `test_vacuum_basic` — write N=1000 keys, overwrite each 5×, vacuum, file size shrinks ≥ 5×, all keys still readable.
- `test_vacuum_reopen_persistence` — write + overwrite, close, reopen (stale_region persisted via superblock), vacuum, file shrinks. Verifies the persistence fix end-to-end.
- `test_vacuum_open_cursor_refuses` — open cursor, call `database_vacuum()`, expect `-EBUSY`; close cursor, vacuum succeeds.
- `test_vacuum_auto_cursor_close_waits` — open cursor, trigger background vacuum (or simulate snapshot threshold); verify vacuum does NOT run while cursor open; close cursor from another thread; verify vacuum fires within ~10ms of cursor close (well before next tick).
- `test_vacuum_concurrent_writer` — writer thread loops `put_sync`; vacuum thread calls `database_vacuum()`; verify writers blocked during vacuum, resumed after, no corruption (key set intact).
- `test_vacuum_crash_recovery` — simulate crash mid-rewrite (delete `vacuum.tmp` mid-pass via test hook), reopen from old file, verify all keys present, verify `vacuum.tmp` cleaned up.
- `test_vacuum_threshold_strict` — push stale_ratio over 30%, call `database_snapshot()`, verify vacuum ran automatically and file shrunk.
- `test_vacuum_adaptive_skip` — saturate `work_pool_queue_len > 32`, call snapshot-triggered vacuum, verify it ran (snapshot always runs); call background-tick vacuum, verify it skipped.
- `test_vacuum_bindings` — Node.js / Dart / Python expose `vacuum()`; round-trip write/overwrite/vacuum/read.
- `test_vacuum_min_file_size` — file below `min_file_size_bytes`, vacuum refuses with `-ECANCELED` (no-op); above threshold, runs.
- `test_vacuum_max_runtime` — artificially slow rewrite via test hook; verify `-ETIMEDOUT` after `max_runtime_ms`.

### Integration

Extend the existing reopen + NUL-free scan gate (`tests/test_reopen_nulfree_scan.c` or equivalent) to run after a vacuum pass — verify scan still produces same key set post-vacuum.

### Performance benchmark

`benchmarks/benchmark_vacuum.cpp`:
- Write N keys (N ∈ {1k, 10k, 100k, 1M}), overwrite each K times (K ∈ {1, 5, 20, 100}).
- Measure: file size before/after each overwrite pass, vacuum duration, file size after vacuum, ops/sec during normal writes vs. during vacuum drain window.
- Compare 3 modes: `MANUAL_ONLY` (baseline growth, no auto-vacuum), `STRICT` (background 60s), `ADAPTIVE` (background 60s + busy skip).
- Halt-window impact: measure latency spike on `put_sync` during drain. Tune `drain_timeout_ms` from data.
- Rewrite cost: vacuum walks every live bnode once. Verify O(live bnodes), not O(file size).

### Validation gates

- All existing unit tests pass with no regressions.
- `valgrind --leak-check=full` on `test_vacuum_*` shows zero leaks.
- ASAN build passes for `test_vacuum_concurrent_writer` (no races on condvar / atomic flag).
- File size after vacuum ≤ file size after initial load (no growth from vacuum itself).
- Pre-existing `*_async` test failures and `test_encryption` failures (per the 0.1.12 doc note) remain unchanged — not regressions.

## Performance notes & open questions

1. **Default `drain_timeout_ms=5000`** is a guess. Measure under realistic write load and tune. If drains consistently complete <500ms, lower to 1000. If they often hit 5s, the abort rate will be high — consider raising or switching default mode to ADAPTIVE.

2. **Default `max_runtime_ms=30000`** — for a 100k-bnode DB the rewrite should be well under 1s. 30s is a safety valve for pathological cases. Measure and tighten.

3. **Adaptive busy threshold = 32** queue depth is a guess. The right value depends on the user's writer throughput and work_pool size. Document that this is tunable; users with sustained heavy write load should consider MANUAL_ONLY.

4. **Should background vacuum back off if it aborts N times in a row?** Probably yes — log a warning and double the interval up to a cap (exponential backoff). Open for the implementation plan to decide.

5. **Memory cost during vacuum** — the remap hashmap holds one entry per live bnode. For 100k bnodes, ~1.6 MB (16 bytes per entry, 50% load factor). Bounded and acceptable. For very large DBs (10M+ bnodes), consider a disk-spilled remap, but defer until measured.

## References

- `docs/page-file-reclamation-tech-debt.md` — original tech debt write-up
- `src/Storage/page_file.c:264` — `page_file_write_node` (live write path; the 0.1.12 sub-block packing fix)
- `src/Storage/page_file.c:436` — `page_file_read_node` (read path; sub-block-safe)
- `src/Storage/page_file.c:658` — `page_file_get_reusable_blocks` (dead; not used by this design — Option A territory)
- `src/Storage/page_file.c:686` — `page_file_write_superblock` (extended for stale_region_offset)
- `src/Storage/stale_region.c:144` — `stale_region_serialize` (revived; wired into superblock)
- `src/Database/database.c:602` — `database_flush_dirty_bnodes` (existing CoW flush; pattern to mirror)
- `src/Database/database.c:1816` — `database_snapshot` (snapshot threshold hook point)
- `src/Storage/bnode_cache.c:341` — `page_file_read_node` direct call (why quiescence is required)
- `src/Database/database_iterator.c:311`, `:505` — cursor cached `child_disk_offset` (why cursors block vacuum)
- `src/Workers/pool.c:89` — `work_pool_wait_for_idle_signal` (drain primitive)