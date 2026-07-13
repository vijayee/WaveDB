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

int vector_slsh_search_sync(vector_layer_t *vl, const float *query, int k,
                            vl_result_t **out, int *out_n) {
    (void)vl; (void)query; (void)k; (void)out; (void)out_n;
    return -1;
}

int vector_slsh_delete_sync(vector_layer_t *vl, const char *id) {
    (void)vl; (void)id;
    return -1;
}

int vector_slsh_train(vector_layer_t *vl) {
    (void)vl;
    return 0;
}

int vector_slsh_rebuild(vector_layer_t *vl) {
    (void)vl;
    return 0;
}