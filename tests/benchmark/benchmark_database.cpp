//
// Database Performance Benchmarks
// End-to-end tests for database operations
//

#include "benchmark_base.h"
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <future>
#include <vector>
#include <thread>
extern "C" {
#include "Database/database.h"
#include "Time/wheel.h"
#include "Workers/pool.h"
#include "Workers/promise.h"
#include "Workers/priority.h"
#include "HBTrie/path.h"
#include "HBTrie/identifier.h"
#include "Buffer/buffer.h"
#include "Util/allocator.h"
}

// Test configuration
#define DATABASE_BENCH_ITERATIONS 100
#define DATABASE_BENCH_BATCH_SIZE 1000

// Context for async callbacks
typedef struct {
    std::promise<void>* promise;
} bench_ctx;

// Callback wrappers
extern "C" void bench_callback(void* ctx, void* payload) {
    auto bctx = static_cast<bench_ctx*>(ctx);
    bctx->promise->set_value();
    free(ctx);
}

extern "C" void bench_error_callback(void* ctx, async_error_t* payload) {
    auto bctx = static_cast<bench_ctx*>(ctx);
    bctx->promise->set_exception(std::make_exception_ptr(std::runtime_error((const char*)payload->message)));
    error_destroy(payload);
    free(ctx);
}

extern "C" void bench_get_callback(void* ctx, void* payload) {
    auto bctx = static_cast<bench_ctx*>(ctx);
    // Don't need the value for benchmark
    if (payload) {
        identifier_t* id = (identifier_t*)payload;
        identifier_destroy(id);
    }
    bctx->promise->set_value();
    free(ctx);
}

// Helper to create a path
static path_t* make_path(const char* key) {
    path_t* path = path_create();
    buffer_t* buf = buffer_create_from_pointer_copy((uint8_t*)key, strlen(key));
    identifier_t* id = identifier_create(buf, 0);
    buffer_destroy(buf);
    path_append(path, id);
    identifier_destroy(id);
    return path;
}

// Helper to create a value
static identifier_t* make_value(const char* data) {
    buffer_t* buf = buffer_create_from_pointer_copy((uint8_t*)data, strlen(data));
    identifier_t* id = identifier_create(buf, 0);
    buffer_destroy(buf);
    return id;
}

// Setup helper
static void setup_database(work_pool_t** pool, hierarchical_timing_wheel_t** wheel,
                          database_t** db, const char* test_dir) {
    *pool = work_pool_create(platform_core_count());
    work_pool_launch(*pool);

    *wheel = hierarchical_timing_wheel_create(8, *pool);
    hierarchical_timing_wheel_run(*wheel);

    int error = 0;
    *db = database_create(test_dir, 1000, 128 * 1024, 4, 4096, 0, 0, *pool, *wheel, &error);
    if (error != 0) {
        fprintf(stderr, "Failed to create database: %d\n", error);
        exit(1);
    }
}

// Teardown helper
static void teardown_database(work_pool_t* pool, hierarchical_timing_wheel_t* wheel, database_t* db) {
    if (db) {
        database_destroy(db);
    }

    if (wheel) {
        hierarchical_timing_wheel_wait_for_idle_signal(wheel);
        hierarchical_timing_wheel_stop(wheel);
    }

    if (pool) {
        work_pool_shutdown(pool);
        work_pool_join_all(pool);
        work_pool_destroy(pool);
    }

    if (wheel) {
        hierarchical_timing_wheel_destroy(wheel);
    }
}

// Benchmark: Single Put operation
static void benchmark_database_put(void* user_data, uint64_t iterations) {
    database_t* db = (database_t*)user_data;

    for (uint64_t i = 0; i < iterations; i++) {
        char key[32], value[32];
        snprintf(key, sizeof(key), "key_%lu", i);
        snprintf(value, sizeof(value), "value_%lu", i);

        path_t* path = make_path(key);
        identifier_t* val = make_value(value);

        std::promise<void> promise;
        bench_ctx* ctx = (bench_ctx*)malloc(sizeof(bench_ctx));
        ctx->promise = &promise;
        promise_t* prom = promise_create(bench_callback, bench_error_callback, ctx);

        priority_t priority = {0, 0};  // Default priority for benchmarks
        database_put(db, priority, path, val, prom);
        promise.get_future().get();

        promise_destroy(prom);
        path_destroy(path);
        identifier_destroy(val);
    }
}

// Benchmark: Single Get operation (requires pre-populated data)
static void benchmark_database_get(void* user_data, uint64_t iterations) {
    database_t* db = (database_t*)user_data;

    for (uint64_t i = 0; i < iterations; i++) {
        char key[32];
        snprintf(key, sizeof(key), "key_%lu", i % DATABASE_BENCH_BATCH_SIZE);

        path_t* path = make_path(key);

        std::promise<void> promise;
        bench_ctx* ctx = (bench_ctx*)malloc(sizeof(bench_ctx));
        ctx->promise = &promise;
        promise_t* prom = promise_create(bench_get_callback, bench_error_callback, ctx);

        priority_t priority = {1, 1}; // Static priority for benchmarks
        database_get(db, priority, path, prom);
        promise.get_future().get();

        promise_destroy(prom);
        path_destroy(path);
    }
}

// Benchmark: Single Delete operation
static void benchmark_database_delete(void* user_data, uint64_t iterations) {
    database_t* db = (database_t*)user_data;

    for (uint64_t i = 0; i < iterations; i++) {
        char key[32];
        snprintf(key, sizeof(key), "key_%lu", i % DATABASE_BENCH_BATCH_SIZE);

        path_t* path = make_path(key);

        std::promise<void> promise;
        bench_ctx* ctx = (bench_ctx*)malloc(sizeof(bench_ctx));
        ctx->promise = &promise;
        promise_t* prom = promise_create(bench_callback, bench_error_callback, ctx);

        priority_t priority = {1, 1}; // Static priority for benchmarks
        database_delete(db, priority, path, prom);
        promise.get_future().get();

        promise_destroy(prom);
        path_destroy(path);
    }
}

// Benchmark: Batch Put operations
static void benchmark_database_batch_put(void* user_data, uint64_t iterations) {
    database_t* db = (database_t*)user_data;

    for (uint64_t i = 0; i < iterations; i++) {
        char key[32], value[32];
        snprintf(key, sizeof(key), "batch_key_%lu", i);
        snprintf(value, sizeof(value), "batch_value_%lu", i);

        path_t* path = make_path(key);
        identifier_t* val = make_value(value);

        std::promise<void> promise;
        bench_ctx* ctx = (bench_ctx*)malloc(sizeof(bench_ctx));
        ctx->promise = &promise;
        promise_t* prom = promise_create(bench_callback, bench_error_callback, ctx);

        priority_t priority = {0, 0};  // Default priority for benchmarks
        database_put(db, priority, path, val, prom);
        promise.get_future().get();

        promise_destroy(prom);
        path_destroy(path);
        identifier_destroy(val);
    }
}

// Benchmark: Mixed workload (70% read, 20% write, 10% delete)
static void benchmark_database_mixed(void* user_data, uint64_t iterations) {
    database_t* db = (database_t*)user_data;

    for (uint64_t i = 0; i < iterations; i++) {
        int op = rand() % 100;

        if (op < 70) {
            // Read operation (70%)
            char key[32];
            snprintf(key, sizeof(key), "key_%d", rand() % DATABASE_BENCH_BATCH_SIZE);

            path_t* path = make_path(key);

            std::promise<void> promise;
            bench_ctx* ctx = (bench_ctx*)malloc(sizeof(bench_ctx));
            ctx->promise = &promise;
            promise_t* prom = promise_create(bench_get_callback, bench_error_callback, ctx);

            priority_t priority = {1, 1}; // Static priority for benchmarks
            database_get(db, priority, path, prom);
            promise.get_future().get();

            promise_destroy(prom);
            path_destroy(path);
        } else if (op < 90) {
            // Write operation (20%)
            char key[32], value[32];
            snprintf(key, sizeof(key), "key_%d", rand() % DATABASE_BENCH_BATCH_SIZE);
            snprintf(value, sizeof(value), "value_%lu", i);

            path_t* path = make_path(key);
            identifier_t* val = make_value(value);

            std::promise<void> promise;
            bench_ctx* ctx = (bench_ctx*)malloc(sizeof(bench_ctx));
            ctx->promise = &promise;
            promise_t* prom = promise_create(bench_callback, bench_error_callback, ctx);

            priority_t priority = {1, 1}; // Static priority for benchmarks
            database_put(db, priority, path, val, prom);
            promise.get_future().get();

            promise_destroy(prom);
            path_destroy(path);
            identifier_destroy(val);
        } else {
            // Delete operation (10%)
            char key[32];
            snprintf(key, sizeof(key), "key_%d", rand() % DATABASE_BENCH_BATCH_SIZE);

            path_t* path = make_path(key);

            std::promise<void> promise;
            bench_ctx* ctx = (bench_ctx*)malloc(sizeof(bench_ctx));
            ctx->promise = &promise;
            promise_t* prom = promise_create(bench_callback, bench_error_callback, ctx);

            priority_t priority = {1, 1}; // Static priority for benchmarks
            database_delete(db, priority, path, prom);
            promise.get_future().get();

            promise_destroy(prom);
            path_destroy(path);
        }
    }
}

// Pre-populate database with test data
static void populate_database(database_t* db, size_t count) {
    for (size_t i = 0; i < count; i++) {
        char key[32], value[32];
        snprintf(key, sizeof(key), "key_%lu", i);
        snprintf(value, sizeof(value), "value_%lu", i);

        path_t* path = make_path(key);
        identifier_t* val = make_value(value);

        std::promise<void> promise;
        bench_ctx* ctx = (bench_ctx*)malloc(sizeof(bench_ctx));
        ctx->promise = &promise;
        promise_t* prom = promise_create(bench_callback, bench_error_callback, ctx);

        priority_t priority = {0, 0};  // Default priority for benchmarks
        database_put(db, priority, path, val, prom);
        promise.get_future().get();

        promise_destroy(prom);
        path_destroy(path);
        identifier_destroy(val);
    }
}

void run_database_benchmarks(void) {
    printf("========================================\n");
    printf("Database Integration Benchmarks\n");
    printf("========================================\n\n");

    // Create temporary test directory
    char test_dir[] = "/tmp/wavedb_bench_XXXXXX";
    mkdtemp(test_dir);

    // Setup
    work_pool_t* pool = nullptr;
    hierarchical_timing_wheel_t* wheel = nullptr;
    database_t* db = nullptr;
    setup_database(&pool, &wheel, &db, test_dir);

    printf("Setup complete. Running benchmarks...\n\n");

    // Pre-populate for read benchmarks
    printf("Pre-populating database with %d entries...\n", DATABASE_BENCH_BATCH_SIZE);
    populate_database(db, DATABASE_BENCH_BATCH_SIZE);
    printf("Pre-population complete.\n\n");

    // Benchmark 1: Single Put operation
    printf("--- Single Put Operation ---\n");
    benchmark_metrics_t put_metrics = benchmark_run(
        "Database Put (single)",
        benchmark_database_put,
        db,
        DATABASE_BENCH_ITERATIONS,
        DATABASE_BENCH_ITERATIONS
    );
    benchmark_print_results(&put_metrics);
    printf("\n");

    // Benchmark 2: Single Get operation
    printf("--- Single Get Operation ---\n");
    benchmark_metrics_t get_metrics = benchmark_run(
        "Database Get (single)",
        benchmark_database_get,
        db,
        DATABASE_BENCH_ITERATIONS,
        DATABASE_BENCH_ITERATIONS
    );
    benchmark_print_results(&get_metrics);
    printf("\n");

    // Benchmark 3: Batch Put operations
    printf("--- Batch Put Operations ---\n");
    benchmark_metrics_t batch_metrics = benchmark_run(
        "Database Put (batch)",
        benchmark_database_batch_put,
        db,
        DATABASE_BENCH_ITERATIONS,
        DATABASE_BENCH_BATCH_SIZE
    );
    benchmark_print_results(&batch_metrics);
    printf("\n");

    // Benchmark 4: Mixed workload
    printf("--- Mixed Workload (70%% read, 20%% write, 10%% delete) ---\n");
    benchmark_metrics_t mixed_metrics = benchmark_run(
        "Database Mixed",
        benchmark_database_mixed,
        db,
        DATABASE_BENCH_ITERATIONS,
        DATABASE_BENCH_BATCH_SIZE
    );
    benchmark_print_results(&mixed_metrics);
    printf("\n");

    // Benchmark 5: Delete operations
    printf("--- Single Delete Operation ---\n");
    benchmark_metrics_t delete_metrics = benchmark_run(
        "Database Delete (single)",
        benchmark_database_delete,
        db,
        DATABASE_BENCH_ITERATIONS,
        DATABASE_BENCH_ITERATIONS
    );
    benchmark_print_results(&delete_metrics);
    printf("\n");

    // Teardown
    teardown_database(pool, wheel, db);

    // Cleanup test directory
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", test_dir);
    system(cmd);

    printf("========================================\n");
    printf("Database Benchmarks Complete\n");
    printf("========================================\n\n");

    // Print summary
    printf("Performance Summary:\n");
    printf("  Put:   %.0f ops/sec (avg: %.0f ns)\n",
           put_metrics.operations_per_second, put_metrics.avg_latency_ns);
    printf("  Get:   %.0f ops/sec (avg: %.0f ns)\n",
           get_metrics.operations_per_second, get_metrics.avg_latency_ns);
    printf("  Batch: %.0f ops/sec (avg: %.0f ns)\n",
           batch_metrics.operations_per_second, batch_metrics.avg_latency_ns);
    printf("  Mixed: %.0f ops/sec (avg: %.0f ns)\n",
           mixed_metrics.operations_per_second, mixed_metrics.avg_latency_ns);
    printf("  Delete: %.0f ops/sec (avg: %.0f ns)\n",
           delete_metrics.operations_per_second, delete_metrics.avg_latency_ns);
    printf("\n");
}

int main(int argc, char** argv) {
    // Create benchmark directory if it doesn't exist
    system("mkdir -p .benchmarks");

    run_database_benchmarks();

    return 0;
}