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

### Batched Helpers (8x throughput)

For throughput-sensitive workloads, use the batched helpers which amortize
the promise/callback overhead across all items:

```python
async def main():
    async with WaveDB("/path/to/db") as db:
        # 8x faster than individual await db.put() calls
        await db.put_many([("k1", "v1"), ("k2", "v2"), ("k3", "v3")])
        results = await db.get_many(["k1", "k2", "k3"])
        await db.delete_many(["k1", "k2"])

asyncio.run(main())
```

## Performance

Benchmark results (in-memory, `in_memory=True`):

| Operation | ops/sec | us/op |
|-----------|---------|-------|
| sync put | 161K | 6.2 |
| sync get | 469K | 2.1 |
| async put (sequential) | 22K | 46 |
| async put_many (100/batch) | 200K | 5.0 |
| async get_many (concurrent) | 35K | 29 |
| batch | 321K | 3.1 |
| stream scan | 793K entries/sec | |

Run `python benchmark.py` with `WAVEDB_LIB_PATH` set to reproduce.

## License

MIT. See [LICENSE](LICENSE).