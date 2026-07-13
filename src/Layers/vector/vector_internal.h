#ifndef WAVEDB_VECTOR_INTERNAL_H
#define WAVEDB_VECTOR_INTERNAL_H
#include "vector_layer.h"
#include "../../Database/database.h"
#include "../../Database/database_subtree.h"
#include "../../Database/database_iterator.h"

/* Full definition — visible to vector_*.c implementation files, opaque to
   public consumers of vector_layer.h. */
struct vector_layer_t {
    database_t *db;
    database_subtree_t *subtree;   /* Non-NULL if ops route to subtree prefix */
    int owns_db;
    char *index_name;
    vector_layer_format_t format;
    vector_layer_runtime_t runtime;
};

/* Key encoding: malloc'd NUL-terminated key string, caller frees. */
char* vl_key_vector(const char *idx, char d, const char *id);
char* vl_key_count(const char *idx, char d);
char* vl_key_centroid(const char *idx, char d, int cid);
char* vl_key_cluster_member(const char *idx, char d, int cid, const char *id);
char* vl_key_hash(const char *idx, char d, const uint8_t *lsh, size_t llen, const char *id);
char* vl_key_proj(const char *idx, char d, int t);

/* Distance dispatch. */
float vl_distance(const float *a, const float *b, int dim, vl_distance_t metric);

/* Result array alloc/free. */
vl_result_t* vl_results_alloc(int n);
void vl_results_free(vl_result_t *results, int n);

/* ── Subtree-aware db op wrappers (Task 13) ──
 *
 * Each wrapper dispatches to database_subtree_* when vl->subtree is set,
 * or database_* on vl->db otherwise. Key encoders stay subtree-agnostic
 * (the subtree API prepends the prefix). Value/results freeing is the same
 * for both paths (database_raw_value_free / database_raw_results_free),
 * so no wrappers are needed for those — call the engine helpers directly.
 *
 * Returns the same rc the underlying engine call returns. */
int  vl_put(vector_layer_t *vl, const char *key, size_t klen,
            const uint8_t *value, size_t vlen);
int  vl_get(vector_layer_t *vl, const char *key, size_t klen,
            uint8_t **out, size_t *out_len);
int  vl_delete(vector_layer_t *vl, const char *key, size_t klen);
int  vl_scan_range(vector_layer_t *vl,
                   const char *start, size_t slen,
                   const char *end, size_t elen,
                   raw_result_t **results, size_t *count);
int  vl_batch(vector_layer_t *vl, const raw_op_t *ops, size_t count);

/* Iterator variants — used by SLSH search's bidirectional scan. The
 * database_scan_next/prev/end helpers operate on the iterator handle
 * directly (no db needed), so no wrappers for those. The subtree
 * iterator strips the prefix from result paths, so callers see
 * subtree-relative paths (same shape as the non-subtree case). */
database_iterator_t* vl_scan_start(vector_layer_t *vl,
                                    path_t *start_path, path_t *end_path);
database_iterator_t* vl_scan_start_reverse(vector_layer_t *vl,
                                            path_t *start_path, path_t *end_path);

/* Index-specific insert/search/etc. — stubs in Task 1, implemented Tasks 3-11. */
int vector_flat_insert_sync(vector_layer_t *vl, const char *id, const float *vec,
                            const uint8_t *metadata, size_t metadata_len);
int vector_flat_search_sync(vector_layer_t *vl, const float *query, int k,
                            vl_result_t **out, int *out_n);
int vector_flat_delete_sync(vector_layer_t *vl, const char *id);
int vector_flat_train(vector_layer_t *vl);
int vector_flat_rebuild(vector_layer_t *vl);
int vector_ivf_insert_sync(vector_layer_t *vl, const char *id, const float *vec,
                           const uint8_t *metadata, size_t metadata_len);
int vector_ivf_search_sync(vector_layer_t *vl, const float *query, int k,
                           vl_result_t **out, int *out_n);
int vector_ivf_delete_sync(vector_layer_t *vl, const char *id);
int vector_ivf_train(vector_layer_t *vl);
int vector_ivf_rebuild(vector_layer_t *vl);
int vector_slsh_insert_sync(vector_layer_t *vl, const char *id, const float *vec,
                            const uint8_t *metadata, size_t metadata_len);
int vector_slsh_search_sync(vector_layer_t *vl, const float *query, int k,
                            vl_result_t **out, int *out_n);
int vector_slsh_delete_sync(vector_layer_t *vl, const char *id);
int vector_slsh_train(vector_layer_t *vl);
int vector_slsh_rebuild(vector_layer_t *vl);
#endif