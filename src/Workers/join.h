//
// Async Join - Wait for multiple async operations to complete
// Created: 2026-04-12
//

#ifndef WAVEDB_JOIN_H
#define WAVEDB_JOIN_H

#include "../Util/threadding.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    PLATFORMLOCKTYPE(lock);
    PLATFORMCONDITIONTYPE(condition);
    _Atomic int pending;       // Number of outstanding operations
    void** results;             // Array[index] = result payload from each op
    int count;                 // Total number of operations
} async_join_t;

/**
 * Create a join that waits for 'count' operations.
 *
 * @param count  Number of operations to wait for
 * @return New join or NULL on failure
 */
async_join_t* async_join_create(int count);

/**
 * Destroy a join and free all resources.
 *
 * Does NOT free the results array contents — callers own those.
 *
 * @param join  Join to destroy
 */
void async_join_destroy(async_join_t* join);

/**
 * Mark operation at 'index' as complete with the given result.
 *
 * Thread-safe: can be called from any thread (typically a promise resolve callback).
 * Decrements pending count and signals the condition when all operations complete.
 *
 * @param join    Join to update
 * @param index   Index of the completed operation
 * @param result  Result payload (caller retains ownership)
 */
void async_join_complete(async_join_t* join, int index, void* result);

/**
 * Block until all operations have completed.
 *
 * @param join  Join to wait on
 */
void async_join_wait(async_join_t* join);

#ifdef __cplusplus
}
#endif

#endif // WAVEDB_JOIN_H