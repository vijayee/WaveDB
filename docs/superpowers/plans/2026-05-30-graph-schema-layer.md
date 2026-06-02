# Graph Schema Layer Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement a graph schema layer on top of WaveDB that encodes RDF triples (subject, predicate, object) as hierarchical paths and supports Gremlin-inspired graph traversals with set operations.

**Architecture:** The graph layer sits on top of `database_t`, encoding triples across SPO and POS indices as prefix paths. Queries are built as chains of steps (Vertex, Out, In, Intersect, Union, Limit) and executed as a pipeline that decomposes traversals into prefix scans against the appropriate index. Vertex sets use hash-based O(1) lookup for efficient set operations.

**Tech Stack:** C11 (same as WaveDB core), WaveDB raw API (`database_put_sync_raw`, `database_scan_sync_raw`, `database_batch_sync_raw`), libcbor, CMake build system

---

## File Structure

```
src/Layers/graph/
├── graph.h              — Public API: graph_layer_t, graph_query_t, graph_result_t, all exported functions
├── graph_internal.h     — Internal shared types: vertex_set_t, query_step_t, scan helpers
├── graph.c              — Layer lifecycle, triple insert/delete, query builder, executor
├── graph_set.c          — Hash-based vertex set + set operations (intersect, union)
└── graph_ops.c          — Operator execution (Out/In scan, Limit)

tests/test_graph.cpp    — Full test suite (gtest, C++ test harness)
```

### Type Overview

```
graph_layer_t {
    database_t* db;
}

query_step_t (linked list node):
    type: VERTEX | OUT | IN | INTERSECT | UNION | LIMIT
    vertex_id / predicate / limit
    children[] — for INTERSECT/UNION owning child step-lists
    next — next step in chain

graph_query_t {
    layer, head, tail step pointers
}

vertex_set_t {
    vertices[] — string array
    buckets[]  — hash table (open addressing, UINT32_MAX = empty)
    count, capacity, num_buckets
}

graph_result_t {
    vertex_set_t set
}
```

---

### Task 1: Vertex Set Type + Internal Headers

**Files:**
- Create: `src/Layers/graph/graph_internal.h`
- Create: `src/Layers/graph/graph_set.h` (if separate from internal.h)
- Create: `src/Layers/graph/graph_set.c`

- [ ] **Step 1: Write graph_internal.h**

```c
//
// Created by victor on 05/30/26.
//

#ifndef WAVEDB_GRAPH_INTERNAL_H
#define WAVEDB_GRAPH_INTERNAL_H

#include "graph.h"
#include "../../Util/allocator.h"
#include <string.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Vertex Set (hash-based) ── */

#define VERTEX_SET_EMPTY UINT32_MAX

typedef struct {
    char** vertices;       // String array (owned strings)
    size_t count;          // Number of entries
    size_t capacity;       // Allocated vertex slots
    uint32_t* buckets;     // Hash table: index into vertices[], VERTEX_SET_EMPTY = empty
    uint32_t num_buckets;  // Power of 2
} vertex_set_t;

void vertex_set_init(vertex_set_t* set, size_t initial_capacity);
void vertex_set_destroy(vertex_set_t* set);
int vertex_set_add(vertex_set_t* set, const char* vertex);
int vertex_set_contains(vertex_set_t* set, const char* vertex);
int vertex_set_intersect(vertex_set_t* result, const vertex_set_t* a, const vertex_set_t* b);
int vertex_set_union(vertex_set_t* result, const vertex_set_t* a, const vertex_set_t* b);
void vertex_set_clear(vertex_set_t* set);
int vertex_set_copy(vertex_set_t* result, const vertex_set_t* src);

/* ── Query Step (internal) ── */

typedef struct query_step_t query_step_t;

struct query_step_t {
    graph_step_type_t type;
    char* vertex_id;                // For GRAPH_STEP_VERTEX
    char* predicate;                // For GRAPH_STEP_OUT, GRAPH_STEP_IN
    size_t limit;                   // For GRAPH_STEP_LIMIT
    query_step_t** children;        // For GRAPH_STEP_INTERSECT, GRAPH_STEP_UNION
    size_t num_children;
    query_step_t* next;             // Linked list
};

/* ── Operator execution ── */

// Execute a chain of steps starting from an empty seed set.
// Returns the result vertex_set (caller must destroy via vertex_set_destroy).
int graph_execute_chain(database_t* db, query_step_t* steps, vertex_set_t* output);

// Single-step execution on an input set.
int graph_execute_vertex(database_t* db, query_step_t* step, vertex_set_t* output);
int graph_execute_out(database_t* db, const vertex_set_t* input, const char* predicate, vertex_set_t* output);
int graph_execute_in(database_t* db, const vertex_set_t* input, const char* predicate, vertex_set_t* output);

#ifdef __cplusplus
}
#endif

#endif // WAVEDB_GRAPH_INTERNAL_H
```

- [ ] **Step 2: Write the vertex_set implementation in graph_set.c**

```c
//
// Created by victor on 05/30/26.
//

#include "graph_internal.h"

// FNV-1a hash for strings
static uint32_t str_hash(const char* s) {
    uint32_t h = 2166136261u;
    while (*s) {
        h ^= (unsigned char)*s++;
        h *= 16777619u;
    }
    return h;
}

// Power-of-2 ceiling
static size_t round_pow2(size_t v) {
    v--;
    v |= v >> 1; v |= v >> 2; v |= v >> 4;
    v |= v >> 8; v |= v >> 16;
    return v + 1;
}

// Find bucket index for a vertex. Returns the first empty or matching bucket.
// Uses open addressing with linear probing.
static uint32_t find_bucket(const vertex_set_t* set, const char* vertex) {
    if (set->num_buckets == 0) return VERTEX_SET_EMPTY;
    uint32_t h = str_hash(vertex);
    uint32_t idx = h & (set->num_buckets - 1);
    for (uint32_t i = 0; i < set->num_buckets; i++) {
        uint32_t b = (idx + i) & (set->num_buckets - 1);
        if (set->buckets[b] == VERTEX_SET_EMPTY) return b;
        if (strcmp(set->vertices[set->buckets[b]], vertex) == 0) return b;
    }
    return VERTEX_SET_EMPTY;
}

void vertex_set_init(vertex_set_t* set, size_t initial_capacity) {
    memset(set, 0, sizeof(*set));
    if (initial_capacity < 8) initial_capacity = 8;
    set->capacity = round_pow2(initial_capacity);
    set->vertices = (char**)get_clear_memory(set->capacity * sizeof(char*));
    set->num_buckets = (uint32_t)(set->capacity * 2);
    set->buckets = (uint32_t*)get_clear_memory(set->num_buckets * sizeof(uint32_t));
    for (uint32_t i = 0; i < set->num_buckets; i++) {
        set->buckets[i] = VERTEX_SET_EMPTY;
    }
    set->count = 0;
}

void vertex_set_destroy(vertex_set_t* set) {
    if (!set) return;
    for (size_t i = 0; i < set->count; i++) {
        free(set->vertices[i]);
    }
    free(set->vertices);
    free(set->buckets);
    memset(set, 0, sizeof(*set));
}

static int vertex_set_resize(vertex_set_t* set, size_t new_capacity) {
    // Save old state
    char** old_vertices = set->vertices;
    size_t old_count = set->count;
    size_t old_capacity = set->capacity;
    uint32_t* old_buckets = set->buckets;
    uint32_t old_num_buckets = set->num_buckets;

    // Init new state
    set->capacity = round_pow2(new_capacity);
    set->vertices = (char**)get_clear_memory(set->capacity * sizeof(char*));
    set->num_buckets = (uint32_t)(set->capacity * 2);
    set->buckets = (uint32_t*)get_clear_memory(set->num_buckets * sizeof(uint32_t));
    for (uint32_t i = 0; i < set->num_buckets; i++) {
        set->buckets[i] = VERTEX_SET_EMPTY;
    }
    set->count = 0;

    // Re-insert old entries
    for (size_t i = 0; i < old_count; i++) {
        vertex_set_add(set, old_vertices[i]);
        free(old_vertices[i]);
    }
    free(old_vertices);
    free(old_buckets);
    return 0;
}

int vertex_set_add(vertex_set_t* set, const char* vertex) {
    if (!set || !vertex) return -1;

    // Check duplicates
    uint32_t b = find_bucket(set, vertex);
    if (b != VERTEX_SET_EMPTY && set->buckets[b] != VERTEX_SET_EMPTY) {
        // Already present — the bucket has a valid index in it
        return 0; // Not an error, just already present
    }

    // Resize if needed (load factor > 0.75)
    if (set->count >= set->capacity * 3 / 4) {
        vertex_set_resize(set, set->capacity * 2);
        b = find_bucket(set, vertex); // Re-find after resize
    }

    // Store string
    size_t len = strlen(vertex);
    set->vertices[set->count] = (char*)get_memory(len + 1);
    memcpy(set->vertices[set->count], vertex, len + 1);

    // Insert into hash table
    if (b == VERTEX_SET_EMPTY) {
        // Linear probe from hash position
        uint32_t h = str_hash(vertex);
        uint32_t idx = h & (set->num_buckets - 1);
        for (uint32_t i = 0; i < set->num_buckets; i++) {
            b = (idx + i) & (set->num_buckets - 1);
            if (set->buckets[b] == VERTEX_SET_EMPTY) break;
        }
        if (b == VERTEX_SET_EMPTY) return -1; // Should not happen
    }
    set->buckets[b] = (uint32_t)set->count;
    set->count++;
    return 0;
}

int vertex_set_contains(vertex_set_t* set, const char* vertex) {
    if (!set || !vertex || set->count == 0) return 0;
    uint32_t h = str_hash(vertex);
    uint32_t idx = h & (set->num_buckets - 1);
    for (uint32_t i = 0; i < set->num_buckets; i++) {
        uint32_t b = (idx + i) & (set->num_buckets - 1);
        if (set->buckets[b] == VERTEX_SET_EMPTY) return 0;
        if (strcmp(set->vertices[set->buckets[b]], vertex) == 0) return 1;
    }
    return 0;
}

int vertex_set_intersect(vertex_set_t* result, const vertex_set_t* a, const vertex_set_t* b) {
    if (!result || !a || !b) return -1;
    vertex_set_clear(result);
    // Iterate over the smaller set for fewer hash lookups
    const vertex_set_t* smaller = (a->count <= b->count) ? a : b;
    const vertex_set_t* larger  = (a->count <= b->count) ? b : a;
    for (size_t i = 0; i < smaller->count; i++) {
        if (vertex_set_contains((vertex_set_t*)larger, smaller->vertices[i])) {
            vertex_set_add(result, smaller->vertices[i]);
        }
    }
    return 0;
}

int vertex_set_union(vertex_set_t* result, const vertex_set_t* a, const vertex_set_t* b) {
    if (!result || !a || !b) return -1;
    vertex_set_clear(result);
    for (size_t i = 0; i < a->count; i++) {
        vertex_set_add(result, a->vertices[i]);
    }
    for (size_t i = 0; i < b->count; i++) {
        vertex_set_add(result, b->vertices[i]);
    }
    return 0;
}

void vertex_set_clear(vertex_set_t* set) {
    if (!set) return;
    for (size_t i = 0; i < set->count; i++) {
        free(set->vertices[i]);
        set->vertices[i] = NULL;
    }
    set->count = 0;
    for (uint32_t i = 0; i < set->num_buckets; i++) {
        set->buckets[i] = VERTEX_SET_EMPTY;
    }
}

int vertex_set_copy(vertex_set_t* result, const vertex_set_t* src) {
    if (!result || !src) return -1;
    vertex_set_clear(result);
    for (size_t i = 0; i < src->count; i++) {
        if (vertex_set_add(result, src->vertices[i]) != 0) return -1;
    }
    return 0;
}
```

- [ ] **Step 3: Write a quick standalone smoke test (C, in tests/)**

Create `tests/test_graph_set.c`:

```c
#include <stdio.h>
#include <string.h>
#include "../src/Layers/graph/graph_internal.h"

static int tests_passed = 0, tests_failed = 0;
#define TEST(name, expr) do { \
    if (!(expr)) { fprintf(stderr, "FAIL: %s\n", name); tests_failed++; } \
    else { tests_passed++; printf("PASS: %s\n", name); } \
} while(0)

int main(void) {
    vertex_set_t set;
    vertex_set_init(&set, 8);

    TEST("empty set has count 0", set.count == 0);
    TEST("contains returns 0 for missing", vertex_set_contains(&set, "alice") == 0);

    vertex_set_add(&set, "alice");
    TEST("add increases count", set.count == 1);
    TEST("contains finds added item", vertex_set_contains(&set, "alice") == 1);
    TEST("contains returns 0 for different", vertex_set_contains(&set, "bob") == 0);

    // Duplicate add
    vertex_set_add(&set, "alice");
    TEST("duplicate add does not increase count", set.count == 1);

    // Bulk add
    vertex_set_add(&set, "bob");
    vertex_set_add(&set, "charlie");
    TEST("multiple adds work", set.count == 3);

    // Intersection
    vertex_set_t a, b, result;
    vertex_set_init(&a, 8);
    vertex_set_init(&b, 8);
    vertex_set_init(&result, 8);
    vertex_set_add(&a, "alice"); vertex_set_add(&a, "bob"); vertex_set_add(&a, "charlie");
    vertex_set_add(&b, "bob"); vertex_set_add(&b, "dave");
    vertex_set_intersect(&result, &a, &b);
    TEST("intersect has correct count", result.count == 1);
    TEST("intersect contains bob", vertex_set_contains(&result, "bob") == 1);
    TEST("intersect does not contain alice", vertex_set_contains(&result, "alice") == 0);

    // Union
    vertex_set_t uresult;
    vertex_set_init(&uresult, 8);
    vertex_set_union(&uresult, &a, &b);
    TEST("union has correct count", uresult.count == 4);
    TEST("union contains alice", vertex_set_contains(&uresult, "alice") == 1);
    TEST("union contains dave", vertex_set_contains(&uresult, "dave") == 1);

    vertex_set_destroy(&a);
    vertex_set_destroy(&b);
    vertex_set_destroy(&result);
    vertex_set_destroy(&uresult);
    vertex_set_destroy(&set);

    printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
```

- [ ] **Step 4: Compile and run the smoke test**

```bash
cd build && cmake .. -DBUILD_TESTS=ON && make test_graph_set && ./test_graph_set
```

Build manually if not yet in CMake. Expected: all PASS.

- [ ] **Step 5: Commit**

```bash
git add src/Layers/graph/graph_internal.h src/Layers/graph/graph_set.c tests/test_graph_set.c
git commit -m "feat: add vertex set type with hash-based lookup and set operations"
```

---

### Task 2: Graph Layer Public API Header (graph.h)

**Files:**
- Create: `src/Layers/graph/graph.h`

- [ ] **Step 1: Write graph.h**

```c
//
// Created by victor on 05/30/26.
//

#ifndef WAVEDB_GRAPH_H
#define WAVEDB_GRAPH_H

#include <stdint.h>
#include <stddef.h>
#include "../../Database/database.h"
#include "../../Workers/promise.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Opaque types ── */

typedef struct graph_layer_t graph_layer_t;
typedef struct graph_query_t graph_query_t;
typedef struct graph_result_t graph_result_t;

/* ── Step types for query builder ── */

typedef enum {
    GRAPH_STEP_VERTEX,      // Start from a single vertex
    GRAPH_STEP_OUT,         // Traverse outgoing edges (SPO scan)
    GRAPH_STEP_IN,          // Traverse incoming edges (POS scan)
    GRAPH_STEP_INTERSECT,   // Intersection of sub-query results
    GRAPH_STEP_UNION,       // Union of sub-query results
    GRAPH_STEP_LIMIT,       // Cap result count
} graph_step_type_t;

/* ── Layer lifecycle ── */

graph_layer_t* graph_layer_create(const char* path, database_config_t* config);
void graph_layer_destroy(graph_layer_t* layer);
database_t* graph_layer_get_db(graph_layer_t* layer);

/* ── Triple operations (sync) ── */

int graph_insert_sync(graph_layer_t* layer, const char* s, const char* p, const char* o);
int graph_delete_sync(graph_layer_t* layer, const char* s, const char* p, const char* o);

/* ── Triple operations (async) ── */

void graph_insert(graph_layer_t* layer, const char* s, const char* p, const char* o, promise_t* promise);
void graph_delete(graph_layer_t* layer, const char* s, const char* p, const char* o, promise_t* promise);

/* ── Query builder ── */

graph_query_t* graph_query_create(graph_layer_t* layer);
void graph_query_destroy(graph_query_t* q);

int graph_query_vertex(graph_query_t* q, const char* id);
int graph_query_out(graph_query_t* q, const char* predicate);
int graph_query_in(graph_query_t* q, const char* predicate);
int graph_query_intersect(graph_query_t* q, graph_query_t* left, graph_query_t* right);
int graph_query_union(graph_query_t* q, graph_query_t* left, graph_query_t* right);
int graph_query_limit(graph_query_t* q, size_t limit);

/* ── Execution (sync) ── */

graph_result_t* graph_query_execute_sync(graph_query_t* q);

/* ── Execution (async) ── */

void graph_query_execute(graph_query_t* q, promise_t* promise);

/* ── Result handling ── */

size_t graph_result_count(graph_result_t* r);
const char* const* graph_result_vertices(graph_result_t* r);
void graph_result_destroy(graph_result_t* r);

#ifdef __cplusplus
}
#endif

#endif // WAVEDB_GRAPH_H
```

- [ ] **Step 2: Commit**

```bash
git add src/Layers/graph/graph.h
git commit -m "feat: add graph layer public API header"
```

---

### Task 3: Graph Layer Lifecycle + Triple Insert/Delete

**Files:**
- Create: `src/Layers/graph/graph.c`

- [ ] **Step 1: Write graph.c with layer lifecycle and triple operations**

```c
//
// Created by victor on 05/30/26.
//

#include "graph_internal.h"
#include "../../Util/allocator.h"
#include <string.h>
#include <stdio.h>

/* ── Forward declarations ── */

static void graph_execute_work(void* ctx);

/* ── Helpers ── */

// Build a raw path string for a triple in a given index.
// Returns length written (excluding null), or -1 on truncation.
static int build_index_path(char* buf, size_t buf_size,
                            const char* index_name,
                            const char* c1, const char* c2, const char* c3) {
    return snprintf(buf, buf_size, "/%s/%s/%s/%s", index_name, c1, c2, c3);
}

/* ── Layer lifecycle ── */

struct graph_layer_t {
    database_t* db;
};

graph_layer_t* graph_layer_create(const char* path, database_config_t* config) {
    graph_layer_t* layer = (graph_layer_t*)get_clear_memory(sizeof(graph_layer_t));
    int error_code = 0;
    if (config) {
        layer->db = database_create_with_config(path, config, &error_code);
    } else {
        layer->db = database_create(path, 0, NULL, 0, 0, 0, NULL, NULL, &error_code);
    }
    if (!layer->db) {
        free(layer);
        return NULL;
    }
    return layer;
}

void graph_layer_destroy(graph_layer_t* layer) {
    if (!layer) return;
    if (layer->db) {
        database_destroy(layer->db);
    }
    free(layer);
}

database_t* graph_layer_get_db(graph_layer_t* layer) {
    return layer ? layer->db : NULL;
}

/* ── Triple operations (sync) ── */

int graph_insert_sync(graph_layer_t* layer, const char* s, const char* p, const char* o) {
    if (!layer || !s || !p || !o) return -1;

    char path[1024];
    uint8_t empty_val = 0;

    // SPO: /spo/<s>/<p>/<o>
    build_index_path(path, sizeof(path), "spo", s, p, o);
    if (database_put_sync_raw(layer->db, path, strlen(path), '/', &empty_val, 0) != 0) return -1;

    // POS: /pos/<p>/<o>/<s>
    build_index_path(path, sizeof(path), "pos", p, o, s);
    if (database_put_sync_raw(layer->db, path, strlen(path), '/', &empty_val, 0) != 0) return -1;

    return 0;
}

int graph_delete_sync(graph_layer_t* layer, const char* s, const char* p, const char* o) {
    if (!layer || !s || !p || !o) return -1;

    char path[1024];

    // SPO
    build_index_path(path, sizeof(path), "spo", s, p, o);
    database_delete_sync_raw(layer->db, path, strlen(path), '/');

    // POS
    build_index_path(path, sizeof(path), "pos", p, o, s);
    database_delete_sync_raw(layer->db, path, strlen(path), '/');

    return 0;
}

/* ── Triple operations (async) ── */

typedef struct {
    graph_layer_t* layer;
    char* s;
    char* p;
    char* o;
    int is_delete;
    promise_t* promise;
} triple_work_ctx_t;

static void triple_work_execute(void* ctx) {
    triple_work_ctx_t* tc = (triple_work_ctx_t*)ctx;
    int result;
    if (tc->is_delete) {
        result = graph_delete_sync(tc->layer, tc->s, tc->p, tc->o);
    } else {
        result = graph_insert_sync(tc->layer, tc->s, tc->p, tc->o);
    }
    // Resolve the promise with the result
    int* res = (int*)get_memory(sizeof(int));
    *res = result;
    promise_resolve(tc->promise, res, sizeof(int));
    // Cleanup
    free(tc->s); free(tc->p); free(tc->o);
    promise_destroy(tc->promise);
    free(tc);
}

static void triple_work_abort(void* ctx) {
    triple_work_ctx_t* tc = (triple_work_ctx_t*)ctx;
    async_error_t* err = async_error_create("Operation aborted", __FILE__, __FUNCTION__, __LINE__);
    promise_reject(tc->promise, err);
    async_error_destroy(err);
    free(tc->s); free(tc->p); free(tc->o);
    promise_destroy(tc->promise);
    free(tc);
}

void graph_insert(graph_layer_t* layer, const char* s, const char* p, const char* o, promise_t* promise) {
    if (!layer || !s || !p || !o) {
        async_error_t* err = async_error_create("NULL argument", __FILE__, __FUNCTION__, __LINE__);
        promise_reject(promise, err);
        async_error_destroy(err);
        return;
    }
    triple_work_ctx_t* tc = (triple_work_ctx_t*)get_clear_memory(sizeof(triple_work_ctx_t));
    tc->layer = layer;
    tc->s = strdup(s); tc->p = strdup(p); tc->o = strdup(o);
    tc->is_delete = 0;
    tc->promise = promise_reference(promise);

    priority_t prio = {0};
    work_t* work = work_create(prio, tc, triple_work_execute, triple_work_abort);

    // Enqueue via the database's work pool
    work_pool_t* pool = layer->db->pool;
    if (pool) {
        refcounter_yield((refcounter_t*)work);
        work_pool_enqueue(pool, work);
    } else {
        triple_work_execute(tc);
        work_destroy(work);
    }
}

void graph_delete(graph_layer_t* layer, const char* s, const char* p, const char* o, promise_t* promise) {
    if (!layer || !s || !p || !o) {
        async_error_t* err = async_error_create("NULL argument", __FILE__, __FUNCTION__, __LINE__);
        promise_reject(promise, err);
        async_error_destroy(err);
        return;
    }
    triple_work_ctx_t* tc = (triple_work_ctx_t*)get_clear_memory(sizeof(triple_work_ctx_t));
    tc->layer = layer;
    tc->s = strdup(s); tc->p = strdup(p); tc->o = strdup(o);
    tc->is_delete = 1;
    tc->promise = promise_reference(promise);

    priority_t prio = {0};
    work_t* work = work_create(prio, tc, triple_work_execute, triple_work_abort);

    work_pool_t* pool = layer->db->pool;
    if (pool) {
        refcounter_yield((refcounter_t*)work);
        work_pool_enqueue(pool, work);
    } else {
        triple_work_execute(tc);
        work_destroy(work);
    }
}
```

- [ ] **Step 2: Commit**

```bash
git add src/Layers/graph/graph.c
git commit -m "feat: add graph layer lifecycle and triple insert/delete"
```

---

### Task 4: SPO/POS Scan Operators (graph_ops.c)

**Files:**
- Create: `src/Layers/graph/graph_ops.c`

- [ ] **Step 1: Write graph_ops.c with scan-based traversal operators**

```c
//
// Created by victor on 05/30/26.
//

#include "graph_internal.h"
#include "../../Util/allocator.h"
#include <string.h>
#include <stdio.h>

/* ── SPO scan: /spo/<subject>/<predicate>/ → collect objects ── */

int graph_execute_out(database_t* db, const vertex_set_t* input,
                       const char* predicate, vertex_set_t* output) {
    if (!db || !input || !predicate || !output) return -1;

    for (size_t i = 0; i < input->count; i++) {
        char prefix[1024];
        int len = snprintf(prefix, sizeof(prefix), "/spo/%s/%s/", input->vertices[i], predicate);
        if (len < 0 || (size_t)len >= sizeof(prefix)) continue;

        raw_result_t* results = NULL;
        size_t count = 0;
        int rc = database_scan_sync_raw(db, prefix, strlen(prefix), '/', &results, &count);
        if (rc != 0) continue;

        for (size_t j = 0; j < count; j++) {
            // Result key is: /spo/<subject>/<predicate>/<object>
            // Extract the object (last component after the prefix)
            const char* obj = results[j].key + strlen(prefix);
            vertex_set_add(output, obj);
        }
        database_raw_results_free(results, count);
    }
    return 0;
}

/* ── POS scan: /pos/<predicate>/<object>/ → collect subjects ── */

int graph_execute_in(database_t* db, const vertex_set_t* input,
                      const char* predicate, vertex_set_t* output) {
    if (!db || !input || !predicate || !output) return -1;

    for (size_t i = 0; i < input->count; i++) {
        char prefix[1024];
        int len = snprintf(prefix, sizeof(prefix), "/pos/%s/%s/", predicate, input->vertices[i]);
        if (len < 0 || (size_t)len >= sizeof(prefix)) continue;

        raw_result_t* results = NULL;
        size_t count = 0;
        int rc = database_scan_sync_raw(db, prefix, strlen(prefix), '/', &results, &count);
        if (rc != 0) continue;

        for (size_t j = 0; j < count; j++) {
            // Result key is: /pos/<predicate>/<object>/<subject>
            // Extract the subject (last component after the prefix)
            const char* subj = results[j].key + strlen(prefix);
            vertex_set_add(output, subj);
        }
        database_raw_results_free(results, count);
    }
    return 0;
}

/* ── Vertex step: produce a singleton set ── */

int graph_execute_vertex(database_t* db, query_step_t* step, vertex_set_t* output) {
    (void)db;
    if (!step || !step->vertex_id || !output) return -1;
    vertex_set_add(output, step->vertex_id);
    return 0;
}

/* ── Step chain execution ── */

// Forward declaration
static int execute_single_step(database_t* db, query_step_t* step,
                                vertex_set_t* input, vertex_set_t* output);

// Execute a list of child steps (for INTERSECT/UNION children).
// Each child list starts from nothing (Vertex step typically).
static int execute_child_steps(database_t* db, query_step_t* steps, vertex_set_t* output) {
    vertex_set_t current;
    vertex_set_init(&current, 8);
    int first = 1;

    query_step_t* s = steps;
    while (s) {
        vertex_set_t next;
        vertex_set_init(&next, 8);

        if (s->type == GRAPH_STEP_VERTEX && first) {
            graph_execute_vertex(db, s, &next);
        } else if (s->type == GRAPH_STEP_OUT) {
            graph_execute_out(db, &current, s->predicate, &next);
        } else if (s->type == GRAPH_STEP_IN) {
            graph_execute_in(db, &current, s->predicate, &next);
        } else if (s->type == GRAPH_STEP_LIMIT) {
            vertex_set_copy(&next, &current);
            // Truncate if over limit
            if (next.count > s->limit) {
                next.count = s->limit;
            }
        } else if (s->type == GRAPH_STEP_INTERSECT || s->type == GRAPH_STEP_UNION) {
            // Execute children and combine
            vertex_set_t combined;
            vertex_set_init(&combined, 8);
            int first_child = 1;
            for (size_t ci = 0; ci < s->num_children; ci++) {
                vertex_set_t child_result;
                vertex_set_init(&child_result, 8);
                execute_child_steps(db, s->children[ci], &child_result);
                if (first_child) {
                    vertex_set_copy(&combined, &child_result);
                    first_child = 0;
                } else {
                    vertex_set_t tmp;
                    vertex_set_init(&tmp, 8);
                    if (s->type == GRAPH_STEP_INTERSECT) {
                        vertex_set_intersect(&tmp, &combined, &child_result);
                    } else {
                        vertex_set_union(&tmp, &combined, &child_result);
                    }
                    vertex_set_destroy(&combined);
                    memcpy(&combined, &tmp, sizeof(vertex_set_t));
                }
                vertex_set_destroy(&child_result);
            }
            // If there's a current input and this isn't the first step,
            // intersect/union with current
            if (first && current.count > 0) {
                vertex_set_t tmp;
                vertex_set_init(&tmp, 8);
                if (s->type == GRAPH_STEP_INTERSECT) {
                    vertex_set_intersect(&tmp, &current, &combined);
                } else {
                    vertex_set_union(&tmp, &current, &combined);
                }
                vertex_set_destroy(&combined);
                memcpy(&combined, &tmp, sizeof(vertex_set_t));
            }
            vertex_set_copy(&next, &combined);
            vertex_set_destroy(&combined);
        }

        vertex_set_destroy(&current);
        memcpy(&current, &next, sizeof(vertex_set_t));
        s = s->next;
        first = 0;
    }

    vertex_set_copy(output, &current);
    vertex_set_destroy(&current);
    return 0;
}

int graph_execute_chain(database_t* db, query_step_t* steps, vertex_set_t* output) {
    if (!db || !steps || !output) return -1;
    return execute_child_steps(db, steps, output);
}
```

- [ ] **Step 2: Commit**

```bash
git add src/Layers/graph/graph_ops.c
git commit -m "feat: add SPO/POS scan operators and step chain execution"
```

---

### Task 5: Query Builder + Executor (graph.c continuation)

- [ ] **Step 1: Append query builder and executor code to graph.c**

Add at the end of `graph.c` (before the closing of the file):

```c
/* ── Query builder ── */

struct graph_query_t {
    graph_layer_t* layer;
    query_step_t* head;   // First step in chain
    query_step_t* tail;   // Last step in chain (for O(1) append)
};

graph_query_t* graph_query_create(graph_layer_t* layer) {
    if (!layer) return NULL;
    graph_query_t* q = (graph_query_t*)get_clear_memory(sizeof(graph_query_t));
    q->layer = layer;
    return q;
}

void graph_query_destroy(graph_query_t* q) {
    if (!q) return;
    // Free all steps recursively
    query_step_t* s = q->head;
    while (s) {
        query_step_t* next = s->next;
        free(s->vertex_id);
        free(s->predicate);
        for (size_t i = 0; i < s->num_children; i++) {
            // Recursively free child step chains
            query_step_t* cs = s->children[i];
            while (cs) {
                query_step_t* cn = cs->next;
                free(cs->vertex_id);
                free(cs->predicate);
                free(cs->children);
                free(cs);
                cs = cn;
            }
        }
        free(s->children);
        free(s);
        s = next;
    }
    free(q);
}

static query_step_t* alloc_step(graph_step_type_t type) {
    query_step_t* s = (query_step_t*)get_clear_memory(sizeof(query_step_t));
    s->type = type;
    return s;
}

static int append_step(graph_query_t* q, query_step_t* s) {
    if (!q || !s) return -1;
    if (q->tail) {
        q->tail->next = s;
    } else {
        q->head = s;
    }
    q->tail = s;
    return 0;
}

int graph_query_vertex(graph_query_t* q, const char* id) {
    if (!q || !id) return -1;
    query_step_t* s = alloc_step(GRAPH_STEP_VERTEX);
    if (!s) return -1;
    s->vertex_id = strdup(id);
    return append_step(q, s);
}

int graph_query_out(graph_query_t* q, const char* predicate) {
    if (!q || !predicate) return -1;
    query_step_t* s = alloc_step(GRAPH_STEP_OUT);
    if (!s) return -1;
    s->predicate = strdup(predicate);
    return append_step(q, s);
}

int graph_query_in(graph_query_t* q, const char* predicate) {
    if (!q || !predicate) return -1;
    query_step_t* s = alloc_step(GRAPH_STEP_IN);
    if (!s) return -1;
    s->predicate = strdup(predicate);
    return append_step(q, s);
}

int graph_query_intersect(graph_query_t* q, graph_query_t* left, graph_query_t* right) {
    if (!q || !left || !right) return -1;
    query_step_t* s = alloc_step(GRAPH_STEP_INTERSECT);
    if (!s) return -1;
    s->num_children = 2;
    s->children = (query_step_t**)get_clear_memory(2 * sizeof(query_step_t*));
    // Transfer ownership of left/right step chains
    s->children[0] = left->head;
    s->children[1] = right->head;
    // Null the source queries so they don't free the steps
    left->head = NULL; left->tail = NULL;
    right->head = NULL; right->tail = NULL;
    return append_step(q, s);
}

int graph_query_union(graph_query_t* q, graph_query_t* left, graph_query_t* right) {
    if (!q || !left || !right) return -1;
    query_step_t* s = alloc_step(GRAPH_STEP_UNION);
    if (!s) return -1;
    s->num_children = 2;
    s->children = (query_step_t**)get_clear_memory(2 * sizeof(query_step_t*));
    s->children[0] = left->head;
    s->children[1] = right->head;
    left->head = NULL; left->tail = NULL;
    right->head = NULL; right->tail = NULL;
    return append_step(q, s);
}

int graph_query_limit(graph_query_t* q, size_t limit) {
    if (!q) return -1;
    query_step_t* s = alloc_step(GRAPH_STEP_LIMIT);
    if (!s) return -1;
    s->limit = limit;
    return append_step(q, s);
}

/* ── Execution (sync) ── */

graph_result_t* graph_query_execute_sync(graph_query_t* q) {
    if (!q || !q->head) return NULL;

    graph_result_t* r = (graph_result_t*)get_clear_memory(sizeof(graph_result_t));
    vertex_set_init(&r->set, 64);

    query_step_t* steps = q->head;
    // We need to clone the step chain for execution (to avoid modifying the query)
    // Actually, execution is read-only on the step chain, so we can use it directly.
    int rc = graph_execute_chain(q->layer->db, steps, &r->set);
    if (rc != 0) {
        vertex_set_destroy(&r->set);
        free(r);
        return NULL;
    }

    return r;
}

/* ── Execution (async) ── */

typedef struct {
    graph_query_t* q;
    promise_t* promise;
} query_work_ctx_t;

static void query_work_execute(void* ctx) {
    query_work_ctx_t* qc = (query_work_ctx_t*)ctx;
    graph_result_t* result = graph_query_execute_sync(qc->q);
    if (result) {
        promise_resolve(qc->promise, result, sizeof(graph_result_t*));
    } else {
        async_error_t* err = async_error_create("Query execution failed", __FILE__, __FUNCTION__, __LINE__);
        promise_reject(qc->promise, err);
        async_error_destroy(err);
    }
    promise_destroy(qc->promise);
    free(qc);
}

static void query_work_abort(void* ctx) {
    query_work_ctx_t* qc = (query_work_ctx_t*)ctx;
    async_error_t* err = async_error_create("Query aborted", __FILE__, __FUNCTION__, __LINE__);
    promise_reject(qc->promise, err);
    async_error_destroy(err);
    promise_destroy(qc->promise);
    free(qc);
}

void graph_query_execute(graph_query_t* q, promise_t* promise) {
    if (!q || !promise) return;
    query_work_ctx_t* qc = (query_work_ctx_t*)get_clear_memory(sizeof(query_work_ctx_t));
    qc->q = q;
    qc->promise = promise_reference(promise);

    priority_t prio = {0};
    work_t* work = work_create(prio, qc, query_work_execute, query_work_abort);

    work_pool_t* pool = q->layer->db->pool;
    if (pool) {
        refcounter_yield((refcounter_t*)work);
        work_pool_enqueue(pool, work);
    } else {
        query_work_execute(qc);
        work_destroy(work);
    }
}

/* ── Result handling ── */

struct graph_result_t {
    vertex_set_t set;
};

size_t graph_result_count(graph_result_t* r) {
    return r ? r->set.count : 0;
}

const char* const* graph_result_vertices(graph_result_t* r) {
    return r ? (const char* const*)r->set.vertices : NULL;
}

void graph_result_destroy(graph_result_t* r) {
    if (!r) return;
    vertex_set_destroy(&r->set);
    free(r);
}
```

- [ ] **Step 2: Commit**

```bash
git add src/Layers/graph/graph.c
git commit -m "feat: add query builder, executor, and result handling"
```

---

### Task 6: CMake Integration + Test Suite

**Files:**
- Modify: `CMakeLists.txt`
- Create: `tests/test_graph.cpp`

- [ ] **Step 1: Add graph layer sources to CMakeLists.txt**

Edit `CMakeLists.txt` in the root. Add after the GraphQL layer section (around line 115):

```cmake
    # Layers - Graph
    src/Layers/graph/graph.c
    src/Layers/graph/graph_set.c
    src/Layers/graph/graph_ops.c
```

Add the test (after the GraphQL tests, around line 338):

```cmake
    # Test for Graph Layer
    add_executable(test_graph tests/test_graph.cpp)
    target_link_libraries(test_graph wavedb gtest gtest_main)
    add_test(NAME test_graph COMMAND test_graph)
```

- [ ] **Step 2: Write the full test suite in tests/test_graph.cpp**

```cpp
//
// Tests for Graph Schema Layer
//

#include <gtest/gtest.h>
#include <string.h>
#include <stdlib.h>

extern "C" {
#include "../src/Layers/graph/graph.h"
#include "../src/Layers/graph/graph_internal.h"
}

class GraphLayerTest : public ::testing::Test {
protected:
    void SetUp() override {
        layer = graph_layer_create(NULL, NULL);
        ASSERT_NE(layer, nullptr);
    }

    void TearDown() override {
        graph_layer_destroy(layer);
    }

    graph_layer_t* layer = nullptr;
};

TEST_F(GraphLayerTest, CreateDestroy) {
    // Layer was created in SetUp, just verify it exists
    ASSERT_NE(layer, nullptr);
    database_t* db = graph_layer_get_db(layer);
    ASSERT_NE(db, nullptr);
}

TEST_F(GraphLayerTest, InsertAndDelete) {
    int rc = graph_insert_sync(layer, "clip_abc", "tagged_with", "gaming");
    ASSERT_EQ(rc, 0);

    rc = graph_insert_sync(layer, "clip_abc", "tagged_with", "tutorial");
    ASSERT_EQ(rc, 0);

    rc = graph_insert_sync(layer, "clip_abc", "created_by", "alice");
    ASSERT_EQ(rc, 0);

    rc = graph_delete_sync(layer, "clip_abc", "tagged_with", "tutorial");
    ASSERT_EQ(rc, 0);
}

TEST_F(GraphLayerTest, SimpleOutTraversal) {
    graph_insert_sync(layer, "clip_abc", "tagged_with", "gaming");
    graph_insert_sync(layer, "clip_abc", "tagged_with", "tutorial");
    graph_insert_sync(layer, "clip_abc", "created_by", "alice");

    graph_query_t* q = graph_query_create(layer);
    graph_query_vertex(q, "clip_abc");
    graph_query_out(q, "tagged_with");

    graph_result_t* r = graph_query_execute_sync(q);
    ASSERT_NE(r, nullptr);
    ASSERT_EQ(graph_result_count(r), (size_t)2);

    // Collect results for checking
    const char* const* verts = graph_result_vertices(r);
    bool found_gaming = false, found_tutorial = false;
    for (size_t i = 0; i < graph_result_count(r); i++) {
        if (strcmp(verts[i], "gaming") == 0) found_gaming = true;
        if (strcmp(verts[i], "tutorial") == 0) found_tutorial = true;
    }
    EXPECT_TRUE(found_gaming);
    EXPECT_TRUE(found_tutorial);

    graph_result_destroy(r);
    graph_query_destroy(q);
}

TEST_F(GraphLayerTest, SimpleInTraversal) {
    graph_insert_sync(layer, "clip_abc", "tagged_with", "gaming");
    graph_insert_sync(layer, "clip_xyz", "tagged_with", "gaming");

    graph_query_t* q = graph_query_create(layer);
    graph_query_vertex(q, "gaming");
    graph_query_in(q, "tagged_with");

    graph_result_t* r = graph_query_execute_sync(q);
    ASSERT_NE(r, nullptr);
    ASSERT_EQ(graph_result_count(r), (size_t)2);

    const char* const* verts = graph_result_vertices(r);
    bool found_abc = false, found_xyz = false;
    for (size_t i = 0; i < graph_result_count(r); i++) {
        if (strcmp(verts[i], "clip_abc") == 0) found_abc = true;
        if (strcmp(verts[i], "clip_xyz") == 0) found_xyz = true;
    }
    EXPECT_TRUE(found_abc);
    EXPECT_TRUE(found_xyz);

    graph_result_destroy(r);
    graph_query_destroy(q);
}

TEST_F(GraphLayerTest, MultiHopTraversal) {
    // alice --follows--> bob --likes--> clip_abc
    graph_insert_sync(layer, "alice", "follows", "bob");
    graph_insert_sync(layer, "bob", "likes", "clip_abc");

    graph_query_t* q = graph_query_create(layer);
    graph_query_vertex(q, "alice");
    graph_query_out(q, "follows");
    graph_query_out(q, "likes");

    graph_result_t* r = graph_query_execute_sync(q);
    ASSERT_NE(r, nullptr);
    ASSERT_EQ(graph_result_count(r), (size_t)1);
    EXPECT_STREQ(graph_result_vertices(r)[0], "clip_abc");

    graph_result_destroy(r);
    graph_query_destroy(q);
}

TEST_F(GraphLayerTest, Intersection) {
    // clip_abc tagged_with gaming AND tutorial
    // clip_xyz tagged_with gaming only
    graph_insert_sync(layer, "clip_abc", "tagged_with", "gaming");
    graph_insert_sync(layer, "clip_abc", "tagged_with", "tutorial");
    graph_insert_sync(layer, "clip_xyz", "tagged_with", "gaming");

    // Build: intersect(
    //   V("gaming").In("tagged_with"),
    //   V("tutorial").In("tagged_with")
    // )
    graph_query_t* q1 = graph_query_create(layer);
    graph_query_vertex(q1, "gaming");
    graph_query_in(q1, "tagged_with");

    graph_query_t* q2 = graph_query_create(layer);
    graph_query_vertex(q2, "tutorial");
    graph_query_in(q2, "tagged_with");

    graph_query_t* q = graph_query_create(layer);
    graph_query_intersect(q, q1, q2);

    graph_result_t* r = graph_query_execute_sync(q);
    ASSERT_NE(r, nullptr);
    ASSERT_EQ(graph_result_count(r), (size_t)1);
    EXPECT_STREQ(graph_result_vertices(r)[0], "clip_abc");

    graph_result_destroy(r);
    graph_query_destroy(q1); // q1 and q2 were consumed by q, but their structs still exist
    graph_query_destroy(q2);
    graph_query_destroy(q);
}

TEST_F(GraphLayerTest, Union) {
    graph_insert_sync(layer, "clip_abc", "tagged_with", "gaming");
    graph_insert_sync(layer, "clip_xyz", "tagged_with", "tutorial");

    graph_query_t* q1 = graph_query_create(layer);
    graph_query_vertex(q1, "gaming");
    graph_query_in(q1, "tagged_with");

    graph_query_t* q2 = graph_query_create(layer);
    graph_query_vertex(q2, "tutorial");
    graph_query_in(q2, "tagged_with");

    graph_query_t* q = graph_query_create(layer);
    graph_query_union(q, q1, q2);

    graph_result_t* r = graph_query_execute_sync(q);
    ASSERT_NE(r, nullptr);
    ASSERT_EQ(graph_result_count(r), (size_t)2);

    graph_result_destroy(r);
    graph_query_destroy(q1);
    graph_query_destroy(q2);
    graph_query_destroy(q);
}

TEST_F(GraphLayerTest, Limit) {
    graph_insert_sync(layer, "clip_abc", "tagged_with", "gaming");
    graph_insert_sync(layer, "clip_xyz", "tagged_with", "gaming");

    graph_query_t* q = graph_query_create(layer);
    graph_query_vertex(q, "gaming");
    graph_query_in(q, "tagged_with");
    graph_query_limit(q, 1);

    graph_result_t* r = graph_query_execute_sync(q);
    ASSERT_NE(r, nullptr);
    ASSERT_EQ(graph_result_count(r), (size_t)1);

    graph_result_destroy(r);
    graph_query_destroy(q);
}

TEST_F(GraphLayerTest, EmptyResult) {
    graph_query_t* q = graph_query_create(layer);
    graph_query_vertex(q, "nonexistent");
    graph_query_out(q, "unknown");

    graph_result_t* r = graph_query_execute_sync(q);
    ASSERT_NE(r, nullptr);
    ASSERT_EQ(graph_result_count(r), (size_t)0);

    graph_result_destroy(r);
    graph_query_destroy(q);
}

TEST_F(GraphLayerTest, IntersectionEmpty) {
    graph_insert_sync(layer, "clip_abc", "tagged_with", "gaming");

    graph_query_t* q1 = graph_query_create(layer);
    graph_query_vertex(q1, "gaming");
    graph_query_in(q1, "tagged_with");

    graph_query_t* q2 = graph_query_create(layer);
    graph_query_vertex(q2, "nonexistent");
    graph_query_in(q2, "tagged_with");

    graph_query_t* q = graph_query_create(layer);
    graph_query_intersect(q, q1, q2);

    graph_result_t* r = graph_query_execute_sync(q);
    ASSERT_NE(r, nullptr);
    ASSERT_EQ(graph_result_count(r), (size_t)0);

    graph_result_destroy(r);
    graph_query_destroy(q1);
    graph_query_destroy(q2);
    graph_query_destroy(q);
}

TEST_F(GraphLayerTest, IntersectionThenOut) {
    // clip_abc tagged_with gaming, created_by alice
    // clip_xyz tagged_with gaming, created_by bob
    graph_insert_sync(layer, "clip_abc", "tagged_with", "gaming");
    graph_insert_sync(layer, "clip_abc", "created_by", "alice");
    graph_insert_sync(layer, "clip_xyz", "tagged_with", "gaming");
    graph_insert_sync(layer, "clip_xyz", "created_by", "bob");

    // Find content tagged "gaming" AND "tutorial", then get creator
    // (no clip has both, so result should be empty)
    graph_query_t* q1 = graph_query_create(layer);
    graph_query_vertex(q1, "gaming");
    graph_query_in(q1, "tagged_with");

    graph_query_t* q2 = graph_query_create(layer);
    graph_query_vertex(q2, "tutorial");
    graph_query_in(q2, "tagged_with");

    graph_query_t* q = graph_query_create(layer);
    graph_query_intersect(q, q1, q2);
    graph_query_out(q, "created_by");

    graph_result_t* r = graph_query_execute_sync(q);
    ASSERT_NE(r, nullptr);
    ASSERT_EQ(graph_result_count(r), (size_t)0);

    graph_result_destroy(r);
    graph_query_destroy(q1);
    graph_query_destroy(q2);
    graph_query_destroy(q);
}
```

- [ ] **Step 3: Build and run tests**

```bash
cd build && cmake .. -DBUILD_TESTS=ON && make test_graph && ./test_graph
```

Expected: All tests pass.

- [ ] **Step 4: Commit**

```bash
git add CMakeLists.txt tests/test_graph.cpp
git commit -m "feat: add graph layer tests and CMake integration"
```

---

### Task 7: Async Execution Test

**Files:**
- Modify: `tests/test_graph.cpp`

- [ ] **Step 1: Add async tests to test_graph.cpp**

Add these includes at the top of the test file:
```cpp
#include "../src/Workers/pool.h"
#include "../src/Time/wheel.h"
#include <future>
#include <chrono>
```

Add a new test fixture for async tests:
```cpp
class GraphLayerAsyncTest : public ::testing::Test {
protected:
    void SetUp() override {
        pool = work_pool_create(platform_core_count());
        work_pool_launch(pool);
        wheel = hierarchical_timing_wheel_create(8, pool);
        hierarchical_timing_wheel_run(wheel);

        database_config_t config = database_config_default();
        config.worker_threads = platform_core_count();
        config.external_pool = pool;
        config.external_wheel = wheel;

        int error = 0;
        db = database_create_with_config(NULL, &config, &error);
        ASSERT_NE(db, nullptr);

        layer = graph_layer_create(NULL, &config);
        // Override the layer's db to use our configured one
        // (Since graph_layer_create creates its own, we use the config approach)
    }

    void TearDown() override {
        graph_layer_destroy(layer);

        if (db) {
            database_destroy(db);
        }

        if (wheel) {
            hierarchical_timing_wheel_wait_for_idle_signal(wheel);
            hierarchical_timing_wheel_stop(wheel);
            hierarchical_timing_wheel_destroy(wheel);
        }
        if (pool) {
            work_pool_shutdown(pool);
            work_pool_join_all(pool);
            work_pool_destroy(pool);
        }
    }

    graph_layer_t* layer = nullptr;
    database_t* db = nullptr;
    work_pool_t* pool = nullptr;
    hierarchical_timing_wheel_t* wheel = nullptr;
};
```

Add the async test:
```cpp
TEST_F(GraphLayerAsyncTest, AsyncInsert) {
    auto p = std::make_shared<std::promise<int>>();
    auto fut = p->get_future();

    promise_t* cpromise = promise_create(
        [](void* ctx, void* payload) {
            auto* prom = static_cast<std::promise<int>*>(ctx);
            prom->set_value(*(int*)payload);
        },
        [](void* ctx, async_error_t* err) {
            auto* prom = static_cast<std::promise<int>*>(ctx);
            prom->set_value(-1);
            async_error_destroy(err);
        },
        p.get()
    );

    graph_insert(layer, "clip_abc", "tagged_with", "gaming", cpromise);

    auto status = fut.wait_for(std::chrono::seconds(5));
    ASSERT_EQ(status, std::future_status::ready);
    int result = fut.get();
    EXPECT_EQ(result, 0);
}
```

Actually, the async test is complex to set up because graph_layer_create creates its own database. Let me simplify the async test by adding a synchronous wrapper test instead. For now, the sync tests cover the core functionality. Async is just wrapping sync in a work item.

Let me revise: keep the sync-only tests for Task 6 (they're the meat), and add a simpler async test that uses the graph_layer's internal db pool (if one exists). But since graph_layer_create(NULL, NULL) creates an in-memory db with no pool, async ops would execute inline.

Let me revise the test to be more practical:

```cpp
TEST_F(GraphLayerTest, AsyncInsertExecutesInline) {
    // With no pool, async falls back to inline execution
    auto p = std::make_shared<std::promise<int>>();
    auto fut = p->get_future();

    promise_t* cpromise = promise_create(
        [](void* ctx, void* payload) {
            auto* prom = static_cast<std::promise<int>*>(ctx);
            prom->set_value(*(int*)payload);
        },
        [](void* ctx, async_error_t* err) {
            auto* prom = static_cast<std::promise<int>*>(ctx);
            prom->set_value(-1);
            async_error_destroy(err);
        },
        p.get()
    );

    graph_insert(layer, "clip_abc", "tagged_with", "gaming", cpromise);

    auto status = fut.wait_for(std::chrono::seconds(1));
    ASSERT_EQ(status, std::future_status::ready);
    int result = fut.get();
    EXPECT_EQ(result, 0);

    // Verify the data was inserted
    graph_query_t* q = graph_query_create(layer);
    graph_query_vertex(q, "clip_abc");
    graph_query_out(q, "tagged_with");
    graph_result_t* r = graph_query_execute_sync(q);
    ASSERT_NE(r, nullptr);
    ASSERT_EQ(graph_result_count(r), (size_t)1);
    EXPECT_STREQ(graph_result_vertices(r)[0], "gaming");
    graph_result_destroy(r);
    graph_query_destroy(q);
}
```

- [ ] **Step 2: Build and run the full test suite**

```bash
cd build && cmake .. -DBUILD_TESTS=ON && make test_graph && ./test_graph
```

- [ ] **Step 3: Commit**

```bash
git add tests/test_graph.cpp
git commit -m "test: add async execution test for graph layer"
```

---

### Task 8: Self-Review and Cleanup

- [ ] **Step 1: Remove the standalone test_graph_set.c from Task 1**

Since the full test_graph.cpp covers vertex_set functionality:

```bash
git rm tests/test_graph_set.c
```

- [ ] **Step 2: Run final test suite**

```bash
cd build && cmake .. -DBUILD_TESTS=ON && make test_graph && ./test_graph
```

Expected: All tests pass.

- [ ] **Step 3: Final commit**

```bash
git add -A
git commit -m "chore: cleanup standalone set test, all graph tests passing"
```
