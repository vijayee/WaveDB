//
// Benchmark: Concurrent Database Write Performance
// Tests concurrent write throughput with different LRU implementations
//

#include <gtest/gtest.h>
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>
#include <cstring>
#include <iomanip>
#include <sys/stat.h>
#include <unistd.h>
extern "C" {
#include "Database/database.h"
#include "Database/wal_manager.h"
#include "HBTrie/path.h"
#include "HBTrie/identifier.h"
#include "Buffer/buffer.h"
}

// Helper to create a path
static path_t* make_path(const char* key) {
    path_t* path = path_create();
    buffer_t* buf = buffer_create_from_pointer_copy((uint8_t*)key, strlen(key));
    identifier_t* id = identifier_create(buf, 0);
    buffer_destroy(buf);
    path_append(path, id);
    identifier_destroy(id);
    return path;
}

// Helper to create a value
static identifier_t* make_value(const char* data) {
    buffer_t* buf = buffer_create_from_pointer_copy((uint8_t*)data, strlen(data));
    identifier_t* id = identifier_create(buf, 0);
    buffer_destroy(buf);
    return id;
}

class ConcurrentWriteBenchmark : public ::testing::Test {
protected:
    void SetUp() override {
        // Create unique test directory
        snprintf(test_dir, sizeof(test_dir), "/tmp/bench_concurrent_write_%d", getpid());
        mkdir(test_dir, 0755);

        // Configure WAL for maximum performance (no fsync)
        wal_config_t wal_config = {
            .sync_mode = WAL_SYNC_ASYNC,
            .debounce_ms = 100,
            .idle_threshold_ms = 10000,
            .compact_interval_ms = 60000,
            .max_file_size = 100 * 1024 * 1024
        };

        // Create database with synchronous operations (no work pool)
        int error_code = 0;
        db = database_create(
            test_dir,
            50,                    // 50MB LRU cache
            &wal_config,
            0,                     // Default chunk size
            0,                     // Default btree node size
            1,                     // Enable persistent storage
            0,                     // Default storage cache size
            NULL,                  // No work pool (synchronous)
            NULL,                  // No timing wheel
            &error_code
        );
        ASSERT_NE(db, nullptr);
    }

    void TearDown() override {
        if (db) {
            database_destroy(db);
        }
        // Cleanup test directory
        char cmd[512];
        snprintf(cmd, sizeof(cmd), "rm -rf %s", test_dir);
        system(cmd);
    }

    database_t* db;
    char test_dir[256];
};

// Benchmark: Concurrent writes - 4 threads
TEST_F(ConcurrentWriteBenchmark, ConcurrentWrites4Threads) {
    const int NUM_THREADS = 4;
    const int OPS_PER_THREAD = 100;

    auto start = std::chrono::high_resolution_clock::now();

    std::vector<std::thread> threads;
    std::atomic<int> success_count(0);

    for (int t = 0; t < NUM_THREADS; t++) {
        threads.emplace_back([&, t]() {
            for (int i = 0; i < OPS_PER_THREAD; i++) {
                char key[64];
                snprintf(key, sizeof(key), "key_%d_%d", t, i);
                path_t* path = make_path(key);
                identifier_t* value = make_value("concurrent_write_value");

                database_put_sync(db, path, value);
                success_count++;
            }
        });
    }

    for (auto& t : threads) t.join();

    auto end = std::chrono::high_resolution_clock::now();
    auto duration_us = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();

    int total_ops = NUM_THREADS * OPS_PER_THREAD;
    double ops_per_sec = total_ops / (duration_us / 1000000.0);

    std::cout << "\n=== Concurrent Write (4 threads) ===" << std::endl;
    std::cout << "Threads: " << NUM_THREADS << ", Ops/thread: " << OPS_PER_THREAD << std::endl;
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "Total time: " << duration_us << " us" << std::endl;
    std::cout << "Throughput: " << (ops_per_sec / 1000.0) << "K ops/s" << std::endl;
}

// Benchmark: Concurrent writes - 8 threads
TEST_F(ConcurrentWriteBenchmark, ConcurrentWrites8Threads) {
    const int NUM_THREADS = 8;
    const int OPS_PER_THREAD = 100;

    auto start = std::chrono::high_resolution_clock::now();

    std::vector<std::thread> threads;
    std::atomic<int> success_count(0);

    for (int t = 0; t < NUM_THREADS; t++) {
        threads.emplace_back([&, t]() {
            for (int i = 0; i < OPS_PER_THREAD; i++) {
                char key[64];
                snprintf(key, sizeof(key), "key_%d_%d", t, i);
                path_t* path = make_path(key);
                identifier_t* value = make_value("concurrent_write_value");

                database_put_sync(db, path, value);
                success_count++;
            }
        });
    }

    for (auto& t : threads) t.join();

    auto end = std::chrono::high_resolution_clock::now();
    auto duration_us = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();

    int total_ops = NUM_THREADS * OPS_PER_THREAD;
    double ops_per_sec = total_ops / (duration_us / 1000000.0);

    std::cout << "\n=== Concurrent Write (8 threads) ===" << std::endl;
    std::cout << "Threads: " << NUM_THREADS << ", Ops/thread: " << OPS_PER_THREAD << std::endl;
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "Total time: " << duration_us << " us" << std::endl;
    std::cout << "Throughput: " << (ops_per_sec / 1000.0) << "K ops/s" << std::endl;
}

// Benchmark: Concurrent writes - 16 threads
TEST_F(ConcurrentWriteBenchmark, ConcurrentWrites16Threads) {
    const int NUM_THREADS = 16;
    const int OPS_PER_THREAD = 100;

    auto start = std::chrono::high_resolution_clock::now();

    std::vector<std::thread> threads;
    std::atomic<int> success_count(0);

    for (int t = 0; t < NUM_THREADS; t++) {
        threads.emplace_back([&, t]() {
            for (int i = 0; i < OPS_PER_THREAD; i++) {
                char key[64];
                snprintf(key, sizeof(key), "key_%d_%d", t, i);
                path_t* path = make_path(key);
                identifier_t* value = make_value("concurrent_write_value");

                database_put_sync(db, path, value);
                success_count++;
            }
        });
    }

    for (auto& t : threads) t.join();

    auto end = std::chrono::high_resolution_clock::now();
    auto duration_us = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();

    int total_ops = NUM_THREADS * OPS_PER_THREAD;
    double ops_per_sec = total_ops / (duration_us / 1000000.0);

    std::cout << "\n=== Concurrent Write (16 threads) ===" << std::endl;
    std::cout << "Threads: " << NUM_THREADS << ", Ops/thread: " << OPS_PER_THREAD << std::endl;
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "Total time: " << duration_us << " us" << std::endl;
    std::cout << "Throughput: " << (ops_per_sec / 1000.0) << "K ops/s" << std::endl;
}

// Benchmark: Concurrent writes - 32 threads
TEST_F(ConcurrentWriteBenchmark, ConcurrentWrites32Threads) {
    const int NUM_THREADS = 32;
    const int OPS_PER_THREAD = 100;

    auto start = std::chrono::high_resolution_clock::now();

    std::vector<std::thread> threads;
    std::atomic<int> success_count(0);

    for (int t = 0; t < NUM_THREADS; t++) {
        threads.emplace_back([&, t]() {
            for (int i = 0; i < OPS_PER_THREAD; i++) {
                char key[64];
                snprintf(key, sizeof(key), "key_%d_%d", t, i);
                path_t* path = make_path(key);
                identifier_t* value = make_value("concurrent_write_value");

                database_put_sync(db, path, value);
                success_count++;
            }
        });
    }

    for (auto& t : threads) t.join();

    auto end = std::chrono::high_resolution_clock::now();
    auto duration_us = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();

    int total_ops = NUM_THREADS * OPS_PER_THREAD;
    double ops_per_sec = total_ops / (duration_us / 1000000.0);

    std::cout << "\n=== Concurrent Write (32 threads) ===" << std::endl;
    std::cout << "Threads: " << NUM_THREADS << ", Ops/thread: " << OPS_PER_THREAD << std::endl;
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "Total time: " << duration_us << " us" << std::endl;
    std::cout << "Throughput: " << (ops_per_sec / 1000.0) << "K ops/s" << std::endl;
}

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}