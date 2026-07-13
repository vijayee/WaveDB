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
    char d = vl->format.delimiter;

    int cid = ivf_nearest_centroid(vl, vec);

    char *vkey = vl_key_vector(vl->index_name, d, id);
    char *cntkey = vl_key_count(vl->index_name, d);
    char *clist_key = vl_key_clist(vl->index_name, d, cid);
    if (!vkey || !cntkey || !clist_key) {
        free(vkey); free(cntkey); free(clist_key); return -12;
    }

    /* Read current count (read-modify-write — read before the batch). */
    size_t out_len = 0; uint8_t *buf = NULL; size_t cur = 0;
    int rc = database_get_sync_raw(db, cntkey, strlen(cntkey), d, &buf, &out_len);
    if (rc == 0 && buf && out_len >= sizeof(size_t)) memcpy(&cur, buf, sizeof(size_t));
    if (buf) database_raw_value_free(buf);
    size_t next = cur + 1;

    /* Build vector value = float[dim] + metadata. */
    uint8_t *vval = (uint8_t*)malloc(total);
    if (vval == NULL) { free(vkey); free(cntkey); free(clist_key); return -12; }
    memcpy(vval, vec, vec_bytes);
    if (metadata && metadata_len > 0) memcpy(vval + vec_bytes, metadata, metadata_len);

    /* Batch: put vector + put count. */
    raw_op_t ops[2];
    ops[0].key = vkey; ops[0].key_len = strlen(vkey);
    ops[0].value = vval; ops[0].value_len = total;
    ops[0].type = 0;
    ops[1].key = cntkey; ops[1].key_len = strlen(cntkey);
    ops[1].value = (const uint8_t*)&next; ops[1].value_len = sizeof(size_t);
    ops[1].type = 0;

    rc = database_batch_sync_raw(db, d, ops, 2);
    free(vval);
    if (rc != 0) { free(vkey); free(cntkey); free(clist_key); return rc; }

    /* Append id to the cluster's membership list (clist). Read-modify-write. */
    size_t clist_len = 0; uint8_t *clist_buf = NULL;
    rc = database_get_sync_raw(db, clist_key, strlen(clist_key), d,
                               &clist_buf, &clist_len);
    /* id + '\n' + existing list. */
    size_t id_len = strlen(id);
    size_t new_len = id_len + 1 + (rc == 0 ? clist_len : 0);
    uint8_t *new_list = (uint8_t*)malloc(new_len);
    if (new_list == NULL) {
        if (clist_buf) database_raw_value_free(clist_buf);
        free(vkey); free(cntkey); free(clist_key);
        return -12;
    }
    memcpy(new_list, id, id_len);
    new_list[id_len] = '\n';
    if (clist_buf && clist_len > 0) {
        memcpy(new_list + id_len + 1, clist_buf, clist_len);
    }
    if (clist_buf) database_raw_value_free(clist_buf);

    rc = database_put_sync_raw(db, clist_key, strlen(clist_key), d,
                               new_list, new_len);
    free(new_list);
    free(vkey); free(cntkey); free(clist_key);
    return rc;
}

/* ---- IVF search ---- */

/* Read a cluster's membership list (clist) and append each id (strdup'd) to the
   dynamic array. Uses direct get + split instead of prefix scan — avoids the
   database bug where prefix scan doesn't see overwritten/delete-then-put keys. */
static int ivf_scan_cluster(vector_layer_t *vl, int cid,
                            char ***out_ids, size_t *out_n, size_t *out_cap) {
    database_t *db = vl->db;
    char d = vl->format.delimiter;

    char *ckey = vl_key_clist(vl->index_name, d, cid);
    if (ckey == NULL) return -12;

    uint8_t *buf = NULL; size_t buf_len = 0;
    int rc = database_get_sync_raw(db, ckey, strlen(ckey), d, &buf, &buf_len);
    free(ckey);
    if (rc != 0 || buf == NULL || buf_len == 0) {
        if (buf) database_raw_value_free(buf);
        return 0;  /* empty cluster */
    }

    /* Split by '\n'. Each line is an id. */
    size_t start = 0;
    for (size_t i = 0; i <= buf_len; i++) {
        if (i == buf_len || buf[i] == '\n') {
            size_t id_len = i - start;
            if (id_len > 0) {
                if (*out_n == *out_cap) {
                    size_t ncap = *out_cap == 0 ? 16 : (*out_cap * 2);
                    char **nids = (char**)realloc(*out_ids, ncap * sizeof(char*));
                    if (nids == NULL) { database_raw_value_free(buf); return -12; }
                    *out_ids = nids; *out_cap = ncap;
                }
                char *dup = (char*)malloc(id_len + 1);
                if (dup == NULL) { database_raw_value_free(buf); return -12; }
                memcpy(dup, buf + start, id_len);
                dup[id_len] = '\0';
                (*out_ids)[*out_n] = dup;
                (*out_n)++;
            }
            start = i + 1;
        }
    }
    database_raw_value_free(buf);
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

    /* 3. For each selected cid, read the cluster's membership list. */
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
    if (vl == NULL) return -22;
    database_t *db = vl->db;
    if (db == NULL) return -22;
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
    int rc = database_scan_range_sync_raw(db, prefix, plen, end, plen + 1, d,
                                          &vectors, &n_vectors);
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

    /* 4. Write centroids to vec/{index}/centroid/{cid} for cid in 0..K-1. */
    for (int c = 0; c < K; c++) {
        char *ckey = vl_key_centroid(vl->index_name, d, c);
        if (ckey == NULL) {
            free(assignments); free(centroids);
            database_raw_results_free(vectors, n_vectors); return -12;
        }
        rc = database_put_sync_raw(db, ckey, strlen(ckey), d,
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
    database_t *db = vl->db;
    if (db == NULL) return -22;
    int dim = vl->format.dim;
    int K = vl->format.ivf_n_clusters;
    char d = vl->format.delimiter;
    size_t vec_bytes = (size_t)dim * sizeof(float);
    size_t idx_len = strlen(vl->index_name);

    /* 1. Prefix-scan vec/{index}/vector/ → collect all vectors. */
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
    int rc = database_scan_range_sync_raw(db, prefix, plen, end, plen + 1, d,
                                          &vectors, &n_vectors);
    free(prefix); free(end);
    if (rc != 0) return rc;

    /* 2. For each vector, compute nearest cid. Build per-cid id lists. */
    /* dynamic array of ids per cid. */
    char **lists = (char**)calloc(K, sizeof(char*));
    size_t *list_lens = (size_t*)calloc(K, sizeof(size_t));
    size_t *list_caps = (size_t*)calloc(K, sizeof(size_t));
    if (lists == NULL || list_lens == NULL || list_caps == NULL) {
        free(lists); free(list_lens); free(list_caps);
        database_raw_results_free(vectors, n_vectors);
        return -12;
    }

    for (size_t i = 0; i < n_vectors; i++) {
        if (vectors[i].value_len < vec_bytes) continue;
        const char *key = vectors[i].key;
        size_t klen = vectors[i].key_len;
        const char *id = key + klen;
        for (size_t j = klen; j > 0; j--) {
            if (key[j-1] == d) { id = key + j; break; }
        }
        size_t id_len = strlen(id);
        int cid = ivf_nearest_centroid(vl, (const float*)vectors[i].value);
        if (cid < 0 || cid >= K) cid = 0;

        /* Append "id\n" to lists[cid]. */
        size_t needed = list_lens[cid] + id_len + 1;
        if (list_caps[cid] < needed) {
            size_t ncap = list_caps[cid] == 0 ? 64 : list_caps[cid] * 2;
            while (ncap < needed) ncap *= 2;
            char *nl = (char*)realloc(lists[cid], ncap);
            if (nl == NULL) {
                for (int c = 0; c < K; c++) free(lists[c]);
                free(lists); free(list_lens); free(list_caps);
                database_raw_results_free(vectors, n_vectors);
                return -12;
            }
            lists[cid] = nl; list_caps[cid] = ncap;
        }
        memcpy(lists[cid] + list_lens[cid], id, id_len);
        list_lens[cid] += id_len;
        lists[cid][list_lens[cid]] = '\n';
        list_lens[cid]++;
    }
    database_raw_results_free(vectors, n_vectors);

    /* 3. Write each cid's list to vec/{index}/clist/{cid}. This overwrites the
       old list (from insert). Uses put_sync_raw + direct get in search (no
       prefix scan) so the overwrite bug doesn't apply. */
    for (int c = 0; c < K; c++) {
        char *ckey = vl_key_clist(vl->index_name, d, c);
        if (ckey == NULL) {
            for (int cc = 0; cc < K; cc++) free(lists[cc]);
            free(lists); free(list_lens); free(list_caps);
            return -12;
        }
        rc = database_put_sync_raw(db, ckey, strlen(ckey), d,
                                   (const uint8_t*)lists[c], list_lens[c]);
        free(ckey);
        if (rc != 0) {
            for (int cc = 0; cc < K; cc++) free(lists[cc]);
            free(lists); free(list_lens); free(list_caps);
            return rc;
        }
    }

    for (int c = 0; c < K; c++) free(lists[c]);
    free(lists); free(list_lens); free(list_caps);
    return 0;
}