//
// Database Iterator for HBTrie traversal
//

#ifndef WAVEDB_DATABASE_ITERATOR_H
#define WAVEDB_DATABASE_ITERATOR_H

#include <stdint.h>
#include <stddef.h>
#include "../HBTrie/hbtrie.h"
#include "../Workers/transaction_id.h"
#include "database.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Iterator stack frame - tracks position in HBTrie traversal
 */
typedef struct {
    hbtrie_node_t* node;      // Current HBTrie node
    size_t entry_index;        // Current entry index in B+tree
    size_t path_index;         // Index in path for this frame
} iterator_frame_t;

/**
 * database_iterator_t - Iterator for scanning database entries
 *
 * Performs depth-first traversal of HBTrie, yielding (path, value) pairs.
 * Supports optional start and end path bounds.
 */
typedef struct {
    database_t* db;                    // Database being iterated
    path_t* start_path;                // Optional start bound (NULL = beginning)
    path_t* end_path;                  // Optional end bound (NULL = no upper bound)
    path_t* current_path;              // Current path during traversal

    // Stack for depth-first traversal
    iterator_frame_t* stack;           // Stack of frames
    size_t stack_size;                 // Allocated size
    size_t stack_depth;                // Current depth

    transaction_id_t read_txn_id;      // Transaction ID for visibility check
    uint8_t finished;                  // 1 when iteration complete
} database_iterator_t;

/**
 * Start a database scan.
 *
 * Creates an iterator for iterating over all entries in the database.
 * Optionally bounds the scan to a range of paths.
 *
 * @param db         Database to scan
 * @param start_path Optional start path (NULL = beginning). Takes ownership.
 * @param end_path   Optional end path (NULL = no upper bound). Takes ownership.
 * @return Iterator handle, or NULL on failure
 */
database_iterator_t* database_scan_start(database_t* db,
                                          path_t* start_path,
                                          path_t* end_path);

/**
 * Get next entry from iterator.
 *
 * Returns the next (path, value) pair in the iteration.
 * Caller takes ownership of returned path and identifier (must destroy them).
 *
 * @param iter      Iterator handle
 * @param out_path  Output: path key (caller must destroy)
 * @param out_value Output: value (caller must destroy)
 * @return 0 on success, -1 on end of iteration, -2 on error
 */
int database_scan_next(database_iterator_t* iter,
                        path_t** out_path,
                        identifier_t** out_value);

/**
 * End a database scan and free the iterator.
 *
 * @param iter  Iterator to destroy
 */
void database_scan_end(database_iterator_t* iter);

#ifdef __cplusplus
}
#endif

#endif // WAVEDB_DATABASE_ITERATOR_H