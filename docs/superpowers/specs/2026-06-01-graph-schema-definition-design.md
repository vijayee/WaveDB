# Graph Schema Definition & Index Selection Design

## Overview

A small schema definition parser (GraphQL SDL-like) that registers type metadata on the graph layer, then uses that metadata to control which indices are written during insert/delete. Follows the same storage pattern as the GraphQL schema layer (`__schema` paths in the database).

## Schema Syntax

```
type Clip @index(spo, pos) {
  tagged_with: [Tag]
  created_by: User
  name: String @index(pos)
  created_at: DateTime
}

type User @index(spo) {
  follows: [User]
  likes: [Clip]
  name: String @index(pos)
}
```

Grammar:
```
schema       → type_def+
type_def     → "type" NAME "@index" "(" index_list ")" "{" field+ "}"
index_list   → NAME ("," NAME)*      // spo, pos
field        → NAME ":" type_ref ("@" NAME)? 
type_ref     → "[" NAME "]" | NAME | "String" | "DateTime"
```

## Schema Data Model

```c
typedef enum {
    GRAPH_INDEX_NONE = 0,
    GRAPH_INDEX_SPO  = 1 << 0,
    GRAPH_INDEX_POS  = 1 << 1,
} graph_index_flags_t;

typedef struct {
    char* name;                // field/predicate name
    char* type;                // target type ("Tag", "String", etc.)
    uint8_t is_array;          // 1 = [Type], 0 = Type
    graph_index_flags_t indices;  // which indices to maintain
} graph_schema_field_t;

typedef struct {
    char* name;               // type name ("Clip")
    graph_index_flags_t indices;  // default indices for the type
    graph_schema_field_t* fields;
    size_t field_count;
} graph_schema_type_t;
```

## Index Selection

Currently `graph_insert_sync` always writes both SPO and POS. With schema:

- If the predicate is registered in the schema, only the declared indices are written
- If the predicate is NOT in the schema, both SPO and POS are written (backward compatible)
- `graph_delete_sync` follows the same logic

This means existing code that calls `graph_insert_sync` without schema continues to work identically. Adding a schema just reduces write amplification for known predicates.

## Storage

Schema is stored in the database under `__gschema/` paths (g for graph, to avoid conflicts with the GraphQL layer's `__schema`):

```
__gschema/Clip/name            → String
__gschema/Clip/indices         → spo,pos
__gschema/Clip/fields/name     → String
__gschema/Clip/fields/name/indices → pos
__gschema/Clip/fields/tagged_with     → Tag
__gschema/Clip/fields/tagged_with/indices → spo,pos
__gschema/Clip/fields/tagged_with/array → 1
```

## Files

```
src/Layers/graph/
├── graph.h                  — MODIFY: add schema types, graph_schema_parse(), index APIs
├── graph_internal.h         — MODIFY: add graph_schema_t, schema_field_t types
├── graph.c                  — MODIFY: extend layer with schema, schema lifecycle
├── graph_schema_parser.c    — NEW: recursive descent schema parser (~250 lines)
├── graph_set.c              — unchanged
└── graph_ops.c              — MODIFY: index selection in insert_sync/delete_sync

tests/test_graph_schema.cpp  — NEW: schema parser + index selection tests
```

## C API

```c
// Parse schema DSL and register on the layer.
// Stores metadata in the database under __gschema/ paths.
// Returns 0 on success, -1 on parse error.
int graph_schema_parse(graph_layer_t* layer, const char* dsl, char** error_out);

// Check if a predicate needs a specific index.
// Without schema: always returns 1 (maintain both indices).
// With schema: checks the field's declared indices.
int graph_schema_needs_index(graph_layer_t* layer, const char* predicate, graph_index_flags_t index);
```

The `graph_schema_needs_index` function is called by `graph_insert_sync` and `graph_delete_sync` to decide which indices to write.

## Index Selection in Insert/Delete

Current `graph_insert_sync`:
```c
// Always writes both
database_batch_sync_raw(db, '/', ops, 2);  // SPO + POS
```

With schema:
```c
int count = 0;
if (graph_schema_needs_index(layer, p, GRAPH_INDEX_SPO)) ops[count++] = spo_op;
if (graph_schema_needs_index(layer, p, GRAPH_INDEX_POS)) ops[count++] = pos_op;
if (count > 0) database_batch_sync_raw(db, '/', ops, count);
```

## Schema Parser

A hand-written recursive descent parser following the same pattern as `graph_parser.c`:

```c
typedef struct {
    const char* input;
    size_t pos;
    size_t len;
    graph_layer_t* layer;
    char error[256];
    int error_pos;
} schema_parser_t;
```

The parser produces `graph_schema_type_t` entries, stores them in the layer's schema registry, and persists them to `__gschema/` paths.

## Tests

```
TEST(GraphSchemaTest, ParseSchema) {
    graph_schema_parse(layer, "type Clip @index(spo, pos) { tagged_with: [Tag]; name: String @index(pos); }", NULL);
    // verify fields stored, indices computed
}

TEST(GraphSchemaTest, IndexSelection) {
    graph_schema_parse(layer, "type Clip @index(spo) { name: String @index(pos); }", NULL);
    // spo_only predicate: insert writes only SPO
    // pos_only predicate: insert writes only POS
}

TEST(GraphSchemaTest, UnknownPredicate) {
    // no schema registered: both indices written (backward compat)
    graph_insert_sync(layer, "clip", "unknown_pred", "val");
    // both SPO and POS written
}

TEST(GraphSchemaTest, ParseError) {
    int rc = graph_schema_parse(layer, "type Clip @index(invalid) {}", &error);
    ASSERT_LT(rc, 0);
}
```
