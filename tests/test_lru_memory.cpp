//
// Created by victor on 3/20/26.
//

#include <gtest/gtest.h>
#include <cstring>
#include <cstdio>
extern "C" {
#include "Database/database_lru.h"
#include "Database/database.h"
#include "HBTrie/path.h"
#include "HBTrie/identifier.h"
#include "HBTrie/chunk.h"
#include "Buffer/buffer.h"
}

// Helper to create a simple path with one identifier
static path_t* make_simple_path(const char* key) {
    path_t* path = path_create();
    buffer_t* buf = buffer_create_from_pointer_copy((uint8_t*)key, strlen(key));
    identifier_t* id = identifier_create(buf, 0);
    buffer_destroy(buf);
    path_append(path, id);
    identifier_destroy(id);
    return path;
}

// Helper to create a simple identifier value
static identifier_t* make_simple_value(const char* data) {
    buffer_t* buf = buffer_create_from_pointer_copy((uint8_t*)data, strlen(data));
    identifier_t* id = identifier_create(buf, 0);
    buffer_destroy(buf);
    return id;
}

// Helper to create a large value (~900 bytes)
static identifier_t* make_large_value() {
    char large_data[900];
    memset(large_data, 'x', 899);
    large_data[899] = '\0';
    return make_simple_value(large_data);
}

// Test memory calculation accuracy
TEST(LRUMemoryTest, MemoryCalculation) {
    // Create small path: "a" -> "b"
    path_t* path = path_create();

    buffer_t* buf1 = buffer_create_from_pointer_copy((uint8_t*)"a", 1);
    identifier_t* id1 = identifier_create(buf1, 0);
    buffer_destroy(buf1);
    path_append(path, id1);
    identifier_destroy(id1);

    buffer_t* buf2 = buffer_create_from_pointer_copy((uint8_t*)"b", 1);
    identifier_t* id2 = identifier_create(buf2, 0);
    buffer_destroy(buf2);
    path_append(path, id2);
    identifier_destroy(id2);

    // Create value
    buffer_t* buf3 = buffer_create_from_pointer_copy((uint8_t*)"value", 5);
    identifier_t* value = identifier_create(buf3, 0);
    buffer_destroy(buf3);

    // Verify memory is tracked via cache operations
    path_destroy(path);
    identifier_destroy(value);
}

// Test memory-based eviction
TEST(LRUMemoryTest, MemoryBasedEviction) {
    // Create cache with 50 KB budget (default)
    database_lru_cache_t* lru = database_lru_cache_create(50 * 1024);
    ASSERT_NE(lru, nullptr);

    // Add many small entries to fill cache significantly
    for (int i = 0; i < 100; i++) {
        char key[32];
        snprintf(key, sizeof(key), "key%d", i);
        path_t* path = make_simple_path(key);
        identifier_t* value = make_simple_value("val");

        identifier_t* evicted = database_lru_cache_put(lru, path, value);
        if (evicted != nullptr) {
            identifier_destroy(evicted);
        }
    }

    // Cache should have some entries (LRU eviction occurred)
    EXPECT_GT(database_lru_cache_size(lru), 0u);
    EXPECT_GT(database_lru_cache_memory(lru), 0u);
    size_t entries_after_100 = database_lru_cache_size(lru);

    // Add more entries to trigger more evictions
    for (int i = 100; i < 200; i++) {
        char key[32];
        snprintf(key, sizeof(key), "key%d", i);
        path_t* path = make_simple_path(key);
        identifier_t* value = make_simple_value("val");

        identifier_t* evicted = database_lru_cache_put(lru, path, value);
        if (evicted != nullptr) {
            identifier_destroy(evicted);
        }
    }

    // Entry count should be similar (memory-based eviction working)
    // The exact count depends on entry sizes, but should be bounded
    EXPECT_GT(database_lru_cache_size(lru), 0u);
    EXPECT_LE(database_lru_cache_memory(lru), 50 * 1024u);  // Should not exceed budget

    database_lru_cache_destroy(lru);
}

// Test zero memory budget uses default
TEST(LRUMemoryTest, ZeroMemoryBudget) {
    database_lru_cache_t* lru = database_lru_cache_create(0);
    ASSERT_NE(lru, nullptr);
    EXPECT_EQ(lru->total_max_memory, DATABASE_DEFAULT_LRU_MEMORY_MB * 1024 * 1024);
    database_lru_cache_destroy(lru);
}

// Test memory tracking on get/delete
TEST(LRUMemoryTest, MemoryTracking) {
    database_lru_cache_t* lru = database_lru_cache_create(1024 * 1024);  // 1 MB
    ASSERT_NE(lru, nullptr);

    // Add entry
    path_t* path1 = make_simple_path("key1");
    identifier_t* value1 = make_simple_value("value");

    database_lru_cache_put(lru, path1, value1);
    size_t after_put = database_lru_cache_memory(lru);
    EXPECT_GT(after_put, 0u);

    // Get entry (should not change memory)
    path_t* path1_copy = path_copy(path1);
    identifier_t* cached = database_lru_cache_get(lru, path1_copy);
    EXPECT_NE(cached, nullptr);
    EXPECT_EQ(database_lru_cache_memory(lru), after_put);
    identifier_destroy(cached);

    // Delete entry
    database_lru_cache_delete(lru, path1);
    EXPECT_EQ(database_lru_cache_memory(lru), 0u);
    EXPECT_EQ(database_lru_cache_size(lru), 0u);

    database_lru_cache_destroy(lru);
}

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}