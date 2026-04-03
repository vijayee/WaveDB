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
- `WaveDB(String path, {String delimiter = '/'})` - Open or create database at path

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

### Expected Performance

Dart FFI bindings have comparable performance to Node.js N-API bindings:

| Operation | Dart FFI (ops/sec) | Node.js N-API (ops/sec) | Notes |
|-----------|-------------------|-----------------------|-------|
| putSync   | ~62,500 | ~56,000 | Similar performance |
| getSync   | ~166,000 | ~260,000 | Node.js ~1.5x faster |
| put async | ~50,000 | N/A | - |
| get async | ~166,000 | N/A | - |

*Benchmarks run on Linux x86_64 with 500 iterations. Results vary by platform and workload.*

Both bindings provide excellent performance for most use cases. The Dart FFI is well-suited for Flutter applications requiring native database access.

## Known Limitations

### getObject / getObjectSync
Throws `NOT_SUPPORTED` - requires C `database_scan` API not yet implemented.

### createReadStream
Throws `NOT_SUPPORTED` when used - requires C `database_scan_start/next/end` API not yet implemented.

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