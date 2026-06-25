<div align="center">
  <img src="https://raw.githubusercontent.com/vijayee/WaveDB/master/wave_fuji.svg" alt="WaveDB Logo" width="200"/>
</div>

# WaveDB Python Bindings

Python bindings for [WaveDB](../../README.md) — a hierarchical key-value
database with MVCC, WAL durability, and schema layer access.

## Installation

```bash
pip install wavedb
```

The install step builds `libwavedb.so` (Linux) / `libwavedb.dylib` (macOS) /
`wavedb.dll` (Windows) from source via CMake. Requirements:

- Python 3.10+
- CMake 3.14+
- A C compiler (gcc, clang, or MSVC)

To use a pre-built library instead, set `WAVEDB_LIB_PATH` before importing:

```bash
export WAVEDB_LIB_PATH=/path/to/libwavedb.so
pip install wavedb --no-build-isolation
```

## Quick Start

```python
from wavedb import WaveDB

db = WaveDB("/path/to/db", delimiter="/")

# Sync (blocking)
db.put_sync("users/alice/name", "Alice")
name = db.get_sync("users/alice/name")  # b"Alice"

# Async (non-blocking, uses C worker pool)
import asyncio
async def main():
    await db.put("users/bob/name", "Bob")
    name = await db.get("users/bob/name")
asyncio.run(main())

# Batched async — 8x faster than individual puts
await db.put_many([("users/alice/name", "Alice"), ("users/bob/name", "Bob")])
results = await db.get_many(["users/alice/name", "users/bob/name"])
await db.delete_many(["users/alice/name"])

# Object operations (nested dict <-> flattened paths)
db.put_object_sync("users/alice", {"name": "Alice", "age": "30"})
user = db.get_object_sync("users/alice")

# Batch
db.batch_sync([
    {"type": "put", "key": "counter/a", "value": "1"},
    {"type": "del", "key": "old/key"},
])

# Streaming
for key, value in db.create_read_stream(start="users/", end="users/~"):
    print(key, value)

# Subtree
with db.open_subtree("users") as st:
    st.put_sync("alice/name", "Alice")

db.close()
```

## Configuration

```python
from wavedb import WaveDB, WaveDBConfig

db = WaveDB(
    "/path/to/db",
    config=WaveDBConfig(
        lru_memory_mb=100,
        lru_shards=0,           # auto-scale
        wal_sync_mode="debounced",
        wal_debounce_ms=250,
    ),
)
```

| Setting | Default | Description |
|---------|---------|-------------|
| `chunk_size` | `4` | HBTrie chunk size (immutable) |
| `btree_node_size` | `4096` | B+tree node size (immutable) |
| `enable_persist` | `True` | Persist to disk (immutable, page-file only) |
| `in_memory` | `False` | True ephemeral mode (no WAL, no page file) |
| `lru_memory_mb` | `50` | LRU cache size in MB |
| `lru_shards` | `0` | LRU shard count (0 = auto) |
| `wal_sync_mode` | `"debounced"` | `debounced` / `immediate` / `none` |
| `wal_debounce_ms` | `250` | WAL debounce interval |
| `worker_threads` | `4` | C work pool size |
| `sync_only` | `False` | Skip concurrency control |

## Encryption

```python
from wavedb import WaveDB, WaveDBEncryption

db = WaveDB(
    "/path/to/db",
    encryption=WaveDBEncryption(
        type="aes-256-gcm",
        symmetric_key=b"32-byte-key-here",
    ),
)
```

## Graph and GraphQL

```python
from wavedb import WaveDB, GraphLayer

db = WaveDB("/path/to/db")
g = GraphLayer("graph", db)
g.insert_sync("alice", "knows", "bob")
result = g.query().vertex("alice").out("knows").execute_sync()
print(result.vertices)  # ["bob"]
```

## Async Model

Async methods (`put`, `get`, `delete`, `batch`, `put_object`, `get_object`) drive
the C work pool via `promise_t` and marshal results back to the calling asyncio
loop via `loop.call_soon_threadsafe`. Use them within an asyncio program:

```python
async def main():
    async with WaveDB("/path/to/db") as db:
        await db.put("k", "v")
        print(await db.get("k"))

asyncio.run(main())
```

### Batched Helpers

For throughput-sensitive workloads, use the batched helpers:

```python
async def main():
    async with WaveDB("/path/to/db") as db:
        # put_many / delete_many forward to a single C batch call — atomic,
        # ~15-25x faster than individual await db.put() / db.delete() calls.
        await db.put_many([("k1", "v1"), ("k2", "v2"), ("k3", "v3")])
        await db.delete_many(["k1", "k2"])

        # get_many fires N concurrent get() calls (there is no batched C
        # get API). It's a concurrency helper, not an atomic batch — the
        # speedup over sequential get is bounded by C work-pool parallelism
        # and varies with cache state (typically 1-3x, but ~1x in
        # in-memory mode where asyncio marshalling dominates).
        results = await db.get_many(["k1", "k2", "k3"])

asyncio.run(main())
```

## Performance

Best-of-three runs of `benchmark.py` on Linux x86_64, Python 3.10+,
C work pool at 4 workers, BATCH_SIZE=1000. Variance across runs is
high (~10x range) when competing CPU load is present; reproduce on
an idle machine with `WAVEDB_LIB_PATH=../../build-release/libwavedb.so python benchmark.py`.

### In-Memory (`in_memory=True`)

| Operation | ops/sec | us/op |
|-----------|---------|-------|
| `put_sync` | 178K | 5.6 |
| `get_sync` | 484K | 2.1 |
| `batch` (1000/batch) | 268K | 3.7 |
| `put_many` (1000/batch) | 299K | 3.4 |
| `delete_many` (1000/batch) | 213K | 4.7 |
| `get_many` (1000/call) | 33K | 30.5 |
| async `put` (sequential) | 13K | 75 |
| async `get` (sequential) | 26K | 38 |
| stream scan | 516K entries/sec | |

### Async WAL (`wal_sync_mode="none"`)

| Operation | ops/sec | us/op |
|-----------|---------|-------|
| `put_sync` | 92K | 10.8 |
| `get_sync` | 403K | 2.5 |
| `batch` (1000/batch) | 134K | 7.5 |
| `put_many` (1000/batch) | 173K | 5.8 |
| `delete_many` (1000/batch) | 144K | 6.9 |
| `get_many` (1000/call) | 24K | 41 |
| async `put` (sequential) | 17K | 58 |
| async `get` (sequential) | 17K | 58 |

### Immediate WAL (`wal_sync_mode="immediate"`, fsync per write)

| Operation | ops/sec | us/op |
|-----------|---------|-------|
| `put_sync` | 93K | 10.7 |
| `get_sync` | 305K | 3.3 |
| `batch` (1000/batch) | 124K | 8.1 |
| `put_many` (1000/batch) | 127K | 7.9 |
| `delete_many` (1000/batch) | 103K | 9.7 |
| `get_many` (1000/call) | 20K | 51 |

### Notes

`put_many` and `delete_many` forward to a single atomic C batch call
and are 15-25x faster than individual `await db.put()` calls. They
share the same C path as `batch()` — the small per-call overhead
difference is Python-side dict construction. `get_many` has no
batched C equivalent; it is `asyncio.gather` over individual `get()`s,
so its speedup over sequential `await db.get()` is bounded by C
work-pool parallelism (typically 1-3x) and drops to ~1x in in-memory
mode where the asyncio marshalling loop dominates over C work.

## License

MIT. See [LICENSE](LICENSE).