//
// Created by victor on 03/13/26.
//

#ifndef WAVEDB_SECTION_H
#define WAVEDB_SECTION_H

#include <stddef.h>
#include <stdint.h>
#include "../RefCounter/refcounter.h"
#include "../Util/threadding.h"
#include "../Buffer/buffer.h"
#include <cbor.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Block size enumeration for different section types.
 */
typedef enum {
    BLOCK_SIZE_SMALL = 256,    // 256 bytes - for small nodes
    BLOCK_SIZE_MEDIUM = 4096,  // 4KB - for medium nodes
    BLOCK_SIZE_LARGE = 65536   // 64KB - for large nodes
} block_size_e;

/**
 * fragment_t - Represents a range of free blocks within a section.
 *
 * A fragment tracks contiguous free space: [start, end] inclusive.
 */
typedef struct {
    size_t start;
    size_t end;
} fragment_t;

/**
 * Create a fragment.
 *
 * @param start  Start index
 * @param end    End index (inclusive)
 * @return New fragment
 */
fragment_t* fragment_create(size_t start, size_t end);

/**
 * Destroy a fragment.
 *
 * @param fragment  Fragment to destroy
 */
void fragment_destroy(fragment_t* fragment);

/**
 * Serialize fragment to CBOR.
 */
cbor_item_t* fragment_to_cbor(fragment_t* fragment);

/**
 * Deserialize fragment from CBOR.
 */
fragment_t* cbor_to_fragment(cbor_item_t* cbor);

/**
 * fragment_list_node_t - Node in the doubly-linked fragment list.
 */
typedef struct fragment_list_node_t fragment_list_node_t;

struct fragment_list_node_t {
    fragment_t* fragment;
    fragment_list_node_t* next;
    fragment_list_node_t* previous;
};

/**
 * Create a fragment list node.
 */
fragment_list_node_t* fragment_list_node_create(fragment_t* fragment,
                                                  fragment_list_node_t* next,
                                                  fragment_list_node_t* previous);

/**
 * fragment_list_t - Doubly-linked list of free fragments.
 *
 * Tracks available space within a section.
 */
typedef struct {
    fragment_list_node_t* first;
    fragment_list_node_t* last;
    size_t count;
} fragment_list_t;

/**
 * Create a fragment list.
 */
fragment_list_t* fragment_list_create(void);

/**
 * Destroy a fragment list.
 */
void fragment_list_destroy(fragment_list_t* list);

/**
 * Add fragment to end of list.
 */
void fragment_list_enqueue(fragment_list_t* list, fragment_t* fragment);

/**
 * Remove and return first fragment.
 */
fragment_t* fragment_list_dequeue(fragment_list_t* list);

/**
 * Remove specific node from list.
 */
fragment_t* fragment_list_remove(fragment_list_t* list, fragment_list_node_t* node);

/**
 * Serialize fragment list to CBOR.
 */
cbor_item_t* fragment_list_to_cbor(fragment_list_t* list);

/**
 * Deserialize fragment list from CBOR.
 */
fragment_list_t* cbor_to_fragment_list(cbor_item_t* cbor);

/**
 * section_t - A single file storing fixed-size blocks.
 *
 * Each section is a file that contains multiple fixed-size blocks.
 * Free space is tracked by a fragment list (list of free ranges).
 * Metadata is stored separately in a CBOR file.
 */
typedef struct {
    refcounter_t refcounter;        // MUST be first member
    PLATFORMLOCKTYPE(lock);          // Thread-safe access

    int fd;                          // File descriptor (-1 if not open)
    size_t id;                       // Section ID
    char* meta_path;                 // Path to metadata file
    char* path;                      // Path to data file
    fragment_list_t* fragments;      // Free space tracking
    size_t size;                     // Total blocks in section
    block_size_e block_size;         // Size of each block
} section_t;

/**
 * Create or load a section.
 *
 * @param path       Directory path for section data
 * @param meta_path  Directory path for section metadata
 * @param size       Number of blocks in section
 * @param id         Section ID
 * @param type       Block size type
 * @return New section or NULL on failure
 */
section_t* section_create(char* path, char* meta_path, size_t size, size_t id, block_size_e type);

/**
 * Destroy a section.
 *
 * @param section  Section to destroy
 */
void section_destroy(section_t* section);

/**
 * Write a block to the section.
 *
 * @param section     Section to write to
 * @param data        Buffer to write (must match block_size)
 * @param index       Output: block index where written
 * @param full        Output: 1 if section is now full
 * @return 0 on success, error code on failure
 */
int section_write(section_t* section, buffer_t* data, size_t* index, uint8_t* full);

/**
 * Read a block from the section.
 *
 * @param section  Section to read from
 * @param index    Block index to read
 * @return Buffer with block data, or NULL on failure
 */
buffer_t* section_read(section_t* section, size_t index);

/**
 * Deallocate a block (mark as free).
 *
 * @param section  Section to deallocate from
 * @param index    Block index to free
 * @return 0 on success, error code on failure
 */
int section_deallocate(section_t* section, size_t index);

/**
 * Check if section is full.
 *
 * @param section  Section to check
 * @return 1 if full, 0 otherwise
 */
uint8_t section_full(section_t* section);

#ifdef __cplusplus
}
#endif

#endif // WAVEDB_SECTION_H