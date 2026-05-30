#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../src/Database/lru_actor.h"
#include "../src/Scheduler/scheduler.h"
#include "../src/HBTrie/path.h"
#include "../src/HBTrie/identifier.h"
#include "../src/HBTrie/chunk.h"
#include "../src/Buffer/buffer.h"

/* Completion actor for async results */
typedef struct {
    actor_t actor;
    ATOMIC(uint8_t) done;
    identifier_t* result;
} lru_completion_t;

static void lru_completion_dispatch(void* state, message_t* msg) {
    lru_completion_t* c = (lru_completion_t*)state;
    if (msg->type == LRU_GET_RESULT) {
        lru_result_payload_t* r = (lru_result_payload_t*)msg->payload;
        c->result = r->value;
        r->value = NULL;
    }
    ATOMIC_STORE(&c->done, 1);
    if (msg->payload_destroy && msg->payload) {
        msg->payload_destroy(msg->payload);
        msg->payload = NULL;  /* Signal CONSUME: actor_run won't re-free */
    }
}

static identifier_t* make_identifier(const char* data) {
    buffer_t* buf = buffer_create_from_pointer_copy((uint8_t*)data, strlen(data));
    identifier_t* id = identifier_create(buf, DEFAULT_CHUNK_SIZE);
    buffer_destroy(buf);
    return id;
}

static path_t* make_path1(identifier_t* id) {
    path_t* p = path_create();
    path_append(p, id);
    return p;
}

static identifier_t* lru_async_get(scheduler_pool_t* pool, lru_actor_t* lru, path_t* path) {
    lru_completion_t comp;
    memset(&comp, 0, sizeof(comp));
    actor_init(&comp.actor, &comp, lru_completion_dispatch, pool);
    lru_actor_get(lru, path, &comp.actor);
    while (!ATOMIC_LOAD(&comp.done)) { usleep(100); }
    identifier_t* result = comp.result;
    actor_destroy(&comp.actor);
    return result;
}

static int test_put_and_get(void) {
    scheduler_pool_t* pool = scheduler_pool_create(2);
    scheduler_pool_start(pool);

    lru_actor_t* lru = lru_actor_create(1024 * 1024);
    lru->actor.pool = pool;

    identifier_t* id1 = make_identifier("value1");
    path_t* path1 = make_path1(id1);

    lru_actor_put(lru, path1, id1, NULL);
    scheduler_pool_wait_for_idle(pool);

    identifier_t* result = lru_async_get(pool, lru, path1);

    int failures = 0;
    if (result == NULL) {
        printf("FAIL: put/get expected non-NULL result\n");
        failures++;
    } else {
        printf("PASS: lru put/get returns stored value\n");
        DESTROY(result, identifier);
    }

    DESTROY(path1, path);
    DESTROY(id1, identifier);
    lru_actor_destroy(lru);
    scheduler_pool_stop(pool);
    scheduler_pool_destroy(pool);
    return failures;
}

static int test_get_missing(void) {
    scheduler_pool_t* pool = scheduler_pool_create(2);
    scheduler_pool_start(pool);

    lru_actor_t* lru = lru_actor_create(1024 * 1024);
    lru->actor.pool = pool;

    identifier_t* id1 = make_identifier("nonexistent");
    path_t* path1 = make_path1(id1);

    identifier_t* result = lru_async_get(pool, lru, path1);

    int failures = 0;
    if (result != NULL) {
        printf("FAIL: get missing key expected NULL result\n");
        failures++;
        DESTROY(result, identifier);
    } else {
        printf("PASS: get missing key returns NULL\n");
    }

    DESTROY(path1, path);
    DESTROY(id1, identifier);
    lru_actor_destroy(lru);
    scheduler_pool_stop(pool);
    scheduler_pool_destroy(pool);
    return failures;
}

static int test_delete(void) {
    scheduler_pool_t* pool = scheduler_pool_create(2);
    scheduler_pool_start(pool);

    lru_actor_t* lru = lru_actor_create(1024 * 1024);
    lru->actor.pool = pool;

    identifier_t* id1 = make_identifier("delete_me");
    path_t* path1 = make_path1(id1);

    lru_actor_put(lru, path1, id1, NULL);
    scheduler_pool_wait_for_idle(pool);

    lru_actor_delete(lru, path1);
    scheduler_pool_wait_for_idle(pool);

    identifier_t* result = lru_async_get(pool, lru, path1);

    int failures = 0;
    if (result != NULL) {
        printf("FAIL: get after delete expected NULL\n");
        failures++;
        DESTROY(result, identifier);
    } else {
        printf("PASS: delete removes entry\n");
    }

    DESTROY(path1, path);
    DESTROY(id1, identifier);
    lru_actor_destroy(lru);
    scheduler_pool_stop(pool);
    scheduler_pool_destroy(pool);
    return failures;
}

static int test_overwrite(void) {
    scheduler_pool_t* pool = scheduler_pool_create(2);
    scheduler_pool_start(pool);

    lru_actor_t* lru = lru_actor_create(1024 * 1024);
    lru->actor.pool = pool;

    identifier_t* id1 = make_identifier("first");
    identifier_t* id2 = make_identifier("second");
    path_t* path1 = make_path1(id1);

    lru_actor_put(lru, path1, id1, NULL);
    scheduler_pool_wait_for_idle(pool);

    /* Put again with same path but different value */
    lru_actor_put(lru, path1, id2, NULL);
    scheduler_pool_wait_for_idle(pool);

    identifier_t* result = lru_async_get(pool, lru, path1);

    int failures = 0;
    if (result == NULL) {
        printf("FAIL: get after overwrite expected non-NULL\n");
        failures++;
    } else {
        printf("PASS: overwrite updates value\n");
        DESTROY(result, identifier);
    }

    DESTROY(path1, path);
    DESTROY(id1, identifier);
    DESTROY(id2, identifier);
    lru_actor_destroy(lru);
    scheduler_pool_stop(pool);
    scheduler_pool_destroy(pool);
    return failures;
}

static int test_eviction(void) {
    scheduler_pool_t* pool = scheduler_pool_create(2);
    scheduler_pool_start(pool);

    /* Create LRU with very small memory limit to force eviction */
    lru_actor_t* lru = lru_actor_create(256);
    lru->actor.pool = pool;

    identifier_t* id1 = make_identifier("aaaaaaaaaaaaaaaaaaaa");  /* ~20 bytes */
    identifier_t* id2 = make_identifier("bbbbbbbbbbbbbbbbbbbb");  /* ~20 bytes */
    identifier_t* id3 = make_identifier("cccccccccccccccccccc");  /* ~20 bytes */
    path_t* path1 = make_path1(id1);
    path_t* path2 = make_path1(id2);
    path_t* path3 = make_path1(id3);

    lru_actor_put(lru, path1, id1, NULL);
    scheduler_pool_wait_for_idle(pool);

    lru_actor_put(lru, path2, id2, NULL);
    scheduler_pool_wait_for_idle(pool);

    /* Third put should evict oldest entry (path1) */
    lru_actor_put(lru, path3, id3, NULL);
    scheduler_pool_wait_for_idle(pool);

    int failures = 0;

    /* path1 should be evicted */
    identifier_t* r1 = lru_async_get(pool, lru, path1);
    if (r1 != NULL) {
        printf("FAIL: oldest entry should have been evicted\n");
        failures++;
        DESTROY(r1, identifier);
    } else {
        printf("PASS: eviction removed oldest entry\n");
    }

    /* path2 and path3 should still be present */
    identifier_t* r2 = lru_async_get(pool, lru, path2);
    if (r2 == NULL) {
        printf("FAIL: newer entry should not be evicted\n");
        failures++;
    } else {
        printf("PASS: newer entry survived eviction\n");
        DESTROY(r2, identifier);
    }

    DESTROY(path1, path);
    DESTROY(path2, path);
    DESTROY(path3, path);
    DESTROY(id1, identifier);
    DESTROY(id2, identifier);
    DESTROY(id3, identifier);
    lru_actor_destroy(lru);
    scheduler_pool_stop(pool);
    scheduler_pool_destroy(pool);
    return failures;
}

int main(void) {
    int failures = 0;
    failures += test_put_and_get();
    failures += test_get_missing();
    failures += test_delete();
    failures += test_overwrite();
    failures += test_eviction();
    printf("\n%d failures\n", failures);
    return failures;
}
