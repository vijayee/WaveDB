//
// Created by victor on 7/8/25.
//
#include "debouncer.h"
#include "../Util/allocator.h"

debouncer_t* debouncer_create(hierarchical_timing_wheel_t* wheel, void* ctx, void (* cb)(void*), void (* abort)(void*), uint64_t wait, uint64_t max_wait) {
  debouncer_t* bouncer = get_clear_memory(sizeof(debouncer_t));
  refcounter_init((refcounter_t*) bouncer);
  bouncer->wheel = wheel;
  bouncer->ctx = ctx;
  bouncer->cb = cb;
  bouncer->abort = abort;
  bouncer->wait = wait;
  bouncer->max_wait = max_wait;
  return bouncer;
}
void debouncer_destroy(debouncer_t* bouncer) {
  refcounter_dereference((refcounter_t*) bouncer);
  if (refcounter_count((refcounter_t*) bouncer) == 0) {
    refcounter_destroy_lock((refcounter_t*) bouncer);
    free(bouncer);
  }
}
void debouncer_debounce(debouncer_t* bouncer) {
  if (bouncer->timerId == 0) {
    bouncer->timerId = hierarchical_timing_wheel_set_timer(bouncer->wheel, bouncer->ctx, bouncer->cb, bouncer->abort,(timer_duration_t) {.milliseconds = bouncer->wait});
    if (bouncer->max_wait > 0) {
       get_time(&bouncer->interval_start);
    }
  } else {
    if (bouncer->max_wait > 0) {
      timeval_t now;
      get_time(&now);
      if (elapsed_time(bouncer->interval_start,  now) >= bouncer->max_wait) {
        get_time(&bouncer->interval_start);
      } else {
        hierarchical_timing_wheel_cancel_timer(bouncer->wheel, bouncer->timerId);
      }
    } else {
      hierarchical_timing_wheel_cancel_timer(bouncer->wheel, bouncer->timerId);
    }
    bouncer->timerId = hierarchical_timing_wheel_set_timer(bouncer->wheel, bouncer->ctx, bouncer->cb, bouncer->abort, (timer_duration_t){.milliseconds = bouncer->wait});
  }
}

void debouncer_flush(debouncer_t* bouncer) {
  if (bouncer->timerId != 0) {
    hierarchical_timing_wheel_cancel_timer(bouncer->wheel, bouncer->timerId);
    bouncer->timerId = 0;
    bouncer->cb(bouncer->ctx);
  }
}

uint64_t elapsed_time(timeval_t start, timeval_t end) {
  double elapsedTime;
#if _WIN32
  LARGE_INTEGER frequency;
  QueryPerformanceFrequency(&frequency);
  elapsedTime = (t2.QuadPart - t1.QuadPart) * 1000.0 / frequency.QuadPart;
#else
  elapsedTime = (end.tv_sec - start.tv_sec) * 1000.0;
  elapsedTime += (end.tv_usec - start.tv_usec) / 1000.0;
#endif
  return elapsedTime;
}
void get_time(timeval_t* tv) {
 #if _WIN32
  QueryPerformanceCounter(tv);
 #else
  gettimeofday(tv, NULL);
 #endif
}