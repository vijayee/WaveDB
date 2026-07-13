/* vector_layer.c — lifecycle, config validation, dispatch by index_type.
 * Task 1: skeleton. Tasks 3-11 fill in the index-specific impls. */
#include "vector_layer.h"
#include "vector_internal.h"
#include "../../Database/database.h"
#include "../../Database/database_config.h"
#include "../../Util/allocator.h"
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
    vl->db = db;
    vl->owns_db = 0;
    (void)subtree;  /* subtree support added in Task 13 */
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
    database_config_set_sync_only(cfg, 1);
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
    if (vl->owns_db && vl->db) database_destroy(vl->db);
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
    int rc = database_get_sync_raw(vl->db, key, strlen(key), vl->format.delimiter,
                                   &buf, &out_len);
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

/* Async stubs — Task 12 implements via Workers/promise. For now, route to sync. */
int vector_layer_insert(vector_layer_t *vl, const char *id, const float *vec,
                        const uint8_t *metadata, size_t metadata_len) {
    return vector_layer_insert_sync(vl, id, vec, metadata, metadata_len);
}

int vector_layer_insert_batch(vector_layer_t *vl, const char **ids, const float **vecs,
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

int vector_layer_insert_batch_sync(vector_layer_t *vl, const char **ids, const float **vecs,
                                   const uint8_t **metadatas, const size_t *meta_lens, int n) {
    return vector_layer_insert_batch(vl, ids, vecs, metadatas, meta_lens, n);
}

int vector_layer_search(vector_layer_t *vl, const float *query, int k,
                        vl_result_t **results, int *n_results) {
    return vector_layer_search_sync(vl, query, k, results, n_results);
}

int vector_layer_delete(vector_layer_t *vl, const char *id) {
    return vector_layer_delete_sync(vl, id);
}

void vector_layer_free_results(vl_result_t *results, int n) {
    vl_results_free(results, n);
}