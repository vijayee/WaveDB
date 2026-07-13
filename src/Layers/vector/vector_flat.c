/* vector_flat.c — exact brute-force (baseline + IVF cold-start).
 * Task 3: insert + count. Search/delete/train/rebuild filled in later tasks. */
#include "vector_internal.h"
#include "../../Database/database.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Increment vec/{index}/count via read-modify-write. Returns 0 on success. */
static int flat_increment_count(vector_layer_t *vl) {
    database_t *db = vl->db;
    char *ckey = vl_key_count(vl->index_name, vl->format.delimiter);
    if (ckey == NULL) return -12;

    size_t out_len = 0;
    uint8_t *buf = NULL;
    size_t cur = 0;
    int rc = database_get_sync_raw(db, ckey, strlen(ckey),
                                   vl->format.delimiter, &buf, &out_len);
    if (rc == 0 && buf != NULL && out_len >= sizeof(size_t)) {
        memcpy(&cur, buf, sizeof(size_t));
    }
    if (buf) database_raw_value_free(buf);

    size_t next = cur + 1;
    rc = database_put_sync_raw(db, ckey, strlen(ckey), vl->format.delimiter,
                               (const uint8_t*)&next, sizeof(size_t));
    free(ckey);
    return rc;
}

int vector_flat_insert_sync(vector_layer_t *vl, const char *id, const float *vec,
                            const uint8_t *metadata, size_t metadata_len) {
    if (vl == NULL || id == NULL || vec == NULL) return -22;
    database_t *db = vl->db;
    if (db == NULL) return -22;
    int dim = vl->format.dim;
    size_t vec_bytes = (size_t)dim * sizeof(float);
    (void)metadata; (void)metadata_len;  /* metadata suffix added in Task 5 */

    char *vkey = vl_key_vector(vl->index_name, vl->format.delimiter, id);
    if (vkey == NULL) return -12;
    int rc = database_put_sync_raw(db, vkey, strlen(vkey), vl->format.delimiter,
                                   (const uint8_t*)vec, vec_bytes);
    free(vkey);
    if (rc != 0) return rc;
    return flat_increment_count(vl);
}

/* Brute-force hit entry for top-k sort. */
typedef struct { float dist; const char *id; } vl_flat_hit_t;

/* qsort comparator for hits by distance ascending. */
static int vl_hit_cmp(const void *a, const void *b) {
    float da = ((const vl_flat_hit_t*)a)->dist;
    float db = ((const vl_flat_hit_t*)b)->dist;
    return (da > db) - (da < db);
}

int vector_flat_search_sync(vector_layer_t *vl, const float *query, int k,
                            vl_result_t **out, int *out_n) {
    if (vl == NULL || query == NULL || out == NULL || out_n == NULL) return -22;
    *out = NULL; *out_n = 0;
    database_t *db = vl->db;
    if (db == NULL) return -22;
    if (k <= 0) return 0;
    int dim = vl->format.dim;
    char d = vl->format.delimiter;
    size_t vec_bytes = (size_t)dim * sizeof(float);

    /* Build prefix = "vec/{index}/vector/" and end = prefix + "\x7f". */
    size_t idx_len = strlen(vl->index_name);
    size_t plen = 4 + idx_len + 8;  /* "vec" + d + idx + d + "vector" + d */
    char *prefix = (char*)malloc(plen + 1);
    if (prefix == NULL) return -12;
    snprintf(prefix, plen + 1, "vec%c%s%cvector%c", d, vl->index_name, d, d);
    char *end = (char*)malloc(plen + 2);
    if (end == NULL) { free(prefix); return -12; }
    memcpy(end, prefix, plen);
    end[plen] = '\x7f'; end[plen + 1] = '\0';

    /* Prefix-scan. */
    raw_result_t *results = NULL;
    size_t count = 0;
    int rc = database_scan_range_sync_raw(db, prefix, plen,
                                          end, plen + 1,
                                          d, &results, &count);
    free(prefix); free(end);
    if (rc != 0) return rc;

    /* Brute-force: compute distance per result, collect hits. */
    vl_flat_hit_t *hits = NULL;
    if (count > 0) {
        hits = (vl_flat_hit_t*)malloc(count * sizeof(vl_flat_hit_t));
        if (hits == NULL) { database_raw_results_free(results, count); return -12; }
    }
    size_t n_hits = 0;
    for (size_t i = 0; i < count; i++) {
        /* Key is vec/{index}/vector/{id} — extract id after the last delimiter. */
        const char *key = results[i].key;
        size_t klen = results[i].key_len;
        const char *id = key + klen;
        for (size_t j = klen; j > 0; j--) {
            if (key[j-1] == d) { id = key + j; break; }
        }
        if (results[i].value_len < vec_bytes) continue;
        float dist = vl_distance(query, (const float*)results[i].value,
                                 dim, vl->format.distance);
        hits[n_hits].dist = dist;
        hits[n_hits].id = id;
        n_hits++;
    }

    /* Sort by distance ascending, take top-k. */
    if (n_hits > 1) {
        qsort(hits, n_hits, sizeof(vl_flat_hit_t), vl_hit_cmp);
    }
    int n = (int)(n_hits < (size_t)k ? n_hits : (size_t)k);
    vl_result_t *out_results = NULL;
    if (n > 0) {
        out_results = vl_results_alloc(n);
        if (out_results == NULL) { free(hits); database_raw_results_free(results, count); return -12; }
        for (int i = 0; i < n; i++) {
            out_results[i].id = strdup(hits[i].id);
            out_results[i].distance = hits[i].dist;
            out_results[i].metadata = NULL;
            out_results[i].metadata_len = 0;
        }
    }
    free(hits);
    database_raw_results_free(results, count);
    *out = out_results;
    *out_n = n;
    return 0;
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