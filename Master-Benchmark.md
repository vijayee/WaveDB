# Master Branch Benchmark Results

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
Total time: 83.20 ms
Throughput: 120185.63 ops/sec
Latency:
  Min: 5909 ns (5.91 μs)
  Avg: 8320.46 ns (8.32 μs)
  P50: 7640 ns (7.64 μs)
  P95: 13295 ns (13.29 μs)
  P99: 20977 ns (20.98 μs)
  Max: 253051 ns (253.05 μs)


--- Synchronous Get Operations ---
=== Sync Get ===
Operations: 10000
Total time: 4.16 ms
Throughput: 2403252.27 ops/sec
Latency:
  Min: 391 ns (0.39 μs)
  Avg: 416.10 ns (0.42 μs)
  P50: 404 ns (0.40 μs)
  P95: 441 ns (0.44 μs)
  P99: 660 ns (0.66 μs)
  Max: 14700 ns (14.70 μs)


--- Synchronous Mixed Workload (70% read, 20% write, 10% delete) ---
=== Sync Mixed ===
Operations: 10000
Total time: 4.23 ms
Throughput: 2365616.99 ops/sec
Latency:
  Min: 394 ns (0.39 μs)
  Avg: 422.72 ns (0.42 μs)
  P50: 415 ns (0.41 μs)
  P95: 438 ns (0.44 μs)
  P99: 684 ns (0.68 μs)
  Max: 6635 ns (6.63 μs)


--- Synchronous Delete Operations ---
=== Sync Delete ===
Operations: 10000
Total time: 34.06 ms
Throughput: 293558.44 ops/sec
Latency:
  Min: 3046 ns (3.05 μs)
  Avg: 3406.48 ns (3.41 μs)
  P50: 3201 ns (3.20 μs)
  P95: 4330 ns (4.33 μs)
  P99: 6415 ns (6.42 μs)
  Max: 180072 ns (180.07 μs)


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
Total time: 1.52 ms
Throughput: 65762.43 ops/sec
Latency:
  Min: 10343 ns (10.34 μs)
  Avg: 15206.25 ns (15.21 μs)
  P50: 14866 ns (14.87 μs)
  P95: 21358 ns (21.36 μs)
  P99: 73879 ns (73.88 μs)
  Max: 73879 ns (73.88 μs)


--- Single Get Operation ---
=== Database Get (single) ===
Operations: 100
Total time: 0.95 ms
Throughput: 105280.34 ops/sec
Latency:
  Min: 6738 ns (6.74 μs)
  Avg: 9498.45 ns (9.50 μs)
  P50: 9377 ns (9.38 μs)
  P95: 12159 ns (12.16 μs)
  P99: 29962 ns (29.96 μs)
  Max: 29962 ns (29.96 μs)


--- Batch Put Operations ---
=== Database Put (batch) ===
Operations: 1000
Total time: 23.07 ms
Throughput: 43348.44 ops/sec
Latency:
  Min: 8637 ns (8.64 μs)
  Avg: 23068.88 ns (23.07 μs)
  P50: 18719 ns (18.72 μs)
  P95: 51090 ns (51.09 μs)
  P99: 110016 ns (110.02 μs)
  Max: 367574 ns (367.57 μs)


--- Mixed Workload (70% read, 20% write, 10% delete) ---
=== Database Mixed ===
Operations: 1000
Total time: 13.44 ms
Throughput: 74413.91 ops/sec
Latency:
  Min: 6362 ns (6.36 μs)
  Avg: 13438.35 ns (13.44 μs)
  P50: 11299 ns (11.30 μs)
  P95: 23841 ns (23.84 μs)
  P99: 36615 ns (36.62 μs)
  Max: 90264 ns (90.26 μs)


--- Single Delete Operation ---
=== Database Delete (single) ===
Operations: 100
Total time: 1.69 ms
Throughput: 59104.27 ops/sec
Latency:
  Min: 10850 ns (10.85 μs)
  Avg: 16919.25 ns (16.92 μs)
  P50: 15831 ns (15.83 μs)
  P95: 28089 ns (28.09 μs)
  P99: 31316 ns (31.32 μs)
  Max: 31316 ns (31.32 μs)


========================================
Concurrent Throughput Benchmarks
========================================

--- Concurrent Write Benchmark ---
Concurrent Write (1 threads):
  Operations: 1000
  Ops/sec: 48780
  Avg latency: 20309 ns
  Errors: 0

Concurrent Write (2 threads):
  Operations: 2000
  Ops/sec: 93450
  Avg latency: 20520 ns
  Errors: 0

Concurrent Write (4 threads):
  Operations: 4000
  Ops/sec: 87915
  Avg latency: 44918 ns
  Errors: 0

Concurrent Write (8 threads):
  Operations: 8000
  Ops/sec: 165140
  Avg latency: 47388 ns
  Errors: 0

Concurrent Write (16 threads):
  Operations: 16000
  Ops/sec: 244586
  Avg latency: 63580 ns
  Errors: 0

Concurrent Write (32 threads):
  Operations: 32000
  Ops/sec: 383432
  Avg latency: 80246 ns
  Errors: 0


--- Concurrent Read Benchmark ---
  Pre-populating 10000 keys for read benchmark...
Concurrent Read (1 threads):
  Operations: 1000
  Ops/sec: 79579
  Avg latency: 12487 ns
  Errors: 0

  Pre-populating 10000 keys for read benchmark...
Concurrent Read (2 threads):
  Operations: 2000
  Ops/sec: 177500
  Avg latency: 10991 ns
  Errors: 0

  Pre-populating 10000 keys for read benchmark...
Concurrent Read (4 threads):
  Operations: 4000
  Ops/sec: 147178
  Avg latency: 26818 ns
  Errors: 0

  Pre-populating 10000 keys for read benchmark...
Concurrent Read (8 threads):
  Operations: 8000
  Ops/sec: 170516
  Avg latency: 46248 ns
  Errors: 0

  Pre-populating 10000 keys for read benchmark...
Concurrent Read (16 threads):
  Operations: 16000
  Ops/sec: 235639
  Avg latency: 66493 ns
  Errors: 0

  Pre-populating 10000 keys for read benchmark...
Concurrent Read (32 threads):
  Operations: 32000
  Ops/sec: 308093
  Avg latency: 102115 ns
  Errors: 0


--- Concurrent Mixed Benchmark ---
  Pre-populating 10000 keys for mixed workload benchmark...
Concurrent Mixed Workload (1 threads):
  Operations: 1000
  Ops/sec: 63465
  Avg latency: 15692 ns
  Errors: 0

  Pre-populating 10000 keys for mixed workload benchmark...
Concurrent Mixed Workload (2 threads):
  Operations: 2000
  Ops/sec: 113032
  Avg latency: 16832 ns
  Errors: 0

  Pre-populating 10000 keys for mixed workload benchmark...
Concurrent Mixed Workload (4 threads):
  Operations: 4000
  Ops/sec: 132830
  Avg latency: 29316 ns
  Errors: 0

  Pre-populating 10000 keys for mixed workload benchmark...
Concurrent Mixed Workload (8 threads):
  Operations: 8000
  Ops/sec: 173479
  Avg latency: 45471 ns
  Errors: 0

  Pre-populating 10000 keys for mixed workload benchmark...
Concurrent Mixed Workload (16 threads):
  Operations: 16000
  Ops/sec: 193116
  Avg latency: 80637 ns
  Errors: 0

  Pre-populating 10000 keys for mixed workload benchmark...
Concurrent Mixed Workload (32 threads):
  Operations: 32000
  Ops/sec: 249420
  Avg latency: 125605 ns
  Errors: 0


========================================
Database Benchmarks Complete
========================================

Performance Summary:
  Put:   65762 ops/sec (avg: 15206 ns)
  Get:   105280 ops/sec (avg: 9498 ns)
  Batch: 43348 ops/sec (avg: 23069 ns)
  Mixed: 74414 ops/sec (avg: 13438 ns)
  Delete: 59104 ops/sec (avg: 16919 ns)

Cache Metrics:
  Max Memory Budget: 50.00 MB
  Current Memory: 15.36 MB (16101916 bytes)
  Entry Count: 50831
  Avg Entry Size: 317 bytes

