//
// Lock-Free LRU Cache Implementation
// Based on eBay's high-throughput LRU design
//
// Key insight from eBay: No hazard pointers needed!
// - Reference counting on entries provides safety
// - Remove from map BEFORE freeing
// - Purge holes periodically in batches
//

#include "lockfree_lru.h"
#include "Util/concurrent_hashmap.h"
#include "Util/allocator.h"
#include "Util/threadding.h"
#include <stdlib.h>
#include <string.h>

// Default memory limit
#define DEFAULT_MAX_MEMORY_MB 50

// Forward declarations
static void lru_entry_destroy(lru_entry_t* entry);

// Hash function for path_t*
static size_t hash_path(const void* key) {
    const path_t* path = (const path_t*)key;
    if (path == NULL) return 0;

    size_t hash = 0;
    size_t len = path_length((path_t*)path);

    for (size_t i = 0; i < len; i++) {
        identifier_t* id = path_get((path_t*)path, i);
        if (id != NULL) {
            for (int j = 0; j < id->chunks.length; j++) {
                chunk_t* chunk = id->chunks.data[j];
                if (chunk != NULL && chunk->data != NULL) {
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
static int compare_path(const void* a, const void* b) {
    return path_compare((path_t*)a, (path_t*)b);
}

// Duplicate path for hashmap key
static void* dup_path(const void* key) {
    return path_copy((path_t*)key);
}

// Free path key
static void free_path(void* key) {
    path_destroy((path_t*)key);
}

// Get shard index for a path
static size_t get_shard_index(const lockfree_lru_cache_t* lru, const path_t* path) {
    size_t hash = hash_path(path);
    return hash % lru->num_shards;
}

// Calculate approximate memory usage for a cache entry
static size_t calculate_entry_memory(path_t* path, identifier_t* value) {
    size_t total = sizeof(lru_entry_t) + sizeof(lru_node_t);

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
                        total += sizeof(chunk_t);
                        total += chunk->data->size;
                    }
                }
            }
        }
    }

    // Value memory
    if (value != NULL) {
        total += sizeof(identifier_t);
        total += (size_t)value->chunks.capacity * sizeof(chunk_t*);
        for (int j = 0; j < value->chunks.length; j++) {
            chunk_t* chunk = value->chunks.data[j];
            if (chunk != NULL) {
                total += sizeof(chunk_t);
                total += chunk->data->size;
            }
        }
    }

    return total;
}

// Entry lifecycle
static void lru_entry_destroy(lru_entry_t* entry) {
    if (entry == NULL) return;

    // Free path (this is our copy)
    if (entry->path != NULL) {
        path_destroy(entry->path);
    }

    // Free value (we own it)
    if (entry->value != NULL) {
        identifier_destroy(entry->value);
    }

    // Free the entry struct itself
    free(entry);
}

// Release a reference to an entry, destroying it if refcount reaches 0
static void lru_entry_release(lru_entry_t* entry) {
    if (entry == NULL) return;

    refcounter_dereference(&entry->refcounter);

    // Check if we should free
    if (refcounter_count(&entry->refcounter) == 0) {
        lru_entry_destroy(entry);
    }
}

// Initialize LRU queue
static void lru_queue_init(lru_queue_t* queue) {
    // Create dummy node for sentinel
    lru_node_t* dummy = calloc(1, sizeof(lru_node_t));
    atomic_init(&dummy->entry, NULL);
    atomic_init(&dummy->next, NULL);

    atomic_init(&queue->head, dummy);
    atomic_init(&queue->tail, dummy);
    atomic_init(&queue->node_count, 0);
    atomic_init(&queue->hole_count, 0);
}

// Destroy LRU queue
static void lru_queue_destroy(lru_queue_t* queue) {
    lru_node_t* node = atomic_load(&queue->head);
    while (node != NULL) {
        lru_node_t* next = atomic_load(&node->next);
        free(node);
        node = next;
    }
}

// Enqueue at tail (MRU position)
static void lru_enqueue(lru_queue_t* queue, lru_node_t* node) {
    while (1) {
        lru_node_t* tail = atomic_load(&queue->tail);
        lru_node_t* next = atomic_load(&tail->next);

        if (tail == atomic_load(&queue->tail)) {
            if (next == NULL) {
                if (atomic_compare_exchange_weak(&tail->next, &next, node)) {
                    atomic_compare_exchange_weak(&queue->tail, &tail, node);
                    atomic_fetch_add(&queue->node_count, 1);
                    return;
                }
            } else {
                atomic_compare_exchange_weak(&queue->tail, &tail, next);
            }
        }
    }
}

// Dequeue from head (LRU position)
// Returns the old head (to be freed), or NULL if empty
static lru_node_t* lru_dequeue(lru_queue_t* queue) {
    while (1) {
        lru_node_t* head = atomic_load(&queue->head);
        lru_node_t* tail = atomic_load(&queue->tail);
        lru_node_t* next = atomic_load(&head->next);

        if (head == atomic_load(&queue->head)) {
            if (head == tail) {
                if (next == NULL) {
                    return NULL;  // Queue empty
                }
                atomic_compare_exchange_weak(&queue->tail, &tail, next);
            } else {
                if (atomic_compare_exchange_weak(&queue->head, &head, next)) {
                    atomic_fetch_sub(&queue->node_count, 1);
                    return head;  // Return old head
                }
            }
        }
    }
}

// Maximum entries to check during eviction before giving up
#define MAX_EVICTION_SCAN 100

// Evict least recently used entry
// Caller must hold shard->lock
static int evict_lru_entry_locked(lockfree_lru_shard_t* shard) {
    int scanned = 0;

    size_t node_count = atomic_load(&shard->queue.node_count);
    if (node_count == 0) {
        return 0;  // Queue empty
    }

    while (scanned < MAX_EVICTION_SCAN) {
        lru_node_t* old_head = lru_dequeue(&shard->queue);
        if (old_head == NULL) {
            return 0;  // Queue empty
        }

        lru_node_t* new_head = atomic_load(&shard->queue.head);

        if (new_head == NULL) {
            free(old_head);
            return 0;
        }

        lru_entry_t* entry = atomic_load(&new_head->entry);

        if (entry != NULL) {
            // Mark entry as being evicted
            atomic_store(&entry->node, NULL);
            atomic_store(&new_head->entry, NULL);

            // Remove from hashmap - no new references can be acquired
            concurrent_hashmap_remove(shard->map, entry->path);

            // Update tracking
            atomic_fetch_sub(&shard->current_memory, entry->memory_size);
            atomic_fetch_sub(&shard->entry_count, 1);

            // Release our reference - if count hits 0, free immediately
            refcounter_dereference(&entry->refcounter);
            if (refcounter_count(&entry->refcounter) == 0) {
                lru_entry_destroy(entry);
            }

            free(old_head);
            return 1;  // Eviction successful
        }

        // Entry was NULL (hole)
        atomic_store(&old_head->entry, NULL);
        atomic_fetch_add(&shard->queue.hole_count, 1);
        scanned++;
    }

    return 0;  // No entries evicted
}

lockfree_lru_cache_t* lockfree_lru_cache_create(size_t max_memory_bytes, uint16_t num_shards) {
    // Auto-scale shard count
    if (num_shards == 0) {
        int cores = platform_core_count();
        num_shards = (uint16_t)(cores * 4);
        if (num_shards < 64) num_shards = 64;
        if (num_shards > 256) num_shards = 256;
    }

    lockfree_lru_cache_t* lru = get_clear_memory(sizeof(lockfree_lru_cache_t));
    if (lru == NULL) return NULL;

    lru->shards = get_clear_memory(num_shards * sizeof(lockfree_lru_shard_t));
    if (lru->shards == NULL) {
        free(lru);
        return NULL;
    }

    lru->num_shards = num_shards;

    size_t total_memory = (max_memory_bytes == 0) ?
        DEFAULT_MAX_MEMORY_MB * 1024 * 1024 :
        max_memory_bytes;
    lru->total_max_memory = total_memory;

    size_t shard_memory = total_memory / num_shards;

    // Initialize each shard
    for (size_t i = 0; i < num_shards; i++) {
        lockfree_lru_shard_t* shard = &lru->shards[i];

        // Create concurrent hashmap for this shard
        shard->map = concurrent_hashmap_create(
            16,      // num_stripes
            64,      // initial_bucket_count
            0.75f,   // load_factor
            hash_path,
            compare_path,
            dup_path,
            free_path
        );
        if (shard->map == NULL) {
            // Cleanup
            for (size_t j = 0; j < i; j++) {
                concurrent_hashmap_destroy(lru->shards[j].map);
                lru_queue_destroy(&lru->shards[j].queue);
                platform_lock_destroy(&lru->shards[j].lock);
            }
            free(lru->shards);
            free(lru);
            return NULL;
        }

        lru_queue_init(&shard->queue);
        atomic_init(&shard->current_memory, 0);
        atomic_init(&shard->entry_count, 0);
        shard->max_memory = shard_memory;
        atomic_init(&shard->purging, 0);
        platform_lock_init(&shard->lock);
    }

    return lru;
}

void lockfree_lru_cache_destroy(lockfree_lru_cache_t* lru) {
    if (lru == NULL) return;

    for (size_t i = 0; i < lru->num_shards; i++) {
        lockfree_lru_shard_t* shard = &lru->shards[i];

        platform_lock(&shard->lock);

        // Walk the queue and release all entries
        lru_node_t* node = atomic_load(&shard->queue.head);
        while (node != NULL) {
            lru_node_t* next = atomic_load(&node->next);
            lru_entry_t* entry = atomic_load(&node->entry);

            if (entry != NULL) {
                // Release the hashmap's reference
                lru_entry_release(entry);
            }

            node = next;
        }

        // Now destroy the hashmap (this frees the path copies)
        concurrent_hashmap_destroy(shard->map);

        // Destroy queue (free nodes)
        lru_queue_destroy(&shard->queue);

        platform_unlock(&shard->lock);
        platform_lock_destroy(&shard->lock);
    }

    free(lru->shards);
    free(lru);
}

identifier_t* lockfree_lru_cache_get(lockfree_lru_cache_t* lru, path_t* path) {
    if (lru == NULL || path == NULL) {
        return NULL;
    }

    // Find shard
    size_t shard_idx = get_shard_index(lru, path);
    lockfree_lru_shard_t* shard = &lru->shards[shard_idx];

    // MINIMAL LOCK: Hold lock only for lookup + try_reference
    // The concurrent_hashmap_get is lock-free, but we need the lock
    // to prevent race between lookup and entry removal.
    //
    // This is still faster than sharded LRU because:
    // 1. The hashmap lookup is lock-free (no lock during traversal)
    // 2. Lock is held for minimal time (just refcount increment)
    platform_lock(&shard->lock);

    // Lookup entry in hashmap
    lru_entry_t* entry = concurrent_hashmap_get(shard->map, path);
    if (entry == NULL) {
        platform_unlock(&shard->lock);
        return NULL;  // Cache miss
    }

    // Try to reference the entry while holding the lock
    if (!refcounter_try_reference(&entry->refcounter)) {
        platform_unlock(&shard->lock);
        return NULL;  // Entry is being destroyed
    }

    // Release lock - we now have a reference
    platform_unlock(&shard->lock);

    // Check if entry is still valid (not being evicted)
    lru_node_t* current_node = atomic_load(&entry->node);
    if (current_node == NULL) {
        lru_entry_release(entry);
        return NULL;
    }

    // Return reference-counted value
    identifier_t* result = (identifier_t*)refcounter_reference((refcounter_t*)entry->value);

    // Release our reference on the entry
    lru_entry_release(entry);

    return result;
}

identifier_t* lockfree_lru_cache_put(lockfree_lru_cache_t* lru, path_t* path, identifier_t* value) {
    if (lru == NULL || path == NULL) {
        if (path != NULL) path_destroy(path);
        if (value != NULL) identifier_destroy(value);
        return NULL;
    }

    size_t shard_idx = get_shard_index(lru, path);
    lockfree_lru_shard_t* shard = &lru->shards[shard_idx];

    // Acquire shard lock
    platform_lock(&shard->lock);

    // Calculate memory usage
    size_t entry_memory = calculate_entry_memory(path, value);

    // Check memory budget and evict if needed
    size_t current_mem = atomic_load(&shard->current_memory);
    while (current_mem + entry_memory > shard->max_memory) {
        if (!evict_lru_entry_locked(shard)) {
            break;  // Can't evict more
        }
        current_mem = atomic_load(&shard->current_memory);
    }

    // Check if entry already exists
    lru_entry_t* existing = concurrent_hashmap_get(shard->map, path);
    if (existing != NULL) {
        // Entry exists - try to update value
        if (refcounter_try_reference(&existing->refcounter)) {
            // Successfully referenced - we can safely update
            identifier_t* old_value = existing->value;
            existing->value = value;

            // Update memory tracking
            size_t new_memory = calculate_entry_memory(existing->path, value);
            if (new_memory > existing->memory_size) {
                atomic_fetch_add(&shard->current_memory, new_memory - existing->memory_size);
            } else if (existing->memory_size > new_memory) {
                atomic_fetch_sub(&shard->current_memory, existing->memory_size - new_memory);
            }
            existing->memory_size = new_memory;

            // Increment version for optimistic reads
            atomic_fetch_add(&existing->version, 1);

            // Release our reference
            lru_entry_release(existing);

            // Free the input path since existing entry has its own copy
            path_destroy(path);

            platform_unlock(&shard->lock);
            return old_value;  // Caller must destroy old value
        }
        // try_reference failed - entry is being destroyed
        // Fall through to create new entry
    }

    // Create new entry
    lru_entry_t* entry = calloc(1, sizeof(lru_entry_t));
    if (entry == NULL) {
        platform_unlock(&shard->lock);
        path_destroy(path);
        identifier_destroy(value);
        return NULL;
    }

    // Initialize entry with refcount = 1 (held by hashmap)
    entry->path = path;
    entry->value = value;
    entry->memory_size = entry_memory;
    atomic_init(&entry->node, NULL);
    atomic_init(&entry->version, 0);
    refcounter_init(&entry->refcounter);  // Starts at 1

    // Create LRU node
    lru_node_t* node = calloc(1, sizeof(lru_node_t));
    if (node == NULL) {
        free(entry);
        platform_unlock(&shard->lock);
        path_destroy(path);
        identifier_destroy(value);
        return NULL;
    }

    atomic_init(&node->entry, entry);
    atomic_init(&node->next, NULL);
    atomic_store(&entry->node, node);

    // Put in hashmap
    concurrent_hashmap_put(shard->map, path, entry);

    // Add to LRU queue
    lru_enqueue(&shard->queue, node);
    atomic_fetch_add(&shard->current_memory, entry->memory_size);
    atomic_fetch_add(&shard->entry_count, 1);

    platform_unlock(&shard->lock);
    return NULL;  // No old value for new entry
}

void lockfree_lru_cache_delete(lockfree_lru_cache_t* lru, path_t* path) {
    if (lru == NULL || path == NULL) {
        return;
    }

    size_t shard_idx = get_shard_index(lru, path);
    lockfree_lru_shard_t* shard = &lru->shards[shard_idx];

    platform_lock(&shard->lock);

    // Remove from hashmap
    lru_entry_t* entry = concurrent_hashmap_remove(shard->map, path);
    if (entry == NULL) {
        platform_unlock(&shard->lock);
        return;  // Entry not found, caller keeps path ownership
    }

    // Mark node as hole
    lru_node_t* node = atomic_load(&entry->node);
    if (node != NULL) {
        atomic_store(&node->entry, NULL);
        atomic_fetch_add(&shard->queue.hole_count, 1);
    }

    // Update memory tracking
    atomic_fetch_sub(&shard->current_memory, entry->memory_size);
    atomic_fetch_sub(&shard->entry_count, 1);

    // Release our reference - if count hits 0, free immediately
    refcounter_dereference(&entry->refcounter);
    if (refcounter_count(&entry->refcounter) == 0) {
        lru_entry_destroy(entry);
    }

    platform_unlock(&shard->lock);
}

size_t lockfree_lru_cache_size(lockfree_lru_cache_t* lru) {
    if (lru == NULL) return 0;

    size_t total = 0;
    for (size_t i = 0; i < lru->num_shards; i++) {
        total += atomic_load(&lru->shards[i].entry_count);
    }
    return total;
}

size_t lockfree_lru_cache_memory(lockfree_lru_cache_t* lru) {
    if (lru == NULL) return 0;

    size_t total = 0;
    for (size_t i = 0; i < lru->num_shards; i++) {
        total += atomic_load(&lru->shards[i].current_memory);
    }
    return total;
}

uint8_t lockfree_lru_cache_contains(lockfree_lru_cache_t* lru, path_t* path) {
    if (lru == NULL || path == NULL) return 0;

    size_t shard_idx = get_shard_index(lru, path);
    lockfree_lru_shard_t* shard = &lru->shards[shard_idx];

    // Minimal lock for contains check
    platform_lock(&shard->lock);
    lru_entry_t* entry = concurrent_hashmap_get(shard->map, path);
    platform_unlock(&shard->lock);

    return entry != NULL ? 1 : 0;
}

void lockfree_lru_cache_clear(lockfree_lru_cache_t* lru) {
    if (lru == NULL) return;

    for (size_t i = 0; i < lru->num_shards; i++) {
        lockfree_lru_shard_t* shard = &lru->shards[i];
        platform_lock(&shard->lock);

        // Release all entries
        lru_node_t* node = atomic_load(&shard->queue.head);
        while (node != NULL) {
            lru_entry_t* entry = atomic_load(&node->entry);
            if (entry != NULL) {
                lru_entry_release(entry);
            }
            node = atomic_load(&node->next);
        }

        // Clear hashmap
        concurrent_hashmap_destroy(shard->map);
        shard->map = concurrent_hashmap_create(
            16, 64, 0.75f, hash_path, compare_path, dup_path, free_path);

        // Reinitialize queue
        lru_queue_destroy(&shard->queue);
        lru_queue_init(&shard->queue);

        atomic_store(&shard->current_memory, 0);
        atomic_store(&shard->entry_count, 0);

        platform_unlock(&shard->lock);
    }
}

size_t lockfree_lru_cache_purge(lockfree_lru_cache_t* lru, size_t max_batch) {
    if (lru == NULL) return 0;

    size_t total_purged = 0;

    for (size_t i = 0; i < lru->num_shards; i++) {
        lockfree_lru_shard_t* shard = &lru->shards[i];

        // Try to acquire purge lock
        uint8_t expected = 0;
        if (!atomic_compare_exchange_strong(&shard->purging, &expected, 1)) {
            continue;  // Another purge in progress
        }

        size_t holes_purged = 0;
        size_t to_purge = (max_batch == 0) ? SIZE_MAX : max_batch;

        // Try to remove holes from the head of the queue
        while (holes_purged < to_purge) {
            lru_node_t* head = atomic_load(&shard->queue.head);
            lru_node_t* next = atomic_load(&head->next);

            if (next == NULL) {
                break;  // Queue is empty (only dummy node)
            }

            lru_entry_t* entry = atomic_load(&next->entry);
            if (entry != NULL) {
                break;  // Not a hole
            }

            // Try to move head forward (this is a hole)
            if (atomic_compare_exchange_weak(&shard->queue.head, &head, next)) {
                atomic_fetch_sub(&shard->queue.node_count, 1);
                atomic_fetch_sub(&shard->queue.hole_count, 1);
                free(head);  // Free the hole node
                holes_purged++;
            }
        }

        atomic_store(&shard->purging, 0);
        total_purged += holes_purged;
    }

    return total_purged;
}