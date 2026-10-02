// test_reopen_amnesia.cpp — the post-reopen same-session amnesia reproducer.
//
// The recorded defect (SecretAgent's restart-verification node): in
// CONCURRENT mode, a write committed after a database reopen is durable
// (a later-opened handle reads it) but invisible to the SAME handle's own
// scans and point-gets. This file reproduces it at the smallest possible
// level: one database, three handles across two boots, using the store
// actor's exact entry points (subtree raw get / subtree raw batch write /
// root-level bounded reverse scan).
//
// EXPECTED (a healthy store): every probe below sees the writes, and every
// test passes.
#include <gtest/gtest.h>

extern "C" {
#include "Database/database.h"
#include "Database/database_subtree.h"
#include "Database/database_iterator.h"
#include "Buffer/buffer.h"
}

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

class ReopenAmnesiaTest : public ::testing::Test {
   protected:
    void SetUp() override {
        char tmpdir[128];
#if _WIN32
        strcpy(tmpdir, "C:\\Temp\\wavedb_reopen_XXXXXX");
#else
        std::string base = std::string(getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp") +
                           "/wavedb_reopen_XXXXXX";
        strcpy(tmpdir, base.c_str());
        mkdtemp(tmpdir);
#endif
        location_ = tmpdir;
    }

    void TearDown() override {
        if (db_ != nullptr) {
            database_destroy(db_);
            db_ = nullptr;
        }
        char cmd[512];
#if _WIN32
        snprintf(cmd, sizeof(cmd), "rmdir /s /q %s", location_.c_str());
#else
        snprintf(cmd, sizeof(cmd), "rm -rf %s", location_.c_str());
#endif
        system(cmd);
    }

    // The persistence-phase2 sibling's create_db shape (worker_threads = 2 —
    // concurrent mode runs the background eviction task on those workers).
    database_t* open_db() {
        database_config_t* config = database_config_default();
        config->enable_persist = 1;
        config->sync_only = 0;   // the CONCURRENT mode the recorded defect names
        config->chunk_size = 4;
        config->btree_node_size = 4096;
        config->worker_threads = 2;
        config->timer_resolution_ms = 100;
        int error_code = 0;
        database_t* db = database_create_with_config(location_.c_str(), config, &error_code);
        database_config_destroy(config);
        return db;
    }

    void put(database_t* db, const char* key, const char* value) {
        EXPECT_EQ(database_put_sync_raw(db, key, strlen(key), '/',
                                        (const uint8_t*)value, strlen(value)), 0);
    }

    // Returns the value text, or "" on a miss (the probe, not an assertion).
    std::string probe(database_t* db, const char* key) {
        uint8_t* value = nullptr;
        size_t len = 0;
        int rc = database_get_sync_raw(db, key, strlen(key), '/', &value, &len);
        std::string out;
        if (rc == 0 && value != nullptr) {
            out.assign((char*)value, len);
            free(value);
        }
        return out;
    }

    // The store actor's shape: a subtree raw point-get with the "sessions/<sid>"
    // prefix, key = the subtree-relative remainder.
    std::string subtree_probe(database_subtree_t* st, const char* key) {
        uint8_t* value = nullptr;
        size_t len = 0;
        int rc = database_subtree_get_sync_raw(st, key, strlen(key), '/', &value, &len);
        std::string out;
        if (rc == 0 && value != nullptr) {
            out.assign((char*)value, len);
            free(value);
        }
        return out;
    }

    int subtree_put(database_subtree_t* st, const char* key, const char* value) {
        raw_op_t op;
        op.key = key;
        op.key_len = strlen(key);
        op.value = (const uint8_t*)value;
        op.value_len = strlen(value);
        op.type = 0;
        return database_subtree_batch_sync_raw(st, '/', &op, 1);
    }

    // The store actor's shape: an absolute root-level bounded reverse scan
    // (start <= key < end, half-open), returning the VALUES in scan order
    // (descending), each freed after a copy.
    std::vector<std::string> reverse_scan(database_t* db, const char* start_s,
                                          const char* end_s) {
        std::vector<std::string> out;
        path_t* start = path_create_from_raw(start_s, strlen(start_s), '/', 0);
        path_t* end = path_create_from_raw(end_s, strlen(end_s), '/', 0);
        database_iterator_t* iter = database_scan_start_reverse(db, start, end);
        if (iter == nullptr) return out;
        path_t* k = nullptr;
        identifier_t* v = nullptr;
        while (database_scan_prev(iter, &k, &v) == 0) {
            size_t len = 0;
            uint8_t* data = identifier_get_data_copy(v, &len);
            if (data != nullptr) {
                out.emplace_back((char*)data, len);
                free(data);
            }
            if (k != nullptr) path_destroy(k);
            if (v != nullptr) identifier_destroy(v);
            k = nullptr;
            v = nullptr;
        }
        database_scan_end(iter);
        return out;
    }

    std::string location_;
    database_t* db_ = nullptr;
};

// --- Level 1: raw database point-gets (this passes on a healthy store) ---
TEST_F(ReopenAmnesiaTest, PostReopenWriteVisibleToThatHandlesOwnReads) {
    db_ = open_db();
    ASSERT_NE(db_, nullptr);
    put(db_, "sessions/s1/track/k1", "v1");
    put(db_, "sessions/s2/track/k1", "w1");
    EXPECT_EQ(probe(db_, "sessions/s1/track/k1"), "v1");
    EXPECT_EQ(probe(db_, "sessions/s2/track/k1"), "w1");

    database_destroy(db_);
    db_ = open_db();
    ASSERT_NE(db_, nullptr);
    EXPECT_EQ(probe(db_, "sessions/s1/track/k1"), "v1");
    EXPECT_EQ(probe(db_, "sessions/s2/track/k1"), "w1");

    put(db_, "sessions/s1/track/k2", "v2");
    put(db_, "sessions/s2/track/k2", "w2");
    EXPECT_EQ(probe(db_, "sessions/s1/track/k2"), "v2");
    EXPECT_EQ(probe(db_, "sessions/s2/track/k2"), "w2");
}

// --- Level 2: the store actor's exact entry points ------------------------
// Subtree batch write + subtree raw get + root-level bounded reverse scan.
TEST_F(ReopenAmnesiaTest, SameSessionSubtreeReadsSeePostReopenWrites) {
    // Boot 1: a session subtree writes its first record; the same handle
    // reads it back.
    db_ = open_db();
    ASSERT_NE(db_, nullptr);
    database_subtree_t* s1 = database_subtree_open(db_, "sessions/s1", '/');
    ASSERT_NE(s1, nullptr);
    EXPECT_EQ(subtree_put(s1, "track/k1", "v1"), 0);
    EXPECT_EQ(subtree_probe(s1, "track/k1"), "v1");

    // Boot 2 (recovery): re-attach the SAME subtree, write the second
    // record, then read it back on the SAME handle.
    database_destroy(db_);
    db_ = open_db();
    ASSERT_NE(db_, nullptr);
    database_subtree_t* s2 = database_subtree_open(db_, "sessions/s1", '/');
    ASSERT_NE(s2, nullptr);
    EXPECT_EQ(subtree_probe(s2, "track/k1"), "v1");   // the replayed record

    EXPECT_EQ(subtree_put(s2, "track/k2", "v2"), 0);
    EXPECT_EQ(subtree_probe(s2, "track/k2"), "v2");   // THE PROBE

    // The fresh-handle view (another subtree handle on the same database
    // handles only its own prefix — the root-level scan is how any session
    // reads another's range; the third-session workaround's shape).
    database_subtree_t* other = database_subtree_open(db_, "sessions/s2", '/');
    ASSERT_NE(other, nullptr);
    std::vector<std::string> scanned =
        reverse_scan(db_, "sessions/s1/track", "sessions/s1/track0");
    ASSERT_EQ(scanned.size(), 2u);
    EXPECT_EQ(scanned[0], "v2");
    EXPECT_EQ(scanned[1], "v1");

    database_subtree_close(other);
    database_subtree_close(s1);
    database_subtree_close(s2);
}

// --- Level 3: THE MINIMAL FAILING SHAPE (post-fix contract) ---------------
// A clean close boot (trie materialized to the page file), then on the
// reopened handle: a reverse walk BEFORE the writes (the caller's
// boot-sequence restore scan), in-session batch writes, and the SAME
// handle's tail walk — which must carry every in-session record.
//
// This shape caught the slot-discipline defect: the reverse walk's lazy
// loads materialized has_value=0 entries' deeper trie levels into the
// trie_child slot (a second, read-copies deserialization of the same disk
// node) while writes mutate the child-slot copy, so the walk never saw the
// writing handle's own records. All paths now agree on one canonical copy.
TEST_F(ReopenAmnesiaTest, RestoreWalkBeforeWritesDoesNotMaskInSessionWrites) {
    // Boot 1: a durable pre-restart event; CLEAN close (materializes the
    // trie to the page file — the shape that triggers the defect).
    db_ = open_db();
    ASSERT_NE(db_, nullptr);
    put(db_, "sessions/s1/turn1/events/00000000000000000001", "remember seven");
    put(db_, "sessions/s1/meta/sid", "s1");
    EXPECT_EQ(probe(db_, "sessions/s1/turn1/events/00000000000000000001"),
              "remember seven");
    database_destroy(db_);
    db_ = nullptr;

    // Boot 2 (recovery from the page file): the restore-seq walk BEFORE
    // any write lazily materializes the recovered chain...
    db_ = open_db();
    ASSERT_NE(db_, nullptr);
    std::vector<std::string> restored =
        reverse_scan(db_, "sessions/s1/turn1/events", "sessions/s1/turn1/events0");
    ASSERT_EQ(restored.size(), 1u);
    EXPECT_EQ(restored[0], "remember seven");

    // ...the engine's in-session writes commit...
    put(db_, "sessions/s1/turn1/events/00000000000000000002", "turn one output");
    put(db_, "sessions/s1/turn1/events/00000000000000000003", "step record");

    // ...and the SAME handle's tail walk must carry every record: the
    // pre-restart one AND both in-session ones.
    std::vector<std::string> tail =
        reverse_scan(db_, "sessions/s1/turn1/events", "sessions/s1/turn1/events0");
    ASSERT_EQ(tail.size(), 3u) << "THE PROBE: the same handle's walk after "
                                  "in-session writes on a reopened db";
    EXPECT_EQ(tail[0], "step record");
    EXPECT_EQ(tail[1], "turn one output");
    EXPECT_EQ(tail[2], "remember seven");

    // The durable truth: a fresh handle reads the same range.
    database_destroy(db_);
    db_ = open_db();
    ASSERT_NE(db_, nullptr);
    std::vector<std::string> fresh =
        reverse_scan(db_, "sessions/s1/turn1/events", "sessions/s1/turn1/events0");
    ASSERT_EQ(fresh.size(), 3u);
    EXPECT_EQ(fresh[0], "step record");
    EXPECT_EQ(fresh[1], "turn one output");
    EXPECT_EQ(fresh[2], "remember seven");
}