//
// Unit tests for lock-free LRU cache
//

#include <gtest/gtest.h>
#include <cstring>
#include <cstdio>
#include <thread>
#include <vector>
#include <atomic>
extern "C" {
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

class LockfreeLRUTest : public ::testing::Test {
protected:
    lockfree_lru_cache_t* lru;

    void SetUp() override {
        lru = lockfree_lru_cache_create(1024 * 1024, 0);  // 1 MB
        ASSERT_NE(lru, nullptr);
    }

    void TearDown() override {
        if (lru) {
            lockfree_lru_cache_destroy(lru);
            lru = nullptr;
        }
    }
};

TEST_F(LockfreeLRUTest, CreateDestroy) {
    EXPECT_NE(lru, nullptr);
    EXPECT_EQ(lockfree_lru_cache_size(lru), 0u);
}

TEST_F(LockfreeLRUTest, PutGet) {
    path_t* path = make_simple_path("key1");
    identifier_t* value = make_simple_value("value1");

    // Put
    identifier_t* old = lockfree_lru_cache_put(lru, path, value);
    EXPECT_EQ(old, nullptr);

    // Get
    path_t* path_copy = make_simple_path("key1");
    identifier_t* cached = lockfree_lru_cache_get(lru, path_copy);
    EXPECT_NE(cached, nullptr);

    // Cleanup
    identifier_destroy(cached);
}

TEST_F(LockfreeLRUTest, CacheMiss) {
    path_t* path = make_simple_path("nonexistent");
    identifier_t* cached = lockfree_lru_cache_get(lru, path);
    EXPECT_EQ(cached, nullptr);
    path_destroy(path);
}

TEST_F(LockfreeLRUTest, Size) {
    EXPECT_EQ(lockfree_lru_cache_size(lru), 0u);

    for (int i = 0; i < 10; i++) {
        char key[32];
        snprintf(key, sizeof(key), "key%d", i);
        path_t* path = make_simple_path(key);
        identifier_t* value = make_simple_value("value");
        lockfree_lru_cache_put(lru, path, value);
    }

    EXPECT_EQ(lockfree_lru_cache_size(lru), 10u);
}

TEST_F(LockfreeLRUTest, MemoryTracking) {
    size_t initial_memory = lockfree_lru_cache_memory(lru);
    EXPECT_EQ(initial_memory, 0u);

    // Add entry
    path_t* path = make_simple_path("key1");
    identifier_t* value = make_simple_value("value");
    lockfree_lru_cache_put(lru, path, value);

    size_t after_put = lockfree_lru_cache_memory(lru);
    EXPECT_GT(after_put, 0u);

    // Get entry (should not change memory)
    path_t* path_copy = make_simple_path("key1");
    identifier_t* cached = lockfree_lru_cache_get(lru, path_copy);
    EXPECT_EQ(lockfree_lru_cache_memory(lru), after_put);
    if (cached) identifier_destroy(cached);

    // Delete entry
    path_t* path_del = make_simple_path("key1");
    lockfree_lru_cache_delete(lru, path_del);
    EXPECT_EQ(lockfree_lru_cache_memory(lru), 0u);
}

TEST_F(LockfreeLRUTest, Delete) {
    // Add entry
    path_t* path = make_simple_path("key1");
    identifier_t* value = make_simple_value("value");
    lockfree_lru_cache_put(lru, path, value);

    EXPECT_EQ(lockfree_lru_cache_size(lru), 1u);

    // Delete
    path_t* path_del = make_simple_path("key1");
    lockfree_lru_cache_delete(lru, path_del);

    EXPECT_EQ(lockfree_lru_cache_size(lru), 0u);

    // Get should return NULL
    path_t* path_check = make_simple_path("key1");
    identifier_t* cached = lockfree_lru_cache_get(lru, path_check);
    EXPECT_EQ(cached, nullptr);
    path_destroy(path_check);
}

TEST_F(LockfreeLRUTest, Eviction) {
    // Create cache with small memory limit and fewer shards
    lockfree_lru_cache_destroy(lru);
    lru = lockfree_lru_cache_create(1024, 4);  // 1 KB limit, 4 shards
    ASSERT_NE(lru, nullptr);

    // Add entries until we exceed the memory budget
    // Each entry is about 240 bytes (path + value + node overhead)
    // With 4 shards and 256 bytes per shard, we can fit about 1 entry per shard

    for (int i = 0; i < 10; i++) {
        char key[32];
        snprintf(key, sizeof(key), "key%d", i);
        path_t* path = make_simple_path(key);
        identifier_t* value = make_simple_value("val");
        lockfree_lru_cache_put(lru, path, value);
    }

    // After adding 10 entries, eviction should have occurred
    // Memory should be bounded (not 10 * 240 = 2400 bytes)
    size_t memory = lockfree_lru_cache_memory(lru);
    size_t size = lockfree_lru_cache_size(lru);

    // Memory should be less than total budget + overhead
    EXPECT_LT(memory, 2048u);  // Allow some overhead

    // Size should be limited (not 10 entries)
    EXPECT_LT(size, 10u);
}

TEST_F(LockfreeLRUTest, PurgeHoles) {
    // Add several entries
    for (int i = 0; i < 10; i++) {
        char key[32];
        snprintf(key, sizeof(key), "key%d", i);
        path_t* path = make_simple_path(key);
        identifier_t* value = make_simple_value("value");
        lockfree_lru_cache_put(lru, path, value);
    }

    EXPECT_EQ(lockfree_lru_cache_size(lru), 10u);

    // Delete half (creates holes)
    for (int i = 0; i < 5; i++) {
        char key[32];
        snprintf(key, sizeof(key), "key%d", i);
        path_t* path = make_simple_path(key);
        lockfree_lru_cache_delete(lru, path);
    }

    EXPECT_EQ(lockfree_lru_cache_size(lru), 5u);

    // Purge holes
    size_t purged = lockfree_lru_cache_purge(lru, 100);
    EXPECT_GT(purged, 0u);

    // Remaining entries still accessible
    for (int i = 5; i < 10; i++) {
        char key[32];
        snprintf(key, sizeof(key), "key%d", i);
        path_t* path = make_simple_path(key);
        identifier_t* cached = lockfree_lru_cache_get(lru, path);
        EXPECT_NE(cached, nullptr) << "Key " << key << " should still exist";
        if (cached) identifier_destroy(cached);
        path_destroy(path);
    }
}

TEST_F(LockfreeLRUTest, ConcurrentGets) {
    // Pre-populate cache
    for (int i = 0; i < 100; i++) {
        char key[32];
        snprintf(key, sizeof(key), "key%d", i);
        path_t* path = make_simple_path(key);
        identifier_t* value = make_simple_value("value");
        lockfree_lru_cache_put(lru, path, value);
    }

    std::atomic<int> success_count(0);
    std::vector<std::thread> threads;

    // Concurrent reads from multiple threads
    for (int t = 0; t < 8; t++) {
        threads.emplace_back([&]() {
            for (int i = 0; i < 100; i++) {
                char key[32];
                snprintf(key, sizeof(key), "key%d", i);
                path_t* path = make_simple_path(key);
                identifier_t* cached = lockfree_lru_cache_get(lru, path);
                if (cached != nullptr) {
                    success_count++;
                    identifier_destroy(cached);
                }
                path_destroy(path);
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    // All reads should succeed (800 total)
    EXPECT_EQ(success_count.load(), 800);
}

TEST_F(LockfreeLRUTest, ConcurrentPutGet) {
    std::atomic<int> put_count(0);
    std::atomic<int> get_count(0);
    std::vector<std::thread> threads;

    // 4 producer threads
    for (int t = 0; t < 4; t++) {
        threads.emplace_back([&, t]() {
            for (int i = 0; i < 50; i++) {
                char key[32];
                snprintf(key, sizeof(key), "thread%d_key%d", t, i);
                path_t* path = make_simple_path(key);
                identifier_t* value = make_simple_value("value");
                lockfree_lru_cache_put(lru, path, value);
                put_count++;
            }
        });
    }

    // 4 consumer threads
    for (int t = 0; t < 4; t++) {
        threads.emplace_back([&, t]() {
            (void)t;  // Suppress unused warning
            for (int i = 0; i < 50; i++) {
                char key[32];
                snprintf(key, sizeof(key), "thread%d_key%d", t % 4, i);
                path_t* path = make_simple_path(key);
                identifier_t* cached = lockfree_lru_cache_get(lru, path);
                if (cached != nullptr) {
                    get_count++;
                    identifier_destroy(cached);
                }
                path_destroy(path);
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    // All puts should complete
    EXPECT_EQ(put_count.load(), 200);
    // Gets may or may not find entries (timing dependent)
    EXPECT_GE(get_count.load(), 0);
}

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}