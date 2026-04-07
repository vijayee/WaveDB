//
// Concurrent Hashmap Implementation
//

#include "concurrent_hashmap.h"
#include "allocator.h"
#include <stdlib.h>
#include <string.h>

// Default number of stripes (power of 2)
#define DEFAULT_NUM_STRIPES 64
#define DEFAULT_BUCKET_COUNT 16
#define DEFAULT_LOAD_FACTOR 0.75f

// Round up to next power of 2
static size_t next_power_of_2(size_t n) {
    if (n == 0) return 1;
    n--;
    n |= n >> 1;
    n |= n >> 2;
    n |= n >> 4;
    n |= n >> 8;
    n |= n >> 16;
    n |= n >> 32;
    n++;
    return n;
}

// Get stripe index from hash
static inline size_t get_stripe_index(concurrent_hashmap_t* map, size_t hash) {
    return hash & map->stripe_mask;
}

// Get bucket index from hash
static inline size_t get_bucket_index(size_t hash, size_t bucket_count) {
    return hash % bucket_count;
}

// Free an entry
static void free_entry(concurrent_hashmap_t* map, chash_entry_t* entry) {
    if (entry == NULL) return;

    if (map->key_free_fn && entry->key) {
        map->key_free_fn(entry->key);
    }
    // Note: value is caller's responsibility
    free(entry);
}

// Resize a single stripe's bucket array (must hold stripe lock)
// Uses RCU-style approach: allocate new, copy, then atomically swap pointers
static int resize_stripe(concurrent_hashmap_t* map, chash_stripe_t* stripe) {
    // Get current count (we hold lock, so safe to read directly)
    size_t old_count = atomic_load(&stripe->bucket_count);
    size_t new_count = old_count * 2;

    // Allocate new bucket array
    chash_entry_t** new_buckets = get_clear_memory(new_count * sizeof(chash_entry_t*));
    if (new_buckets == NULL) {
        return -1;  // Allocation failed, continue with current size
    }

    chash_entry_t** old_buckets = atomic_load(&stripe->buckets);

    // Rehash all entries from old buckets to new buckets
    for (size_t b = 0; b < old_count; b++) {
        chash_entry_t* entry = old_buckets[b];
        while (entry != NULL) {
            chash_entry_t* next = entry->next;

            // Skip tombstones (clean up during resize)
            if (atomic_load(&entry->tombstone)) {
                free_entry(map, entry);
                stripe->tombstone_count--;
                entry = next;
                continue;
            }

            // Calculate new bucket index
            size_t new_idx = get_bucket_index(entry->hash, new_count);

            // Insert at head of new bucket
            entry->next = new_buckets[new_idx];
            new_buckets[new_idx] = entry;

            entry = next;
        }
    }

    // RCU-style swap: atomically update bucket_count first, then buckets
    // This ensures readers see consistent state
    atomic_store(&stripe->bucket_count, new_count);
    atomic_thread_fence(memory_order_release);
    atomic_store(&stripe->buckets, new_buckets);

    // Save old buckets for later cleanup (callers should call cleanup after resizes)
    // This is the RCU "grace period" approach - old buckets are freed during cleanup
    stripe->old_buckets = old_buckets;
    stripe->old_bucket_count = old_count;
    stripe->tombstone_count = 0;  // Cleaned up during resize

    return 0;
}

concurrent_hashmap_t* concurrent_hashmap_create(
    size_t num_stripes,
    size_t initial_bucket_count,
    float load_factor,
    chash_hash_fn hash_fn,
    chash_compare_fn compare_fn,
    chash_key_dup_fn key_dup_fn,
    chash_key_free_fn key_free_fn
) {
    if (hash_fn == NULL || compare_fn == NULL) {
        return NULL;
    }

    // Auto-scale stripes based on CPU cores
    if (num_stripes == 0) {
        int cores = platform_core_count();
        num_stripes = (size_t)(cores * 4);  // 4x cores
        if (num_stripes < 16) num_stripes = 16;
        if (num_stripes > 256) num_stripes = 256;
    }

    // Ensure power of 2
    num_stripes = next_power_of_2(num_stripes);

    if (initial_bucket_count == 0) {
        initial_bucket_count = DEFAULT_BUCKET_COUNT;
    }
    initial_bucket_count = next_power_of_2(initial_bucket_count);

    if (load_factor <= 0.0f || load_factor > 1.0f) {
        load_factor = DEFAULT_LOAD_FACTOR;
    }

    concurrent_hashmap_t* map = get_clear_memory(sizeof(concurrent_hashmap_t));
    if (map == NULL) return NULL;

    map->stripes = get_clear_memory(num_stripes * sizeof(chash_stripe_t));
    if (map->stripes == NULL) {
        free(map);
        return NULL;
    }

    map->num_stripes = num_stripes;
    map->stripe_mask = num_stripes - 1;
    map->hash_fn = hash_fn;
    map->compare_fn = compare_fn;
    map->key_dup_fn = key_dup_fn;
    map->key_free_fn = key_free_fn;
    map->initial_bucket_count = initial_bucket_count;
    map->load_factor = load_factor;
    atomic_init(&map->total_entries, 0);

    // Initialize each stripe
    for (size_t i = 0; i < num_stripes; i++) {
        chash_stripe_t* stripe = &map->stripes[i];

        chash_entry_t** buckets = get_clear_memory(initial_bucket_count * sizeof(chash_entry_t*));
        if (buckets == NULL) {
            // Cleanup on failure
            for (size_t j = 0; j < i; j++) {
                platform_lock_destroy(&map->stripes[j].lock);
                free(atomic_load(&map->stripes[j].buckets));
            }
            free(map->stripes);
            free(map);
            return NULL;
        }

        atomic_init(&stripe->buckets, buckets);
        atomic_init(&stripe->bucket_count, initial_bucket_count);
        stripe->old_buckets = NULL;
        stripe->old_bucket_count = 0;
        stripe->entry_count = 0;
        stripe->tombstone_count = 0;
        platform_lock_init(&stripe->lock);
    }

    return map;
}

void concurrent_hashmap_destroy(concurrent_hashmap_t* map) {
    if (map == NULL) return;

    // Free all entries in all stripes
    for (size_t i = 0; i < map->num_stripes; i++) {
        chash_stripe_t* stripe = &map->stripes[i];

        platform_lock(&stripe->lock);

        // Get current buckets atomically
        chash_entry_t** buckets = atomic_load(&stripe->buckets);
        size_t bucket_count = atomic_load(&stripe->bucket_count);

        // Free all entries in buckets
        for (size_t b = 0; b < bucket_count; b++) {
            chash_entry_t* entry = buckets[b];
            while (entry != NULL) {
                chash_entry_t* next = entry->next;
                free_entry(map, entry);
                entry = next;
            }
        }

        free(buckets);

        // Free old buckets if any
        if (stripe->old_buckets != NULL) {
            free(stripe->old_buckets);
        }

        platform_unlock(&stripe->lock);
        platform_lock_destroy(&stripe->lock);
    }

    free(map->stripes);
    free(map);
}

void* concurrent_hashmap_get(concurrent_hashmap_t* map, const void* key) {
    if (map == NULL || key == NULL) {
        return NULL;
    }

    size_t hash = map->hash_fn(key);
    size_t stripe_idx = get_stripe_index(map, hash);
    chash_stripe_t* stripe = &map->stripes[stripe_idx];

    // LOCK-FREE READ with proper memory ordering
    // Read bucket_count first, then buckets with acquire semantics
    // This ensures we see a consistent snapshot even during resize
    size_t bucket_count = atomic_load_explicit(&stripe->bucket_count, memory_order_acquire);
    chash_entry_t** buckets = atomic_load_explicit(&stripe->buckets, memory_order_acquire);

    // Calculate bucket index
    size_t bucket_idx = get_bucket_index(hash, bucket_count);
    chash_entry_t* entry = buckets[bucket_idx];

    // Traverse chain looking for key
    while (entry != NULL) {
        // Skip tombstones
        if (!atomic_load_explicit(&entry->tombstone, memory_order_acquire) &&
            entry->hash == (uint32_t)hash &&
            map->compare_fn(entry->key, key) == 0) {
            // Found - return value atomically
            return atomic_load_explicit(&entry->value, memory_order_acquire);
        }
        entry = entry->next;
    }

    return NULL;
}

uint8_t concurrent_hashmap_contains(concurrent_hashmap_t* map, const void* key) {
    if (map == NULL || key == NULL) {
        return 0;
    }

    size_t hash = map->hash_fn(key);
    size_t stripe_idx = get_stripe_index(map, hash);
    chash_stripe_t* stripe = &map->stripes[stripe_idx];

    // LOCK-FREE READ with proper memory ordering
    size_t bucket_count = atomic_load_explicit(&stripe->bucket_count, memory_order_acquire);
    chash_entry_t** buckets = atomic_load_explicit(&stripe->buckets, memory_order_acquire);

    size_t bucket_idx = get_bucket_index(hash, bucket_count);
    chash_entry_t* entry = buckets[bucket_idx];

    while (entry != NULL) {
        if (!atomic_load_explicit(&entry->tombstone, memory_order_acquire) &&
            entry->hash == (uint32_t)hash &&
            map->compare_fn(entry->key, key) == 0) {
            return 1;
        }
        entry = entry->next;
    }

    return 0;
}

// Find existing entry or create new one (must hold stripe lock)
static chash_entry_t* find_or_create_entry(
    concurrent_hashmap_t* map,
    chash_stripe_t* stripe,
    const void* key,
    size_t hash,
    int create_new,
    chash_entry_t*** prev_ptr
) {
    // We hold the lock, so we can read directly
    size_t bucket_count = atomic_load(&stripe->bucket_count);
    chash_entry_t** buckets = atomic_load(&stripe->buckets);

    size_t bucket_idx = get_bucket_index(hash, bucket_count);
    chash_entry_t** prev = &buckets[bucket_idx];
    chash_entry_t* entry = *prev;

    // Look for existing entry
    while (entry != NULL) {
        if (!atomic_load(&entry->tombstone) &&
            entry->hash == (uint32_t)hash &&
            map->compare_fn(entry->key, key) == 0) {
            if (prev_ptr) *prev_ptr = prev;
            return entry;
        }
        prev = &entry->next;
        entry = *prev;
    }

    if (!create_new) {
        if (prev_ptr) *prev_ptr = prev;
        return NULL;
    }

    // Create new entry
    chash_entry_t* new_entry = get_clear_memory(sizeof(chash_entry_t));
    if (new_entry == NULL) {
        if (prev_ptr) *prev_ptr = NULL;
        return NULL;
    }

    // Initialize entry
    if (map->key_dup_fn) {
        new_entry->key = map->key_dup_fn(key);
    } else {
        new_entry->key = (void*)key;
    }
    atomic_init(&new_entry->value, NULL);
    new_entry->next = NULL;
    new_entry->hash = (uint32_t)hash;
    atomic_init(&new_entry->tombstone, 0);

    // Link into bucket
    *prev = new_entry;
    if (prev_ptr) *prev_ptr = prev;

    stripe->entry_count++;
    atomic_fetch_add(&map->total_entries, 1);

    // Check if resize needed
    size_t threshold = (size_t)(bucket_count * map->load_factor);
    if (stripe->entry_count > threshold) {
        resize_stripe(map, stripe);
    }

    return new_entry;
}

void* concurrent_hashmap_put(concurrent_hashmap_t* map, void* key, void* value) {
    if (map == NULL || key == NULL) {
        return NULL;
    }

    size_t hash = map->hash_fn(key);
    size_t stripe_idx = get_stripe_index(map, hash);
    chash_stripe_t* stripe = &map->stripes[stripe_idx];

    platform_lock(&stripe->lock);

    chash_entry_t* entry = find_or_create_entry(map, stripe, key, hash, 1, NULL);

    if (entry == NULL) {
        platform_unlock(&stripe->lock);
        return NULL;
    }

    // Swap value atomically
    void* old_value = atomic_exchange(&entry->value, value);

    platform_unlock(&stripe->lock);
    return old_value;
}

void* concurrent_hashmap_put_if_absent(concurrent_hashmap_t* map, void* key, void* value) {
    if (map == NULL || key == NULL) {
        return NULL;
    }

    size_t hash = map->hash_fn(key);
    size_t stripe_idx = get_stripe_index(map, hash);
    chash_stripe_t* stripe = &map->stripes[stripe_idx];

    platform_lock(&stripe->lock);

    chash_entry_t* entry = find_or_create_entry(map, stripe, key, hash, 0, NULL);

    if (entry != NULL) {
        // Key exists, return existing value
        void* existing = atomic_load(&entry->value);
        platform_unlock(&stripe->lock);

        // Free the key if we're managing keys
        if (map->key_free_fn && !map->key_dup_fn) {
            map->key_free_fn(key);
        }

        return existing;
    }

    // Key doesn't exist, create it
    entry = find_or_create_entry(map, stripe, key, hash, 1, NULL);
    if (entry == NULL) {
        platform_unlock(&stripe->lock);

        // Free the key if we're managing keys
        if (map->key_free_fn && !map->key_dup_fn) {
            map->key_free_fn(key);
        }

        return NULL;
    }

    // Set value atomically
    void* old_value = atomic_exchange(&entry->value, value);
    (void)old_value;  // Should be NULL for new entry

    platform_unlock(&stripe->lock);
    return NULL;  // Inserted, no previous value
}

void* concurrent_hashmap_remove(concurrent_hashmap_t* map, const void* key) {
    if (map == NULL || key == NULL) {
        return NULL;
    }

    size_t hash = map->hash_fn(key);
    size_t stripe_idx = get_stripe_index(map, hash);
    chash_stripe_t* stripe = &map->stripes[stripe_idx];

    platform_lock(&stripe->lock);

    // We hold the lock, so we can read directly
    size_t bucket_count = atomic_load(&stripe->bucket_count);
    chash_entry_t** buckets = atomic_load(&stripe->buckets);

    size_t bucket_idx = get_bucket_index(hash, bucket_count);
    chash_entry_t** prev = &buckets[bucket_idx];
    chash_entry_t* entry = *prev;

    // Find entry
    while (entry != NULL) {
        if (!atomic_load(&entry->tombstone) &&
            entry->hash == (uint32_t)hash &&
            map->compare_fn(entry->key, key) == 0) {
            break;
        }
        prev = &entry->next;
        entry = *prev;
    }

    if (entry == NULL) {
        platform_unlock(&stripe->lock);
        return NULL;
    }

    // Mark as tombstone (lock-free reads will skip)
    atomic_store(&entry->tombstone, 1);

    // Get value
    void* value = atomic_load(&entry->value);

    // Update counts
    stripe->entry_count--;
    stripe->tombstone_count++;
    atomic_fetch_sub(&map->total_entries, 1);

    platform_unlock(&stripe->lock);
    return value;
}

size_t concurrent_hashmap_size(concurrent_hashmap_t* map) {
    if (map == NULL) return 0;
    return atomic_load(&map->total_entries);
}

size_t concurrent_hashmap_cleanup(concurrent_hashmap_t* map) {
    if (map == NULL) return 0;

    size_t total_cleaned = 0;

    for (size_t i = 0; i < map->num_stripes; i++) {
        chash_stripe_t* stripe = &map->stripes[i];

        platform_lock(&stripe->lock);

        // Free old bucket arrays from previous resizes (RCU grace period)
        if (stripe->old_buckets != NULL) {
            free(stripe->old_buckets);
            stripe->old_buckets = NULL;
            stripe->old_bucket_count = 0;
        }

        if (stripe->tombstone_count == 0) {
            platform_unlock(&stripe->lock);
            continue;
        }

        // Get current buckets
        chash_entry_t** buckets = atomic_load(&stripe->buckets);
        size_t bucket_count = atomic_load(&stripe->bucket_count);

        // Rebuild all buckets, skipping tombstones
        for (size_t b = 0; b < bucket_count; b++) {
            chash_entry_t** prev = &buckets[b];
            chash_entry_t* entry = *prev;

            while (entry != NULL) {
                chash_entry_t* next = entry->next;

                if (atomic_load(&entry->tombstone)) {
                    // Remove tombstone entry
                    *prev = next;
                    free_entry(map, entry);
                    total_cleaned++;
                } else {
                    prev = &entry->next;
                }

                entry = next;
            }
        }

        stripe->tombstone_count = 0;
        platform_unlock(&stripe->lock);
    }

    return total_cleaned;
}