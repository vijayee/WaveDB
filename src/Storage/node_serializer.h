//
// Created by victor on 03/13/26.
//

#ifndef WAVEDB_NODE_SERIALIZER_H
#define WAVEDB_NODE_SERIALIZER_H

#include <stdint.h>
#include <stddef.h>
#include "../HBTrie/bnode.h"
#include "../Buffer/buffer.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * node_location_t - On-disk location of a serialized node.
 *
 * Used to track where each HBTrie node is stored on disk.
 * When a parent node references a child, it stores the child's location
 * instead of the child data directly (for lazy loading).
 */
typedef struct {
    size_t section_id;    // Which section file contains the node
    size_t block_index;   // Which block within the section
} node_location_t;

/**
 * Serialize a B+tree node to a binary buffer.
 *
 * Format:
 *   - Magic byte (0xB3 for new format with level/is_bnode_child)
 *   - Level (uint16_t)
 *   - Number of entries (uint16_t)
 *   - For each entry:
 *     - Chunk data (chunk_size bytes)
 *     - Flags byte: bit 0 = has_value, bit 1 = is_bnode_child, bit 2 = has_versions
 *     - If has_value and has_versions:
 *       - Number of versions (uint16_t)
 *       - For each version:
 *         - is_deleted (uint8_t)
 *         - txn_id: time (uint64_t), nanos (uint64_t), count (uint64_t)
 *         - If not deleted: identifier length (uint32_t) + data
 *     - If has_value and !has_versions:
 *       - Identifier length (uint32_t) + data
 *     - If !has_value and is_bnode_child:
 *       - child_level (uint16_t) placeholder for recursive bnode
 *     - If !has_value and !is_bnode_child:
 *       - section_id (uint64_t)
 *       - block_index (uint64_t)
 *
 * @param node     B+tree node to serialize
 * @param chunk_size Size of each chunk in bytes
 * @param buf      Output: allocated buffer (caller must free)
 * @param len      Output: buffer length
 * @return 0 on success, -1 on failure
 */
int bnode_serialize(bnode_t* node, uint8_t chunk_size, uint8_t** buf, size_t* len);

/**
 * Deserialize a B+tree node from a binary buffer.
 *
 * Supports both old format (starting with uint16_t num_entries)
 * and new format (starting with magic byte 0xB3).
 *
 * @param buf        Binary buffer
 * @param len        Buffer length
 * @param chunk_size Size of each chunk in bytes
 * @param btree_node_size Max B+tree node size
 * @param locations  Array of child locations (output)
 * @param num_locations Output: number of child locations
 * @return New B+tree node or NULL on failure
 */
bnode_t* bnode_deserialize(uint8_t* buf, size_t len, uint8_t chunk_size,
                           uint32_t btree_node_size,
                           node_location_t** locations, size_t* num_locations);

/**
 * Serialize an identifier to a binary buffer.
 *
 * Format:
 *   - Length (uint32_t)
 *   - Data (length bytes)
 *
 * @param ident  Identifier to serialize
 * @param buf    Output: allocated buffer (caller must free)
 * @param len    Output: buffer length
 * @return 0 on success, -1 on failure
 */
int identifier_serialize(identifier_t* ident, uint8_t** buf, size_t* len);

/**
 * Deserialize an identifier from a binary buffer.
 *
 * @param buf        Binary buffer
 * @param len        Buffer length
 * @param chunk_size Size of each chunk in bytes
 * @return New identifier or NULL on failure
 */
identifier_t* identifier_deserialize(uint8_t* buf, size_t len, size_t chunk_size);

/**
 * Serialize a chunk to a binary buffer.
 *
 * Format:
 *   - Data (chunk_size bytes, no length prefix)
 *
 * @param chunk      Chunk to serialize
 * @param chunk_size Size of each chunk in bytes
 * @param buf        Output: allocated buffer (caller must free)
 * @return 0 on success, -1 on failure
 */
int chunk_serialize(chunk_t* chunk, uint8_t chunk_size, uint8_t** buf);

/**
 * Deserialize a chunk from a binary buffer.
 *
 * @param buf        Binary buffer
 * @param chunk_size Size of each chunk in bytes
 * @return New chunk or NULL on failure
 */
chunk_t* chunk_deserialize(uint8_t* buf, uint8_t chunk_size);

#ifdef __cplusplus
}
#endif

#endif // WAVEDB_NODE_SERIALIZER_H