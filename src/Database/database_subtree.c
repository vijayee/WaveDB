//
// database_subtree.c - Subtree view over a database with prefix isolation
//

#include "database_subtree.h"
#include "../Util/allocator.h"
#include "../HBTrie/identifier.h"
#include <string.h>
#include <stdlib.h>

/**
 * Destroy callback for refcounter.
 * Called when reference count drops to 0.
 */
static void subtree_destroy(database_subtree_t* st) {
    if (st == NULL) return;

    free(st->prefix);
    free(st);
}

database_subtree_t* database_subtree_open(database_t* db,
                                           const char* prefix,
                                           char delimiter) {
    if (db == NULL || prefix == NULL) return NULL;

    database_subtree_t* st = get_clear_memory(sizeof(database_subtree_t));

    st->prefix_len = strlen(prefix);
    st->prefix = get_clear_memory(st->prefix_len + 1);
    memcpy(st->prefix, prefix, st->prefix_len + 1);

    st->db = db;
    st->delimiter = delimiter;
    st->chunk_size = db->chunk_size;

    refcounter_init((refcounter_t*) st);
    return st;
}

void database_subtree_close(database_subtree_t* subtree) {
    if (subtree == NULL) return;

    refcounter_dereference((refcounter_t*) subtree);
    if (refcounter_count((refcounter_t*) subtree) == 0) {
        subtree_destroy(subtree);
    }
}

int database_subtree_delete(database_t* db,
                            const char* prefix,
                            char delimiter) {
    if (db == NULL || prefix == NULL) return -1;

    /* Build the scan prefix: "prefix{delimiter}" */
    size_t prefix_len = strlen(prefix);
    size_t scan_len = prefix_len + 1; /* prefix + delimiter */
    char* scan_prefix = get_memory(scan_len + 1); /* +1 for null terminator */

    memcpy(scan_prefix, prefix, prefix_len);
    scan_prefix[prefix_len] = delimiter;
    scan_prefix[scan_len] = '\0';

    /* Scan for all keys under the prefix */
    raw_result_t* results = NULL;
    size_t count = 0;
    int rc = database_scan_sync_raw(db, scan_prefix, scan_len, delimiter,
                                     &results, &count);
    free(scan_prefix);

    if (rc != 0) return -1;

    /* Delete each key */
    for (size_t i = 0; i < count; i++) {
        database_delete_sync_raw(db, results[i].key, results[i].key_len,
                                  delimiter);
    }

    database_raw_results_free(results, count);
    return 0;
}

database_t* database_subtree_get_db(database_subtree_t* st) {
    if (st == NULL) return NULL;
    return st->db;
}

work_pool_t* database_subtree_get_pool(database_subtree_t* st) {
    if (st == NULL) return NULL;
    return st->db->pool;
}

path_t* database_subtree_prepend_path(database_subtree_t* st, path_t* path) {
    if (st == NULL) return NULL;

    /* Create the prefix path from the raw prefix string */
    path_t* prefix_path = path_create_from_raw(st->prefix, st->prefix_len,
                                                st->delimiter, st->chunk_size);
    if (prefix_path == NULL) return NULL;

    /* Append each identifier from the original path */
    size_t len = path_length(path);
    for (size_t i = 0; i < len; i++) {
        identifier_t* id = path_get(path, i);
        if (id == NULL) {
            path_destroy(prefix_path);
            return NULL;
        }
        int rc = path_append(prefix_path, id);
        if (rc != 0) {
            path_destroy(prefix_path);
            return NULL;
        }
    }

    return prefix_path;
}

char* database_subtree_prepend_key(database_subtree_t* st,
                                    const char* key, size_t key_len,
                                    size_t* out_len) {
    if (st == NULL || key == NULL) return NULL;

    /* Allocate: prefix + delimiter + key + null terminator */
    size_t total_len = st->prefix_len + 1 + key_len;
    char* result = get_memory(total_len + 1);

    memcpy(result, st->prefix, st->prefix_len);
    result[st->prefix_len] = st->delimiter;
    memcpy(result + st->prefix_len + 1, key, key_len);
    result[total_len] = '\0';

    if (out_len != NULL) {
        *out_len = total_len;
    }

    return result;
}

/* --- Path-based sync CRUD --- */

int database_subtree_put_sync(database_subtree_t* st, path_t* path, identifier_t* value) {
    if (st == NULL || path == NULL || value == NULL) return -1;

    path_t* full_path = database_subtree_prepend_path(st, path);
    if (full_path == NULL) return -1;

    /* database_put_sync consumes full_path and value */
    return database_put_sync(st->db, full_path, value);
}

int database_subtree_get_sync(database_subtree_t* st, path_t* path, identifier_t** result) {
    if (st == NULL || path == NULL || result == NULL) return -1;

    path_t* full_path = database_subtree_prepend_path(st, path);
    if (full_path == NULL) return -1;

    /* database_get_sync consumes full_path */
    return database_get_sync(st->db, full_path, result);
}

int database_subtree_delete_sync(database_subtree_t* st, path_t* path) {
    if (st == NULL || path == NULL) return -1;

    path_t* full_path = database_subtree_prepend_path(st, path);
    if (full_path == NULL) return -1;

    /* database_delete_sync consumes full_path */
    return database_delete_sync(st->db, full_path);
}

int64_t database_subtree_increment_sync(database_subtree_t* st, path_t* path, int64_t delta) {
    if (st == NULL || path == NULL) return -1;

    path_t* full_path = database_subtree_prepend_path(st, path);
    if (full_path == NULL) return -1;

    /* database_increment_sync consumes full_path */
    return database_increment_sync(st->db, full_path, delta);
}

/* --- Raw (byte) sync CRUD --- */

int database_subtree_put_sync_raw(database_subtree_t* st,
                                   const char* key, size_t key_len, char delimiter,
                                   const uint8_t* value, size_t value_len) {
    if (st == NULL || key == NULL) return -1;

    size_t prefixed_len = 0;
    char* prefixed_key = database_subtree_prepend_key(st, key, key_len, &prefixed_len);
    if (prefixed_key == NULL) return -1;

    int rc = database_put_sync_raw(st->db, prefixed_key, prefixed_len, delimiter,
                                   value, value_len);
    free(prefixed_key);
    return rc;
}

int database_subtree_get_sync_raw(database_subtree_t* st,
                                   const char* key, size_t key_len, char delimiter,
                                   uint8_t** value_out, size_t* value_len_out) {
    if (st == NULL || key == NULL) return -1;

    size_t prefixed_len = 0;
    char* prefixed_key = database_subtree_prepend_key(st, key, key_len, &prefixed_len);
    if (prefixed_key == NULL) return -1;

    int rc = database_get_sync_raw(st->db, prefixed_key, prefixed_len, delimiter,
                                   value_out, value_len_out);
    free(prefixed_key);
    return rc;
}

int database_subtree_delete_sync_raw(database_subtree_t* st,
                                      const char* key, size_t key_len, char delimiter) {
    if (st == NULL || key == NULL) return -1;

    size_t prefixed_len = 0;
    char* prefixed_key = database_subtree_prepend_key(st, key, key_len, &prefixed_len);
    if (prefixed_key == NULL) return -1;

    int rc = database_delete_sync_raw(st->db, prefixed_key, prefixed_len, delimiter);
    free(prefixed_key);
    return rc;
}