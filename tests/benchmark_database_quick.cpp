//
// Quick Database Benchmark: Sharded vs Lock-Free LRU
//

#include <gtest/gtest.h>
#include <cstring>
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>
#include <iomanip>
#include <sstream>
extern "C" {
#include "Database/database.h"
#include "HBTrie/path.h"
#include "HBTrie/identifier.h"
#include "Buffer/buffer.h"
#include "Workers/pool.h"
#include "Time/wheel.h"
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

// Timing helper
class Timer {
public:
    void start() { start_ = std::chrono::high_resolution_clock::now(); }
    void stop() { end_ = std::chrono::high_resolution_clock::now(); }
    double microseconds() const {
        return std::chrono::duration<double, std::micro>(end_ - start_).count();
    }
    double milliseconds() const {
        return std::chrono::duration<double, std::milli>(end_ - start_).count();
    }
private:
    std::chrono::high_resolution_clock::time_point start_, end_;
};

class DatabaseBenchmark : public ::testing::Test {
protected:
    void SetUp() override {
        char tmpdir[] = "/tmp/wavedb_bench_XXXXXX";
        mkdtemp(tmpdir);
        db_path = std::string(tmpdir);

        // Create work pool and timing wheel
        pool = work_pool_create(4);
        ASSERT_NE(pool, nullptr);

        wheel = hierarchical_timing_wheel_create(1000, pool);
        ASSERT_NE(wheel, nullptr);

        int error = 0;
        db = database_create(db_path.c_str(), 0, NULL, 4, 4096, 0, 0, pool, wheel, &error);
        ASSERT_NE(db, nullptr);
        ASSERT_EQ(error, 0);
    }

    void TearDown() override {
        // Stop wheel and pool before destroying database
        if (wheel) {
            hierarchical_timing_wheel_stop(wheel);
        }
        if (pool) {
            work_pool_shutdown(pool);
            work_pool_join_all(pool);
        }
        if (db) {
            database_destroy(db);
        }
        if (wheel) {
            hierarchical_timing_wheel_destroy(wheel);
        }
        if (pool) {
            work_pool_destroy(pool);
        }
        // Cleanup temp directory
        std::string cmd = "rm -rf " + db_path;
        system(cmd.c_str());
    }

    void populateDatabase(int count) {
        for (int i = 0; i < count; i++) {
            char key[32], val[32];
            snprintf(key, sizeof(key), "key%06d", i);
            snprintf(val, sizeof(val), "value%06d", i);
            path_t* path = make_path(key);
            identifier_t* value = make_value(val);
            database_put_sync(db, path, value);
            // database_put_sync takes ownership of path and value
        }
    }

    database_t* db;
    work_pool_t* pool;
    hierarchical_timing_wheel_t* wheel;
    std::string db_path;
};

// Benchmark: Sequential reads
TEST_F(DatabaseBenchmark, SequentialReads) {
    const int NUM_ENTRIES = 1000;
    const int ITERATIONS = 10000;

    populateDatabase(NUM_ENTRIES);

    Timer timer;
    int found_count = 0;

    timer.start();
    for (int i = 0; i < ITERATIONS; i++) {
        char key[32];
        snprintf(key, sizeof(key), "key%06d", i % NUM_ENTRIES);
        path_t* path = make_path(key);
        identifier_t* found = nullptr;
        int result = database_get_sync(db, path, &found);
        if (result == 0 && found) {
            found_count++;
            identifier_destroy(found);
        }
        // database_get_sync takes ownership of path
    }
    timer.stop();

    double time_us = timer.microseconds();
    double ops = ITERATIONS / (time_us / 1000000.0);

    std::cout << "\n=== Sequential Read Benchmark ===" << std::endl;
    std::cout << "Entries: " << NUM_ENTRIES << ", Iterations: " << ITERATIONS << std::endl;
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "Time: " << time_us << " us (" << (time_us / ITERATIONS) << " us/op)" << std::endl;
    std::cout << "Throughput: " << (ops / 1000.0) << "K ops/sec" << std::endl;
    std::cout << "Found: " << found_count << "/" << ITERATIONS << std::endl;
}

// Benchmark: Sequential writes
TEST_F(DatabaseBenchmark, SequentialWrites) {
    const int ITERATIONS = 1000;

    Timer timer;

    timer.start();
    for (int i = 0; i < ITERATIONS; i++) {
        char key[32], val[32];
        snprintf(key, sizeof(key), "write%06d", i);
        snprintf(val, sizeof(val), "value%06d", i);
        path_t* path = make_path(key);
        identifier_t* value = make_value(val);
        database_put_sync(db, path, value);
        // database_put_sync takes ownership of path and value
    }
    timer.stop();

    double time_us = timer.microseconds();
    double ops = ITERATIONS / (time_us / 1000000.0);

    std::cout << "\n=== Sequential Write Benchmark ===" << std::endl;
    std::cout << "Iterations: " << ITERATIONS << std::endl;
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "Time: " << time_us << " us (" << (time_us / ITERATIONS) << " us/op)" << std::endl;
    std::cout << "Throughput: " << (ops / 1000.0) << "K ops/sec" << std::endl;
}

// Benchmark: Concurrent reads
TEST_F(DatabaseBenchmark, ConcurrentReads) {
    const int NUM_ENTRIES = 1000;
    const int NUM_THREADS = 8;
    const int OPS_PER_THREAD = 500;

    populateDatabase(NUM_ENTRIES);

    Timer timer;
    std::atomic<int> found_count(0);

    timer.start();
    {
        std::vector<std::thread> threads;
        for (int t = 0; t < NUM_THREADS; t++) {
            threads.emplace_back([&, t]() {
                unsigned int seed = t * 12345;
                for (int i = 0; i < OPS_PER_THREAD; i++) {
                    int key_idx = rand_r(&seed) % NUM_ENTRIES;
                    char key[32];
                    snprintf(key, sizeof(key), "key%06d", key_idx);
                    path_t* path = make_path(key);
                    identifier_t* found = nullptr;
                    int result = database_get_sync(db, path, &found);
                    if (result == 0 && found) {
                        found_count++;
                        identifier_destroy(found);
                    }
                    // database_get_sync takes ownership of path
                }
            });
        }
        for (auto& t : threads) t.join();
    }
    timer.stop();

    int total_ops = NUM_THREADS * OPS_PER_THREAD;
    double time_us = timer.microseconds();
    double ops = total_ops / (time_us / 1000000.0);

    std::cout << "\n=== Concurrent Read Benchmark ===" << std::endl;
    std::cout << "Threads: " << NUM_THREADS << ", Ops/thread: " << OPS_PER_THREAD << std::endl;
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "Time: " << time_us << " us" << std::endl;
    std::cout << "Throughput: " << (ops / 1000.0) << "K ops/sec" << std::endl;
    std::cout << "Found: " << found_count.load() << "/" << total_ops << std::endl;
}

// Benchmark: Mixed workload (reads and writes)
TEST_F(DatabaseBenchmark, MixedWorkload) {
    const int NUM_ENTRIES = 1000;
    const int NUM_THREADS = 8;
    const int OPS_PER_THREAD = 500;

    populateDatabase(NUM_ENTRIES);

    Timer timer;
    std::atomic<int> read_count(0);
    std::atomic<int> write_count(0);

    timer.start();
    {
        std::vector<std::thread> threads;
        for (int t = 0; t < NUM_THREADS; t++) {
            threads.emplace_back([&, t]() {
                unsigned int seed = t * 12345;
                for (int i = 0; i < OPS_PER_THREAD; i++) {
                    int key_idx = rand_r(&seed) % NUM_ENTRIES;
                    char key[32];
                    snprintf(key, sizeof(key), "key%06d", key_idx);

                    // 80% reads, 20% writes
                    if ((rand_r(&seed) % 100) < 80) {
                        path_t* path = make_path(key);
                        identifier_t* found = nullptr;
                        int result = database_get_sync(db, path, &found);
                        if (result == 0 && found) {
                            read_count++;
                            identifier_destroy(found);
                        }
                        // database_get_sync takes ownership of path
                    } else {
                        char val[32];
                        snprintf(val, sizeof(val), "updated_%d", i);
                        path_t* path = make_path(key);
                        identifier_t* value = make_value(val);
                        database_put_sync(db, path, value);
                        write_count++;
                        // database_put_sync takes ownership of path and value
                    }
                }
            });
        }
        for (auto& t : threads) t.join();
    }
    timer.stop();

    int total_ops = NUM_THREADS * OPS_PER_THREAD;
    double time_us = timer.microseconds();
    double ops = total_ops / (time_us / 1000000.0);

    std::cout << "\n=== Mixed Workload Benchmark ===" << std::endl;
    std::cout << "Threads: " << NUM_THREADS << ", Ops/thread: " << OPS_PER_THREAD << std::endl;
    std::cout << "Read ratio: 80%" << std::endl;
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "Time: " << time_us << " us" << std::endl;
    std::cout << "Throughput: " << (ops / 1000.0) << "K ops/sec" << std::endl;
    std::cout << "Reads: " << read_count.load() << ", Writes: " << write_count.load() << std::endl;
}

// Benchmark: High contention
TEST_F(DatabaseBenchmark, HighContention) {
    const int NUM_ENTRIES = 100;  // Few keys = high contention
    const int NUM_THREADS = 16;
    const int OPS_PER_THREAD = 500;

    populateDatabase(NUM_ENTRIES);

    Timer timer;
    std::atomic<int> found_count(0);

    timer.start();
    {
        std::vector<std::thread> threads;
        for (int t = 0; t < NUM_THREADS; t++) {
            threads.emplace_back([&, t]() {
                unsigned int seed = t * 12345;
                for (int i = 0; i < OPS_PER_THREAD; i++) {
                    int key_idx = rand_r(&seed) % NUM_ENTRIES;
                    char key[32];
                    snprintf(key, sizeof(key), "key%06d", key_idx);
                    path_t* path = make_path(key);
                    identifier_t* found = nullptr;
                    int result = database_get_sync(db, path, &found);
                    if (result == 0 && found) {
                        found_count++;
                        identifier_destroy(found);
                    }
                    // database_get_sync takes ownership of path
                }
            });
        }
        for (auto& t : threads) t.join();
    }
    timer.stop();

    int total_ops = NUM_THREADS * OPS_PER_THREAD;
    double time_us = timer.microseconds();
    double ops = total_ops / (time_us / 1000000.0);

    std::cout << "\n=== High Contention Benchmark ===" << std::endl;
    std::cout << "Threads: " << NUM_THREADS << ", Keys: " << NUM_ENTRIES << std::endl;
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "Time: " << time_us << " us" << std::endl;
    std::cout << "Throughput: " << (ops / 1000.0) << "K ops/sec" << std::endl;
    std::cout << "Found: " << found_count.load() << "/" << total_ops << std::endl;
}

// Summary
TEST_F(DatabaseBenchmark, Summary) {
    std::cout << "\n" << std::string(60, '=') << std::endl;
    std::cout << "DATABASE BENCHMARK SUMMARY" << std::endl;
    std::cout << std::string(60, '=') << std::endl;
    std::cout << "\nLRU Configuration: ";
#if USE_LOCKFREE_LRU
    std::cout << "Lock-Free LRU (USE_LOCKFREE_LRU=1)" << std::endl;
#else
    std::cout << "Sharded LRU (USE_LOCKFREE_LRU=0)" << std::endl;
#endif
    std::cout << std::string(60, '=') << std::endl;
}