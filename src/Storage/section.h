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
#include "../Workers/transaction_id.h"
#include <cbor.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * fragment_t - Represents a range of free bytes within a section.
 *
 * A fragment tracks contiguous free space: [start, end] inclusive (byte offsets).
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
 * section_t - A single file storing variable-size records.
 *
 * Each section is a file that contains multiple variable-size records.
 * Free space is tracked by a fragment list (list of free byte ranges).
 * Metadata is stored separately in a CBOR file.
 *
 * On-disk format per record:
 *   [transaction_id_t (24 bytes)] [data_size (8 bytes, network order)] [data (variable)]
 */
typedef struct {
    refcounter_t refcounter;        // MUST be first member
    PLATFORMLOCKTYPE(lock);          // Thread-safe access

    int fd;                          // File descriptor (-1 if not open)
    size_t id;                       // Section ID
    char* meta_path;                 // Path to metadata file
    char* path;                      // Path to data file
    fragment_list_t* fragments;      // Free space tracking (byte offsets)
    size_t size;                     // Max section size in bytes
} section_t;

/**
 * Create or load a section.
 *
 * @param path       Directory path for section data
 * @param meta_path  Directory path for section metadata
 * @param size       Maximum section size in bytes
 * @param id         Section ID
 * @return New section or NULL on failure
 */
section_t* section_create(char* path, char* meta_path, size_t size, size_t id);

/**
 * Destroy a section.
 *
 * @param section  Section to destroy
 */
void section_destroy(section_t* section);

/**
 * Write variable-size data to the section.
 *
 * @param section     Section to write to
 * @param txn_id      Transaction ID for this write
 * @param data        Buffer to write (any size)
 * @param offset      Output: byte offset where written
 * @param full        Output: 1 if section is now full
 * @return 0 on success, error code on failure
 */
int section_write(section_t* section, transaction_id_t txn_id, buffer_t* data, size_t* offset, uint8_t* full);

/**
 * Read variable-size data from the section.
 *
 * @param section  Section to read from
 * @param offset   Byte offset to read
 * @param txn_id   Output: transaction ID of this record
 * @param data     Output: buffer with data (caller must destroy)
 * @return 0 on success, error code on failure
 */
int section_read(section_t* section, size_t offset, transaction_id_t* txn_id, buffer_t** data);

/**
 * Deallocate a record (mark as free).
 *
 * @param section     Section to deallocate from
 * @param offset       Byte offset to free
 * @param data_size    Size of data to free (without header)
 * @return 0 on success, error code on failure
 */
int section_deallocate(section_t* section, size_t offset, size_t data_size);

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