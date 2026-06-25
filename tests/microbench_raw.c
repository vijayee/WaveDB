// Microbenchmark: Compare sync_raw vs async_raw API overhead
// Build: gcc -O2 -o microbench_raw microbench_raw.c -L../build-release -lwavedb -I../src -lpthread -lm
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>

#include "Database/database.h"
#include "Database/database_config.h"
#include "Workers/promise.h"
#include "Workers/error.h"
#include "Workers/pool.h"
#include "Time/wheel.h"

static double now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e6 + ts.tv_nsec / 1e3;
}

// ---- Benchmark with raw sync API (no persistence) ----
static void bench_sync_raw(const char* db_path, int n_ops) {
    int error = 0;
    work_pool_t* pool = work_pool_create(4);
    hierarchical_timing_wheel_t* wheel = hierarchical_timing_wheel_create(1000, pool);
    database_t* db = database_create(db_path, 0, NULL, 4, 4096, 0, pool, wheel, &error);

    printf("\n=== Sync Raw API (no persist) ===\n");

    // Benchmark Put
    double start = now_us();
    for (int i = 0; i < n_ops; i++) {
        char key[64], val[64];
        snprintf(key, sizeof(key), "key%08d", i);
        snprintf(val, sizeof(val), "value%08d", i);
        database_put_sync_raw(db, key, strlen(key), '\0', (uint8_t*)val, strlen(val));
    }
    double put_elapsed = now_us() - start;
    printf("Put: %d ops in %.0f us = %.0f ops/sec (%.1f us/op)\n",
           n_ops, put_elapsed, n_ops / (put_elapsed / 1e6), put_elapsed / n_ops);

    // Benchmark Get
    start = now_us();
    for (int i = 0; i < n_ops; i++) {
        char key[64];
        snprintf(key, sizeof(key), "key%08d", i);
        uint8_t* val = NULL;
        size_t val_len = 0;
        database_get_sync_raw(db, key, strlen(key), '\0', &val, &val_len);
        if (val) database_raw_value_free(val);
    }
    double get_elapsed = now_us() - start;
    printf("Get: %d ops in %.0f us = %.0f ops/sec (%.1f us/op)\n",
           n_ops, get_elapsed, n_ops / (get_elapsed / 1e6), get_elapsed / n_ops);

    hierarchical_timing_wheel_stop(wheel);
    work_pool_shutdown(pool);
    work_pool_join_all(pool);
    database_destroy(db);
    hierarchical_timing_wheel_destroy(wheel);
    work_pool_destroy(pool);
}

// ---- Benchmark with sync_raw API (WITH persistence, WAL_SYNC_ASYNC) ----
static void bench_sync_raw_persist(const char* db_path, int n_ops) {
    database_config_t* config = database_config_default();
    config->enable_persist = 1;
    config->worker_threads = 4;
    database_config_set_wal_sync_mode(config, WAL_SYNC_ASYNC);
    config->lru_memory_mb = 256;
    mkdir(db_path, 0755);

    int error = 0;
    database_t* db = database_create_with_config(db_path, config, &error);
    database_config_destroy(config);

    printf("\n=== Sync Raw API (persist + WAL_ASYNC) ===\n");

    // Benchmark Put
    double start = now_us();
    for (int i = 0; i < n_ops; i++) {
        char key[64], val[64];
        snprintf(key, sizeof(key), "key%08d", i);
        snprintf(val, sizeof(val), "value%08d", i);
        database_put_sync_raw(db, key, strlen(key), '\0', (uint8_t*)val, strlen(val));
    }
    double put_elapsed = now_us() - start;
    printf("Put: %d ops in %.0f us = %.0f ops/sec (%.1f us/op)\n",
           n_ops, put_elapsed, n_ops / (put_elapsed / 1e6), put_elapsed / n_ops);

    // Benchmark Get
    start = now_us();
    for (int i = 0; i < n_ops; i++) {
        char key[64];
        snprintf(key, sizeof(key), "key%08d", i);
        uint8_t* val = NULL;
        size_t val_len = 0;
        database_get_sync_raw(db, key, strlen(key), '\0', &val, &val_len);
        if (val) database_raw_value_free(val);
    }
    double get_elapsed = now_us() - start;
    printf("Get: %d ops in %.0f us = %.0f ops/sec (%.1f us/op)\n",
           n_ops, get_elapsed, n_ops / (get_elapsed / 1e6), get_elapsed / n_ops);

    database_destroy(db);
}

// ---- Benchmark with async_raw API (simulates KVBench adapter) ----
typedef struct {
    int completed;
    int status;
    uint8_t* val;
    size_t val_len;
} async_result_t;

static void on_complete(void* ctx, void* payload) {
    async_result_t* r = (async_result_t*)ctx;
    r->status = 0;
    r->completed = 1;
}

static void on_get_complete(void* ctx, void* payload) {
    async_result_t* r = (async_result_t*)ctx;
    if (payload) {
        identifier_t* id = (identifier_t*)payload;
        REFERENCE(id, identifier_t);
        size_t len = 0;
        uint8_t* data = identifier_get_data_copy(id, &len);
        r->val = data;
        r->val_len = len;
        identifier_destroy(id);
        r->status = 0;
    } else {
        r->status = -2;
    }
    r->completed = 1;
}

static void on_error(void* ctx, async_error_t* error) {
    async_result_t* r = (async_result_t*)ctx;
    r->completed = 1;
    r->status = -1;
}

static void bench_async_raw(const char* db_path, int n_ops) {
    database_config_t* config = database_config_default();
    config->enable_persist = 1;
    config->worker_threads = 16;
    database_config_set_wal_sync_mode(config, WAL_SYNC_ASYNC);
    config->lru_memory_mb = 256;
    mkdir(db_path, 0755);

    int error = 0;
    database_t* db = database_create_with_config(db_path, config, &error);
    database_config_destroy(config);

    printf("\n=== Async Raw API (persist + WAL_ASYNC, 16 workers) ===\n");

    // Benchmark Put
    double start = now_us();
    for (int i = 0; i < n_ops; i++) {
        char key[64], val[64];
        snprintf(key, sizeof(key), "key%08d", i);
        snprintf(val, sizeof(val), "value%08d", i);

        async_result_t result = {0};
        promise_t* p = promise_create(on_complete, on_error, &result);
        int rc = database_put_raw(db, key, strlen(key), '\0', (uint8_t*)val, strlen(val), p);
        if (rc != 0) { promise_destroy(p); continue; }
        while (!result.completed) { /* busy-wait for microbenchmark */ }
        promise_destroy(p);
    }
    double put_elapsed = now_us() - start;
    printf("Put: %d ops in %.0f us = %.0f ops/sec (%.1f us/op)\n",
           n_ops, put_elapsed, n_ops / (put_elapsed / 1e6), put_elapsed / n_ops);

    // Benchmark Get
    start = now_us();
    for (int i = 0; i < n_ops; i++) {
        char key[64];
        snprintf(key, sizeof(key), "key%08d", i);

        async_result_t result = {0};
        promise_t* p = promise_create(on_get_complete, on_error, &result);
        int rc = database_get_raw(db, key, strlen(key), '\0', p);
        if (rc != 0) { promise_destroy(p); continue; }
        while (!result.completed) { /* busy-wait */ }
        promise_destroy(p);
        if (result.val) free(result.val);
    }
    double get_elapsed = now_us() - start;
    printf("Get: %d ops in %.0f us = %.0f ops/sec (%.1f us/op)\n",
           n_ops, get_elapsed, n_ops / (get_elapsed / 1e6), get_elapsed / n_ops);

    database_destroy(db);
}

int main(void) {
    const int N = 10000;

    bench_sync_raw("/tmp/microbench_sync", N);
    bench_sync_raw_persist("/tmp/microbench_sync_p", N);
    bench_async_raw("/tmp/microbench_async", N);

    printf("\n=== Summary ===\n");
    printf("These numbers show the per-API overhead difference.\n");
    printf("The KVBench adapter uses the Async Raw API.\n");
    return 0;
}
