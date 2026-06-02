# Graph DSL Parser Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement a hand-written recursive descent parser that compiles Gremlin-inspired DSL strings into the existing `graph_query_t` step chain, plus the Has operator and in-memory morphism support.

**Architecture:** The parser is a single C file (`graph_parser.c`, ~400 lines) with zero dependencies. It reads a string like `g.V("alice").Out("follows").All()`, calls the public API (`graph_query_vertex`, `graph_query_out`, etc.) to build the step chain, and returns a `graph_query_t*`. Has and Follow are expanded at parse time using existing step types. Morphisms are stored in-memory on the layer struct.

**Tech Stack:** C11, existing graph layer API

---

## File Structure

```
src/Layers/graph/
├── graph.h              — MODIFY: add GRAPH_STEP_HAS, parse/morphism API, morphism_entry_t forward, graph_parse_error_t
├── graph_internal.h     — MODIFY: add morphism_entry_t, has_predicate/has_value fields to query_step_t, graph_execute_has declaration
├── graph.c              — MODIFY: morphism lifecycle in create/destroy, parse function implementations
├── graph_parser.c       — CREATE: recursive descent parser
├── graph_ops.c          — MODIFY: add GRAPH_STEP_HAS handler in execute_child_steps

tests/
├── test_graph_parser.cpp  — CREATE: test suite
├── CMakeLists.txt         — MODIFY: add parser test target
```

---

### Task 1: Extend Types for Has + Morphisms

**Files:**
- Modify: `src/Layers/graph/graph.h`
- Modify: `src/Layers/graph/graph_internal.h`

- [ ] **Step 1: Add GRAPH_STEP_HAS and graph_parse_error_t to graph.h**

In `graph.h`, add `GRAPH_STEP_HAS` to the enum and declare the parse/morphism API:

```c
typedef enum {
    GRAPH_STEP_VERTEX,
    GRAPH_STEP_OUT,
    GRAPH_STEP_IN,
    GRAPH_STEP_INTERSECT,
    GRAPH_STEP_UNION,
    GRAPH_STEP_LIMIT,
    GRAPH_STEP_HAS,       // filter: intersect with POS scan of (predicate, value)
} graph_step_type_t;
```

Add before the closing `extern "C"`:

```c
/* ── Parse error handling ── */

typedef struct {
    int ok;             // 1 = success, 0 = error
    int position;       // Character position of error (0-based)
    char message[256];  // Error message
} graph_parse_error_t;

/* ── DSL parser ── */

graph_query_t* graph_parse(const char* dsl, graph_layer_t* layer, graph_parse_error_t* error);
graph_result_t* graph_parse_execute(const char* dsl, graph_layer_t* layer, graph_parse_error_t* error);
int graph_parse_count(const char* dsl, graph_layer_t* layer, size_t* count, graph_parse_error_t* error);

/* ── Morphisms ── */

int graph_morphism_define(graph_layer_t* layer, const char* name, const char* dsl, graph_parse_error_t* error);
```

- [ ] **Step 2: Add morphism_entry_t and has fields to graph_internal.h**

In `graph_internal.h`, add the morphism type and extends `query_step_t`:

```c
/* ── Morphism storage ── */

typedef struct {
    char* name;
    query_step_t* steps;
} morphism_entry_t;
```

Extend `query_step_t` to add Has fields:

```c
struct query_step_t {
    graph_step_type_t type;
    char* vertex_id;                // For GRAPH_STEP_VERTEX
    char* predicate;                // For GRAPH_STEP_OUT, GRAPH_STEP_IN
    char* has_predicate;            // For GRAPH_STEP_HAS
    char* has_value;                // For GRAPH_STEP_HAS
    size_t limit;                   // For GRAPH_STEP_LIMIT
    query_step_t** children;        // For GRAPH_STEP_INTERSECT, GRAPH_STEP_UNION
    size_t num_children;
    query_step_t* next;             // Linked list
};
```

Add the `graph_execute_has` declaration:

```c
int graph_execute_has(database_t* db, const vertex_set_t* input,
                       const char* predicate, const char* value,
                       vertex_set_t* output);
```

- [ ] **Step 3: Commit**

```bash
git add src/Layers/graph/graph.h src/Layers/graph/graph_internal.h
git commit -m "feat: add GRAPH_STEP_HAS, morphism types, and parse API declarations"
```

---

### Task 2: Morphism Lifecycle + Parse Functions in graph.c

**Files:**
- Modify: `src/Layers/graph/graph.c`

- [ ] **Step 1: Extend graph_layer_t struct and modify lifecycle**

Update the struct in `graph.c`:

```c
struct graph_layer_t {
    database_t* db;
    morphism_entry_t* morphisms;
    size_t morphism_count;
    size_t morphism_capacity;
};
```

Update `graph_layer_create` to initialize morphisms:

```c
graph_layer_t* graph_layer_create(const char* path, database_config_t* config) {
    graph_layer_t* layer = (graph_layer_t*)get_clear_memory(sizeof(graph_layer_t));
    int error_code = 0;
    // ... existing db creation ...
    layer->morphisms = NULL;
    layer->morphism_count = 0;
    layer->morphism_capacity = 0;
    return layer;
}
```

Update `graph_layer_destroy` to free morphisms:

```c
void graph_layer_destroy(graph_layer_t* layer) {
    if (!layer) return;
    if (layer->db) {
        database_destroy(layer->db);
    }
    for (size_t i = 0; i < layer->morphism_count; i++) {
        free(layer->morphisms[i].name);
        query_step_t* s = layer->morphisms[i].steps;
        while (s) {
            query_step_t* next = s->next;
            free(s->vertex_id);
            free(s->predicate);
            free(s->has_predicate);
            free(s->has_value);
            for (size_t j = 0; j < s->num_children; j++) {
                query_step_t* cs = s->children[j];
                while (cs) {
                    query_step_t* cn = cs->next;
                    free(cs->vertex_id);
                    free(cs->predicate);
                    free(cs->has_predicate);
                    free(cs->has_value);
                    free(cs->children);
                    free(cs);
                    cs = cn;
                }
            }
            free(s->children);
            free(s);
            s = next;
        }
    }
    free(layer->morphisms);
    free(layer);
}
```

Also update `graph_query_destroy` to free the `has_predicate` and `has_value` fields. Find the existing loops that free `s->vertex_id` and `s->predicate`, and add `free(s->has_predicate); free(s->has_value);` right after `free(s->predicate);`.

The existing free loops are at lines ~198 and ~204 (inner child loop). Both need the new frees.

- [ ] **Step 2: Add the graph_parse and graph_morphism_define stubs**

Add at the end of `graph.c` (before the closing `}` of the file):

```c
/* ── DSL parser (implementation in graph_parser.c) ── */

graph_query_t* graph_parse(const char* dsl, graph_layer_t* layer, graph_parse_error_t* error) {
    if (!dsl || !layer) {
        if (error) { error->ok = 0; snprintf(error->message, sizeof(error->message), "NULL argument"); error->position = 0; }
        return NULL;
    }
    // Forwarded to graph_parse_query in graph_parser.c
    extern graph_query_t* graph_parse_query(const char* input, size_t len, graph_layer_t* layer, graph_parse_error_t* error);
    return graph_parse_query(dsl, strlen(dsl), layer, error);
}

graph_result_t* graph_parse_execute(const char* dsl, graph_layer_t* layer, graph_parse_error_t* error) {
    graph_query_t* q = graph_parse(dsl, layer, error);
    if (!q) return NULL;
    graph_result_t* r = graph_query_execute_sync(q);
    graph_query_destroy(q);
    return r;
}

int graph_parse_count(const char* dsl, graph_layer_t* layer, size_t* count, graph_parse_error_t* error) {
    if (!count) return -1;
    graph_result_t* r = graph_parse_execute(dsl, layer, error);
    if (!r) return -1;
    *count = graph_result_count(r);
    graph_result_destroy(r);
    return 0;
}

int graph_morphism_define(graph_layer_t* layer, const char* name, const char* dsl, graph_parse_error_t* error) {
    if (!layer || !name || !dsl) {
        if (error) { error->ok = 0; snprintf(error->message, sizeof(error->message), "NULL argument"); error->position = 0; }
        return -1;
    }

    // Parse the morphism DSL (which starts with g.Morphism("name").steps...
    // But morphism definitions don't start with "g." — they start with ".Out(...)" etc.
    // Actually, looking at the spec: g.Morphism("name").Out("pred").This is a full query parse.
    // Let's just parse the step part after the name.
    // The DSL format is: g.Morphism("name").Out("created_by")
    // We parse the whole thing and extract the steps after the Morphism() call.

    graph_parse_error_t local_error;
    if (!error) error = &local_error;

    // Parse the DSL string. The parser in graph_parser.c will handle this,
    // extracting the steps after g.Morphism("name") and storing them.
    extern int graph_morphism_parse_and_store(graph_layer_t* layer, const char* name,
                                               const char* input, size_t len,
                                               graph_parse_error_t* error);
    return graph_morphism_parse_and_store(layer, name, dsl, strlen(dsl), error);
}
```

Wait, the forward-declaration approach between .c files is fragile. Let me instead declare `graph_parse_query` and `graph_morphism_parse_and_store` in `graph_internal.h` instead:

- [ ] **Step 3: Fix approach — declare parser functions in graph_internal.h instead of extern in graph.c**

Add to `graph_internal.h`:

```c
/* ── Parser internals (implemented in graph_parser.c) ── */

// Parse a DSL query string. Returns NULL on error.
graph_query_t* graph_parse_query(const char* input, size_t len, graph_layer_t* layer, graph_parse_error_t* error);

// Parse a morphism definition DSL and store it on the layer.
int graph_morphism_parse_and_store(graph_layer_t* layer, const char* name,
                                    const char* input, size_t len,
                                    graph_parse_error_t* error);
```

- [ ] **Step 4: Simplify graph.c parse functions (remove extern forward decls, call internal functions)**

Replace the stubs added in Step 2 with cleaner versions:

```c
/* ── DSL parser ── */

graph_query_t* graph_parse(const char* dsl, graph_layer_t* layer, graph_parse_error_t* error) {
    if (!dsl || !layer) {
        if (error) { error->ok = 0; snprintf(error->message, sizeof(error->message), "NULL argument"); error->position = 0; }
        return NULL;
    }
    return graph_parse_query(dsl, strlen(dsl), layer, error);
}

graph_result_t* graph_parse_execute(const char* dsl, graph_layer_t* layer, graph_parse_error_t* error) {
    graph_query_t* q = graph_parse(dsl, layer, error);
    if (!q) return NULL;
    graph_result_t* r = graph_query_execute_sync(q);
    graph_query_destroy(q);
    return r;
}

int graph_parse_count(const char* dsl, graph_layer_t* layer, size_t* count, graph_parse_error_t* error) {
    if (!count) return -1;
    graph_result_t* r = graph_parse_execute(dsl, layer, error);
    if (!r) return -1;
    *count = graph_result_count(r);
    graph_result_destroy(r);
    return 0;
}

int graph_morphism_define(graph_layer_t* layer, const char* name, const char* dsl, graph_parse_error_t* error) {
    if (!layer || !name || !dsl) {
        if (error) { error->ok = 0; snprintf(error->message, sizeof(error->message), "NULL argument"); error->position = 0; }
        return -1;
    }
    return graph_morphism_parse_and_store(layer, name, dsl, strlen(dsl), error);
}
```

- [ ] **Step 5: Commit**

```bash
git add src/Layers/graph/graph_internal.h src/Layers/graph/graph.c
git commit -m "feat: add morphism lifecycle and parse API to graph layer"
```

---

### Task 3: Has Operator Execution

**Files:**
- Modify: `src/Layers/graph/graph_ops.c`

- [ ] **Step 1: Add graph_execute_has function and GRAPH_STEP_HAS handler**

Add before `graph_execute_vertex` in `graph_ops.c`:

```c
/* ── Has scan: POS scan for (predicate, value), intersect with input ── */

int graph_execute_has(database_t* db, const vertex_set_t* input,
                       const char* predicate, const char* value,
                       vertex_set_t* output) {
    if (!db || !input || !predicate || !value || !output) return -1;

    char prefix[1024];
    int len = snprintf(prefix, sizeof(prefix), "/pos/%s/%s/", predicate, value);
    if (len < 0 || (size_t)len >= sizeof(prefix)) return -1;

    raw_result_t* results = NULL;
    size_t count = 0;
    int rc = database_scan_sync_raw(db, prefix, strlen(prefix), '/', &results, &count);
    if (rc != 0) return 0; // No results = no match

    vertex_set_t pos_results;
    vertex_set_init(&pos_results, count > 0 ? count : 8);
    for (size_t j = 0; j < count; j++) {
        if (!key_starts_with_index(results[j].key, results[j].key_len, "pos"))
            continue;
        char buf[1024];
        const char* subj = key_last_component(results[j].key, results[j].key_len, buf, sizeof(buf));
        if (subj) vertex_set_add(&pos_results, subj);
    }
    database_raw_results_free(results, count);

    // Intersect with input (if input is empty, Has is the starting point)
    if (input->count == 0) {
        vertex_set_copy(output, &pos_results);
    } else {
        vertex_set_intersect(output, input, &pos_results);
    }
    vertex_set_destroy(&pos_results);
    return 0;
}
```

Add a `GRAPH_STEP_HAS` branch in `execute_child_steps` (in the if/else chain, after the LIMIT handler and before INTERSECT/UNION):

```c
        } else if (s->type == GRAPH_STEP_HAS) {
            if (first) {
                // Has as starting point: POS scan for (predicate, value)
                vertex_set_t empty;
                vertex_set_init(&empty, 0);
                graph_execute_has(db, &empty, s->has_predicate, s->has_value, &next);
                vertex_set_destroy(&empty);
            } else {
                graph_execute_has(db, &current, s->has_predicate, s->has_value, &next);
            }
```

- [ ] **Step 2: Commit**

```bash
git add src/Layers/graph/graph_ops.c
git commit -m "feat: add Has operator with POS scan and intersect"
```

---

### Task 4: Recursive Descent Parser

**Files:**
- Create: `src/Layers/graph/graph_parser.c`

- [ ] **Step 1: Write graph_parser.c**

```c
//
// Created by victor on 05/31/26.
//
// Recursive descent parser for the Gremlin-inspired graph DSL.
// Compiles strings like g.V("alice").Out("follows").All() into
// graph_query_t step chains.
//

#include "graph_internal.h"
#include <string.h>
#include <stdio.h>
#include <ctype.h>

/* ── Parser state ── */

typedef struct {
    const char* input;
    size_t pos;
    size_t len;
    graph_layer_t* layer;
    graph_parse_error_t* error;
} parser_t;

/* ── Forward declarations ── */

static int parse_query(parser_t* p, graph_query_t* q);
static int parse_step(parser_t* p, graph_query_t* q);

/* ── Helpers ── */

static void set_error(parser_t* p, const char* msg) {
    if (p->error) {
        p->error->ok = 0;
        p->error->position = (int)p->pos;
        snprintf(p->error->message, sizeof(p->error->message), "%s", msg);
    }
}

static void set_error_expect(parser_t* p, const char* expected) {
    char buf[256];
    if (p->pos < p->len) {
        char c = p->input[p->pos];
        snprintf(buf, sizeof(buf), "Expected '%s' but found '%c' at position %zu",
                 expected, c, p->pos);
    } else {
        snprintf(buf, sizeof(buf), "Expected '%s' but reached end of input", expected);
    }
    set_error(p, buf);
}

static void skip_whitespace(parser_t* p) {
    while (p->pos < p->len && isspace((unsigned char)p->input[p->pos]))
        p->pos++;
}

static int peek(parser_t* p) {
    skip_whitespace(p);
    return p->pos < p->len ? (unsigned char)p->input[p->pos] : EOF;
}

static int expect_char(parser_t* p, char c) {
    skip_whitespace(p);
    if (p->pos < p->len && p->input[p->pos] == c) {
        p->pos++;
        return 1;
    }
    char buf[8] = {0};
    buf[0] = c;
    set_error_expect(p, buf);
    return 0;
}

static int expect_string(parser_t* p, const char* s) {
    skip_whitespace(p);
    size_t slen = strlen(s);
    if (p->pos + slen <= p->len && strncmp(p->input + p->pos, s, slen) == 0) {
        p->pos += slen;
        return 1;
    }
    set_error_expect(p, s);
    return 0;
}

static char* parse_string(parser_t* p) {
    skip_whitespace(p);
    if (p->pos >= p->len || p->input[p->pos] != '"') {
        set_error_expect(p, "\"");
        return NULL;
    }
    p->pos++; // skip opening quote

    size_t start = p->pos;
    while (p->pos < p->len && p->input[p->pos] != '"') {
        p->pos++;
    }
    if (p->pos >= p->len) {
        set_error(p, "Unterminated string");
        return NULL;
    }

    size_t slen = p->pos - start;
    char* s = (char*)get_memory(slen + 1);
    memcpy(s, p->input + start, slen);
    s[slen] = '\0';
    p->pos++; // skip closing quote
    return s;
}

static long parse_number(parser_t* p) {
    skip_whitespace(p);
    if (p->pos >= p->len || !isdigit((unsigned char)p->input[p->pos])) {
        set_error_expect(p, "number");
        return -1;
    }
    long n = 0;
    while (p->pos < p->len && isdigit((unsigned char)p->input[p->pos])) {
        n = n * 10 + (p->input[p->pos] - '0');
        p->pos++;
    }
    return n;
}

static char* parse_identifier(parser_t* p) {
    skip_whitespace(p);
    size_t start = p->pos;
    while (p->pos < p->len && (isalpha((unsigned char)p->input[p->pos]) || p->input[p->pos] == '_')) {
        p->pos++;
    }
    if (p->pos == start) return NULL;
    size_t slen = p->pos - start;
    char* s = (char*)get_memory(slen + 1);
    memcpy(s, p->input + start, slen);
    s[slen] = '\0';
    return s;
}

/* ── Parse parens and arguments ── */

// Parses ( ... ) with args for And/Or
// For And/Or the content is a nested query: And(g.V(x).In(y))
static int parse_paren_query(parser_t* p, graph_query_t* q) {
    if (!expect_char(p, '(')) return 0;
    if (!parse_query(p, q)) return 0;
    if (!expect_char(p, ')')) return 0;
    return 1;
}

/* ── Deep copy a step chain (for morphism inlining) ── */

static query_step_t* copy_steps(query_step_t* src) {
    if (!src) return NULL;
    query_step_t* head = NULL;
    query_step_t* tail = NULL;
    while (src) {
        query_step_t* s = (query_step_t*)get_clear_memory(sizeof(query_step_t));
        s->type = src->type;
        s->limit = src->limit;
        if (src->vertex_id) s->vertex_id = strdup(src->vertex_id);
        if (src->predicate) s->predicate = strdup(src->predicate);
        if (src->has_predicate) s->has_predicate = strdup(src->has_predicate);
        if (src->has_value) s->has_value = strdup(src->has_value);
        if (src->num_children > 0) {
            s->num_children = src->num_children;
            s->children = (query_step_t**)get_clear_memory(s->num_children * sizeof(query_step_t*));
            for (size_t i = 0; i < s->num_children; i++) {
                s->children[i] = copy_steps(src->children[i]);
            }
        }
        if (tail) tail->next = s;
        else head = s;
        tail = s;
        src = src->next;
    }
    return head;
}

/* ── Step dispatcher ── */

static int parse_step(parser_t* p, graph_query_t* q) {
    char* ident = parse_identifier(p);
    if (!ident) {
        set_error_expect(p, "method name");
        return 0;
    }

    int rc = 0;
    if (strcmp(ident, "V") == 0) {
        if (!expect_char(p, '(')) goto done;
        char* id = parse_string(p);
        if (!id) goto done;
        if (!expect_char(p, ')')) { free(id); goto done; }
        rc = graph_query_vertex(q, id);
        free(id);
    } else if (strcmp(ident, "Out") == 0) {
        if (!expect_char(p, '(')) goto done;
        char* pred = parse_string(p);
        if (!pred) goto done;
        if (!expect_char(p, ')')) { free(pred); goto done; }
        rc = graph_query_out(q, pred);
        free(pred);
    } else if (strcmp(ident, "In") == 0) {
        if (!expect_char(p, '(')) goto done;
        char* pred = parse_string(p);
        if (!pred) goto done;
        if (!expect_char(p, ')')) { free(pred); goto done; }
        rc = graph_query_in(q, pred);
        free(pred);
    } else if (strcmp(ident, "Has") == 0) {
        if (!expect_char(p, '(')) goto done;
        char* pred = parse_string(p);
        if (!pred) goto done;
        if (!expect_char(p, ',')) { free(pred); goto done; }
        char* val = parse_string(p);
        if (!val) { free(pred); goto done; }
        if (!expect_char(p, ')')) { free(pred); free(val); goto done; }
        // Create HAS step manually (not via public API)
        query_step_t* s = (query_step_t*)get_clear_memory(sizeof(query_step_t));
        s->type = GRAPH_STEP_HAS;
        s->has_predicate = pred;
        s->has_value = val;
        rc = 0;
        // Append to query manually
        if (q->tail) q->tail->next = s;
        else q->head = s;
        q->tail = s;
    } else if (strcmp(ident, "And") == 0) {
        if (!expect_char(p, '(')) goto done;
        // Parse left sub-query
        graph_query_t* left = graph_query_create(q->layer);
        if (!parse_query(p, left)) { graph_query_destroy(left); goto done; }
        if (!expect_char(p, ')')) { graph_query_destroy(left); goto done; }
        // And() takes only one nested query — the inner query already contains
        // the full chain for both sides if written as And(g.V(x).In(y))
        // But for And(a, b) style, we need two. The spec says And(g.V(x).In(y)).
        // Actually, And(g.V(x).In(y)) doesn't make sense — And needs two sub-queries.
        // The spec shows: And(g.V("tutorial").In("tagged_with"))
        // This means And queries wrap a single sub-query that's intersected with current.
        // But wait — that's just Apply(Intersect, subquery). And should take two.
        // Let me re-read the spec...
        // Spec: g.V("gaming").In("tagged_with").And(g.V("tutorial").In("tagged_with")).All()
        // This means: the first branch is the chain before And(), the second is inside And().
        // So And(nested) = Intersect(current, nested). Correct.
        graph_query_destroy(left);
        // Actually we need to keep left alive and transfer its steps.
        // Re-do properly:
        // Destroy left, re-create and keep its chain.
        // Hmm, the issue is graph_query_destroy will free the steps.
        // We need to transfer ownership.
        // Let me use the public API: graph_query_intersect.
        // But intersect takes two query_t and consumes their heads.
        // The current query q has steps before And().
        // We need to split: what's before And() goes to left_query, what's inside goes to right_query.
        // This is complex with the chain API. Simplest approach:
        // 1. Parse And(g.V(x).In(y)) — let the child parse into 'left'
        // 2. Create a new query, copy current chain into it (for the outer branch)
        // 3. Call graph_query_intersect(q, outer_copy, left_child)
        //
        // Even simpler: store And as a step with one child (the sub-query),
        // and the executor does: result = intersect(current, child_result).
        // That's already what execute_child_steps does for INTERSECT!
        // So we just need to create a single-child INTERSECT step.

        query_step_t* s = (query_step_t*)get_clear_memory(sizeof(query_step_t));
        s->type = GRAPH_STEP_INTERSECT;
        s->num_children = 1;
        s->children = (query_step_t**)get_clear_memory(sizeof(query_step_t*));
        s->children[0] = left->head;
        left->head = NULL;
        if (q->tail) q->tail->next = s;
        else q->head = s;
        q->tail = s;
        graph_query_destroy(left);
        rc = 0;
    } else if (strcmp(ident, "Or") == 0) {
        if (!expect_char(p, '(')) goto done;
        graph_query_t* child = graph_query_create(q->layer);
        if (!parse_query(p, child)) { graph_query_destroy(child); goto done; }
        if (!expect_char(p, ')')) { graph_query_destroy(child); goto done; }

        query_step_t* s = (query_step_t*)get_clear_memory(sizeof(query_step_t));
        s->type = GRAPH_STEP_UNION;
        s->num_children = 1;
        s->children = (query_step_t**)get_clear_memory(sizeof(query_step_t*));
        s->children[0] = child->head;
        child->head = NULL;
        if (q->tail) q->tail->next = s;
        else q->head = s;
        q->tail = s;
        graph_query_destroy(child);
        rc = 0;
    } else if (strcmp(ident, "Limit") == 0 || strcmp(ident, "GetLimit") == 0) {
        if (!expect_char(p, '(')) goto done;
        long n = parse_number(p);
        if (n < 0) goto done;
        if (!expect_char(p, ')')) goto done;
        rc = graph_query_limit(q, (size_t)n);
    } else if (strcmp(ident, "All") == 0) {
        if (!expect_char(p, '(')) goto done;
        if (!expect_char(p, ')')) goto done;
        rc = 0; // All() is a terminal — no step needed, query executes normally
    } else if (strcmp(ident, "Count") == 0) {
        if (!expect_char(p, '(')) goto done;
        if (!expect_char(p, ')')) goto done;
        rc = 0; // Count is a terminal — handled by graph_parse_count wrapper
    } else if (strcmp(ident, "Follow") == 0) {
        if (!expect_char(p, '(')) goto done;
        char* name = parse_string(p);
        if (!name) goto done;
        if (!expect_char(p, ')')) { free(name); goto done; }

        // Look up morphism
        morphism_entry_t* found = NULL;
        for (size_t i = 0; i < p->layer->morphism_count; i++) {
            if (strcmp(p->layer->morphisms[i].name, name) == 0) {
                found = &p->layer->morphisms[i];
                break;
            }
        }
        if (!found) {
            char buf[256];
            snprintf(buf, sizeof(buf), "Unknown morphism '%s'", name);
            set_error(p, buf);
            free(name);
            goto done;
        }
        free(name);

        // Deep-copy the morphism's steps into the current query
        query_step_t* copied = copy_steps(found->steps);
        // Append all copied steps
        if (copied) {
            if (q->tail) q->tail->next = copied;
            else q->head = copied;
            // Find the new tail
            query_step_t* t = copied;
            while (t->next) t = t->next;
            q->tail = t;
        }
        rc = 0;
    } else {
        char buf[256];
        snprintf(buf, sizeof(buf), "Unknown method '%s'", ident);
        set_error(p, buf);
        goto done;
    }

done:
    free(ident);
    return rc == 0;
}

/* ── Query parser: g.step().step()... ── */

static int parse_query(parser_t* p, graph_query_t* q) {
    // Expect "g"
    char* ident = parse_identifier(p);
    if (!ident) { set_error_expect(p, "g"); return 0; }
    if (strcmp(ident, "g") != 0) {
        char buf[128];
        snprintf(buf, sizeof(buf), "Expected 'g' but found '%s'", ident);
        set_error(p, buf);
        free(ident);
        return 0;
    }
    free(ident);

    // Expect "."
    if (!expect_char(p, '.')) return 0;

    // Parse steps
    while (p->pos < p->len) {
        int c = peek(p);
        if (c == EOF || c == ')') break; // End of this query (reached by And/Or sub-query)

        if (!parse_step(p, q)) return 0;

        // Check for next '.' or end
        skip_whitespace(p);
        if (p->pos < p->len && p->input[p->pos] == '.') {
            p->pos++;
        } else if (p->pos < p->len && p->input[p->pos] != ')') {
            // Not a dot and not a closing paren — error
            set_error_expect(p, "'.'");
            return 0;
        }
    }

    return 1;
}

/* ── Public API (declared in graph_internal.h) ── */

graph_query_t* graph_parse_query(const char* input, size_t len, graph_layer_t* layer, graph_parse_error_t* error) {
    parser_t p;
    p.input = input;
    p.pos = 0;
    p.len = len;
    p.layer = layer;
    p.error = error;

    if (error) {
        error->ok = 1;
        error->message[0] = '\0';
        error->position = 0;
    }

    graph_query_t* q = graph_query_create(layer);
    if (!q) {
        set_error(&p, "Failed to create query");
        return NULL;
    }

    if (len == 0) {
        set_error(&p, "Empty query");
        graph_query_destroy(q);
        return NULL;
    }

    if (!parse_query(&p, q)) {
        graph_query_destroy(q);
        return NULL;
    }

    return q;
}

int graph_morphism_parse_and_store(graph_layer_t* layer, const char* name,
                                    const char* input, size_t len,
                                    graph_parse_error_t* error) {
    // The input format is: g.Morphism("name").Out("pred").In("val")...
    // We need to skip the g.Morphism("name") part and parse the rest as steps.
    // Simplest: create a parser, consume g . Morphism ( "name" ) . then parse steps.

    parser_t p;
    p.input = input;
    p.pos = 0;
    p.len = len;
    p.layer = layer;
    p.error = error;

    if (error) {
        error->ok = 1;
        error->message[0] = '\0';
        error->position = 0;
    }

    // Expect "g"
    char* ident = parse_identifier(&p);
    if (!ident || strcmp(ident, "g") != 0) {
        free(ident);
        set_error(&p, "Morphism definition must start with 'g.Morphism(\"name\")...'");
        return -1;
    }
    free(ident);

    if (!expect_char(&p, '.')) return -1;

    // Expect "Morphism"
    char* mname = parse_identifier(&p);
    if (!mname || strcmp(mname, "Morphism") != 0) {
        free(mname);
        set_error(&p, "Expected 'Morphism'");
        return -1;
    }
    free(mname);

    if (!expect_char(&p, '(')) return -1;

    // The name inside Morphism("...") should match our parameter
    char* parsed_name = parse_string(&p);
    if (!parsed_name) return -1;
    if (strcmp(parsed_name, name) != 0) {
        free(parsed_name);
        set_error(&p, "Morphism name mismatch");
        return -1;
    }
    free(parsed_name);

    if (!expect_char(&p, ')')) return -1;

    // Parse remaining steps into a temporary query
    graph_query_t* q = graph_query_create(layer);
    while (p.pos < p.len) {
        if (!expect_char(&p, '.')) { graph_query_destroy(q); return -1; }
        if (!parse_step(&p, q)) { graph_query_destroy(q); return -1; }
    }

    // Store on layer
    if (layer->morphism_count >= layer->morphism_capacity) {
        size_t new_cap = layer->morphism_capacity ? layer->morphism_capacity * 2 : 4;
        morphism_entry_t* new_m = (morphism_entry_t*)get_clear_memory(new_cap * sizeof(morphism_entry_t));
        if (layer->morphisms) {
            memcpy(new_m, layer->morphisms, layer->morphism_count * sizeof(morphism_entry_t));
            free(layer->morphisms);
        }
        layer->morphisms = new_m;
        layer->morphism_capacity = new_cap;
    }

    size_t idx = layer->morphism_count;
    layer->morphisms[idx].name = strdup(name);
    layer->morphisms[idx].steps = q->head;
    q->head = NULL; // Transfer ownership
    layer->morphism_count++;

    graph_query_destroy(q);
    return 0;
}
```

- [ ] **Step 2: Commit**

```bash
git add src/Layers/graph/graph_parser.c
git commit -m "feat: add recursive descent DSL parser (graph_parser.c)"
```

---

### Task 5: Parser Tests + CMake

**Files:**
- Create: `tests/test_graph_parser.cpp`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Add test target to CMakeLists.txt**

After the existing `test_graph` entry, add:

```cmake
    # Test for Graph DSL Parser
    add_executable(test_graph_parser tests/test_graph_parser.cpp)
    target_link_libraries(test_graph_parser wavedb gtest gtest_main)
    add_test(NAME test_graph_parser COMMAND test_graph_parser)
```

- [ ] **Step 2: Write tests/test_graph_parser.cpp**

```cpp
//
// Tests for Graph DSL Parser
//

#include <gtest/gtest.h>
#include <string.h>
#include <string>
#include <future>
#include <chrono>

#if _WIN32
#include <io.h>
#include <direct.h>
#include <process.h>
#define getpid() _getpid()
#define mkdir(path, mode) _mkdir(path)
#else
#include <unistd.h>
#endif

extern "C" {
#include "../src/Layers/graph/graph.h"
#include "../src/Layers/graph/graph_internal.h"
}

static int test_counter = 0;

class GraphParserTest : public ::testing::Test {
protected:
    void SetUp() override {
        test_dir = "/tmp/wavedb_graph_parser_test_" + std::to_string(getpid()) + "_" + std::to_string(test_counter++);
        mkdir(test_dir.c_str(), 0700);
        layer = graph_layer_create(test_dir.c_str(), NULL);
        ASSERT_NE(layer, nullptr);
    }

    void TearDown() override {
        graph_layer_destroy(layer);
        std::string cmd = "rm -rf " + test_dir;
        system(cmd.c_str());
    }

    void insert_test_data() {
        graph_insert_sync(layer, "clip_abc", "tagged_with", "gaming");
        graph_insert_sync(layer, "clip_abc", "tagged_with", "tutorial");
        graph_insert_sync(layer, "clip_abc", "created_by", "alice");
        graph_insert_sync(layer, "clip_xyz", "tagged_with", "gaming");
        graph_insert_sync(layer, "clip_xyz", "created_by", "bob");
    }

    std::string test_dir;
    graph_layer_t* layer = nullptr;
};

TEST_F(GraphParserTest, SimpleOutTraversal) {
    insert_test_data();

    graph_parse_error_t err;
    graph_result_t* r = graph_parse_execute(
        "g.V(\"clip_abc\").Out(\"tagged_with\").All()",
        layer, &err);
    ASSERT_NE(r, nullptr) << "Error: " << err.message;
    ASSERT_EQ(graph_result_count(r), (size_t)2);

    const char* const* verts = graph_result_vertices(r);
    bool found_gaming = false, found_tutorial = false;
    for (size_t i = 0; i < graph_result_count(r); i++) {
        if (strcmp(verts[i], "gaming") == 0) found_gaming = true;
        if (strcmp(verts[i], "tutorial") == 0) found_tutorial = true;
    }
    EXPECT_TRUE(found_gaming);
    EXPECT_TRUE(found_tutorial);
    graph_result_destroy(r);
}

TEST_F(GraphParserTest, SimpleInTraversal) {
    insert_test_data();

    graph_parse_error_t err;
    graph_result_t* r = graph_parse_execute(
        "g.V(\"gaming\").In(\"tagged_with\").All()",
        layer, &err);
    ASSERT_NE(r, nullptr) << "Error: " << err.message;
    ASSERT_EQ(graph_result_count(r), (size_t)2);
    graph_result_destroy(r);
}

TEST_F(GraphParserTest, MultiHopTraversal) {
    graph_insert_sync(layer, "alice", "follows", "bob");
    graph_insert_sync(layer, "bob", "likes", "clip_abc");

    graph_parse_error_t err;
    graph_result_t* r = graph_parse_execute(
        "g.V(\"alice\").Out(\"follows\").Out(\"likes\").All()",
        layer, &err);
    ASSERT_NE(r, nullptr) << "Error: " << err.message;
    ASSERT_EQ(graph_result_count(r), (size_t)1);
    EXPECT_STREQ(graph_result_vertices(r)[0], "clip_abc");
    graph_result_destroy(r);
}

TEST_F(GraphParserTest, Intersection) {
    insert_test_data();

    graph_parse_error_t err;
    graph_result_t* r = graph_parse_execute(
        "g.V(\"gaming\").In(\"tagged_with\").And(g.V(\"tutorial\").In(\"tagged_with\")).All()",
        layer, &err);
    ASSERT_NE(r, nullptr) << "Error: " << err.message;
    ASSERT_EQ(graph_result_count(r), (size_t)1);
    EXPECT_STREQ(graph_result_vertices(r)[0], "clip_abc");
    graph_result_destroy(r);
}

TEST_F(GraphParserTest, Union) {
    insert_test_data();

    graph_parse_error_t err;
    graph_result_t* r = graph_parse_execute(
        "g.V(\"gaming\").In(\"tagged_with\").Or(g.V(\"tutorial\").In(\"tagged_with\")).All()",
        layer, &err);
    ASSERT_NE(r, nullptr) << "Error: " << err.message;
    ASSERT_EQ(graph_result_count(r), (size_t)3);
    graph_result_destroy(r);
}

TEST_F(GraphParserTest, HasFilter) {
    graph_insert_sync(layer, "clip_abc", "name", "My Clip");
    graph_insert_sync(layer, "clip_xyz", "name", "Other Clip");

    graph_parse_error_t err;
    graph_result_t* r = graph_parse_execute(
        "g.Has(\"name\", \"My Clip\").All()",
        layer, &err);
    ASSERT_NE(r, nullptr) << "Error: " << err.message;
    ASSERT_EQ(graph_result_count(r), (size_t)1);
    EXPECT_STREQ(graph_result_vertices(r)[0], "clip_abc");
    graph_result_destroy(r);
}

TEST_F(GraphParserTest, Limit) {
    insert_test_data();

    graph_parse_error_t err;
    graph_result_t* r = graph_parse_execute(
        "g.V(\"gaming\").In(\"tagged_with\").Limit(1).All()",
        layer, &err);
    ASSERT_NE(r, nullptr) << "Error: " << err.message;
    ASSERT_EQ(graph_result_count(r), (size_t)1);
    graph_result_destroy(r);
}

TEST_F(GraphParserTest, Count) {
    insert_test_data();

    graph_parse_error_t err;
    size_t count = 0;
    int rc = graph_parse_count(
        "g.V(\"gaming\").In(\"tagged_with\").All()",
        layer, &count, &err);
    ASSERT_EQ(rc, 0) << "Error: " << err.message;
    ASSERT_EQ(count, (size_t)2);
}

TEST_F(GraphParserTest, MorphismDefinitionAndFollow) {
    graph_insert_sync(layer, "alice", "follows", "bob");
    graph_insert_sync(layer, "bob", "likes", "clip_abc");

    graph_parse_error_t err;

    // Define a morphism
    int rc = graph_morphism_define(layer, "followed_content",
        "g.Morphism(\"followed_content\").Out(\"follows\").Out(\"likes\")", &err);
    ASSERT_EQ(rc, 0) << "Error: " << err.message;

    // Use it
    graph_result_t* r = graph_parse_execute(
        "g.V(\"alice\").Follow(\"followed_content\").All()",
        layer, &err);
    ASSERT_NE(r, nullptr) << "Error: " << err.message;
    ASSERT_EQ(graph_result_count(r), (size_t)1);
    EXPECT_STREQ(graph_result_vertices(r)[0], "clip_abc");
    graph_result_destroy(r);
}

TEST_F(GraphParserTest, ParseErrorUnclosedString) {
    graph_parse_error_t err;
    graph_query_t* q = graph_parse("g.V(\"abc)", layer, &err);
    ASSERT_EQ(q, nullptr);
    ASSERT_EQ(err.ok, 0);
    ASSERT_GT(err.position, 0);
    EXPECT_NE(strstr(err.message, "Unterminated"), nullptr);
}

TEST_F(GraphParserTest, ParseErrorBadMethod) {
    graph_parse_error_t err;
    graph_query_t* q = graph_parse("g.X(\"y\").All()", layer, &err);
    ASSERT_EQ(q, nullptr);
    ASSERT_EQ(err.ok, 0);
    EXPECT_NE(strstr(err.message, "Unknown method"), nullptr);
}

TEST_F(GraphParserTest, ParseErrorMissingDot) {
    graph_parse_error_t err;
    graph_query_t* q = graph_parse("g.V(\"x\")Out(\"y\")", layer, &err);
    ASSERT_EQ(q, nullptr);
}

TEST_F(GraphParserTest, EmptyQuery) {
    graph_parse_error_t err;
    graph_query_t* q = graph_parse("", layer, &err);
    ASSERT_EQ(q, nullptr);
    ASSERT_EQ(err.ok, 0);
}

TEST_F(GraphParserTest, UnknownMorphism) {
    graph_parse_error_t err;
    graph_result_t* r = graph_parse_execute(
        "g.V(\"x\").Follow(\"nonexistent\").All()", layer, &err);
    ASSERT_EQ(r, nullptr);
    EXPECT_NE(strstr(err.message, "Unknown morphism"), nullptr);
}
```

- [ ] **Step 3: Build and run**

```bash
cd /home/victor/Workspace/src/github.com/vijayee/WaveDB/build
cmake .. -DBUILD_TESTS=ON 2>&1 | tail -3
make test_graph_parser 2>&1 | tail -10
./test_graph_parser
```

Expected: All tests pass.

- [ ] **Step 4: Commit**

```bash
git add CMakeLists.txt tests/test_graph_parser.cpp
git commit -m "feat: add DSL parser tests and CMake integration"
```

---

### Task 6: Self-Review and Fixes

- [ ] **Step 1: Build and run BOTH test suites to verify nothing regressed**

```bash
cd /home/victor/Workspace/src/github.com/vijayee/WaveDB/build
make test_graph test_graph_parser test_graph_set 2>&1 | tail -5
./test_graph && ./test_graph_parser && ./test_graph_set
```

Expected: All pass.

- [ ] **Step 2: Fix any issues found**

Common issues to watch for:
- The `Has` operator test might fail because the POS scan uses numeric chunk padding. The `graph_execute_has` function calls `key_starts_with_index("pos", ...)` which handles padding.
- `parse_query` might need to handle the `Has(args)` case better when `Has` is the first step (no current set for intersect). The executor code handles this with the `first` flag in `execute_child_steps` — if first, it uses an empty set for the POS scan result.
- Morphism tests need `graph_morphism_define` to work properly — test the parse-then-store flow.

Fix any issues found, then re-build and re-run.

- [ ] **Step 3: Commit final fixes**

```bash
git add -A
git commit -m "fix: parser edge cases and test adjustments"
```