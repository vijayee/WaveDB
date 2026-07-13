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
    /* IVF tuned defaults (spike 2026-07-13: nprobe=8 clears 0.90 on
       10k/30k x 384 clustered; flat_until=1000 is the cold-start threshold
       below which exact FLAT is cheaper than IVF's centroid scan). */
    if (config->format.ivf_n_clusters <= 0) config->format.ivf_n_clusters = 50;
    if (config->runtime.ivf_nprobe <= 0) config->runtime.ivf_nprobe = 8;
    if (config->runtime.ivf_flat_until <= 0) config->runtime.ivf_flat_until = 1000;
    /* SLSH tuned defaults (spike 2026-07-13: scan_radius=200 clears 0.90
       recall@10 on 10k x 384/768 clustered; 30k+ needs radius=1000 — document
       the scaling. bucket_width=2.0 gives the best recall/latency tradeoff;
       wider buckets (10.0) hurt latency without improving recall.
       lsh_tables=4, hash_bits=16 match the spec defaults). */
    if (config->format.slsh_lsh_tables <= 0) config->format.slsh_lsh_tables = 4;
    if (config->format.slsh_hash_bits <= 0) config->format.slsh_hash_bits = 16;
    if (config->format.slsh_bucket_width <= 0) config->format.slsh_bucket_width = 2.0f;
    if (config->runtime.slsh_scan_radius <= 0) config->runtime.slsh_scan_radius = 200;
    if (config->runtime.slsh_bidirectional < 0) config->runtime.slsh_bidirectional = 1;
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

/* ── Async API (Task 12b: true promise-based async) ───────────────────
 *
 * Pattern: mirrors the Graph layer's async (src/Layers/graph/graph.c):
 *   - Public async variants return void and take a caller-owned promise_t*.
 *   - The caller creates the promise (with resolve/reject callbacks + ctx),
 *     passes it in, and wires it to their runtime (JS Promise, Dart Future,
 *     Python asyncio, or a C++ std::promise for tests).
 *   - We build a work_t with execute/abort callbacks + a context struct that
 *     COPIES the args (so the caller's buffers can be reused/freed immediately).
 *   - We REFERENCE the promise (take a worker-owned ref), YIELD the work's
 *     initial ref, and enqueue to the pool (db->pool, or the subtree's pool).
 *   - The worker's execute callback runs the sync version, resolves the
 *     promise with NULL (insert/delete/batch) or a vl_search_result_t*
 *     (search) on success, rejects with an async_error_t* on failure, then
 *     drops the worker's promise ref via promise_destroy and frees the ctx.
 *   - If no pool is available (sync_only mode or db->pool == NULL), the sync
 *     version runs INLINE and the promise is resolved before the function
 *     returns — same as graph_insert's `else` branch. */
typedef struct {
    vector_layer_t *vl;
    char *id;            /* copied (insert/delete) */
    float *vec;          /* copied: dim floats (insert/search) */
    uint8_t *metadata;   /* copied: metadata_len bytes (insert) */
    size_t metadata_len;
    int dim;
    int k;               /* search only */
    promise_t *promise;  /* worker-owned ref */
} vl_insert_ctx_t;

typedef vl_insert_ctx_t vl_search_ctx_t;
typedef vl_insert_ctx_t vl_delete_ctx_t;

typedef struct {
    vector_layer_t *vl;
    char **ids;          /* copied: n strings */
    float **vecs;        /* copied: n × dim floats */
    uint8_t **metadatas; /* copied: n metadata buffers (or NULL entries) */
    size_t *meta_lens;
    int n;
    int dim;
    promise_t *promise;
} vl_batch_ctx_t;

static void vl_insert_execute(void *ctx_) {
    vl_insert_ctx_t *c = (vl_insert_ctx_t *)ctx_;
    int rc = vector_layer_insert_sync(c->vl, c->id, c->vec, c->metadata, c->metadata_len);
    if (rc == 0) {
        promise_resolve(c->promise, NULL);
    } else {
        async_error_t *err = ERROR("Vector insert failed");
        promise_reject(c->promise, err);
        error_destroy(err);
    }
    promise_destroy(c->promise);
    free(c->id); free(c->vec); free(c->metadata);
    free(c);
}

static void vl_insert_abort(void *ctx_) {
    vl_insert_ctx_t *c = (vl_insert_ctx_t *)ctx_;
    async_error_t *err = ERROR("Vector insert aborted");
    promise_reject(c->promise, err);
    error_destroy(err);
    promise_destroy(c->promise);
    free(c->id); free(c->vec); free(c->metadata);
    free(c);
}

static void vl_search_execute(void *ctx_) {
    vl_search_ctx_t *c = (vl_search_ctx_t *)ctx_;
    vl_result_t *results = NULL; int n = 0;
    int rc = vector_layer_search_sync(c->vl, c->vec, c->k, &results, &n);
    if (rc == 0) {
        vl_search_result_t *sr = (vl_search_result_t *)get_clear_memory(sizeof(*sr));
        if (sr == NULL) {
            async_error_t *err = ERROR("Vector search OOM");
            promise_reject(c->promise, err);
            error_destroy(err);
            if (results) vector_layer_free_results(results, n);
        } else {
            sr->results = results;
            sr->n_results = n;
            promise_resolve(c->promise, sr);
        }
    } else {
        async_error_t *err = ERROR("Vector search failed");
        promise_reject(c->promise, err);
        error_destroy(err);
    }
    promise_destroy(c->promise);
    free(c->vec);
    free(c);
}

static void vl_search_abort(void *ctx_) {
    vl_search_ctx_t *c = (vl_search_ctx_t *)ctx_;
    async_error_t *err = ERROR("Vector search aborted");
    promise_reject(c->promise, err);
    error_destroy(err);
    promise_destroy(c->promise);
    free(c->vec);
    free(c);
}

static void vl_delete_execute(void *ctx_) {
    vl_delete_ctx_t *c = (vl_delete_ctx_t *)ctx_;
    int rc = vector_layer_delete_sync(c->vl, c->id);
    if (rc == 0) {
        promise_resolve(c->promise, NULL);
    } else {
        async_error_t *err = ERROR("Vector delete failed");
        promise_reject(c->promise, err);
        error_destroy(err);
    }
    promise_destroy(c->promise);
    free(c->id);
    free(c);
}

static void vl_delete_abort(void *ctx_) {
    vl_delete_ctx_t *c = (vl_delete_ctx_t *)ctx_;
    async_error_t *err = ERROR("Vector delete aborted");
    promise_reject(c->promise, err);
    error_destroy(err);
    promise_destroy(c->promise);
    free(c->id);
    free(c);
}

static void vl_batch_execute(void *ctx_) {
    vl_batch_ctx_t *c = (vl_batch_ctx_t *)ctx_;
    int rc = vector_layer_insert_batch_sync(c->vl,
                                            (const char **)c->ids,
                                            (const float **)c->vecs,
                                            (const uint8_t **)c->metadatas,
                                            c->meta_lens, c->n);
    if (rc == 0) {
        promise_resolve(c->promise, NULL);
    } else {
        async_error_t *err = ERROR("Vector batch insert failed");
        promise_reject(c->promise, err);
        error_destroy(err);
    }
    promise_destroy(c->promise);
    for (int i = 0; i < c->n; i++) {
        free(c->ids[i]);
        free(c->vecs[i]);
        if (c->metadatas) free(c->metadatas[i]);
    }
    free(c->ids); free(c->vecs); free(c->metadatas); free(c->meta_lens);
    free(c);
}

static void vl_batch_abort(void *ctx_) {
    vl_batch_ctx_t *c = (vl_batch_ctx_t *)ctx_;
    async_error_t *err = ERROR("Vector batch insert aborted");
    promise_reject(c->promise, err);
    error_destroy(err);
    promise_destroy(c->promise);
    for (int i = 0; i < c->n; i++) {
        free(c->ids[i]);
        free(c->vecs[i]);
        if (c->metadatas) free(c->metadatas[i]);
    }
    free(c->ids); free(c->vecs); free(c->metadatas); free(c->meta_lens);
    free(c);
}

/* Resolve the promise's worker-owned ref in the inline (no-pool) path: run
 * the sync impl directly and fire the promise. The ctx is freed here. */
static void vl_insert_run_inline(vl_insert_ctx_t *c) {
    int rc = vector_layer_insert_sync(c->vl, c->id, c->vec, c->metadata, c->metadata_len);
    if (rc == 0) promise_resolve(c->promise, NULL);
    else { async_error_t *e = ERROR("Vector insert failed");
           promise_reject(c->promise, e); error_destroy(e); }
    promise_destroy(c->promise);
    free(c->id); free(c->vec); free(c->metadata); free(c);
}
static void vl_search_run_inline(vl_search_ctx_t *c) {
    vl_result_t *results = NULL; int n = 0;
    int rc = vector_layer_search_sync(c->vl, c->vec, c->k, &results, &n);
    if (rc == 0) {
        vl_search_result_t *sr = (vl_search_result_t *)get_clear_memory(sizeof(*sr));
        if (sr == NULL) {
            async_error_t *e = ERROR("Vector search OOM");
            promise_reject(c->promise, e); error_destroy(e);
            if (results) vector_layer_free_results(results, n);
        } else {
            sr->results = results; sr->n_results = n;
            promise_resolve(c->promise, sr);
        }
    } else {
        async_error_t *e = ERROR("Vector search failed");
        promise_reject(c->promise, e); error_destroy(e);
    }
    promise_destroy(c->promise);
    free(c->vec); free(c);
}
static void vl_delete_run_inline(vl_delete_ctx_t *c) {
    int rc = vector_layer_delete_sync(c->vl, c->id);
    if (rc == 0) promise_resolve(c->promise, NULL);
    else { async_error_t *e = ERROR("Vector delete failed");
           promise_reject(c->promise, e); error_destroy(e); }
    promise_destroy(c->promise);
    free(c->id); free(c);
}
static void vl_batch_run_inline(vl_batch_ctx_t *c) {
    int rc = vector_layer_insert_batch_sync(c->vl,
                                            (const char **)c->ids,
                                            (const float **)c->vecs,
                                            (const uint8_t **)c->metadatas,
                                            c->meta_lens, c->n);
    if (rc == 0) promise_resolve(c->promise, NULL);
    else { async_error_t *e = ERROR("Vector batch insert failed");
           promise_reject(c->promise, e); error_destroy(e); }
    promise_destroy(c->promise);
    for (int i = 0; i < c->n; i++) {
        free(c->ids[i]); free(c->vecs[i]);
        if (c->metadatas) free(c->metadatas[i]);
    }
    free(c->ids); free(c->vecs); free(c->metadatas); free(c->meta_lens);
    free(c);
}

/* Public async variants — Graph-style: return void, take a caller-owned
 * promise_t*. See header comment for the contract. */

void vector_layer_insert(vector_layer_t *vl, const char *id, const float *vec,
                         const uint8_t *metadata, size_t metadata_len,
                         promise_t *promise) {
    if (vl == NULL || id == NULL || vec == NULL || promise == NULL) {
        if (promise) {
            async_error_t *err = ERROR("NULL argument");
            promise_reject(promise, err);
            error_destroy(err);
        }
        return;
    }
    if (vl->db == NULL) {
        async_error_t *err = ERROR("Layer has no database");
        promise_reject(promise, err);
        error_destroy(err);
        return;
    }
    int dim = vl->format.dim;
    vl_insert_ctx_t *c = (vl_insert_ctx_t *)get_clear_memory(sizeof(*c));
    if (c == NULL) {
        async_error_t *err = ERROR("OOM");
        promise_reject(promise, err);
        error_destroy(err);
        return;
    }
    c->vl = vl;
    c->dim = dim;
    c->metadata_len = metadata_len;
    c->id = strdup(id);
    c->vec = (float *)get_clear_memory(sizeof(float) * dim);
    if (c->id == NULL || c->vec == NULL) {
        free(c->id); free(c->vec); free(c);
        async_error_t *err = ERROR("OOM");
        promise_reject(promise, err);
        error_destroy(err);
        return;
    }
    memcpy(c->vec, vec, sizeof(float) * dim);
    if (metadata && metadata_len > 0) {
        c->metadata = (uint8_t *)get_memory(metadata_len);
        if (c->metadata == NULL) {
            free(c->id); free(c->vec); free(c->metadata); free(c);
            async_error_t *err = ERROR("OOM");
            promise_reject(promise, err);
            error_destroy(err);
            return;
        }
        memcpy(c->metadata, metadata, metadata_len);
    }
    c->promise = REFERENCE(promise, promise_t);

    work_t *work = work_create(vl_insert_execute, vl_insert_abort, c);
    if (work == NULL) {
        /* Fall back to inline — frees ctx + fires promise. */
        vl_insert_run_inline(c);
        return;
    }
    work_pool_t *pool = vl->subtree ? database_subtree_get_pool(vl->subtree) : vl->db->pool;
    if (vl->runtime.sync_only || pool == NULL) {
        vl_insert_run_inline(c);
        work_destroy(work);
        return;
    }
    refcounter_yield((refcounter_t *)work);
    if (work_pool_enqueue(pool, work) != 0) {
        /* Pool rejected — run inline. Two derefs: consume yield + free. */
        vl_insert_run_inline(c);
        work_destroy(work);
        work_destroy(work);
    }
}

void vector_layer_search(vector_layer_t *vl, const float *query, int k,
                         promise_t *promise) {
    if (vl == NULL || query == NULL || promise == NULL) {
        if (promise) {
            async_error_t *err = ERROR("NULL argument");
            promise_reject(promise, err);
            error_destroy(err);
        }
        return;
    }
    if (vl->db == NULL) {
        async_error_t *err = ERROR("Layer has no database");
        promise_reject(promise, err);
        error_destroy(err);
        return;
    }
    if (k <= 0) k = vl->runtime.top_k;
    int dim = vl->format.dim;
    vl_search_ctx_t *c = (vl_search_ctx_t *)get_clear_memory(sizeof(*c));
    if (c == NULL) {
        async_error_t *err = ERROR("OOM");
        promise_reject(promise, err);
        error_destroy(err);
        return;
    }
    c->vl = vl;
    c->dim = dim;
    c->k = k;
    c->vec = (float *)get_clear_memory(sizeof(float) * dim);
    if (c->vec == NULL) {
        free(c->vec); free(c);
        async_error_t *err = ERROR("OOM");
        promise_reject(promise, err);
        error_destroy(err);
        return;
    }
    memcpy(c->vec, query, sizeof(float) * dim);
    c->promise = REFERENCE(promise, promise_t);

    work_t *work = work_create(vl_search_execute, vl_search_abort, c);
    if (work == NULL) {
        vl_search_run_inline(c);
        return;
    }
    work_pool_t *pool = vl->subtree ? database_subtree_get_pool(vl->subtree) : vl->db->pool;
    if (vl->runtime.sync_only || pool == NULL) {
        vl_search_run_inline(c);
        work_destroy(work);
        return;
    }
    refcounter_yield((refcounter_t *)work);
    if (work_pool_enqueue(pool, work) != 0) {
        vl_search_run_inline(c);
        work_destroy(work);
        work_destroy(work);
    }
}

void vector_layer_delete(vector_layer_t *vl, const char *id, promise_t *promise) {
    if (vl == NULL || id == NULL || promise == NULL) {
        if (promise) {
            async_error_t *err = ERROR("NULL argument");
            promise_reject(promise, err);
            error_destroy(err);
        }
        return;
    }
    if (vl->db == NULL) {
        async_error_t *err = ERROR("Layer has no database");
        promise_reject(promise, err);
        error_destroy(err);
        return;
    }
    vl_delete_ctx_t *c = (vl_delete_ctx_t *)get_clear_memory(sizeof(*c));
    if (c == NULL) {
        async_error_t *err = ERROR("OOM");
        promise_reject(promise, err);
        error_destroy(err);
        return;
    }
    c->vl = vl;
    c->id = strdup(id);
    if (c->id == NULL) {
        free(c);
        async_error_t *err = ERROR("OOM");
        promise_reject(promise, err);
        error_destroy(err);
        return;
    }
    c->promise = REFERENCE(promise, promise_t);

    work_t *work = work_create(vl_delete_execute, vl_delete_abort, c);
    if (work == NULL) {
        vl_delete_run_inline(c);
        return;
    }
    work_pool_t *pool = vl->subtree ? database_subtree_get_pool(vl->subtree) : vl->db->pool;
    if (vl->runtime.sync_only || pool == NULL) {
        vl_delete_run_inline(c);
        work_destroy(work);
        return;
    }
    refcounter_yield((refcounter_t *)work);
    if (work_pool_enqueue(pool, work) != 0) {
        vl_delete_run_inline(c);
        work_destroy(work);
        work_destroy(work);
    }
}

void vector_layer_insert_batch(vector_layer_t *vl,
                               const char **ids, const float **vecs,
                               const uint8_t **metadatas, const size_t *meta_lens,
                               int n, promise_t *promise) {
    if (vl == NULL || ids == NULL || vecs == NULL || promise == NULL || n <= 0) {
        if (promise) {
            async_error_t *err = ERROR("NULL argument");
            promise_reject(promise, err);
            error_destroy(err);
        }
        return;
    }
    if (vl->db == NULL) {
        async_error_t *err = ERROR("Layer has no database");
        promise_reject(promise, err);
        error_destroy(err);
        return;
    }
    int dim = vl->format.dim;
    vl_batch_ctx_t *c = (vl_batch_ctx_t *)get_clear_memory(sizeof(*c));
    if (c == NULL) {
        async_error_t *err = ERROR("OOM");
        promise_reject(promise, err);
        error_destroy(err);
        return;
    }
    c->vl = vl;
    c->n = n;
    c->dim = dim;
    c->ids = (char **)get_clear_memory(sizeof(char *) * n);
    c->vecs = (float **)get_clear_memory(sizeof(float *) * n);
    c->meta_lens = (size_t *)get_clear_memory(sizeof(size_t) * n);
    if (metadatas) {
        c->metadatas = (uint8_t **)get_clear_memory(sizeof(uint8_t *) * n);
    }
    if (c->ids == NULL || c->vecs == NULL || c->meta_lens == NULL ||
        (metadatas && c->metadatas == NULL)) {
        /* Roll back any partial allocations. */
        if (c->ids) { for (int i = 0; i < n; i++) free(c->ids[i]); free(c->ids); }
        if (c->vecs) { for (int i = 0; i < n; i++) free(c->vecs[i]); free(c->vecs); }
        if (c->metadatas) { for (int i = 0; i < n; i++) free(c->metadatas[i]); free(c->metadatas); }
        free(c->meta_lens); free(c);
        async_error_t *err = ERROR("OOM");
        promise_reject(promise, err);
        error_destroy(err);
        return;
    }
    for (int i = 0; i < n; i++) {
        c->ids[i] = strdup(ids[i]);
        c->vecs[i] = (float *)get_clear_memory(sizeof(float) * dim);
        if (c->ids[i] == NULL || c->vecs[i] == NULL) {
            /* Roll back up to i. */
            for (int j = 0; j <= i; j++) { free(c->ids[j]); free(c->vecs[j]); }
            if (c->metadatas) { for (int j = 0; j < i; j++) free(c->metadatas[j]); free(c->metadatas); }
            free(c->ids); free(c->vecs); free(c->meta_lens); free(c);
            async_error_t *err = ERROR("OOM");
            promise_reject(promise, err);
            error_destroy(err);
            return;
        }
        memcpy(c->vecs[i], vecs[i], sizeof(float) * dim);
        c->meta_lens[i] = meta_lens ? meta_lens[i] : 0;
        if (metadatas && metadatas[i] && c->meta_lens[i] > 0) {
            c->metadatas[i] = (uint8_t *)get_memory(c->meta_lens[i]);
            if (c->metadatas[i] == NULL) {
                /* Roll back up to i, plus the metadatas already filled. */
                for (int j = 0; j <= i; j++) { free(c->ids[j]); free(c->vecs[j]); }
                for (int j = 0; j < i; j++) free(c->metadatas[j]);
                free(c->metadatas); free(c->ids); free(c->vecs);
                free(c->meta_lens); free(c);
                async_error_t *err = ERROR("OOM");
                promise_reject(promise, err);
                error_destroy(err);
                return;
            }
            memcpy(c->metadatas[i], metadatas[i], c->meta_lens[i]);
        } else if (c->metadatas) {
            c->metadatas[i] = NULL;
        }
    }
    c->promise = REFERENCE(promise, promise_t);

    work_t *work = work_create(vl_batch_execute, vl_batch_abort, c);
    if (work == NULL) {
        vl_batch_run_inline(c);
        return;
    }
    work_pool_t *pool = vl->subtree ? database_subtree_get_pool(vl->subtree) : vl->db->pool;
    if (vl->runtime.sync_only || pool == NULL) {
        vl_batch_run_inline(c);
        work_destroy(work);
        return;
    }
    refcounter_yield((refcounter_t *)work);
    if (work_pool_enqueue(pool, work) != 0) {
        vl_batch_run_inline(c);
        work_destroy(work);
        work_destroy(work);
    }
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