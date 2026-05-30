//
// Created by victor on 5/6/25.
//

#include "message_queue.h"
#include "../Util/allocator.h"
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>

static inline message_node_t* _set_empty_flag(message_node_t* ptr) {
  return (message_node_t*)((uintptr_t)ptr | 1);
}

static inline message_node_t* _clear_empty_flag(message_node_t* ptr) {
  return (message_node_t*)((uintptr_t)ptr & ~(uintptr_t)1);
}

static inline bool _is_empty_flag_set(message_node_t* ptr) {
  return ((uintptr_t)ptr & 1) != 0;
}

void message_queue_init(message_queue_t* queue) {
  message_node_t* sentinel = get_clear_memory(sizeof(message_node_t));
  atomic_store(&sentinel->next, NULL);
  atomic_store(&queue->head, _set_empty_flag(sentinel));
  queue->tail = sentinel;
  atomic_store(&queue->size, 0);
}

bool message_queue_push(message_queue_t* queue, message_node_t* first, message_node_t* last) {
  atomic_store_explicit(&last->next, NULL, memory_order_relaxed);
  atomic_thread_fence(memory_order_release);
  message_node_t* prev = atomic_exchange_explicit(&queue->head, last, memory_order_acq_rel);
  bool was_empty = _is_empty_flag_set(prev);
  prev = _clear_empty_flag(prev);
  atomic_store_explicit(&prev->next, first, memory_order_relaxed);
  atomic_fetch_add_explicit(&queue->size, 1, memory_order_relaxed);
  return was_empty;
}

bool message_queue_push_single(message_queue_t* queue, message_node_t* first, message_node_t* last) {
  atomic_store_explicit(&last->next, NULL, memory_order_relaxed);
  message_node_t* prev = atomic_load_explicit(&queue->head, memory_order_relaxed);
  atomic_store_explicit(&queue->head, last, memory_order_relaxed);
  bool was_empty = _is_empty_flag_set(prev);
  prev = _clear_empty_flag(prev);
  atomic_store_explicit(&prev->next, first, memory_order_release);
  atomic_fetch_add_explicit(&queue->size, 1, memory_order_relaxed);
  return was_empty;
}

message_node_t* message_queue_pop(message_queue_t* queue) {
  message_node_t* tail = queue->tail;
  message_node_t* next = atomic_load_explicit(&tail->next, memory_order_relaxed);
  if (next != NULL) {
    queue->tail = next;
    atomic_thread_fence(memory_order_acquire);
    atomic_fetch_sub_explicit(&queue->size, 1, memory_order_relaxed);
    free(tail);
    return next;
  }
  return NULL;
}

bool message_queue_markempty(message_queue_t* queue) {
  message_node_t* tail = queue->tail;
  message_node_t* head = atomic_load_explicit(&queue->head, memory_order_relaxed);
  if (_is_empty_flag_set(head)) {
    return true;
  }
  if (head != tail) {
    return false;
  }
  return atomic_compare_exchange_strong_explicit(
    &queue->head, &head, _set_empty_flag(tail),
    memory_order_release, memory_order_relaxed);
}

bool message_queue_isempty(message_queue_t* queue) {
  message_node_t* head = atomic_load_explicit(&queue->head, memory_order_relaxed);
  if (_is_empty_flag_set(head)) {
    return true;
  }
  return _clear_empty_flag(head) == queue->tail;
}

void message_queue_destroy(message_queue_t* queue) {
  message_node_t* node;
  while ((node = message_queue_pop(queue)) != NULL) {
    if (node->msg.payload_destroy != NULL && node->msg.payload != NULL) {
      node->msg.payload_destroy(node->msg.payload);
    }
  }
  message_node_t* sentinel = queue->tail;
  if (sentinel != NULL) {
    free(sentinel);
  }
  atomic_store(&queue->head, NULL);
  queue->tail = NULL;
  atomic_store(&queue->size, 0);
}