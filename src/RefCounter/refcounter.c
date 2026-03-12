//
// Created by victor on 3/18/25.
//
#include "refcounter.h"
#include <stdint.h>
#include <limits.h>

void refcounter_init(refcounter_t* refcounter) {
#ifndef REFCOUNTER_ATOMIC
  platform_lock_init(&refcounter->lock);
  platform_lock(&refcounter->lock);
  refcounter->count++;
  platform_unlock(&refcounter->lock);
#else
  refcounter->count++;
#endif
}

void refcounter_yield(refcounter_t* refcounter) {
#ifndef REFCOUNTER_ATOMIC
  platform_lock(&refcounter->lock);
  refcounter->yield++;
  platform_unlock(&refcounter->lock);
#else
  refcounter->yield++;
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
  if (refcounter->yield > 0) {
    refcounter->yield--;
  } else if (refcounter->count < USHRT_MAX) {
    refcounter->count++;
  }
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
  if ((refcounter->yield == 0) && (refcounter->count > 0)) {
    refcounter->count--;
  }
#endif
}

uint16_t refcounter_count(refcounter_t* refcounter) {
  uint16_t count;
#ifndef REFCOUNTER_ATOMIC
  platform_lock(&refcounter->lock);
  count = refcounter->count;
  platform_unlock(&refcounter->lock);
#else
  count = refcounter->count;
#endif
  return count;
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
#endif
}