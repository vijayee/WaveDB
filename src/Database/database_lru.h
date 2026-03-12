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
    path_t* path;                   // Key (owned by node)
    identifier_t* value;            // Value (reference counted)
    database_lru_node_t* next;      // Next in LRU list (more recently used)
    database_lru_node_t* previous;  // Previous in LRU list (less recently used)
};

/**
 * LRU cache for database path -> identifier lookups.
 *
 * Thread-safe via lock. Uses hashmap for O(1) lookup
 * and doubly-linked list for O(1) LRU ordering.
 */
typedef struct {
    PLATFORMLOCKTYPE(lock);
    HASHMAP(path_t, database_lru_node_t) cache;  // Path -> node mapping
    database_lru_node_t* first;     // Most recently used
    database_lru_node_t* last;       // Least recently used
    size_t size;                     // Current number of entries
    size_t max_size;                 // Maximum entries before eviction
} database_lru_cache_t;

/**
 * Create an LRU cache.
 *
 * @param max_size Maximum number of entries before eviction (0 for unlimited)
 * @return New cache or NULL on failure
 */
database_lru_cache_t* database_lru_cache_create(size_t max_size);

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
 * May evict LRU entry if cache is full.
 *
 * @param lru   Cache to update
 * @param path  Path key (takes ownership of reference)
 * @param value Value to store (takes ownership of reference)
 * @return Evicted value if any (caller must destroy), NULL otherwise
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

#ifdef __cplusplus
}
#endif

#endif //WAVEDB_DATABASE_LRU_H