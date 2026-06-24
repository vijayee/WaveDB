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

// Batched helpers — 150x faster than individual async puts
await db.putMany([['k1', 'v1'], ['k2', 'v2'], ['k3', 'v3']]);
const results = await db.getMany(['k1', 'k2', 'k3']);
await db.deleteMany(['k1', 'k2']);

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
| `'symmetric'` | 32-byte `Buffer` | Simple setups, embedded devices |
| `'asymmetric'` | DER-encoded RSA key pair | Key rotation, write-only nodes |

### Symmetric Encryption

```javascript
const { WaveDB, EncryptionError } = require('wavedb');

const key = Buffer.alloc(32); // your 32-byte AES-256 key
// ... fill key with crypto.randomBytes(32) or a derived key ...

const db = new WaveDB('/path/to/db', {
  enablePersist: true,
  encryption: {
    type: 'symmetric',
    key: key            // required, exactly 32 bytes
  }
});

// All operations work transparently — encryption is internal
await db.put('users/alice/name', 'Alice');
const name = await db.get('users/alice/name');

// Reopen with the same key
const db2 = new WaveDB('/path/to/db', {
  enablePersist: true,
  encryption: { type: 'symmetric', key }
});
```

### Asymmetric Encryption

```javascript
const { WaveDB } = require('wavedb');
const fs = require('fs');

const publicKey = fs.readFileSync('public_key.der');
const privateKey = fs.readFileSync('private_key.der');

const db = new WaveDB('/path/to/db', {
  enablePersist: true,
  encryption: {
    type: 'asymmetric',
    publicKey: publicKey,    // required, DER-encoded
    privateKey: privateKey   // optional — omit for write-only mode
  }
});
```

In asymmetric mode, a random data encryption key (DEK) is generated per session and wrapped with the RSA public key. The private key is required for decryption; a write-only node can omit `privateKey`.

### Encryption Errors

| Condition | Error | Message |
|-----------|-------|---------|
| Opening encrypted DB without a key | `EncryptionError` | "Encryption required: this database was created with encryption" |
| Wrong key on re-open | `EncryptionError` | "Invalid encryption key" |
| Adding encryption to unencrypted DB | `EncryptionError` | "Encryption unsupported" |
| Symmetric key not 32 bytes | `RangeError` | "encryption.key must be exactly 32 bytes for AES-256" |
| Missing required key material | `TypeError` | "encryption.key is required for symmetric encryption..." |

```javascript
try {
  const db = new WaveDB('/path/to/db', { encryption: { type: 'symmetric', key } });
} catch (err) {
  if (err instanceof EncryptionError) {
    console.error('Encryption failed:', err.message);
  }
}
```

## API Reference

### Async Operations

All async methods return Promises and dispatch to the C worker pool (non-blocking).

```javascript
await db.put(key, value)           // Store a value
const val = await db.get(key)      // Retrieve (null if not found)
await db.del(key)                   // Delete
await db.batch(operations)          // Atomic batch of put/del
await db.putMany(items)             // Batch-put key-value pairs (150x faster)
const vals = await db.getMany(keys) // Concurrent get (100x faster)
await db.deleteMany(keys)           // Batch-delete keys (150x faster)
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

## Graph Schema Layer

A triple-store (subject-predicate-object) with Gremlin-style traversal, schema-driven indexing, and cost-based query optimization.

```javascript
const { GraphLayer, g } = require('wavedb');

const graph = new GraphLayer();

// Insert triples
graph.insertSync('clip_abc', 'tagged_with', 'gaming');
graph.insertSync('clip_abc', 'tagged_with', 'tutorial');
graph.insertSync('clip_xyz', 'tagged_with', 'gaming');

// Traversal with query builder
const tags = g.V('clip_abc').Out('tagged_with').All();
// ['gaming', 'tutorial']

// Incoming edges
const clips = g.V('gaming').In('tagged_with').All();
// ['clip_abc', 'clip_xyz']

// Intersection
const both = g.V('gaming').In('tagged_with')
  .And(g.V('tutorial').In('tagged_with')).All();
// ['clip_abc']

// Union
const any = g.V('clip_abc').Out('tagged_with')
  .Or(g.V('clip_xyz').Out('tagged_with')).All();

// Difference
const only = g.V('gaming').In('tagged_with')
  .Not(g.V('tutorial').In('tagged_with')).All();
// ['clip_xyz']

graph.close();
```

### Schema and Indexing

Define types and index hints to enable multi-index lookups and filter pushdown:

```javascript
graph.parseSchema(`
  type Clip @index(spo, pos) {
    tagged_with: [Tag];
    name: String @index(pos);
    age: Int @index(pos);
  }
`);

// Schema enables POS-index scan for Has filters
const named = g.Has('name', 'My Clip').All();

// Range predicates use POS range scans
const adults = g.HasGte('age', '25').All();
```

### Range Predicates

| Method | Operator | Example |
|--------|----------|---------|
| `Has(pred, val)` | `=` | `g.Has('age', '25')` |
| `HasGt(pred, val)` | `>` | `g.HasGt('age', '25')` |
| `HasGte(pred, val)` | `>=` | `g.HasGte('age', '25')` |
| `HasLt(pred, val)` | `<` | `g.HasLt('age', '25')` |
| `HasLte(pred, val)` | `<=` | `g.HasLte('age', '25')` |

### DSL Execution

```javascript
// Direct DSL string
const result = graph.exec('g.V("clip_abc").Out("tagged_with")');

// Async query
const result = await graph.query('g.V("gaming").In("tagged_with")');

// Count
const n = graph.count('g.V("gaming").In("tagged_with")');
```

### Morphisms

Reusable query fragments:

```javascript
graph.defineMorphism('friends_content',
  'g.Morphism("friends_content").Out("follows").Out("likes")');

const result = g.V('alice').Follow('friends_content').All();
```

### Async Operations

```javascript
await graph.insert('clip_abc', 'tagged_with', 'gaming');
await graph.del('clip_abc', 'tagged_with', 'gaming');
const result = await graph.query('g.V("clip_abc").Out("tagged_with")');
```

### Query Builder API

| Method | Description |
|--------|-------------|
| `g.V(id)` | Start from a vertex |
| `.Out(pred)` | Traverse outgoing edges |
| `.In(pred)` | Traverse incoming edges |
| `.Has(pred, val)` | Filter by predicate=value |
| `.HasGt/Gte/Lt/Lte(pred, val)` | Range filter |
| `.And(sub)` | Intersect with sub-query |
| `.Or(sub)` | Union with sub-query |
| `.Not(sub)` | Difference from sub-query |
| `.Limit(n)` | Limit results |
| `.Follow(name)` | Follow a morphism |
| `.All()` | Execute and return results array |
| `.Count()` | Execute and return count |
| `.toString()` | Render DSL string (debugging) |

## Subtrees

Subtrees provide scoped views into a database with automatic prefix isolation, enabling multiple schema layers to coexist on the same database.

```javascript
const { WaveDB, GraphQLLayer, GraphLayer } = require('wavedb');

const db = new WaveDB('/path/to/db');

// Open subtrees with different prefixes
const gqlSub = db.openSubtree('graphql');
const graphSub = db.openSubtree('graph');

// Each subtree is isolated — same key in different subtrees
gqlSub.putSync('Users/1/name', 'Alice');
graphSub.putSync('Users/1/name', 'Bob');

console.log(gqlSub.getSync('Users/1/name'));   // 'Alice'
console.log(graphSub.getSync('Users/1/name'));  // 'Bob'
console.log(gqlSub.count());   // 1
console.log(graphSub.count()); // 1

// Delete all data under a prefix
db.deleteSubtree('graphql');

// Close subtrees (does not close the database)
gqlSub.close();
graphSub.close();
```

### Layer Coexistence

Create multiple layers on the same database by passing a subtree:

```javascript
const gqlSub = db.openSubtree('layer/graphql');
const graphSub = db.openSubtree('layer/graph');

const gqlLayer = new GraphQLLayer('/path/to/db', { subtree: gqlSub });
const graphLayer = new GraphLayer('/path/to/db', { subtree: graphSub });

// Both layers operate independently on the same database
```

Without a subtree, creating a layer on a database owned by a different layer type throws:

```javascript
// Database already has GraphQL data — creating Graph layer fails
try {
  const graph = new GraphLayer('/path/to/db');
} catch (err) {
  // err.message includes "different layer type"
  // Suggests using a subtree for isolation
}
```

### Subtree API

| Method | Description |
|--------|-------------|
| `db.openSubtree(prefix, delimiter)` | Open a scoped subtree view |
| `db.deleteSubtree(prefix, delimiter)` | Delete all keys under a prefix |
| `subtree.putSync(key, value)` | Synchronous put |
| `subtree.getSync(key)` | Synchronous get (null if not found) |
| `subtree.delSync(key)` | Synchronous delete |
| `subtree.batchSync(ops)` | Synchronous batch of put/del |
| `subtree.scanSyncRaw(prefix)` | Scan keys matching prefix |
| `subtree.count()` | Count entries in subtree |
| `subtree.snapshot()` | Snapshot underlying database |
| `subtree.put(key, value)` | Async put (returns Promise) |
| `subtree.get(key)` | Async get (returns Promise) |
| `subtree.del(key)` | Async delete (returns Promise) |
| `subtree.close()` | Close subtree view |

## Error Handling

```javascript
try {
  await db.put('key', 'value');
} catch (err) {
  console.error('Database error:', err.message);
}
```

Error codes: `NOT_FOUND`, `INVALID_PATH`, `IO_ERROR`, `DATABASE_CLOSED`, `INVALID_ARGUMENT`, `ENCRYPTION`

## Performance

Benchmarks on Linux x86_64, Node.js v26, 50MB LRU cache, default config (4 worker threads).

### Sync Operations

| Operation | Throughput |
|-----------|------------|
| `putSync` | 1.3K ops/sec (WAL-bound) |
| `getSync` | 304K ops/sec |

### Async Operations

| Operation | Throughput |
|-----------|------------|
| `put` (sequential) | 1.3K ops/sec |
| `putMany` (5000/batch) | **189K ops/sec (150x)** |
| `getMany` (concurrent 5000) | **134K ops/sec (100x)** |
| `batch` (5000/batch) | 237K ops/sec |

### Concurrent Throughput

| Concurrency | `put` | `get` |
|-------------|-------|-------|
| 1 | 1.2K ops/sec | 45K ops/sec |
| 4 | 2.6K ops/sec | 106K ops/sec |
| 32 | 2.2K ops/sec | 114K ops/sec |

**Tips:** Use `putMany`/`getMany`/`deleteMany` for throughput-sensitive workloads. Scale concurrency to 4-8 for best concurrent throughput. Sync puts are WAL-bound; sync gets are memory-speed.

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