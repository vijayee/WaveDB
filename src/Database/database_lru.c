//
// Created by victor on 3/11/26.
//

#include "database_lru.h"
#include "database.h"
#include "../Util/allocator.h"
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
                if (chunk != NULL && chunk->data != NULL) {
                    // Simple hash combining - faster for short keys than xxHash
                    for (size_t k = 0; k < chunk->data->size; k++) {
                        hash = hash * 31 + chunk->data->data[k];
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
                    if (chunk != NULL && chunk->data != NULL) {
                        total += sizeof(chunk_t);
                        total += chunk->data->size;
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
            if (chunk != NULL && chunk->data != NULL) {
                total += sizeof(chunk_t);
                total += chunk->data->size;
            }
        }
    }

    return total;
}

// Create a new LRU node
static database_lru_node_t* lru_node_create(path_t* path, identifier_t* value) {
    database_lru_node_t* node = get_clear_memory(sizeof(database_lru_node_t));
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
    free(node);
}

// Move node to front of LRU list (most recently used)
static void lru_move_to_front(database_lru_cache_t* lru, database_lru_node_t* node) {
    if (node == lru->first) {
        return; // Already at front
    }

    // Remove from current position
    if (node->previous != NULL) {
        node->previous->next = node->next;
    }
    if (node->next != NULL) {
        node->next->previous = node->previous;
    }
    if (node == lru->last) {
        lru->last = node->previous;
    }

    // Insert at front
    node->previous = NULL;
    node->next = lru->first;
    if (lru->first != NULL) {
        lru->first->previous = node;
    }
    lru->first = node;

    if (lru->last == NULL) {
        lru->last = node;
    }
}

// Evict least recently used entry
static identifier_t* lru_evict(database_lru_cache_t* lru) {
    if (lru->last == NULL) {
        return NULL;
    }

    database_lru_node_t* node = lru->last;

    // Remove from hashmap
    hashmap_remove(&lru->cache, node->path);

    // Remove from list
    if (node->previous != NULL) {
        node->previous->next = NULL;
    }
    if (node == lru->first) {
        lru->first = NULL;
    }
    lru->last = node->previous;

    // Update memory tracking
    lru->current_memory -= node->memory_size;
    lru->entry_count--;

    // Get value to return (caller must destroy)
    identifier_t* value = node->value;
    node->value = NULL;

    // Free path and node
    path_destroy(node->path);
    free(node);

    return value;
}

database_lru_cache_t* database_lru_cache_create(size_t max_memory_bytes) {
    database_lru_cache_t* lru = get_clear_memory(sizeof(database_lru_cache_t));
    if (lru == NULL) return NULL;

    hashmap_init(&lru->cache, (size_t (*)(const path_t*))hash_path, (int (*)(const path_t*, const path_t*))compare_path);
    hashmap_set_key_alloc_funcs(&lru->cache,
        (path_t* (*)(const path_t*))dup_path,
        (void (*)(path_t*))free_path);

    lru->first = NULL;
    lru->last = NULL;
    lru->current_memory = 0;
    lru->max_memory = (max_memory_bytes == 0) ?
        DATABASE_DEFAULT_LRU_MEMORY_MB * 1024 * 1024 :
        max_memory_bytes;
    lru->entry_count = 0;

    platform_lock_init(&lru->lock);

    return lru;
}

void database_lru_cache_destroy(database_lru_cache_t* lru) {
    if (lru == NULL) return;

    platform_lock(&lru->lock);

    // Free all nodes
    database_lru_node_t* node = lru->first;
    while (node != NULL) {
        database_lru_node_t* next = node->next;
        if (node->path != NULL) {
            path_destroy(node->path);
        }
        if (node->value != NULL) {
            identifier_destroy(node->value);
        }
        free(node);
        node = next;
    }

    hashmap_cleanup(&lru->cache);
    platform_unlock(&lru->lock);
    platform_lock_destroy(&lru->lock);
    free(lru);
}

identifier_t* database_lru_cache_get(database_lru_cache_t* lru, path_t* path) {
    if (lru == NULL || path == NULL) {
        return NULL;
    }

    platform_lock(&lru->lock);

    database_lru_node_t* node = hashmap_get(&lru->cache, path);
    if (node == NULL) {
        platform_unlock(&lru->lock);
        return NULL;
    }

    // Move to front (most recently used)
    lru_move_to_front(lru, node);

    // Return reference to value
    identifier_t* value = (identifier_t*)refcounter_reference((refcounter_t*)node->value);

    platform_unlock(&lru->lock);
    return value;
}

identifier_t* database_lru_cache_put(database_lru_cache_t* lru, path_t* path, identifier_t* value) {
    if (lru == NULL || path == NULL) {
        if (path != NULL) path_destroy(path);
        if (value != NULL) identifier_destroy(value);
        return NULL;
    }

    platform_lock(&lru->lock);

    // Check if already exists
    database_lru_node_t* existing = hashmap_get(&lru->cache, path);
    identifier_t* ejected = NULL;

    if (existing != NULL) {
        // Update existing entry
        identifier_t* old_value = existing->value;
        existing->value = value;

        // Update memory tracking
        size_t old_memory = existing->memory_size;
        existing->memory_size = calculate_entry_memory(path, value);
        lru->current_memory += (existing->memory_size - old_memory);

        path_destroy(path); // We don't need the new path, keep the old one
        lru_move_to_front(lru, existing);
        platform_unlock(&lru->lock);
        return old_value; // Caller must destroy old value
    }

    // Check if we need to evict (memory-based)
    size_t entry_memory = calculate_entry_memory(path, value);
    while (lru->current_memory + entry_memory > lru->max_memory && lru->last != NULL) {
        identifier_t* evicted = lru_evict(lru);
        if (evicted != NULL) {
            identifier_destroy(evicted);
        }
    }

    // Create new node
    database_lru_node_t* node = lru_node_create(path, value);
    if (node == NULL) {
        platform_unlock(&lru->lock);
        return ejected;
    }

    // Add to hashmap
    int result = hashmap_put(&lru->cache, node->path, node);
    if (result != 0) {
        lru_node_destroy(node);
        platform_unlock(&lru->lock);
        return ejected;
    }

    // Add to front of list
    node->next = lru->first;
    if (lru->first != NULL) {
        lru->first->previous = node;
    }
    lru->first = node;
    if (lru->last == NULL) {
        lru->last = node;
    }

    // Update memory tracking
    lru->current_memory += node->memory_size;
    lru->entry_count++;

    platform_unlock(&lru->lock);
    return ejected;
}

void database_lru_cache_delete(database_lru_cache_t* lru, path_t* path) {
    if (lru == NULL || path == NULL) {
        return;
    }

    platform_lock(&lru->lock);

    database_lru_node_t* node = hashmap_get(&lru->cache, path);
    if (node == NULL) {
        platform_unlock(&lru->lock);
        return;
    }

    // Remove from hashmap
    hashmap_remove(&lru->cache, path);

    // Remove from list
    if (node->previous != NULL) {
        node->previous->next = node->next;
    }
    if (node->next != NULL) {
        node->next->previous = node->previous;
    }
    if (node == lru->first) {
        lru->first = node->next;
    }
    if (node == lru->last) {
        lru->last = node->previous;
    }

    // Update memory tracking
    lru->current_memory -= node->memory_size;
    lru->entry_count--;

    // Free node
    lru_node_destroy(node);

    platform_unlock(&lru->lock);
}

uint8_t database_lru_cache_contains(database_lru_cache_t* lru, path_t* path) {
    if (lru == NULL || path == NULL) {
        return 0;
    }

    platform_lock(&lru->lock);
    database_lru_node_t* node = hashmap_get(&lru->cache, path);
    platform_unlock(&lru->lock);

    return node != NULL ? 1 : 0;
}

void database_lru_cache_clear(database_lru_cache_t* lru) {
    if (lru == NULL) return;

    platform_lock(&lru->lock);

    // Free all nodes
    database_lru_node_t* node = lru->first;
    while (node != NULL) {
        database_lru_node_t* next = node->next;
        if (node->path != NULL) {
            path_destroy(node->path);
        }
        if (node->value != NULL) {
            identifier_destroy(node->value);
        }
        free(node);
        node = next;
    }

    // Clear hashmap
    hashmap_clear(&lru->cache);

    lru->first = NULL;
    lru->last = NULL;
    lru->current_memory = 0;
    lru->entry_count = 0;

    platform_unlock(&lru->lock);
}

size_t database_lru_cache_size(database_lru_cache_t* lru) {
    if (lru == NULL) return 0;

    platform_lock(&lru->lock);
    size_t size = lru->entry_count;
    platform_unlock(&lru->lock);

    return size;
}