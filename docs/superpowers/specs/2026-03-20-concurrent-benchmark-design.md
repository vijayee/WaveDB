# Concurrent Database Benchmark Design

**Date:** 2026-03-20
**Goal:** Fix memory leaks in database benchmarks and implement concurrent throughput testing

## Problem Statement

### Memory Leaks
AddressSanitizer detected memory leaks in database benchmarks:
- 7,488 bytes leaked from `version_entry_create` objects during `hbtrie_delete_mvcc` operations
- Identifiers and buffers created in `populate_database` not properly cleaned up
- Total: 28,704 bytes leaked in 1,248 allocations

### Missing Concurrent Testing
Current benchmarks only test sequential operations, one at a time with synchronous waits. This doesn't measure:
- True concurrent throughput under multi-threaded load
- Lock contention and scaling limitations
- Performance degradation as thread count increases

## Solution Design

### Part 1: Fix Memory Leaks

#### Fix 1: hbtrie_delete_mvcc cleanup

**Location:** `src/HBTrie/hbtrie.c:1246` in `hbtrie_delete_mvcc`

**Issue:** `version_entry_t` objects are created via `version_entry_create()` but not properly freed when deletion completes.

**Fix:**
- Audit version entry lifecycle in `hbtrie_delete_mvcc`
- Ensure all created version entries are either:
  - Dereferenced via `refcounter_dereference()` and destroyed when count reaches 0
  - Or properly freed directly if owned by deletion operation
- Add cleanup for version chain during deletion operation

**Code Pattern:**
```c
// After deletion logic completes:
version_entry_t* entry = /* ... created during delete ... */;
if (entry) {
    refcounter_dereference((refcounter_t*)entry);
    if (refcounter_count((refcounter_t*)entry) == 0) {
        version_entry_destroy(entry);
    }
}
```

#### Fix 2: Benchmark cleanup

**Location:** `tests/benchmark/benchmark_database.cpp`

**Issue:** Identifiers created in `populate_database` are passed to `database_put` but ownership transfer isn't clear, leading to leaks.

**Fix:**
- After `database_put` completes, the database owns the identifier
- `populate_database` should not destroy identifiers after calling `database_put`
- However, test should verify database cleanup happens correctly
- Or use stack-allocated buffers instead of heap identifiers

**Alternative:** Create identifiers on stack for simple keys:
```cpp
// Instead of:
identifier_t* id = identifier_create(buf, 0);
database_put(db, path, id, prom);
// identifier ownership transferred to database

// Use database helper that creates identifier internally
// (if such helper exists)
```

### Part 2: Concurrent Benchmark Framework

#### Architecture

**Context Structure:**
```cpp
typedef struct {
    database_t* db;
    std::atomic<uint64_t>* total_ops;
    std::atomic<uint64_t>* total_errors;
    std::atomic<uint64_t>* total_latency_ns;
    int thread_id;
    int ops_per_thread;
    int key_offset;      // Each thread gets unique key range
    operation_type op_type;
} concurrent_bench_ctx_t;
```

**Thread Pattern:**
```cpp
// For each thread count: 1, 2, 4, 8, 16
for (int thread_count : {1, 2, 4, 8, 16}) {
    std::vector<std::thread> threads;
    std::atomic<uint64_t> total_ops{0};
    std::atomic<uint64_t> total_errors{0};
    std::atomic<uint64_t> total_latency_ns{0};

    auto start = std::chrono::high_resolution_clock::now();

    for (int t = 0; t < thread_count; t++) {
        threads.emplace_back([&, t]() {
            // Worker function
            for (int i = 0; i < ops_per_thread; i++) {
                // Perform operation
                // Update atomics
            }
        });
    }

    // Join and measure
    for (auto& thread : threads) {
        thread.join();
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    double ops_per_sec = (double)total_ops.load() / (duration.count() / 1000.0);
    double scaling_factor = ops_per_sec / baseline_ops_per_sec;

    printf("  %2d threads: %'.0f ops/sec (%.2fx)\n",
           thread_count, ops_per_sec, scaling_factor);
}
```

#### Benchmark Scenarios

**1. Concurrent Write Throughput**
- **Purpose:** Measure write scalability without conflicts
- **Setup:** Each thread writes to unique key range (thread_id * offset + i)
- **Workload:** 100% writes
- **Precondition:** Empty database
- **Metrics:** ops/sec at each thread count, scaling factor

**2. Concurrent Read Throughput**
- **Purpose:** Measure read scalability and cache contention
- **Setup:** Pre-populate database with N keys
- **Workload:** 100% reads from shared dataset
- **Precondition:** Database populated with test data
- **Metrics:** ops/sec at each thread count, cache hit rate impact

**3. Concurrent Mixed Workload**
- **Purpose:** Realistic production workload simulation
- **Setup:** Pre-populate database, then mixed operations
- **Workload:** 70% read, 20% write, 10% delete
- **Precondition:** Database populated with test data
- **Metrics:** ops/sec, operation-type breakdown, error rate

#### Implementation Details

**Key Assignment Strategy:**
- Each thread gets unique key namespace: `key_<thread_id>_<op_index>`
- Prevents write-write conflicts
- Allows accurate measurement without contention artifacts

**Atomic Counters:**
- Use `std::atomic<uint64_t>` for lock-free aggregation
- No mutex overhead in critical path
- Accurate total operation counting

**Timing:**
- Use `std::chrono::high_resolution_clock::now()` for wall-clock timing
- Measure total duration across all threads
- Calculate ops/sec = total_ops / duration_seconds

**Database Context:**
- Single database instance shared across threads
- Work pool and timing wheel created once
- Tests real concurrent access patterns

#### Metrics Output

**Format:**
```
Concurrent Write Throughput:
  1 thread:  10,234 ops/sec (baseline)
  2 threads: 19,876 ops/sec (1.94x)
  4 threads: 38,456 ops/sec (3.76x)
  8 threads: 71,234 ops/sec (6.96x)
  16 threads: 112,456 ops/sec (10.99x)

Concurrent Read Throughput:
  1 thread:  45,678 ops/sec (baseline)
  2 threads: 89,123 ops/sec (1.95x)
  4 threads: 171,234 ops/sec (3.75x)
  8 threads: 321,456 ops/sec (7.04x)
  16 threads: 512,345 ops/sec (11.22x)

Concurrent Mixed Workload (70/20/10):
  1 thread:  8,456 ops/sec (baseline)
  2 threads: 15,234 ops/sec (1.80x)
  4 threads: 28,567 ops/sec (3.38x)
  8 threads: 49,123 ops/sec (5.81x)
  16 threads: 71,234 ops/sec (8.42x)
```

**Analysis:**
- Linear scaling = 2x for 2 threads, 4x for 4 threads, etc.
- Sub-linear scaling indicates contention
- Identify optimal thread count for workload type

### Part 3: File Changes

**Modified Files:**

1. **`src/HBTrie/hbtrie.c`**
   - Fix `hbtrie_delete_mvcc` to free version_entry objects
   - Add proper cleanup for version chain

2. **`tests/benchmark/benchmark_database.cpp`**
   - Fix `populate_database` identifier cleanup
   - Add `benchmark_concurrent_writes()`
   - Add `benchmark_concurrent_reads()`
   - Add `benchmark_concurrent_mixed()`
   - Add thread worker functions
   - Update `run_database_benchmarks()` to call concurrent tests

**New Structures:**
```cpp
// In tests/benchmark/benchmark_database.cpp

typedef enum {
    OP_PUT,
    OP_GET,
    OP_DELETE,
    OP_MIXED
} operation_type;

typedef struct {
    database_t* db;
    std::atomic<uint64_t>* total_ops;
    std::atomic<uint64_t>* total_errors;
    std::atomic<uint64_t>* total_latency_ns;
    int thread_id;
    int ops_per_thread;
    int key_offset;
    operation_type op_type;
} concurrent_bench_ctx_t;
```

### Part 4: Testing Strategy

#### Memory Leak Verification

```bash
# Build with AddressSanitizer
cmake -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_C_FLAGS="-fsanitize=address -g" \
      -DCMAKE_CXX_FLAGS="-fsanitize=address -g" \
      -DBUILD_BENCHMARKS=ON \
      -DBUILD_TESTS=ON ..

cmake --build . --target benchmark_database -j$(nproc)

# Run and verify no leaks
./benchmark_database
# ASan should report 0 leaks
```

#### Correctness Testing

- All concurrent operations should complete without errors
- Each thread should complete exactly `ops_per_thread` operations
- Total operations across all threads should equal thread_count * ops_per_thread
- All data written should be readable after concurrent writes

#### Performance Validation

- 1-thread baseline should match sequential benchmark performance
- Multi-thread should show scaling (ideal: linear, real: sub-linear due to contention)
- Identify thread count where scaling plateaus or degrades
- Compare scaling across different workload types

#### Stress Testing

```bash
# Run multiple times in same process
for i in {1..5}; do
    ./benchmark_database
done
# Should complete without threading errors
```

#### Integration Testing

- Run existing unit tests to ensure fixes don't break functionality
- Run sequential benchmarks to verify they still work
- Verify concurrent benchmarks don't interfere with sequential ones

## Success Criteria

1. **Memory leaks fixed:** AddressSanitizer reports 0 leaks
2. **Concurrent benchmarks implemented:** 3 scenarios (write, read, mixed)
3. **Scaling measured:** Results for 1, 2, 4, 8, 16 threads
4. **Correctness verified:** All operations complete, no errors
5. **Documentation:** Benchmark output clearly shows scaling factors

## Implementation Notes

**Thread Safety:**
- Database operations must be thread-safe
- Use atomic counters for aggregation (no mutex overhead)
- Each thread gets unique key namespace to avoid conflicts

**Key Patterns:**
- Follow `test_concurrent_sections.cpp` for thread spawning pattern
- Use `std::atomic<uint64_t>` for counters
- Use `std::chrono::high_resolution_clock` for timing

**Performance Considerations:**
- Warm up database before read tests
- Use reasonable operation counts (1000+ per thread)
- Avoid measuring thread creation overhead (start timing after spawn)

**Future Enhancements:**
- Add latency percentiles (p50, p95, p99) for concurrent operations
- Measure lock contention directly (if possible)
- Add benchmark for different key sizes
- Test with different database configurations (cache size, etc.)

## References

- `test_concurrent_sections.cpp` - Thread spawning pattern
- `benchmark_database.cpp` - Existing sequential benchmark structure
- Memory issue discussion: work_t cleanup and version_entry leaks
- User requirement: Measure ops/sec under different thread counts (1, 2, 4, 8, 16)