#ifndef EVICTION_QUEUE_H
#define EVICTION_QUEUE_H

#include <stdint.h>
#include <stddef.h>
#include "../Util/atomic_compat.h"

#define EVICTION_QUEUE_CAPACITY 256

typedef struct eviction_queue_t {
    ATOMIC_TYPE64 head;
    ATOMIC_TYPE64 tail;
    uint64_t offsets[EVICTION_QUEUE_CAPACITY];
} eviction_queue_t;

#ifdef __cplusplus
extern "C" {
#endif

void eviction_queue_init(eviction_queue_t* queue);
int eviction_queue_push(eviction_queue_t* queue, uint64_t offset);
size_t eviction_queue_drain(eviction_queue_t* queue, uint64_t* out, size_t max);

// Current count of queued-but-not-yet-drained offsets (tail - head, read
// atomically like push/drain use them). A relaxed snapshot — safe to call
// concurrently with pushes and drains; drifts only for callers that race
// an in-flight push, which re-reads on their next size() call.
size_t eviction_queue_size(const eviction_queue_t* queue);

#ifdef __cplusplus
}
#endif

#endif