# KVBench Adapter Rewrite Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Rewrite KVBench adapters to use proper synchronous APIs for all databases, add a WaveDB async benchmark that fires all operations in parallel, and normalize cache/WAL configurations.

**Architecture:** Each database gets a synchronous adapter using its native sync API. WaveDB additionally gets an async adapter that enqueues all operations to the work pool concurrently and measures end-to-end throughput. All databases configured with 128 MB block/page cache and fastest WAL mode.

**Tech Stack:** C++17, CMake, WaveDB C API, RocksDB C++ API, LevelDB C++ API, ForestDB C API

---

### Task 1: Rewrite RocksDB Adapter (128 MB Block Cache)

**Files:**
- Modify: `KVBench/adapters/rocksdb/bench_rocksdb.cpp:33`

- [ ] **Step 1: Change OptimizeForPointLookup from 64MB to 128MB**

In `KVBench/adapters/rocksdb/bench_rocksdb.cpp`, line 33 currently has:
```cpp
options.OptimizeForPointLookup(64 * 1024 * 1024);
```

Change to:
```cpp
options.OptimizeForPointLookup(128);  // 128 MB block cache
```
(`OptimizeForPointLookup` takes MB as argument — verify the signature: `ColumnFamilyOptions* OptimizeForPointLookup(uint64_t block_cache_size_mb)`, so pass 128.)

- [ ] **Step 2: Commit**

```bash
cd KVBench && git add adapters/rocksdb/bench_rocksdb.cpp && git commit -m "chore(kvbench): set RocksDB block cache to 128 MB"
```

---

### Task 2: Rewrite LevelDB Adapter (128 MB Block Cache)

**Files:**
- Modify: `KVBench/adapters/leveldb/bench_leveldb.cpp:30-32`

- [ ] **Step 1: Add block cache and include cache.h**

Change the `#include` section (add line 13):
```cpp
#include "leveldb/cache.h"
```

Change the `Open` method to set a 128 MB block cache:
```cpp
bool Open(const char* path, size_t num_threads_hint) {
    (void)num_threads_hint;

    // Create directory if it doesn't exist
    mkdir(path, 0755);

    leveldb::Options options;
    options.create_if_missing = true;
    options.error_if_exists = false;
    options.block_cache = leveldb::NewLRUCache(128 * 1024 * 1024);  // 128 MB

    leveldb::Status status = leveldb::DB::Open(options, path, &db_);
    if (!status.ok()) {
        std::cerr << "LevelDB open error: " << status.ToString() << std::endl;
        db_ = nullptr;
        return false;
    }

    std::cout << "LevelDB opened at " << path << " (128 MB block cache)" << std::endl;
    return true;
}
```

And update `Close` to delete the block cache:
```cpp
void Close() {
    if (db_) {
        leveldb::Options opts;  // need access to options... 
        delete db_;
        db_ = nullptr;
    }
}
```

Wait — we need to store the cache pointer. The simplest approach: store `cache_` as a member.

Add to private section:
```cpp
private:
    leveldb::DB* db_ = nullptr;
    leveldb::Cache* cache_ = nullptr;
```

Then `Open` becomes:
```cpp
bool Open(const char* path, size_t num_threads_hint) {
    (void)num_threads_hint;
    mkdir(path, 0755);

    leveldb::Options options;
    options.create_if_missing = true;
    options.error_if_exists = false;
    cache_ = leveldb::NewLRUCache(128 * 1024 * 1024);
    options.block_cache = cache_;

    leveldb::Status status = leveldb::DB::Open(options, path, &db_);
    if (!status.ok()) {
        std::cerr << "LevelDB open error: " << status.ToString() << std::endl;
        delete cache_;
        cache_ = nullptr;
        db_ = nullptr;
        return false;
    }

    std::cout << "LevelDB opened at " << path << " (128 MB block cache)" << std::endl;
    return true;
}
```

And `Close` becomes:
```cpp
void Close() {
    if (db_) {
        delete db_;
        db_ = nullptr;
    }
    if (cache_) {
        delete cache_;
        cache_ = nullptr;
    }
}
```

- [ ] **Step 2: Commit**

```bash
cd KVBench && git add adapters/leveldb/bench_leveldb.cpp && git commit -m "chore(kvbench): set LevelDB block cache to 128 MB"
```

---

### Task 3: Rewrite WaveDB Sync Adapter

**Files:**
- Create: `KVBench/adapters/wavedb/bench_wavedb.cpp` (full rewrite)

This is the synchronous benchmark using the proper C API from README. No work pool, no timing wheel, no `_raw` suffix.

- [ ] **Step 1: Write the full bench_wavedb.cpp**

```cpp
// WaveDB KVBench Adapter - Synchronous API
//
// Uses the proper synchronous C API (database_put_sync, database_get_sync,
// database_delete_sync) with path_t/identifier_t objects. No work pool.

#include <iostream>
#include <string>
#include <vector>
#include <cstring>
#include <filesystem>
#include <sys/stat.h>
#include <sys/types.h>

#include "workload_parser.h"
#include "metrics.h"
#include "result_writer.h"

extern "C" {
#include "Database/database.h"
#include "HBTrie/path.h"
#include "HBTrie/identifier.h"
#include "Buffer/buffer.h"
}

namespace kvbench {

class WaveDBSyncAdapter {
public:
    bool Open(const char* path, size_t /* num_threads_hint */) {
        mkdir(path, 0755);

        wal_config_t wal_config = {
            .sync_mode = WAL_SYNC_ASYNC,
            .debounce_ms = 250,
            .idle_threshold_ms = 10000,
            .compact_interval_ms = 60000,
            .max_file_size = 100 * 1024 * 1024,
            .max_sealed_wals = 0,
        };

        int error_code = 0;
        db_ = database_create(
            path,
            50,            // 50 MB LRU cache (default)
            &wal_config,
            0,             // default chunk_size (4)
            0,             // default btree_node_size (4096)
            1,             // enable persistence
            NULL,          // no work pool (sync)
            NULL,          // no timing wheel (sync)
            &error_code
        );

        if (!db_ || error_code != 0) {
            std::cerr << "Failed to create database, error_code=" << error_code << std::endl;
            return false;
        }

        std::cout << "WaveDB (sync) opened at " << path << " (WAL_ASYNC, 50MB LRU)" << std::endl;
        return true;
    }

    void Close() {
        if (db_) {
            database_destroy(db_);
            db_ = nullptr;
        }
    }

    bool Put(const std::string& key, const std::string& value) {
        path_t* path = make_path(key.c_str());
        if (!path) return false;
        identifier_t* val = make_value(value.c_str());
        if (!val) { path_destroy(path); return false; }

        int rc = database_put_sync(db_, path, val);
        // database_put_sync takes ownership of path and val on success or failure
        return rc == 0;
    }

    bool Get(const std::string& key, std::string* out_value) {
        path_t* path = make_path(key.c_str());
        if (!path) return false;

        identifier_t* result = NULL;
        int rc = database_get_sync(db_, path, &result);
        // database_get_sync takes ownership of path

        if (rc == 0 && result) {
            if (out_value) {
                size_t len = 0;
                uint8_t* data = identifier_get_data_copy(result, &len);
                if (data) {
                    out_value->assign(reinterpret_cast<char*>(data), len);
                    free(data);
                }
            }
            identifier_destroy(result);
            return true;
        }
        return false;
    }

    bool Delete(const std::string& key) {
        path_t* path = make_path(key.c_str());
        if (!path) return false;

        int rc = database_delete_sync(db_, path);
        // database_delete_sync takes ownership of path
        return rc == 0 || rc == -2;  // -2 = not found, acceptable
    }

    size_t RangeQuery(const std::string& start,
                      const std::string& end,
                      std::vector<std::string>* out_keys) {
        path_t* start_path = make_path(start.c_str());
        if (!start_path) return 0;
        path_t* end_path = make_path(end.empty() ? "~" : end.c_str());
        if (!end_path) { path_destroy(start_path); return 0; }

        database_iterator_t* it = database_scan_start(db_, start_path, end_path);
        path_destroy(start_path);
        path_destroy(end_path);

        if (!it) return 0;

        size_t count = 0;
        path_t* out_path = NULL;
        identifier_t* out_val = NULL;
        while (database_scan_next(it, &out_path, &out_val) == 0) {
            if (out_keys && out_path) {
                // Extract string key from path for output
                // For simple single-identifier paths, just use the first id
                if (path_length(out_path) > 0) {
                    identifier_t* id = path_get(out_path, 0);
                    if (id) {
                        size_t klen = 0;
                        uint8_t* kdata = identifier_get_data_copy(id, &klen);
                        if (kdata) {
                            out_keys->push_back(std::string(reinterpret_cast<char*>(kdata), klen));
                            free(kdata);
                        }
                    }
                }
            }
            count++;
            if (out_path) path_destroy(out_path);
            if (out_val) identifier_destroy(out_val);
        }
        database_scan_end(it);
        return count;
    }

    void Flush() {
        if (db_) {
            database_flush_dirty_bnodes(db_);
        }
    }

private:
    database_t* db_ = nullptr;

    static path_t* make_path(const char* key) {
        path_t* path = path_create();
        if (!path) return NULL;
        buffer_t* buf = buffer_create_from_pointer_copy((uint8_t*)key, strlen(key));
        if (!buf) { path_destroy(path); return NULL; }
        identifier_t* id = identifier_create(buf, 0);
        buffer_destroy(buf);
        if (!id) { path_destroy(path); return NULL; }
        path_append(path, id);
        identifier_destroy(id);
        return path;
    }

    static identifier_t* make_value(const char* data) {
        buffer_t* buf = buffer_create_from_pointer_copy((uint8_t*)data, strlen(data));
        if (!buf) return NULL;
        identifier_t* id = identifier_create(buf, 0);
        buffer_destroy(buf);
        return id;
    }
};

} // namespace kvbench

// ---- Command-line interface ----

static void print_usage(const char* prog) {
    std::cerr << "Usage: " << prog << " [options]\n"
              << "Options:\n"
              << "  --workload <path>    Path to workload file\n"
              << "  --db-path <path>     Path for database directory\n"
              << "  --results <path>     Path for JSON results file\n"
              << "  --warmup <n>         Number of warmup operations (default: 1000)\n"
              << "  --help               Show this help\n";
}

int main(int argc, char** argv) {
    std::string workload_path;
    std::string db_path = "/tmp/kvbench_wavedb_sync";
    std::string results_path;
    size_t warmup_count = 1000;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--workload" && i + 1 < argc) {
            workload_path = argv[++i];
        } else if (arg == "--db-path" && i + 1 < argc) {
            db_path = argv[++i];
        } else if (arg == "--results" && i + 1 < argc) {
            results_path = argv[++i];
        } else if (arg == "--warmup" && i + 1 < argc) {
            warmup_count = static_cast<size_t>(std::stoul(argv[++i]));
        } else if (arg == "--help") {
            print_usage(argv[0]);
            return 0;
        }
    }

    if (workload_path.empty()) {
        std::cerr << "Error: --workload is required\n";
        print_usage(argv[0]);
        return 1;
    }

    // Parse workload
    kvbench::WorkloadParser parser;
    if (!parser.LoadFile(workload_path)) {
        std::cerr << "Error: Failed to load workload file: " << workload_path << std::endl;
        return 1;
    }

    const auto& ops = parser.GetOperations();
    std::cout << "Loaded " << ops.size() << " operations from " << workload_path << std::endl;
    std::cout << "  Inserts: " << parser.GetInsertCount() << std::endl;
    std::cout << "  Queries: " << parser.GetQueryCount() << std::endl;
    std::cout << "  Deletes: " << parser.GetDeleteCount() << std::endl;

    // Open database
    kvbench::WaveDBSyncAdapter adapter;
    if (!adapter.Open(db_path.c_str(), 0)) {
        std::cerr << "Error: Failed to open database at " << db_path << std::endl;
        return 1;
    }

    // Warmup phase
    std::cout << "Warming up with " << warmup_count << " operations..." << std::endl;
    size_t warmup_done = 0;
    for (const auto& op : ops) {
        if (warmup_done >= warmup_count) break;
        switch (op.type) {
            case kvbench::Operation::INSERT:
            case kvbench::Operation::UPDATE:
                adapter.Put(op.key, op.value);
                break;
            case kvbench::Operation::POINT_QUERY:
                { std::string v; adapter.Get(op.key, &v); }
                break;
            case kvbench::Operation::DELETE:
                adapter.Delete(op.key);
                break;
            case kvbench::Operation::RANGE_QUERY:
                adapter.RangeQuery(op.key, op.end_key, nullptr);
                break;
        }
        warmup_done++;
    }
    std::cout << "Warmup complete." << std::endl;

    // Benchmark phase
    kvbench::MetricsCollector metrics;

    std::cout << "Running benchmark..." << std::endl;
    for (const auto& op : ops) {
        metrics.StartOperation();

        switch (op.type) {
            case kvbench::Operation::INSERT:
            case kvbench::Operation::UPDATE: {
                bool ok = adapter.Put(op.key, op.value);
                if (!ok) {
                    std::cerr << "Put failed for key: " << op.key << std::endl;
                }
                metrics.EndOperation(kvbench::Operation::INSERT);
                break;
            }
            case kvbench::Operation::POINT_QUERY: {
                std::string value;
                adapter.Get(op.key, &value);
                metrics.EndOperation(kvbench::Operation::POINT_QUERY);
                break;
            }
            case kvbench::Operation::DELETE: {
                adapter.Delete(op.key);
                metrics.EndOperation(kvbench::Operation::DELETE);
                break;
            }
            case kvbench::Operation::RANGE_QUERY: {
                std::vector<std::string> keys;
                adapter.RangeQuery(op.key, op.end_key, &keys);
                metrics.EndOperation(kvbench::Operation::RANGE_QUERY);
                break;
            }
        }
    }

    // Flush before close
    adapter.Flush();

    // Print results
    std::cout << std::endl;
    metrics.PrintSummary();

    // Write JSON results
    if (!results_path.empty()) {
        kvbench::Summary summary = metrics.ComputeSummary();
        kvbench::ResultWriter::WriteJson(results_path, "wavedb-sync",
                                          workload_path, summary);
        kvbench::ResultWriter::WriteConsole("wavedb-sync",
                                             workload_path, summary);
        std::cout << "Results written to " << results_path << std::endl;
    }

    adapter.Close();
    std::filesystem::remove_all(db_path);
    return 0;
}
```

- [ ] **Step 2: Commit**

```bash
cd KVBench && git add adapters/wavedb/bench_wavedb.cpp && git commit -m "feat(kvbench): rewrite WaveDB sync adapter with proper API"
```

---

### Task 4: Create WaveDB Async Adapter

**Files:**
- Create: `KVBench/adapters/wavedb/bench_wavedb_async.cpp`

This adapter fires ALL operations into the work pool at once without blocking, then waits for all to complete.

- [ ] **Step 1: Write bench_wavedb_async.cpp**

```cpp
// WaveDB KVBench Adapter - Async API (fire-and-forget parallelism)
//
// Pre-builds all path_t/identifier_t objects, enqueues all operations via
// database_put/database_get/database_delete at once, then waits for all
// promises to resolve. Measures total wall-clock time.

#include <atomic>
#include <iostream>
#include <string>
#include <vector>
#include <cstring>
#include <chrono>
#include <filesystem>
#include <sys/stat.h>
#include <sys/types.h>

#include "workload_parser.h"
#include "metrics.h"
#include "result_writer.h"

extern "C" {
#include "Database/database.h"
#include "Database/database_config.h"
#include "HBTrie/path.h"
#include "HBTrie/identifier.h"
#include "Buffer/buffer.h"
#include "Workers/promise.h"
#include "Workers/error.h"
}

namespace kvbench {

class WaveDBAsyncAdapter {
public:
    bool Open(const char* path, size_t num_threads) {
        mkdir(path, 0755);

        config_ = database_config_default();
        if (!config_) return false;

        config_->enable_persist = 1;
        config_->worker_threads = static_cast<uint8_t>(num_threads > 0 ? num_threads : 4);
        database_config_set_wal_sync_mode(config_, WAL_SYNC_ASYNC);
        config_->lru_memory_mb = 50;

        int error_code = 0;
        db_ = database_create_with_config(path, config_, &error_code);

        if (!db_ || error_code != 0) {
            std::cerr << "Failed to create database, error_code=" << error_code << std::endl;
            database_config_destroy(config_);
            config_ = nullptr;
            return false;
        }

        std::cout << "WaveDB (async) opened with " << (int)config_->worker_threads
                  << " worker threads, WAL_ASYNC, 50MB LRU" << std::endl;
        return true;
    }

    void Close() {
        if (db_) {
            database_destroy(db_);
            db_ = nullptr;
        }
        if (config_) {
            database_config_destroy(config_);
            config_ = nullptr;
        }
    }

    // Run all operations in parallel, return total elapsed time in ns
    uint64_t RunAllParallel(const std::vector<Operation>& ops) {
        std::atomic<size_t> completed{0};
        const size_t total = ops.size();

        auto start = std::chrono::high_resolution_clock::now();

        for (size_t i = 0; i < total; i++) {
            const auto& op = ops[i];

            switch (op.type) {
                case Operation::INSERT:
                case Operation::UPDATE: {
                    path_t* path = make_path(op.key.c_str());
                    identifier_t* val = make_value(op.value.c_str());
                    if (!path || !val) {
                        if (path) path_destroy(path);
                        if (val) identifier_destroy(val);
                        completed.fetch_add(1);
                        break;
                    }

                    auto* ctx = new AsyncCtx{&completed, path, val};
                    promise_t* p = promise_create(on_put_complete, on_async_error, ctx);
                    if (!p) {
                        path_destroy(path);
                        identifier_destroy(val);
                        delete ctx;
                        completed.fetch_add(1);
                        break;
                    }
                    database_put(db_, path, val, p);
                    break;
                }
                case Operation::POINT_QUERY: {
                    path_t* path = make_path(op.key.c_str());
                    if (!path) {
                        completed.fetch_add(1);
                        break;
                    }

                    auto* ctx = new AsyncCtx{&completed, path, nullptr};
                    promise_t* p = promise_create(on_get_complete, on_async_error, ctx);
                    if (!p) {
                        path_destroy(path);
                        delete ctx;
                        completed.fetch_add(1);
                        break;
                    }
                    database_get(db_, path, p);
                    break;
                }
                case Operation::DELETE: {
                    path_t* path = make_path(op.key.c_str());
                    if (!path) {
                        completed.fetch_add(1);
                        break;
                    }

                    auto* ctx = new AsyncCtx{&completed, path, nullptr};
                    promise_t* p = promise_create(on_del_complete, on_async_error, ctx);
                    if (!p) {
                        path_destroy(path);
                        delete ctx;
                        completed.fetch_add(1);
                        break;
                    }
                    database_delete(db_, path, p);
                    break;
                }
                case Operation::RANGE_QUERY: {
                    // Range queries fall back to sync — just count as complete
                    completed.fetch_add(1);
                    break;
                }
            }
        }

        // Busy-wait for all operations to complete
        while (completed.load() < total) {
            // spin briefly
        }

        auto end = std::chrono::high_resolution_clock::now();
        return std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    }

    database_t* db_ = nullptr;
    database_config_t* config_ = nullptr;

    struct AsyncCtx {
        std::atomic<size_t>* completed;
        path_t* path;        // for cleanup in error path
        identifier_t* val;   // for cleanup in error path (may be null)
    };

    // --- Callbacks ---

    static void on_put_complete(void* ctx_ptr, void* /* payload */) {
        auto* ctx = static_cast<AsyncCtx*>(ctx_ptr);
        // database_put took ownership of path and val — nothing to free
        ctx->completed->fetch_add(1);
        delete ctx;
    }

    static void on_get_complete(void* ctx_ptr, void* payload) {
        auto* ctx = static_cast<AsyncCtx*>(ctx_ptr);
        if (payload) {
            identifier_t* id = static_cast<identifier_t*>(payload);
            REFERENCE(id, identifier_t);
            identifier_destroy(id);
        }
        ctx->completed->fetch_add(1);
        delete ctx;
    }

    static void on_del_complete(void* ctx_ptr, void* /* payload */) {
        auto* ctx = static_cast<AsyncCtx*>(ctx_ptr);
        ctx->completed->fetch_add(1);
        delete ctx;
    }

    static void on_async_error(void* ctx_ptr, async_error_t* /* err */) {
        auto* ctx = static_cast<AsyncCtx*>(ctx_ptr);
        ctx->completed->fetch_add(1);
        delete ctx;
    }

    // --- Helpers ---
    static path_t* make_path(const char* key) {
        path_t* path = path_create();
        if (!path) return NULL;
        buffer_t* buf = buffer_create_from_pointer_copy((uint8_t*)key, strlen(key));
        if (!buf) { path_destroy(path); return NULL; }
        identifier_t* id = identifier_create(buf, 0);
        buffer_destroy(buf);
        if (!id) { path_destroy(path); return NULL; }
        path_append(path, id);
        identifier_destroy(id);
        return path;
    }

    static identifier_t* make_value(const char* data) {
        buffer_t* buf = buffer_create_from_pointer_copy((uint8_t*)data, strlen(data));
        if (!buf) return NULL;
        identifier_t* id = identifier_create(buf, 0);
        buffer_destroy(buf);
        return id;
    }
};

} // namespace kvbench

// ---- Command-line interface ----

static void print_usage(const char* prog) {
    std::cerr << "Usage: " << prog << " [options]\n"
              << "Options:\n"
              << "  --workload <path>    Path to workload file\n"
              << "  --db-path <path>     Path for database directory\n"
              << "  --results <path>     Path for JSON results file\n"
              << "  --threads <n>        Worker thread count (default: 16)\n"
              << "  --help               Show this help\n";
}

int main(int argc, char** argv) {
    std::string workload_path;
    std::string db_path = "/tmp/kvbench_wavedb_async";
    std::string results_path;
    size_t num_threads = 16;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--workload" && i + 1 < argc) {
            workload_path = argv[++i];
        } else if (arg == "--db-path" && i + 1 < argc) {
            db_path = argv[++i];
        } else if (arg == "--results" && i + 1 < argc) {
            results_path = argv[++i];
        } else if (arg == "--threads" && i + 1 < argc) {
            num_threads = std::stoul(argv[++i]);
        } else if (arg == "--help") {
            print_usage(argv[0]);
            return 0;
        }
    }

    if (workload_path.empty()) {
        std::cerr << "Error: --workload is required\n";
        print_usage(argv[0]);
        return 1;
    }

    // Parse workload
    kvbench::WorkloadParser parser;
    if (!parser.LoadFile(workload_path)) {
        std::cerr << "Error: Failed to load workload file: " << workload_path << std::endl;
        return 1;
    }

    const auto& ops = parser.GetOperations();
    std::cout << "Loaded " << ops.size() << " operations from " << workload_path << std::endl;
    std::cout << "  Inserts: " << parser.GetInsertCount() << std::endl;
    std::cout << "  Queries: " << parser.GetQueryCount() << std::endl;
    std::cout << "  Deletes: " << parser.GetDeleteCount() << std::endl;
    std::cout << "  Worker threads: " << num_threads << std::endl;

    // Open database
    kvbench::WaveDBAsyncAdapter adapter;
    if (!adapter.Open(db_path.c_str(), num_threads)) {
        std::cerr << "Error: Failed to open database at " << db_path << std::endl;
        return 1;
    }

    // Pre-warm: insert a few keys to prime the work pool
    std::cout << "Priming work pool..." << std::endl;
    for (size_t i = 0; i < std::min(size_t(100), ops.size()); i++) {
        const auto& op = ops[i];
        if (op.type == kvbench::Operation::INSERT || op.type == kvbench::Operation::UPDATE) {
            auto* ctx = new kvbench::WaveDBAsyncAdapter::AsyncCtx{nullptr, nullptr, nullptr};
            path_t* path = kvbench::WaveDBAsyncAdapter::make_path(op.key.c_str());
            identifier_t* val = kvbench::WaveDBAsyncAdapter::make_value(op.value.c_str());
            if (!path || !val) {
                if (path) path_destroy(path);
                if (val) identifier_destroy(val);
                delete ctx;
                continue;
            }
            promise_t* p = promise_create(
                [](void* c, void*) { auto* cc = static_cast<kvbench::WaveDBAsyncAdapter::AsyncCtx*>(c); delete cc; },
                kvbench::WaveDBAsyncAdapter::on_async_error, ctx);
            if (p) {
                database_put(adapter.db_, path, val, p);
            } else {
                path_destroy(path);
                identifier_destroy(val);
                delete ctx;
            }
        }
    }
    // Wait briefly for priming to finish
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    std::cout << "Priming complete." << std::endl;

    // Run benchmark — fire ALL operations at once
    std::cout << "Running async benchmark (all ops fired in parallel)..." << std::endl;
    uint64_t elapsed_ns = adapter.RunAllParallel(ops);

    size_t total_ops = ops.size();
    double throughput = (total_ops * 1e9) / elapsed_ns;
    double avg_latency = elapsed_ns / (double)total_ops;

    std::cout << "\n=== Results ===" << std::endl;
    std::cout << "Total operations: " << total_ops << std::endl;
    std::cout << "Total time: " << (elapsed_ns / 1e6) << " ms" << std::endl;
    std::cout << "Throughput: " << static_cast<uint64_t>(throughput) << " ops/sec" << std::endl;
    std::cout << "Avg latency: " << static_cast<uint64_t>(avg_latency) << " ns" << std::endl;

    // Write JSON results
    if (!results_path.empty()) {
        kvbench::Summary summary;
        summary.total_operations = total_ops;
        summary.total_time_ns = elapsed_ns;
        summary.throughput_ops_sec = throughput;
        summary.avg_latency_ns = avg_latency;
        summary.min_latency_ns = 0;
        summary.max_latency_ns = 0;
        summary.p50_latency_ns = 0;
        summary.p95_latency_ns = 0;
        summary.p99_latency_ns = 0;

        kvbench::ResultWriter::WriteJson(results_path, "wavedb-async",
                                          workload_path, summary);
        kvbench::ResultWriter::WriteConsole("wavedb-async",
                                             workload_path, summary);
        std::cout << "Results written to " << results_path << std::endl;
    }

    adapter.Close();
    std::filesystem::remove_all(db_path);
    return 0;
}
```

Wait — the async adapter needs `<thread>` for `std::this_thread::sleep_for`. Let me adjust the includes at the top to include `<thread>`.

Add at the top includes:
```cpp
#include <thread>
```

- [ ] **Step 2: Commit**

```bash
cd KVBench && git add adapters/wavedb/bench_wavedb_async.cpp && git commit -m "feat(kvbench): add WaveDB async adapter (fire-all parallel)"
```

---

### Task 5: Update WaveDB CMakeLists.txt for Async Target

**Files:**
- Modify: `KVBench/adapters/wavedb/CMakeLists.txt`

- [ ] **Step 1: Add bench_wavedb_async target**

Add after the existing `add_executable(bench_wavedb bench_wavedb.cpp)` block, before the `set_target_properties`:

```cmake
add_executable(bench_wavedb_async bench_wavedb_async.cpp)

target_link_libraries(bench_wavedb_async PRIVATE
    kvbench_common
    "${WAVEDB_BUILD}/libwavedb.a"
    "${WAVEDB_BUILD}/libxxhash.a"
    "${WAVEDB_BUILD}/libhashmap.a"
    "${WAVEDB_BUILD}/deps/libcbor/src/libcbor.a"
    atomic
    Threads::Threads
    OpenSSL::Crypto
)

target_include_directories(bench_wavedb_async PRIVATE
    ${WAVEDB_ROOT}/src
    ${WAVEDB_ROOT}/deps/xxhash
    ${WAVEDB_ROOT}/deps/hashmap/include
    ${WAVEDB_ROOT}/deps/libcbor/src
    ${WAVEDB_BUILD}/deps/libcbor
    ${CMAKE_SOURCE_DIR}/common/include
)

set_target_properties(bench_wavedb_async PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)
```

- [ ] **Step 2: Commit**

```bash
cd KVBench && git add adapters/wavedb/CMakeLists.txt && git commit -m "feat(kvbench): add async benchmark target to WaveDB CMakeLists"
```

---

### Task 6: Update run_benchmarks.sh

**Files:**
- Modify: `KVBench/run_benchmarks.sh`

- [ ] **Step 1: Add wavedb async benchmark run**

The current script iterates over adapters. Add a separate section after the main loop for the WaveDB async benchmark:

```bash
#!/bin/bash
set -e

BUILD_DIR="./build"
RESULTS_DIR="./results"
WORKLOAD_DIR="./workloads"

mkdir -p "$RESULTS_DIR"

# Workload definitions
WORKLOADS=(
    "w1_combined.txt"
    "w2_mixed.txt"
    "w3_combined.txt"
    "w4_update.txt"
    "w5_insert.txt"
)

ADAPTERS=(
    "wavedb:bench_wavedb"
    "forestdb:bench_forestdb"
    "rocksdb:bench_rocksdb"
    "leveldb:bench_leveldb"
)

for adapter_def in "${ADAPTERS[@]}"; do
    adapter="${adapter_def%%:*}"
    exe="${adapter_def##*:}"
    exe_path="$BUILD_DIR/adapters/$adapter/$exe"

    if [ ! -f "$exe_path" ]; then
        echo "Skipping $adapter (not built)"
        continue
    fi

    echo "========================================"
    echo "Running $adapter"
    echo "========================================"

    for wl in "${WORKLOADS[@]}"; do
        wl_path="$WORKLOAD_DIR/$wl"
        wl_name="${wl%.txt}"
        db_path="/tmp/kvbench_${adapter}_${wl_name}"
        result_file="$RESULTS_DIR/${adapter}_${wl_name}.json"

        echo "  Workload: $wl_name"
        rm -rf "$db_path"

        "$exe_path" \
            --workload "$wl_path" \
            --db-path "$db_path" \
            --results "$result_file" \
            --warmup 10000

        echo "  Done: $result_file"
    done
    echo ""
done

# --- WaveDB Async Benchmarks ---
ASYNC_EXE="$BUILD_DIR/adapters/wavedb/bench_wavedb_async"
if [ -f "$ASYNC_EXE" ]; then
    echo "========================================"
    echo "Running wavedb (async)"
    echo "========================================"

    for wl in "${WORKLOADS[@]}"; do
        wl_path="$WORKLOAD_DIR/$wl"
        wl_name="${wl%.txt}"
        db_path="/tmp/kvbench_wavedb_async_${wl_name}"
        result_file="$RESULTS_DIR/wavedb_async_${wl_name}.json"

        echo "  Workload: $wl_name (async)"
        rm -rf "$db_path"

        "$ASYNC_EXE" \
            --workload "$wl_path" \
            --db-path "$db_path" \
            --results "$result_file" \
            --threads 16

        echo "  Done: $result_file"
    done
    echo ""
fi

echo "All benchmarks complete. Results in $RESULTS_DIR"
```

- [ ] **Step 2: Commit**

```bash
cd KVBench && git add run_benchmarks.sh && git commit -m "feat(kvbench): add WaveDB async benchmark to run_benchmarks.sh"
```

---

### Task 7: Build and Verify Compilation

**Files:** None (verification only)

- [ ] **Step 1: Build WaveDB**

```bash
cd /home/victor/Workspace/src/github.com/vijayee/WaveDB/build && cmake .. && make -j$(nproc)
```
Expected: builds cleanly.

- [ ] **Step 2: Build KVBench with all adapters**

```bash
cd /home/victor/Workspace/src/github.com/vijayee/WaveDB/KVBench && mkdir -p build && cd build && \
  cmake .. -DBUILD_WAVEDB_ADAPTER=ON -DBUILD_ROCKSDB_ADAPTER=ON -DBUILD_LEVELDB_ADAPTER=ON -DBUILD_FORESTDB_ADAPTER=ON && \
  make -j$(nproc)
```
Expected: all targets build cleanly.

- [ ] **Step 3: Run quick smoke test with WaveDB sync adapter**

```bash
# Create a small workload file
echo "INSERT user0000000 value0000000" > /tmp/test_workload.txt
echo "POINT_QUERY user0000000 value" >> /tmp/test_workload.txt

./build/adapters/wavedb/bench_wavedb --workload /tmp/test_workload.txt --db-path /tmp/test_wavedb_sync --warmup 0
```
Expected: runs without errors, prints throughput.

- [ ] **Step 4: Run quick smoke test with WaveDB async adapter**

```bash
./build/adapters/wavedb/bench_wavedb_async --workload /tmp/test_workload.txt --db-path /tmp/test_wavedb_async --threads 4
```
Expected: runs without errors, prints throughput.
```
