//
// Created by victor on 5/30/26.
//

#ifndef WAVEDB_TIMER_ACTOR_H
#define WAVEDB_TIMER_ACTOR_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declarations */
typedef struct timer_actor_t timer_actor_t;
typedef struct actor_t actor_t;
typedef uint64_t timer_id_t;

/* Timer expiry callback (raw-callback path) */
typedef void (*timer_callback_t)(void* ctx);
typedef void (*timer_abort_t)(void* ctx);

/**
 * Monotonic timestamp in milliseconds.
 * Uses CLOCK_MONOTONIC on POSIX, QueryPerformanceCounter on Windows.
 */
uint64_t timer_actor_now_ms(void);

/**
 * Create and start a timer actor with its own background thread.
 * Returns NULL on failure.
 */
timer_actor_t* timer_actor_create(void);

/**
 * Stop the timer thread, fire abort callbacks for all pending timers,
 * then destroy the actor. Blocks until the thread exits.
 */
void timer_actor_destroy(timer_actor_t* ta);

/* ---- Actor-message path (preferred) ---- */

/**
 * Schedule a one-shot or repeating timer.
 * On expiry, sends message_t{.type = completion_type, .payload = NULL}
 * to target_actor via actor_send().
 * interval_ms=0 for one-shot, >0 for repeating.
 * Returns non-zero timer_id on success, 0 on failure.
 */
timer_id_t timer_actor_set(timer_actor_t* ta,
                            uint64_t timeout_ms,
                            uint64_t interval_ms,
                            actor_t* target_actor,
                            uint32_t completion_type);

/**
 * Cancel a pending timer. Safe to call on already-fired or invalid ID.
 * Returns 1 if found and cancelled, 0 if not found.
 */
int timer_actor_cancel(timer_actor_t* ta, timer_id_t timer_id);

/**
 * Debounce: cancel-rearm per key. Only one debounce timer per key.
 * max_wait_ms>0 prevents starvation (force fire after max_wait_ms from
 * first call). Pass 0 for no max-wait limit.
 * Returns timer_id or 0 on failure.
 */
timer_id_t timer_actor_debounce(timer_actor_t* ta,
                                 const char* key,
                                 uint64_t timeout_ms,
                                 uint64_t max_wait_ms,
                                 actor_t* target_actor,
                                 uint32_t completion_type);

/**
 * Immediately cancel + fire a debounce timer from the calling thread.
 * Sends the completion_type message to target_actor synchronously.
 * Returns 1 if found and flushed, 0 if none.
 */
int timer_actor_debounce_flush(timer_actor_t* ta, const char* key);

/* ---- Raw-callback path (for non-actored callers) ---- */

timer_id_t timer_actor_set_callback(timer_actor_t* ta,
                                     uint64_t timeout_ms,
                                     uint64_t interval_ms,
                                     timer_callback_t cb,
                                     timer_abort_t abort_cb,
                                     void* ctx);

timer_id_t timer_actor_debounce_callback(timer_actor_t* ta,
                                          const char* key,
                                          uint64_t timeout_ms,
                                          uint64_t max_wait_ms,
                                          timer_callback_t cb,
                                          timer_abort_t abort_cb,
                                          void* ctx);

int timer_actor_debounce_flush_callback(timer_actor_t* ta, const char* key);

#ifdef __cplusplus
}
#endif

#endif /* WAVEDB_TIMER_ACTOR_H */
