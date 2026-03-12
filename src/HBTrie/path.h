//
// Created by victor on 3/11/26.
//

#ifndef WAVEDB_PATH_H
#define WAVEDB_PATH_H

#include <stdint.h>
#include <stddef.h>
#include "../RefCounter/refcounter.h"
#include "../Util/vec.h"
#include "identifier.h"
#include <cbor.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * path_t - Sequence of identifiers forming a hierarchical key.
 *
 * Represents a MUMPS-style key path like ^global(subscript1,subscript2,...).
 * Each subscript is an identifier_t, and the path is traversed
 * through the HBTrie one identifier at a time.
 */
typedef struct {
    refcounter_t refcounter;     // MUST be first member
    vec_t(identifier_t*) identifiers; // Path components
} path_t;

/**
 * Create an empty path.
 *
 * @return New empty path or NULL on failure
 */
path_t* path_create(void);

/**
 * Create a path from a single identifier.
 *
 * @param id  First identifier (takes ownership of reference)
 * @return New path or NULL on failure
 */
path_t* path_create_from_identifier(identifier_t* id);

/**
 * Destroy a path.
 *
 * @param path  Path to destroy
 */
void path_destroy(path_t* path);

/**
 * Add an identifier to the end of a path.
 *
 * @param path  Path to modify
 * @param id    Identifier to add (takes ownership of reference)
 * @return 0 on success, -1 on failure
 */
int path_append(path_t* path, identifier_t* id);

/**
 * Get identifier at index.
 *
 * @param path   Path to query
 * @param index  Index (0-based)
 * @return Identifier at index, or NULL if out of bounds
 */
identifier_t* path_get(path_t* path, size_t index);

/**
 * Get number of identifiers in path.
 *
 * @param path  Path to query
 * @return Number of identifiers
 */
size_t path_length(path_t* path);

/**
 * Check if path is empty.
 *
 * @param path  Path to query
 * @return true if empty, false otherwise
 */
int path_is_empty(path_t* path);

/**
 * Create a copy of a path (references all identifiers).
 *
 * @param path  Path to copy
 * @return New path or NULL on failure
 */
path_t* path_copy(path_t* path);

/**
 * Compare two paths.
 *
 * @param a  First path
 * @param b  Second path
 * @return <0 if a < b, 0 if a == b, >0 if a > b
 */
int path_compare(path_t* a, path_t* b);

/**
 * Serialize a path to CBOR.
 *
 * Format: array of identifier arrays
 * [[chunk0, chunk1, ...], [chunk0, chunk1, ...], ...]
 *
 * @param path  Path to serialize
 * @return CBOR item or NULL on failure
 */
cbor_item_t* path_to_cbor(path_t* path);

/**
 * Deserialize a path from CBOR.
 *
 * @param item        CBOR item (array of arrays)
 * @param chunk_size  Chunk size to use for identifiers
 * @return New path or NULL on failure
 */
path_t* cbor_to_path(cbor_item_t* item, size_t chunk_size);

#ifdef __cplusplus
}
#endif

#endif // WAVEDB_PATH_H