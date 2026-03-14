//
// Created by victor on 03/13/26.
//

#ifndef WAVEDB_SECTIONS_H
#define WAVEDB_SECTIONS_H

#include <stddef.h>
#include <stdint.h>
#include "../RefCounter/refcounter.h"
#include "../Util/threadding.h"
#include <hashmap.h>
#include "../Buffer/buffer.h"
#include "../Time/debouncer.h"
#include "section.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * sections_lru_node_t - Node in the LRU cache linked list.
 */
typedef struct sections_lru_node_t sections_lru_node_t;

struct sections_lru_node_t {
    section_t* value;
    sections_lru_node_t* next;
    sections_lru_node_t* previous;
};

/**
 * section_cache_t - Hashmap for section cache (section_id -> LRU node).
 */
typedef HASHMAP(size_t, sections_lru_node_t) section_cache_t;

/**
 * sections_lru_cache_t - LRU cache for keeping hot sections in memory.
 */
typedef struct {
    section_cache_t cache;
    sections_lru_node_t* first;  // Most recently used
    sections_lru_node_t* last;   // Least recently used
    size_t size;                 // Max cache size
} sections_lru_cache_t;

/**
 * Create an LRU cache for sections.
 */
sections_lru_cache_t* sections_lru_cache_create(size_t size);

/**
 * Destroy an LRU cache.
 */
void sections_lru_cache_destroy(sections_lru_cache_t* lru);

/**
 * Get a section from cache by ID.
 */
section_t* sections_lru_cache_get(sections_lru_cache_t* lru, size_t section_id);

/**
 * Remove a section from cache.
 */
void sections_lru_cache_delete(sections_lru_cache_t* lru, size_t section_id);

/**
 * Add a section to cache.
 */
void sections_lru_cache_put(sections_lru_cache_t* lru, section_t* section);

/**
 * Check if cache contains a section.
 */
uint8_t sections_lru_cache_contains(sections_lru_cache_t* lru, size_t section_id);

/**
 * round_robin_node_t - Node in the round-robin list.
 */
typedef struct round_robin_node_t round_robin_node_t;

struct round_robin_node_t {
    size_t id;
    round_robin_node_t* next;
    round_robin_node_t* previous;
};

/**
 * round_robin_t - Round-robin list for distributing writes across sections.
 */
typedef struct {
    PLATFORMLOCKTYPE(lock);
    debouncer_t* debouncer;
    char* path;
    size_t size;
    round_robin_node_t* first;
    round_robin_node_t* last;
} round_robin_t;

/**
 * Create a round-robin list.
 */
round_robin_t* round_robin_create(char* robin_path, hierarchical_timing_wheel_t* wheel);

/**
 * Destroy a round-robin list.
 */
void round_robin_destroy(round_robin_t* robin);

/**
 * Add a section ID to the round-robin.
 */
void round_robin_add(round_robin_t* robin, size_t id);

/**
 * Get next section ID from round-robin.
 */
size_t round_robin_next(round_robin_t* robin);

/**
 * Remove a section ID from round-robin.
 */
void round_robin_remove(round_robin_t* robin, size_t id);

/**
 * Check if round-robin contains a section ID.
 */
uint8_t round_robin_contains(round_robin_t* robin, size_t id);

/**
 * Serialize round-robin to CBOR.
 */
cbor_item_t* round_robin_to_cbor(round_robin_t* robin);

/**
 * Deserialize round-robin from CBOR.
 */
round_robin_t* cbor_to_round_robin(cbor_item_t* cbor, char* robin_path, hierarchical_timing_wheel_t* wheel);

/**
 * checkout_t - Tracks how many times a section is checked out.
 */
typedef struct {
    section_t* section;
    uint8_t count;
} checkout_t;

/**
 * section_checkout_t - Hashmap for checkout tracking (section_id -> checkout).
 */
typedef HASHMAP(size_t, checkout_t) section_checkout_t;

/**
 * sections_t - Pool of sections with LRU cache and checkout management.
 *
 * Manages multiple section files for storing fixed-size blocks.
 * Uses round-robin to distribute writes, LRU cache for hot sections,
 * and checkout/checkin for reference counting.
 */
typedef struct {
    PLATFORMLOCKTYPE(lock);

    sections_lru_cache_t* lru;       // LRU cache for hot sections
    round_robin_t* robin;           // Round-robin for write distribution

    struct {
        PLATFORMLOCKTYPE(lock);
        section_checkout_t sections; // Checkout tracking
    } checkout;

    size_t max_tuple_size;          // Max number of sections to keep open
    size_t next_id;                 // Next section ID to allocate
    size_t size;                    // Blocks per section
    block_size_e type;              // Block size type
    size_t wait;                    // Debounce wait time
    size_t max_wait;                // Max debounce wait time
    hierarchical_timing_wheel_t* wheel; // Timing wheel for debouncer

    char* data_path;                // Path to data directory
    char* meta_path;                // Path to metadata directory
    char* robin_path;               // Path to round-robin file
} sections_t;

/**
 * Create a sections pool.
 *
 * @param path          Directory path for section files
 * @param size          Number of blocks per section
 * @param cache_size    Max sections to keep in cache
 * @param max_tuple_size Max sections to keep open
 * @param type          Block size type
 * @param wheel         Timing wheel for debouncer
 * @param wait          Debounce wait time (ms)
 * @param max_wait      Max debounce wait time (ms)
 * @return New sections pool or NULL on failure
 */
sections_t* sections_create(char* path, size_t size, size_t cache_size, size_t max_tuple_size,
                            block_size_e type, hierarchical_timing_wheel_t* wheel,
                            size_t wait, size_t max_wait);

/**
 * Destroy a sections pool.
 */
void sections_destroy(sections_t* sections);

/**
 * Write data to a section.
 *
 * @param sections     Sections pool
 * @param data         Buffer to write (must match block_size)
 * @param section_id   Output: section ID where written
 * @param section_index Output: block index within section
 * @return 0 on success, error code on failure
 */
int sections_write(sections_t* sections, buffer_t* data, size_t* section_id, size_t* section_index);

/**
 * Read data from a section.
 *
 * @param sections    Sections pool
 * @param section_id  Section ID to read from
 * @param section_index Block index to read
 * @return Buffer with data, or NULL on failure
 */
buffer_t* sections_read(sections_t* sections, size_t section_id, size_t section_index);

/**
 * Deallocate a block from a section.
 *
 * @param sections    Sections pool
 * @param section_id  Section ID
 * @param section_index Block index to free
 * @return 0 on success, error code on failure
 */
int sections_deallocate(sections_t* sections, size_t section_id, size_t section_index);

/**
 * Checkout a section (increment reference count).
 *
 * Loads section from disk if not in cache.
 */
section_t* sections_checkout(sections_t* sections, size_t section_id);

/**
 * Checkin a section (decrement reference count).
 */
void sections_checkin(sections_t* sections, section_t* section);

#ifdef __cplusplus
}
#endif

#endif // WAVEDB_SECTIONS_H