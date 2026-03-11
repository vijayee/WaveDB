//
// Created by victor on 3/11/26.
//

#ifndef WAVEDB_BS_ARRAY_H
#define WAVEDB_BS_ARRAY_H

#include <stdint.h>
#include <stddef.h>
#include "chunk.h"

#ifdef __cplusplus
extern "C" {
#endif

// Forward declaration
struct bnode_entry_t;
typedef struct bnode_entry_t bnode_entry_t;

/**
 * bs_array_t - Sorted array with binary search for B+tree node entries.
 *
 * Maintains entries sorted by chunk key for efficient lookup.
 * Uses binary search for insert, find, and remove operations.
 */
typedef struct {
    bnode_entry_t* entries;   // Array of entries
    size_t count;             // Number of entries
    size_t capacity;          // Allocated capacity
} bs_array_t;

/**
 * Initialize a sorted array (stack allocation).
 *
 * @param arr  Array to initialize
 */
void bs_array_init(bs_array_t* arr);

/**
 * Create a sorted array (heap allocation).
 *
 * @param initial_capacity  Initial capacity (0 for default)
 * @return New sorted array or NULL on failure
 */
bs_array_t* bs_array_create(size_t initial_capacity);

/**
 * Destroy a sorted array.
 *
 * @param arr  Array to destroy
 */
void bs_array_destroy(bs_array_t* arr);

/**
 * Find an entry by key using binary search.
 *
 * @param arr  Sorted array
 * @param key  Key to find
 * @param out_index  Output: index where found or would be inserted
 * @return Entry if found, NULL if not found
 */
bnode_entry_t* bs_array_find(bs_array_t* arr, chunk_t* key, size_t* out_index);

/**
 * Insert an entry in sorted order.
 *
 * @param arr    Sorted array
 * @param entry  Entry to insert
 * @return 0 on success, -1 on failure
 */
int bs_array_insert(bs_array_t* arr, bnode_entry_t* entry);

/**
 * Remove an entry at a specific index.
 *
 * @param arr    Sorted array
 * @param index  Index to remove
 * @return Removed entry, or NULL if index out of bounds
 */
bnode_entry_t* bs_array_remove_at(bs_array_t* arr, size_t index);

/**
 * Get entry at index.
 *
 * @param arr    Sorted array
 * @param index  Index
 * @return Entry at index, or NULL if out of bounds
 */
bnode_entry_t* bs_array_get(bs_array_t* arr, size_t index);

/**
 * Get the number of entries.
 *
 * @param arr  Sorted array
 * @return Number of entries
 */
size_t bs_array_count(bs_array_t* arr);

/**
 * Check if array is empty.
 *
 * @param arr  Sorted array
 * @return true if empty, false otherwise
 */
int bs_array_is_empty(bs_array_t* arr);

/**
 * Find the first entry >= key.
 *
 * @param arr  Sorted array
 * @param key  Key to find
 * @return Entry if found, NULL if all entries < key
 */
bnode_entry_t* bs_array_find_first(bs_array_t* arr, chunk_t* key);

/**
 * Find the last entry <= key.
 *
 * @param arr  Sorted array
 * @param key  Key to find
 * @return Entry if found, NULL if all entries > key
 */
bnode_entry_t* bs_array_find_last(bs_array_t* arr, chunk_t* key);

#ifdef __cplusplus
}
#endif

#endif // WAVEDB_BS_ARRAY_H