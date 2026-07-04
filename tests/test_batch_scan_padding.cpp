// Test: scan after database_batch_sync_raw — verify no per-subscript null padding.
// Reproduces the padding bug observed via the graph layer's batch path.

#include <gtest/gtest.h>

extern "C" {
#include "Database/database.h"
#include "Database/database_config.h"
#include "Database/database_subtree.h"
#include "Database/database_iterator.h"
#include "HBTrie/hbtrie.h"
#include "HBTrie/path.h"
#include "HBTrie/identifier.h"
#include "Buffer/buffer.h"
#include "Util/allocator.h"
}

#if _WIN32
#include <io.h>
#include <direct.h>
#define getpid() _getpid()
#define mkdir(path, mode) _mkdir(path)
#else
#include <unistd.h>
#include <sys/stat.h>
#endif
#include <cstdlib>
#include <cstring>
#include <cstdio>

class BatchScanPaddingTest : public ::testing::Test {
protected:
    char tmpdir[256];

    void SetUp() override {
#if _WIN32
        strcpy(tmpdir, getenv("TEMP"));
        strcat(tmpdir, "/wavedb_batch_scan_XXXXXX");
        _mktemp(tmpdir);
        _mkdir(tmpdir);
#else
        strcpy(tmpdir, "/tmp/wavedb_batch_scan_XXXXXX");
        mkdtemp(tmpdir);
#endif
    }

    void TearDown() override {
        char cmd[512];
#if _WIN32
        snprintf(cmd, sizeof(cmd), "rmdir /s /q %s", tmpdir);
#else
        snprintf(cmd, sizeof(cmd), "rm -rf %s", tmpdir);
#endif
        system(cmd);
    }

    database_t* create_db() {
        database_config_t* config = database_config_default();
        config->chunk_size = 4;
        config->btree_node_size = 4096;
        config->worker_threads = 2;
        config->enable_persist = 1;
        config->timer_resolution_ms = 100;
        int error_code = 0;
        database_t* result = database_create_with_config(tmpdir, config, &error_code);
        database_config_destroy(config);
        return result;
    }
};

// Reproduce: insert via database_batch_sync_raw, then scan and check for padding.
TEST_F(BatchScanPaddingTest, BatchInsertThenScanNoPadding) {
    database_t* db = create_db();
    ASSERT_NE(db, nullptr);

    // Insert via batch (mirrors the graph layer's write path).
    // Key: "graph/spo/alice/knows/bob" — 25 bytes, multi-subscript, multi-chunk subscripts.
    const char* key = "graph/spo/alice/knows/bob";
    size_t key_len = strlen(key);  // 25
    raw_op_t ops[1];
    ops[0].type = 0;  // put
    ops[0].key = const_cast<char*>(key);
    ops[0].key_len = key_len;
    ops[0].value = const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(""));
    ops[0].value_len = 0;

    int rc = database_batch_sync_raw(db, '/', ops, 1);
    ASSERT_EQ(rc, 0);

    // Scan and verify no padding.
    raw_result_t* results = NULL;
    size_t count = 0;
    rc = database_scan_range_sync_raw(db, NULL, 0, NULL, 0, '/', &results, &count);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(count, 1u);

    // The key must be exactly "graph/spo/alice/knows/bob" (25 bytes, no nulls).
    EXPECT_EQ(results[0].key_len, key_len)
        << "Expected key length " << key_len << ", got " << results[0].key_len;
    EXPECT_EQ(0, memcmp(results[0].key, key, key_len))
        << "Key data mismatch — padding present?";

    // Explicitly check for null bytes inside the key (padding indicator).
    for (size_t j = 0; j < results[0].key_len; j++) {
        EXPECT_NE(results[0].key[j], '\0')
            << "Null byte found in key at position " << j;
    }

    database_raw_results_free(results, count);
    database_destroy(db);
}

// Reproduce: insert via database_subtree_batch_sync_raw, then scan the parent
// db and check for padding. This is the exact path the graph layer uses.
TEST_F(BatchScanPaddingTest, SubtreeBatchInsertThenScanNoPadding) {
    database_t* db = create_db();
    ASSERT_NE(db, nullptr);

    database_subtree_t* st = database_subtree_open(db, "graph", '/');
    ASSERT_NE(st, nullptr);

    // Insert via subtree batch (mirrors graph_insert_sync's path).
    // Key passed to subtree batch: "/spo/alice/knows/bob" (with leading "/").
    // The subtree prepends "graph" + delimiter → "graph//spo/alice/knows/bob".
    // path_create_from_raw skips empty segments, so the stored path is
    // [graph, spo, alice, knows, bob] — rendered as "graph/spo/alice/knows/bob".
    const char* key = "/spo/alice/knows/bob";
    size_t key_len = strlen(key);  // 20
    const char* expected_key = "graph/spo/alice/knows/bob";
    size_t expected_len = strlen(expected_key);  // 25

    raw_op_t ops[1];
    ops[0].type = 0;
    ops[0].key = const_cast<char*>(key);
    ops[0].key_len = key_len;
    ops[0].value = const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(""));
    ops[0].value_len = 0;

    int rc = database_subtree_batch_sync_raw(st, '/', ops, 1);
    ASSERT_EQ(rc, 0);

    database_subtree_close(st);

    // Scan the parent db and verify no padding.
    raw_result_t* results = NULL;
    size_t count = 0;
    rc = database_scan_range_sync_raw(db, NULL, 0, NULL, 0, '/', &results, &count);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(count, 1u);

    EXPECT_EQ(results[0].key_len, expected_len)
        << "Expected key length " << expected_len << ", got " << results[0].key_len;
    EXPECT_EQ(0, memcmp(results[0].key, expected_key, expected_len))
        << "Key data mismatch — padding present?";

    for (size_t j = 0; j < results[0].key_len; j++) {
        EXPECT_NE(results[0].key[j], '\0')
            << "Null byte found in key at position " << j;
    }

    database_raw_results_free(results, count);
    database_destroy(db);
}