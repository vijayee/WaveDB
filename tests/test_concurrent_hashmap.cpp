//
// Unit tests for concurrent hashmap
//

#include <gtest/gtest.h>
#include <cstring>
#include <thread>
#include <vector>
#include <atomic>
extern "C" {
#include "Util/concurrent_hashmap.h"
}

// Simple string key helpers
static size_t hash_string(const void* key) {
    const char* str = (const char*)key;
    size_t hash = 5381;
    int c;
    while ((c = *str++)) {
        hash = ((hash << 5) + hash) + c;
    }
    return hash;
}

static int compare_string(const void* a, const void* b) {
    return strcmp((const char*)a, (const char*)b);
}

static void* dup_string(const void* key) {
    const char* str = (const char*)key;
    return strdup(str);
}

static void free_string(void* key) {
    free(key);
}

class ConcurrentHashmapTest : public ::testing::Test {
protected:
    concurrent_hashmap_t* map;

    void SetUp() override {
        map = concurrent_hashmap_create(
            16,     // 16 stripes
            8,      // 8 initial buckets per stripe
            0.75f,  // load factor
            hash_string,
            compare_string,
            dup_string,
            free_string
        );
        ASSERT_NE(map, nullptr);
    }

    void TearDown() override {
        if (map) {
            concurrent_hashmap_destroy(map);
            map = nullptr;
        }
    }
};

TEST_F(ConcurrentHashmapTest, CreateDestroy) {
    EXPECT_NE(map, nullptr);
    EXPECT_EQ(concurrent_hashmap_size(map), 0u);
}

TEST_F(ConcurrentHashmapTest, PutGet) {
    char* key = strdup("test_key");
    char* value = strdup("test_value");

    // Put
    void* old = concurrent_hashmap_put(map, key, value);
    EXPECT_EQ(old, nullptr);

    // Get
    void* result = concurrent_hashmap_get(map, "test_key");
    EXPECT_EQ(result, value);

    // Cleanup (map owns the key copy)
    free(key);
}

TEST_F(ConcurrentHashmapTest, Contains) {
    char* key = strdup("test_key");
    char* value = strdup("test_value");

    // Not contains initially
    EXPECT_EQ(concurrent_hashmap_contains(map, "test_key"), 0);

    // Put
    concurrent_hashmap_put(map, key, value);

    // Contains after put
    EXPECT_EQ(concurrent_hashmap_contains(map, "test_key"), 1);

    free(key);
}

TEST_F(ConcurrentHashmapTest, Replace) {
    char* key = strdup("test_key");
    char* value1 = strdup("value1");
    char* value2 = strdup("value2");

    // Put first value
    void* old1 = concurrent_hashmap_put(map, key, value1);
    EXPECT_EQ(old1, nullptr);

    // Put second value (replace)
    key = strdup("test_key");  // Duplicate since map owns first key
    void* old2 = concurrent_hashmap_put(map, key, value2);
    EXPECT_EQ(old2, value1);

    // Get should return new value
    void* result = concurrent_hashmap_get(map, "test_key");
    EXPECT_EQ(result, value2);

    free(old2);  // Clean up old value
}

TEST_F(ConcurrentHashmapTest, PutIfAbsent) {
    char* key1 = strdup("test_key");
    char* value1 = strdup("value1");

    // First put should succeed
    void* result1 = concurrent_hashmap_put_if_absent(map, key1, value1);
    EXPECT_EQ(result1, nullptr);

    // Second put should return existing
    char* key2 = strdup("test_key");
    char* value2 = strdup("value2");
    void* result2 = concurrent_hashmap_put_if_absent(map, key2, value2);
    EXPECT_EQ(result2, value1);

    // Clean up key2 since it wasn't stored
    free(key2);
    free(value2);

    // Get should return first value
    void* value = concurrent_hashmap_get(map, "test_key");
    EXPECT_EQ(value, value1);
}

TEST_F(ConcurrentHashmapTest, Remove) {
    char* key = strdup("test_key");
    char* value = strdup("value");

    // Put
    concurrent_hashmap_put(map, key, value);
    EXPECT_EQ(concurrent_hashmap_contains(map, "test_key"), 1);

    // Remove
    void* removed = concurrent_hashmap_remove(map, "test_key");
    EXPECT_EQ(removed, value);
    EXPECT_EQ(concurrent_hashmap_contains(map, "test_key"), 0);
    EXPECT_EQ(concurrent_hashmap_size(map), 0u);

    free(removed);
}

TEST_F(ConcurrentHashmapTest, Size) {
    EXPECT_EQ(concurrent_hashmap_size(map), 0u);

    for (int i = 0; i < 10; i++) {
        char key[32];
        snprintf(key, sizeof(key), "key%d", i);
        concurrent_hashmap_put(map, strdup(key), strdup("value"));
    }

    EXPECT_EQ(concurrent_hashmap_size(map), 10u);
}

TEST_F(ConcurrentHashmapTest, ConcurrentReads) {
    // Pre-populate with some entries
    for (int i = 0; i < 100; i++) {
        char key[32];
        snprintf(key, sizeof(key), "key%d", i);
        char* value = strdup(key);
        concurrent_hashmap_put(map, strdup(key), value);
    }

    // Concurrent reads from multiple threads
    std::atomic<int> success_count(0);
    std::vector<std::thread> threads;

    for (int t = 0; t < 8; t++) {
        threads.emplace_back([&, t]() {
            (void)t;  // Suppress unused warning
            for (int i = 0; i < 100; i++) {
                char key[32];
                snprintf(key, sizeof(key), "key%d", i);
                void* value = concurrent_hashmap_get(map, key);
                if (value != nullptr) {
                    success_count++;
                }
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    // All reads should succeed (800 total: 8 threads * 100 keys)
    EXPECT_EQ(success_count.load(), 800);
}

TEST_F(ConcurrentHashmapTest, ConcurrentWrites) {
    std::vector<std::thread> threads;
    std::atomic<int> success_count(0);

    // Each thread writes to different keys
    for (int t = 0; t < 8; t++) {
        threads.emplace_back([&, t]() {
            for (int i = 0; i < 100; i++) {
                char key[32];
                snprintf(key, sizeof(key), "thread%d_key%d", t, i);
                char* value = strdup(key);
                void* old = concurrent_hashmap_put(map, strdup(key), value);
                if (old == nullptr) {
                    success_count++;
                }
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    // All puts should succeed (no duplicates)
    EXPECT_EQ(success_count.load(), 800);
    EXPECT_EQ(concurrent_hashmap_size(map), 800u);
}

TEST_F(ConcurrentHashmapTest, ConcurrentReadWrite) {
    // Pre-populate
    for (int i = 0; i < 100; i++) {
        char key[32];
        snprintf(key, sizeof(key), "key%d", i);
        concurrent_hashmap_put(map, strdup(key), strdup("init"));
    }

    std::atomic<int> read_count(0);
    std::atomic<int> write_count(0);
    std::vector<std::thread> threads;

    // Reader threads
    for (int t = 0; t < 4; t++) {
        threads.emplace_back([&]() {
            for (int i = 0; i < 100; i++) {
                char key[32];
                snprintf(key, sizeof(key), "key%d", i);
                if (concurrent_hashmap_get(map, key) != nullptr) {
                    read_count++;
                }
            }
        });
    }

    // Writer threads
    for (int t = 0; t < 4; t++) {
        threads.emplace_back([&, t]() {
            for (int i = 0; i < 100; i++) {
                char key[32];
                snprintf(key, sizeof(key), "key%d", i);
                char value[32];
                snprintf(value, sizeof(value), "thread%d", t);
                concurrent_hashmap_put(map, strdup(key), strdup(value));
                write_count++;
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    // All operations should complete without crashes
    EXPECT_EQ(read_count.load(), 400);
    EXPECT_EQ(write_count.load(), 400);
}

TEST_F(ConcurrentHashmapTest, Resize) {
    // Create map with small initial size
    concurrent_hashmap_destroy(map);
    map = concurrent_hashmap_create(
        4,      // 4 stripes
        4,      // 4 initial buckets
        0.75f,  // load factor
        hash_string,
        compare_string,
        dup_string,
        free_string
    );
    ASSERT_NE(map, nullptr);

    // Add entries to trigger resize
    for (int i = 0; i < 100; i++) {
        char key[32];
        snprintf(key, sizeof(key), "key%d", i);
        concurrent_hashmap_put(map, strdup(key), strdup("value"));
    }

    // All entries should still be accessible
    for (int i = 0; i < 100; i++) {
        char key[32];
        snprintf(key, sizeof(key), "key%d", i);
        void* value = concurrent_hashmap_get(map, key);
        EXPECT_NE(value, nullptr) << "Key " << key << " not found after resize";
    }

    EXPECT_EQ(concurrent_hashmap_size(map), 100u);
}

TEST_F(ConcurrentHashmapTest, TombstoneCleanup) {
    // Add entries
    for (int i = 0; i < 50; i++) {
        char key[32];
        snprintf(key, sizeof(key), "key%d", i);
        concurrent_hashmap_put(map, strdup(key), strdup("value"));
    }

    EXPECT_EQ(concurrent_hashmap_size(map), 50u);

    // Remove half
    for (int i = 0; i < 25; i++) {
        char key[32];
        snprintf(key, sizeof(key), "key%d", i);
        void* value = concurrent_hashmap_remove(map, key);
        free(value);
    }

    EXPECT_EQ(concurrent_hashmap_size(map), 25u);

    // Cleanup tombstones
    size_t cleaned = concurrent_hashmap_cleanup(map);
    EXPECT_GT(cleaned, 0u);

    // Remaining entries still accessible
    for (int i = 25; i < 50; i++) {
        char key[32];
        snprintf(key, sizeof(key), "key%d", i);
        EXPECT_NE(concurrent_hashmap_get(map, key), nullptr);
    }
}

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}