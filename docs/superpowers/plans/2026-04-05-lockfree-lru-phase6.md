# Integration and Benchmarking Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the existing sharded LRU with the lock-free implementation and validate performance improvements.

**Architecture:** Maintain API compatibility with existing `database_lru`. Use conditional compilation or runtime selection to switch between implementations.

**Tech Stack:** Existing database API, benchmark framework

**Prerequisites:** Phases 1-5 completed

**Spec Reference:** `docs/superpowers/specs/2026-04-05-lockfree-lru-design.md` lines 427-461

---

### Task 1: Create API Compatibility Layer

**Files:**
- Modify: `src/Database/database_lru.h`
- Modify: `src/Database/database_lru.c`

- [ ] **Step 1: Add configuration option**

In `src/Database/database_lru.h`, add compile-time flag:

```c
// Enable lock-free LRU implementation
// Uncomment to use lock-free implementation
// #define USE_LOCKFREE_LRU 1
```

- [ ] **Step 2: Conditionally include lockfree header**

```c
#ifdef USE_LOCKFREE_LRU
#include "lockfree_lru.h"
#endif
```

- [ ] **Step 3: Modify database_lru_cache_t structure**

```c
#ifdef USE_LOCKFREE_LRU
// Lock-free LRU wrapper
typedef struct {
    lockfree_lru_cache_t* impl;
} database_lru_cache_t;
#else
// ... existing sharded LRU structure ...
#endif
```

- [ ] **Step 4: Commit**

```bash
git add src/Database/database_lru.h
git commit -m "feat(lru): add compile-time flag for lock-free implementation"
```

---

### Task 2: Implement Wrapper Functions

**Files:**
- Modify: `src/Database/database_lru.c`

- [ ] **Step 1: Create wrapper for create**

```c
#ifdef USE_LOCKFREE_LRU

database_lru_cache_t* database_lru_cache_create(size_t max_memory_bytes, uint16_t num_shards) {
    database_lru_cache_t* lru = get_clear_memory(sizeof(database_lru_cache_t));
    if (lru == NULL) return NULL;
    
    lru->impl = lockfree_lru_cache_create(max_memory_bytes, num_shards);
    if (lru->impl == NULL) {
        free(lru);
        return NULL;
    }
    
    return lru;
}

#else
// ... existing implementation ...
#endif
```

- [ ] **Step 2: Create wrapper for destroy**

```c
#ifdef USE_LOCKFREE_LRU

void database_lru_cache_destroy(database_lru_cache_t* lru) {
    if (lru == NULL) return;
    
    if (lru->impl != NULL) {
        lockfree_lru_cache_destroy(lru->impl);
    }
    
    free(lru);
}

#else
// ... existing implementation ...
#endif
```

- [ ] **Step 3: Create wrapper for get**

```c
#ifdef USE_LOCKFREE_LRU

identifier_t* database_lru_cache_get(database_lru_cache_t* lru, path_t* path) {
    if (lru == NULL || path == NULL) return NULL;
    return lockfree_lru_cache_get(lru->impl, path);
}

#else
// ... existing implementation ...
#endif
```

- [ ] **Step 4: Create wrapper for put**

```c
#ifdef USE_LOCKFREE_LRU

identifier_t* database_lru_cache_put(database_lru_cache_t* lru, path_t* path, identifier_t* value) {
    if (lru == NULL || path == NULL) {
        if (path != NULL) path_destroy(path);
        if (value != NULL) identifier_destroy(value);
        return NULL;
    }
    return lockfree_lru_cache_put(lru->impl, path, value);
}

#else
// ... existing implementation ...
#endif
```

- [ ] **Step 5: Create wrapper for delete**

```c
#ifdef USE_LOCKFREE_LRU

void database_lru_cache_delete(database_lru_cache_t* lru, path_t* path) {
    if (lru == NULL || path == NULL) return;
    lockfree_lru_cache_delete(lru->impl, path);
}

#else
// ... existing implementation ...
#endif
```

- [ ] **Step 6: Create wrapper for size and memory**

```c
#ifdef USE_LOCKFREE_LRU

size_t database_lru_cache_size(database_lru_cache_t* lru) {
    if (lru == NULL) return 0;
    return lockfree_lru_cache_size(lru->impl);
}

size_t database_lru_cache_memory(database_lru_cache_t* lru) {
    if (lru == NULL) return 0;
    return lockfree_lru_cache_memory(lru->impl);
}

#else
// ... existing implementation ...
#endif
```

- [ ] **Step 7: Commit**

```bash
git add src/Database/database_lru.c
git commit -m "feat(lru): implement wrapper functions for lock-free LRU"
```

---

### Task 3: Create Benchmark Comparison

**Files:**
- Create: `tests/benchmark_lru_comparison.cpp`

- [ ] **Step 1: Create benchmark file**

```cpp
//
// Benchmark comparison: Sharded LRU vs Lock-Free LRU
//

#include <benchmark/benchmark.h>
#include <cstring>
#include <thread>
#include <vector>
extern "C" {
#include "Database/database_lru.h"
#include "HBTrie/path.h"
#include "HBTrie/identifier.h"
#include "Buffer/buffer.h"
}

// Helper to create a simple path
static path_t* make_simple_path(const char* key) {
    path_t* path = path_create();
    buffer_t* buf = buffer_create_from_pointer_copy((uint8_t*)key, strlen(key));
    identifier_t* id = identifier_create(buf, 0);
    buffer_destroy(buf);
    path_append(path, id);
    identifier_destroy(id);
    return path;
}

// Helper to create a simple value
static identifier_t* make_simple_value(const char* data) {
    buffer_t* buf = buffer_create_from_pointer_copy((uint8_t*)data, strlen(data));
    identifier_t* id = identifier_create(buf, 0);
    buffer_destroy(buf);
    return id;
}

// Benchmark: Sequential gets
static void BM_LRUGetSequential(benchmark::State& state) {
    database_lru_cache_t* lru = database_lru_cache_create(10 * 1024 * 1024, 0);
    
    // Pre-populate
    for (int i = 0; i < 1000; i++) {
        char key[32];
        snprintf(key, sizeof(key), "key%d", i);
        path_t* path = make_simple_path(key);
        identifier_t* value = make_simple_value("value");
        database_lru_cache_put(lru, path, value);
    }
    
    for (auto _ : state) {
        for (int i = 0; i < 100; i++) {
            char key[32];
            snprintf(key, sizeof(key), "key%d", i);
            path_t* path = make_simple_path(key);
            identifier_t* cached = database_lru_cache_get(lru, path);
            if (cached) identifier_destroy(cached);
            path_destroy(path);
        }
    }
    
    database_lru_cache_destroy(lru);
}
BENCHMARK(BM_LRUGetSequential);

// Benchmark: Concurrent gets
static void BM_LRUGetConcurrent(benchmark::State& state) {
    database_lru_cache_t* lru = database_lru_cache_create(10 * 1024 * 1024, 0);
    
    // Pre-populate
    for (int i = 0; i < 1000; i++) {
        char key[32];
        snprintf(key, sizeof(key), "key%d", i);
        path_t* path = make_simple_path(key);
        identifier_t* value = make_simple_value("value");
        database_lru_cache_put(lru, path, value);
    }
    
    const int num_threads = state.range(0);
    
    for (auto _ : state) {
        std::vector<std::thread> threads;
        std::atomic<int> count(0);
        
        for (int t = 0; t < num_threads; t++) {
            threads.emplace_back([&]() {
                for (int i = 0; i < 100; i++) {
                    char key[32];
                    snprintf(key, sizeof(key), "key%d", i);
                    path_t* path = make_simple_path(key);
                    identifier_t* cached = database_lru_cache_get(lru, path);
                    if (cached) identifier_destroy(cached);
                    path_destroy(path);
                    count++;
                }
            });
        }
        
        for (auto& thread : threads) {
            thread.join();
        }
    }
    
    database_lru_cache_destroy(lru);
}
BENCHMARK(BM_LRUGetConcurrent)->Arg(2)->Arg(4)->Arg(8)->Arg(16);

// Benchmark: Concurrent put/get mix
static void BM_LRUPutGetConcurrent(benchmark::State& state) {
    database_lru_cache_t* lru = database_lru_cache_create(10 * 1024 * 1024, 0);
    
    const int num_threads = state.range(0);
    
    for (auto _ : state) {
        std::vector<std::thread> threads;
        std::atomic<int> ops(0);
        
        for (int t = 0; t < num_threads; t++) {
            threads.emplace_back([&, t]() {
                for (int i = 0; i < 50; i++) {
                    char key[32];
                    snprintf(key, sizeof(key), "thread%d_key%d", t, i);
                    path_t* path = make_simple_path(key);
                    
                    if (i % 2 == 0) {
                        identifier_t* value = make_simple_value("value");
                        database_lru_cache_put(lru, path, value);
                    } else {
                        identifier_t* cached = database_lru_cache_get(lru, path);
                        if (cached) identifier_destroy(cached);
                        path_destroy(path);
                    }
                    
                    ops++;
                }
            });
        }
        
        for (auto& thread : threads) {
            thread.join();
        }
    }
    
    database_lru_cache_destroy(lru);
}
BENCHMARK(BM_LRUPutGetConcurrent)->Arg(2)->Arg(4)->Arg(8)->Arg(16);

BENCHMARK_MAIN();
```

- [ ] **Step 2: Update tests CMakeLists.txt**

Add benchmark target for LRU comparison.

- [ ] **Step 3: Commit**

```bash
git add tests/benchmark_lru_comparison.cpp tests/CMakeLists.txt
git commit -m "test(lru): add benchmark comparison for sharded vs lock-free"
```

---

### Task 4: Run Baseline Benchmarks (Sharded LRU)

**Files:**
- Run benchmarks without lock-free flag

- [ ] **Step 1: Build with sharded LRU**

```bash
cd build && cmake .. && make benchmark_lru_comparison
```

- [ ] **Step 2: Run baseline benchmarks**

```bash
./tests/benchmark_lru_comparison
```

- [ ] **Step 3: Record baseline results**

Save the output for comparison:

```bash
./tests/benchmark_lru_comparison > baseline_results.txt
```

---

### Task 5: Run Lock-Free Benchmarks

**Files:**
- Run benchmarks with lock-free flag enabled

- [ ] **Step 1: Enable lock-free LRU**

In `src/Database/database_lru.h`, uncomment:

```c
#define USE_LOCKFREE_LRU 1
```

- [ ] **Step 2: Rebuild with lock-free LRU**

```bash
cd build && cmake .. && make clean && make benchmark_lru_comparison
```

- [ ] **Step 3: Run lock-free benchmarks**

```bash
./tests/benchmark_lru_comparison
```

- [ ] **Step 4: Record lock-free results**

```bash
./tests/benchmark_lru_comparison > lockfree_results.txt
```

- [ ] **Step 5: Compare results**

Review both files and verify lock-free shows improvement for read-heavy workloads.

---

### Task 6: Run Existing Database Tests

**Files:**
- Run existing test suite with lock-free LRU

- [ ] **Step 1: Build all tests**

```bash
cd build && cmake .. && make
```

- [ ] **Step 2: Run test suite**

```bash
ctest --output-on-failure
```

- [ ] **Step 3: Verify all tests pass**

All existing database tests should pass with lock-free LRU:

```bash
./tests/test_database
./tests/test_lru_memory
```

---

### Task 7: Run Memory Sanitizers

**Files:**
- Run with valgrind and thread sanitizer

- [ ] **Step 1: Run valgrind check**

```bash
valgrind --leak-check=full --show-leak-kinds=all ./tests/test_lockfree_lru
```

Expected: No memory leaks

- [ ] **Step 2: Run thread sanitizer**

Build with TSAN:

```bash
cd build && cmake -DCMAKE_C_FLAGS="-fsanitize=thread" -DCMAKE_CXX_FLAGS="-fsanitize=thread" .. && make
./tests/test_lockfree_lru
```

Expected: No data races detected

- [ ] **Step 3: Fix any issues found**

If sanitizers report issues, fix them in this step.

---

### Task 8: Create Performance Report

**Files:**
- Create: `docs/superpowers/reports/2026-04-05-lockfree-lru-performance.md`

- [ ] **Step 1: Create report with benchmark comparison**

```markdown
# Lock-Free LRU Performance Report

**Date:** 2026-04-05
**Implementation:** Lock-Free LRU vs Sharded LRU

## Summary

This report compares the performance of the new lock-free LRU implementation
against the existing sharded LRU cache.

## Methodology

- **Hardware:** [CPU info]
- **OS:** [OS info]
- **Compiler:** [Compiler version]
- **Workloads:**
  - Sequential gets (100 keys, single-threaded)
  - Concurrent gets (100 keys, 2/4/8/16 threads)
  - Concurrent put/get mix (50 operations per thread, 2/4/8/16 threads)

## Results

### Sequential Gets

| Implementation | Time (ns/iter) | Improvement |
|---------------|----------------|-------------|
| Sharded LRU   | [baseline]     | -           |
| Lock-Free LRU | [result]       | [X%]       |

### Concurrent Gets (8 threads)

| Implementation | Time (ns/iter) | Improvement |
|---------------|----------------|-------------|
| Sharded LRU   | [baseline]     | -           |
| Lock-Free LRU | [result]       | [X%]       |

## Conclusions

[Analysis and recommendations]
```

- [ ] **Step 2: Commit report**

```bash
git add docs/superpowers/reports/2026-04-05-lockfree-lru-performance.md
git commit -m "docs: add lock-free LRU performance report"
```

---

### Task 9: Clean Up and Final Commit

**Files:**
- All modified files

- [ ] **Step 1: Run final test suite**

```bash
cd build && cmake .. && make && ctest
```

- [ ] **Step 2: Update documentation**

Update any relevant documentation about the LRU cache.

- [ ] **Step 3: Create final commit**

```bash
git add -A
git commit -m "feat(lockfree-lru): integrate lock-free LRU cache

- Replace sharded LRU with lock-free implementation
- Maintain API compatibility with conditional compilation
- Add benchmark comparison
- All tests pass with lock-free LRU enabled
- Performance improvements for read-heavy workloads"
```

---

## Success Criteria for Phase 6

1. All existing database tests pass with lock-free LRU
2. No memory leaks (valgrind clean)
3. No data races (TSAN clean)
4. Benchmark shows improvement for read-heavy workloads
5. API compatibility maintained
6. Performance report documented