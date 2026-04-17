//
// Created by victor on 3/18/25.
//

#ifndef WAVEDB_THREADDING_H
#define WAVEDB_THREADDING_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if _WIN32
#include <windows.h>
#define sched_yield() SwitchToThread()
#define PLATFORMLOCKTYPE(N) CRITICAL_SECTION N
#define PLATFORMLOCKTYPEPTR(N) CRITICAL_SECTION* N
#define PLATFORMCONDITIONTYPE(N) CONDITION_VARIABLE N
#define PLATFORMCONDITIONTYPEPTR(N) CONDITION_VARIABLE* N
#define PLATFORMBARRIERTYPE(N) SYNCHRONIZATION_BARRIER N
#define PLATFORMTHREADTYPE HANDLE
#define PLATFORMRWLOCKTYPE(N) SRWLOCK N
void platform_lock(CRITICAL_SECTION* lock);
void platform_unlock(CRITICAL_SECTION* lock);
void platform_rw_lock_r(SRWLOCK* lock);
void platform_rw_lock_w(SRWLOCK* lock);
void platform_rw_unlock_r(SRWLOCK* lock);
void platform_rw_unlock_w(SRWLOCK* lock);
void platform_lock_init(CRITICAL_SECTION* lock);
void platform_rw_lock_init(SRWLOCK* lock);
void platform_lock_destroy(CRITICAL_SECTION* lock);
void platform_rw_lock_destroy(SRWLOCK* lock);
void platform_condition_init(CONDITION_VARIABLE* condition);
void platform_condition_wait(CRITICAL_SECTION* lock, CONDITION_VARIABLE* condition);
void platform_condition_destroy(CONDITION_VARIABLE* condition);
void platform_signal_condition(CONDITION_VARIABLE* condition);
void platform_broadcast_condition(CONDITION_VARIABLE* condition);
void platform_barrier_init(SYNCHRONIZATION_BARRIER* barrier, long count);
int platform_barrier_wait(SYNCHRONIZATION_BARRIER* barrier);
void platform_barrier_destroy(SYNCHRONIZATION_BARRIER* barrier);
int platform_join(HANDLE thread);
int platform_core_count();
uint64_t platform_self();
#else
#include <pthread.h>
#include <sched.h>
#define PLATFORMLOCKTYPE(N) pthread_mutex_t N
#define PLATFORMLOCKTYPEPTR(N) pthread_mutex_t* N
#define PLATFORMCONDITIONTYPE(N) pthread_cond_t N
#define PLATFORMCONDITIONTYPEPTR(N) pthread_cond_t* N
#define PLATFORMBARRIERTYPE(N) pthread_barrier_t N
#define PLATFORMTHREADTYPE pthread_t
#define PLATFORMRWLOCKTYPE(N) pthread_rwlock_t N
void platform_lock(pthread_mutex_t* lock);
void platform_unlock(pthread_mutex_t* lock);
void platform_rw_lock_r(pthread_rwlock_t* lock);
void platform_rw_lock_w(pthread_rwlock_t* lock);
void platform_rw_unlock_r(pthread_rwlock_t* lock);
void platform_rw_unlock_w(pthread_rwlock_t* lock);
void platform_lock_init(pthread_mutex_t* lock);
void platform_rw_lock_init(pthread_rwlock_t* lock);
void platform_lock_destroy(pthread_mutex_t* lock);
void platform_rw_lock_destroy(pthread_rwlock_t* lock);
void platform_condition_init(pthread_cond_t* condition);
void platform_condition_wait(pthread_mutex_t* lock, pthread_cond_t* condition);
void platform_condition_destroy(pthread_cond_t* condition);
void platform_signal_condition(pthread_cond_t* condition);
void platform_broadcast_condition(pthread_cond_t* condition);
void platform_barrier_init(pthread_barrier_t* barrier, unsigned int count);
int platform_barrier_wait(pthread_barrier_t* barrier);
void platform_barrier_destroy(pthread_barrier_t* barrier);
int platform_join(pthread_t thread);
int platform_core_count();
uint64_t platform_self();
#endif

// Spin-loop hint: reduces power consumption and improves performance
// on SMT/HT processors during busy-wait loops
#if defined(__x86_64__) || defined(__i386__)
#define cpu_relax() __asm__ __volatile__("pause" ::: "memory")
#elif defined(__aarch64__)
#define cpu_relax() __asm__ __volatile__("yield" ::: "memory")
#else
#define cpu_relax() ((void)0)
#endif

/* Adaptive spinlock - spins up to 128 iterations, then yields */
typedef struct {
    uint8_t state;  /* 0=unlocked, 1=exclusive (accessed via __atomic builtins) */
} spinlock_t;

static inline void spinlock_init(spinlock_t* l) {
    __atomic_store_n(&l->state, 0, __ATOMIC_RELEASE);
}

static inline void spinlock_lock(spinlock_t* l) {
    int spins = 0;
    while (__atomic_exchange_n(&l->state, 1, __ATOMIC_ACQUIRE)) {
        spins++;
        if (spins > 128) {
            sched_yield();
            spins = 0;
        }
        cpu_relax();
    }
}

static inline void spinlock_unlock(spinlock_t* l) {
    __atomic_store_n(&l->state, 0, __ATOMIC_RELEASE);
}

static inline void spinlock_destroy(spinlock_t* l) {
    (void)l; /* No-op — nothing to destroy */
}

// Cache-line-aware memory layout macros
// Apple M-series uses 128-byte L1 cache lines; most others use 64 bytes
#if defined(__aarch64__) && defined(__APPLE__)
#define CACHE_LINE_SIZE 128
#else
#define CACHE_LINE_SIZE 64
#endif

#define CACHE_ALIGNED __attribute__((aligned(CACHE_LINE_SIZE)))

#ifdef __cplusplus
}
#endif

#endif //WAVEDB_THREADDING_H