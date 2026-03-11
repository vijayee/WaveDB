//
// Created by victor on 3/11/26.
//

#ifndef WAVEDB_BNODE_H
#define WAVEDB_BNODE_H

#include <stdint.h>
#include <stddef.h>
#include "../RefCounter/refcounter.h"
#include "../Util/vec.h"
#include "../Util/threadding.h"
#include "chunk.h"
#include "identifier.h"

#ifdef __cplusplus
extern "C" {
#endif

// Forward declaration
struct hbtrie_node_t;

/**
 * bnode_entry_t - Entry in a B+tree node.
 *
 * Each entry maps a single chunk to either a child HBTrie node or a leaf value.
 */
typedef struct bnode_entry_t {
    chunk_t* key;                      // Single chunk for comparison
    union {
        struct hbtrie_node_t* child;   // Next HBTrie node (if has_value == 0)
        identifier_t* value;            // Leaf value (if has_value == 1)
    };
    uint8_t has_value;                  // 0 = child node, 1 = value
} bnode_entry_t;

/**
 * bnode_t - B+tree node for HBTrie.
 *
 * Contains a sorted array of entries, each comparing chunks.
 */
typedef struct bnode_t {
    refcounter_t refcounter;          // MUST be first member
    PLATFORMLOCKTYPE(lock);           // Node-level lock

    uint32_t node_size;               // Configurable max size in bytes

    vec_t(bnode_entry_t) entries;     // Sorted by chunk key
} bnode_t;

/**
 * Create a B+tree node.
 *
 * @param node_size  Maximum node size in bytes (0 for default)
 * @return New node or NULL on failure
 */
bnode_t* bnode_create(uint32_t node_size);

/**
 * Destroy a B+tree node.
 *
 * @param node  Node to destroy
 */
void bnode_destroy(bnode_t* node);

/**
 * Find an entry by key (chunk).
 *
 * @param node  Node to search
 * @param key   Chunk key to find
 * @param out_index  Output: index where found or would be inserted
 * @return Entry if found, NULL if not found
 */
bnode_entry_t* bnode_find(bnode_t* node, chunk_t* key, size_t* out_index);

/**
 * Insert an entry in sorted order.
 *
 * @param node   Node to insert into
 * @param entry  Entry to insert (key will be referenced)
 * @return 0 on success, -1 on failure
 */
int bnode_insert(bnode_t* node, bnode_entry_t* entry);

/**
 * Remove an entry by key.
 *
 * @param node  Node to remove from
 * @param key   Chunk key to remove
 * @return Removed entry (copy), or NULL if not found
 */
bnode_entry_t bnode_remove(bnode_t* node, chunk_t* key);

/**
 * Remove an entry at a specific index.
 *
 * @param node   Node to remove from
 * @param index  Index to remove
 * @return Removed entry (copy)
 */
bnode_entry_t bnode_remove_at(bnode_t* node, size_t index);

/**
 * Get entry at index.
 *
 * @param node   Node to get from
 * @param index  Index
 * @return Entry pointer, or NULL if out of bounds
 */
bnode_entry_t* bnode_get(bnode_t* node, size_t index);

/**
 * Get the number of entries.
 *
 * @param node  Node to query
 * @return Number of entries
 */
size_t bnode_count(bnode_t* node);

/**
 * Check if node is empty.
 *
 * @param node  Node to query
 * @return true if empty, false otherwise
 */
int bnode_is_empty(bnode_t* node);

/**
 * Split a node into two when it exceeds max size.
 *
 * @param node      Node to split
 * @param right_out  Output: new right node
 * @param split_key  Output: key to promote to parent
 * @return 0 on success, -1 on failure
 */
int bnode_split(bnode_t* node, bnode_t** right_out, chunk_t** split_key);

/**
 * Compare two entries by key.
 *
 * @param a   First entry
 * @param b   Second entry (or chunk key directly)
 * @return <0 if a < b, 0 if a == b, >0 if a > b
 */
int bnode_entry_compare(bnode_entry_t* a, chunk_t* key);

#ifdef __cplusplus
}
#endif

#endif // WAVEDB_BNODE_H