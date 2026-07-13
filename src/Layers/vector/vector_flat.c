/* vector_flat.c — exact brute-force (baseline + IVF cold-start).
 * Task 1: stubs only. Tasks 3-5 implement. */
#include "vector_internal.h"

int vector_flat_insert_sync(vector_layer_t *vl, const char *id, const float *vec,
                            const uint8_t *metadata, size_t metadata_len) {
    (void)vl; (void)id; (void)vec; (void)metadata; (void)metadata_len;
    return -1;
}

int vector_flat_search_sync(vector_layer_t *vl, const float *query, int k,
                            vl_result_t **out, int *out_n) {
    (void)vl; (void)query; (void)k; (void)out; (void)out_n;
    return -1;
}

int vector_flat_delete_sync(vector_layer_t *vl, const char *id) {
    (void)vl; (void)id;
    return -1;
}

int vector_flat_train(vector_layer_t *vl) {
    (void)vl;
    return 0;
}

int vector_flat_rebuild(vector_layer_t *vl) {
    (void)vl;
    return 0;
}