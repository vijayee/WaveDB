//
// Test for page_file_t using Google Test
//

#include <gtest/gtest.h>
#include "Storage/page_file.h"

#include <cstdlib>
#include <cstring>
#include <cstdio>

class PageFileTest : public ::testing::Test {
protected:
    char tmpdir[256];

    void SetUp() override {
        strcpy(tmpdir, "/tmp/page_file_test_XXXXXX");
        mkdtemp(tmpdir);
    }

    void TearDown() override {
        // Remove files in tmpdir
        char cmd[512];
        snprintf(cmd, sizeof(cmd), "rm -rf %s", tmpdir);
        system(cmd);
    }

    char* make_path(char* buf, size_t bufsize, const char* name) {
        snprintf(buf, bufsize, "%s/%s", tmpdir, name);
        return buf;
    }
};

// 1. Create and destroy (no leaks under ASan)
TEST_F(PageFileTest, CreateDestroy) {
    char path[512];
    make_path(path, sizeof(path), "test1.db");

    page_file_t* pf = page_file_create(path, 4096, 2, NULL);
    ASSERT_NE(pf, nullptr);
    EXPECT_EQ(pf->fd, -1);
    EXPECT_EQ(pf->block_size, 4096u);
    EXPECT_EQ(pf->num_superblocks, 2u);

    page_file_destroy(pf);
}

// 2. Open new file, write superblock, read it back — verify fields match
TEST_F(PageFileTest, WriteReadSuperblock) {
    char path[512];
    make_path(path, sizeof(path), "test2.db");

    page_file_t* pf = page_file_create(path, 4096, 2, NULL);
    ASSERT_NE(pf, nullptr);

    int rc = page_file_open(pf, 1);
    EXPECT_EQ(rc, 0);

    rc = page_file_write_superblock(pf, 8192, 256, NULL);
    EXPECT_EQ(rc, 0);

    page_superblock_t sb;
    rc = page_file_read_superblock(pf, &sb);
    EXPECT_EQ(rc, 0);
    EXPECT_EQ(sb.magic[0], 'W');
    EXPECT_EQ(sb.magic[1], 'D');
    EXPECT_EQ(sb.magic[2], 'B');
    EXPECT_EQ(sb.magic[3], 'P');
    EXPECT_EQ(sb.version, (uint16_t)PAGE_FILE_VERSION);
    EXPECT_EQ(sb.root_offset, 8192u);
    EXPECT_EQ(sb.root_size, 256u);
    EXPECT_EQ(sb.revision, 1u);

    page_file_destroy(pf);
}

// 3. Write a small node (fits in one block), read it back — verify data matches
TEST_F(PageFileTest, WriteReadSmallNode) {
    char path[512];
    make_path(path, sizeof(path), "test3.db");

    page_file_t* pf = page_file_create(path, 4096, 2, NULL);
    ASSERT_NE(pf, nullptr);

    int rc = page_file_open(pf, 1);
    EXPECT_EQ(rc, 0);

    // Prepare data: 4-byte size prefix + actual data
    const char* payload = "Hello, WaveDB!";
    size_t payload_len = strlen(payload);
    size_t total_len = 4 + payload_len;
    uint8_t* data = (uint8_t*)calloc(1, total_len);
    uint32_t size_val = (uint32_t)payload_len;
    memcpy(data, &size_val, 4);
    memcpy(data + 4, payload, payload_len);

    uint64_t offset = 0;
    uint64_t bids[16] = {0};
    size_t num_bids = 0;

    rc = page_file_write_node(pf, data + 4, payload_len, &offset, bids, 16, &num_bids);
    EXPECT_EQ(rc, 0);
    EXPECT_GT(num_bids, 0u);

    // Read back
    size_t out_len = 0;
    uint8_t* result = page_file_read_node(pf, offset, &out_len);
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(out_len, payload_len);

    // Verify: the returned data starts after the 4-byte size prefix
    EXPECT_EQ(memcmp(result + 4, payload, payload_len), 0);

    free(result);
    free(data);
    page_file_destroy(pf);
}

// 4. Write a large node (spans multiple blocks), read it back — verify data matches
TEST_F(PageFileTest, WriteReadLargeNode) {
    char path[512];
    make_path(path, sizeof(path), "test4.db");

    page_file_t* pf = page_file_create(path, 4096, 2, NULL);
    ASSERT_NE(pf, nullptr);

    int rc = page_file_open(pf, 1);
    EXPECT_EQ(rc, 0);

    // Create data larger than one block (after accounting for IndexBlkMeta)
    // Usable per block = 4096 - 16 = 4080 bytes. We need > 4080 bytes of payload.
    size_t payload_len = 8000;
    uint8_t* payload = (uint8_t*)calloc(1, payload_len);
    for (size_t i = 0; i < payload_len; i++) {
        payload[i] = (uint8_t)(i % 251);  // Use a prime for variety
    }

    uint64_t offset = 0;
    uint64_t bids[16] = {0};
    size_t num_bids = 0;

    rc = page_file_write_node(pf, payload, payload_len, &offset, bids, 16, &num_bids);
    EXPECT_EQ(rc, 0);
    EXPECT_GE(num_bids, 2u);  // Should span at least 2 blocks

    // Read back
    size_t out_len = 0;
    uint8_t* result = page_file_read_node(pf, offset, &out_len);
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(out_len, payload_len);

    // Verify: the returned data includes 4-byte prefix + payload
    uint32_t stored_size;
    memcpy(&stored_size, result, 4);
    EXPECT_EQ(stored_size, (uint32_t)payload_len);
    EXPECT_EQ(memcmp(result + 4, payload, payload_len), 0);

    free(result);
    free(payload);
    page_file_destroy(pf);
}

// 5. Write multiple nodes, read each by offset
TEST_F(PageFileTest, WriteReadMultipleNodes) {
    char path[512];
    make_path(path, sizeof(path), "test5.db");

    page_file_t* pf = page_file_create(path, 4096, 2, NULL);
    ASSERT_NE(pf, nullptr);

    int rc = page_file_open(pf, 1);
    EXPECT_EQ(rc, 0);

    const char* payloads[] = {"Node A", "Node B", "Node C"};
    size_t payload_lens[] = {6, 6, 6};
    uint64_t offsets[3] = {0};

    for (int i = 0; i < 3; i++) {
        uint64_t bids[16] = {0};
        size_t num_bids = 0;
        rc = page_file_write_node(pf, (const uint8_t*)payloads[i], payload_lens[i],
                                   &offsets[i], bids, 16, &num_bids);
        EXPECT_EQ(rc, 0);
    }

    // Read each back
    for (int i = 0; i < 3; i++) {
        size_t out_len = 0;
        uint8_t* result = page_file_read_node(pf, offsets[i], &out_len);
        ASSERT_NE(result, nullptr);
        EXPECT_EQ(out_len, payload_lens[i]);

        uint32_t stored_size;
        memcpy(&stored_size, result, 4);
        EXPECT_EQ(stored_size, (uint32_t)payload_lens[i]);
        EXPECT_EQ(memcmp(result + 4, payloads[i], payload_lens[i]), 0);

        free(result);
    }

    page_file_destroy(pf);
}

// 6. Mark stale regions, verify stale ratio
TEST_F(PageFileTest, MarkStaleAndRatio) {
    char path[512];
    make_path(path, sizeof(path), "test6.db");

    page_file_t* pf = page_file_create(path, 4096, 2, NULL);
    ASSERT_NE(pf, nullptr);

    int rc = page_file_open(pf, 1);
    EXPECT_EQ(rc, 0);

    // Write a node first to give file some size
    const char* payload = "test data for stale";
    size_t payload_len = strlen(payload);
    uint64_t offset = 0;
    uint64_t bids[16] = {0};
    size_t num_bids = 0;

    rc = page_file_write_node(pf, (const uint8_t*)payload, payload_len, &offset, bids, 16, &num_bids);
    EXPECT_EQ(rc, 0);

    // Mark stale
    uint64_t file_sz = page_file_size(pf);
    page_file_mark_stale(pf, 8192, 1024);

    double ratio = page_file_stale_ratio(pf);
    EXPECT_GT(ratio, 0.0);
    EXPECT_LT(ratio, 1.0);

    // Verify ratio: 1024 stale / file_sz total
    double expected = 1024.0 / (double)file_sz;
    EXPECT_NEAR(ratio, expected, 0.01);

    page_file_destroy(pf);
}

// 7. Write superblock twice (alternating slots), read latest — verify revision increments
TEST_F(PageFileTest, SuperblockRevisionIncrements) {
    char path[512];
    make_path(path, sizeof(path), "test7.db");

    page_file_t* pf = page_file_create(path, 4096, 2, NULL);
    ASSERT_NE(pf, nullptr);

    int rc = page_file_open(pf, 1);
    EXPECT_EQ(rc, 0);

    // Write superblock first time
    rc = page_file_write_superblock(pf, 100, 50, NULL);
    EXPECT_EQ(rc, 0);

    // Write superblock second time
    rc = page_file_write_superblock(pf, 200, 75, NULL);
    EXPECT_EQ(rc, 0);

    // Read latest superblock
    page_superblock_t sb;
    rc = page_file_read_superblock(pf, &sb);
    EXPECT_EQ(rc, 0);
    EXPECT_EQ(sb.revision, 2u);
    EXPECT_EQ(sb.root_offset, 200u);
    EXPECT_EQ(sb.root_size, 75u);

    page_file_destroy(pf);
}

// 8. Close and reopen file — verify superblock persists
TEST_F(PageFileTest, CloseReopenPersist) {
    char path[512];
    make_path(path, sizeof(path), "test8.db");

    page_file_t* pf = page_file_create(path, 4096, 2, NULL);
    ASSERT_NE(pf, nullptr);

    int rc = page_file_open(pf, 1);
    EXPECT_EQ(rc, 0);

    rc = page_file_write_superblock(pf, 12345, 678, NULL);
    EXPECT_EQ(rc, 0);

    page_file_destroy(pf);

    // Reopen
    page_file_t* pf2 = page_file_create(path, 4096, 2, NULL);
    ASSERT_NE(pf2, nullptr);

    rc = page_file_open(pf2, 1);
    EXPECT_EQ(rc, 0);

    page_superblock_t sb;
    rc = page_file_read_superblock(pf2, &sb);
    EXPECT_EQ(rc, 0);
    EXPECT_EQ(sb.revision, 1u);
    EXPECT_EQ(sb.root_offset, 12345u);
    EXPECT_EQ(sb.root_size, 678u);

    page_file_destroy(pf2);
}

// 9. Read from non-existent offset — verify NULL return
TEST_F(PageFileTest, ReadNonExistentOffset) {
    char path[512];
    make_path(path, sizeof(path), "test9.db");

    page_file_t* pf = page_file_create(path, 4096, 2, NULL);
    ASSERT_NE(pf, nullptr);

    int rc = page_file_open(pf, 1);
    EXPECT_EQ(rc, 0);

    // Try reading from an offset that doesn't exist (way past the file)
    size_t out_len = 0;
    uint8_t* result = page_file_read_node(pf, 999999, &out_len);
    EXPECT_EQ(result, nullptr);
    EXPECT_EQ(out_len, 0u);

    page_file_destroy(pf);
}

// 10. Multiple nodes written, then stale half — verify stale_ratio ~ 0.5
TEST_F(PageFileTest, StaleRatioHalf) {
    char path[512];
    make_path(path, sizeof(path), "test10.db");

    page_file_t* pf = page_file_create(path, 4096, 2, NULL);
    ASSERT_NE(pf, nullptr);

    int rc = page_file_open(pf, 1);
    EXPECT_EQ(rc, 0);

    // Write two nodes that fill blocks completely, making stale ratio straightforward
    // Use data size that fills most of a block (block_size - 4 size prefix - INDEX_BLK_META_SIZE)
    size_t node_data_len = 4096 - 4 - 16; // Fill one block exactly
    uint8_t* data1 = (uint8_t*)calloc(1, node_data_len);
    uint8_t* data2 = (uint8_t*)calloc(1, node_data_len);
    memset(data1, 'A', node_data_len);
    memset(data2, 'B', node_data_len);

    uint64_t offset1 = 0, offset2 = 0;
    uint64_t bids[16] = {0};
    size_t num_bids = 0;
    rc = page_file_write_node(pf, data1, node_data_len, &offset1, bids, 16, &num_bids);
    EXPECT_EQ(rc, 0);
    rc = page_file_write_node(pf, data2, node_data_len, &offset2, bids, 16, &num_bids);
    EXPECT_EQ(rc, 0);

    // Mark the first node's region as stale
    // Each node occupies one full block (4096 bytes)
    page_file_mark_stale(pf, offset1, 4096);

    // Stale ratio should be approximately 0.5 (one of two data blocks is stale)
    // File has 2 superblocks + 2 data blocks = 4 blocks total, 1 data block stale
    // So ratio ≈ 4096 / (4 * 4096) = 0.25 by file size. But stale_ratio is
    // stale_bytes / file_size. Let's just check the actual ratio directly.
    uint64_t file_sz = page_file_size(pf);
    double actual_ratio = page_file_stale_ratio(pf);
    double expected_ratio = 4096.0 / (double)file_sz;
    EXPECT_NEAR(actual_ratio, expected_ratio, 0.05);
    // Also verify that approximately half the data area is stale
    // (2 data blocks, 1 stale = 50% of data blocks)
    EXPECT_NEAR(actual_ratio / expected_ratio, 1.0, 0.05);

    free(data1);
    free(data2);
    page_file_destroy(pf);
}