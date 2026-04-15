//
// Async Join - Wait for multiple async operations to complete
// Created: 2026-04-12
//

#include "join.h"
#include "../Util/allocator.h"
#include <stdlib.h>

async_join_t* async_join_create(int count) {
    if (count <= 0) return NULL;

    async_join_t* join = get_clear_memory(sizeof(async_join_t));
    if (join == NULL) return NULL;

    platform_lock_init(&join->lock);
    platform_condition_init(&join->condition);

    join->results = calloc(count, sizeof(void*));
    if (join->results == NULL) {
        platform_lock_destroy(&join->lock);
        platform_condition_destroy(&join->condition);
        free(join);
        return NULL;
    }

    join->count = count;
    join->pending = count;  // _Atomic init

    return join;
}

void async_join_destroy(async_join_t* join) {
    if (join == NULL) return;

    platform_lock_destroy(&join->lock);
    platform_condition_destroy(&join->condition);
    free(join->results);
    free(join);
}

void async_join_complete(async_join_t* join, int index, void* result) {
    if (join == NULL || index < 0 || index >= join->count) return;

    platform_lock(&join->lock);
    join->results[index] = result;
    join->pending--;
    platform_unlock(&join->lock);

    platform_signal_condition(&join->condition);
}

void async_join_wait(async_join_t* join) {
    if (join == NULL) return;

    platform_lock(&join->lock);
    while (join->pending > 0) {
        platform_condition_wait(&join->lock, &join->condition);
    }
    platform_unlock(&join->lock);
}