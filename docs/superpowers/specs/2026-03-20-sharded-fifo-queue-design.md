# Sharded FIFO Queue Design

**Date:** 2026-03-20
**Status:** Proposed
**Author:** Claude (Superpowers Brainstorming)

## Overview

Replace the current O(n) priority queue with an O(1) sharded FIFO queue to improve concurrent throughput by 10-50x.

## Problem Statement

### Current Architecture

The work pool uses a single sorted linked list for work items:

```c
typedef struct {
    work_queue_item_t* first;
    work_queue_item_t* last;
} work_queue_t;

// Single queue with single lock
work_pool_t {
    work_queue_t queue;              // ONE queue
    PLATFORMLOCKTYPE(lock);          // ONE lock for all operations
}
```

**Performance Issues:**

1. **O(n) insertion** - Priority-based insertion traverses the list comparing priorities
2. **Single lock contention** - All submissions and dequeues compete for one lock
3. **Priority overhead** - Priority comparison and sorting add CPU cycles
4. **Bottleneck under concurrency** - Single queue serializes all work submissions

### Measured Impact

From `OPTIMIZATION_RESULTS.md`:
- Write throughput: 412 ops/sec (limited by work pool queue)
- Read throughput: 79,619 ops/sec (limited by work pool queue and LRU cache)
- Mixed workload: 1,718 ops/sec

**Identified as next bottleneck after sharded write locks (2.6x improvement).**

## Requirements

### Functional Requirements

1. **FIFO ordering** - Work items complete in submission order (no priority needed)
2. **Thread-safe** - Support concurrent submission and dequeue from multiple threads
3. **Fair scheduling** - All work items have equal chance of being processed
4. **Work stealing** - Idle workers can steal from busy workers' queues

### Performance Requirements

1. **O(1) insertion** - No list traversal, constant time enqueue
2. **O(1) dequeue** - Constant time dequeue
3. **Reduced contention** - Multiple shards to reduce lock contention
4. **Linear scaling** - Throughput scales with number of shards and threads

### Non-Functional Requirements

1. **Minimal code changes** - Reuse existing queue structure and work pool logic
2. **Maintainability** - Simple, easy to understand implementation
3. **Testability** - Can verify correctness with existing test suite
4. **No priority needed** - Remove unused priority system entirely

## Design

### Architecture

**Sharded Queue Structure:**

```c
#define QUEUE_SHARDS 16

typedef struct {
    work_queue_item_t* first;
    work_queue_item_t* last;
} work_queue_t;  // Unchanged - simple FIFO

typedef struct {
    work_queue_t queues[QUEUE_SHARDS];        // 16 FIFO queues
    PLATFORMLOCKTYPE(locks[QUEUE_SHARDS]);     // 16 locks
    _Atomic uint64_t next_shard;               // Round-robin counter
} sharded_work_queue_t;
```

**Key Changes:**

1. Remove priority field from `work_t` (no longer needed)
2. Replace single queue with array of 16 FIFO queues
3. Replace single lock with array of 16 locks
4. Round-robin distribution for submissions
5. Work stealing for dequeue
6. Simplify `work_enqueue()` to O(1) tail insertion

### Data Flow

**Submitter Thread:**

```c
void sharded_work_enqueue(sharded_work_queue_t* sq, work_t* work) {
    // Round-robin distribution
    size_t shard = atomic_fetch_add(&sq->next_shard, 1) % QUEUE_SHARDS;

    platform_lock(&sq->locks[shard]);
    work_enqueue_tail(&sq->queues[shard], work);  // O(1)
    platform_unlock(&sq->locks[shard]);
}
```

**Worker Thread:**

```c
work_t* sharded_work_dequeue(sharded_work_queue_t* sq) {
    // Try all shards (work stealing)
    for (size_t i = 0; i < QUEUE_SHARDS; i++) {
        platform_lock(&sq->locks[i]);
        work_t* work = work_dequeue_head(&sq->queues[i]);  // O(1)
        platform_unlock(&sq->locks[i]);

        if (work) return work;
    }
    return NULL;  // All shards empty
}
```

**Integration with Condition Variables:**

The pool lock remains for condition variable signaling:

```c
void* workerFunction(void* args) {
    work_pool_t* pool = args;

    while (true) {
        platform_lock(&pool->lock);  // For condition wait

        work_t* work = sharded_work_dequeue(&pool->sharded_queue);

        if (!work && !pool->stop) {
            pool->idleCount++;
            if (pool->idleCount == pool->size) {
                platform_signal_condition(&pool->idle);
            }
            platform_condition_wait(&pool->lock, &pool->condition);
            pool->idleCount--;
            work = sharded_work_dequeue(&pool->sharded_queue);
        }

        platform_unlock(&pool->lock);

        if (work) {
            work->execute(work->ctx);
            work_destroy(work);
        } else if (pool->stop) {
            break;
        }
    }
    return NULL;
}
```

### Implementation Details

**Files to Modify:**

**1. src/Workers/work.h** - Remove priority field
```c
// Remove:
#include "priority.h"

typedef struct {
    refcounter_t refcounter;
    priority_t priority;  // ← REMOVE
    void (*execute)(void* ctx);
    void (*abort)(void* ctx);
    void* ctx;
} work_t;

// Simplify to:
typedef struct {
    refcounter_t refcounter;
    void (*execute)(void* ctx);
    void (*abort)(void* ctx);
    void* ctx;
} work_t;
```

**2. src/Workers/work.c** - Simplify work creation
```c
work_t* work_create(void (*execute)(void*), void (*abort)(void*), void* ctx) {
    work_t* work = get_clear_memory(sizeof(work_t));
    work->execute = execute;
    work->abort = abort;
    work->ctx = ctx;
    refcounter_init((refcounter_t*) work);
    return work;
}
```

**3. src/Workers/priority.h & priority.c** - Delete entirely

**4. src/Workers/queue.h** - Add sharded structures
```c
#define QUEUE_SHARDS 16

typedef struct {
    work_queue_item_t* first;
    work_queue_item_t* last;
} work_queue_t;

typedef struct {
    work_queue_t queues[QUEUE_SHARDS];
    PLATFORMLOCKTYPE(locks[QUEUE_SHARDS]);
    _Atomic uint64_t next_shard;
} sharded_work_queue_t;

// Existing API
void work_queue_init(work_queue_t* queue);
void work_enqueue(work_queue_t* queue, work_t* work);  // Simplified
work_t* work_dequeue(work_queue_t* queue);

// New sharded API
void sharded_work_queue_init(sharded_work_queue_t* sq);
void sharded_work_enqueue(sharded_work_queue_t* sq, work_t* work);
work_t* sharded_work_dequeue(sharded_work_queue_t* sq);
void sharded_work_queue_destroy(sharded_work_queue_t* sq);
```

**5. src/Workers/queue.c** - Implement sharded functions
```c
// Simplify to O(1) tail insertion
void work_enqueue(work_queue_t* queue, work_t* work) {
    work_queue_item_t* item = get_clear_memory(sizeof(work_queue_item_t));
    item->work = work;
    item->next = NULL;
    item->previous = queue->last;

    if (queue->last) {
        queue->last->next = item;
    } else {
        queue->first = item;
    }
    queue->last = item;
}

void sharded_work_queue_init(sharded_work_queue_t* sq) {
    for (size_t i = 0; i < QUEUE_SHARDS; i++) {
        work_queue_init(&sq->queues[i]);
        platform_lock_init(&sq->locks[i]);
    }
    atomic_init(&sq->next_shard, 0);
}

void sharded_work_enqueue(sharded_work_queue_t* sq, work_t* work) {
    size_t shard = atomic_fetch_add(&sq->next_shard, 1) % QUEUE_SHARDS;

    platform_lock(&sq->locks[shard]);
    work_enqueue(&sq->queues[shard], work);
    platform_unlock(&sq->locks[shard]);
}

work_t* sharded_work_dequeue(sharded_work_queue_t* sq) {
    for (size_t i = 0; i < QUEUE_SHARDS; i++) {
        platform_lock(&sq->locks[i]);
        work_t* work = work_dequeue(&sq->queues[i]);
        platform_unlock(&sq->locks[i]);

        if (work) return work;
    }
    return NULL;
}

void sharded_work_queue_destroy(sharded_work_queue_t* sq) {
    for (size_t i = 0; i < QUEUE_SHARDS; i++) {
        platform_lock_destroy(&sq->locks[i]);
    }
}
```

**6. src/Workers/pool.h** - Use sharded queue
```c
typedef struct {
    refcounter_t refcounter;
    PLATFORMTHREADTYPE* workers;
    size_t size;

    sharded_work_queue_t sharded_queue;  // ← Changed
    PLATFORMLOCKTYPE(lock);               // For condition variable only
    PLATFORMCONDITIONTYPE(condition);
    PLATFORMCONDITIONTYPE(shutdown);
    PLATFORMCONDITIONTYPE(idle);
    PLATFORMBARRIERTYPE(barrier);
    int stop;
    size_t idleCount;
} work_pool_t;
```

**7. src/Workers/pool.c** - Update to use sharded queue
```c
work_pool_t* work_pool_create(size_t size) {
    work_pool_t* pool = get_clear_memory(sizeof(work_pool_t));
    // ... existing init ...
    sharded_work_queue_init(&pool->sharded_queue);
    // ... rest unchanged ...
}

int work_pool_enqueue(work_pool_t* pool, work_t* work) {
    platform_lock(&pool->lock);
    if (pool->stop) {
        platform_unlock(&pool->lock);
        return 1;
    }
    platform_unlock(&pool->lock);

    sharded_work_enqueue(&pool->sharded_queue, work);
    platform_signal_condition(&pool->condition);
    return 0;
}
```

**8. Database Layer** - Remove priority from work creation
- `src/Database/database.c` - All `work_create()` calls remove priority parameter
- Test files - Update work creation calls

**9. CMakeLists.txt** - Remove priority.c from build
```cmake
# Remove from WAVEDB_SOURCES:
# src/Workers/priority.c
```

### Work Distribution Strategy

**Why Round-Robin:**
- No work ID to hash (work items don't have unique IDs)
- Perfect distribution across shards (no hot spots)
- Atomic counter increment is cache-friendly (single cache line)
- Simple and predictable

**Work Stealing Benefits:**
- Load balancing: busy threads help idle threads
- No centralized coordinator needed
- Better cache locality (workers tend to reuse same shard)
- Reduced contention (spread across 16 locks)

### Performance Expectations

**Expected Improvements:**

| Metric | Current | Expected | Improvement |
|--------|---------|----------|-------------|
| **Insert** | O(n) priority traversal | O(1) tail append | **~100-1000x faster** |
| **Dequeue** | O(1) but single lock | O(1) with 16 locks | **~16x less contention** |
| **Concurrent submission** | Serialized by lock | 16-way parallel | **~16x throughput** |

**Combined with Existing Optimizations:**
- Sharded write locks: +157% write throughput
- Atomic transaction ID: +9.4% read throughput
- Sharded FIFO queue: Expected +100-500% all operations

**Total Expected Improvement:** 10-50x concurrent throughput (similar to sharded write locks improvement)

## Testing Strategy

### Unit Tests

1. **Test FIFO ordering** - Submit items A, B, C, verify dequeue order A, B, C
2. **Test concurrent submission** - Multiple threads submit simultaneously
3. **Test work stealing** - Worker threads steal from other shards
4. **Test empty queue** - All shards empty returns NULL
5. **Test shutdown** - Stop signal empties all shards correctly

### Integration Tests

1. **Database operations** - Existing test suite should pass unchanged
2. **Benchmark comparison** - Measure throughput improvement

### Performance Tests

1. **Single-threaded** - Verify O(1) insertion performance
2. **Multi-threaded** - Verify linear scaling with threads
3. **High contention** - Measure throughput under heavy load

## Migration Path

### Phase 1: Remove Priority System
1. Delete `priority.h` and `priority.c`
2. Remove priority field from `work_t`
3. Update all `work_create()` calls
4. Verify tests pass

### Phase 2: Implement Sharded Queue
1. Add `sharded_work_queue_t` structures
2. Implement sharded functions in `queue.c`
3. Update `work_pool_t` to use sharded queue
4. Update worker function to use `sharded_work_dequeue()`
5. Verify tests pass

### Phase 3: Benchmark and Validate
1. Run existing test suite
2. Run performance benchmarks
3. Compare throughput before/after
4. Document results

## Risks and Mitigations

### Risk: Priority System Still Needed
**Likelihood:** Low (user confirmed FIFO sufficient)
**Impact:** High (would require rework)
**Mitigation:** Verify with user that no code paths require priority ordering

### Risk: Work Stealing Starvation
**Likelihood:** Low (work stealing is fair)
**Impact:** Medium (some work delayed)
**Mitigation:** Implement random start offset for dequeue to prevent systematic bias

### Risk: Lock Contention Still High
**Likelihood:** Low (16 shards should be sufficient)
**Impact:** Medium (less improvement than expected)
**Mitigation:** Increase QUEUE_SHARDS to 32 or 64 if needed

### Risk: Memory Overhead
**Likelihood:** Low (16 queues × small overhead)
**Impact:** Low (acceptable for performance gain)
**Mitigation:** Monitor memory usage, adjust shard count if needed

## Success Criteria

1. **All existing tests pass** - No regressions in functionality
2. **O(1) insertion verified** - Benchmark shows constant time
3. **Throughput improvement** - Measurable increase in concurrent operations
4. **Code simplicity** - Cleaner than priority queue implementation
5. **Linear scaling** - Throughput scales with thread count

## Alternatives Considered

### Alternative 1: Lock-Free MPMC Queue
- **Pros:** True FIFO, zero lock overhead
- **Cons:** Complex to implement, fixed capacity, single bottleneck remains
- **Rejected:** Sharding provides better scaling and simplicity

### Alternative 2: Per-Worker Work Stealing Queue
- **Pros:** Best cache locality, minimal contention
- **Cons:** Complex implementation, load balancing challenges
- **Rejected:** Too much architectural change, higher risk

### Alternative 3: Keep Priority Queue but Shard
- **Pros:** Preserves priority ordering
- **Cons:** Priority not needed, more complex sharding
- **Rejected:** User confirmed FIFO is sufficient

## References

- `OPTIMIZATION_RESULTS.md` - Current bottleneck analysis
- `CONCURRENCY_ANALYSIS.md` - Detailed lock contention analysis
- `src/Workers/pool.c` - Current work pool implementation
- `src/Workers/queue.c` - Current priority queue implementation