# Raw Binding API — Phase 3: Dart Binding

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Rewrite the Dart FFI binding to use the raw C API from Phase 1, reducing FFI calls from ~8-15 per operation to 1-2 and eliminating per-segment calloc/free overhead.

**Architecture:** Add raw FFI function bindings, update sync/async methods to use `database_*_sync_raw`/`database_*_raw`, fix `putObjectSync` to use batch instead of individual puts, use `database_scan_sync_raw` for `getObjectSync`.

**Tech Stack:** Dart FFI, C

**Spec:** `docs/superpowers/specs/2026-04-18-raw-binding-api-design.md`
**Depends on:** Phase 1 (C core raw API)

---

## Files

### Modify
- `bindings/dart/lib/src/native/types.dart` — Add `RawOp`, `RawResult` struct definitions
- `bindings/dart/lib/src/native/wavedb_bindings.dart` — Add raw FFI function typedefs and lookups
- `bindings/dart/lib/src/database.dart` — Rewrite sync/async methods to use raw API
- `bindings/dart/lib/src/path.dart` — Bypassed by raw API (keep for backward compat)
- `bindings/dart/lib/src/identifier.dart` — Bypassed by raw API (keep for backward compat)

---

## Task 1: Add Raw FFI Type Definitions

**Files:**
- Modify: `bindings/dart/lib/src/native/types.dart`

- [ ] **Step 1: Add RawOp and RawResult struct definitions**

Add to `types.dart`:

```dart
// Raw batch operation (input)
final class RawOp extends Struct {
  external Pointer<Utf8> key;
  @Size()
  external int keyLen;
  external Pointer<Uint8> value;
  @Size()
  external int valueLen;
  @Int32()
  external int type; // 0 = put, 1 = delete
}

// Raw scan result (output)
final class RawResult extends Struct {
  external Pointer<Utf8> key;
  @Size()
  external int keyLen;
  external Pointer<Uint8> value;
  @Size()
  external int valueLen;
}
```

- [ ] **Step 2: Verify compilation**

Run: `cd bindings/dart && dart analyze lib/src/native/types.dart 2>&1 | tail -5`
Expected: No errors.

- [ ] **Step 3: Commit**

```
feat(dart): add RawOp and RawResult FFI struct definitions
```

---

## Task 2: Add Raw FFI Function Bindings

**Files:**
- Modify: `bindings/dart/lib/src/native/wavedb_bindings.dart`

- [ ] **Step 1: Add raw FFI typedefs**

Add C and Dart typedefs for all raw functions:

```dart
// Sync raw functions
typedef DatabasePutSyncRawC = Int32 Function(
    Pointer<database_t>, Pointer<Utf8>, Size, Int8, Pointer<Uint8>, Size);
typedef DatabasePutSyncRawDart = int Function(
    Pointer<database_t>, Pointer<Utf8>, int, int, Pointer<Uint8>, int);

typedef DatabaseGetSyncRawC = Int32 Function(
    Pointer<database_t>, Pointer<Utf8>, Size, Int8,
    Pointer<Pointer<Uint8>>, Pointer<Size>);
typedef DatabaseGetSyncRawDart = int Function(
    Pointer<database_t>, Pointer<Utf8>, int, int,
    Pointer<Pointer<Uint8>>, Pointer<Size>);

typedef DatabaseDeleteSyncRawC = Int32 Function(
    Pointer<database_t>, Pointer<Utf8>, Size, Int8);
typedef DatabaseDeleteSyncRawDart = int Function(
    Pointer<database_t>, Pointer<Utf8>, int, int);

typedef DatabaseRawValueFreeC = Void Function(Pointer<Uint8>);
typedef DatabaseRawValueFreeDart = void Function(Pointer<Uint8>);

// Async raw functions
typedef DatabasePutRawC = Int32 Function(
    Pointer<database_t>, Pointer<Utf8>, Size, Int8,
    Pointer<Uint8>, Size, Pointer<promise_t>);
typedef DatabasePutRawDart = int Function(
    Pointer<database_t>, Pointer<Utf8>, int, int,
    Pointer<Uint8>, int, Pointer<promise_t>);

typedef DatabaseGetRawC = Int32 Function(
    Pointer<database_t>, Pointer<Utf8>, Size, Int8, Pointer<promise_t>);
typedef DatabaseGetRawDart = int Function(
    Pointer<database_t>, Pointer<Utf8>, int, int, Pointer<promise_t>);

typedef DatabaseDeleteRawC = Int32 Function(
    Pointer<database_t>, Pointer<Utf8>, Size, Int8, Pointer<promise_t>);
typedef DatabaseDeleteRawDart = int Function(
    Pointer<database_t>, Pointer<Utf8>, int, int, Pointer<promise_t>);

// Batch raw
typedef DatabaseBatchSyncRawC = Int32 Function(
    Pointer<database_t>, Int8, Pointer<RawOp>, Size);
typedef DatabaseBatchSyncRawDart = int Function(
    Pointer<database_t>, int, Pointer<RawOp>, int);

typedef DatabaseBatchRawC = Int32 Function(
    Pointer<database_t>, Int8, Pointer<RawOp>, Size, Pointer<promise_t>);
typedef DatabaseBatchRawDart = int Function(
    Pointer<database_t>, int, Pointer<RawOp>, int, Pointer<promise_t>);

// Scan raw
typedef DatabaseScanSyncRawC = Int32 Function(
    Pointer<database_t>, Pointer<Utf8>, Size, Int8,
    Pointer<Pointer<RawResult>>, Pointer<Size>);
typedef DatabaseScanSyncRawDart = int Function(
    Pointer<database_t>, Pointer<Utf8>, int, int,
    Pointer<Pointer<RawResult>>, Pointer<Size>);

typedef DatabaseRawResultsFreeC = Void Function(Pointer<RawResult>, Size);
typedef DatabaseRawResultsFreeDart = void Function(Pointer<RawResult>, int);
```

- [ ] **Step 2: Add lazy-loaded function pointers to WaveDBNative**

Add `static late final` fields for each raw function:

```dart
// Sync raw
static late final int Function(Pointer<database_t>, Pointer<Utf8>, int, int,
    Pointer<Uint8>, int) databasePutSyncRaw =
    WaveDBLibrary.load().lookupFunction<DatabasePutSyncRawC, DatabasePutSyncRawDart>(
        'database_put_sync_raw');

static late final int Function(Pointer<database_t>, Pointer<Utf8>, int, int,
    Pointer<Pointer<Uint8>>, Pointer<Size>) databaseGetSyncRaw =
    WaveDBLibrary.load().lookupFunction<DatabaseGetSyncRawC, DatabaseGetSyncRawDart>(
        'database_get_sync_raw');

static late final int Function(Pointer<database_t>, Pointer<Utf8>, int, int)
    databaseDeleteSyncRaw =
    WaveDBLibrary.load().lookupFunction<DatabaseDeleteSyncRawC, DatabaseDeleteSyncRawDart>(
        'database_delete_sync_raw');

static late final void Function(Pointer<Uint8>) databaseRawValueFree =
    WaveDBLibrary.load().lookupFunction<DatabaseRawValueFreeC, DatabaseRawValueFreeDart>(
        'database_raw_value_free');

// Async raw
static late final int Function(Pointer<database_t>, Pointer<Utf8>, int, int,
    Pointer<Uint8>, int, Pointer<promise_t>) databasePutRaw =
    WaveDBLibrary.load().lookupFunction<DatabasePutRawC, DatabasePutRawDart>(
        'database_put_raw');

static late final int Function(Pointer<database_t>, Pointer<Utf8>, int, int,
    Pointer<promise_t>) databaseGetRaw =
    WaveDBLibrary.load().lookupFunction<DatabaseGetRawC, DatabaseGetRawDart>(
        'database_get_raw');

static late final int Function(Pointer<database_t>, Pointer<Utf8>, int, int,
    Pointer<promise_t>) databaseDeleteRaw =
    WaveDBLibrary.load().lookupFunction<DatabaseDeleteRawC, DatabaseDeleteRawDart>(
        'database_delete_raw');

// Batch raw
static late final int Function(Pointer<database_t>, int, Pointer<RawOp>, int)
    databaseBatchSyncRaw =
    WaveDBLibrary.load().lookupFunction<DatabaseBatchSyncRawC, DatabaseBatchSyncRawDart>(
        'database_batch_sync_raw');

static late final int Function(Pointer<database_t>, int, Pointer<RawOp>, int,
    Pointer<promise_t>) databaseBatchRaw =
    WaveDBLibrary.load().lookupFunction<DatabaseBatchRawC, DatabaseBatchRawDart>(
        'database_batch_raw');

// Scan raw
static late final int Function(Pointer<database_t>, Pointer<Utf8>, int, int,
    Pointer<Pointer<RawResult>>, Pointer<Size>) databaseScanSyncRaw =
    WaveDBLibrary.load().lookupFunction<DatabaseScanSyncRawC, DatabaseScanSyncRawDart>(
        'database_scan_sync_raw');

static late final void Function(Pointer<RawResult>, int) databaseRawResultsFree =
    WaveDBLibrary.load().lookupFunction<DatabaseRawResultsFreeC, DatabaseRawResultsFreeDart>(
        'database_raw_results_free');
```

- [ ] **Step 3: Verify compilation**

Run: `cd bindings/dart && dart analyze lib/src/native/wavedb_bindings.dart 2>&1 | tail -5`
Expected: No errors.

- [ ] **Step 4: Commit**

```
feat(dart): add raw FFI function bindings for sync/async/batch/scan
```

---

## Task 3: Rewrite Sync Methods to Use Raw API

**Files:**
- Modify: `bindings/dart/lib/src/database.dart`

- [ ] **Step 1: Rewrite putSync**

Replace the current `_putSyncInternal`:

```dart
void putSync(String key, Uint8List value) {
    _checkClosed();
    final keyPtr = key.toNativeUtf8();
    final valuePtr = calloc<Uint8>(value.length);
    valuePtr.asTypedList(value.length).setAll(0, value);
    try {
        final rc = WaveDBNative.databasePutSyncRaw(
            _db!, keyPtr, key.length, _delimiter, valuePtr, value.length);
        if (rc != 0) throw WaveDBException.putFailed();
    } finally {
        calloc.free(keyPtr);
        calloc.free(valuePtr);
    }
}
```

- [ ] **Step 2: Rewrite getSync**

```dart
Uint8List? getSync(String key) {
    _checkClosed();
    final keyPtr = key.toNativeUtf8();
    final valueOutPtr = calloc<Pointer<Uint8>>();
    final valueLenPtr = calloc<Size>();
    try {
        final rc = WaveDBNative.databaseGetSyncRaw(
            _db!, keyPtr, key.length, _delimiter, valueOutPtr, valueLenPtr);
        if (rc == -2) return null;
        if (rc != 0) throw WaveDBException.getFailed();

        final valuePtr = valueOutPtr.value;
        final valueLen = valueLenPtr.value;
        final result = Uint8List.fromList(valuePtr.asTypedList(valueLen));
        WaveDBNative.databaseRawValueFree(valuePtr);
        return result;
    } finally {
        calloc.free(keyPtr);
        calloc.free(valueOutPtr);
        calloc.free(valueLenPtr);
    }
}
```

- [ ] **Step 3: Rewrite deleteSync**

```dart
void deleteSync(String key) {
    _checkClosed();
    final keyPtr = key.toNativeUtf8();
    try {
        final rc = WaveDBNative.databaseDeleteSyncRaw(
            _db!, keyPtr, key.length, _delimiter);
        if (rc != 0) throw WaveDBException.deleteFailed();
    } finally {
        calloc.free(keyPtr);
    }
}
```

- [ ] **Step 4: Build and run Dart tests**

Run: `cd bindings/dart && dart test test/wavedb_test.dart 2>&1 | tail -20`
Expected: All tests PASS.

- [ ] **Step 5: Commit**

```
feat(dart): rewrite sync methods to use raw C API
```

---

## Task 4: Rewrite Async Methods to Use Raw API

**Files:**
- Modify: `bindings/dart/lib/src/database.dart`

- [ ] **Step 1: Rewrite put (async)**

Replace the current `put` method to use `database_put_raw`:

```dart
Future<void> put(String key, Uint8List value) {
    _checkClosed();
    final keyPtr = key.toNativeUtf8();
    final valuePtr = calloc<Uint8>(value.length);
    valuePtr.asTypedList(value.length).setAll(0, value);
    return _dispatchRawAsync(() {
        final promise = WaveDBNative.promiseCreate(
            _cResolveCallback.nativeFunction,
            _cRejectCallback.nativeFunction,
            Pointer<Void>.fromAddress(_nextRequestId()));
        final rc = WaveDBNative.databasePutRaw(
            _db!, keyPtr, key.length, _delimiter, valuePtr, value.length, promise);
        if (rc != 0) {
            WaveDBNative.promiseDestroy(promise);
            calloc.free(keyPtr);
            calloc.free(valuePtr);
            throw WaveDBException.putFailed();
        }
    }, keyPtr, valuePtr);
}
```

Note: The `_dispatchRawAsync` helper must ensure the key/value pointers stay alive until the C worker thread copies them. The existing `_dispatchAsync` pattern handles this via the `_PendingOp` map.

- [ ] **Step 2: Rewrite get (async)**

```dart
Future<Uint8List?> get(String key) {
    _checkClosed();
    final keyPtr = key.toNativeUtf8();
    return _dispatchRawGetAsync(() {
        final promise = WaveDBNative.promiseCreate(
            _cResolveCallback.nativeFunction,
            _cRejectCallback.nativeFunction,
            Pointer<Void>.fromAddress(_nextRequestId()));
        final rc = WaveDBNative.databaseGetRaw(
            _db!, keyPtr, key.length, _delimiter, promise);
        if (rc != 0) {
            WaveDBNative.promiseDestroy(promise);
            calloc.free(keyPtr);
            throw WaveDBException.getFailed();
        }
    }, keyPtr);
}
```

The result callback still receives `identifier_t*` — the existing `_cResolveCallback` handles `identifierReference` + `identifierDestroy` + `IdentifierConverter.fromNative`. No changes to the callback itself.

- [ ] **Step 3: Rewrite delete (async)**

```dart
Future<void> delete(String key) {
    _checkClosed();
    final keyPtr = key.toNativeUtf8();
    return _dispatchRawDeleteAsync(() {
        final promise = WaveDBNative.promiseCreate(
            _cResolveCallback.nativeFunction,
            _cRejectCallback.nativeFunction,
            Pointer<Void>.fromAddress(_nextRequestId()));
        final rc = WaveDBNative.databaseDeleteRaw(
            _db!, keyPtr, key.length, _delimiter, promise);
        if (rc != 0) {
            WaveDBNative.promiseDestroy(promise);
            calloc.free(keyPtr);
            throw WaveDBException.deleteFailed();
        }
    }, keyPtr);
}
```

- [ ] **Step 4: Build and run Dart tests**

Run: `cd bindings/dart && dart test test/wavedb_test.dart 2>&1 | tail -20`
Expected: All tests PASS.

- [ ] **Step 5: Commit**

```
feat(dart): rewrite async methods to use raw C API
```

---

## Task 5: Rewrite Object/Batch Operations

**Files:**
- Modify: `bindings/dart/lib/src/database.dart`

- [ ] **Step 1: Rewrite putObjectSync to use batch**

Replace the current iterative `_putSyncInternal` loop:

```dart
void putObjectSync(String key, Map<String, dynamic> obj) {
    _checkClosed();
    final ops = ObjectOps.flattenObject(key, obj, _delimiter);
    if (ops.isEmpty) return;

    // Allocate flat raw_op_t array
    final opsPtr = calloc<RawOp>(ops.length);
    final keyPtrs = <Pointer<Utf8>>[];
    final valuePtrs = <Pointer<Uint8>>[];

    try {
        for (int i = 0; i < ops.length; i++) {
            final keyStr = ops[i]['key'].join(_delimiter);
            final keyPtr = keyStr.toNativeUtf8();
            keyPtrs.add(keyPtr);
            opsPtr[i].key = keyPtr;
            opsPtr[i].keyLen = keyStr.length;
            opsPtr[i].type = 0; // put

            if (ops[i]['value'] is Uint8List) {
                final val = ops[i]['value'] as Uint8List;
                final valPtr = calloc<Uint8>(val.length);
                valPtr.asTypedList(val.length).setAll(0, val);
                valuePtrs.add(valPtr);
                opsPtr[i].value = valPtr;
                opsPtr[i].valueLen = val.length;
            } else {
                final valStr = ops[i]['value'].toString();
                final valBytes = utf8.encode(valStr);
                final valPtr = calloc<Uint8>(valBytes.length);
                valPtr.asTypedList(valBytes.length).setAll(0, valBytes);
                valuePtrs.add(valPtr);
                opsPtr[i].value = valPtr;
                opsPtr[i].valueLen = valBytes.length;
            }
        }

        final rc = WaveDBNative.databaseBatchSyncRaw(
            _db!, _delimiter.codeUnitAt(0), opsPtr, ops.length);
        if (rc != 0) throw WaveDBException.batchFailed();
    } finally {
        for (final p in keyPtrs) calloc.free(p);
        for (final p in valuePtrs) calloc.free(p);
        calloc.free(opsPtr);
    }
}
```

- [ ] **Step 2: Rewrite getObjectSync to use scan_raw**

```dart
Map<String, dynamic>? getObjectSync(String key) {
    _checkClosed();
    final prefixPtr = key.toNativeUtf8();
    final resultsPtr = calloc<Pointer<RawResult>>();
    final countPtr = calloc<Size>();

    try {
        final rc = WaveDBNative.databaseScanSyncRaw(
            _db!, prefixPtr, key.length, _delimiter.codeUnitAt(0),
            resultsPtr, countPtr);
        if (rc != 0) throw WaveDBException.scanFailed();

        final results = resultsPtr.value;
        final count = countPtr.value;

        if (count == 0) return {};

        // Build map from flat results
        final Map<String, dynamic> result = {};
        for (int i = 0; i < count; i++) {
            final rawKey = results[i].key;
            final keyLen = results[i].keyLen;
            final rawVal = results[i].value;
            final valLen = results[i].valueLen;

            final keyStr = rawKey.cast<Utf8>().toDartString(length: keyLen);
            final valBytes = Uint8List.fromList(rawVal.asTypedList(valLen));

            // Set in nested map structure
            _setNestedValue(result, keyStr.split(_delimiter), valBytes);
        }

        WaveDBNative.databaseRawResultsFree(results, count);
        return result;
    } finally {
        calloc.free(prefixPtr);
        calloc.free(resultsPtr);
        calloc.free(countPtr);
    }
}
```

- [ ] **Step 3: Rewrite batchSync**

Use `database_batch_sync_raw` instead of iterating individual puts.

- [ ] **Step 4: Build and run Dart tests**

Run: `cd bindings/dart && dart test test/wavedb_test.dart 2>&1 | tail -20`
Expected: All tests PASS.

- [ ] **Step 5: Commit**

```
feat(dart): rewrite object/batch operations to use raw batch/scan API
```

---

## Task 6: Validation and Benchmark

**Files:**
- No new files

- [ ] **Step 1: Run full Dart test suite**

Run: `cd bindings/dart && dart test 2>&1`
Expected: All tests PASS.

- [ ] **Step 2: Run Dart benchmark**

Run: `cd bindings/dart && dart run benchmark/benchmark.dart 2>&1`
Expected: Measurable throughput improvement.

- [ ] **Step 3: Run Node.js vs Dart comparison**

Run: `cd bindings/dart && bash benchmark/compare.sh 2>&1`
Expected: Dart and Node.js throughput now closer to C baseline.

- [ ] **Step 4: Commit any fixes if needed**

---

## Summary

Phase 3 delivers the complete Dart binding rewrite:

| Method | Before (FFI calls) | After (FFI calls) |
|--------|:------------------:|:-----------------:|
| putSync | ~8-15 | 1 |
| getSync | ~8-15 | 1 |
| deleteSync | ~8-15 | 1 |
| put (async) | ~8-15 | 1 |
| get (async) | ~8-15 | 1 |
| delete (async) | ~8-15 | 1 |
| putObjectSync (N props) | ~5-8N (individual puts!) | 1 (batch) |
| getObjectSync (N results) | ~6-10N+2 | 1 |
| batchSync (N ops) | ~5-8N | 1 |