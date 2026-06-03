//
// database_subtree.c - Subtree view over a database with prefix isolation
//

#include "database_subtree.h"
#include "database_iterator.h"
#include "batch.h"
#include "../Util/allocator.h"
#include "../HBTrie/identifier.h"
#include "../Workers/work.h"
#include <string.h>
#include <stdlib.h>

/**
 * Destroy the subtree when its reference count drops to 0.
 * Uses an atomic-ish destroying flag to prevent double-free from
 * concurrent close calls (the TOCTOU race between dereference and count check).
 */
static void subtree_destroy(database_subtree_t* st) {
    if (st == NULL || st->destroying) return;

    // Mark as destroying so a concurrent close cannot also enter this path.
    st->destroying = true;

    // Capture locals before freeing the struct.
    database_t* db = st->db;
    char* prefix = st->prefix;

    free(prefix);
    free(st);

    if (db) {
        database_destroy(db);
    }
}

database_subtree_t* database_subtree_open(database_t* db,
                                           const char* prefix,
                                           char delimiter) {
    if (db == NULL || prefix == NULL) return NULL;
    if (delimiter == '\0') return NULL;

    database_subtree_t* st = get_clear_memory(sizeof(database_subtree_t));
    if (st == NULL) return NULL;

    st->prefix_len = strlen(prefix);
    st->prefix = get_clear_memory(st->prefix_len + 1);
    if (st->prefix == NULL) {
        free(st);
        return NULL;
    }
    memcpy(st->prefix, prefix, st->prefix_len + 1);

    st->db = db;
    st->delimiter = delimiter;
    st->chunk_size = db->chunk_size;

    // Keep the database alive for as long as any subtree references it.
    refcounter_reference((refcounter_t*) db);
    refcounter_init((refcounter_t*) st);
    return st;
}

void database_subtree_close(database_subtree_t* subtree) {
    if (subtree == NULL || subtree->destroying) return;

    refcounter_dereference((refcounter_t*) subtree);
    if (refcounter_count((refcounter_t*) subtree) == 0) {
        subtree_destroy(subtree);
    }
}

int database_subtree_delete_prefix(database_t* db,
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

    /* Delete each key, tracking errors */
    int delete_errors = 0;
    for (size_t i = 0; i < count; i++) {
        int rc = database_delete_sync_raw(db, results[i].key, results[i].key_len,
                                          delimiter);
        if (rc != 0) delete_errors++;
    }

    database_raw_results_free(results, count);
    return delete_errors > 0 ? -1 : 0;
}

database_t* database_subtree_get_db(database_subtree_t* st) {
    if (st == NULL) return NULL;
    return st->db;
}

work_pool_t* database_subtree_get_pool(database_subtree_t* st) {
    if (st == NULL || st->db == NULL) return NULL;
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

    /* Overflow check: prefix_len + 1 + key_len must not wrap around */
    if (key_len > SIZE_MAX - st->prefix_len - 1) return NULL;

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
    path_destroy(path);
    if (full_path == NULL) {
        identifier_destroy(value);
        return -1;
    }

    /* database_put_sync consumes full_path and value */
    return database_put_sync(st->db, full_path, value);
}

int database_subtree_get_sync(database_subtree_t* st, path_t* path, identifier_t** result) {
    if (st == NULL || path == NULL || result == NULL) return -1;

    path_t* full_path = database_subtree_prepend_path(st, path);
    path_destroy(path);
    if (full_path == NULL) return -1;

    /* database_get_sync consumes full_path */
    return database_get_sync(st->db, full_path, result);
}

int database_subtree_delete_sync(database_subtree_t* st, path_t* path) {
    if (st == NULL || path == NULL) return -1;

    path_t* full_path = database_subtree_prepend_path(st, path);
    path_destroy(path);
    if (full_path == NULL) return -1;

    /* database_delete_sync consumes full_path */
    return database_delete_sync(st->db, full_path);
}

int64_t database_subtree_increment_sync(database_subtree_t* st, path_t* path, int64_t delta) {
    if (st == NULL || path == NULL) return -1;

    path_t* full_path = database_subtree_prepend_path(st, path);
    path_destroy(path);
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

/* --- Async context structs --- */

typedef struct {
    database_subtree_t* st;   // Reference held for lifetime of async op
    database_t* db;
    path_t* path;
    identifier_t* value;
    promise_t* promise;
} subtree_put_ctx_t;

typedef struct {
    database_subtree_t* st;   // Reference held for lifetime of async op
    database_t* db;
    path_t* path;
    promise_t* promise;
} subtree_get_ctx_t;

typedef struct {
    database_subtree_t* st;   // Reference held for lifetime of async op
    database_t* db;
    path_t* path;
    promise_t* promise;
} subtree_delete_ctx_t;

/* --- Async worker functions --- */

static void _subtree_put_worker(void* ctx_ptr) {
    subtree_put_ctx_t* ctx = (subtree_put_ctx_t*)ctx_ptr;
    database_put(ctx->db, ctx->path, ctx->value, ctx->promise);
    database_subtree_close(ctx->st);
    free(ctx);
}

static void _subtree_get_worker(void* ctx_ptr) {
    subtree_get_ctx_t* ctx = (subtree_get_ctx_t*)ctx_ptr;
    database_get(ctx->db, ctx->path, ctx->promise);
    database_subtree_close(ctx->st);
    free(ctx);
}

static void _subtree_delete_worker(void* ctx_ptr) {
    subtree_delete_ctx_t* ctx = (subtree_delete_ctx_t*)ctx_ptr;
    database_delete(ctx->db, ctx->path, ctx->promise);
    database_subtree_close(ctx->st);
    free(ctx);
}

/* --- Async abort functions --- */

static void abort_subtree_put(void* ctx_ptr) {
    subtree_put_ctx_t* ctx = (subtree_put_ctx_t*)ctx_ptr;
    if (ctx->path) path_destroy(ctx->path);
    if (ctx->value) identifier_destroy(ctx->value);
    if (ctx->st) database_subtree_close(ctx->st);
    free(ctx);
}

static void abort_subtree_get(void* ctx_ptr) {
    subtree_get_ctx_t* ctx = (subtree_get_ctx_t*)ctx_ptr;
    if (ctx->path) path_destroy(ctx->path);
    if (ctx->st) database_subtree_close(ctx->st);
    free(ctx);
}

static void abort_subtree_delete(void* ctx_ptr) {
    subtree_delete_ctx_t* ctx = (subtree_delete_ctx_t*)ctx_ptr;
    if (ctx->path) path_destroy(ctx->path);
    if (ctx->st) database_subtree_close(ctx->st);
    free(ctx);
}

/* --- Path-based async CRUD --- */

void database_subtree_put(database_subtree_t* st, path_t* path,
                           identifier_t* value, promise_t* promise) {
    if (st == NULL || path == NULL || value == NULL || promise == NULL) {
        if (path) path_destroy(path);
        if (value) identifier_destroy(value);
        if (promise) promise_resolve(promise, NULL);
        return;
    }

    /* Prepend prefix to path; original path is consumed */
    path_t* full_path = database_subtree_prepend_path(st, path);
    path_destroy(path);
    if (full_path == NULL) {
        identifier_destroy(value);
        promise_resolve(promise, NULL);
        return;
    }

    /* Sync-only mode: call sync variant and resolve promise */
    if (st->db->sync_only) {
        int rc = database_put_sync(st->db, full_path, value);
        int* result = malloc(sizeof(int));
        if (result) *result = rc;
        promise_resolve(promise, result);
        return;
    }

    subtree_put_ctx_t* ctx = get_clear_memory(sizeof(subtree_put_ctx_t));
    if (ctx == NULL) {
        path_destroy(full_path);
        identifier_destroy(value);
        promise_resolve(promise, NULL);
        return;
    }

    /* Hold a reference on the subtree so it stays alive during async work.
     * Released in the worker function or abort handler. */
    refcounter_reference((refcounter_t*) st);
    ctx->st = st;
    ctx->db = st->db;
    ctx->path = full_path;
    ctx->value = value;
    ctx->promise = promise;

    work_t* work = work_create(
        _subtree_put_worker,
        abort_subtree_put,
        ctx);
    if (work == NULL) {
        free(ctx);
        path_destroy(full_path);
        identifier_destroy(value);
        promise_resolve(promise, NULL);
        return;
    }

    refcounter_yield((refcounter_t*) work);
    work_pool_enqueue(database_subtree_get_pool(st), work);
}

void database_subtree_get(database_subtree_t* st, path_t* path,
                           promise_t* promise) {
    if (st == NULL || path == NULL || promise == NULL) {
        if (path) path_destroy(path);
        promise_resolve(promise, NULL);
        return;
    }

    /* Prepend prefix to path; original path is consumed */
    path_t* full_path = database_subtree_prepend_path(st, path);
    path_destroy(path);
    if (full_path == NULL) {
        promise_resolve(promise, NULL);
        return;
    }

    /* Sync-only mode: call sync variant and resolve promise */
    if (st->db->sync_only) {
        identifier_t* result = NULL;
        int rc = database_get_sync(st->db, full_path, &result);
        (void)rc; /* result pointer conveys not-found vs error */
        promise_resolve(promise, result);
        return;
    }

    subtree_get_ctx_t* ctx = get_clear_memory(sizeof(subtree_get_ctx_t));
    if (ctx == NULL) {
        path_destroy(full_path);
        promise_resolve(promise, NULL);
        return;
    }

    /* Hold a reference on the subtree so it stays alive during async work. */
    refcounter_reference((refcounter_t*) st);
    ctx->st = st;
    ctx->db = st->db;
    ctx->path = full_path;
    ctx->promise = promise;

    work_t* work = work_create(
        _subtree_get_worker,
        abort_subtree_get,
        ctx);
    if (work == NULL) {
        free(ctx);
        path_destroy(full_path);
        promise_resolve(promise, NULL);
        return;
    }

    refcounter_yield((refcounter_t*) work);
    work_pool_enqueue(database_subtree_get_pool(st), work);
}

void database_subtree_delete(database_subtree_t* st, path_t* path,
                              promise_t* promise) {
    if (st == NULL || path == NULL || promise == NULL) {
        if (path) path_destroy(path);
        promise_resolve(promise, NULL);
        return;
    }

    /* Prepend prefix to path; original path is consumed */
    path_t* full_path = database_subtree_prepend_path(st, path);
    path_destroy(path);
    if (full_path == NULL) {
        promise_resolve(promise, NULL);
        return;
    }

    /* Sync-only mode: call sync variant and resolve promise */
    if (st->db->sync_only) {
        int rc = database_delete_sync(st->db, full_path);
        int* result = malloc(sizeof(int));
        if (result) *result = rc;
        promise_resolve(promise, result);
        return;
    }

    subtree_delete_ctx_t* ctx = get_clear_memory(sizeof(subtree_delete_ctx_t));
    if (ctx == NULL) {
        path_destroy(full_path);
        promise_resolve(promise, NULL);
        return;
    }

    /* Hold a reference on the subtree so it stays alive during async work. */
    refcounter_reference((refcounter_t*) st);
    ctx->st = st;
    ctx->db = st->db;
    ctx->path = full_path;
    ctx->promise = promise;

    work_t* work = work_create(
        _subtree_delete_worker,
        abort_subtree_delete,
        ctx);
    if (work == NULL) {
        free(ctx);
        path_destroy(full_path);
        promise_resolve(promise, NULL);
        return;
    }

    refcounter_yield((refcounter_t*) work);
    work_pool_enqueue(database_subtree_get_pool(st), work);
}

/* --- Raw async CRUD --- */

int database_subtree_put_raw(database_subtree_t* st,
                              const char* key, size_t key_len, char delimiter,
                              const uint8_t* value, size_t value_len,
                              promise_t* promise) {
    if (st == NULL || key == NULL || promise == NULL) {
        if (promise) promise_resolve(promise, NULL);
        return -1;
    }

    size_t prefixed_len = 0;
    char* prefixed_key = database_subtree_prepend_key(st, key, key_len, &prefixed_len);
    if (prefixed_key == NULL) {
        promise_resolve(promise, NULL);
        return -1;
    }

    /* Sync-only mode: call sync variant and resolve promise */
    if (st->db->sync_only) {
        int rc = database_put_sync_raw(st->db, prefixed_key, prefixed_len, delimiter,
                                        value, value_len);
        free(prefixed_key);
        int* result = malloc(sizeof(int));
        if (result) *result = rc;
        promise_resolve(promise, result);
        return 0;
    }

    /* Delegate to database_put_raw which handles its own async work */
    int rc = database_put_raw(st->db, prefixed_key, prefixed_len, delimiter,
                               value, value_len, promise);
    free(prefixed_key);
    return rc;
}

int database_subtree_get_raw(database_subtree_t* st,
                              const char* key, size_t key_len, char delimiter,
                              promise_t* promise) {
    if (st == NULL || key == NULL || promise == NULL) {
        if (promise) promise_resolve(promise, NULL);
        return -1;
    }

    size_t prefixed_len = 0;
    char* prefixed_key = database_subtree_prepend_key(st, key, key_len, &prefixed_len);
    if (prefixed_key == NULL) {
        promise_resolve(promise, NULL);
        return -1;
    }

    /* Sync-only mode: call sync variant and resolve promise */
    if (st->db->sync_only) {
        uint8_t* value_out = NULL;
        size_t value_len_out = 0;
        int rc = database_get_sync_raw(st->db, prefixed_key, prefixed_len, delimiter,
                                       &value_out, &value_len_out);
        free(prefixed_key);
        if (rc == 0 && value_out != NULL) {
            /* Wrap raw value in an identifier for promise resolution */
            identifier_t* id = identifier_create_from_raw(value_out, value_len_out,
                                                           st->db->chunk_size);
            free(value_out);
            promise_resolve(promise, id);
        } else {
            promise_resolve(promise, NULL);
        }
        return 0;
    }

    /* Delegate to database_get_raw which handles its own async work */
    int rc = database_get_raw(st->db, prefixed_key, prefixed_len, delimiter, promise);
    free(prefixed_key);
    return rc;
}

int database_subtree_delete_raw(database_subtree_t* st,
                                 const char* key, size_t key_len, char delimiter,
                                 promise_t* promise) {
    if (st == NULL || key == NULL || promise == NULL) {
        if (promise) promise_resolve(promise, NULL);
        return -1;
    }

    size_t prefixed_len = 0;
    char* prefixed_key = database_subtree_prepend_key(st, key, key_len, &prefixed_len);
    if (prefixed_key == NULL) {
        promise_resolve(promise, NULL);
        return -1;
    }

    /* Sync-only mode: call sync variant and resolve promise */
    if (st->db->sync_only) {
        int rc = database_delete_sync_raw(st->db, prefixed_key, prefixed_len, delimiter);
        free(prefixed_key);
        int* result = malloc(sizeof(int));
        if (result) *result = rc;
        promise_resolve(promise, result);
        return 0;
    }

    /* Delegate to database_delete_raw which handles its own async work */
    int rc = database_delete_raw(st->db, prefixed_key, prefixed_len, delimiter, promise);
    free(prefixed_key);
    return rc;
}

/* --- Batch operations --- */

int database_subtree_write_batch_sync(database_subtree_t* st, batch_t* batch) {
    if (st == NULL || batch == NULL) return -1;

    platform_lock(&batch->lock);
    size_t count = batch->count;
    uint8_t submitted = batch->submitted;

    if (count == 0 || submitted) {
        platform_unlock(&batch->lock);
        return count == 0 ? -3 : -6;
    }

    /* Snapshot the ops while holding the lock to prevent concurrent modification. */
    batch_op_t* ops_snapshot = malloc(count * sizeof(batch_op_t));
    if (ops_snapshot == NULL) {
        platform_unlock(&batch->lock);
        return -1;
    }
    memcpy(ops_snapshot, batch->ops, count * sizeof(batch_op_t));
    platform_unlock(&batch->lock);

    /* Create a new batch with prefixed paths */
    batch_t* prefixed_batch = batch_create(count);
    if (prefixed_batch == NULL) {
        free(ops_snapshot);
        return -1;
    }

    for (size_t i = 0; i < count; i++) {
        path_t* prefixed_path = database_subtree_prepend_path(st, ops_snapshot[i].path);
        if (prefixed_path == NULL) {
            batch_destroy(prefixed_batch);
            free(ops_snapshot);
            return -1;
        }

        if (ops_snapshot[i].type == WAL_PUT) {
            /* REFERENCE the value so the original batch keeps its copy */
            identifier_t* value = (identifier_t*)refcounter_reference(
                (refcounter_t*) ops_snapshot[i].value);
            int rc = batch_add_put(prefixed_batch, prefixed_path, value);
            if (rc != 0) {
                path_destroy(prefixed_path);
                identifier_destroy(value);
                batch_destroy(prefixed_batch);
                free(ops_snapshot);
                return -1;
            }
        } else {
            /* WAL_DELETE */
            int rc = batch_add_delete(prefixed_batch, prefixed_path);
            if (rc != 0) {
                path_destroy(prefixed_path);
                batch_destroy(prefixed_batch);
                free(ops_snapshot);
                return -1;
            }
        }
    }

    free(ops_snapshot);

    int rc = database_write_batch_sync(st->db, prefixed_batch);
    batch_destroy(prefixed_batch);
    return rc;
}

/* --- Async batch context --- */

typedef struct {
    database_subtree_t* st;
    batch_t* batch;
    promise_t* promise;
} subtree_batch_ctx_t;

static void _subtree_batch_worker(void* ctx_ptr) {
    subtree_batch_ctx_t* ctx = (subtree_batch_ctx_t*)ctx_ptr;
    int rc = database_subtree_write_batch_sync(ctx->st, ctx->batch);
    int* result = malloc(sizeof(int));
    if (result) *result = rc;
    promise_resolve(ctx->promise, result);
    /* batch is NOT owned by this context — it was referenced, so dereference */
    batch_destroy(ctx->batch);
    database_subtree_close(ctx->st);
    free(ctx);
}

static void abort_subtree_batch(void* ctx_ptr) {
    subtree_batch_ctx_t* ctx = (subtree_batch_ctx_t*)ctx_ptr;
    if (ctx->batch) batch_destroy(ctx->batch);
    if (ctx->st) database_subtree_close(ctx->st);
    if (ctx->promise) {
        int* error = malloc(sizeof(int));
        if (error) *error = -1;
        promise_resolve(ctx->promise, error ? error : NULL);
    }
    free(ctx);
}

void database_subtree_write_batch(database_subtree_t* st, batch_t* batch, promise_t* promise) {
    if (st == NULL || batch == NULL || promise == NULL) {
        if (promise) {
            int* error = malloc(sizeof(int));
            if (error) *error = -1;
            promise_resolve(promise, error);
        }
        return;
    }

    /* Sync-only mode: call sync variant and resolve promise */
    if (st->db->sync_only) {
        int rc = database_subtree_write_batch_sync(st, batch);
        int* result = malloc(sizeof(int));
        if (result) *result = rc;
        promise_resolve(promise, result);
        return;
    }

    /* Reference the batch so it stays alive during async work */
    refcounter_reference((refcounter_t*) batch);
    /* Reference the subtree so it stays alive during async work */
    refcounter_reference((refcounter_t*) st);

    subtree_batch_ctx_t* ctx = get_clear_memory(sizeof(subtree_batch_ctx_t));
    if (ctx == NULL) {
        batch_destroy(batch);  /* drops the reference we just took */
        database_subtree_close(st);  /* drops the reference we just took */
        int* error = malloc(sizeof(int));
        if (error) *error = -1;
        promise_resolve(promise, error ? error : NULL);
        return;
    }

    ctx->st = st;
    ctx->batch = batch;
    ctx->promise = promise;

    work_t* work = work_create(
        _subtree_batch_worker,
        abort_subtree_batch,
        ctx);
    if (work == NULL) {
        batch_destroy(batch);
        database_subtree_close(st);
        free(ctx);
        int* error = malloc(sizeof(int));
        if (error) *error = -1;
        promise_resolve(promise, error ? error : NULL);
        return;
    }

    refcounter_yield((refcounter_t*) work);
    work_pool_enqueue(database_subtree_get_pool(st), work);
}

int database_subtree_batch_sync_raw(database_subtree_t* st, char delimiter,
                                      const raw_op_t* ops, size_t count) {
    if (st == NULL || ops == NULL || count == 0) return -1;

    /* Build prefixed raw op array */
    raw_op_t* prefixed_ops = malloc(count * sizeof(raw_op_t));
    if (prefixed_ops == NULL) return -1;

    for (size_t i = 0; i < count; i++) {
        size_t prefixed_len = 0;
        char* prefixed_key = database_subtree_prepend_key(st, ops[i].key, ops[i].key_len,
                                                           &prefixed_len);
        if (prefixed_key == NULL) {
            /* Clean up already-allocated keys */
            for (size_t j = 0; j < i; j++) {
                free((void*)prefixed_ops[j].key);
            }
            free(prefixed_ops);
            return -1;
        }
        prefixed_ops[i].key = prefixed_key;
        prefixed_ops[i].key_len = prefixed_len;
        prefixed_ops[i].value = ops[i].value;
        prefixed_ops[i].value_len = ops[i].value_len;
        prefixed_ops[i].type = ops[i].type;
    }

    int rc = database_batch_sync_raw(st->db, delimiter, prefixed_ops, count);

    /* Free the prefixed keys */
    for (size_t i = 0; i < count; i++) {
        free((void*)prefixed_ops[i].key);
    }
    free(prefixed_ops);

    return rc;
}

int database_subtree_batch_raw(database_subtree_t* st, char delimiter,
                                const raw_op_t* ops, size_t count,
                                promise_t* promise) {
    if (st == NULL || ops == NULL || count == 0 || promise == NULL) return -1;

    /* Build prefixed raw op array */
    raw_op_t* prefixed_ops = malloc(count * sizeof(raw_op_t));
    if (prefixed_ops == NULL) {
        int* error = malloc(sizeof(int));
        if (error) *error = -1;
        promise_resolve(promise, error);
        return -1;
    }

    for (size_t i = 0; i < count; i++) {
        size_t prefixed_len = 0;
        char* prefixed_key = database_subtree_prepend_key(st, ops[i].key, ops[i].key_len,
                                                           &prefixed_len);
        if (prefixed_key == NULL) {
            for (size_t j = 0; j < i; j++) {
                free((void*)prefixed_ops[j].key);
            }
            free(prefixed_ops);
            int* error = malloc(sizeof(int));
            if (error) *error = -1;
            promise_resolve(promise, error);
            return -1;
        }
        prefixed_ops[i].key = prefixed_key;
        prefixed_ops[i].key_len = prefixed_len;
        prefixed_ops[i].value = ops[i].value;
        prefixed_ops[i].value_len = ops[i].value_len;
        prefixed_ops[i].type = ops[i].type;
    }

    /* Delegate to database_batch_raw which handles its own async work */
    int rc = database_batch_raw(st->db, delimiter, prefixed_ops, count, promise);

    /* Free the prefixed keys */
    for (size_t i = 0; i < count; i++) {
        free((void*)prefixed_ops[i].key);
    }
    free(prefixed_ops);

    return rc;
}

/* --- Snapshot and introspection operations --- */

int database_subtree_snapshot(database_subtree_t* st) {
    if (st == NULL) return -1;
    return database_snapshot(st->db);
}

int database_subtree_flush_dirty_bnodes(database_subtree_t* st) {
    if (st == NULL) return -1;
    return database_flush_dirty_bnodes(st->db);
}

size_t database_subtree_count(database_subtree_t* st) {
    if (st == NULL) return 0;

    /* Build the scan prefix: "prefix{delimiter}" */
    size_t scan_len = st->prefix_len + 1; /* prefix + delimiter */
    char* scan_prefix = get_memory(scan_len + 1); /* +1 for null terminator */
    if (scan_prefix == NULL) return 0;

    memcpy(scan_prefix, st->prefix, st->prefix_len);
    scan_prefix[st->prefix_len] = st->delimiter;
    scan_prefix[scan_len] = '\0';

    raw_result_t* results = NULL;
    size_t count = 0;
    int rc = database_scan_sync_raw(st->db, scan_prefix, scan_len, st->delimiter,
                                     &results, &count);
    free(scan_prefix);

    if (rc != 0) return 0;

    database_raw_results_free(results, count);
    return count;
}

/* --- Scan/Iterator operations --- */

database_iterator_t* database_subtree_scan_start(database_subtree_t* st,
                                                  path_t* start_path,
                                                  path_t* end_path) {
    if (st == NULL) {
        if (start_path) path_destroy(start_path);
        if (end_path) path_destroy(end_path);
        return NULL;
    }

    path_t* prefixed_start = NULL;
    path_t* prefixed_end = NULL;

    if (start_path != NULL) {
        prefixed_start = database_subtree_prepend_path(st, start_path);
        path_destroy(start_path);
        if (prefixed_start == NULL) {
            if (end_path) path_destroy(end_path);
            return NULL;
        }
    }

    if (end_path != NULL) {
        prefixed_end = database_subtree_prepend_path(st, end_path);
        path_destroy(end_path);
        if (prefixed_end == NULL) {
            if (prefixed_start) path_destroy(prefixed_start);
            return NULL;
        }
    }

    database_iterator_t* iter = database_scan_start(st->db, prefixed_start, prefixed_end);
    /* database_scan_start takes ownership of prefixed_start and prefixed_end */
    return iter;
}

database_iterator_t* database_subtree_scan_range(database_subtree_t* st,
                                                  const char* start,
                                                  const char* end) {
    if (st == NULL) return NULL;

    char* prefixed_start = NULL;
    char* prefixed_end = NULL;
    size_t prefixed_start_len = 0;
    size_t prefixed_end_len = 0;

    if (start != NULL) {
        prefixed_start = database_subtree_prepend_key(st, start, strlen(start),
                                                      &prefixed_start_len);
        if (prefixed_start == NULL) return NULL;
    }

    if (end != NULL) {
        prefixed_end = database_subtree_prepend_key(st, end, strlen(end),
                                                     &prefixed_end_len);
        if (prefixed_end == NULL) {
            free(prefixed_start);
            return NULL;
        }
    }

    /* database_scan_range takes C string arguments */
    database_iterator_t* iter = database_scan_range(st->db, prefixed_start, prefixed_end);

    free(prefixed_start);
    free(prefixed_end);

    return iter;
}

/**
 * Helper: count the number of path components in the subtree prefix.
 * Splits st->prefix by st->delimiter.
 */
static size_t subtree_prefix_component_count(database_subtree_t* st) {
    if (st->prefix_len == 0) return 0;
    size_t count = 1;
    for (size_t i = 0; i < st->prefix_len; i++) {
        if (st->prefix[i] == st->delimiter) count++;
    }
    return count;
}

/**
 * Helper: convert a path to a human-readable key string with the given delimiter.
 * Assembles identifier data with delimiters between components.
 *
 * @param path       Path to convert
 * @param delimiter  Delimiter character
 * @param out_len    Output: length of resulting string
 * @return Newly allocated string, or NULL on failure
 */
static char* path_to_key_string(path_t* path, char delimiter, size_t* out_len) {
    size_t len = path_length(path);
    if (len == 0) {
        char* empty = malloc(1);
        if (empty) empty[0] = '\0';
        if (out_len) *out_len = 0;
        return empty;
    }

    /* Calculate total length */
    size_t total = 0;
    for (size_t i = 0; i < len; i++) {
        identifier_t* id = path_get(path, i);
        if (id == NULL) return NULL;
        total += id->length;
        if (i > 0) total++;  /* delimiter */
    }

    char* result = malloc(total + 1);
    if (result == NULL) return NULL;

    size_t pos = 0;
    for (size_t i = 0; i < len; i++) {
        if (i > 0) result[pos++] = delimiter;
        identifier_t* id = path_get(path, i);
        size_t id_len = 0;
        uint8_t* id_data = identifier_get_data_copy(id, &id_len);
        if (id_data) {
            memcpy(result + pos, id_data, id_len);
            pos += id_len;
            free(id_data);
        }
    }
    result[pos] = '\0';

    if (out_len) *out_len = pos;
    return result;
}

/**
 * Helper: strip prefix identifiers from a path and return the remaining
 * path as a human-readable key string.
 *
 * @param st              Subtree providing the prefix
 * @param path            Full path (with prefix included)
 * @param prefix_count    Number of prefix components (from subtree_prefix_component_count)
 * @param out_len         Output: length of resulting string
 * @return Newly allocated stripped key string, or NULL on failure
 */
static char* subtree_path_to_stripped_key(database_subtree_t* st,
                                           path_t* path,
                                           size_t prefix_count,
                                           size_t* out_len) {
    size_t total_len = path_length(path);
    if (total_len < prefix_count) return NULL;

    size_t remaining = total_len - prefix_count;
    if (remaining == 0) {
        char* empty = malloc(1);
        if (empty) empty[0] = '\0';
        if (out_len) *out_len = 0;
        return empty;
    }

    /* Build a path from the remaining identifiers */
    path_t* stripped_path = path_create();
    if (stripped_path == NULL) return NULL;

    for (size_t i = prefix_count; i < total_len; i++) {
        identifier_t* id = path_get(path, i);
        if (id == NULL) {
            path_destroy(stripped_path);
            return NULL;
        }
        identifier_t* id_copy = (identifier_t*)refcounter_reference((refcounter_t*)id);
        int rc = path_append(stripped_path, id_copy);
        if (rc != 0) {
            identifier_destroy(id_copy);
            path_destroy(stripped_path);
            return NULL;
        }
        /* path_append took ownership via REFERENCE, so dereference our ref */
        refcounter_dereference((refcounter_t*)id_copy);
    }

    char* key = path_to_key_string(stripped_path, st->delimiter, out_len);
    path_destroy(stripped_path);
    return key;
}

int database_subtree_scan_sync_raw(database_subtree_t* st,
                                    const char* prefix, size_t prefix_len,
                                    char delimiter,
                                    raw_result_t** results, size_t* count) {
    if (st == NULL || results == NULL || count == NULL) return -1;

    *results = NULL;
    *count = 0;

    /* Build start path from combined prefix: st->prefix + delimiter + user_prefix */
    path_t* start_path = NULL;
    if (prefix_len > 0) {
        size_t combined_len = st->prefix_len + 1 + prefix_len;
        char* combined = malloc(combined_len + 1);
        if (combined == NULL) return -1;
        memcpy(combined, st->prefix, st->prefix_len);
        combined[st->prefix_len] = delimiter;
        memcpy(combined + st->prefix_len + 1, prefix, prefix_len);
        combined[combined_len] = '\0';

        start_path = path_create_from_raw(combined, combined_len, delimiter, st->chunk_size);
        free(combined);
        if (start_path == NULL) return -1;
    } else {
        /* Empty prefix: start from the subtree prefix itself */
        start_path = path_create_from_raw(st->prefix, st->prefix_len, delimiter, st->chunk_size);
        if (start_path == NULL) return -1;
    }

    /* database_scan_start takes ownership of start_path */
    database_iterator_t* iter = database_scan_start(st->db, start_path, NULL);
    if (iter == NULL) return 0;

    size_t prefix_count = subtree_prefix_component_count(st);

    size_t capacity = 64;
    size_t n = 0;
    raw_result_t* out = malloc(capacity * sizeof(raw_result_t));
    if (out == NULL) {
        database_scan_end(iter);
        return -1;
    }

    while (true) {
        path_t* out_path = NULL;
        identifier_t* out_value = NULL;
        int rc = database_scan_next(iter, &out_path, &out_value);
        if (rc != 0) break;

        /* Strip prefix from the path and convert to raw key */
        size_t stripped_key_len = 0;
        char* stripped_key = subtree_path_to_stripped_key(st, out_path, prefix_count,
                                                           &stripped_key_len);

        if (stripped_key != NULL && stripped_key_len > 0) {
            if (n >= capacity) {
                capacity *= 2;
                raw_result_t* new_out = realloc(out, capacity * sizeof(raw_result_t));
                if (new_out == NULL) {
                    free(stripped_key);
                    path_destroy(out_path);
                    identifier_destroy(out_value);
                    for (size_t j = 0; j < n; j++) {
                        free(out[j].key);
                        free(out[j].value);
                    }
                    free(out);
                    database_scan_end(iter);
                    return -1;
                }
                out = new_out;
            }

            out[n].key = stripped_key;
            out[n].key_len = stripped_key_len;
            size_t value_len = 0;
            uint8_t* value_data = identifier_get_data_copy(out_value, &value_len);
            if (value_data == NULL && value_len > 0) {
                /* OOM — skip this entry rather than store a NULL value */
                free(stripped_key);
                path_destroy(out_path);
                identifier_destroy(out_value);
                continue;
            }
            out[n].value = value_data;
            out[n].value_len = value_len;
            n++;
        } else {
            if (stripped_key) free(stripped_key);
        }

        path_destroy(out_path);
        identifier_destroy(out_value);
    }

    database_scan_end(iter);

    *results = out;
    *count = n;
    return 0;
}

int database_subtree_scan_range_sync_raw(database_subtree_t* st,
                                          const char* start_prefix, size_t start_len,
                                          const char* end_prefix, size_t end_len,
                                          char delimiter,
                                          raw_result_t** results, size_t* count) {
    if (st == NULL || results == NULL || count == NULL) return -1;

    *results = NULL;
    *count = 0;

    /* Build start and end paths with subtree prefix prepended */
    path_t* start_path = NULL;
    path_t* end_path = NULL;

    if (start_prefix != NULL && start_len > 0) {
        size_t prefixed_start_len = 0;
        char* prefixed_start = database_subtree_prepend_key(st, start_prefix, start_len,
                                                             &prefixed_start_len);
        if (prefixed_start == NULL) return -1;
        start_path = path_create_from_raw(prefixed_start, prefixed_start_len,
                                          delimiter, st->chunk_size);
        free(prefixed_start);
        if (start_path == NULL) return -1;
    }

    if (end_prefix != NULL && end_len > 0) {
        size_t prefixed_end_len = 0;
        char* prefixed_end = database_subtree_prepend_key(st, end_prefix, end_len,
                                                           &prefixed_end_len);
        if (prefixed_end == NULL) {
            if (start_path) path_destroy(start_path);
            return -1;
        }
        end_path = path_create_from_raw(prefixed_end, prefixed_end_len,
                                        delimiter, st->chunk_size);
        free(prefixed_end);
        if (end_path == NULL) {
            if (start_path) path_destroy(start_path);
            return -1;
        }
    }

    /* database_scan_start takes ownership of start_path and end_path */
    database_iterator_t* iter = database_scan_start(st->db, start_path, end_path);
    if (iter == NULL) return 0;

    size_t prefix_count = subtree_prefix_component_count(st);

    size_t capacity = 64;
    size_t n = 0;
    raw_result_t* out = malloc(capacity * sizeof(raw_result_t));
    if (out == NULL) {
        database_scan_end(iter);
        return -1;
    }

    while (true) {
        path_t* out_path = NULL;
        identifier_t* out_value = NULL;
        int rc = database_scan_next(iter, &out_path, &out_value);
        if (rc != 0) break;

        /* Strip prefix from the path and convert to raw key */
        size_t stripped_key_len = 0;
        char* stripped_key = subtree_path_to_stripped_key(st, out_path, prefix_count,
                                                           &stripped_key_len);

        if (stripped_key != NULL && stripped_key_len > 0) {
            if (n >= capacity) {
                capacity *= 2;
                raw_result_t* new_out = realloc(out, capacity * sizeof(raw_result_t));
                if (new_out == NULL) {
                    free(stripped_key);
                    path_destroy(out_path);
                    identifier_destroy(out_value);
                    for (size_t j = 0; j < n; j++) {
                        free(out[j].key);
                        free(out[j].value);
                    }
                    free(out);
                    database_scan_end(iter);
                    return -1;
                }
                out = new_out;
            }

            out[n].key = stripped_key;
            out[n].key_len = stripped_key_len;
            size_t value_len = 0;
            uint8_t* value_data = identifier_get_data_copy(out_value, &value_len);
            if (value_data == NULL && value_len > 0) {
                /* OOM — skip this entry rather than store a NULL value */
                free(stripped_key);
                path_destroy(out_path);
                identifier_destroy(out_value);
                continue;
            }
            out[n].value = value_data;
            out[n].value_len = value_len;
            n++;
        } else {
            if (stripped_key) free(stripped_key);
        }

        path_destroy(out_path);
        identifier_destroy(out_value);
    }

    database_scan_end(iter);

    *results = out;
    *count = n;
    return 0;
}