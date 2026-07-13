/* vector_ivf.c — inverted file.
 * Task 1: stubs only. Task 6: insert. Search/delete/train/rebuild in later tasks. */
#include "vector_internal.h"
#include "../../Database/database.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Find the nearest centroid to `vec` by prefix-scanning vec/{index}/centroid/.
   Returns the cid of the nearest, or 0 if no centroids exist yet (pre-train). */
static int ivf_nearest_centroid(vector_layer_t *vl, const float *vec) {
    database_t *db = vl->db;
    char d = vl->format.delimiter;
    int dim = vl->format.dim;

    /* Build prefix = "vec/{index}/centroid/" and end = prefix + "\x7f". */
    size_t idx_len = strlen(vl->index_name);
    size_t plen = 4 + idx_len + 10;  /* "vec" + d + idx + d + "centroid" + d */
    char *prefix = (char*)malloc(plen + 1);
    if (prefix == NULL) return 0;
    snprintf(prefix, plen + 1, "vec%c%s%ccentroid%c", d, vl->index_name, d, d);
    char *end = (char*)malloc(plen + 2);
    if (end == NULL) { free(prefix); return 0; }
    memcpy(end, prefix, plen);
    end[plen] = '\x7f'; end[plen + 1] = '\0';

    raw_result_t *results = NULL;
    size_t count = 0;
    int rc = database_scan_range_sync_raw(db, prefix, plen, end, plen + 1, d,
                                          &results, &count);
    free(prefix); free(end);
    if (rc != 0 || count == 0) {
        if (results) database_raw_results_free(results, count);
        return 0;  /* no centroids — default cid=0 */
    }

    int best_cid = 0;
    float best_dist = 1e30f;
    for (size_t i = 0; i < count; i++) {
        /* Extract cid from key: vec/{index}/centroid/{cid} — parse after last delimiter. */
        const char *key = results[i].key;
        size_t klen = results[i].key_len;
        const char *cid_str = key + klen;
        for (size_t j = klen; j > 0; j--) {
            if (key[j-1] == d) { cid_str = key + j; break; }
        }
        int cid = atoi(cid_str);
        if (results[i].value_len < (size_t)(dim * sizeof(float))) continue;
        float dist = vl_distance(vec, (const float*)results[i].value, dim,
                                 vl->format.distance);
        if (dist < best_dist) { best_dist = dist; best_cid = cid; }
    }
    database_raw_results_free(results, count);
    return best_cid;
}

int vector_ivf_insert_sync(vector_layer_t *vl, const char *id, const float *vec,
                           const uint8_t *metadata, size_t metadata_len) {
    if (vl == NULL || id == NULL || vec == NULL) return -22;
    database_t *db = vl->db;
    if (db == NULL) return -22;
    int dim = vl->format.dim;
    size_t vec_bytes = (size_t)dim * sizeof(float);
    size_t total = vec_bytes + metadata_len;

    int cid = ivf_nearest_centroid(vl, vec);

    char *vkey = vl_key_vector(vl->index_name, vl->format.delimiter, id);
    char *ckey = vl_key_cluster_member(vl->index_name, vl->format.delimiter, cid, id);
    char *cntkey = vl_key_count(vl->index_name, vl->format.delimiter);
    if (!vkey || !ckey || !cntkey) { free(vkey); free(ckey); free(cntkey); return -12; }

    /* Read current count (read-modify-write — read before the batch). */
    size_t out_len = 0; uint8_t *buf = NULL; size_t cur = 0;
    int rc = database_get_sync_raw(db, cntkey, strlen(cntkey),
                                   vl->format.delimiter, &buf, &out_len);
    if (rc == 0 && buf && out_len >= sizeof(size_t)) memcpy(&cur, buf, sizeof(size_t));
    if (buf) database_raw_value_free(buf);
    size_t next = cur + 1;

    /* Build vector value = float[dim] + metadata. */
    uint8_t *vval = (uint8_t*)malloc(total);
    if (vval == NULL) { free(vkey); free(ckey); free(cntkey); return -12; }
    memcpy(vval, vec, vec_bytes);
    if (metadata && metadata_len > 0) memcpy(vval + vec_bytes, metadata, metadata_len);

    /* Build the batch — raw_op_t{key, key_len, value, value_len, type}; type 0=put, 1=delete. */
    raw_op_t ops[3];
    /* op 0: put vector */
    ops[0].key = vkey; ops[0].key_len = strlen(vkey);
    ops[0].value = vval; ops[0].value_len = total;
    ops[0].type = 0;
    /* op 1: put cluster membership (value = id) */
    ops[1].key = ckey; ops[1].key_len = strlen(ckey);
    ops[1].value = (const uint8_t*)id; ops[1].value_len = strlen(id);
    ops[1].type = 0;
    /* op 2: put count = cur + 1 */
    ops[2].key = cntkey; ops[2].key_len = strlen(cntkey);
    ops[2].value = (const uint8_t*)&next; ops[2].value_len = sizeof(size_t);
    ops[2].type = 0;

    rc = database_batch_sync_raw(db, vl->format.delimiter, ops, 3);
    free(vval);
    free(vkey); free(ckey); free(cntkey);
    return rc;
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