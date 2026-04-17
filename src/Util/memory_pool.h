//
// Memory Pool Allocator
//
// High-performance allocator for frequently-allocated small objects.
// Uses static memory pools to avoid malloc/free overhead.
//

#ifndef WAVEDB_MEMORY_POOL_H
#define WAVEDB_MEMORY_POOL_H

#include <stddef.h>
#include <stdint.h>
#include "../Util/threadding.h"

#ifndef container_of
#define container_of(ptr, type, member) \
    ((type*)((char*)(ptr) - offsetof(type, member)))
#endif

#ifdef __cplusplus
extern "C" {
#endif

// Configuration
#define MEMORY_POOL_SMALL_SIZE    64      // Small allocations: 1-64 bytes
#define MEMORY_POOL_MEDIUM_SIZE   256     // Medium allocations: 65-256 bytes
#define MEMORY_POOL_LARGE_SIZE    1024    // Large allocations: 257-1024 bytes

#define MEMORY_POOL_SMALL_COUNT   10000   // Number of small blocks
#define MEMORY_POOL_MEDIUM_COUNT  5000    // Number of medium blocks
#define MEMORY_POOL_LARGE_COUNT   2000    // Number of large blocks

// Calculate total pool size
#define MEMORY_POOL_TOTAL_SIZE \
    (MEMORY_POOL_SMALL_SIZE * MEMORY_POOL_SMALL_COUNT + \
     MEMORY_POOL_MEDIUM_SIZE * MEMORY_POOL_MEDIUM_COUNT + \
     MEMORY_POOL_LARGE_SIZE * MEMORY_POOL_LARGE_COUNT)

// Statistics tracking
typedef struct {
    uint64_t small_allocs;      // Number of small allocations
    uint64_t medium_allocs;      // Number of medium allocations
    uint64_t large_allocs;       // Number of large allocations
    uint64_t small_frees;        // Number of small frees
    uint64_t medium_frees;       // Number of medium frees
    uint64_t large_frees;        // Number of large frees
    uint64_t fallback_allocs;    // Allocations that fell back to malloc
    uint64_t fallback_frees;    // Frees that fell back to free
    uint64_t small_pool_hits;    // Small allocations from pool
    uint64_t medium_pool_hits;  // Medium allocations from pool
    uint64_t large_pool_hits;    // Large allocations from pool
} memory_pool_stats_t;

// Size class enumeration
typedef enum {
    MEMORY_POOL_SMALL = 0,
    MEMORY_POOL_MEDIUM = 1,
    MEMORY_POOL_LARGE = 2,
    MEMORY_POOL_FALLBACK = 3  // Too large for pool
} memory_pool_size_class_e;

// Free block header (stored at beginning of each free block)
typedef struct memory_pool_block_t {
    struct memory_pool_block_t* next;
} memory_pool_block_t;

// Per-size-class pool
typedef struct {
    PLATFORMLOCKTYPE(lock);
    memory_pool_block_t* free_list;  // Head of free blocks
    uint8_t* pool_start;              // Start of memory region
    size_t block_size;                // Size of each block
    size_t total_blocks;               // Total number of blocks
    size_t free_blocks;                // Number of free blocks
} memory_pool_class_t;

// Global memory pool
typedef struct {
    memory_pool_class_t classes[3];  // Small, medium, large
    memory_pool_stats_t stats;
    uint8_t initialized;
} memory_pool_t;

// Thread-local cache configuration
#define TLS_CACHE_SIZE 32

// Thread-local cache for fast allocation without locks
typedef struct {
    void* cache[TLS_CACHE_SIZE];
    size_t count;
    size_t block_size;
} tls_cache_t;

// Initialize global memory pool
// Must be called once before any allocations
void memory_pool_init(void);

// Destroy global memory pool
// Must be called after all allocations are freed
void memory_pool_destroy(void);

// Allocate memory from pool
// Falls back to malloc if size is too large or pool is exhausted
void* memory_pool_alloc(size_t size);

// Free memory to pool
// Falls back to free if pointer is not from pool
void memory_pool_free(void* ptr, size_t size);

// Get memory pool statistics
memory_pool_stats_t memory_pool_get_stats(void);

// Reset statistics
void memory_pool_reset_stats(void);

// Print memory pool statistics (for debugging)
void memory_pool_print_stats(void);

#ifdef __cplusplus
}
#endif

#endif // WAVEDB_MEMORY_POOL_H