//
// Created by victor on 3/11/26.
//

#ifndef WAVEDB_CHUNK_H
#define WAVEDB_CHUNK_H

#include <stdint.h>
#include <stddef.h>
#include "../RefCounter/refcounter.h"
#include "../Buffer/buffer.h"

#ifdef __cplusplus
extern "C" {
#endif

// Default chunk size in bytes (configurable per HBTrie instance)
#define DEFAULT_CHUNK_SIZE 4

/**
 * chunk_t - Fixed-size buffer for HBTrie key comparison.
 *
 * A chunk is the atomic unit of comparison in the HBTrie.
 * Each chunk holds exactly chunk_size bytes.
 *
 * Keys are split into chunks for hierarchical trie traversal:
 * - chunk[0] compared at root B+tree
 * - chunk[1] compared at next level B+tree
 * - etc.
 *
 * Data is stored inline (no buffer_t indirection) for cache-friendly
 * access during comparisons. Reference-counted for efficient sharing
 * via chunk_share().
 */
typedef struct {
    refcounter_t refcounter;     // MUST be first member (for refcounting)
    size_t size;                  // Data size in bytes
    uint8_t data[];               // Inline data (flexible array member)
} chunk_t;

/**
 * Create a chunk from raw data.
 *
 * @param data   Raw data pointer (must be at least chunk_size bytes)
 * @param chunk_size  Number of bytes to copy
 * @return New chunk_t or NULL on failure
 */
chunk_t* chunk_create(const void* data, size_t chunk_size);

/**
 * Create a chunk from a buffer.
 *
 * @param buf    Source buffer (chunk_size bytes copied)
 * @param chunk_size  Number of bytes to copy from buffer
 * @return New chunk_t or NULL on failure
 */
chunk_t* chunk_create_from_buffer(buffer_t* buf, size_t chunk_size);

/**
 * Create an empty chunk (zero-initialized).
 *
 * @param chunk_size  Size in bytes
 * @return New chunk_t or NULL on failure
 */
chunk_t* chunk_create_empty(size_t chunk_size);

/**
 * Destroy a chunk.
 *
 * Decrements reference count. Frees memory when count reaches 0.
 *
 * @param chunk  Chunk to destroy
 */
void chunk_destroy(chunk_t* chunk);

/**
 * Create a chunk that shares data with another chunk.
 *
 * Returns the same pointer with incremented reference count.
 * Call chunk_destroy() on the result when done (decrements refcount).
 *
 * @param chunk  Chunk to share
 * @return Same chunk pointer with incremented refcount, or NULL on failure
 */
chunk_t* chunk_share(chunk_t* chunk);

/**
 * Compare two chunks.
 *
 * @param a   First chunk
 * @param b   Second chunk
 * @return <0 if a < b, 0 if a == b, >0 if a > b
 */
int chunk_compare(chunk_t* a, chunk_t* b);

/**
 * Get data pointer from chunk.
 *
 * @param chunk  Chunk to get data from
 * @return Pointer to chunk data
 */
void* chunk_data(chunk_t* chunk);

/**
 * Get chunk data as const pointer.
 *
 * @param chunk  Chunk to get data from
 * @return Const pointer to chunk data
 */
const void* chunk_data_const(const chunk_t* chunk);

#ifdef __cplusplus
}
#endif

#endif // WAVEDB_CHUNK_H