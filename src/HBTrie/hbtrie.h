//
// Created by victor on 3/11/26.
//

#ifndef WAVEDB_HBTRIE_H
#define WAVEDB_HBTRIE_H

#include <stdint.h>
#include <stddef.h>
#include "../RefCounter/refcounter.h"
#include "../Util/threadding.h"
#include "chunk.h"
#include "identifier.h"
#include "path.h"
#include "bnode.h"

#ifdef __cplusplus
extern "C" {
#endif

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
    PLATFORMLOCKTYPE(lock);           // Node-level lock

    bnode_t* btree;                   // B+tree comparing chunks at this level
} hbtrie_node_t;

/**
 * hbtrie_cursor_t - Cursor for HBTrie traversal.
 *
 * Tracks position during traversal of a path through the HBTrie.
 * Used for both reading and inserting (creating nodes as needed).
 */
typedef struct {
    hbtrie_node_t* current;           // Current HBTrie node
    size_t identifier_index;          // Which identifier in the path we're on
    size_t chunk_pos;                 // Current chunk position within identifier
} hbtrie_cursor_t;

/**
 * hbtrie_t - Top-level HBTrie structure.
 */
typedef struct hbtrie_t {
    refcounter_t refcounter;          // MUST be first member
    PLATFORMLOCKTYPE(lock);           // Trie-level lock

    uint8_t chunk_size;               // Configurable chunk size (default: DEFAULT_CHUNK_SIZE)
    uint32_t btree_node_size;         // Max B+tree node size in bytes

    hbtrie_node_t* root;              // Root HBTrie node
} hbtrie_t;

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
 * Insert a value into the HBTrie at the given path.
 *
 * Traverses the path, creating missing nodes along the way,
 * then stores the value at the leaf.
 *
 * @param trie   HBTrie to insert into
 * @param path   Path key (sequence of identifiers)
 * @param value  Value to store at leaf (takes ownership of reference)
 * @return 0 on success, -1 on failure
 */
int hbtrie_insert(hbtrie_t* trie, path_t* path, identifier_t* value);

/**
 * Find a value in the HBTrie at the given path.
 *
 * @param trie  HBTrie to search
 * @param path  Path key to find
 * @return Value if found, NULL if not found
 */
identifier_t* hbtrie_find(hbtrie_t* trie, path_t* path);

/**
 * Remove a value from the HBTrie at the given path.
 *
 * @param trie  HBTrie to remove from
 * @param path  Path key to remove
 * @return Removed value, or NULL if not found
 */
identifier_t* hbtrie_remove(hbtrie_t* trie, path_t* path);

/**
 * Initialize a cursor for path traversal.
 *
 * @param cursor  Cursor to initialize
 * @param trie    HBTrie to traverse
 * @param path    Path to traverse (or NULL for root-only)
 */
void hbtrie_cursor_init(hbtrie_cursor_t* cursor, hbtrie_t* trie, path_t* path);

/**
 * Move cursor to next position in the path.
 *
 * @param cursor  Cursor to advance
 * @return 0 on success, -1 at end of path
 */
int hbtrie_cursor_next(hbtrie_cursor_t* cursor);

/**
 * Check if cursor is at end of path.
 *
 * @param cursor  Cursor to check
 * @return 1 if at end, 0 otherwise
 */
int hbtrie_cursor_at_end(hbtrie_cursor_t* cursor);

/**
 * Get current chunk at cursor position.
 *
 * @param cursor  Cursor position
 * @return Current chunk, or NULL if none
 */
chunk_t* hbtrie_cursor_get_chunk(hbtrie_cursor_t* cursor);

/**
 * Get current HBTrie node at cursor position.
 *
 * @param cursor  Cursor position
 * @return Current node, or NULL if none
 */
hbtrie_node_t* hbtrie_cursor_get_node(hbtrie_cursor_t* cursor);

#ifdef __cplusplus
}
#endif

#endif // WAVEDB_HBTRIE_H