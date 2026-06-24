# Known Issues

## C-Level Limitations

### ~~Scan iterator pads keys with null bytes~~ (FIXED)

**Status: Fixed.** The C layer now stores per-subscript {chunk_count, byte_length}
metadata (`path_meta` on `bnode_entry_t`, serialized via the V3 `0x10` flag bit).
The iterator reconstructs keys at their exact original byte length — no null
padding. Binary keys with real trailing nulls are preserved exactly.

Old on-disk files written before the fix fall back to the legacy padded path.
Users with such data should re-insert to get exact keys.

The Python binding's `_strip_chunk_padding` workaround has been removed from
the scan path (the function is retained as deprecated reference only).

### ~~GraphQL scan queries in subtree mode return wrong entity IDs~~ (FIXED)

**Status: Fixed.** The subtree scan iterator now strips the prefix from result
paths via a `prefix_skip` field on `database_iterator_t`. `database_subtree_scan_start`
and `database_subtree_scan_range` set `prefix_skip` to the number of prefix
components, and `database_scan_next` skips those identifiers when building
result paths. The GraphQL resolver receives subtree-relative paths and
extracts entity IDs correctly.

The mutation-delete double-prefix bug (iterator yields prefixed paths,
`database_subtree_delete_sync` prepends again) is also fixed — the iterator
now yields stripped paths, so delete prepends once.

Note: prefix stripping only works for entries with `path_meta` (new data
written after the scan padding fix). Legacy entries without metadata still
include the prefix in scan results.

### ~~`enable_persist=False` does not disable WAL persistence~~ (FIXED)

**Status: Fixed.** Added `in_memory: bool = False` to `WaveDBConfig`. When `True`,
the binding passes `location=NULL` to `database_create_with_config`, which sets
`is_memory_only=true` in the C layer — no WAL, no page file, no config save.
Data is truly ephemeral and lost on close.

`enable_persist=False` retains its original meaning (skip page-file setup, WAL
still active). Use `in_memory=True` for true ephemeral mode.

### ~~Scan crash with 50+ keys~~ (FIXED)

**Status: Fixed.** The scan iterator (`database_scan_next`) did not handle
internal B+tree nodes (`is_bnode_child=1` entries created by bnode splits).
It misinterpreted `bnode_t*` as `hbtrie_node_t*`, causing type confusion
and a segfault when accessing `node->btree`. Fix: added `bnode_t*` support
to `iterator_frame_t` with `push_bnode_frame()` for descending through
multi-level B+trees. Verified with scans up to 40K keys.

### ~~Memory pool corruption with multiple database lifecycles~~ (FIXED)

**Status: Fixed.** `hbtrie_null_entries_by_offset` called `free()` on
`hbtrie_combined_t` blocks that were allocated via `memory_pool_alloc`. The
blocks live in the memory pool's static BSS arrays — calling `free()` on a
BSS pointer corrupts glibc's free list metadata, which overwrites adjacent
pool blocks' `next` pointers. Fix: replaced `free(evicted)` with
`memory_pool_free(container_of(evicted, hbtrie_combined_t, node),
sizeof(hbtrie_combined_t))` at both call sites. Verified: full benchmark
(4 WAL modes) completes under ASAN with zero crashes.

## ~~Async Error Messages Lack Source Location~~ (FIXED)

**Status: Fixed.** Added `error_get_file`, `error_get_function`, `error_get_line`
accessors to the C API (`src/Workers/error.h`/`.c`). The Python binding's
`_async.py:_on_reject` now enriches async error messages with C source location:
`message (file:line in function)`.

## All Known Issues Resolved

No remaining known issues.