//
// Synchronous Database Benchmarks
// Performance comparison: sync vs async API
//

#include "benchmark_base.h"
#include "../../src/Database/database.h"
#include "../../src/Buffer/buffer.h"
#include "../../src/Workers/transaction_id.h"
#include "../../src/Util/allocator.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#ifdef __cplusplus
extern "C" {
#endif

// Test context for sync benchmarks (no work pool or timing wheel needed)
typedef struct {
    database_t* db;
    char test_dir[256];
    uint64_t counter;
} sync_benchmark_ctx_t;

// Initialize sync benchmark database
static void sync_benchmark_init(sync_benchmark_ctx_t* ctx, const char* test_name) {
    snprintf(ctx->test_dir, sizeof(ctx->test_dir), "/tmp/sync_db_bench_%s_%d", test_name, getpid());
    mkdir(ctx->test_dir, 0755);

    // Configure WAL with large file size for benchmarks
    wal_config_t wal_config;
    wal_config.sync_mode = WAL_SYNC_ASYNC;  // Use ASYNC for performance testing
    wal_config.debounce_ms = WAL_DEFAULT_DEBOUNCE_MS;
    wal_config.idle_threshold_ms = WAL_DEFAULT_IDLE_THRESHOLD_MS;
    wal_config.compact_interval_ms = WAL_DEFAULT_COMPACT_INTERVAL_MS;
    wal_config.max_file_size = 100 * 1024 * 1024;  // 100MB

    int error = 0;
    ctx->db = database_create(ctx->test_dir, 50, &wal_config, 0, 4096, 0, 0, NULL, NULL, &error);
    ctx->counter = 0;

    if (error != 0 || ctx->db == NULL) {
        fprintf(stderr, "Failed to create database: error_code=%d\n", error);
        exit(1);
    }
}

// Cleanup sync benchmark database
static void sync_benchmark_cleanup(sync_benchmark_ctx_t* ctx) {
    if (ctx->db) {
        database_destroy(ctx->db);
    }

    // Remove test directory
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", ctx->test_dir);
    system(cmd);
}

// Generate test key path
static path_t* generate_test_path(uint64_t counter) {
    char key[64];
    snprintf(key, sizeof(key), "key_%lu", counter);

    identifier_t* id = identifier_create(strlen(key));
    memcpy(id->data, key, strlen(key));

    path_t* path = path_create();
    path_append(path, id);

    identifier_destroy(id);
    return path;
}

// Generate test value
static identifier_t* generate_test_value(uint64_t counter) {
    char value[128];
    snprintf(value, sizeof(value), "value_%lu_test_data_for_database", counter);

    identifier_t* id = identifier_create(strlen(value));
    memcpy(id->data, value, strlen(value));

    return id;
}

// Benchmark: Put operation
static void benchmark_sync_put(void* user_data, uint64_t iterations) {
    sync_benchmark_ctx_t* ctx = (sync_benchmark_ctx_t*)user_data;

    for (uint64_t i = 0; i < iterations; i++) {
        path_t* path = generate_test_path(ctx->counter);
        identifier_t* value = generate_test_value(ctx->counter++);

        int result = database_put_sync(ctx->db, path, value);

        if (result != 0) {
            fprintf(stderr, "Put failed at iteration %lu\n", i);
        }
    }
}

// Benchmark: Get operation
static void benchmark_sync_get(void* user_data, uint64_t iterations) {
    sync_benchmark_ctx_t* ctx = (sync_benchmark_ctx_t*)user_data;

    for (uint64_t i = 0; i < iterations; i++) {
        path_t* path = generate_test_path(i % ctx->counter);  // Get existing keys
        identifier_t* result = NULL;

        int rc = database_get_sync(ctx->db, path, &result);

        if (rc == 0 && result != NULL) {
            identifier_destroy(result);
        }
    }
}

// Benchmark: Mixed put/get operations
static void benchmark_sync_mixed(void* user_data, uint64_t iterations) {
    sync_benchmark_ctx_t* ctx = (sync_benchmark_ctx_t*)user_data;

    for (uint64_t i = 0; i < iterations; i++) {
        if (i % 2 == 0) {
            // Put operation
            path_t* path = generate_test_path(ctx->counter);
            identifier_t* value = generate_test_value(ctx->counter++);

            int result = database_put_sync(ctx->db, path, value);

            if (result != 0) {
                fprintf(stderr, "Put failed at iteration %lu\n", i);
            }
        } else {
            // Get operation
            path_t* path = generate_test_path(i % ctx->counter);
            identifier_t* result = NULL;

            int rc = database_get_sync(ctx->db, path, &result);

            if (rc == 0 && result != NULL) {
                identifier_destroy(result);
            }
        }
    }
}

// Benchmark: Delete operation
static void benchmark_sync_delete(void* user_data, uint64_t iterations) {
    sync_benchmark_ctx_t* ctx = (sync_benchmark_ctx_t*)user_data;

    // First insert keys
    for (uint64_t i = 0; i < iterations; i++) {
        path_t* path = generate_test_path(i);
        identifier_t* value = generate_test_value(i);
        database_put_sync(ctx->db, path, value);
    }

    // Then delete them
    for (uint64_t i = 0; i < iterations; i++) {
        path_t* path = generate_test_path(i);

        int result = database_delete_sync(ctx->db, path);

        if (result != 0) {
            fprintf(stderr, "Delete failed at iteration %lu\n", i);
        }
    }
}

#ifdef __cplusplus
}
#endif

// Run all benchmarks
void run_sync_benchmarks(void) {
    printf("========================================\n");
    printf("Synchronous Database Benchmarks\n");
    printf("========================================\n\n");

    sync_benchmark_ctx_t ctx;

    // Single-threaded: Put
    printf("Running Sync Put benchmark...\n");
    sync_benchmark_init(&ctx, "put");
    benchmark_metrics_t put_metrics = benchmark_run(
        "Sync Put",
        benchmark_sync_put,
        &ctx,
        10,      // warmup
        10000    // measurement iterations
    );
    benchmark_print_results(&put_metrics);
    benchmark_save_json(".benchmarks/sync_put.json", &put_metrics);
    sync_benchmark_cleanup(&ctx);

    // Single-threaded: Get
    printf("\nRunning Sync Get benchmark...\n");
    sync_benchmark_init(&ctx, "get");
    // Pre-populate with keys
    for (int i = 0; i < 10000; i++) {
        path_t* path = generate_test_path(i);
        identifier_t* value = generate_test_value(i);
        database_put_sync(ctx.db, path, value);
    }
    ctx.counter = 10000;  // Track how many keys exist

    benchmark_metrics_t get_metrics = benchmark_run(
        "Sync Get",
        benchmark_sync_get,
        &ctx,
        10,
        10000
    );
    benchmark_print_results(&get_metrics);
    benchmark_save_json(".benchmarks/sync_get.json", &get_metrics);
    sync_benchmark_cleanup(&ctx);

    // Single-threaded: Mixed
    printf("\nRunning Sync Mixed benchmark...\n");
    sync_benchmark_init(&ctx, "mixed");
    benchmark_metrics_t mixed_metrics = benchmark_run(
        "Sync Mixed",
        benchmark_sync_mixed,
        &ctx,
        10,
        10000
    );
    benchmark_print_results(&mixed_metrics);
    benchmark_save_json(".benchmarks/sync_mixed.json", &mixed_metrics);
    sync_benchmark_cleanup(&ctx);

    // Single-threaded: Delete
    printf("\nRunning Sync Delete benchmark...\n");
    sync_benchmark_init(&ctx, "delete");
    benchmark_metrics_t delete_metrics = benchmark_run(
        "Sync Delete",
        benchmark_sync_delete,
        &ctx,
        10,
        10000
    );
    benchmark_print_results(&delete_metrics);
    benchmark_save_json(".benchmarks/sync_delete.json", &delete_metrics);
    sync_benchmark_cleanup(&ctx);

    printf("========================================\n\n");
}

int main(int argc, char** argv) {
    // Initialize transaction ID system
    transaction_id_init();

    // Create benchmark directory if it doesn't exist
    system("mkdir -p .benchmarks");

    run_sync_benchmarks();

    return 0;
}