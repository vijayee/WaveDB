//
// Lock-Free LRU Cache
//
// Based on eBay's high-throughput LRU design with Michael-Scott lock-free queue
//

#ifndef WAVEDB_LOCKFREE_LRU_H
#define WAVEDB_LOCKFREE_LRU_H

#include <stdint.h>
#include <stddef.h>
#include <stdatomic.h>
#include "../RefCounter/refcounter.h"
#include "../HBTrie/path.h"
#include "../HBTrie/identifier.h"

#ifdef __cplusplus
extern "C" {
#endif

// Forward declarations
typedef struct lru_node_t lru_node_t;
typedef struct lru_entry_t lru_entry_t;
typedef struct lru_queue_t lru_queue_t;
typedef struct lockfree_lru_shard_t lockfree_lru_shard_t;
typedef struct lockfree_lru_cache_t lockfree_lru_cache_t;

/**
 * LRU node - lives in the lock-free queue
 * Head side = LRU, Tail side = MRU
 */
struct lru_node_t {
    _Atomic(lru_entry_t*) entry;    // NULL = hole (marked for cleanup)
    _Atomic(lru_node_t*) next;      // Next in queue (toward MRU)
};

/**
 * LRU entry - lives in the concurrent hashmap
 */
struct lru_entry_t {
    refcounter_t refcounter;         // MUST be first
    path_t* path;                     // Key (immutable)
    identifier_t* value;              // Value (reference counted)
    _Atomic(lru_node_t*) node;       // Current position in LRU queue
    size_t memory_size;               // Memory footprint
};

/**
 * Lock-free LRU queue (Michael-Scott)
 */
struct lru_queue_t {
    _Atomic(lru_node_t*) head;       // LRU end (for eviction)
    _Atomic(lru_node_t*) tail;        // MRU end (for insertion)
    _Atomic(size_t) node_count;      // Approximate count (including holes)
    _Atomic(size_t) hole_count;       // Holes pending cleanup
};

/**
 * Per-shard LRU state
 */
struct lockfree_lru_shard_t {
    struct concurrent_hashmap_t* map; // path_t* -> lru_entry_t*
    lru_queue_t queue;                // Lock-free LRU queue

    _Atomic(size_t) current_memory;
    _Atomic(size_t) entry_count;
    size_t max_memory;

    _Atomic(uint8_t) purging;         // Purge in progress flag
};

/**
 * Top-level cache
 */
struct lockfree_lru_cache_t {
    lockfree_lru_shard_t* shards;
    uint16_t num_shards;
    size_t total_max_memory;
};

/**
 * Create a lock-free LRU cache.
 *
 * @param max_memory_bytes Maximum memory budget (0 for default)
 * @param num_shards Number of shards (0 for auto-scale)
 * @return New cache or NULL on failure
 */
lockfree_lru_cache_t* lockfree_lru_cache_create(size_t max_memory_bytes, uint16_t num_shards);

/**
 * Destroy an LRU cache.
 *
 * @param lru Cache to destroy
 */
void lockfree_lru_cache_destroy(lockfree_lru_cache_t* lru);

/**
 * Get a value from the cache (lock-free).
 *
 * Updates position to most recently used if found.
 *
 * @param lru Cache to query
 * @param path Path to look up
 * @return Value if found (reference counted), NULL if not
 */
identifier_t* lockfree_lru_cache_get(lockfree_lru_cache_t* lru, path_t* path);

/**
 * Put a value into the cache.
 *
 * @param lru Cache to update
 * @param path Path key (takes ownership)
 * @param value Value to store (takes ownership)
 * @return Old value if replaced (caller must destroy), NULL otherwise
 */
identifier_t* lockfree_lru_cache_put(lockfree_lru_cache_t* lru, path_t* path, identifier_t* value);

/**
 * Delete a value from the cache.
 *
 * @param lru Cache to update
 * @param path Path to delete
 */
void lockfree_lru_cache_delete(lockfree_lru_cache_t* lru, path_t* path);

/**
 * Get approximate entry count.
 *
 * @param lru Cache to query
 * @return Approximate number of entries
 */
size_t lockfree_lru_cache_size(lockfree_lru_cache_t* lru);

/**
 * Get approximate memory usage.
 *
 * @param lru Cache to query
 * @return Approximate memory usage in bytes
 */
size_t lockfree_lru_cache_memory(lockfree_lru_cache_t* lru);

/**
 * Purge holes from the queue (background cleanup).
 *
 * @param lru Cache to purge
 * @param max_batch Maximum holes to purge in one call (0 for unlimited)
 * @return Number of holes purged
 */
size_t lockfree_lru_cache_purge(lockfree_lru_cache_t* lru, size_t max_batch);

#ifdef __cplusplus
}
#endif

#endif // WAVEDB_LOCKFREE_LRU_H