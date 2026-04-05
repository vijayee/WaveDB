//
// Michael-Scott Lock-Free Queue Implementation
//

#include "ms_queue.h"
#include "allocator.h"
#include "memory_pool.h"
#include <stdlib.h>

int ms_queue_init(ms_queue_t* queue) {
    if (queue == NULL) return -1;

    // Create dummy node for empty queue
    ms_queue_node_t* dummy = memory_pool_alloc(sizeof(ms_queue_node_t));
    if (dummy == NULL) return -1;

    atomic_init(&dummy->data, NULL);
    atomic_init(&dummy->next, NULL);

    atomic_init(&queue->head, dummy);
    atomic_init(&queue->tail, dummy);
    atomic_init(&queue->size, 0);

    return 0;
}

void ms_queue_destroy(ms_queue_t* queue, void (*free_data)(void*)) {
    if (queue == NULL) return;

    // Drain all nodes
    ms_queue_node_t* node = atomic_load(&queue->head);
    while (node != NULL) {
        ms_queue_node_t* next = atomic_load(&node->next);

        if (free_data != NULL) {
            void* data = atomic_load(&node->data);
            if (data != NULL) {
                free_data(data);
            }
        }

        memory_pool_free(node, sizeof(ms_queue_node_t));
        node = next;
    }

    atomic_init(&queue->head, NULL);
    atomic_init(&queue->tail, NULL);
    atomic_init(&queue->size, 0);
}

int ms_queue_enqueue(ms_queue_t* queue, ms_queue_node_t* node) {
    if (queue == NULL || node == NULL) return -1;

    // Initialize node
    atomic_store(&node->next, NULL);

    while (1) {
        ms_queue_node_t* tail = atomic_load(&queue->tail);
        ms_queue_node_t* next = atomic_load(&tail->next);

        // Check if tail is still consistent
        if (tail == atomic_load(&queue->tail)) {
            if (next == NULL) {
                // Tail is pointing to last node, try to link new node
                if (atomic_compare_exchange_weak(&tail->next, &next, node)) {
                    // Successfully linked, try to advance tail
                    atomic_compare_exchange_weak(&queue->tail, &tail, node);
                    atomic_fetch_add(&queue->size, 1);
                    return 0;
                }
            } else {
                // Tail not pointing to last node, advance it
                atomic_compare_exchange_weak(&queue->tail, &tail, next);
            }
        }
    }
}

ms_queue_node_t* ms_queue_dequeue(ms_queue_t* queue) {
    if (queue == NULL) return NULL;

    while (1) {
        ms_queue_node_t* head = atomic_load(&queue->head);
        ms_queue_node_t* tail = atomic_load(&queue->tail);
        ms_queue_node_t* next = atomic_load(&head->next);

        // Check if head is still consistent
        if (head == atomic_load(&queue->head)) {
            if (head == tail) {
                // Queue might be empty
                if (next == NULL) {
                    // Queue is empty
                    return NULL;
                }
                // Tail is falling behind, advance it
                atomic_compare_exchange_weak(&queue->tail, &tail, next);
            } else {
                // Read value before CAS (ABA problem prevention)
                ms_queue_node_t* node = next;

                // Try to swing head to next node
                if (atomic_compare_exchange_weak(&queue->head, &head, node)) {
                    // Successfully dequeued
                    atomic_fetch_sub(&queue->size, 1);

                    // Return the OLD head (dummy), not the new value
                    // Caller is responsible for freeing old head
                    return head;
                }
            }
        }
    }
}

size_t ms_queue_size(ms_queue_t* queue) {
    if (queue == NULL) return 0;
    return atomic_load(&queue->size);
}

int ms_queue_is_empty(ms_queue_t* queue) {
    if (queue == NULL) return 1;

    ms_queue_node_t* head = atomic_load(&queue->head);
    ms_queue_node_t* tail = atomic_load(&queue->tail);

    return head == tail && atomic_load(&head->next) == NULL;
}

ms_queue_node_t* ms_queue_node_alloc(void* data) {
    ms_queue_node_t* node = memory_pool_alloc(sizeof(ms_queue_node_t));
    if (node == NULL) return NULL;

    atomic_init(&node->data, data);
    atomic_init(&node->next, NULL);

    return node;
}

void ms_queue_node_free(ms_queue_node_t* node) {
    if (node != NULL) {
        memory_pool_free(node, sizeof(ms_queue_node_t));
    }
}