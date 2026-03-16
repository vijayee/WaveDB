//
// Test memory pool allocator
//

#include <gtest/gtest.h>
extern "C" {
#include "Util/memory_pool.h"
}

class MemoryPoolTest : public ::testing::Test {
protected:
    void SetUp() override {
        memory_pool_init();
    }

    void TearDown() override {
        memory_pool_destroy();
    }
};

TEST_F(MemoryPoolTest, SmallAllocation) {
    void* ptr = memory_pool_alloc(32);
    ASSERT_NE(ptr, nullptr);
    memory_pool_free(ptr, 32);
}

TEST_F(MemoryPoolTest, MediumAllocation) {
    void* ptr = memory_pool_alloc(128);
    ASSERT_NE(ptr, nullptr);
    memory_pool_free(ptr, 128);
}

TEST_F(MemoryPoolTest, LargeAllocation) {
    void* ptr = memory_pool_alloc(512);
    ASSERT_NE(ptr, nullptr);
    memory_pool_free(ptr, 512);
}

TEST_F(MemoryPoolTest, FallbackAllocation) {
    // Too large for pool, should fall back to malloc
    void* ptr = memory_pool_alloc(2048);
    ASSERT_NE(ptr, nullptr);
    memory_pool_free(ptr, 2048);
}

TEST_F(MemoryPoolTest, MultipleAllocations) {
    void* ptrs[100];

    // Allocate 100 small blocks
    for (int i = 0; i < 100; i++) {
        ptrs[i] = memory_pool_alloc(32);
        ASSERT_NE(ptrs[i], nullptr);
    }

    // Free them all
    for (int i = 0; i < 100; i++) {
        memory_pool_free(ptrs[i], 32);
    }
}

TEST_F(MemoryPoolTest, Statistics) {
    // Allocate and free some blocks
    void* small = memory_pool_alloc(32);
    void* medium = memory_pool_alloc(128);
    void* large = memory_pool_alloc(512);
    void* fallback = memory_pool_alloc(2048);

    memory_pool_free(small, 32);
    memory_pool_free(medium, 128);
    memory_pool_free(large, 512);
    memory_pool_free(fallback, 2048);

    memory_pool_stats_t stats = memory_pool_get_stats();

    EXPECT_EQ(stats.small_allocs, 1);
    EXPECT_EQ(stats.medium_allocs, 1);
    EXPECT_EQ(stats.large_allocs, 1);
    EXPECT_EQ(stats.fallback_allocs, 1);

    memory_pool_print_stats();
}

TEST_F(MemoryPoolTest, PoolExhaustion) {
    // Allocate more than pool size (should fall back to malloc)
    void* ptrs[15000];

    for (int i = 0; i < 15000; i++) {
        ptrs[i] = memory_pool_alloc(32);
        // Pool should handle exhaustion gracefully
    }

    for (int i = 0; i < 15000; i++) {
        memory_pool_free(ptrs[i], 32);
    }
}

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}