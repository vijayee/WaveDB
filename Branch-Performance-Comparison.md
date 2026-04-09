# Branch Performance Comparison

Benchmarks run on 2026-04-08. Each branch tested sequentially on the same machine.

| Branch | Description |
|--------|-------------|
| **master** | Base implementation with sharded checkout locks, debounced WAL fsync |
| **experimental-hbtrie** | Optimistic lock-free reads with version validation in HBTrie |
| **experimental-lru** | Lock-free LRU cache implementation |

> **Note**: The experimental-lru branch's async database benchmark hung during the mixed workload phase and did not complete within 20 minutes. Only synchronous and partial async results are available for that branch.

---

## Synchronous Benchmarks (Single-Threaded, No Work Pool)

### Throughput Comparison (ops/sec)

```
                    Put          Get            Mixed          Delete
master          120,186      2,403,252      2,365,617      293,558
exp-hbtrie      111,553      2,242,694      2,243,364      286,298
exp-lru         115,510      2,213,931      2,298,828      272,009
```

### Sync Throughput Bar Chart (ops/sec, logarithmic scale)

```
Put (Kops/sec)
master      ████████████████████████████████████████████  120K
exp-hbtrie  █████████████████████████████████████████     112K
exp-lru     ██████████████████████████████████████████    116K

Get (Mops/sec)
master      ████████████████████████████████████████      2.40M
exp-hbtrie  ████████████████████████████████████████      2.24M
exp-lru     ████████████████████████████████████████      2.21M

Mixed (Mops/sec)
master      ████████████████████████████████████████      2.37M
exp-hbtrie  ████████████████████████████████████████      2.24M
exp-lru     ████████████████████████████████████████      2.30M

Delete (Kops/sec)
master      ████████████████████████████████████████████  294K
exp-hbtrie  ██████████████████████████████████████████   286K
exp-lru     ██████████████████████████████████████████    272K
```

### Sync Latency Comparison (avg ns)

```
                    Put         Get       Mixed      Delete
master               8,320       416       423       3,407
exp-hbtrie           8,964       446       446       3,493
exp-lru              8,657       452       435       3,676
```

---

## Async Database Benchmarks (Work Pool + Timing Wheel)

### Single-Operation Throughput (ops/sec)

```
                    Put          Get         Batch        Mixed        Delete
master           65,762      105,280       43,348       74,414       59,104
exp-hbtrie       40,724      115,420       58,933       60,118       67,190
exp-lru         65,242      117,899        56,195        —             —
                                                         ^hung         ^hung
```

### Async Single-Op Bar Chart (ops/sec)

```
Put (Kops/sec)
master      ████████████████████████████████████████████   66K
exp-hbtrie  ████████████████████████████████               41K
exp-lru     ████████████████████████████████████████████   65K

Get (Kops/sec)
master      ██████████████████████████████████████████   105K
exp-hbtrie  ██████████████████████████████████████████████ 115K
exp-lru     ██████████████████████████████████████████████ 118K

Mixed (Kops/sec)
master      ████████████████████████████████████████████   74K
exp-hbtrie  ████████████████████████████████████████       60K
exp-lru     ████████████████████ (HUNG)

Delete (Kops/sec)
master      ████████████████████████████████████████████   59K
exp-hbtrie  ████████████████████████████████████████████   67K
exp-lru     ████████████████████ (HUNG)
```

### Async Latency Comparison (avg ns)

```
                    Put         Get       Batch      Mixed      Delete
master              15,206      9,498     23,069     13,438     16,919
exp-hbtrie          24,556      8,664     16,968     16,634     14,883
exp-lru             15,328      8,482     17,796     —           —
```

---

## Concurrent Throughput (ops/sec)

### Concurrent Write

```
Threads     master      exp-hbtrie    exp-lru
-------     ------      ----------    -------
1           48,780      50,814        —
2           93,450      93,245        —
4           87,915      88,697        —
8          165,140     149,456        —
16         244,586     182,194        —
32         383,432     388,728        —
```

### Concurrent Read

```
Threads     master      exp-hbtrie    exp-lru
-------     ------      ----------    -------
1           79,579      79,978        —
2          177,500     150,573        —
4          147,178     156,593        —
8          170,516     218,709        —
16         235,639     259,301        —
32         308,093     301,777        —
```

### Concurrent Mixed (70% read, 20% write, 10% delete)

```
Threads     master      exp-hbtrie    exp-lru
-------     ------      ----------    -------
1           63,465      45,493        —
2          113,032      95,985        —
4          132,830     117,421        —
8          173,479     162,565        —
16         193,116     197,460        —
32         249,420     244,532        —
```

### Concurrent Write Throughput Scaling

```
  400K |                                          ■ master
       |                                    ■     ▲ exp-hbtrie
  300K |                          ■    ■    ■    
       |                    ■    ▲    ▲    ▲    
  200K |              ■    ▲    ■               
       |        ■    ▲                          
  100K |   ■    ■                               
       |   ▲                                     
    0K +--+----+----+----+----+----+----+       
        1    2    4    8   16   32  Threads
```

---

## Summary

### Sync Performance (single-threaded)
All three branches perform similarly in synchronous (single-threaded) benchmarks. The differences are within noise margins (~5-8%). No branch shows a clear advantage in sync mode.

### Async Performance (work pool + timing wheel)
- **master**: Best overall async Put and Mixed throughput. Solid concurrent scaling.
- **exp-hbtrie**: Better Get latency (8.6μs vs 9.5μs master) and Batch Put (59K vs 43K ops/sec), but lower single Put (41K vs 66K) and Mixed (60K vs 74K). Optimistic reads benefit read-heavy workloads.
- **exp-lru**: Comparable async Put/Get to master for single operations, but **hangs during concurrent mixed workload testing**. The lock-free LRU implementation has a severe performance regression or deadlock under concurrent access patterns.

### Key Finding: experimental-lru hangs under concurrent load
The experimental-lru branch's async benchmark hangs during the mixed workload phase (after completing single-operation benchmarks). This indicates a performance regression or potential deadlock in the lock-free LRU cache under concurrent access. The synchronous benchmarks (single-threaded, no work pool) work fine, suggesting the issue is specific to multi-threaded concurrent access patterns.