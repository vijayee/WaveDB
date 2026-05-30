//
// Created by victor on 5/30/26.
//

#include "timer_actor.h"
#include "../Platform/platform_thread.h"
#include "../Actor/actor.h"
#include "../Actor/message.h"
#include "../Util/allocator.h"
#include <stdlib.h>
#include <string.h>

#if _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
#else
  #include <unistd.h>
#endif

#if !_WIN32
  #include <time.h>
#endif

/* ---- Internal timer entry (sorted linked list by deadline_ms) ---- */

typedef struct timer_entry_t {
    timer_id_t id;
    uint64_t deadline_ms;         /* Absolute monotonic deadline */
    uint64_t interval_ms;         /* 0 = one-shot, >0 = repeating */
    char* key;                    /* strdup'd, NULL for non-debounce */

    /* Debounce starvation protection */
    uint64_t first_call_ms;       /* Monotonic time of first debounce call (0 = not set) */
    uint64_t max_wait_ms;         /* Max ms from first_call before forced fire (0 = none) */

    /* Actor path */
    actor_t* target_actor;
    uint32_t completion_type;

    /* Raw-callback path */
    timer_callback_t cb;
    timer_abort_t abort_cb;
    void* ctx;

    int is_actor_target;          /* 1 = actor_send, 0 = raw callback */
    int cancelled;                /* 1 = lazy deletion marker */

    struct timer_entry_t* next;   /* Sorted by deadline_ms ascending */
} timer_entry_t;

struct timer_actor_t {
    platform_thread_t* thread;
    platform_mutex_t* mutex;
    timer_entry_t* head;
    timer_id_t next_id;
    volatile int running;
};

/* ---- Platform sleep ---- */

static void _sleep_ms(uint64_t ms) {
#if _WIN32
    Sleep((DWORD)ms);
#else
    usleep((useconds_t)(ms * 1000));
#endif
}

/* ---- Monotonic clock ---- */

uint64_t timer_actor_now_ms(void) {
#if _WIN32
    LARGE_INTEGER freq, count;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&count);
    return (uint64_t)(count.QuadPart * 1000ULL / freq.QuadPart);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
#endif
}

/* ---- Sorted list helpers ---- */

static void _sorted_insert(timer_entry_t** head, timer_entry_t* entry) {
    if (*head == NULL || entry->deadline_ms < (*head)->deadline_ms) {
        entry->next = *head;
        *head = entry;
        return;
    }
    timer_entry_t* cur = *head;
    while (cur->next != NULL && cur->next->deadline_ms <= entry->deadline_ms) {
        cur = cur->next;
    }
    entry->next = cur->next;
    cur->next = entry;
}

static void _free_entry(timer_entry_t* entry) {
    free(entry->key);
    free(entry);
}

static int _remove_by_id(timer_entry_t** head, timer_id_t id) {
    timer_entry_t* cur = *head;
    timer_entry_t* prev = NULL;
    while (cur != NULL) {
        if (cur->id == id) {
            if (prev == NULL)
                *head = cur->next;
            else
                prev->next = cur->next;
            cur->next = NULL;
            _free_entry(cur);
            return 1;
        }
        prev = cur;
        cur = cur->next;
    }
    return 0;
}

static timer_entry_t* _remove_by_key(timer_entry_t** head, const char* key) {
    timer_entry_t* cur = *head;
    timer_entry_t* prev = NULL;
    while (cur != NULL) {
        if (cur->key && strcmp(cur->key, key) == 0) {
            if (prev == NULL)
                *head = cur->next;
            else
                prev->next = cur->next;
            cur->next = NULL;
            return cur;
        }
        prev = cur;
        cur = cur->next;
    }
    return NULL;
}

/* ---- Timer thread ---- */

static void _fire_entry(timer_entry_t* entry) {
    if (entry->cancelled) return;

    if (entry->is_actor_target && entry->target_actor) {
        message_t msg;
        msg.type = entry->completion_type;
        msg.payload = NULL;
        msg.payload_destroy = NULL;
        actor_send(entry->target_actor, &msg);
    } else if (!entry->is_actor_target && entry->cb) {
        entry->cb(entry->ctx);
    }
}

static void* _timer_thread(void* arg) {
    timer_actor_t* ta = (timer_actor_t*)arg;

    platform_thread_setup_stack();

    while (ta->running) {
        platform_mutex_lock(ta->mutex);

        uint64_t now = timer_actor_now_ms();

        /* Collect expired timers */
        timer_entry_t* expired_head = NULL;
        timer_entry_t** expired_tail = &expired_head;
        timer_entry_t* remaining_head = NULL;
        timer_entry_t** remaining_tail = &remaining_head;

        timer_entry_t* cur = ta->head;
        while (cur != NULL) {
            timer_entry_t* next = cur->next;
            cur->next = NULL;
            if (cur->deadline_ms <= now) {
                *expired_tail = cur;
                expired_tail = &cur->next;
            } else {
                *remaining_tail = cur;
                remaining_tail = &cur->next;
            }
            cur = next;
        }
        ta->head = remaining_head;

        /* Compute sleep to nearest remaining timer */
        uint64_t sleep_ms = 0;
        if (ta->head != NULL && !ta->head->cancelled && ta->head->deadline_ms > now)
            sleep_ms = ta->head->deadline_ms - now;

        platform_mutex_unlock(ta->mutex);

        /* Fire expired timers (outside lock) */
        timer_entry_t* fe = expired_head;
        while (fe != NULL) {
            timer_entry_t* next = fe->next;

            if (!fe->cancelled)
                _fire_entry(fe);

            if (!fe->cancelled && fe->interval_ms > 0) {
                fe->deadline_ms = now + fe->interval_ms;
                fe->cancelled = 0;
                fe->next = NULL;
                platform_mutex_lock(ta->mutex);
                _sorted_insert(&ta->head, fe);
                platform_mutex_unlock(ta->mutex);
                fe = next;
                continue;
            }
            _free_entry(fe);
            fe = next;
        }

        /* Sleep in 1ms chunks so cancel/set wake up promptly */
        if (sleep_ms > 0) {
            if (sleep_ms > 100)
                sleep_ms = 100;
            _sleep_ms(sleep_ms);
        } else {
            _sleep_ms(1);
        }
    }

    /* Shutdown: fire abort callbacks for remaining entries */
    platform_mutex_lock(ta->mutex);
    timer_entry_t* cur = ta->head;
    while (cur != NULL) {
        if (!cur->cancelled && !cur->is_actor_target && cur->abort_cb)
            cur->abort_cb(cur->ctx);
        timer_entry_t* next = cur->next;
        _free_entry(cur);
        cur = next;
    }
    ta->head = NULL;
    platform_mutex_unlock(ta->mutex);

    return NULL;
}

/* ---- Public API ---- */

timer_actor_t* timer_actor_create(void) {
    timer_actor_t* ta = (timer_actor_t*)get_clear_memory(sizeof(timer_actor_t));
    if (ta == NULL) return NULL;

    ta->mutex = platform_mutex_create();
    if (ta->mutex == NULL) { free(ta); return NULL; }

    ta->next_id = 1;
    ta->running = 1;

    ta->thread = platform_thread_create(_timer_thread, ta);
    if (ta->thread == NULL) {
        platform_mutex_destroy(ta->mutex);
        free(ta);
        return NULL;
    }

    return ta;
}

void timer_actor_destroy(timer_actor_t* ta) {
    if (ta == NULL) return;
    ta->running = 0;
    platform_thread_join(ta->thread);
    platform_mutex_destroy(ta->mutex);
    free(ta);
}

static timer_id_t _next_id(timer_actor_t* ta) {
    timer_id_t id = ta->next_id++;
    if (id == 0) id = ta->next_id++;
    return id;
}

/* ---- Actor-message path ---- */

timer_id_t timer_actor_set(timer_actor_t* ta,
                            uint64_t timeout_ms,
                            uint64_t interval_ms,
                            actor_t* target_actor,
                            uint32_t completion_type) {
    timer_entry_t* entry = (timer_entry_t*)get_clear_memory(sizeof(timer_entry_t));
    if (entry == NULL) return 0;

    entry->deadline_ms = timer_actor_now_ms() + timeout_ms;
    entry->interval_ms = interval_ms;
    entry->target_actor = target_actor;
    entry->completion_type = completion_type;
    entry->is_actor_target = 1;
    entry->cancelled = 0;

    platform_mutex_lock(ta->mutex);
    entry->id = _next_id(ta);
    _sorted_insert(&ta->head, entry);
    platform_mutex_unlock(ta->mutex);
    return entry->id;
}

int timer_actor_cancel(timer_actor_t* ta, timer_id_t timer_id) {
    if (timer_id == 0) return 0;
    platform_mutex_lock(ta->mutex);
    int rc = _remove_by_id(&ta->head, timer_id);
    platform_mutex_unlock(ta->mutex);
    return rc;
}

timer_id_t timer_actor_debounce(timer_actor_t* ta,
                                 const char* key,
                                 uint64_t timeout_ms,
                                 uint64_t max_wait_ms,
                                 actor_t* target_actor,
                                 uint32_t completion_type) {
    if (key == NULL) return 0;
    uint64_t now = timer_actor_now_ms();

    platform_mutex_lock(ta->mutex);

    /* Remove existing entry with this key completely */
    timer_entry_t* old = _remove_by_key(&ta->head, key);
    uint64_t first_call = (old != NULL) ? old->first_call_ms : now;
    uint64_t max_deadline = (max_wait_ms > 0) ? first_call + max_wait_ms : UINT64_MAX;
    if (old != NULL) _free_entry(old);

    timer_entry_t* entry = (timer_entry_t*)get_clear_memory(sizeof(timer_entry_t));
    if (entry == NULL) { platform_mutex_unlock(ta->mutex); return 0; }

    /* Clamp deadline to not exceed max_wait from first call */
    uint64_t desired = now + timeout_ms;
    entry->deadline_ms = (desired < max_deadline) ? desired : max_deadline;
    entry->interval_ms = 0;
    entry->first_call_ms = first_call;
    entry->max_wait_ms = max_wait_ms;
    entry->key = strdup(key);
    entry->target_actor = target_actor;
    entry->completion_type = completion_type;
    entry->is_actor_target = 1;
    entry->cancelled = 0;
    entry->id = _next_id(ta);

    _sorted_insert(&ta->head, entry);
    platform_mutex_unlock(ta->mutex);
    return entry->id;
}

int timer_actor_debounce_flush(timer_actor_t* ta, const char* key) {
    if (key == NULL) return 0;
    platform_mutex_lock(ta->mutex);
    timer_entry_t* entry = _remove_by_key(&ta->head, key);
    platform_mutex_unlock(ta->mutex);

    if (entry == NULL) return 0;
    if (!entry->cancelled) {
        message_t msg;
        msg.type = entry->completion_type;
        msg.payload = NULL;
        msg.payload_destroy = NULL;
        actor_send(entry->target_actor, &msg);
    }
    _free_entry(entry);
    return 1;
}

/* ---- Raw-callback path ---- */

timer_id_t timer_actor_set_callback(timer_actor_t* ta,
                                     uint64_t timeout_ms,
                                     uint64_t interval_ms,
                                     timer_callback_t cb,
                                     timer_abort_t abort_cb,
                                     void* ctx) {
    if (cb == NULL) return 0;
    timer_entry_t* entry = (timer_entry_t*)get_clear_memory(sizeof(timer_entry_t));
    if (entry == NULL) return 0;

    entry->deadline_ms = timer_actor_now_ms() + timeout_ms;
    entry->interval_ms = interval_ms;
    entry->cb = cb;
    entry->abort_cb = abort_cb;
    entry->ctx = ctx;
    entry->is_actor_target = 0;
    entry->cancelled = 0;

    platform_mutex_lock(ta->mutex);
    entry->id = _next_id(ta);
    _sorted_insert(&ta->head, entry);
    platform_mutex_unlock(ta->mutex);
    return entry->id;
}

timer_id_t timer_actor_debounce_callback(timer_actor_t* ta,
                                          const char* key,
                                          uint64_t timeout_ms,
                                          uint64_t max_wait_ms,
                                          timer_callback_t cb,
                                          timer_abort_t abort_cb,
                                          void* ctx) {
    if (key == NULL || cb == NULL) return 0;
    uint64_t now = timer_actor_now_ms();

    platform_mutex_lock(ta->mutex);

    /* Remove existing entry with this key completely */
    timer_entry_t* old = _remove_by_key(&ta->head, key);
    uint64_t first_call = (old != NULL) ? old->first_call_ms : now;
    uint64_t max_deadline = (max_wait_ms > 0) ? first_call + max_wait_ms : UINT64_MAX;
    if (old != NULL) _free_entry(old);

    timer_entry_t* entry = (timer_entry_t*)get_clear_memory(sizeof(timer_entry_t));
    if (entry == NULL) { platform_mutex_unlock(ta->mutex); return 0; }

    /* Clamp deadline to not exceed max_wait from first call */
    uint64_t desired = now + timeout_ms;
    entry->deadline_ms = (desired < max_deadline) ? desired : max_deadline;
    entry->interval_ms = 0;
    entry->first_call_ms = first_call;
    entry->max_wait_ms = max_wait_ms;
    entry->key = strdup(key);
    entry->cb = cb;
    entry->abort_cb = abort_cb;
    entry->ctx = ctx;
    entry->is_actor_target = 0;
    entry->cancelled = 0;
    entry->id = _next_id(ta);

    _sorted_insert(&ta->head, entry);
    platform_mutex_unlock(ta->mutex);
    return entry->id;
}

int timer_actor_debounce_flush_callback(timer_actor_t* ta, const char* key) {
    if (key == NULL) return 0;
    platform_mutex_lock(ta->mutex);
    timer_entry_t* entry = _remove_by_key(&ta->head, key);
    platform_mutex_unlock(ta->mutex);

    if (entry == NULL) return 0;
    if (!entry->cancelled && entry->cb)
        entry->cb(entry->ctx);
    _free_entry(entry);
    return 1;
}
