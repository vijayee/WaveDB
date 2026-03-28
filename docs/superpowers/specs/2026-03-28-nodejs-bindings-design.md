# Node.js Bindings for WaveDB - Design Document

**Goal:** Provide a LevelUP-compatible Node.js API for WaveDB with async/sync operations, streaming, and object manipulation capabilities.

**Architecture:** Node.js native bindings using node-addon-api with AsyncWorker pattern. Wraps database_t (not hbtrie_t) as the primary interface, treating HBTrie as an implementation detail.

**Tech Stack:**
- node-addon-api (C++ wrapper for N-API)
- AsyncWorker pattern for non-blocking operations
- Node.js streams for iteration
- JSON serialization for object operations

---

## Architecture

### Module Structure

**Core Principle:** Expose database_t as the primary interface, not hbtrie_t. HBTrie is an internal implementation detail.

```
bindings/nodejs/
├── src/                    # C++ native code
│   ├── binding.cpp         # Module init
│   ├── database.cc         # WaveDB class
│   ├── path.cc             # Path conversion
│   ├── identifier.cc       # Value conversion
│   ├── async_worker.cc     # Base async worker
│   ├── put_worker.cc       # Async put
│   ├── get_worker.cc       # Async get
│   ├── del_worker.cc       # Async delete
│   ├── batch_worker.cc     # Async batch
│   └── iterator.cc         # Stream iterator
├── lib/                    # JavaScript API
│   ├── wavedb.js           # Main export
│   └── iterator.js         # Stream wrapper
├── test/                   # Tests
│   ├── wavedb.test.js
│   └── integration.test.js
├── package.json
├── binding.gyp
└── README.md
```

### Database Wrapper Class

**C++ Side:**
```cpp
class WaveDB : public Napi::ObjectWrap<WaveDB> {
public:
  static Napi::Object Init(Napi::Env env, Napi::Object exports);

private:
  database_t* db_;

  // Async methods
  Napi::Value Put(const Napi::CallbackInfo& info);
  Napi::Value Get(const Napi::CallbackInfo& info);
  Napi::Value Delete(const Napi::CallbackInfo& info);
  Napi::Value Batch(const Napi::CallbackInfo& info);

  // Sync methods
  Napi::Value PutSync(const Napi::CallbackInfo& info);
  Napi::Value GetSync(const Napi::CallbackInfo& info);
  Napi::Value DeleteSync(const Napi::CallbackInfo& info);
  Napi::Value BatchSync(const Napi::CallbackInfo& info);

  // Object operations
  Napi::Value PutObject(const Napi::CallbackInfo& info);
  Napi::Value GetObject(const Napi::CallbackInfo& info);

  // Streaming
  Napi::Value CreateReadStream(const Napi::CallbackInfo& info);
};
```

**JavaScript Side:**
```javascript
class WaveDB {
  constructor(path, options = {}) {
    // Initialize native binding
    this._db = new WaveDBNative(path, options);
  }

  // Async operations (callback optional, returns Promise)
  async put(key, value, callback) { /* ... */ }
  async get(key, callback) { /* ... */ }
  async del(key, callback) { /* ... */ }
  async batch(ops, callback) { /* ... */ }

  // Sync operations
  putSync(key, value) { /* ... */ }
  getSync(key) { /* ... */ }
  delSync(key) { /* ... */ }
  batchSync(ops) { /* ... */ }

  // Object operations
  async putObject(obj, callback) { /* ... */ }
  async getObject(path, callback) { /* ... */ }

  // Streaming
  createReadStream(options) { /* ... */ }
}
```

---

## Key and Value Conversion

### Path Conversion

**Configurable delimiter (default: '/'):**
```javascript
const db = new WaveDB('/path/to/db', { delimiter: '/' });

// String key → path_t
await db.put('users/alice/name', 'Alice');
// → path_t { identifiers: ['users', 'alice', 'name'] }

// Array key → path_t
await db.put(['users', 'bob', 'age'], '30');
// → path_t { identifiers: ['users', 'bob', 'age'] }
```

**C++ implementation:**
```cpp
// path.cc
path_t* PathFromJS(Napi::Env env, Napi::Value key, char delimiter) {
  path_t* path = path_create();

  if (key.IsString()) {
    std::string str = key.As<Napi::String>().Utf8Value();
    std::vector<std::string> parts = Split(str, delimiter);

    for (const auto& part : parts) {
      buffer_t* buf = buffer_create_from_pointer_copy(
        (uint8_t*)part.c_str(), part.size());
      identifier_t* id = identifier_create(buf, 0);
      buffer_destroy(buf);
      path_append(path, id);
      identifier_destroy(id);
    }
  } else if (key.IsArray()) {
    Napi::Array arr = key.As<Napi::Array>();
    for (uint32_t i = 0; i < arr.Length(); i++) {
      std::string part = arr.Get(i).As<Napi::String>().Utf8Value();
      buffer_t* buf = buffer_create_from_pointer_copy(
        (uint8_t*)part.c_str(), part.size());
      identifier_t* id = identifier_create(buf, 0);
      buffer_destroy(buf);
      path_append(path, id);
      identifier_destroy(id);
    }
  }

  return path;
}

std::string PathToJS(path_t* path, char delimiter) {
  std::string result;
  for (size_t i = 0; i < path->identifiers.length; i++) {
    if (i > 0) result += delimiter;
    identifier_t* id = path->identifiers.data[i];
    // Extract identifier bytes to string
    result += IdentifierToString(id);
  }
  return result;
}
```

### Value Conversion

**JavaScript → identifier_t:**
```cpp
// identifier.cc
identifier_t* ValueFromJS(Napi::Env env, Napi::Value value) {
  if (value.IsString()) {
    std::string str = value.As<Napi::String>().Utf8Value();
    buffer_t* buf = buffer_create_from_pointer_copy(
      (uint8_t*)str.c_str(), str.size());
    identifier_t* id = identifier_create(buf, 0);
    buffer_destroy(buf);
    return id;
  } else if (value.IsBuffer()) {
    Napi::Buffer<uint8_t> buf = value.As<Napi::Buffer<uint8_t>>();
    buffer_t* b = buffer_create_from_pointer_copy(
      buf.Data(), buf.Length());
    identifier_t* id = identifier_create(b, 0);
    buffer_destroy(b);
    return id;
  }
  throw Napi::TypeError::New(env, "Value must be string or Buffer");
}
```

**identifier_t → JavaScript:**
```cpp
Napi::Value ValueToJS(Napi::Env env, identifier_t* id) {
  if (id == nullptr) {
    return env.Null();
  }

  // Convert to string if printable ASCII, otherwise Buffer
  std::vector<uint8_t> bytes;
  for (size_t i = 0; i < id->chunks.length; i++) {
    chunk_t* chunk = id->chunks.data[i];
    bytes.insert(bytes.end(),
      chunk_data_const(chunk),
      chunk_data_const(chunk) + chunk->data->size);
  }

  if (IsPrintableASCII(bytes)) {
    return Napi::String::New(env,
      std::string(bytes.begin(), bytes.end()));
  } else {
    return Napi::Buffer<uint8_t>::Copy(env,
      bytes.data(), bytes.size());
  }
}
```

---

## Async Operations

### AsyncWorker Pattern

**Base class:**
```cpp
class WaveDBAsyncWorker : public Napi::AsyncWorker {
public:
  WaveDBAsyncWorker(Napi::Env env, Napi::Function callback)
    : AsyncWorker(callback), deferred_(Napi::Promise::Deferred::New(env)) {}

  void Execute() override {
    // Implemented by subclass
  }

  void OnOK() override {
    Napi::HandleScope scope(Env());
    Napi::Value result = GetResult();
    Callback().Call({ env.Null(), result });
    deferred_.Resolve(result);
  }

  void OnError(const std::string& error) override {
    Napi::HandleScope scope(Env());
    Napi::Error err = Napi::Error::New(Env(), error);
    Callback().Call({ err.Value(), env.Undefined() });
    deferred_.Reject(err.Value());
  }

protected:
  virtual Napi::Value GetResult() = 0;
  Napi::Promise::Deferred deferred_;
};
```

**Put implementation:**
```cpp
class PutWorker : public WaveDBAsyncWorker {
public:
  PutWorker(Napi::Env env, database_t* db,
            path_t* path, identifier_t* value,
            Napi::Function callback)
    : WaveDBAsyncWorker(env, callback),
      db_(db), path_(path), value_(value) {}

  void Execute() override {
    int rc = database_put(db_, path_, value_);
    if (rc != 0) {
      SetError("Failed to put value");
    }
  }

  Napi::Value GetResult() override {
    return Env().Undefined();
  }

private:
  database_t* db_;
  path_t* path_;
  identifier_t* value_;
};
```

**JavaScript API:**
```javascript
class WaveDB {
  async put(key, value, callback) {
    const path = PathFromJS(key, this.delimiter);
    const id = ValueFromJS(value);

    return new Promise((resolve, reject) => {
      const cb = (err, result) => {
        if (callback) callback(err, result);
        if (err) reject(err);
        else resolve(result);
      };

      this._db.put(path, id, cb);
    });
  }
}
```

---

## Sync Operations

**Direct synchronous calls:**
```cpp
Napi::Value PutSync(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 2) {
    Napi::TypeError::New(env, "Expected key and value").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  path_t* path = PathFromJS(env, info[0], delimiter_);
  identifier_t* value = ValueFromJS(env, info[1]);

  int rc = database_put_sync(db_, path, value);

  // Cleanup
  path_destroy(path);
  identifier_destroy(value);

  if (rc != 0) {
    Napi::Error::New(env, "Failed to put value").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  return env.Undefined();
}
```

---

## Batch Operations

**Batch operation format:**
```javascript
await db.batch([
  { type: 'put', key: 'users/alice/name', value: 'Alice' },
  { type: 'put', key: 'users/bob/name', value: 'Bob' },
  { type: 'del', key: 'users/charlie/name' }
]);
```

**C++ implementation:**
```cpp
class BatchWorker : public WaveDBAsyncWorker {
public:
  BatchWorker(Napi::Env env, database_t* db,
              std::vector<BatchOp> ops,
              Napi::Function callback)
    : WaveDBAsyncWorker(env, callback), db_(db), ops_(std::move(ops)) {}

  void Execute() override {
    for (const auto& op : ops_) {
      int rc;
      if (op.type == BatchOpType::PUT) {
        rc = database_put(db_, op.path, op.value);
      } else {
        rc = database_delete(db_, op.path);
      }

      if (rc != 0) {
        SetError("Batch operation failed");
        return;
      }
    }
  }

  Napi::Value GetResult() override {
    return Env().Undefined();
  }

private:
  database_t* db_;
  std::vector<BatchOp> ops_;
};
```

---

## Object Operations

### putObject(obj)

**Flatten nested object to paths:**
```javascript
await db.putObject({
  users: {
    alice: {
      name: 'Alice',
      age: '30',
      roles: ['admin', 'user']
    }
  }
});

// Creates entries:
// 'users/alice/name' → 'Alice'
// 'users/alice/age' → '30'
// 'users/alice/roles/0' → 'admin'
// 'users/alice/roles/1' → 'user'
```

**Implementation:**
```cpp
void FlattenObject(Napi::Env env,
                   Napi::Object obj,
                   std::vector<std::string>& path_parts,
                   std::vector<BatchOp>& ops) {
  Napi::Array keys = obj.GetPropertyNames();

  for (uint32_t i = 0; i < keys.Length(); i++) {
    std::string key = keys.Get(i).As<Napi::String>().Utf8Value();
    Napi::Value value = obj.Get(key);

    path_parts.push_back(key);

    if (value.IsObject() && !value.IsArray() && !value.IsBuffer()) {
      // Nested object - recurse
      FlattenObject(env, value.As<Napi::Object>(), path_parts, ops);
    } else if (value.IsArray()) {
      // Array - use numeric indices
      Napi::Array arr = value.As<Napi::Array>();
      for (uint32_t j = 0; j < arr.Length(); j++) {
        path_parts.push_back(std::to_string(j));
        identifier_t* id = ValueFromJS(env, arr.Get(j));
        ops.push_back({ BatchOpType::PUT, PathFromParts(path_parts), id });
        path_parts.pop_back();
      }
    } else {
      // Leaf value
      identifier_t* id = ValueFromJS(env, value);
      ops.push_back({ BatchOpType::PUT, PathFromParts(path_parts), id });
    }

    path_parts.pop_back();
  }
}
```

### getObject(path)

**Reconstruct object from paths:**
```javascript
const obj = await db.getObject('users/alice');
// Returns:
// {
//   name: 'Alice',
//   age: '30',
//   roles: ['admin', 'user']
// }
```

**Implementation:**
```cpp
Napi::Object ReconstructObject(Napi::Env env,
                                const std::vector<KeyValue>& entries,
                                const std::string& base_path) {
  Napi::Object result = Napi::Object::New(env);

  for (const auto& entry : entries) {
    // Remove base path prefix
    std::string rel_path = entry.key.substr(base_path.length());

    // Split relative path
    std::vector<std::string> parts = Split(rel_path, '/');

    // Navigate/create nested structure
    Napi::Object current = result;
    for (size_t i = 0; i < parts.size() - 1; i++) {
      if (!current.Has(parts[i])) {
        current.Set(parts[i], Napi::Object::New(env));
      }
      current = current.Get(parts[i]).As<Napi::Object>();
    }

    // Set final value
    current.Set(parts.back(), ValueToJS(env, entry.value));
  }

  // Convert to arrays if all numeric keys
  return ConvertArrays(result);
}

Napi::Object ConvertArrays(Napi::Object obj) {
  if (IsArrayLike(obj)) {
    Napi::Array arr = Napi::Array::New(Env());
    Napi::Array keys = obj.GetPropertyNames();

    // Sort numeric keys
    std::vector<int> indices;
    for (uint32_t i = 0; i < keys.Length(); i++) {
      indices.push_back(std::stoi(keys.Get(i).As<Napi::String>().Utf8Value()));
    }
    std::sort(indices.begin(), indices.end());

    // Build array
    for (int idx : indices) {
      arr[idx] = obj.Get(std::to_string(idx));
    }

    return arr;
  }

  // Recurse into nested objects
  Napi::Array keys = obj.GetPropertyNames();
  for (uint32_t i = 0; i < keys.Length(); i++) {
    std::string key = keys.Get(i).As<Napi::String>().Utf8Value();
    Napi::Value value = obj.Get(key);

    if (value.IsObject()) {
      obj.Set(key, ConvertArrays(value.As<Napi::Object>()));
    }
  }

  return obj;
}
```

---

## Streaming Implementation

### Iterator Pattern

**C++ iterator using Node.js streams:**
```cpp
class Iterator : public Napi::ObjectWrap<Iterator> {
public:
  static Napi::Object Init(Napi::Env env);

  Iterator(const Napi::CallbackInfo& info);

private:
  Napi::Value Read(const Napi::CallbackInfo& info);
  void Destroy(const Napi::CallbackInfo& info);

  database_t* db_;
  std::string start_;
  std::string end_;
  bool reverse_;
  bool destroyed_;
  void* scan_handle_;  // Opaque scan handle
};
```

**JavaScript stream wrapper:**
```javascript
const { Readable } = require('stream');

class WaveDBIterator extends Readable {
  constructor(db, options = {}) {
    super({ objectMode: true });
    this._db = db;
    this._start = options.start;
    this._end = options.end;
    this._reverse = options.reverse || false;
    this._keys = options.keys !== false;
    this._values = options.values !== false;
    this._keyAsArray = options.keyAsArray || false;
  }

  _read(size) {
    this._db._readNext(this._start, this._end, this._reverse, (err, entry) => {
      if (err) {
        this.destroy(err);
        return;
      }

      if (!entry) {
        this.push(null);  // End of stream
        return;
      }

      let result = {};
      if (this._keys) {
        result.key = this._keyAsArray ? entry.key.split('/') : entry.key;
      }
      if (this._values) {
        result.value = entry.value;
      }

      this.push(this._keys || this._values ? result : entry.value);
    });
  }

  _destroy(err, callback) {
    this._db._endScan();
    callback(err);
  }
}
```

**Stream options:**
- `start`: Start path (inclusive)
- `end`: End path (exclusive)
- `reverse`: Scan in reverse order (default: false)
- `keys`: Emit `{ key, value }` objects (default: true)
- `values`: Include values in emitted objects (default: true)
- `keyAsArray`: Return keys as arrays instead of strings (default: false)

**Usage:**
```javascript
// Stream all entries
db.createReadStream()
  .on('data', ({ key, value }) => {
    console.log(key, value);
  })
  .on('end', () => {
    console.log('Stream ended');
  });

// Stream specific range
db.createReadStream({
  start: 'users/alice',
  end: 'users/bob'
});

// Stream keys only
db.createReadStream({ keys: true, values: false });

// Early termination
const stream = db.createReadStream();
stream.on('data', ({ key, value }) => {
  if (key === 'users/alice/name') {
    stream.destroy();  // End early
  }
});
```

---

## Error Handling

### Error Classes

**JavaScript error types:**
```javascript
class WaveDBError extends Error {
  constructor(code, message) {
    super(message);
    this.code = code;
    this.name = 'WaveDBError';
  }
}

class NotFoundError extends WaveDBError {
  constructor(key) {
    super('NOT_FOUND', `Key not found: ${key}`);
    this.name = 'NotFoundError';
  }
}

class InvalidPathError extends WaveDBError {
  constructor(path, reason) {
    super('INVALID_PATH', `Invalid path "${path}": ${reason}`);
    this.name = 'InvalidPathError';
  }
}

class IOError extends WaveDBError {
  constructor(operation, details) {
    super('IO_ERROR', `${operation} failed: ${details}`);
    this.name = 'IOError';
  }
}
```

### Error Propagation

**C++ error handling:**
```cpp
void OnError(const std::string& error) override {
  Napi::HandleScope scope(Env());

  // Parse error code from message
  std::string code = "UNKNOWN";
  std::string message = error;

  size_t colonPos = error.find(':');
  if (colonPos != std::string::npos) {
    code = error.substr(0, colonPos);
    message = error.substr(colonPos + 2);
  }

  Napi::Object errorObj = Napi::Error::New(Env(), message).Value();
  errorObj.Set("code", Napi::String::New(Env(), code));

  Callback().Call({ errorObj, env.Undefined() });
  deferred_.Reject(errorObj);
}
```

**Native error mapping:**
- `DATABASE_NOT_FOUND` → `IOError`
- `INVALID_PATH` → `InvalidPathError`
- `KEY_NOT_FOUND` → `NotFoundError`
- `CORRUPTION` → `WaveDBError`

---

## Testing Strategy

### Unit Tests

**Test coverage:**
- Async and sync put/get/delete operations
- Batch operations (put and delete mixed)
- Object operations (putObject, getObject)
- Stream iteration (full scan, range, early termination)
- Error handling (invalid keys, missing keys)
- Edge cases (empty strings, binary data, large values)

**Test structure:**
```javascript
describe('WaveDB', () => {
  let db;

  beforeEach(() => {
    db = new WaveDB('/tmp/test-db-' + Date.now());
  });

  afterEach(() => {
    db.close();
    // Clean up test directory
  });

  describe('put/get', () => {
    it('should put and get a value (async)', async () => {
      await db.put('users/alice/name', 'Alice');
      const value = await db.get('users/alice/name');
      assert.strictEqual(value, 'Alice');
    });

    it('should put and get a value (sync)', () => {
      db.putSync('users/alice/name', 'Alice');
      const value = db.getSync('users/alice/name');
      assert.strictEqual(value, 'Alice');
    });

    it('should handle string and array keys', async () => {
      await db.put('users/alice/name', 'Alice');
      await db.put(['users', 'bob', 'name'], 'Bob');

      assert.strictEqual(await db.get('users/alice/name'), 'Alice');
      assert.strictEqual(await db.get(['users', 'bob', 'name']), 'Bob');
    });

    it('should handle binary values', async () => {
      const buf = Buffer.from([0x01, 0x02, 0x03]);
      await db.put('binary/key', buf);
      const result = await db.get('binary/key');
      assert.deepStrictEqual(result, buf);
    });

    it('should return null for missing keys', async () => {
      const value = await db.get('missing/key');
      assert.strictEqual(value, null);
    });
  });

  describe('batch', () => {
    it('should execute batch operations', async () => {
      await db.batch([
        { type: 'put', key: 'users/alice/name', value: 'Alice' },
        { type: 'put', key: 'users/bob/name', value: 'Bob' },
        { type: 'del', key: 'users/charlie/name' }
      ]);

      assert.strictEqual(await db.get('users/alice/name'), 'Alice');
      assert.strictEqual(await db.get('users/bob/name'), 'Bob');
      assert.strictEqual(await db.get('users/charlie/name'), null);
    });
  });

  describe('objects', () => {
    it('should flatten objects to paths', async () => {
      await db.putObject({
        users: {
          alice: { name: 'Alice', age: '30' },
          bob: { name: 'Bob' }
        }
      });

      assert.strictEqual(await db.get('users/alice/name'), 'Alice');
      assert.strictEqual(await db.get('users/alice/age'), '30');
      assert.strictEqual(await db.get('users/bob/name'), 'Bob');
    });

    it('should reconstruct objects from paths', async () => {
      await db.put('users/alice/name', 'Alice');
      await db.put('users/alice/age', '30');
      await db.put('users/alice/roles/0', 'admin');
      await db.put('users/alice/roles/1', 'user');

      const obj = await db.getObject('users/alice');
      assert.deepEqual(obj, {
        name: 'Alice',
        age: '30',
        roles: ['admin', 'user']
      });
    });

    it('should handle nested arrays', async () => {
      await db.putObject({
        data: {
          matrix: [[1, 2], [3, 4]]
        }
      });

      const obj = await db.getObject('data');
      assert.deepEqual(obj, {
        matrix: [[1, 2], [3, 4]]
      });
    });
  });

  describe('streams', () => {
    beforeEach(async () => {
      await db.put('users/alice/name', 'Alice');
      await db.put('users/bob/name', 'Bob');
      await db.put('users/charlie/name', 'Charlie');
    });

    it('should stream all entries', (done) => {
      const entries = [];
      db.createReadStream()
        .on('data', entry => entries.push(entry))
        .on('end', () => {
          assert.strictEqual(entries.length, 3);
          done();
        });
    });

    it('should stream range', (done) => {
      const entries = [];
      db.createReadStream({
        start: 'users/alice',
        end: 'users/bob'
      })
        .on('data', entry => entries.push(entry))
        .on('end', () => {
          assert.strictEqual(entries.length, 1);
          assert.strictEqual(entries[0].key, 'users/alice/name');
          done();
        });
    });

    it('should end stream early', (done) => {
      let count = 0;
      const stream = db.createReadStream()
        .on('data', () => {
          count++;
          if (count === 1) {
            stream.destroy();
          }
        })
        .on('close', () => {
          assert.strictEqual(count, 1);
          done();
        });
    });

    it('should stream keys only', (done) => {
      const keys = [];
      db.createReadStream({ keys: true, values: false })
        .on('data', key => keys.push(key))
        .on('end', () => {
          assert.deepStrictEqual(keys.sort(), [
            'users/alice/name',
            'users/bob/name',
            'users/charlie/name'
          ]);
          done();
        });
    });
  });
});
```

### Integration Tests

**Concurrent operations:**
```javascript
describe('WaveDB Integration', () => {
  it('should handle concurrent writes', async () => {
    const db = new WaveDB('/tmp/test-concurrent');

    const writes = [];
    for (let i = 0; i < 100; i++) {
      writes.push(db.put(`key${i}`, `value${i}`));
    }

    await Promise.all(writes);

    for (let i = 0; i < 100; i++) {
      const value = await db.get(`key${i}`);
      assert.strictEqual(value, `value${i}`);
    }
  });

  it('should handle large values', async () => {
    const large = 'x'.repeat(1024 * 1024);  // 1MB
    await db.put('large/key', large);
    const value = await db.get('large/key');
    assert.strictEqual(value.length, large.length);
  });
});
```

**Memory leak detection:**
```bash
npm run test:valgrind
```

---

## Build Configuration

### package.json

```json
{
  "name": "wavedb",
  "version": "0.1.0",
  "description": "Node.js bindings for WaveDB",
  "main": "lib/wavedb.js",
  "license": "MIT",
  "dependencies": {
    "node-addon-api": "^7.0.0",
    "node-pre-gyp": "^0.17.0"
  },
  "devDependencies": {
    "mocha": "^10.0.0",
    "node-gyp": "^9.0.0"
  },
  "scripts": {
    "build": "node-pre-gyp build",
    "test": "npm run build && mocha test/*.test.js",
    "test:valgrind": "valgrind --leak-check=full node test/*.test.js"
  },
  "engines": {
    "node": ">=14.0.0"
  }
}
```

### binding.gyp

```python
{
  "targets": [{
    "target_name": "wavedb",
    "sources": [
      "src/binding.cpp",
      "src/database.cc",
      "src/path.cc",
      "src/identifier.cc",
      "src/async_worker.cc",
      "src/put_worker.cc",
      "src/get_worker.cc",
      "src/del_worker.cc",
      "src/batch_worker.cc",
      "src/iterator.cc"
    ],
    "include_dirs": [
      "<!@(node -p \"require('node-addon-api').include\")",
      "../../src"
    ],
    "dependencies": [
      "<!@(node -p \"require('node-addon-api').gyp\")"
    ],
    "cflags!": ["-fno-exceptions"],
    "cflags_cc!": ["-fno-exceptions"],
    "libraries": [
      "-L../../build",
      "-lwavedb"
    ],
    "conditions": [
      ["OS=='linux'", {
        "libraries": ["-lpthread"]
      }],
      ["OS=='mac'", {
        "xcode_settings": {
          "GCC_ENABLE_CPP_EXCEPTIONS": "YES",
          "MACOSX_DEPLOYMENT_TARGET": "10.7"
        }
      }],
      ["OS=='win'", {
        "libraries": ["ws2_32.lib"]
      }]
    ]
  }]
}
```

---

## Usage Documentation

### README.md

```markdown
# WaveDB Node.js Bindings

LevelUP-compatible Node.js bindings for WaveDB.

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
    alice: { name: 'Alice', roles: ['admin'] }
  }
});
// Creates: users/alice/name, users/alice/roles/0

// Reconstruct object from subtree
const user = await db.getObject('users/alice');
// { name: 'Alice', roles: ['admin'] }
```

### Streams

```javascript
db.createReadStream(options)
  .on('data', ({ key, value }) => { })
  .on('end', () => { });

// Options:
// - start: Start path (inclusive)
// - end: End path (exclusive)
// - reverse: Reverse order (default: false)
// - keys: Include keys (default: true)
// - values: Include values (default: true)
// - keyAsArray: Return keys as arrays (default: false)
```

### Keys

Keys can be strings or arrays:
```javascript
await db.put('users/alice/name', 'Alice');
await db.put(['users', 'bob', 'name'], 'Bob');
```

Values can be strings or Buffers:
```javascript
await db.put('binary/key', Buffer.from([0x01, 0x02]));
```

## License

MIT
```

---

## Implementation Checklist

1. **Core Infrastructure**
   - [ ] Module initialization (binding.cpp)
   - [ ] WaveDB class wrapper (database.cc)
   - [ ] Path conversion utilities (path.cc)
   - [ ] Value conversion utilities (identifier.cc)

2. **Async Workers**
   - [ ] Base async worker class (async_worker.cc)
   - [ ] Put worker (put_worker.cc)
   - [ ] Get worker (get_worker.cc)
   - [ ] Delete worker (del_worker.cc)
   - [ ] Batch worker (batch_worker.cc)

3. **Sync Operations**
   - [ ] Put sync
   - [ ] Get sync
   - [ ] Delete sync
   - [ ] Batch sync

4. **Object Operations**
   - [ ] putObject implementation
   - [ ] getObject implementation
   - [ ] Array detection and conversion

5. **Streaming**
   - [ ] Iterator native class (iterator.cc)
   - [ ] Stream wrapper (lib/iterator.js)
   - [ ] Range scanning

6. **Testing**
   - [ ] Unit tests (test/wavedb.test.js)
   - [ ] Integration tests (test/integration.test.js)
   - [ ] Memory leak tests

7. **Documentation**
   - [ ] README.md
   - [ ] API reference
   - [ ] Build instructions

---

## Dependencies

**Native Dependencies:**
- WaveDB C library (`libwavedb.a`)
- libcbor (CBOR serialization)
- xxHash (hashing)

**Node.js Dependencies:**
- node-addon-api ^7.0.0 (C++ wrapper for N-API)
- node-pre-gyp ^0.17.0 (binary distribution)

**Build Dependencies:**
- node-gyp ^9.0.0 (native build tool)
- CMake 3.14+ (WaveDB build system)

---

## Platform Support

**Operating Systems:**
- Linux (tested on Ubuntu 20.04+)
- macOS (tested on 10.14+)
- Windows (tested on Windows 10+)

**Node.js Versions:**
- Node.js 14.x or later
- Uses N-API v6 (stable ABI)

---

## Performance Considerations

**Async vs Sync:**
- Async: Non-blocking, use for production workloads
- Sync: Simpler, use for initialization/migration scripts

**Batch Operations:**
- Batch is more efficient than individual puts for bulk data
- Uses database_write_batch internally

**Stream Buffering:**
- Streams use internal buffer of 100 entries
- Backpressure handled automatically

**Object Operations:**
- putObject uses batch internally
- getObject scans subtree, may be slow for large subtrees

---

## Future Enhancements

**Not in initial implementation:**
- Snapshot support
- Write-ahead log configuration
- Compression options
- Custom comparators
- Secondary indices

**Could be added later:**
- Event emitters for database events
- Transaction support (if added to WaveDB)
- Metrics and monitoring hooks