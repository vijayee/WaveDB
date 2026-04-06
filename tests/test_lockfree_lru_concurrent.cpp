//
// Concurrent Tests for Lock-Free LRU (eBay-style reference counting)
//

#include <gtest/gtest.h>
#include <thread>
#include <vector>
#include <atomic>
#include <random>
#include <chrono>
#include <cstring>
#include <cstdio>
extern "C" {
#include "Database/lockfree_lru.h"
#include "HBTrie/path.h"
#include "HBTrie/identifier.h"
#include "Buffer/buffer.h"
}

class LockFreeLRUConcurrentTest : public ::testing::Test {
protected:
    void SetUp() override {
        // No HP init needed - eBay-style uses reference counting only
    }
    void TearDown() override {
        // No HP cleanup needed
    }
};

// Helper to create a simple path from string
static path_t* make_simple_path(const char* key) {
    path_t* path = path_create();
    buffer_t* buf = buffer_create_from_pointer_copy((uint8_t*)key, strlen(key));
    identifier_t* id = identifier_create(buf, 0);
    buffer_destroy(buf);
    path_append(path, id);
    identifier_destroy(id);
    return path;
}

// Helper to create a simple value from string
static identifier_t* make_simple_value(const char* data) {
    buffer_t* buf = buffer_create_from_pointer_copy((uint8_t*)data, strlen(data));
    identifier_t* id = identifier_create(buf, 0);
    buffer_destroy(buf);
    return id;
}

// Helper to create path from two components
static path_t* make_path_2(const char* prefix, const char* suffix) {
    path_t* path = path_create();

    buffer_t* buf1 = buffer_create_from_pointer_copy((uint8_t*)prefix, strlen(prefix));
    identifier_t* id1 = identifier_create(buf1, 0);
    buffer_destroy(buf1);
    path_append(path, id1);
    identifier_destroy(id1);

    buffer_t* buf2 = buffer_create_from_pointer_copy((uint8_t*)suffix, strlen(suffix));
    identifier_t* id2 = identifier_create(buf2, 0);
    buffer_destroy(buf2);
    path_append(path, id2);
    identifier_destroy(id2);

    return path;
}

TEST_F(LockFreeLRUConcurrentTest, ConcurrentPutGet) {
    constexpr int NUM_THREADS = 8;
    constexpr int ITERATIONS = 1000;

    lockfree_lru_cache_t* cache = lockfree_lru_cache_create(1024 * 1024, 16);
    ASSERT_NE(cache, nullptr);

    std::atomic<int> puts{0};
    std::atomic<int> gets{0};
    std::atomic<int> hits{0};
    std::atomic<int> misses{0};
    std::atomic<int> errors{0};

    std::vector<std::thread> threads;
    for (int t = 0; t < NUM_THREADS; t++) {
        threads.emplace_back([&, t]() {
            for (int i = 0; i < ITERATIONS; i++) {
                // Create unique key per thread and iteration
                char key[64];
                snprintf(key, sizeof(key), "%d_%d", t, i);
                path_t* path = make_simple_path(key);

                if (i % 2 == 0) {
                    // Put
                    identifier_t* value = make_simple_value("value");
                    lockfree_lru_cache_put(cache, path, value);
                    puts++;
                } else {
                    // Get
                    identifier_t* value = lockfree_lru_cache_get(cache, path);
                    if (value != nullptr) {
                        hits++;
                        identifier_destroy(value);
                    } else {
                        misses++;
                    }
                    path_destroy(path);
                    gets++;
                }
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    lockfree_lru_cache_destroy(cache);

    EXPECT_EQ(errors.load(), 0);
    EXPECT_GT(puts.load(), 0);
    EXPECT_GT(gets.load(), 0);

    std::cout << "ConcurrentPutGet: "
              << puts.load() << " puts, "
              << hits.load() << " hits, "
              << misses.load() << " misses" << std::endl;
}

TEST_F(LockFreeLRUConcurrentTest, ConcurrentEviction) {
    constexpr int NUM_THREADS = 4;
    constexpr int ITERATIONS = 500;
    constexpr size_t MAX_MEMORY = 64 * 1024;  // 64KB - small to force evictions

    lockfree_lru_cache_t* cache = lockfree_lru_cache_create(MAX_MEMORY, 4);
    ASSERT_NE(cache, nullptr);

    std::atomic<int> puts{0};
    std::atomic<int> evictions{0};
    std::atomic<int> errors{0};

    std::vector<std::thread> threads;
    for (int t = 0; t < NUM_THREADS; t++) {
        threads.emplace_back([&, t]() {
            for (int i = 0; i < ITERATIONS; i++) {
                // Create unique key
                char key[64];
                snprintf(key, sizeof(key), "%d_%d", t, i);
                path_t* path = make_simple_path(key);

                // Create large entry to force eviction
                identifier_t* value = make_simple_value("large_value_padding_string_for_eviction");

                lockfree_lru_cache_put(cache, path, value);
                puts++;

                // Small delay to allow evictions
                if (i % 100 == 0) {
                    std::this_thread::sleep_for(std::chrono::microseconds(1));
                }
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    size_t final_memory = lockfree_lru_cache_memory(cache);
    lockfree_lru_cache_destroy(cache);

    // Memory should be within budget
    EXPECT_LE(final_memory, MAX_MEMORY * 2);  // Allow some slack

    std::cout << "ConcurrentEviction: "
              << puts.load() << " puts, "
              << "final memory: " << final_memory << " bytes" << std::endl;
}

TEST_F(LockFreeLRUConcurrentTest, ConcurrentDelete) {
    constexpr int NUM_THREADS = 8;
    constexpr int ITERATIONS = 500;
    constexpr int KEY_RANGE = 100;  // Shared keys

    lockfree_lru_cache_t* cache = lockfree_lru_cache_create(1024 * 1024, 16);
    ASSERT_NE(cache, nullptr);

    // Pre-populate cache
    for (int i = 0; i < KEY_RANGE; i++) {
        char key[64];
        snprintf(key, sizeof(key), "key_%d", i);
        path_t* path = make_simple_path(key);
        identifier_t* value = make_simple_value("initial_value");
        lockfree_lru_cache_put(cache, path, value);
    }

    std::atomic<int> deletes{0};
    std::atomic<int> gets{0};
    std::atomic<int> hits{0};
    std::atomic<int> misses{0};

    std::vector<std::thread> threads;
    for (int t = 0; t < NUM_THREADS; t++) {
        threads.emplace_back([&, t]() {
            std::mt19937 rng(t);  // Deterministic per-thread RNG
            for (int i = 0; i < ITERATIONS; i++) {
                int key_idx = rng() % KEY_RANGE;
                char key[64];
                snprintf(key, sizeof(key), "key_%d", key_idx);
                path_t* path = make_simple_path(key);

                if (rng() % 3 == 0) {
                    // Delete
                    lockfree_lru_cache_delete(cache, path);
                    deletes++;
                } else {
                    // Get
                    identifier_t* value = lockfree_lru_cache_get(cache, path);
                    if (value != nullptr) {
                        hits++;
                        identifier_destroy(value);
                    } else {
                        misses++;
                    }
                    path_destroy(path);
                    gets++;
                }
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    lockfree_lru_cache_destroy(cache);

    std::cout << "ConcurrentDelete: "
              << deletes.load() << " deletes, "
              << gets.load() << " gets ("
              << hits.load() << " hits, "
              << misses.load() << " misses)" << std::endl;
}

TEST_F(LockFreeLRUConcurrentTest, MixedOperations) {
    constexpr int NUM_THREADS = 8;
    constexpr int ITERATIONS = 2000;
    constexpr int KEY_RANGE = 50;

    lockfree_lru_cache_t* cache = lockfree_lru_cache_create(256 * 1024, 16);
    ASSERT_NE(cache, nullptr);

    std::atomic<int> puts{0};
    std::atomic<int> gets{0};
    std::atomic<int> deletes{0};
    std::atomic<int> hits{0};
    std::atomic<int> misses{0};

    std::vector<std::thread> threads;
    for (int t = 0; t < NUM_THREADS; t++) {
        threads.emplace_back([&, t]() {
            std::mt19937 rng(t * 1000);
            for (int i = 0; i < ITERATIONS; i++) {
                int key_idx = rng() % KEY_RANGE;
                char key[64];
                snprintf(key, sizeof(key), "mixed_%d", key_idx);
                path_t* path = make_simple_path(key);

                int op = rng() % 10;
                if (op < 4) {
                    // 40% put
                    identifier_t* value = make_simple_value("data");
                    lockfree_lru_cache_put(cache, path, value);
                    puts++;
                } else if (op < 8) {
                    // 40% get
                    identifier_t* value = lockfree_lru_cache_get(cache, path);
                    if (value != nullptr) {
                        hits++;
                        identifier_destroy(value);
                    } else {
                        misses++;
                    }
                    path_destroy(path);
                    gets++;
                } else {
                    // 20% delete
                    lockfree_lru_cache_delete(cache, path);
                    deletes++;
                }
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    size_t final_size = lockfree_lru_cache_size(cache);
    lockfree_lru_cache_destroy(cache);

    std::cout << "MixedOperations: "
              << puts.load() << " puts, "
              << gets.load() << " gets, "
              << deletes.load() << " deletes, "
              << "final size: " << final_size << std::endl;

    // Test passes if it doesn't crash
    SUCCEED();
}

TEST_F(LockFreeLRUConcurrentTest, StressTest) {
    constexpr int NUM_THREADS = 16;
    constexpr int ITERATIONS = 10000;
    constexpr size_t MAX_MEMORY = 512 * 1024;  // 512KB

    lockfree_lru_cache_t* cache = lockfree_lru_cache_create(MAX_MEMORY, 32);
    ASSERT_NE(cache, nullptr);

    std::atomic<bool> running{true};
    std::atomic<int> operations{0};
    std::atomic<int> errors{0};

    // Start multiple threads doing operations
    std::vector<std::thread> threads;
    for (int t = 0; t < NUM_THREADS; t++) {
        threads.emplace_back([&, t]() {
            std::mt19937 rng(t);
            for (int i = 0; i < ITERATIONS && running.load(); i++) {
                char key[64];
                snprintf(key, sizeof(key), "stress_%d", (int)(rng() % 1000));
                path_t* path = make_simple_path(key);

                int op = rng() % 100;
                if (op < 50) {
                    // 50% put
                    identifier_t* value = make_simple_value("stress_data");
                    lockfree_lru_cache_put(cache, path, value);
                } else if (op < 90) {
                    // 40% get
                    identifier_t* value = lockfree_lru_cache_get(cache, path);
                    if (value != nullptr) {
                        identifier_destroy(value);
                    }
                    path_destroy(path);
                } else {
                    // 10% delete
                    lockfree_lru_cache_delete(cache, path);
                }

                operations++;
            }
        });
    }

    // Let it run for a bit
    std::this_thread::sleep_for(std::chrono::seconds(2));
    running = false;

    for (auto& t : threads) {
        t.join();
    }

    size_t final_memory = lockfree_lru_cache_memory(cache);
    lockfree_lru_cache_destroy(cache);

    std::cout << "StressTest: "
              << operations.load() << " operations completed, "
              << errors.load() << " errors, "
              << "final memory: " << final_memory << " bytes" << std::endl;

    EXPECT_EQ(errors.load(), 0);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}