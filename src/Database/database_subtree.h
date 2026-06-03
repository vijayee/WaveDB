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

#ifdef __cplusplus
}
#endif

#endif // WAVEDB_DATABASE_SUBTREE_H