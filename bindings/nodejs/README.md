<div align="center">
  <img src="wave_fuji.svg" alt="WaveDB Logo" width="200"/>

  # WaveDB Node.js Bindings
</div>

Node.js bindings for [WaveDB](../../README.md) — a hierarchical key-value database with MVCC, WAL durability, and schema layer access.

## Installation

```bash
npm install wavedb
```

## Quick Start

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
  { type: 'put', key: 'users/alice/name', value: 'Alice' },
  { type: 'del', key: 'users/bob/name' }
]);

// Iterator
const iter = new Iterator(db, { start: 'users/alice', end: 'users/alice~' });
let entry;
while ((entry = iter.read())) {
  console.log(entry.key, entry.value);
}
iter.end();

// Stream
db.createReadStream({ start: 'users/', end: 'users/~' })
  .on('data', ({ key, value }) => console.log(key, '=', value))
  .on('end', () => console.log('Done'));

db.close();
```

## Configuration

```javascript
const db = new WaveDB(path, options);
```

| Option | Default | Description |
|--------|---------|-------------|
| `delimiter` | `'/'` | Key path delimiter |
| `chunkSize` | `4` | HBTrie chunk size (immutable, set at creation) |
| `btreeNodeSize` | `4096` | B+tree node size (immutable) |
| `enablePersist` | `true` | Persist data to disk (immutable) |
| `lruMemoryMb` | `50` | LRU cache size in MB |
| `lruShards` | `64` | LRU shard count (0 = auto-scale) |
| `workerThreads` | `4` | Worker pool size |
| `wal.syncMode` | `'debounced'` | `'immediate'`, `'debounced'`, or `'async'` |
| `wal.debounceMs` | `250` | fsync debounce window (ms) |
| `wal.maxFileSize` | `131072` | Max WAL file size before sealing |

### WAL Sync Modes

| Mode | Behavior | Durability | Throughput |
|------|----------|------------|------------|
| `'immediate'` | fsync after every write | Highest | ~1K ops/sec |
| `'debounced'` | batched fsync (default 250ms) | High | ~300K ops/sec |
| `'async'` | buffered write, idle drain every 250ms | Process crash only | ~400K ops/sec |

## API Reference

### Async Operations

All async methods return Promises and dispatch to the C worker pool (non-blocking).

```javascript
await db.put(key, value)           // Store a value
const val = await db.get(key)      // Retrieve (null if not found)
await db.del(key)                   // Delete
await db.batch(operations)          // Atomic batch of put/del
await db.putObject(key, obj)        // Store nested object as flattened paths
const obj = await db.getObject(key) // Reconstruct nested object
```

### Callback Operations

Same as async, but with Node.js-style error-first callbacks. Also returns a Promise.

```javascript
db.putCb(key, value, (err) => { /* ... */ })
db.getCb(key, (err, value) => { /* ... */ })
db.delCb(key, (err) => { /* ... */ })
db.batchCb(operations, (err) => { /* ... */ })
```

### Sync Operations

Block the event loop. Use for initialization/migration scripts, not hot paths.

```javascript
db.putSync(key, value)
const val = db.getSync(key)      // null if not found
db.delSync(key)
db.batchSync(operations)
db.putObjectSync(key, obj)
const obj = db.getObjectSync(key)
```

### Keys and Values

```javascript
// String keys with delimiter
await db.put('users/alice/name', 'Alice');

// Array keys
await db.put(['users', 'bob', 'name'], 'Bob');

// Custom delimiter
const db = new WaveDB('/path/to/db', { delimiter: ':' });
await db.put('users:charlie:name', 'Charlie');

// String or Buffer values
await db.put('key', 'text value');
await db.put('binary/key', Buffer.from([0x01, 0x02]));
```

### Streams

```javascript
db.createReadStream({ start, end, reverse, keys, values, keyAsArray })
  .on('data', ({ key, value }) => { /* ... */ })
  .on('end', () => { /* ... */ });
```

| Option | Default | Description |
|--------|---------|-------------|
| `start` | — | Start path (inclusive) |
| `end` | — | End path (exclusive) |
| `reverse` | `false` | Reverse order |
| `keys` | `true` | Include keys |
| `values` | `true` | Include values |
| `keyAsArray` | `false` | Return keys as arrays |

## GraphQL Schema Layer

```javascript
const { GraphQLLayer } = require('wavedb');

const layer = new GraphQLLayer('/path/to/db', {
  enablePersist: true,
  chunkSize: 4,
  workerThreads: 4
});

// Define schema
layer.parseSchema(`
  type User {
    name: String
    age: Int
  }
`);

// Query
const result = layer.querySync('{ User { name } }');
// { success: true, data: { User: { name: null } }, errors: [] }

// Mutation
const created = layer.mutateSync('mutation { createUser(name: "Alice") { id name } }');
// { success: true, data: { createUser: { id: "abc123", name: "Alice" } } }

// Async variants
const result = await layer.query('{ User { name } }');
const created = await layer.mutate('mutation { createUser(name: "Bob") { id } }');

layer.close();
```

### Introspection

```javascript
const schema = await layer.query('{ __schema { types { name kind } } }');
const userType = await layer.query('{ __type(name: "User") { name kind fields { name } } }');
```

### Required Fields

Fields marked `!` in the schema are required for create mutations:

```javascript
layer.parseSchema('type User { name: String! age: Int }');

// Missing required field — fails
layer.mutateSync('mutation { createUser(age: "30") { id } }');
// { success: false, errors: ['Missing required fields: name'] }

// Providing required field — succeeds
layer.mutateSync('mutation { createUser(name: "Alice") { id name } }');
// { success: true, data: { createUser: { id: '1', name: 'Alice' } } }
```

## Error Handling

```javascript
try {
  await db.put('key', 'value');
} catch (err) {
  console.error('Database error:', err.message);
}
```

Error codes: `NOT_FOUND`, `INVALID_PATH`, `IO_ERROR`, `DATABASE_CLOSED`, `INVALID_ARGUMENT`

## Performance

Benchmarks on Linux x86_64, Node.js v24, 50MB LRU cache, 4 worker threads.

### C Library (ASYNC WAL, no work pool, single-threaded)

| Operation | Throughput | P50 Latency | P99 Latency |
|-----------|------------|-------------|-------------|
| Get | 2.11M ops/sec | 474 ns | 490 ns |
| Put | 446K ops/sec | 2.02 µs | 4.80 µs |
| Delete | 268K ops/sec | 3.52 µs | 7.33 µs |
| Mixed (70% read) | 2.17M ops/sec | 459 ns | 490 ns |

### Node.js (DEBOUNCED WAL, 4 worker threads)

| Operation | Throughput |
|-----------|------------|
| `getSync` | 203K ops/sec |
| `putSync` | 1.2K ops/sec |
| `get` (async) | 31K ops/sec |
| `put` (async) | 950 ops/sec |
| `batch` (async, 1K ops) | 54K ops/sec |

### Node.js Concurrent Throughput (DEBOUNCED WAL)

| Concurrency | `put` | `get` |
|-------------|-------|-------|
| 1 | 1.1K ops/sec | 41K ops/sec |
| 2 | 1.7K ops/sec | 58K ops/sec |
| 4 | 2.3K ops/sec | 67K ops/sec |
| 8 | 2.0K ops/sec | 13K ops/sec |
| 16 | 2.0K ops/sec | 11K ops/sec |
| 32 | 2.0K ops/sec | 14K ops/sec |

**Tips:** Use async operations in production. Use sync for scripts/initialization. Batch for bulk loads (10-100x faster than individual puts).

## Building from Source

```bash
# Build C library
cd WaveDB && mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j$(nproc)

# Build Node.js addon
cd ../bindings/nodejs
npm install
npm run build

# Run tests
npm test
```

## License

MIT