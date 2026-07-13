/* vector_ivf.c — inverted file.
 * Task 1: stubs only. Task 6: insert. Task 7: search. Task 8: train + rebuild.
 * Task 8b: reverted the clist workaround — IVF operations now respect MVCC
 * snapshot isolation by scanning first (read-only), computing in memory,
 * then batching all writes atomically. Never interleave scans with writes. */
#include "vector_internal.h"
#include "../../Database/database.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Find the nearest centroid to `vec` by prefix-scanning vec/{index}/centroid/.
   Returns the cid of the nearest, or 0 if no centroids exist yet (pre-train).
   READ-ONLY scan — caller ensures no writes interleave. */
static int ivf_nearest_centroid(vector_layer_t *vl, const float *vec) {
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
    int rc = vl_scan_range(vl, prefix, plen, end, plen + 1, &results, &count);
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
    if (vl->db == NULL) return -22;
    int dim = vl->format.dim;
    char d = vl->format.delimiter;
    size_t vec_bytes = (size_t)dim * sizeof(float);
    size_t total = vec_bytes + metadata_len;

    /* 1. Scan centroids (READ-ONLY) — happens BEFORE any writes. */
    int cid = ivf_nearest_centroid(vl, vec);

    /* 2. Read current count (READ-ONLY). */
    char *cntkey = vl_key_count(vl->index_name, d);
    if (cntkey == NULL) return -12;
    size_t out_len = 0; uint8_t *buf = NULL; size_t cur = 0;
    int rc = vl_get(vl, cntkey, strlen(cntkey), &buf, &out_len);
    if (rc == 0 && buf && out_len >= sizeof(size_t)) memcpy(&cur, buf, sizeof(size_t));
    if (buf) database_raw_value_free(buf);
    size_t next = cur + 1;

    /* 3. Build ONE batch of 3 ops: vector + cluster membership + count.
       All writes are issued atomically via a single database_batch_sync_raw
       call. No scans happen after this point — MVCC happy. */
    char *vkey = vl_key_vector(vl->index_name, d, id);
    char *ckey = vl_key_cluster_member(vl->index_name, d, cid, id);
    if (!vkey || !ckey) { free(vkey); free(ckey); free(cntkey); return -12; }

    uint8_t *vval = (uint8_t*)malloc(total);
    if (vval == NULL) { free(vkey); free(ckey); free(cntkey); return -12; }
    memcpy(vval, vec, vec_bytes);
    if (metadata && metadata_len > 0) memcpy(vval + vec_bytes, metadata, metadata_len);

    size_t id_len = strlen(id);

    raw_op_t ops[3];
    ops[0].key = vkey; ops[0].key_len = strlen(vkey);
    ops[0].value = vval; ops[0].value_len = total; ops[0].type = 0;
    ops[1].key = ckey; ops[1].key_len = strlen(ckey);
    ops[1].value = (const uint8_t*)id; ops[1].value_len = id_len; ops[1].type = 0;
    ops[2].key = cntkey; ops[2].key_len = strlen(cntkey);
    ops[2].value = (const uint8_t*)&next; ops[2].value_len = sizeof(size_t);
    ops[2].type = 0;

    rc = vl_batch(vl, ops, 3);
    free(vval); free(vkey); free(ckey); free(cntkey);
    return rc;
}

/* ---- IVF search ---- */

/* Prefix-scan vec/{idx}/cluster/{cid}/ for member ids. READ-ONLY — used at
   search time, never interleaved with writes. Restored from the clist
   get+split workaround. */
static int ivf_scan_cluster(vector_layer_t *vl, int cid,
                            char ***out_ids, size_t *out_n, size_t *out_cap) {
    char d = vl->format.delimiter;

    /* Build prefix = "vec/{index}/cluster/{cid:010}/" and end = prefix + "\x7f". */
    size_t idx_len = strlen(vl->index_name);
    size_t plen = 4 + idx_len + 9 + 11;  /* "vec"+d + idx+d + "cluster"+d + cid:010 + d */
    char *pfx = (char*)malloc(plen + 1);
    if (pfx == NULL) return -12;
    snprintf(pfx, plen + 1, "vec%c%s%ccluster%c%010d%c", d, vl->index_name, d, d, cid, d);
    char *end = (char*)malloc(plen + 2);
    if (end == NULL) { free(pfx); return -12; }
    memcpy(end, pfx, plen);
    end[plen] = '\x7f'; end[plen + 1] = '\0';

    raw_result_t *results = NULL;
    size_t count = 0;
    int rc = vl_scan_range(vl, pfx, plen, end, plen + 1, &results, &count);
    free(pfx); free(end);
    if (rc != 0) return rc;

    for (size_t i = 0; i < count; i++) {
        const char *key = results[i].key;
        size_t klen = results[i].key_len;
        /* id is the substring after the last delimiter. */
        const char *id = key + klen;
        for (size_t j = klen; j > 0; j--) {
            if (key[j-1] == d) { id = key + j; break; }
        }
        if (*out_n == *out_cap) {
            size_t ncap = *out_cap == 0 ? 16 : *out_cap * 2;
            char **new_ids = (char**)realloc(*out_ids, ncap * sizeof(char*));
            if (new_ids == NULL) { database_raw_results_free(results, count); return -12; }
            *out_ids = new_ids; *out_cap = ncap;
        }
        (*out_ids)[*out_n] = strdup(id);
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
    if (vl->db == NULL) return -22;
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
    int rc = vl_scan_range(vl, prefix, plen, end, plen + 1,
                           &centroids, &n_centroids);
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

    /* 3. For each selected cid, prefix-scan the cluster's membership keys.
       READ-ONLY — no writes interleave with search. */
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
        int grc = vl_get(vl, vkey, strlen(vkey), &vbuf, &vlen);
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
    if (vl == NULL) return -22;
    if (vl->db == NULL) return -22;
    int dim = vl->format.dim;
    int K = vl->format.ivf_n_clusters;
    if (K <= 0) return -22;
    char d = vl->format.delimiter;
    size_t vec_bytes = (size_t)dim * sizeof(float);

    /* 1. Prefix-scan vec/{index}/vector/ to read all stored vectors. */
    size_t idx_len = strlen(vl->index_name);
    size_t plen = 4 + idx_len + 7;  /* "vec" + d + idx + d + "vector" + d */
    char *prefix = (char*)malloc(plen + 1);
    if (prefix == NULL) return -12;
    snprintf(prefix, plen + 1, "vec%c%s%cvector%c", d, vl->index_name, d, d);
    char *end = (char*)malloc(plen + 2);
    if (end == NULL) { free(prefix); return -12; }
    memcpy(end, prefix, plen);
    end[plen] = '\x7f'; end[plen + 1] = '\0';

    raw_result_t *vectors = NULL;
    size_t n_vectors = 0;
    int rc = vl_scan_range(vl, prefix, plen, end, plen + 1, &vectors, &n_vectors);
    free(prefix); free(end);
    if (rc != 0) return rc;
    if (n_vectors == 0) { database_raw_results_free(vectors, n_vectors); return 0; }

    /* 2. Initialize K centroids from the first K vectors (first-K init for
       reproducibility — the spike's test uses well-separated clusters so
       first-K converges correctly). */
    float *centroids = (float*)calloc((size_t)K * dim, sizeof(float));
    if (centroids == NULL) { database_raw_results_free(vectors, n_vectors); return -12; }
    for (int c = 0; c < K && c < (int)n_vectors; c++) {
        if (vectors[c].value_len >= vec_bytes) {
            memcpy(centroids + c * dim, vectors[c].value, vec_bytes);
        }
    }

    /* 3. Iterate k-means: assign each vector to nearest centroid, recompute. */
    int *assignments = (int*)calloc(n_vectors, sizeof(int));
    if (assignments == NULL) {
        free(centroids); database_raw_results_free(vectors, n_vectors); return -12;
    }
    /* Initialize to -1 so the first iteration's `changed` count is correct. */
    for (size_t i = 0; i < n_vectors; i++) assignments[i] = -1;

    int max_iters = 10;
    for (int iter = 0; iter < max_iters; iter++) {
        int changed = 0;
        for (size_t i = 0; i < n_vectors; i++) {
            if (vectors[i].value_len < vec_bytes) { assignments[i] = 0; continue; }
            float best_d = 1e30f; int best_c = 0;
            for (int c = 0; c < K; c++) {
                float dist = vl_distance((const float*)vectors[i].value,
                                         centroids + c * dim, dim,
                                         vl->format.distance);
                if (dist < best_d) { best_d = dist; best_c = c; }
            }
            if (assignments[i] != best_c) { changed++; assignments[i] = best_c; }
        }
        if (iter > 0 && changed == 0) break;  /* converged */

        /* Recompute centroids as the mean of assigned vectors. */
        float *new_centroids = (float*)calloc((size_t)K * dim, sizeof(float));
        int *counts = (int*)calloc((size_t)K, sizeof(int));
        if (new_centroids == NULL || counts == NULL) {
            free(new_centroids); free(counts); free(assignments);
            free(centroids); database_raw_results_free(vectors, n_vectors);
            return -12;
        }
        for (size_t i = 0; i < n_vectors; i++) {
            if (vectors[i].value_len < vec_bytes) continue;
            int c = assignments[i];
            if (c < 0 || c >= K) continue;
            const float *v = (const float*)vectors[i].value;
            for (int dd = 0; dd < dim; dd++) new_centroids[c * dim + dd] += v[dd];
            counts[c]++;
        }
        for (int c = 0; c < K; c++) {
            if (counts[c] > 0) {
                for (int dd = 0; dd < dim; dd++)
                    new_centroids[c * dim + dd] /= counts[c];
                memcpy(centroids + c * dim, new_centroids + c * dim, vec_bytes);
            }
            /* else: keep old centroid (empty cluster) */
        }
        free(new_centroids); free(counts);
    }

    /* 4. Write centroids to vec/{index}/centroid/{cid} for cid in 0..K-1.
       Train only writes centroids — no membership mutation, no interleaving
       concern. Each put is independent and idempotent under overwrite. */
    for (int c = 0; c < K; c++) {
        char *ckey = vl_key_centroid(vl->index_name, d, c);
        if (ckey == NULL) {
            free(assignments); free(centroids);
            database_raw_results_free(vectors, n_vectors); return -12;
        }
        rc = vl_put(vl, ckey, strlen(ckey),
                    (const uint8_t*)(centroids + c * dim), vec_bytes);
        free(ckey);
        if (rc != 0) {
            free(assignments); free(centroids);
            database_raw_results_free(vectors, n_vectors); return rc;
        }
    }

    free(assignments);
    free(centroids);
    database_raw_results_free(vectors, n_vectors);
    return 0;
}

int vector_ivf_rebuild(vector_layer_t *vl) {
    if (vl == NULL) return -22;
    if (vl->db == NULL) return -22;
    int dim = vl->format.dim;
    char d = vl->format.delimiter;
    size_t vec_bytes = (size_t)dim * sizeof(float);
    size_t idx_len = strlen(vl->index_name);

    /* 1. Scan vec/{idx}/cluster/ for old membership keys (READ-ONLY). */
    size_t cprefix_len = 4 + idx_len + 8;  /* "vec" + d + idx + d + "cluster" + d */
    char *cprefix = (char*)malloc(cprefix_len + 1);
    if (cprefix == NULL) return -12;
    snprintf(cprefix, cprefix_len + 1, "vec%c%s%ccluster%c", d, vl->index_name, d, d);
    char *cend = (char*)malloc(cprefix_len + 2);
    if (cend == NULL) { free(cprefix); return -12; }
    memcpy(cend, cprefix, cprefix_len);
    cend[cprefix_len] = '\x7f'; cend[cprefix_len + 1] = '\0';

    raw_result_t *old_members = NULL;
    size_t n_old = 0;
    int rc = vl_scan_range(vl, cprefix, cprefix_len, cend,
                           cprefix_len + 1, &old_members, &n_old);
    free(cprefix); free(cend);
    if (rc != 0) return rc;

    /* 2. Scan vec/{idx}/vector/ for all vectors (READ-ONLY). */
    size_t vprefix_len = 4 + idx_len + 7;  /* "vec" + d + idx + d + "vector" + d */
    char *vprefix = (char*)malloc(vprefix_len + 1);
    if (vprefix == NULL) { database_raw_results_free(old_members, n_old); return -12; }
    snprintf(vprefix, vprefix_len + 1, "vec%c%s%cvector%c", d, vl->index_name, d, d);
    char *vend = (char*)malloc(vprefix_len + 2);
    if (vend == NULL) { free(vprefix); database_raw_results_free(old_members, n_old); return -12; }
    memcpy(vend, vprefix, vprefix_len);
    vend[vprefix_len] = '\x7f'; vend[vprefix_len + 1] = '\0';

    raw_result_t *vectors = NULL;
    size_t n_vectors = 0;
    rc = vl_scan_range(vl, vprefix, vprefix_len, vend,
                       vprefix_len + 1, &vectors, &n_vectors);
    free(vprefix); free(vend);
    if (rc != 0) { database_raw_results_free(old_members, n_old); return rc; }

    /* 3. Scan vec/{idx}/centroid/ once and cache (cid, float[dim]) in memory.
       READ-ONLY — one scan, reused for all vectors below. */
    size_t cent_prefix_len = 4 + idx_len + 10;  /* "vec" + d + idx + d + "centroid" + d */
    char *cent_prefix = (char*)malloc(cent_prefix_len + 1);
    if (cent_prefix == NULL) {
        database_raw_results_free(old_members, n_old);
        database_raw_results_free(vectors, n_vectors);
        return -12;
    }
    snprintf(cent_prefix, cent_prefix_len + 1, "vec%c%s%ccentroid%c",
             d, vl->index_name, d, d);
    char *cent_end = (char*)malloc(cent_prefix_len + 2);
    if (cent_end == NULL) {
        free(cent_prefix);
        database_raw_results_free(old_members, n_old);
        database_raw_results_free(vectors, n_vectors);
        return -12;
    }
    memcpy(cent_end, cent_prefix, cent_prefix_len);
    cent_end[cent_prefix_len] = '\x7f'; cent_end[cent_prefix_len + 1] = '\0';

    raw_result_t *centroids = NULL;
    size_t n_centroids = 0;
    rc = vl_scan_range(vl, cent_prefix, cent_prefix_len, cent_end,
                       cent_prefix_len + 1, &centroids, &n_centroids);
    free(cent_prefix); free(cent_end);
    if (rc != 0) {
        database_raw_results_free(old_members, n_old);
        database_raw_results_free(vectors, n_vectors);
        return rc;
    }

    /* Cache centroids into arrays for in-memory nearest-centroid lookups. */
    int *cent_cids = NULL;
    float *cent_coords = NULL;
    if (n_centroids > 0) {
        cent_cids = (int*)malloc(n_centroids * sizeof(int));
        cent_coords = (float*)malloc(n_centroids * dim * sizeof(float));
        if (cent_cids == NULL || cent_coords == NULL) {
            free(cent_cids); free(cent_coords);
            database_raw_results_free(old_members, n_old);
            database_raw_results_free(vectors, n_vectors);
            database_raw_results_free(centroids, n_centroids);
            return -12;
        }
    }
    size_t n_cached = 0;
    for (size_t i = 0; i < n_centroids; i++) {
        if (centroids[i].value_len < vec_bytes) continue;
        const char *key = centroids[i].key;
        size_t klen = centroids[i].key_len;
        const char *cid_str = key + klen;
        for (size_t j = klen; j > 0; j--) {
            if (key[j-1] == d) { cid_str = key + j; break; }
        }
        cent_cids[n_cached] = atoi(cid_str);
        memcpy(cent_coords + n_cached * dim, centroids[i].value, vec_bytes);
        n_cached++;
    }
    /* centroids results no longer needed — we have the cache. */
    database_raw_results_free(centroids, n_centroids);

    /* 4. Compute new memberships in memory: for each vector, find the nearest
       cached centroid. Build the new membership key (vec/{idx}/cluster/{cid}/{id})
       and remember the id value pointer (borrows into vectors[i].key, valid
       until we free vectors — done AFTER the batch). */
    char **new_member_keys = (char**)malloc((n_vectors ? n_vectors : 1) * sizeof(char*));
    const char **new_id_ptrs = (const char**)malloc((n_vectors ? n_vectors : 1) * sizeof(const char*));
    if (new_member_keys == NULL || new_id_ptrs == NULL) {
        free(new_member_keys); free(new_id_ptrs);
        free(cent_cids); free(cent_coords);
        database_raw_results_free(old_members, n_old);
        database_raw_results_free(vectors, n_vectors);
        return -12;
    }
    size_t n_new = 0;

    for (size_t i = 0; i < n_vectors; i++) {
        if (vectors[i].value_len < vec_bytes) continue;
        const char *key = vectors[i].key;
        size_t klen = vectors[i].key_len;
        const char *id = key + klen;
        for (size_t j = klen; j > 0; j--) {
            if (key[j-1] == d) { id = key + j; break; }
        }

        int best_cid = 0;
        if (n_cached > 0) {
            float best_d = 1e30f;
            for (size_t c = 0; c < n_cached; c++) {
                float dist = vl_distance((const float*)vectors[i].value,
                                         cent_coords + c * dim, dim,
                                         vl->format.distance);
                if (dist < best_d) { best_d = dist; best_cid = cent_cids[c]; }
            }
        }

        char *mkey = vl_key_cluster_member(vl->index_name, d, best_cid, id);
        if (mkey == NULL) {
            for (size_t j = 0; j < n_new; j++) free(new_member_keys[j]);
            free(new_member_keys); free(new_id_ptrs);
            free(cent_cids); free(cent_coords);
            database_raw_results_free(old_members, n_old);
            database_raw_results_free(vectors, n_vectors);
            return -12;
        }
        new_member_keys[n_new] = mkey;
        new_id_ptrs[n_new] = id;
        n_new++;
    }

    /* 5. Build the batch. Partition old and new keys into:
       - to_delete = old keys NOT in new set (membership removed/changed)
       - to_put    = new keys NOT in old set (membership added/changed)
       - unchanged = intersection — left as-is, no op needed
       Skipping unchanged keys entirely avoids same-key delete-then-put under
       MVCC (where both ops share the same txn_id and conflict) AND avoids
       re-putting an existing key at a new txn_id (which can also shadow the
       prior version in the same-txn version chain). */
    size_t n_ops_max = n_old + n_new;
    raw_op_t *ops = NULL;
    if (n_ops_max > 0) {
        ops = (raw_op_t*)malloc(n_ops_max * sizeof(raw_op_t));
        if (ops == NULL) {
            for (size_t j = 0; j < n_new; j++) free(new_member_keys[j]);
            free(new_member_keys); free(new_id_ptrs);
            free(cent_cids); free(cent_coords);
            database_raw_results_free(old_members, n_old);
            database_raw_results_free(vectors, n_vectors);
            return -12;
        }
    }
    size_t op_i = 0;

    /* 5a. Deletes for old membership keys not in the new set. The key pointer
       borrows into old_members[i].key (valid until we free old_members — done
       AFTER the batch). */
    for (size_t i = 0; i < n_old; i++) {
        const char *old_key = old_members[i].key;
        int still_present = 0;
        for (size_t j = 0; j < n_new; j++) {
            if (strcmp(old_key, new_member_keys[j]) == 0) {
                still_present = 1;
                break;
            }
        }
        if (still_present) continue;  /* unchanged — leave it alone */
        ops[op_i].key = old_key;
        ops[op_i].key_len = old_members[i].key_len;
        ops[op_i].value = NULL;
        ops[op_i].value_len = 0;
        ops[op_i].type = 1;  /* delete */
        op_i++;
    }

    /* 5b. Puts for new membership keys not in the old set. Unchanged keys are
       skipped (they already exist with the correct value from insert/a prior
       rebuild). */
    for (size_t j = 0; j < n_new; j++) {
        const char *new_key = new_member_keys[j];
        int already_present = 0;
        for (size_t i = 0; i < n_old; i++) {
            if (strcmp(new_key, old_members[i].key) == 0) {
                already_present = 1;
                break;
            }
        }
        if (already_present) continue;  /* unchanged — leave it alone */
        ops[op_i].key = new_key;
        ops[op_i].key_len = strlen(new_key);
        ops[op_i].value = (const uint8_t*)new_id_ptrs[j];
        ops[op_i].value_len = strlen(new_id_ptrs[j]);
        ops[op_i].type = 0;  /* put */
        op_i++;
    }

    /* 6. Apply the batch atomically: all deletes + all puts in ONE call.
       MVCC: a fresh scan after this commit sees the new memberships. */
    if (op_i > 0) {
        rc = vl_batch(vl, ops, op_i);
    } else {
        rc = 0;
    }

    /* 7. Cleanup. Free new membership keys, ops array, centroid cache, and
       the scan results (now that the batch has copied what it needs). */
    for (size_t j = 0; j < n_new; j++) free(new_member_keys[j]);
    free(new_member_keys);
    free(new_id_ptrs);
    free(ops);
    free(cent_cids);
    free(cent_coords);
    database_raw_results_free(old_members, n_old);
    database_raw_results_free(vectors, n_vectors);
    return rc;
}