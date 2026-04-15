//
// Test for bnode_cache_t using Google Test
//

#include <gtest/gtest.h>
#include "Storage/bnode_cache.h"
#include "Storage/page_file.h"

#include <cstdlib>
#include <cstring>
#include <cstdio>

class BnodeCacheTest : public ::testing::Test {
protected:
    char tmpdir[256];

    void SetUp() override {
        strcpy(tmpdir, "/tmp/bnode_cache_test_XXXXXX");
        mkdtemp(tmpdir);
    }

    void TearDown() override {
        char cmd[512];
        snprintf(cmd, sizeof(cmd), "rm -rf %s", tmpdir);
        system(cmd);
    }

    char* make_path(char* buf, size_t bufsize, const char* name) {
        snprintf(buf, bufsize, "%s/%s", tmpdir, name);
        return buf;
    }

    /* Helper: build data buffer with 4-byte size prefix */
    static uint8_t* make_prefixed_data(const uint8_t* payload, size_t payload_len, size_t* out_total_len) {
        size_t total = 4 + payload_len;
        uint8_t* buf = (uint8_t*)calloc(1, total);
        uint32_t sz = (uint32_t)payload_len;
        memcpy(buf, &sz, 4);
        memcpy(buf + 4, payload, payload_len);
        *out_total_len = total;
        return buf;
    }
};

// 1. Create and destroy manager (no leaks under ASan)
TEST_F(BnodeCacheTest, CreateDestroyManager) {
    bnode_cache_mgr_t* mgr = bnode_cache_mgr_create(1024 * 1024, 4);
    ASSERT_NE(mgr, nullptr);
    EXPECT_EQ(mgr->file_count, 0u);
    EXPECT_EQ(mgr->max_total_memory, 1024u * 1024u);
    EXPECT_EQ(mgr->num_shards, 4u);

    bnode_cache_mgr_destroy(mgr);
}

// 2. Create file cache, write a node, read it back — verify data matches
TEST_F(BnodeCacheTest, WriteReadNode) {
    char path[512];
    make_path(path, sizeof(path), "test2.db");

    page_file_t* pf = page_file_create(path, 4096, 2);
    ASSERT_NE(pf, nullptr);
    int rc = page_file_open(pf, 1);
    EXPECT_EQ(rc, 0);

    bnode_cache_mgr_t* mgr = bnode_cache_mgr_create(1024 * 1024, 4);
    ASSERT_NE(mgr, nullptr);

    file_bnode_cache_t* fcache = bnode_cache_create_file_cache(mgr, pf, "test2.db");
    ASSERT_NE(fcache, nullptr);

    /* Write a node to the cache at offset 8192 (past superblocks) */
    const char* payload = "Hello, BnodeCache!";
    size_t payload_len = strlen(payload);
    size_t total_len = 0;
    uint8_t* data = make_prefixed_data((const uint8_t*)payload, payload_len, &total_len);

    rc = bnode_cache_write(fcache, 8192, data, total_len);
    EXPECT_EQ(rc, 0);

    /* Read it back */
    bnode_cache_item_t* item = bnode_cache_read(fcache, 8192);
    ASSERT_NE(item, nullptr);
    EXPECT_EQ(item->data_len, total_len);
    EXPECT_EQ(memcmp(item->data, data, total_len), 0);
    EXPECT_EQ(item->is_dirty, 1);
    EXPECT_EQ(item->ref_count, 1u);

    bnode_cache_release(fcache, item);
    free(data);
    bnode_cache_destroy_file_cache(fcache);
    bnode_cache_mgr_destroy(mgr);
    page_file_destroy(pf);
}

// 3. Write multiple nodes, read each by offset
TEST_F(BnodeCacheTest, WriteReadMultipleNodes) {
    char path[512];
    make_path(path, sizeof(path), "test3.db");

    page_file_t* pf = page_file_create(path, 4096, 2);
    ASSERT_NE(pf, nullptr);
    int rc = page_file_open(pf, 1);
    EXPECT_EQ(rc, 0);

    bnode_cache_mgr_t* mgr = bnode_cache_mgr_create(1024 * 1024, 2);
    ASSERT_NE(mgr, nullptr);

    file_bnode_cache_t* fcache = bnode_cache_create_file_cache(mgr, pf, "test3.db");
    ASSERT_NE(fcache, nullptr);

    /* Write 3 nodes at different offsets */
    const char* payloads[] = {"Node Alpha", "Node Beta", "Node Gamma"};
    uint64_t offsets[] = {8192, 12288, 16384};
    size_t total_lens[3] = {0};
    uint8_t* datas[3] = {nullptr};

    for (int i = 0; i < 3; i++) {
        datas[i] = make_prefixed_data((const uint8_t*)payloads[i], strlen(payloads[i]), &total_lens[i]);
        rc = bnode_cache_write(fcache, offsets[i], datas[i], total_lens[i]);
        EXPECT_EQ(rc, 0);
    }

    /* Read each back and verify */
    for (int i = 0; i < 3; i++) {
        bnode_cache_item_t* item = bnode_cache_read(fcache, offsets[i]);
        ASSERT_NE(item, nullptr) << "Failed to read node at offset " << offsets[i];
        EXPECT_EQ(item->data_len, total_lens[i]);
        EXPECT_EQ(memcmp(item->data, datas[i], total_lens[i]), 0);
        bnode_cache_release(fcache, item);
    }

    for (int i = 0; i < 3; i++) {
        free(datas[i]);
    }
    bnode_cache_destroy_file_cache(fcache);
    bnode_cache_mgr_destroy(mgr);
    page_file_destroy(pf);
}

// 4. Write dirty nodes, flush, verify dirty count drops to 0
TEST_F(BnodeCacheTest, FlushDirty) {
    char path[512];
    make_path(path, sizeof(path), "test4.db");

    page_file_t* pf = page_file_create(path, 4096, 2);
    ASSERT_NE(pf, nullptr);
    int rc = page_file_open(pf, 1);
    EXPECT_EQ(rc, 0);

    bnode_cache_mgr_t* mgr = bnode_cache_mgr_create(1024 * 1024, 1);
    ASSERT_NE(mgr, nullptr);

    file_bnode_cache_t* fcache = bnode_cache_create_file_cache(mgr, pf, "test4.db");
    ASSERT_NE(fcache, nullptr);

    /* Write dirty nodes */
    const char* payload = "dirty data";
    size_t total_len = 0;
    uint8_t* data = make_prefixed_data((const uint8_t*)payload, strlen(payload), &total_len);

    uint64_t offsets[] = {8192, 12288, 16384};
    for (int i = 0; i < 3; i++) {
        rc = bnode_cache_write(fcache, offsets[i], data, total_len);
        EXPECT_EQ(rc, 0);
    }

    /* Verify dirty count before flush */
    EXPECT_EQ(bnode_cache_dirty_count(fcache), 3u);
    EXPECT_GT(bnode_cache_dirty_bytes(fcache), 0u);

    /* Flush all dirty nodes */
    rc = bnode_cache_flush_dirty(fcache);
    EXPECT_EQ(rc, 0);

    /* Verify dirty count drops to 0 */
    EXPECT_EQ(bnode_cache_dirty_count(fcache), 0u);
    EXPECT_EQ(bnode_cache_dirty_bytes(fcache), 0u);

    free(data);
    bnode_cache_destroy_file_cache(fcache);
    bnode_cache_mgr_destroy(mgr);
    page_file_destroy(pf);
}

// 5. Write more than cache limit, verify eviction of clean nodes
TEST_F(BnodeCacheTest, EvictionOnMemoryPressure) {
    char path[512];
    make_path(path, sizeof(path), "test5.db");

    page_file_t* pf = page_file_create(path, 4096, 2);
    ASSERT_NE(pf, nullptr);
    int rc = page_file_open(pf, 1);
    EXPECT_EQ(rc, 0);

    /* Small memory limit to force eviction */
    bnode_cache_mgr_t* mgr = bnode_cache_mgr_create(200, 1);
    ASSERT_NE(mgr, nullptr);

    file_bnode_cache_t* fcache = bnode_cache_create_file_cache(mgr, pf, "test5.db");
    ASSERT_NE(fcache, nullptr);

    /* Write node A (80 bytes total with prefix) — marks dirty */
    const char* payload_a = "AAAA";
    size_t total_a = 0;
    uint8_t* data_a = make_prefixed_data((const uint8_t*)payload_a, strlen(payload_a), &total_a);

    rc = bnode_cache_write(fcache, 8192, data_a, total_a);
    EXPECT_EQ(rc, 0);

    /* Write node B (80 bytes total with prefix) — marks dirty */
    const char* payload_b = "BBBB";
    size_t total_b = 0;
    uint8_t* data_b = make_prefixed_data((const uint8_t*)payload_b, strlen(payload_b), &total_b);

    rc = bnode_cache_write(fcache, 12288, data_b, total_b);
    EXPECT_EQ(rc, 0);

    /* Flush to make A and B clean */
    rc = bnode_cache_flush_dirty(fcache);
    EXPECT_EQ(rc, 0);
    EXPECT_EQ(bnode_cache_dirty_count(fcache), 0u);

    /* Release references so they become evictable */
    bnode_cache_item_t* item_a = bnode_cache_read(fcache, 8192);
    ASSERT_NE(item_a, nullptr);
    bnode_cache_item_t* item_b = bnode_cache_read(fcache, 12288);
    ASSERT_NE(item_b, nullptr);
    bnode_cache_release(fcache, item_a);
    bnode_cache_release(fcache, item_b);

    /* After flush, offsets changed. Read at the new offsets to release refs. */
    /* Since we flushed, offsets may have changed. But item_a/item_b still
       point to valid cache items with updated offsets. Release them. */

    /* Now write node C to exceed the memory limit — should evict A or B */
    const char* payload_c = "CCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC";
    size_t total_c = 0;
    uint8_t* data_c = make_prefixed_data((const uint8_t*)payload_c, strlen(payload_c), &total_c);

    rc = bnode_cache_write(fcache, 16384, data_c, total_c);
    EXPECT_EQ(rc, 0);

    /* At least one clean node should have been evicted */
    /* current_memory should be <= max_memory (200) */
    EXPECT_LE(fcache->current_memory, fcache->max_memory);

    free(data_a);
    free(data_b);
    free(data_c);
    bnode_cache_destroy_file_cache(fcache);
    bnode_cache_mgr_destroy(mgr);
    page_file_destroy(pf);
}

// 6. Reference counting: read a node twice, verify ref_count, release both
TEST_F(BnodeCacheTest, ReferenceCounting) {
    char path[512];
    make_path(path, sizeof(path), "test6.db");

    page_file_t* pf = page_file_create(path, 4096, 2);
    ASSERT_NE(pf, nullptr);
    int rc = page_file_open(pf, 1);
    EXPECT_EQ(rc, 0);

    bnode_cache_mgr_t* mgr = bnode_cache_mgr_create(1024 * 1024, 1);
    ASSERT_NE(mgr, nullptr);

    file_bnode_cache_t* fcache = bnode_cache_create_file_cache(mgr, pf, "test6.db");
    ASSERT_NE(fcache, nullptr);

    /* Write a node (ref_count = 0 since no read) */
    const char* payload = "refcount test";
    size_t total_len = 0;
    uint8_t* data = make_prefixed_data((const uint8_t*)payload, strlen(payload), &total_len);

    rc = bnode_cache_write(fcache, 8192, data, total_len);
    EXPECT_EQ(rc, 0);

    /* Read the node first time — ref_count should be 1 */
    bnode_cache_item_t* item1 = bnode_cache_read(fcache, 8192);
    ASSERT_NE(item1, nullptr);
    EXPECT_EQ(item1->ref_count, 1u);

    /* Read the same node second time — ref_count should be 2 */
    bnode_cache_item_t* item2 = bnode_cache_read(fcache, 8192);
    ASSERT_NE(item2, nullptr);
    EXPECT_EQ(item1->ref_count, 2u);
    EXPECT_EQ(item2, item1); /* Same item pointer */

    /* Release first — ref_count should be 1 */
    bnode_cache_release(fcache, item1);
    EXPECT_EQ(item2->ref_count, 1u);

    /* Release second — ref_count should be 0 */
    bnode_cache_release(fcache, item2);
    EXPECT_EQ(item1->ref_count, 0u);

    free(data);
    bnode_cache_destroy_file_cache(fcache);
    bnode_cache_mgr_destroy(mgr);
    page_file_destroy(pf);
}

// 7. Invalidate a node, verify it's removed from cache
TEST_F(BnodeCacheTest, InvalidateNode) {
    char path[512];
    make_path(path, sizeof(path), "test7.db");

    page_file_t* pf = page_file_create(path, 4096, 2);
    ASSERT_NE(pf, nullptr);
    int rc = page_file_open(pf, 1);
    EXPECT_EQ(rc, 0);

    bnode_cache_mgr_t* mgr = bnode_cache_mgr_create(1024 * 1024, 1);
    ASSERT_NE(mgr, nullptr);

    file_bnode_cache_t* fcache = bnode_cache_create_file_cache(mgr, pf, "test7.db");
    ASSERT_NE(fcache, nullptr);

    /* Write a node */
    const char* payload = "invalidate me";
    size_t total_len = 0;
    uint8_t* data = make_prefixed_data((const uint8_t*)payload, strlen(payload), &total_len);

    rc = bnode_cache_write(fcache, 8192, data, total_len);
    EXPECT_EQ(rc, 0);

    /* Verify it exists */
    bnode_cache_item_t* item = bnode_cache_read(fcache, 8192);
    ASSERT_NE(item, nullptr);
    bnode_cache_release(fcache, item);

    /* Invalidate the node */
    rc = bnode_cache_invalidate(fcache, 8192);
    EXPECT_EQ(rc, 0);

    /* Verify it's gone — read should return NULL (cache miss + page file miss) */
    item = bnode_cache_read(fcache, 8192);
    EXPECT_EQ(item, nullptr);

    free(data);
    bnode_cache_destroy_file_cache(fcache);
    bnode_cache_mgr_destroy(mgr);
    page_file_destroy(pf);
}

// 8. Flush with no dirty nodes — no-op, returns 0
TEST_F(BnodeCacheTest, FlushNoDirty) {
    char path[512];
    make_path(path, sizeof(path), "test8.db");

    page_file_t* pf = page_file_create(path, 4096, 2);
    ASSERT_NE(pf, nullptr);
    int rc = page_file_open(pf, 1);
    EXPECT_EQ(rc, 0);

    bnode_cache_mgr_t* mgr = bnode_cache_mgr_create(1024 * 1024, 1);
    ASSERT_NE(mgr, nullptr);

    file_bnode_cache_t* fcache = bnode_cache_create_file_cache(mgr, pf, "test8.db");
    ASSERT_NE(fcache, nullptr);

    /* No writes — no dirty nodes */
    EXPECT_EQ(bnode_cache_dirty_count(fcache), 0u);

    /* Flush should be a no-op */
    rc = bnode_cache_flush_dirty(fcache);
    EXPECT_EQ(rc, 0);
    EXPECT_EQ(bnode_cache_dirty_count(fcache), 0u);

    bnode_cache_destroy_file_cache(fcache);
    bnode_cache_mgr_destroy(mgr);
    page_file_destroy(pf);
}

// 9. Multi-shard: write nodes at different offsets, verify all readable
TEST_F(BnodeCacheTest, MultiShardReadWrite) {
    char path[512];
    make_path(path, sizeof(path), "test9.db");

    page_file_t* pf = page_file_create(path, 4096, 2);
    ASSERT_NE(pf, nullptr);
    int rc = page_file_open(pf, 1);
    EXPECT_EQ(rc, 0);

    /* Use 4 shards */
    bnode_cache_mgr_t* mgr = bnode_cache_mgr_create(1024 * 1024, 4);
    ASSERT_NE(mgr, nullptr);

    file_bnode_cache_t* fcache = bnode_cache_create_file_cache(mgr, pf, "test9.db");
    ASSERT_NE(fcache, nullptr);

    /* Write nodes at offsets that map to different shards (offset % 4) */
    struct {
        uint64_t offset;
        const char* payload;
    } nodes[] = {
        { 8192,  "shard0" },  /* 8192 % 4 = 0 */
        { 8193,  "shard1" },  /* 8193 % 4 = 1 */
        { 8194,  "shard2" },  /* 8194 % 4 = 2 */
        { 8195,  "shard3" },  /* 8195 % 4 = 3 */
    };
    int num_nodes = 4;

    uint8_t* datas[4] = {nullptr};
    size_t total_lens[4] = {0};

    for (int i = 0; i < num_nodes; i++) {
        datas[i] = make_prefixed_data((const uint8_t*)nodes[i].payload,
                                       strlen(nodes[i].payload), &total_lens[i]);
        rc = bnode_cache_write(fcache, nodes[i].offset, datas[i], total_lens[i]);
        EXPECT_EQ(rc, 0);
    }

    /* Read each back and verify */
    for (int i = 0; i < num_nodes; i++) {
        bnode_cache_item_t* item = bnode_cache_read(fcache, nodes[i].offset);
        ASSERT_NE(item, nullptr) << "Failed to read node at offset " << nodes[i].offset;
        EXPECT_EQ(item->data_len, total_lens[i]);
        EXPECT_EQ(memcmp(item->data, datas[i], total_lens[i]), 0);
        bnode_cache_release(fcache, item);
    }

    for (int i = 0; i < num_nodes; i++) {
        free(datas[i]);
    }
    bnode_cache_destroy_file_cache(fcache);
    bnode_cache_mgr_destroy(mgr);
    page_file_destroy(pf);
}