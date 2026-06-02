//
// Created by victor on 05/30/26.
//

#ifndef WAVEDB_GRAPH_INTERNAL_H
#define WAVEDB_GRAPH_INTERNAL_H

#include "graph.h"
#include "../../Util/allocator.h"
#include "../../Util/vec.h"
#include "../../Util/threadding.h"
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
int vertex_set_difference(vertex_set_t* result, const vertex_set_t* a, const vertex_set_t* b);
void vertex_set_clear(vertex_set_t* set);
int vertex_set_copy(vertex_set_t* result, const vertex_set_t* src);

/* ── Morphism storage ── */

typedef struct {
    char* name;
    struct query_step_t* steps;
} morphism_entry_t;

/* ── Schema types (internal) ── */

typedef struct {
    char* name;                  // field/predicate name
    char* type;                  // target type
    uint8_t is_array;            // 1 = [Type], 0 = Type
    graph_index_flags_t indices;  // which indices to maintain
} graph_schema_field_t;

/* ── Statistics for cost-based reordering ── */

typedef struct {
    char* predicate;
    size_t triple_count;       // total triples with this predicate
    size_t distinct_subjects;  // distinct subjects for this predicate
} graph_pred_stats_t;

typedef struct {
    vec_t(graph_pred_stats_t) predicates;
} graph_stats_t;

typedef struct {
    char* name;                  // type name
    graph_index_flags_t indices;  // default indices for the type
    vec_t(graph_schema_field_t) fields;
} graph_schema_type_t;

struct graph_schema_t {
    vec_t(graph_schema_type_t) types;
};

/* ── Query Step (internal) ── */

typedef struct query_step_t query_step_t;

struct query_step_t {
    graph_step_type_t type;
    char* vertex_id;                // For GRAPH_STEP_VERTEX
    char* predicate;                // For GRAPH_STEP_OUT, GRAPH_STEP_IN
    char* has_predicate;            // For GRAPH_STEP_HAS, or fused filter on OUT/IN
    char* has_value;                 // For GRAPH_STEP_HAS, or fused filter on OUT/IN
    graph_cmp_op_t has_cmp;         // For GRAPH_STEP_HAS (comparison operator)
    char* morphism_name;            // For GRAPH_STEP_MORPHISM
    size_t limit;                   // For GRAPH_STEP_LIMIT
    vec_t(query_step_t*) children;  // For GRAPH_STEP_INTERSECT, GRAPH_STEP_UNION, GRAPH_STEP_DIFFERENCE
    query_step_t* next;             // Linked list
};

/* ── Operator execution ── */

// Execute a chain of steps starting from an empty seed set.
int graph_execute_chain(graph_layer_t* layer, query_step_t* steps, vertex_set_t* output);

// Single-step execution on an input set.
int graph_execute_vertex(database_t* db, query_step_t* step, vertex_set_t* output);
int graph_execute_out(database_t* db, const vertex_set_t* input, const char* predicate, vertex_set_t* output);
int graph_execute_in(database_t* db, const vertex_set_t* input, const char* predicate, vertex_set_t* output);
int graph_execute_has(database_t* db, const vertex_set_t* input,
                       const char* predicate, const char* value,
                       graph_cmp_op_t cmp, vertex_set_t* output);
int graph_execute_osp(database_t* db, const vertex_set_t* input,
                       const char* object, vertex_set_t* output);
int graph_execute_pso(database_t* db, const char* predicate, vertex_set_t* output);

int graph_optimize(query_step_t** steps);
int graph_optimize_reorder_has(query_step_t** steps, graph_layer_t* layer);
int graph_optimize_reorder_intersect_children(query_step_t** steps, graph_layer_t* layer);

/* ── Layer + Query struct (shared by graph.c and graph_parser.c) ── */

struct graph_layer_t {
    database_t* db;
    graph_schema_t* schema;
    vec_t(morphism_entry_t) morphisms;
    graph_stats_t* stats;
    int stats_computed;
    PLATFORMRWLOCKTYPE(lock);
};

struct graph_query_t {
    graph_layer_t* layer;
    query_step_t* head;
    query_step_t* tail;
};

/* ── Path building helpers (implemented in graph.c) ── */

void build_path_vec(vec_char_t* v, const char* index_name,
                    const char* c1, const char* c2, const char* c3);
void build_prefix_vec(vec_char_t* v, const char* index_name,
                      const char* c1, const char* c2);

/* ── Key component extraction (implemented in graph_ops.c) ── */

const char* key_nth_component(const char* key, size_t key_len, int n,
                              char* buf, size_t buf_size);
size_t append_successor(char* buf, size_t prefix_len);

/* ── Parser internals (implemented in graph_parser.c) ── */

query_step_t* copy_steps(query_step_t* steps);
graph_query_t* graph_parse_query(const char* input, size_t len, graph_layer_t* layer, graph_parse_error_t* error);
int graph_morphism_parse_and_store(graph_layer_t* layer, const char* name,
                                    const char* input, size_t len,
                                    graph_parse_error_t* error);

/* ── Statistics (implemented in graph_stats.c) ── */

int graph_stats_compute(graph_layer_t* layer);
size_t graph_stats_estimate_has(graph_stats_t* stats, const char* predicate,
                                 const char* value, graph_cmp_op_t cmp);
size_t graph_stats_estimate_out(graph_stats_t* stats, const char* predicate, size_t input_size);
size_t graph_stats_estimate_in(graph_stats_t* stats, const char* predicate, size_t input_size);
size_t graph_stats_estimate_chain(graph_stats_t* stats, query_step_t* head);

/* ── Schema parser internals (implemented in graph_schema_parser.c) ── */

int graph_schema_parse_dsl(graph_layer_t* layer, const char* input, size_t len, char** error_out);
int graph_schema_store_type(graph_layer_t* layer, graph_schema_type_t* type);
int graph_schema_load(graph_layer_t* layer);

#ifdef __cplusplus
}
#endif

#endif // WAVEDB_GRAPH_INTERNAL_H
