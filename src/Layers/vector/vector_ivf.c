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

/* ---- IVF search ---- */

/* Scan a single cluster's members; append each candidate id (strdup'd) to the
   dynamic array. Returns 0 on success. On error, partial allocations remain
   owned by *out_ids and must be freed by the caller. */
static int ivf_scan_cluster(vector_layer_t *vl, int cid,
                            char ***out_ids, size_t *out_n, size_t *out_cap) {
    database_t *db = vl->db;
    char d = vl->format.delimiter;
    size_t idx_len = strlen(vl->index_name);
    /* "vec" + d + idx + d + "cluster" + d + "{cid:010}" + d  →  3+1+idx+1+7+1+10+1 = 4+idx+9+11 */
    size_t plen = 4 + idx_len + 9 + 11;
    char *pfx = (char*)malloc(plen + 1);
    if (pfx == NULL) return -12;
    snprintf(pfx, plen + 1, "vec%c%s%ccluster%c%010d%c",
             d, vl->index_name, d, d, cid, d);
    char *end = (char*)malloc(plen + 2);
    if (end == NULL) { free(pfx); return -12; }
    memcpy(end, pfx, plen);
    end[plen] = '\x7f';
    end[plen + 1] = '\0';

    raw_result_t *results = NULL;
    size_t count = 0;
    int rc = database_scan_range_sync_raw(db, pfx, plen, end, plen + 1,
                                          d, &results, &count);
    free(pfx); free(end);
    if (rc != 0) return rc;

    for (size_t i = 0; i < count; i++) {
        const char *key = results[i].key;
        size_t klen = results[i].key_len;
        const char *id = key + klen;
        for (size_t j = klen; j > 0; j--) {
            if (key[j-1] == d) { id = key + j; break; }
        }
        if (*out_n == *out_cap) {
            size_t ncap = *out_cap == 0 ? 16 : (*out_cap * 2);
            char **nids = (char**)realloc(*out_ids, ncap * sizeof(char*));
            if (nids == NULL) { database_raw_results_free(results, count); return -12; }
            *out_ids = nids; *out_cap = ncap;
        }
        char *dup = strdup(id);
        if (dup == NULL) { database_raw_results_free(results, count); return -12; }
        (*out_ids)[*out_n] = dup;
        (*out_n)++;
    }
    database_raw_results_free(results, count);
    return 0;
}

typedef struct { float dist; int cid; } vl_centroid_hit_t;

static int vl_centroid_cmp(const void *a, const void *b) {
    float da = ((const vl_centroid_hit_t*)a)->dist;
    float db = ((const vl_centroid_hit_t*)b)->dist;
    return (da > db) - (da < db);
}

typedef struct { float dist; char *id; uint8_t *metadata; size_t meta_len; } vl_ivf_cand_t;

static int vl_ivf_cand_cmp(const void *a, const void *b) {
    float da = ((const vl_ivf_cand_t*)a)->dist;
    float db = ((const vl_ivf_cand_t*)b)->dist;
    return (da > db) - (da < db);
}

int vector_ivf_search_sync(vector_layer_t *vl, const float *query, int k,
                           vl_result_t **out, int *out_n) {
    if (vl == NULL || query == NULL || out == NULL || out_n == NULL) return -22;
    *out = NULL; *out_n = 0;
    database_t *db = vl->db;
    if (db == NULL) return -22;
    if (k <= 0) return 0;
    int dim = vl->format.dim;
    char d = vl->format.delimiter;
    size_t vec_bytes = (size_t)dim * sizeof(float);

    /* Cold-start: below the flat-until threshold, exact brute-force. */
    if (vector_layer_count(vl) < (size_t)vl->runtime.ivf_flat_until) {
        return vector_flat_search_sync(vl, query, k, out, out_n);
    }

    /* 1. Prefix-scan centroids. */
    size_t idx_len = strlen(vl->index_name);
    size_t plen = 4 + idx_len + 10;  /* "vec"+d+idx+d+"centroid"+d */
    char *prefix = (char*)malloc(plen + 1);
    if (prefix == NULL) return -12;
    snprintf(prefix, plen + 1, "vec%c%s%ccentroid%c", d, vl->index_name, d, d);
    char *end = (char*)malloc(plen + 2);
    if (end == NULL) { free(prefix); return -12; }
    memcpy(end, prefix, plen);
    end[plen] = '\x7f'; end[plen + 1] = '\0';

    raw_result_t *centroids = NULL;
    size_t n_centroids = 0;
    int rc = database_scan_range_sync_raw(db, prefix, plen, end, plen + 1,
                                          d, &centroids, &n_centroids);
    free(prefix); free(end);
    if (rc != 0) return rc;
    if (n_centroids == 0) {
        /* Pre-train but count >= flat_until: fall back to exact FLAT. */
        if (centroids) database_raw_results_free(centroids, n_centroids);
        return vector_flat_search_sync(vl, query, k, out, out_n);
    }

    /* 2. Compute query→centroid distance, pick top-nprobe nearest. */
    vl_centroid_hit_t *chits = (vl_centroid_hit_t*)malloc(n_centroids * sizeof(vl_centroid_hit_t));
    if (chits == NULL) { database_raw_results_free(centroids, n_centroids); return -12; }
    size_t n_chits = 0;
    for (size_t i = 0; i < n_centroids; i++) {
        const char *key = centroids[i].key;
        size_t klen = centroids[i].key_len;
        const char *cid_str = key + klen;
        for (size_t j = klen; j > 0; j--) {
            if (key[j-1] == d) { cid_str = key + j; break; }
        }
        int cid = atoi(cid_str);
        if (centroids[i].value_len < vec_bytes) continue;
        float dist = vl_distance(query, (const float*)centroids[i].value,
                                 dim, vl->format.distance);
        chits[n_chits].dist = dist;
        chits[n_chits].cid = cid;
        n_chits++;
    }
    database_raw_results_free(centroids, n_centroids);

    if (n_chits == 0) {
        free(chits);
        return vector_flat_search_sync(vl, query, k, out, out_n);
    }
    if (n_chits > 1) {
        qsort(chits, n_chits, sizeof(vl_centroid_hit_t), vl_centroid_cmp);
    }

    int nprobe = vl->runtime.ivf_nprobe;
    if (nprobe <= 0) nprobe = 1;
    if ((size_t)nprobe > n_chits) nprobe = (int)n_chits;

    /* 3. For each selected cid, prefix-scan the cluster for candidate ids. */
    char **candidate_ids = NULL;
    size_t n_candidates = 0, cap_candidates = 0;
    for (int i = 0; i < nprobe; i++) {
        rc = ivf_scan_cluster(vl, chits[i].cid,
                              &candidate_ids, &n_candidates, &cap_candidates);
        if (rc != 0) {
            for (size_t j = 0; j < n_candidates; j++) free(candidate_ids[j]);
            free(candidate_ids); free(chits);
            return rc;
        }
    }
    free(chits);

    /* 4. Fetch candidate vectors, exact rerank, top-k. */
    vl_ivf_cand_t *hits = NULL;
    if (n_candidates > 0) {
        hits = (vl_ivf_cand_t*)malloc(n_candidates * sizeof(vl_ivf_cand_t));
        if (hits == NULL) {
            for (size_t j = 0; j < n_candidates; j++) free(candidate_ids[j]);
            free(candidate_ids);
            return -12;
        }
    }
    size_t n_hits = 0;
    for (size_t i = 0; i < n_candidates; i++) {
        char *vkey = vl_key_vector(vl->index_name, d, candidate_ids[i]);
        if (vkey == NULL) { free(candidate_ids[i]); continue; }
        uint8_t *vbuf = NULL; size_t vlen = 0;
        int grc = database_get_sync_raw(db, vkey, strlen(vkey), d, &vbuf, &vlen);
        free(vkey);
        if (grc != 0 || vbuf == NULL || vlen < vec_bytes) {
            if (vbuf) database_raw_value_free(vbuf);
            free(candidate_ids[i]);
            continue;
        }
        float dist = vl_distance(query, (const float*)vbuf, dim, vl->format.distance);
        /* Extract metadata tail (if any). */
        uint8_t *meta = NULL; size_t meta_len = 0;
        if (vlen > vec_bytes) {
            meta_len = vlen - vec_bytes;
            meta = (uint8_t*)malloc(meta_len);
            if (meta != NULL) memcpy(meta, vbuf + vec_bytes, meta_len);
            else meta_len = 0;
        }
        database_raw_value_free(vbuf);
        hits[n_hits].dist = dist;
        hits[n_hits].id = candidate_ids[i];  /* transfer ownership */
        hits[n_hits].metadata = meta;
        hits[n_hits].meta_len = meta_len;
        n_hits++;
    }
    /* candidate_ids entries are either transferred to hits or freed above. */
    free(candidate_ids);

    /* Sort by distance ascending, take top-k. */
    if (n_hits > 1) {
        qsort(hits, n_hits, sizeof(vl_ivf_cand_t), vl_ivf_cand_cmp);
    }
    int n = (int)(n_hits < (size_t)k ? n_hits : (size_t)k);
    vl_result_t *out_results = NULL;
    if (n > 0) {
        out_results = vl_results_alloc(n);
        if (out_results == NULL) {
            for (size_t i = 0; i < n_hits; i++) {
                free(hits[i].id);
                free(hits[i].metadata);
            }
            free(hits);
            return -12;
        }
        for (int i = 0; i < n; i++) {
            out_results[i].id = hits[i].id;            /* transfer */
            out_results[i].distance = hits[i].dist;
            out_results[i].metadata = hits[i].metadata; /* transfer */
            out_results[i].metadata_len = hits[i].meta_len;
        }
        /* Free hits beyond top-k (their id + metadata were not transferred). */
        for (size_t i = (size_t)n; i < n_hits; i++) {
            free(hits[i].id);
            free(hits[i].metadata);
        }
    }
    free(hits);

    *out = out_results;
    *out_n = n;
    return 0;
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