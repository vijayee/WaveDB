//
// Created by victor on 5/6/25.
//

#ifndef OFFS_SCHEDULER_H
#define OFFS_SCHEDULER_H

#include "deque.h"
#include "../Platform/platform_thread.h"
#include "../Actor/actor.h"
#include "../Util/atomic_compat.h"
#include <stddef.h>
#include <stdint.h>

#define CACHE_LINE_SIZE 64

typedef struct pending_deref_node_t {
  struct pending_deref_node_t* next;
  void* object;
  void (*destructor)(void*);
} pending_deref_node_t;

typedef struct inject_node_t {
  actor_t* actor;
  struct inject_node_t* next;
} inject_node_t;

typedef struct inject_queue_t {
  platform_mutex_t* lock;
  platform_condvar_t* condition;
  inject_node_t* head;
  inject_node_t* tail;
} inject_queue_t;

typedef struct scheduler_t {
  size_t index;
  platform_thread_t* thread;
  deque_t local_queue;
  ATOMIC(uint32_t) last_victim;
  char _pad[CACHE_LINE_SIZE - sizeof(size_t) - sizeof(platform_thread_t*) - sizeof(deque_t) - sizeof(ATOMIC(uint32_t))];
  actor_t* current;
} scheduler_t;

typedef struct scheduler_pool_t {
  scheduler_t* workers;
  size_t worker_count;
  inject_queue_t inject;
  platform_barrier_t* barrier;
  platform_mutex_t* idle_lock;
  platform_condvar_t* idle;
  ATOMIC(size_t) idle_count;
  ATOMIC(uint32_t) active_count;
  ATOMIC(uint8_t) terminate;
  platform_mutex_t* deref_lock;
  pending_deref_node_t* pending_derefs;
} scheduler_pool_t;

scheduler_pool_t* scheduler_pool_create(size_t worker_count);
void scheduler_pool_destroy(scheduler_pool_t* pool);
void scheduler_pool_start(scheduler_pool_t* pool);
void scheduler_pool_stop(scheduler_pool_t* pool);
void scheduler_pool_wait_for_idle(scheduler_pool_t* pool);
void scheduler_pool_drain_pending_derefs(scheduler_pool_t* pool);

void scheduler_inject(scheduler_pool_t* pool, actor_t* actor);
void scheduler_pool_defer_cleanup(scheduler_pool_t* pool, void* object, void (*destructor)(void*));

scheduler_t* scheduler_get_current(void);

#endif // OFFS_SCHEDULER_H
