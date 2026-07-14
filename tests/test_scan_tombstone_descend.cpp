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

// Regression: delete-then-RE-PUT of multi-component keys (chunk_size=4) must
// remain visible to a sync_only range scan. The SLSH same-type migrate path
// (delete all hash keys vec/{idx}/hash/<4hex>/<id>, then retrain -> re-put the
// identical keys) exposed this: after delete+re-put, point-get still finds the
// live value but database_scan_range_sync_raw drops the re-put keys.
//
// Root cause (sync_only MVCC read_txn_id mismatch, NOT a trie-walk bug): the
// scan DOES reach the re-put leaves (the traversal is fine), but it resolves
// each leaf's version chain with version_entry_find_visible(read_txn_id), and
// in sync_only mode that read_txn_id was tx_manager_get_last_committed() --
// which is ZERO, because sync commits bypass tx_manager_commit (the fast
// put/get/increment paths skip the tx_manager for speed), so
// last_committed_txn_id never advances past its zero init. The re-put stamps
// its live version with a real txn_id (> 0); find_visible()'s fast path test
// `head.txn_id <= 0` is false, the slow path finds no version with txn_id <=
// 0, and it returns NULL -- the key vanishes from the scan. A key that was
// never delete+re-put has no version chain (legacy single-value leaf) and
// bypasses find_visible, so it stays visible. That asymmetry is exactly why
// point-get (sync_only database_get_sync uses hbtrie_find_unsafe, which
// ignores read_txn_id and returns the head version directly) still sees the
// re-put value while the scan does not.
//
// Fix: scan_read_txn_id() in database_iterator.c returns a sentinel MAX
// read_txn_id in sync_only mode so find_visible()'s fast path always returns
// the head (live head -> visible, tombstone head -> NULL) -- matching
// hbtrie_find_unsafe's sync_only visibility. This test guards that fix.
//
// Shape mirrors SLSH hash keys: prefix "aaaa" (one chunk, no value at t/aaaa)
// owns child keys t/aaaa/v0, t/aaaa/v1. Delete both children, re-put both,
// scan t/ -> must emit v0 and v1. A never-deleted sibling t/bbbb/keep proves
// the scan reaches past the re-put subtree (it does not abort) and that the
// drop is specific to versioned (delete+reput) leaves.
TEST_F(ScanTombstoneDescendTest, ReputChildKeysRemainVisibleToScan) {
    database_t* db = create_db();
    ASSERT_NE(db, nullptr);

    /* Two prefixes: "aaaa" gets delete-then-reput; "bbbb" is a never-deleted
       sibling keeper so we can see WHICH subtree the scan drops. */
    const raw_op_t puts1[] = {
        {"t/aaaa/v0", 9, (const uint8_t*)"1", 1, 0},
        {"t/aaaa/v1", 9, (const uint8_t*)"2", 1, 0},
        {"t/bbbb/keep", 11, (const uint8_t*)"K", 1, 0},
    };
    ASSERT_EQ(database_batch_sync_raw(db, '/', puts1, 3), 0);

    /* Baseline: scan sees all three. */
    raw_result_t* r0 = NULL; size_t n0 = 0;
    ASSERT_EQ(database_scan_range_sync_raw(db, "t", 1, "u", 1, '/', &r0, &n0), 0);
    ASSERT_EQ(n0, 3u) << "baseline scan lost keys before any delete";
    database_raw_results_free(r0, n0);

    /* Delete only the "aaaa" children (tombstones under that prefix node). */
    const raw_op_t dels[] = {
        {"t/aaaa/v0", 9, NULL, 0, 1},
        {"t/aaaa/v1", 9, NULL, 0, 1},
    };
    ASSERT_EQ(database_batch_sync_raw(db, '/', dels, 2), 0);

    /* Re-put the "aaaa" children (the migrate/retrain pattern: same keys, new txn). */
    const raw_op_t reput[] = {
        {"t/aaaa/v0", 9, (const uint8_t*)"1", 1, 0},
        {"t/aaaa/v1", 9, (const uint8_t*)"2", 1, 0},
    };
    ASSERT_EQ(database_batch_sync_raw(db, '/', reput, 2), 0);

    /* Point-get must find the re-put value (sanity: the key IS present). */
    uint8_t* gv = NULL; size_t gvl = 0;
    ASSERT_EQ(database_get_sync_raw(db, "t/aaaa/v1", 9, '/', &gv, &gvl), 0);
    ASSERT_EQ(gvl, 1u);
    EXPECT_EQ(gv[0], '2');
    database_raw_value_free(gv);

    /* Scan must also see all three keys. Pre-fix this dropped the re-put
       "aaaa" children while still emitting the never-deleted sibling (the scan
       reaches past the re-put subtree; the drop is version-chain visibility,
       not a traversal abort). Print which keys the scan returns. */
    auto dump_scan = [](database_t* d, const char* tag) -> size_t {
        raw_result_t* r = NULL; size_t n = 0;
        int rc = database_scan_range_sync_raw(d, "t", 1, "u", 1, '/', &r, &n);
        std::cout << "reput scan (" << tag << "): count=" << n << " keys={";
        if (rc == 0) {
            for (size_t i = 0; i < n; i++) {
                std::cout << " \"" << std::string((const char*)r[i].key, r[i].key_len) << "\"";
            }
        }
        std::cout << " }\n";
        database_raw_results_free(r, n);
        return (rc == 0) ? n : (size_t)-1;
    };
    size_t same_count = dump_scan(db, "same handle");

    /* Reopen (fresh read_txn_id + fresh bnode cache + WAL replay) and rescan.
       The bug persisted across reopen -- the on-disk version chains are stamped
       with real txn_ids and a fresh reader's sync_only read_txn_id was still 0,
       so the reopened scan dropped the re-put keys too. */
    database_destroy(db);
    db = create_db();
    ASSERT_NE(db, nullptr);
    size_t reopened_count = dump_scan(db, "reopened  ");

    /* CONFIRMATION point-get: on the REOPENED db (fresh reader, post-commit)
       point-get still finds the re-put value. This proves the key is durably
       present and the scan's failure to emit it is the sync_only read_txn_id
       visibility bug, not a missing key / uncommitted re-put. */
    uint8_t* rg = NULL; size_t rgl = 0;
    int rgrc = database_get_sync_raw(db, "t/aaaa/v1", 9, '/', &rg, &rgl);
    std::cout << "reput point-get (reopened): rc=" << rgrc
              << " len=" << (int)rgl << "\n";
    if (rg) database_raw_value_free(rg);

    /* A freshly-opened sync_only reader must see committed re-put keys
       alongside the never-deleted sibling. */
    ASSERT_EQ(reopened_count, 3u)
        << "reopened sync_only scan lost re-put keys -- version_entry_find_"
           "visible was called with read_txn_id==0 (sync commits never advance "
           "last_committed_txn_id), hiding every versioned (delete+reput) leaf";
    ASSERT_EQ(same_count, 3u);
    database_destroy(db);
}
