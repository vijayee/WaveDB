//
// Created by victor on 3/18/25.
//
#include "refcounter.h"
#include <stdint.h>
#include <limits.h>
#include <stdatomic.h>

void refcounter_init(refcounter_t* refcounter) {
    if (refcounter == NULL) return;
    atomic_store(&refcounter->count, 1);
    atomic_store(&refcounter->yield, 0);
}

void refcounter_yield(refcounter_t* refcounter) {
    if (refcounter == NULL) return;
    atomic_fetch_add(&refcounter->yield, 1);
}

void* refcounter_reference(refcounter_t* refcounter) {
    if (refcounter == NULL) return NULL;
    // Try to consume a yield first, then fall back to incrementing count
    uint8_t expected_yield = atomic_load(&refcounter->yield);
    while (expected_yield > 0) {
        if (atomic_compare_exchange_weak(&refcounter->yield, &expected_yield, expected_yield - 1)) {
            return refcounter;
        }
        // CAS failed, expected_yield was updated with current value, retry
    }
    // No yield to consume, increment count normally
    atomic_fetch_add(&refcounter->count, 1);
    return refcounter;
}

void refcounter_dereference(refcounter_t* refcounter) {
    if (refcounter == NULL) return;
    // Try to consume a yield first (matching refcounter_reference pattern)
    uint8_t expected_yield = atomic_load(&refcounter->yield);
    while (expected_yield > 0) {
        if (atomic_compare_exchange_weak(&refcounter->yield, &expected_yield, expected_yield - 1)) {
            return;  // Consumed a yield, don't decrement count
        }
    }
    // No yield to consume, decrement count
    uint16_t count_val = atomic_load(&refcounter->count);
    if (count_val > 0) {
        atomic_fetch_sub(&refcounter->count, 1);
    }
}

uint16_t refcounter_count(refcounter_t* refcounter) {
    if (refcounter == NULL) return 0;
    return (uint16_t)atomic_load(&refcounter->count);
}

refcounter_t* refcounter_consume(refcounter_t** refcounter) {
    if (refcounter == NULL || *refcounter == NULL) return NULL;
    refcounter_yield(*refcounter);
    refcounter_t* holder = *refcounter;
    *refcounter = NULL;
    return holder;
}