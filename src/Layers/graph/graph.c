//
// Created by victor on 05/30/26.
//

#include "graph_internal.h"
#include "../../Util/allocator.h"
#include "../../Util/vec.h"
#include <string.h>
#include <stdio.h>

/* ── Forward declarations ── */

static void triple_work_execute(void* ctx);
static void triple_work_abort(void* ctx);

/* ── Helpers ── */

void build_path_vec(vec_char_t* v, const char* index_name,
                    const char* c1, const char* c2, const char* c3) {
    vec_pusharr(v, "/", 1);
    vec_pusharr(v, index_name, strlen(index_name));
    vec_pusharr(v, "/", 1);
    vec_pusharr(v, c1, strlen(c1));
    vec_pusharr(v, "/", 1);
    vec_pusharr(v, c2, strlen(c2));
    vec_pusharr(v, "/", 1);
    vec_pusharr(v, c3, strlen(c3));
    vec_push(v, '\0');
}

void build_prefix_vec(vec_char_t* v, const char* index_name,
                      const char* c1, const char* c2) {
    vec_pusharr(v, "/", 1);
    vec_pusharr(v, index_name, strlen(index_name));
    vec_pusharr(v, "/", 1);
    vec_pusharr(v, c1, strlen(c1));
    vec_pusharr(v, "/", 1);
    vec_pusharr(v, c2, strlen(c2));
    vec_pusharr(v, "/", 1);
    vec_push(v, '\0');
}

/* ── Layer lifecycle ── */

graph_layer_t* graph_layer_create(const char* path, database_config_t* config) {
    graph_layer_t* layer = (graph_layer_t*)get_clear_memory(sizeof(graph_layer_t));
    int error_code = 0;
    if (config) {
        layer->db = database_create_with_config(path, config, &error_code);
    } else if (path) {
        database_config_t* default_config = database_config_default();
        if (!default_config) { free(layer); return NULL; }
        default_config->enable_persist = 1;
        // Use a small number of worker threads for persistence background ops
        default_config->worker_threads = 1;
        layer->db = database_create_with_config(path, default_config, &error_code);
        database_config_destroy(default_config);
    } else {
        layer->db = database_create(NULL, 0, NULL, 0, 0, 0, NULL, NULL, &error_code);
    }
    if (!layer->db) {
        free(layer);
        return NULL;
    }
    layer->schema = NULL;
    vec_init(&layer->morphisms);

    // Load persisted schema if reopening an existing database
    if (layer->db) {
        graph_schema_load(layer);
    }

    return layer;
}

void graph_layer_destroy(graph_layer_t* layer) {
    if (!layer) return;
    if (layer->db) {
        // Flush any pending writes (schema metadata etc.) before closing
        database_snapshot(layer->db);
        database_destroy(layer->db);
    }
    for (size_t i = 0; i < (size_t)layer->morphisms.length; i++) {
        free(layer->morphisms.data[i].name);
        query_step_t* s = layer->morphisms.data[i].steps;
        while (s) {
            query_step_t* next = s->next;
            free(s->vertex_id);
            free(s->predicate);
            free(s->has_predicate);
            free(s->has_value);
            free(s->morphism_name);
            for (size_t j = 0; j < (size_t)s->children.length; j++) {
                query_step_t* cs = s->children.data[j];
                while (cs) {
                    query_step_t* cn = cs->next;
                    free(cs->vertex_id);
                    free(cs->predicate);
                    free(cs->has_predicate);
                    free(cs->has_value);
                    free(cs->morphism_name);
                    vec_deinit(&cs->children);
                    free(cs);
                    cs = cn;
                }
            }
            vec_deinit(&s->children);
            free(s);
            s = next;
        }
    }
    if (layer->schema) {
        for (size_t i = 0; i < (size_t)layer->schema->types.length; i++) {
            free(layer->schema->types.data[i].name);
            for (size_t j = 0; j < (size_t)layer->schema->types.data[i].fields.length; j++) {
                free(layer->schema->types.data[i].fields.data[j].name);
                free(layer->schema->types.data[i].fields.data[j].type);
            }
            vec_deinit(&layer->schema->types.data[i].fields);
        }
        vec_deinit(&layer->schema->types);
        free(layer->schema);
    }
    if (layer->stats) {
        for (size_t i = 0; i < (size_t)layer->stats->predicates.length; i++) {
            free(layer->stats->predicates.data[i].predicate);
        }
        vec_deinit(&layer->stats->predicates);
        free(layer->stats);
    }
    vec_deinit(&layer->morphisms);
    free(layer);
}

database_t* graph_layer_get_db(graph_layer_t* layer) {
    return layer ? layer->db : NULL;
}

/* ── Triple operations (sync) ── */

int graph_insert_sync(graph_layer_t* layer, const char* s, const char* p, const char* o) {
    if (!layer || !s || !p || !o) return -1;
    if (!layer->db) return -1;

    vec_char_t path_spo, path_pos, path_osp, path_pso;
    uint8_t empty_val = 0;
    raw_op_t ops[4];
    int count = 0;

    if (graph_schema_needs_index(layer, p, GRAPH_INDEX_SPO)) {
        vec_init(&path_spo);
        build_path_vec(&path_spo, "spo", s, p, o);
        ops[count].key = path_spo.data;
        ops[count].key_len = path_spo.length - 1;
        ops[count].value = &empty_val;
        ops[count].value_len = 0;
        ops[count].type = 0;
        count++;
    }
    if (graph_schema_needs_index(layer, p, GRAPH_INDEX_POS)) {
        vec_init(&path_pos);
        build_path_vec(&path_pos, "pos", p, o, s);
        ops[count].key = path_pos.data;
        ops[count].key_len = path_pos.length - 1;
        ops[count].value = &empty_val;
        ops[count].value_len = 0;
        ops[count].type = 0;
        count++;
    }
    if (graph_schema_needs_index(layer, p, GRAPH_INDEX_OSP)) {
        vec_init(&path_osp);
        build_path_vec(&path_osp, "osp", o, s, p);
        ops[count].key = path_osp.data;
        ops[count].key_len = path_osp.length - 1;
        ops[count].value = &empty_val;
        ops[count].value_len = 0;
        ops[count].type = 0;
        count++;
    }
    if (graph_schema_needs_index(layer, p, GRAPH_INDEX_PSO)) {
        vec_init(&path_pso);
        build_path_vec(&path_pso, "pso", p, s, o);
        ops[count].key = path_pso.data;
        ops[count].key_len = path_pso.length - 1;
        ops[count].value = &empty_val;
        ops[count].value_len = 0;
        ops[count].type = 0;
        count++;
    }

    if (count == 0) return 0;  // Schema says no indices needed — valid

    int rc = database_batch_sync_raw(layer->db, '/', ops, count);

    if (graph_schema_needs_index(layer, p, GRAPH_INDEX_SPO)) vec_deinit(&path_spo);
    if (graph_schema_needs_index(layer, p, GRAPH_INDEX_POS)) vec_deinit(&path_pos);
    if (graph_schema_needs_index(layer, p, GRAPH_INDEX_OSP)) vec_deinit(&path_osp);
    if (graph_schema_needs_index(layer, p, GRAPH_INDEX_PSO)) vec_deinit(&path_pso);

    return rc;
}

int graph_delete_sync(graph_layer_t* layer, const char* s, const char* p, const char* o) {
    if (!layer || !s || !p || !o) return -1;
    if (!layer->db) return -1;

    vec_char_t path;
    int rc = 0;

    if (graph_schema_needs_index(layer, p, GRAPH_INDEX_SPO)) {
        vec_init(&path);
        build_path_vec(&path, "spo", s, p, o);
        rc = database_delete_sync_raw(layer->db, path.data, path.length - 1, '/');
        vec_deinit(&path);
        if (rc != 0) return rc;
    }
    if (graph_schema_needs_index(layer, p, GRAPH_INDEX_POS)) {
        vec_init(&path);
        build_path_vec(&path, "pos", p, o, s);
        rc = database_delete_sync_raw(layer->db, path.data, path.length - 1, '/');
        vec_deinit(&path);
        if (rc != 0) return rc;
    }
    if (graph_schema_needs_index(layer, p, GRAPH_INDEX_OSP)) {
        vec_init(&path);
        build_path_vec(&path, "osp", o, s, p);
        rc = database_delete_sync_raw(layer->db, path.data, path.length - 1, '/');
        vec_deinit(&path);
        if (rc != 0) return rc;
    }
    if (graph_schema_needs_index(layer, p, GRAPH_INDEX_PSO)) {
        vec_init(&path);
        build_path_vec(&path, "pso", p, s, o);
        rc = database_delete_sync_raw(layer->db, path.data, path.length - 1, '/');
        vec_deinit(&path);
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
    promise_t* promise;
} triple_work_ctx_t;

static void triple_work_execute(void* ctx) {
    triple_work_ctx_t* tc = (triple_work_ctx_t*)ctx;
    int rc = tc->is_delete
        ? graph_delete_sync(tc->layer, tc->s, tc->p, tc->o)
        : graph_insert_sync(tc->layer, tc->s, tc->p, tc->o);
    if (rc != 0) {
        async_error_t* err = ERROR("Operation failed");
        promise_reject(tc->promise, err);
        error_destroy(err);
    } else {
        promise_resolve(tc->promise, NULL);
    }
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
    if (!layer || !s || !p || !o || !promise) {
        if (promise) {
            async_error_t* err = ERROR("NULL argument");
            promise_reject(promise, err);
            error_destroy(err);
        }
        return;
    }
    if (!layer->db) {
        async_error_t* err = ERROR("Layer has no database");
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
    if (!layer || !s || !p || !o || !promise) {
        if (promise) {
            async_error_t* err = ERROR("NULL argument");
            promise_reject(promise, err);
            error_destroy(err);
        }
        return;
    }
    if (!layer->db) {
        async_error_t* err = ERROR("Layer has no database");
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
        free(s->morphism_name);
        for (size_t i = 0; i < (size_t)s->children.length; i++) {
            query_step_t* cs = s->children.data[i];
            while (cs) {
                query_step_t* cn = cs->next;
                free(cs->vertex_id);
                free(cs->predicate);
                free(cs->has_predicate);
                free(cs->has_value);
                free(cs->morphism_name);
                vec_deinit(&cs->children);
                free(cs);
                cs = cn;
            }
        }
        vec_deinit(&s->children);
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

int graph_query_has(graph_query_t* q, const char* predicate, const char* value) {
    return graph_query_has_cmp(q, predicate, value, GRAPH_CMP_EQ);
}

int graph_query_has_cmp(graph_query_t* q, const char* predicate, const char* value, graph_cmp_op_t cmp) {
    if (!q || !predicate || !value) return -1;
    query_step_t* s = alloc_step(GRAPH_STEP_HAS);
    if (!s) return -1;
    s->has_predicate = strdup(predicate);
    s->has_value = strdup(value);
    s->has_cmp = cmp;
    return append_step(q, s);
}

int graph_query_intersect(graph_query_t* q, graph_query_t* left, graph_query_t* right) {
    if (!q || !left || !right) return -1;
    query_step_t* s = alloc_step(GRAPH_STEP_INTERSECT);
    if (!s) return -1;
    vec_init(&s->children);
    vec_push(&s->children, left->head);
    vec_push(&s->children, right->head);
    left->head = NULL; left->tail = NULL;
    right->head = NULL; right->tail = NULL;
    return append_step(q, s);
}

int graph_query_union(graph_query_t* q, graph_query_t* left, graph_query_t* right) {
    if (!q || !left || !right) return -1;
    query_step_t* s = alloc_step(GRAPH_STEP_UNION);
    if (!s) return -1;
    vec_init(&s->children);
    vec_push(&s->children, left->head);
    vec_push(&s->children, right->head);
    left->head = NULL; left->tail = NULL;
    right->head = NULL; right->tail = NULL;
    return append_step(q, s);
}

int graph_query_difference(graph_query_t* q, graph_query_t* left, graph_query_t* right) {
    if (!q || !left || !right) return -1;
    query_step_t* s = alloc_step(GRAPH_STEP_DIFFERENCE);
    if (!s) return -1;
    vec_init(&s->children);
    vec_push(&s->children, left->head);
    vec_push(&s->children, right->head);
    left->head = NULL; left->tail = NULL;
    right->head = NULL; right->tail = NULL;
    return append_step(q, s);
}

int graph_query_follow(graph_query_t* q, const char* name) {
    if (!q || !name) return -1;
    query_step_t* s = alloc_step(GRAPH_STEP_MORPHISM);
    if (!s) return -1;
    s->morphism_name = strdup(name);
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
    graph_optimize_reorder_has(&q->head, q->layer);

    int rc = graph_execute_chain(q->layer, q->head, &r->set);
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
    graph_query_destroy(qc->q);
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
    graph_query_destroy(qc->q);
    async_error_t* err = ERROR("Query aborted");
    promise_reject(qc->promise, err);
    error_destroy(err);
    promise_destroy(qc->promise);
    free(qc);
}

void graph_query_execute(graph_query_t* q, promise_t* promise) {
    if (!q) return;
    if (!promise) {
        graph_query_destroy(q);
        return;
    }
    if (!q->layer || !q->layer->db) {
        async_error_t* err = ERROR("Query has no database");
        promise_reject(promise, err);
        error_destroy(err);
        graph_query_destroy(q);
        return;
    }
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

    for (size_t i = 0; i < (size_t)layer->schema->types.length; i++) {
        for (size_t j = 0; j < (size_t)layer->schema->types.data[i].fields.length; j++) {
            graph_schema_field_t* f = &layer->schema->types.data[i].fields.data[j];
            if (strcmp(f->name, predicate) == 0) {
                return (f->indices & index) != 0;
            }
        }
    }
    return 1;  // Unknown predicate: maintain index (backward compat)
}
