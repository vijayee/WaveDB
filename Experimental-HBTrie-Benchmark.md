# Experimental HBTrie Branch Benchmark Results

## Synchronous Database Benchmarks

Starting synchronous database benchmarks...

========================================
Synchronous Database Benchmarks
========================================

Configuration:
  - WAL mode: ASYNC (no fsync)
  - LRU cache: 50 MB
  - WAL file size: 100 MB
  - Single-threaded (no work pool)

Setup complete. Running benchmarks...

Pre-populating database with 10000 entries...
  Pre-populated 0/10000 entries...
  Pre-populated 1000/10000 entries...
  Pre-populated 2000/10000 entries...
  Pre-populated 3000/10000 entries...
  Pre-populated 4000/10000 entries...
  Pre-populated 5000/10000 entries...
  Pre-populated 6000/10000 entries...
  Pre-populated 7000/10000 entries...
  Pre-populated 8000/10000 entries...
  Pre-populated 9000/10000 entries...
Pre-population complete.

--- Synchronous Put Operations ---
=== Sync Put ===
Operations: 10000
Total time: 89.64 ms
Throughput: 111552.98 ops/sec
Latency:
  Min: 5941 ns (5.94 μs)
  Avg: 8964.35 ns (8.96 μs)
  P50: 7894 ns (7.89 μs)
  P95: 15539 ns (15.54 μs)
  P99: 21563 ns (21.56 μs)
  Max: 150428 ns (150.43 μs)


--- Synchronous Get Operations ---
=== Sync Get ===
Operations: 10000
Total time: 4.46 ms
Throughput: 2242694.03 ops/sec
Latency:
  Min: 405 ns (0.41 μs)
  Avg: 445.89 ns (0.45 μs)
  P50: 426 ns (0.43 μs)
  P95: 556 ns (0.56 μs)
  P99: 841 ns (0.84 μs)
  Max: 14094 ns (14.09 μs)


--- Synchronous Mixed Workload (70% read, 20% write, 10% delete) ---
=== Sync Mixed ===
Operations: 10000
Total time: 4.46 ms
Throughput: 2243363.68 ops/sec
Latency:
  Min: 425 ns (0.42 μs)
  Avg: 445.76 ns (0.45 μs)
  P50: 436 ns (0.44 μs)
  P95: 473 ns (0.47 μs)
  P99: 495 ns (0.49 μs)
  Max: 7691 ns (7.69 μs)


--- Synchronous Delete Operations ---
=== Sync Delete ===
Operations: 10000
Total time: 34.93 ms
Throughput: 286297.70 ops/sec
Latency:
  Min: 3260 ns (3.26 μs)
  Avg: 3492.87 ns (3.49 μs)
  P50: 3409 ns (3.41 μs)
  P95: 3821 ns (3.82 μs)
  P99: 5504 ns (5.50 μs)
  Max: 56243 ns (56.24 μs)


========================================
All synchronous benchmarks complete.
========================================

## Async Database Benchmarks

========================================
Database Integration Benchmarks
========================================

NOTE: These benchmarks may fail with lock errors when run
multiple times in the same process. For reliable testing,
use test_database which properly isolates each test.

Setup complete. Running benchmarks...

Pre-populating database with 1000 entries...
Pre-population complete.

--- Single Put Operation ---
=== Database Put (single) ===
Operations: 100
Total time: 2.46 ms
Throughput: 40723.76 ops/sec
Latency:
  Min: 9821 ns (9.82 μs)
  Avg: 24555.69 ns (24.56 μs)
  P50: 19972 ns (19.97 μs)
  P95: 58928 ns (58.93 μs)
  P99: 146058 ns (146.06 μs)
  Max: 146058 ns (146.06 μs)


--- Single Get Operation ---
=== Database Get (single) ===
Operations: 100
Total time: 0.87 ms
Throughput: 115420.26 ops/sec
Latency:
  Min: 5514 ns (5.51 μs)
  Avg: 8663.99 ns (8.66 μs)
  P50: 7771 ns (7.77 μs)
  P95: 15579 ns (15.58 μs)
  P99: 74746 ns (74.75 μs)
  Max: 74746 ns (74.75 μs)


--- Batch Put Operations ---
=== Database Put (batch) ===
Operations: 1000
Total time: 16.97 ms
Throughput: 58933.30 ops/sec
Latency:
  Min: 8710 ns (8.71 μs)
  Avg: 16968.33 ns (16.97 μs)
  P50: 15344 ns (15.34 μs)
  P95: 28942 ns (28.94 μs)
  P99: 50732 ns (50.73 μs)
  Max: 86996 ns (87.00 μs)


--- Mixed Workload (70% read, 20% write, 10% delete) ---
=== Database Mixed ===
Operations: 1000
Total time: 16.63 ms
Throughput: 60118.13 ops/sec
Latency:
  Min: 6583 ns (6.58 μs)
  Avg: 16633.92 ns (16.63 μs)
  P50: 12024 ns (12.02 μs)
  P95: 38720 ns (38.72 μs)
  P99: 68932 ns (68.93 μs)
  Max: 352543 ns (352.54 μs)


--- Single Delete Operation ---
=== Database Delete (single) ===
Operations: 100
Total time: 1.49 ms
Throughput: 67189.85 ops/sec
Latency:
  Min: 8781 ns (8.78 μs)
  Avg: 14883.20 ns (14.88 μs)
  P50: 14670 ns (14.67 μs)
  P95: 20859 ns (20.86 μs)
  P99: 30213 ns (30.21 μs)
  Max: 30213 ns (30.21 μs)


========================================
Concurrent Throughput Benchmarks
========================================

--- Concurrent Write Benchmark ---
Concurrent Write (1 threads):
  Operations: 1000
  Ops/sec: 50814
  Avg latency: 19562 ns
  Errors: 0

Concurrent Write (2 threads):
  Operations: 2000
  Ops/sec: 93245
  Avg latency: 20742 ns
  Errors: 0

Concurrent Write (4 threads):
  Operations: 4000
  Ops/sec: 88697
  Avg latency: 44507 ns
  Errors: 0

Concurrent Write (8 threads):
  Operations: 8000
  Ops/sec: 149456
  Avg latency: 52719 ns
  Errors: 0

Concurrent Write (16 threads):
  Operations: 16000
  Ops/sec: 182194
  Avg latency: 86565 ns
  Errors: 0

Concurrent Write (32 threads):
  Operations: 32000
  Ops/sec: 388728
  Avg latency: 79997 ns
  Errors: 0


--- Concurrent Read Benchmark ---
  Pre-populating 10000 keys for read benchmark...
Concurrent Read (1 threads):
  Operations: 1000
  Ops/sec: 79978
  Avg latency: 12423 ns
  Errors: 0

  Pre-populating 10000 keys for read benchmark...
Concurrent Read (2 threads):
  Operations: 2000
  Ops/sec: 150573
  Avg latency: 13181 ns
  Errors: 0

  Pre-populating 10000 keys for read benchmark...
Concurrent Read (4 threads):
  Operations: 4000
  Ops/sec: 156593
  Avg latency: 24473 ns
  Errors: 0

  Pre-populating 10000 keys for read benchmark...
Concurrent Read (8 threads):
  Operations: 8000
  Ops/sec: 218709
  Avg latency: 36146 ns
  Errors: 0

  Pre-populating 10000 keys for read benchmark...
Concurrent Read (16 threads):
  Operations: 16000
  Ops/sec: 259301
  Avg latency: 60817 ns
  Errors: 0

  Pre-populating 10000 keys for read benchmark...
Concurrent Read (32 threads):
  Operations: 32000
  Ops/sec: 301777
  Avg latency: 104472 ns
  Errors: 0


--- Concurrent Mixed Benchmark ---
  Pre-populating 10000 keys for mixed workload benchmark...
Concurrent Mixed Workload (1 threads):
  Operations: 1000
  Ops/sec: 45493
  Avg latency: 21905 ns
  Errors: 0

  Pre-populating 10000 keys for mixed workload benchmark...
Concurrent Mixed Workload (2 threads):
  Operations: 2000
  Ops/sec: 95985
  Avg latency: 19456 ns
  Errors: 0

  Pre-populating 10000 keys for mixed workload benchmark...
Concurrent Mixed Workload (4 threads):
  Operations: 4000
  Ops/sec: 117421
  Avg latency: 30884 ns
  Errors: 0

  Pre-populating 10000 keys for mixed workload benchmark...
Concurrent Mixed Workload (8 threads):
  Operations: 8000
  Ops/sec: 162565
  Avg latency: 48242 ns
  Errors: 0

  Pre-populating 10000 keys for mixed workload benchmark...
Concurrent Mixed Workload (16 threads):
  Operations: 16000
  Ops/sec: 197460
  Avg latency: 79409 ns
  Errors: 0

  Pre-populating 10000 keys for mixed workload benchmark...
Concurrent Mixed Workload (32 threads):
  Operations: 32000
  Ops/sec: 244532
  Avg latency: 127039 ns
  Errors: 0


========================================
Database Benchmarks Complete
========================================

Performance Summary:
  Put:   40724 ops/sec (avg: 24556 ns)
  Get:   115420 ops/sec (avg: 8664 ns)
  Batch: 58933 ops/sec (avg: 16968 ns)
  Mixed: 60118 ops/sec (avg: 16634 ns)
  Delete: 67190 ops/sec (avg: 14883 ns)

Cache Metrics:
  Max Memory Budget: 50.00 MB
  Current Memory: 16.51 MB (17314800 bytes)
  Entry Count: 50812
  Avg Entry Size: 341 bytes

