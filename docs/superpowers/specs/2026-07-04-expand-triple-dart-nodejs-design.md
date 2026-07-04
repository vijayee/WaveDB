# `expandTriple` for Dart and Node.js + Python README GraphQL example

**Date:** 2026-07-04
**Scope:** Port the cross-subtree atomic batch helper added in commit `4cea6d2`
(`graph_triple_expand_ops` / `GraphLayer.expand_triple`) to the Dart and
Node.js bindings; add a GraphQL example to the Python README; release-build,
test, and publish to PyPI and npm.

## Context

Commit `4cea6d2` introduced `graph_triple_expand_ops` in the C layer and
mirrored it in the Python binding as `GraphLayer.expand_triple(s, p, o, *,
delete=False) -> list[dict]`. It expands a single triple into root-namespace
`raw_op_t` entries — one per graph index the layer's schema requires
(SPO/POS/OSP/PSO) — so a triple's index writes can be merged into a single
`database_batch_sync_raw` call alongside ops from other subtrees, giving the
whole set shared atomicity in one transaction/WAL record.

The Dart and Node.js bindings lack this helper. The Python README also lacks
any GraphQL example, despite shipping a `GraphQLLayer` class.

## Goals

1. Add `expandTriple` to the Node.js `GraphLayer` class.
2. Add `expandTriple` to the Dart `GraphLayer` class.
3. Add a GraphQL example section to the Python README.
4. Test the new helper in both bindings.
5. Release-build the C library, run all test suites green.
6. Bump Python `0.1.4 → 0.1.5` and publish to PyPI.
7. Bump Node.js `0.14.0 → 0.14.1` and publish to npm.
8. Leave the Dart `pubspec.yaml` at `0.1.0` (not published this round).

## Non-goals

- No new C API surface — `graph_triple_expand_ops` is already exported in
  `src/wavedb.def` and the C layer is unchanged.
- No Dart publish (user only asked for PyPI + npm).
- No changes to async paths — `expandTriple` is a synchronous helper that
  returns op dicts/maps for the caller to feed into the existing sync
  `batchSync` API. An async batch that includes expanded triples is still
  the caller's responsibility (build the list, call `batch(...)`).

## API shape (mirrors Python exactly)

**Node.js**

```js
const ops = graph.expandTriple('alice', 'knows', 'bob');        // put
const dels = graph.expandTriple('alice', 'knows', 'bob', {delete: true});
// ops === [
//   { type: 'put', key: 'graph/spo/alice/knows/bob', value: '' },
//   { type: 'put', key: 'graph/pos/knows/bob/alice', value: '' },
//   { type: 'put', key: 'graph/osp/bob/alice/knows', value: '' },
//   { type: 'put', key: 'graph/pso/knows/alice/bob', value: '' },
// ]
// dels === [
//   { type: 'del', key: 'graph/spo/alice/knows/bob' },
//   { type: 'del', key: 'graph/pos/knows/bob/alice' },
//   { type: 'del', key: 'graph/osp/bob/alice/knows' },
//   { type: 'del', key: 'graph/pso/knows/alice/bob' },
// ]
db.batchSync([...contentOps, ...ops]); // one atomic transaction
```

**Dart**

```dart
final ops = graph.expandTriple('alice', 'knows', 'bob');
final dels = graph.expandTriple('alice', 'knows', 'bob', delete: true);
db.batchSync([...contentOps, ...ops]);
```

Both return the same op shape their existing `batchSync` consumers expect:
`{type: 'put'|'del', key: string, value: string}` for put (value is the empty
string — the graph index presence marker), `{type: 'del', key: string}` for
del. The keys are already in the **root** database namespace (the C helper
prepends the subtree prefix); callers must pass them to the **root**
`batchSync`, not to a subtree batch (which would re-prepend the prefix and
double-prefix the key).

## Implementation

### Node.js

`bindings/nodejs/src/graph_layer.cc`:
- Add `Napi::Value ExpandTriple(const Napi::CallbackInfo& info);` private method
  declaration.
- Implement: validate 3 string args, optional 4th object arg with `delete`
  boolean (default false). Allocate
  `std::vector<raw_op_t> ops_buf(4)` on the stack (the C helper fills at most
  4). Call `graph_triple_expand_ops(layer_, s, p, o, type_int, ops_buf.data(),
  4)`. Build a `Napi::Array` of op objects; for each filled entry, copy the
  key bytes into a `std::string` (so we can `free()` the C key immediately),
  then construct the JS object. `free()` every `ops_buf[i].key` in a `finally`
  block (C++ RAII via a small scope guard) so a Napi exception can't leak the
  C-allocated keys. Return the `Napi::Array`.
- Register `InstanceMethod("expandTriple", &GraphLayer::ExpandTriple)` in
  `DefineClass`.

`bindings/nodejs/lib/graph.js`:
- Add `expandTriple(s, p, o, opts = {})` JS method: thin wrapper that
  validates `_closed`, reads `opts.delete`, calls `this._layer.expandTriple(...)`
  and returns the array. Mirror the existing `insertSync`/`deleteSync` style.

### Dart

`bindings/dart/lib/src/native/wavedb_bindings.dart`:
- Add FFI typedefs:

  ```dart
  typedef GraphTripleExpandOpsC = Size Function(
    Pointer<graph_layer_t> layer,
    Pointer<Utf8> s, Pointer<Utf8> p, Pointer<Utf8> o,
    Int32 type,
    Pointer<RawOp> outOps, Size maxOps,
  );
  typedef GraphTripleExpandOps = int Function(
    Pointer<graph_layer_t> layer,
    Pointer<Utf8> s, Pointer<Utf8> p, Pointer<Utf8> o,
    int type,
    Pointer<RawOp> outOps, int maxOps,
  );
  ```

- `static late final GraphTripleExpandOps _graphTripleExpandOps = ...lookup('graph_triple_expand_ops')`.
- Add a `graphTripleExpandOps` static helper that takes a `Pointer<RawOp>
  outOps` and `int maxOps` (caller owns the buffer and the per-op key frees)
  and returns the count — leaves key ownership to the caller, matching the C
  contract.

`bindings/dart/lib/src/graph_layer.dart`:
- Add `List<Map<String, dynamic>> expandTriple(String s, String p, String o,
  {bool delete = false})`. Allocate `Pointer<RawOp> opsPtr = calloc<RawOp>(4)`,
  encode s/p/o to `Pointer<Utf8>`, call `graphTripleExpandOps`, build the
  result list (for each filled entry: copy the key bytes via
  `Utf8`-cast `opsPtr[i].key` into a Dart string, `malloc.free` the key
  pointer), free the Utf8 args, free `opsPtr`. Use try/finally so a throw
  can't leak the C keys or the ops buffer.

### Python README

Add a "GraphQL" subsection (the existing "Graph and GraphQL" section only
shows the Graph API). The example will:

1. Create a `WaveDB` and a `GraphQLLayer("gql", db)`.
2. Parse a `type User { id: ID! name: String age: Int }` schema.
3. Write entity data via `db.put_sync("gql/Users/1/name", "Alice")` (matching
   the default resolver's `<plural>/<id>/<field>` lookup path).
4. Run `result = layer.query_sync('{ User(id: "1") { name age } }')`.
5. Show `result.data["User"][0]["name"] == "Alice"`.

Mirrors `test_graphql.py::test_graphql_query_multiple_fields`.

### Tests

Node.js — `bindings/nodejs/test/graph.test.js`, add a `describe('expandTriple',
...)` block with four `it()` cases mirroring `test_graph.py`:
- `shape`: 4 ops, all `type === 'put'`, all `value === ''`, all keys start with
  `graph/`, includes `graph/spo/alice/knows/bob`.
- `batch atomic equivalence`: splice `expandTriple('alice','knows','bob')`
  into a `batchSync` with a content op, assert both the content value is
  readable and `query().vertex('alice').out('knows').All()` returns `['bob']`.
- `delete via batch`: `insertSync`, then `batchSync(expandTriple(..., {delete:
  true}))`, assert the query returns no `bob`.
- `stored keys match insertSync`: write via `expandTriple`+`batchSync` in one
  db, write via `insertSync` in another, scan `graph/` in both and assert the
  key sets are equal and contain all four canonical index keys.

Use `tmp` directories (or in-memory `GraphLayer()`) — the existing tests use
the no-arg `new GraphLayer()` (in-memory temp dir) pattern.

Dart — `bindings/dart/test/graph_layer_test.dart`, add a `group('expandTriple',
...)` block with the same four cases. Use `GraphLayer()` (temp dir) and
`WaveDB()` instances following the existing test file's style.

### Release build

```bash
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release -j
```

Run tests with `WAVEDB_LIB_PATH=$PWD/build-release/libwavedb.so` for Python;
the Node.js build copies `c_src/` and rebuilds; the Dart tests link the same
Release `.so`.

### Version bumps and publish

- Python: bump `pyproject.toml` `version` and `src/wavedb/__init__.py`
  `__version__` from `0.1.4` → `0.1.5`. Build sdist+wheel, `twine upload
  dist/*`.
- Node.js: bump `package.json` `version` `0.14.0` → `0.14.1`. `npm publish`
  (the `prepublishOnly` script runs copy-sources + build + test).
- Dart: leave `pubspec.yaml` at `0.1.0`.

## Risk notes

- **Publish is hard to reverse.** PyPI releases cannot be deleted (only
  yanked, and only within a window); npm unpublish has tight limits. Before
  publishing I will run the full test suites on the release build and
  confirm the dist artifacts look right.
- **Credentials** are claimed to already be on the machine (`~/.pypirc` and
  `~/.npmrc`). I will verify by reading those files (without printing the
  tokens) before invoking `twine upload` / `npm publish`. If a credential is
  missing, I stop before the publish step and report.
- **Cross-subtree atomicity depends on the C batch path**, not on the
  binding. The binding's job is just to emit the right op dicts and let the
  caller call `batchSync`. The four-test suite proves equivalence with
  `insertSync`, which is the user-visible contract.

## Out of scope

- No Dart publish.
- No new C API.
- No async `expandTriple` (the sync helper returns op dicts; the caller can
  feed them to async `batch()` if desired — that path already exists).
- No Node.js / Dart README updates for `expandTriple` beyond a brief mention
  in each binding's existing Graph section (kept short — the Python README
  is the canonical reference).