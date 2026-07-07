//
// Vacuum performance microbenchmark.
//
// Measures:
//   1. Initial load throughput (N keys)
//   2. K overwrite passes (each CoW's bnodes, growing the page file with stale
//      regions reclaimed only by vacuum)
//   3. database_vacuum() wall-clock time + resulting file size
//   4. Post-vacuum write throughput (verifies the file is small again so
//      subsequent writes are not paying for a bloated file)
//
// Usage: ./benchmark_vacuum [N=10000] [K=20]
//
// The put() helper mirrors the established pattern in tests/test_vacuum.cpp:
// identifier_create(buf, 0) (chunk_size=0 => default), path_append + destroy,
// database_put_sync (which consumes both path and value).

#if _WIN32
#include <io.h>
#include <direct.h>
#include <process.h>
#define getpid() _getpid()
#define mkdir(path, mode) _mkdir(path)
#else
#include <unistd.h>
#include <sys/stat.h>
#endif
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
// Include <atomic> BEFORE the extern "C" block: atomic_compat.h pulls in
// <atomic> when compiled as C++, and C++ templates cannot be defined inside
// an extern "C" region. Matching the include order used by the existing
// benchmarks (e.g. tests/benchmark_database_quick.cpp) and test_vacuum.cpp.
#include <atomic>
extern "C" {
#include "Database/database.h"
#include "Database/database_config.h"
#include "HBTrie/path.h"
#include "HBTrie/identifier.h"
#include "Buffer/buffer.h"
}

static double now_seconds() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

static uint64_t file_size(const std::string& path) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return 0;
    return (uint64_t)st.st_size;
}

int main(int argc, char** argv) {
    int N = argc > 1 ? atoi(argv[1]) : 10000;
    int K = argc > 2 ? atoi(argv[2]) : 20;  // overwrite passes

    std::string dir = "/tmp/wavedb_bench_vacuum_" + std::to_string(getpid());
    mkdir(dir.c_str(), 0700);
    std::string dbpath = dir;  // location is a directory; data file is dir/data.wdbp

    database_config_t* cfg = database_config_default();
    database_config_set_sync_only(cfg, 1);
    // Bypass min-size gates so vacuum runs even on our (relatively) small DB.
    cfg->vacuum_config.min_file_size_bytes = 0;
    cfg->vacuum_config.min_stale_bytes = 0;

    int err = 0;
    database_t* db = database_create_with_config(dbpath.c_str(), cfg, &err);
    database_config_destroy(cfg);
    if (db == NULL) {
        fprintf(stderr, "failed to create db: err=%d\n", err);
        return 1;
    }

    // put() builds a single-component path from the key string and a value
    // identifier from the value string, then calls database_put_sync which
    // consumes both. Mirrors tests/test_vacuum.cpp's VacuumTest::put.
    auto put = [&](const std::string& k, const std::string& v) {
        path_t* p = path_create();
        buffer_t* kbuf = buffer_create_from_pointer_copy(
            (uint8_t*)k.data(), k.size());
        identifier_t* kid = identifier_create(kbuf, 0);
        buffer_destroy(kbuf);
        path_append(p, kid);
        identifier_destroy(kid);

        buffer_t* vbuf = buffer_create_from_pointer_copy(
            (uint8_t*)v.data(), v.size());
        identifier_t* vid = identifier_create(vbuf, 0);
        buffer_destroy(vbuf);

        int rc = database_put_sync(db, p, vid);
        if (rc != 0) {
            fprintf(stderr, "database_put_sync failed: rc=%d key=%s\n", rc, k.c_str());
        }
    };

    // ---- Initial load ----
    double t0 = now_seconds();
    for (int i = 0; i < N; i++) {
        put("k/" + std::to_string(i), "v0");
    }
    database_flush_dirty_bnodes(db);
    double init_t = now_seconds() - t0;
    uint64_t init_size = file_size(dbpath + "/data.wdbp");
    printf("initial load N=%d: %.3fs, %llu bytes, %.0f ops/s\n",
           N, init_t, (unsigned long long)init_size,
           init_t > 0 ? N / init_t : 0.0);

    // ---- Overwrite passes ----
    for (int rep = 1; rep <= K; rep++) {
        double t1 = now_seconds();
        for (int i = 0; i < N; i++) {
            put("k/" + std::to_string(i), "v" + std::to_string(rep));
        }
        database_flush_dirty_bnodes(db);
        double pass_t = now_seconds() - t1;
        uint64_t sz = file_size(dbpath + "/data.wdbp");
        printf("overwrite pass %d: %.3fs, %llu bytes, %.0f ops/s\n",
               rep, pass_t, (unsigned long long)sz,
               pass_t > 0 ? N / pass_t : 0.0);
    }

    uint64_t before_vacuum = file_size(dbpath + "/data.wdbp");

    // ---- Vacuum ----
    double t2 = now_seconds();
    int rc = database_vacuum(db);
    double vac_t = now_seconds() - t2;
    uint64_t after = file_size(dbpath + "/data.wdbp");
    printf("vacuum: %.3fs, %llu bytes (rc=%d, was %llu bytes, shrank %.1f%%)\n",
           vac_t, (unsigned long long)after, rc,
           (unsigned long long)before_vacuum,
           before_vacuum > 0
               ? 100.0 * (double)(before_vacuum - after) / (double)before_vacuum
               : 0.0);

    // ---- Post-vacuum write throughput ----
    double t3 = now_seconds();
    for (int i = 0; i < N; i++) {
        put("k/" + std::to_string(i), "post");
    }
    database_flush_dirty_bnodes(db);
    double post_t = now_seconds() - t3;
    uint64_t post_size = file_size(dbpath + "/data.wdbp");
    printf("post-vacuum write: %.3fs, %llu bytes, %.0f ops/s\n",
           post_t, (unsigned long long)post_size,
           post_t > 0 ? N / post_t : 0.0);

    database_destroy(db);
    std::string cmd = "rm -rf " + dir;
    system(cmd.c_str());
    return 0;
}