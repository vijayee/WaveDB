# Dart Bindings for WaveDB - Design Document

**Goal:** Provide Dart FFI bindings for WaveDB with async/sync operations, streaming, and object manipulation capabilities, matching the Node.js bindings API.

**Architecture:** Dart FFI bindings using manual type definitions and function signatures. Thin FFI layer wraps C functions, with Dart wrapper classes providing the public API.

**Tech Stack:**
- dart:ffi for native interop
- Manual FFI bindings (no code generation)
- Stream-based iteration (Dart idiomatic)
- Async/sync API variants

---

## Architecture

### Module Structure

**Core Principle:** Expose database_t as the primary interface, not hbtrie_t. HBTrie is an internal implementation detail.

```
bindings/dart/
├── lib/
│   ├── wavedb.dart              # Library entry point
│   ├── src.dart                  # Internal exports
│   └── src/
│       ├── native/
│       │   ├── types.dart        # FFI type definitions
│       │   ├── wavedb_bindings.dart  # FFI function bindings
│       │   └── wavedb_library.dart  # Platform-specific loader
│       ├── database.dart         # WaveDB public class
│       ├── path.dart             # Path conversion
│       ├── identifier.dart       # Value conversion
│       ├── exceptions.dart       # WaveDBException
│       ├── iterator.dart         # Stream iterator
│       └── object_ops.dart       # putObject/getObject helpers
├── test/
│   ├── wavedb_test.dart          # Core operations tests
│   ├── object_ops_test.dart      # Object operations tests
│   └── stream_test.dart          # Iterator tests
├── example/
│   └── example.dart              # Usage examples
├── pubspec.yaml
├── build.yaml
└── README.md
```

### Layered Architecture

```
┌─────────────────────────────────────────────────────────┐
│                    Dart Application                       │
├─────────────────────────────────────────────────────────┤
│                   Public API Layer                        │
│  ┌─────────────┐  ┌───────────┐  ┌─────────────────┐    │
│  │  Database   │  │ WaveDB    │  │  StreamIterator │    │
│  │  (public)   │  │Exception  │  │  (public)       │    │
│  └──────┬──────┘  └───────────┘  └─────────────────┘    │
├─────────┼───────────────────────────────────────────────┤
│         │           Conversion Layer                      │
│  ┌──────┴──────┐  ┌───────────┐  ┌─────────────────┐    │
│  │    Path     │  │Identifier │  │    Object       │    │
│  │ Conversion  │  │Conversion │  │   Flatten/Build  │    │
│  └──────┬──────┘  └─────┬─────┘  └─────────────────┘    │
├─────────┼───────────────┼─────────────────────────────────┤
│         │     FFI Bindings Layer                          │
│  ┌──────┴───────────────┴──────┐                         │
│  │     WaveDBNative (dart:ffi) │                         │
│  └──────────────┬──────────────┘                         │
├────────────────┼─────────────────────────────────────────┤
│                │    libwavedb.so / wavedb.dll            │
│                │    (C Library)                          │
│                │                                           │
│  ┌─────────────┴─────────────┐                           │
│  │    database_t / path_t    │                           │
│  │    identifier_t / etc.    │                           │
│  └───────────────────────────┘                           │
└─────────────────────────────────────────────────────────┘
```

---

## FFI Bindings Layer

### Type Definitions

```dart
// lib/src/native/types.dart
import 'dart:ffi';

/// Opaque database handle
class database_t extends Opaque {}

/// Opaque path handle  
class path_t extends Opaque {}

/// Opaque identifier handle
class identifier_t extends Opaque {}

/// Opaque iterator handle
class database_iterator_t extends Opaque {}

/// Reference counter (first field of refcounted structs)
class refcounter_t extends Struct {
  @Uint32()
  external int count;
}
```

### Function Bindings

```dart
// lib/src/native/wavedb_bindings.dart
import 'dart:ffi';
import 'dart:io';
import 'types.dart';

typedef DatabaseCreateC = Pointer<database_t> Function(
  Pointer<Utf8> path,
  Uint32 chunk_size,
  Pointer<Void> options,
  Uint32 btree_node_size,
  Uint32 wal_enabled,
  Uint32 snapshot_enabled,
  Pointer<Void> callback,
  Pointer<Int32> error_code,
);
typedef DatabaseCreate = Pointer<database_t> Function(
  Pointer<Utf8> path,
  int chunk_size,
  Pointer<Void> options,
  int btree_node_size,
  int wal_enabled,
  int snapshot_enabled,
  Pointer<Void> callback,
  Pointer<Int32> error_code,
);

typedef DatabaseDestroyC = Void Function(Pointer<database_t> db);
typedef DatabaseDestroy = void Function(Pointer<database_t> db);

typedef DatabasePutSyncC = Int32 Function(
  Pointer<database_t> db,
  Pointer<path_t> path,
  Pointer<identifier_t> value,
);
typedef DatabasePutSync = int Function(
  Pointer<database_t> db,
  Pointer<path_t> path,
  Pointer<identifier_t> value,
);

typedef DatabaseGetSyncC = Int32 Function(
  Pointer<database_t> db,
  Pointer<path_t> path,
  Pointer<Pointer<identifier_t>> result,
);
typedef DatabaseGetSync = int Function(
  Pointer<database_t> db,
  Pointer<path_t> path,
  Pointer<Pointer<identifier_t>> result,
);

typedef DatabaseDeleteSyncC = Int32 Function(
  Pointer<database_t> db,
  Pointer<path_t> path,
);
typedef DatabaseDeleteSync = int Function(
  Pointer<database_t> db,
  Pointer<path_t> path,
);

typedef PathCreateC = Pointer<path_t> Function();
typedef PathCreate = Pointer<path_t> Function();

typedef PathAppendC = Int32 Function(
  Pointer<path_t> path,
  Pointer<identifier_t> identifier,
);
typedef PathAppend = int Function(
  Pointer<path_t> path,
  Pointer<identifier_t> identifier,
);

typedef PathDestroyC = Void Function(Pointer<path_t> path);
typedef PathDestroy = void Function(Pointer<path_t> path);

typedef IdentifierCreateC = Pointer<identifier_t> Function(
  Pointer<Uint8> data,
  Uint32 length,
);
typedef IdentifierCreate = Pointer<identifier_t> Function(
  Pointer<Uint8> data,
  int length,
);

typedef IdentifierDestroyC = Void Function(Pointer<identifier_t> id);
typedef IdentifierDestroy = void Function(Pointer<identifier_t> id);

// Iterator functions
typedef DatabaseScanStartC = Pointer<database_iterator_t> Function(
  Pointer<database_t> db,
  Pointer<path_t> start_path,
  Pointer<path_t> end_path,
);
typedef DatabaseScanStart = Pointer<database_iterator_t> Function(
  Pointer<database_t> db,
  Pointer<path_t> start_path,
  Pointer<path_t> end_path,
);

typedef DatabaseScanNextC = Int32 Function(
  Pointer<database_iterator_t> iter,
  Pointer<Pointer<path_t>> out_path,
  Pointer<Pointer<identifier_t>> out_value,
);
typedef DatabaseScanNext = int Function(
  Pointer<database_iterator_t> iter,
  Pointer<Pointer<path_t>> out_path,
  Pointer<Pointer<identifier_t>> out_value,
);

typedef DatabaseScanEndC = Void Function(Pointer<database_iterator_t> iter);
typedef DatabaseScanEnd = void Function(Pointer<database_iterator_t> iter);

/// FFI bindings wrapper
class WaveDBNative {
  static DynamicLibrary? _lib;
  
  static DynamicLibrary _openLibrary() {
    if (_lib != null) return _lib!;
    
    if (Platform.isLinux) {
      _lib = DynamicLibrary.open('libwavedb.so');
    } else if (Platform.isMacOS) {
      _lib = DynamicLibrary.open('libwavedb.dylib');
    } else if (Platform.isWindows) {
      _lib = DynamicLibrary.open('wavedb.dll');
    } else {
      throw UnsupportedError('Unsupported platform');
    }
    return _lib!;
  }

  // Lazy-loaded function pointers
  static late final DatabaseCreate _databaseCreate = 
      _openLibrary().lookupFunction<DatabaseCreateC, DatabaseCreate>('database_create');
  static late final DatabaseDestroy _databaseDestroy = 
      _openLibrary().lookupFunction<DatabaseDestroyC, DatabaseDestroy>('database_destroy');
  static late final DatabasePutSync _databasePutSync = 
      _openLibrary().lookupFunction<DatabasePutSyncC, DatabasePutSync>('database_put_sync');
  static late final DatabaseGetSync _databaseGetSync = 
      _openLibrary().lookupFunction<DatabaseGetSyncC, DatabaseGetSync>('database_get_sync');
  static late final DatabaseDeleteSync _databaseDeleteSync = 
      _openLibrary().lookupFunction<DatabaseDeleteSyncC, DatabaseDeleteSync>('database_delete_sync');
  static late final PathCreate _pathCreate = 
      _openLibrary().lookupFunction<PathCreateC, PathCreate>('path_create');
  static late final PathAppend _pathAppend = 
      _openLibrary().lookupFunction<PathAppendC, PathAppend>('path_append');
  static late final PathDestroy _pathDestroy = 
      _openLibrary().lookupFunction<PathDestroyC, PathDestroy>('path_destroy');
  static late final IdentifierCreate _identifierCreate = 
      _openLibrary().lookupFunction<IdentifierCreateC, IdentifierCreate>('identifier_create');
  static late final IdentifierDestroy _identifierDestroy = 
      _openLibrary().lookupFunction<IdentifierDestroyC, IdentifierDestroy>('identifier_destroy');

  // Public methods delegate to native functions
  static Pointer<database_t> databaseCreate(String path) {
    final pathPtr = path.toNativeUtf8();
    final errorPtr = calloc<Int32>();
    try {
      final db = _databaseCreate(
        pathPtr.cast(),
        0,        // default chunk_size
        nullptr,  // options
        0,        // default btree_node_size  
        0,        // default wal_enabled
        1,        // snapshot_enabled
        nullptr,  // callback
        errorPtr,
      );
      if (db == nullptr) {
        throw WaveDBException('DATABASE_ERROR', 'Failed to create database');
      }
      return db;
    } finally {
      calloc.free(pathPtr);
      calloc.free(errorPtr);
    }
  }

  static void databaseDestroy(Pointer<database_t> db) {
    _databaseDestroy(db);
  }

  static int databasePutSync(Pointer<database_t> db, Pointer<path_t> path, Pointer<identifier_t> value) {
    return _databasePutSync(db, path, value);
  }

  static int databaseGetSync(Pointer<database_t> db, Pointer<path_t> path, Pointer<Pointer<identifier_t>> result) {
    return _databaseGetSync(db, path, result);
  }

  static int databaseDeleteSync(Pointer<database_t> db, Pointer<path_t> path) {
    return _databaseDeleteSync(db, path);
  }

  static Pointer<path_t> pathCreate() => _pathCreate();
  static int pathAppend(Pointer<path_t> path, Pointer<identifier_t> id) => _pathAppend(path, id);
  static void pathDestroy(Pointer<path_t> path) => _pathDestroy(path);
  static Pointer<identifier_t> identifierCreate(Pointer<Uint8> data, int length) => 
      _identifierCreate(data, length);
  static void identifierDestroy(Pointer<identifier_t> id) => _identifierDestroy(id);
}
```

---

## Public API

### Database Class

```dart
// lib/src/database.dart
import 'dart:async';
import 'dart:ffi';
import 'package:meta/meta.dart';

import 'native/types.dart';
import 'native/wavedb_bindings.dart';
import 'path.dart';
import 'identifier.dart';
import 'exceptions.dart';
import 'iterator.dart';
import 'object_ops.dart';

/// WaveDB database instance
class WaveDB {
  Pointer<database_t>? _db;
  final String _path;
  final String _delimiter;
  bool _isClosed = false;

  WaveDB(String path, {String delimiter = '/'})
      : _path = path,
        _delimiter = delimiter {
    _db = WaveDBNative.databaseCreate(path);
  }

  bool get isClosed => _isClosed;

  void _checkClosed() {
    if (_isClosed || _db == null) {
      throw WaveDBException('DATABASE_CLOSED', 'Database is closed');
    }
  }

  // ============================================================
  // ASYNC OPERATIONS
  // ============================================================

  Future<void> put(dynamic key, dynamic value) async {
    _checkClosed();
    if (value == null) {
      throw ArgumentError('Value is required for put operation');
    }
    return _runInIsolate(() => _putSyncInternal(key, value));
  }

  Future<dynamic> get(dynamic key) async {
    _checkClosed();
    return _runInIsolate(() => _getSyncInternal(key));
  }

  Future<void> del(dynamic key) async {
    _checkClosed();
    return _runInIsolate(() => _delSyncInternal(key));
  }

  Future<void> batch(List<Map<String, dynamic>> operations) async {
    _checkClosed();
    return _runInIsolate(() => _batchSyncInternal(operations));
  }

  Future<void> putObject(dynamic key, Map<String, dynamic> obj) async {
    _checkClosed();
    return _runInIsolate(() => _putObjectSyncInternal(key, obj));
  }

  Future<Map<String, dynamic>?> getObject(dynamic key) async {
    _checkClosed();
    return _runInIsolate(() => _getObjectSyncInternal(key));
  }

  // ============================================================
  // SYNC OPERATIONS
  // ============================================================

  void putSync(dynamic key, dynamic value) {
    _checkClosed();
    _putSyncInternal(key, value);
  }

  dynamic getSync(dynamic key) {
    _checkClosed();
    return _getSyncInternal(key);
  }

  void delSync(dynamic key) {
    _checkClosed();
    _delSyncInternal(key);
  }

  void batchSync(List<Map<String, dynamic>> operations) {
    _checkClosed();
    _batchSyncInternal(operations);
  }

  void putObjectSync(dynamic key, Map<String, dynamic> obj) {
    _checkClosed();
    _putObjectSyncInternal(key, obj);
  }

  Map<String, dynamic>? getObjectSync(dynamic key) {
    _checkClosed();
    return _getObjectSyncInternal(key);
  }

  // ... internal implementations ...

  // ============================================================
  // STREAMING
  // ============================================================

  Stream<KeyValue> createReadStream({
    dynamic start,
    dynamic end,
    bool reverse = false,
    bool keys = true,
    bool values = true,
  }) {
    _checkClosed();
    return WaveDBIterator(
      db: _db!,
      delimiter: _delimiter,
      startPath: start,
      endPath: end,
      reverse: reverse,
      keys: keys,
      values: values,
    ).stream;
  }

  // ============================================================
  // LIFECYCLE
  // ============================================================

  void close() {
    if (_db != null && !_isClosed) {
      WaveDBNative.databaseDestroy(_db!);
      _db = null;
      _isClosed = true;
    }
  }

  Future<T> _runInIsolate<T>(T Function() operation) async {
    return operation();
  }
}

class KeyValue {
  final dynamic key;
  final dynamic value;
  KeyValue({this.key, this.value});
}
```

---

## Key/Value Conversion

### Path Conversion

```dart
// lib/src/path.dart
import 'dart:ffi';
import 'native/types.dart';
import 'native/wavedb_bindings.dart';
import 'identifier.dart';

class PathConverter {
  static Pointer<path_t> toNative(dynamic key, String delimiter) {
    final path = WaveDBNative.pathCreate();
    
    try {
      List<String> parts;
      
      if (key is String) {
        parts = key.split(delimiter).where((s) => s.isNotEmpty).toList();
      } else if (key is List) {
        parts = key.map((e) => e.toString()).toList();
      } else {
        throw ArgumentError('Key must be String or List<String>');
      }
      
      if (parts.isEmpty) {
        throw ArgumentError('Key cannot be empty');
      }
      
      for (final part in parts) {
        final partPtr = part.toNativeUtf8();
        try {
          final id = IdentifierConverter.fromDataPointer(
            partPtr.cast(),
            part.length,
          );
          try {
            final rc = WaveDBNative.pathAppend(path, id);
            if (rc != 0) {
              throw WaveDBException('INVALID_PATH', 'Failed to append path component');
            }
          } finally {
            // pathAppend takes ownership of identifier
          }
        } finally {
          calloc.free(partPtr);
        }
      }
      
      return path;
    } catch (e) {
      WaveDBNative.pathDestroy(path);
      rethrow;
    }
  }

  static dynamic fromNative(Pointer<path_t> path, String delimiter, {bool asArray = false}) {
    // TODO: Implement proper path reconstruction
    final parts = <String>[];
    if (asArray) {
      return parts;
    } else {
      return parts.join(delimiter);
    }
  }
}
```

### Identifier Conversion

```dart
// lib/src/identifier.dart
import 'dart:ffi';
import 'dart:typed_data';
import 'native/types.dart';
import 'native/wavedb_bindings.dart';

class IdentifierConverter {
  static Pointer<identifier_t> toNative(dynamic value) {
    if (value is String) {
      final utf8Bytes = value.toNativeUtf8();
      try {
        return fromDataPointer(utf8Bytes.cast(), value.length);
      } finally {
        // identifier_create should copy the data
      }
    } else if (value is Uint8List) {
      final ptr = calloc<Uint8>(value.length);
      try {
        ptr.asTypedList(value.length).setAll(0, value);
        return fromDataPointer(ptr.cast(), value.length);
      } finally {
        // Cleanup handled by caller
      }
    } else if (value is List<int>) {
      final bytes = Uint8List.fromList(value);
      final ptr = calloc<Uint8>(bytes.length);
      try {
        ptr.asTypedList(bytes.length).setAll(0, bytes);
        return fromDataPointer(ptr.cast(), bytes.length);
      } finally {
        // Cleanup handled by caller
      }
    } else {
      throw ArgumentError('Value must be String, Uint8List, or List<int>');
    }
  }

  static Pointer<identifier_t> fromDataPointer(Pointer<Uint8> data, int length) {
    return WaveDBNative.identifierCreate(data, length);
  }

  static dynamic fromNative(Pointer<identifier_t> id) {
    if (id == nullptr) return null;
    
    final bytes = _readIdentifierBytes(id);
    
    if (_isPrintableUTF8(bytes)) {
      return String.fromCharCodes(bytes);
    } else {
      return Uint8List.fromList(bytes);
    }
  }

  static List<int> _readIdentifierBytes(Pointer<identifier_t> id) {
    // TODO: Implement chunk reading via FFI
    return [];
  }

  static bool _isPrintableUTF8(List<int> bytes) {
    if (bytes.isEmpty) return true;
    try {
      final str = String.fromCharCodes(bytes);
      return str.codeUnits.every((c) => c >= 32 && c < 127);
    } catch (e) {
      return false;
    }
  }
}
```

---

## Object Operations (Iterative)

```dart
// lib/src/object_ops.dart
import 'dart:typed_data';

class ObjectOps {
  static List<Map<String, dynamic>> flattenObject(
    dynamic key,
    Map<String, dynamic> obj,
    String delimiter,
  ) {
    final operations = <Map<String, dynamic>>[];
    final basePath = <String>[];
    
    if (key != null) {
      if (key is String) {
        basePath.addAll(key.split(delimiter).where((s) => s.isNotEmpty));
      } else if (key is List) {
        basePath.addAll(key.map((e) => e.toString()));
      }
    }
    
    // Stack entries: (object/value, current path)
    final stack = <MapEntry<dynamic, List<String>>>[
      MapEntry(obj, List<String>.from(basePath)),
    ];
    
    while (stack.isNotEmpty) {
      final entry = stack.removeLast();
      final value = entry.key;
      final pathParts = entry.value;
      
      if (value is Map<String, dynamic> || value is Map) {
        final map = value as Map;
        final keys = value.keys.toList().reversed;
        
        for (final k in keys) {
          final newPath = List<String>.from(pathParts);
          newPath.add(k.toString());
          stack.add(MapEntry(map[k], newPath));
        }
      } else if (value is List) {
        for (var i = value.length - 1; i >= 0; i--) {
          final newPath = List<String>.from(pathParts);
          newPath.add(i.toString());
          stack.add(MapEntry(value[i], newPath));
        }
      } else {
        operations.add({
          'type': 'put',
          'key': pathParts,
          'value': _convertLeafValue(value),
        });
      }
    }
    
    return operations;
  }

  static dynamic _convertLeafValue(dynamic value) {
    if (value == null) return '';
    if (value is String || value is Uint8List || value is List<int>) return value;
    return value.toString();
  }

  static Map<String, dynamic> reconstructObject(
    List<MapEntry<List<String>, dynamic>> entries,
    List<String> basePath,
  ) {
    final result = <String, dynamic>{};
    
    for (final entry in entries) {
      final relativePath = _getRelativePath(entry.key, basePath);
      if (relativePath.isEmpty) continue;
      
      var current = result;
      
      for (var i = 0; i < relativePath.length - 1; i++) {
        final key = relativePath[i];
        if (!current.containsKey(key)) {
          current[key] = <String, dynamic>{};
        }
        current = current[key] as Map<String, dynamic>;
      }
      
      current[relativePath.last] = entry.value;
    }
    
    return _convertArrays(result) as Map<String, dynamic>;
  }

  static List<String> _getRelativePath(List<String> path, List<String> basePath) {
    if (basePath.isEmpty) return path;
    if (path.length < basePath.length) return [];
    
    for (var i = 0; i < basePath.length; i++) {
      if (path[i] != basePath[i]) return [];
    }
    
    return path.sublist(basePath.length);
  }

  static dynamic _convertArrays(dynamic obj) {
    if (obj is! Map<String, dynamic>) return obj;
    
    final keys = obj.keys.toList();
    if (_isContiguousNumericKeys(keys)) {
      final indices = keys.map(int.parse).toList()..sort();
      final arr = <dynamic>[];
      
      for (final idx in indices) {
        arr.add(_convertArrays(obj[idx.toString()]));
      }
      return arr;
    }
    
    final result = <String, dynamic>{};
    obj.forEach((key, value) {
      result[key] = _convertArrays(value);
    });
    
    return result;
  }

  static bool _isContiguousNumericKeys(List<String> keys) {
    if (keys.isEmpty) return false;
    
    final indices = <int>[];
    for (final key in keys) {
      final num = int.tryParse(key);
      if (num == null) return false;
      indices.add(num);
    }
    
    indices.sort();
    return indices.first == 0 && indices.last == indices.length - 1;
  }
}
```

---

## Streaming

```dart
// lib/src/iterator.dart
import 'dart:async';
import 'dart:ffi';
import 'native/types.dart';
import 'native/wavedb_bindings.dart';
import 'path.dart';
import 'identifier.dart';
import 'exceptions.dart';

class WaveDBIterator {
  final Pointer<database_t> _db;
  final String _delimiter;
  final dynamic _startPath;
  final dynamic _endPath;
  final bool _reverse;
  final bool _keys;
  final bool _values;

  Pointer<database_iterator_t>? _nativeIterator;
  bool _started = false;
  bool _done = false;

  WaveDBIterator({
    required Pointer<database_t> db,
    required String delimiter,
    dynamic startPath,
    dynamic endPath,
    bool reverse = false,
    bool keys = true,
    bool values = true,
  })  : _db = db,
        _delimiter = delimiter,
        _startPath = startPath,
        _endPath = endPath,
        _reverse = reverse,
        _keys = keys,
        _values = values;

  Stream<KeyValue> get stream => _createStream();

  Stream<KeyValue> _createStream() async* {
    try {
      _startIterator();
      
      while (!_done) {
        final entry = _readNext();
        if (entry == null) {
          _done = true;
          break;
        }
        
        yield KeyValue(
          key: _keys ? entry.key : null,
          value: _values ? entry.value : null,
        );
      }
    } finally {
      _endIterator();
    }
  }

  void _startIterator() {
    if (_started) return;
    _started = true;

    Pointer<path_t>? startNative;
    Pointer<path_t>? endNative;

    try {
      if (_startPath != null) {
        startNative = PathConverter.toNative(_startPath, _delimiter);
      }
      if (_endPath != null) {
        endNative = PathConverter.toNative(_endPath, _delimiter);
      }

      _nativeIterator = WaveDBNative.databaseScanStart(
        _db,
        startNative ?? nullptr,
        endNative ?? nullptr,
      );

      if (_nativeIterator == nullptr) {
        throw WaveDBException('IO_ERROR', 'Failed to create iterator');
      }
    } finally {
      // Scan start takes ownership of paths
    }
  }

  KeyValue? _readNext() {
    if (_nativeIterator == null || _nativeIterator == nullptr) {
      return null;
    }

    final pathPtr = calloc<Pointer<path_t>>();
    final valuePtr = calloc<Pointer<identifier_t>>();

    try {
      final rc = WaveDBNative.databaseScanNext(
        _nativeIterator!,
        pathPtr,
        valuePtr,
      );

      if (rc != 0) {
        _done = true;
        return null;
      }

      final path = pathPtr.value;
      final value = valuePtr.value;

      try {
        final keyResult = PathConverter.fromNative(path, _delimiter);
        final valueResult = IdentifierConverter.fromNative(value);

        return KeyValue(key: keyResult, value: valueResult);
      } finally {
        WaveDBNative.pathDestroy(path);
        WaveDBNative.identifierDestroy(value);
      }
    } finally {
      calloc.free(pathPtr);
      calloc.free(valuePtr);
    }
  }

  void _endIterator() {
    if (_nativeIterator != null && _nativeIterator != nullptr) {
      WaveDBNative.databaseScanEnd(_nativeIterator!);
      _nativeIterator = null;
    }
  }
}

class KeyValue {
  final dynamic key;
  final dynamic value;
  KeyValue({this.key, this.value});
}
```

---

## Error Handling

```dart
// lib/src/exceptions.dart

class WaveDBException implements Exception {
  final String code;
  final String message;

  WaveDBException(this.code, this.message);

  @override
  String toString() => 'WaveDBException($code): $message';

  static const String notFound = 'NOT_FOUND';
  static const String invalidPath = 'INVALID_PATH';
  static const String ioError = 'IO_ERROR';
  static const String databaseClosed = 'DATABASE_CLOSED';
  static const String invalidArgument = 'INVALID_ARGUMENT';
  static const String notSupported = 'NOT_SUPPORTED';
  static const String corruption = 'CORRUPTION';
  static const String conflict = 'CONFLICT';

  factory WaveDBException.notFound([String? key]) {
    return WaveDBException(notFound, key != null ? 'Key not found: $key' : 'Key not found');
  }

  factory WaveDBException.invalidPath(String path, [String? reason]) {
    return WaveDBException(
      invalidPath,
      reason != null ? 'Invalid path "$path": $reason' : 'Invalid path: $path',
    );
  }

  factory WaveDBException.ioError(String operation, [String? details]) {
    return WaveDBException(
      ioError,
      details != null ? '$operation failed: $details' : '$operation failed',
    );
  }

  factory WaveDBException.databaseClosed() {
    return WaveDBException(databaseClosed, 'Database is closed');
  }

  factory WaveDBException.invalidArgument(String message) {
    return WaveDBException(invalidArgument, message);
  }

  factory WaveDBException.notSupported(String operation) {
    return WaveDBException(notSupported, 'Operation not supported: $operation');
  }
}

class ErrorConverter {
  static WaveDBException? fromReturnCode(int rc, {String? context}) {
    switch (rc) {
      case 0:
        return null;
      case -2:
        return WaveDBException.notFound(context);
      case -1:
        return WaveDBException.ioError(context ?? 'Unknown operation');
      default:
        return WaveDBException('UNKNOWN', 'Unknown error code: $rc');
    }
  }

  static WaveDBException fromMessage(String message) {
    final upperMessage = message.toUpperCase();
    
    if (upperMessage.contains('NOT_FOUND') || upperMessage.contains('KEY NOT FOUND')) {
      return WaveDBException.notFound();
    }
    if (upperMessage.contains('INVALID_PATH') || upperMessage.contains('INVALID KEY')) {
      return WaveDBException.invalidPath(message);
    }
    if (upperMessage.contains('IO_ERROR') || upperMessage.contains('I/O ERROR')) {
      return WaveDBException.ioError('Operation', message);
    }
    if (upperMessage.contains('DATABASE_CLOSED')) {
      return WaveDBException.databaseClosed();
    }
    
    return WaveDBException('UNKNOWN', message);
  }
}
```

---

## Platform Support

```dart
// lib/src/native/wavedb_library.dart
import 'dart:io';
import 'dart:ffi';
import '../exceptions.dart';

class WaveDBLibrary {
  static DynamicLibrary? _lib;
  static String? _libPath;

  static void setLibraryPath(String path) {
    _libPath = path;
    _lib = null;
  }

  static DynamicLibrary load() {
    if (_lib != null) return _lib!;

    if (_libPath != null) {
      _lib = DynamicLibrary.open(_libPath!);
      return _lib!;
    }

    if (Platform.isLinux) {
      final paths = [
        'libwavedb.so',
        './libwavedb.so',
        '/usr/local/lib/libwavedb.so',
        '/usr/lib/libwavedb.so',
      ];
      for (final path in paths) {
        try {
          _lib = DynamicLibrary.open(path);
          return _lib!;
        } catch (_) {}
      }
      throw WaveDBException('LIBRARY_NOT_FOUND', 
        'libwavedb.so not found. Tried: ${paths.join(", ")}');
    }
    
    if (Platform.isMacOS) {
      final paths = [
        'libwavedb.dylib',
        './libwavedb.dylib',
        '/usr/local/lib/libwavedb.dylib',
      ];
      for (final path in paths) {
        try {
          _lib = DynamicLibrary.open(path);
          return _lib!;
        } catch (_) {}
      }
      throw WaveDBException('LIBRARY_NOT_FOUND',
        'libwavedb.dylib not found. Tried: ${paths.join(", ")}');
    }
    
    if (Platform.isWindows) {
      final paths = [
        'wavedb.dll',
        '.\\wavedb.dll',
      ];
      for (final path in paths) {
        try {
          _lib = DynamicLibrary.open(path);
          return _lib!;
        } catch (_) {}
      }
      throw WaveDBException('LIBRARY_NOT_FOUND',
        'wavedb.dll not found. Tried: ${paths.join(", ")}');
    }
    
    throw WaveDBException('UNSUPPORTED_PLATFORM',
      'WaveDB is not supported on ${Platform.operatingSystem}');
  }
}
```

---

## Build Configuration

```yaml
# pubspec.yaml
name: wavedb
version: 0.1.0
description: Dart bindings for WaveDB - hierarchical key-value database

environment:
  sdk: '>=3.0.0 <4.0.0'

dependencies:
  ffi: ^2.1.0
  meta: ^1.9.0

dev_dependencies:
  test: ^1.24.0
  lints: ^3.0.0

platforms:
  linux:
  macos:
  windows:
```

```dart
// lib/wavedb.dart
library wavedb;

export 'src/exceptions.dart';
export 'src/database.dart';
export 'src/iterator.dart' show KeyValue;
```

---

## Implementation Checklist

1. **Core Infrastructure**
   - [ ] FFI type definitions (types.dart)
   - [ ] FFI function bindings (wavedb_bindings.dart)
   - [ ] Platform library loader (wavedb_library.dart)
   - [ ] WaveDBException class (exceptions.dart)

2. **Conversion Layer**
   - [ ] PathConverter (path.dart)
   - [ ] IdentifierConverter (identifier.dart)
   - [ ] ObjectOps flatten/reconstruct (object_ops.dart)

3. **Public API**
   - [ ] WaveDB class async methods (database.dart)
   - [ ] WaveDB class sync methods (database.dart)
   - [ ] putObject/getObject (database.dart)

4. **Streaming**
   - [ ] WaveDBIterator stream implementation (iterator.dart)
   - [ ] Range scanning support
   - [ ] Keys-only and values-only options

5. **Testing**
   - [ ] Core operations tests (wavedb_test.dart)
   - [ ] Object operations tests (object_ops_test.dart)
   - [ ] Stream tests (stream_test.dart)
   - [ ] Error handling tests

6. **Documentation**
   - [ ] README.md with usage examples
   - [ ] API reference documentation
   - [ ] Build instructions

---

## Dependencies

**Native Dependencies:**
- WaveDB C library (`libwavedb.so` / `libwavedb.dylib` / `wavedb.dll`)

**Dart Dependencies:**
- `ffi: ^2.1.0` - FFI support
- `meta: ^1.9.0` - Annotations

**Dev Dependencies:**
- `test: ^1.24.0` - Testing framework
- `lints: ^3.0.0` - Linting rules

---

## Platform Support

| Platform | Library | Status |
|----------|---------|--------|
| Linux | libwavedb.so | Supported |
| macOS | libwavedb.dylib | Supported |
| Windows | wavedb.dll | Supported |
| Android | Bundled via Flutter | Supported |
| iOS | Bundled via Flutter | Supported |

---

## License

GNU General Public License v3.0 or later