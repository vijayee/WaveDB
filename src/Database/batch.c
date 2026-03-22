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