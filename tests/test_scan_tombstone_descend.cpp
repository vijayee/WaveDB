// Test: forward scan over tombstoned prefix entries that still own a subtree.
//
// Regression test for the scan frame-confusion loop misdiagnosed as a trie
// cycle in docs/trie-cycle-investigation.md (see docs/trie-cycle-resolution.md).
//
// Setup: with chunk_size=4, the key component "aaaax" splits into chunks
// ["aaaa", "x"], so inserting both "t/aaaa" and "t/aaaax" gives the leaf
// entry for "aaaa" BOTH a value and a trie_child. Deleting "t/aaaa" then
// yields a tombstoned entry that still owns a live subtree. Two such
// entries in one bnode ("aaaa" and "bbbb") made database_scan_next push
// both trie_child frames while continuing to walk the parent; its
// stack[stack_depth - 2] parent re-fetch then resolved to the first pushed
// child, and the scan re-walked the parent's entries with the child
// frame's cursor, re-pushing the same child until the depth guard fired.
// The scan then errored out mid-range and the caller returned partial (or
// empty) results.
//
// Expected (fixed) behavior: the scan terminates and returns exactly the
// live subtree keys, in sorted order.

#include <gtest/gtest.h>

extern "C" {
#include "Database/database.h"
#include "Database/database_config.h"
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

class ScanTombstoneDescendTest : public ::testing::Test {
protected:
    char tmpdir[256];

    void SetUp() override {
#if _WIN32
        strcpy(tmpdir, getenv("TEMP"));
        strcat(tmpdir, "/wavedb_tomb_scan_XXXXXX");
        _mktemp(tmpdir);
        _mkdir(tmpdir);
#else
        strcpy(tmpdir, "/tmp/wavedb_tomb_scan_XXXXXX");
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

    // sync_only database — the mode the IVF rebuild runs in (tombstoning
    // deletes via hbtrie_delete_unsafe, scans over the tombstones).
    database_t* create_db() {
        database_config_t* config = database_config_default();
        config->chunk_size = 4;
        config->btree_node_size = 4096;
        config->worker_threads = 2;
        config->enable_persist = 1;
        config->timer_resolution_ms = 100;
        database_config_set_sync_only(config, 1);
        int error_code = 0;
        database_t* result = database_create_with_config(tmpdir, config, &error_code);
        database_config_destroy(config);
        return result;
    }

    // Insert two prefix keys + one extension each, then tombstone both
    // prefixes. Leaves the leaf bnode with two tombstoned entries that both
    // still carry a trie_child (the minimal shape that triggered the loop).
    void populate(database_t* db) {
        const raw_op_t puts[] = {
            {"t/aaaa",  6, (const uint8_t*)"1", 1, 0},
            {"t/aaaax", 7, (const uint8_t*)"2", 1, 0},
            {"t/bbbb",  6, (const uint8_t*)"3", 1, 0},
            {"t/bbbbx", 7, (const uint8_t*)"4", 1, 0},
        };
        ASSERT_EQ(database_batch_sync_raw(db, '/', puts, 4), 0);

        // Same delete-heavy batch shape as the IVF rebuild.
        const raw_op_t dels[] = {
            {"t/aaaa", 6, NULL, 0, 1},
            {"t/bbbb", 6, NULL, 0, 1},
        };
        ASSERT_EQ(database_batch_sync_raw(db, '/', dels, 2), 0);
    }

    void expect_live_keys(raw_result_t* results, size_t count) {
        ASSERT_EQ(count, 2u)
            << "scan lost results — iterator aborted mid-range (frame "
               "confusion on tombstoned prefix entries)";
        EXPECT_EQ(results[0].key_len, 7u);
        EXPECT_EQ(0, memcmp(results[0].key, "t/aaaax", 7));
        ASSERT_EQ(results[0].value_len, 1u);
        EXPECT_EQ(results[0].value[0], '2');
        EXPECT_EQ(results[1].key_len, 7u);
        EXPECT_EQ(0, memcmp(results[1].key, "t/bbbbx", 7));
        ASSERT_EQ(results[1].value_len, 1u);
        EXPECT_EQ(results[1].value[0], '4');
    }
};

// Full scan (no bounds): must terminate and emit exactly the two live
// subtree keys. Pre-fix this returned 0 results: the scan looped on the
// tombstoned prefixes until the depth guard fired, before ever processing
// the pushed child frames.
TEST_F(ScanTombstoneDescendTest, FullScanEmitsSubtreesOfTombstonedPrefixes) {
    database_t* db = create_db();
    ASSERT_NE(db, nullptr);
    populate(db);

    raw_result_t* results = NULL;
    size_t count = 0;
    int rc = database_scan_range_sync_raw(db, NULL, 0, NULL, 0, '/', &results, &count);
    ASSERT_EQ(rc, 0);
    expect_live_keys(results, count);

    database_raw_results_free(results, count);
    database_destroy(db);
}

// Bounded range scan (the IVF cluster-scan pattern that surfaced the bug).
TEST_F(ScanTombstoneDescendTest, RangeScanEmitsSubtreesOfTombstonedPrefixes) {
    database_t* db = create_db();
    ASSERT_NE(db, nullptr);
    populate(db);

    raw_result_t* results = NULL;
    size_t count = 0;
    int rc = database_scan_range_sync_raw(db, "t", 1, "u", 1, '/', &results, &count);
    ASSERT_EQ(rc, 0);
    expect_live_keys(results, count);

    database_raw_results_free(results, count);
    database_destroy(db);
}
