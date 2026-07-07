#include <gtest/gtest.h>
extern "C" {
#include "Util/offset_remap.h"
#include "Util/allocator.h"
}

TEST(OffsetRemapTest, BasicInsertLookup) {
    offset_remap_t* m = offset_remap_create(64);
    ASSERT_NE(m, nullptr);

    EXPECT_EQ(offset_remap_get(m, 100), UINT64_MAX);  // not found sentinel

    offset_remap_put(m, 100, 200);
    offset_remap_put(m, 300, 400);
    EXPECT_EQ(offset_remap_get(m, 100), 200u);
    EXPECT_EQ(offset_remap_get(m, 300), 400u);
    EXPECT_EQ(offset_remap_get(m, 999), UINT64_MAX);

    offset_remap_destroy(m);
}

TEST(OffsetRemapTest, GrowsBeyondInitialCapacity) {
    offset_remap_t* m = offset_remap_create(4);
    for (uint64_t i = 0; i < 1000; i++) {
        offset_remap_put(m, i * 16, i * 32);
    }
    for (uint64_t i = 0; i < 1000; i++) {
        EXPECT_EQ(offset_remap_get(m, i * 16), i * 32) << "at i=" << i;
    }
    offset_remap_destroy(m);
}

TEST(OffsetRemapTest, OverwriteExistingKey) {
    offset_remap_t* m = offset_remap_create(8);
    offset_remap_put(m, 42, 100);
    offset_remap_put(m, 42, 200);  // overwrite
    EXPECT_EQ(offset_remap_get(m, 42), 200u);
    offset_remap_destroy(m);
}

TEST(OffsetRemapTest, Size) {
    offset_remap_t* m = offset_remap_create(8);
    EXPECT_EQ(offset_remap_size(m), 0u);
    offset_remap_put(m, 1, 2);
    offset_remap_put(m, 3, 4);
    offset_remap_put(m, 1, 5);  // overwrite, no size change
    EXPECT_EQ(offset_remap_size(m), 2u);
    offset_remap_destroy(m);
}