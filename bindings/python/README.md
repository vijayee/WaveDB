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
        # ~16x faster than individual await db.put() calls.
        await db.put_many([("k1", "v1"), ("k2", "v2"), ("k3", "v3")])
        await db.delete_many(["k1", "k2"])

        # get_many fires N concurrent get() calls (there is no batched C
        # get API). It's a concurrency helper, not an atomic batch — the
        # speedup over sequential get is bounded by C work-pool parallelism
        # and varies with cache state (typically 1-3x).
        results = await db.get_many(["k1", "k2", "k3"])

asyncio.run(main())
```

## Performance

Benchmark results (in-memory, `in_memory=True`, BATCH_SIZE=1000):

| Operation | ops/sec | us/op |
|-----------|---------|-------|
| sync put | 195K | 5.1 |
| sync get | 576K | 1.7 |
| async put (sequential) | 19K | 52 |
| async get (sequential) | 14K | 74 |
| async put_many (1000/batch) | 231K | 4.3 |
| async get_many (concurrent) | 37K | 27 |
| async delete_many (1000/batch) | 419K | 2.4 |
| batch (1000/batch) | 417K | 2.4 |
| stream scan | 801K entries/sec | |

`put_many`/`delete_many` are ~12-30x faster than individual `put`/`del`
because they forward to a single atomic C batch call. `get_many` is
~2.6x faster than sequential `get` here — it has no batched C
equivalent, just `asyncio.gather` over individual `get()`s, so the
speedup is bounded by C work-pool parallelism and varies with cache
state (1-3x is typical). Numbers vary run-to-run by ~30% due to
tree-size and LRU-cache effects; reproduce with `python benchmark.py`.

Run `python benchmark.py` with `WAVEDB_LIB_PATH` set to reproduce.

## License

MIT. See [LICENSE](LICENSE).