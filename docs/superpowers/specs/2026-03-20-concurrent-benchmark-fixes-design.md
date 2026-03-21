# Concurrent Benchmark Implementation & Memory Leak Fixes

**Date:** 2026-03-20
**Status:** Design Approved
**Context:** Fix memory leaks in database benchmarks and implement true concurrent throughput testing

## Problem Statement

The database benchmarks have two critical issues:

1. **Memory Leaks:** AddressSanitizer detected 28KB+ leaks:
   - 7,488 bytes from `version_entry_create` in `hbtrie_delete_mvcc` (104 objects)
   - Identifier and buffer leaks from `populate_database` cleanup issues
   - Prevents proper valgrind/ASan testing

2. **No Concurrent Throughput Testing:** Current benchmarks only measure sequential operations, not true concurrent throughput under load. Need to measure ops/sec at different thread counts (1, 2, 4, 8, 16) to identify scaling bottlenecks and lock contention.

## Goals

- Fix all memory leaks in database benchmarks
- Implement concurrent benchmark framework for database operations
- Measure ops/sec under different thread counts (1, 2, 4, 8, 16)
- Identify lock contention and throughput degradation points
- Compare concurrent vs sequential performance

## Non-Goals

- Not optimizing database implementation (only measuring current performance)
- Not adding new database features
- Not fixing threading issues in existing unit tests

## Design

### 1. Memory Leak Fixes

#### 1.1 version_entry Leak in hbtrie_delete_mvcc

**Location:** `src/HBTrie/hbtrie.c` around line 1246

**Issue:** `version_entry_create` allocates objects that aren't freed during deletion operations.

**Fix:**
- Audit `hbtrie_delete_mvcc` function
- Ensure all version entries in the version chain are properly dereferenced
- Add cleanup for version entries that are no longer needed after deletion
- Verify reference counting is correct (dereference before destruction)

**Verification:**
- Run with AddressSanitizer
- Confirm 0 leaks from `version_entry_create`

#### 1.2 Identifier/Buffer Leaks in Benchmarks

**Location:** `tests/benchmark/benchmark_database.cpp` in `populate_database` function

**Issue:** Identifiers created during population are not cleaned up.

**Fix Options:**
- **Option A (Preferred):** Use stack-allocated buffers instead of heap-allocated identifiers
- **Option B:** Add cleanup loop after population to destroy all created identifiers
- **Option C:** Skip pre-population and use keys that don't need cleanup

**Recommendation:** Option A - use stack buffers and avoid allocation overhead

**Verification:**
- Run with AddressSanitizer
- Confirm 0 indirect leaks from identifier creation

### 2. Concurrent Benchmark Architecture

#### 2.1 Framework Structure

**Context Structure:**
```cpp
typedef struct {
    database_t* db;
    std::atomic<uint64_t>* total_ops;
    std::atomic<uint64_t>* total_errors;
    std::atomic<uint64_t>* total_latency_ns;
    int thread_id;
    int ops_per_thread;
    int key_range_start;  // Each thread gets unique key range
    int key_range_end;
} concurrent_bench_ctx_t;
```

**Thread Spawning Pattern:**
```cpp
for (int thread_count : {1, 2, 4, 8, 16}) {
    std::vector<std::thread> threads;
    std::atomic<uint64_t> total_ops{0};
    std::atomic<uint64_t> total_errors{0};
    std::atomic<uint64_t> total_latency_ns{0};

    auto start = std::chrono::high_resolution_clock::now();

    for (int t = 0; t < thread_count; t++) {
        threads.emplace_back([&, t]() {
            // Perform operations
            // Update atomic counters
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();

    double ops_per_sec = (total_ops.load() * 1e9) / duration_ns;
}
```

#### 2.2 Benchmark Scenarios

**Scenario 1: Concurrent Write Throughput**
- Each thread writes to unique keys (no conflicts)
- Key format: `key_{thread_id}_{operation_id}`
- Measures pure write scalability
- Tests: 1, 2, 4, 8, 16 threads

**Scenario 2: Concurrent Read Throughput**
- Pre-populate database with N keys before test
- Each thread reads from shared dataset
- Measures read scalability and LRU cache contention
- Tests: 1, 2, 4, 8, 16 threads

**Scenario 3: Concurrent Mixed Workload**
- 70% read, 20% write, 10% delete
- Realistic production workload simulation
- Tests interaction between operations
- Identifies cross-operation contention
- Tests: 1, 2, 4, 8, 16 threads

#### 2.3 Metrics Collection

For each thread count, measure:
- **Operations/second** (primary metric)
- **Total operations** completed
- **Error count** (should be 0)
- **Average latency per operation**
- **Throughput scaling factor** vs single-threaded baseline

**Output Format:**
```
Concurrent Write Throughput:
  1 thread:  10,234 ops/sec (baseline)
  2 threads: 19,876 ops/sec (1.94x)
  4 threads: 38,456 ops/sec (3.76x)
  8 threads: 71,234 ops/sec (6.96x)
  16 threads: 112,456 ops/sec (10.99x)
```

### 3. Implementation Details

#### 3.1 File Changes

**src/HBTrie/hbtrie.c:**
- Fix version_entry cleanup in `hbtrie_delete_mvcc`
- Ensure proper dereferencing before destruction

**tests/benchmark/benchmark_database.cpp:**
- Fix identifier cleanup in `populate_database`
- Add concurrent benchmark functions:
  - `benchmark_concurrent_writes()`
  - `benchmark_concurrent_reads()`
  - `benchmark_concurrent_mixed()`
- Add thread worker functions
- Update `run_database_benchmarks()` to include concurrent tests

#### 3.2 Key Design Decisions

**Thread-Safety:**
- Use `std::atomic<uint64_t>` for counters (lock-free, no mutex overhead)
- Each thread operates on separate key ranges to avoid write-write conflicts
- Pre-populate shared key space for reads to test cache contention

**Timing:**
- Use `std::chrono::high_resolution_clock` for nanosecond precision
- Measure wall-clock time across all threads

**Cleanup:**
- Ensure database is properly destroyed between scenarios
- Use unique test directories per run (already implemented)
- Proper teardown sequence: database → timing wheel → worker pool

**Error Handling:**
- Track errors in atomic counter
- Report errors in output
- Continue benchmarking even if errors occur (but report)

#### 3.3 Code Organization

**New Functions:**
```cpp
// Concurrent benchmark entry points
static void run_concurrent_write_benchmark(database_t* db, int thread_count, int ops_per_thread);
static void run_concurrent_read_benchmark(database_t* db, int thread_count, int ops_per_thread);
static void run_concurrent_mixed_benchmark(database_t* db, int thread_count, int ops_per_thread);

// Thread worker functions
static void concurrent_write_worker(concurrent_bench_ctx_t* ctx);
static void concurrent_read_worker(concurrent_bench_ctx_t* ctx);
static void concurrent_mixed_worker(concurrent_bench_ctx_t* ctx);

// Results printing
static void print_concurrent_results(const char* scenario, int thread_count,
                                     uint64_t ops, uint64_t duration_ns);
```

**Integration:**
- Add concurrent benchmarks after existing sequential benchmarks
- Use same database instance (freshly created)
- Clean database between scenarios if needed

### 4. Testing Strategy

#### 4.1 Memory Leak Verification

```bash
# Build with AddressSanitizer
cmake -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_C_FLAGS="-fsanitize=address -g" \
      -DCMAKE_CXX_FLAGS="-fsanitize=address -g" \
      -DBUILD_BENCHMARKS=ON \
      -DBUILD_TESTS=ON ..

make benchmark_database

# Run and check for leaks
./benchmark_database 2>&1 | grep "ERROR: LeakSanitizer"
# Should output nothing (no leaks)
```

#### 4.2 Correctness Testing

- Each concurrent benchmark completes all operations
- Error count is 0
- All data written during tests is readable
- Run existing unit tests to ensure no regressions

#### 4.3 Performance Validation

- 1-thread baseline should match sequential benchmark throughput
- Multi-thread should show scaling (ideally near-linear)
- Contention bottlenecks visible in scaling curve
- Document scaling efficiency at each thread count

#### 4.4 Stress Testing

- Run benchmarks multiple times in same process
- Verify no threading/lock errors
- Ensure proper cleanup between runs
- Test with AddressSanitizer and ThreadSanitizer

### 5. Expected Outcomes

#### 5.1 Memory Fixes

- Zero memory leaks reported by AddressSanitizer
- Clean valgrind output
- Enables future memory debugging

#### 5.2 Concurrent Performance Insights

- Identify optimal thread count for each workload type
- Measure lock contention impact
- Quantify throughput scaling
- Compare concurrent vs sequential performance

#### 5.3 Documentation

- Clear output showing ops/sec at each thread count
- Scaling factors vs baseline
- Bottleneck identification for future optimization

## Implementation Notes

- Follow existing code patterns from `test_concurrent_sections.cpp`
- Use atomic counters for thread-safe aggregation
- Ensure proper cleanup in all code paths
- Keep benchmark code in same file for cohesion
- Add comments explaining concurrent patterns for future maintainers

## Success Criteria

1. All memory leaks fixed (AddressSanitizer reports 0 leaks)
2. Concurrent benchmarks run successfully for all thread counts
3. Ops/sec measured and reported for 1, 2, 4, 8, 16 threads
4. Scaling factors calculated and displayed
5. No threading errors or lock initialization failures
6. All existing tests continue to pass