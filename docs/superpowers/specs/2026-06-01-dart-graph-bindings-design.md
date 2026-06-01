# Dart Graph Bindings Design

## Overview

Dart FFI bindings for the C graph layer (insert/delete triples, DSL query parse/execute), plus a pure-Dart Query builder with `g.V("x").Out("y").All()` auto-execution, mirroring the Node.js bindings.

## Architecture

```
Dart                              FFI (dart:ffi)                  C (graph layer)
────                              ──────────────                  ──────────────
g.V("gaming")
  .In("tagged_with")
  .All()                          graph_parse_execute(dsl)   →    graph_parser.c
                            ──▶  ──────────────────────────▶    graph_execute_chain()
                                  return List<String>      ←    graph_result_t

graph.insertSync(s,p,o)      ──▶  graph_insert_sync(...)   →    graph_insert_sync()
graph.deleteSync(s,p,o)      ──▶  graph_delete_sync(...)   →    graph_delete_sync()
graph.count(dsl)             ──▶  graph_parse_count(...)   →    returns size_t
```

## Dart Query Builder (lib/src/graph_query.dart)

Pure Dart, no FFI. Same pattern as the Node.js `Query` class.

```dart
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

  List<String> All() {
    if (_layer == null) throw StateError('Query not bound to a GraphLayer');
    return _layer!.exec(_toDSL());
  }

  @override
  String toString() => _toDSL();
}

GraphQuery g([GraphLayer? layer]) => GraphQuery(layer ?? _defaultGraph);
```

Module-level `_defaultGraph` is set when a new `GraphLayer` is created.

## GraphLayer API

| Dart method | C call | Returns |
|------------|--------|---------|
| `GraphLayer(path?)` | `graph_layer_create()` | instance |
| `exec(dsl)` | `graph_parse_execute()` | `List<String>` |
| `count(dsl)` | `graph_parse_count()` | `int` |
| `insertSync(s, p, o)` | `graph_insert_sync()` | `void` |
| `deleteSync(s, p, o)` | `graph_delete_sync()` | `void` |
| `close()` | `graph_layer_destroy()` | `void` |

Usage:
```dart
final graph = GraphLayer();  // in-memory
// or: GraphLayer('/path/to/db')

// Query builder with auto-execute
final clips = g.V("gaming").In("tagged_with").All();
print(clips);  // ['clip_abc', 'clip_xyz']

// Explicit DSL
final count = graph.count('g.V("gaming").In("tagged_with")');

// Triple operations
graph.insertSync('clip_abc', 'tagged_with', 'gaming');
graph.deleteSync('clip_abc', 'tagged_with', 'gaming');

graph.close();
```

## FFI Functions Required

| C function | Returns | Purpose |
|-----------|---------|---------|
| `graph_layer_create` | `graph_layer_t*` | Create layer |
| `graph_layer_destroy` | `void` | Destroy layer |
| `graph_insert_sync` | `int` | Insert triple |
| `graph_delete_sync` | `int` | Delete triple |
| `graph_parse_execute` | `graph_result_t*` | Execute DSL query |
| `graph_parse_count` | `int` | Count results |
| `graph_result_destroy` | `void` | Free result |
| `graph_result_count` | `size_t` | Get result count |
| `graph_result_vertices` | `const char* const*` | Get vertex array |

The result conversion follows a different pattern than GraphQL — instead of JSON serialization, we access `graph_result_vertices()` directly through FFI by reading contiguous string pointers.

## Result Conversion

```dart
List<String> _resultToList(Pointer<graph_result_t> resultPtr) {
  final count = WaveDBNative.graphResultCount(resultPtr);
  final verts = WaveDBNative.graphResultVertices(resultPtr);

  final result = <String>[];
  for (int i = 0; i < count; i++) {
    final strPtr = verts.elementAt(i).value;
    result.add(strPtr.toDartString());
  }
  return result;
}
```

`graph_result_vertices` returns `const char* const*` — a pointer to an array of string pointers. In FFI this is `Pointer<Pointer<Utf8>>`.

## Files

```
lib/src/
├── native/
│   ├── types.dart            — MODIFY: add graph types
│   └── wavedb_bindings.dart  — MODIFY: add graph FFI bindings
├── graph_layer.dart          — NEW: GraphLayer class + Query builder
lib/wavedb.dart               — MODIFY: add graph export
test/graph_layer_test.dart    — NEW: test suite
```

## Module Exports

From `lib/wavedb.dart`:
```dart
export 'src/graph_layer.dart' show GraphLayer, GraphQuery, g;
```

## Tests

```dart
void main() {
  test('insert and query triples', () {
    final graph = GraphLayer();
    graph.insertSync('clip_abc', 'tagged_with', 'gaming');
    graph.insertSync('clip_abc', 'tagged_with', 'tutorial');

    final results = g.V('clip_abc').Out('tagged_with').All();
    expect(results.length, 2);
    expect(results, contains('gaming'));
    expect(results, contains('tutorial'));

    graph.close();
  });

  // Multi-hop, intersection, Has, Limit, Count, delete, empty,
  // DSL string exec, Query.toString()
}
```