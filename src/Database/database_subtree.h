//
// database_subtree.h - Subtree view over a database with prefix isolation
//
// A subtree provides a scoped view into a database, automatically prepending
// a prefix to all keys. This enables namespace isolation and hierarchical
// data partitioning.
//

#ifndef WAVEDB_DATABASE_SUBTREE_H
#define WAVEDB_DATABASE_SUBTREE_H

#include <stdint.h>
#include <stddef.h>
#include "../RefCounter/refcounter.h"
#include "database.h"
#include "../HBTrie/path.h"
#include "../Workers/promise.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * database_subtree_t - A scoped view into a database with prefix isolation.
 *
 * All operations on a subtree automatically prepend the prefix to keys,
 * providing namespace isolation. The subtree holds a reference on the
 * database, preventing it from being destroyed while any subtree is open.
 * Closing a subtree releases that reference; the database is only torn
 * down when its reference count reaches zero.
 */
typedef struct {
    refcounter_t refcounter;     // MUST be first member
    database_t* db;              // Referenced — subtree keeps db alive
    char* prefix;                // Key prefix (owned copy)
    size_t prefix_len;           // Length of prefix string
    char delimiter;              // Path delimiter character
    uint8_t chunk_size;          // Chunk size copied from db
    bool destroying;             // True once destruction begins
} database_subtree_t;

/**
 * Open a subtree view on a database.
 *
 * Creates a subtree that prepends the given prefix to all keys.
 * The subtree holds a reference on the database, keeping it alive
 * until all subtrees and other references are released.
 *
 * @param db        Database to operate on
 * @param prefix    Key prefix string (e.g. "layer/graphql")
 * @param delimiter Path delimiter character (e.g. '/')
 * @return New subtree or NULL on failure
 */
database_subtree_t* database_subtree_open(database_t* db,
                                           const char* prefix,
                                           char delimiter);

/**
 * Close a subtree view.
 *
 * Decrements the reference count. When count reaches 0, frees the prefix,
 * the subtree struct, and releases the reference held on the database.
 * The database is only destroyed when its own reference count reaches zero.
 *
 * @param subtree  Subtree to close
 */
void database_subtree_close(database_subtree_t* subtree);

/**
 * Delete all keys under a prefix from the database.
 *
 * Scans for all keys matching "prefix{delimiter}*" and deletes each one.
 * This is a convenience function that does not require an open subtree.
 *
 * @param db        Database to modify
 * @param prefix    Key prefix to delete under
 * @param delimiter Path delimiter character
 * @return 0 on success, -1 on failure
 */
int database_subtree_delete_prefix(database_t* db,
                                    const char* prefix,
                                    char delimiter);

/**
 * Get the database associated with a subtree.
 *
 * @param st  Subtree to query
 * @return Database pointer
 */
database_t* database_subtree_get_db(database_subtree_t* st);

/**
 * Get the work pool from the subtree's database.
 *
 * @param st  Subtree to query
 * @return Work pool pointer
 */
work_pool_t* database_subtree_get_pool(database_subtree_t* st);

/**
 * Prepend the subtree prefix to a path.
 *
 * Creates a new path: prefix_path + original identifiers.
 * Caller owns the returned path and must destroy it.
 *
 * @param st    Subtree providing the prefix
 * @param path  Original path (not consumed)
 * @return New path with prefix prepended, or NULL on failure
 */
path_t* database_subtree_prepend_path(database_subtree_t* st, path_t* path);

/**
 * Prepend the subtree prefix to a raw key string.
 *
 * Constructs "prefix{delimiter}key" in a newly allocated buffer.
 * Caller must free the returned string.
 *
 * @param st       Subtree providing the prefix
 * @param key      Raw key bytes
 * @param key_len  Length of key
 * @param out_len  Output: total length of the resulting string
 * @return Newly allocated string with prefixed key, or NULL on failure
 */
char* database_subtree_prepend_key(database_subtree_t* st,
                                    const char* key, size_t key_len,
                                    size_t* out_len);

/* --- Path-based sync CRUD --- */

/**
 * Synchronously put a value into the subtree.
 *
 * Prepends the subtree prefix to the path and delegates to database_put_sync.
 * Ownership of path and value is consumed by the underlying call.
 *
 * @param st     Subtree to operate on
 * @param path   Path key (ownership transferred to underlying database_put_sync)
 * @param value  Value to store (ownership transferred to underlying database_put_sync)
 * @return 0 on success, -1 on error
 */
int database_subtree_put_sync(database_subtree_t* st, path_t* path, identifier_t* value);

/**
 * Synchronously get a value from the subtree.
 *
 * Prepends the subtree prefix to the path and delegates to database_get_sync.
 * Ownership of path is consumed by the underlying call.
 *
 * @param st      Subtree to query
 * @param path    Path key (ownership transferred to underlying database_get_sync)
 * @param result  Output: found value (caller must destroy) or NULL if not found
 * @return 0 on success (result found), -1 on error, -2 on not found
 */
int database_subtree_get_sync(database_subtree_t* st, path_t* path, identifier_t** result);

/**
 * Synchronously delete a value from the subtree.
 *
 * Prepends the subtree prefix to the path and delegates to database_delete_sync.
 * Ownership of path is consumed by the underlying call.
 *
 * @param st    Subtree to modify
 * @param path  Path key (ownership transferred to underlying database_delete_sync)
 * @return 0 on success, -1 on error
 */
int database_subtree_delete_sync(database_subtree_t* st, path_t* path);

/**
 * Atomically increment a numeric value in the subtree.
 *
 * Prepends the subtree prefix to the path and delegates to database_increment_sync.
 * Ownership of path is consumed by the underlying call.
 *
 * @param st     Subtree to modify
 * @param path   Path key (ownership transferred to underlying database_increment_sync)
 * @param delta  Amount to increment by
 * @return New value after increment, or -1 on error
 */
int64_t database_subtree_increment_sync(database_subtree_t* st, path_t* path, int64_t delta);

/* --- Raw (byte) sync CRUD --- */

/**
 * Synchronously put a raw key-value pair into the subtree.
 *
 * Prepends the subtree prefix to the key and delegates to database_put_sync_raw.
 *
 * @param st         Subtree to operate on
 * @param key        Raw key bytes
 * @param key_len    Length of key
 * @param delimiter  Path delimiter character
 * @param value      Value bytes to store
 * @param value_len  Length of value
 * @return 0 on success, -1 on error
 */
int database_subtree_put_sync_raw(database_subtree_t* st,
                                   const char* key, size_t key_len, char delimiter,
                                   const uint8_t* value, size_t value_len);

/**
 * Synchronously get a raw value from the subtree.
 *
 * Prepends the subtree prefix to the key and delegates to database_get_sync_raw.
 * The returned value does NOT contain the prefix (values are not prefixed).
 *
 * @param st             Subtree to query
 * @param key            Raw key bytes
 * @param key_len        Length of key
 * @param delimiter      Path delimiter character
 * @param value_out      Output: allocated value bytes (caller must free with database_raw_value_free)
 * @param value_len_out  Output: length of value
 * @return 0 on success (value found), -1 on error, -2 on not found
 */
int database_subtree_get_sync_raw(database_subtree_t* st,
                                   const char* key, size_t key_len, char delimiter,
                                   uint8_t** value_out, size_t* value_len_out);

/**
 * Synchronously delete a raw key from the subtree.
 *
 * Prepends the subtree prefix to the key and delegates to database_delete_sync_raw.
 *
 * @param st         Subtree to modify
 * @param key        Raw key bytes
 * @param key_len    Length of key
 * @param delimiter  Path delimiter character
 * @return 0 on success, -1 on error
 */
int database_subtree_delete_sync_raw(database_subtree_t* st,
                                      const char* key, size_t key_len, char delimiter);

/* --- Path-based async CRUD --- */

/**
 * Asynchronously put a value into the subtree.
 *
 * Prepends the subtree prefix to the path and delegates to database_put.
 * Takes ownership of path and value on all code paths.
 *
 * @param st       Subtree to operate on
 * @param path     Path key (ownership transferred)
 * @param value    Value to store (ownership transferred)
 * @param promise  Promise to resolve on completion
 */
void database_subtree_put(database_subtree_t* st, path_t* path,
                           identifier_t* value, promise_t* promise);

/**
 * Asynchronously get a value from the subtree.
 *
 * Prepends the subtree prefix to the path and delegates to database_get.
 * Takes ownership of path on all code paths.
 * Resolves promise with identifier_t* (caller must destroy) or NULL if not found.
 *
 * @param st       Subtree to query
 * @param path     Path key (ownership transferred)
 * @param promise  Promise to resolve with value
 */
void database_subtree_get(database_subtree_t* st, path_t* path,
                           promise_t* promise);

/**
 * Asynchronously delete a value from the subtree.
 *
 * Prepends the subtree prefix to the path and delegates to database_delete.
 * Takes ownership of path on all code paths.
 *
 * @param st       Subtree to modify
 * @param path     Path key (ownership transferred)
 * @param promise  Promise to resolve on completion
 */
void database_subtree_delete(database_subtree_t* st, path_t* path,
                              promise_t* promise);

/* --- Raw async CRUD --- */

/**
 * Asynchronously put a raw key-value pair into the subtree.
 *
 * Prepends the subtree prefix to the key and delegates to database_put_raw.
 *
 * @param st         Subtree to operate on
 * @param key        Raw key bytes
 * @param key_len    Length of key
 * @param delimiter  Path delimiter character
 * @param value      Value bytes to store
 * @param value_len  Length of value
 * @param promise    Promise to resolve on completion
 * @return 0 on success (work enqueued), -1 on error
 */
int database_subtree_put_raw(database_subtree_t* st,
                              const char* key, size_t key_len, char delimiter,
                              const uint8_t* value, size_t value_len,
                              promise_t* promise);

/**
 * Asynchronously get a raw value from the subtree.
 *
 * Prepends the subtree prefix to the key and delegates to database_get_raw.
 *
 * @param st         Subtree to query
 * @param key        Raw key bytes
 * @param key_len    Length of key
 * @param delimiter  Path delimiter character
 * @param promise    Promise to resolve with value
 * @return 0 on success (work enqueued), -1 on error
 */
int database_subtree_get_raw(database_subtree_t* st,
                              const char* key, size_t key_len, char delimiter,
                              promise_t* promise);

/**
 * Asynchronously delete a raw key from the subtree.
 *
 * Prepends the subtree prefix to the key and delegates to database_delete_raw.
 *
 * @param st         Subtree to modify
 * @param key        Raw key bytes
 * @param key_len    Length of key
 * @param delimiter  Path delimiter character
 * @param promise    Promise to resolve on completion
 * @return 0 on success (work enqueued), -1 on error
 */
int database_subtree_delete_raw(database_subtree_t* st,
                                 const char* key, size_t key_len, char delimiter,
                                 promise_t* promise);

/* --- Batch operations --- */

/**
 * Synchronously submit a batch of write operations to the subtree.
 *
 * Each entry's path is prefixed with the subtree prefix before submission.
 * The original batch is NOT consumed; a new temporary batch is created
 * with prefixed paths and submitted, then destroyed.
 *
 * @param st     Subtree to operate on
 * @param batch  Batch of operations (not consumed)
 * @return 0 on success, error code on failure
 */
int database_subtree_write_batch_sync(database_subtree_t* st, batch_t* batch);

/**
 * Asynchronously submit a batch of write operations to the subtree.
 *
 * Each entry's path is prefixed with the subtree prefix before submission.
 * Resolves promise with int* result (0 on success, error code on failure).
 *
 * @param st      Subtree to operate on
 * @param batch   Batch of operations (not consumed)
 * @param promise Promise to resolve on completion
 */
void database_subtree_write_batch(database_subtree_t* st, batch_t* batch, promise_t* promise);

/**
 * Synchronously submit a batch of raw operations to the subtree.
 *
 * Each key is prefixed with "st->prefix{delimiter}key" before submission.
 *
 * @param st         Subtree to operate on
 * @param delimiter  Path delimiter character
 * @param ops        Array of raw operations
 * @param count      Number of operations
 * @return 0 on success, error code on failure
 */
int database_subtree_batch_sync_raw(database_subtree_t* st, char delimiter,
                                     const raw_op_t* ops, size_t count);

/**
 * Asynchronously submit a batch of raw operations to the subtree.
 *
 * Each key is prefixed with "st->prefix{delimiter}key" before submission.
 *
 * @param st         Subtree to operate on
 * @param delimiter  Path delimiter character
 * @param ops        Array of raw operations
 * @param count      Number of operations
 * @param promise    Promise to resolve on completion
 * @return 0 on success (work enqueued), -1 on error
 */
int database_subtree_batch_raw(database_subtree_t* st, char delimiter,
                                const raw_op_t* ops, size_t count,
                                promise_t* promise);

/* --- Snapshot and introspection operations --- */

/**
 * Snapshot the subtree's underlying database.
 *
 * Delegates to database_snapshot on the underlying database.
 *
 * @param st  Subtree to snapshot
 * @return 0 on success, -1 on error
 */
int database_subtree_snapshot(database_subtree_t* st);

/**
 * Flush dirty bnodes for the subtree's underlying database.
 *
 * Delegates to database_flush_dirty_bnodes on the underlying database.
 *
 * @param st  Subtree to flush
 * @return 0 on success, -1 on error
 */
int database_subtree_flush_dirty_bnodes(database_subtree_t* st);

/**
 * Count the number of entries under the subtree's prefix.
 *
 * Scans all keys matching "prefix{delimiter}*" and returns the count.
 * The scan results are freed internally; only the count is returned.
 *
 * @param st  Subtree to count entries in
 * @return Number of entries, or 0 on error
 */
size_t database_subtree_count(database_subtree_t* st);

/* --- Scan/Iterator operations --- */

/**
 * Start a scan over the subtree using path bounds.
 *
 * Prepends the subtree prefix to start_path and end_path, then
 * delegates to database_scan_start. The returned iterator walks the
 * underlying database; result paths include the subtree prefix.
 *
 * @param st          Subtree to scan
 * @param start_path  Optional start bound (NULL = beginning). Ownership transferred.
 * @param end_path    Optional end bound (NULL = no upper bound). Ownership transferred.
 * @return Iterator handle, or NULL on failure
 */
database_iterator_t* database_subtree_scan_start(database_subtree_t* st,
                                                  path_t* start_path,
                                                  path_t* end_path);

/**
 * Start a scan over the subtree using string-based path bounds.
 *
 * Prepends the subtree prefix to start and end strings, then
 * delegates to database_scan_range.
 *
 * @param st     Subtree to scan
 * @param start  Start path string (NULL = beginning)
 * @param end    End path string (NULL = no upper bound)
 * @return Iterator handle, or NULL on failure
 */
database_iterator_t* database_subtree_scan_range(database_subtree_t* st,
                                                  const char* start,
                                                  const char* end);

/**
 * Synchronously scan the subtree for keys matching a prefix.
 *
 * Builds the combined prefix (st->prefix + delimiter + user_prefix),
 * scans the underlying database, then strips the subtree prefix
 * from each result key.
 *
 * Caller must free results with database_raw_results_free.
 *
 * @param st           Subtree to scan
 * @param prefix       Key prefix to match (within subtree namespace)
 * @param prefix_len   Length of prefix
 * @param delimiter    Path delimiter character
 * @param results      Output: array of results (caller must free)
 * @param count        Output: number of results
 * @return 0 on success, -1 on error
 */
int database_subtree_scan_sync_raw(database_subtree_t* st,
                                    const char* prefix, size_t prefix_len,
                                    char delimiter,
                                    raw_result_t** results, size_t* count);

/**
 * Synchronously scan the subtree for keys in a range.
 *
 * Builds prefixed start and end bounds, scans the underlying database,
 * then strips the subtree prefix from each result key.
 *
 * Caller must free results with database_raw_results_free.
 *
 * @param st              Subtree to scan
 * @param start_prefix    Start prefix (within subtree namespace)
 * @param start_len       Length of start prefix
 * @param end_prefix      End prefix (within subtree namespace)
 * @param end_len         Length of end prefix
 * @param delimiter       Path delimiter character
 * @param results         Output: array of results (caller must free)
 * @param count           Output: number of results
 * @return 0 on success, -1 on error
 */
int database_subtree_scan_range_sync_raw(database_subtree_t* st,
                                          const char* start_prefix, size_t start_len,
                                          const char* end_prefix, size_t end_len,
                                          char delimiter,
                                          raw_result_t** results, size_t* count);

#ifdef __cplusplus
}
#endif

#endif // WAVEDB_DATABASE_SUBTREE_H