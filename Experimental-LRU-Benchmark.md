# Experimental LRU Branch Benchmark Results

## Synchronous Database Benchmarks

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
Total time: 86.57 ms
Throughput: 115510.00 ops/sec
Latency:
  Min: 6525 ns (6.53 μs)
  Avg: 8657.26 ns (8.66 μs)
  P50: 8072 ns (8.07 μs)
  P95: 11790 ns (11.79 μs)
  P99: 16186 ns (16.19 μs)
  Max: 233346 ns (233.35 μs)


--- Synchronous Get Operations ---
=== Sync Get ===
Operations: 10000
Total time: 4.52 ms
Throughput: 2213931.30 ops/sec
Latency:
  Min: 421 ns (0.42 μs)
  Avg: 451.69 ns (0.45 μs)
  P50: 433 ns (0.43 μs)
  P95: 473 ns (0.47 μs)
  P99: 720 ns (0.72 μs)
  Max: 18107 ns (18.11 μs)


--- Synchronous Mixed Workload (70% read, 20% write, 10% delete) ---
=== Sync Mixed ===
Operations: 10000
Total time: 4.35 ms
Throughput: 2298827.85 ops/sec
Latency:
  Min: 406 ns (0.41 μs)
  Avg: 435.00 ns (0.44 μs)
  P50: 418 ns (0.42 μs)
  P95: 448 ns (0.45 μs)
  P99: 780 ns (0.78 μs)
  Max: 8784 ns (8.78 μs)


--- Synchronous Delete Operations ---
=== Sync Delete ===
Operations: 10000
Total time: 36.76 ms
Throughput: 272009.34 ops/sec
Latency:
  Min: 3371 ns (3.37 μs)
  Avg: 3676.34 ns (3.68 μs)
  P50: 3542 ns (3.54 μs)
  P95: 4750 ns (4.75 μs)
  P99: 5936 ns (5.94 μs)
  Max: 42366 ns (42.37 μs)


========================================
All synchronous benchmarks complete.
========================================

## Async Database Benchmarks

**TIMEOUT**: The async database benchmark hung during the mixed workload phase and did not
complete within 20 minutes. The single-threaded operations completed with the following
partial results before the hang:

--- Single Put Operation ---
=== Database Put (single) ===
Operations: 100
Total time: 1.53 ms
Throughput: 65242.04 ops/sec
Latency:
  Min: 7789 ns (7.79 μs)
  Avg: 15327.54 ns (15.33 μs)
  P50: 13958 ns (13.96 μs)
  P95: 50060 ns (50.06 μs)
  P99: 65655 ns (65.66 μs)
  Max: 65655 ns (65.66 μs)

--- Single Get Operation ---
=== Database Get (single) ===
Operations: 100
Total time: 0.85 ms
Throughput: 117898.81 ops/sec
Latency:
  Min: 5597 ns (5.60 μs)
  Avg: 8481.85 ns (8.48 μs)
  P50: 7094 ns (7.09 μs)
  P95: 12321 ns (12.32 μs)
  P99: 48851 ns (48.85 μs)
  Max: 48851 ns (48.85 μs)

--- Batch Put Operations ---
=== Database Put (batch) ===
Operations: 1000
Total time: 17.80 ms
Throughput: 56194.95 ops/sec
Latency:
  Min: 8589 ns (8.59 μs)
  Avg: 17795.19 ns (17.80 μs)
  P50: 16029 ns (16.03 μs)
  P95: 29689 ns (29.69 μs)
  P99: 54767 ns (54.77 μs)
  Max: 103032 ns (103.03 μs)

Concurrent benchmarks: HUNG (did not complete within 20-minute timeout)