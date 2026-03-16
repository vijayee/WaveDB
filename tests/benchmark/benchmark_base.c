//
// Created by victor on 3/16/26.
// Benchmark infrastructure implementation
//

#include "benchmark_base.h"
#include "../../src/Util/allocator.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

// Compare function for qsort
static int compare_uint64(const void* a, const void* b) {
    uint64_t va = *(const uint64_t*)a;
    uint64_t vb = *(const uint64_t*)b;
    if (va < vb) return -1;
    if (va > vb) return 1;
    return 0;
}

benchmark_results_t* benchmark_results_create(void) {
    benchmark_results_t* results = get_clear_memory(sizeof(benchmark_results_t));
    results->capacity = 16;
    results->count = 0;
    results->metrics = get_clear_memory(results->capacity * sizeof(benchmark_metrics_t));
    return results;
}

void benchmark_results_destroy(benchmark_results_t* results) {
    if (results) {
        free(results->metrics);
        free(results);
    }
}

uint64_t benchmark_measure_batch(
    benchmark_func_t func,
    void* user_data,
    uint64_t iterations
) {
    benchmark_timer_t timer;
    benchmark_start(&timer);
    func(user_data, iterations);
    return benchmark_end(&timer);
}

void benchmark_calculate_percentiles(
    uint64_t* latencies,
    size_t count,
    uint64_t* p50,
    uint64_t* p95,
    uint64_t* p99
) {
    if (count == 0) {
        *p50 = *p95 = *p99 = 0;
        return;
    }

    // Sort the latencies
    qsort(latencies, count, sizeof(uint64_t), compare_uint64);

    // Calculate percentiles
    *p50 = latencies[count / 2];
    *p95 = latencies[(count * 95) / 100];
    *p99 = latencies[(count * 99) / 100];
}

benchmark_metrics_t benchmark_run(
    const char* name,
    benchmark_func_t func,
    void* user_data,
    uint64_t warmup_iterations,
    uint64_t measurement_iterations
) {
    benchmark_metrics_t metrics;
    memset(&metrics, 0, sizeof(metrics));
    strncpy(metrics.name, name, sizeof(metrics.name) - 1);

    // Warmup phase (no measurements)
    if (warmup_iterations > 0) {
        func(user_data, warmup_iterations);
    }

    // Allocate array for per-operation latencies
    uint64_t* latencies = malloc(measurement_iterations * sizeof(uint64_t));
    if (!latencies) {
        fprintf(stderr, "Failed to allocate latency array\n");
        return metrics;
    }

    // Measurement phase - measure each operation individually
    metrics.min_time_ns = UINT64_MAX;
    metrics.max_time_ns = 0;
    metrics.total_time_ns = 0;

    for (uint64_t i = 0; i < measurement_iterations; i++) {
        benchmark_timer_t timer;
        benchmark_start(&timer);
        func(user_data, 1);
        uint64_t elapsed = benchmark_end(&timer);

        latencies[i] = elapsed;
        metrics.total_time_ns += elapsed;

        if (elapsed < metrics.min_time_ns) {
            metrics.min_time_ns = elapsed;
        }
        if (elapsed > metrics.max_time_ns) {
            metrics.max_time_ns = elapsed;
        }
    }

    metrics.operations_count = measurement_iterations;
    metrics.avg_latency_ns = (double)metrics.total_time_ns / measurement_iterations;
    metrics.operations_per_second = (measurement_iterations * 1e9) / metrics.total_time_ns;

    // Calculate percentiles
    benchmark_calculate_percentiles(
        latencies,
        measurement_iterations,
        &metrics.p50_time_ns,
        &metrics.p95_time_ns,
        &metrics.p99_time_ns
    );

    free(latencies);
    return metrics;
}

void benchmark_print_results(const benchmark_metrics_t* metrics) {
    printf("=== %s ===\n", metrics->name);
    printf("Operations: %lu\n", metrics->operations_count);
    printf("Total time: %.2f ms\n", metrics->total_time_ns / 1e6);
    printf("Throughput: %.2f ops/sec\n", metrics->operations_per_second);
    printf("Latency:\n");
    printf("  Min: %lu ns (%.2f μs)\n", metrics->min_time_ns, metrics->min_time_ns / 1e3);
    printf("  Avg: %.2f ns (%.2f μs)\n", metrics->avg_latency_ns, metrics->avg_latency_ns / 1e3);
    printf("  P50: %lu ns (%.2f μs)\n", metrics->p50_time_ns, metrics->p50_time_ns / 1e3);
    printf("  P95: %lu ns (%.2f μs)\n", metrics->p95_time_ns, metrics->p95_time_ns / 1e3);
    printf("  P99: %lu ns (%.2f μs)\n", metrics->p99_time_ns, metrics->p99_time_ns / 1e3);
    printf("  Max: %lu ns (%.2f μs)\n", metrics->max_time_ns, metrics->max_time_ns / 1e3);
    printf("\n");
}

void benchmark_print_comparison(
    const char* name,
    const benchmark_metrics_t* baseline,
    const benchmark_metrics_t* optimized
) {
    printf("=== %s Comparison ===\n", name);
    printf("Baseline: %.2f ops/sec, %.2f μs avg latency\n",
           baseline->operations_per_second,
           baseline->avg_latency_ns / 1e3);
    printf("Optimized: %.2f ops/sec, %.2f μs avg latency\n",
           optimized->operations_per_second,
           optimized->avg_latency_ns / 1e3);

    double speedup = optimized->operations_per_second / baseline->operations_per_second;
    printf("Speedup: %.2fx\n", speedup);

    double latency_improvement = (baseline->avg_latency_ns - optimized->avg_latency_ns) / baseline->avg_latency_ns * 100;
    printf("Latency improvement: %.1f%%\n", latency_improvement);
    printf("\n");
}

int benchmark_save_json(
    const char* filename,
    const benchmark_metrics_t* metrics
) {
    FILE* fp = fopen(filename, "w");
    if (!fp) {
        perror("Failed to open file for writing");
        return -1;
    }

    fprintf(fp, "{\n");
    fprintf(fp, "  \"name\": \"%s\",\n", metrics->name);
    fprintf(fp, "  \"operations_count\": %lu,\n", metrics->operations_count);
    fprintf(fp, "  \"total_time_ns\": %lu,\n", metrics->total_time_ns);
    fprintf(fp, "  \"min_time_ns\": %lu,\n", metrics->min_time_ns);
    fprintf(fp, "  \"max_time_ns\": %lu,\n", metrics->max_time_ns);
    fprintf(fp, "  \"p50_time_ns\": %lu,\n", metrics->p50_time_ns);
    fprintf(fp, "  \"p95_time_ns\": %lu,\n", metrics->p95_time_ns);
    fprintf(fp, "  \"p99_time_ns\": %lu,\n", metrics->p99_time_ns);
    fprintf(fp, "  \"operations_per_second\": %.2f,\n", metrics->operations_per_second);
    fprintf(fp, "  \"avg_latency_ns\": %.2f\n", metrics->avg_latency_ns);
    fprintf(fp, "}\n");

    fclose(fp);
    return 0;
}

int benchmark_load_json(
    const char* filename,
    benchmark_metrics_t* metrics
) {
    FILE* fp = fopen(filename, "r");
    if (!fp) {
        perror("Failed to open file for reading");
        return -1;
    }

    // Simple JSON parsing (production code should use a proper JSON library)
    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        if (strstr(line, "\"name\":")) {
            char* start = strchr(line, '"');
            start = strchr(start + 1, '"');
            start = strchr(start + 1, '"') + 1;
            char* end = strchr(start, '"');
            size_t len = end - start;
            strncpy(metrics->name, start, len < sizeof(metrics->name) ? len : sizeof(metrics->name) - 1);
        } else if (strstr(line, "\"operations_count\":")) {
            sscanf(line, " \"operations_count\": %lu,", &metrics->operations_count);
        } else if (strstr(line, "\"total_time_ns\":")) {
            sscanf(line, " \"total_time_ns\": %lu,", &metrics->total_time_ns);
        } else if (strstr(line, "\"min_time_ns\":")) {
            sscanf(line, " \"min_time_ns\": %lu,", &metrics->min_time_ns);
        } else if (strstr(line, "\"max_time_ns\":")) {
            sscanf(line, " \"max_time_ns\": %lu,", &metrics->max_time_ns);
        } else if (strstr(line, "\"p50_time_ns\":")) {
            sscanf(line, " \"p50_time_ns\": %lu,", &metrics->p50_time_ns);
        } else if (strstr(line, "\"p95_time_ns\":")) {
            sscanf(line, " \"p95_time_ns\": %lu,", &metrics->p95_time_ns);
        } else if (strstr(line, "\"p99_time_ns\":")) {
            sscanf(line, " \"p99_time_ns\": %lu,", &metrics->p99_time_ns);
        } else if (strstr(line, "\"operations_per_second\":")) {
            sscanf(line, " \"operations_per_second\": %lf,", &metrics->operations_per_second);
        } else if (strstr(line, "\"avg_latency_ns\":")) {
            sscanf(line, " \"avg_latency_ns\": %lf", &metrics->avg_latency_ns);
        }
    }

    fclose(fp);
    return 0;
}