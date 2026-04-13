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
#include "../Time/debouncer.h"
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
 * fragment_list_t - Sorted array of free fragments (by size, ascending).
 *
 * Tracks available space within a section for O(log n) binary search.
 * Replaces linked list for better performance.
 */
typedef struct {
    fragment_t* fragments;      // Sorted array of fragments (by size)
    size_t count;               // Number of fragments
    size_t capacity;            // Array capacity
    size_t total_free_space;    // Total free space (for quick rejection)
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
 * Add fragment to list (maintains sorted order by size).
 */
void fragment_list_insert(fragment_list_t* list, fragment_t* fragment);

/**
 * Find and remove first fragment that fits (binary search by size).
 * Returns the offset and removes/updates the fragment.
 * Returns 0 on success, 1 if no fragment found.
 */
int fragment_list_find_fit(fragment_list_t* list, size_t size, size_t* offset);

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

    // Debounced metadata save
    uint8_t meta_dirty;              // Flag: metadata needs saving
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
 * Flush dirty metadata to disk if needed.
 *
 * Saves fragment list to CBOR file if meta_dirty flag is set.
 *
 * @param section  Section to flush
 */
void section_flush_metadata(section_t* section);

/**
 * Check if section is full (has no free fragments at all).
 *
 * @param section  Section to check
 * @return 1 if full, 0 otherwise
 */
uint8_t section_full(section_t* section);

/**
 * Check if section can fit a write of the given size.
 *
 * Unlike section_full(), this checks actual fragment sizes, not just
 * whether any fragments exist. A section may have fragments that are
 * all too small for the requested write.
 *
 * @param section         Section to check
 * @param required_bytes  Minimum contiguous bytes needed
 * @return 1 if the section can fit the write, 0 otherwise
 */
uint8_t section_can_fit(section_t* section, size_t required_bytes);

/**
 * Flush dirty metadata to disk if needed.
 *
 * Debounced metadata save: only flushes if metadata is dirty.
 * Call this when section is being checked in or closed.
 *
 * @param section  Section to flush
 */
void section_flush_metadata(section_t* section);

#ifdef __cplusplus
}
#endif

#endif // WAVEDB_SECTION_H