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
    int* res = (int*)get_memory(sizeof(int));
    *res = result;
    promise_resolve(tc->promise, res);
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

struct graph_query_t {
    graph_layer_t* layer;
    query_step_t* head;
    query_step_t* tail;
};

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
        for (size_t i = 0; i < s->num_children; i++) {
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

// graph_result_t struct defined at line ~295 (before graph_query_execute_sync)

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
