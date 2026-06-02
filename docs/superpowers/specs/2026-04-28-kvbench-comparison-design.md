# KVBench Multi-Database Comparison Design

## Overview

Implement a standalone benchmark suite that uses the KVBench methodology (BU-DiSC/DBTest 2024) to compare WaveDB against YottaDB, ForestDB, RocksDB, and LevelDB across five realistic workload patterns. The suite lives in a new `KVBench/` directory with its own git repository.

## Architecture

The benchmark suite is organized into three layers:

1. **Workload generator** (git submodule): BU-DiSC's C++ `load_gen` produces text workload files.
2. **Common harness** (shared C++ library): Parses workload files, measures latency per operation, computes percentiles, writes results to JSON.
3. **Database adapters** (one executable per database): Each implements a common `DatabaseAdapter` interface and links only against its database.

A Python script reads all JSON results and generates comparison graphs (PNG/SVG).

---

## Repository Layout

```
KVBench/                          # New git repo (inside WaveDB)
├── .git/
├── .gitmodules                   # Submodules: kv-bench, rocksdb, leveldb
├── CMakeLists.txt                # Root: builds common + available adapters
├── README.md
├── .gitignore
├── third_party/
│   ├── kv-bench/                 # git submodule: BU-DiSC/kv-bench
│   │   ├── K-V-Workload-Generator/   # C++ workload generator
│   │   └── kv-bench-rocksdb-example/ # Reference adapter (RocksDB)
│   ├── rocksdb/                  # git submodule: facebook/rocksdb
│   └── leveldb/                  # git submodule: google/leveldb
├── common/
│   ├── CMakeLists.txt
│   ├── include/
│   │   ├── workload_parser.h     # Parse KVBench workload text files
│   │   ├── metrics.h             # Latency tracking, percentiles, throughput
│   │   └── result_writer.h       # JSON and CSV result output
│   └── src/
│       ├── workload_parser.cpp
│       ├── metrics.cpp
│       └── result_writer.cpp
├── adapters/
│   ├── wavedb/
│   │   ├── CMakeLists.txt
│   │   └── bench_wavedb.cpp      # Links against ../../ (WaveDB parent)
│   ├── yottadb/
│   │   ├── CMakeLists.txt
│   │   └── bench_yottadb.cpp     # Links against ../../references/YDB
│   ├── forestdb/
│   │   ├── CMakeLists.txt
│   │   └── bench_forestdb.cpp    # Links against ../../references/forestdb
│   ├── rocksdb/
│   │   ├── CMakeLists.txt
│   │   └── bench_rocksdb.cpp     # Links against ../third_party/rocksdb
│   └── leveldb/
│       ├── CMakeLists.txt
│       └── bench_leveldb.cpp     # Links against ../third_party/leveldb
├── scripts/
│   ├── generate_workloads.sh     # Generate all KVBench workload files
│   ├── run_all.sh              # Run all adapters on all workloads
│   └── plot_results.py           # Generate comparison graphs from JSON
├── workloads/                    # Generated workload files (gitignored)
└── results/                      # Benchmark JSON/CSV output (gitignored)
```

---

## Workload Generator

The KVBench workload generator (`third_party/kv-bench/K-V-Workload-Generator/load_gen`) is a command-line tool that produces text files. Each line in a workload file represents a single operation.

### Supported Operations

| Op Code | Format | Description |
|---------|--------|-------------|
| `I` | `I <key> <value>` | Insert key-value pair |
| `U` | `U <key> <value>` | Update existing key |
| `D` | `D <key>` | Point delete key |
| `Q` | `Q <key>` | Point query (lookup key) |
| `R` | `R <start_key> <end_key>` | Range query |

### Workload Generation Commands

All workloads use 16-byte keys and 100-byte values by default.

#### Workload 1: Empty Point Query-Heavy (Preloading)
```bash
./load_gen -I1000000 --OP workloads/w1_insert.txt
./load_gen --PL -Q500000 -Z0.8 --OP workloads/w1_query.txt
```
80% empty point queries, 20% non-empty. Tests Bloom filter / negative lookup performance.

#### Workload 2: Interleaved Mixed
```bash
./load_gen -I400000 -U100000 -D50000 -Q300000 -Z0.3 --OP workloads/w2_mixed.txt
```
50% inserts, 10% deletes, 15% empty PQs, 25% updates. Tests mixed workload throughput.

#### Workload 3: Multi-Distribution Update and PQ
```bash
./load_gen --PL -I1000000 --OP workloads/w3_insert.txt
./load_gen --PL -U500000 --U_D 3 --U_ZALPHA 1.1 -Q500000 --E_D 0 --Z_D 0 --OP workloads/w3_query.txt
```
Updates follow Zipfian (skewed), point queries follow uniform. Tests cache hit rate under skew.

#### Workload 4: Update and Range Delete Heavy
```bash
./load_gen --PL -I1000000 --OP workloads/w4_insert.txt
./load_gen --PL -U500000 -R500000 -Y0.1 --OP workloads/w4_update.txt
```
50% updates, 50% range deletes with 10% selectivity. Tests compaction efficiency.

#### Workload 5: Insert-Heavy with Skewed Prefixes
```bash
./load_gen -I950000 --I_D 3 --I_ZALPHA 1.5 -Q50000 --OP workloads/w5_insert.txt
```
95% inserts with Zipfian prefix distribution, 5% non-empty PQs. Tests write amplification.

---

## Common Harness Library (`common/`)

### Workload Parser

Parses KVBench text workload files into an in-memory sequence of operations.

```cpp
struct Operation {
    enum Type { INSERT, UPDATE, DELETE, POINT_QUERY, RANGE_QUERY } type;
    std::string key;
    std::string value;       // For INSERT, UPDATE
    std::string end_key;     // For RANGE_QUERY
};

class WorkloadParser {
public:
    bool LoadFile(const std::string& filepath);
    const std::vector<Operation>& GetOperations() const;
    size_t GetInsertCount() const;
    size_t GetQueryCount() const;
    size_t GetDeleteCount() const;
};
```

### Metrics

Per-operation latency tracking with nanosecond precision using `std::chrono::high_resolution_clock`.

```cpp
class MetricsCollector {
public:
    void StartOperation();
    void EndOperation(Operation::Type type);
    void PrintSummary() const;
    void SaveJson(const std::string& filepath) const;

    struct Summary {
        uint64_t total_operations;
        uint64_t total_time_ns;
        double throughput_ops_sec;
        uint64_t min_latency_ns;
        uint64_t avg_latency_ns;
        uint64_t p50_latency_ns;
        uint64_t p95_latency_ns;
        uint64_t p99_latency_ns;
        uint64_t max_latency_ns;
        std::map<Operation::Type, uint64_t> per_type_count;
        std::map<Operation::Type, uint64_t> per_type_avg_ns;
    };
    Summary ComputeSummary() const;
};
```

### Result Writer

Writes results to JSON (machine-readable) and console (human-readable).

JSON schema:
```json
{
  "database": "wavedb",
  "workload": "w2_mixed",
  "timestamp": "2026-04-28T20:00:00Z",
  "summary": {
    "total_operations": 850000,
    "total_time_ns": 1250000000,
    "throughput_ops_sec": 680000.0,
    "min_latency_ns": 120,
    "avg_latency_ns": 1470,
    "p50_latency_ns": 890,
    "p95_latency_ns": 3200,
    "p99_latency_ns": 8500,
    "max_latency_ns": 45000,
    "per_type": {
      "insert": {"count": 400000, "avg_ns": 1800},
      "update": {"count": 100000, "avg_ns": 2100},
      "delete": {"count": 50000, "avg_ns": 1500},
      "point_query": {"count": 300000, "avg_ns": 650}
    }
  }
}
```

---

## Database Adapter Interface

Each adapter implements a common C++ interface. The benchmark harness is identical across all databases.

```cpp
class DatabaseAdapter {
public:
    virtual ~DatabaseAdapter() = default;

    // Open/create database at given path.
    // Returns true on success.
    virtual bool Open(const char* path, size_t num_threads_hint) = 0;

    // Close database and free resources.
    virtual void Close() = 0;

    // Insert or update key-value pair.
    // Returns true on success.
    virtual bool Put(const std::string& key, const std::string& value) = 0;

    // Lookup key. If found, store value in out_value and return true.
    // If not found, return false (not an error).
    virtual bool Get(const std::string& key, std::string* out_value) = 0;

    // Delete key. Returns true if key was deleted or did not exist.
    virtual bool Delete(const std::string& key) = 0;

    // Range query: return all keys in [start, end).
    // Returns count of keys found.
    virtual size_t RangeQuery(const std::string& start,
                               const std::string& end,
                               std::vector<std::string>* out_keys) = 0;

    // Flush any pending writes to disk (WAL flush).
    virtual void Flush() = 0;
};

// Factory function type: each adapter provides one.
typedef DatabaseAdapter* (*CreateAdapterFunc)(void);
```

### Adapter Execution Flow

1. Parse workload file into `std::vector<Operation>`
2. Separate preloading operations (inserts) from benchmark operations
3. Open adapter at fresh database path
4. Run preloading: `for (op : preload) adapter->Put(op.key, op.value)`
5. Run benchmark: for each operation, record latency with `MetricsCollector`
6. Flush adapter
7. Close adapter
8. Save metrics to JSON

### CLI per Adapter

All adapters accept the same CLI:

```
./bench_<database> \
  --workload <path>         # Path to KVBench workload file
  --db-path <path>          # Directory for database files
  --results <path>          # Output JSON file
  --warmup <count>          # Number of warmup operations (default: 0)
  --threads <count>         # Number of threads (default: 1)
  --preload-only            # Only run preloading phase
```

---

## Database-Specific Notes

### WaveDB Adapter

- Uses `database_create_with_config()` with `enable_persist = 1`
- Keys: flat string keys (no delimiter needed for benchmark)
- Values: `identifier_create_from_string()` for string values
- Operations: `database_put_sync_raw()`, `database_get_sync_raw()`, `database_delete_sync()`
- For range queries: `database_scan_start()` / `database_scan_next()`
- Links against `wavedb` target from parent CMake (via `add_subdirectory`)

### YottaDB Adapter

- YottaDB is a MUMPS/GT.M descendant with hierarchical key-value access
- Uses the C API: `ydb_init()`, `ydb_set_s()`, `ydb_get_s()`, `ydb_delete_s()`
- Keys map to YottaDB global nodes (e.g., `^wavedb("key")`)
- Must call `ydb_exit()` on close
- Build: compile YottaDB from `../../references/YDB/` first, then link `libgtmshr`

### ForestDB Adapter

- Uses ForestDB C API: `fdb_open()`, `fdb_set()`, `fdb_get()`, `fdb_del()`, `fdb_iterator_init()`
- Configuration via `fdb_config` struct
- Supports MVCC (snapshots) but benchmark uses default config
- Build: compile ForestDB from `../../references/forestdb/`, link `libforestdb`

### RocksDB Adapter

- Uses RocksDB C++ API: `rocksdb::DB::Open()`, `Put()`, `Get()`, `Delete()`, `NewIterator()`
- Options: default with `create_if_missing = true`
- Build: compile RocksDB from `third_party/rocksdb/` as static library, link `librocksdb.a`

### LevelDB Adapter

- Uses LevelDB C++ API: `leveldb::DB::Open()`, `Put()`, `Get()`, `Delete()`, `NewIterator()`
- Options: default with `create_if_missing = true`
- Build: compile LevelDB from `third_party/leveldb/` as static library

---

## Build System

### Root CMakeLists.txt

```cmake
cmake_minimum_required(VERSION 3.16)
project(KVBenchComparison VERSION 1.0.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Common library
add_subdirectory(common)

# Available adapters (each checks if its database is buildable)
option(BUILD_WAVEDB_ADAPTER "Build WaveDB adapter" ON)
option(BUILD_YOTTADB_ADAPTER "Build YottaDB adapter" ON)
option(BUILD_FORESTDB_ADAPTER "Build ForestDB adapter" ON)
option(BUILD_ROCKSDB_ADAPTER "Build RocksDB adapter" ON)
option(BUILD_LEVELDB_ADAPTER "Build LevelDB adapter" ON)

if(BUILD_WAVEDB_ADAPTER)
    add_subdirectory(adapters/wavedb)
endif()
if(BUILD_YOTTADB_ADAPTER)
    add_subdirectory(adapters/yottadb)
endif()
if(BUILD_FORESTDB_ADAPTER)
    add_subdirectory(adapters/forestdb)
endif()
if(BUILD_ROCKSDB_ADAPTER)
    add_subdirectory(adapters/rocksdb)
endif()
if(BUILD_LEVELDB_ADAPTER)
    add_subdirectory(adapters/leveldb)
endif()
```

Each adapter subdirectory checks whether its database library is available. If not, the adapter is silently skipped with a CMake status message. This allows partial builds when dependencies are missing.

---

## Graph Generation (`scripts/plot_results.py`)

A Python script that reads all JSON result files and generates comparison graphs.

### Graph Types

1. **Throughput Comparison Bar Chart**
   - X-axis: workload (W1 through W5)
   - Y-axis: throughput (ops/sec)
   - Grouped bars: one bar per database
   - Output: `results/graphs/throughput_comparison.png`

2. **Latency Breakdown Bar Chart**
   - X-axis: database
   - Y-axis: latency (μs), log scale
   - Grouped bars: P50, P95, P99
   - One chart per workload
   - Output: `results/graphs/latency_w<N>.png`

3. **Operation-Type Latency Breakdown**
   - X-axis: operation type (insert, update, delete, point_query)
   - Y-axis: average latency (μs)
   - Grouped bars: one per database
   - One chart per workload
   - Output: `results/graphs/per_op_latency_w<N>.png`

4. **Speedup Comparison (WaveDB vs Others)**
   - X-axis: workload
   - Y-axis: speedup factor (WaveDB throughput / other throughput)
   - One line per competing database
   - Output: `results/graphs/speedup_vs_wavedb.png`

### Python Dependencies

```bash
pip install matplotlib numpy
```

### Usage

```bash
python3 scripts/plot_results.py \
  --results-dir results/ \
  --output-dir results/graphs/
```

---

## Running the Full Comparison

### Step 1: Generate Workloads

```bash
./scripts/generate_workloads.sh
```

This runs all five `load_gen` commands and produces files in `workloads/`.

### Step 2: Build All Adapters

```bash
mkdir build && cd build
cmake ..
make -j$(nproc)
cd ..
```

### Step 3: Run All Benchmarks

```bash
./scripts/run_all.sh
```

Runs each adapter against each workload, saving JSON results to `results/`.

### Step 4: Generate Graphs

```bash
python3 scripts/plot_results.py --results-dir results/ --output-dir results/graphs/
```

### Step 5: View Results

Open `results/graphs/` for all comparison charts.

---

## Database Sources

| Database | Source | Build Method |
|----------|--------|-------------|
| **WaveDB** | Parent repo `../../` | `add_subdirectory(../../)` — CMake target `wavedb` |
| **YottaDB** | `../../references/YDB/` | Source build: `cmake` + `make` in references/YDB |
| **ForestDB** | `../../references/forestdb/` | Source build: `cmake` + `make` in references/forestdb |
| **RocksDB** | `third_party/rocksdb/` (submodule) | `make static_lib` in submodule |
| **LevelDB** | `third_party/leveldb/` (submodule) | `mkdir build && cmake .. && make` in submodule |

---

## Metrics and Success Criteria

The benchmark suite is considered working when:

1. All five KVBench workloads can be generated by `scripts/generate_workloads.sh`
2. All five database adapters compile successfully (or are gracefully skipped if dependencies missing)
3. Each adapter can run each workload and produce valid JSON results
4. The Python script can read all JSON files and produce at least the three core graph types
5. Results are reproducible across runs (variance < 5% on identical hardware)

---

## Error Handling

- If a database adapter fails to open (e.g., database library not built), print an error and skip that adapter. Other adapters continue.
- If a workload file is missing, print an error and skip that workload.
- If an individual operation fails during benchmark, log the failure but continue with remaining operations. Include failure count in JSON output.

---

## Future Extensions (out of scope for initial implementation)

- Multi-threaded benchmarks (concurrent clients)
- Throughput-vs-latency curves (sweeping client count)
- Memory usage tracking (RSS peak during benchmark)
- Storage amplification (database size vs raw data size)
- Custom workload definitions (user-defined operation mixes)
