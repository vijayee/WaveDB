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

// Maximum key bytes stored inline in bnode_entry_t.
// Keys with size <= BNODE_INLINE_KEY_SIZE are stored directly in the entry,
// eliminating pointer chasing during binary search.
// Covers DEFAULT_CHUNK_SIZE=4 and chunk sizes up to 8.
#define BNODE_INLINE_KEY_SIZE 8

#ifdef __cplusplus
extern "C" {
#endif

// Forward declarations
struct hbtrie_node_t;
struct bnode_t;

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
 * Each entry maps a single chunk to one of:
 * - A child HBTrie node (has_value==0, is_bnode_child==0): trie descent
 * - A child bnode (has_value==0, is_bnode_child==1): internal B+tree descent
 * - A leaf value or version chain (has_value==1): stored data
 *
 * Key data is stored inline for fast comparison during binary search.
 * When key_len <= BNODE_INLINE_KEY_SIZE, the key bytes are in key_data[]
 * and no pointer dereference is needed during comparison.
 * The key field always holds a valid chunk_t* reference for non-comparison
 * purposes (serialization, iteration, splitting).
 *
 * When has_value == 1 (leaf entry), path_chunk_counts stores the number of chunks
 * per identifier in the path, enabling path reconstruction during iteration.
 */
typedef struct bnode_entry_t {
    // Hot fields: accessed during binary search and MVCC visibility checks
    uint8_t key_data[BNODE_INLINE_KEY_SIZE]; // Inline key data for fast comparison
    union {
        struct hbtrie_node_t* child;   // Next HBTrie node (if has_value == 0, is_bnode_child == 0)
        struct bnode_t* child_bnode;   // Child B+tree node (if has_value == 0, is_bnode_child == 1)
        identifier_t* value;            // Leaf value (if has_value == 1 and has_versions == 0)
        version_entry_t* versions;      // Version chain (if has_value == 1 and has_versions == 1)
    };
    chunk_t* key;                      // Chunk reference (always valid, for non-comparison uses)
    uint8_t key_len;                   // Key length: 0=no key, 1-8=inline data valid, >8=use key ptr
    uint8_t has_value;                  // 0 = child node, 1 = value or versions
    uint8_t has_versions;               // 1 if version chain present, 0 for legacy single value
    uint8_t is_bnode_child;             // 1 when entry points to child bnode (internal B+tree node)

    // Cold fields: accessed during iteration, serialization, and lazy loading
    // Transaction ID for legacy mode (valid when has_value == 1 and has_versions == 0)
    transaction_id_t value_txn_id;

    // Path metadata for iteration (valid when has_value == 1)
    // Stores the number of chunks per identifier in the path
    // NULL for entries inserted before this field was added (treat as single identifier)
    // Example: path ['users', 'alice'] with chunk_size=4 might have chunk_counts = [2, 2]
    //         meaning 'users' has 2 chunks, 'alice' has 2 chunks
    vec_t(size_t) path_chunk_counts;

    // Child HBTrie node for entries that ALSO have a value.
    // When has_value==1 and a longer key shares this entry's chunk prefix,
    // the value stays in the union but the child hbtrie node goes here.
    // When has_value==0, use the union's child field instead.
    struct hbtrie_node_t* trie_child;

    // Storage location for lazy-loaded children (Phase 2: page file offset)
    uint64_t child_disk_offset;        // File offset of child bnode or hbtrie_node root (0 = not on disk)
} bnode_entry_t;

/**
 * bnode_t - B+tree node for HBTrie.
 *
 * Contains a sorted array of entries, each comparing chunks.
 * Level 1 = leaf node (entries point to values or HBTrie children).
 * Level > 1 = internal node (entries point to child bnodes for B+tree fanout).
 *
 * Field order optimized for cache-line efficiency:
 * - Hot fields (level, entries) placed first for binary search access
 * - Cold fields (seq, write_lock) placed after for write-path only access
 */
typedef struct bnode_t {
    refcounter_t refcounter;          // MUST be first member (16-48 bytes)
    _Atomic(uint16_t) level;           // B+tree level: 1 = leaf, > 1 = internal
    uint32_t node_size;                // Configurable max size in bytes
    vec_t(bnode_entry_t) entries;      // Sorted by chunk key
    _Atomic(uint64_t) seq;             // Seqlock: even=stable, odd=writing
    PLATFORMLOCKTYPE(write_lock);       // Writer mutual exclusion

    // Per-bnode disk tracking (Phase 2: flat per-bnode persistence)
    uint64_t disk_offset;              // File offset of this bnode (UINT64_MAX if not persisted)
    uint8_t is_dirty;                  // 1 if modified since last write
} bnode_t;

/**
 * Create a B+tree node.
 *
 * @param node_size  Maximum node size in bytes (0 for default)
 * @return New node or NULL on failure
 */
bnode_t* bnode_create(uint32_t node_size);

/**
 * Create a B+tree node with a specific level.
 *
 * @param node_size  Maximum node size in bytes (0 for default)
 * @param level      B+tree level (1 = leaf, > 1 = internal)
 * @return New node or NULL on failure
 */
bnode_t* bnode_create_with_level(uint32_t node_size, uint16_t level);

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

/**
 * Get the minimum key from a B+tree node.
 *
 * @param node  Node to get key from
 * @return First key in the node (borrowed reference, do not free)
 */
chunk_t* bnode_get_min_key(bnode_t* node);

// ============================================================================
// Inline Key Management Functions
// ============================================================================

/**
 * Set the key on a bnode entry, copying data inline if possible.
 *
 * Copies key data into key_data[] if key->size <= BNODE_INLINE_KEY_SIZE.
 * Always stores a reference to the chunk_t via chunk_share().
 *
 * @param entry  Entry to set key on
 * @param key    Chunk key (shared via chunk_share)
 */
void bnode_entry_set_key(bnode_entry_t* entry, chunk_t* key);

/**
 * Get the chunk_t key reference from a bnode entry.
 *
 * Always returns the chunk_t* reference, regardless of whether
 * the key data is stored inline or not.
 *
 * @param entry  Entry to get key from
 * @return Chunk key reference
 */
chunk_t* bnode_entry_get_key(bnode_entry_t* entry);

/**
 * Destroy the key reference on a bnode entry.
 *
 * Calls chunk_destroy on the key reference. Does not modify key_data[].
 *
 * @param entry  Entry to destroy key on
 */
void bnode_entry_destroy_key(bnode_entry_t* entry);

/**
 * Insert a child pointer entry into a B+tree node.
 * Used during split propagation to add new child nodes.
 *
 * @param parent      Parent node to insert into
 * @param key         Key for the entry (will be shared, not owned)
 * @param child       Child HBTrie node pointer
 * @return 0 on success, -1 on failure
 */
int bnode_insert_child(bnode_t* parent, chunk_t* key, struct hbtrie_node_t* child);

/**
 * Insert a child bnode pointer into an internal B+tree node.
 * Used during B+tree split propagation within an hbtrie_node.
 *
 * @param parent      Internal node to insert into
 * @param key         Separator key (will be shared, not owned)
 * @param child       Child bnode pointer
 * @return 0 on success, -1 on failure
 */
int bnode_insert_bnode_child(bnode_t* parent, chunk_t* key, struct bnode_t* child);

/**
 * Descend from root through internal B+tree nodes to reach the leaf node
 * containing the given key.
 *
 * At each internal node (level > 1), uses bnode_find to locate the
 * correct child entry. For non-exact matches, follows the entry at
 * index-1 (or index 0 if key < all entries).
 *
 * @param root  Root bnode of the B+tree (can be internal or leaf)
 * @param key   Chunk key to search for
 * @return Leaf bnode where the key would reside
 */
bnode_t* bnode_descend(bnode_t* root, chunk_t* key);

/**
 * Find an entry in a multi-level B+tree by descending to the leaf
 * and performing an exact match.
 *
 * @param root       Root bnode of the B+tree
 * @param key        Chunk key to find
 * @param out_index  Output: index in the leaf node (can be NULL)
 * @return Entry if found, NULL if not found
 */
bnode_entry_t* bnode_find_leaf(bnode_t* root, chunk_t* key, size_t* out_index);

/**
 * Recursively destroy a bnode tree, including all internal child bnodes.
 * For leaf bnodes (level == 1), behaves like bnode_destroy.
 * For internal bnodes (level > 1), recursively destroys child bnodes
 * where is_bnode_child == 1.
 *
 * @param root  Root bnode to destroy
 */
void bnode_destroy_tree(bnode_t* root);

/**
 * Check if a bnode tree has any leaf entries.
 *
 * A bnode tree is considered empty if no leaf bnode has any entries.
 * For a single-level bnode (level == 1), this is equivalent to bnode_is_empty.
 * For multi-level bnodes, walks through all internal nodes to find leaf entries.
 *
 * @param root  Root bnode of the B+tree
 * @return 1 if the tree has no leaf entries, 0 otherwise
 */
int bnode_tree_is_empty(bnode_t* root);

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