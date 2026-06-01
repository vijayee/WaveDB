# Node.js Graph Bindings Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Expose the C graph layer (insert/delete triples, DSL query parsing) to Node.js via N-API, with a JS Gremlin-like Query builder and `g.V("x").Out("y").All()` auto-execution.

**Architecture:** Two C++ N-API files (graph_layer.cc + async glue) following the exact same `Napi::ObjectWrap` pattern as the existing `graphql_layer.cc`. A pure-JS `lib/graph.js` module provides the Query builder with `.All()` auto-execute via a module-level default graph instance.

**Tech Stack:** C++ N-API (node-addon-api), existing graph C API, Mocha for JS testing

---

## File Structure

```
bindings/nodejs/
├── src/
│   ├── graph_layer.cc       — NEW: N-API C++ class (GraphLayer)
│   ├── graph_result_js.cc   — NEW: convert C result to JS array
│   ├── graph_result_js.h    — NEW: header for result conversion
│   └── binding.cpp          — MODIFY: add GraphLayer::Init and graph module
├── lib/
│   └── graph.js             — NEW: Query builder + g() function + exports
├── test/
│   └── graph.test.js        — NEW: Mocha test suite
├── binding.gyp              — MODIFY: add graph_layer.cc source target
└── package.json             — MODIFY: add exports path for ./graph
```

---

### Task 1: N-API C++ GraphLayer class

**Files:**
- Create: `bindings/nodejs/src/graph_layer.cc`
- Create: `bindings/nodejs/src/graph_result_js.h`
- Create: `bindings/nodejs/src/graph_result_js.cc`

- [ ] **Step 1: Write graph_result_js.h**

```cpp
// C++ headers that include <atomic> must come before C headers
// that use ATOMIC_TYPE() macros expanding to std::atomic<T> in C++.
#include <atomic>

#include <napi.h>
#include "../../../src/Layers/graph/graph.h"

#ifndef WAVEDB_BINDINGS_GRAPH_RESULT_JS_H
#define WAVEDB_BINDINGS_GRAPH_RESULT_JS_H

// Convert a C graph_result_t to a JS array of strings.
// Returns an empty array if result is NULL or has count 0.
// The caller must still destroy the C result separately.
Napi::Array GraphResultToJS(Napi::Env env, graph_result_t* result);

#endif
```

- [ ] **Step 2: Write graph_result_js.cc**

```cpp
// C++ headers that include <atomic> must come before C headers
#include <atomic>

#include "graph_result_js.h"

Napi::Array GraphResultToJS(Napi::Env env, graph_result_t* result) {
  if (!result) return Napi::Array::New(env, 0);

  size_t count = graph_result_count(result);
  const char* const* verts = graph_result_vertices(result);

  Napi::Array arr = Napi::Array::New(env, count);
  for (size_t i = 0; i < count; i++) {
    arr.Set(i, Napi::String::New(env, verts[i]));
  }
  return arr;
}
```

- [ ] **Step 3: Write graph_layer.cc**

The full N-API class following the GraphQLLayer pattern exactly:

```cpp
// C++ headers that include <atomic> must come before C headers
// that use ATOMIC_TYPE() macros expanding to std::atomic<T> in C++.
#include <atomic>

#include <napi.h>
#include <cstdlib>
#include <string>
#include "../../../src/Layers/graph/graph.h"
#include "graph_result_js.h"

class GraphLayer : public Napi::ObjectWrap<GraphLayer> {
public:
  static Napi::Object Init(Napi::Env env, Napi::Object exports);

  GraphLayer(const Napi::CallbackInfo& info);
  ~GraphLayer();

private:
  static Napi::FunctionReference constructor_;
  graph_layer_t* layer_;

  Napi::Value Exec(const Napi::CallbackInfo& info);
  Napi::Value Count(const Napi::CallbackInfo& info);
  Napi::Value InsertSync(const Napi::CallbackInfo& info);
  Napi::Value DeleteSync(const Napi::CallbackInfo& info);
  Napi::Value Close(const Napi::CallbackInfo& info);
};

Napi::FunctionReference GraphLayer::constructor_;

Napi::Object GraphLayer::Init(Napi::Env env, Napi::Object exports) {
  Napi::HandleScope scope(env);

  Napi::Function func = DefineClass(env, "GraphLayer", {
    InstanceMethod("exec", &GraphLayer::Exec),
    InstanceMethod("count", &GraphLayer::Count),
    InstanceMethod("insertSync", &GraphLayer::InsertSync),
    InstanceMethod("deleteSync", &GraphLayer::DeleteSync),
    InstanceMethod("close", &GraphLayer::Close),
  });

  constructor_ = Napi::Persistent(func);
  exports.Set("GraphLayer", func);

  return exports;
}

GraphLayer::GraphLayer(const Napi::CallbackInfo& info)
  : Napi::ObjectWrap<GraphLayer>(info),
    layer_(nullptr) {

  Napi::Env env = info.Env();

  const char* db_path = nullptr;
  std::string path_str;
  if (info.Length() > 0 && info[0].IsString()) {
    path_str = info[0].As<Napi::String>().Utf8Value();
    db_path = path_str.c_str();
  }

  int error_code = 0;
  if (db_path) {
    layer_ = graph_layer_create(db_path, NULL);
  } else {
    // In-memory graph layer
    database_config_t config = database_config_default();
    config.lru_memory_mb = 50;
    layer_ = graph_layer_create(NULL, &config);
  }

  if (!layer_) {
    Napi::Error::New(env, "Failed to create GraphLayer").ThrowAsJavaScriptException();
    return;
  }
}

GraphLayer::~GraphLayer() {
  if (layer_) {
    graph_layer_destroy(layer_);
    layer_ = nullptr;
  }
}

Napi::Value GraphLayer::Exec(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (!layer_) {
    Napi::Error::New(env, "GraphLayer is closed").ThrowAsJavaScriptException();
    return env.Null();
  }

  if (info.Length() < 1 || !info[0].IsString()) {
    Napi::TypeError::New(env, "DSL string required").ThrowAsJavaScriptException();
    return env.Null();
  }

  std::string dsl = info[0].As<Napi::String>().Utf8Value();

  graph_parse_error_t err;
  graph_result_t* result = graph_parse_execute(dsl.c_str(), layer_, &err);

  if (!result) {
    Napi::Error::New(env, err.message).ThrowAsJavaScriptException();
    return env.Null();
  }

  Napi::Array arr = GraphResultToJS(env, result);
  graph_result_destroy(result);
  return arr;
}

Napi::Value GraphLayer::Count(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (!layer_) {
    Napi::Error::New(env, "GraphLayer is closed").ThrowAsJavaScriptException();
    return env.Null();
  }

  if (info.Length() < 1 || !info[0].IsString()) {
    Napi::TypeError::New(env, "DSL string required").ThrowAsJavaScriptException();
    return env.Null();
  }

  std::string dsl = info[0].As<Napi::String>().Utf8Value();

  graph_parse_error_t err;
  size_t count = 0;
  int rc = graph_parse_count(dsl.c_str(), layer_, &count, &err);

  if (rc != 0) {
    Napi::Error::New(env, err.message).ThrowAsJavaScriptException();
    return env.Null();
  }

  return Napi::Number::New(env, count);
}

Napi::Value GraphLayer::InsertSync(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (!layer_) {
    Napi::Error::New(env, "GraphLayer is closed").ThrowAsJavaScriptException();
    return env.Null();
  }

  if (info.Length() < 3) {
    Napi::TypeError::New(env, "s, p, o strings required").ThrowAsJavaScriptException();
    return env.Null();
  }

  std::string s = info[0].As<Napi::String>().Utf8Value();
  std::string p = info[1].As<Napi::String>().Utf8Value();
  std::string o = info[2].As<Napi::String>().Utf8Value();

  int rc = graph_insert_sync(layer_, s.c_str(), p.c_str(), o.c_str());
  if (rc != 0) {
    Napi::Error::New(env, "Insert failed").ThrowAsJavaScriptException();
  }

  return env.Undefined();
}

Napi::Value GraphLayer::DeleteSync(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (!layer_) {
    Napi::Error::New(env, "GraphLayer is closed").ThrowAsJavaScriptException();
    return env.Null();
  }

  if (info.Length() < 3) {
    Napi::TypeError::New(env, "s, p, o strings required").ThrowAsJavaScriptException();
    return env.Null();
  }

  std::string s = info[0].As<Napi::String>().Utf8Value();
  std::string p = info[1].As<Napi::String>().Utf8Value();
  std::string o = info[2].As<Napi::String>().Utf8Value();

  int rc = graph_delete_sync(layer_, s.c_str(), p.c_str(), o.c_str());
  if (rc != 0) {
    Napi::Error::New(env, "Delete failed").ThrowAsJavaScriptException();
  }

  return env.Undefined();
}

Napi::Value GraphLayer::Close(const Napi::CallbackInfo& info) {
  if (layer_) {
    graph_layer_t* layer = layer_;
    layer_ = nullptr;
    graph_layer_destroy(layer);
  }
  return info.Env().Undefined();
}

// Module init for the graph addon
Napi::Object InitGraph(Napi::Env env, Napi::Object exports) {
  return GraphLayer::Init(env, exports);
}

NODE_API_MODULE(wavedb_graph, InitGraph)
```

Note: This file registers itself as a separate native module `wavedb_graph` (via `NODE_API_MODULE`). That's because it needs its own `.node` file that links to the same `libwavedb.a`. The `binding.gyp` will have a separate target for it, just like `graphql`.

- [ ] **Step 4: Commit**

```bash
git add bindings/nodejs/src/graph_layer.cc bindings/nodejs/src/graph_result_js.h bindings/nodejs/src/graph_result_js.cc
git commit -m "feat(nodejs): add GraphLayer N-API class with exec/count/insertSync/deleteSync"
```

---

### Task 2: binding.gyp target + binding.cpp registration

**Files:**
- Modify: `bindings/nodejs/binding.gyp`
- Modify: `bindings/nodejs/src/binding.cpp`

- [ ] **Step 1: Add the graph target to binding.gyp**

After the closing `}` of the `graphql` target (line 122), add a new target:

```json
  , {
    "target_name": "graph",
    "sources": [
      "src/graph_layer.cc",
      "src/graph_result_js.cc"
    ],
    "include_dirs": [
      "<!@(node -p \"require('node-addon-api').include\")",
      "../../src",
      "../../deps/libcbor/src",
      "../../build/deps/libcbor/src",
      "../../build/deps/libcbor",
      "../../deps/hashmap/include",
      "../../deps/xxhash"
    ],
    "dependencies": [
      "<!@(node -p \"require('node-addon-api').gyp\")"
    ],
    "cflags!": ["-fno-exceptions"],
    "cflags": ["-O3"],
    "cflags_cc!": ["-fno-exceptions"],
    "cflags_cc": ["-O3"],
    "libraries": [
      "../../../build/libwavedb.a",
      "../../../build/libxxhash.a",
      "../../../build/libhashmap.a",
      "../../../build/deps/libcbor/src/libcbor.a"
    ],
    "conditions": [
      ["OS=='linux'", {
        "libraries": ["-lpthread", "-latomic", "-lcrypto", "-lssl"]
      }],
      ["OS=='mac'", {
        "xcode_settings": {
          "GCC_ENABLE_CPP_EXCEPTIONS": "YES",
          "MACOSX_DEPLOYMENT_TARGET": "10.7"
        },
        "libraries": ["-lcrypto", "-lssl"]
      }],
      ["OS=='win'", {
        "libraries": [
          "../../../build/Release/wavedb.lib",
          "../../../build/Release/xxhash.lib",
          "../../../build/Release/hashmap.lib",
          "../../../build/deps/libcbor/src/Release/cbor.lib",
          "ws2_32.lib",
          "bcrypt.lib"
        ],
        "msvs_settings": {
          "VCCLCompilerTool": {
            "AdditionalOptions": ["/std:c11"]
          }
        }
      }]
    ]
  }]
```

Make sure the ending is `]` (closing the outer targets array). The file currently ends with `]}` — the new target needs to be inserted before the last `]`.

- [ ] **Step 2: Build the native addon to verify compilation**

```bash
cd bindings/nodejs
npm run build 2>&1 | tail -20
```

Expected: `graph.target.mk` is generated, `graph.node` is linked, no errors.

- [ ] **Step 3: Commit**

```bash
git add bindings/nodejs/binding.gyp
git commit -m "feat(nodejs): add graph target to binding.gyp"
```

---

### Task 3: JS Query Builder + module exports

**Files:**
- Create: `bindings/nodejs/lib/graph.js`

- [ ] **Step 1: Write lib/graph.js**

```js
'use strict';

const { GraphLayer: GraphLayerNative } = require('../build/Release/graph.node');

let _defaultGraph = null;

class Query {
  constructor(layer) {
    this._layer = layer !== undefined ? layer : _defaultGraph;
    this._steps = [];
  }

  V(id)          { this._steps.push(`V("${id}")`); return this; }
  Out(pred)      { this._steps.push(`Out("${pred}")`); return this; }
  In(pred)       { this._steps.push(`In("${pred}")`); return this; }
  Has(pred, val) { this._steps.push(`Has("${pred}","${val}")`); return this; }
  And(sub)       { this._steps.push(`And(${sub._toDSL()})`); return this; }
  Or(sub)        { this._steps.push(`Or(${sub._toDSL()})`); return this; }
  Limit(n)       { this._steps.push(`Limit(${n})`); return this; }
  Count()        { this._steps.push(`Count()`); return this; }

  _toDSL() {
    return `g.${this._steps.join('.')}`;
  }

  All() {
    if (!this._layer) {
      throw new Error('Query is not bound to a GraphLayer. ' +
        'Create a GraphLayer or use graph.exec(query._toDSL())');
    }
    return this._layer.exec(this._toDSL());
  }

  /**
   * Render the query to its DSL string without executing.
   * Useful for debugging or explicit execution.
   */
  toString() {
    return this._toDSL();
  }
}

/**
 * Create a new Query bound to the default graph instance.
 * @returns {Query}
 */
function g(layer) {
  if (layer !== undefined) return new Query(layer);
  return new Query(_defaultGraph);
}

/**
 * Set the default graph instance for `g.V(...).All()` queries.
 * Called automatically when a new GraphLayer is constructed.
 * @param {GraphLayer} layer
 */
function setDefaultGraph(layer) {
  _defaultGraph = layer;
}

/**
 * GraphLayer wrapper
 */
class GraphLayer {
  /**
   * @param {string} [path] - Database path (omit for in-memory)
   * @param {Object} [options] - Options (future use)
   */
  constructor(path, options = {}) {
    this._layer = new GraphLayerNative(path, options);
    this._closed = false;
    // Auto-set as default so g.V(...).All() works
    if (!_defaultGraph) setDefaultGraph(this);
  }

  /**
   * Execute a DSL query and return results as an array of strings.
   * @param {string|Query} dsl - DSL string or Query object
   * @returns {string[]} Array of vertex IDs
   */
  exec(dsl) {
    if (this._closed) throw new Error('GraphLayer is closed');
    if (dsl instanceof Query) dsl = dsl._toDSL();
    return this._layer.exec(dsl);
  }

  /**
   * Execute a DSL query and return only the count.
   * @param {string|Query} dsl - DSL string or Query object
   * @returns {number}
   */
  count(dsl) {
    if (this._closed) throw new Error('GraphLayer is closed');
    if (dsl instanceof Query) dsl = dsl._toDSL();
    return this._layer.count(dsl);
  }

  /**
   * Insert a triple synchronously.
   */
  insertSync(s, p, o) {
    if (this._closed) throw new Error('GraphLayer is closed');
    this._layer.insertSync(s, p, o);
  }

  /**
   * Delete a triple synchronously.
   */
  deleteSync(s, p, o) {
    if (this._closed) throw new Error('GraphLayer is closed');
    this._layer.deleteSync(s, p, o);
  }

  /**
   * Close the graph layer and release C resources.
   */
  close() {
    if (this._layer && !this._closed) {
      this._closed = true;
      this._layer.close();
    }
  }

  get isClosed() {
    return this._closed;
  }
}

module.exports = {
  GraphLayer,
  Query,
  g,
  setDefaultGraph,
};
```

- [ ] **Step 2: Commit**

```bash
git add bindings/nodejs/lib/graph.js
git commit -m "feat(nodejs): add JS Query builder with g().V().Out().All()"
```

---

### Task 4: JS Tests

**Files:**
- Create: `bindings/nodejs/test/graph.test.js`

- [ ] **Step 1: Write test/graph.test.js**

```js
'use strict';

const assert = require('assert');
const { GraphLayer, g, Query } = require('../lib/graph');

describe('GraphLayer', function() {
  this.timeout(10000);

  let graph;
  beforeEach(function() {
    graph = new GraphLayer();
  });
  afterEach(function() {
    graph.close();
  });

  it('should insert and query triples', function() {
    graph.insertSync('clip_abc', 'tagged_with', 'gaming');
    graph.insertSync('clip_abc', 'tagged_with', 'tutorial');
    graph.insertSync('clip_abc', 'created_by', 'alice');

    const result = g.V('clip_abc').Out('tagged_with').All();
    assert.ok(Array.isArray(result));
    assert.strictEqual(result.length, 2);
    assert.ok(result.includes('gaming'));
    assert.ok(result.includes('tutorial'));
  });

  it('should traverse incoming edges', function() {
    graph.insertSync('clip_abc', 'tagged_with', 'gaming');
    graph.insertSync('clip_xyz', 'tagged_with', 'gaming');

    const result = g.V('gaming').In('tagged_with').All();
    assert.strictEqual(result.length, 2);
    assert.ok(result.includes('clip_abc'));
    assert.ok(result.includes('clip_xyz'));
  });

  it('should do multi-hop traversals', function() {
    graph.insertSync('alice', 'follows', 'bob');
    graph.insertSync('bob', 'likes', 'clip_abc');

    const result = g.V('alice').Out('follows').Out('likes').All();
    assert.strictEqual(result.length, 1);
    assert.strictEqual(result[0], 'clip_abc');
  });

  it('should handle intersection queries', function() {
    graph.insertSync('clip_abc', 'tagged_with', 'gaming');
    graph.insertSync('clip_abc', 'tagged_with', 'tutorial');
    graph.insertSync('clip_xyz', 'tagged_with', 'gaming');

    const result = g.V('gaming').In('tagged_with')
      .And(g.V('tutorial').In('tagged_with'))
      .All();

    assert.strictEqual(result.length, 1);
    assert.strictEqual(result[0], 'clip_abc');
  });

  it('should handle Has filters', function() {
    graph.insertSync('clip_abc', 'name', 'My Clip');
    graph.insertSync('clip_xyz', 'name', 'Other Clip');

    const result = g.Has('name', 'My Clip').All();
    assert.strictEqual(result.length, 1);
    assert.strictEqual(result[0], 'clip_abc');
  });

  it('should support Limit', function() {
    graph.insertSync('clip_abc', 'tagged_with', 'gaming');
    graph.insertSync('clip_xyz', 'tagged_with', 'gaming');

    const result = g.V('gaming').In('tagged_with').Limit(1).All();
    assert.strictEqual(result.length, 1);
  });

  it('should support Count', function() {
    graph.insertSync('clip_abc', 'tagged_with', 'gaming');
    graph.insertSync('clip_xyz', 'tagged_with', 'gaming');

    const count = graph.count('g.V("gaming").In("tagged_with")');
    assert.strictEqual(count, 2);
  });

  it('should delete triples', function() {
    graph.insertSync('clip_abc', 'tagged_with', 'gaming');
    graph.deleteSync('clip_abc', 'tagged_with', 'gaming');

    const result = g.V('clip_abc').Out('tagged_with').All();
    assert.strictEqual(result.length, 0);
  });

  it('should handle empty results', function() {
    const result = g.V('nonexistent').Out('unknown').All();
    assert.ok(Array.isArray(result));
    assert.strictEqual(result.length, 0);
  });

  it('should accept DSL strings directly via exec', function() {
    graph.insertSync('clip_abc', 'tagged_with', 'gaming');

    const result = graph.exec('g.V("clip_abc").Out("tagged_with")');
    assert.strictEqual(result.length, 1);
    assert.strictEqual(result[0], 'gaming');
  });

  it('should accept Query objects via exec', function() {
    graph.insertSync('clip_abc', 'tagged_with', 'gaming');

    const q = new Query(graph);
    q.V('clip_abc').Out('tagged_with');
    const result = graph.exec(q);
    assert.strictEqual(result.length, 1);
  });

  it('should be able to use Query.toString() for debugging', function() {
    const q = g.V('alice').Out('follows').Out('likes').Limit(5);
    assert.strictEqual(q.toString(), 'g.V("alice").Out("follows").Out("likes").Limit(5)');
  });
});
```

- [ ] **Step 2: Build and run the tests**

```bash
cd /home/victor/Workspace/src/github.com/vijayee/WaveDB/build && cmake .. -DBUILD_TESTS=ON 2>&1 | tail -2 && make -j4 2>&1 | tail -5
cd /home/victor/Workspace/src/github.com/vijayee/WaveDB/bindings/nodejs && npm run build 2>&1 | tail -20
cd /home/victor/Workspace/src/github.com/vijayee/WaveDB/bindings/nodejs && npx mocha test/graph.test.js 2>&1
```

Expected: All 12 tests pass.

- [ ] **Step 3: Commit**

```bash
git add bindings/nodejs/test/graph.test.js
git commit -m "test(nodejs): add graph layer test suite"
```

---

### Task 5: Self-Review and Fixes

- [ ] **Step 1: Build the full C library + node addon + run all tests**

```bash
cd /home/victor/Workspace/src/github.com/vijayee/WaveDB/build && cmake .. -DBUILD_TESTS=ON 2>&1 | tail -2 && make -j4 2>&1 | tail -5
cd /home/victor/Workspace/src/github.com/vijayee/WaveDB/bindings/nodejs && npm run build 2>&1 | tail -20
```

- [ ] **Step 2: Run all graph tests**

```bash
cd /home/victor/Workspace/src/github.com/vijayee/WaveDB/bindings/nodejs && npx mocha test/graph.test.js 2>&1
```

- [ ] **Step 3: Run existing C tests to verify no regressions**

```bash
cd /home/victor/Workspace/src/github.com/vijayee/WaveDB/build && ./test_graph && ./test_graph_parser && ./test_graph_set
```

Expected: All pass.

- [ ] **Step 4: Run existing Node.js tests to verify no regressions**

```bash
cd /home/victor/Workspace/src/github.com/vijayee/WaveDB/bindings/nodejs && npm test 2>&1 | tail -30
```

Expected: Existing tests still pass (the existing `wavedb` and `graphql` modules are unaffected).

- [ ] **Step 5: Fix any issues and commit**

```bash
git add -A
git commit -m "fix: address review feedback and edge cases"
```