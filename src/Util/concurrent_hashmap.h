//
// Concurrent Hashmap - Lock-free reads with striped write locks
//

#ifndef WAVEDB_CONCURRENT_HASHMAP_H
#define WAVEDB_CONCURRENT_HASHMAP_H

#include <stdint.h>
#include <stddef.h>
#include <stdatomic.h>
#include "threadding.h"

#ifdef __cplusplus
extern "C" {
#endif

// Forward declarations
typedef struct chash_entry_t chash_entry_t;
typedef struct chash_stripe_t chash_stripe_t;
typedef struct concurrent_hashmap_t concurrent_hashmap_t;

// Function pointer types
typedef size_t (*chash_hash_fn)(const void* key);
typedef int (*chash_compare_fn)(const void* a, const void* b);
typedef void* (*chash_key_dup_fn)(const void* key);
typedef void (*chash_key_free_fn)(void* key);

/**
 * Hashmap entry - lives in collision chain
 */
struct chash_entry_t {
    void* key;                          // Key (owned by entry)
    _Atomic(void*) value;               // Value (atomic for lock-free reads)
    chash_entry_t* next;                // Chain for collision resolution
    uint32_t hash;                      // Cached hash value
    _Atomic uint8_t tombstone;          // 1 if deleted (for lock-free removal)
};

/**
 * Per-stripe hashmap segment with its own lock
 *
 * Lock-free read invariants:
 * - bucket_count and buckets are read atomically with memory_order_acquire
 * - resize() atomically swaps both (new buckets, new count)
 * - old_buckets holds previous bucket array for safe cleanup
 * - readers never see torn state: either (old_buckets, old_count) or (new_buckets, new_count)
 */
struct chash_stripe_t {
    PLATFORMLOCKTYPE(lock);                // Lock for write operations only
    _Atomic(chash_entry_t**) buckets;      // Current bucket array (atomic for lock-free reads)
    _Atomic(size_t) bucket_count;          // Number of buckets (atomic for lock-free reads)
    chash_entry_t** old_buckets;           // Previous bucket array (freed on cleanup)
    size_t old_bucket_count;               // Previous bucket count
    size_t entry_count;                    // Entries in this stripe (write-protected)
    size_t tombstone_count;                // Deleted entries awaiting cleanup
};

/**
 * Concurrent hashmap with striped locks
 */
struct concurrent_hashmap_t {
    chash_stripe_t* stripes;      // Array of stripes
    size_t num_stripes;           // Number of stripes (power of 2)
    size_t stripe_mask;           // Mask for stripe selection (num_stripes - 1)

    chash_hash_fn hash_fn;
    chash_compare_fn compare_fn;
    chash_key_dup_fn key_dup_fn;
    chash_key_free_fn key_free_fn;

    size_t initial_bucket_count;  // Initial buckets per stripe
    float load_factor;            // Resize threshold

    _Atomic size_t total_entries;  // Approximate total entries
};

/**
 * Create a concurrent hashmap.
 *
 * @param num_stripes Number of stripes (0 for auto-scale, must be power of 2)
 * @param initial_bucket_count Initial buckets per stripe (0 for default)
 * @param load_factor Resize threshold (0.0-1.0, 0 for default)
 * @param hash_fn Hash function
 * @param compare_fn Key comparison function
 * @param key_dup_fn Key duplication (NULL for pointer storage)
 * @param key_free_fn Key free function (NULL for no free)
 * @return New hashmap or NULL on failure
 */
concurrent_hashmap_t* concurrent_hashmap_create(
    size_t num_stripes,
    size_t initial_bucket_count,
    float load_factor,
    chash_hash_fn hash_fn,
    chash_compare_fn compare_fn,
    chash_key_dup_fn key_dup_fn,
    chash_key_free_fn key_free_fn
);

/**
 * Destroy a concurrent hashmap.
 *
 * @param map Hashmap to destroy
 */
void concurrent_hashmap_destroy(concurrent_hashmap_t* map);

/**
 * Get a value from the hashmap (lock-free).
 *
 * @param map Hashmap to query
 * @param key Key to look up
 * @return Value if found, NULL if not found
 */
void* concurrent_hashmap_get(concurrent_hashmap_t* map, const void* key);

/**
 * Check if a key exists (lock-free).
 *
 * @param map Hashmap to query
 * @param key Key to check
 * @return 1 if exists, 0 if not
 */
uint8_t concurrent_hashmap_contains(concurrent_hashmap_t* map, const void* key);

/**
 * Put a value into the hashmap.
 *
 * @param map Hashmap to update
 * @param key Key (ownership depends on key_dup_fn)
 * @param value Value to store
 * @return Old value if key existed (caller must free), NULL otherwise
 */
void* concurrent_hashmap_put(concurrent_hashmap_t* map, void* key, void* value);

/**
 * Put a value only if key does not exist.
 *
 * @param map Hashmap to update
 * @param key Key (ownership depends on key_dup_fn)
 * @param value Value to store
 * @return Existing value if key exists, NULL if inserted
 */
void* concurrent_hashmap_put_if_absent(concurrent_hashmap_t* map, void* key, void* value);

/**
 * Remove a value from the hashmap.
 *
 * @param map Hashmap to update
 * @param key Key to remove
 * @return Value if found (caller must free), NULL otherwise
 */
void* concurrent_hashmap_remove(concurrent_hashmap_t* map, const void* key);

/**
 * Get approximate entry count.
 *
 * @param map Hashmap to query
 * @return Approximate number of entries
 */
size_t concurrent_hashmap_size(concurrent_hashmap_t* map);

/**
 * Clean up tombstones and optionally shrink.
 *
 * @param map Hashmap to clean
 * @return Number of tombstones removed
 */
size_t concurrent_hashmap_cleanup(concurrent_hashmap_t* map);

#ifdef __cplusplus
}
#endif

#endif // WAVEDB_CONCURRENT_HASHMAP_H