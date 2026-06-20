# Known Issues

## C-Level Limitations

### Scan iterator pads keys with null bytes

`database_scan_range_sync_raw` returns keys with per-subscript null padding
(e.g., `users/u0` comes back as `b'users\x00\x00\x00/u0\x00\x00'`). The Python
binding strips this padding in `iterator._strip_chunk_padding`, which works for
UTF-8 string keys but would mangle binary keys with intentional trailing nulls.

**Root cause:** `build_identifier_from_chunks` in `src/Database/database_iterator.c`
sets `id->length` to the padded size, not the original byte count.

**Workaround:** Use string keys. Binary keys with trailing nulls are not supported
in v1.

### GraphQL scan queries in subtree mode return wrong entity IDs

`{ User { name } }` (no `id` argument) in subtree mode returns wrong entity IDs
because `database_subtree_scan_start` doesn't strip the subtree prefix from
scanned paths.

**Root cause:** `database_subtree_scan_start` returns a plain iterator without
prefix stripping; the GraphQL resolver expects prefix-stripped paths.

**Workaround:** Use id-argument queries (`User(id: "1") { name }`) which bypass
the scan path.

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