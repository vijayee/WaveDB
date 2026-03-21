# Sharded FIFO Queue Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace O(n) priority queue with O(1) sharded FIFO queue to improve concurrent throughput by 10-50x.

**Architecture:** 16 shards with round-robin distribution and work stealing. Workers dequeue without holding pool lock in fast path, enabling true parallelism. Remove priority system entirely (FIFO sufficient).

**Tech Stack:** C11 atomics, pthread mutexes, thread-local storage

**Spec:** `docs/superpowers/specs/2026-03-20-sharded-fifo-queue-design.md`

---

## File Structure

**Delete:**
- `src/Workers/priority.h` - Priority system (no longer needed)
- `src/Workers/priority.c` - Priority implementation

**Modify:**
- `src/Workers/work.h` - Remove priority field, simplify signature
- `src/Workers/work.c` - Simplify work_create
- `src/Workers/queue.h` - Add sharded structures
- `src/Workers/queue.c` - Implement sharded functions, simplify work_enqueue
- `src/Workers/pool.h` - Use sharded queue
- `src/Workers/pool.c` - Update to sharded queue with correct lock ordering
- `src/Database/database.c` - Update work_create calls (lines 631, 663, 694)
- `src/Time/wheel.c` - Update work_create calls (lines 160, 209)
- `CMakeLists.txt` - Remove priority.c from build

**Test Files:**
- Existing tests in `tests/` should pass unchanged
- No new tests needed (existing test suite validates correctness)

---

## Task 1: Remove Priority System

**Files:**
- Delete: `src/Workers/priority.h`
- Delete: `src/Workers/priority.c`
- Modify: `src/Workers/work.h`
- Modify: `src/Workers/work.c`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Remove priority field from work.h**

Open `src/Workers/work.h` and remove priority-related code:

```c
// REMOVE these lines:
#include "priority.h"

typedef struct {
    refcounter_t refcounter;
    priority_t priority;  // ← REMOVE THIS FIELD
    void (*execute)(void* ctx);
    void (*abort)(void* ctx);
    void* ctx;
} work_t;

// SIMPLIFY TO:
typedef struct {
    refcounter_t refcounter;
    void (*execute)(void* ctx);
    void (*abort)(void* ctx);
    void* ctx;
} work_t;

// UPDATE function signature:
work_t* work_create(void (*execute)(void*), void (*abort)(void*), void* ctx);
```

- [ ] **Step 2: Simplify work_create in work.c**

Open `src/Workers/work.c` and simplify:

```c
// BEFORE:
work_t* work_create(priority_t priority, void* ctx,
                    void (*execute)(void*), void (*abort)(void*)) {
    work_t* work = get_clear_memory(sizeof(work_t));
    work->priority = priority;
    work->execute = execute;
    work->abort = abort;
    work->ctx = ctx;
    refcounter_init((refcounter_t*) work);
    return work;
}

// AFTER:
work_t* work_create(void (*execute)(void*), void (*abort)(void*), void* ctx) {
    work_t* work = get_clear_memory(sizeof(work_t));
    work->execute = execute;
    work->abort = abort;
    work->ctx = ctx;
    refcounter_init((refcounter_t*) work);
    return work;
}
```

- [ ] **Step 3: Remove priority from CMakeLists.txt**

Open `CMakeLists.txt` and find the WAVEDB_SOURCES list. Remove the line:

```cmake
# REMOVE this line from WAVEDB_SOURCES:
    src/Workers/priority.c
```

- [ ] **Step 4: Delete priority files**

```bash
rm src/Workers/priority.h
rm src/Workers/priority.c
```

- [ ] **Step 5: Verify build still works**

```bash
cd build
cmake ..
make -j$(nproc)
```

Expected: Build succeeds without errors (but callers may fail to compile due to signature change)

- [ ] **Step 6: Commit priority system removal**

```bash
git add -A
git commit -m "refactor: remove priority system from work queue

- Remove priority.h and priority.c
- Simplify work_create signature (FIFO sufficient)
- Update CMakeLists.txt

Part of sharded FIFO queue implementation"
```

---

## Task 2: Implement Sharded Queue Structures

**Files:**
- Modify: `src/Workers/queue.h`

- [ ] **Step 1: Add sharded structures to queue.h**

Open `src/Workers/queue.h` and add:

```c
#include <stdatomic.h>

#define QUEUE_SHARDS 16

// Existing FIFO queue (unchanged)
typedef struct {
    work_queue_item_t* first;
    work_queue_item_t* last;
} work_queue_t;

// New sharded queue
typedef struct {
    work_queue_t queues[QUEUE_SHARDS];
    PLATFORMLOCKTYPE(locks[QUEUE_SHARDS]);
    _Atomic uint64_t next_shard;  // Round-robin counter
} sharded_work_queue_t;

// Existing API (unchanged)
void work_queue_init(work_queue_t* queue);
void work_enqueue(work_queue_t* queue, work_t* work);  // Simplified to tail insert
work_t* work_dequeue(work_queue_t* queue);

// New sharded API
void sharded_work_queue_init(sharded_work_queue_t* sq);
void sharded_work_enqueue(sharded_work_queue_t* sq, work_t* work);
work_t* sharded_work_dequeue(sharded_work_queue_t* sq);
void sharded_work_queue_destroy(sharded_work_queue_t* sq);
```

- [ ] **Step 2: Verify header compiles**

```bash
cd build
cmake .. && make -j$(nproc) 2>&1 | head -50
```

Expected: No errors in queue.h (linker errors expected for undefined sharded functions)

- [ ] **Step 3: Commit sharded structures**

```bash
git add src/Workers/queue.h
git commit -m "feat: add sharded work queue structures

- Define QUEUE_SHARDS = 16
- Add sharded_work_queue_t with per-shard locks
- Declare sharded queue API

Part of sharded FIFO queue implementation"
```

---

## Task 3: Implement Sharded Queue Functions

**Files:**
- Modify: `src/Workers/queue.c`

- [ ] **Step 1: Simplify work_enqueue to O(1) tail insertion**

Open `src/Workers/queue.c` and replace the entire `work_enqueue` function:

```c
// BEFORE: O(n) priority insertion with traversal

// AFTER: O(1) tail insertion
void work_enqueue(work_queue_t* queue, work_t* work) {
    work_queue_item_t* item = get_clear_memory(sizeof(work_queue_item_t));
    item->work = work;
    item->next = NULL;
    item->previous = queue->last;

    if (queue->last) {
        queue->last->next = item;
    } else {
        queue->first = item;  // Empty queue
    }
    queue->last = item;
}
```

- [ ] **Step 2: Add sharded_work_queue_init**

```c
void sharded_work_queue_init(sharded_work_queue_t* sq) {
    for (size_t i = 0; i < QUEUE_SHARDS; i++) {
        work_queue_init(&sq->queues[i]);
        platform_lock_init(&sq->locks[i]);
    }
    atomic_init(&sq->next_shard, 0);
}
```

- [ ] **Step 3: Add sharded_work_enqueue with round-robin distribution**

```c
void sharded_work_enqueue(sharded_work_queue_t* sq, work_t* work) {
    size_t shard = atomic_fetch_add(&sq->next_shard, 1) % QUEUE_SHARDS;

    platform_lock(&sq->locks[shard]);
    work_enqueue(&sq->queues[shard], work);
    platform_unlock(&sq->locks[shard]);
}
```

- [ ] **Step 4: Add sharded_work_dequeue with work stealing**

```c
work_t* sharded_work_dequeue(sharded_work_queue_t* sq) {
    // Fair work stealing: thread-local starting offset prevents bias
    static _Thread_local size_t start_shard = 0;
    size_t shard = start_shard;
    start_shard = (start_shard + 1) % QUEUE_SHARDS;  // Rotate for fairness

    for (size_t i = 0; i < QUEUE_SHARDS; i++) {
        size_t idx = (shard + i) % QUEUE_SHARDS;
        platform_lock(&sq->locks[idx]);
        work_t* work = work_dequeue(&sq->queues[idx]);
        platform_unlock(&sq->locks[idx]);

        if (work) return work;
    }
    return NULL;  // All shards empty
}
```

- [ ] **Step 5: Add sharded_work_queue_destroy**

```c
void sharded_work_queue_destroy(sharded_work_queue_t* sq) {
    for (size_t i = 0; i < QUEUE_SHARDS; i++) {
        platform_lock_destroy(&sq->locks[i]);
    }
}
```

- [ ] **Step 6: Verify compilation**

```bash
cd build
cmake .. && make -j$(nproc) 2>&1 | grep -E "error|undefined" | head -20
```

Expected: No errors in queue.c

- [ ] **Step 7: Commit sharded queue implementation**

```bash
git add src/Workers/queue.c
git commit -m "feat: implement sharded FIFO queue

- Simplify work_enqueue to O(1) tail insertion
- Add sharded_work_queue_init with 16 shards
- Add sharded_work_enqueue with round-robin distribution
- Add sharded_work_dequeue with fair work stealing
- Add sharded_work_queue_destroy

Part of sharded FIFO queue implementation"
```

---

## Task 4: Update Work Pool to Use Sharded Queue

**Files:**
- Modify: `src/Workers/pool.h`
- Modify: `src/Workers/pool.c`

- [ ] **Step 1: Update pool.h to use sharded queue**

Open `src/Workers/pool.h` and modify the work_pool_t struct:

```c
// BEFORE:
typedef struct {
    refcounter_t refcounter;
    PLATFORMTHREADTYPE* workers;
    size_t size;
    work_queue_t queue;              // ← REMOVE
    PLATFORMLOCKTYPE(lock);
    // ... rest ...
} work_pool_t;

// AFTER:
typedef struct {
    refcounter_t refcounter;
    PLATFORMTHREADTYPE* workers;
    size_t size;
    sharded_work_queue_t sharded_queue;  // ← ADD
    PLATFORMLOCKTYPE(lock);               // For condition variable only
    PLATFORMCONDITIONTYPE(condition);
    PLATFORMCONDITIONTYPE(shutdown);
    PLATFORMCONDITIONTYPE(idle);
    PLATFORMBARRIERTYPE(barrier);
    int stop;
    size_t idleCount;
} work_pool_t;
```

- [ ] **Step 2: Update work_pool_create to initialize sharded queue**

Open `src/Workers/pool.c` and find `work_pool_create`:

```c
// BEFORE:
work_pool_t* work_pool_create(size_t size) {
    work_pool_t* pool = get_clear_memory(sizeof(work_pool_t));
    // ... existing init ...
    work_queue_init(&pool->queue);  // ← REMOVE
    // ... rest ...
}

// AFTER:
work_pool_t* work_pool_create(size_t size) {
    work_pool_t* pool = get_clear_memory(sizeof(work_pool_t));
    // ... existing init ...
    sharded_work_queue_init(&pool->sharded_queue);  // ← CHANGE
    // ... rest ...
}
```

- [ ] **Step 3: Update work_pool_destroy**

```c
// ADD at the beginning of work_pool_destroy:
void work_pool_destroy(work_pool_t* pool) {
    refcounter_dereference((refcounter_t*) pool);
    if (refcounter_count((refcounter_t*) pool) == 0) {
        sharded_work_queue_destroy(&pool->sharded_queue);  // ← ADD THIS LINE
        platform_lock_destroy(&pool->lock);
        // ... rest ...
    }
}
```

- [ ] **Step 4: Update work_pool_enqueue with correct lock ordering**

```c
// BEFORE:
int work_pool_enqueue(work_pool_t* pool, work_t* work) {
    platform_lock(&pool->lock);
    if (pool->stop) {
        platform_unlock(&pool->lock);
        return 1;
    } else {
        work_enqueue(&pool->queue, work);  // ← REMOVE
        platform_unlock(&pool->lock);
        platform_signal_condition(&pool->condition);
        return 0;
    }
}

// AFTER (CRITICAL: release pool->lock before shard operations):
int work_pool_enqueue(work_pool_t* pool, work_t* work) {
    platform_lock(&pool->lock);
    bool stopped = pool->stop;
    platform_unlock(&pool->lock);

    if (stopped) return 1;

    sharded_work_enqueue(&pool->sharded_queue, work);  // ← CHANGE
    platform_signal_condition(&pool->condition);
    return 0;
}
```

- [ ] **Step 5: Update workerFunction with correct lock ordering**

This is the CRITICAL change to prevent serialization. Find `workerFunction`:

```c
// BEFORE (WRONG - holds pool->lock during dequeue):
void* workerFunction(void* args) {
    work_pool_t* pool = args;
    platform_barrier_wait(&pool->barrier);

    while (true) {
        platform_lock(&pool->lock);  // ← WRONG! Serializes dequeue

        work_t* work = work_dequeue(&pool->queue);  // ← WRONG

        while (!work && !pool->stop) {
            // ... wait logic ...
        }

        platform_unlock(&pool->lock);
        // ... execute ...
    }
}

// AFTER (CORRECT - fast path without pool lock):
void* workerFunction(void* args) {
    work_pool_t* pool = args;
    platform_barrier_wait(&pool->barrier);

    while (true) {
        // Fast path: dequeue WITHOUT pool lock (enables 16-way parallelism)
        work_t* work = sharded_work_dequeue(&pool->sharded_queue);

        // Wait path: only acquire pool lock when queue is empty
        if (!work) {
            platform_lock(&pool->lock);

            // Re-check queue while holding lock
            work = sharded_work_dequeue(&pool->sharded_queue);

            // If still empty, wait for signal
            if (!work && !pool->stop) {
                pool->idleCount++;
                if (pool->idleCount == pool->size) {
                    platform_signal_condition(&pool->idle);
                }
                platform_condition_wait(&pool->lock, &pool->condition);
                pool->idleCount--;

                // Try again after wakeup
                work = sharded_work_dequeue(&pool->sharded_queue);
            }

            bool should_stop = pool->stop;
            platform_unlock(&pool->lock);

            if (!work && should_stop) break;
        }

        if (work) {
            work = (work_t*) refcounter_reference((refcounter_t*) work);
            work->execute(work->ctx);
            work_destroy(work);
        }
    }
    return NULL;
}
```

- [ ] **Step 6: Verify compilation**

```bash
cd build
cmake .. && make -j$(nproc) 2>&1 | head -50
```

Expected: No errors in pool.h or pool.c

- [ ] **Step 7: Commit work pool changes**

```bash
git add src/Workers/pool.h src/Workers/pool.c
git commit -m "feat: update work pool to use sharded FIFO queue

- Replace work_queue_t with sharded_work_queue_t
- Update work_pool_create/destroy for sharded queue
- CRITICAL: Fix lock ordering in work_pool_enqueue
- CRITICAL: Fix lock ordering in workerFunction (fast path without pool lock)
- Enable 16-way parallelism for dequeue operations

Part of sharded FIFO queue implementation"
```

---

## Task 5: Update Database Layer Callers

**Files:**
- Modify: `src/Database/database.c`
- Modify: `src/Time/wheel.c`

- [ ] **Step 1: Find work_create calls in database.c**

```bash
grep -n "work_create" src/Database/database.c
```

Expected output:
```
631:    work_t* work = work_create(priority, ctx, execute, abort);
663:    work_t* work = work_create(priority, ctx, execute, abort);
694:    work_t* work = work_create(priority, ctx, execute, abort);
```

- [ ] **Step 2: Update work_create signature in database.c**

Open `src/Database/database.c` and update all three calls:

```c
// BEFORE (lines 631, 663, 694):
work_t* work = work_create(priority, ctx, execute, abort);

// AFTER (remove priority parameter, reorder: execute, abort, ctx):
work_t* work = work_create(execute, abort, ctx);
```

Note: The parameter order changed: `(execute, abort, ctx)` instead of `(priority, ctx, execute, abort)`

- [ ] **Step 3: Find work_create calls in wheel.c**

```bash
grep -n "work_create" src/Time/wheel.c
```

Expected output:
```
160:    work_t* work = work_create(priority, ctx, execute, abort);
209:    work_t* work = work_create(priority, ctx, execute, abort);
```

- [ ] **Step 4: Update work_create signature in wheel.c**

Open `src/Time/wheel.c` and update both calls:

```c
// BEFORE (lines 160, 209):
work_t* work = work_create(priority, ctx, execute, abort);

// AFTER (remove priority parameter, reorder):
work_t* work = work_create(execute, abort, ctx);
```

- [ ] **Step 5: Verify compilation**

```bash
cd build
cmake .. && make -j$(nproc) 2>&1 | tail -20
```

Expected: Build succeeds, all compilation errors resolved

- [ ] **Step 6: Commit caller updates**

```bash
git add src/Database/database.c src/Time/wheel.c
git commit -m "fix: update work_create callers for simplified signature

- Remove priority parameter from database.c (lines 631, 663, 694)
- Remove priority parameter from wheel.c (lines 160, 209)
- Reorder parameters: (execute, abort, ctx)

Part of sharded FIFO queue implementation"
```

---

## Task 6: Build and Test

**Files:**
- All modified files

- [ ] **Step 1: Clean build from scratch**

```bash
cd build
rm -rf *
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

Expected: Build succeeds without errors

- [ ] **Step 2: Run existing test suite**

```bash
cd build
ctest --output-on-failure
```

Expected: All tests pass (8/8 tests or similar)

- [ ] **Step 3: Run database benchmark (if BUILD_BENCHMARKS enabled)**

```bash
cd build
# If benchmarks not built, rebuild with:
cmake .. -DBUILD_BENCHMARKS=ON
make -j$(nproc)

./benchmark_database
```

Expected: Benchmark runs without errors, shows throughput metrics

- [ ] **Step 4: Verify no priority references remain**

```bash
grep -r "priority" src/Workers/ --include="*.h" --include="*.c"
```

Expected: No matches (priority system fully removed)

- [ ] **Step 5: Commit test verification**

```bash
git add -A
git commit -m "test: verify sharded FIFO queue implementation

- All existing tests pass
- Benchmark runs successfully
- No priority system references remain

Implementation complete"
```

---

## Task 7: Performance Validation

**Files:**
- Benchmark results

- [ ] **Step 1: Run benchmark multiple times**

```bash
cd build
./benchmark_database > ../benchmark_sharded_queue_results.txt 2>&1
```

- [ ] **Step 2: Compare with previous results**

```bash
cat ../benchmark_sharded_queue_results.txt
# Compare with previous benchmark_*.txt files
```

Expected improvements:
- Insert operations: O(n) → O(1) (should be measurably faster)
- Concurrent operations: Better throughput under multi-threaded load
- Write throughput: Should see improvement similar to sharded write locks

- [ ] **Step 3: Document performance results**

Create `BENCHMARK_SHARDED_QUEUE_RESULTS.md`:

```markdown
# Sharded FIFO Queue Performance Results

## Implementation
- Replaced O(n) priority queue with O(1) sharded FIFO queue
- 16 shards with round-robin distribution
- Work stealing with thread-local starting offset
- Fast-path dequeue without pool lock

## Results
[Copy benchmark output]

## Analysis
- Insert time: [Compare before/after]
- Concurrent throughput: [Compare before/after]
- Comparison with baseline: [Improvement percentage]
```

- [ ] **Step 4: Commit performance documentation**

```bash
git add BENCHMARK_SHARDED_QUEUE_RESULTS.md
git commit -m "docs: add sharded FIFO queue performance results

- Document performance improvements
- Compare with baseline measurements"
```

---

## Success Criteria

- [ ] All existing tests pass
- [ ] Build succeeds without errors
- [ ] No priority system references remain
- [ ] Benchmark shows O(1) insert performance
- [ ] Concurrent throughput improved (or at least not degraded)
- [ ] Lock ordering verified: workers dequeue without pool lock in fast path

## Rollback Plan

If critical issues found:

```bash
# Revert all commits
git log --oneline --grep="sharded FIFO queue" | head -10
git revert <commit-hash>

# Or hard reset to before implementation
git reset --hard <commit-before-start>
```