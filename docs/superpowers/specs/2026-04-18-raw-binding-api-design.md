# Raw Binding API Design Spec

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add C API functions that accept raw byte buffers (key strings, value bytes) instead of pre-constructed `path_t*`/`identifier_t*`, eliminating the per-operation allocation cascade in the Node.js and Dart bindings. Target: match C API performance within 1.5-2x for sync operations, and maximize async throughput by reducing FFI-layer marshaling overhead.

**Problem:** Both bindings construct `path_t*` and `identifier_t*` through a multi-step allocation chain: JS/Dart string → `calloc` → `buffer_create_from_pointer_copy` → `identifier_create` → `buffer_destroy` → `path_append` → `identifier_destroy`. This results in 8-15 FFI calls and 20-25 heap operations per operation. The C baseline pays none of this cost.

**Architecture:** New `_raw` C functions accept `const char*/size_t` key-value pairs and a `char` delimiter, constructing path/identifier hierarchies internally using the memory pool. Both sync and async variants. New batch and scan raw functions accept/return flat C structs for bulk operations.

**Tech Stack:** C (core), C++ (Node.js binding), Dart FFI (Dart binding)

---

## API Design

### New C Structs

```c
// Flat batch operation (input to batch functions)
typedef struct {
    const char* key;         // path string, e.g. "users/alice/name"
    size_t key_len;          // length of key (no null terminator needed)
    const uint8_t* value;    // value bytes (NULL for delete operations)
    size_t value_len;        // length of value
    int type;                // 0 = put, 1 = delete
} raw_op_t;

// Flat scan result (output from scan functions, caller must free)
typedef struct {
    char* key;               // path string, owned by result
    size_t key_len;          // length of key
    uint8_t* value;          // value bytes, owned by result
    size_t value_len;        // length of value
} raw_result_t;
```

### Single-Key Sync Operations

```c
/**
 * Synchronously insert a value using raw key/value bytes.
 * Key is parsed by delimiter into path segments internally.
 *
 * @param db        Database to modify
 * @param key       Key string (e.g. "users/alice/name")
 * @param key_len   Length of key string
 * @param delimiter Path segment delimiter (e.g. '/')
 * @param value     Value bytes
 * @param value_len Length of value
 * @return 0 on success, -1 on error
 */
int database_put_sync_raw(database_t* db,
    const char* key, size_t key_len, char delimiter,
    const uint8_t* value, size_t value_len);

/**
 * Synchronously get a value using raw key bytes.
 * Caller must free *value_out via database_raw_value_free().
 *
 * @param db        Database to query
 * @param key       Key string
 * @param key_len   Length of key string
 * @param delimiter Path segment delimiter
 * @param value_out Output: pointer to value bytes (caller frees)
 * @param value_len_out Output: length of value
 * @return 0 on success, -1 on error, -2 on not found
 */
int database_get_sync_raw(database_t* db,
    const char* key, size_t key_len, char delimiter,
    uint8_t** value_out, size_t* value_len_out);

/**
 * Synchronously delete a value using raw key bytes.
 *
 * @param db        Database to modify
 * @param key       Key string
 * @param key_len   Length of key string
 * @param delimiter Path segment delimiter
 * @return 0 on success, -1 on error
 */
int database_delete_sync_raw(database_t* db,
    const char* key, size_t key_len, char delimiter);

/** Free value buffer returned by database_get_sync_raw */
void database_raw_value_free(uint8_t* value);
```

### Single-Key Async Operations

```c
/**
 * Asynchronously insert a value using raw key/value bytes.
 * Data is copied internally before dispatch to worker pool.
 * Key is parsed by delimiter into path segments on the worker thread.
 *
 * @param db        Database to modify
 * @param key       Key string (copied internally)
 * @param key_len   Length of key string
 * @param delimiter Path segment delimiter
 * @param value     Value bytes (copied internally)
 * @param value_len Length of value
 * @param promise   C promise for async resolution
 * @return 0 on dispatch success, -1 on error
 */
int database_put_raw(database_t* db,
    const char* key, size_t key_len, char delimiter,
    const uint8_t* value, size_t value_len,
    promise_t* promise);

/**
 * Asynchronously get a value using raw key bytes.
 * On resolve, payload is identifier_t* (same as existing database_get).
 * Binding layer uses identifier_get_data or ValueToJS to convert result.
 *
 * @param db        Database to query
 * @param key       Key string (copied internally)
 * @param key_len   Length of key string
 * @param delimiter Path segment delimiter
 * @param promise   C promise for async resolution
 * @return 0 on dispatch success, -1 on error
 */
int database_get_raw(database_t* db,
    const char* key, size_t key_len, char delimiter,
    promise_t* promise);

/**
 * Asynchronously delete a value using raw key bytes.
 *
 * @param db        Database to modify
 * @param key       Key string (copied internally)
 * @param key_len   Length of key string
 * @param delimiter Path segment delimiter
 * @param promise   C promise for async resolution
 * @return 0 on dispatch success, -1 on error
 */
int database_delete_raw(database_t* db,
    const char* key, size_t key_len, char delimiter,
    promise_t* promise);
```

**Design note on async raw data ownership:** Async raw functions copy key and value data into C-allocated buffers before dispatching to the worker pool. This is necessary because the caller's buffers (e.g., Dart's `calloc` memory or Node.js stack buffers) may go out of scope before the worker thread picks up the work. The copies happen in C using the memory pool, which is fast. The worker thread then constructs path/identifier from the copied data and frees the copies afterward.

For `database_get_raw`, the result is still an `identifier_t*` delivered via the existing promise callback mechanism. This allows the binding layer to reuse the existing `ValueToJS` / `IdentifierConverter.fromNative` conversion. A future optimization could return raw bytes, but that would require a different callback signature and break the existing async bridge pattern.

### Batch Operations

```c
/**
 * Synchronously execute a batch of operations using raw key/value bytes.
 * All operations execute within a single MVCC transaction.
 *
 * @param db        Database to modify
 * @param delimiter Path segment delimiter
 * @param ops       Array of raw operations
 * @param count     Number of operations
 * @return 0 on success, -1 on error
 */
int database_batch_sync_raw(database_t* db, char delimiter,
    const raw_op_t* ops, size_t count);

/**
 * Asynchronously execute a batch of operations using raw key/value bytes.
 * All operations execute within a single MVCC transaction on the worker thread.
 * Data from ops array is copied internally before dispatch.
 *
 * @param db        Database to modify
 * @param delimiter Path segment delimiter
 * @param ops       Array of raw operations (copied internally)
 * @param count     Number of operations
 * @param promise   C promise for async resolution
 * @return 0 on dispatch success, -1 on error
 */
int database_batch_raw(database_t* db, char delimiter,
    const raw_op_t* ops, size_t count,
    promise_t* promise);
```

### Scan/Prefix Operations

```c
/**
 * Synchronously scan all entries under a prefix.
 * Returns results as a flat array of key/value pairs.
 * Caller must free results via database_raw_results_free().
 *
 * @param db        Database to query
 * @param prefix    Prefix key string (e.g. "users/alice")
 * @param prefix_len Length of prefix string
 * @param delimiter Path segment delimiter
 * @param results   Output: array of results (caller frees)
 * @param count     Output: number of results
 * @return 0 on success, -1 on error
 */
int database_scan_sync_raw(database_t* db,
    const char* prefix, size_t prefix_len, char delimiter,
    raw_result_t** results, size_t* count);

/** Free result array returned by database_scan_sync_raw */
void database_raw_results_free(raw_result_t* results, size_t count);
```

**Design note on scan results:** Each `raw_result_t` contains owned copies of the key and value data. The key is the full path string (e.g. "users/alice/name"), joined by the delimiter. The value is a copy of the stored bytes. The binding layer strips the prefix to reconstruct nested objects. Returning the full path is simpler and more flexible than stripping in C.

### Supporting Internal Functions

```c
// --- identifier.h ---

/**
 * Create identifier directly from raw bytes, skipping buffer_t.
 * Eliminates 2 transient allocations (buffer_t struct + buffer data).
 */
identifier_t* identifier_create_from_raw(
    const uint8_t* data, size_t len, size_t chunk_size);

/**
 * Get raw byte data from identifier without creating buffer_t.
 * Returns pointer into identifier's own chunk data.
 * Valid until identifier is destroyed.
 * For identifiers with multiple chunks, data is assembled into
 * a single contiguous buffer (caller must free via free()).
 */
const uint8_t* identifier_get_data(const identifier_t* id, size_t* len_out);

// --- path.h ---

/**
 * Create path by parsing a raw key string with delimiter.
 * Uses identifier_create_from_raw internally.
 */
path_t* path_create_from_raw(
    const char* key, size_t key_len, char delimiter, size_t chunk_size);
```

**`identifier_get_data` detail:** For single-chunk identifiers (very common for short values), returns a pointer directly into the chunk's data array with zero allocation. For multi-chunk identifiers, allocates a contiguous buffer and copies chunk data into it (same as `identifier_to_buffer` but returns raw pointer instead of `buffer_t*`). The caller must `free()` the returned pointer if `*len_out > chunk_size` (multi-chunk case). To simplify the API, we could instead always return a `uint8_t*` that the caller must free, with a `bool* needs_free` output parameter. But that adds complexity — better to just always copy for consistency, since the multi-chunk case is the only one that allocates, and the binding layer was going to copy the data into JS/Dart anyway.

**Revised:**
```c
/**
 * Get raw byte data from identifier as contiguous buffer.
 * Caller MUST free the returned pointer with free().
 * Always returns a new allocation (even for single-chunk identifiers)
 * for consistent ownership semantics.
 */
uint8_t* identifier_get_data_copy(const identifier_t* id, size_t* len_out);
```

---

## Implementation Design

### Step 1: `identifier_create_from_raw`

Replace the `buffer_t` intermediate in `identifier_create`:

**Current path:**
```
buffer_create_from_pointer_copy(data, len)  → 2 allocs (struct + data copy)
identifier_create(buf, chunk_size)          → allocs identifier_t + vec + chunks, copies buf data into chunks
buffer_destroy(buf)                         → 2 frees
```

**New path:**
```
identifier_create_from_raw(data, len, chunk_size)  → allocs identifier_t + vec + chunks, copies data directly
```

Savings: 2 allocs + 2 frees + 1 memcpy per identifier creation.

Implementation: Extract the core logic from `identifier_create` that iterates data in `chunk_size` increments and creates chunks, but read directly from the `data` pointer instead of from a `buffer_t`.

### Step 2: `identifier_get_data_copy`

Single allocation + memcpy for result values, replacing:

**Current path (binding layer):**
```
identifier_to_buffer(id)       → allocs buffer_t struct + buffer data, copies chunk data into contiguous buffer
read buffer_t.data / .size     → FFI struct field access
buffer_destroy(buf)            → 2 frees
```

**New path:**
```
identifier_get_data_copy(id, &len)  → single malloc + memcpy, returns raw pointer
```

Savings: 1 fewer allocation (no buffer_t struct), simpler FFI access (pointer + length instead of struct field reads).

### Step 3: `path_create_from_raw`

Parse delimiter-separated key in a single pass:

```c
path_t* path_create_from_raw(const char* key, size_t key_len,
                              char delimiter, size_t chunk_size) {
    path_t* path = path_create();
    if (!path) return NULL;

    if (chunk_size == 0) chunk_size = DEFAULT_CHUNK_SIZE;

    size_t start = 0;
    for (size_t i = 0; i <= key_len; i++) {
        if (i == key_len || key[i] == delimiter) {
            size_t seg_len = i - start;
            if (seg_len == 0) { start = i + 1; continue; }

            identifier_t* id = identifier_create_from_raw(
                (const uint8_t*)(key + start), seg_len, chunk_size);
            if (!id) { path_destroy(path); return NULL; }

            path_append(path, id);
            identifier_destroy(id);  // path holds reference
            start = i + 1;
        }
    }

    return path;
}
```

### Step 4: Sync Raw Functions

Thin wrappers that construct path/value and delegate to existing sync functions:

```c
int database_put_sync_raw(database_t* db,
    const char* key, size_t key_len, char delimiter,
    const uint8_t* value, size_t value_len) {
    if (!db || !key || !value) return -1;

    path_t* path = path_create_from_raw(key, key_len, delimiter, db->trie->chunk_size);
    if (!path) return -1;

    identifier_t* id = identifier_create_from_raw(value, value_len, db->trie->chunk_size);
    if (!id) { path_destroy(path); return -1; }

    return database_put_sync(db, path, id);  // takes ownership
}

int database_get_sync_raw(database_t* db,
    const char* key, size_t key_len, char delimiter,
    uint8_t** value_out, size_t* value_len_out) {
    if (!db || !key || !value_out || !value_len_out) return -1;

    path_t* path = path_create_from_raw(key, key_len, delimiter, db->trie->chunk_size);
    if (!path) return -1;

    identifier_t* result = NULL;
    int rc = database_get_sync(db, path, &result);  // consumes path

    if (rc == 0 && result) {
        *value_out = identifier_get_data_copy(result, value_len_out);
        identifier_destroy(result);
        return 0;
    }
    *value_out = NULL;
    *value_len_out = 0;
    return rc;
}

int database_delete_sync_raw(database_t* db,
    const char* key, size_t key_len, char delimiter) {
    if (!db || !key) return -1;

    path_t* path = path_create_from_raw(key, key_len, delimiter, db->trie->chunk_size);
    if (!path) return -1;

    return database_delete_sync(db, path);  // takes ownership
}
```

### Step 5: Async Raw Functions

Async variants copy data before dispatching to the worker pool:

```c
// Internal context for async raw operations
typedef struct {
    database_t* db;
    char* key_buf;           // copied key
    size_t key_len;
    char delimiter;
    uint8_t* value_buf;      // copied value (NULL for get/delete)
    size_t value_len;
    promise_t* promise;
    int op_type;             // 0=put, 1=get, 2=delete
} raw_async_ctx_t;

static void _database_put_raw_worker(work_t* work) {
    raw_async_ctx_t* ctx = work_data(work);
    path_t* path = path_create_from_raw(ctx->key_buf, ctx->key_len,
                                         ctx->delimiter,
                                         ctx->db->trie->chunk_size);
    if (!path) {
        async_error_t* err = error_create("Failed to create path");
        promise_reject(ctx->promise, err);
        return;
    }

    identifier_t* value = identifier_create_from_raw(
        ctx->value_buf, ctx->value_len, ctx->db->trie->chunk_size);
    if (!value) {
        path_destroy(path);
        async_error_t* err = error_create("Failed to create value");
        promise_reject(ctx->promise, err);
        return;
    }

    // Delegate to existing internal put logic
    _database_put(ctx->db, path, value, ctx->promise);
}

int database_put_raw(database_t* db,
    const char* key, size_t key_len, char delimiter,
    const uint8_t* value, size_t value_len,
    promise_t* promise) {
    if (!db || !key || !value || !promise) return -1;

    // Copy key and value for async ownership
    raw_async_ctx_t* ctx = memory_pool_alloc(sizeof(raw_async_ctx_t));
    if (!ctx) return -1;

    ctx->db = db;
    ctx->key_len = key_len;
    ctx->key_buf = memory_pool_alloc(key_len);
    if (!ctx->key_buf) { memory_pool_free(ctx); return -1; }
    memcpy(ctx->key_buf, key, key_len);

    ctx->delimiter = delimiter;
    ctx->value_len = value_len;
    ctx->value_buf = memory_pool_alloc(value_len);
    if (!ctx->value_buf) { memory_pool_free(ctx->key_buf); memory_pool_free(ctx); return -1; }
    memcpy(ctx->value_buf, value, value_len);

    ctx->promise = promise;
    ctx->op_type = 0;

    work_t* work = work_create(_database_put_raw_worker,
                                _database_raw_abort,
                                ctx);
    if (!work) {
        memory_pool_free(ctx->value_buf);
        memory_pool_free(ctx->key_buf);
        memory_pool_free(ctx);
        return -1;
    }

    refcounter_yield(work);
    work_pool_enqueue(db->pool, work);
    return 0;
}
```

`database_get_raw` and `database_delete_raw` follow the same pattern, with get having no value copy and delete having neither value nor result.

**Worker thread path construction:** The worker thread constructs path/identifier from the copied raw data using `path_create_from_raw` and `identifier_create_from_raw`, then calls the existing `_database_put`/`_database_get`/`_database_delete` internal functions. The result for get is still an `identifier_t*` delivered via the promise callback, matching the existing async bridge pattern.

### Step 6: Batch Raw Functions

```c
int database_batch_sync_raw(database_t* db, char delimiter,
    const raw_op_t* ops, size_t count) {
    if (!db || !ops || count == 0) return -1;

    batch_t* batch = batch_create(count);
    if (!batch) return -1;

    for (size_t i = 0; i < count; i++) {
        path_t* path = path_create_from_raw(ops[i].key, ops[i].key_len,
                                             delimiter,
                                             db->trie->chunk_size);
        if (!path) { batch_destroy(batch); return -1; }

        if (ops[i].type == 0) {  // put
            identifier_t* value = identifier_create_from_raw(
                ops[i].value, ops[i].value_len,
                db->trie->chunk_size);
            if (!value) { path_destroy(path); batch_destroy(batch); return -1; }

            int rc = batch_add_put(batch, path, value);
            if (rc != 0) { path_destroy(path); identifier_destroy(value); batch_destroy(batch); return -1; }
        } else {  // delete
            int rc = batch_add_delete(batch, path);
            if (rc != 0) { path_destroy(path); batch_destroy(batch); return -1; }
        }
    }

    // Execute the batch synchronously
    return database_write_batch_sync(db, batch);  // takes ownership
}
```

**Note:** `database_write_batch_sync` already exists in the codebase (`database.h:263`). The batch raw sync function delegates to it directly.

For async batch raw:
```c
int database_batch_raw(database_t* db, char delimiter,
    const raw_op_t* ops, size_t count,
    promise_t* promise) {
    if (!db || !ops || count == 0 || !promise) return -1;

    // Convert raw ops to a batch_t, then dispatch via existing async path
    batch_t* batch = batch_create(count);
    if (!batch) return -1;

    for (size_t i = 0; i < count; i++) {
        path_t* path = path_create_from_raw(ops[i].key, ops[i].key_len,
                                             delimiter,
                                             db->trie->chunk_size);
        if (!path) { batch_destroy(batch); return -1; }

        if (ops[i].type == 0) {  // put
            identifier_t* value = identifier_create_from_raw(
                ops[i].value, ops[i].value_len,
                db->trie->chunk_size);
            if (!value) { path_destroy(path); batch_destroy(batch); return -1; }

            int rc = batch_add_put(batch, path, value);
            if (rc != 0) { path_destroy(path); identifier_destroy(value); batch_destroy(batch); return -1; }
        } else {  // delete
            int rc = batch_add_delete(batch, path);
            if (rc != 0) { path_destroy(path); batch_destroy(batch); return -1; }
        }
    }

    // Delegate to existing async batch dispatch
    return database_write_batch(db, batch, promise);
}
```

### Step 7: Scan Raw Function

```c
int database_scan_sync_raw(database_t* db,
    const char* prefix, size_t prefix_len, char delimiter,
    raw_result_t** results, size_t* count) {
    if (!db || !results || !count) return -1;

    // Build start path from prefix
    path_t* start_path = path_create_from_raw(prefix, prefix_len, delimiter,
                                               db->trie->chunk_size);
    if (!start_path) return -1;

    database_iterator_t* iter = database_scan_start(db, start_path, NULL);
    path_destroy(start_path);
    if (!iter) { *results = NULL; *count = 0; return 0; }

    // Collect results into dynamic array
    size_t capacity = 64;
    size_t n = 0;
    raw_result_t* out = malloc(capacity * sizeof(raw_result_t));
    if (!out) { database_scan_end(iter); return -1; }

    while (true) {
        path_t* out_path = NULL;
        identifier_t* out_value = NULL;
        int rc = database_scan_next(iter, &out_path, &out_value);
        if (rc != 0) break;

        if (n >= capacity) {
            capacity *= 2;
            raw_result_t* new_out = realloc(out, capacity * sizeof(raw_result_t));
            if (!new_out) {
                // Free collected results on OOM
                for (size_t j = 0; j < n; j++) {
                    free(out[j].key);
                    free(out[j].value);
                }
                free(out);
                path_destroy(out_path);
                identifier_destroy(out_value);
                database_scan_end(iter);
                return -1;
            }
            out = new_out;
        }

        // Convert path to key string with delimiters
        size_t key_total = 0;
        for (size_t i = 0; i < (size_t)out_path->identifiers.length; i++) {
            identifier_t* id = out_path->identifiers.data[i];
            key_total += id->length;
            if (i > 0) key_total++;  // delimiter
        }
        out[n].key = malloc(key_total + 1);
        size_t pos = 0;
        for (size_t i = 0; i < (size_t)out_path->identifiers.length; i++) {
            identifier_t* id = out_path->identifiers.data[i];
            size_t id_len;
            uint8_t* id_data = identifier_get_data_copy(id, &id_len);
            if (i > 0) out[n].key[pos++] = delimiter;
            memcpy(out[n].key + pos, id_data, id_len);
            pos += id_len;
            free(id_data);
        }
        out[n].key[pos] = '\0';
        out[n].key_len = pos;

        // Copy value
        size_t vlen;
        out[n].value = identifier_get_data_copy(out_value, &vlen);
        out[n].value_len = vlen;

        path_destroy(out_path);
        identifier_destroy(out_value);
        n++;
    }

    database_scan_end(iter);
    *results = out;
    *count = n;
    return 0;
}

void database_raw_results_free(raw_result_t* results, size_t count) {
    if (!results) return;
    for (size_t i = 0; i < count; i++) {
        free(results[i].key);
        free(results[i].value);
    }
    free(results);
}
```

---

## Binding Layer Changes

### Node.js Binding

**New methods in database.cc:**

Replace `PathFromJS` + `ValueFromJS` + `database_put_sync` with single raw FFI call:

```cpp
Napi::Value WaveDB::PutSync(const Napi::CallbackInfo& info) {
    // Get key string into stack buffer (zero copy from V8)
    size_t key_len = info[0].As<Napi::String>().Utf8Length();
    char* key_buf = static_cast<char*>(alloca(key_len + 1));
    napi_get_value_string_utf8(env, info[0], key_buf, key_len + 1, &key_len);

    // Get value bytes
    size_t val_len;
    uint8_t* val_buf;
    if (info[1].IsBuffer()) {
        val_buf = info[1].As<Napi::Buffer<uint8_t>>().Data();
        val_len = info[1].As<Napi::Buffer<uint8_t>>().Length();
    } else {
        val_len = info[1].As<Napi::String>().Utf8Length();
        val_buf = static_cast<uint8_t*>(alloca(val_len + 1));
        napi_get_value_string_utf8(env, info[1], reinterpret_cast<char*>(val_buf),
                                    val_len + 1, &val_len);
    }

    int rc = database_put_sync_raw(db_, key_buf, key_len, delimiter_, val_buf, val_len);
    if (rc != 0) Napi::Error::New(env, "IO_ERROR").ThrowAsJavaScriptException();
    return env.Undefined();
}
```

**Async raw methods** similarly extract strings into stack buffers and call `database_put_raw`:

```cpp
Napi::Value WaveDB::Put(const Napi::CallbackInfo& info) {
    // ... same key/value extraction as PutSync ...
    AsyncOpContext* ctx = CreateOpContext(env, AsyncOpType::Put, info, 2);
    promise_t* promise_c = bridge_.CreatePromise(ctx);
    ctx->promise_c = promise_c;

    // Single FFI call — data is copied internally by C
    database_put_raw(db_, key_buf, key_len, delimiter_, val_buf, val_len, promise_c);

    return Napi::Value(env, ctx->promise);
}
```

**Object operations:**

`PutObjectSync` collects all leaf key-value pairs into a `raw_op_t[]`, then calls `database_batch_sync_raw`.

`GetObjectSync` calls `database_scan_sync_raw`, then iterates the flat `raw_result_t[]` array to reconstruct the JS object — no per-entry FFI calls.

### Dart Binding

**New FFI bindings in wavedb_bindings.dart:**

```dart
// Sync raw functions
static int databasePutSyncRaw(Pointer<database_t> db,
    Pointer<Utf8> key, int keyLen, int delimiter,
    Pointer<Uint8> value, int valueLen) { ... }

static int databaseGetSyncRaw(Pointer<database_t> db,
    Pointer<Utf8> key, int keyLen, int delimiter,
    Pointer<Pointer<Uint8>> valueOut, Pointer<Size> valueLenOut) { ... }

static int databaseDeleteSyncRaw(Pointer<database_t> db,
    Pointer<Utf8> key, int keyLen, int delimiter) { ... }

static void databaseRawValueFree(Pointer<Uint8> value) { ... }

// Async raw functions
static int databasePutRaw(Pointer<database_t> db,
    Pointer<Utf8> key, int keyLen, int delimiter,
    Pointer<Uint8> value, int valueLen,
    Pointer<promise_t> promise) { ... }

// Batch and scan
static int databaseBatchSyncRaw(Pointer<database_t> db, int delimiter,
    Pointer<RawOp> ops, int count) { ... }

static int databaseScanSyncRaw(Pointer<database_t> db,
    Pointer<Utf8> prefix, int prefixLen, int delimiter,
    Pointer<Pointer<RawResult>> results, Pointer<Size> count) { ... }
```

**putObjectSync** (currently loops individual `databasePutSync`):
```dart
void putObjectSync(String key, Map<String, dynamic> obj) {
    final ops = ObjectOps.flattenObject(key, obj, _delimiter);
    // Allocate flat raw_op_t array
    final opsPtr = calloc<RawOp>(ops.length);
    // ... fill key/value pointers from utf8.encode ...
    try {
        _databaseBatchSyncRaw(db, _delimiter, opsPtr, ops.length);
    } finally {
        // free all key/value calloc buffers + opsPtr
    }
}
```

**getObjectSync**:
```dart
Map<String, dynamic>? getObjectSync(String key) {
    final prefixPtr = key.toNativeUtf8();
    final resultsPtr = calloc<Pointer<RawResult>>();
    final countPtr = calloc<Size>();
    try {
        _databaseScanSyncRaw(db, prefixPtr, key.length, _delimiter,
                              resultsPtr, countPtr);
        // Iterate flat results, reconstruct Dart map
        final results = resultsPtr.value;
        final count = countPtr.value;
        return ObjectOps.reconstructObjectRaw(results, count, _delimiter);
    } finally {
        _databaseRawResultsFree(resultsPtr.value, countPtr.value);
        calloc.free(prefixPtr);
        calloc.free(resultsPtr);
        calloc.free(countPtr);
    }
}
```

**Fix Dart byte-by-byte copy:** `PathConverter._createIdentifierFromString` uses a manual byte loop instead of `asTypedList().setAll()`. With the raw API, this entire function is bypassed.

---

## Performance Expectations

| Operation | Current (Node.js) | Expected Raw | C Baseline | Gap |
|-----------|-------------------:|------------:|-----------:|----:|
| putSync   | 83K ops/s (11.2µs) | 250-330K (3-4µs) | 445K (2.0µs) | 1.5-2× |
| getSync   | 2.1M ops/s (0.46µs)| ~2M (0.5µs) | 2.1M (0.47µs) | ~1× |
| delSync   | 167K ops/s (6.5µs) | 330-500K (2-3µs) | 268K (3.5µs) | ~1× |
| putObject (N props) | ~3-5N FFI calls | 1 FFI call | N/A | 3-5× |
| getObject (N results)| ~4-6N+2 FFI calls | 1 FFI call | N/A | 4-6× |
| async put | dominated by TSFN overhead | 1 FFI call (marshal) | N/A | 2-3× for dispatch |

The remaining sync gap is unavoidable V8/Dart-VM to native boundary cost plus internal path/chunk construction. For async operations, the raw API reduces marshaling time on the main thread (better event loop responsiveness) and dispatches to the worker pool faster.

---

## Files

### Modified (C core)

- **src/HBTrie/identifier.h** — Add `identifier_create_from_raw`, `identifier_get_data_copy`
- **src/HBTrie/identifier.c** — Implement new functions
- **src/HBTrie/path.h** — Add `path_create_from_raw`
- **src/HBTrie/path.c** — Implement new function
- **src/Database/database.h** — Add all `_raw` function declarations, `raw_op_t`, `raw_result_t`
- **src/Database/database.c** — Implement sync and async raw functions, batch raw, scan raw

### Modified (Node.js binding)

- **bindings/nodejs/src/database.cc** — Add `_raw` method variants for put/get/del/batch/object ops
- **bindings/nodejs/src/path.cc** — Add `PathFromJSRaw` (stack-buffer extraction) or inline in database.cc
- **bindings/nodejs/src/identifier.cc** — Add `ValueFromJSRaw` (zero-copy buffer access) or inline

### Modified (Dart binding)

- **bindings/dart/lib/src/native/wavedb_bindings.dart** — Add raw FFI function typedefs and lookups
- **bindings/dart/lib/src/native/types.dart** — Add `RawOp`, `RawResult` struct definitions
- **bindings/dart/lib/src/database.dart** — Update putSync/getSync/delSync/putObjectSync/getObjectSync to use raw API
- **bindings/dart/lib/src/path.dart** — Add `PathConverter.toNativeRaw` or remove (bypassed by raw API)
- **bindings/dart/lib/src/identifier.dart** — Add `IdentifierConverter.toNativeRaw` or remove (bypassed by raw API)

### Created

- **tests/benchmark/benchmark_database_sync_raw.cpp** — Benchmark raw sync API vs original sync API

---

## Implementation Order

1. `identifier_create_from_raw` + `identifier_get_data_copy` (C core)
2. `path_create_from_raw` (C core)
3. Sync raw single-key functions (C core)
4. Async raw single-key functions (C core)
5. Batch raw functions (C core) — `database_write_batch_sync` already exists
6. Scan raw function (C core)
7. Node.js binding: raw sync methods
8. Node.js binding: raw async methods
9. Node.js binding: raw object/batch methods
10. Dart binding: raw FFI bindings + sync methods
11. Dart binding: raw async methods
12. Dart binding: raw object/batch methods
13. Benchmarks and validation

---

## Success Criteria

- All existing tests pass (no regressions)
- Sync raw API matches C baseline within 2× for put/delete (get already at parity)
- Async raw API reduces per-operation main-thread time by 3-5×
- `putObject`/`getObject` reduce FFI calls from O(N) to O(1)
- Dart `putObjectSync` uses batch instead of individual puts
- No memory leaks in raw API paths (valgrind verified)
- Benchmark comparison: raw vs original binding throughput