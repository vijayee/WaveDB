//
// Created by victor on 6/17/25.
//

#ifndef WAVEDB_WHEEL_H
#define WAVEDB_WHEEL_H
#include <hashmap.h>
#include "../Util/vec.h"
#include "../RefCounter/refcounter.h"
#include "ticker.h"
#include "../Workers/pool.h"
#include "../Util/threadding.h"
#include <stdint.h>
#ifdef _WIN32
#define Time_Milliseconds 1
#else
#define Time_Milliseconds 1000
#endif
#define Time_Seconds 1000
#define Time_Minutes 60
#define Time_Hours 60
#define Time_Days 24

typedef struct timing_wheel_t timing_wheel_t;

typedef struct {
  uint64_t delay;
  timing_wheel_t* wheel;
} timer_wheel_plan_step_t;

typedef struct {
  size_t current;
  timer_wheel_plan_step_t* steps;
  size_t size;
} timer_wheel_plan_t;

typedef struct {
  size_t timerId;
  timer_wheel_plan_t plan;
  void* ctx;
  void (* cb)(void*);
  void (* abort)(void*);
  uint8_t removed;
  int32_t circle;
} timer_st;

typedef struct timer_list_node_t timer_list_node_t;
struct timer_list_node_t {
  timer_st* timer;
  timer_list_node_t* next;
  timer_list_node_t* previous;
};
typedef struct {
  timer_list_node_t* first;
  timer_list_node_t* last;
} timer_list_t;


typedef struct {
  uint64_t days;
  uint64_t hours;
  uint64_t minutes;
  uint64_t seconds;
  uint64_t milliseconds;
} timer_duration_t;


timer_list_t* timer_list_create();
void timer_list_destroy(timer_list_t* list);
void timer_list_enqueue(timer_list_t* list, timer_st* timer);
timer_st* timer_list_dequeue(timer_list_t* list);

typedef HASHMAP(size_t, timer_st) timer_map_t;
typedef vec_t(timer_list_t*) slots_t;


struct timing_wheel_t {
  refcounter_t refcounter;
  PLATFORMLOCKTYPE(lock);
  PLATFORMCONDITIONTYPEPTR(idle);
  size_t position;
  timer_map_t* timers;
  timing_wheel_t* wheel;
  slots_t* slots;
  uint64_t interval;
  work_pool_t* pool;
  uint8_t stopped;
  uint8_t simulated;
};

typedef struct {
  refcounter_t refcounter;
  PLATFORMLOCKTYPE(lock);
  PLATFORMCONDITIONTYPE(idle);
  timer_map_t timers;
  size_t next_id;
  uint8_t stopped;
  timing_wheel_t* days;
  timing_wheel_t* hours;
  timing_wheel_t* minutes;
  timing_wheel_t* seconds;
  timing_wheel_t* milliseconds;
} hierarchical_timing_wheel_t;

hierarchical_timing_wheel_t* hierarchical_timing_wheel_create(size_t slot_count, work_pool_t* pool);
void hierarchical_timing_wheel_destroy(hierarchical_timing_wheel_t* wheel);
uint64_t hierarchical_timing_wheel_set_timer(hierarchical_timing_wheel_t* wheel, void* ctx, void (* cb)(void*), void (* abort)(void*), timer_duration_t delay);
void hierarchical_timing_wheel_cancel_timer(hierarchical_timing_wheel_t* wheel, uint64_t timerId);
void hierarchical_timing_wheel_stop(hierarchical_timing_wheel_t* wheel);
void hierarchical_timing_wheel_run(hierarchical_timing_wheel_t* wheel);
void hierarchical_timing_wheel_simulate(hierarchical_timing_wheel_t* wheel);

timing_wheel_t* timing_wheel_create(uint64_t interval, size_t slot_count, work_pool_t* pool, timer_map_t* timers, PLATFORMCONDITIONTYPEPTR(idle));
void timing_wheel_destroy(timing_wheel_t* wheel);
void timing_wheel_set_timer(timing_wheel_t* wheel, timer_st* timer);
void timing_wheel_stop(timing_wheel_t* wheel);
void timing_wheel_run(timing_wheel_t* wheel);
void hierarchical_timing_wheel_wait_for_idle_signal(hierarchical_timing_wheel_t* wheel);

#endif //WAVEDB_WHEEL_H
