//
// Created by victor on 3/11/26.
//

#include "database_lru.h"
#include "database.h"
#include "../Util/allocator.h"
#include "../Util/memory_pool.h"
#include "../Util/hash.h"
#include <stdlib.h>
#include <string.h>

// Hash function for path_t*
static size_t hash_path(const path_t* path) {
    if (path == NULL) return 0;

    size_t hash = 0;
    size_t len = path_length((path_t*)path);

    for (size_t i = 0; i < len; i++) {
        identifier_t* id = path_get((path_t*)path, i);
        if (id != NULL) {
            // Hash each chunk in the identifier
            for (int j = 0; j < id->chunks.length; j++) {
                chunk_t* chunk = id->chunks.data[j];
                if (chunk != NULL) {
                    // Simple hash combining - faster for short keys than xxHash
                    for (size_t k = 0; k < chunk->size; k++) {
                        hash = hash * 31 + chunk->data[k];
                    }
                }
            }
        }
    }

    return hash;
}

// Compare function for path_t*
static int compare_path(const path_t* a, const path_t* b) {
    if (a == NULL && b == NULL) return 0;
    if (a == NULL) return -1;
    if (b == NULL) return 1;
    return path_compare((path_t*)a, (path_t*)b);
}

// Duplicate path for hashmap key
static path_t* dup_path(const path_t* path) {
    if (path == NULL) return NULL;
    return path_copy((path_t*)path);
}

// Free path key
static void free_path(path_t* path) {
    if (path != NULL) {
        path_destroy(path);
    }
}

// Get shard index for a path (consistent hashing)
static size_t get_shard_index(const database_lru_cache_t* lru, const path_t* path) {
    if (path == NULL) return 0;
    size_t hash = hash_path(path);
    return hash % lru->num_shards;
}

// Calculate approximate memory usage for a cache entry
static size_t calculate_entry_memory(path_t* path, identifier_t* value) {
    size_t total = sizeof(database_lru_node_t);  // Node overhead

    // Path memory
    if (path != NULL) {
        total += sizeof(path_t);
        total += (size_t)path->identifiers.capacity * sizeof(identifier_t*);
        for (int i = 0; i < path->identifiers.length; i++) {
            identifier_t* id = path->identifiers.data[i];
            if (id != NULL) {
                total += sizeof(identifier_t);
                total += (size_t)id->chunks.capacity * sizeof(chunk_t*);
                for (int j = 0; j < id->chunks.length; j++) {
                    chunk_t* chunk = id->chunks.data[j];
                    if (chunk != NULL) {
                        total += sizeof(chunk_t) + chunk->size;
                    }
                }
            }
        }
    }

    // Value memory (similar calculation)
    if (value != NULL) {
        total += sizeof(identifier_t);
        total += (size_t)value->chunks.capacity * sizeof(chunk_t*);
        for (int j = 0; j < value->chunks.length; j++) {
            chunk_t* chunk = value->chunks.data[j];
            if (chunk != NULL) {
                total += sizeof(chunk_t) + chunk->size;
            }
        }
    }

    return total;
}

// Create a new LRU node
static database_lru_node_t* lru_node_create(path_t* path, identifier_t* value) {
    database_lru_node_t* node = memory_pool_alloc(sizeof(database_lru_node_t));
    if (node == NULL) return NULL;

    node->path = path;
    node->value = value;
    node->memory_size = calculate_entry_memory(path, value);  // Calculate memory usage
    node->next = NULL;
    node->previous = NULL;

    return node;
}

// Destroy an LRU node
static void lru_node_destroy(database_lru_node_t* node) {
    if (node == NULL) return;

    if (node->path != NULL) {
        path_destroy(node->path);
    }
    if (node->value != NULL) {
        identifier_destroy(node->value);
    }
    memory_pool_free(node, sizeof(database_lru_node_t));
}

// Move node to front of LRU list (most recently used)
static void lru_move_to_front(database_lru_shard_t* shard, database_lru_node_t* node) {
    if (node == shard->first) {
        return; // Already at front
    }

    // Remove from current position
    if (node->previous != NULL) {
        node->previous->next = node->next;
    }
    if (node->next != NULL) {
        node->next->previous = node->previous;
    }
    if (node == shard->last) {
        shard->last = node->previous;
    }

    // Insert at front
    node->previous = NULL;
    node->next = shard->first;
    if (shard->first != NULL) {
        shard->first->previous = node;
    }
    shard->first = node;

    if (shard->last == NULL) {
        shard->last = node;
    }
}

// Evict least recently used entry from a shard
static identifier_t* lru_evict(database_lru_shard_t* shard) {
    if (shard->last == NULL) {
        return NULL;
    }

    database_lru_node_t* node = shard->last;

    // Remove from hashmap
    hashmap_remove(&shard->cache, node->path);

    // Remove from list
    if (node->previous != NULL) {
        node->previous->next = NULL;
    }
    if (node == shard->first) {
        shard->first = NULL;
    }
    shard->last = node->previous;

    // Update memory tracking
    shard->current_memory -= node->memory_size;
    shard->entry_count--;

    // Get value to return (caller must destroy)
    identifier_t* value = node->value;
    node->value = NULL;

    // Free path and node
    path_destroy(node->path);
    memory_pool_free(node, sizeof(database_lru_node_t));

    return value;
}

database_lru_cache_t* database_lru_cache_create(size_t max_memory_bytes, uint16_t num_shards) {
    // Auto-scale shard count based on CPU cores if not specified
    if (num_shards == 0) {
        int cores = platform_core_count();
        // Use 4x cores for better contention distribution, minimum 64, maximum 256
        // Higher multiplier because LRU gets heavy read/write traffic
        num_shards = (uint16_t)(cores * 4);
        if (num_shards < 64) num_shards = 64;
        if (num_shards > 256) num_shards = 256;
    }

    database_lru_cache_t* lru = get_clear_memory(sizeof(database_lru_cache_t));
    if (lru == NULL) return NULL;

    // Allocate shards array
    lru->shards = get_clear_memory(num_shards * sizeof(database_lru_shard_t));
    if (lru->shards == NULL) {
        free(lru);
        return NULL;
    }
    lru->num_shards = num_shards;

    size_t total_memory = (max_memory_bytes == 0) ?
        DATABASE_DEFAULT_LRU_MEMORY_MB * 1024 * 1024 :
        max_memory_bytes;
    lru->total_max_memory = total_memory;

    // Initialize each shard
    size_t shard_memory = total_memory / num_shards;
    for (size_t i = 0; i < num_shards; i++) {
        database_lru_shard_t* shard = &lru->shards[i];

        hashmap_init(&shard->cache, (size_t (*)(const path_t*))hash_path, (int (*)(const path_t*, const path_t*))compare_path);
        hashmap_set_key_alloc_funcs(&shard->cache,
            (path_t* (*)(const path_t*))dup_path,
            (void (*)(path_t*))free_path);

        shard->first = NULL;
        shard->last = NULL;
        shard->current_memory = 0;
        shard->max_memory = shard_memory;
        shard->entry_count = 0;

        platform_lock_init(&shard->lock);
    }

    return lru;
}

void database_lru_cache_destroy(database_lru_cache_t* lru) {
    if (lru == NULL) return;

    // Free all shards
    for (size_t i = 0; i < lru->num_shards; i++) {
        database_lru_shard_t* shard = &lru->shards[i];

        platform_lock(&shard->lock);

        // Free all nodes in this shard
        database_lru_node_t* node = shard->first;
        while (node != NULL) {
            database_lru_node_t* next = node->next;
            if (node->path != NULL) {
                path_destroy(node->path);
            }
            if (node->value != NULL) {
                identifier_destroy(node->value);
            }
            memory_pool_free(node, sizeof(database_lru_node_t));
            node = next;
        }

        hashmap_cleanup(&shard->cache);
        platform_unlock(&shard->lock);
        platform_lock_destroy(&shard->lock);
    }

    free(lru->shards);
    free(lru);
}

identifier_t* database_lru_cache_get(database_lru_cache_t* lru, path_t* path) {
    if (lru == NULL || path == NULL) {
        return NULL;
    }

    size_t shard_idx = get_shard_index(lru, path);
    database_lru_shard_t* shard = &lru->shards[shard_idx];

    platform_lock(&shard->lock);

    database_lru_node_t* node = hashmap_get(&shard->cache, path);
    if (node == NULL) {
        platform_unlock(&shard->lock);
        return NULL;
    }

    // Move to front (most recently used)
    lru_move_to_front(shard, node);

    // Return reference to value
    identifier_t* value = (identifier_t*)refcounter_reference((refcounter_t*)node->value);

    platform_unlock(&shard->lock);
    return value;
}

identifier_t* database_lru_cache_put(database_lru_cache_t* lru, path_t* path, identifier_t* value) {
    if (lru == NULL || path == NULL) {
        if (path != NULL) path_destroy(path);
        if (value != NULL) identifier_destroy(value);
        return NULL;
    }

    size_t shard_idx = get_shard_index(lru, path);
    database_lru_shard_t* shard = &lru->shards[shard_idx];

    platform_lock(&shard->lock);

    // Check if already exists
    database_lru_node_t* existing = hashmap_get(&shard->cache, path);
    identifier_t* ejected = NULL;

    if (existing != NULL) {
        // Update existing entry
        identifier_t* old_value = existing->value;
        existing->value = value;

        // Update memory tracking
        size_t old_memory = existing->memory_size;
        existing->memory_size = calculate_entry_memory(path, value);
        shard->current_memory += (existing->memory_size - old_memory);

        path_destroy(path); // We don't need the new path, keep the old one
        lru_move_to_front(shard, existing);
        platform_unlock(&shard->lock);
        return old_value; // Caller must destroy old value
    }

    // Check if we need to evict (memory-based)
    size_t entry_memory = calculate_entry_memory(path, value);
    while (shard->current_memory + entry_memory > shard->max_memory && shard->last != NULL) {
        identifier_t* evicted = lru_evict(shard);
        if (evicted != NULL) {
            identifier_destroy(evicted);
        }
    }

    // Create new node
    database_lru_node_t* node = lru_node_create(path, value);
    if (node == NULL) {
        platform_unlock(&shard->lock);
        return ejected;
    }

    // Add to hashmap
    int result = hashmap_put(&shard->cache, node->path, node);
    if (result != 0) {
        lru_node_destroy(node);
        platform_unlock(&shard->lock);
        return ejected;
    }

    // Add to front of list
    node->next = shard->first;
    if (shard->first != NULL) {
        shard->first->previous = node;
    }
    shard->first = node;
    if (shard->last == NULL) {
        shard->last = node;
    }

    // Update memory tracking
    shard->current_memory += node->memory_size;
    shard->entry_count++;

    platform_unlock(&shard->lock);
    return ejected;
}

void database_lru_cache_delete(database_lru_cache_t* lru, path_t* path) {
    if (lru == NULL || path == NULL) {
        return;
    }

    size_t shard_idx = get_shard_index(lru, path);
    database_lru_shard_t* shard = &lru->shards[shard_idx];

    platform_lock(&shard->lock);

    database_lru_node_t* node = hashmap_get(&shard->cache, path);
    if (node == NULL) {
        platform_unlock(&shard->lock);
        return;
    }

    // Remove from hashmap
    hashmap_remove(&shard->cache, path);

    // Remove from list
    if (node->previous != NULL) {
        node->previous->next = node->next;
    }
    if (node->next != NULL) {
        node->next->previous = node->previous;
    }
    if (node == shard->first) {
        shard->first = node->next;
    }
    if (node == shard->last) {
        shard->last = node->previous;
    }

    // Update memory tracking
    shard->current_memory -= node->memory_size;
    shard->entry_count--;

    // Free node
    lru_node_destroy(node);

    platform_unlock(&shard->lock);
}

uint8_t database_lru_cache_contains(database_lru_cache_t* lru, path_t* path) {
    if (lru == NULL || path == NULL) {
        return 0;
    }

    size_t shard_idx = get_shard_index(lru, path);
    database_lru_shard_t* shard = &lru->shards[shard_idx];

    platform_lock(&shard->lock);
    database_lru_node_t* node = hashmap_get(&shard->cache, path);
    platform_unlock(&shard->lock);

    return node != NULL ? 1 : 0;
}

void database_lru_cache_clear(database_lru_cache_t* lru) {
    if (lru == NULL) return;

    // Clear all shards
    for (size_t i = 0; i < lru->num_shards; i++) {
        database_lru_shard_t* shard = &lru->shards[i];

        platform_lock(&shard->lock);

        // Free all nodes in this shard
        database_lru_node_t* node = shard->first;
        while (node != NULL) {
            database_lru_node_t* next = node->next;
            if (node->path != NULL) {
                path_destroy(node->path);
            }
            if (node->value != NULL) {
                identifier_destroy(node->value);
            }
            memory_pool_free(node, sizeof(database_lru_node_t));
            node = next;
        }

        // Clear hashmap
        hashmap_clear(&shard->cache);

        shard->first = NULL;
        shard->last = NULL;
        shard->current_memory = 0;
        shard->entry_count = 0;

        platform_unlock(&shard->lock);
    }
}

size_t database_lru_cache_size(database_lru_cache_t* lru) {
    if (lru == NULL) return 0;

    size_t total_size = 0;
    for (size_t i = 0; i < lru->num_shards; i++) {
        database_lru_shard_t* shard = &lru->shards[i];
        platform_lock(&shard->lock);
        total_size += shard->entry_count;
        platform_unlock(&shard->lock);
    }

    return total_size;
}

size_t database_lru_cache_memory(database_lru_cache_t* lru) {
    if (lru == NULL) return 0;

    size_t total_memory = 0;
    for (size_t i = 0; i < lru->num_shards; i++) {
        database_lru_shard_t* shard = &lru->shards[i];
        platform_lock(&shard->lock);
        total_memory += shard->current_memory;
        platform_unlock(&shard->lock);
    }

    return total_memory;
}