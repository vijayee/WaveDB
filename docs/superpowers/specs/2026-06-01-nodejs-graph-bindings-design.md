# Node.js Graph Bindings Design

## Overview

JavaScript Gremlin-like query builder that serializes to DSL strings, plus a thin N-API wrapper around the C graph layer API.

## Architecture

```
JS (lib/graph.js)              N-API (graph_layer.cc)          C (graph layer)
─────────────────              ──────────────────────          ──────────────
g.V("gaming")
  .In("tagged_with")
  .All()                  ──▶  graph.exec(dsl)            →    graph_parse_execute()
                              return string[]             ←    C result → JS array

graph.insertSync(s,p,o)  ──▶  graph_insert_sync(...)     →    graph_insert_sync()
graph.deleteSync(s,p,o)  ──▶  graph_delete_sync(...)     →    graph_delete_sync()
graph.count(dsl)         ──▶  graph_parse_count(...)      →    returns size_t
```

## JS Query Builder (`lib/graph.js`)

Pure JS. Each Query optionally holds a reference to a GraphLayer. `.All()` auto-executes when bound:

```js
class Query {
  constructor(layer) {
    this._layer = layer || null;
    this._steps = [];
  }

  V(id)          { this._steps.push(`V("${id}")`); return this; }
  Out(pred)      { this._steps.push(`Out("${pred}")`); return this; }
  In(pred)       { this._steps.push(`In("${pred}")`); return this; }
  Has(p,v)       { this._steps.push(`Has("${p}","${v}")`); return this; }
  And(sub)       { this._steps.push(`And(${sub._toDSL()})`); return this; }
  Or(sub)        { this._steps.push(`Or(${sub._toDSL()})`); return this; }
  Limit(n)       { this._steps.push(`Limit(${n})`); return this; }

  _toDSL() { return `g.${this._steps.join('.')}`; }

  All() {
    if (!this._layer) throw new Error('Not bound to a graph layer');
    return this._layer.exec(this._toDSL());
  }
}

let _defaultGraph = null;

function g() { return new Query(_defaultGraph); }
```

Module auto-sets `_defaultGraph` when a new GraphLayer is constructed, so `g.V("x").Out("y").All()` works out of the box.

## GraphLayer API

| Method | JS signature | C call | Returns |
|--------|-------------|--------|---------|
| `constructor` | `new GraphLayer(path?, opts?)` | `graph_layer_create()` | instance |
| `exec` | `exec(dsl: string)` | `graph_parse_execute()` | `string[]` |
| `count` | `count(dsl: string)` | `graph_parse_count()` | `number` |
| `insertSync` | `insertSync(s, p, o)` | `graph_insert_sync()` | `void` |
| `deleteSync` | `deleteSync(s, p, o)` | `graph_delete_sync()` | `void` |
| `close` | `close()` | `graph_layer_destroy()` | `void` |

Usage:
```js
const { GraphLayer } = require('wavedb');
const graph = new GraphLayer();

// Query builder with auto-execute
const clips = g.V("gaming").In("tagged_with").All();
// clips = ['clip_abc', 'clip_xyz']

// Explicit DSL
const n = graph.count('g.V("gaming").In("tagged_with")');
// n = 2

// Triple operations
graph.insertSync('clip_abc', 'tagged_with', 'gaming');
graph.deleteSync('clip_abc', 'tagged_with', 'gaming');

graph.close();
```

## N-API Layer (`src/graph_layer.cc`)

Single C++ class following the same `Napi::ObjectWrap` pattern as `graphql_layer.cc`:

```cpp
class GraphLayer : public Napi::ObjectWrap<GraphLayer> {
public:
  static Napi::Object Init(Napi::Env env, Napi::Object exports);
  GraphLayer(const Napi::CallbackInfo& info);
  ~GraphLayer();

private:
  static Napi::FunctionReference constructor_;
  graph_layer_t* layer_;

  Napi::Value Exec(const Napi::CallbackInfo& info);    // sync query
  Napi::Value Count(const Napi::CallbackInfo& info);   // count only
  Napi::Value InsertSync(const Napi::CallbackInfo& info);
  Napi::Value DeleteSync(const Napi::CallbackInfo& info);
  Napi::Value Close(const Napi::CallbackInfo& info);
};
```

Exec implementation:
```cpp
Napi::Value GraphLayer::Exec(const Napi::CallbackInfo& info) {
  std::string dsl = info[0].As<Napi::String>().Utf8Value();
  graph_parse_error_t err;
  graph_result_t* result = graph_parse_execute(dsl.c_str(), layer_, &err);
  if (!result) throw Napi::Error::New(env, err.message);

  size_t count = graph_result_count(result);
  const char* const* verts = graph_result_vertices(result);

  Napi::Array arr = Napi::Array::New(env, count);
  for (size_t i = 0; i < count; i++)
    arr.Set(i, Napi::String::New(env, verts[i]));

  graph_result_destroy(result);
  return arr;
}
```

## Files

- `bindings/nodejs/src/graph_layer.cc` — NEW: N-API C++ class
- `bindings/nodejs/lib/graph.js` — NEW: JS Query builder + module exports
- `bindings/nodejs/src/binding.cpp` — MODIFY: register GraphLayer
- `bindings/nodejs/binding.gyp` — MODIFY: add `src/graph_layer.cc` to sources
- `bindings/nodejs/package.json` — MODIFY: add `./graph` export path
- `bindings/nodejs/test/test_graph.js` — NEW: test file

## Module Exports

From `lib/graph.js`:
```js
module.exports = {
  GraphLayer,
  g,
  Query,
  setDefaultGraph,  // for wiring custom instances
};
```

Entry point via `binding.gyp` / `package.json`: the graph layer is a separate require path:
```js
const { GraphLayer, g } = require('wavedb').graph;
// or via package.json exports:
const { GraphLayer, g } = require('wavedb/graph');
```