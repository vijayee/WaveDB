# Dart Bindings Design

**Date:** 2026-04-02  
**Status:** Approved  
**Author:** Claude Sonnet 4.6

## Overview

This document describes the design for creating Dart FFI bindings for WaveDB, equivalent to the existing Node.js bindings. The bindings will support both server-side Dart VM applications and Flutter mobile/desktop apps.

## Goals

1. **Complete API parity** with Node.js bindings (sync + async operations, batch, objects, streaming)
2. **Cross-platform support** (Linux x64/ARM64, macOS x64/ARM64, Windows x64, Android ARM64/x64, iOS ARM64)
3. **Pre-built binaries** bundled in package for easy installation
4. **Idiomatic Dart API** using async/await, Streams, and null safety
5. **Comprehensive testing** porting all Node.js tests to Dart

## Architecture

### Package Structure

```
bindings/dart/
├── lib/
│   ├── wavedb.dart              # Main library export
│   ├── src/
│   │   ├── wavedb.dart          # WaveDB class implementation
│   │   ├── ffi/
│   │   │   ├── bindings.dart   # FFI function signatures
│   │   │   ├── structs.dart    # FFI struct definitions
│   │   │   └── library.dart    # Native library loading
│   │   ├── types/
│   │   │   ├── path.dart       # Path type wrapper
│   │   │   ├── identifier.dart # Identifier type wrapper
│   │   │   └── batch.dart      # Batch operation types
│   │   ├── utils/
│   │   │   ├── iterator.dart   # ReadStream iterator
│   │   │   └── converter.dart  # JS/Dart type conversion helpers
│   │   └── exceptions.dart     # Exception hierarchy
├── src/                         # C wrapper source
│   ├── wavedb_wrapper.h        # C wrapper header
│   └── wavedb_wrapper.c        # C wrapper implementation
├── build/                       # Pre-built binaries
│   ├── linux-x64/
│   ├── linux-arm64/
│   ├── macos-x64/
│   ├── macos-arm64/
│   ├── windows-x64/
│   ├── android-arm64/
│   └── android-x64/
├── test/
│   ├── unit/
│   ├── integration/
│   ├── performance/
│   └── platform/
├── pubspec.yaml
├── CMakeLists.txt
└── README.md
```

### Key Design Decisions

1. **Thin C Wrapper**: Expose a FFI-friendly C API that wraps WaveDB's complex structs
2. **Opaque Handles**: Use opaque pointers for database/batch/iterator handles
3. **Manual FFI Bindings**: Hand-written FFI bindings (not ffigen) for full control
4. **Thread Pool for Async**: C wrapper maintains thread pool for background operations
5. **Pre-built Binaries**: Platform-specific libraries bundled in package

## C Wrapper API Design

### Rationale

WaveDB's C API uses complex structs (`database_t`, `path_t`, `identifier_t`) that are difficult to work with via FFI. A thin C wrapper provides:

- **Opaque handles** instead of complex struct pointers
- **Simpler function signatures** that map cleanly to FFI
- **Error codes** instead of complex error structs
- **String-based APIs** for paths instead of struct construction
- **Thread pool** for async operations without isolates

### API Functions

```c
// Lifecycle
wavedb_handle_t* wavedb_open(const char* path, int* error_code);
void wavedb_close(wavedb_handle_t* handle);
const char* wavedb_get_version();

// Synchronous Operations
int wavedb_put(wavedb_handle_t* handle, const char* key, const char* value);
int wavedb_get(wavedb_handle_t* handle, const char* key, char** value, int* value_len);
int wavedb_delete(wavedb_handle_t* handle, const char* key);

// Batch Operations
wavedb_batch_t* wavedb_batch_create();
int wavedb_batch_put(wavedb_batch_t* batch, const char* key, const char* value);
int wavedb_batch_delete(wavedb_batch_t* batch, const char* key);
int wavedb_batch_execute(wavedb_handle_t* handle, wavedb_batch_t* batch);
void wavedb_batch_destroy(wavedb_batch_t* batch);

// Async Operations (Thread Pool)
async_handle_t* wavedb_put_async(wavedb_handle_t* handle, 
                                   const char* key, 
                                   const char* value,
                                   async_callback_t callback,
                                   void* user_data);

async_handle_t* wavedb_get_async(wavedb_handle_t* handle,
                                   const char* key,
                                   async_callback_t callback,
                                   void* user_data);

int wavedb_async_wait(async_handle_t* async_handle, int timeout_ms);
int wavedb_async_is_complete(async_handle_t* async_handle);
int wavedb_async_get_result(async_handle_t* async_handle);
char* wavedb_async_get_value(async_handle_t* async_handle, int* value_len);
void wavedb_async_destroy(async_handle_t* async_handle);

// Iteration
wavedb_iterator_t* wavedb_iterator_create(wavedb_handle_t* handle);
int wavedb_iterator_next(wavedb_iterator_t* iter, char** key, int* key_len, char** value, int* value_len);
void wavedb_iterator_destroy(wavedb_iterator_t* iter);

// Object Operations
int wavedb_put_object(wavedb_handle_t* handle, const char* base_key, const char* json_value);
int wavedb_get_object(wavedb_handle_t* handle, const char* base_key, char** json_value, int* value_len);

// Memory Management
void wavedb_free_string(char* str);
void wavedb_cleanup(void);

// Error Messages
const char* wavedb_get_error_message(int error_code);
```

### Memory Ownership

- **Input strings**: Dart owns, C wrapper copies if needed
- **Output strings**: C wrapper allocates, Dart must call `wavedb_free_string()`
- **Handles**: C wrapper manages, Dart calls `*_destroy()` functions
- **Async handles**: C wrapper manages, Dart calls `wavedb_async_destroy()`

### Thread Pool Implementation

The C wrapper maintains a thread pool (8 threads by default) for async operations:

1. Async operations are queued to thread pool
2. Worker threads execute operations concurrently
3. Results stored in async_handle with completion flag
4. Dart polls completion or waits on condition variable

Advantages:
- **True parallelism** - Multiple operations run concurrently
- **Efficient** - No thread creation overhead, threads are reused
- **Scalable** - Configurable thread pool size
- **Same handle** - Database handle shared across threads (WaveDB internally thread-safe)

## Dart FFI Bindings Layer

### Library Loading

```dart
import 'dart:ffi';
import 'dart:io';

DynamicLibrary _openLibrary() {
  if (Platform.isLinux) {
    if (Platform.version.contains('arm64')) {
      return DynamicLibrary.open('build/linux-arm64/libwavedb_wrapper.so');
    }
    return DynamicLibrary.open('build/linux-x64/libwavedb_wrapper.so');
  }
  if (Platform.isMacOS) {
    if (Platform.version.contains('arm64')) {
      return DynamicLibrary.open('build/macos-arm64/libwavedb_wrapper.dylib');
    }
    return DynamicLibrary.open('build/macos-x64/libwavedb_wrapper.dylib');
  }
  if (Platform.isWindows) {
    return DynamicLibrary.open('build/windows-x64/wavedb_wrapper.dll');
  }
  // Android handled via jniLibs
  throw UnsupportedError('Unsupported platform');
}

final DynamicLibrary _library = _openLibrary();
```

### FFI Type Definitions

```dart
// Opaque handles
typedef WavedbHandle = Pointer<Void>;
typedef WavedbBatch = Pointer<Void>;
typedef WavedbIterator = Pointer<Void>;
typedef AsyncHandle = Pointer<Void>;

// Native function signatures (C side)
typedef WavedbOpenNative = Pointer<WavedbHandle> Function(Pointer<Utf8> path, Pointer<Int32> error_code);
typedef WavedbPutAsyncNative = Pointer<AsyncHandle> Function(
  Pointer<WavedbHandle> handle,
  Pointer<Utf8> key,
  Pointer<Utf8> value,
  Pointer<NativeFunction<AsyncCallback>> callback,
  Pointer<Void> user_data
);

// Dart wrapper signatures
typedef WavedbOpen = Pointer<WavedbHandle> Function(Pointer<Utf8> path, Pointer<Int32> error_code);
typedef WavedbPutAsync = Pointer<AsyncHandle> Function(
  Pointer<WavedbHandle> handle,
  Pointer<Utf8> key,
  Pointer<Utf8> value,
  Pointer<NativeFunction<AsyncCallback>> callback,
  Pointer<Void> user_data
);

// Binding class
class WaveDBBindings {
  final WavedbOpen open;
  final WavedbPutAsync put_async;
  final WavedbAsyncWait async_wait;
  final WavedbAsyncGetResult async_get_result;
  // ... more bindings
  
  WaveDBBindings(DynamicLibrary library)
      : open = library.lookupFunction<WavedbOpenNative, WavedbOpen>('wavedb_open'),
        put_async = library.lookupFunction<WavedbPutAsyncNative, WavedbPutAsync>('wavedb_put_async');
}
```

### Memory Management

```dart
// Helper to allocate and convert Dart string to C string
Pointer<Utf8> stringToNative(String str) {
  return str.toNativeUtf8();
}

// Helper to convert C string to Dart string and free C memory
String nativeToString(Pointer<Utf8> native, int length) {
  final str = native.toDartString(length: length);
  calloc.free(native);
  return str;
}
```

## Public Dart API Design

### Main WaveDB Class

```dart
class WaveDB {
  Pointer<WavedbHandle>? _handle;
  final String _path;
  final String _delimiter;
  static final WaveDBBindings _bindings = WaveDBBindings(_library);
  
  // Constructor is private - use factory methods
  WaveDB._(this._path, this._delimiter, this._handle);
  
  // ==================== OPEN/CLOSE ====================
  
  static Future<WaveDB> open(String path, {String delimiter = '/'}) async {
    return _openInternal(path, delimiter);
  }
  
  static WaveDB openSync(String path, {String delimiter = '/'}) {
    return _openInternal(path, delimiter);
  }
  
  static WaveDB _openInternal(String path, String delimiter) {
    final pathPtr = path.toNativeUtf8();
    final errorCodePtr = calloc<Int32>();
    
    try {
      final handle = _bindings.open(pathPtr, errorCodePtr);
      if (errorCodePtr.value != 0) {
        throwWaveDBError(errorCodePtr.value, _bindings);
      }
      
      final instance = WaveDB._(path, delimiter, handle);
      _finalizer.attach(instance, handle, detach: instance);
      return instance;
    } finally {
      calloc.free(pathPtr);
      calloc.free(errorCodePtr);
    }
  }
  
  Future<void> close() async => closeSync();
  
  void closeSync() {
    if (_handle != null) {
      _finalizer.detach(this);
      _bindings.close(_handle!);
      _handle = null;
    }
  }
  
  // ==================== SYNC OPERATIONS ====================
  
  void putSync(Path key, String value);
  String? getSync(Path key);
  void deleteSync(Path key);
  void batchSync(List<BatchOperation> operations);
  void putObjectSync(Path baseKey, Map<String, dynamic> object);
  Map<String, dynamic>? getObjectSync(Path baseKey);
  
  // ==================== ASYNC OPERATIONS ====================
  
  /// Async put - uses background thread pool in C
  Future<void> put(Path key, String value) async {
    _ensureOpen();
    
    final keyPtr = key.toString().toNativeUtf8();
    final valuePtr = value.toNativeUtf8();
    
    try {
      final asyncHandle = _bindings.put_async(_handle!, keyPtr, valuePtr, nullptr, nullptr);
      
      // Wait in isolate for true async
      await Isolate.run(() {
        while (_bindings.async_is_complete(asyncHandle) == 0) {
          usleep(1000); // 1ms sleep
        }
        
        final result = _bindings.async_get_result(asyncHandle);
        _bindings.async_destroy(asyncHandle);
        return result;
      });
      
      if (result != 0) {
        throwWaveDBError(result, _bindings, context: 'Failed to put key: $key');
      }
    } finally {
      calloc.free(keyPtr);
      calloc.free(valuePtr);
    }
  }
  
  Future<String?> get(Path key);
  Future<void> delete(Path key);
  Future<void> batch(List<BatchOperation> operations);
  Future<void> putObject(Path baseKey, Map<String, dynamic> object);
  Future<Map<String, dynamic>?> getObject(Path baseKey);
  
  // ==================== STREAMING ====================
  
  Stream<MapEntry<Path, String>> createReadStream({Path? prefix});
  WaveDBIterator createSyncIterator({Path? prefix});
}
```

### Path Type

```dart
class Path {
  final List<String> parts;
  final String delimiter;
  
  Path(this.parts, {this.delimiter = '/'});
  
  factory Path.fromString(String path, {String delimiter = '/'}) {
    return Path(path.split(delimiter), delimiter: delimiter);
  }
  
  Path operator +(String part) => Path([...parts, part], delimiter: delimiter);
  
  bool startsWith(Path other) {
    if (other.parts.length > parts.length) return false;
    for (int i = 0; i < other.parts.length; i++) {
      if (parts[i] != other.parts[i]) return false;
    }
    return true;
  }
  
  @override
  String toString() => parts.join(delimiter);
}
```

### Batch Operations

```dart
abstract class BatchOperation {
  Path get key;
}

class PutOperation implements BatchOperation {
  @override
  final Path key;
  final String value;
  
  PutOperation(this.key, this.value);
}

class DeleteOperation implements BatchOperation {
  @override
  final Path key;
  
  DeleteOperation(this.key);
}
```

### Usage Example

```dart
import 'package:wavedb/wavedb.dart';

void main() async {
  // Async API
  final db = await WaveDB.open('/path/to/db');
  
  await db.put(Path(['users', 'alice', 'name']), 'Alice');
  final name = await db.get(Path(['users', 'alice', 'name']));
  
  await db.putObject(Path(['users', 'bob']), {'name': 'Bob', 'age': 30});
  final user = await db.getObject(Path(['users', 'bob']));
  
  await db.batch([
    PutOperation(Path(['key1']), 'value1'),
    DeleteOperation(Path(['old'])),
  ]);
  
  await for (final entry in db.createReadStream(prefix: Path(['users']))) {
    print('${entry.key}: ${entry.value}');
  }
  
  await db.close();
  
  // Sync API
  final dbSync = WaveDB.openSync('/path/to/db');
  dbSync.putSync(Path(['counter']), '1');
  print(dbSync.getSync(Path(['counter'])));
  dbSync.closeSync();
}
```

## Error Handling

### Error Codes

```c
#define WAVEDB_OK                    0
#define WAVEDB_ERROR_UNKNOWN        1
#define WAVEDB_ERROR_INVALID_HANDLE 2
#define WAVEDB_ERROR_INVALID_PATH   3
#define WAMEDB_ERROR_KEY_NOT_FOUND   404
#define WAVEDB_ERROR_OUT_OF_MEMORY  5
#define WAVEDB_ERROR_DATABASE       100
#define WAVEDB_ERROR_LOCKING        101
#define WAVEDB_ERROR_CORRUPTION     102
#define WAVEDB_ERROR_IO             103
```

### Exception Hierarchy

```dart
class WaveDBException implements Exception {
  final int errorCode;
  final String message;
  
  WaveDBException(this.errorCode, this.message);
  @override
  String toString() => 'WaveDBException($errorCode): $message';
}

class KeyNotFoundException extends WaveDBException {
  KeyNotFoundException(String key) : super(404, 'Key not found: $key');
}

class InvalidHandleException extends WaveDBException {
  InvalidHandleException() : super(2, 'Invalid database handle');
}

class DatabaseClosedException extends StateError {
  DatabaseClosedException() : super('Database is closed');
}
```

## Build and Distribution

### CMake Build

```cmake
cmake_minimum_required(VERSION 3.15)
project(wavedb_wrapper)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O3 -fPIC")

set(WRAPPER_SOURCES src/wavedb_wrapper.c)

include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src
    ${CMAKE_CURRENT_SOURCE_DIR}/../../deps/libcbor/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../../deps/hashmap/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../deps/xxhash
)

add_library(wavedb_wrapper SHARED ${WRAPPER_SOURCES})

target_link_libraries(wavedb_wrapper
    ${CMAKE_CURRENT_SOURCE_DIR}/../../build-release/libwavedb.a
    ${CMAKE_CURRENT_SOURCE_DIR}/../../build-release/libxxhash.a
    ${CMAKE_CURRENT_SOURCE_DIR}/../../build-release/libhashmap.a
    ${CMAKE_CURRENT_SOURCE_DIR}/../../build-release/deps/libcbor/src/libcbor.a
    pthread
)
```

### Platform Support

| Platform | Architecture | Library Name |
|----------|--------------|--------------|
| Linux | x64, ARM64 | libwavedb_wrapper.so |
| macOS | x64, ARM64 | libwavedb_wrapper.dylib |
| Windows | x64 | wavedb_wrapper.dll |
| Android | ARM64, x64 | libwavedb_wrapper.so |
| iOS | ARM64 | wavedb_wrapper.framework |

### Build Script

A comprehensive `build_all_platforms.sh` script builds for all target platforms:

1. Ensures WaveDB library is built
2. Compiles wrapper for each platform/architecture
3. Uses cross-compilers where needed (ARM, Windows)
4. Uses Android NDK for Android builds
5. Uses iOS toolchain for iOS builds

### GitHub Actions CI/CD

Automated builds for:
- Linux x64/ARM64
- macOS x64/ARM64
- Windows x64
- Android ARM64/x64

Artifacts packaged and uploaded for distribution.

### pubspec.yaml

```yaml
name: wavedb
version: 0.1.0
description: Dart bindings for WaveDB

environment:
  sdk: '>=3.0.0 <4.0.0'

dependencies:
  ffi: ^2.1.0
  path: ^1.8.0

dev_dependencies:
  test: ^1.24.0
  benchmark_harness: ^2.2.2

platforms:
  linux:
  macos:
  windows:
```

## Testing Strategy

### Test Organization

```
test/
├── unit/
│   ├── path_test.dart
│   ├── batch_test.dart
│   └── exception_test.dart
├── integration/
│   ├── wavedb_test.dart        # Basic operations
│   ├── mvcc_test.dart          # MVCC version chains
│   ├── snapshot_test.dart      # Snapshot serialization
│   ├── persistence_test.dart   # Persistence tests
│   ├── batch_test.dart         # Batch operations
│   ├── object_test.dart        # Object operations
│   └── stream_test.dart        # Streaming
├── performance/
│   └── benchmark_test.dart
└── platform/
    ├── flutter_test.dart
    └── isolate_test.dart
```

### Test Coverage

Porting all Node.js tests:

1. **Basic Operations** (wavedb_test.dart)
   - Put/Get/Delete
   - Overwrite
   - Non-existent keys
   - Sync operations

2. **MVCC Version Chains** (mvcc_test.dart)
   - Simple overwrite
   - Multiple overwrites
   - Delete operations
   - Overwrite then delete
   - Async operations with overwrites
   - Batch operations with overwrites
   - Multiple keys with overwrites
   - Database close and reopen

3. **Snapshot with Version Chains** (snapshot_test.dart)
   - Single write without version chain
   - Two writes to same key
   - Multiple overwrites (10 versions)
   - Complex version chains across restart
   - Empty database snapshot

4. **Batch Operations** (batch_test.dart)
   - Batch put operations
   - Batch mixed operations
   - Large batch (1000 operations)
   - Sync batch

5. **Object Operations** (object_test.dart)
   - Put and get object
   - Nested object
   - Empty object
   - Non-existent object

6. **Streaming** (stream_test.dart)
   - Read all entries
   - Read with prefix
   - Sync iterator

7. **Isolate Concurrency** (isolate_test.dart)
   - Concurrent writes from multiple isolates
   - Concurrent reads

### Test Fixtures

```dart
class TestFixture {
  late WaveDB db;
  late Directory tempDir;
  
  Future<void> setUp({String delimiter = '/'}) async {
    tempDir = await Directory.systemTemp.createTemp('wavedb_test_');
    db = await WaveDB.open(tempDir.path, delimiter: delimiter);
  }
  
  Future<void> tearDown() async {
    await db.close();
    await tempDir.delete(recursive: true);
  }
  
  Future<void> reopen() async {
    await db.close();
    db = await WaveDB.open(tempDir.path);
  }
}
```

### Performance Benchmarks

Using `benchmark_harness` package:

- **Put throughput**: Target >10,000 ops/sec
- **Get throughput**: Target >50,000 ops/sec
- **Batch throughput**: Target >5,000 ops/sec
- **Iterator throughput**: Target >20,000 entries/sec

## Implementation Approach

### Phase 1: C Wrapper (Week 1)
1. Implement C wrapper with sync operations
2. Implement thread pool for async operations
3. Test C wrapper independently
4. Build system for all platforms

### Phase 2: FFI Bindings (Week 1-2)
1. Write FFI bindings for all C functions
2. Implement memory management helpers
3. Implement error handling
4. Test FFI layer

### Phase 3: Public API (Week 2)
1. Implement WaveDB class
2. Implement Path, Batch types
3. Implement sync operations
4. Implement async operations
5. Implement streaming

### Phase 4: Testing (Week 2-3)
1. Port all Node.js tests
2. Write unit tests
3. Write integration tests
4. Write isolate tests
5. Performance benchmarks

### Phase 5: Distribution (Week 3)
1. Build binaries for all platforms
2. Package for pub.dev
3. Write documentation
4. Write usage examples
5. Set up CI/CD

## Success Criteria

- [ ] All Node.js API functionality available in Dart
- [ ] All Node.js tests passing in Dart
- [ ] Performance within 20% of Node.js bindings
- [ ] Works on all target platforms (Linux, macOS, Windows, Android, iOS)
- [ ] Easy installation via pub.dev
- [ ] Comprehensive documentation and examples

## References

- Node.js bindings: `bindings/nodejs/`
- WaveDB C API: `src/`
- C wrapper implementation: `bindings/dart/src/wavedb_wrapper.c`
- FFI documentation: https://dart.dev/guides/libraries/c-interop