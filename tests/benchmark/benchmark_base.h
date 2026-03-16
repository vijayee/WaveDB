//
// Created by victor on 3/16/26.
// Benchmark infrastructure for performance testing
//

#ifndef WAVEDB_BENCHMARK_BASE_H
#define WAVEDB_BENCHMARK_BASE_H

#include <stdint.h>
#include <stddef.h>
#include "../../src/Time/debouncer.h"

#ifdef __cplusplus
extern "C" {
#endif

// Performance metrics for a benchmark run
typedef struct {
    uint64_t operations_count;
    uint64_t total_time_ns;
    uint64_t min_time_ns;
    uint64_t max_time_ns;
    uint64_t p50_time_ns;    // 50th percentile (median)
    uint64_t p95_time_ns;    // 95th percentile
    uint64_t p99_time_ns;    // 99th percentile
    double operations_per_second;
    double avg_latency_ns;
    char name[256];
} benchmark_metrics_t;

// Benchmark result structure
typedef struct {
    benchmark_metrics_t* metrics;
    size_t count;
    size_t capacity;
} benchmark_results_t;

// Benchmark configuration
typedef struct {
    const char* name;
    uint64_t warmup_iterations;      // Number of warmup iterations
    uint64_t measurement_iterations; // Number of measurement iterations
    uint64_t batch_size;             // Operations per iteration
    void* user_data;                 // User-provided context
} benchmark_config_t;

// Benchmark function type
typedef void (*benchmark_func_t)(void* user_data, uint64_t iterations);

// Initialize benchmark results structure
benchmark_results_t* benchmark_results_create(void);

// Destroy benchmark results structure
void benchmark_results_destroy(benchmark_results_t* results);

// Run a single benchmark
benchmark_metrics_t benchmark_run(
    const char* name,
    benchmark_func_t func,
    void* user_data,
    uint64_t warmup_iterations,
    uint64_t measurement_iterations
);

// Run a batch of operations and measure time
uint64_t benchmark_measure_batch(
    benchmark_func_t func,
    void* user_data,
    uint64_t iterations
);

// Calculate percentiles from sorted array of latencies
void benchmark_calculate_percentiles(
    uint64_t* latencies,
    size_t count,
    uint64_t* p50,
    uint64_t* p95,
    uint64_t* p99
);

// Print benchmark results to stdout
void benchmark_print_results(const benchmark_metrics_t* metrics);

// Print comparison between two benchmarks
void benchmark_print_comparison(
    const char* name,
    const benchmark_metrics_t* baseline,
    const benchmark_metrics_t* optimized
);

// Save benchmark results to JSON file
int benchmark_save_json(
    const char* filename,
    const benchmark_metrics_t* metrics
);

// Load benchmark results from JSON file
int benchmark_load_json(
    const char* filename,
    benchmark_metrics_t* metrics
);

#ifdef __cplusplus
}
#endif

#endif // WAVEDB_BENCHMARK_BASE_H