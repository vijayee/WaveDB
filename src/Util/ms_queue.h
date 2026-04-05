//
// Michael-Scott Lock-Free Queue
//
// Based on: https://www.cs.rochester.edu/u/scott/papers/1996_PODC_queues.pdf
//

#ifndef WAVEDB_MS_QUEUE_H
#define WAVEDB_MS_QUEUE_H

#include <stdint.h>
#include <stddef.h>
#include <stdatomic.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Queue node - generic container for lock-free queue
 */
typedef struct ms_queue_node_t {
    _Atomic(void*) data;              // Payload (NULL for holes/removed)
    _Atomic(struct ms_queue_node_t*) next;  // Next node in queue
} ms_queue_node_t;

/**
 * Michael-Scott lock-free queue
 */
typedef struct {
    _Atomic(ms_queue_node_t*) head;   // LRU end (for dequeue)
    _Atomic(ms_queue_node_t*) tail;   // MRU end (for enqueue)
    _Atomic(size_t) size;             // Approximate count (including holes)
} ms_queue_t;

/**
 * Initialize a queue.
 *
 * @param queue Queue to initialize
 * @return 0 on success, -1 on failure
 */
int ms_queue_init(ms_queue_t* queue);

/**
 * Destroy a queue.
 *
 * @param queue Queue to destroy
 * @param free_data Function to free data payloads (NULL for no free)
 */
void ms_queue_destroy(ms_queue_t* queue, void (*free_data)(void*));

/**
 * Enqueue an item (lock-free).
 *
 * @param queue Queue to update
 * @param node Pre-allocated node (caller owns allocation)
 * @return 0 on success
 */
int ms_queue_enqueue(ms_queue_t* queue, ms_queue_node_t* node);

/**
 * Dequeue an item (lock-free).
 *
 * @param queue Queue to update
 * @return Node if available, NULL if empty
 */
ms_queue_node_t* ms_queue_dequeue(ms_queue_t* queue);

/**
 * Get approximate size.
 *
 * @param queue Queue to query
 * @return Approximate number of items
 */
size_t ms_queue_size(ms_queue_t* queue);

/**
 * Check if queue is empty.
 *
 * @param queue Queue to query
 * @return 1 if empty, 0 otherwise
 */
int ms_queue_is_empty(ms_queue_t* queue);

/**
 * Allocate a queue node from the memory pool.
 *
 * @param data Initial payload
 * @return New node or NULL on failure
 */
ms_queue_node_t* ms_queue_node_alloc(void* data);

/**
 * Free a queue node to the memory pool.
 *
 * @param node Node to free
 */
void ms_queue_node_free(ms_queue_node_t* node);

#ifdef __cplusplus
}
#endif

#endif // WAVEDB_MS_QUEUE_H