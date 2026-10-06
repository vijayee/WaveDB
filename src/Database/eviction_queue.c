#include "eviction_queue.h"
#include <string.h>

void eviction_queue_init(eviction_queue_t* queue) {
    atomic_store(&queue->head, 0);
    atomic_store(&queue->tail, 0);
    memset(queue->offsets, 0, sizeof(queue->offsets));
}

int eviction_queue_push(eviction_queue_t* queue, uint64_t offset) {
    uint64_t tail = atomic_load(&queue->tail);
    uint64_t head = atomic_load(&queue->head);

    if (tail - head >= EVICTION_QUEUE_CAPACITY) {
        return -1;  // Full
    }

    queue->offsets[tail % EVICTION_QUEUE_CAPACITY] = offset;
    atomic_store(&queue->tail, tail + 1);
    return 0;
}

size_t eviction_queue_drain(eviction_queue_t* queue, uint64_t* out, size_t max) {
    uint64_t head = atomic_load(&queue->head);
    uint64_t tail = atomic_load(&queue->tail);
    size_t available = (size_t)(tail - head);

    size_t count = available < max ? available : max;
    for (size_t i = 0; i < count; i++) {
        out[i] = queue->offsets[(head + i) % EVICTION_QUEUE_CAPACITY];
    }

    atomic_store(&queue->head, head + count);
    return count;
}

size_t eviction_queue_size(const eviction_queue_t* queue) {
    if (queue == NULL) return 0;
    // head/tail are read atomically with the same plain seq-cst loads push
    // and drain use. tail - head never underflows: only drain advances head
    // and only push advances tail, so the snapshot is monotonic per side.
    uint64_t head = atomic_load(&queue->head);
    uint64_t tail = atomic_load(&queue->tail);
    return (size_t)(tail - head);
}