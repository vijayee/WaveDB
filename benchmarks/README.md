# WaveDB Performance Benchmarks

This directory contains performance benchmarks and regression detection tools for WaveDB.

## Quick Start

### Build Benchmarks

```bash
# Configure with benchmarks enabled
cmake .. -DBUILD_BENCHMARKS=ON

# Build all benchmarks
make benchmark_wal benchmark_sections benchmark_database

# Or build all at once
make
```

### Run Benchmarks via CMake (Recommended)

```bash
# Run individual benchmarks
make run_benchmark_wal
make run_benchmark_sections
make run_benchmark_database

# Run all benchmarks at once
make run_benchmarks

# Results are displayed in terminal
```

### Run Benchmarks via Script (Alternative)

```bash
# Run all benchmarks with baseline comparison
./scripts/run_benchmarks.sh
```

```bash
# Run performance regression checks (fails if >10% degradation)
./scripts/run_benchmarks.sh
```

### Generate Performance Report

```bash
# Generate human-readable performance report
python3 scripts/generate_perf_report.py .benchmarks performance_report.json
```

## Benchmark Infrastructure

### Available Benchmarks

1. **benchmark_wal** - WAL write performance ✓
   - Single write latency
   - Batch write throughput
   - WAL rotation overhead
   - Status: **Working**

2. **benchmark_sections** - Section pool performance ✓
   - Fragment allocation/deallocation
   - Concurrent write throughput
   - Lock contention analysis
   - Status: **Working**

3. **benchmark_database** - End-to-end database performance ⚠️
   - Put/Get/Delete latency
   - Concurrent workload throughput
   - Mixed workload performance
   - Status: **Known Issue** - Lock initialization errors when running multiple
     benchmarks in sequence. Individual benchmarks work correctly.
   - **Workaround**: Run benchmarks separately or use `test_database` for validation

### Stress Tests

Located in `tests/stress/`:

- **test_concurrent_sections** - Concurrent access tests (1-16 threads)
- **test_resource_limits** - Resource exhaustion tests
- **test_long_running** - Long-running stability tests

Build with: `cmake .. -DBUILD_STRESS_TESTS=ON`

## Regression Prevention

### How It Works

1. **Baseline Storage** (`.benchmarks/`)
   - `*_baseline.json` - Reference performance metrics
   - `*_current.json` - Latest benchmark results

2. **Regression Detection**
   - Compares current vs baseline metrics
   - Fails if performance degrades >10%
   - Reports improvement/stability

3. **CI/CD Integration**
   - Runs on every pull request
   - Uploads results as artifacts
   - Warns on regressions

### Updating Baselines

When performance improvements are intentional:

```bash
# Accept new baselines
cp .benchmarks/*_current.json .benchmarks/*_baseline.json
git add .benchmarks/*_baseline.json
git commit -m "Update performance baselines"
```

## Performance Targets

### Phase 1-4 Optimizations

| Optimization | Target Improvement | Measured |
|--------------|-------------------|----------|
| WAL fsync batching | 10-100x throughput | Benchmark: `benchmark_wal` |
| Transaction ID lock | 20-50x under concurrency | Stress: `test_concurrent_sections` |
| Write syscall batching | 3-5x for small writes | Benchmark: `benchmark_sections` |
| Fragment scan | 100x for 100 fragments | Benchmark: `benchmark_sections` |
| Metadata debouncing | 99% reduction | Benchmark: `benchmark_sections` |
| Checkout lock sharding | 10-16x concurrency | Stress: `test_concurrent_sections` |

### Key Metrics

- **Write Throughput**: Target 10,000-100,000 ops/sec
- **Write Latency P99**: Target <10ms
- **Read Latency P99**: Target <1ms
- **Concurrent Writers**: Target 100+ without contention

## Continuous Monitoring

### Nightly Reports

A scheduled CI job runs nightly to:

1. Execute all benchmarks
2. Generate performance report
3. Track trends over time
4. Alert on new hotspots

### Manual Analysis

```bash
# View historical performance
python3 scripts/generate_perf_report.py .benchmarks

# Compare specific benchmark
diff .benchmarks/wal_baseline.json .benchmarks/wal_current.json
```

## Troubleshooting

### "No baseline found"

First run establishes baseline:
```bash
./scripts/run_benchmarks.sh
# Baselines created automatically
```

### "Performance degraded"

Check the specific regression:
```bash
# View detailed comparison
cat .benchmarks/wal_current.json
cat .benchmarks/wal_baseline.json
```

### "Benchmark failed to run"

Ensure dependencies are built:
```bash
cmake .. -DBUILD_BENCHMARKS=ON
make benchmark
```

## Contributing

When adding new features:

1. **Add benchmarks** for the feature
2. **Run regression check** before PR
3. **Update baselines** if improvements are intentional
4. **Document performance impact** in PR description

## References

- [Performance Testing Plan](../docs/PERFORMANCE_PLAN.md)
- [Optimization Results](../docs/OPTIMIZATION_RESULTS.md)
- [Stress Test Guide](../tests/stress/README.md)