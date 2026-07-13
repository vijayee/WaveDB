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

### Graph (triples)

```python
from wavedb import WaveDB, GraphLayer

db = WaveDB("/path/to/db")
g = GraphLayer("graph", db)
g.insert_sync("alice", "knows", "bob")
result = g.query().vertex("alice").out("knows").execute_sync()
print(result.vertices)  # ["bob"]
```

### Atomic cross-subtree batches

`GraphLayer.expand_triple` expands a triple into op dicts addressed in the
*root* database namespace, so a triple's index updates can share one atomic
transaction with content writes:

```python
db = WaveDB("/path/to/db")
g = GraphLayer("graph", db)

# One atomic batch: a content write in the root namespace plus a graph
# triple expanded into root-namespace index ops.
ops = [
    {"type": "put", "key": "content/ep1/summary", "value": "alice met bob"},
] + g.expand_triple("alice", "knows", "bob")
db.batch_sync(ops)

assert db.get_sync("content/ep1/summary") == b"alice met bob"
assert "bob" in g.query().vertex("alice").out("knows").execute_sync().vertices

# delete via batch is the batch equivalent of g.delete_sync(...):
db.batch_sync(g.expand_triple("alice", "knows", "bob", delete=True))
```

### GraphQL

```python
from wavedb import WaveDB, GraphQLLayer

db = WaveDB("/path/to/db")
gql = GraphQLLayer("gql", db)

# Define a schema. The default resolver stores entity data under the
# type's plural path prefix: gql/Users/<id>/<field>.
gql.schema_parse("""
    type User {
        id: ID!
        name: String
        age: Int
    }
""")

# Write entity data via the parent db (the subtree prefix "gql" is applied
# by the subtree, so we write through the parent db with the full key).
db.put_sync("gql/Users/1/id", "1")
db.put_sync("gql/Users/1/name", "Alice")
db.put_sync("gql/Users/1/age", "30")

# Query by id — the default resolver looks up <plural>/<id>/<field>.
result = gql.query_sync('{ User(id: "1") { id name age } }')
print(result.success)             # True
print(result.data["User"][0])     # {'id': '1', 'name': 'Alice', 'age': 30}
print(result.to_json())           # raw GraphQL JSON response string

gql.close()
db.close()
```

The result object exposes `data` (parsed JSON), `errors` (list of
`GraphQLError` with `message`/`path`/`locations`), `success` (True when
`errors` is empty), and `to_json()` (raw response string). Call
`result.close()` to free the underlying C result, or use `GraphQLLayer` as
a context manager.

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

## Vector Layer

Approximate-nearest-neighbour (ANN) vector similarity search on top of
WaveDB, in the same process. Three index types: **FLAT** (exact
brute-force), **IVF** (k-means inverted file), **SLSH** (sortable compound
LSH with bidirectional scan). Config is split into a **format tier**
(immutable after create — drop + recreate to change) and a **runtime tier**
(mutable via `VectorLayer.reconfigure`). See
[`src/Layers/vector/README.md`](../../src/Layers/vector/README.md) for the
authoritative C-layer config reference with spike-measured impact columns;
[`bench/vector/REPORT.md`](../../bench/vector/REPORT.md) for the full
bench numbers.

### Config

```python
from wavedb import VectorLayer, Format, Runtime, IndexType, Distance

# Dedicated database (no key-space sharing). Use VectorLayer.open(...)
# to share a WaveDB instance instead, optionally scoped to a Subtree.
vl = VectorLayer.open_separate(
    "/path/to/vecdb",
    "embeddings",
    Format(index_type=IndexType.IVF, dim=384, distance=Distance.COSINE,
           ivf_n_clusters=50),
    Runtime(top_k=10, sync_only=1, ivf_nprobe=8, ivf_flat_until=1000),
)

# Runtime tier is mutable; format tier is not.
vl.reconfigure(Runtime(top_k=20, ivf_nprobe=16))
```

#### Format tier (immutable after create)

| Field | Type | Default | Effect on recall / latency / storage |
|---|---|---|---|
| `index_type` | `IndexType` | FLAT | FLAT exact (1.0); IVF 0.96-0.99 on clustered / 0.36-0.48 on gaussian; SLSH 0.91-0.95 on clustered. FLAT O(N), IVF O(nprobe·N/K), SLSH O(radius). IVF/SLSH +~86-89 bytes/vec over FLAT |
| `dim` | int | — (required) | Higher dim → lower ANN recall (curse of dimensionality). Latency and storage linear in dim |
| `delimiter` | str | `'/'` | Negligible effect; change only if '/' conflicts with your id scheme |
| `distance` | `Distance` | COSINE | Used for assignment + rerank; match your embedding model (COSINE for normalized, L2 for general, DOT for inner-product) |
| `ivf_n_clusters` | int | 50 | More clusters → higher recall (diminishing); training is O(K²·N·dim). ~sqrt(N) rule of thumb (50 for 10k, 170 for 30k) |
| `slsh_lsh_tables` | int | 4 | More tables → higher recall (diminishing); 2-4 clustered, 4-8 uniform |
| `slsh_hash_bits` | int | 16 | More bits → finer buckets → higher recall up to sparsity; 8-16, 16 standard |
| `slsh_bucket_width` | float | 2.0 | Smaller W → higher recall up to sparsity. W=2.0 gives 9x lower latency than 10.0 at equal recall. 1.0-4.0 |

#### Runtime tier (mutable via `reconfigure`)

| Field | Type | Default | Effect on recall / latency / storage |
|---|---|---|---|
| `top_k` | int | 10 | Linear in k (rerank cost); match your use case |
| `sync_only` | int | 1 | 1 = single-threaded, no MVCC overhead; 0 = async worker pool for concurrent callers |
| `ivf_nprobe` | int | 8 | Higher → higher recall (linear cost). 8 clears 0.90 on clustered; 16-32 if data is near-uniform |
| `ivf_flat_until` | int | 1000 | Below this count, FLAT (exact) is used; raise if cold-start recall is low |
| `slsh_scan_radius` | int | 200 | **ADAPTIVE: actual = max(configured, count/30).** Configured is a floor; auto-scales with dataset size (~1000 at 30k). Raise for more recall on small datasets |

### Example

```python
from wavedb import VectorLayer, Format, Runtime, IndexType, Distance

vl = VectorLayer.open_separate(
    "/path/to/vecdb", "embeddings",
    Format(index_type=IndexType.IVF, dim=384, distance=Distance.COSINE,
           ivf_n_clusters=50),
    Runtime(top_k=10, sync_only=1, ivf_nprobe=8, ivf_flat_until=1000),
)

# Insert vectors (id, vec, optional metadata bytes).
vl.insert_sync("doc/1", [0.12, -0.04, ...], metadata=b"payload")
vl.insert_sync("doc/2", [0.08,  0.11, ...])

# Train k-means / regenerate projections. Call after a bulk load and
# periodically as the dataset grows; FLAT is a no-op.
vl.train()

# Search — returns a list[VectorResult] sorted by distance.
results = vl.search_sync([0.10, 0.05, ...], k=10)
for r in results:
    print(r.id_str, r.distance, r.metadata)

# Mutate the runtime tier at any time.
vl.reconfigure(Runtime(top_k=20, ivf_nprobe=16))

vl.close()
```

## License

MIT. See [LICENSE](LICENSE).