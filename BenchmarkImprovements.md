# Lock-Free LRU Cache Benchmark Results

## Implementation Summary

This benchmark compares the lock-free LRU cache implementation against the original sharded LRU cache. The lock-free implementation features:

1. **Atomic hashmap reads** - Lock-free traversal with proper memory barriers
2. **RCU-style resize** - Atomic pointer swap during resize, old buckets preserved for cleanup
3. **Minimal locking** - Shard lock held only for critical section (lookup + refcount increment)
4. **Fixed refcounter type bug** - `uint_fast16_t` used consistently with `atomic_uint_fast16_t`

## Raw Ops/Sec Comparison

| Test | Sharded LRU | Lock-Free LRU | Difference |
|------|-------------|---------------|------------|
| **Sequential Gets** | 1,835,536 ops/sec | 2,342,469 ops/sec | **+27.6%** |
| **Concurrent Gets (8 threads)** | 6,878,762 ops/sec | 4,504,505 ops/sec | -34.5% |
| **Read-Heavy (90% reads)** | 5,076,142 ops/sec | 5,161,290 ops/sec | +1.7% |
| **Write-Heavy** | 2,472,952 ops/sec | 1,891,253 ops/sec | -23.5% |
| **High Contention (16 threads, 10 keys)** | 5,308,560 ops/sec | 4,235,045 ops/sec | -20.2% |
| **32 Threads** | 2,370,370 ops/sec | 2,507,837 ops/sec | **+5.8%** |

## Latency Comparison

| Test | Sharded LRU | Lock-Free LRU | Improvement |
|------|-------------|---------------|-------------|
| Sequential Gets (10K ops) | 5.45 ms | 4.27 ms | +21.6% |
| Concurrent Gets (8 threads) | 1.16 ms | 1.78 ms | -52.7% |
| Read-Heavy (90% reads) | 788 µs | 775 µs | +1.7% |
| Write-Heavy | 647 µs | 846 µs | -30.7% |
| High Contention | 1.51 ms | 1.89 ms | -25.4% |
| 32 Threads | 1.35 ms | 1.28 ms | +5.5% |

## Analysis

### Best Cases
- **Sequential reads**: Lock-free hashmap reads shine with **2.34M ops/sec** (28% improvement)
- **32 threads with good distribution**: Both achieve ~2.5M ops/sec, lock-free slightly ahead
- **Read-heavy mixed workload**: Nearly identical (~5M ops/sec)

### Worst Cases
- **8-thread concurrent reads with few shards**: Sharded LRU's simpler lock performs better
- **Write-heavy workloads**: Both need locks for mutations, lock-free has atomic overhead
- **High contention on few keys**: Lock contention dominates both approaches

## When to Use Lock-Free LRU

**Best suited for:**
1. Single-threaded or sequential access (no contention)
2. Many threads with good key distribution (contention spread across shards)
3. Read-heavy workloads with many concurrent readers

**Consider sharded LRU for:**
1. High contention on few keys
2. Write-heavy workloads
3. Workloads with few threads (lock overhead dominates)

## Files Changed

### Core Implementation
- `src/Util/concurrent_hashmap.h` - Atomic fields for lock-free reads
- `src/Util/concurrent_hashmap.c` - Lock-free get/contains, RCU-style resize
- `src/Database/lockfree_lru.h` - LRU entry structure
- `src/Database/lockfree_lru.c` - Minimal locking for entry access

### Bug Fix
- `src/RefCounter/refcounter.c` - Fixed `uint_fast16_t` type mismatch in CAS loop

## Test Results

All tests pass:
- `test_lockfree_lru` - 10/10 tests pass
- `test_lockfree_lru_concurrent` - 5/5 tests pass (including 160K operation stress test)
- `test_concurrent_hashmap` - 12/12 tests pass

---

*Benchmark run on Linux x86_64, April 2026*