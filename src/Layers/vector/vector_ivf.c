/* vector_ivf.c — inverted file.
 * Task 1: stubs only. Tasks 6-8 implement. */
#include "vector_internal.h"

int vector_ivf_insert_sync(vector_layer_t *vl, const char *id, const float *vec,
                           const uint8_t *metadata, size_t metadata_len) {
    (void)vl; (void)id; (void)vec; (void)metadata; (void)metadata_len;
    return -1;
}

int vector_ivf_search_sync(vector_layer_t *vl, const float *query, int k,
                           vl_result_t **out, int *out_n) {
    (void)vl; (void)query; (void)k; (void)out; (void)out_n;
    return -1;
}

int vector_ivf_delete_sync(vector_layer_t *vl, const char *id) {
    (void)vl; (void)id;
    return -1;
}

int vector_ivf_train(vector_layer_t *vl) {
    (void)vl;
    return 0;
}

int vector_ivf_rebuild(vector_layer_t *vl) {
    (void)vl;
    return 0;
}