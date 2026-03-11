//
// Created by victor on 6/17/25.
//
#include "wheel.h"
#include "../Workers/work.h"
#include "../Util/allocator.h"
#include "../Util/hash.h"
#include <stdio.h>


void timing_wheel_on_tick(void* ctx);
void timing_wheel_worker_execute(void* ctx);
void timing_wheel_worker_abort(void* ctx);
timer_st* timer_list_remove_by_id(timer_list_t* list, size_t timerId);
timer_st* timer_list_remove(timer_list_t* list, timer_list_node_t* node);
timer_list_t* timing_wheel_maintenance(timing_wheel_t* wheel);
void timing_wheel_fire_expired(timing_wheel_t* wheel, timer_list_t* expired);
void timer_duration_rectify(timer_duration_t* duration);

timer_list_t*  timer_list_create() {
  timer_list_t* list = get_clear_memory(sizeof(timer_list_t));
  list->first = NULL;
  list->last = NULL;
  return list;
}

void timer_list_destroy(timer_list_t* list) {
  timer_list_node_t* current = list->first;
  timer_list_node_t* next = NULL;
  while (current != NULL ) {
    next = current->next;
    free(current->timer->plan.steps);
    free(current->timer);
    free(current);
      current = next;
  }
  free(list);
}

void timer_list_enqueue(timer_list_t* list, timer_st* timer) {
  timer_list_node_t* node = get_clear_memory(sizeof(timer_list_node_t));
  node->timer = timer;
  node->previous = NULL;
  node->next = NULL;
  if ((list->last == NULL) && (list->first == NULL)) {
    list->first = node;
    list->last = node;
  } else {
    node->previous = list->last;
    list->last->next= node;
    list->last = node;
  }
}


timer_st* timer_list_dequeue(timer_list_t* list) {
  if ((list->last == NULL) && (list->first == NULL)) {
    return NULL;
  } else {
    timer_list_node_t* node = list->first;
    list->first = node->next;
    if (node->next != NULL) {
      list->first->previous = NULL;
    }
    if (list->last == node) {
      list->last = NULL;
    }
    timer_st* timer = node->timer;
    free(node);
    return timer;
  }
}

timer_st* timer_list_remove_by_id(timer_list_t* list, size_t timerId) {
  if ((list->last == NULL) && (list->first == NULL)) {
    return NULL;
  }
  timer_list_node_t* current = list->first;
  timer_list_node_t* previous = NULL;
  while (current != NULL) {
    if (current->timer->timerId == timerId) {
      if ((previous != NULL) && (current->next != NULL)) {
        previous->next = current->next;
        current->next->previous = previous;
      } if (previous != NULL) {
        previous->next= NULL;
      } else {
        list->first = NULL;
        list->last = NULL;
      }
      timer_st* timer = current->timer;
      free(current);
      return timer;
    } else {
      previous = current;
      current = current->next;
    }
  }
  return NULL;
}

timer_st* timer_list_remove(timer_list_t* list, timer_list_node_t* node) {
  if ((list->last == NULL) && (list->first == NULL)) {
    return NULL;
  }
  if (list->last == node) {
    list->last = node->previous;
  }
  if (list->first == node) {
    list->first = node->next;
  }
  if (node->previous != NULL) {
    node->previous->next = node->next;
  }
  if (node->next != NULL) {
    node->next->previous = node->previous;
  }
  timer_st* timer = node->timer;
  free(node);
  return timer;
}

timing_wheel_t* timing_wheel_create(uint64_t interval, size_t slot_count, work_pool_t* pool, timer_map_t* timers, PLATFORMCONDITIONTYPEPTR(idle)) {
  timing_wheel_t* wheel = get_clear_memory(sizeof(timing_wheel_t));
  refcounter_init((refcounter_t*) wheel);
  platform_lock_init(&wheel->lock);
  wheel->pool = (work_pool_t*) refcounter_reference((refcounter_t*) pool);
  wheel->interval = interval;
  wheel->position = slot_count - 1;
  wheel->slots = get_clear_memory(sizeof(slots_t));
  wheel->timers = timers;
  wheel->idle = idle;
  vec_init(wheel->slots);
  vec_reserve(wheel->slots, slot_count);
  for (size_t i = 0; i < slot_count; i++) {
    vec_push(wheel->slots, timer_list_create());
  }
  return wheel;
}

void timing_wheel_destroy(timing_wheel_t* wheel) {
  refcounter_dereference((refcounter_t*) wheel);
  if (refcounter_count((refcounter_t*) wheel) == 0) {
    for (size_t i = 0; i < wheel->slots->length; i++) {
      timer_list_t* list = wheel->slots->data[i];
      timer_list_destroy(list);
    }
    work_pool_destroy(wheel->pool);
    vec_deinit(wheel->slots);
    free(wheel->slots);
    platform_lock_destroy(&wheel->lock);
    free(wheel);
  }
}

void timing_wheel_run(timing_wheel_t* wheel) {
  if (wheel->wheel == NULL) {
    wheel->stopped = 0;
    priority_t priority = {0};
    work_t* work = work_create(priority, (void *) wheel, timing_wheel_worker_execute, timing_wheel_worker_abort);
    refcounter_yield((refcounter_t*) work);
    work_pool_enqueue(wheel->pool, work);
  } else {
    timer_st* timer = get_clear_memory(sizeof(timer_st));
    timer->cb = timing_wheel_on_tick;
    timer->abort = timing_wheel_worker_abort;
    timer->ctx = wheel;
    timer->plan.size = 1;
    timer->plan.current = 0;
    timer->plan.steps = get_clear_memory(sizeof(timer_wheel_plan_step_t));
    timer->plan.steps[0].delay = wheel->interval;
    timer->plan.steps[0].wheel = wheel->wheel;
    timing_wheel_set_timer(wheel->wheel, timer);
  }
}

void timing_wheel_on_tick(void* ctx) {
  timing_wheel_t* wheel = (timing_wheel_t*) ctx;
  if (wheel->stopped == 1) {
    return;
  }
  timer_list_t* expired = NULL;
  platform_lock(&wheel->lock);
  wheel->position = (wheel->position + 1) % wheel->slots->length;
  expired = timing_wheel_maintenance(wheel);
  platform_unlock(&wheel->lock);
  timing_wheel_run(wheel);
  if(expired != NULL) {
    timing_wheel_fire_expired(wheel, expired);
  }
}

void timing_wheel_fire_expired(timing_wheel_t* wheel, timer_list_t* expired) {
  timer_st* current = timer_list_dequeue(expired);
  while (current != NULL) {
    if (current->plan.current < (current->plan.size - 1)) {
      current->plan.current++;
      //Move to first non-zero wheel
      while((current->plan.current < current->plan.size) && (current->plan.steps[current->plan.current].delay == 0)) {
        current->plan.current++;
      }
      if (current->plan.current < current->plan.size) {
        timing_wheel_set_timer(current->plan.steps[current->plan.current].wheel, current);
        current = timer_list_dequeue(expired);
        continue;
      }
    }
    priority_t priority = {0};
    work_t* work = work_create(priority, current->ctx, current->cb, current->abort);
    refcounter_yield((refcounter_t*) work);
    work_pool_enqueue(wheel->pool, work);

    free(current->plan.steps);
    free(current);

    current = timer_list_dequeue(expired);

  }
  if (hashmap_size(wheel->timers) == 0) {
    platform_signal_condition(wheel->idle);
  }
  timer_list_destroy(expired);
}

timer_list_t* timing_wheel_maintenance(timing_wheel_t* wheel) {
  timer_list_t* list = wheel->slots->data[wheel->position];
  timer_list_t* expired = NULL;
  timer_list_node_t* current = list->first;
  while (current != NULL) {
    timer_list_node_t* next = NULL;
    timer_st* timer = current->timer;
    if (timer->removed) {
      next = current->next;
      timer_list_remove(list, current);
      free(timer->plan.steps);
      free(timer);
      current = next;
      continue;
    } else if (timer->circle > 0) {
      timer->circle--;
      if (timer->circle > 0) {
        current = current->next;
        continue;
      }
    }
    if (expired == NULL) {
      expired = timer_list_create();
    }
    timer_list_enqueue(expired, timer);
    next = current->next;
    hashmap_remove(wheel->timers, &timer->timerId);
    timer_list_remove(list, current);
    current = next;
  }
  return expired;
}

void timing_wheel_worker_execute(void* ctx) {
  timing_wheel_t* wheel = (timing_wheel_t*) ctx;
  ticker_t ticker = {0};
  ticker.ctx = ctx;
  ticker.cb = timing_wheel_on_tick;
  ticker_start(ticker, wheel->simulated == 1 ? 0 : Time_Milliseconds);
}

void timing_wheel_worker_abort(void* ctx) {
  timing_wheel_t* wheel = (timing_wheel_t*) ctx;
  platform_lock(&wheel->lock);
  wheel->stopped = 1;
  for(size_t i = 0; i < wheel->slots->length; i++) {
    timer_list_t* list = wheel->slots->data[i];
    timer_st* current = timer_list_dequeue(list);
    while (current != NULL) {
      current->abort(current->ctx);
      free(current);
    }
  }
  platform_unlock(&wheel->lock);
}

void timing_wheel_set_timer(timing_wheel_t* wheel, timer_st* timer) {
  platform_lock(&wheel->lock);
  size_t steps = timer->plan.steps[timer->plan.current].delay;
  size_t position = (wheel->position + steps) % wheel->slots->length;
  timer->circle = (steps - 1) / wheel->slots->length;
  timer_list_t* list = wheel->slots->data[position];
  timer_list_enqueue(list, timer);
  platform_unlock(&wheel->lock);
}

void timing_wheel_stop(timing_wheel_t* wheel) {
  platform_lock(&wheel->lock);
  wheel->stopped = 1;
  platform_unlock(&wheel->lock);
}

void hierachical_timing_wheel_stop(hierarchical_timing_wheel_t* wheel) {
  platform_lock(&wheel->lock);
  wheel->stopped = 1;
  timing_wheel_stop(wheel->milliseconds);
  timing_wheel_stop(wheel->seconds);
  timing_wheel_stop(wheel->minutes);
  timing_wheel_stop(wheel->hours);
  timing_wheel_stop(wheel->days);
  platform_unlock(&wheel->lock);
}

uint64_t hierarchical_timing_wheel_set_timer(hierarchical_timing_wheel_t* wheel, void* ctx, void (* cb)(void*), void (* abort)(void*), timer_duration_t delay) {
  timer_duration_rectify(&delay);
  timer_st* timer = get_clear_memory(sizeof(timer_st));
  timer->cb = cb;
  timer->abort = abort;
  timer->ctx = ctx;
  platform_lock(&wheel->lock);
  timer->timerId = wheel->next_id++;
  hashmap_put(&wheel->timers, &timer->timerId, timer);
  platform_unlock(&wheel->lock);
  if (delay.days > 0) {
    timer->plan.size = 5;
    timer->plan.steps = get_clear_memory(sizeof(timer_wheel_plan_step_t) * timer->plan.size);
    timer->plan.steps[0].delay = delay.days;
    timer->plan.steps[0].wheel = wheel->days;
    timer->plan.steps[1].delay = delay.hours;
    timer->plan.steps[1].wheel = wheel->hours;
    timer->plan.steps[2].delay = delay.minutes;
    timer->plan.steps[2].wheel = wheel->minutes;
    timer->plan.steps[3].delay = delay.seconds;
    timer->plan.steps[3].wheel = wheel->seconds;
    timer->plan.steps[4].delay = delay.milliseconds;
    timer->plan.steps[4].wheel = wheel->milliseconds;
    timer->plan.current = 0;
    timing_wheel_set_timer(wheel->days, timer);
  } else if (delay.hours > 0) {
    timer->plan.size = 4;
    timer->plan.steps = get_clear_memory(sizeof(timer_wheel_plan_step_t) * timer->plan.size);
    timer->plan.steps[0].delay = delay.hours;
    timer->plan.steps[0].wheel = wheel->hours;
    timer->plan.steps[1].delay = delay.minutes;
    timer->plan.steps[1].wheel = wheel->minutes;
    timer->plan.steps[2].delay = delay.seconds;
    timer->plan.steps[2].wheel = wheel->seconds;
    timer->plan.steps[3].delay = delay.milliseconds;
    timer->plan.steps[3].wheel = wheel->milliseconds;
    timer->plan.current = 0;
    timing_wheel_set_timer(wheel->hours, timer);
  } else if (delay.minutes > 0) {
    timer->plan.size = 3;
    timer->plan.steps = get_clear_memory(sizeof(timer_wheel_plan_step_t) * timer->plan.size);
    timer->plan.steps[0].delay = delay.minutes;
    timer->plan.steps[0].wheel = wheel->minutes;
    timer->plan.steps[1].delay = delay.seconds;
    timer->plan.steps[1].wheel = wheel->seconds;
    timer->plan.steps[2].delay = delay.milliseconds;
    timer->plan.steps[2].wheel = wheel->milliseconds;
    timer->plan.current = 0;
    timing_wheel_set_timer(wheel->minutes, timer);
  } else if (delay.seconds > 0) {
    timer->plan.size = 2;
    timer->plan.steps = get_clear_memory(sizeof(timer_wheel_plan_step_t) * timer->plan.size);
    timer->plan.steps[0].delay = delay.seconds;
    timer->plan.steps[0].wheel = wheel->seconds;
    timer->plan.steps[1].delay = delay.milliseconds;
    timer->plan.steps[1].wheel = wheel->milliseconds;
    timer->plan.current = 0;
    timing_wheel_set_timer(wheel->seconds, timer);
  } else {
    timer->plan.size = 1;
    timer->plan.steps = get_clear_memory(sizeof(timer_wheel_plan_step_t) * timer->plan.size);
    timer->plan.steps[0].delay = delay.milliseconds;
    timer->plan.steps[0].wheel = wheel->milliseconds;
    timer->plan.current = 0;
    timing_wheel_set_timer(wheel->milliseconds, timer);
  }
  return timer->timerId;
}

void hierarchical_timing_wheel_cancel_timer(hierarchical_timing_wheel_t* wheel, uint64_t timerId) {
  platform_lock(&wheel->lock);
  timer_st* timer = hashmap_get(&wheel->timers, &timerId);
  if (timer != NULL) {
    timer->removed = 1;
    hashmap_remove(&wheel->timers, &timerId);
    if (hashmap_size(&wheel->timers) == 0) {
      platform_signal_condition(&wheel->idle);
    }
  }
  platform_unlock(&wheel->lock);
}

hierarchical_timing_wheel_t* hierarchical_timing_wheel_create(size_t slot_count, work_pool_t* pool) {
  hierarchical_timing_wheel_t* wheel = get_clear_memory(sizeof(hierarchical_timing_wheel_t));
  refcounter_init((refcounter_t*) wheel);
  platform_lock_init(&wheel->lock);
  platform_condition_init(&wheel->idle);
  wheel->next_id = 1;
  hashmap_init(&wheel->timers, (void*)hash_uint64, (void*)compare_uint64);
  hashmap_set_key_alloc_funcs(&wheel->timers, duplicate_uint64, (void*)free);
  wheel->milliseconds = timing_wheel_create(1, slot_count, pool, &wheel->timers, &wheel->idle);
  wheel->seconds = timing_wheel_create(Time_Seconds, slot_count, pool, &wheel->timers, &wheel->idle);
  wheel->minutes = timing_wheel_create(Time_Minutes, slot_count, pool, &wheel->timers, &wheel->idle);
  wheel->hours = timing_wheel_create(Time_Hours, slot_count, pool, &wheel->timers, &wheel->idle);
  wheel->days = timing_wheel_create(Time_Days, slot_count, pool, &wheel->timers, &wheel->idle);

  wheel->seconds->wheel = wheel->milliseconds;
  wheel->minutes->wheel = wheel->seconds;
  wheel->hours->wheel = wheel->minutes;
  wheel->days->wheel = wheel->hours;
  return wheel;
}

void hierarchical_timing_wheel_destroy(hierarchical_timing_wheel_t* wheel) {
  refcounter_dereference((refcounter_t*) wheel);
  if (refcounter_count((refcounter_t*) wheel) == 0) {
    timing_wheel_destroy(wheel->days);
    timing_wheel_destroy(wheel->hours);
    timing_wheel_destroy(wheel->minutes);
    timing_wheel_destroy(wheel->seconds);
    timing_wheel_destroy(wheel->milliseconds);
    hashmap_cleanup(&wheel->timers);
    free(wheel);
  }
}

void hierarchical_timing_wheel_simulate(hierarchical_timing_wheel_t* wheel) {
  wheel->days->simulated = 1;
  wheel->hours->simulated = 1;
  wheel->minutes->simulated = 1;
  wheel->seconds->simulated = 1;
  wheel->milliseconds->simulated = 1;
}

void timer_duration_rectify(timer_duration_t* duration) {
  if (duration->milliseconds > Time_Seconds) {
    duration->seconds += duration->milliseconds/ Time_Seconds;
    duration->milliseconds = duration->milliseconds % Time_Seconds;
  }
  if (duration->seconds > Time_Minutes) {
    duration->minutes += duration->seconds / Time_Minutes;
    duration->seconds = duration->seconds % Time_Minutes;
  }
  if (duration->minutes > Time_Hours) {
    duration->hours += duration->minutes / Time_Hours;
    duration->minutes = duration->minutes % Time_Hours;
  }
  if (duration->hours > Time_Days) {
    duration->hours += duration->hours / Time_Days;
    duration->minutes = duration->hours / Time_Days;
  }
}

void hierarchical_timing_wheel_run(hierarchical_timing_wheel_t* wheel) {
  timing_wheel_run(wheel->milliseconds);
  timing_wheel_run(wheel->seconds);
  timing_wheel_run(wheel->minutes);
  timing_wheel_run(wheel->hours);
  timing_wheel_run(wheel->days);
}
void hierarchical_timing_wheel_stop(hierarchical_timing_wheel_t* wheel) {
  timing_wheel_stop(wheel->milliseconds);
  timing_wheel_stop(wheel->seconds);
  timing_wheel_stop(wheel->minutes);
  timing_wheel_stop(wheel->hours);
  timing_wheel_stop(wheel->days);
}

void hierarchical_timing_wheel_wait_for_idle_signal(hierarchical_timing_wheel_t* wheel) {
  platform_lock(&wheel->lock);
  if (hashmap_size(&wheel->timers) != 0) {
    platform_condition_wait(&wheel->lock, &wheel->idle);
  }
  platform_unlock(&wheel->lock);
}