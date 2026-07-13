/* vector_flat.c — exact brute-force (baseline + IVF cold-start).
 * Task 3: insert + count. Search/delete/train/rebuild filled in later tasks. */
#include "vector_internal.h"
#include "../../Database/database.h"
#include <stdlib.h>
#include <string.h>

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

int vector_flat_search_sync(vector_layer_t *vl, const float *query, int k,
                            vl_result_t **out, int *out_n) {
    (void)vl; (void)query; (void)k; (void)out; (void)out_n;
    return -1;
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