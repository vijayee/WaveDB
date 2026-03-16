//
// Memory Pool Allocator Implementation
//

#include "memory_pool.h"
#include "allocator.h"
#include "log.h"
#include <string.h>

// Global memory pool
static memory_pool_t g_pool = {0};

// Static memory pools (allocated once at init)
static uint8_t g_small_pool[MEMORY_POOL_SMALL_SIZE * MEMORY_POOL_SMALL_COUNT];
static uint8_t g_medium_pool[MEMORY_POOL_MEDIUM_SIZE * MEMORY_POOL_MEDIUM_COUNT];
static uint8_t g_large_pool[MEMORY_POOL_LARGE_SIZE * MEMORY_POOL_LARGE_COUNT];

// Initialize a size class pool
static void memory_pool_class_init(memory_pool_class_t* cls, uint8_t* pool,
                                   size_t block_size, size_t total_blocks) {
    platform_lock_init(&cls->lock);
    cls->pool_start = pool;
    cls->block_size = block_size;
    cls->total_blocks = total_blocks;
    cls->free_blocks = total_blocks;

    // Initialize free list - all blocks are free
    cls->free_list = (memory_pool_block_t*)pool;
    memory_pool_block_t* current = cls->free_list;

    for (size_t i = 0; i < total_blocks - 1; i++) {
        current->next = (memory_pool_block_t*)(pool + (i + 1) * block_size);
        current = current->next;
    }
    current->next = NULL;
}

// Destroy a size class pool
static void memory_pool_class_destroy(memory_pool_class_t* cls) {
    platform_lock_destroy(&cls->lock);
}

// Determine size class for allocation
static memory_pool_size_class_e memory_pool_get_class(size_t size) {
    if (size <= MEMORY_POOL_SMALL_SIZE) {
        return MEMORY_POOL_SMALL;
    } else if (size <= MEMORY_POOL_MEDIUM_SIZE) {
        return MEMORY_POOL_MEDIUM;
    } else if (size <= MEMORY_POOL_LARGE_SIZE) {
        return MEMORY_POOL_LARGE;
    } else {
        return MEMORY_POOL_FALLBACK;
    }
}

// Allocate from a specific class
static void* memory_pool_class_alloc(memory_pool_class_t* cls) {
    platform_lock(&cls->lock);

    if (cls->free_list == NULL) {
        // Pool exhausted
        platform_unlock(&cls->lock);
        return NULL;
    }

    // Pop from free list
    memory_pool_block_t* block = cls->free_list;
    cls->free_list = block->next;
    cls->free_blocks--;

    platform_unlock(&cls->lock);

    return block;
}

// Free to a specific class
static int memory_pool_class_free(memory_pool_class_t* cls, void* ptr) {
    // Check if pointer is in this pool's range
    uint8_t* start = cls->pool_start;
    uint8_t* end = start + cls->block_size * cls->total_blocks;

    if ((uint8_t*)ptr < start || (uint8_t*)ptr >= end) {
        return 0;  // Not in this pool
    }

    platform_lock(&cls->lock);

    // Push to free list
    memory_pool_block_t* block = (memory_pool_block_t*)ptr;
    block->next = cls->free_list;
    cls->free_list = block;
    cls->free_blocks++;

    platform_unlock(&cls->lock);

    return 1;  // Successfully freed
}

// Initialize global memory pool
void memory_pool_init(void) {
    if (g_pool.initialized) {
        return;  // Already initialized
    }

    // Initialize each size class
    memory_pool_class_init(&g_pool.classes[MEMORY_POOL_SMALL],
                           g_small_pool, MEMORY_POOL_SMALL_SIZE,
                           MEMORY_POOL_SMALL_COUNT);

    memory_pool_class_init(&g_pool.classes[MEMORY_POOL_MEDIUM],
                           g_medium_pool, MEMORY_POOL_MEDIUM_SIZE,
                           MEMORY_POOL_MEDIUM_COUNT);

    memory_pool_class_init(&g_pool.classes[MEMORY_POOL_LARGE],
                           g_large_pool, MEMORY_POOL_LARGE_SIZE,
                           MEMORY_POOL_LARGE_COUNT);

    // Reset statistics
    memset(&g_pool.stats, 0, sizeof(memory_pool_stats_t));

    g_pool.initialized = 1;

    log_info("Memory pool initialized:");
    log_info("  Small:  %d blocks × %d bytes = %d KB",
             MEMORY_POOL_SMALL_COUNT, MEMORY_POOL_SMALL_SIZE,
             (MEMORY_POOL_SMALL_COUNT * MEMORY_POOL_SMALL_SIZE) / 1024);
    log_info("  Medium: %d blocks × %d bytes = %d KB",
             MEMORY_POOL_MEDIUM_COUNT, MEMORY_POOL_MEDIUM_SIZE,
             (MEMORY_POOL_MEDIUM_COUNT * MEMORY_POOL_MEDIUM_SIZE) / 1024);
    log_info("  Large:  %d blocks × %d bytes = %d KB",
             MEMORY_POOL_LARGE_COUNT, MEMORY_POOL_LARGE_SIZE,
             (MEMORY_POOL_LARGE_COUNT * MEMORY_POOL_LARGE_SIZE) / 1024);
    log_info("  Total:  %.2f MB",
             (double)MEMORY_POOL_TOTAL_SIZE / (1024 * 1024));
}

// Destroy global memory pool
void memory_pool_destroy(void) {
    if (!g_pool.initialized) {
        return;
    }

    // Destroy each size class
    for (int i = 0; i < 3; i++) {
        memory_pool_class_destroy(&g_pool.classes[i]);
    }

    g_pool.initialized = 0;
}

// Allocate memory from pool
void* memory_pool_alloc(size_t size) {
    if (!g_pool.initialized) {
        // Pool not initialized, fallback to malloc
        return malloc(size);
    }

    memory_pool_size_class_e class = memory_pool_get_class(size);
    void* ptr = NULL;

    switch (class) {
        case MEMORY_POOL_SMALL:
            ptr = memory_pool_class_alloc(&g_pool.classes[MEMORY_POOL_SMALL]);
            if (ptr) {
                g_pool.stats.small_allocs++;
                g_pool.stats.small_pool_hits++;
            } else {
                g_pool.stats.fallback_allocs++;
                ptr = malloc(size);
            }
            break;

        case MEMORY_POOL_MEDIUM:
            ptr = memory_pool_class_alloc(&g_pool.classes[MEMORY_POOL_MEDIUM]);
            if (ptr) {
                g_pool.stats.medium_allocs++;
                g_pool.stats.medium_pool_hits++;
            } else {
                g_pool.stats.fallback_allocs++;
                ptr = malloc(size);
            }
            break;

        case MEMORY_POOL_LARGE:
            ptr = memory_pool_class_alloc(&g_pool.classes[MEMORY_POOL_LARGE]);
            if (ptr) {
                g_pool.stats.large_allocs++;
                g_pool.stats.large_pool_hits++;
            } else {
                g_pool.stats.fallback_allocs++;
                ptr = malloc(size);
            }
            break;

        case MEMORY_POOL_FALLBACK:
        default:
            // Too large for pool, use malloc
            g_pool.stats.fallback_allocs++;
            ptr = malloc(size);
            break;
    }

    return ptr;
}

// Free memory to pool
void memory_pool_free(void* ptr, size_t size) {
    if (!ptr) {
        return;
    }

    if (!g_pool.initialized) {
        // Pool not initialized, use free
        free(ptr);
        return;
    }

    memory_pool_size_class_e class = memory_pool_get_class(size);
    int freed = 0;

    // Try to free to each pool (only one will succeed)
    for (int i = 0; i < 3; i++) {
        if (memory_pool_class_free(&g_pool.classes[i], ptr)) {
            freed = 1;
            switch (i) {
                case MEMORY_POOL_SMALL:
                    g_pool.stats.small_frees++;
                    break;
                case MEMORY_POOL_MEDIUM:
                    g_pool.stats.medium_frees++;
                    break;
                case MEMORY_POOL_LARGE:
                    g_pool.stats.large_frees++;
                    break;
            }
            break;
        }
    }

    if (!freed) {
        // Not in any pool, use free
        g_pool.stats.fallback_frees++;
        free(ptr);
    }
}

// Get memory pool statistics
memory_pool_stats_t memory_pool_get_stats(void) {
    return g_pool.stats;
}

// Reset statistics
void memory_pool_reset_stats(void) {
    memset(&g_pool.stats, 0, sizeof(memory_pool_stats_t));
}

// Print memory pool statistics (for debugging)
void memory_pool_print_stats(void) {
    log_info("Memory Pool Statistics:");
    log_info("  Small allocations: %lu (pool hits: %lu, fallback: %lu)",
             g_pool.stats.small_allocs,
             g_pool.stats.small_pool_hits,
             g_pool.stats.fallback_allocs);
    log_info("  Medium allocations: %lu (pool hits: %lu, fallback: %lu)",
             g_pool.stats.medium_allocs,
             g_pool.stats.medium_pool_hits,
             g_pool.stats.fallback_allocs);
    log_info("  Large allocations: %lu (pool hits: %lu, fallback: %lu)",
             g_pool.stats.large_allocs,
             g_pool.stats.large_pool_hits,
             g_pool.stats.fallback_allocs);
    log_info("  Small frees: %lu", g_pool.stats.small_frees);
    log_info("  Medium frees: %lu", g_pool.stats.medium_frees);
    log_info("  Large frees: %lu", g_pool.stats.large_frees);
    log_info("  Fallback frees: %lu", g_pool.stats.fallback_frees);

    // Calculate pool utilization
    size_t small_used = MEMORY_POOL_SMALL_COUNT - g_pool.classes[MEMORY_POOL_SMALL].free_blocks;
    size_t medium_used = MEMORY_POOL_MEDIUM_COUNT - g_pool.classes[MEMORY_POOL_MEDIUM].free_blocks;
    size_t large_used = MEMORY_POOL_LARGE_COUNT - g_pool.classes[MEMORY_POOL_LARGE].free_blocks;

    log_info("  Pool utilization:");
    log_info("    Small:  %zu / %zu blocks (%.1f%%)",
             small_used, MEMORY_POOL_SMALL_COUNT,
             100.0 * small_used / MEMORY_POOL_SMALL_COUNT);
    log_info("    Medium: %zu / %zu blocks (%.1f%%)",
             medium_used, MEMORY_POOL_MEDIUM_COUNT,
             100.0 * medium_used / MEMORY_POOL_MEDIUM_COUNT);
    log_info("    Large:  %zu / %zu blocks (%.1f%%)",
             large_used, MEMORY_POOL_LARGE_COUNT,
             100.0 * large_used / MEMORY_POOL_LARGE_COUNT);
}