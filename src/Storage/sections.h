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
 * Number of shards for checkout locks (reduces contention)
 */
#define CHECKOUT_LOCK_SHARDS 16

/**
 * checkout_shard_t - Per-shard checkout tracking with lock.
 */
typedef struct {
    PLATFORMLOCKTYPE(lock);
    section_checkout_t sections;
} checkout_shard_t;

/**
 * Default defragmentation idle threshold in milliseconds.
 * Section must be idle (no writes/deallocations) for this long
 * before defragmentation is considered.
 */
#define SECTIONS_DEFAULT_DEFRAG_IDLE_MS 30000  // 30 seconds

/**
 * Default free space ratio threshold for triggering defragmentation.
 * Sections with less free space than this ratio are not defragmented.
 */
#define SECTIONS_DEFAULT_DEFRAG_THRESHOLD 0.5  // 50%

/**
 * sections_t - Pool of sections with LRU cache and checkout management.
 *
 * Manages multiple section files for storing variable-size records.
 * Uses round-robin to distribute writes, LRU cache for hot sections,
 * and checkout/checkin for reference tracking.
 */
typedef struct sections_t {
    PLATFORMLOCKTYPE(lock);

    sections_lru_cache_t* lru;       // LRU cache for hot sections
    round_robin_t* robin;           // Round-robin for write distribution

    checkout_shard_t checkout_shards[CHECKOUT_LOCK_SHARDS]; // Sharded checkout tracking

    size_t section_concurrency;      // Max sections to keep open
    size_t next_id;                  // Next section ID to allocate
    size_t size;                     // Max section size in bytes
    size_t wait;                     // Debounce wait time
    size_t max_wait;                 // Max debounce wait time
    hierarchical_timing_wheel_t* wheel; // Timing wheel for debouncer

    // Defragmentation scheduling
    debouncer_t* defrag_debouncer;   // Idle-triggered defrag timer
    double defrag_threshold;          // Free space ratio to trigger defrag (0.0-1.0)

    transaction_id_t oldest_txn_id; // Oldest transaction since last compaction
    transaction_id_t newest_txn_id; // Newest transaction written to disk
    char* range_path;                // Path to .range file

    char* data_path;                 // Path to data directory
    char* meta_path;                 // Path to metadata directory
    char* robin_path;                // Path to round-robin file
} sections_t;

/**
 * Create a sections pool.
 *
 * @param path               Directory path for section files
 * @param size               Max section size in bytes
 * @param cache_size         Max sections to keep in cache
 * @param section_concurrency Max sections to keep open
 * @param wheel              Timing wheel for debouncer
 * @param wait               Debounce wait time (ms)
 * @param max_wait           Max debounce wait time (ms)
 * @return New sections pool or NULL on failure
 */
sections_t* sections_create(char* path, size_t size, size_t cache_size, size_t section_concurrency,
                            hierarchical_timing_wheel_t* wheel, size_t wait, size_t max_wait);

/**
 * Destroy a sections pool.
 */
void sections_destroy(sections_t* sections);

/**
 * Write data to a section.
 *
 * @param sections     Sections pool
 * @param txn_id       Transaction ID for this write
 * @param data         Buffer to write
 * @param section_id   Output: section ID where written
 * @param offset       Output: byte offset within section
 * @return 0 on success, error code on failure
 */
int sections_write(sections_t* sections, transaction_id_t txn_id, buffer_t* data, size_t* section_id, size_t* offset);

/**
 * Read data from a section.
 *
 * @param sections    Sections pool
 * @param section_id  Section ID to read from
 * @param offset      Byte offset to read
 * @param txn_id      Output: transaction ID of this record
 * @param data        Output: buffer with data (caller must destroy)
 * @return 0 on success, error code on failure
 */
int sections_read(sections_t* sections, size_t section_id, size_t offset, transaction_id_t* txn_id, buffer_t** data);

/**
 * Deallocate a record from a section.
 *
 * @param sections    Sections pool
 * @param section_id  Section ID
 * @param offset      Byte offset to free
 * @param data_size   Size of data to free
 * @return 0 on success, error code on failure
 */
int sections_deallocate(sections_t* sections, size_t section_id, size_t offset, size_t data_size);

/**
 * Update transaction range tracking.
 *
 * Called after each write to track oldest and newest transactions.
 *
 * @param sections  Sections pool
 * @param txn_id    Transaction ID that was just written
 */
void sections_update_txn_range(sections_t* sections, transaction_id_t txn_id);

/**
 * Save transaction range to disk.
 *
 * @param sections  Sections pool
 */
void sections_save_txn_range(sections_t* sections);

/**
 * Load transaction range from disk.
 *
 * @param sections  Sections pool
 * @return 0 on success, error code on failure
 */
int sections_load_txn_range(sections_t* sections);

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