//
// Created by victor on 3/22/26.
//

#include "batch.h"
#include "../Util/allocator.h"
#include <stdlib.h>

// Default initial capacity for operations array
#define BATCH_DEFAULT_CAPACITY 16

batch_t* batch_create(size_t reserve_count) {
    // Allocate batch structure
    batch_t* batch = get_clear_memory(sizeof(batch_t));
    if (batch == NULL) {
        return NULL;
    }

    // Set capacity (use default if reserve_count is 0)
    size_t capacity = (reserve_count > 0) ? reserve_count : BATCH_DEFAULT_CAPACITY;

    // Allocate operations array
    batch->ops = get_clear_memory(capacity * sizeof(batch_op_t));
    if (batch->ops == NULL) {
        free(batch);
        return NULL;
    }

    // Initialize fields
    batch->count = 0;
    batch->capacity = capacity;
    batch->max_size = BATCH_DEFAULT_MAX_SIZE;
    batch->estimated_size = 0;
    batch->submitted = 0;

    // Initialize lock and refcounter LAST
    platform_lock_init(&batch->lock);
    refcounter_init((refcounter_t*)batch);

    return batch;
}

void batch_destroy(batch_t* batch) {
    if (batch == NULL) return;

    refcounter_dereference((refcounter_t*)batch);
    if (refcounter_count((refcounter_t*)batch) == 0) {
        // Free all operations
        if (batch->ops != NULL) {
            for (size_t i = 0; i < batch->count; i++) {
                if (batch->ops[i].path != NULL) {
                    path_destroy(batch->ops[i].path);
                }
                if (batch->ops[i].value != NULL) {
                    identifier_destroy(batch->ops[i].value);
                }
            }
            free(batch->ops);
        }

        // Destroy lock and free batch
        platform_lock_destroy(&batch->lock);
        refcounter_destroy_lock((refcounter_t*)batch);
        free(batch);
    }
}

/**
 * Estimate serialized size of an operation.
 *
 * Conservative estimate for CBOR overhead and WAL header.
 * Each identifier is ~4 bytes overhead + data length.
 * Path is array of identifiers, value is single identifier.
 */
static size_t estimate_operation_size(path_t* path, identifier_t* value) {
    size_t size = 0;

    // WAL entry type (1 byte)
    size += 1;

    // Path size: array overhead + each identifier
    if (path != NULL) {
        size += 4; // CBOR array overhead
        size += path_length(path) * 4; // Per-identifier overhead
        for (size_t i = 0; i < path_length(path); i++) {
            identifier_t* id = path_get(path, i);
            if (id != NULL) {
                size += id->length + 4; // length + data + overhead
            }
        }
    }

    // Value size (for PUT operations)
    if (value != NULL) {
        size += 4; // CBOR overhead
        size += value->length + 4; // length + data
    }

    // CRC32 + data_len header overhead
    size += 8;

    return size;
}

/**
 * Grow operations array if needed.
 *
 * @return 0 on success, -1 on failure
 */
static int grow_ops_array(batch_t* batch) {
    if (batch->count < batch->capacity) {
        return 0; // No need to grow
    }

    // Double capacity
    size_t new_capacity = batch->capacity * 2;
    if (new_capacity == 0) {
        new_capacity = BATCH_DEFAULT_CAPACITY;
    }

    // Check against max_size
    if (new_capacity > batch->max_size) {
        new_capacity = batch->max_size;
    }

    // Reallocate
    batch_op_t* new_ops = realloc(batch->ops, new_capacity * sizeof(batch_op_t));
    if (new_ops == NULL) {
        return -1;
    }

    batch->ops = new_ops;
    batch->capacity = new_capacity;
    return 0;
}

int batch_add_put(batch_t* batch, path_t* path, identifier_t* value) {
    // Validate inputs
    if (batch == NULL || path == NULL || value == NULL) {
        return -1;
    }

    // Lock batch
    platform_lock(&batch->lock);

    // Check if already submitted
    if (batch->submitted) {
        platform_unlock(&batch->lock);
        return -6;
    }

    // Check if batch is full
    if (batch->count >= batch->max_size) {
        platform_unlock(&batch->lock);
        return -2;
    }

    // Grow array if needed
    if (grow_ops_array(batch) != 0) {
        platform_unlock(&batch->lock);
        return -1;
    }

    // Add operation
    batch->ops[batch->count].type = WAL_PUT;
    batch->ops[batch->count].path = path;
    batch->ops[batch->count].value = value;
    batch->count++;

    // Update estimated size
    batch->estimated_size += estimate_operation_size(path, value);

    // Unlock batch
    platform_unlock(&batch->lock);

    return 0;
}

int batch_add_delete(batch_t* batch, path_t* path) {
    // Validate inputs
    if (batch == NULL || path == NULL) {
        return -1;
    }

    // Lock batch
    platform_lock(&batch->lock);

    // Check if already submitted
    if (batch->submitted) {
        platform_unlock(&batch->lock);
        return -6;
    }

    // Check if batch is full
    if (batch->count >= batch->max_size) {
        platform_unlock(&batch->lock);
        return -2;
    }

    // Grow array if needed
    if (grow_ops_array(batch) != 0) {
        platform_unlock(&batch->lock);
        return -1;
    }

    // Add operation
    batch->ops[batch->count].type = WAL_DELETE;
    batch->ops[batch->count].path = path;
    batch->ops[batch->count].value = NULL;
    batch->count++;

    // Update estimated size
    batch->estimated_size += estimate_operation_size(path, NULL);

    // Unlock batch
    platform_unlock(&batch->lock);

    return 0;
}

size_t batch_estimate_size(batch_t* batch) {
    if (batch == NULL) {
        return 0;
    }

    platform_lock(&batch->lock);
    size_t size = batch->estimated_size;
    platform_unlock(&batch->lock);

    return size;
}