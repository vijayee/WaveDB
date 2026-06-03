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

#ifdef __cplusplus
extern "C" {
#endif

/**
 * database_subtree_t - A scoped view into a database with prefix isolation.
 *
 * All operations on a subtree automatically prepend the prefix to keys,
 * providing namespace isolation. The subtree does NOT own the database;
 * closing a subtree does not destroy the underlying database.
 */
typedef struct {
    refcounter_t refcounter;     // MUST be first member
    database_t* db;              // Borrowed reference to the database
    char* prefix;                // Key prefix (owned copy)
    size_t prefix_len;           // Length of prefix string
    char delimiter;              // Path delimiter character
    uint8_t chunk_size;          // Chunk size copied from db
} database_subtree_t;

/**
 * Open a subtree view on a database.
 *
 * Creates a subtree that prepends the given prefix to all keys.
 * The database is NOT referenced — the caller must ensure db outlives
 * the subtree.
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
 * Decrements the reference count. When count reaches 0, frees the prefix
 * and the subtree struct. Does NOT destroy or dereference the database.
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
int database_subtree_delete(database_t* db,
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

#ifdef __cplusplus
}
#endif

#endif // WAVEDB_DATABASE_SUBTREE_H