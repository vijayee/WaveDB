//
// Write Amplification Benchmark for Phase 2 per-bnode CoW persistence.
//
// Measures file growth per snapshot under different access patterns:
//   1. Same-key updates: worst case for both approaches (same path rewritten)
//   2. Random-key updates: best case for Phase 2 (only dirty path rewritten)
//   3. Sequential-key updates: moderate case
//
// Phase 2 (dirty-only flush) should show significantly lower bytes_per_update
// for random-key updates compared to Phase 1 (all-nodes flush) because Phase 2
// only rewrites the modified path, not the entire trie.
//
// Usage: ./benchmark_write_amplification [num_keys [num_updates]]

// C++ headers must be included before extern "C" blocks
// because <atomic> contains templates requiring C++ linkage.
#include <atomic>
#include <iostream>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <random>

extern "C" {
#include "Database/database.h"
#include "Database/database_config.h"
#include "Storage/page_file.h"
#include "HBTrie/path.h"
#include "HBTrie/identifier.h"
#include "Buffer/buffer.h"
}

static char tmpdir[256];

static path_t* make_path(const char* key) {
    path_t* p = path_create();
    buffer_t* buf = buffer_create_from_pointer_copy((uint8_t*)key, strlen(key));
    identifier_t* id = identifier_create(buf, 0);
    buffer_destroy(buf);
    path_append(p, id);
    identifier_destroy(id);
    return p;
}

static identifier_t* make_value(const char* data) {
    buffer_t* buf = buffer_create_from_pointer_copy((uint8_t*)data, strlen(data));
    identifier_t* id = identifier_create(buf, 0);
    buffer_destroy(buf);
    return id;
}

static database_t* create_db() {
    database_config_t* config = database_config_default();
    config->enable_persist = 1;
    config->chunk_size = 4;
    config->btree_node_size = 4096;
    config->worker_threads = 2;
    config->timer_resolution_ms = 100;
    int error_code = 0;
    database_t* db = database_create_with_config(tmpdir, config, &error_code);
    database_config_destroy(config);
    return db;
}

struct BenchResult {
    uint64_t file_size_before;
    uint64_t file_size_after;
    uint64_t total_bytes_written;
    double bytes_per_update;
    double stale_ratio;
    double snapshots_per_sec;
    double duration_ms;
};

// Context for key/value generation functions
struct PatternCtx {
    int num_keys;
    int seq_offset;
    std::mt19937* rng;
    std::uniform_int_distribution<int>* dist;
};

typedef path_t* (*KeyFn)(int i, PatternCtx* ctx);
typedef identifier_t* (*ValFn)(int i, PatternCtx* ctx);

static BenchResult run_pattern(database_t* db, int num_updates,
                                const char* pattern_name,
                                KeyFn key_fn, ValFn val_fn,
                                PatternCtx* ctx) {
    // Snapshot to establish baseline
    database_snapshot(db);
    uint64_t file_size_before = (db->page_file != nullptr) ? page_file_size(db->page_file) : 0;

    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < num_updates; i++) {
        path_t* key = key_fn(i, ctx);
        identifier_t* val = val_fn(i, ctx);
        database_put_sync(db, key, val);
        database_snapshot(db);
    }

    auto end = std::chrono::high_resolution_clock::now();
    uint64_t file_size_after = (db->page_file != nullptr) ? page_file_size(db->page_file) : 0;
    double stale_ratio = (db->page_file != nullptr) ? page_file_stale_ratio(db->page_file) : 0.0;

    uint64_t total_bytes = 0;
    if (file_size_after > file_size_before) {
        total_bytes = file_size_after - file_size_before;
    }

    double bytes_per_update = (num_updates > 0)
        ? (double)total_bytes / (double)num_updates : 0;

    auto duration_us = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
    double duration_ms = duration_us.count() / 1e3;
    double snaps_per_sec = (duration_us.count() > 0)
        ? (double)num_updates / (duration_us.count() / 1e6) : 0;

    std::cout << "\n--- " << pattern_name << " ---" << std::endl;
    std::cout << "  file_size_before = " << file_size_before << std::endl;
    std::cout << "  file_size_after  = " << file_size_after << std::endl;
    std::cout << "  total_bytes_written = " << total_bytes << std::endl;
    std::cout << "  bytes_per_update = " << bytes_per_update << std::endl;
    std::cout << "  stale_ratio      = " << stale_ratio << std::endl;
    std::cout << "  snapshots_per_sec = " << snaps_per_sec << std::endl;
    std::cout << "  duration_ms      = " << duration_ms << std::endl;

    BenchResult r;
    r.file_size_before = file_size_before;
    r.file_size_after = file_size_after;
    r.total_bytes_written = total_bytes;
    r.bytes_per_update = bytes_per_update;
    r.stale_ratio = stale_ratio;
    r.snapshots_per_sec = snaps_per_sec;
    r.duration_ms = duration_ms;
    return r;
}

// Pattern 1: Same-key updates
static path_t* same_key_fn(int, PatternCtx*) {
    return make_path("key00000000");
}
static identifier_t* same_val_fn(int i, PatternCtx*) {
    char buf[64];
    snprintf(buf, sizeof(buf), "updated_%d", i);
    return make_value(buf);
}

// Pattern 2: Random-key updates
static path_t* random_key_fn(int, PatternCtx* ctx) {
    int k = (*ctx->dist)(*ctx->rng);
    char buf[64];
    snprintf(buf, sizeof(buf), "key%08d", k);
    return make_path(buf);
}
static identifier_t* random_val_fn(int i, PatternCtx*) {
    char buf[64];
    snprintf(buf, sizeof(buf), "randval_%d", i);
    return make_value(buf);
}

// Pattern 3: Sequential-key updates
static path_t* seq_key_fn(int i, PatternCtx* ctx) {
    char buf[64];
    snprintf(buf, sizeof(buf), "key%08d", ctx->seq_offset + i);
    return make_path(buf);
}
static identifier_t* seq_val_fn(int i, PatternCtx*) {
    char buf[64];
    snprintf(buf, sizeof(buf), "seqval_%d", i);
    return make_value(buf);
}

int main(int argc, char* argv[]) {
    int NUM_KEYS = 10000;
    int NUM_UPDATES = 50;

    if (argc > 1) NUM_KEYS = atoi(argv[1]);
    if (argc > 2) NUM_UPDATES = atoi(argv[2]);

    strcpy(tmpdir, "/tmp/wavedb_bench_phase2_XXXXXX");
    mkdtemp(tmpdir);

    database_t* db = create_db();
    if (db == nullptr) {
        std::cerr << "Failed to create database" << std::endl;
        return 1;
    }

    std::mt19937 rng(42);
    std::uniform_int_distribution<int> dist(0, NUM_KEYS - 1);

    std::cout << "=== Phase 2 Write Amplification Benchmark ===" << std::endl;
    std::cout << "num_keys=" << NUM_KEYS << " num_updates=" << NUM_UPDATES
              << " chunk_size=4 btree_node_size=4096" << std::endl;
    std::cout << "page_file=" << (void*)db->page_file << std::endl;

    // Bulk insert initial keys
    auto bulk_start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < NUM_KEYS; i++) {
        char buf[64];
        snprintf(buf, sizeof(buf), "key%08d", i);
        path_t* key = make_path(buf);
        identifier_t* val = make_value(buf);
        database_put_sync(db, key, val);
    }
    auto bulk_end = std::chrono::high_resolution_clock::now();
    auto bulk_ms = std::chrono::duration_cast<std::chrono::microseconds>(bulk_end - bulk_start).count() / 1e3;
    std::cout << "\nBulk insert: " << NUM_KEYS << " keys in " << bulk_ms << " ms" << std::endl;

    // Pattern 1: Same-key updates (worst case for CoW)
    {
        PatternCtx ctx = {NUM_KEYS, 0, nullptr, nullptr};
        run_pattern(db, NUM_UPDATES, "Same-key updates",
                    same_key_fn, same_val_fn, &ctx);
    }

    // Pattern 2: Random-key updates (best case for Phase 2)
    {
        PatternCtx ctx = {NUM_KEYS, 0, &rng, &dist};
        run_pattern(db, NUM_UPDATES, "Random-key updates",
                    random_key_fn, random_val_fn, &ctx);
    }

    // Pattern 3: Sequential-key updates (moderate case)
    {
        PatternCtx ctx = {NUM_KEYS, NUM_KEYS / 2, nullptr, nullptr};
        run_pattern(db, NUM_UPDATES, "Sequential-key updates",
                    seq_key_fn, seq_val_fn, &ctx);
    }

    // Verify data persistence without WAL
    database_snapshot(db);
    database_destroy(db);
    db = nullptr;

    // Delete WAL files and manifest to force page-file-only load
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -f %s/*.wal %s/manifest.dat", tmpdir, tmpdir);
    system(cmd);

    // Reopen and verify a few keys
    db = create_db();
    if (db != nullptr) {
        int verified = 0;
        for (int i = 0; i < 10; i++) {
            char buf[64];
            snprintf(buf, sizeof(buf), "key%08d", i * 1000);
            path_t* key = make_path(buf);
            identifier_t* result = nullptr;
            int rc = database_get_sync(db, key, &result);
            if (rc == 0 && result != nullptr) {
                verified++;
                identifier_destroy(result);
            }
        }
        std::cout << "\nPersistence verification (without WAL): "
                  << verified << "/10 keys found" << std::endl;
    }

    database_destroy(db);

    snprintf(cmd, sizeof(cmd), "rm -rf %s", tmpdir);
    system(cmd);

    return 0;
}