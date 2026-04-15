//
// Write Amplification Benchmark for Phase 2 per-bnode CoW persistence.
//
// Measures:
//   - File size growth per update cycle (put + snapshot)
//   - Stale ratio after repeated updates
//   - Snapshot throughput (snapshots/sec)
//

#include <iostream>
#include <chrono>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <cstdio>

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

int main() {
    strcpy(tmpdir, "/tmp/wavedb_bench_phase2_XXXXXX");
    mkdtemp(tmpdir);

    database_t* db = create_db();
    if (db == nullptr) {
        std::cerr << "Failed to create database" << std::endl;
        return 1;
    }

    const int NUM_KEYS = 1000;
    const int NUM_UPDATES = 100;

    std::cout << "=== Phase 2 Write Amplification Benchmark ===" << std::endl;

    // Insert initial keys
    for (int i = 0; i < NUM_KEYS; i++) {
        char buf[64];
        snprintf(buf, sizeof(buf), "key%d", i);
        path_t* key = make_path(buf);
        identifier_t* val = make_value(buf);
        database_put_sync(db, key, val);
    }

    // Snapshot to flush everything
    database_snapshot(db);
    uint64_t file_size_before = page_file_size(db->page_file);
    double stale_before = page_file_stale_ratio(db->page_file);

    std::cout << "Initial state: file_size=" << file_size_before
              << " stale_ratio=" << stale_before << std::endl;

    // Update same key repeatedly, snapshot after each
    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < NUM_UPDATES; i++) {
        path_t* key = make_path("key0");
        char val_buf[64];
        snprintf(val_buf, sizeof(val_buf), "updated_val_%d", i);
        identifier_t* val = make_value(val_buf);
        database_put_sync(db, key, val);
        database_snapshot(db);
    }

    auto end = std::chrono::high_resolution_clock::now();
    uint64_t file_size_after = page_file_size(db->page_file);
    double stale_after = page_file_stale_ratio(db->page_file);

    uint64_t total_bytes_written = 0;
    if (file_size_after > file_size_before) {
        total_bytes_written = file_size_after - file_size_before;
    }

    double bytes_per_update = (total_bytes_written > 0)
        ? (double)total_bytes_written / (double)NUM_UPDATES
        : 0;

    std::cout << "\nAfter " << NUM_UPDATES << " updates to same key:" << std::endl;
    std::cout << "  file_size=" << file_size_after << std::endl;
    std::cout << "  total_bytes_written=" << total_bytes_written << std::endl;
    std::cout << "  bytes_per_update=" << bytes_per_update << std::endl;
    std::cout << "  stale_ratio=" << stale_after << std::endl;

    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
    double updates_per_sec = (duration.count() > 0)
        ? (double)NUM_UPDATES / (duration.count() / 1e6)
        : 0;
    std::cout << "  snapshot_throughput=" << updates_per_sec << " snapshots/sec" << std::endl;

    database_destroy(db);

    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", tmpdir);
    system(cmd);

    return 0;
}