//
// Created by victor on 05/30/26.
//

#include "graph_internal.h"
#include "../../Util/allocator.h"
#include <string.h>
#include <stdio.h>

/* ── Forward declarations ── */

static void triple_work_execute(void* ctx);
static void triple_work_abort(void* ctx);

/* ── Helpers ── */

static int build_index_path(char* buf, size_t buf_size,
                            const char* index_name,
                            const char* c1, const char* c2, const char* c3) {
    return snprintf(buf, buf_size, "/%s/%s/%s/%s", index_name, c1, c2, c3);
}

/* ── Layer lifecycle ── */

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
    layer->schema = NULL;
    layer->morphisms = NULL;
    layer->morphism_count = 0;
    layer->morphism_capacity = 0;
    return layer;
}

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
    if (layer->schema) {
        for (size_t i = 0; i < layer->schema->type_count; i++) {
            free(layer->schema->types[i].name);
            for (size_t j = 0; j < layer->schema->types[i].field_count; j++) {
                free(layer->schema->types[i].fields[j].name);
                free(layer->schema->types[i].fields[j].type);
            }
            free(layer->schema->types[i].fields);
        }
        free(layer->schema->types);
        free(layer->schema);
    }
    free(layer->morphisms);
    free(layer);
}

database_t* graph_layer_get_db(graph_layer_t* layer) {
    return layer ? layer->db : NULL;
}

/* ── Triple operations (sync) ── */

int graph_insert_sync(graph_layer_t* layer, const char* s, const char* p, const char* o) {
    if (!layer || !s || !p || !o) return -1;

    char path_spo[1024], path_pos[1024], path_osp[1024], path_pso[1024];
    uint8_t empty_val = 0;
    raw_op_t ops[4];
    int count = 0;

    if (graph_schema_needs_index(layer, p, GRAPH_INDEX_SPO)) {
        build_index_path(path_spo, sizeof(path_spo), "spo", s, p, o);
        ops[count].key = path_spo;
        ops[count].key_len = strlen(path_spo);
        ops[count].value = &empty_val;
        ops[count].value_len = 0;
        ops[count].type = 0;
        count++;
    }
    if (graph_schema_needs_index(layer, p, GRAPH_INDEX_POS)) {
        build_index_path(path_pos, sizeof(path_pos), "pos", p, o, s);
        ops[count].key = path_pos;
        ops[count].key_len = strlen(path_pos);
        ops[count].value = &empty_val;
        ops[count].value_len = 0;
        ops[count].type = 0;
        count++;
    }
    if (graph_schema_needs_index(layer, p, GRAPH_INDEX_OSP)) {
        build_index_path(path_osp, sizeof(path_osp), "osp", o, s, p);
        ops[count].key = path_osp;
        ops[count].key_len = strlen(path_osp);
        ops[count].value = &empty_val;
        ops[count].value_len = 0;
        ops[count].type = 0;
        count++;
    }
    if (graph_schema_needs_index(layer, p, GRAPH_INDEX_PSO)) {
        build_index_path(path_pso, sizeof(path_pso), "pso", p, s, o);
        ops[count].key = path_pso;
        ops[count].key_len = strlen(path_pso);
        ops[count].value = &empty_val;
        ops[count].value_len = 0;
        ops[count].type = 0;
        count++;
    }

    if (count == 0) return 0;  // Schema says no indices needed — valid

    return database_batch_sync_raw(layer->db, '/', ops, count);
}

int graph_delete_sync(graph_layer_t* layer, const char* s, const char* p, const char* o) {
    if (!layer || !s || !p || !o) return -1;

    char path[1024];
    int rc = 0;

    if (graph_schema_needs_index(layer, p, GRAPH_INDEX_SPO)) {
        build_index_path(path, sizeof(path), "spo", s, p, o);
        rc = database_delete_sync_raw(layer->db, path, strlen(path), '/');
        if (rc != 0) return rc;
    }
    if (graph_schema_needs_index(layer, p, GRAPH_INDEX_POS)) {
        build_index_path(path, sizeof(path), "pos", p, o, s);
        rc = database_delete_sync_raw(layer->db, path, strlen(path), '/');
        if (rc != 0) return rc;
    }
    if (graph_schema_needs_index(layer, p, GRAPH_INDEX_OSP)) {
        build_index_path(path, sizeof(path), "osp", o, s, p);
        rc = database_delete_sync_raw(layer->db, path, strlen(path), '/');
        if (rc != 0) return rc;
    }
    if (graph_schema_needs_index(layer, p, GRAPH_INDEX_PSO)) {
        build_index_path(path, sizeof(path), "pso", p, s, o);
        rc = database_delete_sync_raw(layer->db, path, strlen(path), '/');
        if (rc != 0) return rc;
    }

    return 0;
}

/* ── Triple operations (async) ── */

typedef struct {
    graph_layer_t* layer;
    char* s;
    char* p;
    char* o;
    int is_delete;
    int result;             // Store result inline, no malloc needed
    promise_t* promise;
} triple_work_ctx_t;

static void triple_work_execute(void* ctx) {
    triple_work_ctx_t* tc = (triple_work_ctx_t*)ctx;
    tc->result = tc->is_delete
        ? graph_delete_sync(tc->layer, tc->s, tc->p, tc->o)
        : graph_insert_sync(tc->layer, tc->s, tc->p, tc->o);
    promise_resolve(tc->promise, &tc->result);
    free(tc->s); free(tc->p); free(tc->o);
    promise_destroy(tc->promise);
    free(tc);
}

static void triple_work_abort(void* ctx) {
    triple_work_ctx_t* tc = (triple_work_ctx_t*)ctx;
    async_error_t* err = ERROR("Operation aborted");
    promise_reject(tc->promise, err);
    error_destroy(err);
    free(tc->s); free(tc->p); free(tc->o);
    promise_destroy(tc->promise);
    free(tc);
}

void graph_insert(graph_layer_t* layer, const char* s, const char* p, const char* o, promise_t* promise) {
    if (!layer || !s || !p || !o) {
        async_error_t* err = ERROR("NULL argument");
        promise_reject(promise, err);
        error_destroy(err);
        return;
    }
    triple_work_ctx_t* tc = (triple_work_ctx_t*)get_clear_memory(sizeof(triple_work_ctx_t));
    tc->layer = layer;
    tc->s = strdup(s); tc->p = strdup(p); tc->o = strdup(o);
    tc->is_delete = 0;
    tc->promise = REFERENCE(promise, promise_t);

    work_t* work = work_create(triple_work_execute, triple_work_abort, tc);

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
        async_error_t* err = ERROR("NULL argument");
        promise_reject(promise, err);
        error_destroy(err);
        return;
    }
    triple_work_ctx_t* tc = (triple_work_ctx_t*)get_clear_memory(sizeof(triple_work_ctx_t));
    tc->layer = layer;
    tc->s = strdup(s); tc->p = strdup(p); tc->o = strdup(o);
    tc->is_delete = 1;
    tc->promise = REFERENCE(promise, promise_t);

    work_t* work = work_create(triple_work_execute, triple_work_abort, tc);

    work_pool_t* pool = layer->db->pool;
    if (pool) {
        refcounter_yield((refcounter_t*)work);
        work_pool_enqueue(pool, work);
    } else {
        triple_work_execute(tc);
        work_destroy(work);
    }
}

/* ── Query builder ── */

graph_query_t* graph_query_create(graph_layer_t* layer) {
    if (!layer) return NULL;
    graph_query_t* q = (graph_query_t*)get_clear_memory(sizeof(graph_query_t));
    q->layer = layer;
    return q;
}

void graph_query_destroy(graph_query_t* q) {
    if (!q) return;
    query_step_t* s = q->head;
    while (s) {
        query_step_t* next = s->next;
        free(s->vertex_id);
        free(s->predicate);
        free(s->has_predicate);
        free(s->has_value);
        for (size_t i = 0; i < s->num_children; i++) {
            query_step_t* cs = s->children[i];
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
    s->children[0] = left->head;
    s->children[1] = right->head;
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

/* ── Result struct (must be before functions that use sizeof/access members) ── */

struct graph_result_t {
    vertex_set_t set;
};

/* ── Execution (sync) ── */

graph_result_t* graph_query_execute_sync(graph_query_t* q) {
    if (!q || !q->head) return NULL;

    graph_result_t* r = (graph_result_t*)get_clear_memory(sizeof(graph_result_t));
    vertex_set_init(&r->set, 64);

    graph_optimize(&q->head);

    int rc = graph_execute_chain(q->layer->db, q->head, &r->set);
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
        promise_resolve(qc->promise, result);
    } else {
        async_error_t* err = ERROR("Query execution failed");
        promise_reject(qc->promise, err);
        error_destroy(err);
    }
    promise_destroy(qc->promise);
    free(qc);
}

static void query_work_abort(void* ctx) {
    query_work_ctx_t* qc = (query_work_ctx_t*)ctx;
    async_error_t* err = ERROR("Query aborted");
    promise_reject(qc->promise, err);
    error_destroy(err);
    promise_destroy(qc->promise);
    free(qc);
}

void graph_query_execute(graph_query_t* q, promise_t* promise) {
    if (!q || !promise) return;
    query_work_ctx_t* qc = (query_work_ctx_t*)get_clear_memory(sizeof(query_work_ctx_t));
    qc->q = q;
    qc->promise = REFERENCE(promise, promise_t);

    work_t* work = work_create(query_work_execute, query_work_abort, qc);

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

/* ── Schema definition ── */

int graph_schema_parse(graph_layer_t* layer, const char* dsl, char** error_out) {
    if (!layer || !dsl) return -1;
    extern int graph_schema_parse_dsl(graph_layer_t* layer, const char* input, size_t len, char** error_out);
    return graph_schema_parse_dsl(layer, dsl, strlen(dsl), error_out);
}

int graph_schema_needs_index(graph_layer_t* layer, const char* predicate, graph_index_flags_t index) {
    if (!layer || !predicate) return 1;
    if (!layer->schema) return 1;

    for (size_t i = 0; i < layer->schema->type_count; i++) {
        for (size_t j = 0; j < layer->schema->types[i].field_count; j++) {
            graph_schema_field_t* f = &layer->schema->types[i].fields[j];
            if (strcmp(f->name, predicate) == 0) {
                return (f->indices & index) != 0;
            }
        }
    }
    return 1;  // Unknown predicate: maintain index (backward compat)
}
