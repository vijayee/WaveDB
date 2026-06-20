# Known Issues

## C-Level Limitations

### ~~Scan iterator pads keys with null bytes~~ (FIXED)

**Status: Fixed.** The C layer now stores per-subscript {chunk_count, byte_length}
metadata (`path_meta` on `bnode_entry_t`, serialized via the V3 0x10 flag bit).
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

### `enable_persist=False` does not disable WAL persistence

Setting `WaveDBConfig(enable_persist=False)` does NOT prevent data from surviving
close/reopen. The C layer gates WAL on `location != NULL`, not on `enable_persist`.
True in-memory mode requires `location == NULL`, which the Python binding rejects
(via `InvalidPathError` on empty paths).

**Root cause:** `enable_persist` only controls the page-file layer, not WAL.

**Workaround:** Use a fresh `tmp_path` for each test if you need isolation. True
in-memory mode is not supported in v1.

## Async Error Messages Lack Source Location

`async_error_t` carries `file`/`function`/`line` fields (see `src/Workers/error.h`),
but the C API only exposes `error_get_message` — no accessors for the location
fields. The binding's async error messages contain only the message string,
not the C source location where the error originated.

**Root cause:** Missing C accessors (`error_get_file`, `error_get_function`,
`error_get_line`). Declaring `async_error_t` non-opaque in the cdef would
require knowing `refcounter_t`'s layout, which is fragile across platforms.

**Workaround:** The error message is sufficient for most debugging. For
C-source-level tracing, use the sync API (which has integer error codes) or
check the C logs.