<div align="center">
  <img src="wave_fuji.svg" alt="WaveDB Logo" width="200"/>

  # WaveDB Node.js Bindings
</div>

[Node.js](https://nodejs.org/) bindings for [WaveDB](../../README.md) - A hierarchical B+trie database. It is a love child of ForrestDB and MUMPS.

## Installation

```bash
npm install wavedb
```

## Quick Start

```javascript
const { WaveDB } = require('wavedb');

// Open database
const db = new WaveDB('/path/to/db');

// Async operations
await db.put('users/alice/name', 'Alice');
const name = await db.get('users/alice/name');

// Sync operations
db.putSync('users/bob/name', 'Bob');
const name2 = db.getSync('users/bob/name');

// Object operations
await db.putObject({
  users: {
    alice: { name: 'Alice', age: '30' }
  }
});

const user = await db.getObject('users/alice');
// { name: 'Alice', age: '30' }

// Stream all entries
db.createReadStream()
  .on('data', ({ key, value }) => {
    console.log(key, '=', value);
  })
  .on('end', () => {
    console.log('Done');
  });

// Close database
db.close();
```

## API Reference

### Constructor

```javascript
const db = new WaveDB(path, options);
```

- `path` (string): Path to database directory
- `options` (object):
  - `delimiter` (string): Key path delimiter (default: '/')

### Async Operations

```javascript
// Put
await db.put(key, value);

// Get (returns null if not found)
const value = await db.get(key);

// Delete
await db.del(key);

// Batch
await db.batch([
  { type: 'put', key: 'k1', value: 'v1' },
  { type: 'del', key: 'k2' }
]);
```

All async operations support both Promise and callback patterns:

```javascript
// Promise
await db.put('key', 'value');

// Callback
db.put('key', 'value', (err) => {
  if (err) throw err;
});
```

### Sync Operations

```javascript
db.putSync(key, value);
const value = db.getSync(key);
db.delSync(key);
db.batchSync(ops);
```

### Object Operations

```javascript
// Flatten object to paths
await db.putObject({
  users: {
    alice: { name: 'Alice', roles: ['admin', 'user'] }
  }
});
// Creates:
//   users/alice/name → 'Alice'
//   users/alice/roles/0 → 'admin'
//   users/alice/roles/1 → 'user'

// Reconstruct object from subtree
const user = await db.getObject('users/alice');
// { name: 'Alice', roles: ['admin', 'user'] }
```

### Streams

```javascript
db.createReadStream(options)
  .on('data', ({ key, value }) => { })
  .on('end', () => { });
```

**Options:**
- `start`: Start path (inclusive)
- `end`: End path (exclusive)
- `reverse`: Reverse order (default: false)
- `keys`: Include keys (default: true)
- `values`: Include values (default: true)
- `keyAsArray`: Return keys as arrays (default: false)

**Early termination:**

```javascript
const stream = db.createReadStream();
stream.on('data', ({ key, value }) => {
  if (key === 'target') {
    stream.destroy();  // End stream early
  }
});
```

### Keys

Keys can be strings or arrays:

```javascript
// String with delimiter
await db.put('users/alice/name', 'Alice');

// Array
await db.put(['users', 'bob', 'name'], 'Bob');

// Custom delimiter
const db = new WaveDB('/path/to/db', { delimiter: ':' });
await db.put('users:charlie:name', 'Charlie');
```

### Values

Values can be strings or Buffers:

```javascript
// String
await db.put('key', 'value');

// Buffer
await db.put('binary/key', Buffer.from([0x01, 0x02]));
```

### Error Handling

```javascript
try {
  await db.put('key', 'value');
} catch (err) {
  console.error('Database error:', err.message);
}

// With callbacks
db.get('key', (err, value) => {
  if (err) {
    console.error('Error:', err.message);
    return;
  }
  console.log('Value:', value);
});
```

## Building from Source

### Prerequisites

- Node.js >= 14.0.0
- CMake >= 3.14
- C compiler (gcc, clang, or MSVC)

### Build Steps

```bash
# Clone repository
git clone https://github.com/vijayee/WaveDB.git
cd WaveDB

# Build WaveDB library
mkdir build && cd build
cmake ..
make

# Build Node.js bindings
cd ../bindings/nodejs
npm install
npm run build
```

### Run Tests

```bash
npm test
```

### Memory Leak Detection

```bash
npm run test:valgrind
```

## Architecture

The bindings use [node-addon-api](https://github.com/nodejs/node-addon-api) (C++ wrapper for N-API) with the AsyncWorker pattern for non-blocking operations.

**Key components:**
- `binding.cpp`: Module initialization
- `database.cc`: WaveDB class wrapper
- `path.cc`: JavaScript ↔ path_t conversion
- `identifier.cc`: JavaScript ↔ identifier_t conversion
- `*_worker.cc`: Async workers for put/get/del/batch
- `iterator.cc`: Stream iterator

## Performance

Benchmarks run on Linux x86_64 with Node.js v20.5.1:

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

### Node.js Bindings Performance

Benchmarks run on Node.js v20.5.1 with 10,000 iterations:

**Async Operations (non-blocking, thread-pool based):**
- `put`: ~44,000 ops/sec
- `get`: ~70,000 ops/sec

**Sync Operations (blocking, direct C++ calls):**
- `putSync`: ~83,000 ops/sec
- `getSync`: ~468,000 ops/sec

**Batch Operations:**
- `batch`: ~72,000 ops/sec (1,000 operations per batch)
- Recommended for bulk inserts: 10-100x faster than individual puts

**Stream Operations:**
- Internal buffer of 100 entries
- Backpressure handled automatically
- Suitable for large dataset iteration

**Performance Tips:**
- Use async operations for production (non-blocking event loop)
- Use sync operations for initialization/migration scripts
- Batch operations for bulk data loading
- Streams for iterating over large datasets

**MVCC & WAL:**
- Multi-Version Concurrency Control enables lock-free reads
- Write-Ahead Logging ensures durability
- Atomic transaction IDs with CLOCK_MONOTONIC for stable ordering
- Optimized with atomic operations (no mutex contention)

## License

MIT