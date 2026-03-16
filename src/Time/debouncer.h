//
// Created by victor on 7/8/25.
//

#ifndef WAVEDB_DEBOUNCER_H
#define WAVEDB_DEBOUNCER_H
#include <stdint.h>
#include <time.h>
#include "wheel.h"
#include "../RefCounter/refcounter.h"
#if _WIN32
#include <windows.h>
typedef LARGE_INTEGER timeval_t;
#else
#include <sys/time.h>
typedef struct timeval timeval_t;
#endif


typedef struct {
  refcounter_t refcounter;
  uint64_t timerId;
  void* ctx;
  void (* cb)(void*);
  void (* abort)(void*);
  timeval_t interval_start;
  uint64_t wait;
  uint64_t max_wait;
  hierarchical_timing_wheel_t* wheel;
} debouncer_t;

debouncer_t* debouncer_create(hierarchical_timing_wheel_t* wheel, void* ctx, void (* cb)(void*), void (* abort)(void*), uint64_t wait, uint64_t max_wait);
void debouncer_destroy(debouncer_t* bouncer);
void debouncer_debounce(debouncer_t* bouncer);
void debouncer_flush(debouncer_t* bouncer);
uint64_t elapsed_time(timeval_t start, timeval_t end);
void get_time(timeval_t* tv);

// High-resolution benchmark timers (nanosecond precision)
#if _WIN32
typedef struct {
    LARGE_INTEGER start;
    LARGE_INTEGER end;
} benchmark_timer_t;
#else
typedef struct {
    struct timespec start;
    struct timespec end;
} benchmark_timer_t;
#endif

void benchmark_start(benchmark_timer_t* timer);
uint64_t benchmark_end(benchmark_timer_t* timer);  // Returns nanoseconds

#endif //WAVEDB_DEBOUNCER_H
