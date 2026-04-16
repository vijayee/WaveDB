//
// Created by victor on 4/15/25.
//

#ifndef WAVEDB_QUEUE_H
#define WAVEDB_QUEUE_H
#include "work.h"
#include "../Util/atomic_compat.h"


typedef struct work_queue_item_t work_queue_item_t;
struct work_queue_item_t {
  work_t* work;
  work_queue_item_t* next;
  work_queue_item_t* previous;
};
typedef struct {
  work_queue_item_t* first;
  work_queue_item_t* last;
} work_queue_t;

#define QUEUE_SHARDS 16

// Sharded work queue for 16-way parallelism
typedef struct {
  work_queue_t queues[QUEUE_SHARDS];
  PLATFORMLOCKTYPE(locks[QUEUE_SHARDS]);
  ATOMIC_TYPE(uint64_t) next_shard;   // Round-robin counter
} sharded_work_queue_t;

void work_queue_init(work_queue_t* queue);
void work_enqueue(work_queue_t* queue, work_t* work);
work_t* work_dequeue(work_queue_t* queue);

// New sharded API
void sharded_work_queue_init(sharded_work_queue_t* sq);
void sharded_work_enqueue(sharded_work_queue_t* sq, work_t* work);
work_t* sharded_work_dequeue(sharded_work_queue_t* sq);
void sharded_work_queue_destroy(sharded_work_queue_t* sq);

#endif //WAVEDB_QUEUE_H
