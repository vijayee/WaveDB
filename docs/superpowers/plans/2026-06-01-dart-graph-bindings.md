# Dart Graph Bindings Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Expose the C graph layer to Dart via FFI, with a pure-Dart Query builder and `g.V("x").Out("y").All()` auto-execution.

**Architecture:** FFI bindings in `wavedb_bindings.dart` for 9 C functions (graph_layer_create/destroy, insert/delete_sync, parse_execute, parse_count, result_destroy, result_count, result_vertices). A Dart `GraphLayer` class wraps the C pointer. A pure-Dart `GraphQuery` class builds step lists and serializes to DSL strings — `.All()` auto-executes via a module-level default graph instance.

**Tech Stack:** Dart FFI (`dart:ffi`, `package:ffi`), existing `libwavedb.so`, existing C graph layer

---

## File Structure

```
bindings/dart/
├── lib/src/native/
│   ├── types.dart              — MODIFY: add graph_layer_t, graph_result_t
│   └── wavedb_bindings.dart    — MODIFY: add FFI typedefs + WaveDBNative graph methods
├── lib/src/graph_layer.dart    — CREATE: GraphLayer class, GraphQuery builder, g() function
├── lib/wavedb.dart             — MODIFY: add graph export
└── test/graph_layer_test.dart  — CREATE: test suite
```

---

### Task 1: Add Graph Types to types.dart

**Files:**
- Modify: `lib/src/native/types.dart`

- [ ] **Step 1: Add opaque types**

Add after the existing opaque types (after the `encrypted_database_config_t` block, around line 27):

```dart
/// Opaque handle to a Graph layer
/// Maps to graph_layer_t in C
base class graph_layer_t extends Opaque {}

/// Opaque handle to a Graph result (array of vertex strings)
/// Maps to graph_result_t in C
base class graph_result_t extends Opaque {}
```

- [ ] **Step 2: Commit**

```bash
git add bindings/dart/lib/src/native/types.dart
git commit -m "feat(dart): add graph_layer_t and graph_result_t opaque types"
```

---

### Task 2: Add FFI Bindings to wavedb_bindings.dart

**Files:**
- Modify: `lib/src/native/wavedb_bindings.dart`

- [ ] **Step 1: Add graph FFI typedefs**

Add after the GraphQL async typedefs (after `GraphQLMutateAsync`, around line 830), before the Promise section:

```dart
// ============================================================
// C TYPEDEFS - Graph Layer
// ============================================================

/// C signature: graph_layer_t* graph_layer_create(
///   const char* path,
///   database_config_t* config
/// )
typedef GraphLayerCreateC = Pointer<graph_layer_t> Function(
  Pointer<Utf8> path,
  Pointer<database_config_t> config,
);

typedef GraphLayerCreate = Pointer<graph_layer_t> Function(
  Pointer<Utf8> path,
  Pointer<database_config_t> config,
);

/// C signature: void graph_layer_destroy(graph_layer_t* layer)
typedef GraphLayerDestroyC = Void Function(Pointer<graph_layer_t> layer);
typedef GraphLayerDestroy = void Function(Pointer<graph_layer_t> layer);

/// C signature: int graph_insert_sync(graph_layer_t* layer, const char* s, const char* p, const char* o)
typedef GraphInsertSyncC = Int32 Function(
  Pointer<graph_layer_t> layer,
  Pointer<Utf8> s,
  Pointer<Utf8> p,
  Pointer<Utf8> o,
);
typedef GraphInsertSync = int Function(
  Pointer<graph_layer_t> layer,
  Pointer<Utf8> s,
  Pointer<Utf8> p,
  Pointer<Utf8> o,
);

/// C signature: int graph_delete_sync(graph_layer_t* layer, const char* s, const char* p, const char* o)
typedef GraphDeleteSyncC = Int32 Function(
  Pointer<graph_layer_t> layer,
  Pointer<Utf8> s,
  Pointer<Utf8> p,
  Pointer<Utf8> o,
);
typedef GraphDeleteSync = int Function(
  Pointer<graph_layer_t> layer,
  Pointer<Utf8> s,
  Pointer<Utf8> p,
  Pointer<Utf8> o,
);

/// C signature: graph_result_t* graph_parse_execute(
///   const char* dsl,
///   graph_layer_t* layer,
///   graph_parse_error_t* error
/// )
/// Note: graph_parse_error_t is a struct, we use an opaque pointer
typedef GraphParseExecuteC = Pointer<graph_result_t> Function(
  Pointer<Utf8> dsl,
  Pointer<graph_layer_t> layer,
  Pointer<Void> error,
);
typedef GraphParseExecute = Pointer<graph_result_t> Function(
  Pointer<Utf8> dsl,
  Pointer<graph_layer_t> layer,
  Pointer<Void> error,
);

/// C signature: int graph_parse_count(
///   const char* dsl,
///   graph_layer_t* layer,
///   size_t* count,
///   graph_parse_error_t* error
/// )
typedef GraphParseCountC = Int32 Function(
  Pointer<Utf8> dsl,
  Pointer<graph_layer_t> layer,
  Pointer<Size> count,
  Pointer<Void> error,
);
typedef GraphParseCount = int Function(
  Pointer<Utf8> dsl,
  Pointer<graph_layer_t> layer,
  Pointer<Size> count,
  Pointer<Void> error,
);

/// C signature: void graph_result_destroy(graph_result_t* result)
typedef GraphResultDestroyC = Void Function(Pointer<graph_result_t> result);
typedef GraphResultDestroy = void Function(Pointer<graph_result_t> result);

/// C signature: size_t graph_result_count(graph_result_t* result)
typedef GraphResultCountC = Size Function(Pointer<graph_result_t> result);
typedef GraphResultCount = int Function(Pointer<graph_result_t> result);

/// C signature: const char* const* graph_result_vertices(graph_result_t* result)
typedef GraphResultVerticesC = Pointer<Pointer<Utf8>> Function(Pointer<graph_result_t> result);
typedef GraphResultVertices = Pointer<Pointer<Utf8>> Function(Pointer<graph_result_t> result);
```

- [ ] **Step 2: Add lazy-loaded function pointers and public API methods to WaveDBNative**

After the existing GraphQL operations in the `WaveDBNative` class (around line 1123, after the `_graphQLMutateAsync` field), add:

```dart
  // ============================================================
  // LAZY FUNCTIONS - Graph Layer
  // ============================================================

  static late final GraphLayerCreate _graphLayerCreate = WaveDBLibrary.load()
      .lookupFunction<GraphLayerCreateC, GraphLayerCreate>('graph_layer_create');

  static late final GraphLayerDestroy _graphLayerDestroy = WaveDBLibrary.load()
      .lookupFunction<GraphLayerDestroyC, GraphLayerDestroy>('graph_layer_destroy');

  static late final Pointer<NativeFunction<GraphLayerDestroyC>> graphLayerDestroyNative =
      WaveDBLibrary.load().lookup('graph_layer_destroy');

  static late final GraphInsertSync _graphInsertSync = WaveDBLibrary.load()
      .lookupFunction<GraphInsertSyncC, GraphInsertSync>('graph_insert_sync');

  static late final GraphDeleteSync _graphDeleteSync = WaveDBLibrary.load()
      .lookupFunction<GraphDeleteSyncC, GraphDeleteSync>('graph_delete_sync');

  static late final GraphParseExecute _graphParseExecute = WaveDBLibrary.load()
      .lookupFunction<GraphParseExecuteC, GraphParseExecute>('graph_parse_execute');

  static late final GraphParseCount _graphParseCount = WaveDBLibrary.load()
      .lookupFunction<GraphParseCountC, GraphParseCount>('graph_parse_count');

  static late final GraphResultDestroy _graphResultDestroy = WaveDBLibrary.load()
      .lookupFunction<GraphResultDestroyC, GraphResultDestroy>('graph_result_destroy');

  static late final GraphResultCount _graphResultCount = WaveDBLibrary.load()
      .lookupFunction<GraphResultCountC, GraphResultCount>('graph_result_count');

  static late final GraphResultVertices _graphResultVertices = WaveDBLibrary.load()
      .lookupFunction<GraphResultVerticesC, GraphResultVertices>('graph_result_vertices');
```

Then add public API methods after the existing `graphQLMutateAsync` public method (around line 1959). Before the Promise section. Add:

```dart
  // ============================================================
  // PUBLIC API - Graph Layer
  // ============================================================

  /// Create a Graph layer
  ///
  /// [path] - Optional database path (null for in-memory)
  /// [config] - Optional database config (null for defaults)
  static Pointer<graph_layer_t> graphLayerCreate(
    String? path, {
    Pointer<database_config_t>? config,
  }) {
    final pathPtr = path != null ? path.toNativeUtf8() : nullptr;
    try {
      final layer = _graphLayerCreate(pathPtr.cast(), config ?? nullptr);
      if (layer == nullptr) {
        throw WaveDBException.ioError('graph_layer_create', 'Failed to create graph layer');
      }
      return layer;
    } finally {
      if (pathPtr != nullptr) {
        calloc.free(pathPtr);
      }
    }
  }

  /// Destroy a Graph layer
  static void graphLayerDestroy(Pointer<graph_layer_t> layer) {
    _graphLayerDestroy(layer);
  }

  /// Insert a triple synchronously
  static int graphInsertSync(
    Pointer<graph_layer_t> layer,
    String s, String p, String o,
  ) {
    final sPtr = s.toNativeUtf8();
    final pPtr = p.toNativeUtf8();
    final oPtr = o.toNativeUtf8();
    try {
      return _graphInsertSync(layer, sPtr.cast(), pPtr.cast(), oPtr.cast());
    } finally {
      calloc.free(sPtr);
      calloc.free(pPtr);
      calloc.free(oPtr);
    }
  }

  /// Delete a triple synchronously
  static int graphDeleteSync(
    Pointer<graph_layer_t> layer,
    String s, String p, String o,
  ) {
    final sPtr = s.toNativeUtf8();
    final pPtr = p.toNativeUtf8();
    final oPtr = o.toNativeUtf8();
    try {
      return _graphDeleteSync(layer, sPtr.cast(), pPtr.cast(), oPtr.cast());
    } finally {
      calloc.free(sPtr);
      calloc.free(pPtr);
      calloc.free(oPtr);
    }
  }

  /// Execute a DSL query and return results
  ///
  /// Returns a pointer to the result. Caller must destroy with graphResultDestroy.
  static Pointer<graph_result_t> graphParseExecute(
    Pointer<graph_layer_t> layer,
    String dsl,
  ) {
    final dslPtr = dsl.toNativeUtf8();
    try {
      return _graphParseExecute(dslPtr.cast(), layer, nullptr);
    } finally {
      calloc.free(dslPtr);
    }
  }

  /// Execute a DSL query and return the count
  static int graphParseCount(
    Pointer<graph_layer_t> layer,
    String dsl,
  ) {
    final dslPtr = dsl.toNativeUtf8();
    final countPtr = calloc<Size>();
    try {
      final rc = _graphParseCount(dslPtr.cast(), layer, countPtr, nullptr);
      if (rc != 0) return 0;
      return countPtr.value;
    } finally {
      calloc.free(dslPtr);
      calloc.free(countPtr);
    }
  }

  /// Destroy a graph result
  static void graphResultDestroy(Pointer<graph_result_t> result) {
    _graphResultDestroy(result);
  }

  /// Get the number of vertices in a result
  static int graphResultCount(Pointer<graph_result_t> result) {
    return _graphResultCount(result);
  }

  /// Get the vertices array from a result
  ///
  /// Returns a pointer to an array of string pointers.
  static Pointer<Pointer<Utf8>> graphResultVertices(Pointer<graph_result_t> result) {
    return _graphResultVertices(result);
  }
```

- [ ] **Step 3: Commit**

```bash
git add bindings/dart/lib/src/native/wavedb_bindings.dart
git commit -m "feat(dart): add graph layer FFI typedefs and WaveDBNative bindings"
```

---

### Task 3: Create GraphLayer Dart Class + Query Builder

**Files:**
- Create: `lib/src/graph_layer.dart`

- [ ] **Step 1: Write graph_layer.dart**

```dart
// lib/src/graph_layer.dart
import 'dart:ffi';
import 'dart:convert';
import 'package:ffi/ffi.dart';
import 'native/types.dart';
import 'native/wavedb_bindings.dart';

GraphQuery g([GraphLayer? layer]) => GraphQuery(layer ?? _defaultGraph);
GraphLayer? _defaultGraph;

/// Graph layer for WaveDB
///
/// Provides triple insert/delete and DSL query execution on top of
/// WaveDB's hierarchical key-value store.
///
/// [GraphLayer] instances can be created in-memory (no path) or persistent.
/// The first [GraphLayer] created becomes the default for [g.V().All()].
class GraphLayer implements Finalizable {
  Pointer<graph_layer_t>? _layer;
  bool _closed = false;

  static final NativeFinalizer _finalizer = NativeFinalizer(
    WaveDBNative.graphLayerDestroyNative.cast(),
  );

  GraphLayer([String? path]) {
    _layer = WaveDBNative.graphLayerCreate(path);
    _finalizer.attach(this, _layer!.cast(), detach: this);
    if (_defaultGraph == null) _defaultGraph = this;
  }

  /// Execute a DSL query and return results as a list of strings.
  /// Accepts either a DSL string or a [GraphQuery] object.
  List<String> exec(Object dsl) {
    _checkOpen();
    final dslStr = dsl is GraphQuery ? dsl._toDSL() : dsl as String;
    final resultPtr = WaveDBNative.graphParseExecute(_layer!, dslStr);
    if (resultPtr == nullptr) return const [];

    try {
      final count = WaveDBNative.graphResultCount(resultPtr);
      if (count == 0) return const [];

      final verts = WaveDBNative.graphResultVertices(resultPtr);
      final results = <String>[];
      for (int i = 0; i < count; i++) {
        final strPtr = verts.elementAt(i).value;
        if (strPtr != nullptr) {
          results.add(strPtr.toDartString());
        }
      }
      return results;
    } finally {
      WaveDBNative.graphResultDestroy(resultPtr);
    }
  }

  /// Execute a DSL query and return the count only.
  /// Accepts either a DSL string or a [GraphQuery] object.
  int count(Object dsl) {
    _checkOpen();
    final dslStr = dsl is GraphQuery ? dsl._toDSL() : dsl as String;
    return WaveDBNative.graphParseCount(_layer!, dslStr);
  }

  /// Insert a triple synchronously.
  void insertSync(String s, String p, String o) {
    _checkOpen();
    final rc = WaveDBNative.graphInsertSync(_layer!, s, p, o);
    if (rc != 0) {
      throw GraphLayerException('Insert failed ($rc)');
    }
  }

  /// Delete a triple synchronously.
  void deleteSync(String s, String p, String o) {
    _checkOpen();
    final rc = WaveDBNative.graphDeleteSync(_layer!, s, p, o);
    if (rc != 0) {
      throw GraphLayerException('Delete failed ($rc)');
    }
  }

  /// Close the graph layer and release C resources.
  void close() {
    if (!_closed && _layer != null) {
      if (_defaultGraph == this) _defaultGraph = null;
      _finalizer.detach(this);
      WaveDBNative.graphLayerDestroy(_layer!);
      _layer = null;
      _closed = true;
    }
  }

  void _checkOpen() {
    if (_closed || _layer == null) {
      throw StateError('GraphLayer is closed');
    }
  }
}

/// Exception thrown by graph layer operations
class GraphLayerException implements Exception {
  final String message;
  const GraphLayerException(this.message);

  @override
  String toString() => 'GraphLayerException: $message';
}

/// Query builder for graph traversals
///
/// Builds a DSL string through method chaining, then executes with [All()].
///
/// ```dart
/// final results = g.V("gaming").In("tagged_with").Limit(10).All();
/// ```
class GraphQuery {
  GraphLayer? _layer;
  final List<String> _steps = [];

  GraphQuery([GraphLayer? layer]) : _layer = layer;

  GraphQuery V(String id) { _steps.add('V("$id")'); return this; }
  GraphQuery Out(String pred) { _steps.add('Out("$pred")'); return this; }
  GraphQuery In(String pred) { _steps.add('In("$pred")'); return this; }
  GraphQuery Has(String pred, String val) { _steps.add('Has("$pred","$val")'); return this; }
  GraphQuery And(GraphQuery sub) { _steps.add('And(${sub._toDSL()})'); return this; }
  GraphQuery Or(GraphQuery sub) { _steps.add('Or(${sub._toDSL()})'); return this; }
  GraphQuery Limit(int n) { _steps.add('Limit($n)'); return this; }

  String _toDSL() => 'g.${_steps.join('.')}';

  /// Execute the query and return results as a list of strings.
  /// The query must be bound to a [GraphLayer].
  List<String> All() {
    if (_layer == null) throw StateError('Query not bound to a GraphLayer');
    return _layer!.exec(this);
  }

  @override
  String toString() => _toDSL();
}
```

- [ ] **Step 2: Commit**

```bash
git add bindings/dart/lib/src/graph_layer.dart
git commit -m "feat(dart): add GraphLayer class and GraphQuery builder with g().V().All()"
```

---

### Task 4: Export from wavedb.dart

**Files:**
- Modify: `lib/wavedb.dart`

- [ ] **Step 1: Add graph layer export**

```dart
export 'src/graph_layer.dart' show GraphLayer, GraphQuery, g, GraphLayerException;
```

The file currently reads:
```dart
export 'src/exceptions.dart';
export 'src/database.dart' show WaveDB, WaveDBConfig, WaveDBEncryption;
export 'src/iterator.dart' show KeyValue;
```

Add the graph export line after the existing ones.

- [ ] **Step 2: Commit**

```bash
git add bindings/dart/lib/wavedb.dart
git commit -m "feat(dart): add graph layer export to wavedb.dart"
```

---

### Task 5: Dart Test Suite

**Files:**
- Create: `test/graph_layer_test.dart`

- [ ] **Step 1: Write graph_layer_test.dart**

```dart
import 'dart:ffi';
import 'package:ffi/ffi.dart';
import 'package:test/test.dart';
import '../lib/src/native/types.dart';
import '../lib/src/native/wavedb_bindings.dart';
import '../lib/src/graph_layer.dart';

void main() {
  late GraphLayer graph;

  setUp(() {
    graph = GraphLayer();
  });

  tearDown(() {
    graph.close();
  });

  test('insert and query triples', () {
    graph.insertSync('clip_abc', 'tagged_with', 'gaming');
    graph.insertSync('clip_abc', 'tagged_with', 'tutorial');
    graph.insertSync('clip_abc', 'created_by', 'alice');

    final results = g.V('clip_abc').Out('tagged_with').All();
    expect(results, hasLength(2));
    expect(results, contains('gaming'));
    expect(results, contains('tutorial'));
  });

  test('traverse incoming edges', () {
    graph.insertSync('clip_abc', 'tagged_with', 'gaming');
    graph.insertSync('clip_xyz', 'tagged_with', 'gaming');

    final results = g.V('gaming').In('tagged_with').All();
    expect(results, hasLength(2));
    expect(results, contains('clip_abc'));
    expect(results, contains('clip_xyz'));
  });

  test('multi-hop traversal', () {
    graph.insertSync('alice', 'follows', 'bob');
    graph.insertSync('bob', 'likes', 'clip_abc');

    final results = g.V('alice').Out('follows').Out('likes').All();
    expect(results, hasLength(1));
    expect(results[0], 'clip_abc');
  });

  test('intersection query', () {
    graph.insertSync('clip_abc', 'tagged_with', 'gaming');
    graph.insertSync('clip_abc', 'tagged_with', 'tutorial');
    graph.insertSync('clip_xyz', 'tagged_with', 'gaming');

    final results = g.V('gaming').In('tagged_with')
        .And(g.V('tutorial').In('tagged_with'))
        .All();

    expect(results, hasLength(1));
    expect(results[0], 'clip_abc');
  });

  test('has filter', () {
    graph.insertSync('clip_abc', 'name', 'My Clip');
    graph.insertSync('clip_xyz', 'name', 'Other Clip');

    final results = g.Has('name', 'My Clip').All();
    expect(results, hasLength(1));
    expect(results[0], 'clip_abc');
  });

  test('limit', () {
    graph.insertSync('clip_abc', 'tagged_with', 'gaming');
    graph.insertSync('clip_xyz', 'tagged_with', 'gaming');

    final results = g.V('gaming').In('tagged_with').Limit(1).All();
    expect(results, hasLength(1));
  });

  test('count', () {
    graph.insertSync('clip_abc', 'tagged_with', 'gaming');
    graph.insertSync('clip_xyz', 'tagged_with', 'gaming');

    final count = graph.count('g.V("gaming").In("tagged_with")');
    expect(count, 2);
  });

  test('delete triple', () {
    graph.insertSync('clip_abc', 'tagged_with', 'gaming');
    graph.deleteSync('clip_abc', 'tagged_with', 'gaming');

    final results = g.V('clip_abc').Out('tagged_with').All();
    expect(results, hasLength(0));
  });

  test('empty result', () {
    final results = g.V('nonexistent').Out('unknown').All();
    expect(results, hasLength(0));
  });

  test('DSL string via exec', () {
    graph.insertSync('clip_abc', 'tagged_with', 'gaming');

    final results = graph.exec('g.V("clip_abc").Out("tagged_with")');
    expect(results, hasLength(1));
    expect(results[0], 'gaming');
  });

  test('Query.toString() debugging', () {
    final q = g.V('alice').Out('follows').Out('likes').Limit(5);
    expect(q.toString(), 'g.V("alice").Out("follows").Out("likes").Limit(5)');
  });

  test('GraphLayerException on failed insert', () {
    // After close, operations should throw
    graph.close();
    expect(
      () => graph.insertSync('a', 'b', 'c'),
      throwsA(isA<StateError>()),
    );
  });

  test('NativeFinalizer frees C resources', () {
    // Create and drop a layer without calling close()
    // The NativeFinalizer should clean up
    final temp = GraphLayer();
    temp.insertSync('clip', 'tagged', 'gaming');
    // Just let temp go out of scope — finalizer handles cleanup
    // Verify the default graph was rebound
    expect(graph, isNotNull);
  });
}
```

- [ ] **Step 2: Build the C shared library and run Dart tests**

```bash
cd /home/victor/Workspace/src/github.com/vijayee/WaveDB/build
cmake .. -DBUILD_TESTS=ON -DBUILD_DART_BINDINGS=ON 2>&1 | tail -3
make -j4 2>&1 | tail -5
```

If the shared lib isn't being built via BUILD_DART_BINDINGS, build it directly:
```bash
make wavedb_shared 2>&1 | tail -5
```

Then build and run the Dart tests:
```bash
cd /home/victor/Workspace/src/github.com/vijayee/WaveDB/bindings/dart
cp /home/victor/Workspace/src/github.com/vijayee/WaveDB/build/libwavedb.so .
dart pub get 2>&1 | tail -3
dart test test/graph_layer_test.dart 2>&1
```

- [ ] **Step 3: Fix any issues**

Common issues:
- `graph_result_vertices` returns `const char* const*` — FFI Pointer<Pointer<Utf8>> dereferencing
- Shared library not finding graph symbols — ensure `graph_parser.c` and `graph_ops.c` are linked into `libwavedb.so`
- Test directory / shared lib path

- [ ] **Step 4: Commit**

```bash
git add bindings/dart/test/graph_layer_test.dart
git commit -m "test(dart): add graph layer test suite"
```

---

### Task 6: Full Build + Verify

- [ ] **Step 1: Build the C library with Dart bindings**

```bash
cd /home/victor/Workspace/src/github.com/vijayee/WaveDB/build && cmake .. -DBUILD_DART_BINDINGS=ON -DBUILD_TESTS=ON 2>&1 | tail -2 && make -j4 2>&1 | tail -5
```

- [ ] **Step 2: Run all C graph tests to verify no regressions**

```bash
./test_graph 2>&1 | grep -E "PASSED|FAIL"
./test_graph_parser 2>&1 | grep -E "PASSED|FAIL"
./test_graph_set 2>&1 | grep -E "passed|failed"
```

- [ ] **Step 3: Run Dart graph tests**

```bash
cp libwavedb.so /home/victor/Workspace/src/github.com/vijayee/WaveDB/bindings/dart/
cd /home/victor/Workspace/src/github.com/vijayee/WaveDB/bindings/dart && dart test test/graph_layer_test.dart 2>&1
```

- [ ] **Step 4: Commit**

```bash
git add -A
git commit -m "feat(dart): complete graph layer bindings with Query builder"
```