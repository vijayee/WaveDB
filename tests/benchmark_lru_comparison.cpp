//
// Benchmark comparison: Sharded LRU vs Lock-Free LRU
//

#include <gtest/gtest.h>
#include <cstring>
#include <thread>
#include <vector>
#include <atomic>
#include <random>
#include <chrono>
#include <iomanip>
#include <sstream>
extern "C" {
#include "Database/database_lru.h"
#include "Database/lockfree_lru.h"
#include "HBTrie/path.h"
#include "HBTrie/identifier.h"
#include "Buffer/buffer.h"
}

// Helper to create a simple path
static path_t* make_simple_path(const char* key) {
    path_t* path = path_create();
    buffer_t* buf = buffer_create_from_pointer_copy((uint8_t*)key, strlen(key));
    identifier_t* id = identifier_create(buf, 0);
    buffer_destroy(buf);
    path_append(path, id);
    identifier_destroy(id);
    return path;
}

// Helper to create a simple value
static identifier_t* make_simple_value(const char* data) {
    buffer_t* buf = buffer_create_from_pointer_copy((uint8_t*)data, strlen(data));
    identifier_t* id = identifier_create(buf, 0);
    buffer_destroy(buf);
    return id;
}

// Pre-populate cache with entries
static void populate_sharded_cache(database_lru_cache_t* lru, int count) {
    for (int i = 0; i < count; i++) {
        char key[32];
        snprintf(key, sizeof(key), "key%d", i);
        path_t* path = make_simple_path(key);
        identifier_t* value = make_simple_value("value");
        database_lru_cache_put(lru, path, value);
    }
}

static void populate_lockfree_cache(lockfree_lru_cache_t* lru, int count) {
    for (int i = 0; i < count; i++) {
        char key[32];
        snprintf(key, sizeof(key), "key%d", i);
        path_t* path = make_simple_path(key);
        identifier_t* value = make_simple_value("value");
        lockfree_lru_cache_put(lru, path, value);
    }
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

class LRUBenchmark : public ::testing::Test {
protected:
    void SetUp() override {
        sharded_lru = database_lru_cache_create(10 * 1024 * 1024, 0);
        lockfree_lru = lockfree_lru_cache_create(10 * 1024 * 1024, 0);
        ASSERT_NE(sharded_lru, nullptr);
        ASSERT_NE(lockfree_lru, nullptr);
    }

    void TearDown() override {
        if (sharded_lru) database_lru_cache_destroy(sharded_lru);
        if (lockfree_lru) lockfree_lru_cache_destroy(lockfree_lru);
    }

    database_lru_cache_t* sharded_lru;
    lockfree_lru_cache_t* lockfree_lru;
};

// Benchmark: Sequential gets
TEST_F(LRUBenchmark, SequentialGets) {
    const int NUM_ENTRIES = 1000;
    const int ITERATIONS = 10000;

    populate_sharded_cache(sharded_lru, NUM_ENTRIES);
    populate_lockfree_cache(lockfree_lru, NUM_ENTRIES);

    Timer timer;

    // Sharded LRU
    timer.start();
    for (int i = 0; i < ITERATIONS; i++) {
        char key[32];
        snprintf(key, sizeof(key), "key%d", i % NUM_ENTRIES);
        path_t* path = make_simple_path(key);
        identifier_t* cached = database_lru_cache_get(sharded_lru, path);
        if (cached) identifier_destroy(cached);
        path_destroy(path);
    }
    timer.stop();
    double sharded_time = timer.microseconds();

    // Lock-Free LRU
    timer.start();
    for (int i = 0; i < ITERATIONS; i++) {
        char key[32];
        snprintf(key, sizeof(key), "key%d", i % NUM_ENTRIES);
        path_t* path = make_simple_path(key);
        identifier_t* cached = lockfree_lru_cache_get(lockfree_lru, path);
        if (cached) identifier_destroy(cached);
        path_destroy(path);
    }
    timer.stop();
    double lockfree_time = timer.microseconds();

    std::cout << "\n=== Sequential Get Benchmark ===" << std::endl;
    std::cout << "Iterations: " << ITERATIONS << std::endl;
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "Sharded LRU:   " << sharded_time << " us (" << (sharded_time / ITERATIONS) << " us/op)" << std::endl;
    std::cout << "Lock-Free LRU: " << lockfree_time << " us (" << (lockfree_time / ITERATIONS) << " us/op)" << std::endl;
    std::cout << "Improvement:   " << ((sharded_time - lockfree_time) / sharded_time * 100) << "%" << std::endl;
}

// Benchmark: Concurrent gets
TEST_F(LRUBenchmark, ConcurrentGets) {
    const int NUM_ENTRIES = 1000;
    const int NUM_THREADS = 8;
    const int OPS_PER_THREAD = 1000;

    populate_sharded_cache(sharded_lru, NUM_ENTRIES);
    populate_lockfree_cache(lockfree_lru, NUM_ENTRIES);

    Timer timer;

    // Sharded LRU
    timer.start();
    {
        std::vector<std::thread> threads;
        std::atomic<int> total_count(0);
        for (int t = 0; t < NUM_THREADS; t++) {
            threads.emplace_back([&]() {
                for (int i = 0; i < OPS_PER_THREAD; i++) {
                    char key[32];
                    snprintf(key, sizeof(key), "key%d", i % NUM_ENTRIES);
                    path_t* path = make_simple_path(key);
                    identifier_t* cached = database_lru_cache_get(sharded_lru, path);
                    if (cached) {
                        total_count++;
                        identifier_destroy(cached);
                    }
                    path_destroy(path);
                }
            });
        }
        for (auto& t : threads) t.join();
    }
    timer.stop();
    double sharded_time = timer.microseconds();

    // Lock-Free LRU
    timer.start();
    {
        std::vector<std::thread> threads;
        std::atomic<int> total_count(0);
        for (int t = 0; t < NUM_THREADS; t++) {
            threads.emplace_back([&]() {
                for (int i = 0; i < OPS_PER_THREAD; i++) {
                    char key[32];
                    snprintf(key, sizeof(key), "key%d", i % NUM_ENTRIES);
                    path_t* path = make_simple_path(key);
                    identifier_t* cached = lockfree_lru_cache_get(lockfree_lru, path);
                    if (cached) {
                        total_count++;
                        identifier_destroy(cached);
                    }
                    path_destroy(path);
                }
            });
        }
        for (auto& t : threads) t.join();
    }
    timer.stop();
    double lockfree_time = timer.microseconds();

    std::cout << "\n=== Concurrent Get Benchmark ===" << std::endl;
    std::cout << "Threads: " << NUM_THREADS << ", Ops/thread: " << OPS_PER_THREAD << std::endl;
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "Sharded LRU:   " << sharded_time << " us (" << (sharded_time / (NUM_THREADS * OPS_PER_THREAD)) << " us/op)" << std::endl;
    std::cout << "Lock-Free LRU: " << lockfree_time << " us (" << (lockfree_time / (NUM_THREADS * OPS_PER_THREAD)) << " us/op)" << std::endl;
    std::cout << "Improvement:   " << ((sharded_time - lockfree_time) / sharded_time * 100) << "%" << std::endl;
}

// Benchmark: Read-heavy workload (90% reads, 10% writes)
TEST_F(LRUBenchmark, ReadHeavyWorkload) {
    const int NUM_ENTRIES = 1000;
    const int NUM_THREADS = 8;
    const int OPS_PER_THREAD = 500;

    populate_sharded_cache(sharded_lru, NUM_ENTRIES);
    populate_lockfree_cache(lockfree_lru, NUM_ENTRIES);

    Timer timer;
    std::atomic<int> write_counter(0);

    // Sharded LRU
    timer.start();
    {
        std::vector<std::thread> threads;
        for (int t = 0; t < NUM_THREADS; t++) {
            threads.emplace_back([&]() {
                std::random_device rd;
                std::mt19937 gen(rd());
                std::uniform_int_distribution<> dis(0, 99);

                for (int i = 0; i < OPS_PER_THREAD; i++) {
                    int op = dis(gen);
                    if (op < 90) {
                        // Read
                        int key = dis(gen) % NUM_ENTRIES;
                        char key_str[32];
                        snprintf(key_str, sizeof(key_str), "key%d", key);
                        path_t* path = make_simple_path(key_str);
                        identifier_t* cached = database_lru_cache_get(sharded_lru, path);
                        if (cached) identifier_destroy(cached);
                        path_destroy(path);
                    } else {
                        // Write
                        int key = write_counter.fetch_add(1);
                        char key_str[32];
                        snprintf(key_str, sizeof(key_str), "newkey%d", key);
                        path_t* path = make_simple_path(key_str);
                        identifier_t* value = make_simple_value("value");
                        database_lru_cache_put(sharded_lru, path, value);
                    }
                }
            });
        }
        for (auto& t : threads) t.join();
    }
    timer.stop();
    double sharded_time = timer.microseconds();

    write_counter = 0;

    // Lock-Free LRU
    timer.start();
    {
        std::vector<std::thread> threads;
        for (int t = 0; t < NUM_THREADS; t++) {
            threads.emplace_back([&]() {
                std::random_device rd;
                std::mt19937 gen(rd());
                std::uniform_int_distribution<> dis(0, 99);

                for (int i = 0; i < OPS_PER_THREAD; i++) {
                    int op = dis(gen);
                    if (op < 90) {
                        // Read
                        int key = dis(gen) % NUM_ENTRIES;
                        char key_str[32];
                        snprintf(key_str, sizeof(key_str), "key%d", key);
                        path_t* path = make_simple_path(key_str);
                        identifier_t* cached = lockfree_lru_cache_get(lockfree_lru, path);
                        if (cached) identifier_destroy(cached);
                        path_destroy(path);
                    } else {
                        // Write
                        int key = write_counter.fetch_add(1);
                        char key_str[32];
                        snprintf(key_str, sizeof(key_str), "newkey%d", key);
                        path_t* path = make_simple_path(key_str);
                        identifier_t* value = make_simple_value("value");
                        lockfree_lru_cache_put(lockfree_lru, path, value);
                    }
                }
            });
        }
        for (auto& t : threads) t.join();
    }
    timer.stop();
    double lockfree_time = timer.microseconds();

    std::cout << "\n=== Read-Heavy Workload (90% reads) ===" << std::endl;
    std::cout << "Threads: " << NUM_THREADS << ", Ops/thread: " << OPS_PER_THREAD << std::endl;
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "Sharded LRU:   " << sharded_time << " us" << std::endl;
    std::cout << "Lock-Free LRU: " << lockfree_time << " us" << std::endl;
    std::cout << "Improvement:   " << ((sharded_time - lockfree_time) / sharded_time * 100) << "%" << std::endl;
}

// Benchmark: Write-heavy workload
TEST_F(LRUBenchmark, WriteHeavyWorkload) {
    const int NUM_THREADS = 8;
    const int OPS_PER_THREAD = 200;

    Timer timer;
    std::atomic<int> key_counter(0);

    // Sharded LRU
    timer.start();
    {
        std::vector<std::thread> threads;
        for (int t = 0; t < NUM_THREADS; t++) {
            threads.emplace_back([&]() {
                for (int i = 0; i < OPS_PER_THREAD; i++) {
                    int key = key_counter.fetch_add(1);
                    char key_str[32];
                    snprintf(key_str, sizeof(key_str), "writekey%d", key);
                    path_t* path = make_simple_path(key_str);
                    identifier_t* value = make_simple_value("value");
                    database_lru_cache_put(sharded_lru, path, value);
                }
            });
        }
        for (auto& t : threads) t.join();
    }
    timer.stop();
    double sharded_time = timer.microseconds();

    key_counter = 0;

    // Lock-Free LRU
    timer.start();
    {
        std::vector<std::thread> threads;
        for (int t = 0; t < NUM_THREADS; t++) {
            threads.emplace_back([&]() {
                for (int i = 0; i < OPS_PER_THREAD; i++) {
                    int key = key_counter.fetch_add(1);
                    char key_str[32];
                    snprintf(key_str, sizeof(key_str), "writekey%d", key);
                    path_t* path = make_simple_path(key_str);
                    identifier_t* value = make_simple_value("value");
                    lockfree_lru_cache_put(lockfree_lru, path, value);
                }
            });
        }
        for (auto& t : threads) t.join();
    }
    timer.stop();
    double lockfree_time = timer.microseconds();

    std::cout << "\n=== Write-Heavy Workload ===" << std::endl;
    std::cout << "Threads: " << NUM_THREADS << ", Ops/thread: " << OPS_PER_THREAD << std::endl;
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "Sharded LRU:   " << sharded_time << " us" << std::endl;
    std::cout << "Lock-Free LRU: " << lockfree_time << " us" << std::endl;
    std::cout << "Improvement:   " << ((sharded_time - lockfree_time) / sharded_time * 100) << "%" << std::endl;
}

// Benchmark: High contention on few keys (lock-free should excel here)
TEST_F(LRUBenchmark, HighContentionFewKeys) {
    // Create caches with only 4 shards to increase contention
    database_lru_cache_destroy(sharded_lru);
    lockfree_lru_cache_destroy(lockfree_lru);
    sharded_lru = database_lru_cache_create(10 * 1024 * 1024, 4);  // Only 4 shards
    lockfree_lru = lockfree_lru_cache_create(10 * 1024 * 1024, 4);

    const int NUM_THREADS = 16;
    const int OPS_PER_THREAD = 500;
    const int NUM_KEYS = 10;  // Only 10 keys - all threads compete for same keys

    // Pre-populate
    for (int i = 0; i < NUM_KEYS; i++) {
        char key[32];
        snprintf(key, sizeof(key), "hotkey%d", i);
        path_t* path = make_simple_path(key);
        identifier_t* value = make_simple_value("value");
        database_lru_cache_put(sharded_lru, path, value);
        path = make_simple_path(key);
        value = make_simple_value("value");
        lockfree_lru_cache_put(lockfree_lru, path, value);
    }

    Timer timer;

    // Sharded LRU - high contention scenario
    timer.start();
    {
        std::vector<std::thread> threads;
        std::atomic<int> success_count(0);
        for (int t = 0; t < NUM_THREADS; t++) {
            threads.emplace_back([&]() {
                std::random_device rd;
                std::mt19937 gen(rd());
                std::uniform_int_distribution<> dis(0, NUM_KEYS - 1);

                for (int i = 0; i < OPS_PER_THREAD; i++) {
                    int key = dis(gen);
                    char key_str[32];
                    snprintf(key_str, sizeof(key_str), "hotkey%d", key);
                    path_t* path = make_simple_path(key_str);
                    identifier_t* cached = database_lru_cache_get(sharded_lru, path);
                    if (cached) {
                        success_count++;
                        identifier_destroy(cached);
                    }
                    path_destroy(path);
                }
            });
        }
        for (auto& t : threads) t.join();
    }
    timer.stop();
    double sharded_time = timer.microseconds();

    // Lock-Free LRU - high contention scenario
    timer.start();
    {
        std::vector<std::thread> threads;
        std::atomic<int> success_count(0);
        for (int t = 0; t < NUM_THREADS; t++) {
            threads.emplace_back([&]() {
                std::random_device rd;
                std::mt19937 gen(rd());
                std::uniform_int_distribution<> dis(0, NUM_KEYS - 1);

                for (int i = 0; i < OPS_PER_THREAD; i++) {
                    int key = dis(gen);
                    char key_str[32];
                    snprintf(key_str, sizeof(key_str), "hotkey%d", key);
                    path_t* path = make_simple_path(key_str);
                    identifier_t* cached = lockfree_lru_cache_get(lockfree_lru, path);
                    if (cached) {
                        success_count++;
                        identifier_destroy(cached);
                    }
                    path_destroy(path);
                }
            });
        }
        for (auto& t : threads) t.join();
    }
    timer.stop();
    double lockfree_time = timer.microseconds();

    std::cout << "\n=== High Contention (Few Keys) Benchmark ===" << std::endl;
    std::cout << "Threads: " << NUM_THREADS << ", Keys: " << NUM_KEYS << ", Shards: 4" << std::endl;
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "Sharded LRU:   " << sharded_time << " us (" << (sharded_time / (NUM_THREADS * OPS_PER_THREAD)) << " us/op)" << std::endl;
    std::cout << "Lock-Free LRU: " << lockfree_time << " us (" << (lockfree_time / (NUM_THREADS * OPS_PER_THREAD)) << " us/op)" << std::endl;
    std::cout << "Improvement:   " << ((sharded_time - lockfree_time) / sharded_time * 100) << "%" << std::endl;
}

// Benchmark: Many threads, high concurrency
TEST_F(LRUBenchmark, ManyThreadsHighConcurrency) {
    const int NUM_THREADS = 32;
    const int OPS_PER_THREAD = 100;
    const int NUM_KEYS = 1000;

    populate_sharded_cache(sharded_lru, NUM_KEYS);
    populate_lockfree_cache(lockfree_lru, NUM_KEYS);

    Timer timer;

    // Sharded LRU
    timer.start();
    {
        std::vector<std::thread> threads;
        for (int t = 0; t < NUM_THREADS; t++) {
            threads.emplace_back([&]() {
                std::random_device rd;
                std::mt19937 gen(rd());
                std::uniform_int_distribution<> dis(0, NUM_KEYS - 1);

                for (int i = 0; i < OPS_PER_THREAD; i++) {
                    int key = dis(gen);
                    char key_str[32];
                    snprintf(key_str, sizeof(key_str), "key%d", key);
                    path_t* path = make_simple_path(key_str);
                    identifier_t* cached = database_lru_cache_get(sharded_lru, path);
                    if (cached) identifier_destroy(cached);
                    path_destroy(path);
                }
            });
        }
        for (auto& t : threads) t.join();
    }
    timer.stop();
    double sharded_time = timer.microseconds();

    // Lock-Free LRU
    timer.start();
    {
        std::vector<std::thread> threads;
        for (int t = 0; t < NUM_THREADS; t++) {
            threads.emplace_back([&]() {
                std::random_device rd;
                std::mt19937 gen(rd());
                std::uniform_int_distribution<> dis(0, NUM_KEYS - 1);

                for (int i = 0; i < OPS_PER_THREAD; i++) {
                    int key = dis(gen);
                    char key_str[32];
                    snprintf(key_str, sizeof(key_str), "key%d", key);
                    path_t* path = make_simple_path(key_str);
                    identifier_t* cached = lockfree_lru_cache_get(lockfree_lru, path);
                    if (cached) identifier_destroy(cached);
                    path_destroy(path);
                }
            });
        }
        for (auto& t : threads) t.join();
    }
    timer.stop();
    double lockfree_time = timer.microseconds();

    std::cout << "\n=== Many Threads (32) Benchmark ===" << std::endl;
    std::cout << "Threads: " << NUM_THREADS << ", Ops/thread: " << OPS_PER_THREAD << std::endl;
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "Sharded LRU:   " << sharded_time << " us" << std::endl;
    std::cout << "Lock-Free LRU: " << lockfree_time << " us" << std::endl;
    std::cout << "Improvement:   " << ((sharded_time - lockfree_time) / sharded_time * 100) << "%" << std::endl;
}

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}