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

char* vl_key_vector(const char *idx, char d, const char *id) {
    if (idx == NULL || id == NULL) return NULL;
    size_t cap = 4 + strlen(idx) + 8 + strlen(id) + 1;
    char *buf = (char*)malloc(cap);
    if (buf == NULL) return NULL;
    snprintf(buf, cap, "vec%c%s%cvector%c%s", d, idx, d, d, id);
    return buf;
}

char* vl_key_centroid(const char *idx, char d, int cid) {
    if (idx == NULL) return NULL;
    size_t cap = 4 + strlen(idx) + 10 + 11 + 1;
    char *buf = (char*)malloc(cap);
    if (buf == NULL) return NULL;
    snprintf(buf, cap, "vec%c%s%ccentroid%c%010d", d, idx, d, d, cid);
    return buf;
}

char* vl_key_cluster_member(const char *idx, char d, int cid, const char *id) {
    if (idx == NULL || id == NULL) return NULL;
    size_t cap = 4 + strlen(idx) + 9 + 11 + 1 + strlen(id) + 1;
    char *buf = (char*)malloc(cap);
    if (buf == NULL) return NULL;
    snprintf(buf, cap, "vec%c%s%ccluster%c%010d%c%s", d, idx, d, d, cid, d, id);
    return buf;
}

char* vl_key_hash(const char *idx, char d, const uint8_t *lsh, size_t llen, const char *id) {
    if (idx == NULL || lsh == NULL || id == NULL) return NULL;
    size_t cap = 4 + strlen(idx) + 6 + 2*llen + 1 + strlen(id) + 1;
    char *buf = (char*)malloc(cap);
    if (buf == NULL) return NULL;
    int off = snprintf(buf, cap, "vec%c%s%chash%c", d, idx, d, d);
    for (size_t i = 0; i < llen && off < (int)cap; i++) {
        off += snprintf(buf + off, cap - off, "%02x", lsh[i]);
    }
    snprintf(buf + off, cap - off, "%c%s", d, id);
    return buf;
}

char* vl_key_proj(const char *idx, char d, int t) {
    if (idx == NULL) return NULL;
    size_t cap = 4 + strlen(idx) + 6 + 11 + 1;
    char *buf = (char*)malloc(cap);
    if (buf == NULL) return NULL;
    snprintf(buf, cap, "vec%c%s%cproj%c%010d", d, idx, d, d, t);
    return buf;
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