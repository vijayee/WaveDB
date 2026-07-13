#ifndef WAVEDB_VECTOR_INTERNAL_H
#define WAVEDB_VECTOR_INTERNAL_H
#include "vector_layer.h"
#include "../../Database/database.h"

/* Full definition — visible to vector_*.c implementation files, opaque to
   public consumers of vector_layer.h. */
struct vector_layer_t {
    database_t *db;
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
char* vl_key_clist(const char *idx, char d, int cid);
char* vl_key_hash(const char *idx, char d, const uint8_t *lsh, size_t llen, const char *id);
char* vl_key_proj(const char *idx, char d, int t);

/* Distance dispatch. */
float vl_distance(const float *a, const float *b, int dim, vl_distance_t metric);

/* Result array alloc/free. */
vl_result_t* vl_results_alloc(int n);
void vl_results_free(vl_result_t *results, int n);

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