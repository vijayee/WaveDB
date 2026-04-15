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
    uint64_t offset;     // File offset in page file (0 if not on disk)
} node_location_t;

/**
 * Serialize a B+tree node to a binary buffer.
 *
 * Format (V2, magic 0xB4):
 *   - Magic byte (0xB4 for V2 with inline child bnodes)
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
 *     - If !has_value and is_bnode_child (V2 inline):
 *       - Child bnode size (uint32_t)
 *       - Child bnode data (recursively serialized bnode)
 *     - If !has_value and !is_bnode_child:
 *       - child_disk_offset (uint64_t)
 *       - reserved (uint64_t, was block_index)
 *
 * Legacy format (V1, magic 0xB3):
 *   Same as V2 except is_bnode_child entries use child_disk_offset + reserved
 *   instead of inline child bnode data.
 *
 * Old format (no magic byte):
 *   - num_entries (uint16_t)
 *   - For each entry: chunk + has_value + value/section data
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

#define BNODE_SERIALIZE_MAGIC_V3 0xB5  // V3: flat per-bnode with file_offset references

/**
 * Serialize a B+tree node to a V3 binary buffer.
 *
 * V3 format (magic 0xB5): flat per-bnode with file_offset references.
 * Same as V2 for values and inline child bnodes, but non-inline children
 * are stored as 8-byte file offsets (child_disk_offset) instead of
 * child_disk_offset + reserved pairs.
 *
 * @param node        B+tree node to serialize
 * @param chunk_size  Size of each chunk in bytes
 * @param buf         Output: allocated buffer (caller must free)
 * @param len         Output: buffer length
 * @return 0 on success, -1 on failure
 */
int bnode_serialize_v3(bnode_t* node, uint8_t chunk_size, uint8_t** buf, size_t* len);

/**
 * Deserialize a B+tree node from a V3 binary buffer.
 *
 * Reads V3 format. For non-value, non-inline entries, reads child_disk_offset
 * (8 bytes) and stores in entry->child_disk_offset, leaving child/child_bnode
 * as NULL for lazy loading. Populates node_location_t array with offsets.
 *
 * @param buf             Binary buffer
 * @param len             Buffer length
 * @param chunk_size      Size of each chunk in bytes
 * @param btree_node_size Max B+tree node size
 * @param locations       Array of child locations (output)
 * @param num_locations   Output: number of child locations
 * @return New B+tree node or NULL on failure
 */
bnode_t* bnode_deserialize_v3(uint8_t* buf, size_t len, uint8_t chunk_size,
                               uint32_t btree_node_size,
                               node_location_t** locations, size_t* num_locations);

#ifdef __cplusplus
}
#endif

#endif // WAVEDB_NODE_SERIALIZER_H