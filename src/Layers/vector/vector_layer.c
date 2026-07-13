/* vector_layer.c — lifecycle, config validation, dispatch by index_type.
 * Task 1: skeleton. Tasks 3-11 fill in the index-specific impls. */
#include "vector_layer.h"
#include "vector_internal.h"
#include "../../Database/database.h"
#include "../../Database/database_config.h"
#include "../../Util/allocator.h"
#include "../../Util/threadding.h"
#include "../../Workers/promise.h"
#include "../../Workers/work.h"
#include "../../Workers/pool.h"
#include "../../RefCounter/refcounter.h"
#include <stdlib.h>
#include <string.h>

static int vl_init(vector_layer_t *vl, database_t *db, database_subtree_t *subtree,
                   const char *index_name, vector_layer_config_t *config) {
    if (vl == NULL || index_name == NULL || config == NULL) return -22;
    if (config->format.dim <= 0) return -22;
    if (config->format.delimiter == 0) config->format.delimiter = '/';
    if (config->runtime.top_k <= 0) config->runtime.top_k = 10;
    vl->format = config->format;
    vl->runtime = config->runtime;
    vl->index_name = strdup(index_name);
    if (vl->index_name == NULL) return -12;
    if (subtree) {
        /* Subtree mode: use the root db from the subtree, route ops through
         * the subtree so keys land under the prefix. Take a reference on the
         * subtree so it stays alive until vector_layer_destroy drops it. */
        vl->db = database_subtree_get_db(subtree);
        vl->subtree = subtree;
        refcounter_reference((refcounter_t*)subtree);
    } else {
        vl->db = db;
        vl->subtree = NULL;
    }
    vl->owns_db = 0;
    return 0;
}

vector_layer_t* vector_layer_create(const char *index_name, database_t *db,
                                     database_subtree_t *subtree,
                                     vector_layer_config_t *config, int *error_code) {
    if (error_code) *error_code = 0;
    if (db == NULL) {
        if (error_code) *error_code = -22;
        return NULL;
    }
    vector_layer_t *vl = (vector_layer_t*)get_clear_memory(sizeof(*vl));
    if (vl == NULL) {
        if (error_code) *error_code = -12;
        return NULL;
    }
    int rc = vl_init(vl, db, subtree, index_name, config);
    if (rc != 0) {
        if (error_code) *error_code = rc;
        free(vl->index_name);
        free(vl);
        return NULL;
    }
    return vl;
}

vector_layer_t* vector_layer_open_separate(const char *db_location, const char *index_name,
                                            vector_layer_config_t *config, int *error_code) {
    if (error_code) *error_code = 0;
    if (db_location == NULL) {
        if (error_code) *error_code = -22;
        return NULL;
    }
    database_config_t *cfg = database_config_default();
    if (cfg == NULL) {
        if (error_code) *error_code = -12;
        return NULL;
    }
    database_config_set_sync_only(cfg, config->runtime.sync_only ? 1 : 0);
    int db_err = 0;
    database_t *db = database_create_with_config(db_location, cfg, &db_err);
    database_config_destroy(cfg);
    if (db == NULL) {
        if (error_code) *error_code = db_err;
        return NULL;
    }
    vector_layer_t *vl = (vector_layer_t*)get_clear_memory(sizeof(*vl));
    if (vl == NULL) {
        database_destroy(db);
        if (error_code) *error_code = -12;
        return NULL;
    }
    int rc = vl_init(vl, db, NULL, index_name, config);
    if (rc != 0) {
        database_destroy(db);
        if (error_code) *error_code = rc;
        free(vl->index_name);
        free(vl);
        return NULL;
    }
    vl->owns_db = 1;
    return vl;
}

void vector_layer_destroy(vector_layer_t *vl) {
    if (vl == NULL) return;
    free(vl->index_name);
    if (vl->subtree) {
        /* Subtree mode: drop the layer's reference on the subtree. The caller
         * still holds their own reference (from database_subtree_open) and
         * closes it separately. Do NOT destroy the shared db. */
        database_subtree_close(vl->subtree);
        vl->subtree = NULL;
        vl->db = NULL;
    } else if (vl->owns_db && vl->db) {
        database_destroy(vl->db);
    }
    vl->db = NULL;
    free(vl);
}

int vector_layer_reconfigure(vector_layer_t *vl, vector_layer_runtime_t *runtime) {
    if (vl == NULL || runtime == NULL) return -22;
    vl->runtime = *runtime;
    return 0;
}

size_t vector_layer_count(vector_layer_t *vl) {
    if (vl == NULL || vl->db == NULL) return 0;
    char *key = vl_key_count(vl->index_name, vl->format.delimiter);
    if (key == NULL) return 0;
    size_t out_len = 0;
    uint8_t *buf = NULL;
    int rc = vl_get(vl, key, strlen(key), &buf, &out_len);
    free(key);
    if (rc != 0 || buf == NULL || out_len < sizeof(size_t)) {
        if (buf) database_raw_value_free(buf);
        return 0;
    }
    size_t count;
    memcpy(&count, buf, sizeof(size_t));
    database_raw_value_free(buf);
    return count;
}

/* Dispatch — Task 3+ fills in the index-specific impls. For Task 1, route
   insert_sync to the flat stub (returns -1). The tests don't call insert yet. */
int vector_layer_insert_sync(vector_layer_t *vl, const char *id, const float *vec,
                             const uint8_t *metadata, size_t metadata_len) {
    if (vl == NULL) return -22;
    switch (vl->format.index_type) {
        case VL_INDEX_FLAT: return vector_flat_insert_sync(vl, id, vec, metadata, metadata_len);
        case VL_INDEX_IVF:  return vector_ivf_insert_sync(vl, id, vec, metadata, metadata_len);
        case VL_INDEX_SLSH: return vector_slsh_insert_sync(vl, id, vec, metadata, metadata_len);
    }
    return -22;
}

int vector_layer_search_sync(vector_layer_t *vl, const float *query, int k,
                             vl_result_t **results, int *n_results) {
    if (vl == NULL) return -22;
    if (k <= 0) k = vl->runtime.top_k;
    switch (vl->format.index_type) {
        case VL_INDEX_FLAT: return vector_flat_search_sync(vl, query, k, results, n_results);
        case VL_INDEX_IVF:  return vector_ivf_search_sync(vl, query, k, results, n_results);
        case VL_INDEX_SLSH: return vector_slsh_search_sync(vl, query, k, results, n_results);
    }
    return -22;
}

int vector_layer_delete_sync(vector_layer_t *vl, const char *id) {
    if (vl == NULL) return -22;
    switch (vl->format.index_type) {
        case VL_INDEX_FLAT: return vector_flat_delete_sync(vl, id);
        case VL_INDEX_IVF:  return vector_ivf_delete_sync(vl, id);
        case VL_INDEX_SLSH: return vector_slsh_delete_sync(vl, id);
    }
    return -22;
}

int vector_layer_train(vector_layer_t *vl) {
    if (vl == NULL) return -22;
    switch (vl->format.index_type) {
        case VL_INDEX_FLAT: return vector_flat_train(vl);
        case VL_INDEX_IVF:  return vector_ivf_train(vl);
        case VL_INDEX_SLSH: return vector_slsh_train(vl);
    }
    return -22;
}

int vector_layer_rebuild(vector_layer_t *vl) {
    if (vl == NULL) return -22;
    switch (vl->format.index_type) {
        case VL_INDEX_FLAT: return vector_flat_rebuild(vl);
        case VL_INDEX_IVF:  return vector_ivf_rebuild(vl);
        case VL_INDEX_SLSH: return vector_slsh_rebuild(vl);
    }
    return -22;
}

/* ── Async API (Task 12) ─────────────────────────────────────────────
 *
 * Pattern: mirrors the Graph layer's async (src/Layers/graph/graph.c):
 *   - Build a work_t with execute/abort callbacks + a context struct.
 *   - Create a promise_t with resolve/reject callbacks.
 *   - Enqueue to the database's work_pool (db->pool).
 *   - If no pool is available (sync_only mode), fall back to running the
 *     sync version inline (same as graph_insert's `else` branch).
 *
 * Difference from Graph: the vector layer's async API returns `int` and
 * fills out-pointers (results, n_results), so it is "blocking async" —
 * the caller blocks on a condvar until the worker resolves the promise.
 * Graph instead returns void and hands the caller the promise to wire up
 * (e.g. the Node binding bridges the promise to a JS Promise). The C
 * vector API chose the blocking shape because it has no external caller
 * to attach callbacks; the bindings (Tasks 18-20) can wrap this in their
 * own promise/future if they want non-blocking behavior, or call the
 * *_sync variants directly on a worker thread.
 *
 * When vl->runtime.sync_only is set OR db->pool is NULL, the async
 * variants route to the sync versions (no enqueue). This matches
 * Graph's "no pool → run inline" fallback. */

typedef enum { VL_ASYNC_INSERT, VL_ASYNC_SEARCH, VL_ASYNC_DELETE } vl_async_kind_t;

typedef struct {
    vector_layer_t *vl;
    vl_async_kind_t kind;
    /* insert/delete args (owned by ctx) */
    char *id;
    float *vec;            /* dim floats (insert/search) */
    uint8_t *metadata;     /* metadata_len bytes (insert only) */
    size_t metadata_len;
    int k;                 /* search only */
    /* outputs (filled by worker) */
    int rc;
    vl_result_t *results;  /* search only — ownership transferred to caller */
    int n_results;
    /* sync — caller waits on this */
    PLATFORMLOCKTYPE(mtx);
    PLATFORMCONDITIONTYPE(cv);
    uint8_t done;
    promise_t *promise;
} vl_async_ctx_t;

static void vl_promise_resolved(void *ctx_, void *payload) {
    (void)payload;
    vl_async_ctx_t *c = (vl_async_ctx_t *)ctx_;
    platform_lock(&c->mtx);
    c->done = 1;
    platform_broadcast_condition(&c->cv);
    platform_unlock(&c->mtx);
}

static void vl_promise_rejected(void *ctx_, async_error_t *err) {
    (void)err;
    vl_async_ctx_t *c = (vl_async_ctx_t *)ctx_;
    platform_lock(&c->mtx);
    c->done = 1;
    c->rc = -1;
    platform_broadcast_condition(&c->cv);
    platform_unlock(&c->mtx);
}

static void vl_work_run_sync(vl_async_ctx_t *c) {
    switch (c->kind) {
        case VL_ASYNC_INSERT:
            c->rc = vector_layer_insert_sync(c->vl, c->id, c->vec,
                                             c->metadata, c->metadata_len);
            break;
        case VL_ASYNC_SEARCH:
            c->rc = vector_layer_search_sync(c->vl, c->vec, c->k,
                                             &c->results, &c->n_results);
            break;
        case VL_ASYNC_DELETE:
            c->rc = vector_layer_delete_sync(c->vl, c->id);
            break;
    }
}

static void vl_work_execute(void *ctx_) {
    vl_async_ctx_t *c = (vl_async_ctx_t *)ctx_;
    vl_work_run_sync(c);
    if (c->promise) {
        promise_resolve(c->promise, NULL);
    }
}

static void vl_work_abort(void *ctx_) {
    vl_async_ctx_t *c = (vl_async_ctx_t *)ctx_;
    async_error_t *err = ERROR("Vector async operation aborted");
    promise_reject(c->promise, err);
    error_destroy(err);
}

static void vl_async_ctx_destroy(vl_async_ctx_t *c) {
    if (c == NULL) return;
    platform_lock_destroy(&c->mtx);
    platform_condition_destroy(&c->cv);
    free(c->id);
    free(c->vec);
    free(c->metadata);
    /* results ownership transferred to caller via the out-pointer; do not free here. */
    free(c);
}

/* Runs the async op on the worker pool, blocking until it completes.
 * Returns the rc from the sync impl, or -1 on abort/reject. Does NOT
 * destroy the ctx — the caller reads outputs from it, then destroys it. */
static int vl_async_run(vector_layer_t *vl, vl_async_ctx_t *c) {
    if (vl == NULL || vl->db == NULL) {
        return -22;
    }
    work_pool_t *pool = vl->db->pool;
    /* sync_only or no pool → run inline (mirrors graph_insert fallback).
       No promise is created in this path; the caller reads c->rc directly. */
    if (vl->runtime.sync_only || pool == NULL) {
        vl_work_run_sync(c);
        return c->rc;
    }
    /* Real async: enqueue + block on condvar. */
    c->promise = promise_create(vl_promise_resolved, vl_promise_rejected, c);
    if (c->promise == NULL) {
        return -12;
    }
    work_t *work = work_create(vl_work_execute, vl_work_abort, c);
    if (work == NULL) {
        promise_destroy(c->promise);
        c->promise = NULL;
        return -12;
    }
    /* Yield the work's initial ref BEFORE enqueue so the worker's
       refcounter_reference consumes the yield (not increments count).
       Same pattern as graph_insert. */
    refcounter_yield((refcounter_t *)work);
    if (work_pool_enqueue(pool, work) != 0) {
        /* Pool stopped / enqueue failed — run inline as a fallback, then
           destroy the work_t. Two dereferences: first consumes the yield,
           second decrements count to 0 and frees. We must resolve the
           promise ourselves since vl_work_run_sync doesn't. */
        vl_work_run_sync(c);
        promise_resolve(c->promise, NULL);
        work_destroy(work);
        work_destroy(work);
    }
    /* Wait for the promise to fire. */
    platform_lock(&c->mtx);
    while (!c->done) {
        platform_condition_wait(&c->mtx, &c->cv);
    }
    int rc = c->rc;
    platform_unlock(&c->mtx);
    /* Drop our reference to the promise. The worker does NOT destroy the
       promise (unlike graph_insert's triple_work_execute) — we own it
       solely so we can wait on its callbacks. */
    promise_destroy(c->promise);
    c->promise = NULL;
    return rc;
}

/* Public async variants. */

int vector_layer_insert(vector_layer_t *vl, const char *id, const float *vec,
                        const uint8_t *metadata, size_t metadata_len) {
    if (vl == NULL || id == NULL || vec == NULL) return -22;
    int dim = vl->format.dim;
    vl_async_ctx_t *c = (vl_async_ctx_t *)get_clear_memory(sizeof(*c));
    if (c == NULL) return -12;
    platform_lock_init(&c->mtx);
    platform_condition_init(&c->cv);
    c->vl = vl;
    c->kind = VL_ASYNC_INSERT;
    c->id = strdup(id);
    c->vec = (float *)get_clear_memory(sizeof(float) * dim);
    if (c->id == NULL || c->vec == NULL) { vl_async_ctx_destroy(c); return -12; }
    memcpy(c->vec, vec, sizeof(float) * dim);
    if (metadata && metadata_len > 0) {
        c->metadata = (uint8_t *)get_memory(metadata_len);
        if (c->metadata == NULL) { vl_async_ctx_destroy(c); return -12; }
        memcpy(c->metadata, metadata, metadata_len);
        c->metadata_len = metadata_len;
    }
    int rc = vl_async_run(vl, c);
    vl_async_ctx_destroy(c);
    return rc;
}

int vector_layer_search(vector_layer_t *vl, const float *query, int k,
                        vl_result_t **results, int *n_results) {
    if (vl == NULL || query == NULL || results == NULL || n_results == NULL) return -22;
    if (k <= 0) k = vl->runtime.top_k;
    int dim = vl->format.dim;
    vl_async_ctx_t *c = (vl_async_ctx_t *)get_clear_memory(sizeof(*c));
    if (c == NULL) return -12;
    platform_lock_init(&c->mtx);
    platform_condition_init(&c->cv);
    c->vl = vl;
    c->kind = VL_ASYNC_SEARCH;
    c->k = k;
    c->vec = (float *)get_clear_memory(sizeof(float) * dim);
    if (c->vec == NULL) { vl_async_ctx_destroy(c); return -12; }
    memcpy(c->vec, query, sizeof(float) * dim);
    int rc = vl_async_run(vl, c);
    /* If the op succeeded, transfer results ownership to the caller. */
    if (rc == 0) {
        *results = c->results;
        *n_results = c->n_results;
        c->results = NULL;  /* prevent vl_async_ctx_destroy from freeing them */
    } else {
        *results = NULL;
        *n_results = 0;
    }
    vl_async_ctx_destroy(c);
    return rc;
}

int vector_layer_delete(vector_layer_t *vl, const char *id) {
    if (vl == NULL || id == NULL) return -22;
    vl_async_ctx_t *c = (vl_async_ctx_t *)get_clear_memory(sizeof(*c));
    if (c == NULL) return -12;
    platform_lock_init(&c->mtx);
    platform_condition_init(&c->cv);
    c->vl = vl;
    c->kind = VL_ASYNC_DELETE;
    c->id = strdup(id);
    if (c->id == NULL) { vl_async_ctx_destroy(c); return -12; }
    int rc = vl_async_run(vl, c);
    vl_async_ctx_destroy(c);
    return rc;
}

int vector_layer_insert_batch(vector_layer_t *vl, const char **ids, const float **vecs,
                              const uint8_t **metadatas, const size_t *meta_lens, int n) {
    if (vl == NULL) return -22;
    if (n <= 0) return 0;
    int rc = 0;
    for (int i = 0; i < n && rc == 0; i++) {
        const uint8_t *meta = metadatas ? metadatas[i] : NULL;
        size_t mlen = meta_lens ? meta_lens[i] : 0;
        rc = vector_layer_insert(vl, ids[i], vecs[i], meta, mlen);
    }
    return rc;
}

int vector_layer_insert_batch_sync(vector_layer_t *vl, const char **ids, const float **vecs,
                                   const uint8_t **metadatas, const size_t *meta_lens, int n) {
    if (vl == NULL) return -22;
    if (n <= 0) return 0;
    int rc = 0;
    for (int i = 0; i < n && rc == 0; i++) {
        const uint8_t *meta = metadatas ? metadatas[i] : NULL;
        size_t mlen = meta_lens ? meta_lens[i] : 0;
        rc = vector_layer_insert_sync(vl, ids[i], vecs[i], meta, mlen);
    }
    return rc;
}

void vector_layer_free_results(vl_result_t *results, int n) {
    vl_results_free(results, n);
}