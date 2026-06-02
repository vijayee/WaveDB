# Raw Binding API — Phase 2: Node.js Binding

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Rewrite the Node.js N-API binding to use the raw C API from Phase 1, reducing FFI calls from ~15 per operation to 1 and eliminating all JS-side allocation overhead.

**Architecture:** Replace `PathFromJS` + `ValueFromJS` + individual C calls with `napi_get_value_string_utf8` into stack buffers + single `database_*_raw` call. Object operations use `database_batch_sync_raw` / `database_scan_sync_raw`.

**Tech Stack:** C++ (N-API / node-addon-api), JavaScript

**Spec:** `docs/superpowers/specs/2026-04-18-raw-binding-api-design.md`
**Depends on:** Phase 1 (C core raw API)

---

## Files

### Modify
- `bindings/nodejs/src/database.cc` — Rewrite all sync/async/object methods to use raw API
- `bindings/nodejs/src/path.cc` — Add `KeyFromJS` stack-buffer helper (or inline in database.cc)
- `bindings/nodejs/src/identifier.cc` — Add `ValueFromJSRaw` zero-copy helper (or inline)
- `bindings/nodejs/src/async_bridge.cc` — No changes needed (get still returns `identifier_t*`)

---

## Task 1: Add Raw API Helper Functions

**Files:**
- Modify: `bindings/nodejs/src/path.cc`
- Modify: `bindings/nodejs/src/path.h`

- [ ] **Step 1: Add KeyFromJS helper to path.cc**

Add a helper that extracts a JS string key into a stack buffer with zero `std::string` allocation:

```cpp
// Extract JS string key into caller-provided buffer.
// Returns false on type error.
bool KeyFromJS(Napi::Env env, Napi::Value key, char* buf, size_t buf_size, size_t* out_len) {
    if (!key.IsString()) {
        Napi::TypeError::New(env, "Key must be a string").ThrowAsJavaScriptException();
        return false;
    }
    // napi_get_value_string_utf8 writes into buf (including null terminator)
    // and returns the byte length (excluding null) in out_len
    napi_status status = napi_get_value_string_utf8(env, key, buf, buf_size, out_len);
    if (status != napi_ok) {
        Napi::Error::New(env, "Failed to extract key string").ThrowAsJavaScriptException();
        return false;
    }
    return true;
}
```

- [ ] **Step 2: Add KeyFromJS declaration to path.h**

```cpp
bool KeyFromJS(Napi::Env env, Napi::Value key, char* buf, size_t buf_size, size_t* out_len);
```

- [ ] **Step 3: Add ValueFromJSRaw helper to identifier.cc**

```cpp
// Extract JS value (string or Buffer) into caller-provided buffers.
// For strings: writes into str_buf, sets val_buf/val_len.
// For Buffers: sets val_buf to the Buffer's data pointer (zero-copy, valid during call).
// Returns false on type error.
bool ValueFromJSRaw(Napi::Env env, Napi::Value value, char* str_buf, size_t str_buf_size,
                    const uint8_t** val_buf, size_t* val_len) {
    if (value.IsString()) {
        size_t len;
        napi_status status = napi_get_value_string_utf8(env, value, str_buf, str_buf_size, &len);
        if (status != napi_ok) {
            Napi::Error::New(env, "Failed to extract value string").ThrowAsJavaScriptException();
            return false;
        }
        *val_buf = reinterpret_cast<const uint8_t*>(str_buf);
        *val_len = len;
        return true;
    } else if (value.IsBuffer()) {
        Napi::Buffer<uint8_t> buffer = value.As<Napi::Buffer<uint8_t>>();
        *val_buf = buffer.Data();
        *val_len = buffer.Length();
        return true;
    } else {
        Napi::TypeError::New(env, "Value must be string or Buffer").ThrowAsJavaScriptException();
        return false;
    }
}
```

- [ ] **Step 4: Add ValueFromJSRaw declaration to identifier.h**

```cpp
bool ValueFromJSRaw(Napi::Env env, Napi::Value value, char* str_buf, size_t str_buf_size,
                    const uint8_t** val_buf, size_t* val_len);
```

- [ ] **Step 5: Build and verify compilation**

Run: `cd bindings/nodejs && npm run build 2>&1 | tail -5`
Expected: Build succeeds (new functions are declared but not yet called).

- [ ] **Step 6: Commit**

```
feat(nodejs): add KeyFromJS and ValueFromJSRaw stack-buffer helpers
```

---

## Task 2: Rewrite Sync Methods to Use Raw API

**Files:**
- Modify: `bindings/nodejs/src/database.cc`

- [ ] **Step 1: Rewrite PutSync**

Replace the existing `WaveDB::PutSync` implementation:

```cpp
Napi::Value WaveDB::PutSync(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    if (!db_) {
        Napi::Error::New(env, "DATABASE_CLOSED: Database is closed").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    if (info.Length() < 2) {
        Napi::TypeError::New(env, "Key and value required").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    // Extract key into stack buffer
    size_t key_len;
    char key_buf[4096];
    if (!KeyFromJS(env, info[0], key_buf, sizeof(key_buf), &key_len)) return env.Undefined();

    // Extract value
    const uint8_t* val_buf;
    size_t val_len;
    char val_str_buf[4096];
    if (!ValueFromJSRaw(env, info[1], val_str_buf, sizeof(val_str_buf), &val_buf, &val_len)) return env.Undefined();

    int rc = database_put_sync_raw(db_, key_buf, key_len, delimiter_, val_buf, val_len);
    if (rc != 0) {
        Napi::Error::New(env, "IO_ERROR: Failed to put value").ThrowAsJavaScriptException();
    }

    return env.Undefined();
}
```

- [ ] **Step 2: Rewrite GetSync**

```cpp
Napi::Value WaveDB::GetSync(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    if (!db_) {
        Napi::Error::New(env, "DATABASE_CLOSED: Database is closed").ThrowAsJavaScriptException();
        return env.Null();
    }

    if (info.Length() < 1) {
        Napi::TypeError::New(env, "Key required").ThrowAsJavaScriptException();
        return env.Null();
    }

    size_t key_len;
    char key_buf[4096];
    if (!KeyFromJS(env, info[0], key_buf, sizeof(key_buf), &key_len)) return env.Null();

    uint8_t* value = NULL;
    size_t value_len = 0;
    int rc = database_get_sync_raw(db_, key_buf, key_len, delimiter_, &value, &value_len);

    if (rc == 0 && value != NULL) {
        // Return as string if printable ASCII, otherwise Buffer
        bool printable = true;
        for (size_t i = 0; i < value_len; i++) {
            if (!isprint(value[i]) && value[i] != '\t' && value[i] != '\n' && value[i] != '\r') {
                printable = false;
                break;
            }
        }
        Napi::Value result;
        if (printable) {
            result = Napi::String::New(env, std::string(reinterpret_cast<const char*>(value), value_len));
        } else {
            result = Napi::Buffer<uint8_t>::Copy(env, value, value_len);
        }
        database_raw_value_free(value);
        return result;
    } else if (rc == -2) {
        return env.Null();
    } else {
        Napi::Error::New(env, "IO_ERROR: Failed to get value").ThrowAsJavaScriptException();
        return env.Null();
    }
}
```

- [ ] **Step 3: Rewrite DeleteSync**

```cpp
Napi::Value WaveDB::DeleteSync(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    if (!db_) {
        Napi::Error::New(env, "DATABASE_CLOSED: Database is closed").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    if (info.Length() < 1) {
        Napi::TypeError::New(env, "Key required").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    size_t key_len;
    char key_buf[4096];
    if (!KeyFromJS(env, info[0], key_buf, sizeof(key_buf), &key_len)) return env.Undefined();

    int rc = database_delete_sync_raw(db_, key_buf, key_len, delimiter_);
    if (rc != 0) {
        Napi::Error::New(env, "IO_ERROR: Failed to delete value").ThrowAsJavaScriptException();
    }

    return env.Undefined();
}
```

- [ ] **Step 4: Build and run Node.js tests**

Run: `cd bindings/nodejs && npm run build && npm test 2>&1 | tail -20`
Expected: All existing tests PASS (behavior unchanged).

- [ ] **Step 5: Commit**

```
feat(nodejs): rewrite sync methods to use raw C API
```

---

## Task 3: Rewrite Async Methods to Use Raw API

**Files:**
- Modify: `bindings/nodejs/src/database.cc`

- [ ] **Step 1: Rewrite Put (async)**

```cpp
Napi::Value WaveDB::Put(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    if (!db_) {
        Napi::Error::New(env, "DATABASE_CLOSED: Database is closed").ThrowAsJavaScriptException();
        return env.Null();
    }

    if (info.Length() < 2) {
        Napi::TypeError::New(env, "Key and value required").ThrowAsJavaScriptException();
        return env.Null();
    }

    size_t key_len;
    char key_buf[4096];
    if (!KeyFromJS(env, info[0], key_buf, sizeof(key_buf), &key_len)) return env.Null();

    const uint8_t* val_buf;
    size_t val_len;
    char val_str_buf[4096];
    if (!ValueFromJSRaw(env, info[1], val_str_buf, sizeof(val_str_buf), &val_buf, &val_len)) return env.Null();

    AsyncOpContext* ctx = CreateOpContext(env, AsyncOpType::Put, info, 2);
    promise_t* promise_c = bridge_.CreatePromise(ctx);
    if (!promise_c) {
        napi_value error_val = Napi::Error::New(env, "Failed to create async promise").Value();
        napi_value promise_val = ctx->promise;
        napi_reject_deferred(env, ctx->deferred, error_val);
        if (ctx->callback_ref) napi_delete_reference(env, ctx->callback_ref);
        delete ctx;
        return Napi::Value(env, promise_val);
    }

    ctx->promise_c = promise_c;

    // Single FFI call — data is copied internally by C
    database_put_raw(db_, key_buf, key_len, delimiter_, val_buf, val_len, promise_c);

    return Napi::Value(env, ctx->promise);
}
```

- [ ] **Step 2: Rewrite Get (async)**

```cpp
Napi::Value WaveDB::Get(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    if (!db_) {
        Napi::Error::New(env, "DATABASE_CLOSED: Database is closed").ThrowAsJavaScriptException();
        return env.Null();
    }

    if (info.Length() < 1) {
        Napi::TypeError::New(env, "Key required").ThrowAsJavaScriptException();
        return env.Null();
    }

    size_t key_len;
    char key_buf[4096];
    if (!KeyFromJS(env, info[0], key_buf, sizeof(key_buf), &key_len)) return env.Null();

    AsyncOpContext* ctx = CreateOpContext(env, AsyncOpType::Get, info, 1);
    promise_t* promise_c = bridge_.CreatePromise(ctx);
    if (!promise_c) {
        napi_value error_val = Napi::Error::New(env, "Failed to create async promise").Value();
        napi_value promise_val = ctx->promise;
        napi_reject_deferred(env, ctx->deferred, error_val);
        if (ctx->callback_ref) napi_delete_reference(env, ctx->callback_ref);
        delete ctx;
        return Napi::Value(env, promise_val);
    }

    ctx->promise_c = promise_c;

    database_get_raw(db_, key_buf, key_len, delimiter_, promise_c);

    return Napi::Value(env, ctx->promise);
}
```

Note: The async get result is still `identifier_t*` via the existing promise/TSFN bridge. The `AsyncBridge::ConvertResult` for `Get` already handles this using `ValueToJS`. No changes needed to async_bridge.cc.

- [ ] **Step 3: Rewrite Delete (async)**

```cpp
Napi::Value WaveDB::Delete(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    if (!db_) {
        Napi::Error::New(env, "DATABASE_CLOSED: Database is closed").ThrowAsJavaScriptException();
        return env.Null();
    }

    if (info.Length() < 1) {
        Napi::TypeError::New(env, "Key required").ThrowAsJavaScriptException();
        return env.Null();
    }

    size_t key_len;
    char key_buf[4096];
    if (!KeyFromJS(env, info[0], key_buf, sizeof(key_buf), &key_len)) return env.Null();

    AsyncOpContext* ctx = CreateOpContext(env, AsyncOpType::Delete, info, 1);
    promise_t* promise_c = bridge_.CreatePromise(ctx);
    if (!promise_c) {
        napi_value error_val = Napi::Error::New(env, "Failed to create async promise").Value();
        napi_value promise_val = ctx->promise;
        napi_reject_deferred(env, ctx->deferred, error_val);
        if (ctx->callback_ref) napi_delete_reference(env, ctx->callback_ref);
        delete ctx;
        return Napi::Value(env, promise_val);
    }

    ctx->promise_c = promise_c;

    database_delete_raw(db_, key_buf, key_len, delimiter_, promise_c);

    return Napi::Value(env, ctx->promise);
}
```

- [ ] **Step 4: Build and run Node.js tests**

Run: `cd bindings/nodejs && npm run build && npm test 2>&1 | tail -20`
Expected: All tests PASS.

- [ ] **Step 5: Commit**

```
feat(nodejs): rewrite async methods to use raw C API
```

---

## Task 4: Rewrite Object Operations to Use Raw Batch/Scan

**Files:**
- Modify: `bindings/nodejs/src/database.cc`

- [ ] **Step 1: Rewrite PutObjectSync**

Replace the current `PutObject`/`FlattenObject` pattern with `database_batch_sync_raw`:

```cpp
Napi::Value WaveDB::PutObjectSync(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    if (!db_) {
        Napi::Error::New(env, "DATABASE_CLOSED: Database is closed").ThrowAsJavaScriptException();
        return env.Null();
    }

    if (info.Length() < 1 || !info[0].IsObject()) {
        Napi::TypeError::New(env, "Object required").ThrowAsJavaScriptException();
        return env.Null();
    }

    // Collect all leaf key-value pairs
    std::vector<std::string> key_strings;
    std::vector<std::string> val_strings;
    std::vector<raw_op_t> ops;

    FlattenObjectRaw(env, info[0].As<Napi::Object>(), delimiter_, "", ops, key_strings, val_strings);
    if (ops.empty()) return env.Undefined();

    int rc = database_batch_sync_raw(db_, delimiter_, ops.data(), ops.size());
    if (rc != 0) {
        Napi::Error::New(env, "IO_ERROR: Batch put failed").ThrowAsJavaScriptException();
    }

    return env.Undefined();
}
```

Where `FlattenObjectRaw` recursively walks the JS object and builds `raw_op_t` entries with pointers into the `key_strings`/`val_strings` vectors (which must stay alive during the call).

- [ ] **Step 2: Rewrite GetObjectSync**

```cpp
Napi::Value WaveDB::GetObjectSync(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    if (!db_) {
        Napi::Error::New(env, "DATABASE_CLOSED: Database is closed").ThrowAsJavaScriptException();
        return env.Null();
    }

    if (info.Length() < 1) {
        Napi::TypeError::New(env, "Path required").ThrowAsJavaScriptException();
        return env.Null();
    }

    size_t prefix_len;
    char prefix_buf[4096];
    if (!KeyFromJS(env, info[0], prefix_buf, sizeof(prefix_buf), &prefix_len)) return env.Null();

    raw_result_t* results = NULL;
    size_t count = 0;
    int rc = database_scan_sync_raw(db_, prefix_buf, prefix_len, delimiter_, &results, &count);
    if (rc != 0) {
        Napi::Error::New(env, "IO_ERROR: Scan failed").ThrowAsJavaScriptException();
        return env.Null();
    }

    // Reconstruct JS object from flat results
    Napi::Object result_obj = Napi::Object::New(env);
    for (size_t i = 0; i < count; i++) {
        // Parse the key string by delimiter and build nested object
        std::string key(results[i].key, results[i].key_len);
        Napi::Value val;
        bool printable = true;
        for (size_t j = 0; j < results[i].value_len; j++) {
            if (!isprint(results[i].value[j]) && results[i].value[j] != '\t' &&
                results[i].value[j] != '\n' && results[i].value[j] != '\r') {
                printable = false;
                break;
            }
        }
        if (printable) {
            val = Napi::String::New(env, std::string(reinterpret_cast<const char*>(results[i].value), results[i].value_len));
        } else {
            val = Napi::Buffer<uint8_t>::Copy(env, results[i].value, results[i].value_len);
        }

        // Walk/create nested object path and set the value at the leaf
        Napi::Object current = result_obj;
        size_t start = 0;
        for (size_t j = 0; j <= key.size(); j++) {
            if (j == key.size() || key[j] == delimiter_) {
                std::string segment = key.substr(start, j - start);
                if (j == key.size()) {
                    // Leaf — set the value
                    current.Set(segment, val);
                } else {
                    // Intermediate — create or navigate
                    if (!current.Has(segment) || !current.Get(segment).IsObject()) {
                        current.Set(segment, Napi::Object::New(env));
                    }
                    current = current.Get(segment).As<Napi::Object>();
                }
                start = j + 1;
            }
        }
    }

    database_raw_results_free(results, count);

    // Convert numeric-keyed objects to arrays
    return ConvertArrays(env, result_obj);
}
```

- [ ] **Step 3: Build and run Node.js tests**

Run: `cd bindings/nodejs && npm run build && npm test 2>&1 | tail -20`
Expected: All tests PASS.

- [ ] **Step 4: Run benchmark comparison**

Run: `cd bindings/nodejs && node benchmark.js 2>&1 | head -30`
Expected: Throughput improvement visible in sync operations.

- [ ] **Step 5: Commit**

```
feat(nodejs): rewrite object operations to use raw batch/scan API
```

---

## Task 5: Rewrite Batch Methods and Callback Variants

**Files:**
- Modify: `bindings/nodejs/src/database.cc`

- [ ] **Step 1: Rewrite BatchSync**

Replace the current iterative `database_put_sync` loop with `database_batch_sync_raw`:

```cpp
Napi::Value WaveDB::BatchSync(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    if (!db_) {
        Napi::Error::New(env, "DATABASE_CLOSED: Database is closed").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    if (info.Length() < 1 || !info[0].IsArray()) {
        Napi::TypeError::New(env, "Array of operations required").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    Napi::Array ops = info[0].As<Napi::Array>();
    std::vector<raw_op_t> raw_ops(ops.Length());
    std::vector<std::string> key_bufs(ops.Length());
    std::vector<std::string> val_bufs(ops.Length());

    for (uint32_t i = 0; i < ops.Length(); i++) {
        Napi::Object op = ops.Get(i).As<Napi::Object>();
        if (!op.Has("type") || !op.Has("key")) {
            Napi::TypeError::New(env, "Operation must have 'type' and 'key'").ThrowAsJavaScriptException();
            return env.Undefined();
        }

        std::string type = op.Get("type").As<Napi::String>().Utf8Value();
        key_bufs[i] = op.Get("key").As<Napi::String>().Utf8Value();
        raw_ops[i].key = key_bufs[i].c_str();
        raw_ops[i].key_len = key_bufs[i].size();

        if (type == "put") {
            if (!op.Has("value")) {
                Napi::TypeError::New(env, "Put operation must have 'value'").ThrowAsJavaScriptException();
                return env.Undefined();
            }
            // Handle string or buffer values
            Napi::Value v = op.Get("value");
            if (v.IsBuffer()) {
                Napi::Buffer<uint8_t> buf = v.As<Napi::Buffer<uint8_t>>();
                val_bufs[i] = std::string(reinterpret_cast<const char*>(buf.Data()), buf.Length());
            } else {
                val_bufs[i] = v.As<Napi::String>().Utf8Value();
            }
            raw_ops[i].value = reinterpret_cast<const uint8_t*>(val_bufs[i].c_str());
            raw_ops[i].value_len = val_bufs[i].size();
            raw_ops[i].type = 0;
        } else if (type == "del") {
            raw_ops[i].value = NULL;
            raw_ops[i].value_len = 0;
            raw_ops[i].type = 1;
        } else {
            Napi::TypeError::New(env, "Operation type must be 'put' or 'del'").ThrowAsJavaScriptException();
            return env.Undefined();
        }
    }

    int rc = database_batch_sync_raw(db_, delimiter_, raw_ops.data(), raw_ops.size());
    if (rc != 0) {
        Napi::Error::New(env, "IO_ERROR: Batch operation failed").ThrowAsJavaScriptException();
    }

    return env.Undefined();
}
```

- [ ] **Step 2: Rewrite Batch (async)**

Similarly use `database_batch_raw` for the async batch. The conversion loop is the same, but dispatch is via the C promise/TSFN bridge.

- [ ] **Step 3: Rewrite callback variants (PutCb, GetCb, DeleteCb, BatchCb)**

These follow the same pattern as the promise variants but pass a callback reference. The key/value extraction is identical to the async variants; only the `CreateOpContext` call differs.

- [ ] **Step 4: Build and run Node.js tests**

Run: `cd bindings/nodejs && npm run build && npm test 2>&1 | tail -20`
Expected: All tests PASS.

- [ ] **Step 5: Commit**

```
feat(nodejs): rewrite batch and callback methods to use raw C API
```

---

## Task 6: Validation and Benchmark

**Files:**
- No new files

- [ ] **Step 1: Run full Node.js test suite**

Run: `cd bindings/nodejs && npm test 2>&1`
Expected: All tests PASS.

- [ ] **Step 2: Run Node.js benchmark**

Run: `cd bindings/nodejs && node benchmark.js 2>&1`
Expected: Measurable throughput improvement for sync and async operations.

- [ ] **Step 3: Run valgrind on Node.js tests**

Run: `cd bindings/nodejs && npm run test:valgrind 2>&1 | grep -E "(ERROR SUMMARY|definitely lost|indirectly lost)"`
Expected: No new leaks introduced by raw API usage.

- [ ] **Step 4: Commit any fixes if needed**

If issues found, fix and commit.

---

## Summary

Phase 2 delivers the complete Node.js binding rewrite:

| Method | Before | After |
|--------|--------|-------|
| putSync | PathFromJS + ValueFromJS + database_put_sync | KeyFromJS + ValueFromJSRaw + database_put_sync_raw |
| getSync | PathFromJS + database_get_sync + ValueToJS | KeyFromJS + database_get_sync_raw + inline conversion |
| delSync | PathFromJS + database_delete_sync | KeyFromJS + database_delete_sync_raw |
| put (async) | PathFromJS + ValueFromJS + database_put | KeyFromJS + ValueFromJSRaw + database_put_raw |
| get (async) | PathFromJS + database_get | KeyFromJS + database_get_raw |
| del (async) | PathFromJS + database_delete | KeyFromJS + database_delete_raw |
| putObjectSync | FlattenObject + N×database_put_sync | FlattenObjectRaw + database_batch_sync_raw |
| getObjectSync | scan_start + N×scan_next + ReconstructObject | database_scan_sync_raw + inline reconstruction |
| batchSync | N×database_put_sync loop | database_batch_sync_raw |

Phase 3 (Dart binding) will be written as a separate plan file.