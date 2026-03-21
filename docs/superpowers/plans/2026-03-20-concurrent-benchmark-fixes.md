# Concurrent Benchmark & Memory Leak Fixes Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fix memory leaks in database benchmarks and implement concurrent throughput testing for 1, 2, 4, 8, 16 threads

**Architecture:** Fix version_entry leaks in hbtrie_delete_mvcc, fix identifier leaks in populate_database, then add concurrent benchmark framework using std::thread and std::atomic counters to measure ops/sec at different thread counts

**Tech Stack:** C/C++, pthreads, std::thread, std::atomic, AddressSanitizer

---

## File Structure

**Files to modify:**
- `src/HBTrie/hbtrie.c` - Fix version_entry cleanup in hbtrie_delete_mvcc
- `tests/benchmark/benchmark_database.cpp` - Fix leaks, add concurrent benchmarks

---

## Task 1: Investigate version_entry Leak

**Files:**
- Read: `src/HBTrie/hbtrie.c`
- Read: `src/HBTrie/bnode.c` (version_entry_create definition)

- [ ] **Step 1: Read hbtrie_delete_mvcc function**

Find the `hbtrie_delete_mvcc` function in `src/HBTrie/hbtrie.c` around line 1246 and read it completely to understand the version_entry allocation pattern.

- [ ] **Step 2: Read version_entry_create definition**

Read `version_entry_create` in `src/HBTrie/bnode.c` to understand how version entries are allocated and what cleanup they need.

- [ ] **Step 3: Identify leak pattern**

Document where version_entry objects are created and where they should be destroyed but aren't. Look for missing `version_entry_destroy` or `refcounter_dereference` calls.

---

## Task 2: Fix version_entry Leak in hbtrie_delete_mvcc

**Files:**
- Modify: `src/HBTrie/hbtrie.c:hbtrie_delete_mvcc`

- [ ] **Step 1: Add version_entry cleanup**

Add proper cleanup for version_entry objects in `hbtrie_delete_mvcc`. Based on the leak pattern identified in Task 1, add the necessary destroy or dereference calls.

- [ ] **Step 2: Verify reference counting**

Ensure version_entry objects are properly dereferenced (not double-freed) by checking refcount before destruction.

- [ ] **Step 3: Build with ASan**

```bash
cd /home/victor/Workspace/src/github.com/vijayee/WaveDB
rm -rf build-asan
mkdir build-asan && cd build-asan
cmake -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_C_FLAGS="-fsanitize=address -g" \
      -DCMAKE_CXX_FLAGS="-fsanitize=address -g" \
      -DBUILD_BENCHMARKS=ON \
      -DBUILD_TESTS=ON ..
make -j$(nproc)
```

- [ ] **Step 4: Run unit tests**

```bash
cd build-asan
./test_database 2>&1 | head -100
```

Verify no version_entry leaks reported. Look for "ERROR: LeakSanitizer" in output.

- [ ] **Step 5: Commit fix**

```bash
git add src/HBTrie/hbtrie.c
git commit -m "fix: cleanup version_entry in hbtrie_delete_mvcc to prevent memory leak"
```

---

## Task 3: Fix Identifier Leaks in populate_database

**Files:**
- Modify: `tests/benchmark/benchmark_database.cpp:populate_database`

- [ ] **Step 1: Read populate_database function**

Read the `populate_database` function in `tests/benchmark/benchmark_database.cpp` around line 166 to understand how identifiers are created.

- [ ] **Step 2: Choose fix approach**

Based on spec recommendation, use stack-allocated buffers instead of heap-allocated identifiers. This avoids allocation overhead and cleanup complexity.

- [ ] **Step 3: Refactor to use stack buffers**

Change the `populate_database` function to not allocate identifiers on the heap. Use temporary stack buffers that are automatically cleaned up.

- [ ] **Step 4: Build and run with ASan**

```bash
cd build-asan
make benchmark_database -j$(nproc)
./benchmark_database 2>&1 | head -50
```

Verify no identifier/buffer leaks. Should see "ERROR: LeakSanitizer: detected memory leaks" count drop significantly.

- [ ] **Step 5: Commit fix**

```bash
git add tests/benchmark/benchmark_database.cpp
git commit -m "fix: use stack buffers in populate_database to prevent identifier leaks"
```

---

## Task 4: Verify Zero Memory Leaks

**Files:**
- Test: Run full ASan verification

- [ ] **Step 1: Run full benchmark suite with ASan**

```bash
cd build-asan
./benchmark_database 2>&1 | grep -A 5 "LeakSanitizer"
```

Expected: No leak summary (or "0 byte(s) leaked in 0 allocation(s)")

- [ ] **Step 2: Run database tests with ASan**

```bash
cd build-asan
./test_database --gtest_filter="*" 2>&1 | tail -20
```

Verify all tests pass and no memory errors.

- [ ] **Step 3: Mark leaks fixed**

Confirm all memory leaks from AddressSanitizer initial run are now fixed:
- ✓ version_entry leaks (7,488 bytes) - fixed
- ✓ identifier/buffer leaks - fixed
- Total leaks should be 0 bytes

---

## Task 5: Add Concurrent Benchmark Context Structure

**Files:**
- Modify: `tests/benchmark/benchmark_database.cpp`

- [ ] **Step 1: Add concurrent benchmark context struct**

Add the following struct definition near the top of `benchmark_database.cpp` (after includes, before existing structs):

```cpp
// Context for concurrent benchmark threads
typedef struct {
    database_t* db;
    std::atomic<uint64_t>* total_ops;
    std::atomic<uint64_t>* total_errors;
    std::atomic<uint64_t>* total_latency_ns;
    int thread_id;
    int ops_per_thread;
    int key_range_start;
    int key_range_end;
    work_pool_t* pool;
    hierarchical_timing_wheel_t* wheel;
} concurrent_bench_ctx_t;
```

- [ ] **Step 2: Build to verify syntax**

```bash
cd build-asan
make benchmark_database -j$(nproc)
```

Should compile without errors.

- [ ] **Step 3: Commit structure**

```bash
git add tests/benchmark/benchmark_database.cpp
git commit -m "feat: add concurrent benchmark context structure"
```

---

## Task 6: Implement Concurrent Write Worker

**Files:**
- Modify: `tests/benchmark/benchmark_database.cpp`

- [ ] **Step 1: Add concurrent_write_worker function**

Add the following worker function after the context struct:

```cpp
// Worker thread function for concurrent write benchmark
static void concurrent_write_worker(concurrent_bench_ctx_t* ctx) {
    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < ctx->ops_per_thread; i++) {
        // Each thread writes to unique keys: key_{thread_id}_{op_id}
        char key[64];
        snprintf(key, sizeof(key), "key_%d_%d", ctx->thread_id, i);

        path_t* path = make_path(key);
        identifier_t* val = make_value("concurrent_value");

        std::promise<void> promise;
        bench_ctx* bctx = (bench_ctx*)malloc(sizeof(bench_ctx));
        bctx->promise = &promise;
        promise_t* prom = promise_create(bench_callback, bench_error_callback, bctx);

        database_put(ctx->db, path, val, prom);
        promise.get_future().get();

        promise_destroy(prom);

        ctx->total_ops->fetch_add(1);
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    ctx->total_latency_ns->fetch_add(duration_ns);
}
```

- [ ] **Step 2: Build to verify compilation**

```bash
cd build-asan
make benchmark_database -j$(nproc)
```

- [ ] **Step 3: Commit worker function**

```bash
git add tests/benchmark/benchmark_database.cpp
git commit -m "feat: add concurrent write worker function"
```

---

## Task 7: Implement Concurrent Write Benchmark

**Files:**
- Modify: `tests/benchmark/benchmark_database.cpp`

- [ ] **Step 1: Add run_concurrent_write_benchmark function**

Add the benchmark entry point function:

```cpp
static void run_concurrent_write_benchmark(database_t* db, work_pool_t* pool,
                                           hierarchical_timing_wheel_t* wheel,
                                           int thread_count, int ops_per_thread) {
    std::vector<std::thread> threads;
    std::atomic<uint64_t> total_ops{0};
    std::atomic<uint64_t> total_errors{0};
    std::atomic<uint64_t> total_latency_ns{0};

    auto start = std::chrono::high_resolution_clock::now();

    for (int t = 0; t < thread_count; t++) {
        threads.emplace_back([&, t]() {
            concurrent_bench_ctx_t ctx;
            ctx.db = db;
            ctx.total_ops = &total_ops;
            ctx.total_errors = &total_errors;
            ctx.total_latency_ns = &total_latency_ns;
            ctx.thread_id = t;
            ctx.ops_per_thread = ops_per_thread;
            ctx.pool = pool;
            ctx.wheel = wheel;

            concurrent_write_worker(&ctx);
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();

    double ops_per_sec = (total_ops.load() * 1e9) / duration_ns;
    double avg_latency_ns = total_latency_ns.load() / (double)total_ops.load();

    printf("Concurrent Write (%d threads):\n", thread_count);
    printf("  Operations: %lu\n", total_ops.load());
    printf("  Ops/sec: %.0f\n", ops_per_sec);
    printf("  Avg latency: %.0f ns\n", avg_latency_ns);
    printf("  Errors: %lu\n", total_errors.load());
    printf("\n");
}
```

- [ ] **Step 2: Build**

```bash
cd build-asan
make benchmark_database -j$(nproc)
```

- [ ] **Step 3: Commit function**

```bash
git add tests/benchmark/benchmark_database.cpp
git commit -m "feat: add concurrent write benchmark runner"
```

---

## Task 8: Implement Concurrent Read Worker

**Files:**
- Modify: `tests/benchmark/benchmark_database.cpp`

- [ ] **Step 1: Add concurrent_read_worker function**

Add after concurrent_write_worker:

```cpp
static void concurrent_read_worker(concurrent_bench_ctx_t* ctx) {
    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < ctx->ops_per_thread; i++) {
        // Read from pre-populated shared key space
        char key[64];
        snprintf(key, sizeof(key), "readkey_%d", i % ctx->key_range_end);

        path_t* path = make_path(key);

        std::promise<void> promise;
        bench_ctx* bctx = (bench_ctx*)malloc(sizeof(bench_ctx));
        bctx->promise = &promise;
        promise_t* prom = promise_create(bench_get_callback, bench_error_callback, bctx);

        database_get(ctx->db, path, prom);
        promise.get_future().get();

        promise_destroy(prom);

        ctx->total_ops->fetch_add(1);
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    ctx->total_latency_ns->fetch_add(duration_ns);
}
```

- [ ] **Step 2: Build**

```bash
cd build-asan
make benchmark_database -j$(nproc)
```

- [ ] **Step 3: Commit**

```bash
git add tests/benchmark/benchmark_database.cpp
git commit -m "feat: add concurrent read worker function"
```

---

## Task 9: Implement Concurrent Read Benchmark

**Files:**
- Modify: `tests/benchmark/benchmark_database.cpp`

- [ ] **Step 1: Add run_concurrent_read_benchmark function**

Add the read benchmark entry point:

```cpp
static void run_concurrent_read_benchmark(database_t* db, work_pool_t* pool,
                                          hierarchical_timing_wheel_t* wheel,
                                          int thread_count, int ops_per_thread,
                                          int prepopulate_count) {
    // Pre-populate database with shared key space
    printf("  Pre-populating %d keys for read benchmark...\n", prepopulate_count);
    for (int i = 0; i < prepopulate_count; i++) {
        char key[64];
        snprintf(key, sizeof(key), "readkey_%d", i);

        path_t* path = make_path(key);
        identifier_t* val = make_value("read_value");

        std::promise<void> promise;
        bench_ctx* bctx = (bench_ctx*)malloc(sizeof(bench_ctx));
        bctx->promise = &promise;
        promise_t* prom = promise_create(bench_callback, bench_error_callback, bctx);

        database_put(db, path, val, prom);
        promise.get_future().get();
        promise_destroy(prom);
    }

    std::vector<std::thread> threads;
    std::atomic<uint64_t> total_ops{0};
    std::atomic<uint64_t> total_errors{0};
    std::atomic<uint64_t> total_latency_ns{0};

    auto start = std::chrono::high_resolution_clock::now();

    for (int t = 0; t < thread_count; t++) {
        threads.emplace_back([&, t]() {
            concurrent_bench_ctx_t ctx;
            ctx.db = db;
            ctx.total_ops = &total_ops;
            ctx.total_errors = &total_errors;
            ctx.total_latency_ns = &total_latency_ns;
            ctx.thread_id = t;
            ctx.ops_per_thread = ops_per_thread;
            ctx.key_range_end = prepopulate_count;
            ctx.pool = pool;
            ctx.wheel = wheel;

            concurrent_read_worker(&ctx);
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();

    double ops_per_sec = (total_ops.load() * 1e9) / duration_ns;
    double avg_latency_ns = total_latency_ns.load() / (double)total_ops.load();

    printf("Concurrent Read (%d threads):\n", thread_count);
    printf("  Operations: %lu\n", total_ops.load());
    printf("  Ops/sec: %.0f\n", ops_per_sec);
    printf("  Avg latency: %.0f ns\n", avg_latency_ns);
    printf("  Errors: %lu\n", total_errors.load());
    printf("\n");
}
```

- [ ] **Step 2: Build**

```bash
cd build-asan
make benchmark_database -j$(nproc)
```

- [ ] **Step 3: Commit**

```bash
git add tests/benchmark/benchmark_database.cpp
git commit -m "feat: add concurrent read benchmark runner"
```

---

## Task 10: Implement Concurrent Mixed Worker

**Files:**
- Modify: `tests/benchmark/benchmark_database.cpp`

- [ ] **Step 1: Add concurrent_mixed_worker function**

Add the mixed workload worker:

```cpp
static void concurrent_mixed_worker(concurrent_bench_ctx_t* ctx) {
    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < ctx->ops_per_thread; i++) {
        int op = rand() % 100;

        if (op < 70) {
            // Read operation (70%)
            char key[64];
            snprintf(key, sizeof(key), "mixedkey_%d", rand() % ctx->key_range_end);

            path_t* path = make_path(key);

            std::promise<void> promise;
            bench_ctx* bctx = (bench_ctx*)malloc(sizeof(bench_ctx));
            bctx->promise = &promise;
            promise_t* prom = promise_create(bench_get_callback, bench_error_callback, bctx);

            database_get(ctx->db, path, prom);
            promise.get_future().get();
            promise_destroy(prom);

            ctx->total_ops->fetch_add(1);
        } else if (op < 90) {
            // Write operation (20%)
            char key[64], val[64];
            snprintf(key, sizeof(key), "mixedkey_%d", rand() % ctx->key_range_end);
            snprintf(val, sizeof(val), "mixed_value_%d_%d", ctx->thread_id, i);

            path_t* path = make_path(key);
            identifier_t* value = make_value(val);

            std::promise<void> promise;
            bench_ctx* bctx = (bench_ctx*)malloc(sizeof(bench_ctx));
            bctx->promise = &promise;
            promise_t* prom = promise_create(bench_callback, bench_error_callback, bctx);

            database_put(ctx->db, path, value, prom);
            promise.get_future().get();
            promise_destroy(prom);

            ctx->total_ops->fetch_add(1);
        } else {
            // Delete operation (10%)
            char key[64];
            snprintf(key, sizeof(key), "mixedkey_%d", rand() % ctx->key_range_end);

            path_t* path = make_path(key);

            std::promise<void> promise;
            bench_ctx* bctx = (bench_ctx*)malloc(sizeof(bench_ctx));
            bctx->promise = &promise;
            promise_t* prom = promise_create(bench_callback, bench_error_callback, bctx);

            database_delete(ctx->db, path, prom);
            promise.get_future().get();
            promise_destroy(prom);

            ctx->total_ops->fetch_add(1);
        }
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    ctx->total_latency_ns->fetch_add(duration_ns);
}
```

- [ ] **Step 2: Build**

```bash
cd build-asan
make benchmark_database -j$(nproc)
```

- [ ] **Step 3: Commit**

```bash
git add tests/benchmark/benchmark_database.cpp
git commit -m "feat: add concurrent mixed workload worker"
```

---

## Task 11: Implement Concurrent Mixed Benchmark

**Files:**
- Modify: `tests/benchmark/benchmark_database.cpp`

- [ ] **Step 1: Add run_concurrent_mixed_benchmark function**

Add the mixed workload benchmark entry point:

```cpp
static void run_concurrent_mixed_benchmark(database_t* db, work_pool_t* pool,
                                           hierarchical_timing_wheel_t* wheel,
                                           int thread_count, int ops_per_thread,
                                           int prepopulate_count) {
    // Pre-populate some keys for mixed workload
    printf("  Pre-populating %d keys for mixed benchmark...\n", prepopulate_count);
    for (int i = 0; i < prepopulate_count; i++) {
        char key[64];
        snprintf(key, sizeof(key), "mixedkey_%d", i);

        path_t* path = make_path(key);
        identifier_t* val = make_value("mixed_initial_value");

        std::promise<void> promise;
        bench_ctx* bctx = (bench_ctx*)malloc(sizeof(bench_ctx));
        bctx->promise = &promise;
        promise_t* prom = promise_create(bench_callback, bench_error_callback, bctx);

        database_put(db, path, val, prom);
        promise.get_future().get();
        promise_destroy(prom);
    }

    std::vector<std::thread> threads;
    std::atomic<uint64_t> total_ops{0};
    std::atomic<uint64_t> total_errors{0};
    std::atomic<uint64_t> total_latency_ns{0};

    auto start = std::chrono::high_resolution_clock::now();

    for (int t = 0; t < thread_count; t++) {
        threads.emplace_back([&, t]() {
            concurrent_bench_ctx_t ctx;
            ctx.db = db;
            ctx.total_ops = &total_ops;
            ctx.total_errors = &total_errors;
            ctx.total_latency_ns = &total_latency_ns;
            ctx.thread_id = t;
            ctx.ops_per_thread = ops_per_thread;
            ctx.key_range_end = prepopulate_count;
            ctx.pool = pool;
            ctx.wheel = wheel;

            concurrent_mixed_worker(&ctx);
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();

    double ops_per_sec = (total_ops.load() * 1e9) / duration_ns;
    double avg_latency_ns = total_latency_ns.load() / (double)total_ops.load();

    printf("Concurrent Mixed (%d threads):\n", thread_count);
    printf("  Operations: %lu\n", total_ops.load());
    printf("  Ops/sec: %.0f\n", ops_per_sec);
    printf("  Avg latency: %.0f ns\n", avg_latency_ns);
    printf("  Errors: %lu\n", total_errors.load());
    printf("\n");
}
```

- [ ] **Step 2: Build**

```bash
cd build-asan
make benchmark_database -j$(nproc)
```

- [ ] **Step 3: Commit**

```bash
git add tests/benchmark/benchmark_database.cpp
git commit -m "feat: add concurrent mixed workload benchmark runner"
```

---

## Task 12: Add Helper Function for Scaling Comparison

**Files:**
- Modify: `tests/benchmark/benchmark_database.cpp`

- [ ] **Step 1: Add print_concurrent_results helper**

Add helper function to calculate and print scaling factors:

```cpp
static void print_concurrent_summary(const char* scenario,
                                     uint64_t* ops_per_sec_per_thread,
                                     int* thread_counts,
                                     int num_configs) {
    printf("========================================\n");
    printf("%s Throughput Summary\n", scenario);
    printf("========================================\n\n");

    double baseline = ops_per_sec_per_thread[0];

    for (int i = 0; i < num_configs; i++) {
        double scaling = ops_per_sec_per_thread[i] / baseline;
        printf("  %2d thread(s): %8.0f ops/sec", thread_counts[i], ops_per_sec_per_thread[i]);

        if (i == 0) {
            printf(" (baseline)\n");
        } else {
            printf(" (%.2fx)\n", scaling);
        }
    }
    printf("\n");
}
```

- [ ] **Step 2: Build**

```bash
cd build-asan
make benchmark_database -j$(nproc)
```

- [ ] **Step 3: Commit**

```bash
git add tests/benchmark/benchmark_database.cpp
git commit -m "feat: add helper to print concurrent benchmark scaling summary"
```

---

## Task 13: Integrate Concurrent Benchmarks into Main Runner

**Files:**
- Modify: `tests/benchmark/benchmark_database.cpp:run_database_benchmarks`

- [ ] **Step 1: Add concurrent benchmark calls to run_database_benchmarks**

In `run_database_benchmarks()` after existing sequential benchmarks, add:

```cpp
    printf("========================================\n");
    printf("Concurrent Throughput Benchmarks\n");
    printf("========================================\n\n");

    // Test thread counts
    int thread_counts[] = {1, 2, 4, 8, 16};
    int num_configs = sizeof(thread_counts) / sizeof(thread_counts[0]);
    int ops_per_thread = 1000;  // Operations per thread
    int prepopulate_count = 10000;  // Keys to pre-populate for read/mixed tests

    // Arrays to store results for summary
    uint64_t write_ops[num_configs];
    uint64_t read_ops[num_configs];
    uint64_t mixed_ops[num_configs];

    // Run concurrent write benchmark for each thread count
    printf("--- Concurrent Write Benchmark ---\n");
    for (int i = 0; i < num_configs; i++) {
        run_concurrent_write_benchmark(ctx.db, ctx.pool, ctx.wheel,
                                       thread_counts[i], ops_per_thread);
        // Store result (extracted from output parsing or passed back)
        // For now, results are printed inline
    }
    printf("\n");

    // Run concurrent read benchmark for each thread count
    printf("--- Concurrent Read Benchmark ---\n");
    for (int i = 0; i < num_configs; i++) {
        run_concurrent_read_benchmark(ctx.db, ctx.pool, ctx.wheel,
                                      thread_counts[i], ops_per_thread,
                                      prepopulate_count);
    }
    printf("\n");

    // Run concurrent mixed benchmark for each thread count
    printf("--- Concurrent Mixed Benchmark ---\n");
    for (int i = 0; i < num_configs; i++) {
        run_concurrent_mixed_benchmark(ctx.db, ctx.pool, ctx.wheel,
                                       thread_counts[i], ops_per_thread,
                                       prepopulate_count);
    }
    printf("\n");
```

- [ ] **Step 2: Build**

```bash
cd build-asan
make benchmark_database -j$(nproc)
```

- [ ] **Step 3: Commit**

```bash
git add tests/benchmark/benchmark_database.cpp
git commit -m "feat: integrate concurrent benchmarks into main runner"
```

---

## Task 14: Test Concurrent Benchmarks with ASan

**Files:**
- Test: Run benchmarks and verify

- [ ] **Step 1: Run benchmark with AddressSanitizer**

```bash
cd build-asan
./benchmark_database 2>&1 | head -200
```

Look for:
- Sequential benchmarks still run
- Concurrent benchmarks execute for 1, 2, 4, 8, 16 threads
- No memory leaks or threading errors
- Ops/sec metrics printed for each thread count

- [ ] **Step 2: Check for memory leaks**

```bash
cd build-asan
./benchmark_database 2>&1 | grep -A 10 "LeakSanitizer"
```

Expected: "0 byte(s) leaked in 0 allocation(s)" or no leak summary

- [ ] **Step 3: Verify output format**

Check that output shows:
- Sequential benchmarks (existing)
- Concurrent write benchmarks (new)
- Concurrent read benchmarks (new)
- Concurrent mixed benchmarks (new)
- Ops/sec for each thread count

---

## Task 15: Test Without ASan for Performance

**Files:**
- Test: Build Release and run

- [ ] **Step 1: Build release version**

```bash
cd /home/victor/Workspace/src/github.com/vijayee/WaveDB
rm -rf build-release
mkdir build-release && cd build-release
cmake -DCMAKE_BUILD_TYPE=Release -DBUILD_BENCHMARKS=ON -DBUILD_TESTS=ON ..
make -j$(nproc)
```

- [ ] **Step 2: Run release benchmark**

```bash
cd build-release
./benchmark_database 2>&1 | tee ../benchmark_results.txt
```

Expected: Clean output with ops/sec metrics for all thread counts

- [ ] **Step 3: Verify scaling**

Check that ops/sec generally increases with thread count (though may plateau due to contention).

---

## Task 16: Run Unit Tests for Regression Check

**Files:**
- Test: Run all tests

- [ ] **Step 1: Run database unit tests**

```bash
cd build-asan
./test_database
```

Verify all tests pass.

- [ ] **Step 2: Run HBTrie tests**

```bash
cd build-asan
./test_hbtrie
```

Verify deletion tests pass (version_entry fix shouldn't break anything).

- [ ] **Step 3: Run full test suite**

```bash
cd build-asan
ctest --output-on-failure
```

All tests should pass.

---

## Task 17: Documentation and Cleanup

**Files:**
- Update: `tests/benchmark/README.md` (if exists) or add comments

- [ ] **Step 1: Add comments explaining concurrent benchmarks**

Add header comment to concurrent benchmark section in `benchmark_database.cpp`:

```cpp
// ========================================
// Concurrent Throughput Benchmarks
// ========================================
//
// These benchmarks measure database ops/sec under concurrent load.
//
// Thread counts: 1, 2, 4, 8, 16
// Scenarios:
//   - Write: Each thread writes to unique keys (no conflicts)
//   - Read: Threads read from shared pre-populated dataset
//   - Mixed: 70% read, 20% write, 10% delete
//
// Metrics collected:
//   - Operations/second for each thread count
//   - Average latency per operation
//   - Scaling factor vs single-threaded baseline
//
// Uses std::atomic for lock-free aggregation across threads.
```

- [ ] **Step 2: Commit documentation**

```bash
git add tests/benchmark/benchmark_database.cpp
git commit -m "docs: add comments explaining concurrent benchmark design"
```

---

## Task 18: Final Verification

**Files:**
- Test: Complete end-to-end verification

- [ ] **Step 1: Clean build with ASan**

```bash
cd /home/victor/Workspace/src/github.com/vijayee/WaveDB
rm -rf build-asan
mkdir build-asan && cd build-asan
cmake -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_C_FLAGS="-fsanitize=address -g" \
      -DCMAKE_CXX_FLAGS="-fsanitize=address -g" \
      -DBUILD_BENCHMARKS=ON \
      -DBUILD_TESTS=ON ..
make -j$(nproc)
```

- [ ] **Step 2: Run full benchmark suite**

```bash
cd build-asan
./benchmark_database 2>&1 | tee ../final_benchmark_run.log
```

- [ ] **Step 3: Verify no memory leaks**

```bash
grep -i "leak" ../final_benchmark_run.log
```

Should show 0 leaks.

- [ ] **Step 4: Verify concurrent benchmarks ran**

```bash
grep "Concurrent" ../final_benchmark_run.log | head -20
```

Should see entries for Write, Read, and Mixed benchmarks at each thread count.

- [ ] **Step 5: Run all tests**

```bash
cd build-asan
ctest --output-on-failure
```

All tests pass.

---

## Task 19: Commit Final Changes

**Files:**
- Commit: All changes

- [ ] **Step 1: Review all changes**

```bash
git status
git diff HEAD
```

Verify all changes are committed.

- [ ] **Step 2: Create summary commit if needed**

If there are uncommitted changes:

```bash
git add -A
git commit -m "feat: complete concurrent benchmark implementation with memory leak fixes"
```

- [ ] **Step 3: Verify git log**

```bash
git log --oneline -15
```

Should show all commits for:
- version_entry leak fix
- identifier leak fix
- concurrent benchmark infrastructure
- worker functions
- benchmark runners
- integration
- documentation

---

## Success Criteria

- [ ] All memory leaks fixed (AddressSanitizer reports 0 leaks)
- [ ] Concurrent benchmarks run successfully for all thread counts (1, 2, 4, 8, 16)
- [ ] Ops/sec measured and reported for each thread count
- [ ] Scaling visible in output (higher thread counts show more ops/sec)
- [ ] No threading errors or lock initialization failures
- [ ] All existing unit tests continue to pass
- [ ] Clean build with ASan and Release modes