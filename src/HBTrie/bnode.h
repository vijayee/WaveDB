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
#include "../Workers/transaction_id.h"
#include "chunk.h"
#include "identifier.h"

#ifdef __cplusplus
extern "C" {
#endif

// Forward declaration
struct hbtrie_node_t;

/**
 * version_entry_t - Version metadata for MVCC.
 *
 * Each entry stores multiple versions as a doubly-linked list.
 * Newest versions are at the front of the chain.
 */
typedef struct version_entry_t {
    refcounter_t refcounter;           // MUST be first member
    transaction_id_t txn_id;            // Transaction that created this version
    identifier_t* value;                // Value for this version (or NULL if deleted)
    uint8_t is_deleted;                 // Tombstone marker (1 if deleted)
    struct version_entry_t* next;       // Newer version (NULL if newest)
    struct version_entry_t* prev;      // Older version (NULL if oldest)
} version_entry_t;

/**
 * bnode_entry_t - Entry in a B+tree node.
 *
 * Each entry maps a single chunk to either a child HBTrie node or a leaf value.
 * When has_value == 1 (leaf entry), path_chunk_counts stores the number of chunks
 * per identifier in the path, enabling path reconstruction during iteration.
 */
typedef struct bnode_entry_t {
    chunk_t* key;                      // Single chunk for comparison
    union {
        struct hbtrie_node_t* child;   // Next HBTrie node (if has_value == 0)
        identifier_t* value;            // Leaf value (if has_value == 1 and has_versions == 0)
        version_entry_t* versions;      // Version chain (if has_value == 1 and has_versions == 1)
    };
    uint8_t has_value;                  // 0 = child node, 1 = value or versions
    uint8_t has_versions;               // 1 if version chain present, 0 for legacy single value

    // Transaction ID for legacy mode (valid when has_value == 1 and has_versions == 0)
    transaction_id_t value_txn_id;

    // Path metadata for iteration (valid when has_value == 1)
    // Stores the number of chunks per identifier in the path
    // NULL for entries inserted before this field was added (treat as single identifier)
    // Example: path ['users', 'alice'] with chunk_size=4 might have chunk_counts = [2, 2]
    //          meaning 'users' has 2 chunks, 'alice' has 2 chunks
    vec_t(size_t) path_chunk_counts;

    // Storage location for lazy-loaded children
    // Valid only when has_value == 0 and child pointer is loaded from disk
    size_t child_section_id;           // Section where child is stored
    size_t child_block_index;           // Block index within section
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
 * Calculate the approximate size of a node in bytes.
 *
 * @param node  Node to measure
 * @param chunk_size  Size of each chunk in bytes
 * @return Approximate size in bytes
 */
size_t bnode_size(bnode_t* node, uint8_t chunk_size);

/**
 * Check if node needs to be split (exceeds size limit).
 *
 * @param node  Node to check
 * @param chunk_size  Size of each chunk in bytes
 * @return 1 if needs split, 0 otherwise
 */
int bnode_needs_split(bnode_t* node, uint8_t chunk_size);

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

// ============================================================================
// MVCC Version Chain Functions
// ============================================================================

/**
 * Create a version entry (refcounted).
 *
 * @param txn_id      Transaction ID for this version
 * @param value       Value for this version (takes ownership of reference)
 * @param is_deleted  1 if this is a tombstone (deletion marker), 0 otherwise
 * @return New version entry or NULL on failure
 */
version_entry_t* version_entry_create(transaction_id_t txn_id,
                                       identifier_t* value,
                                       uint8_t is_deleted);

/**
 * Destroy a version entry (dereference).
 *
 * @param entry  Version entry to destroy
 */
void version_entry_destroy(version_entry_t* entry);

/**
 * Find the visible version for a transaction.
 *
 * Walks the version chain to find the newest version that is visible
 * to the given transaction (txn_id <= read_txn_id and not deleted before read).
 *
 * @param versions      Version chain head
 * @param read_txn_id   Transaction ID for visibility check
 * @return Visible version entry, or NULL if not visible/deleted
 */
version_entry_t* version_entry_find_visible(version_entry_t* versions,
                                             transaction_id_t read_txn_id);

/**
 * Add a new version to the chain.
 *
 * Creates a new version entry and inserts it at the front of the chain.
 * The chain is ordered newest-first for O(1) access to recent versions.
 *
 * @param versions      Pointer to version chain head (updated to new head)
 * @param txn_id        Transaction ID for the new version
 * @param value         Value for the new version (takes ownership of reference)
 * @param is_deleted    1 if this is a tombstone, 0 otherwise
 * @return 0 on success, -1 on failure
 */
int version_entry_add(version_entry_t** versions,
                      transaction_id_t txn_id,
                      identifier_t* value,
                      uint8_t is_deleted);

/**
 * Garbage collect old versions from a version chain.
 *
 * Removes versions older than min_active_txn_id.
 * Always keeps at least one version (newest committed).
 *
 * @param versions           Pointer to version chain head
 * @param min_active_txn_id  Oldest transaction ID that may still be active
 * @return Number of versions removed
 */
size_t version_entry_gc(version_entry_t** versions, transaction_id_t min_active_txn_id);

// ============================================================================
// Path Chunk Counts Functions
// ============================================================================

/**
 * Set path chunk counts on a bnode entry.
 *
 * Initializes the path_chunk_counts vector with the number of chunks
 * per identifier in the path. Used during insertion to enable
 * path reconstruction during iteration.
 *
 * @param entry      Entry to modify (must have has_value == 1)
 * @param counts     Array of chunk counts (one per identifier)
 * @param count      Number of identifiers
 * @return 0 on success, -1 on failure
 */
int bnode_entry_set_path_chunk_counts(bnode_entry_t* entry,
                                       const size_t* counts,
                                       size_t count);

/**
 * Get path chunk counts from a bnode entry.
 *
 * Returns the stored chunk counts per identifier, or NULL if not set.
 * For entries inserted before path metadata was added, returns NULL.
 *
 * @param entry      Entry to query
 * @param out_count  Output: number of identifiers (size of array)
 * @return Pointer to chunk counts array, or NULL if not set
 */
const size_t* bnode_entry_get_path_chunk_counts(const bnode_entry_t* entry,
                                                size_t* out_count);

#ifdef __cplusplus
}
#endif

#endif // WAVEDB_BNODE_H