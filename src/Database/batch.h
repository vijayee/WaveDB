//
// Created by victor on 3/22/26.
//

#ifndef WAVEDB_BATCH_H
#define WAVEDB_BATCH_H

#include <stdint.h>
#include <stddef.h>
#include "../RefCounter/refcounter.h"
#include "../HBTrie/path.h"
#include "../HBTrie/identifier.h"
#include "../Util/threadding.h"
#include "wal.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Batch operation - single PUT or DELETE operation in a batch
 */
typedef struct {
    wal_type_e type;        // WAL_PUT or WAL_DELETE
    path_t* path;           // Key (ownership transfers to batch)
    identifier_t* value;    // Value for PUT (NULL for DELETE)
} batch_op_t;

/**
 * Batch handle - collects write operations for atomic submission
 *
 * Batch operations are collected in memory, serialized as a single WAL_BATCH
 * entry with one transaction ID, and applied atomically to the database.
 *
 * Thread-safe for concurrent batch_add_* calls.
 *
 * Ownership semantics:
 * - On successful add: ownership of path/value transfers to batch
 * - On error: ownership remains with caller (caller must destroy)
 * - After submission: batch is marked as submitted, cannot add more operations
 * - Caller should destroy batch after submission completes
 */
typedef struct {
    refcounter_t refcounter;        // MUST be first field
    PLATFORMLOCKTYPE(lock);          // Thread-safe for concurrent additions
    batch_op_t* ops;                 // Dynamic array of operations
    size_t count;                    // Current operation count
    size_t capacity;                 // Array capacity
    size_t max_size;                 // Maximum allowed operations
    size_t estimated_size;           // Running total of estimated serialized size
    uint8_t submitted;               // 0 = not submitted, 1 = submitted
} batch_t;

/**
 * Default maximum batch size (10,000 operations)
 */
#define BATCH_DEFAULT_MAX_SIZE 10000

/**
 * Create a batch for collecting write operations.
 *
 * Pre-allocates space for reserve_count operations to minimize reallocations.
 *
 * @param reserve_count Pre-allocate space for this many operations (0 for default)
 * @return New batch or NULL on failure
 */
batch_t* batch_create(size_t reserve_count);

/**
 * Add a PUT operation to batch.
 *
 * Ownership semantics:
 *   - On success: ownership of path and value transfers to batch
 *   - On error: ownership remains with caller (caller must destroy)
 *
 * Validation performed:
 *   1. Check batch is not full (count < max_size)
 *   2. Check batch is not already submitted
 *   3. Check path is not NULL
 *   4. Check value is not NULL
 *   5. Validate path can be serialized
 *   6. Validate value can be serialized
 *   7. Update estimated_size
 *
 * @param batch Batch to modify
 * @param path Key path (ownership transfers on success)
 * @param value Value to store (ownership transfers on success)
 * @return 0 on success, -1 on error, -2 if batch is full
 */
int batch_add_put(batch_t* batch, path_t* path, identifier_t* value);

/**
 * Add a DELETE operation to batch.
 *
 * Ownership semantics:
 *   - On success: ownership of path transfers to batch
 *   - On error: ownership remains with caller (caller must destroy)
 *
 * Validation performed:
 *   1. Check batch is not full
 *   2. Check batch is not already submitted
 *   3. Check path is not NULL
 *   4. Validate path can be serialized
 *   5. Update estimated_size
 *
 * @param batch Batch to modify
 * @param path Key path to delete (ownership transfers on success)
 * @return 0 on success, -1 on error, -2 if batch is full
 */
int batch_add_delete(batch_t* batch, path_t* path);

/**
 * Estimate serialized size of batch.
 *
 * Useful for checking against WAL max_size before submission.
 *
 * @param batch Batch to estimate
 * @return Estimated size in bytes
 */
size_t batch_estimate_size(batch_t* batch);

/**
 * Destroy a batch.
 *
 * Frees all operations and their paths/values.
 * Safe to call even if batch was submitted.
 *
 * @param batch Batch to destroy
 */
void batch_destroy(batch_t* batch);

#ifdef __cplusplus
}
#endif

#endif //WAVEDB_BATCH_H