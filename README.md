<div align="center">
  <img src="wave_fuji.svg" alt="WaveDB Logo" width="200">
</div>

# WaveDB

A hierarchical key-value database with MVCC concurrency, WAL durability, and schema layer access patterns. Inspired by MUMPS multi-dimensional subscripts and ForestDB's copy-on-write HB+trie architecture.

## Features

- **Hierarchical key paths** — Store and retrieve values at paths like `users/alice/name`
- **MVCC** — Lock-free concurrent reads with snapshot isolation
- **WAL durability** — Thread-local write-ahead log with configurable sync modes
- **Schema Layers** — GraphQL access today, SQL roadmap
- **Language bindings** — C, Node.js, Dart
- **High throughput** — Sharded LRU cache with memory pooling, inline key comparison, MVCC fast-path

## Quick Start

```c
#include "Database/database.h"
#include "Database/database_config.h"

int main() {
    int error = 0;

    // Create with default config (in-memory, ASYNC WAL)
    database_config_t* config = database_config_default();
    database_t* db = database_create_with_config("/tmp/mydb", config, &error);
    database_config_destroy(config);

    if (!db) { fprintf(stderr, "Error: %d\n", error); return 1; }

    // Write
    path_t* key = path_create_from_string("/", "users/alice/name", 0);
    identifier_t* val = identifier_create_from_string("Alice", 0);
    database_put_sync(db, key, val);
    path_destroy(key);
    identifier_destroy(val);

    // Read
    path_t* key2 = path_create_from_string("/", "users/alice/name", 0);
    identifier_t* result = NULL;
    int rc = database_get_sync(db, key2, &result);
    if (rc == 0 && result) {
        // use result...
        identifier_destroy(result);
    }
    path_destroy(key2);

    // Batch
    batch_t* batch = batch_create(0);
    path_t* p1 = path_create_from_string("/", "users/bob/age", 0);
    identifier_t* v1 = identifier_create_from_string("30", 0);
    batch_add_put(batch, p1, v1);  // ownership transfers on success
    database_write_batch_sync(db, batch);
    batch_destroy(batch);

    // Iterate
    path_t* start = path_create_from_string("/", "users/alice", 0);
    path_t* end = path_create_from_string("/", "users/alice/~", 0);
    database_iterator_t* it = database_scan_start(db, start, end);
    path_t* out_path = NULL;
    identifier_t* out_val = NULL;
    while (database_scan_next(it, &out_path, &out_val) == 0) {
        // process out_path, out_val
        path_destroy(out_path);
        identifier_destroy(out_val);
    }
    database_scan_end(it);
    path_destroy(start);
    path_destroy(end);

    database_destroy(db);
    return 0;
}
```

## Architecture

WaveDB stores values at hierarchical key paths using an HBTrie — a B+tree where each level is itself a B+tree. A path like `["users", "alice", "name"]` traverses three levels of B+trees, one per path component.

```
                     root bnode
                    /    |    \
              "users"   "posts"   ...
              /  |  \      |
           bnode  ...  bnode  ...
           /  \          |
     "alice" "bob"   "title"
        |       |        |
      bnode   bnode    value
      / | \     |
   "name" "age" ...
     |      |
   value  value
```

**Concurrency:** MVCC (Multi-Version Concurrency Control) provides snapshot isolation — readers never block writers and never see partial updates. Each write creates a versioned entry; reads see the latest committed version at their transaction ID.

**Durability:** Thread-local WAL files eliminate write contention. Three sync modes trade durability for throughput: IMMEDIATE (fsync every write), DEBOUNCED (batched fsync every 250ms, recommended), ASYNC (buffer flushed to kernel on 250ms idle timer, survives process crash but not power failure).

**Schema Layers:** Access the same data through different query paradigms. The GraphQL layer maps type definitions to hierarchical paths and resolves queries with scan plans.

## Configuration

```c
database_config_t* config = database_config_default();

// Mutable — can change when reopening
config->lru_memory_mb = 100;       // LRU cache size (default: 50 MB)
config->lru_shards = 64;           // LRU shards (default: 64, 0 = auto-scale)
config->wal_config.sync_mode = WAL_SYNC_DEBOUNCED;  // WAL mode
config->wal_config.debounce_ms = 250;                // fsync window (default: 250ms)
config->worker_threads = 4;        // Worker pool size (default: 4)

// Immutable — set at creation, persisted
config->chunk_size = 4;            // HBTrie chunk size (default: 4)
config->btree_node_size = 4096;    // B+tree node size (default: 4096)
config->enable_persist = 1;        // 0 = in-memory, 1 = persistent (default: 1)

database_t* db = database_create_with_config("/path/to/db", config, &error);
database_config_destroy(config);
```

Config is persisted as CBOR at `<db_path>/.config` and automatically loaded on reopen. `database_config_merge` reconciles saved and passed configs — immutable settings always use the on-disk values.

### WAL Sync Modes

| Mode | Behavior | Durability | Performance |
|------|----------|------------|-------------|
| `WAL_SYNC_IMMEDIATE` | fsync after every write | Highest | ~1K ops/sec |
| `WAL_SYNC_DEBOUNCED` | batched fsync (default 250ms) | High | ~300K ops/sec |
| `WAL_SYNC_ASYNC` | buffered write, idle drain every 250ms | Process crash only | ~400K ops/sec |

### Database Operations

| Operation | Async (promise) | Sync |
|-----------|-----------------|------|
| Put | `database_put(db, path, val, promise)` | `database_put_sync(db, path, val)` |
| Get | `database_get(db, path, promise)` | `database_get_sync(db, path, &result)` |
| Delete | `database_delete(db, path, promise)` | `database_delete_sync(db, path)` |
| Batch | `database_write_batch(db, batch, promise)` | `database_write_batch_sync(db, batch)` |
| Increment | — | `database_increment_sync(db, path, delta)` |
| Snapshot | — | `database_snapshot(db)` |

## Schema Layers

### GraphQL

The GraphQL layer maps SDL type definitions to hierarchical paths and resolves queries with scan plans.

```c
#include "Layers/graphql/graphql.h"

graphql_layer_t* layer = graphql_layer_create("/path/to/db", NULL);

// Register schema
const char* sdl =
    "type User { name: String age: Int }"
    "type Query { user(id: ID!): User }";
graphql_schema_parse(layer, sdl, NULL);

// Query
graphql_result_t* result = graphql_query_sync(layer, "{ user(id: \"alice\") { name } }");
// result contains { "data": { "user": { "name": "Alice" } } }
graphql_result_destroy(result);

graphql_layer_destroy(layer);
```

Query plans are compiled from SDL + query and executed as scan, get, or batch-get operations against the database. Custom resolvers can be registered for computed fields.

## Language Bindings

### Node.js

```bash
npm install wavedb
```

```javascript
const { WaveDB } = require('wavedb');

const db = new WaveDB('/path/to/db', {
  delimiter: '/',
  lruMemoryMb: 50,
  wal: { syncMode: 'debounced' }
});

// Async (Promise)
await db.put('users/alice/name', 'Alice');
const name = await db.get('users/alice/name');

// Sync (blocking)
db.putSync('users/bob/name', 'Bob');
const name2 = db.getSync('users/bob/name');

// Callback style
db.putCb('users/charlie/name', 'Charlie', (err) => { /* ... */ });
db.getCb('users/charlie/name', (err, val) => { /* ... */ });

// Object operations (nested data → flattened paths)
await db.putObject('users/alice', { name: 'Alice', age: 30 });
const user = await db.getObject('users/alice'); // { name: 'Alice', age: 30 }

// Batch
await db.batch([
  { type: 'put', path: 'users/alice/name', value: 'Alice' },
  { type: 'del', path: 'users/bob/name' }
]);

// Iterator
const iter = new Iterator(db, { start: 'users/alice', end: 'users/alice~' });
let entry;
while ((entry = iter.read())) {
  console.log(entry.key, entry.value);
}
iter.end();
```

See [bindings/nodejs/README.md](bindings/nodejs/README.md) for full API reference.

### Dart

```yaml
# pubspec.yaml
dependencies:
  wavedb: ^0.1.0
```

```dart
import 'package:wavedb/wavedb.dart';

final db = WaveDB('/path/to/db', delimiter: '/');

// Sync
db.putSync('users/alice/name', 'Alice');
final name = db.getSync('users/alice/name');

// Async
await db.put('users/bob/name', 'Bob');
final name2 = await db.get('users/bob/name');

// Object operations
await db.putObject('users/alice', { 'name': 'Alice', 'age': 30 });
final user = await db.getObject('users/alice');

// Streaming
db.createReadStream(start: 'users/alice', end: 'users/alice~').listen((kv) {
  print('${kv.key}: ${kv.value}');
});

// GraphQL
final layer = GraphQLLayer();
layer.create(GraphQLLayerConfig(path: '/path/to/db'));
layer.parseSchema('type User { name: String } type Query { user(id: ID!): User }');
final result = layer.querySync('{ user(id: "alice") { name } }');

db.close();
```

See [bindings/dart/README.md](bindings/dart/README.md) for full API reference.

## Performance

Benchmarks on Linux x86_64, 8-core CPU, 50MB LRU cache.

### Sync Operations (ASYNC WAL mode, single-threaded)

| Operation | Throughput | P50 Latency | P99 Latency |
|-----------|------------|-------------|-------------|
| Get | 1.71M ops/sec | 565 ns | 919 ns |
| Put | 352K ops/sec | 2.35 µs | 6.34 µs |
| Delete | 278K ops/sec | 3.49 µs | 5.78 µs |
| Mixed (70% read) | 1.71M ops/sec | 582 ns | 709 ns |

### Concurrent Throughput (DEBOUNCED WAL mode, 250ms)

| Threads | Write | Read | Mixed (70R/20W/10D) |
|---------|-------|------|---------------------|
| 1 | 201K ops/sec | 838K ops/sec | 298K ops/sec |
| 4 | 413K ops/sec | 2.38M ops/sec | 613K ops/sec |
| 16 | 693K ops/sec | 7.25M ops/sec | 905K ops/sec |
| 32 | 938K ops/sec | 9.06M ops/sec | 1.02M ops/sec |

## Building

```bash
git clone --recursive https://github.com/vijayee/WaveDB.git
cd WaveDB
mkdir build && cd build
cmake ..
make -j$(nproc)

# Run tests
ctest --output-on-failure

# Build shared library for Dart/FFI bindings
cmake -DBUILD_DART_BINDINGS=ON ..
make -j$(nproc)
```

### Installation

```bash
cmake -DCMAKE_INSTALL_PREFIX=/usr/local ..
make
sudo make install
```

Installs headers, static library (`libwavedb.a`), and CMake config.

### Using as a CMake Dependency

```cmake
# After install
find_package(WaveDB REQUIRED)
target_link_libraries(myapp WaveDB::wavedb)

# Or embedded
add_subdirectory(path/to/WaveDB)
target_link_libraries(myapp WaveDB::wavedb)
```

### Dependencies

Bundled — no external dependencies required:
- **xxhash** — fast hashing
- **hashmap** — hash map implementation
- **libcbor** — CBOR serialization

## Roadmap

- **P2P Database Replication** — Distributed multi-master replication with conflict resolution
- **SQL Schema Layer** — SQL query interface over hierarchical data

## License

MIT