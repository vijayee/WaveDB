#include "vector_internal.h"
#include "../../Util/allocator.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char* vl_key_count(const char *idx, char d) {
    if (idx == NULL) return NULL;
    /* "vec" + d + idx + d + "count" + '\0' */
    size_t cap = 3 + 1 + strlen(idx) + 1 + 5 + 1;
    char *buf = (char*)malloc(cap);
    if (buf == NULL) return NULL;
    snprintf(buf, cap, "vec%c%s%ccount", d, idx, d);
    return buf;
}

/* Stubs — Task 2 implements these. */
char* vl_key_vector(const char *idx, char d, const char *id) {
    (void)idx; (void)d; (void)id; return NULL;
}
char* vl_key_centroid(const char *idx, char d, int cid) {
    (void)idx; (void)d; (void)cid; return NULL;
}
char* vl_key_cluster_member(const char *idx, char d, int cid, const char *id) {
    (void)idx; (void)d; (void)cid; (void)id; return NULL;
}
char* vl_key_hash(const char *idx, char d, const uint8_t *lsh, size_t llen, const char *id) {
    (void)idx; (void)d; (void)lsh; (void)llen; (void)id; return NULL;
}
char* vl_key_proj(const char *idx, char d, int t) {
    (void)idx; (void)d; (void)t; return NULL;
}

vl_result_t* vl_results_alloc(int n) {
    if (n <= 0) return NULL;
    return (vl_result_t*)get_clear_memory((size_t)n * sizeof(vl_result_t));
}

void vl_results_free(vl_result_t *results, int n) {
    if (results == NULL) return;
    for (int i = 0; i < n; i++) {
        free(results[i].id);
        free(results[i].metadata);
    }
    free(results);
}