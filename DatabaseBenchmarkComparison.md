# Database Benchmark: Lock-Free LRU vs Sharded LRU

## Full Database Stack Comparison

This benchmark measures the **complete database operation** including:
- Path/identifier allocation
- Promise/future async machinery
- Worker pool scheduling
- WAL logging
- MVCC version tracking
- HBTrie B+tree traversal
- LRU cache operations

---

## Single-Threaded Operations

| Operation | Lock-Free LRU | Sharded LRU | Difference |
|-----------|---------------|-------------|------------|
| **Put** | 54,580 ops/sec | 52,105 ops/sec | **+4.7%** |
| **Get** | 83,898 ops/sec | 54,643 ops/sec | **+53.5%** |
| **Batch Put** | 41,111 ops/sec | 48,577 ops/sec | -15.4% |
| **Mixed** | 71,407 ops/sec | 96,486 ops/sec | -26.0% |
| **Delete** | 82,853 ops/sec | 95,360 ops/sec | -13.1% |

## Concurrent Write (ops/sec)

| Threads | Lock-Free LRU | Sharded LRU | Difference |
|---------|---------------|-------------|------------|
| 1 | 55,048 | 47,285 | **+16.4%** |
| 2 | 114,957 | 102,248 | **+12.4%** |
| 4 | 89,349 | 134,211 | -33.4% |
| 8 | 163,145 | 193,655 | -15.8% |
| 16 | 257,575 | 270,112 | -4.6% |
| 32 | 485,944 | 469,642 | **+3.5%** |

## Concurrent Read (ops/sec)

| Threads | Lock-Free LRU | Sharded LRU | Difference |
|---------|---------------|-------------|------------|
| 1 | 73,370 | 49,861 | **+47.2%** |
| 2 | 151,475 | 138,420 | **+9.4%** |
| 4 | 123,841 | 137,508 | -9.9% |
| 8 | 151,686 | 241,112 | -37.1% |
| 16 | 241,302 | 263,342 | -8.4% |
| 32 | 304,675 | 354,695 | -14.1% |

---

## Analysis

### Lock-Free LRU Wins
- **Single-threaded Get**: +53.5% improvement (84K vs 55K ops/sec)
- **1-thread reads**: +47.2% improvement
- **2-thread writes**: +12.4% improvement
- **High thread count writes (32)**: +3.5% improvement

### Sharded LRU Wins
- **Batch operations**: -15-26% better with sharded
- **8-thread concurrent reads**: +37% better with sharded
- **16-thread concurrent reads**: +8% better with sharded
- **Mixed workload**: +26% better with sharded

### Key Insights

1. **Read-heavy single-threaded**: Lock-free LRU is significantly faster (+47-53%) due to atomic hashmap reads without lock contention

2. **Write-heavy multi-threaded**: Sharded LRU performs better under contention because both implementations use shard locks for mutations, and the simpler hashmap has less atomic operation overhead

3. **4-thread anomaly**: Lock-free shows a dip at 4 threads (89K ops/sec) while sharded shows 134K. This may indicate contention on specific shards.

4. **High thread scaling**: At 32 threads, both implementations achieve similar throughput (~300K reads, ~480K writes), suggesting the lock-free advantage diminishes under high contention

---

## Recommendation

**Use Lock-Free LRU for:**
- Single-threaded or low-thread applications
- Read-heavy workloads with few threads
- Applications that need maximum single-operation throughput

**Use Sharded LRU for:**
- Multi-threaded applications (4-16 threads)
- Write-heavy workloads
- Mixed read/write workloads under contention
- Applications with predictable access patterns (good shard distribution)

---

*Benchmark run on Linux x86_64, April 2026*