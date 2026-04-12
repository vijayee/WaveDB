# WaveDB Dart Bindings

Dart FFI bindings for [WaveDB](../../README.md) - A hierarchical B+trie database. It is a love child of ForrestDB and MUMPS.

## Installation

Add to your `pubspec.yaml`:

```yaml
dependencies:
  wavedb: ^0.1.0
```

## Prerequisites

You must have the WaveDB C library available:
- Linux: `libwavedb.so`
- macOS: `libwavedb.dylib`
- Windows: `wavedb.dll`

Build from source in the WaveDB project.

## Quick Start

```dart
import 'package:wavedb/wavedb.dart';

void main() async {
  final db = WaveDB('/path/to/database');

  // Sync operations
  db.putSync('users/alice/name', 'Alice');
  final name = db.getSync('users/alice/name');
  print(name); // 'Alice'

  // Async operations (non-blocking, uses C worker pool)
  await db.put('users/bob/name', 'Bob');
  final name2 = await db.get('users/bob/name');

  // Object operations (nested data)
  await db.putObject('users', {
    'alice': {'name': 'Alice', 'age': '30'},
    'bob': {'name': 'Bob', 'age': '25'},
  });

  // Batch operations
  await db.batch([
    {'type': 'put', 'key': 'counter/a', 'value': '1'},
    {'type': 'put', 'key': 'counter/b', 'value': '2'},
    {'type': 'del', 'key': 'old/key'},
  ]);

  // Stream all entries
  db.createReadStream().listen((kv) {
    print('${kv.key} = ${kv.value}');
  });

  db.close();
}
```

## API Reference

### WaveDB Class

#### Constructor
- `WaveDB(String path, {String delimiter = '/', WaveDBConfig? config})` - Open or create database at path with optional configuration

## Configuration

WaveDB uses sensible defaults for most use cases.

### Database Configuration

| Setting | Default | Description |
|---------|---------|-------------|
| `chunkSize` | 4 | HBTrie chunk size in bytes (atomic comparison unit) |
| `btreeNodeSize` | 4096 | B+tree node size in bytes |
| `enablePersist` | true | Persistence enabled (data saved to disk) |

### Cache Configuration

| Setting | Default | Description |
|---------|---------|-------------|
| `lruMemoryMb` | 50 | LRU cache size in megabytes |
| `lruShards` | 64 | LRU cache shard count (0 = auto-scale to CPU cores) |
| `storageCacheSize` | 1024 | Section cache size (number of sections) |

### WAL (Write-Ahead Log) Configuration

| Setting | Default | Description |
|---------|---------|-------------|
| `walSyncMode` | 'debounced' | Durability mode: 'immediate', 'debounced', or 'async' |
| `walDebounceMs` | 100 | Debounce window for fsync (debounced mode) |
| `walMaxFileSize` | 131072 | Max WAL file size before sealing (128KB) |

### Threading Configuration

| Setting | Default | Description |
|---------|---------|-------------|
| `workerThreads` | 4 | Number of background worker threads |

### Sync Modes Explained

- **'immediate'**: Each write calls `fsync()` - maximum durability, lowest performance
- **'debounced'** (default): Batches fsync calls within debounce window - good balance
- **'async'**: No fsync guarantees - highest performance, potential data loss on crash

### Performance Tuning Tips

1. **High write throughput**: Use `'async'` mode for bulk imports
2. **Low latency reads**: Increase `lruMemoryMb` (50-500MB typical)
3. **Many CPU cores**: Set `lruShards` to 0 for auto-scaling
4. **Large datasets**: Increase `storageCacheSize` for better section locality

### Example with Configuration

```dart
import 'package:wavedb/wavedb.dart';

final db = WaveDB(
  '/path/to/database',
  delimiter: '/',
  config: WaveDBConfig(
    lruMemoryMb: 100,      // 100 MB cache
    lruShards: 0,          // auto-scale to CPU cores
    walSyncMode: 'debounced',
    walDebounceMs: 50,
  ),
);
```

### Async Operations

Async operations dispatch work to the C worker pool via `promise_t` and bridge C thread callbacks back to the Dart isolate using `NativeCallable.listener`.

```dart
// Put
await db.put(key, value);

// Get (returns null if not found)
final value = await db.get(key);

// Delete
await db.del(key);

// Batch
await db.batch([
  {'type': 'put', 'key': 'k1', 'value': 'v1'},
  {'type': 'del', 'key': 'k2'},
]);
```

### Sync Operations

Sync operations block the current isolate and call C functions directly via FFI.

```dart
db.putSync(key, value);
final value = db.getSync(key);
db.delSync(key);
db.batchSync(ops);
```

### Object Operations

```dart
// Flatten object to paths
await db.putObject('users', {
  'alice': {'name': 'Alice', 'roles': ['admin', 'user']},
});
// Creates:
//   users/alice/name → 'Alice'
//   users/alice/roles/0 → 'admin'
//   users/alice/roles/1 → 'user'

// Reconstruct object from subtree
final user = await db.getObject('users/alice');
// {name: 'Alice', roles: ['admin', 'user']}
```

### Keys

Keys can be strings or lists:

```dart
// String with delimiter
await db.put('users/alice/name', 'Alice');

// List
await db.put(['users', 'bob', 'name'], 'Bob');

// Custom delimiter
final db = WaveDB('/path/to/db', delimiter: ':');
await db.put('users:charlie:name', 'Charlie');
```

### Values

Values can be strings or `Uint8List`:

```dart
// String
await db.put('key', 'value');

// Uint8List (binary)
await db.put('binary/key', Uint8List.fromList([0x01, 0x02]));
```

### Error Handling

```dart
try {
  await db.put('key', 'value');
} on WaveDBException catch (e) {
  print('Database error: ${e.message}');
}
```

### Streaming

```dart
db.createReadStream(
  start: 'users/',
  end: 'users/z',
  reverse: false,
  keys: true,
  values: true,
).listen((KeyValue kv) {
  print('${kv.key} = ${kv.value}');
});
```

**Options:**
- `start`: Start path (inclusive)
- `end`: End path (exclusive)
- `reverse`: Reverse order (default: false, not yet implemented in C API)
- `keys`: Include keys (default: true)
- `values`: Include values (default: true)

### Lifecycle

- `void close()` - Close the database (waits for pending async operations)
- `bool get isClosed` - Check if database is closed
- `String get path` - Get database path
- `String get delimiter` - Get path delimiter

### WaveDBException Class

Error codes: `NOT_FOUND`, `INVALID_PATH`, `IO_ERROR`, `DATABASE_CLOSED`, `INVALID_ARGUMENT`, `NOT_SUPPORTED`, `CORRUPTION`, `CONFLICT`, `LIBRARY_NOT_FOUND`, `UNSUPPORTED_PLATFORM`

## Testing

Tests require the native WaveDB library (`libwavedb.so`, `libwavedb.dylib`, or `wavedb.dll`).

Build the library first:
```bash
cd /path/to/WaveDB
mkdir build && cd build
cmake ..
make
```

Then run tests:
```bash
cd bindings/dart
export LD_LIBRARY_PATH=/path/to/WaveDB/build:$LD_LIBRARY_PATH  # Linux
# or: export DYLD_LIBRARY_PATH=... on macOS
dart test
```

## Architecture

The bindings use Dart FFI with `NativeCallable.listener` to bridge C `promise_t` callbacks from the worker pool back to the Dart isolate's event loop. This enables true non-blocking async operations that dispatch work to the C worker pool and receive results via C promise resolution callbacks.

**Key components:**
- `database.dart`: WaveDB class with async operations via C promise/pool
- `async_bridge`: NativeCallable.listener + C promise_t bridge for async operations
- `path.dart`: Dart ↔ path_t conversion
- `identifier.dart`: Dart ↔ identifier_t conversion
- `iterator.dart`: Stream-based iterator
- `object_ops.dart`: Object flattening/reconstruction

Async operations dispatch work to the C worker pool via `database_put`, `database_get`, `database_delete`, and `database_write_batch`, bridging C promise callbacks back to the Dart isolate through `NativeCallable.listener`.

## Performance

Benchmarks run on Linux x86_64:

### Native C Library Performance

**Single-Threaded Operations:**

| Operation | Throughput | Avg Latency | P99 Latency |
|-----------|------------|-------------|-------------|
| Put | 36,169 ops/sec | 27.6 µs | 53.0 µs |
| Get | 70,772 ops/sec | 14.1 µs | 112.6 µs |
| Batch | 27,944 ops/sec | 35.8 µs | 89.1 µs |
| Mixed | 50,542 ops/sec | 19.8 µs | 65.8 µs |

**Concurrent Operations (Multi-Threaded):**

| Threads | Write | Read | Mixed |
|---------|-------|------|-------|
| 1 | 26,839 ops/sec | 111,927 ops/sec | 50,090 ops/sec |
| 2 | 64,908 ops/sec | 190,248 ops/sec | 84,652 ops/sec |
| 4 | 147,573 ops/sec | 191,212 ops/sec | 148,334 ops/sec |
| 8 | 148,767 ops/sec | 210,073 ops/sec | 142,025 ops/sec |
| 16 | 200,344 ops/sec | 209,424 ops/sec | 158,298 ops/sec |

### Dart FFI Performance

Preliminary benchmarks (100 iterations, limited sample):

**Async Operations (C promise/pool-based, non-blocking):**
- `put`: ~741 ops/sec
- `get`: ~14,286 ops/sec

**Sync Operations (blocking, direct FFI calls):**
- `putSync`: ~1,064 ops/sec
- `getSync`: ~20,000 ops/sec

*Note: These are preliminary results from a small sample size. Full benchmarks require a release-mode build of the native library.*

### Comparison with Node.js

| Operation | Dart FFI | Node.js N-API | Notes |
|-----------|-----------|---------------|-------|
| putSync | ~1,064 ops/sec | ~4,400 ops/sec | Node.js ~4x faster |
| getSync | ~20,000 ops/sec | ~240,000 ops/sec | Node.js ~12x faster |
| put async | ~741 ops/sec | ~1,000 ops/sec | Similar |
| get async | ~14,286 ops/sec | ~38,000 ops/sec | Node.js ~2.7x faster |

*Both bindings use the same C async API (promise_t + worker pool) for non-blocking operations.*

**MVCC & WAL:**
- Multi-Version Concurrency Control enables lock-free reads
- Write-Ahead Logging ensures durability
- Atomic transaction IDs with CLOCK_MONOTONIC for stable ordering
- Optimized with atomic operations (no mutex contention)

## Known Limitations

### Reverse Iteration
The `reverse` parameter in `createReadStream` is reserved for future use. The C API doesn't support reverse iteration yet.

## License

GNU General Public License v3.0 or later