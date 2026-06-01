# Graph Query Language — DSL Parser Design

## Overview

A hand-written recursive descent parser that compiles a Gremlin-inspired DSL string into the existing `graph_query_t` step chain. No external dependencies (no yacc/lex, no JS engine).

## Syntax

```
// Core traversals
g.V("gaming").In("tagged_with").All()
g.V("alice").Out("follows").Out("likes").All()

// Set operations
g.V("gaming").In("tagged_with").And(g.V("tutorial").In("tagged_with")).All()
g.V("gaming").In("tagged_with").Or(g.V("tutorial").In("tagged_with")).All()

// Filter by value (POS scan then intersect)
g.V("gaming").In("tagged_with").Has("name", "My Clip").All()

// Find all vertices with a predicate=value (starting point)
g.Has("name", "Casablanca").All()

// Limit and count
g.V("gaming").In("tagged_with").Limit(10).All()
g.V("gaming").In("tagged_with").Count()

// Morphisms (named reusable path definitions, separate from queries)
g.Morphism("content_to_creator").Out("created_by")
g.V("gaming").In("tagged_with").Follow("content_to_creator").All()
```

## Grammar

```
query          → "g" "." step+
step           → vertex | out | in | has | limit | count | all 
               | and_expr | or_expr | follow
vertex         → "V(" string ")"
out            → "Out(" string ")"
in             → "In(" string ")"
has            → "Has(" string "," string ")"
limit          → "Limit(" number ")"   // backward compat: accepts GetLimit(n) too
count          → "Count()"
all            → "All()"
and_expr       → "And(" query ")"
or_expr        → "Or(" query ")"
follow         → "Follow(" string ")"
string         → '"' [^"]* '"'         // double-quoted string
number         → [0-9]+
```

Notes:
- `V()` always requires a vertex ID — `g.V()` with no argument is not supported. Use `g.Has("pred", "val")` as a starting filter.
- `Morphism` is not a query step — it's a separate top-level statement (see Morphism API).
- `Limit` and `GetLimit` are both accepted for backward compatibility.

## Architecture

```
┌──────────────┐     ┌──────────────────┐     ┌──────────────────┐
│  DSL String   │ ──▶ │  Recursive Desc. │ ──▶ │  graph_query_t   │
│  "g.V(x)..."  │     │  Parser           │     │  step chain      │
└──────────────┘     └──────────────────┘     └──────────────────┘
                                                     │
                                                     ▼
                                            ┌──────────────────┐
                                            │  Existing Phase   │
                                            │  1+2 Executor     │
                                            │  (graph_ops.c)    │
                                            └──────────────────┘
```

### Parser API

```c
// Parse a DSL string into a query (no execution).
// Returns NULL on parse error.
graph_query_t* graph_parse(const char* dsl, graph_layer_t* layer, graph_parse_error_t* error);

// Parse and execute, returning all results.
// Returns NULL on parse or execution error.
graph_result_t* graph_parse_execute(const char* dsl, graph_layer_t* layer, graph_parse_error_t* error);

// Parse and execute, returning count only.
// Returns 0 on success, -1 on error.
int graph_parse_count(const char* dsl, graph_layer_t* layer, size_t* count, graph_parse_error_t* error);

// Define a named morphism (stored in-memory on the layer, like a prepared statement).
int graph_morphism_define(graph_layer_t* layer, const char* name, const char* dsl, graph_parse_error_t* error);

// Error type
typedef struct {
    int ok;             // 1 = success, 0 = error
    int position;       // Character position of error (0-based)
    char message[256];  // Human-readable error message
} graph_parse_error_t;
```

## Parser-Only Constructs (No New Step Types)

`Has` and `Follow` are expanded at parse time — they compile down to existing step types.

**Has** compiles to:
1. Scan POS index for the predicate/value pair → produces a vertex set
2. Insert an INTERSECT step between the current set and that result

This means `Has(pred, val)` is equivalent to an Intersection with a POS scan — no new execution code needed. The POS scan is performed eagerly at parse time (not query time) since the value is known statically. The result set is embedded into the INTERSECT step as a VERTEX list.

Wait — this requires executing a database scan during parsing, which couples parsing to execution. Alternative: store the Has parameters in a new step type and let the executor handle the POS scan. This is cleaner.

**Revised approach**: Add `GRAPH_STEP_HAS` as a new step type. The executor handles it as: scan POS for the predicate/value, intersect with current set.

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

// query_step_t gets a new field for Has:
//   has_predicate (char*)  — for GRAPH_STEP_HAS
//   has_value (char*)      — for GRAPH_STEP_HAS
```

**Follow** compiles to:
1. Look up morphism by name in the layer's morphism registry
2. Copy the morphism's step chain into the current query (inlined at parse time)

**Count** is a terminal marker: execute the chain normally, return `graph_result_count()` instead of the full set. The parser builds the chain, and the count API (`graph_parse_count`) calls `graph_query_execute_sync` then returns `graph_result_count()`.

## Has Execution

In `graph_ops.c`, the executor handles `GRAPH_STEP_HAS`:

```
graph_execute_has(database_t* db, const vertex_set_t* input,
                   const char* predicate, const char* value,
                   vertex_set_t* output):
    result = POS_scan(predicate, value)
    intersect(output, input, result)
```

This reuses the existing `graph_execute_in` logic (POS scan) plus `vertex_set_intersect`. No new scan code.

## Morphism Storage

Morphisms are stored in-memory on `graph_layer_t`:

```c
typedef struct {
    char* name;
    query_step_t* steps;   // Parsed step chain (owned)
} morphism_entry_t;

// Extended graph_layer_t:
struct graph_layer_t {
    database_t* db;
    morphism_entry_t* morphisms;
    size_t morphism_count;
    size_t morphism_capacity;
};
```

No persistence — morphisms must be redefined after layer creation (like prepared statements). The morphism `steps` are deep-copied when inlined via Follow.

## New / Modified Files

```
src/Layers/graph/
├── graph.h              — Add graph_parse*, morphism API, GRAPH_STEP_HAS type
├── graph_internal.h     — Add morphism_entry_t, parser state types, has fields
├── graph.c              — Add morphism lifecycle (init/free in create/destroy)
├── graph_parser.c       — NEW: recursive descent parser (~400 lines)
├── graph_set.c          — unchanged
└── graph_ops.c          — Add graph_execute_has step handler

tests/test_graph_parser.cpp  — NEW: parser test suite
```

## Parser Implementation

```c
typedef struct {
    const char* input;      // Full input string
    size_t pos;             // Current position
    size_t len;             // Input length
    graph_layer_t* layer;   // For morphism lookups
    graph_parse_error_t* error;  // Error output
} parser_t;

// Internal functions
static void skip_whitespace(parser_t* p);
static int peek_char(parser_t* p);
static int expect_char(parser_t* p, char c);
static int expect_string(parser_t* p, const char* s);
static char* parse_string(parser_t* p);      // "...", returns malloc'd copy
static long parse_number(parser_t* p);        // returns number
static int parse_query(parser_t* p, graph_query_t* q);
static int parse_step(parser_t* p, graph_query_t* q);
```

The parser is a straightforward recursive descent:

```
parse_query:
    expect "g"
    expect "."
    while not at end and peek != ')':
        parse_step
        if next is '.', consume it

parse_step:
    peek identifier
    switch on identifier:
        "V"     → parse_string, append VERTEX step
        "Out"   → parse_string, append OUT step
        "In"    → parse_string, append IN step
        "Has"   → parse_string, parse_string, append HAS step
        "And"   → "(", parse_query, ")", append INTERSECT step
        "Or"    → "(", parse_query, ")", append UNION step
        "Limit" / "GetLimit" → parse_number, append LIMIT step
        "All"   → no-op (terminal marker, nothing to append)
        "Count" → no-op (terminal marker, nothing to append)
        "Follow" → parse_string, look up morphism, inline its steps
```

## Error Handling

The parser produces structured errors with position:

```
Parse error at position 18: Expected '.' but found 'X'
g.V("gaming")XOut("tagged_with")
              ^
```

- `error->ok` = 0 on error
- `error->position` = character position (0-based)
- `error->message` = human-readable string
- Parsing stops at first error (no error recovery)

## Testing

Test cases for `tests/test_graph_parser.cpp`:

- `SimpleOutTraversal` — `g.V("alice").Out("follows").All()`
- `SimpleInTraversal` — `g.V("gaming").In("tagged_with").All()`
- `MultiHop` — `g.V("alice").Out("follows").Out("likes").All()`
- `Intersection` — `g.V("gaming").In("tagged_with").And(g.V("tutorial").In("tagged_with")).All()`
- `Union` — `g.V("gaming").In("tagged_with").Or(g.V("tutorial").In("tagged_with")).All()`
- `HasFilter` — `g.V("gaming").In("tagged_with").Has("name", "test").All()`
- `HasStartingPoint` — `g.Has("name", "Casablanca").All()`
- `Limit` — `g.V("gaming").In("tagged_with").Limit(5).All()`
- `GetLimit` — `g.V("gaming").In("tagged_with").GetLimit(5).All()`
- `Count` — `g.V("gaming").In("tagged_with").Count()`
- `MorphismDefinition` — `g.Morphism("my_path").Out("created_by")`
- `MorphismFollow` — define morphism, then `g.V("x").Follow("my_path").All()`
- `ParseErrorUnclosedString` — `g.V("abc)`
- `ParseErrorBadMethod` — `g.X("y").All()`
- `ParseErrorMissingDot` — `g.V("x")Out("y")`
- `EmptyQuery` — `""`
- `UnknownMorphism` — `g.V("x").Follow("nonexistent").All()`
