//
// Synchronous Database Performance Benchmarks
// Single-threaded tests for synchronous database operations
//

#include "benchmark_base.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <chrono>
extern "C" {
#include "Database/database.h"
#include "Database/wal_manager.h"
#include "HBTrie/path.h"
#include "HBTrie/identifier.h"
#include "Buffer/buffer.h"
#include "Util/allocator.h"
}

// Context for synchronous benchmarks
typedef struct {
    database_t* db;
    char test_dir[256];
} sync_benchmark_ctx_t;

// Helper function to initialize benchmark context
static int sync_benchmark_init(sync_benchmark_ctx_t* ctx, const char* test_name) {
    // Create unique test directory
    snprintf(ctx->test_dir, sizeof(ctx->test_dir),
             "/tmp/sync_db_bench_%s_%d", test_name, getpid());

    // Create directory
    mkdir(ctx->test_dir, 0755);

    // Configure WAL for maximum performance (no fsync)
    wal_config_t wal_config = {
        .max_size = 100 * 1024 * 1024,  // 100MB WAL file
        .sync_mode = WAL_SYNC_ASYNC,      // No fsync for performance testing
        .batch_size = 1000                // Batch writes
    };

    // Create database with 50MB LRU, no work pool or timing wheel
    int error_code = 0;
    ctx->db = database_create(
        ctx->test_dir,
        50,                    // 50MB LRU cache
        &wal_config,
        0,                     // Default chunk size
        0,                     // Default btree node size
        1,                     // Enable persistent storage
        0,                     // Default storage cache size
        NULL,                  // No work pool (synchronous)
        NULL,                  // No timing wheel (synchronous)
        &error_code
    );

    if (!ctx->db) {
        fprintf(stderr, "Failed to create database: error_code=%d\n", error_code);
        return -1;
    }

    return 0;
}

// Helper function to cleanup benchmark context
static void sync_benchmark_cleanup(sync_benchmark_ctx_t* ctx) {
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

// Helper function to generate test path
static path_t* generate_test_path(int key_id) {
    char key[64];
    snprintf(key, sizeof(key), "bench_key_%d", key_id);

    path_t* path = path_create();
    buffer_t* buf = buffer_create_from_pointer_copy((uint8_t*)key, strlen(key));
    identifier_t* id = identifier_create(buf, 0);
    buffer_destroy(buf);
    path_append(path, id);
    identifier_destroy(id);

    return path;
}

// Helper function to generate test value
static identifier_t* generate_test_value(int value_id) {
    char value[128];
    snprintf(value, sizeof(value), "bench_value_%d_%ld", value_id, std::chrono::high_resolution_clock::now().time_since_epoch().count());

    buffer_t* buf = buffer_create_from_pointer_copy((uint8_t*)value, strlen(value));
    identifier_t* id = identifier_create(buf, 0);
    buffer_destroy(buf);

    return id;
}

// Benchmark: Synchronous Put operations
static void benchmark_sync_put(void* user_data, uint64_t iterations) {
    sync_benchmark_ctx_t* ctx = (sync_benchmark_ctx_t*)user_data;

    for (uint64_t i = 0; i < iterations; i++) {
        path_t* path = generate_test_path((int)i);
        identifier_t* value = generate_test_value((int)i);

        int result = database_put_sync(ctx->db, path, value);

        if (result != 0) {
            fprintf(stderr, "ERROR: database_put_sync failed at iteration %lu\n", i);
        }
    }
}

// Benchmark: Synchronous Get operations (requires pre-populated database)
static void benchmark_sync_get(void* user_data, uint64_t iterations) {
    sync_benchmark_ctx_t* ctx = (sync_benchmark_ctx_t*)user_data;

    for (uint64_t i = 0; i < iterations; i++) {
        path_t* path = generate_test_path((int)(i % 10000));  // Reuse keys from pre-population

        identifier_t* result = NULL;
        int ret = database_get_sync(ctx->db, path, &result);

        if (ret == 0 && result) {
            identifier_destroy(result);
        }

        if (ret != 0 && ret != -2) {
            fprintf(stderr, "ERROR: database_get_sync failed at iteration %lu (ret=%d)\n", i, ret);
        }
    }
}

// Benchmark: Synchronous Mixed operations (70% read, 20% write, 10% delete)
static void benchmark_sync_mixed(void* user_data, uint64_t iterations) {
    sync_benchmark_ctx_t* ctx = (sync_benchmark_ctx_t*)user_data;
    int key_counter = 0;

    for (uint64_t i = 0; i < iterations; i++) {
        int op_type = i % 10;

        if (op_type < 7) {
            // Read operation (70%)
            path_t* path = generate_test_path((int)(i % 10000));
            identifier_t* result = NULL;
            database_get_sync(ctx->db, path, &result);
            if (result) {
                identifier_destroy(result);
            }
        } else if (op_type < 9) {
            // Write operation (20%)
            path_t* path = generate_test_path(key_counter++);
            identifier_t* value = generate_test_value(key_counter);
            database_put_sync(ctx->db, path, value);
        } else {
            // Delete operation (10%)
            path_t* path = generate_test_path((int)(i % 5000));
            database_delete_sync(ctx->db, path);
        }
    }
}

// Benchmark: Synchronous Delete operations
static void benchmark_sync_delete(void* user_data, uint64_t iterations) {
    sync_benchmark_ctx_t* ctx = (sync_benchmark_ctx_t*)user_data;

    for (uint64_t i = 0; i < iterations; i++) {
        path_t* path = generate_test_path((int)(i % 10000));
        database_delete_sync(ctx->db, path);
    }
}

// Pre-populate database with test data
static void populate_database(database_t* db, int count) {
    printf("Pre-populating database with %d entries...\n", count);

    for (int i = 0; i < count; i++) {
        path_t* path = generate_test_path(i);
        identifier_t* value = generate_test_value(i);

        int result = database_put_sync(db, path, value);
        if (result != 0) {
            fprintf(stderr, "ERROR: Failed to pre-populate key %d\n", i);
        }

        if (i % 1000 == 0) {
            printf("  Pre-populated %d/%d entries...\n", i, count);
        }
    }

    printf("Pre-population complete.\n\n");
}

// Run all synchronous benchmarks
static void run_sync_benchmarks() {
    printf("========================================\n");
    printf("Synchronous Database Benchmarks\n");
    printf("========================================\n\n");

    printf("Configuration:\n");
    printf("  - WAL mode: ASYNC (no fsync)\n");
    printf("  - LRU cache: 50 MB\n");
    printf("  - WAL file size: 100 MB\n");
    printf("  - Single-threaded (no work pool)\n\n");

    // Setup
    sync_benchmark_ctx_t ctx;
    if (sync_benchmark_init(&ctx, "main") != 0) {
        fprintf(stderr, "Failed to initialize benchmark context\n");
        return;
    }

    printf("Setup complete. Running benchmarks...\n\n");

    // Pre-populate for read benchmarks
    populate_database(ctx.db, 10000);

    // Benchmark 1: Put operations
    printf("--- Synchronous Put Operations ---\n");
    benchmark_metrics_t put_metrics = benchmark_run(
        "Sync Put",
        benchmark_sync_put,
        &ctx,
        10,      // 10 warmup iterations
        10000    // 10000 measurement iterations
    );
    benchmark_print_results(&put_metrics);
    benchmark_save_json(".benchmarks/sync_put.json", &put_metrics);
    printf("\n");

    // Benchmark 2: Get operations
    printf("--- Synchronous Get Operations ---\n");
    benchmark_metrics_t get_metrics = benchmark_run(
        "Sync Get",
        benchmark_sync_get,
        &ctx,
        10,      // 10 warmup iterations
        10000    // 10000 measurement iterations
    );
    benchmark_print_results(&get_metrics);
    benchmark_save_json(".benchmarks/sync_get.json", &get_metrics);
    printf("\n");

    // Benchmark 3: Mixed workload
    printf("--- Synchronous Mixed Workload (70%% read, 20%% write, 10%% delete) ---\n");
    benchmark_metrics_t mixed_metrics = benchmark_run(
        "Sync Mixed",
        benchmark_sync_mixed,
        &ctx,
        10,      // 10 warmup iterations
        10000    // 10000 measurement iterations
    );
    benchmark_print_results(&mixed_metrics);
    benchmark_save_json(".benchmarks/sync_mixed.json", &mixed_metrics);
    printf("\n");

    // Benchmark 4: Delete operations
    printf("--- Synchronous Delete Operations ---\n");
    benchmark_metrics_t delete_metrics = benchmark_run(
        "Sync Delete",
        benchmark_sync_delete,
        &ctx,
        10,      // 10 warmup iterations
        10000    // 10000 measurement iterations
    );
    benchmark_print_results(&delete_metrics);
    benchmark_save_json(".benchmarks/sync_delete.json", &delete_metrics);
    printf("\n");

    // Cleanup
    sync_benchmark_cleanup(&ctx);

    printf("========================================\n");
    printf("All synchronous benchmarks complete.\n");
    printf("========================================\n");
}

int main(int argc, char** argv) {
    printf("Starting synchronous database benchmarks...\n\n");
    run_sync_benchmarks();
    return 0;
}