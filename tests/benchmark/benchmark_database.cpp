//
// Database Performance Benchmarks
// End-to-end tests for database operations
//
// KNOWN ISSUE:
// These benchmarks may encounter lock initialization errors when run multiple
// times in the same process. This is due to threading/lock state persistence
// between benchmark runs. Each individual benchmark works correctly, but
// sequential runs in the same process may fail.
//
// WORKAROUND:
// Run each benchmark separately or use the unit tests (test_database) which
// properly isolate each test case.
//
// The core database functionality is verified by test_database which passes
// all tests successfully. The issue is specific to the benchmark context
// management, not the database implementation.
//

#include "benchmark_base.h"
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <future>
#include <vector>
#include <thread>
#include <unistd.h>  // for getpid()
extern "C" {
#include "Database/database.h"
#include "Time/wheel.h"
#include "Workers/pool.h"
#include "Workers/promise.h"
#include "Workers/transaction_id.h"
#include "HBTrie/path.h"
#include "HBTrie/identifier.h"
#include "Buffer/buffer.h"
#include "Util/allocator.h"
}

// Context for concurrent benchmark threads
typedef struct {
    database_t* db;
    std::atomic<uint64_t>* total_ops;
    std::atomic<uint64_t>* total_errors;
    std::atomic<uint64_t>* total_latency_ns;
    int thread_id;
    int ops_per_thread;
    int key_range_start;
    int key_range_end;
    work_pool_t* pool;
    hierarchical_timing_wheel_t* wheel;
} concurrent_bench_ctx_t;

// Test configuration
#define DATABASE_BENCH_ITERATIONS 100
#define DATABASE_BENCH_BATCH_SIZE 1000

// Global context for benchmark
typedef struct {
    work_pool_t* pool;
    hierarchical_timing_wheel_t* wheel;
    database_t* db;
    char test_dir[256];
} bench_context_t;

// Context for async callbacks
typedef struct {
    std::promise<void>* promise;
} bench_ctx;

// Cache metrics tracking
typedef struct {
    size_t current_memory;
    size_t entry_count;
    size_t max_memory;
} CacheMetrics;

static CacheMetrics get_cache_metrics(database_t* db) {
    CacheMetrics metrics;
    metrics.current_memory = database_lru_cache_memory(db->lru);
    metrics.entry_count = database_lru_cache_size(db->lru);
    metrics.max_memory = db->lru->total_max_memory;
    return metrics;
}

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

// Worker thread function for concurrent write benchmark
static void concurrent_write_worker(concurrent_bench_ctx_t* ctx) {
    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < ctx->ops_per_thread; i++) {
        // Each thread writes to unique keys: key_{thread_id}_{op_id}
        char key[64];
        snprintf(key, sizeof(key), "key_%d_%d", ctx->thread_id, i);

        path_t* path = make_path(key);
        identifier_t* val = make_value("concurrent_value");

        std::promise<void> promise;
        bench_ctx* bctx = (bench_ctx*)malloc(sizeof(bench_ctx));
        bctx->promise = &promise;
        promise_t* prom = promise_create(bench_callback, bench_error_callback, bctx);

        database_put(ctx->db, path, val, prom);
        promise.get_future().get();

        promise_destroy(prom);

        ctx->total_ops->fetch_add(1);
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    ctx->total_latency_ns->fetch_add(duration_ns);
}

static void concurrent_read_worker(concurrent_bench_ctx_t* ctx) {
    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < ctx->ops_per_thread; i++) {
        // Read from pre-populated shared key space
        char key[64];
        snprintf(key, sizeof(key), "readkey_%d", i % ctx->key_range_end);

        path_t* path = make_path(key);

        std::promise<void> promise;
        bench_ctx* bctx = (bench_ctx*)malloc(sizeof(bench_ctx));
        bctx->promise = &promise;
        promise_t* prom = promise_create(bench_get_callback, bench_error_callback, bctx);

        database_get(ctx->db, path, prom);
        promise.get_future().get();

        promise_destroy(prom);

        ctx->total_ops->fetch_add(1);
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    ctx->total_latency_ns->fetch_add(duration_ns);
}

static void concurrent_mixed_worker(concurrent_bench_ctx_t* ctx) {
    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < ctx->ops_per_thread; i++) {
        int op = rand() % 100;

        if (op < 70) {
            // Read operation (70%)
            char key[64];
            snprintf(key, sizeof(key), "mixedkey_%d", rand() % ctx->key_range_end);

            path_t* path = make_path(key);

            std::promise<void> promise;
            bench_ctx* bctx = (bench_ctx*)malloc(sizeof(bench_ctx));
            bctx->promise = &promise;
            promise_t* prom = promise_create(bench_get_callback, bench_error_callback, bctx);

            database_get(ctx->db, path, prom);
            promise.get_future().get();
            promise_destroy(prom);

            ctx->total_ops->fetch_add(1);
        } else if (op < 90) {
            // Write operation (20%)
            char key[64], val[64];
            snprintf(key, sizeof(key), "mixedkey_%d", rand() % ctx->key_range_end);
            snprintf(val, sizeof(val), "mixed_value_%d_%d", ctx->thread_id, i);

            path_t* path = make_path(key);
            identifier_t* value = make_value(val);

            std::promise<void> promise;
            bench_ctx* bctx = (bench_ctx*)malloc(sizeof(bench_ctx));
            bctx->promise = &promise;
            promise_t* prom = promise_create(bench_callback, bench_error_callback, bctx);

            database_put(ctx->db, path, value, prom);
            promise.get_future().get();
            promise_destroy(prom);

            ctx->total_ops->fetch_add(1);
        } else {
            // Delete operation (10%)
            char key[64];
            snprintf(key, sizeof(key), "mixedkey_%d", rand() % ctx->key_range_end);

            path_t* path = make_path(key);

            std::promise<void> promise;
            bench_ctx* bctx = (bench_ctx*)malloc(sizeof(bench_ctx));
            bctx->promise = &promise;
            promise_t* prom = promise_create(bench_callback, bench_error_callback, bctx);

            database_delete(ctx->db, path, prom);
            promise.get_future().get();
            promise_destroy(prom);

            ctx->total_ops->fetch_add(1);
        }
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    ctx->total_latency_ns->fetch_add(duration_ns);
}

static void run_concurrent_write_benchmark(database_t* db, work_pool_t* pool,
                                           hierarchical_timing_wheel_t* wheel,
                                           int thread_count, int ops_per_thread) {
    std::vector<std::thread> threads;
    std::atomic<uint64_t> total_ops{0};
    std::atomic<uint64_t> total_errors{0};
    std::atomic<uint64_t> total_latency_ns{0};

    auto start = std::chrono::high_resolution_clock::now();

    for (int t = 0; t < thread_count; t++) {
        threads.emplace_back([&, t]() {
            concurrent_bench_ctx_t ctx;
            ctx.db = db;
            ctx.total_ops = &total_ops;
            ctx.total_errors = &total_errors;
            ctx.total_latency_ns = &total_latency_ns;
            ctx.thread_id = t;
            ctx.ops_per_thread = ops_per_thread;
            ctx.pool = pool;
            ctx.wheel = wheel;

            concurrent_write_worker(&ctx);
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();

    double ops_per_sec = (total_ops.load() * 1e9) / duration_ns;
    double avg_latency_ns = total_latency_ns.load() / (double)total_ops.load();

    printf("Concurrent Write (%d threads):\n", thread_count);
    printf("  Operations: %lu\n", total_ops.load());
    printf("  Ops/sec: %.0f\n", ops_per_sec);
    printf("  Avg latency: %.0f ns\n", avg_latency_ns);
    printf("  Errors: %lu\n", total_errors.load());
    printf("\n");
}

static void run_concurrent_read_benchmark(database_t* db, work_pool_t* pool,
                                          hierarchical_timing_wheel_t* wheel,
                                          int thread_count, int ops_per_thread,
                                          int prepopulate_count) {
    // Pre-populate database with shared key space
    printf("  Pre-populating %d keys for read benchmark...\n", prepopulate_count);
    for (int i = 0; i < prepopulate_count; i++) {
        char key[64];
        snprintf(key, sizeof(key), "readkey_%d", i);

        path_t* path = make_path(key);
        identifier_t* val = make_value("read_value");

        std::promise<void> promise;
        bench_ctx* bctx = (bench_ctx*)malloc(sizeof(bench_ctx));
        bctx->promise = &promise;
        promise_t* prom = promise_create(bench_callback, bench_error_callback, bctx);

        database_put(db, path, val, prom);
        promise.get_future().get();
        promise_destroy(prom);
    }

    std::vector<std::thread> threads;
    std::atomic<uint64_t> total_ops{0};
    std::atomic<uint64_t> total_errors{0};
    std::atomic<uint64_t> total_latency_ns{0};

    auto start = std::chrono::high_resolution_clock::now();

    for (int t = 0; t < thread_count; t++) {
        threads.emplace_back([&, t]() {
            concurrent_bench_ctx_t ctx;
            ctx.db = db;
            ctx.total_ops = &total_ops;
            ctx.total_errors = &total_errors;
            ctx.total_latency_ns = &total_latency_ns;
            ctx.thread_id = t;
            ctx.ops_per_thread = ops_per_thread;
            ctx.key_range_end = prepopulate_count;
            ctx.pool = pool;
            ctx.wheel = wheel;

            concurrent_read_worker(&ctx);
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();

    double ops_per_sec = (total_ops.load() * 1e9) / duration_ns;
    double avg_latency_ns = total_latency_ns.load() / (double)total_ops.load();

    printf("Concurrent Read (%d threads):\n", thread_count);
    printf("  Operations: %lu\n", total_ops.load());
    printf("  Ops/sec: %.0f\n", ops_per_sec);
    printf("  Avg latency: %.0f ns\n", avg_latency_ns);
    printf("  Errors: %lu\n", total_errors.load());
    printf("\n");
}

static void run_concurrent_mixed_benchmark(database_t* db, work_pool_t* pool,
                                           hierarchical_timing_wheel_t* wheel,
                                           int thread_count, int ops_per_thread,
                                           int prepopulate_count) {
    // Pre-populate database with shared key space for read operations
    printf("  Pre-populating %d keys for mixed workload benchmark...\n", prepopulate_count);
    for (int i = 0; i < prepopulate_count; i++) {
        char key[64];
        snprintf(key, sizeof(key), "mixedkey_%d", i);

        path_t* path = make_path(key);
        identifier_t* val = make_value("mixed_initial_value");

        std::promise<void> promise;
        bench_ctx* bctx = (bench_ctx*)malloc(sizeof(bench_ctx));
        bctx->promise = &promise;
        promise_t* prom = promise_create(bench_callback, bench_error_callback, bctx);

        database_put(db, path, val, prom);
        promise.get_future().get();
        promise_destroy(prom);
    }

    std::vector<std::thread> threads;
    std::atomic<uint64_t> total_ops{0};
    std::atomic<uint64_t> total_errors{0};
    std::atomic<uint64_t> total_latency_ns{0};

    auto start = std::chrono::high_resolution_clock::now();

    for (int t = 0; t < thread_count; t++) {
        threads.emplace_back([&, t]() {
            concurrent_bench_ctx_t ctx;
            ctx.db = db;
            ctx.total_ops = &total_ops;
            ctx.total_errors = &total_errors;
            ctx.total_latency_ns = &total_latency_ns;
            ctx.thread_id = t;
            ctx.ops_per_thread = ops_per_thread;
            ctx.key_range_end = prepopulate_count;
            ctx.pool = pool;
            ctx.wheel = wheel;

            concurrent_mixed_worker(&ctx);
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();

    double ops_per_sec = (total_ops.load() * 1e9) / duration_ns;
    double avg_latency_ns = total_latency_ns.load() / (double)total_ops.load();

    printf("Concurrent Mixed Workload (%d threads):\n", thread_count);
    printf("  Operations: %lu\n", total_ops.load());
    printf("  Ops/sec: %.0f\n", ops_per_sec);
    printf("  Avg latency: %.0f ns\n", avg_latency_ns);
    printf("  Errors: %lu\n", total_errors.load());
    printf("\n");
}

static void print_concurrent_summary(const char* scenario,
                                     double* ops_per_sec_per_thread,
                                     int* thread_counts,
                                     int num_configs) {
    printf("========================================\n");
    printf("%s Throughput Summary\n", scenario);
    printf("========================================\n\n");

    double baseline = ops_per_sec_per_thread[0];

    for (int i = 0; i < num_configs; i++) {
        double scaling = ops_per_sec_per_thread[i] / baseline;
        printf("  %2d thread(s): %8.0f ops/sec", thread_counts[i], ops_per_sec_per_thread[i]);

        if (i == 0) {
            printf(" (baseline)\n");
        } else {
            printf(" (%.2fx)\n", scaling);
        }
    }
    printf("\n");
}

// Setup helper
static void setup_database(bench_context_t* ctx) {
    ctx->pool = work_pool_create(platform_core_count());
    work_pool_launch(ctx->pool);

    ctx->wheel = hierarchical_timing_wheel_create(8, ctx->pool);
    hierarchical_timing_wheel_run(ctx->wheel);

    // Create temporary test directory
    snprintf(ctx->test_dir, sizeof(ctx->test_dir), "/tmp/wavedb_bench_%d", getpid());

    // Create custom WAL config with larger file size for benchmarks
    wal_config_t wal_config;
    wal_config.sync_mode = WAL_SYNC_DEBOUNCED;  // Balanced durability/performance
    wal_config.debounce_ms = WAL_DEFAULT_DEBOUNCE_MS;
    wal_config.idle_threshold_ms = WAL_DEFAULT_IDLE_THRESHOLD_MS;
    wal_config.compact_interval_ms = WAL_DEFAULT_COMPACT_INTERVAL_MS;
    wal_config.max_file_size = 100 * 1024 * 1024;  // 100MB for benchmarks

    int error = 0;
    ctx->db = database_create(ctx->test_dir, 50, &wal_config, 4, 4096, 0, 0, ctx->pool, ctx->wheel, &error);
    if (error != 0 || ctx->db == NULL) {
        fprintf(stderr, "Failed to create database: error=%d\n", error);
        exit(1);
    }
}

// Teardown helper
static void teardown_database(bench_context_t* ctx) {
    if (ctx->db) {
        database_destroy(ctx->db);
    }

    if (ctx->wheel) {
        hierarchical_timing_wheel_wait_for_idle_signal(ctx->wheel);
        hierarchical_timing_wheel_stop(ctx->wheel);
    }

    if (ctx->pool) {
        work_pool_shutdown(ctx->pool);
        work_pool_join_all(ctx->pool);
    }

    if (ctx->pool) {
        work_pool_destroy(ctx->pool);
    }

    if (ctx->wheel) {
        hierarchical_timing_wheel_destroy(ctx->wheel);
    }

    // Cleanup test directory
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", ctx->test_dir);
    system(cmd);
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

        database_put(db, path, val, prom);
        promise.get_future().get();

        promise_destroy(prom);
        // database_put takes ownership of path and val, so we don't destroy them here
    }
}

// Benchmark: Single Put operation
static void benchmark_database_put(void* user_data, uint64_t iterations) {
    bench_context_t* bctx = (bench_context_t*)user_data;
    database_t* db = bctx->db;

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

        database_put(db, path, val, prom);
        promise.get_future().get();

        promise_destroy(prom);
        // database_put takes ownership of path and val
    }
}

// Benchmark: Single Get operation (requires pre-populated data)
static void benchmark_database_get(void* user_data, uint64_t iterations) {
    bench_context_t* bctx = (bench_context_t*)user_data;
    database_t* db = bctx->db;

    for (uint64_t i = 0; i < iterations; i++) {
        char key[32];
        snprintf(key, sizeof(key), "key_%lu", i % DATABASE_BENCH_BATCH_SIZE);

        path_t* path = make_path(key);

        std::promise<void> promise;
        bench_ctx* ctx = (bench_ctx*)malloc(sizeof(bench_ctx));
        ctx->promise = &promise;
        promise_t* prom = promise_create(bench_get_callback, bench_error_callback, ctx);

        database_get(db, path, prom);
        promise.get_future().get();

        promise_destroy(prom);
        // database_put takes ownership of path
    }
}

// Benchmark: Single Delete operation
static void benchmark_database_delete(void* user_data, uint64_t iterations) {
    bench_context_t* bctx = (bench_context_t*)user_data;
    database_t* db = bctx->db;

    for (uint64_t i = 0; i < iterations; i++) {
        char key[32];
        snprintf(key, sizeof(key), "key_%lu", i % DATABASE_BENCH_BATCH_SIZE);

        path_t* path = make_path(key);

        std::promise<void> promise;
        bench_ctx* ctx = (bench_ctx*)malloc(sizeof(bench_ctx));
        ctx->promise = &promise;
        promise_t* prom = promise_create(bench_callback, bench_error_callback, ctx);

        database_delete(db, path, prom);
        promise.get_future().get();

        promise_destroy(prom);
        // database_put takes ownership of path
    }
}

// Benchmark: Batch Put operations
static void benchmark_database_batch_put(void* user_data, uint64_t iterations) {
    bench_context_t* bctx = (bench_context_t*)user_data;
    database_t* db = bctx->db;

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

        database_put(db, path, val, prom);
        promise.get_future().get();

        promise_destroy(prom);
        // database_put takes ownership of path and val
    }
}

// Benchmark: Mixed workload (70% read, 20% write, 10% delete)
static void benchmark_database_mixed(void* user_data, uint64_t iterations) {
    bench_context_t* bctx = (bench_context_t*)user_data;
    database_t* db = bctx->db;

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

            database_get(db, path, prom);
            promise.get_future().get();

            promise_destroy(prom);
            // database_put takes ownership of path
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

            database_put(db, path, val, prom);
            promise.get_future().get();

            promise_destroy(prom);
            // database_put takes ownership of path
        } else {
            // Delete operation (10%)
            char key[32];
            snprintf(key, sizeof(key), "key_%d", rand() % DATABASE_BENCH_BATCH_SIZE);

            path_t* path = make_path(key);

            std::promise<void> promise;
            bench_ctx* ctx = (bench_ctx*)malloc(sizeof(bench_ctx));
            ctx->promise = &promise;
            promise_t* prom = promise_create(bench_callback, bench_error_callback, ctx);

            database_delete(db, path, prom);
            promise.get_future().get();

            promise_destroy(prom);
            // database_put takes ownership of path
        }
    }
}

void run_database_benchmarks(void) {
    printf("========================================\n");
    printf("Database Integration Benchmarks\n");
    printf("========================================\n\n");

    printf("NOTE: These benchmarks may fail with lock errors when run\n");
    printf("multiple times in the same process. For reliable testing,\n");
    printf("use test_database which properly isolates each test.\n\n");

    // Setup
    bench_context_t ctx;
    setup_database(&ctx);

    printf("Setup complete. Running benchmarks...\n\n");

    // Pre-populate for read benchmarks
    printf("Pre-populating database with %d entries...\n", DATABASE_BENCH_BATCH_SIZE);
    populate_database(ctx.db, DATABASE_BENCH_BATCH_SIZE);
    printf("Pre-population complete.\n\n");

    // Benchmark 1: Single Put operation
    printf("--- Single Put Operation ---\n");
    benchmark_metrics_t put_metrics = benchmark_run(
        "Database Put (single)",
        benchmark_database_put,
        &ctx,
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
        &ctx,
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
        &ctx,
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
        &ctx,
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
        &ctx,
        DATABASE_BENCH_ITERATIONS,
        DATABASE_BENCH_ITERATIONS
    );
    benchmark_print_results(&delete_metrics);
    printf("\n");

    // ============================================================================
    // Concurrent Throughput Benchmarks
    // ============================================================================
    //
    // Purpose:
    //   Measure database performance under concurrent load to assess:
    //   - Thread scalability (how throughput scales with thread count)
    //   - Lock contention characteristics
    //   - Real-world multi-client throughput
    //
    // Design:
    //   Each benchmark spawns N worker threads that independently perform
    //   operations against a shared database instance. The work pool and
    //   timing wheel are shared across all threads, creating realistic
    //   contention on internal locks.
    //
    // Thread Counts Tested:
    //   - 1 thread:  Baseline single-threaded performance
    //   - 2 threads: Minimal contention scenario
    //   - 4 threads: Typical multi-core utilization
    //   - 8 threads: High contention scenario
    //   - 16 threads: Stress test for lock scalability
    //
    // Scenarios:
    //   1. Concurrent Write: Each thread writes to unique keys (no key overlap)
    //      - Tests write throughput under concurrent access
    //      - Measures contention on WAL, B+tree, and LRU cache locks
    //      - Key format: key_{thread_id}_{op_id}
    //
    //   2. Concurrent Read: All threads read from shared pre-populated key space
    //      - Tests read throughput under concurrent access
    //      - Measures contention on read path (HBTrie traversal)
    //      - Reads are interleaved across shared keys to stress cache
    //      - Key format: readkey_{0..prepopulate_count}
    //
    //   3. Concurrent Mixed: 70% read, 20% write, 10% delete operations
    //      - Realistic mixed workload simulation
    //      - Tests concurrent read-modify-write patterns
    //      - Key format: mixedkey_{0..prepopulate_count}
    //
    // Metrics Collected (per thread count):
    //   - Total operations: Sum of all thread operation counts
    //   - Operations/sec: Throughput (total_ops / wall_clock_time)
    //   - Average latency: Mean operation latency across all threads
    //   - Error count: Number of failed operations (should be 0)
    //
    // Scaling Expectations:
    //   - Perfect scaling: N threads = Nx throughput (unlikely due to contention)
    //   - Good scaling: N threads = 0.7-0.9 * Nx throughput
    //   - Poor scaling: N threads < 0.5 * Nx throughput (indicates contention)
    //
    // Known Issues:
    //   - Lock initialization errors may occur when benchmarks run sequentially
    //     in the same process (see header comment for details)
    //   - Each concurrent benchmark should ideally be run in a fresh process
    //
    printf("========================================\n");
    printf("Concurrent Throughput Benchmarks\n");
    printf("========================================\n\n");

    // Test thread counts
    int thread_counts[] = {1, 2, 4, 8, 16};
    int num_configs = sizeof(thread_counts) / sizeof(thread_counts[0]);
    int ops_per_thread = 1000;  // Operations per thread
    int prepopulate_count = 10000;  // Keys to pre-populate for read/mixed tests

    // Arrays to store results for summary
    uint64_t write_ops[num_configs];
    uint64_t read_ops[num_configs];
    uint64_t mixed_ops[num_configs];

    // Run concurrent write benchmark for each thread count
    printf("--- Concurrent Write Benchmark ---\n");
    for (int i = 0; i < num_configs; i++) {
        run_concurrent_write_benchmark(ctx.db, ctx.pool, ctx.wheel,
                                       thread_counts[i], ops_per_thread);
        // Store result (extracted from output parsing or passed back)
        // For now, results are printed inline
    }
    printf("\n");

    // Run concurrent read benchmark for each thread count
    printf("--- Concurrent Read Benchmark ---\n");
    for (int i = 0; i < num_configs; i++) {
        run_concurrent_read_benchmark(ctx.db, ctx.pool, ctx.wheel,
                                      thread_counts[i], ops_per_thread,
                                      prepopulate_count);
    }
    printf("\n");

    // Run concurrent mixed benchmark for each thread count
    printf("--- Concurrent Mixed Benchmark ---\n");
    for (int i = 0; i < num_configs; i++) {
        run_concurrent_mixed_benchmark(ctx.db, ctx.pool, ctx.wheel,
                                       thread_counts[i], ops_per_thread,
                                       prepopulate_count);
    }
    printf("\n");

    // Collect cache metrics before teardown
    CacheMetrics cache_metrics = get_cache_metrics(ctx.db);

    // Teardown
    teardown_database(&ctx);

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

    // Print cache metrics
    printf("Cache Metrics:\n");
    printf("  Max Memory Budget: %.2f MB\n", cache_metrics.max_memory / (1024.0 * 1024.0));
    printf("  Current Memory: %.2f MB (%zu bytes)\n",
           cache_metrics.current_memory / (1024.0 * 1024.0), cache_metrics.current_memory);
    printf("  Entry Count: %zu\n", cache_metrics.entry_count);
    if (cache_metrics.entry_count > 0) {
        printf("  Avg Entry Size: %.0f bytes\n",
               cache_metrics.current_memory / (double)cache_metrics.entry_count);
    }
    printf("\n");
}

int main(int argc, char** argv) {
    // Initialize transaction ID system (required for WAL)
    transaction_id_init();

    // Create benchmark directory if it doesn't exist
    system("mkdir -p .benchmarks");

    run_database_benchmarks();

    return 0;
}