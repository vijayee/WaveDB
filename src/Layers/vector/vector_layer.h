#ifndef WAVEDB_VECTOR_LAYER_H
#define WAVEDB_VECTOR_LAYER_H

#include <stdint.h>
#include <stddef.h>
#include "../../Database/database.h"
#include "../../Database/database_subtree.h"
#include "../../Workers/promise.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct vector_layer_t vector_layer_t;

typedef enum {
    VL_INDEX_FLAT = 0,   /* exact brute-force; baseline + IVF cold-start */
    VL_INDEX_IVF  = 1,   /* inverted file */
    VL_INDEX_SLSH = 2    /* SK-LSH bidirectional scan (always forward+backward) */
} vl_index_type_t;

typedef enum {
    VL_DIST_L2     = 0,
    VL_DIST_COSINE = 1,   /* default for embedding workloads */
    VL_DIST_DOT    = 2
} vl_distance_t;

/* Returned by vector_layer_create / open_separate (via *error_code) when an
   existing index is reopened with an index_type that does not match the
   persisted __format leaf. A loud error (not silent degradation): only
   vector_layer_migrate changes the type on disk. */
#define VL_EFORMAT_MISMATCH (-1001)

/* Format tier — IMMUTABLE after create EXCEPT via the explicit
   vector_layer_migrate() operation (the only way to change index_type /
   ivf_n_clusters / slsh_* / distance in place; it restructures the aux keys +
   retrains + updates __format). dim and delimiter are fixed for the life of
   the index (the stored vectors / key paths depend on them) and migrate
   rejects a change to them. vector_layer_reconfigure accepts only the
   runtime tier; the C signature makes runtime-only mutation obvious. */
typedef struct {
    vl_index_type_t index_type;
    int      dim;                 /* vector dimensionality */
    char     delimiter;           /* path-segment separator, default '/' */
    vl_distance_t distance;       /* distance metric for assignment + rerank */
    /* IVF format-defining */
    int      ivf_n_clusters;      /* default 50 (set in vl_init if <= 0) */
    /* SLSH format-defining */
    int      slsh_lsh_tables;     /* compound hash width (L); default 4 */
    int      slsh_hash_bits;      /* bits per table; default 16 */
    float    slsh_bucket_width;   /* LSH projection width W; default 2.0 */
} vector_layer_format_t;

/* Runtime tier — freely mutable via vector_layer_reconfigure. */
typedef struct {
    int      top_k;               /* default result count */
    int      sync_only;           /* 1 = single-threaded, disable MVCC */
    /* IVF runtime */
    int      ivf_nprobe;          /* default 8 (clears 0.90 recall@10 on 10k/30k x 384) */
    int      ivf_flat_until;      /* exact FLAT below this many vectors; default 1000 */
    /* SLSH runtime */
    int      slsh_scan_radius;    /* FLOOR on forward+backward scan depth each
                                     direction; default 200 (clears 0.90 recall@10
                                     on 10k x 384/768 clustered). The actual radius
                                     used is max(slsh_scan_radius, count/30) — the
                                     adaptive part scales with dataset size so users
                                     don't need to manually reconfigure for larger
                                     datasets (~333 for 10k, ~1000 for 30k, ~1666
                                     for 50k). */
} vector_layer_runtime_t;

typedef struct {
    vector_layer_format_t  format;
    vector_layer_runtime_t runtime;
} vector_layer_config_t;

typedef struct {
    char    *id;
    float    distance;
    uint8_t *metadata;            /* caller frees via vector_layer_free_results */
    size_t   metadata_len;
} vl_result_t;

/* Result of an async search — resolved payload of the promise handed to
 * vector_layer_search. The caller frees `results` via
 * vector_layer_free_results, then frees the struct itself. */
typedef struct {
    vl_result_t *results;
    int          n_results;
} vl_search_result_t;

/* Open on an EXISTING database_t (shared key space, e.g. with Graph).
   If subtree is non-NULL, all keys land under that subtree prefix. */
vector_layer_t* vector_layer_create(const char *index_name,
                                     database_t *db,
                                     database_subtree_t *subtree,
                                     vector_layer_config_t *config,
                                     int *error_code);

/* Open a DEDICATED vector_db instance at db_location (clean LRU/WAL tuning,
   mirrors the Hippo document_db split). No subtree. */
vector_layer_t* vector_layer_open_separate(const char *db_location,
                                            const char *index_name,
                                            vector_layer_config_t *config,
                                            int *error_code);

void  vector_layer_destroy(vector_layer_t *vl);

/* Reconfigure runtime-tunable params only. Format tier is immutable after
   create; to change index_type/dim/delimiter/ivf_n_clusters/slsh_*, call
   vector_layer_migrate (NOT reconfigure -- reconfigure is runtime-only and
   cannot change the format). dim/delimiter are fixed for the index's life. */
int   vector_layer_reconfigure(vector_layer_t *vl,
                               vector_layer_runtime_t *runtime);

/* Read the layer's in-memory Format (index_type confirmed against the
   persisted __format leaf at open). out must point to a caller-owned
   vector_layer_format_t. Returns 0 on success, -22 on NULL args. */
int   vector_layer_get_format(vector_layer_t *vl,
                               vector_layer_format_t *out);

/* MIGRATION IS A DESTRUCTIVE, EXPLICIT OPERATION -- the ONLY way to change
 * the index type. Reopening with a mismatched index_type returns
 * VL_EFORMAT_MISMATCH (it does NOT migrate and no longer silently degrades);
 * only migrate() restructures the aux keys + retrains + updates __format.
 *
 * new_fmt->dim and new_fmt->delimiter MUST match the layer's current values
 * (stored vectors/key paths depend on them); other fields (index_type,
 * ivf_n_clusters, slsh_*, distance) may change. Deletes the OLD type's aux
 * child prefixes (cluster/+centroid/ for IVF, hash/+proj/ for SLSH; FLAT none),
 * writes __format = new type (the recovery anchor -- disk+memory in sync),
 * adopts new_fmt, then trains the new index. Stored vectors, count, and
 * __format are preserved (delete is by specific child prefix, never the
 * parent vec/{idx}/).
 *
 * Crash safety: NOT atomic across the whole op. __format is the recovery
 * anchor: a crash leaves __format=target + half-built aux + all vectors
 * intact (no data loss). Re-run migrate(new_fmt) to converge (idempotent).
 * The no-op-on-identical guard is intentionally REMOVED so a re-run always
 * completes a half-built index; use train()/rebuild() for a cheaper same-type
 * retrain that does NOT rewrite __format.
 *
 * Progress: emits log_info events "vector\t{phase}\t{current}\t{total}"
 * (phases scan/delete/train/rebuild/done) per 8000-op chunk. Register a
 * callback via wavedb_log_add_callback (see src/Util/log.h). Works correctly
 * with NO callback registered (events simply go to stderr unless suppressed
 * via log_set_quiet).
 *
 * Returns 0 on success, -22 on NULL args or dim/delimiter mismatch, or a
 * propagated train/scan error. */
int   vector_layer_migrate(vector_layer_t *vl,
                           const vector_layer_format_t *new_fmt);

/* Async variants — Graph-style: return void, take a caller-owned promise_t*.
 * The caller creates the promise (with resolve/reject callbacks + ctx), passes
 * it in, and wires it to their runtime. The worker resolves the promise with
 * NULL (insert/delete/batch) or a vl_search_result_t* (search) on success, or
 * rejects with an async_error_t* on failure. The caller destroys the promise
 * on their side after it fires. If no worker pool is available (sync_only mode
 * or db->pool == NULL), the sync version runs inline and the promise is
 * resolved before the function returns. */
void  vector_layer_insert(vector_layer_t *vl, const char *id,
                          const float *vec,
                          const uint8_t *metadata, size_t metadata_len,
                          promise_t *promise);
int   vector_layer_insert_sync(vector_layer_t *vl, const char *id,
                               const float *vec,
                               const uint8_t *metadata, size_t metadata_len);

void  vector_layer_insert_batch(vector_layer_t *vl,
                                const char **ids, const float **vecs,
                                const uint8_t **metadatas, const size_t *meta_lens,
                                int n, promise_t *promise);
int   vector_layer_insert_batch_sync(vector_layer_t *vl,
                                     const char **ids, const float **vecs,
                                     const uint8_t **metadatas, const size_t *meta_lens,
                                     int n);

void  vector_layer_search(vector_layer_t *vl, const float *query, int k,
                          promise_t *promise);
int   vector_layer_search_sync(vector_layer_t *vl, const float *query, int k,
                               vl_result_t **results, int *n_results);

void  vector_layer_delete(vector_layer_t *vl, const char *id,
                          promise_t *promise);
int   vector_layer_delete_sync(vector_layer_t *vl, const char *id);

/* train: IVF (re)compute centroids (k-means); SLSH (re)gen projections;
          FLAT no-op. Sync-only (caller off-threads if needed). */
int   vector_layer_train(vector_layer_t *vl);

/* rebuild: rewrite the index's per-vector entries (IVF memberships, SLSH
   hashes) from the stored vectors. Preserves vec/{index}/vector/<id>. Sync-only. */
int   vector_layer_rebuild(vector_layer_t *vl);

size_t vector_layer_count(vector_layer_t *vl);
void   vector_layer_free_results(vl_result_t *results, int n);

#ifdef __cplusplus
}
#endif
#endif