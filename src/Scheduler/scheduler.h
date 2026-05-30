//
// Minimal stub for scheduler.h — provides types/functions needed by actor.c.
// This will be overwritten by Task 1.4 (Import scheduler) with the full scheduler.
//

#ifndef WAVEDB_SCHEDULER_H
#define WAVEDB_SCHEDULER_H

#include "../Actor/actor.h"
#include <stddef.h>

#define MAILBOX_MUTE_THRESHOLD 64

typedef struct scheduler_t {
    actor_t* current;
} scheduler_t;

typedef struct scheduler_pool_t {
    int _unused;
} scheduler_pool_t;

scheduler_t* scheduler_get_current(void);
void scheduler_inject(scheduler_pool_t* pool, actor_t* actor);

#endif /* WAVEDB_SCHEDULER_H */
