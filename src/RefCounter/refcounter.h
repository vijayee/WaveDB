//
// Created by victor on 3/18/25.
//

#ifndef WAVEDB_REFCOUNTER_H
#define WAVEDB_REFCOUNTER_H
#include <stdint.h>
#include "../Util/threadding.h"

#ifdef REFCOUNTER_ATOMIC
  #ifdef __cplusplus
    #include <atomic>
  #else
    #include <stdatomic.h>
  #endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define REFERENCE(N,T) (T*) refcounter_reference((refcounter_t*) N)
#define YIELD(N) refcounter_yield((refcounter_t*) N)
#define DEREFERENCE(N) refcounter_dereference((refcounter_t*) N); N = NULL
#define DESTROY(N,T)  T##_destroy(N); N = NULL
#define CONSUME(N, T) (T*) refcounter_consume((refcounter_t**) &N)

typedef struct refcounter_t {
#ifdef REFCOUNTER_ATOMIC
  #ifdef __cplusplus
    ::std::atomic<uint_fast16_t> count;
    ::std::atomic<uint_fast8_t> yield;
  #else
    atomic_uint_fast16_t count;
    atomic_uint_fast8_t yield;
  #endif
#else
  uint16_t count;
  uint8_t yield;
  PLATFORMLOCKTYPE(lock);
#endif
} refcounter_t;

void refcounter_init(refcounter_t* refcounter);
void refcounter_yield(refcounter_t* refcounter);
void* refcounter_reference(refcounter_t* refcounter);
void refcounter_dereference(refcounter_t* refcounter);
refcounter_t* refcounter_consume(refcounter_t** refcounter);
uint16_t refcounter_count(refcounter_t* refcounter);
void refcounter_destroy_lock(refcounter_t* refcounter);

/**
 * Try to acquire a reference only if the object is still alive.
 * Returns true if reference was acquired, false if object is being destroyed.
 * This is safe to call on objects that might be in the process of destruction.
 */
#ifdef REFCOUNTER_ATOMIC
uint8_t refcounter_try_reference(refcounter_t* refcounter);
#else
uint8_t refcounter_try_reference(refcounter_t* refcounter);
#endif

#ifdef __cplusplus
}
#endif

#endif //WAVEDB_REFCOUNTER_H