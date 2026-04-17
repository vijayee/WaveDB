#ifndef EVICTION_QUEUE_H
#define EVICTION_QUEUE_H

#include <stdint.h>
#include <stddef.h>
#include "../Util/atomic_compat.h"

#define EVICTION_QUEUE_CAPACITY 256

typedef struct eviction_queue_t {
    ATOMIC_TYPE(uint64_t) head;
    ATOMIC_TYPE(uint64_t) tail;
    uint64_t offsets[EVICTION_QUEUE_CAPACITY];
} eviction_queue_t;

#ifdef __cplusplus
extern "C" {
#endif

void eviction_queue_init(eviction_queue_t* queue);
int eviction_queue_push(eviction_queue_t* queue, uint64_t offset);
size_t eviction_queue_drain(eviction_queue_t* queue, uint64_t* out, size_t max);

#ifdef __cplusplus
}
#endif

#endif