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
int graph_execute_chain(database_t* db, query_step_t* steps, vertex_set_t* output);

// Single-step execution on an input set.
int graph_execute_vertex(database_t* db, query_step_t* step, vertex_set_t* output);
int graph_execute_out(database_t* db, const vertex_set_t* input, const char* predicate, vertex_set_t* output);
int graph_execute_in(database_t* db, const vertex_set_t* input, const char* predicate, vertex_set_t* output);

#ifdef __cplusplus
}
#endif

#endif // WAVEDB_GRAPH_INTERNAL_H
