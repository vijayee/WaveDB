//
// Test for stale_region_t using Google Test
//

#include <gtest/gtest.h>
#include "Storage/stale_region.h"

class StaleRegionTest : public ::testing::Test {
protected:
    void SetUp() override {
    }

    void TearDown() override {
    }
};

// 1. Create and destroy (no leaks under ASan)
TEST_F(StaleRegionTest, CreateDestroy) {
    stale_region_mgr_t* mgr = stale_region_mgr_create();
    ASSERT_NE(mgr, nullptr);
    EXPECT_EQ(mgr->count, 0u);
    EXPECT_EQ(mgr->total_stale_bytes, 0u);
    stale_region_mgr_destroy(mgr);
}

// 2. Add a single region, verify total
TEST_F(StaleRegionTest, AddSingleRegion) {
    stale_region_mgr_t* mgr = stale_region_mgr_create();
    stale_region_add(mgr, 100, 50);
    EXPECT_EQ(mgr->count, 1u);
    EXPECT_EQ(mgr->regions[0].offset, 100u);
    EXPECT_EQ(mgr->regions[0].length, 50u);
    EXPECT_EQ(stale_region_total(mgr), 50u);
    stale_region_mgr_destroy(mgr);
}

// 3. Add two adjacent regions, verify they merge
TEST_F(StaleRegionTest, AdjacentRegionsMerge) {
    stale_region_mgr_t* mgr = stale_region_mgr_create();
    // Region 1: [100, 150)
    stale_region_add(mgr, 100, 50);
    // Region 2: [150, 200) - adjacent (touches end of region 1)
    stale_region_add(mgr, 150, 50);
    EXPECT_EQ(mgr->count, 1u);
    EXPECT_EQ(mgr->regions[0].offset, 100u);
    EXPECT_EQ(mgr->regions[0].length, 100u);
    EXPECT_EQ(stale_region_total(mgr), 100u);
    stale_region_mgr_destroy(mgr);
}

// 4. Add overlapping regions, verify merge
TEST_F(StaleRegionTest, OverlappingRegionsMerge) {
    stale_region_mgr_t* mgr = stale_region_mgr_create();
    // Region 1: [100, 200)
    stale_region_add(mgr, 100, 100);
    // Region 2: [150, 250) - overlaps with region 1
    stale_region_add(mgr, 150, 100);
    EXPECT_EQ(mgr->count, 1u);
    EXPECT_EQ(mgr->regions[0].offset, 100u);
    EXPECT_EQ(mgr->regions[0].length, 150u);
    EXPECT_EQ(stale_region_total(mgr), 150u);
    stale_region_mgr_destroy(mgr);
}

// 5. Add non-adjacent regions, verify they stay separate
TEST_F(StaleRegionTest, NonAdjacentRegionsStaySeparate) {
    stale_region_mgr_t* mgr = stale_region_mgr_create();
    stale_region_add(mgr, 100, 50);
    stale_region_add(mgr, 300, 50);
    EXPECT_EQ(mgr->count, 2u);
    EXPECT_EQ(mgr->regions[0].offset, 100u);
    EXPECT_EQ(mgr->regions[0].length, 50u);
    EXPECT_EQ(mgr->regions[1].offset, 300u);
    EXPECT_EQ(mgr->regions[1].length, 50u);
    EXPECT_EQ(stale_region_total(mgr), 100u);
    stale_region_mgr_destroy(mgr);
}

// 6. Get reusable blocks above threshold
TEST_F(StaleRegionTest, GetReusableBlocks) {
    stale_region_mgr_t* mgr = stale_region_mgr_create();
    // File size = 1000, threshold 10% = 100 bytes
    stale_region_add(mgr, 0, 200);    // 200 bytes - above threshold
    stale_region_add(mgr, 400, 50);   // 50 bytes - below threshold
    stale_region_add(mgr, 600, 150);  // 150 bytes - above threshold

    size_t out_count = 0;
    stale_region_t* blocks = stale_region_get_reusable(mgr, 1000, 0.1, &out_count);
    EXPECT_EQ(out_count, 2u);
    ASSERT_NE(blocks, nullptr);

    EXPECT_EQ(blocks[0].offset, 0u);
    EXPECT_EQ(blocks[0].length, 200u);
    EXPECT_EQ(blocks[1].offset, 600u);
    EXPECT_EQ(blocks[1].length, 150u);

    free(blocks);
    stale_region_mgr_destroy(mgr);
}

// 7. Clear and verify empty
TEST_F(StaleRegionTest, ClearRegions) {
    stale_region_mgr_t* mgr = stale_region_mgr_create();
    stale_region_add(mgr, 100, 50);
    stale_region_add(mgr, 300, 50);
    EXPECT_EQ(mgr->count, 2u);
    EXPECT_EQ(stale_region_total(mgr), 100u);

    stale_region_clear(mgr);
    EXPECT_EQ(mgr->count, 0u);
    EXPECT_EQ(stale_region_total(mgr), 0u);

    stale_region_mgr_destroy(mgr);
}

// 8. Serialize and deserialize round-trip
TEST_F(StaleRegionTest, SerializeDeserialize) {
    stale_region_mgr_t* mgr = stale_region_mgr_create();
    stale_region_add(mgr, 100, 50);
    stale_region_add(mgr, 300, 80);
    stale_region_add(mgr, 500, 120);

    size_t out_len = 0;
    uint8_t* data = stale_region_serialize(mgr, &out_len);
    EXPECT_GT(out_len, 0u);

    stale_region_mgr_t* mgr2 = stale_region_deserialize(data, out_len);
    ASSERT_NE(mgr2, nullptr);
    EXPECT_EQ(mgr2->count, 3u);
    EXPECT_EQ(mgr2->total_stale_bytes, mgr->total_stale_bytes);
    EXPECT_EQ(mgr2->regions[0].offset, 100u);
    EXPECT_EQ(mgr2->regions[0].length, 50u);
    EXPECT_EQ(mgr2->regions[1].offset, 300u);
    EXPECT_EQ(mgr2->regions[1].length, 80u);
    EXPECT_EQ(mgr2->regions[2].offset, 500u);
    EXPECT_EQ(mgr2->regions[2].length, 120u);

    free(data);
    stale_region_mgr_destroy(mgr);
    stale_region_mgr_destroy(mgr2);
}

// 9. Large number of regions (stress test)
TEST_F(StaleRegionTest, StressManyRegions) {
    stale_region_mgr_t* mgr = stale_region_mgr_create();
    const int N = 1000;
    // Add N non-adjacent regions with gaps
    for (int i = 0; i < N; i++) {
        stale_region_add(mgr, i * 200, 100);
    }
    EXPECT_EQ(mgr->count, (size_t)N);
    EXPECT_EQ(stale_region_total(mgr), (uint64_t)(N * 100));

    // Now add adjacent regions that merge them all into one
    for (int i = 0; i < N - 1; i++) {
        stale_region_add(mgr, i * 200 + 100, 100);
    }
    // After merging all adjacent, should be 1 region
    EXPECT_EQ(mgr->count, 1u);
    EXPECT_EQ(mgr->regions[0].offset, 0u);
    EXPECT_EQ(mgr->regions[0].length, (uint64_t)(N * 200 - 100));
    EXPECT_EQ(stale_region_total(mgr), mgr->regions[0].length);

    stale_region_mgr_destroy(mgr);
}

// Additional: Add zero-length region does nothing
TEST_F(StaleRegionTest, AddZeroLength) {
    stale_region_mgr_t* mgr = stale_region_mgr_create();
    stale_region_add(mgr, 100, 0);
    EXPECT_EQ(mgr->count, 0u);
    EXPECT_EQ(stale_region_total(mgr), 0u);
    stale_region_mgr_destroy(mgr);
}

// Additional: Insert preserves sorted order (add out of order)
TEST_F(StaleRegionTest, InsertOutOfOrder) {
    stale_region_mgr_t* mgr = stale_region_mgr_create();
    stale_region_add(mgr, 300, 50);
    stale_region_add(mgr, 100, 50);
    stale_region_add(mgr, 200, 50);
    // Should be sorted: 100, 200, 300 - all non-adjacent
    EXPECT_EQ(mgr->count, 3u);
    EXPECT_EQ(mgr->regions[0].offset, 100u);
    EXPECT_EQ(mgr->regions[1].offset, 200u);
    EXPECT_EQ(mgr->regions[2].offset, 300u);
    stale_region_mgr_destroy(mgr);
}

// Additional: New region fully contained in existing
TEST_F(StaleRegionTest, FullyContainedMerge) {
    stale_region_mgr_t* mgr = stale_region_mgr_create();
    stale_region_add(mgr, 100, 200);  // [100, 300)
    stale_region_add(mgr, 150, 50);   // [150, 200) fully inside
    EXPECT_EQ(mgr->count, 1u);
    EXPECT_EQ(mgr->regions[0].offset, 100u);
    EXPECT_EQ(mgr->regions[0].length, 200u);
    EXPECT_EQ(stale_region_total(mgr), 200u);
    stale_region_mgr_destroy(mgr);
}

// Additional: New region spans multiple existing regions
TEST_F(StaleRegionTest, SpanMultipleMerge) {
    stale_region_mgr_t* mgr = stale_region_mgr_create();
    stale_region_add(mgr, 100, 50);   // [100, 150)
    stale_region_add(mgr, 300, 50);   // [300, 350)
    stale_region_add(mgr, 500, 50);   // [500, 550)
    // Add a region that bridges all three with gaps
    stale_region_add(mgr, 100, 450);  // [100, 550)
    EXPECT_EQ(mgr->count, 1u);
    EXPECT_EQ(mgr->regions[0].offset, 100u);
    EXPECT_EQ(mgr->regions[0].length, 450u);
    EXPECT_EQ(stale_region_total(mgr), 450u);
    stale_region_mgr_destroy(mgr);
}

// Additional: Deserialize with invalid data returns NULL
TEST_F(StaleRegionTest, DeserializeInvalidData) {
    stale_region_mgr_t* mgr = stale_region_deserialize((const uint8_t*)"short", 5);
    EXPECT_EQ(mgr, nullptr);
}

// Additional: Destroy NULL is safe
TEST_F(StaleRegionTest, DestroyNull) {
    stale_region_mgr_destroy(NULL);
}

// Additional: Total on NULL returns 0
TEST_F(StaleRegionTest, TotalOnNull) {
    EXPECT_EQ(stale_region_total(NULL), 0u);
}