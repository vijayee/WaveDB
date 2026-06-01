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
