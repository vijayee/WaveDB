//
// Created by victor on 5/6/25.
//

#include "scheduler.h"
#include "../Actor/actor.h"
#include "../RefCounter/refcounter.h"
#include "../Util/allocator.h"
#include "../Util/log.h"
#include "../Platform/platform_thread.h"

#include <stdlib.h>
#include <string.h>

#define STEAL_THRESHOLD 10

static __thread scheduler_t* current_scheduler = NULL;

static void _inject_queue_init(inject_queue_t* queue) {
  queue->lock = platform_mutex_create();
  queue->condition = platform_condvar_create();
  queue->head = NULL;
  queue->tail = NULL;
}

static void _inject_queue_destroy(inject_queue_t* queue) {
  platform_mutex_destroy(queue->lock);
  platform_condvar_destroy(queue->condition);
  inject_node_t* node = queue->head;
  while (node != NULL) {
    inject_node_t* next = node->next;
    free(node);
    node = next;
  }
}

static void _inject_queue_push(inject_queue_t* queue, actor_t* actor) {
  inject_node_t* node = get_clear_memory(sizeof(inject_node_t));
  node->actor = actor;
  node->next = NULL;
  platform_mutex_lock(queue->lock);
  if (queue->tail != NULL) {
    queue->tail->next = node;
  } else {
    queue->head = node;
  }
  queue->tail = node;
  platform_mutex_unlock(queue->lock);
  platform_condvar_signal(queue->condition);
}

static actor_t* _inject_queue_pop(inject_queue_t* queue) {
  platform_mutex_lock(queue->lock);
  inject_node_t* node = queue->head;
  if (node == NULL) {
    platform_mutex_unlock(queue->lock);
    return NULL;
  }
  queue->head = node->next;
  if (queue->head == NULL) {
    queue->tail = NULL;
  }
  actor_t* actor = node->actor;
  free(node);
  platform_mutex_unlock(queue->lock);
  return actor;
}

static actor_t* _scheduler_try_steal(scheduler_pool_t* pool, scheduler_t* self) {
  uint32_t victim_index = atomic_load_explicit(&self->last_victim, memory_order_relaxed);
  for (uint32_t attempt = 0; attempt < pool->worker_count; attempt++) {
    victim_index = (victim_index == 0) ? (uint32_t)(pool->worker_count - 1) : victim_index - 1;
    if (victim_index == self->index) {
      continue;
    }
    void* stolen = deque_steal(&pool->workers[victim_index].local_queue);
    if (stolen != DEQUE_EMPTY && stolen != DEQUE_ABORT) {
      atomic_store_explicit(&self->last_victim, victim_index, memory_order_relaxed);
      return (actor_t*)stolen;
    }
  }
  return NULL;
}

static actor_t* _scheduler_find_work(scheduler_pool_t* pool, scheduler_t* self) {
  // Fast path: pop from local deque
  void* item = deque_pop(&self->local_queue);
  if (item != DEQUE_EMPTY && item != DEQUE_ABORT) {
    return (actor_t*)item;
  }

  // Try inject queue
  actor_t* actor = _inject_queue_pop(&pool->inject);
  if (actor != NULL) {
    return actor;
  }

  // Try stealing from other workers
  return _scheduler_try_steal(pool, self);
}

static void* _scheduler_worker_loop(void* arg) {
  platform_thread_setup_stack();
  scheduler_pool_t* pool = (scheduler_pool_t*)arg;
  // Each worker thread atomically claims its index via fetch_add
  uint32_t my_index = atomic_fetch_add(&pool->active_count, 1);
  scheduler_t* self = &pool->workers[my_index];
  current_scheduler = self;

  pl_barrier_wait(pool->barrier);

  uint32_t spin_count = 0;
  while (!atomic_load_explicit(&pool->terminate, memory_order_acquire)) {
    actor_t* actor = _scheduler_find_work(pool, self);
    if (actor != NULL) {
      spin_count = 0;
      self->current = actor;
      uint8_t flags = atomic_load(&actor->flags);
      if (flags & ACTOR_FLAG_DESTROY) {
        /* Actor has been destroyed — skip it */
        self->current = NULL;
        continue;
      }
      if (flags & ACTOR_FLAG_MUTED) {
        /* Actor is muted due to backpressure — re-queue it for later */
        self->current = NULL;
        deque_push(&self->local_queue, (void*)actor);
        continue;
      }
      if (flags & ACTOR_FLAG_RUNNING) {
        /* Another worker is already running this actor; re-queue it */
        deque_push(&self->local_queue, (void*)actor);
        continue;
      }
      if (!atomic_compare_exchange_strong(&actor->flags, &flags, flags | ACTOR_FLAG_RUNNING)) {
        deque_push(&self->local_queue, (void*)actor);
        continue;
      }
      bool has_more = actor_run(actor, ACTOR_BATCH_SIZE);
      atomic_fetch_and(&actor->flags, ~ACTOR_FLAG_RUNNING);
      self->current = NULL;
      if (has_more) {
        deque_push(&self->local_queue, (void*)actor);
        // Signal other workers that might be sleeping
        platform_mutex_lock(pool->inject.lock);
        platform_condvar_broadcast(pool->inject.condition);
        platform_mutex_unlock(pool->inject.lock);
      }
    } else {
      spin_count++;
      if (spin_count >= STEAL_THRESHOLD) {
        platform_mutex_lock(pool->inject.lock);

        /* Re-check inject queue under the lock to prevent lost wakeup.
           _inject_queue_push holds inject.lock while enqueuing work, so if
           we see an empty queue here, any future push will see our idle_count
           increment and its signal will arrive after we begin waiting. */
        if (pool->inject.head != NULL) {
          platform_mutex_unlock(pool->inject.lock);
          spin_count = 0;
          continue;
        }

        atomic_fetch_add(&pool->idle_count, 1);
        if (atomic_load(&pool->idle_count) == pool->worker_count) {
          platform_mutex_lock(pool->idle_lock);
          platform_condvar_signal(pool->idle);
          platform_mutex_unlock(pool->idle_lock);
        }

        if (!atomic_load_explicit(&pool->terminate, memory_order_acquire)) {
          platform_condvar_wait(pool->inject.condition, pool->inject.lock);
        }
        platform_mutex_unlock(pool->inject.lock);
        atomic_fetch_sub(&pool->idle_count, 1);
        spin_count = 0;
      }
    }
  }

  current_scheduler = NULL;
  return 0;
}

scheduler_pool_t* scheduler_pool_create(size_t worker_count) {
  scheduler_pool_t* pool = get_clear_memory(sizeof(scheduler_pool_t));
  pool->worker_count = worker_count;
  pool->workers = get_clear_memory(sizeof(scheduler_t) * worker_count);
  for (size_t index = 0; index < worker_count; index++) {
    pool->workers[index].index = index;
    deque_init(&pool->workers[index].local_queue);
    atomic_store_explicit(&pool->workers[index].last_victim, (uint32_t)((index + 1) % worker_count), memory_order_relaxed);
  }
  _inject_queue_init(&pool->inject);
  pool->barrier = pl_barrier_create(worker_count + 1);
  pool->idle_lock = platform_mutex_create();
  pool->idle = platform_condvar_create();
  pool->deref_lock = platform_mutex_create();
  pool->pending_derefs = NULL;
  atomic_store_explicit(&pool->idle_count, 0, memory_order_relaxed);
  atomic_store_explicit(&pool->active_count, 0, memory_order_relaxed);
  atomic_store_explicit(&pool->terminate, 0, memory_order_relaxed);
  return pool;
}

void scheduler_pool_destroy(scheduler_pool_t* pool) {
  /* Drain any remaining pending derefs */
  scheduler_pool_drain_pending_derefs(pool);
  platform_mutex_destroy(pool->deref_lock);
  for (size_t index = 0; index < pool->worker_count; index++) {
    deque_destroy(&pool->workers[index].local_queue);
  }
  _inject_queue_destroy(&pool->inject);
  pl_barrier_destroy(pool->barrier);
  platform_mutex_destroy(pool->idle_lock);
  platform_condvar_destroy(pool->idle);
  free(pool->workers);
  free(pool);
}

void scheduler_pool_start(scheduler_pool_t* pool) {
  atomic_store_explicit(&pool->idle_count, 0, memory_order_relaxed);
  atomic_store_explicit(&pool->active_count, 0, memory_order_relaxed);
  for (size_t index = 0; index < pool->worker_count; index++) {
    pool->workers[index].thread = platform_thread_create(_scheduler_worker_loop, pool);
    if (pool->workers[index].thread == NULL) {
      log_error("Failed to start scheduler worker thread");
      abort();
    }
  }
  pl_barrier_wait(pool->barrier);
}

void scheduler_pool_stop(scheduler_pool_t* pool) {
  atomic_store_explicit(&pool->terminate, 1, memory_order_release);
  // Wake all sleeping workers
  platform_mutex_lock(pool->inject.lock);
  platform_condvar_broadcast(pool->inject.condition);
  platform_mutex_unlock(pool->inject.lock);
  for (size_t index = 0; index < pool->worker_count; index++) {
    platform_thread_join(pool->workers[index].thread);
  }
}

void scheduler_inject(scheduler_pool_t* pool, actor_t* actor) {
  _inject_queue_push(&pool->inject, actor);
}

void scheduler_pool_defer_cleanup(scheduler_pool_t* pool, void* object, void (*destructor)(void*)) {
  if (object == NULL) return;
  // Hold a reference so the object can't be freed before the drain runs
  refcounter_reference((refcounter_t*)object);
  pending_deref_node_t* node = get_clear_memory(sizeof(pending_deref_node_t));
  node->object = object;
  node->destructor = destructor;
  platform_mutex_lock(pool->deref_lock);
  node->next = pool->pending_derefs;
  pool->pending_derefs = node;
  platform_mutex_unlock(pool->deref_lock);
}

void scheduler_pool_drain_pending_derefs(scheduler_pool_t* pool) {
  pending_deref_node_t* node;

  platform_mutex_lock(pool->deref_lock);
  node = pool->pending_derefs;
  pool->pending_derefs = NULL;
  platform_mutex_unlock(pool->deref_lock);

  while (node != NULL) {
    pending_deref_node_t* next = node->next;
    node->destructor(node->object);
    free(node);
    node = next;
  }
}

void scheduler_pool_wait_for_idle(scheduler_pool_t* pool) {
  platform_mutex_lock(pool->idle_lock);
  while (atomic_load(&pool->idle_count) != pool->worker_count) {
    platform_condvar_wait(pool->idle, pool->idle_lock);
  }
  platform_mutex_unlock(pool->idle_lock);

  /* Drain pending deferred derefs — no actors running, safe to destroy */
  scheduler_pool_drain_pending_derefs(pool);
}

scheduler_t* scheduler_get_current(void) {
  return current_scheduler;
}
