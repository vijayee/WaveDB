//
// Hazard Pointers Implementation
//

#include "hazard_pointers.h"
#include "threadding.h"
#include <stdlib.h>
#include <string.h>

// Global registry of all thread contexts
static hp_registry_t g_registry;
static PLATFORMLOCKTYPE(g_lock);
static int g_initialized = 0;
static int g_lock_initialized = 0;

int hp_init(void) {
    // Initialize lock first (once)
    if (!g_lock_initialized) {
        platform_lock_init(&g_lock);
        g_lock_initialized = 1;
    }

    // Lock the global registry to ensure thread-safe initialization
    platform_lock(&g_lock);

    if (g_initialized) {
        platform_unlock(&g_lock);
        return 0;  // Already initialized
    }

    atomic_init(&g_registry.head, NULL);
    atomic_init(&g_registry.context_count, 0);
    g_initialized = 1;

    platform_unlock(&g_lock);
    return 0;
}

void hp_destroy(void) {
    if (!g_initialized) {
        return;
    }

    platform_lock(&g_lock);

    // Free all contexts and their retired objects
    hp_context_t* ctx = atomic_load(&g_registry.head);
    while (ctx != NULL) {
        hp_context_t* next = ctx->next;

        // Mark context as inactive
        atomic_store(&ctx->active, 0);

        // Free retired objects
        hp_retired_t* retired = ctx->retired_list;
        while (retired != NULL) {
            hp_retired_t* next_retired = retired->next;
            if (retired->reclaim != NULL) {
                retired->reclaim(retired->ptr);
            }
            free(retired);
            retired = next_retired;
        }

        free(ctx);
        ctx = next;
    }

    atomic_store(&g_registry.head, NULL);
    atomic_store(&g_registry.context_count, 0);

    platform_unlock(&g_lock);
    platform_lock_destroy(&g_lock);
    g_initialized = 0;
    g_lock_initialized = 0;
}

void hp_reset(void) {
    // Destroy and reinitialize
    hp_destroy();
    hp_init();
}

// Check if a context is still valid (in the registry)
int hp_is_context_valid(hp_context_t* ctx) {
    if (ctx == NULL || !g_initialized) {
        return 0;
    }

    platform_lock(&g_lock);
    hp_context_t* iter = atomic_load(&g_registry.head);
    while (iter != NULL) {
        if (iter == ctx) {
            platform_unlock(&g_lock);
            return atomic_load(&ctx->active);
        }
        iter = iter->next;
    }
    platform_unlock(&g_lock);
    return 0;
}

hp_context_t* hp_register_thread(void) {
    if (!g_initialized) {
        return NULL;
    }

    hp_context_t* ctx = calloc(1, sizeof(hp_context_t));
    if (ctx == NULL) {
        return NULL;
    }

    // Initialize slots
    for (size_t i = 0; i < HP_MAX_SLOTS; i++) {
        atomic_init(&ctx->slots[i].pointer, NULL);
        atomic_init(&ctx->slots[i].version, 0);
    }

    ctx->retired_list = NULL;
    ctx->retired_count = 0;
    atomic_init(&ctx->active, 1);

    // Add to global registry under lock
    platform_lock(&g_lock);
    ctx->next = atomic_load(&g_registry.head);
    atomic_store(&g_registry.head, ctx);
    atomic_fetch_add(&g_registry.context_count, 1);
    platform_unlock(&g_lock);

    return ctx;
}

void hp_unregister_thread(hp_context_t* ctx) {
    if (ctx == NULL || !g_initialized) {
        return;
    }

    // Mark inactive
    atomic_store(&ctx->active, 0);

    // Clear all hazard pointers
    for (size_t i = 0; i < HP_MAX_SLOTS; i++) {
        atomic_store(&ctx->slots[i].pointer, NULL);
    }

    // Scan to reclaim any remaining retired objects
    hp_scan(ctx);

    // Remove from registry under lock
    platform_lock(&g_lock);

    hp_context_t** pp = (hp_context_t**)&g_registry.head;
    while (*pp != NULL && *pp != ctx) {
        pp = (hp_context_t**)&(*pp)->next;
    }
    if (*pp == ctx) {
        *pp = ctx->next;
        atomic_fetch_sub(&g_registry.context_count, 1);
    }

    platform_unlock(&g_lock);

    // Free context
    free(ctx);
}

void hp_acquire(hp_context_t* ctx, void* ptr, size_t slot) {
    if (ctx == NULL || ptr == NULL || slot >= HP_MAX_SLOTS || !g_initialized) {
        return;
    }

    // Increment version to prevent ABA problems
    atomic_fetch_add(&ctx->slots[slot].version, 1);

    // Set hazard pointer
    atomic_store(&ctx->slots[slot].pointer, ptr);

    // Memory barrier to ensure pointer is visible before any subsequent loads
    atomic_thread_fence(memory_order_seq_cst);
}

void hp_release(hp_context_t* ctx, size_t slot) {
    if (ctx == NULL || slot >= HP_MAX_SLOTS || !g_initialized) {
        return;
    }

    // Clear hazard pointer
    atomic_store(&ctx->slots[slot].pointer, NULL);
}

void hp_retire(hp_context_t* ctx, void* ptr, hp_reclaim_func_t reclaim) {
    if (ctx == NULL || ptr == NULL || !g_initialized) {
        return;
    }

    // Create retired object entry
    hp_retired_t* retired = malloc(sizeof(hp_retired_t));
    if (retired == NULL) {
        // Allocation failed - reclaim immediately
        if (reclaim != NULL) {
            reclaim(ptr);
        } else {
            free(ptr);
        }
        return;
    }

    retired->ptr = ptr;
    retired->reclaim = reclaim;
    retired->next = ctx->retired_list;
    ctx->retired_list = retired;
    ctx->retired_count++;

    // Scan if threshold exceeded
    if (ctx->retired_count >= HP_RETIRE_THRESHOLD) {
        hp_scan(ctx);
    }
}

void hp_scan(hp_context_t* ctx) {
    if (ctx == NULL || !g_initialized) {
        return;
    }

    // Hold lock during entire scan to prevent races with new hazard pointers
    platform_lock(&g_lock);

    // Phase 1: Count and collect all hazard pointers from active threads
    size_t hp_count = 0;
    hp_context_t* iter = atomic_load(&g_registry.head);
    while (iter != NULL) {
        if (atomic_load(&iter->active)) {
            hp_count += HP_MAX_SLOTS;
        }
        iter = iter->next;
    }

    // Allocate array to hold hazard pointers
    void** hp_array = NULL;
    if (hp_count > 0) {
        hp_array = malloc(hp_count * sizeof(void*));
        if (hp_array == NULL) {
            platform_unlock(&g_lock);
            return;
        }
    }

    // Collect all hazard pointers
    size_t idx = 0;
    iter = atomic_load(&g_registry.head);
    while (iter != NULL) {
        if (atomic_load(&iter->active)) {
            for (size_t i = 0; i < HP_MAX_SLOTS; i++) {
                hp_array[idx++] = atomic_load(&iter->slots[i].pointer);
            }
        }
        iter = iter->next;
    }

    // Phase 2: Check retired objects against hazard pointers (still holding lock)
    hp_retired_t** pp = &ctx->retired_list;
    while (*pp != NULL) {
        hp_retired_t* retired = *pp;
        int protected = 0;

        // Check if any hazard pointer references this object
        for (size_t i = 0; i < hp_count; i++) {
            if (hp_array[i] == retired->ptr) {
                protected = 1;
                break;
            }
        }

        if (!protected) {
            // Safe to reclaim - remove from list
            *pp = retired->next;
            ctx->retired_count--;

            // Reclaim the object
            if (retired->reclaim != NULL) {
                retired->reclaim(retired->ptr);
            } else {
                free(retired->ptr);
            }
            free(retired);
        } else {
            // Still protected - keep in list and advance
            pp = &retired->next;
        }
    }

    // Release lock after entire scan is complete
    platform_unlock(&g_lock);

    // Free the hazard pointer array
    if (hp_array != NULL) {
        free(hp_array);
    }
}

int hp_is_protected(hp_context_t* ctx, void* ptr) {
    if (ctx == NULL || ptr == NULL || !g_initialized) {
        return 0;
    }

    platform_lock(&g_lock);

    int found = 0;
    hp_context_t* iter = atomic_load(&g_registry.head);
    while (iter != NULL && !found) {
        if (atomic_load(&iter->active)) {
            for (size_t i = 0; i < HP_MAX_SLOTS; i++) {
                if (atomic_load(&iter->slots[i].pointer) == ptr) {
                    found = 1;
                    break;
                }
            }
        }
        iter = iter->next;
    }

    platform_unlock(&g_lock);
    return found;
}