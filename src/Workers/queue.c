//
// Created by victor on 4/15/25.
//
#include "queue.h"
#include "../Util/allocator.h"
#include "../Util/threadding.h"


void work_queue_init(work_queue_t* queue) {
  queue->first = NULL;
  queue->last = NULL;
}

// O(1) tail insertion (FIFO - no priority needed)
void work_enqueue(work_queue_t* queue, work_t* work) {
  work_queue_item_t* item = get_clear_memory(sizeof(work_queue_item_t));
  item->work = work;
  item->next = NULL;
  item->previous = queue->last;

  if (queue->last) {
    queue->last->next = item;  // Add to tail
  } else {
    queue->first = item;  // Queue was empty
  }
  queue->last = item;  // Update tail
}

work_t* work_dequeue(work_queue_t* queue) {
  if (queue->first == NULL) {
    return NULL;
  } else {
    work_queue_item_t* temp = queue->first;
    work_t* work= temp->work;
    queue->first = temp->next;
    if(queue->first != NULL) {
      queue->first->previous = NULL;
    }
    if (queue->last == temp) {
      queue->last = NULL;
    }
    free(temp);
    return work;
  }
}

// Sharded work queue implementation

void sharded_work_queue_init(sharded_work_queue_t* sq) {
  for (size_t i = 0; i < QUEUE_SHARDS; i++) {
    work_queue_init(&sq->queues[i]);
    platform_lock_init(&sq->locks[i]);
  }
  atomic_init(&sq->next_shard, 0);
}

void sharded_work_enqueue(sharded_work_queue_t* sq, work_t* work) {
  size_t shard = atomic_fetch_add(&sq->next_shard, 1) % QUEUE_SHARDS;

  platform_lock(&sq->locks[shard]);
  work_enqueue(&sq->queues[shard], work);
  platform_unlock(&sq->locks[shard]);
}

work_t* sharded_work_dequeue(sharded_work_queue_t* sq) {
  // Fair work stealing: thread-local starting offset prevents bias
  static _Thread_local size_t start_shard = 0;
  size_t shard = start_shard;
  start_shard = (start_shard + 1) % QUEUE_SHARDS;  // Rotate for fairness

  for (size_t i = 0; i < QUEUE_SHARDS; i++) {
    size_t idx = (shard + i) % QUEUE_SHARDS;
    platform_lock(&sq->locks[idx]);
    work_t* work = work_dequeue(&sq->queues[idx]);
    platform_unlock(&sq->locks[idx]);

    if (work) return work;
  }
  return NULL;  // All shards empty
}

void sharded_work_queue_destroy(sharded_work_queue_t* sq) {
  for (size_t i = 0; i < QUEUE_SHARDS; i++) {
    // Drain and abort remaining work items in this shard
    platform_lock(&sq->locks[i]);
    work_t* work;
    while ((work = work_dequeue(&sq->queues[i])) != NULL) {
      // Consume yield from original enqueue and abort the work
      refcounter_reference((refcounter_t*) work);
      work_abort(work);
      work_destroy(work);
    }
    platform_unlock(&sq->locks[i]);
    platform_lock_destroy(&sq->locks[i]);
  }
}