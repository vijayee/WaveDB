//
// Lock-Free LRU Cache Implementation
//

#include "lockfree_lru.h"
#include "Util/concurrent_hashmap.h"
#include "Util/allocator.h"
#include "Util/memory_pool.h"
#include "Util/threadding.h"
#include <stdlib.h>
#include <string.h>

// Default memory limit
#define DEFAULT_MAX_MEMORY_MB 50

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
                    if (chunk != NULL && chunk->data != NULL) {
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
            if (chunk != NULL && chunk->data != NULL) {
                total += sizeof(chunk_t);
                total += chunk->data->size;
            }
        }
    }

    return total;
}

// Initialize LRU queue
static void lru_queue_init(lru_queue_t* queue) {
    // Create dummy node for empty queue
    lru_node_t* dummy = memory_pool_alloc(sizeof(lru_node_t));
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
        memory_pool_free(node, sizeof(lru_node_t));
        node = next;
    }
}

// Enqueue a node at tail (MRU position) - lock-free
static void lru_enqueue(lru_queue_t* queue, lru_node_t* node) {
    atomic_store(&node->next, NULL);

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

// Dequeue from head (LRU position) - lock-free
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
static int evict_lru_entry(lockfree_lru_shard_t* shard) {
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

        // After dequeue, the new head is what was old_head->next
        // The LRU entry is now stored in the NEW head
        lru_node_t* new_head = atomic_load(&shard->queue.head);

        if (new_head == NULL) {
            // Queue is now empty
            memory_pool_free(old_head, sizeof(lru_node_t));
            return 0;
        }

        lru_entry_t* entry = atomic_load(&new_head->entry);

        if (entry != NULL) {
            // Try to claim this entry for eviction
            if (atomic_compare_exchange_strong(&new_head->entry, &entry, NULL)) {
                // Successfully claimed, now remove from hashmap
                concurrent_hashmap_remove(shard->map, entry->path);

                // Update memory tracking
                atomic_fetch_sub(&shard->current_memory, entry->memory_size);
                atomic_fetch_sub(&shard->entry_count, 1);

                // Free entry resources
                if (entry->path != NULL) {
                    path_destroy(entry->path);
                }
                if (entry->value != NULL) {
                    identifier_destroy(entry->value);
                }
                memory_pool_free(entry, sizeof(lru_entry_t));

                // Free old head (was dummy/sentinel)
                memory_pool_free(old_head, sizeof(lru_node_t));

                return 1;  // Eviction successful
            }
        }

        // Entry was NULL (hole) or CAS failed
        // Mark old head as hole and continue
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
            1,      // Single stripe per shard (we shard at LRU level)
            16,     // Initial buckets
            0.75f,  // Load factor
            hash_path,
            compare_path,
            dup_path,
            free_path
        );

        if (shard->map == NULL) {
            // Cleanup on failure
            for (size_t j = 0; j < i; j++) {
                concurrent_hashmap_destroy(lru->shards[j].map);
                lru_queue_destroy(&lru->shards[j].queue);
            }
            free(lru->shards);
            free(lru);
            return NULL;
        }

        // Initialize LRU queue
        lru_queue_init(&shard->queue);

        atomic_init(&shard->current_memory, 0);
        atomic_init(&shard->entry_count, 0);
        shard->max_memory = shard_memory;
        atomic_init(&shard->purging, 0);
    }

    return lru;
}

void lockfree_lru_cache_destroy(lockfree_lru_cache_t* lru) {
    if (lru == NULL) return;

    for (size_t i = 0; i < lru->num_shards; i++) {
        lockfree_lru_shard_t* shard = &lru->shards[i];

        // Free all entries in hashmap
        // Note: We need to iterate and free manually
        // For now, just destroy the hashmap
        concurrent_hashmap_destroy(shard->map);

        // Destroy queue
        lru_queue_destroy(&shard->queue);
    }

    free(lru->shards);
    free(lru);
}

identifier_t* lockfree_lru_cache_get(lockfree_lru_cache_t* lru, path_t* path) {
    if (lru == NULL || path == NULL) {
        return NULL;
    }

    // 1. Find shard
    size_t shard_idx = get_shard_index(lru, path);
    lockfree_lru_shard_t* shard = &lru->shards[shard_idx];

    // 2. Lookup entry in concurrent hashmap (lock-free)
    lru_entry_t* entry = concurrent_hashmap_get(shard->map, path);
    if (entry == NULL) {
        return NULL;  // Cache miss
    }

    // 3. Read current node pointer atomically
    lru_node_t* current_node = atomic_load(&entry->node);
    if (current_node == NULL) {
        return NULL;  // Entry being purged
    }

    // 4. Create new node for MRU position
    lru_node_t* new_node = memory_pool_alloc(sizeof(lru_node_t));
    if (new_node == NULL) {
        // Fall back to returning value without promotion
        return (identifier_t*)refcounter_reference((refcounter_t*)entry->value);
    }

    atomic_init(&new_node->entry, entry);
    atomic_init(&new_node->next, NULL);

    // 5. CAS loop to atomically update entry->node
    lru_node_t* expected = current_node;
    while (expected != NULL) {
        if (atomic_compare_exchange_weak(&entry->node, &expected, new_node)) {
            break;  // Success
        }
        if (expected == NULL) {
            // Entry was purged
            memory_pool_free(new_node, sizeof(lru_node_t));
            return NULL;
        }
    }

    // 6. Mark old node as hole (lock-free)
    atomic_store(&current_node->entry, NULL);
    atomic_fetch_add(&shard->queue.hole_count, 1);

    // 7. Enqueue new node at tail (MRU)
    lru_enqueue(&shard->queue, new_node);

    // 8. Return reference-counted value
    return (identifier_t*)refcounter_reference((refcounter_t*)entry->value);
}

identifier_t* lockfree_lru_cache_put(lockfree_lru_cache_t* lru, path_t* path, identifier_t* value) {
    if (lru == NULL || path == NULL) {
        if (path != NULL) path_destroy(path);
        if (value != NULL) identifier_destroy(value);
        return NULL;
    }

    size_t shard_idx = get_shard_index(lru, path);
    lockfree_lru_shard_t* shard = &lru->shards[shard_idx];

    // Calculate memory usage
    size_t entry_memory = calculate_entry_memory(path, value);

    // Check memory budget and evict if needed
    size_t current_mem = atomic_load(&shard->current_memory);
    size_t evictions = 0;
    while (current_mem + entry_memory > shard->max_memory) {
        if (!evict_lru_entry(shard)) {
            break;  // Can't evict more
        }
        evictions++;
        current_mem = atomic_load(&shard->current_memory);
    }
    (void)evictions;  // Suppress unused warning

    // Create entry
    lru_entry_t* entry = memory_pool_alloc(sizeof(lru_entry_t));
    if (entry == NULL) {
        path_destroy(path);
        identifier_destroy(value);
        return NULL;
    }

    // Initialize entry
    entry->path = path;
    entry->value = value;
    entry->memory_size = entry_memory;
    atomic_init(&entry->node, NULL);
    refcounter_init((refcounter_t*)entry);

    // Create LRU node
    lru_node_t* node = memory_pool_alloc(sizeof(lru_node_t));
    if (node == NULL) {
        memory_pool_free(entry, sizeof(lru_entry_t));
        path_destroy(path);
        identifier_destroy(value);
        return NULL;
    }

    atomic_init(&node->entry, entry);
    atomic_init(&node->next, NULL);
    atomic_store(&entry->node, node);

    // Try to put in hashmap
    lru_entry_t* existing = concurrent_hashmap_put_if_absent(shard->map, path, entry);

    if (existing != NULL) {
        // Entry already exists, free our new entry and use existing
        memory_pool_free(node, sizeof(lru_node_t));
        memory_pool_free(entry, sizeof(lru_entry_t));
        path_destroy(path);

        // Update existing entry's value
        // TODO: Handle value replacement with CAS
        return NULL;
    }

    // Successfully inserted, add to LRU queue
    lru_enqueue(&shard->queue, node);
    atomic_fetch_add(&shard->current_memory, entry->memory_size);
    atomic_fetch_add(&shard->entry_count, 1);

    return NULL;  // No old value for new entry
}

void lockfree_lru_cache_delete(lockfree_lru_cache_t* lru, path_t* path) {
    if (lru == NULL || path == NULL) {
        return;
    }

    size_t shard_idx = get_shard_index(lru, path);
    lockfree_lru_shard_t* shard = &lru->shards[shard_idx];

    // Remove from hashmap
    lru_entry_t* entry = concurrent_hashmap_remove(shard->map, path);
    if (entry == NULL) {
        path_destroy(path);
        return;
    }

    // Mark node as hole (lock-free)
    lru_node_t* node = atomic_load(&entry->node);
    if (node != NULL) {
        atomic_store(&node->entry, NULL);
        atomic_fetch_add(&shard->queue.hole_count, 1);
    }

    // Update memory tracking
    atomic_fetch_sub(&shard->current_memory, entry->memory_size);
    atomic_fetch_sub(&shard->entry_count, 1);

    // Free entry resources
    if (entry->path != NULL) {
        path_destroy(entry->path);
    }
    if (entry->value != NULL) {
        identifier_destroy(entry->value);
    }
    memory_pool_free(entry, sizeof(lru_entry_t));

    // Free the input path since we took ownership
    path_destroy(path);
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

// Maximum holes to purge per call
#define MAX_PURGE_BATCH 100

size_t lockfree_lru_cache_purge(lockfree_lru_cache_t* lru, size_t max_batch) {
    if (lru == NULL) return 0;

    if (max_batch == 0) {
        max_batch = MAX_PURGE_BATCH;
    }

    size_t total_purged = 0;

    for (size_t i = 0; i < lru->num_shards; i++) {
        lockfree_lru_shard_t* shard = &lru->shards[i];

        // Try to claim purge ownership
        uint8_t expected = 0;
        if (!atomic_compare_exchange_strong(&shard->purging, &expected, 1)) {
            continue;  // Another thread is purging this shard
        }

        // Drain holes from head of queue
        size_t holes_purged = 0;
        while (atomic_load(&shard->queue.hole_count) > 0 && holes_purged < max_batch) {
            lru_node_t* head = atomic_load(&shard->queue.head);
            lru_node_t* next = atomic_load(&head->next);

            if (next == NULL) {
                break;  // Queue empty (only dummy left)
            }

            lru_entry_t* entry = atomic_load(&next->entry);
            if (entry == NULL) {
                // This is a hole, advance head
                if (atomic_compare_exchange_strong(&shard->queue.head, &head, next)) {
                    memory_pool_free(head, sizeof(lru_node_t));
                    atomic_fetch_sub(&shard->queue.hole_count, 1);
                    holes_purged++;
                    total_purged++;
                }
            } else {
                break;  // Non-hole entry, stop draining
            }
        }

        atomic_store(&shard->purging, 0);
    }

    return total_purged;
}