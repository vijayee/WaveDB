//
// Hazard Pointers - Lock-Free Memory Reclamation
//
// Implements hazard pointers for safe lock-free data structure operations.
// Each thread publishes pointers it's accessing; before freeing an object,
// we check if any thread has published a hazard pointer to it.
//

#ifndef WAVEDB_HAZARD_POINTERS_H
#define WAVEDB_HAZARD_POINTERS_H

#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>

#ifdef __cplusplus
extern "C" {
#endif

// Maximum hazard pointer slots per thread
#define HP_MAX_SLOTS 4

// Maximum retired objects per thread before forced scan
// Higher threshold = fewer scans = better throughput, but more memory usage
#define HP_RETIRE_THRESHOLD 64

// Reclaim function type - called to free a retired object
typedef void (*hp_reclaim_func_t)(void* ptr);

// Single hazard pointer slot
typedef struct {
    _Atomic(void*) pointer;      // Protected pointer
    _Atomic(uintptr_t) version;  // Version counter for ABA prevention
} hp_slot_t;

// Retired object waiting for reclamation
typedef struct hp_retired_t hp_retired_t;
struct hp_retired_t {
    void* ptr;              // Pointer to retired object
    hp_reclaim_func_t reclaim;  // Function to call to free the object
    hp_retired_t* next;     // Next in linked list
};

// Per-thread hazard pointer context
typedef struct hp_context_t {
    hp_slot_t slots[HP_MAX_SLOTS];  // Hazard pointer slots
    hp_retired_t* retired_list;      // List of retired objects
    size_t retired_count;            // Number of retired objects
    _Atomic(int) active;            // Is this context active?
    struct hp_context_t* next;      // Next context in registry
} hp_context_t;

// Global registry of all thread contexts
typedef struct {
    _Atomic(hp_context_t*) head;   // Head of context list
    _Atomic(size_t) context_count;  // Number of registered contexts
} hp_registry_t;

/**
 * Initialize global hazard pointer registry.
 * Call once at program start.
 *
 * @return 0 on success, -1 on failure
 */
int hp_init(void);

/**
 * Destroy global hazard pointer registry.
 * Call at program shutdown. Frees all contexts and retired objects.
 */
void hp_destroy(void);

/**
 * Reset global hazard pointer registry.
 * Call between tests to clear state.
 * Frees all contexts and retired objects, then reinitializes.
 */
void hp_reset(void);

/**
 * Register current thread for hazard pointer use.
 * Call at thread start. Creates a per-thread context.
 *
 * @return Thread context, or NULL on failure
 */
hp_context_t* hp_register_thread(void);

/**
 * Unregister current thread.
 * Call at thread end. Reclaims all retired objects.
 *
 * @param ctx Thread context from hp_register_thread()
 */
void hp_unregister_thread(hp_context_t* ctx);

/**
 * Set a hazard pointer to protect an object.
 * After this call, the object cannot be freed until hp_release().
 *
 * @param ctx  Thread context
 * @param ptr  Pointer to protect
 * @param slot Slot index (0 to HP_MAX_SLOTS-1)
 */
void hp_acquire(hp_context_t* ctx, void* ptr, size_t slot);

/**
 * Clear a hazard pointer.
 * The object may now be freed by other threads.
 *
 * @param ctx  Thread context
 * @param slot Slot index
 */
void hp_release(hp_context_t* ctx, size_t slot);

/**
 * Retire an object for deferred reclamation.
 * The object will be freed when no hazard pointers reference it.
 *
 * @param ctx     Thread context
 * @param ptr     Pointer to retire
 * @param reclaim Function to call to free the object (or NULL to just free())
 */
void hp_retire(hp_context_t* ctx, void* ptr, hp_reclaim_func_t reclaim);

/**
 * Scan retired objects and reclaim those not protected.
 * Called automatically when retired count exceeds threshold.
 *
 * @param ctx Thread context
 */
void hp_scan(hp_context_t* ctx);

/**
 * Check if any thread has a hazard pointer to an object.
 *
 * @param ctx Thread context (for registry access)
 * @param ptr Pointer to check
 * @return 1 if protected, 0 if not
 */
int hp_is_protected(hp_context_t* ctx, void* ptr);

/**
 * Check if a thread context is still valid in the registry.
 * Used to verify thread-local contexts haven't been invalidated.
 *
 * @param ctx Thread context to check
 * @return 1 if valid and active, 0 if invalid
 */
int hp_is_context_valid(hp_context_t* ctx);

#ifdef __cplusplus
}
#endif

#endif // WAVEDB_HAZARD_POINTERS_H