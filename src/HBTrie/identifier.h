//
// Created by victor on 3/11/26.
//

#ifndef WAVEDB_IDENTIFIER_H
#define WAVEDB_IDENTIFIER_H

#include <stdint.h>
#include <stddef.h>
#include "../RefCounter/refcounter.h"
#include "../Util/vec.h"
#include "chunk.h"
#include <cbor.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * identifier_t - Variable-length sequence of chunks.
 *
 * An identifier represents both keys (path components) and values
 * in the HBTrie structure. Data is split into fixed-size chunks
 * for hierarchical trie traversal.
 *
 * Number of chunks: nchunk = (length - 1) / chunk_size + 1
 * Last chunk size:  length - (nchunk - 1) * chunk_size
 */
typedef struct {
    refcounter_t refcounter;     // MUST be first member
    vec_t(chunk_t*) chunks;      // Variable number of chunks
    size_t length;               // Original data length in bytes
    size_t chunk_size;           // Chunk size used for this identifier
} identifier_t;

/**
 * Calculate number of chunks needed for a given length.
 *
 * @param length      Data length in bytes
 * @param chunk_size  Size of each chunk
 * @return Number of chunks required
 */
static inline size_t identifier_calc_nchunk(size_t length, size_t chunk_size) {
    if (length == 0) return 1;
    return (length - 1) / chunk_size + 1;
}

/**
 * Calculate last chunk size for a given length.
 *
 * @param length      Data length in bytes
 * @param chunk_size  Size of each chunk
 * @return Size of the last chunk (may be less than chunk_size)
 */
static inline size_t identifier_calc_last_chunk_size(size_t length, size_t chunk_size) {
    if (length == 0) return chunk_size;
    size_t remainder = length % chunk_size;
    return remainder == 0 ? chunk_size : remainder;
}

/**
 * Create an identifier from a buffer.
 *
 * @param buf        Source buffer (takes ownership of buffer data)
 * @param chunk_size Size of each chunk (use DEFAULT_CHUNK_SIZE if 0)
 * @return New identifier_t or NULL on failure
 */
identifier_t* identifier_create(buffer_t* buf, size_t chunk_size);

/**
 * Create an identifier directly from raw bytes, skipping the buffer_t intermediate.
 *
 * Saves 2 allocations + 2 frees + 1 memcpy compared to creating a buffer_t
 * first and then calling identifier_create().
 *
 * @param data        Raw byte data (may be NULL if len is 0)
 * @param len         Length of data in bytes
 * @param chunk_size  Size of each chunk (use DEFAULT_CHUNK_SIZE if 0)
 * @return New identifier_t or NULL on failure
 */
identifier_t* identifier_create_from_raw(const uint8_t* data, size_t len, size_t chunk_size);

/**
 * Get a contiguous copy of the identifier's data.
 *
 * Allocates and returns a malloc'd buffer containing the original byte data
 * reconstructed from all chunks. Caller must free() the returned pointer.
 *
 * @param id       Identifier to get data from
 * @param out_len  Output: length of data in bytes
 * @return Pointer to malloc'd data buffer, or NULL on failure. Caller must free().
 */
uint8_t* identifier_get_data_copy(const identifier_t* id, size_t* out_len);

/**
 * Create an empty identifier.
 *
 * @param chunk_size Size of each chunk (use DEFAULT_CHUNK_SIZE if 0)
 * @return New empty identifier_t or NULL on failure
 */
identifier_t* identifier_create_empty(size_t chunk_size);

/**
 * Destroy an identifier.
 *
 * @param id  Identifier to destroy
 */
void identifier_destroy(identifier_t* id);

/**
 * Compare two identifiers chunk by chunk.
 *
 * @param a   First identifier
 * @param b   Second identifier
 * @return <0 if a < b, 0 if a == b, >0 if a > b
 */
int identifier_compare(identifier_t* a, identifier_t* b);

/**
 * Get a specific chunk from an identifier.
 *
 * @param id    Identifier to get chunk from
 * @param index Chunk index (0-based)
 * @return Chunk pointer or NULL if out of bounds
 */
chunk_t* identifier_get_chunk(identifier_t* id, size_t index);

/**
 * Get the number of chunks in an identifier.
 *
 * @param id  Identifier
 * @return Number of chunks
 */
size_t identifier_chunk_count(identifier_t* id);

/**
 * Reconstruct original data from identifier into a buffer.
 *
 * @param id  Identifier to reconstruct
 * @return New buffer containing original data, or NULL on failure
 */
buffer_t* identifier_to_buffer(identifier_t* id);

/**
 * Get raw data pointer and length from an identifier.
 *
 * Reconstructs the original byte data by concatenating all chunks.
 * The returned pointer is valid only while the identifier is alive.
 * Caller must NOT free the returned pointer (it points into a heap buffer).
 *
 * @param id       Identifier to get data from
 * @param out_len  Output: length of data in bytes
 * @return Pointer to data, or NULL on failure. Caller must free the returned buffer.
 */
uint8_t* identifier_get_data(identifier_t* id, size_t* out_len);

/**
 * Serialize an identifier to CBOR.
 *
 * Format: single byte string with original data
 * bstr(original_data)
 *
 * @param id  Identifier to serialize
 * @return CBOR item or NULL on failure
 */
cbor_item_t* identifier_to_cbor(identifier_t* id);

/**
 * Deserialize an identifier from CBOR.
 *
 * Accepts new format (single bytestring) and deprecated format (array of chunk bytestrings).
 *
 * @param item  CBOR item (bytestring or array of bytestrings)
 * @param chunk_size  Chunk size to use
 * @return New identifier or NULL on failure
 */
identifier_t* cbor_to_identifier(cbor_item_t* item, size_t chunk_size);

#ifdef __cplusplus
}
#endif

#endif // WAVEDB_IDENTIFIER_H