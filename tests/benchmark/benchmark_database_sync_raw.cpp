//
// Raw Synchronous Database Performance Benchmarks
// Compares raw string-based API against the original path/identifier API
//

// C++ headers with <atomic> must come before benchmark_base.h
// which transitively includes C headers with ATOMIC_TYPE() macros
#include <atomic>
#include <chrono>

#include "benchmark_base.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
extern "C" {
#include "Database/database.h"
}

// Context for raw sync benchmarks
typedef struct {
    database_t* db;
    char test_dir[256];
} raw_sync_benchmark_ctx_t;

// Helper function to initialize benchmark context
static int raw_sync_benchmark_init(raw_sync_benchmark_ctx_t* ctx, const char* test_name) {
    // Create unique test directory
    snprintf(ctx->test_dir, sizeof(ctx->test_dir),
             "/tmp/raw_sync_db_bench_%s_%d", test_name, getpid());

    // Create directory
    mkdir(ctx->test_dir, 0755);

    // Create database with default config, persistence disabled
    database_config_t* config = database_config_default();
    config->enable_persist = 0;
    ctx->db = database_create_with_config(ctx->test_dir, config, NULL);
    database_config_destroy(config);

    if (!ctx->db) {
        fprintf(stderr, "Failed to create database\n");
        return -1;
    }

    return 0;
}

// Helper function to cleanup benchmark context
static void raw_sync_benchmark_cleanup(raw_sync_benchmark_ctx_t* ctx) {
    if (ctx->db) {
        database_destroy(ctx->db);
    }

    // Clean up test directory
    if (ctx->test_dir[0] != '\0') {
        char cmd[512];
        snprintf(cmd, sizeof(cmd), "rm -rf %s", ctx->test_dir);
        system(cmd);
    }
}

// Helper: format key string into buffer, return length
static int format_key(char* buf, size_t bufsize, int key_id) {
    return snprintf(buf, bufsize, "bench_key_%d", key_id);
}

// Helper: format value string into buffer, return length
static int format_value(char* buf, size_t bufsize, int value_id) {
    return snprintf(buf, bufsize, "bench_value_%d_%ld", value_id,
                    std::chrono::high_resolution_clock::now().time_since_epoch().count());
}

// Benchmark: Raw Synchronous Put operations
static void benchmark_raw_sync_put(void* user_data, uint64_t iterations) {
    raw_sync_benchmark_ctx_t* ctx = (raw_sync_benchmark_ctx_t*)user_data;

    char key_buf[64];
    char val_buf[128];

    for (uint64_t i = 0; i < iterations; i++) {
        int key_len = format_key(key_buf, sizeof(key_buf), (int)i);
        int val_len = format_value(val_buf, sizeof(val_buf), (int)i);

        int result = database_put_sync_raw(ctx->db, key_buf, key_len, '/',
                                           (const uint8_t*)val_buf, val_len);
        if (result != 0) {
            fprintf(stderr, "ERROR: database_put_sync_raw failed at iteration %lu\n", i);
        }
    }
}

// Benchmark: Raw Synchronous Get operations (requires pre-populated database)
static void benchmark_raw_sync_get(void* user_data, uint64_t iterations) {
    raw_sync_benchmark_ctx_t* ctx = (raw_sync_benchmark_ctx_t*)user_data;

    char key_buf[64];

    for (uint64_t i = 0; i < iterations; i++) {
        int key_len = format_key(key_buf, sizeof(key_buf), (int)(i % 10000));

        uint8_t* value_out = NULL;
        size_t value_len_out = 0;
        int ret = database_get_sync_raw(ctx->db, key_buf, key_len, '/',
                                        &value_out, &value_len_out);

        if (ret == 0 && value_out) {
            database_raw_value_free(value_out);
        }

        if (ret != 0 && ret != -2) {
            fprintf(stderr, "ERROR: database_get_sync_raw failed at iteration %lu (ret=%d)\n", i, ret);
        }
    }
}

// Benchmark: Raw Synchronous Mixed operations (70% read, 20% write, 10% delete)
static void benchmark_raw_sync_mixed(void* user_data, uint64_t iterations) {
    raw_sync_benchmark_ctx_t* ctx = (raw_sync_benchmark_ctx_t*)user_data;
    int key_counter = 0;

    char key_buf[64];
    char val_buf[128];

    for (uint64_t i = 0; i < iterations; i++) {
        int op_type = i % 10;

        if (op_type < 7) {
            // Read operation (70%)
            int key_len = format_key(key_buf, sizeof(key_buf), (int)(i % 10000));
            uint8_t* value_out = NULL;
            size_t value_len_out = 0;
            database_get_sync_raw(ctx->db, key_buf, key_len, '/',
                                  &value_out, &value_len_out);
            if (value_out) {
                database_raw_value_free(value_out);
            }
        } else if (op_type < 9) {
            // Write operation (20%)
            int current_key = key_counter++;
            int key_len = format_key(key_buf, sizeof(key_buf), current_key);
            int val_len = format_value(val_buf, sizeof(val_buf), current_key);
            database_put_sync_raw(ctx->db, key_buf, key_len, '/',
                                  (const uint8_t*)val_buf, val_len);
        } else {
            // Delete operation (10%)
            int key_len = format_key(key_buf, sizeof(key_buf), (int)(i % 5000));
            database_delete_sync_raw(ctx->db, key_buf, key_len, '/');
        }
    }
}

// Benchmark: Raw Synchronous Delete operations
static void benchmark_raw_sync_delete(void* user_data, uint64_t iterations) {
    raw_sync_benchmark_ctx_t* ctx = (raw_sync_benchmark_ctx_t*)user_data;

    char key_buf[64];

    for (uint64_t i = 0; i < iterations; i++) {
        int key_len = format_key(key_buf, sizeof(key_buf), (int)(i % 10000));
        database_delete_sync_raw(ctx->db, key_buf, key_len, '/');
    }
}

// Pre-populate database with test data using raw API
static void populate_database_raw(database_t* db, int count) {
    printf("Pre-populating database with %d entries (raw API)...\n", count);

    char key_buf[64];
    char val_buf[128];

    for (int i = 0; i < count; i++) {
        int key_len = format_key(key_buf, sizeof(key_buf), i);
        int val_len = format_value(val_buf, sizeof(val_buf), i);

        int result = database_put_sync_raw(db, key_buf, key_len, '/',
                                           (const uint8_t*)val_buf, val_len);
        if (result != 0) {
            fprintf(stderr, "ERROR: Failed to pre-populate key %d\n", i);
        }

        if (i % 1000 == 0) {
            printf("  Pre-populated %d/%d entries...\n", i, count);
        }
    }

    printf("Pre-population complete.\n\n");
}

// Run all raw synchronous benchmarks
static void run_raw_sync_benchmarks() {
    printf("========================================\n");
    printf("Raw Synchronous Database Benchmarks\n");
    printf("========================================\n\n");

    printf("Configuration:\n");
    printf("  - Persistence: DISABLED (in-memory)\n");
    printf("  - Single-threaded (no work pool)\n\n");

    // Setup
    raw_sync_benchmark_ctx_t ctx;
    if (raw_sync_benchmark_init(&ctx, "main") != 0) {
        fprintf(stderr, "Failed to initialize benchmark context\n");
        return;
    }

    printf("Setup complete. Running benchmarks...\n\n");

    // Pre-populate for read benchmarks
    populate_database_raw(ctx.db, 10000);

    // Benchmark 1: Raw Put operations
    printf("--- Raw Synchronous Put Operations ---\n");
    benchmark_metrics_t put_metrics = benchmark_run(
        "Raw Sync Put",
        benchmark_raw_sync_put,
        &ctx,
        10,      // 10 warmup iterations
        10000    // 10000 measurement iterations
    );
    benchmark_print_results(&put_metrics);
    benchmark_save_json(".benchmarks/raw_sync_put.json", &put_metrics);
    printf("\n");

    // Benchmark 2: Raw Get operations
    printf("--- Raw Synchronous Get Operations ---\n");
    benchmark_metrics_t get_metrics = benchmark_run(
        "Raw Sync Get",
        benchmark_raw_sync_get,
        &ctx,
        10,      // 10 warmup iterations
        10000    // 10000 measurement iterations
    );
    benchmark_print_results(&get_metrics);
    benchmark_save_json(".benchmarks/raw_sync_get.json", &get_metrics);
    printf("\n");

    // Benchmark 3: Raw Mixed workload
    printf("--- Raw Synchronous Mixed Workload (70%% read, 20%% write, 10%% delete) ---\n");
    benchmark_metrics_t mixed_metrics = benchmark_run(
        "Raw Sync Mixed",
        benchmark_raw_sync_mixed,
        &ctx,
        10,      // 10 warmup iterations
        10000    // 10000 measurement iterations
    );
    benchmark_print_results(&mixed_metrics);
    benchmark_save_json(".benchmarks/raw_sync_mixed.json", &mixed_metrics);
    printf("\n");

    // Benchmark 4: Raw Delete operations
    printf("--- Raw Synchronous Delete Operations ---\n");
    benchmark_metrics_t delete_metrics = benchmark_run(
        "Raw Sync Delete",
        benchmark_raw_sync_delete,
        &ctx,
        10,      // 10 warmup iterations
        10000    // 10000 measurement iterations
    );
    benchmark_print_results(&delete_metrics);
    benchmark_save_json(".benchmarks/raw_sync_delete.json", &delete_metrics);
    printf("\n");

    // Cleanup
    raw_sync_benchmark_cleanup(&ctx);

    printf("========================================\n");
    printf("All raw synchronous benchmarks complete.\n");
    printf("========================================\n");
}

int main(int argc, char** argv) {
    // Ensure benchmark output directory exists
    system("mkdir -p .benchmarks");

    printf("Starting raw synchronous database benchmarks...\n\n");
    run_raw_sync_benchmarks();
    return 0;
}