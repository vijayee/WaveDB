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
    VL_INDEX_SLSH = 2    /* SK-LSH bidirectional (uses engine backward scan) */
} vl_index_type_t;

typedef enum {
    VL_DIST_L2     = 0,
    VL_DIST_COSINE = 1,   /* default for embedding workloads */
    VL_DIST_DOT    = 2
} vl_distance_t;

/* Format tier — IMMUTABLE after create. To change any field here, drop the
   subtree (or separate db) and recreate the layer. vector_layer_reconfigure
   accepts only the runtime tier; the C signature makes format mutation
   structurally impossible. */
typedef struct {
    vl_index_type_t index_type;
    int      dim;                 /* vector dimensionality */
    char     delimiter;           /* path-segment separator, default '/' */
    vl_distance_t distance;       /* distance metric for assignment + rerank */
    /* IVF format-defining */
    int      ivf_n_clusters;
    /* SLSH format-defining */
    int      slsh_lsh_tables;     /* compound hash width (L) */
    int      slsh_hash_bits;      /* bits per table */
    float    slsh_bucket_width;   /* LSH projection width W */
} vector_layer_format_t;

/* Runtime tier — freely mutable via vector_layer_reconfigure. */
typedef struct {
    int      top_k;               /* default result count */
    int      sync_only;           /* 1 = single-threaded, disable MVCC */
    /* IVF runtime */
    int      ivf_nprobe;
    int      ivf_flat_until;      /* exact FLAT below this many vectors */
    /* SLSH runtime */
    int      slsh_scan_radius;    /* forward+backward scan depth each direction */
    int      slsh_bidirectional;  /* 1 = scan both directions (default);
                                     0 = right-only (lower recall, cheaper) */
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
   create; to change index_type/dim/delimiter/ivf_n_clusters/slsh_*, drop the
   subtree (or separate db) and recreate. */
int   vector_layer_reconfigure(vector_layer_t *vl,
                               vector_layer_runtime_t *runtime);

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