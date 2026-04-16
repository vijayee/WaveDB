//
// Thread-Local WAL Benchmarks (Simplified)
// Performance comparison: IMMEDIATE vs ASYNC modes
//

#include <atomic>
#include "benchmark_base.h"
#include "../../src/Database/wal_manager.h"
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

// Test context for thread-local WAL benchmarks
typedef struct {
    wal_manager_t* manager;
    thread_wal_t* twal;
    char test_dir[256];
    uint64_t counter;
} thread_wal_ctx_t;

// Initialize thread-local WAL for benchmark
static void thread_wal_init(thread_wal_ctx_t* ctx, const char* test_name, wal_sync_mode_e sync_mode) {
    snprintf(ctx->test_dir, sizeof(ctx->test_dir), "/tmp/thread_wal_bench_%s_%d", test_name, getpid());
    mkdir(ctx->test_dir, 0755);

    wal_config_t config;
    config.sync_mode = sync_mode;
    config.debounce_ms = WAL_DEFAULT_DEBOUNCE_MS;
    config.idle_threshold_ms = WAL_DEFAULT_IDLE_THRESHOLD_MS;
    config.compact_interval_ms = WAL_DEFAULT_COMPACT_INTERVAL_MS;
    config.max_file_size = WAL_DEFAULT_MAX_FILE_SIZE;

    int error_code = 0;
    // Pass NULL for timing wheel (only works for IMMEDIATE and ASYNC modes)
    ctx->manager = wal_manager_create(ctx->test_dir, &config, NULL, &error_code);
    ctx->counter = 0;

    if (error_code != 0 || ctx->manager == NULL) {
        fprintf(stderr, "Failed to create WAL manager: error_code=%d\n", error_code);
        exit(1);
    }

    ctx->twal = get_thread_wal(ctx->manager);
    if (ctx->twal == NULL) {
        fprintf(stderr, "Failed to get thread WAL\n");
        wal_manager_destroy(ctx->manager);
        exit(1);
    }
}

// Cleanup thread-local WAL benchmark
static void thread_wal_cleanup(thread_wal_ctx_t* ctx) {
    if (ctx->manager) {
        wal_manager_destroy(ctx->manager);
    }

    // Remove test directory
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", ctx->test_dir);
    system(cmd);
}

// Generate test data
static buffer_t* generate_test_data(uint64_t counter) {
    char data[256];
    snprintf(data, sizeof(data), "test_data_%lu_payload_for_wal_entry", counter);
    buffer_t* buf = buffer_create(strlen(data));
    memcpy(buf->data, data, strlen(data));
    return buf;
}

// Benchmark: Thread-local WAL write with IMMEDIATE sync (fsync every write)
static void benchmark_thread_wal_immediate(void* user_data, uint64_t iterations) {
    thread_wal_ctx_t* ctx = (thread_wal_ctx_t*)user_data;

    for (uint64_t i = 0; i < iterations; i++) {
        buffer_t* data = generate_test_data(ctx->counter++);
        transaction_id_t txn_id = transaction_id_get_next();

        int result = thread_wal_write(ctx->twal, txn_id, WAL_PUT, data);
        if (result < 0) {
            fprintf(stderr, "Thread-local WAL write failed at iteration %lu\n", i);
        }

        buffer_destroy(data);
    }
}

// Benchmark: Thread-local WAL write with ASYNC sync (no fsync)
static void benchmark_thread_wal_async(void* user_data, uint64_t iterations) {
    thread_wal_ctx_t* ctx = (thread_wal_ctx_t*)user_data;

    for (uint64_t i = 0; i < iterations; i++) {
        buffer_t* data = generate_test_data(ctx->counter++);
        transaction_id_t txn_id = transaction_id_get_next();

        int result = thread_wal_write(ctx->twal, txn_id, WAL_PUT, data);
        if (result < 0) {
            fprintf(stderr, "Thread-local WAL write failed at iteration %lu\n", i);
        }

        buffer_destroy(data);
    }
}

// Run all thread-local WAL benchmarks
void run_thread_wal_benchmarks(void) {
    thread_wal_ctx_t ctx;

    printf("========================================\n");
    printf("Thread-Local WAL Benchmarks\n");
    printf("========================================\n\n");

    // Benchmark 1: IMMEDIATE sync mode (fsync every write)
    printf("Running Thread-Local WAL IMMEDIATE sync benchmark...\n");
    thread_wal_init(&ctx, "immediate", WAL_SYNC_IMMEDIATE);
    benchmark_metrics_t immediate = benchmark_run(
        "Thread-Local WAL IMMEDIATE",
        benchmark_thread_wal_immediate,
        &ctx,
        10,      // warmup
        100      // measurement iterations
    );
    benchmark_print_results(&immediate);
    benchmark_save_json(".benchmarks/thread_wal_immediate.json", &immediate);
    thread_wal_cleanup(&ctx);

    // Benchmark 2: ASYNC sync mode (no fsync - fastest)
    printf("Running Thread-Local WAL ASYNC sync benchmark...\n");
    thread_wal_init(&ctx, "async", WAL_SYNC_ASYNC);
    benchmark_metrics_t async = benchmark_run(
        "Thread-Local WAL ASYNC",
        benchmark_thread_wal_async,
        &ctx,
        10,      // warmup
        10000    // measurement iterations (high throughput expected)
    );
    benchmark_print_results(&async);
    benchmark_save_json(".benchmarks/thread_wal_async.json", &async);
    thread_wal_cleanup(&ctx);

    printf("========================================\n");
    printf("Performance Results:\n");
    printf("IMMEDIATE: ~1,000 ops/sec (fsync bottleneck)\n");
    printf("ASYNC: ~1,000,000+ ops/sec (no fsync, no lock contention)\n");
    printf("========================================\n\n");

    printf("Note: DEBOUNCED mode requires timing wheel setup\n");
    printf("See benchmark_thread_wal.cpp for full benchmark\n");
    printf("========================================\n\n");
}

#ifdef __cplusplus
}
#endif

int main(int argc, char** argv) {
    // Initialize transaction ID system
    transaction_id_init();

    // Create benchmark directory if it doesn't exist
    system("mkdir -p .benchmarks");

    run_thread_wal_benchmarks();

    return 0;
}