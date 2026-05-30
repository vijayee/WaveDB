//
// Synchronous Database Performance Benchmarks - sync_only mode
// Single-threaded tests using sync_only fast path (no concurrency control)
//

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
#include "Database/database_config.h"
#include "Database/wal_manager.h"
#include "HBTrie/path.h"
#include "HBTrie/identifier.h"
#include "Buffer/buffer.h"
#include "Util/allocator.h"
}

typedef struct {
    database_t* db;
    char test_dir[256];
} sync_benchmark_ctx_t;

static int sync_benchmark_init(sync_benchmark_ctx_t* ctx, const char* test_name) {
    snprintf(ctx->test_dir, sizeof(ctx->test_dir),
             "/tmp/sync_db_bench_%s_%d", test_name, getpid());

    mkdir(ctx->test_dir, 0755);

    database_config_t* config = database_config_default();
    if (!config) {
        fprintf(stderr, "Failed to create database config\n");
        return -1;
    }

    database_config_set_sync_only(config, 1);
    database_config_set_lru_memory_mb(config, 50);
    database_config_set_enable_persist(config, 1);
    database_config_set_wal_sync_mode(config, WAL_SYNC_ASYNC);
    database_config_set_wal_debounce_ms(config, 100);
    database_config_set_wal_max_file_size(config, 100 * 1024 * 1024);

    int error_code = 0;
    ctx->db = database_create_with_config(ctx->test_dir, config, &error_code);
    database_config_destroy(config);

    if (!ctx->db) {
        fprintf(stderr, "Failed to create database: error_code=%d\n", error_code);
        return -1;
    }

    return 0;
}

static void sync_benchmark_cleanup(sync_benchmark_ctx_t* ctx) {
    if (ctx->db) {
        database_destroy(ctx->db);
    }

    if (ctx->test_dir[0] != '\0') {
        char cmd[512];
        snprintf(cmd, sizeof(cmd), "rm -rf %s", ctx->test_dir);
        system(cmd);
    }
}

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

static identifier_t* generate_test_value(int value_id) {
    char value[128];
    snprintf(value, sizeof(value), "bench_value_%d_%ld", value_id, std::chrono::high_resolution_clock::now().time_since_epoch().count());

    buffer_t* buf = buffer_create_from_pointer_copy((uint8_t*)value, strlen(value));
    identifier_t* id = identifier_create(buf, 0);
    buffer_destroy(buf);

    return id;
}

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

static void benchmark_sync_get(void* user_data, uint64_t iterations) {
    sync_benchmark_ctx_t* ctx = (sync_benchmark_ctx_t*)user_data;

    for (uint64_t i = 0; i < iterations; i++) {
        path_t* path = generate_test_path((int)(i % 10000));

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

static void benchmark_sync_mixed(void* user_data, uint64_t iterations) {
    sync_benchmark_ctx_t* ctx = (sync_benchmark_ctx_t*)user_data;
    int key_counter = 0;

    for (uint64_t i = 0; i < iterations; i++) {
        int op_type = i % 10;

        if (op_type < 7) {
            path_t* path = generate_test_path((int)(i % 10000));
            identifier_t* result = NULL;
            database_get_sync(ctx->db, path, &result);
            if (result) {
                identifier_destroy(result);
            }
        } else if (op_type < 9) {
            int current_key = key_counter++;
            path_t* path = generate_test_path(current_key);
            identifier_t* value = generate_test_value(current_key);
            database_put_sync(ctx->db, path, value);
        } else {
            path_t* path = generate_test_path((int)(i % 5000));
            database_delete_sync(ctx->db, path);
        }
    }
}

static void benchmark_sync_delete(void* user_data, uint64_t iterations) {
    sync_benchmark_ctx_t* ctx = (sync_benchmark_ctx_t*)user_data;

    for (uint64_t i = 0; i < iterations; i++) {
        path_t* path = generate_test_path((int)(i % 10000));
        database_delete_sync(ctx->db, path);
    }
}

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

static void run_sync_benchmarks() {
    printf("========================================\n");
    printf("Synchronous Database Benchmarks (sync_only)\n");
    printf("========================================\n\n");

    printf("Configuration:\n");
    printf("  - Mode: sync_only (no spinlocks, MVCC, LRU mutex)\n");
    printf("  - WAL mode: ASYNC (no fsync)\n");
    printf("  - LRU cache: 50 MB\n");
    printf("  - WAL file size: 100 MB\n");
    printf("  - Single-threaded (no work pool)\n\n");

    sync_benchmark_ctx_t ctx;
    if (sync_benchmark_init(&ctx, "sync_only") != 0) {
        fprintf(stderr, "Failed to initialize benchmark context\n");
        return;
    }

    printf("Setup complete. Running benchmarks...\n\n");

    populate_database(ctx.db, 10000);

    printf("--- Synchronous Put Operations ---\n");
    benchmark_metrics_t put_metrics = benchmark_run(
        "Sync Put (sync_only)",
        benchmark_sync_put,
        &ctx,
        10,
        10000
    );
    benchmark_print_results(&put_metrics);
    benchmark_save_json(".benchmarks/sync_only_put.json", &put_metrics);
    printf("\n");

    printf("--- Synchronous Get Operations ---\n");
    benchmark_metrics_t get_metrics = benchmark_run(
        "Sync Get (sync_only)",
        benchmark_sync_get,
        &ctx,
        10,
        10000
    );
    benchmark_print_results(&get_metrics);
    benchmark_save_json(".benchmarks/sync_only_get.json", &get_metrics);
    printf("\n");

    printf("--- Synchronous Mixed Workload (70%% read, 20%% write, 10%% delete) ---\n");
    benchmark_metrics_t mixed_metrics = benchmark_run(
        "Sync Mixed (sync_only)",
        benchmark_sync_mixed,
        &ctx,
        10,
        10000
    );
    benchmark_print_results(&mixed_metrics);
    benchmark_save_json(".benchmarks/sync_only_mixed.json", &mixed_metrics);
    printf("\n");

    printf("--- Synchronous Delete Operations ---\n");
    benchmark_metrics_t delete_metrics = benchmark_run(
        "Sync Delete (sync_only)",
        benchmark_sync_delete,
        &ctx,
        10,
        10000
    );
    benchmark_print_results(&delete_metrics);
    benchmark_save_json(".benchmarks/sync_only_delete.json", &delete_metrics);
    printf("\n");

    sync_benchmark_cleanup(&ctx);

    printf("========================================\n");
    printf("All sync_only benchmarks complete.\n");
    printf("========================================\n");
}

int main(int argc, char** argv) {
    system("mkdir -p .benchmarks");

    printf("Starting sync_only database benchmarks...\n\n");
    run_sync_benchmarks();
    return 0;
}
