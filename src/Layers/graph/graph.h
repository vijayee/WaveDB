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
    GRAPH_STEP_DIFFERENCE,  // Set difference: left minus right
    GRAPH_STEP_LIMIT,       // Cap result count
    GRAPH_STEP_HAS,         // Filter: intersect with POS scan of (predicate, value)
    GRAPH_STEP_MORPHISM,    // Lazy reference to a named morphism
} graph_step_type_t;

/* ── Comparison operators for Has filter ── */

typedef enum {
    GRAPH_CMP_EQ = 0,
    GRAPH_CMP_GT,
    GRAPH_CMP_GTE,
    GRAPH_CMP_LT,
    GRAPH_CMP_LTE
} graph_cmp_op_t;

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
int graph_query_has(graph_query_t* q, const char* predicate, const char* value);
int graph_query_has_cmp(graph_query_t* q, const char* predicate, const char* value, graph_cmp_op_t cmp);
int graph_query_intersect(graph_query_t* q, graph_query_t* left, graph_query_t* right);
int graph_query_union(graph_query_t* q, graph_query_t* left, graph_query_t* right);
int graph_query_difference(graph_query_t* q, graph_query_t* left, graph_query_t* right);
int graph_query_limit(graph_query_t* q, size_t limit);
int graph_query_follow(graph_query_t* q, const char* name);

/* ── Execution (sync) ── */

graph_result_t* graph_query_execute_sync(graph_query_t* q);

/* ── Execution (async) ── */

void graph_query_execute(graph_query_t* q, promise_t* promise);

/* ── Result handling ── */

size_t graph_result_count(graph_result_t* r);
const char* const* graph_result_vertices(graph_result_t* r);
void graph_result_destroy(graph_result_t* r);

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

/* ── Index types for schema ── */

typedef enum {
    GRAPH_INDEX_NONE = 0,
    GRAPH_INDEX_SPO  = 1 << 0,
    GRAPH_INDEX_POS  = 1 << 1,
    GRAPH_INDEX_OSP  = 1 << 2,
    GRAPH_INDEX_PSO  = 1 << 3,
} graph_index_flags_t;

/* ── Schema definition ── */

typedef struct graph_schema_t graph_schema_t;

int graph_schema_parse(graph_layer_t* layer, const char* dsl, char** error_out);
int graph_schema_needs_index(graph_layer_t* layer, const char* predicate, graph_index_flags_t index);

#ifdef __cplusplus
}
#endif

#endif // WAVEDB_GRAPH_H
