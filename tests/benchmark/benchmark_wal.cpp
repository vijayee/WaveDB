//
// Created by victor on 3/16/26.
// WAL (Write-Ahead Log) micro-benchmarks
//

#include "benchmark_base.h"
#include "../../src/Database/wal.h"
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

// Test context for WAL benchmarks
typedef struct {
    wal_t* wal;
    char test_dir[256];
    uint64_t counter;
} wal_benchmark_context_t;

// Initialize WAL for benchmark
static void wal_benchmark_init(wal_benchmark_context_t* ctx, const char* test_name) {
    snprintf(ctx->test_dir, sizeof(ctx->test_dir), "/tmp/wal_benchmark_%s_%d", test_name, getpid());
    mkdir(ctx->test_dir, 0755);

    int error_code = 0;
    ctx->wal = wal_create(ctx->test_dir, 0, &error_code);
    ctx->counter = 0;

    if (error_code != 0 || ctx->wal == NULL) {
        fprintf(stderr, "Failed to create WAL: error_code=%d\n", error_code);
        exit(1);
    }
}

// Cleanup WAL benchmark
static void wal_benchmark_cleanup(wal_benchmark_context_t* ctx) {
    if (ctx->wal) {
        wal_destroy(ctx->wal);
    }

    // Remove test directory
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", ctx->test_dir);
    system(cmd);
}

// Generate test data
static buffer_t* generate_test_data(uint64_t counter) {
    char data[256];
    snprintf(data, sizeof(data), "test_data_%lu_%s", counter, "payload_for_wal_entry");
    buffer_t* buf = buffer_create(strlen(data));
    memcpy(buf->data, data, strlen(data));
    return buf;
}

// Generate transaction ID
static transaction_id_t generate_txn_id(uint64_t counter) {
    transaction_id_t txn_id;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    txn_id.time = ts.tv_sec;
    txn_id.nanos = ts.tv_nsec;
    txn_id.count = counter;
    return txn_id;
}

// Benchmark: Single WAL write (with implicit fsync)
static void benchmark_wal_write_single(void* user_data, uint64_t iterations) {
    wal_benchmark_context_t* ctx = (wal_benchmark_context_t*)user_data;

    for (uint64_t i = 0; i < iterations; i++) {
        buffer_t* data = generate_test_data(ctx->counter++);
        transaction_id_t txn_id = generate_txn_id(i);

        int result = wal_write(ctx->wal, txn_id, WAL_PUT, data);
        if (result < 0) {
            fprintf(stderr, "WAL write failed at iteration %lu\n", i);
        }

        buffer_destroy(data);
    }
}

// Benchmark: WAL write batch (no explicit fsync control - uses default behavior)
static void benchmark_wal_write_batch(void* user_data, uint64_t iterations) {
    wal_benchmark_context_t* ctx = (wal_benchmark_context_t*)user_data;

    for (uint64_t i = 0; i < iterations; i++) {
        buffer_t* data = generate_test_data(ctx->counter++);
        transaction_id_t txn_id = generate_txn_id(i);

        int result = wal_write(ctx->wal, txn_id, WAL_PUT, data);
        if (result < 0) {
            fprintf(stderr, "WAL write failed at iteration %lu\n", i);
        }

        buffer_destroy(data);
    }
}

// Benchmark: WAL rotation (force rotation by creating many small entries)
static void benchmark_wal_rotation(void* user_data, uint64_t iterations) {
    wal_benchmark_context_t* ctx = (wal_benchmark_context_t*)user_data;

    // Set a small max_size to force rotation
    // (This is a simulation - actual rotation happens when size exceeds limit)
    for (uint64_t i = 0; i < iterations; i++) {
        // Create a new WAL for each iteration to simulate rotation
        wal_destroy(ctx->wal);

        int error_code = 0;
        ctx->wal = wal_create(ctx->test_dir, 1024, &error_code);  // 1KB max size
        if (error_code != 0 || ctx->wal == NULL) {
            fprintf(stderr, "Failed to create WAL: error_code=%d\n", error_code);
            continue;
        }

        // Write one entry to trigger initialization
        buffer_t* data = generate_test_data(ctx->counter++);
        transaction_id_t txn_id = generate_txn_id(i);
        wal_write(ctx->wal, txn_id, WAL_PUT, data);
        buffer_destroy(data);
    }
}

// Run all WAL benchmarks
void run_wal_benchmarks(void) {
    wal_benchmark_context_t ctx;

    printf("========================================\n");
    printf("WAL (Write-Ahead Log) Benchmarks\n");
    printf("========================================\n\n");

    // Benchmark 1: Single write with default fsync behavior
    printf("Running WAL single write benchmark...\n");
    wal_benchmark_init(&ctx, "single_write");
    benchmark_metrics_t single_write = benchmark_run(
        "WAL Single Write",
        benchmark_wal_write_single,
        &ctx,
        10,      // warmup (WAL operations are expensive)
        100      // measurement iterations
    );
    benchmark_print_results(&single_write);
    benchmark_save_json(".benchmarks/wal_single_write_baseline.json", &single_write);
    wal_benchmark_cleanup(&ctx);

    // Benchmark 2: Batch writes (measure throughput)
    printf("Running WAL batch write benchmark...\n");
    wal_benchmark_init(&ctx, "batch_write");
    benchmark_metrics_t batch_write = benchmark_run(
        "WAL Batch Write",
        benchmark_wal_write_batch,
        &ctx,
        10,      // warmup
        1000     // measurement iterations
    );
    benchmark_print_results(&batch_write);
    benchmark_save_json(".benchmarks/wal_batch_write_baseline.json", &batch_write);
    wal_benchmark_cleanup(&ctx);

    // Benchmark 3: WAL rotation (expensive operation)
    printf("Running WAL rotation benchmark...\n");
    wal_benchmark_init(&ctx, "rotation");
    benchmark_metrics_t rotation = benchmark_run(
        "WAL Rotation",
        benchmark_wal_rotation,
        &ctx,
        5,       // warmup (rotation is very expensive)
        20       // measurement iterations (keep low due to cost)
    );
    benchmark_print_results(&rotation);
    benchmark_save_json(".benchmarks/wal_rotation_baseline.json", &rotation);
    wal_benchmark_cleanup(&ctx);

    printf("========================================\n");
    printf("Expected Baselines:\n");
    printf("Single write with fsync: ~1-10ms (disk-dependent)\n");
    printf("Write without fsync: ~50-200μs\n");
    printf("WAL rotation: ~5-20ms\n");
    printf("========================================\n\n");
}

#ifdef __cplusplus
}
#endif

int main(int argc, char** argv) {
    // Create benchmark directory if it doesn't exist
    system("mkdir -p .benchmarks");

    run_wal_benchmarks();

    return 0;
}