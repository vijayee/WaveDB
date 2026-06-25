# WaveDB Python Bindings — Design

Date: 2026-06-19
Status: Draft (pending user review)
Owner: Victor Morrow

## Goal

Provide Python bindings for WaveDB with feature parity to the existing Node.js
(`bindings/nodejs/`) and Dart (`bindings/dart/`) bindings: WaveDB core, Subtree,
GraphLayer, GraphQLLayer, iterator/stream, object ops, encryption — in both
synchronous and asynchronous variants.

## Non-goals

- New C/C++ wrapper code. The binding reuses the existing shared library
  (`libwavedb.so` / `libwavedb.dylib` / `wavedb.dll`) built from `src/`.
- Prebuilt binary wheels for every platform. v1 ships an sdist that builds the
  shared library at install time; prebuilt wheels via cibuildwheel are a
  follow-up.
- Out-of-line cffi mode. The binding uses cffi ABI mode (pure runtime
  `ffi.cdef()`), keeping the binding layer pure-Python.

## Decisions

| Axis | Decision |
| --- | --- |
| FFI technology | cffi, ABI mode, reuse `libwavedb.so` |
| Async model | Sync methods wrap `database_*_sync`. Async methods (`async def put`, etc.) drive the C `work_pool_t` via `promise_t` and marshal results back to the calling asyncio loop via `loop.call_soon_threadsafe()`. |
| Python versions | 3.10+ |
| Packaging | `pyproject.toml` sdist that runs CMake at install time and bundles the produced `.so` inside the wheel via `importlib.resources` |
| v1 scope | Full parity with Node/Dart (WaveDB + Subtree + GraphLayer + GraphQLLayer + iterator + object ops + encryption) |
| Naming | snake_case (`put_sync`, `get_sync`, `open_subtree`, `create_read_stream`); async methods drop the `_sync` suffix (`async def put`, `async def get`) |
| Missing key behavior | `get_sync` and `async get` return `None` for missing keys (matches Node default). v1 always returns `None`; opt-in `NotFoundError` raise is a follow-up. |

## Architecture

### Package layout

```
bindings/python/
├── pyproject.toml                # PEP 517 build backend; build_ext invokes CMake
├── setup.py                      # shim for build_ext that runs cmake + bundles .so
├── README.md
├── LICENSE
├── src/
│   └── wavedb/
│       ├── __init__.py           # re-exports public API
│       ├── _native.py            # cffi loader: ffi.cdef() + library path resolution
│       ├── _async.py             # Promise->asyncio bridge: callback registry, loop marshal
│       ├── _errors.py            # error code -> exception mapping
│       ├── exceptions.py         # WaveDBError, NotFoundError, InvalidPathError, IOError_, EncryptionError
│       ├── config.py             # WaveDBConfig, WaveDBEncryption dataclasses
│       ├── path.py               # path_t / identifier_t builders and converters
│       ├── database.py           # WaveDB class (sync + async + context managers)
│       ├── iterator.py           # create_read_stream: sync generator + async iterator
│       ├── object_ops.py         # put_object / get_object (nested dict <-> flattened paths)
│       ├── subtree.py            # Subtree class
│       ├── graph_layer.py        # GraphLayer, GraphQuery (g.out(...).has(...))
│       └── graphql_layer.py      # GraphQLLayer, GraphQLResult
└── tests/
    ├── conftest.py
    ├── test_wavedb.py
    ├── test_batch.py
    ├── test_object_ops.py
    ├── test_iterator.py
    ├── test_subtree.py
    ├── test_mvcc.py
    ├── test_graph.py
    ├── test_graphql.py
    ├── test_async_bridge.py
    ├── test_encryption.py
    └── test_persistence.py
```

### cffi layer (`_native.py`)

- Single module-scoped `FFI()` instance with `ffi.cdef()` declarations for every
  C function and struct used by the binding: `database_t`, `database_config_t`,
  `encrypted_database_config_t`, `path_t`, `identifier_t`, `promise_t`,
  `work_pool_t`, `hierarchical_timing_wheel_t`, `database_iterator_t`, and all
  `database_*` functions exposed by `src/Database/database.h` (sync, raw, async,
  batch, scan, config setters/getters).
- Library resolution order:
  1. Bundled copy under `importlib.resources.files("wavedb._lib")` (wheel install).
  2. `WAVEDB_LIB_PATH` environment variable.
  3. `ctypes.util.find_library("wavedb")` (system install).
  4. Fail with `WaveDBError("could not locate libwavedb")`.
- Import-time sanity check: verify `ffi.sizeof("database_t")` and a handful of
  struct offsets match expected values; fail loud if the loaded library was
  built against an incompatible header version.
- All cffi callbacks are created at module scope and held in a module-level
  registry so they are never garbage-collected while C holds a function
  pointer.

### Async bridge (`_async.py`) — highest-risk module

The C library's async ops work as follows:

1. Caller creates a `promise_t` and passes it to `database_put` / `get` / `del`
   / `batch` along with a callback.
2. C enqueues the op on `work_pool_t`; a worker thread executes it, then invokes
   the promise callback with the result or error.
3. The callback fires **on the C worker thread**, not the Python thread.

The bridge:

- Module-scoped registry `dict[int, PendingOp]` keyed by an integer op-id.
  The C `promise_t` struct (`src/Workers/promise.h`) has a `void* ctx` field
  that `promise_create` forwards as the first argument to the resolve/reject
  callbacks. We allocate a heap-allocated `intptr_t` op-id per async op and
  pass its address as `ctx`; the callback reads the op-id back through that
  pointer. The op-id integer is freed in `_deliver` after the registry lookup
  completes.
- `PendingOp` dataclass holds: `promise_ptr`, `op_id_ptr` (the cffi-allocated
  `intptr_t*`), `future: asyncio.Future`, `loop: asyncio.AbstractEventLoop`,
  `op_type: str`, `cancelled: bool`.
- Each `async def put(...)` (and get/del/batch/put_object/get_object):
  1. Captures `asyncio.get_running_loop()` at call time (never at import time —
     the loop may not exist yet and binding to one loop would break cross-loop
     use).
  2. Allocates a `promise_t` via cffi.
  3. Creates an `asyncio.Future` on the captured loop.
  4. Allocates an op-id (monotonic counter) and a heap `intptr_t*` holding it.
  5. Registers a `PendingOp` in the registry under the op-id.
  6. Calls `promise_create(resolve_cb, reject_cb, op_id_ptr)` — the same shared
     resolve/reject callbacks are used for every op; per-op state is recovered
     via the `ctx` pointer.
  7. Awaits the future.
- The shared resolve and reject cffi callbacks, decorated with
  `@ffi.callback("...")` (which acquires the GIL automatically when fired from
  a C thread):
  1. Reads the op-id from the `ctx` pointer.
  2. Looks up the `PendingOp` in the registry. If absent (already cancelled /
     closed), no-op.
  3. Schedules a loop-bound `_deliver(pending, result_or_err)` via
     `loop.call_soon_threadsafe()`. **Never** touches the Future directly from
     the C thread — `Future.set_result` is not thread-safe and would race with
     the loop.
  4. `_deliver` runs on the loop thread: if `pending.loop.is_closed()`, drop the
     result silently (log at WARNING); otherwise set the future's result or
     exception (mapped through `_errors._map_error`), remove the op from the
     registry, decref/destroy the promise.
- Cancellation: `asyncio.CancelledError` propagates up through `await future`.
  Mark the pending op cancelled in the registry. The C op cannot be cancelled
  mid-flight (the C API has no per-op cancel — matches Node/Dart), so it still
  runs to completion but its result is dropped in `_deliver`. This matches the
  behavior of the existing bindings.
- Close ordering:
  - `close()` (sync): cancels all in-flight futures (sets `CancelledError`),
    then calls `database_destroy` which internally does `wheel_stop ->
    wait_for_idle -> pool_shutdown -> pool_join -> pool_destroy -> wheel_destroy`
    and drains the C work pool. If a worker callback fires after the registry is
    cleared, the lookup no-ops.
  - `aclose()` (async): awaits all in-flight futures (so callers see their
    results or cancellations), then calls `close()`.
- cffi callback lifetime: callbacks are created once at module scope and held
  in a module-level list; they must never be GC'd while C holds a function
  pointer. Tests assert this invariant (no callback object is per-op).

### Public API

#### Exceptions

```python
class WaveDBError(Exception): ...
class NotFoundError(WaveDBError): ...
class InvalidPathError(WaveDBError): ...
class IOError_(WaveDBError): ...        # avoid shadowing builtin IOError
class EncryptionError(WaveDBError): ...
```

`_errors._map_error(code: int, message: str)` maps C error codes
(`DATABASE_ERR_ENCRYPTION_REQUIRED` / `-101` / `-102`) and the string markers
Node uses (`NOT_FOUND`, `INVALID_PATH`, `IO_ERROR`, `DATABASE_CLOSED`) to the
typed exceptions. Every public method wraps its C call in `try/except` and
re-raises via `_map_error`.

#### Config

```python
@dataclass
class WaveDBConfig:
    chunk_size: int = 4
    btree_node_size: int = 4096
    enable_persist: bool = True
    lru_memory_mb: int = 50
    lru_shards: int = 0                # 0 = auto-scale
    wal_sync_mode: str = "debounced"   # "debounced" | "immediate" | "none"
    wal_debounce_ms: int = 250
    worker_threads: int = 0            # 0 = auto
    sync_only: bool = False

@dataclass
class WaveDBEncryption:
    type: str                          # "aes-256-gcm" etc.
    symmetric_key: bytes | None = None
    asymmetric_private_key: bytes | None = None
    asymmetric_public_key: bytes | None = None
```

#### `WaveDB`

```python
class WaveDB:
    def __init__(
        self,
        path: str,
        *,
        delimiter: str = "/",
        config: WaveDBConfig | None = None,
        encryption: WaveDBEncryption | None = None,
    ): ...

    # sync
    def put_sync(self, key: str | list[str], value: bytes | str) -> None: ...
    def get_sync(self, key: str | list[str]) -> bytes | None: ...
    def del_sync(self, key: str | list[str]) -> None: ...
    def batch_sync(self, ops: list[dict]) -> None: ...
    def put_object_sync(self, key: str | list[str], obj: dict) -> None: ...
    def get_object_sync(self, key: str | list[str]) -> dict: ...
    def create_read_stream(
        self,
        *,
        start: str | None = None,
        end: str | None = None,
        limit: int | None = None,
    ) -> Iterator[tuple[str, bytes]]: ...

    # async (coroutines — use the C work_pool)
    async def put(self, key, value) -> None: ...
    async def get(self, key) -> bytes | None: ...
    async def del(self, key) -> None: ...
    async def batch(self, ops) -> None: ...
    async def put_object(self, key, obj) -> None: ...
    async def get_object(self, key) -> dict: ...
    def create_read_stream_async(
        self,
        *,
        start: str | None = None,
        end: str | None = None,
        limit: int | None = None,
    ) -> AsyncIterator[tuple[str, bytes]]: ...

    # subtree
    def open_subtree(self, prefix: str | list[str], delimiter: str = "/") -> Subtree: ...
    def delete_subtree(self, prefix: str | list[str], delimiter: str = "/") -> None: ...

    # lifecycle
    def close(self) -> None: ...
    async def aclose(self) -> None: ...

    def __enter__(self): return self
    def __exit__(self, *exc): self.close()
    async def __aenter__(self): return self
    async def __aexit__(self, *exc): await self.aclose()
```

#### `Subtree`

Same surface scoped under a prefix: `put_sync` / `get_sync` / `del_sync` /
`batch_sync` / `put_object_sync` / `get_object_sync`, async `put` / `get` /
`del` / `batch` / `put_object` / `get_object`, `create_read_stream` /
`create_read_stream_async`, `close` (releases the underlying subtree handle).

Subtree refcounting mirrors Dart — Python holds a refcount on the underlying
subtree handle; `close()` decrefs. GC also decrefs via `__del__` as a safety
net (matches Dart).

#### `GraphLayer` and `GraphQLLayer`

Mirror the Node/Dart surface:

- `GraphLayer(db, ...)`: graph query DSL with a builder (`g.out(...).has(...)
  .in_(...)`), range predicates, intersect/union, cost-based reordering.
- `GraphQLLayer(...)`: GraphQL query execution returning `GraphQLResult`.

Detailed API is in the existing graph/graphql design specs
(`docs/superpowers/specs/2026-05-31-graph-query-language-design.md`,
`2026-06-01-nodejs-graph-bindings-design.md`,
`2026-06-01-dart-graph-bindings-design.md`). The Python binding mirrors the
Dart surface method-for-method with snake_case renames.

### Conversions

- **Keys.** `str` keys split on `delimiter` into `path_t`. `list[str]` keys used
  directly. Empty path → `InvalidPathError`.
- **Values.** `str` values UTF-8 encoded to `bytes` before passing to C. C
  returns `bytes`. `get_sync` and `async get` return `bytes | None`.
- **Object ops.** Pure client-side path flattening (mirrors Dart's
  `ObjectOps.flattenObject`): nested `dict` is walked iteratively; each leaf
  (str/bytes/int/float/bool/None) becomes a batch put with the flattened path.
  `list` becomes integer-indexed subpaths. No CBOR/JSON serialization — leaf
  values are stored as bytes/strings via the regular put/batch API.
  `get_object_sync` / `async get_object` scan the prefix and reconstruct the
  nested dict.
- **Missing key.** `get_sync` / `async get` return `None`; no exception in v1.
  (Opt-in raise via a `require: bool = False` parameter is a follow-up.)
- **Batch ops.** `ops: list[dict]` with `{"type": "put"|"del", "key": ..., "value": ...}`.
  Same shape as Node/Dart.

## Build and packaging

### CMake integration

Add a `BUILD_SHARED_LIB` option to the root `CMakeLists.txt`. The existing
`BUILD_DART_BINDINGS` option already builds a `wavedb_shared` SHARED target
from `${WAVEDB_SOURCES}`; we generalize it so both `BUILD_DART_BINDINGS` and
`BUILD_PYTHON_BINDINGS` imply `BUILD_SHARED_LIB=ON`. The output name remains
`libwavedb.so` / `libwavedb.dylib` / `wavedb.dll`. No new C/C++ code is needed
— the shared library is identical regardless of which binding consumes it.

### Install-time build

`pyproject.toml` uses setuptools as the build backend with a custom `build_ext`
that:

1. Checks `WAVEDB_LIB_PATH` and `WAVEDB_USE_SYSTEM_LIB=1`. If the user has
   pointed at a pre-built library, skip the build and copy that library into the
   wheel.
2. Otherwise invokes `cmake -S <repo root> -B <build dir> -DBUILD_SHARED_LIB=ON`
   then `cmake --build <build dir> --config Release`.
3. Copies the produced `.so` / `.dylib` / `.dll` into the wheel under
   `wavedb/_lib/` and ships it via `importlib.resources.files("wavedb._lib")`.
4. `_native.py` resolves the library at runtime in the order described in the
   cffi layer section.

The sdist must include the full `src/`, `deps/` (xxhash, hashmap, libcbor), and
`CMakeLists.txt` so the install-time build has everything it needs without a
network fetch. `MANIFEST.in` enumerates these explicitly.

### Development workflow

During development, point `WAVEDB_LIB_PATH` at the Dart-built
`bindings/dart/libwavedb.so` (or a Release-flagged build from
`build-release/libwavedb.a` relinked as shared — the Dart shared lib is already
correct). No rebuild per Python edit. CI rebuilds the shared lib fresh.

## Testing

Framework: `pytest` + `pytest-asyncio`.

Tests live under `bindings/python/tests/`:

- `test_wavedb.py` — put/get/del sync + async, key forms (str/list/delimiter),
  value forms (str/bytes/large), missing key → `None`.
- `test_batch.py` — `batch_sync` and async `batch`, mixed put/del.
- `test_object_ops.py` — nested dict → flattened paths round-trip.
- `test_iterator.py` — range scan, `limit`, sync generator + async iterator.
- `test_subtree.py` — open/delete, scoped ops, refcount via `__del__`.
- `test_mvcc.py` — overwrites, deletes (tombstones), version chains, no crash on
  close (the Node MVCC serialization fix is exercised here too).
- `test_graph.py` — GraphLayer query DSL, range predicates, intersect/union.
- `test_graphql.py` — GraphQLLayer queries, result shape.
- `test_async_bridge.py` — concurrent ops across tasks, cancellation, callback
  fired after loop close, pending op at `close()`, GIL contention under load
  (1000 concurrent puts). **Highest-risk module, gets the most coverage.**
- `test_encryption.py` — encrypted DB open, invalid key → `EncryptionError`,
  unsupported type → `EncryptionError`.
- `test_persistence.py` — close/reopen, WAL recovery. The known thread-local
  WAL caveat from Node applies: `database_snapshot()` is disabled during close
  and persistence is via WAL recovery on next open; sync ops recommended for
  persistence-critical tests. Tests assert data survives close/reopen via WAL
  replay, not via snapshot.

### Leak criteria

ASAN/valgrind pass criteria match the C/Node/Dart suites: zero WaveDB-originated
leaks. Known acceptable leaks (carried over from the existing binding memory
audit) are the 32-byte Node.js `ContextifyScript` leak (N/A here) and the
120-byte persistence batch-delete version chain leak. The Python binding should
introduce **no new leaks** beyond what already exists in the C library.

## Risks and mitigations

| Risk | Mitigation |
| --- | --- |
| cffi callback GC'd while C holds a function pointer | All callbacks created at module scope, held in a module-level list; test asserts no per-op callback is allocated. |
| `Future.set_result` called from C worker thread (race with loop) | Callback schedules `_deliver` via `loop.call_soon_threadsafe()`; Future is only touched on the loop thread. |
| Loop closed before delivery | `_deliver` checks `pending.loop.is_closed()` and drops the result silently (WARNING log). |
| Pending op at `close()` | `close()` cancels all in-flight futures; `database_destroy` drains the work pool. Worker callbacks after close find an empty registry and no-op. |
| GIL contention under load | Sync `_sync` calls release the GIL around the C call (cffi does this by default for `void`/non-Python calls). Async ops run on the C work pool; the GIL is only held during the brief callback dispatch into `call_soon_threadsafe`. |
| cffi `cdef` drift from C headers | Import-time `ffi.sizeof` / offset sanity checks fail loud on mismatch. CI runs against a freshly-built lib. |
| Wheel bundling `.so` for wrong platform | sdist only (no prebuilt wheels in v1); install always rebuilds for the host platform. Prebuilt wheels are a follow-up. |
| Thread-local WAL caveat (carried over from Node) | `database_snapshot()` not called during close; persistence tests assert WAL recovery, not snapshot. Documented in README. |

## Out of scope for v1 (follow-ups)

- Prebuilt binary wheels via cibuildwheel (manylinux, macOS, Windows).
- Opt-in `require=True` parameter on `get` / `get_sync` to raise
  `NotFoundError` instead of returning `None`.
- Sync-mode-only fast path (no `work_pool_t` overhead) — already available via
  `WaveDBConfig(sync_only=True)`.
- Type stubs (`.pyi`) for IDE auto-completion — straightforward follow-up once
  the API stabilizes.
- asyncio.TaskGroup-based batch helpers — callers can compose these themselves
  from the existing async API.

## Acceptance criteria

- All `bindings/python/tests/` tests pass under pytest + pytest-asyncio.
- Zero WaveDB-originated leaks under ASAN/valgrind (matching the C/Node/Dart
  baseline).
- API surface matches the Node/Dart bindings method-for-method (with snake_case
  renames): WaveDB, Subtree, GraphLayer, GraphQLLayer, iterator, object ops,
  encryption configs.
- `pip install` from sdist produces a working `wavedb` module that imports and
  runs round-trip put/get on a fresh Python 3.10+ environment with CMake + a C
  compiler available.
- README mirrors the Dart README structure: install, quick start (sync + async
  + object + batch + stream + subtree), config table, encryption section.