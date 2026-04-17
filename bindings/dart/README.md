# WaveDB Dart Bindings

Dart FFI bindings for [WaveDB](../../README.md) — a hierarchical key-value database with MVCC, WAL durability, and schema layer access.

## Installation

```yaml
# pubspec.yaml
dependencies:
  wavedb: ^0.1.0
```

### Prerequisites

Build the shared library from source:

```bash
cd WaveDB
mkdir build && cd build
cmake -DBUILD_DART_BINDINGS=ON ..
make -j$(nproc)
# Produces libwavedb.so (Linux), libwavedb.dylib (macOS), or wavedb.dll (Windows)
```

## Quick Start

```dart
import 'package:wavedb/wavedb.dart';

final db = WaveDB('/path/to/db', delimiter: '/');

// Sync (blocking)
db.putSync('users/alice/name', 'Alice');
final name = db.getSync('users/alice/name');

// Async (non-blocking, uses C worker pool)
await db.put('users/bob/name', 'Bob');
final name2 = await db.get('users/bob/name');

// Object operations (nested data → flattened paths)
await db.putObject('users/alice', {'name': 'Alice', 'age': '30'});
final user = await db.getObject('users/alice');
// {name: 'Alice', age: '30'}

// Batch
await db.batch([
  {'type': 'put', 'key': 'counter/a', 'value': '1'},
  {'type': 'del', 'key': 'old/key'},
]);

// Streaming
db.createReadStream(start: 'users/', end: 'users/~').listen((kv) {
  print('${kv.key} = ${kv.value}');
});

db.close();
```

## Configuration

```dart
final db = WaveDB(
  '/path/to/db',
  delimiter: '/',
  config: WaveDBConfig(
    lruMemoryMb: 100,       // 100 MB cache
    lruShards: 0,           // auto-scale to CPU cores
    walSyncMode: 'debounced',
    walDebounceMs: 50,
  ),
);
```

| Setting | Default | Description |
|---------|---------|-------------|
| `chunkSize` | `4` | HBTrie chunk size (immutable, set at creation) |
| `btreeNodeSize` | `4096` | B+tree node size (immutable) |
| `enablePersist` | `true` | Persist data to disk (immutable) |
| `lruMemoryMb` | `50` | LRU cache size in MB |
| `lruShards` | `64` | LRU shard count (0 = auto-scale) |
| `workerThreads` | `4` | Worker pool size |
| `walSyncMode` | `'debounced'` | `'immediate'`, `'debounced'`, or `'async'` |
| `walDebounceMs` | `100` | fsync debounce window (ms) |
| `walMaxFileSize` | `131072` | Max WAL file size before sealing |

### WAL Sync Modes

| Mode | Behavior | Durability | Throughput |
|------|----------|------------|------------|
| `'immediate'` | fsync after every write | Highest | ~1K ops/sec |
| `'debounced'` | batched fsync (default 100ms) | High | ~300K ops/sec |
| `'async'` | no fsync, OS cache only | Lowest | ~400K ops/sec |

## API Reference

### Async Operations

All async methods return `Future` and dispatch to the C worker pool (non-blocking).

```dart
await db.put(key, value)              // Store a value
final val = await db.get(key)          // Retrieve (null if not found)
await db.del(key)                      // Delete
await db.batch(operations)             // Atomic batch of put/del
await db.putObject(key, obj)           // Store nested object as flattened paths
final obj = await db.getObject(key)    // Reconstruct nested object
```

### Sync Operations

Block the current isolate. Use for initialization/migration, not hot paths.

```dart
db.putSync(key, value)
final val = db.getSync(key)            // null if not found
db.delSync(key)
db.batchSync(operations)
db.putObjectSync(key, obj)
final obj = db.getObjectSync(key)
```

### Keys and Values

```dart
// String keys with delimiter
await db.put('users/alice/name', 'Alice');

// List keys
await db.put(['users', 'bob', 'name'], 'Bob');

// Custom delimiter
final db = WaveDB('/path/to/db', delimiter: ':');
await db.put('users:charlie:name', 'Charlie');

// String or Uint8List values
await db.put('key', 'text value');
await db.put('binary/key', Uint8List.fromList([0x01, 0x02]));
```

### Streaming

```dart
db.createReadStream(
  start: 'users/',
  end: 'users/~',
  reverse: false,
  keys: true,
  values: true,
).listen((KeyValue kv) {
  print('${kv.key} = ${kv.value}');
});
```

| Option | Default | Description |
|--------|---------|-------------|
| `start` | — | Start path (inclusive) |
| `end` | — | End path (exclusive) |
| `reverse` | `false` | Reserved (not yet supported in C API) |
| `keys` | `true` | Include keys |
| `values` | `true` | Include values |

### Lifecycle

```dart
db.close()               // Wait for pending ops, then destroy
bool isClosed = db.isClosed
String path = db.path
String delim = db.delimiter
```

### Error Handling

```dart
try {
  await db.put('key', 'value');
} on WaveDBException catch (e) {
  print('Database error: ${e.message}');
}
```

Error codes: `NOT_FOUND`, `INVALID_PATH`, `IO_ERROR`, `DATABASE_CLOSED`, `INVALID_ARGUMENT`, `NOT_SUPPORTED`, `CORRUPTION`, `CONFLICT`, `LIBRARY_NOT_FOUND`

## GraphQL Schema Layer

```dart
import 'package:wavedb/graphql_layer.dart';

final layer = GraphQLLayer();
layer.create(GraphQLLayerConfig(
  path: '/path/to/db',
  enablePersist: true,
  chunkSize: 4,
  workerThreads: 4,
));

// Define schema
layer.parseSchema('''
  type User {
    name: String
    age: Int
  }
''');

// Query
final result = layer.querySync('{ User { name } }');
// GraphQLResult(success: true, data: {User: {name: null}}, errors: [])

// Mutation
final created = layer.mutateSync('mutation { createUser(name: "Alice") { id name } }');
// GraphQLResult(success: true, data: {createUser: {id: "abc123", name: "Alice"}})

// Async variants
final result = await layer.query('{ User { name } }');
final created = await layer.mutate('mutation { createUser(name: "Bob") { id } }');

layer.close();
```

### Introspection

```dart
final schema = await layer.query('{ __schema { types { name kind } } }');
final userType = await layer.query('{ __type(name: "User") { name kind fields { name } } }');
```

### Required Fields

Fields marked `!` in the schema are required for create mutations:

```dart
layer.parseSchema('type User { name: String! age: Int }');

// Missing required field — fails
layer.mutateSync('mutation { createUser(age: "30") { id } }');
// GraphQLResult(success: false, errors: ['Missing required fields: name'])

// Providing required field — succeeds
layer.mutateSync('mutation { createUser(name: "Alice") { id name } }');
// GraphQLResult(success: true, data: {...})
```

### GraphQLResult

- `bool success` — Whether the operation succeeded
- `dynamic data` — Returned data
- `List<GraphQLError> errors` — Error messages

## Performance

Benchmarks on Linux x86_64, Dart 3.11, 50MB LRU cache, 4 worker threads.

### C Library (ASYNC WAL, no work pool, single-threaded)

| Operation | Throughput | P50 Latency | P99 Latency |
|-----------|------------|-------------|-------------|
| Get | 1.46M ops/sec | 679 ns | 777 ns |
| Put | 160K ops/sec | 5.8 µs | 9.9 µs |
| Delete | 216K ops/sec | 4.4 µs | 7.0 µs |
| Mixed (70% read) | 1.49M ops/sec | 664 ns | 750 ns |

### Dart FFI (DEBOUNCED WAL, 4 worker threads)

| Operation | Throughput |
|-----------|------------|
| `getSync` | 526K ops/sec |
| `putSync` | 1.2K ops/sec |
| `get` (async) | 26K ops/sec |
| `put` (async) | 900 ops/sec |
| `batch` (async, 1K ops) | 85K ops/sec |

### Dart Concurrent Throughput (DEBOUNCED WAL)

| Concurrency | `put` | `get` |
|-------------|-------|-------|
| 1 | 1.1K ops/sec | 46K ops/sec |
| 2 | 1.9K ops/sec | 66K ops/sec |
| 4 | 2.3K ops/sec | 94K ops/sec |
| 8 | 2.4K ops/sec | 111K ops/sec |
| 16 | 2.4K ops/sec | 123K ops/sec |
| 32 | 2.1K ops/sec | 21K ops/sec |

### Comparison with Node.js

| Operation | Dart FFI | Node.js N-API | Ratio |
|-----------|-----------|---------------|-------|
| `getSync` | 526K ops/sec | 203K ops/sec | Dart 2.6x |
| `get` (async) | 26K ops/sec | 31K ops/sec | Similar |
| `batch` (async) | 85K ops/sec | 54K ops/sec | Dart 1.6x |
| `putSync` | 1.2K ops/sec | 1.2K ops/sec | Similar |

Both bindings use the same C async API (`promise_t` + worker pool) for non-blocking operations.

## Testing

```bash
# Build shared library
cd /path/to/WaveDB
mkdir build && cd build
cmake -DBUILD_DART_BINDINGS=ON ..
make -j$(nproc)

# Run tests
cd ../bindings/dart
cp ../../build/libwavedb.so .   # or set LD_LIBRARY_PATH
dart test
```

## License

MIT