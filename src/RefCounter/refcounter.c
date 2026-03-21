//
// Created by victor on 3/18/25.
//
#include "refcounter.h"
#include <stdint.h>
#include <limits.h>
#ifdef REFCOUNTER_ATOMIC
#include <stdatomic.h>
#endif

void refcounter_init(refcounter_t* refcounter) {
#ifndef REFCOUNTER_ATOMIC
  platform_lock_init(&refcounter->lock);
  platform_lock(&refcounter->lock);
  refcounter->count++;
  platform_unlock(&refcounter->lock);
#else
  // Initialize to 1 (reference count starts at 1)
  // Direct assignment is atomic for _Atomic types in C11
  atomic_store(&refcounter->count, 1);
  atomic_store(&refcounter->yield, 0);
#endif
}

void refcounter_yield(refcounter_t* refcounter) {
#ifndef REFCOUNTER_ATOMIC
  platform_lock(&refcounter->lock);
  refcounter->yield++;
  platform_unlock(&refcounter->lock);
#else
  // Atomic increment
  atomic_fetch_add(&refcounter->yield, 1);
#endif
}

void* refcounter_reference(refcounter_t* refcounter) {
  if (refcounter == NULL) {
    return NULL;
  }
#ifndef REFCOUNTER_ATOMIC
  platform_lock(&refcounter->lock);
  if (refcounter->yield > 0) {
    refcounter->yield--;
  } else if (refcounter->count < USHRT_MAX) {
    refcounter->count++;
  }
  platform_unlock(&refcounter->lock);
#else
  // With atomics, we need to handle yield optimization correctly
  // Try to consume a yield first, then fall back to incrementing count
  uint8_t expected_yield = atomic_load(&refcounter->yield);
  while (expected_yield > 0) {
    if (atomic_compare_exchange_weak(&refcounter->yield, &expected_yield, expected_yield - 1)) {
      // Successfully consumed a yield, don't increment count
      return refcounter;
    }
    // CAS failed, expected_yield was updated with current value, retry
  }
  // No yield to consume, increment count normally
  atomic_fetch_add(&refcounter->count, 1);
#endif
  return refcounter;
}

void refcounter_dereference(refcounter_t* refcounter) {
#ifndef REFCOUNTER_ATOMIC
  platform_lock(&refcounter->lock);
  if ((refcounter->yield == 0) && (refcounter->count > 0)) {
    refcounter->count--;
  }
  platform_unlock(&refcounter->lock);
#else
  // With atomics, we need to respect yield optimization
  // Only decrement count if yield == 0 and count > 0
  uint8_t yield_val = atomic_load(&refcounter->yield);
  if (yield_val == 0) {
    uint16_t count_val = atomic_load(&refcounter->count);
    if (count_val > 0) {
      atomic_fetch_sub(&refcounter->count, 1);
    }
  }
#endif
}

uint16_t refcounter_count(refcounter_t* refcounter) {
#ifndef REFCOUNTER_ATOMIC
  platform_lock(&refcounter->lock);
  uint16_t count = refcounter->count;
  platform_unlock(&refcounter->lock);
  return count;
#else
  // Fast atomic read
  return (uint16_t)atomic_load(&refcounter->count);
#endif
}

refcounter_t* refcounter_consume(refcounter_t** refcounter) {
  refcounter_t* holder = *refcounter;
  refcounter_yield(holder);
  *refcounter = NULL;
  return holder;
}

void refcounter_destroy_lock(refcounter_t* refcounter) {
#ifndef REFCOUNTER_ATOMIC
  platform_lock_destroy(&refcounter->lock);
#else
  // No lock to destroy when using atomics
  (void)refcounter;  // Suppress unused parameter warning
#endif
}