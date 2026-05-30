#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../src/Actor/actor.h"
#include "../src/Actor/message.h"
#include "../src/Scheduler/scheduler.h"
#include "../src/Util/allocator.h"

/* Simple counter actor for testing */
typedef struct {
    actor_t actor;
    ATOMIC(int) count;
    ATOMIC(int) errors;
} counter_actor_t;

static void counter_dispatch(void* state, message_t* msg) {
    counter_actor_t* ca = (counter_actor_t*)state;
    switch (msg->type) {
        case 100: {  /* MSG_INCREMENT */
            int* val = (int*)msg->payload;
            ATOMIC_FETCH_ADD(&ca->count, *val);
            break;
        }
        default:
            ATOMIC_FETCH_ADD(&ca->errors, 1);
            break;
    }
    if (msg->payload_destroy && msg->payload) {
        msg->payload_destroy(msg->payload);
        msg->payload = NULL;  /* Signal CONSUME: actor_run won't re-free */
    }
}

static int test_basic_message_passing(void) {
    scheduler_pool_t* pool = scheduler_pool_create(4);
    scheduler_pool_start(pool);

    counter_actor_t ca;
    memset(&ca, 0, sizeof(ca));
    actor_init(&ca.actor, &ca, counter_dispatch, pool);

    /* Send 100 messages */
    for (int i = 0; i < 100; i++) {
        int* val = malloc(sizeof(int));
        *val = 1;
        message_t msg = { .type = 100, .payload = val, .payload_destroy = free };
        actor_send(&ca.actor, &msg);
    }

    /* Wait for processing */
    scheduler_pool_wait_for_idle(pool);

    scheduler_pool_stop(pool);
    actor_destroy(&ca.actor);
    scheduler_pool_destroy(pool);

    if (ca.count != 100) {
        printf("FAIL: expected 100, got %d\n", (int)ca.count);
        return 1;
    }
    if (ca.errors != 0) {
        printf("FAIL: expected 0 errors, got %d\n", (int)ca.errors);
        return 1;
    }
    printf("PASS: basic message passing\n");
    return 0;
}

static int test_multiple_actors(void) {
    scheduler_pool_t* pool = scheduler_pool_create(4);
    scheduler_pool_start(pool);

    #define N_ACTORS 16
    #define N_MSGS 500

    counter_actor_t actors[N_ACTORS];
    for (int i = 0; i < N_ACTORS; i++) {
        memset(&actors[i], 0, sizeof(counter_actor_t));
        actor_init(&actors[i].actor, &actors[i], counter_dispatch, pool);
    }

    for (int i = 0; i < N_ACTORS; i++) {
        for (int j = 0; j < N_MSGS; j++) {
            int* val = malloc(sizeof(int));
            *val = 1;
            message_t msg = { .type = 100, .payload = val, .payload_destroy = free };
            actor_send(&actors[i].actor, &msg);
        }
    }

    scheduler_pool_wait_for_idle(pool);

    scheduler_pool_stop(pool);
    for (int i = 0; i < N_ACTORS; i++) {
        actor_destroy(&actors[i].actor);
        if (actors[i].count != N_MSGS) {
            printf("FAIL: actor %d expected %d, got %d\n", i, N_MSGS, (int)actors[i].count);
            scheduler_pool_destroy(pool);
            return 1;
        }
    }
    scheduler_pool_destroy(pool);

    printf("PASS: multiple actors (%d x %d msgs)\n", N_ACTORS, N_MSGS);
    return 0;
}

static int test_backpressure(void) {
    scheduler_pool_t* pool = scheduler_pool_create(4);
    counter_actor_t ca;
    memset(&ca, 0, sizeof(ca));
    actor_init(&ca.actor, &ca, counter_dispatch, pool);

    /* Send messages BEFORE starting the pool so the queue fills
       up without workers draining it concurrently */
    for (int i = 0; i < 300; i++) {
        int* val = malloc(sizeof(int));
        *val = 1;
        message_t msg = { .type = 100, .payload = val, .payload_destroy = free };
        actor_send(&ca.actor, &msg);
    }

    /* After 256, the actor should be PRESSURED */
    int flags = ATOMIC_LOAD(&ca.actor.flags);
    if (!(flags & ACTOR_FLAG_PRESSURED)) {
        printf("FAIL: expected PRESSURED flag after 300 messages\n");
        scheduler_pool_destroy(pool);
        actor_destroy(&ca.actor);
        return 1;
    }

    /* Now start the pool — workers will drain the queue */
    scheduler_pool_start(pool);
    scheduler_pool_wait_for_idle(pool);

    /* After drain, PRESSURE should be released */
    flags = ATOMIC_LOAD(&ca.actor.flags);
    if (flags & ACTOR_FLAG_PRESSURED) {
        printf("FAIL: expected PRESSURED released after drain\n");
        scheduler_pool_stop(pool);
        actor_destroy(&ca.actor);
        scheduler_pool_destroy(pool);
        return 1;
    }

    if (ca.count != 300) {
        printf("FAIL: expected 300, got %d\n", (int)ca.count);
        scheduler_pool_stop(pool);
        actor_destroy(&ca.actor);
        scheduler_pool_destroy(pool);
        return 1;
    }

    scheduler_pool_stop(pool);
    actor_destroy(&ca.actor);
    scheduler_pool_destroy(pool);

    printf("PASS: backpressure apply/release\n");
    return 0;
}

int main(void) {
    int failures = 0;
    failures += test_basic_message_passing();
    failures += test_multiple_actors();
    failures += test_backpressure();
    printf("\n%d failures\n", failures);
    return failures;
}
