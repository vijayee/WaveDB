/* vector_slsh.c — SK-LSH bidirectional (uses engine backward scan).
 * Task 1: stubs only. Task 9: insert (atomic batch — vector + hash + count).
 * Tasks 10-11 implement search/train/rebuild/delete.
 *
 * MVCC pattern (learned in Task 8): scan first (read-only), compute in
 * memory, then batch all writes atomically. Never interleave scans with
 * writes. The SLSH insert flow:
 *   1. Load projections (vec/{idx}/proj/{t}, t in 0..L-1) — read-only.
 *   2. Compute LSH key (dot product per table, quantize, pack).
 *   3. Read current count — read-only.
 *   4. ONE batch of 3 ops: vector + hash entry + count. */
#include "vector_internal.h"
#include "../../Database/database.h"
#include "../../Database/database_iterator.h"
#include "../../HBTrie/path.h"
#include "../../HBTrie/identifier.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

/* ---- Projection cache ---- */

typedef struct {
    int n_tables;       /* L = slsh_lsh_tables (capped at how many loaded) */
    int dim;
    float *proj;        /* n_tables * dim floats, row-major; NULL if pre-train */
} slsh_proj_cache_t;

/* Load each vec/{idx}/proj/{t} via direct get. READ-ONLY.
 * Pre-train (no projections): n_tables=0, proj=NULL.
 * Returns 0 on success, negative errno on alloc/get failure. */
static int slsh_load_projections(vector_layer_t *vl, slsh_proj_cache_t *cache) {
    database_t *db = vl->db;
    char d = vl->format.delimiter;
    int dim = vl->format.dim;
    int L = vl->format.slsh_lsh_tables;

    cache->n_tables = 0;
    cache->dim = dim;
    cache->proj = NULL;
    if (L <= 0) return 0;

    cache->proj = (float*)calloc((size_t)L * dim, sizeof(float));
    if (cache->proj == NULL) return -12;
    int loaded = 0;
    for (int t = 0; t < L; t++) {
        char *pkey = vl_key_proj(vl->index_name, d, t);
        if (pkey == NULL) { free(cache->proj); cache->proj = NULL; return -12; }
        uint8_t *buf = NULL; size_t out_len = 0;
        int rc = database_get_sync_raw(db, pkey, strlen(pkey), d, &buf, &out_len);
        free(pkey);
        if (rc == 0 && buf != NULL && out_len >= (size_t)dim * sizeof(float)) {
            memcpy(cache->proj + (size_t)t * dim, buf, (size_t)dim * sizeof(float));
            loaded++;
        }
        if (buf) database_raw_value_free(buf);
    }
    cache->n_tables = loaded;
    if (loaded == 0) { free(cache->proj); cache->proj = NULL; }
    return 0;
}

static void slsh_free_projections(slsh_proj_cache_t *cache) {
    free(cache->proj);
    cache->proj = NULL;
    cache->n_tables = 0;
}

/* Compute the LSH key for `vec` under the cached projections.
 * out_key: caller-allocated buffer of at least (L * bits + 7) / 8 bytes.
 * out_len: set to the actual key length in bytes.
 * Pre-train (no projections): zero key of 1 byte. */
static void slsh_compute_key(vector_layer_t *vl, const float *vec,
                             const slsh_proj_cache_t *cache,
                             uint8_t *out_key, size_t *out_len) {
    int L = vl->format.slsh_lsh_tables;
    int bits = vl->format.slsh_hash_bits;
    float W = vl->format.slsh_bucket_width;
    int dim = vl->format.dim;

    size_t key_bytes = ((size_t)L * bits + 7) / 8;
    if (key_bytes == 0) key_bytes = 1;
    memset(out_key, 0, key_bytes);
    *out_len = key_bytes;

    if (cache->proj == NULL || cache->n_tables == 0) {
        return;  /* pre-train: zero key */
    }
    if (W <= 0.0f) W = 1.0f;  /* guard against div-by-zero */

    /* For each table t: project vec, quantize to `bits` bits, pack into out_key. */
    size_t bit_off = 0;
    for (int t = 0; t < L; t++) {
        float dot = 0.0f;
        for (int dd = 0; dd < dim; dd++) dot += vec[dd] * cache->proj[(size_t)t * dim + dd];
        /* Quantize: floor(dot/W), take low `bits` bits. */
        int q = (int)floorf(dot / W);
        unsigned uq = (unsigned)q;
        for (int b = 0; b < bits; b++) {
            if (uq & (1u << b)) {
                size_t byte = (bit_off + (size_t)b) / 8;
                size_t bit = (bit_off + (size_t)b) % 8;
                if (byte < key_bytes) out_key[byte] |= (uint8_t)(1u << bit);
            }
        }
        bit_off += (size_t)bits;
    }
}

/* ---- Insert ---- */

int vector_slsh_insert_sync(vector_layer_t *vl, const char *id, const float *vec,
                            const uint8_t *metadata, size_t metadata_len) {
    if (vl == NULL || id == NULL || vec == NULL) return -22;
    database_t *db = vl->db;
    if (db == NULL) return -22;
    int dim = vl->format.dim;
    char d = vl->format.delimiter;
    size_t vec_bytes = (size_t)dim * sizeof(float);
    size_t total = vec_bytes + metadata_len;

    /* 1. Load projections (READ-ONLY). */
    slsh_proj_cache_t cache;
    int rc = slsh_load_projections(vl, &cache);
    if (rc != 0) return rc;

    /* 2. Compute LSH key. */
    uint8_t lsh_key[64];
    size_t lsh_len = 0;
    slsh_compute_key(vl, vec, &cache, lsh_key, &lsh_len);
    slsh_free_projections(&cache);

    /* 3. Read current count (READ-ONLY). */
    char *cntkey = vl_key_count(vl->index_name, d);
    if (cntkey == NULL) return -12;
    size_t out_len = 0; uint8_t *buf = NULL; size_t cur = 0;
    rc = database_get_sync_raw(db, cntkey, strlen(cntkey), d, &buf, &out_len);
    if (rc == 0 && buf && out_len >= sizeof(size_t)) memcpy(&cur, buf, sizeof(size_t));
    if (buf) database_raw_value_free(buf);
    size_t next = cur + 1;

    /* 4. Build ONE batch of 3 ops: vector + hash entry + count.
     * All writes are issued atomically via a single database_batch_sync_raw
     * call. No scans happen after this point — MVCC happy. */
    char *vkey = vl_key_vector(vl->index_name, d, id);
    char *hkey = vl_key_hash(vl->index_name, d, lsh_key, lsh_len, id);
    if (!vkey || !hkey) { free(vkey); free(hkey); free(cntkey); return -12; }

    uint8_t *vval = (uint8_t*)malloc(total);
    if (vval == NULL) { free(vkey); free(hkey); free(cntkey); return -12; }
    memcpy(vval, vec, vec_bytes);
    if (metadata && metadata_len > 0) memcpy(vval + vec_bytes, metadata, metadata_len);

    size_t id_len = strlen(id);

    raw_op_t ops[3];
    ops[0].key = vkey; ops[0].key_len = strlen(vkey);
    ops[0].value = vval; ops[0].value_len = total; ops[0].type = 0;
    ops[1].key = hkey; ops[1].key_len = strlen(hkey);
    ops[1].value = (const uint8_t*)id; ops[1].value_len = id_len; ops[1].type = 0;
    ops[2].key = cntkey; ops[2].key_len = strlen(cntkey);
    ops[2].value = (const uint8_t*)&next; ops[2].value_len = sizeof(size_t);
    ops[2].type = 0;

    rc = database_batch_sync_raw(db, d, ops, 3);
    free(vval); free(vkey); free(hkey); free(cntkey);
    return rc;
}

/* ---- Search ---- */

/* Extract the id from a hash key path: vec/{idx}/hash/{hex-lsh}/{id} — id is
   the last path identifier. Returns a malloc'd NUL-terminated string, or
   NULL on failure. */
static char* slsh_extract_id_from_path(path_t *p) {
    size_t n = path_length(p);
    if (n == 0) return NULL;
    identifier_t *last_id = path_get(p, n - 1);
    if (last_id == NULL) return NULL;
    size_t dlen = 0;
    uint8_t *data = identifier_get_data_copy(last_id, &dlen);
    if (data == NULL) return NULL;
    char *id = (char*)malloc(dlen + 1);
    if (id == NULL) { free(data); return NULL; }
    memcpy(id, data, dlen);
    id[dlen] = '\0';
    free(data);
    return id;
}

/* Append a candidate id to the dynamic array, deduping against existing
 * entries. Returns 0 on success, -12 on alloc failure. On success, ownership
 * of `id` is transferred to the array (either stored or freed as a dup). */
static int slsh_append_candidate(char ***arr, size_t *n, size_t *cap, char *id) {
    if (id == NULL) return 0;
    for (size_t i = 0; i < *n; i++) {
        if (strcmp((*arr)[i], id) == 0) { free(id); return 0; }
    }
    if (*n == *cap) {
        size_t ncap = *cap == 0 ? 16 : *cap * 2;
        char **narr = (char**)realloc(*arr, ncap * sizeof(char*));
        if (narr == NULL) { free(id); return -12; }
        *arr = narr; *cap = ncap;
    }
    (*arr)[*n] = id;
    (*n)++;
    return 0;
}

typedef struct { float dist; char *id; uint8_t *metadata; size_t meta_len; } vl_slsh_cand_t;

static int vl_slsh_cand_cmp(const void *a, const void *b) {
    float da = ((const vl_slsh_cand_t*)a)->dist;
    float db = ((const vl_slsh_cand_t*)b)->dist;
    return (da > db) - (da < db);
}

int vector_slsh_search_sync(vector_layer_t *vl, const float *query, int k,
                            vl_result_t **out, int *out_n) {
    if (vl == NULL || query == NULL || out == NULL || out_n == NULL) return -22;
    *out = NULL; *out_n = 0;
    database_t *db = vl->db;
    if (db == NULL) return -22;
    if (k <= 0) return 0;
    int dim = vl->format.dim;
    char d = vl->format.delimiter;
    size_t vec_bytes = (size_t)dim * sizeof(float);

    /* 1. Load projections + compute q_key (READ-ONLY). */
    slsh_proj_cache_t cache;
    int rc = slsh_load_projections(vl, &cache);
    if (rc != 0) return rc;
    uint8_t q_key[64]; size_t qk_len = 0;
    slsh_compute_key(vl, query, &cache, q_key, &qk_len);
    slsh_free_projections(&cache);

    /* 2. Build the q_key bucket prefix: "vec{d}{idx}{d}hash{d}{q_key_hex}{d}".
       vl_key_hash with empty id produces exactly this (trailing delimiter
       before empty id). */
    char *fwd_prefix_str = vl_key_hash(vl->index_name, d, q_key, qk_len, "");
    if (fwd_prefix_str == NULL) return -12;
    size_t fwd_prefix_len = strlen(fwd_prefix_str);

    /* Forward end = prefix + "\x7f" — "everything under prefix" upper bound. */
    char *fwd_end_str = (char*)malloc(fwd_prefix_len + 2);
    if (fwd_end_str == NULL) { free(fwd_prefix_str); return -12; }
    memcpy(fwd_end_str, fwd_prefix_str, fwd_prefix_len);
    fwd_end_str[fwd_prefix_len] = '\x7f';
    fwd_end_str[fwd_prefix_len + 1] = '\0';

    /* Build path_t objects for the scan bounds. database_scan_start copies
       the paths internally, so we can destroy them after the call. */
    path_t *fwd_start_path = path_create_from_raw(fwd_prefix_str, fwd_prefix_len,
                                                   d, db->chunk_size);
    path_t *fwd_end_path = path_create_from_raw(fwd_end_str, fwd_prefix_len + 1,
                                                 d, db->chunk_size);
    free(fwd_prefix_str);
    free(fwd_end_str);
    if (fwd_start_path == NULL || fwd_end_path == NULL) {
        if (fwd_start_path) path_destroy(fwd_start_path);
        if (fwd_end_path) path_destroy(fwd_end_path);
        return -12;
    }

    /* 3. Forward scan for right neighbors (depth scan_radius). */
    char **candidate_ids = NULL;
    size_t n_candidates = 0, cap_candidates = 0;
    int radius = vl->runtime.slsh_scan_radius;
    if (radius <= 0) radius = 10;

    database_iterator_t *fwd = database_scan_start(db, fwd_start_path, fwd_end_path);
    if (fwd != NULL) {
        path_t *p = NULL; identifier_t *v = NULL;
        for (int r = 0; r < radius && database_scan_next(fwd, &p, &v) == 0; r++) {
            char *id = slsh_extract_id_from_path(p);
            int arc = slsh_append_candidate(&candidate_ids, &n_candidates,
                                             &cap_candidates, id);
            if (arc != 0) {
                if (p) path_destroy(p);
                if (v) identifier_destroy(v);
                p = NULL; v = NULL;
                break;
            }
            if (p) path_destroy(p);
            if (v) identifier_destroy(v);
            p = NULL; v = NULL;
        }
        database_scan_end(fwd);
    }

    /* 4. Backward scan for left neighbors (depth scan_radius), if bidirectional.
       Reverse scan: end = fwd_start_path (positions at largest key < end_path),
       start = "vec{d}{idx}{d}hash{d}" — lower bound covers all hash buckets. */
    if (vl->runtime.slsh_bidirectional) {
        /* Build lower-bound prefix: "vec{d}{idx}{d}hash{d}". */
        size_t idx_len = strlen(vl->index_name);
        size_t hash_pfx_len = 3 + 1 + idx_len + 1 + 4 + 1;  /* "vec"+d+idx+d+"hash"+d */
        char *hash_pfx_str = (char*)malloc(hash_pfx_len + 1);
        if (hash_pfx_str != NULL) {
            snprintf(hash_pfx_str, hash_pfx_len + 1, "vec%c%s%chash%c",
                     d, vl->index_name, d, d);
            path_t *bwd_start_path = path_create_from_raw(hash_pfx_str, hash_pfx_len,
                                                          d, db->chunk_size);
            free(hash_pfx_str);
            if (bwd_start_path != NULL) {
                /* database_scan_start_reverse takes ownership of the path_t args;
                   we still own fwd_start_path (the iterator copies), so reuse it. */
                database_iterator_t *bwd = database_scan_start_reverse(db,
                                                                       bwd_start_path,
                                                                       fwd_start_path);
                path_destroy(bwd_start_path);
                if (bwd != NULL) {
                    path_t *p = NULL; identifier_t *v = NULL;
                    for (int r = 0; r < radius && database_scan_prev(bwd, &p, &v) == 0; r++) {
                        char *id = slsh_extract_id_from_path(p);
                        int arc = slsh_append_candidate(&candidate_ids, &n_candidates,
                                                         &cap_candidates, id);
                        if (arc != 0) {
                            if (p) path_destroy(p);
                            if (v) identifier_destroy(v);
                            p = NULL; v = NULL;
                            break;
                        }
                        if (p) path_destroy(p);
                        if (v) identifier_destroy(v);
                        p = NULL; v = NULL;
                    }
                    database_scan_end(bwd);
                }
            }
        }
    }

    path_destroy(fwd_start_path);
    path_destroy(fwd_end_path);

    /* 5. Fetch candidate vectors, exact rerank, top-k. */
    vl_slsh_cand_t *hits = NULL;
    if (n_candidates > 0) {
        hits = (vl_slsh_cand_t*)malloc(n_candidates * sizeof(vl_slsh_cand_t));
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
    free(candidate_ids);

    /* Sort by distance ascending, take top-k. */
    if (n_hits > 1) {
        qsort(hits, n_hits, sizeof(vl_slsh_cand_t), vl_slsh_cand_cmp);
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
            out_results[i].id = hits[i].id;
            out_results[i].distance = hits[i].dist;
            out_results[i].metadata = hits[i].metadata;
            out_results[i].metadata_len = hits[i].meta_len;
        }
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

/* ---- Train ---- */

int vector_slsh_train(vector_layer_t *vl) {
    if (vl == NULL) return -22;
    database_t *db = vl->db;
    if (db == NULL) return -22;
    int dim = vl->format.dim;
    int L = vl->format.slsh_lsh_tables;
    char d = vl->format.delimiter;
    if (L <= 0) return -22;

    /* Fixed seed (42) for reproducibility — same projections across
       reopen/train calls. Uniform [0,1] random projections are fine for the
       spike; production would use a proper RNG (and likely Gaussian). */
    srand(42);
    for (int t = 0; t < L; t++) {
        float *proj = (float*)malloc((size_t)dim * sizeof(float));
        if (proj == NULL) return -12;
        for (int dd = 0; dd < dim; dd++) {
            proj[dd] = (float)rand() / (float)RAND_MAX;  /* [0, 1] */
        }
        char *pkey = vl_key_proj(vl->index_name, d, t);
        if (pkey == NULL) { free(proj); return -12; }
        int rc = database_put_sync_raw(db, pkey, strlen(pkey), d,
                                       (const uint8_t*)proj,
                                       (size_t)dim * sizeof(float));
        free(proj); free(pkey);
        if (rc != 0) return rc;
    }
    return 0;
}

/* ---- Rebuild ---- */

int vector_slsh_rebuild(vector_layer_t *vl) {
    if (vl == NULL) return -22;
    database_t *db = vl->db;
    if (db == NULL) return -22;
    int dim = vl->format.dim;
    char d = vl->format.delimiter;
    size_t vec_bytes = (size_t)dim * sizeof(float);
    size_t idx_len = strlen(vl->index_name);

    /* 1. Scan vec/{idx}/hash/ for old hash entries (READ-ONLY). */
    size_t hprefix_len = 4 + idx_len + 6;  /* "vec"+d+idx+d+"hash"+d */
    char *hprefix = (char*)malloc(hprefix_len + 1);
    if (hprefix == NULL) return -12;
    snprintf(hprefix, hprefix_len + 1, "vec%c%s%chash%c", d, vl->index_name, d, d);
    char *hend = (char*)malloc(hprefix_len + 2);
    if (hend == NULL) { free(hprefix); return -12; }
    memcpy(hend, hprefix, hprefix_len);
    hend[hprefix_len] = '\x7f'; hend[hprefix_len + 1] = '\0';

    raw_result_t *old_hashes = NULL;
    size_t n_old = 0;
    int rc = database_scan_range_sync_raw(db, hprefix, hprefix_len, hend,
                                          hprefix_len + 1, d,
                                          &old_hashes, &n_old);
    free(hprefix); free(hend);
    if (rc != 0) return rc;

    /* 2. Scan vec/{idx}/vector/ for all vectors (READ-ONLY). */
    size_t vprefix_len = 4 + idx_len + 7;  /* "vec"+d+idx+d+"vector"+d */
    char *vprefix = (char*)malloc(vprefix_len + 1);
    if (vprefix == NULL) { database_raw_results_free(old_hashes, n_old); return -12; }
    snprintf(vprefix, vprefix_len + 1, "vec%c%s%cvector%c", d, vl->index_name, d, d);
    char *vend = (char*)malloc(vprefix_len + 2);
    if (vend == NULL) { free(vprefix); database_raw_results_free(old_hashes, n_old); return -12; }
    memcpy(vend, vprefix, vprefix_len);
    vend[vprefix_len] = '\x7f'; vend[vprefix_len + 1] = '\0';

    raw_result_t *vectors = NULL;
    size_t n_vectors = 0;
    rc = database_scan_range_sync_raw(db, vprefix, vprefix_len, vend,
                                      vprefix_len + 1, d,
                                      &vectors, &n_vectors);
    free(vprefix); free(vend);
    if (rc != 0) { database_raw_results_free(old_hashes, n_old); return rc; }

    /* 3. Load projections (READ-ONLY). */
    slsh_proj_cache_t cache;
    rc = slsh_load_projections(vl, &cache);
    if (rc != 0) {
        database_raw_results_free(old_hashes, n_old);
        database_raw_results_free(vectors, n_vectors);
        return rc;
    }

    /* 4. Compute new hash keys for each vector. */
    char **new_hash_keys = (char**)malloc((n_vectors ? n_vectors : 1) * sizeof(char*));
    const char **new_id_ptrs = (const char**)malloc((n_vectors ? n_vectors : 1) * sizeof(const char*));
    if (new_hash_keys == NULL || new_id_ptrs == NULL) {
        free(new_hash_keys); free(new_id_ptrs);
        slsh_free_projections(&cache);
        database_raw_results_free(old_hashes, n_old);
        database_raw_results_free(vectors, n_vectors);
        return -12;
    }
    size_t n_new = 0;
    for (size_t i = 0; i < n_vectors; i++) {
        if (vectors[i].value_len < vec_bytes) continue;
        /* extract id from key (after last delimiter) */
        const char *key = vectors[i].key;
        size_t klen = vectors[i].key_len;
        const char *id = key + klen;
        for (size_t j = klen; j > 0; j--) {
            if (key[j-1] == d) { id = key + j; break; }
        }
        uint8_t lsh_key[64]; size_t lsh_len = 0;
        slsh_compute_key(vl, (const float*)vectors[i].value, &cache,
                         lsh_key, &lsh_len);
        char *hkey = vl_key_hash(vl->index_name, d, lsh_key, lsh_len, id);
        if (hkey == NULL) {
            for (size_t j = 0; j < n_new; j++) free(new_hash_keys[j]);
            free(new_hash_keys); free(new_id_ptrs);
            slsh_free_projections(&cache);
            database_raw_results_free(old_hashes, n_old);
            database_raw_results_free(vectors, n_vectors);
            return -12;
        }
        new_hash_keys[n_new] = hkey;
        new_id_ptrs[n_new] = id;  /* borrows into vectors[i].key */
        n_new++;
    }
    slsh_free_projections(&cache);

    /* 5. Build the batch. Partition old vs new — skip intersection (same-key
       delete-then-put under MVCC shares the same txn_id and shadows).
       O(N²) intersection is fine for the spike. */
    size_t n_ops_max = n_old + n_new;
    raw_op_t *ops = NULL;
    if (n_ops_max > 0) {
        ops = (raw_op_t*)malloc(n_ops_max * sizeof(raw_op_t));
        if (ops == NULL) {
            for (size_t j = 0; j < n_new; j++) free(new_hash_keys[j]);
            free(new_hash_keys); free(new_id_ptrs);
            database_raw_results_free(old_hashes, n_old);
            database_raw_results_free(vectors, n_vectors);
            return -12;
        }
    }
    size_t op_i = 0;

    /* 5a. Deletes for old hash keys not in the new set. Key pointer borrows
       into old_hashes[i].key (valid until we free old_hashes — AFTER batch). */
    for (size_t i = 0; i < n_old; i++) {
        const char *old_key = old_hashes[i].key;
        int still_present = 0;
        for (size_t j = 0; j < n_new; j++) {
            if (strcmp(old_key, new_hash_keys[j]) == 0) {
                still_present = 1;
                break;
            }
        }
        if (still_present) continue;  /* unchanged — leave alone */
        ops[op_i].key = old_key;
        ops[op_i].key_len = old_hashes[i].key_len;
        ops[op_i].value = NULL;
        ops[op_i].value_len = 0;
        ops[op_i].type = 1;  /* delete */
        op_i++;
    }

    /* 5b. Puts for new hash keys not in the old set. */
    for (size_t j = 0; j < n_new; j++) {
        const char *new_key = new_hash_keys[j];
        int already_present = 0;
        for (size_t i = 0; i < n_old; i++) {
            if (strcmp(new_key, old_hashes[i].key) == 0) {
                already_present = 1;
                break;
            }
        }
        if (already_present) continue;  /* unchanged — leave alone */
        ops[op_i].key = new_key;
        ops[op_i].key_len = strlen(new_key);
        ops[op_i].value = (const uint8_t*)new_id_ptrs[j];
        ops[op_i].value_len = strlen(new_id_ptrs[j]);
        ops[op_i].type = 0;  /* put */
        op_i++;
    }

    /* 6. Apply the batch atomically. */
    if (op_i > 0) {
        rc = database_batch_sync_raw(db, d, ops, op_i);
    } else {
        rc = 0;
    }

    /* 7. Cleanup. */
    for (size_t j = 0; j < n_new; j++) free(new_hash_keys[j]);
    free(new_hash_keys);
    free(new_id_ptrs);
    free(ops);
    database_raw_results_free(old_hashes, n_old);
    database_raw_results_free(vectors, n_vectors);
    return rc;
}

/* ---- Delete ---- */

int vector_slsh_delete_sync(vector_layer_t *vl, const char *id) {
    if (vl == NULL || id == NULL) return -22;
    database_t *db = vl->db;
    if (db == NULL) return -22;
    int dim = vl->format.dim;
    char d = vl->format.delimiter;
    size_t vec_bytes = (size_t)dim * sizeof(float);

    /* 1. Read the vector (READ-ONLY) to compute its lsh_key. */
    char *vkey = vl_key_vector(vl->index_name, d, id);
    if (vkey == NULL) return -12;
    uint8_t *vbuf = NULL; size_t vlen = 0;
    int rc = database_get_sync_raw(db, vkey, strlen(vkey), d, &vbuf, &vlen);
    if (rc != 0 || vbuf == NULL || vlen < vec_bytes) {
        if (vbuf) database_raw_value_free(vbuf);
        free(vkey);
        return rc != 0 ? rc : -2;  /* not found */
    }

    /* 2. Compute lsh_key. */
    slsh_proj_cache_t cache;
    rc = slsh_load_projections(vl, &cache);
    if (rc != 0) { database_raw_value_free(vbuf); free(vkey); return rc; }
    uint8_t lsh_key[64]; size_t lsh_len = 0;
    slsh_compute_key(vl, (const float*)vbuf, &cache, lsh_key, &lsh_len);
    slsh_free_projections(&cache);
    database_raw_value_free(vbuf);

    char *hkey = vl_key_hash(vl->index_name, d, lsh_key, lsh_len, id);
    free(vkey);  /* rebuilt below for the batch */
    if (hkey == NULL) return -12;

    /* 3. Read count (READ-ONLY). */
    char *cntkey = vl_key_count(vl->index_name, d);
    if (cntkey == NULL) { free(hkey); return -12; }
    uint8_t *cbuf = NULL; size_t clen = 0; size_t cur = 0;
    rc = database_get_sync_raw(db, cntkey, strlen(cntkey), d, &cbuf, &clen);
    if (rc == 0 && cbuf && clen >= sizeof(size_t)) memcpy(&cur, cbuf, sizeof(size_t));
    if (cbuf) database_raw_value_free(cbuf);

    /* 4. Build the batch: delete vector + delete hash + put count-1 (floor 0). */
    vkey = vl_key_vector(vl->index_name, d, id);
    if (vkey == NULL) { free(hkey); free(cntkey); return -12; }
    size_t next = cur > 0 ? cur - 1 : 0;

    raw_op_t ops[3];
    ops[0].key = vkey; ops[0].key_len = strlen(vkey);
    ops[0].value = NULL; ops[0].value_len = 0; ops[0].type = 1;  /* delete vector */
    ops[1].key = hkey; ops[1].key_len = strlen(hkey);
    ops[1].value = NULL; ops[1].value_len = 0; ops[1].type = 1;  /* delete hash */
    ops[2].key = cntkey; ops[2].key_len = strlen(cntkey);
    ops[2].value = (const uint8_t*)&next; ops[2].value_len = sizeof(size_t);
    ops[2].type = 0;  /* put count */

    rc = database_batch_sync_raw(db, d, ops, 3);
    free(vkey); free(hkey); free(cntkey);
    return rc;
}