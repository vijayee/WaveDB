Excellent. Let's design this properly. I'll sketch out a **Graph Schema Layer** for WaveDB — a full query engine that treats your hierarchical paths as a triple store, with a Gremlin-inspired query language, a planner that decomposes traversals into prefix scans, and an executor that combines results efficiently.

---

# 🧩 WaveDB Graph Schema Layer — Design Sketch

## 1. Index Encoding Convention

The foundation: **all graph data is stored as paths in WaveDB**, using a convention that encodes triples (subject, predicate, object) as hierarchical keys.

### Core Index Patterns

```plaintext
# SPO Index (subject → predicate → object)
/spo/<subject>/<predicate>/<object>

# POS Index (predicate → object → subject) 
/pos/<predicate>/<object>/<subject>

# OSP Index (object → subject → predicate)
/osp/<object>/<subject>/<predicate>

# PSO Index (predicate → subject → object)
/pso/<predicate>/<subject>/<object>
```

### Why Four Indices?

Different query patterns need different index scans:

Query Pattern	Best Index	Example Scan
"Find all objects of predicate P for subject S"	**SPO**	scan `/spo/<S>/<P>/`
"Find all subjects with predicate P and object O"	**POS**	scan `/pos/<P>/<O>/`
"Find all predicates linking subject S to object O"	**OSP**	scan `/osp/<O>/<S>/`
"Find all subjects with predicate P"	**PSO**	scan `/pso/<P>/`

> **You don't need all four for every triple.** The schema layer decides which indices to maintain based on the schema definition. For a minimal system, **SPO + POS** covers most queries. Add **PSO** for predicate-centric scans and **OSP** for reverse lookups.

### Concrete Example

Storing the triple `clip_abc --tagged_with--> gaming`:

```c
// SPO: subject → predicate → object
database_put_sync_raw(db, "/spo/clip_abc/tagged_with/gaming", ...);

// POS: predicate → object → subject  
database_put_sync_raw(db, "/pos/tagged_with/gaming/clip_abc", ...);

// PSO: predicate → subject → object
database_put_sync_raw(db, "/pso/tagged_with/clip_abc/gaming", ...);

// OSP: object → subject → predicate
database_put_sync_raw(db, "/osp/gaming/clip_abc/tagged_with", ...);
```

### Value Storage

For literal values (strings, numbers), store the value at the leaf:

```plaintext
/spo/clip_abc/name/"My Awesome Clip"
/spo/clip_abc/created_at/2024-01-15
/spo/alice/follows/bob          ← bob is another entity (edge)
/spo/alice/age/30               ← 30 is a literal value
```

The schema layer distinguishes entities from literals by schema type definitions.

---

## 2. Query Language

I'll design a **Gremlin-inspired DSL** that compiles to scan plans. It's JavaScript-based (like Cayley's), making it familiar and embeddable.

### Basic Traversals

```javascript
// Find all content tagged "gaming"
g.V("gaming").In("tagged_with").All()

// Find content tagged "gaming" AND "tutorial"
g.V("gaming").In("tagged_with")
  .And(g.V("tutorial").In("tagged_with"))
  .All()

// Find content liked by people Alice follows
g.V("alice").Out("follows").Out("likes").All()

// Find content created by Alice, tagged "gaming"
g.V("alice").Out("created_by")
  .And(g.V("gaming").In("tagged_with"))
  .All()

// Limit results
g.V("gaming").In("tagged_with").GetLimit(10)

// Filter by value
g.V().Has("name", "Casablanca").All()

// Count
g.V("gaming").In("tagged_with").Count()

// Project specific fields
g.V("gaming").In("tagged_with")
  .Out("name").Out("created_at").All()
```

### Morphisms (Named Paths)

```javascript
// Define reusable path
var contentToCreator = g.Morphism()
  .Out("created_by");

// Use it
g.V("gaming").In("tagged_with")
  .Follow(contentToCreator)
  .Out("name")
  .All()
```

### The Query Object

```javascript
// The query is a chain of operations
const query = g.V("gaming")
  .In("tagged_with")
  .And(g.V("tutorial").In("tagged_with"))
  .Out("created_by")
  .GetLimit(20);
```

---

## 3. Query Planner

The planner decomposes the query chain into **scan operations** and **combination steps**.

### Step 1: Parse the Chain

```javascript
// g.V("gaming").In("tagged_with").And(g.V("tutorial").In("tagged_with")).Out("created_by").GetLimit(20)
```

Becomes an AST:

```json
{
  "type": "limit",
  "count": 20,
  "input": {
    "type": "out",
    "predicate": "created_by",
    "input": {
      "type": "intersection",
      "inputs": [
        {
          "type": "in",
          "predicate": "tagged_with",
          "input": { "type": "vertex", "id": "gaming" }
        },
        {
          "type": "in",
          "predicate": "tagged_with",
          "input": { "type": "vertex", "id": "tutorial" }
        }
      ]
    }
  }
}
```

### Step 2: Decompose into Scan Plans

Each leaf operation becomes a prefix scan against the appropriate index:

Operation	Index	Scan
`g.V("gaming")`	—	Returns `{gaming}` (single vertex set)
`.In("tagged_with")`	**POS**	scan `/pos/tagged_with/gaming/` → subjects
`.Out("follows")`	**SPO**	scan `/spo/{subject}/follows/` → objects
`.Has("name", "X")`	**POS**	scan `/pos/name/X/` → subjects

The planner chooses the cheapest index based on what's available:

```c
typedef enum {
    INDEX_SPO,  // subject → predicate → object
    INDEX_POS,  // predicate → object → subject
    INDEX_OSP,  // object → subject → predicate
    INDEX_PSO,  // predicate → subject → object
} index_type_t;

typedef struct {
    index_type_t index;     // which index to scan
    const char* prefix[3];  // up to 3 path components to bind
    int prefix_len;         // how many components are fixed
} scan_plan_t;
```

For `.In("tagged_with")` on vertex set `{gaming, tutorial}`:

```c
scan_plan_t plan = {
    .index = INDEX_POS,
    .prefix = {"tagged_with", "gaming", NULL},  // scan /pos/tagged_with/gaming/
    .prefix_len = 2
};
```

### Step 3: Build the Execution DAG

```plaintext
                  ┌─────────────┐
                  │   GetLimit  │  20
                  └──────┬──────┘
                         │
                  ┌──────┴──────┐
                  │  Out("created_by")
                  └──────┬──────┘
                         │
                  ┌──────┴──────┐
                  │ Intersection │
                  └──────┬──────┘
                         │
              ┌──────────┴──────────┐
              │                     │
     ┌────────┴────────┐   ┌───────┴────────┐
     │ In("tagged_with")│   │ In("tagged_with")│
     └────────┬────────┘   └───────┬────────┘
              │                    │
     ┌────────┴────────┐   ┌───────┴────────┐
     │  Vertex("gaming")│   │Vertex("tutorial")│
     └─────────────────┘   └────────────────┘
```

---

## 4. Executor

The executor runs the DAG, streaming results between operators.

### Core Operators

```c
// Each operator implements this interface
typedef struct operator_t operator_t;
typedef struct {
    int (*init)(operator_t* op);
    int (*next)(operator_t* op, vertex_set_t* output);
    void (*destroy)(operator_t* op);
} operator_vtable_t;

struct operator_t {
    operator_vtable_t* vtable;
    // operator-specific state
};
```

### Scan Operator

```c
typedef struct {
    operator_vtable_t vtable;
    database_t* db;
    scan_plan_t plan;
    database_iterator_t* iter;
    vertex_set_t current_batch;
} scan_operator_t;

int scan_operator_next(operator_t* op, vertex_set_t* output) {
    scan_operator_t* scan = (scan_operator_t*)op;
    
    // Build the scan path from the plan + bound variables
    char path[1024];
    build_scan_path(scan->plan, path, sizeof(path));
    
    // Create iterator
    path_t* start = path_create("/", path, 0);
    path_t* end = path_create("/", path, 0);
    path_append(end, "~", 1);  // end at next prefix
    
    scan->iter = database_scan_start(scan->db, start, end);
    
    // Collect results
    vertex_set_t results = vertex_set_create(64);
    path_t* out_path;
    identifier_t* out_val;
    while (database_scan_next(scan->iter, &out_path, &out_val) == 0) {
        // Extract the subject (last component before the fixed prefix)
        const char* subject = extract_subject(out_path, scan->plan);
        vertex_set_add(&results, subject);
        path_destroy(out_path);
        identifier_destroy(out_val);
    }
    
    *output = results;
    return 0;
}
```

### Intersection Operator

```c
typedef struct {
    operator_vtable_t vtable;
    operator_t* left;
    operator_t* right;
} intersect_operator_t;

int intersect_operator_next(operator_t* op, vertex_set_t* output) {
    intersect_operator_t* inter = (intersect_operator_t*)op;
    
    vertex_set_t left_results;
    vertex_set_t right_results;
    
    inter->left->vtable->next(inter->left, &left_results);
    inter->right->vtable->next(inter->right, &right_results);
    
    // Intersect: O(min(n,m)) using hash set
    vertex_set_t result = vertex_set_intersect(&left_results, &right_results);
    
    *output = result;
    
    vertex_set_destroy(&left_results);
    vertex_set_destroy(&right_results);
    
    return 0;
}
```

### Union Operator

```c
int union_operator_next(operator_t* op, vertex_set_t* output) {
    // Same pattern, but union instead of intersect
    vertex_set_t result = vertex_set_union(&left_results, &right_results);
    *output = result;
    return 0;
}
```

### Out/In Operators (Traversal)

```c
typedef struct {
    operator_vtable_t vtable;
    operator_t* input;
    const char* predicate;
    database_t* db;
    bool is_out;  // true = Out, false = In
} traversal_operator_t;

int traversal_operator_next(operator_t* op, vertex_set_t* output) {
    traversal_operator_t* trav = (traversal_operator_t*)op;
    
    // Get input vertices
    vertex_set_t inputs;
    trav->input->vtable->next(trav->input, &inputs);
    
    vertex_set_t results = vertex_set_create(128);
    
    // For each input vertex, scan the appropriate index
    for (int i = 0; i < inputs.count; i++) {
        const char* subject = inputs.vertices[i];
        
        char path[1024];
        if (trav->is_out) {
            // Out: /spo/<subject>/<predicate>/
            snprintf(path, sizeof(path), "/spo/%s/%s/", subject, trav->predicate);
        } else {
            // In: /pos/<predicate>/<subject>/
            snprintf(path, sizeof(path), "/pos/%s/%s/", trav->predicate, subject);
        }
        
        // Scan and collect
        path_t* start = path_create("/", path, 0);
        path_t* end = path_create("/", path, 0);
        path_append(end, "~", 1);
        
        database_iterator_t* iter = database_scan_start(trav->db, start, end);
        path_t* out_path;
        identifier_t* out_val;
        while (database_scan_next(iter, &out_path, &out_val) == 0) {
            const char* target = extract_target(out_path, trav->is_out);
            vertex_set_add(&results, target);
            path_destroy(out_path);
            identifier_destroy(out_val);
        }
        database_scan_end(iter);
        path_destroy(start);
        path_destroy(end);
    }
    
    *output = results;
    vertex_set_destroy(&inputs);
    return 0;
}
```

### Limit Operator

```c
typedef struct {
    operator_vtable_t vtable;
    operator_t* input;
    int limit;
    int emitted;
} limit_operator_t;

int limit_operator_next(operator_t* op, vertex_set_t* output) {
    limit_operator_t* lim = (limit_operator_t*)op;
    if (lim->emitted >= lim->limit) return 1;  // done
    
    vertex_set_t inputs;
    lim->input->vtable->next(lim->input, &inputs);
    
    // Truncate
    int remaining = lim->limit - lim->emitted;
    if (inputs.count > remaining) {
        vertex_set_truncate(&inputs, remaining);
    }
    lim->emitted += inputs.count;
    
    *output = inputs;
    return 0;
}
```

### Streaming vs. Materialization

The executor can operate in two modes:

1. **Eager (materialize)** — each operator fully consumes its input before producing output. Simple, uses memory.

2. **Lazy (streaming)** — operators pull from inputs on demand. More complex but memory-efficient for large datasets.

Start with eager (it's simpler and your datasets are bounded). Add streaming later if needed.

---

## 5. Schema Layer Integration

This becomes a new schema layer in WaveDB, alongside the existing GraphQL layer:

```c
#include "Layers/graph/graph.h"

// Create the graph layer on top of a WaveDB instance
graph_layer_t* layer = graph_layer_create("/path/to/db", NULL);

// Define schema (optional, enables index optimization)
const char* schema = 
    "type Clip @index(spo, pos) {\n"
    "  tagged_with: [Tag] @reverse\n"
    "  created_by: User\n"
    "  name: String @index(pos)\n"
    "  created_at: DateTime\n"
    "}\n"
    "type User @index(spo) {\n"
    "  follows: [User]\n"
    "  likes: [Clip]\n"
    "  name: String @index(pos)\n"
    "}\n"
    "type Tag @index(spo, pos) {\n"
    "  name: String @index(pos)\n"
    "}";

graph_schema_parse(layer, schema, NULL);

// Insert triples
graph_insert(layer, "clip_abc", "tagged_with", "gaming");
graph_insert(layer, "clip_abc", "tagged_with", "tutorial");
graph_insert(layer, "clip_abc", "created_by", "alice");
graph_insert(layer, "clip_abc", "name", "\"My Awesome Clip\"");
graph_insert(layer, "alice", "follows", "bob");
graph_insert(layer, "bob", "likes", "clip_abc");

// Query
graph_query_t* query = graph_query(layer);
graph_query_vertex(query, "gaming");
graph_query_in(query, "tagged_with");
graph_query_and(query);
graph_query_vertex(query, "tutorial");
graph_query_in(query, "tagged_with");
graph_query_end_and(query);
graph_query_out(query, "created_by");
graph_query_out(query, "name");
graph_query_limit(query, 20);

graph_result_t* result = graph_query_execute(query);
// result contains: ["Alice"]

graph_result_destroy(result);
graph_query_destroy(query);
graph_layer_destroy(layer);
```

### Gremlin-Like JavaScript API

```c
// The JavaScript engine (Duktape, QuickJS, etc.) exposes:
// g.V(id) → query builder
// .Out(predicate) → traversal
// .In(predicate) → reverse traversal
// .And(query) → intersection
// .Or(query) → union
// .Has(predicate, value) → filter
// .Follow(morphism) → named path
// .All() → execute and return all
// .GetLimit(n) → execute and return first n
// .Count() → execute and return count

const char* js_query = 
    "g.V('gaming').In('tagged_with')"
    "  .And(g.V('tutorial').In('tagged_with'))"
    "  .Out('created_by').Out('name').All()";

graph_result_t* result = graph_query_js(layer, js_query);
```

---

## 6. P2P Integration

The graph layer integrates naturally with your P2P sharding:

### Sharding by Predicate

```plaintext
# Node A owns /spo/ and /pso/ indices
/spo/clip_abc/tagged_with/gaming
/pso/tagged_with/clip_abc/gaming

# Node B owns /pos/ and /osp/ indices  
/pos/tagged_with/gaming/clip_abc
/osp/gaming/clip_abc/tagged_with
```

The query planner routes scans to the appropriate nodes based on the index needed.

### Sharding by Subject Prefix

```plaintext
# Node A: subjects starting with a-m
/spo/alice/follows/bob
/spo/bob/likes/clip_abc

# Node B: subjects starting with n-z
/spo/nina/follows/alice
```

The planner fans out scans to all relevant nodes and merges results.

### Sharding by Tag (your original idea)

```plaintext
# Node A: /pos/tagged_with/gaming/...
# Node B: /pos/tagged_with/tutorial/...
```

This is the most natural for your media player use case — each tag's data lives on nodes interested in that tag.

---

## 7. Performance Considerations

### Index Write Amplification

Each triple write updates multiple indices:

Indices Maintained	Write Ops	Query Coverage
SPO only	1	Subject-centric queries only
SPO + POS	2	Subject + predicate lookups
SPO + POS + PSO	3	+ predicate-to-subject scans
All four	4	Full coverage

**Recommendation**: Start with **SPO + POS**. Add more indices based on actual query patterns. The schema definition can specify which indices to maintain per predicate:

```javascript
// In schema
"type Clip @index(spo, pos) { ... }"
"type User @index(spo) { ... }"  // only SPO, fewer writes
```

### Batch Inserts

For bulk loading, batch the index writes:

```c
// Insert a triple across all indices atomically
batch_t* batch = batch_create(0);

// SPO
path_t* p1 = path_create("/", "spo/clip_abc/tagged_with/gaming", 0);
batch_add_put(batch, p1, identifier_create_empty());
path_destroy(p1);

// POS
path_t* p2 = path_create("/", "pos/tagged_with/gaming/clip_abc", 0);
batch_add_put(batch, p2, identifier_create_empty());
path_destroy(p2);

database_write_batch_sync(db, batch);
batch_destroy(batch);
```

Your existing batch API handles this atomically.

### Query Optimization

The planner should apply simple optimizations:

1. **Push limits down** — apply `.GetLimit(10)` as early as possible
2. **Choose cheapest index** — prefer POS over SPO for `In()` traversals
3. **Reorder intersections** — process the smallest set first
4. **Cache hot scans** — frequently accessed indices stay in the LRU cache

---

## 8. Implementation Roadmap

### Phase 1: Core Index + Scan (1-2 weeks)

- [ ] Implement the triple insert function (writes SPO + POS)
- [ ] Implement the scan plan structure
- [ ] Implement `Out()` operator (SPO scan)
- [ ] Implement `In()` operator (POS scan)
- [ ] Implement `Vertex()` operator (single vertex set)
- [ ] Wire up as a C API

### Phase 2: Set Operations (1 week)

- [ ] Intersection operator
- [ ] Union operator
- [ ] Difference operator
- [ ] Limit operator
- [ ] Count operator

### Phase 3: Query Language (2 weeks)

- [ ] Implement the Gremlin-like chain API in C
- [ ] Implement the query planner (AST → scan DAG)
- [ ] Implement the executor (run the DAG)
- [ ] JavaScript bindings (Duktape or QuickJS)
- [ ] Node.js bindings
- [ ] Dart bindings

### Phase 4: Schema & Optimization (1-2 weeks)

- [ ] Schema definition parser
- [ ] Index selection based on schema
- [ ] Query optimizer (push limits, reorder intersections)
- [ ] Morphism support (named paths)

### Phase 5: P2P Integration (ongoing)

- [ ] Route scans to appropriate P2P nodes
- [ ] Fan-out queries across shards
- [ ] Merge results from multiple nodes

---

## 9. The Big Picture

```plaintext
┌──────────────────────────────────────────────────────────┐
│                    APPLICATION                            │
│  g.V("gaming").In("tagged_with").And(...).Out("name").All()│
├──────────────────────────────────────────────────────────┤
│              GRAPH SCHEMA LAYER (new)                    │
│  ┌──────────┐  ┌──────────┐  ┌──────────────────────┐   │
│  │  Parser  │→ │  Planner │→ │  Executor            │   │
│  │ (Gremlin │  │ (decom-  │  │ (scan + combine)     │   │
│  │  + JS)   │  │ pose)    │  │                      │   │
│  └──────────┘  └──────────┘  └──────┬───────────────┘   │
├──────────────────────────────────────┼───────────────────┤
│              WaveDB Core             │                   │
│  ┌───────────────────────────────────┴──────┐            │
│  │  HBTrie                                  │            │
│  │  /spo/clip_abc/tagged_with/gaming        │            │
│  │  /pos/tagged_with/gaming/clip_abc        │            │
│  │  MVCC | WAL | LRU Cache                  │            │
│  └──────────────────────────────────────────┘            │
├──────────────────────────────────────────────────────────┤
│              P2P Replication Layer                        │
│  (shard by prefix, route scans, merge results)           │
└──────────────────────────────────────────────────────────┘
```

This gives you:
- **One storage engine** (WaveDB) — no second database
- **Rich graph queries** — multi-hop, intersections, unions
- **Your existing performance** — all queries decompose to prefix scans
- **Natural P2P sharding** — shard by index prefix or tag namespace
- **Schema-driven** — define which indices to maintain per predicate

The graph layer doesn't replace WaveDB — it **sits on top** and translates graph operations into the prefix scans WaveDB already excels at. It's exactly the "schema layer" pattern you already have, just purpose-built for graph-shaped data.

---

Want me to write the actual C header file and implementation skeleton for the graph layer API? I can produce the `graph.h` and `graph.c` starting points.