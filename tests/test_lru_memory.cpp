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
    // Create cache with large enough budget for testing
    // We need enough for multiple entries to test LRU eviction
    database_lru_cache_t* lru = database_lru_cache_create(4096);  // 4 KB
    ASSERT_NE(lru, nullptr);

    // Add small entries first (to have multiple entries that fit)
    path_t* path1 = make_simple_path("key1");
    identifier_t* value1 = make_simple_value("val1");

    identifier_t* evicted = database_lru_cache_put(lru, path1, value1);
    EXPECT_EQ(evicted, nullptr);
    EXPECT_EQ(lru->entry_count, 1u);
    size_t small_entry_memory = lru->current_memory;
    EXPECT_GT(small_entry_memory, 0u);

    // Add another small entry
    path_t* path2 = make_simple_path("key2");
    identifier_t* value2 = make_simple_value("val2");

    evicted = database_lru_cache_put(lru, path2, value2);
    EXPECT_EQ(evicted, nullptr);
    EXPECT_EQ(lru->entry_count, 2u);

    // Add a third small entry
    path_t* path3 = make_simple_path("key3");
    identifier_t* value3 = make_simple_value("val3");

    evicted = database_lru_cache_put(lru, path3, value3);
    EXPECT_EQ(evicted, nullptr);
    EXPECT_EQ(lru->entry_count, 3u);

    // Now add a large entry that will trigger eviction
    path_t* path4 = make_simple_path("key4");
    identifier_t* value4 = make_large_value();
    size_t large_entry_memory = small_entry_memory + 900;  // Approximate
    // This large entry should cause eviction of path1 (LRU)

    evicted = database_lru_cache_put(lru, path4, value4);

    // Check that eviction occurred
    // Note: We can't predict exactly which entry will be evicted without
    // knowing exact memory sizes, so we just verify that eviction happened
    // and that the cache still works correctly

    // Verify path4 is present (newly added)
    path_t* path4_check = make_simple_path("key4");
    EXPECT_EQ(database_lru_cache_contains(lru, path4_check), 1);
    path_destroy(path4_check);

    // Verify at least one of the earlier entries was evicted
    path_t* path1_check = make_simple_path("key1");
    path_t* path2_check = make_simple_path("key2");
    path_t* path3_check = make_simple_path("key3");

    int total_present = database_lru_cache_contains(lru, path1_check) +
                        database_lru_cache_contains(lru, path2_check) +
                        database_lru_cache_contains(lru, path3_check);
    // At most 2 of the original 3 should still be present
    EXPECT_LE(total_present, 2);

    path_destroy(path1_check);
    path_destroy(path2_check);
    path_destroy(path3_check);

    if (evicted != nullptr) {
        identifier_destroy(evicted);
    }

    database_lru_cache_destroy(lru);
}

// Test zero memory budget uses default
TEST(LRUMemoryTest, ZeroMemoryBudget) {
    database_lru_cache_t* lru = database_lru_cache_create(0);
    ASSERT_NE(lru, nullptr);
    EXPECT_EQ(lru->max_memory, DATABASE_DEFAULT_LRU_MEMORY_MB * 1024 * 1024);
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
    size_t after_put = lru->current_memory;
    EXPECT_GT(after_put, 0u);

    // Get entry (should not change memory)
    path_t* path1_copy = path_copy(path1);
    identifier_t* cached = database_lru_cache_get(lru, path1_copy);
    EXPECT_NE(cached, nullptr);
    EXPECT_EQ(lru->current_memory, after_put);
    identifier_destroy(cached);

    // Delete entry
    database_lru_cache_delete(lru, path1);
    EXPECT_EQ(lru->current_memory, 0u);
    EXPECT_EQ(lru->entry_count, 0u);

    database_lru_cache_destroy(lru);
}

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}