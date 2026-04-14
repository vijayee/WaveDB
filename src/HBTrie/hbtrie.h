//
// Created by victor on 3/11/26.
//

#ifndef WAVEDB_HBTRIE_H
#define WAVEDB_HBTRIE_H

#include <stdint.h>
#include <stddef.h>
#include <stdatomic.h>
#include "../RefCounter/refcounter.h"
#include "../Util/threadding.h"
#include "chunk.h"
#include "identifier.h"
#include "path.h"
#include "bnode.h"
#include <cbor.h>

#ifdef __cplusplus
extern "C" {
#endif

// Forward declarations (for circular dependency with mvcc.h)
struct txn_desc_t;
typedef struct txn_desc_t txn_desc_t;
typedef struct txn_desc_t txn_desc_t;

// Note: mvcc.h is NOT included here to avoid circular dependency
// mvcc.h includes this file (hbtrie.h)

/**
 * hbtrie_node_t - HBTrie node containing a B+tree for chunk comparison.
 *
 * Each HBTrie node represents one "level" in the path traversal.
 * The B+tree at each node compares chunks of the current identifier.
 * When a path component (identifier) is fully traversed, we move to
 * the next identifier in the path via the child pointer.
 */
typedef struct hbtrie_node_t {
    refcounter_t refcounter;          // MUST be first member
    _Atomic(uint64_t) seq;            // Seqlock: even=stable, odd=writing
    PLATFORMLOCKTYPE(write_lock);      // Writer mutual exclusion

    bnode_t* btree;                   // Root bnode of multi-level B+tree at this level
    uint16_t btree_height;           // Height of B+tree (1 = single leaf, > 1 = has internal nodes)

    // Storage location tracking for incremental persistence
    struct sections_t* storage;       // Storage system (NULL if in-memory only)
    size_t section_id;                // Section where this node is stored
    size_t block_index;               // Block index within section
    size_t data_size;                 // Serialized size in section (0 if not in section)
    uint8_t is_loaded;                // 1 if in memory, 0 if on-disk stub
    uint8_t is_dirty;                 // 1 if modified since last save
} hbtrie_node_t;

/**
 * hbtrie_t - Top-level HBTrie structure.
 */
typedef struct hbtrie_t {
    refcounter_t refcounter;          // MUST be first member

    uint8_t chunk_size;               // Configurable chunk size (default: DEFAULT_CHUNK_SIZE)
    uint32_t btree_node_size;         // Max B+tree node size in bytes

    _Atomic(hbtrie_node_t*) root;     // Root HBTrie node (atomic for lock-free reads)
} hbtrie_t;

/**
 * hbtrie_cursor_frame_t - Stack frame for DFS traversal of HBTrie.
 */
typedef struct {
    hbtrie_node_t* node;              // Current HBTrie node at this level
    size_t entry_index;               // Current entry index in the B+tree
} hbtrie_cursor_frame_t;

#define HBTRIE_CURSOR_MAX_DEPTH 32

/**
 * hbtrie_cursor_t - Cursor for HBTrie traversal.
 *
 * Performs depth-first traversal yielding entries with values.
 * Maintains a stack for backtracking from child nodes to parents.
 */
typedef struct {
    hbtrie_t* trie;                    // The HBTrie being traversed
    hbtrie_cursor_frame_t stack[HBTRIE_CURSOR_MAX_DEPTH];
    size_t stack_depth;                // Current depth in the trie
    int finished;                      // 1 when traversal is complete
} hbtrie_cursor_t;

/**
 * Create an HBTrie.
 *
 * @param chunk_size      Size of each chunk in bytes (0 for default)
 * @param btree_node_size Max B+tree node size in bytes (0 for default)
 * @return New HBTrie or NULL on failure
 */
hbtrie_t* hbtrie_create(uint8_t chunk_size, uint32_t btree_node_size);

/**
 * Destroy an HBTrie.
 *
 * @param trie  HBTrie to destroy
 */
void hbtrie_destroy(hbtrie_t* trie);

/**
 * Copy an HBTrie.
 *
 * Creates a deep copy of the trie structure. Chunks are shared by reference
 * (treated as immutable). Identifiers (values) are also shared by reference.
 *
 * @param trie  HBTrie to copy
 * @return New HBTrie copy or NULL on failure
 */
hbtrie_t* hbtrie_copy(hbtrie_t* trie);

/**
 * Create an HBTrie node.
 *
 * @param btree_node_size  Max B+tree node size in bytes
 * @return New HBTrie node or NULL on failure
 */
hbtrie_node_t* hbtrie_node_create(uint32_t btree_node_size);

/**
 * Destroy an HBTrie node.
 *
 * @param node  Node to destroy
 */
void hbtrie_node_destroy(hbtrie_node_t* node);

/**
 * Copy an HBTrie node recursively.
 *
 * Creates a deep copy of the node structure. Chunks are shared by reference
 * (treated as immutable). Identifiers (values) are also shared by reference.
 *
 * @param node  Node to copy
 * @return New node copy or NULL on failure
 */
hbtrie_node_t* hbtrie_node_copy(hbtrie_node_t* node);

/**
 * Initialize a cursor for DFS traversal of the HBTrie.
 *
 * @param cursor  Cursor to initialize
 * @param trie    HBTrie to traverse
 * @param path    Optional path for future seek support (currently unused)
 */
void hbtrie_cursor_init(hbtrie_cursor_t* cursor, hbtrie_t* trie, path_t* path);

/**
 * Create and initialize a heap-allocated cursor.
 *
 * @param trie    HBTrie to traverse
 * @param path    Optional path for seek (currently unused)
 * @return New cursor, or NULL on failure
 */
hbtrie_cursor_t* hbtrie_cursor_create(hbtrie_t* trie, path_t* path);

/**
 * Destroy a heap-allocated cursor.
 *
 * @param cursor  Cursor to destroy
 */
void hbtrie_cursor_destroy(hbtrie_cursor_t* cursor);

/**
 * Advance cursor to the next entry with a value (DFS traversal).
 *
 * Skips internal entries (no value), descends into child nodes,
 * and backtracks when a level is exhausted.
 *
 * @param cursor  Cursor to advance
 * @return 0 on success, -1 at end of traversal
 */
int hbtrie_cursor_next(hbtrie_cursor_t* cursor);

/**
 * Check if cursor has finished traversal.
 *
 * @param cursor  Cursor to check
 * @return 1 if at end, 0 otherwise
 */
int hbtrie_cursor_at_end(hbtrie_cursor_t* cursor);

/**
 * Get current entry at cursor position.
 *
 * Valid after a successful hbtrie_cursor_next() call.
 *
 * @param cursor  Cursor position
 * @return Current bnode entry, or NULL if none
 */
bnode_entry_t* hbtrie_cursor_get_entry(hbtrie_cursor_t* cursor);

/**
 * Get current node at cursor position.
 *
 * @param cursor  Cursor position
 * @return Current hbtrie node, or NULL if none
 */
hbtrie_node_t* hbtrie_cursor_get_node(hbtrie_cursor_t* cursor);

/**
 * Get current HBTrie node at cursor position.
 *
 * @param cursor  Cursor position
 * @return Current node, or NULL if none
 */
hbtrie_node_t* hbtrie_cursor_get_node(hbtrie_cursor_t* cursor);

/**
 * Serialize an HBTrie to CBOR.
 *
 * Format: map with keys:
 *   - "chunk_size": uint
 *   - "btree_node_size": uint
 *   - "root": serialized root node (or null)
 *
 * @param trie  HBTrie to serialize
 * @return CBOR item or NULL on failure
 */
cbor_item_t* hbtrie_to_cbor(hbtrie_t* trie);

/**
 * Deserialize an HBTrie from CBOR.
 *
 * @param item  CBOR item (map with trie data)
 * @return New HBTrie or NULL on failure
 */
hbtrie_t* cbor_to_hbtrie(cbor_item_t* item);

/**
 * Compute CRC32/XXH32 hash of HBTrie serialized form.
 *
 * Used for index file checksums.
 *
 * @param trie  HBTrie to hash
 * @return 32-bit hash value
 */
uint32_t hbtrie_compute_hash(hbtrie_t* trie);

/**
 * Serialize an HBTrie to a binary buffer.
 *
 * @param trie  HBTrie to serialize
 * @param buf   Output buffer (allocated by function, caller must free)
 * @param len   Output length
 * @return 0 on success, -1 on failure
 */
int hbtrie_serialize(hbtrie_t* trie, uint8_t** buf, size_t* len);

/**
 * Deserialize an HBTrie from a binary buffer.
 *
 * @param buf            Binary buffer
 * @param len            Buffer length
 * @param chunk_size     HBTrie chunk size
 * @param btree_node_size B+tree node size
 * @return New HBTrie or NULL on failure
 */
hbtrie_t* hbtrie_deserialize(uint8_t* buf, size_t len, uint8_t chunk_size, uint32_t btree_node_size);

/**
 * Find a value in the HBTrie using MVCC visibility rules.
 *
 * Traverses the path and returns the version visible to the given transaction ID.
 *
 * @param trie          HBTrie to search
 * @param path          Path key to find
 * @param read_txn_id   Transaction ID for visibility check
 * @return Value if found and visible, NULL if not found or deleted
 */
identifier_t* hbtrie_find(hbtrie_t* trie, path_t* path, transaction_id_t read_txn_id);

/**
 * Find a value using a transaction descriptor.
 *
 * Convenience wrapper that uses the transaction's ID.
 *
 * @param trie  HBTrie to search
 * @param path  Path key to find
 * @param txn   Transaction descriptor
 * @return Value if found and visible, NULL if not found or deleted
 */
identifier_t* hbtrie_find_with_txn(hbtrie_t* trie, path_t* path, txn_desc_t* txn);

/**
 * Insert a value into the HBTrie.
 *
 * Creates a new version for the given transaction ID.
 *
 * @param trie    HBTrie to insert into
 * @param path    Path key (sequence of identifiers)
 * @param value   Value to store (takes ownership of reference)
 * @param txn_id  Transaction ID for this write
 * @return 0 on success, -1 on failure
 */
int hbtrie_insert(hbtrie_t* trie, path_t* path, identifier_t* value, transaction_id_t txn_id);

/**
 * Delete a value from the HBTrie.
 *
 * Creates a tombstone version for the given transaction ID.
 *
 * @param trie    HBTrie to remove from
 * @param path    Path key to remove
 * @param txn_id  Transaction ID for this delete
 * @return Removed value (last visible), or NULL if not found
 */
identifier_t* hbtrie_delete(hbtrie_t* trie, path_t* path, transaction_id_t txn_id);

/**
 * Garbage collect old versions in the trie.
 *
 * Removes versions older than min_active_txn_id from all entries.
 *
 * @param trie               HBTrie to clean
 * @param min_active_txn_id  Oldest transaction ID that may still be active
 * @return Total number of versions removed
 */
size_t hbtrie_gc(hbtrie_t* trie, transaction_id_t min_active_txn_id);

#ifdef __cplusplus
}
#endif

#endif // WAVEDB_HBTRIE_H