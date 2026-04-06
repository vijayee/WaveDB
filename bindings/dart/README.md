# WaveDB Dart Bindings

Dart FFI bindings for WaveDB, a hierarchical key-value database.

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

  // Async operations
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
    lruShards: 0,         // auto-scale to CPU cores
    walSyncMode: 'debounced',
    walDebounceMs: 50,
  ),
);
```

## Configuration

WaveDB uses sensible defaults for most use cases. The current bindings use these default settings:

### Database Configuration

| Setting | Default | Description |
|---------|---------|-------------|
| `chunk_size` | 4 | HBTrie chunk size in bytes (atomic comparison unit) |
| `btree_node_size` | 4096 | B+tree node size in bytes |
| `enable_persist` | true | Persistence enabled (data saved to disk) |

### Cache Configuration

| Setting | Default | Description |
|---------|---------|-------------|
| `lru_memory_mb` | 50 | LRU cache size in megabytes |
| `lru_shards` | 64 | LRU cache shard count (0 = auto-scale to CPU cores) |
| `storage_cache_size` | 1024 | Section cache size (number of sections) |

### WAL (Write-Ahead Log) Configuration

| Setting | Default | Description |
|---------|---------|-------------|
| `sync_mode` | DEBOUNCED | Durability mode: IMMEDIATE, DEBOUNCED, or ASYNC |
| `debounce_ms` | 100 | Debounce window for fsync (DEBOUNCED mode) |
| `idle_threshold_ms` | 10000 | Idle time before compaction (10 seconds) |
| `compact_interval_ms` | 60000 | Compaction interval (60 seconds) |
| `max_file_size` | 131072 | Max WAL file size before sealing (128KB) |

### Threading Configuration

| Setting | Default | Description |
|---------|---------|-------------|
| `worker_threads` | 4 | Number of background worker threads |
| `timer_resolution_ms` | 10 | Timer wheel resolution in milliseconds |

### Sync Modes Explained

- **IMMEDIATE**: Each write calls `fsync()` - maximum durability, lowest performance
- **DEBOUNCED** (default): Batches fsync calls within debounce window - good balance
- **ASYNC**: No fsync guarantees - highest performance, potential data loss on crash

### Performance Tuning Tips

1. **High write throughput**: Use `ASYNC` mode for bulk imports
2. **Low latency reads**: Increase `lru_memory_mb` (50-500MB typical)
3. **Many CPU cores**: Set `lru_shards` to 0 for auto-scaling
4. **Large datasets**: Increase `storage_cache_size` for better section locality

### Future API

The bindings will expose full configuration in a future release:

```dart
// Planned API (not yet available)
final db = WaveDB(
  '/path/to/db',
  delimiter: '/',
  config: WaveDBConfig(
    lruMemoryMb: 100,
    lruShards: 0,  // auto-scale
    wal: WalConfig(
      syncMode: SyncMode.debounced,
      debounceMs: 50,
    ),
  ),
);
```

#### Async Operations
- `Future<void> put(dynamic key, dynamic value)` - Store a value
- `Future<dynamic> get(dynamic key)` - Retrieve a value
- `Future<void> del(dynamic key)` - Delete a value
- `Future<void> batch(List<Map<String, dynamic>> operations)` - Execute multiple operations
- `Future<void> putObject(dynamic key, Map<String, dynamic> obj)` - Store nested object
- `Future<Map<String, dynamic>?> getObject(dynamic key)` - Retrieve nested object (NOT_SUPPORTED)

#### Sync Operations
- `void putSync(dynamic key, dynamic value)` - Store synchronously
- `dynamic getSync(dynamic key)` - Retrieve synchronously
- `void delSync(dynamic key)` - Delete synchronously
- `void batchSync(List<Map<String, dynamic>> operations)` - Batch synchronously
- `void putObjectSync(dynamic key, Map<String, dynamic> obj)` - Store object synchronously
- `Map<String, dynamic>? getObjectSync(dynamic key)` - Retrieve object synchronously (NOT_SUPPORTED)

#### Streaming
- `Stream<KeyValue> createReadStream({dynamic start, dynamic end, bool reverse = false, bool keys = true, bool values = true})` - Stream entries

#### Lifecycle
- `void close()` - Close the database
- `bool get isClosed` - Check if database is closed
- `String get path` - Get database path
- `String get delimiter` - Get path delimiter

### KeyValue Class

- `dynamic key` - The key (String or List<String>)
- `dynamic value` - The value (String or Uint8List)

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

## Benchmarks

Run performance benchmarks comparing Dart and Node.js:

```bash
cd bindings/dart/benchmark
./compare.sh
```

This will:
1. Build the native library if needed
2. Run Node.js benchmark
3. Run Dart benchmark
4. Display comparison results

### Performance (Native C Library)

Benchmarks run on Linux x86_64 with 50MB LRU cache:

**Single-Threaded:**
| Operation | Throughput | Avg Latency |
|-----------|------------|-------------|
| Put | 36,169 ops/sec | 27.6 µs |
| Get | 70,772 ops/sec | 14.1 µs |
| Batch | 27,944 ops/sec | 35.8 µs |
| Mixed | 50,542 ops/sec | 19.8 µs |

**Concurrent (8 threads):**
| Operation | Throughput |
|-----------|------------|
| Write | 148,767 ops/sec |
| Read | 210,073 ops/sec |
| Mixed | 142,025 ops/sec |

**Concurrent (16 threads):**
| Operation | Throughput |
|-----------|------------|
| Write | 200,344 ops/sec |
| Read | 209,424 ops/sec |
| Mixed | 158,298 ops/sec |

### Dart FFI vs Node.js N-API

| Operation | Dart FFI (ops/sec) | Node.js N-API (ops/sec) | Notes |
|-----------|-------------------|-----------------------|-------|
| putSync   | ~62,500 | ~56,000 | Similar performance |
| getSync   | ~166,000 | ~260,000 | Node.js ~1.5x faster |
| put async | ~50,000 | N/A | - |
| get async | ~166,000 | N/A | - |

*Benchmarks run on Linux x86_64. Results vary by platform and workload.*

Both bindings provide excellent performance for most use cases. The Dart FFI is well-suited for Flutter applications requiring native database access.

## Known Limitations

### getObject / getObjectSync
Throws `NOT_SUPPORTED` - requires C scan API for reconstruction. Use `createReadStream` to iterate over entries and reconstruct objects manually.

### Reverse Iteration
The `reverse` parameter in `createReadStream` is reserved for future use. The C API doesn't support reverse iteration yet.

## Architecture

```
bindings/dart/
├── lib/
│   ├── wavedb.dart          # Public API exports
│   ├── src.dart             # Internal exports
│   └── src/
│       ├── database.dart    # WaveDB main class
│       ├── iterator.dart    # Stream-based iterator
│       ├── exceptions.dart  # WaveDBException
│       ├── path.dart        # PathConverter (Dart ↔ path_t)
│       ├── identifier.dart  # IdentifierConverter (Dart ↔ identifier_t)
│       ├── object_ops.dart  # Object flattening/reconstruction
│       └── native/
│           ├── types.dart   # FFI opaque types
│           ├── wavedb_library.dart  # Library loader
│           └── wavedb_bindings.dart # FFI bindings
├── test/
│   ├── wavedb_test.dart     # Integration tests
│   ├── object_ops_test.dart # ObjectOps tests
│   ├── conversion_test.dart # Converter tests
│   └── library_test.dart   # Library loader tests
├── example/
│   └── example.dart         # Usage example
└── benchmark/
    ├── benchmark.dart       # Dart benchmark
    └── compare.sh           # Comparison script
```

## License

GNU General Public License v3.0 or later