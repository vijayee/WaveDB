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
    walDebounceMs: 250,
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
| `walDebounceMs` | `250` | fsync debounce window (ms) |
| `walMaxFileSize` | `131072` | Max WAL file size before sealing |
| `encryption` | — | Encryption configuration (see below) |

### WAL Sync Modes

| Mode | Behavior | Durability | Throughput |
|------|----------|------------|------------|
| `'immediate'` | fsync after every write | Highest | ~1K ops/sec |
| `'debounced'` | batched fsync (default 250ms) | High | ~300K ops/sec |
| `'async'` | buffered write, idle drain every 250ms | Process crash only | ~400K ops/sec |

## Encryption

WaveDB supports AES-256-GCM encryption at rest for WAL files and persisted pages. Encryption is set at database creation and cannot be changed on an existing database. Key material is never persisted — only a salt and verification check are stored.

| Mode | Key | Use case |
|------|-----|----------|
| `'symmetric'` | 32-byte `Uint8List` | Simple setups, embedded devices |
| `'asymmetric'` | DER-encoded RSA key pair | Key rotation, write-only nodes |

### Symmetric Encryption

```dart
import 'package:wavedb/wavedb.dart';
import 'dart:typed_data';

final key = Uint8List.fromList(List.filled(32, 0)); // your 32-byte AES-256 key

final db = WaveDB('/path/to/db',
  delimiter: '/',
  encryption: WaveDBEncryption.symmetric(key: key),
);

// All operations work transparently — encryption is internal
db.putSync('users/alice/name', 'Alice');
final name = db.getSync('users/alice/name');

// Reopen with the same key
final db2 = WaveDB('/path/to/db',
  delimiter: '/',
  encryption: WaveDBEncryption.symmetric(key: key),
);
```

### Asymmetric Encryption

```dart
import 'dart:io';

final publicKey = File('public_key.der').readAsBytesSync();
final privateKey = File('private_key.der').readAsBytesSync();

final db = WaveDB('/path/to/db',
  delimiter: '/',
  encryption: WaveDBEncryption.asymmetric(
    publicKey: publicKey,    // required, DER-encoded
    privateKey: privateKey,  // optional — omit for write-only mode
  ),
);
```

In asymmetric mode, a random data encryption key (DEK) is generated per session and wrapped with the RSA public key. The private key is required for decryption; a write-only node can omit `privateKey`.

### Encryption Errors

Encryption failures throw `WaveDBException` with code `IO_ERROR`:

| Condition | Exception |
|-----------|-----------|
| Opening encrypted DB without a key | `WaveDBException(IO_ERROR)` |
| Wrong key on re-open | `WaveDBException(IO_ERROR)` |
| Adding encryption to unencrypted DB | `WaveDBException(IO_ERROR)` |
| Invalid encryption type | `ArgumentError` |

```dart
try {
  final db = WaveDB('/path/to/db',
    encryption: WaveDBEncryption.symmetric(key: key));
} on WaveDBException catch (e) {
  if (e.code == 'IO_ERROR') {
    print('Encryption failed: ${e.message}');
  }
}
```

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

Benchmarks on Linux x86_64, Dart 3.11, 50MB LRU cache, 32 worker threads, async WAL.

### Dart FFI Sync (ASYNC WAL, 32 worker threads)

| Operation | Throughput |
|-----------|------------|
| `putSync` | 127K ops/sec |
| `getSync` | 417K ops/sec |

### Dart Concurrent Throughput (ASYNC WAL, 32 worker threads)

| Concurrency | `put` | `get` |
|-------------|-------|-------|
| 1 | 28K ops/sec | 41K ops/sec |
| 2 | 43K ops/sec | 64K ops/sec |
| 4 | 65K ops/sec | 81K ops/sec |
| 8 | 80K ops/sec | 101K ops/sec |
| 16 | 88K ops/sec | 92K ops/sec |
| 32 | 86K ops/sec | 78K ops/sec |

### Comparison with Node.js (ASYNC WAL, 32 worker threads)

| Operation | Dart FFI | Node.js N-API | Ratio |
|-----------|-----------|---------------|-------|
| `putSync` | 127K ops/sec | 1.3K ops/sec | Dart 98x |
| `getSync` | 417K ops/sec | 304K ops/sec | Dart 1.4x |
| `put` (c=32) | 86K ops/sec | 2.2K ops/sec | Dart 39x |
| `get` (c=32) | 78K ops/sec | 114K ops/sec | Node 1.5x |

Dart's FFI avoids N-API's per-call overhead, giving dramatically higher sync and async put throughput. Node.js async gets scale better at high concurrency due to its event-loop architecture.

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