# WaveDB Performance Benchmarking

This document describes WaveDB's performance benchmarking infrastructure and regression detection system.

## Overview

WaveDB includes comprehensive benchmarking tools to:
- Measure baseline performance metrics
- Detect performance regressions
- Track performance trends over time
- Ensure optimizations don't degrade performance

## Benchmark Suite

### Available Benchmarks

1. **WAL Benchmark** (`benchmark_wal`)
   - Single write latency with fsync
   - Batch write throughput
   - WAL rotation overhead
   - Read performance

2. **Sections Benchmark** (`benchmark_sections`)
   - Fragment allocation overhead
   - Section write/read latency
   - Metadata save overhead
   - Checkout/checkin latency under load

3. **Database Benchmark** (`benchmark_database`)
   - End-to-end Put/Get/Delete latency
   - Concurrent writers (8 threads)
   - Concurrent readers (8 threads)
   - Mixed workload (70% read, 20% write, 10% delete)

## Running Benchmarks

### Prerequisites

Build with benchmark support:
```bash
mkdir build && cd build
cmake .. -DBUILD_BENCHMARKS=ON
make benchmark
```

### Manual Execution

Run individual benchmarks:
```bash
./build/benchmark_wal
./build/benchmark_sections
./build/benchmark_database
```

### Automated Regression Detection

Run all benchmarks with regression detection:
```bash
./scripts/run_benchmarks.sh
```

This script will:
1. Run all benchmarks
2. Compare results against baselines
3. Fail if performance degrades >10%
4. Generate JSON reports in `.benchmarks/`

## Baseline Management

### Establishing Baselines

First run creates baselines:
```bash
./scripts/run_benchmarks.sh
# Accept current results as baselines
cp .benchmarks/*_current.json .benchmarks/*_baseline.json
```

### Updating Baselines

After intentional performance improvements:
```bash
# Run benchmarks
./scripts/run_benchmarks.sh

# Review results
cat .benchmarks/wal_current.json

# Accept new baselines
cp .benchmarks/*_current.json .benchmarks/*_baseline.json
```

### Baseline Files

- `.benchmarks/wal_baseline.json` - WAL performance baseline
- `.benchmarks/sections_baseline.json` - Sections performance baseline
- `.benchmarks/database_baseline.json` - Database performance baseline

## CI/CD Integration

### GitHub Actions

Performance checks run automatically on:
- Pull requests to `main` or `develop`
- Pushes to `main`

See `.github/workflows/performance.yml` for configuration.

### Workflow

1. Build project with benchmarks enabled
2. Run unit tests
3. Run performance benchmarks
4. Upload results as artifacts
5. Check for regressions (>10% degradation)
6. Report status

### Nightly Reports

Generate performance trend reports:
```bash
python3 scripts/generate_perf_report.py .benchmarks/
```

This creates `.benchmarks/performance_report.json` with:
- Current vs baseline metrics
- Performance trends
- Change percentages
- Regression warnings

## Performance Metrics

### Key Metrics Tracked

- **Throughput**: Operations per second (ops/sec)
- **Latency**: Average, P50, P95, P99 (nanoseconds)
- **Resource Usage**: CPU, memory, I/O
- **Concurrency**: Lock contention, thread scaling

### Expected Baselines

Based on Phase 3-4 optimizations:

| Benchmark | Metric | Baseline | Target |
|-----------|--------|----------|--------|
| WAL Write | Throughput | 100-1,000 ops/sec | 10,000-100,000 ops/sec (debounced) |
| Transaction ID | Gen Time | 100-500 ns | <50 ns (thread-local) |
| Section Write | Syscalls | 3 per write | 1 per write (writev) |
| Fragment Scan | Complexity | O(n) | O(log n) |
| Checkout Lock | Contention | Global lock | 16-way sharded |

## Interpreting Results

### Performance Report

```json
{
  "generated_at": "2026-03-16T18:00:00",
  "benchmarks": {
    "WAL": {
      "current": {
        "ops_per_sec": 45000,
        "avg_latency_ns": 22000
      },
      "baseline": {
        "ops_per_sec": 50000,
        "avg_latency_ns": 20000
      },
      "change_from_baseline_pct": -10.0,
      "trend": "stable (-10.0%)"
    }
  }
}
```

### Status Codes

- **PASS**: Performance within threshold (<10% degradation)
- **FAIL**: Performance regression detected (>10% degradation)
- **IMPROVED**: Performance improved from baseline

## Stress Testing

Stress tests validate optimizations under extreme load:

### Running Stress Tests

```bash
mkdir build && cd build
cmake .. -DBUILD_STRESS_TESTS=ON
make test_concurrent_sections test_resource_limits test_long_running

# Run stress tests
./test_concurrent_sections
./test_resource_limits
./test_long_running
```

### Stress Test Categories

1. **Concurrent Access** (`test_concurrent_sections`)
   - 8 threads, 10K operations each
   - Lock contention measurement
   - Read/write mix testing

2. **Resource Limits** (`test_resource_limits`)
   - Maximum open sections
   - WAL rotation under load
   - Memory pressure

3. **Long-Running** (`test_long_running`)
   - 100K+ sustained operations
   - Performance degradation detection
   - Memory leak checks

## Troubleshooting

### Common Issues

**Benchmark fails to run:**
```bash
# Ensure benchmarks are built
cmake .. -DBUILD_BENCHMARKS=ON
make benchmark
```

**No baseline found:**
```bash
# First run creates baseline
./scripts/run_benchmarks.sh
cp .benchmarks/*_current.json .benchmarks/*_baseline.json
```

**Performance regression detected:**
```bash
# Review current results
cat .benchmarks/*_current.json

# If regression is intentional, update baseline
cp .benchmarks/*_current.json .benchmarks/*_baseline.json

# If unexpected, investigate root cause
git diff HEAD~1 src/
```

**Lock initialization errors:**
- Check platform-specific threading code
- Verify `transaction_id_init()` called before tests
- Ensure timing wheel properly initialized

## Best Practices

### Committing Changes

1. Run benchmarks before committing:
   ```bash
   ./scripts/run_benchmarks.sh
   ```

2. If performance improved:
   ```bash
   # Update baselines
   cp .benchmarks/*_current.json .benchmarks/*_baseline.json
   git add .benchmarks/*_baseline.json
   ```

3. If performance degraded unexpectedly:
   - Investigate before committing
   - Consider reverting problematic changes

### Code Review

When reviewing performance-related changes:

1. Check benchmark results in CI
2. Review stress test output
3. Verify no new lock contention
4. Confirm memory usage stable

### Baseline Maintenance

- Update baselines after intentional optimizations
- Include performance metrics in commit messages
- Document optimization rationale in PR descriptions

## Advanced Usage

### Custom Benchmarks

Add new benchmarks in `tests/benchmark/`:
```cpp
#include "../benchmark_base.h"

void my_benchmark() {
    benchmark_timer_t timer;
    benchmark_start(&timer);

    // Your code here

    uint64_t elapsed_ns = benchmark_end(&timer);

    benchmark_metrics_t metrics;
    metrics.operations_count = 1;
    metrics.total_time_ns = elapsed_ns;
    // ... set other metrics

    benchmark_print("MyBenchmark", &metrics);
}
```

### Performance Profiling

Use Linux perf tools:
```bash
perf record -g ./build/benchmark_wal
perf report
```

### Memory Profiling

Use Valgrind:
```bash
valgrind --leak-check=full ./build/test_long_running
```

## References

- [Performance Optimization Plan](../plans/performance_optimization.md)
- [Stress Testing Guide](./docs/stress_testing.md)
- [CI/CD Integration](./docs/ci_cd.md)