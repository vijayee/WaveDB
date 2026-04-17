//
// Created by victor on 3/11/26.
//

#ifndef WAVEDB_DATABASE_LRU_H
#define WAVEDB_DATABASE_LRU_H

#include <stdint.h>
#include <stddef.h>
#include "../RefCounter/refcounter.h"
#include "../HBTrie/path.h"
#include "../HBTrie/identifier.h"
#include "../Util/threadding.h"
#include <hashmap.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * LRU cache node for database entries.
 *
 * Forms a doubly-linked list for LRU ordering.
 */
typedef struct database_lru_node_t database_lru_node_t;
struct database_lru_node_t {
    path_t* path;                   // Key (reference counted, shared with hashmap)
    uint64_t key_hash;              // hash_path() result, compared first
    identifier_t* value;            // Value (reference counted)
    size_t memory_size;             // Approximate memory for this entry
    database_lru_node_t* next;      // Next in LRU list (more recently used)
    database_lru_node_t* previous;  // Previous in LRU list (less recently used)
};

/**
 * Single shard of the LRU cache.
 *
 * Each shard has its own lock for reduced contention.
 */
typedef struct database_lru_shard database_lru_shard_t;
struct database_lru_shard {
    HASHMAP(path_t, database_lru_node_t) cache;  // Path -> node mapping
    database_lru_node_t* first;     // Most recently used
    database_lru_node_t* last;      // Least recently used
    size_t current_memory;          // Current memory usage in bytes
    size_t max_memory;              // Max memory for this shard
    size_t entry_count;             // Number of entries in this shard
    PLATFORMLOCKTYPE(lock);          // Lock for this shard only
};

/**
 * Sharded LRU cache for database path -> identifier lookups.
 *
 * Thread-safe via per-shard locks. Uses hashmap for O(1) lookup
 * and doubly-linked list for O(1) LRU ordering.
 */
typedef struct {
    database_lru_shard_t* shards;   // Dynamically allocated array of shards
    uint16_t num_shards;             // Number of shards
    size_t total_max_memory;         // Total memory budget across all shards
} database_lru_cache_t;

/**
 * Create an LRU cache.
 *
 * @param max_memory_bytes Maximum memory budget in bytes (0 for unlimited)
 * @param num_shards       Number of shards (0 for auto-scale based on CPU cores)
 * @return New cache or NULL on failure
 */
database_lru_cache_t* database_lru_cache_create(size_t max_memory_bytes, uint16_t num_shards);

/**
 * Destroy an LRU cache.
 *
 * Frees all nodes and releases value references.
 *
 * @param lru Cache to destroy
 */
void database_lru_cache_destroy(database_lru_cache_t* lru);

/**
 * Get a value from the cache.
 *
 * Updates position to most recently used if found.
 *
 * @param lru  Cache to query
 * @param path Path to look up
 * @return Value if found (reference counted), NULL if not found
 */
identifier_t* database_lru_cache_get(database_lru_cache_t* lru, path_t* path);

/**
 * Put a value into the cache.
 *
 * Replaces existing value if path already exists.
 * May evict LRU entries if cache is full (evicted values are destroyed internally).
 *
 * @param lru   Cache to update
 * @param path  Path key (takes ownership of reference)
 * @param value Value to store (takes ownership of reference)
 * @return Old value if path already existed (caller must destroy), NULL otherwise
 */
identifier_t* database_lru_cache_put(database_lru_cache_t* lru, path_t* path, identifier_t* value);

/**
 * Delete a value from the cache.
 *
 * @param lru  Cache to update
 * @param path Path to delete
 */
void database_lru_cache_delete(database_lru_cache_t* lru, path_t* path);

/**
 * Check if a path exists in the cache.
 *
 * Does not update LRU ordering.
 *
 * @param lru  Cache to query
 * @param path Path to check
 * @return 1 if exists, 0 if not
 */
uint8_t database_lru_cache_contains(database_lru_cache_t* lru, path_t* path);

/**
 * Clear all entries from the cache.
 *
 * @param lru Cache to clear
 */
void database_lru_cache_clear(database_lru_cache_t* lru);

/**
 * Get current cache size.
 *
 * @param lru Cache to query
 * @return Number of entries
 */
size_t database_lru_cache_size(database_lru_cache_t* lru);

/**
 * Get current total memory usage across all shards.
 *
 * @param lru Cache to query
 * @return Total memory usage in bytes
 */
size_t database_lru_cache_memory(database_lru_cache_t* lru);

#ifdef __cplusplus
}
#endif

#endif //WAVEDB_DATABASE_LRU_H