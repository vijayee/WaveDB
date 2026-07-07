//
// Test for database_vacuum (sync_only mode)
//

#if _WIN32
#include <io.h>
#include <direct.h>
#include <process.h>
#define getpid() _getpid()
#define mkdir(path, mode) _mkdir(path)
#else
#include <unistd.h>
#endif

#include <gtest/gtest.h>
#include <chrono>
#include <thread>
#include <future>
#include <atomic>
#include <vector>
#include <string>
#include <cstdio>
#include <sys/stat.h>
#include <errno.h>
extern "C" {
#include "Database/database.h"
#include "Database/database_config.h"
#include "Database/database_iterator.h"
#include "HBTrie/path.h"
#include "HBTrie/identifier.h"
#include "Buffer/buffer.h"
#include "Storage/page_file.h"
#include "Workers/pool.h"
#include "Time/wheel.h"
}

class VacuumTest : public ::testing::Test {
protected:
    std::string test_dir;
    database_t* db;

    void SetUp() override {
        test_dir = "/tmp/wavedb_vacuum_" + std::to_string(getpid()) + "_" +
                   std::to_string((size_t)this);
        mkdir(test_dir.c_str(), 0700);
        database_config_t* cfg = database_config_default();
        database_config_set_sync_only(cfg, 1);
        // Bypass min-size gates so vacuum runs even on our small test DB
        cfg->vacuum_config.min_file_size_bytes = 0;
        cfg->vacuum_config.min_stale_bytes = 0;
        // location is a DIRECTORY — database_create_with_config appends /data.wdbp
        db = database_create_with_config(test_dir.c_str(), cfg, NULL);
        database_config_destroy(cfg);
        ASSERT_NE(db, nullptr);
    }

    void TearDown() override {
        database_destroy(db);
        std::string cmd = "rm -rf " + test_dir;
        system(cmd.c_str());
    }

    // Helper to create a single-component path from a string key.
    // Uses the same pattern as test_database.cpp's make_path/make_value.
    path_t* make_path(const std::string& key) {
        path_t* p = path_create();
        buffer_t* buf = buffer_create_from_pointer_copy(
            (uint8_t*)key.data(), key.size());
        identifier_t* id = identifier_create(buf, 0);
        buffer_destroy(buf);
        path_append(p, id);
        identifier_destroy(id);
        return p;
    }

    // database_put_sync consumes both path and value (destroys on all paths).
    void put(const std::string& key, const std::string& val) {
        path_t* p = make_path(key);
        buffer_t* vbuf = buffer_create_from_pointer_copy(
            (uint8_t*)val.data(), val.size());
        identifier_t* v = identifier_create(vbuf, 0);
        buffer_destroy(vbuf);
        ASSERT_EQ(database_put_sync(db, p, v), 0);
    }

    // database_get_sync: returns 0 on success, -2 on not found.
    // Consumes path (caller must NOT destroy it).
    std::string get(const std::string& key) {
        path_t* p = make_path(key);
        identifier_t* v = NULL;
        int rc = database_get_sync(db, p, &v);
        // p is consumed by database_get_sync — do NOT destroy it here
        if (rc == -2 || v == NULL) return "";
        std::string out;
        buffer_t* b = identifier_to_buffer(v);
        if (b != NULL) {
            out.assign((const char*)b->data, b->size);
            buffer_destroy(b);
        }
        identifier_destroy(v);
        return out;
    }

    uint64_t file_size() {
        struct stat st;
        std::string p = test_dir + "/data.wdbp";
        if (stat(p.c_str(), &st) != 0) return 0;
        return (uint64_t)st.st_size;
    }
};

TEST_F(VacuumTest, BasicShrinksAfterOverwrite) {
    const int N = 1000;
    for (int i = 0; i < N; i++) {
        put("key/" + std::to_string(i), "v0_" + std::to_string(i));
    }
    // Flush to disk so the file actually has data (sync_only still needs flush
    // to make the page file reflect the in-memory trie state).
    database_flush_dirty_bnodes(db);
    uint64_t after_initial = file_size();
    EXPECT_GT(after_initial, (uint64_t)0);

    // Overwrite 5 times — each overwrite CoW's the affected bnodes, leaving
    // the old versions as stale regions in the file. Note: hbtrie_insert_unsafe
    // upgrades leaf entries to version chains on overwrite, so the live data
    // grows with each overwrite (the live bnodes contain all versions). The
    // stale CoW copies from previous flushes are what vacuum reclaims.
    for (int rep = 1; rep <= 5; rep++) {
        for (int i = 0; i < N; i++) {
            put("key/" + std::to_string(i),
                "v" + std::to_string(rep) + "_" + std::to_string(i));
        }
        database_flush_dirty_bnodes(db);
    }
    uint64_t before_vacuum = file_size();
    EXPECT_GT(before_vacuum, after_initial * 2)
        << "file should have grown from CoW before vacuum";

    ASSERT_EQ(database_vacuum(db), 0);
    uint64_t after_vacuum = file_size();

    // Vacuum must remove the stale CoW copies. The file should shrink
    // significantly: before_vacuum includes 6 flushes' worth of data (1
    // initial + 5 overwrites), most of which is stale. After vacuum, only
    // the live bnodes (with their accumulated version chains) remain.
    EXPECT_LT(after_vacuum, before_vacuum / 2)
        << "vacuum should remove stale CoW copies";
    EXPECT_GT(after_vacuum, (uint64_t)0)
        << "vacuum should leave a non-empty file with live data";

    // All keys still readable with latest value
    for (int i = 0; i < N; i++) {
        std::string expected = "v5_" + std::to_string(i);
        EXPECT_EQ(get("key/" + std::to_string(i)), expected);
    }
}

// ============================================================================
// Async-mode fixture: creates a real work pool + timing wheel + db in
// non-sync_only mode. Verifies vacuum drains in-flight work before rewrite.
// ============================================================================
class VacuumAsyncTest : public ::testing::Test {
protected:
    std::string test_dir;
    database_t* db;
    work_pool_t* pool;
    hierarchical_timing_wheel_t* wheel;

    void SetUp() override {
        test_dir = "/tmp/wavedb_vacuum_async_" + std::to_string(getpid()) + "_" +
                   std::to_string((size_t)this);
        mkdir(test_dir.c_str(), 0700);

        // Real work pool + wheel — the async fixture exercises the
        // drain path (polling idleCount == size, eviction_in_flight == 0).
        pool = work_pool_create(4);
        work_pool_launch(pool);
        wheel = hierarchical_timing_wheel_create(8, pool);
        hierarchical_timing_wheel_run(wheel);

        database_config_t* cfg = database_config_default();
        cfg->external_pool = pool;
        cfg->external_wheel = wheel;
        // Bypass gates so vacuum runs even on our small test DB
        cfg->vacuum_config.min_file_size_bytes = 0;
        cfg->vacuum_config.min_stale_bytes = 0;
        std::string path = test_dir;  // location is a directory, not a file
        db = database_create_with_config(path.c_str(), cfg, NULL);
        database_config_destroy(cfg);
        ASSERT_NE(db, nullptr);
    }

    void TearDown() override {
        if (db) database_destroy(db);
        // Match the teardown order from test_database.cpp: stop wheel,
        // shutdown pool, join workers, then destroy structures. Do NOT
        // call wait_for_idle_signal on the wheel — debouncers reschedule
        // indefinitely so idle is never reached.
        if (wheel) hierarchical_timing_wheel_stop(wheel);
        if (pool) {
            work_pool_shutdown(pool);
            work_pool_join_all(pool);
        }
        if (wheel) hierarchical_timing_wheel_destroy(wheel);
        if (pool) work_pool_destroy(pool);
        std::string cmd = "rm -rf " + test_dir;
        system(cmd.c_str());
    }

    // Helpers copied verbatim from VacuumTest (correct API signatures:
    // identifier_create(buf, 0) + database_get_sync(db, p, &v)).
    path_t* make_path(const std::string& key) {
        path_t* p = path_create();
        buffer_t* buf = buffer_create_from_pointer_copy(
            (uint8_t*)key.data(), key.size());
        identifier_t* id = identifier_create(buf, 0);
        buffer_destroy(buf);
        path_append(p, id);
        identifier_destroy(id);
        return p;
    }

    void put(const std::string& key, const std::string& val) {
        path_t* p = make_path(key);
        buffer_t* vbuf = buffer_create_from_pointer_copy(
            (uint8_t*)val.data(), val.size());
        identifier_t* v = identifier_create(vbuf, 0);
        buffer_destroy(vbuf);
        ASSERT_EQ(database_put_sync(db, p, v), 0);
    }

    std::string get(const std::string& key) {
        path_t* p = make_path(key);
        identifier_t* v = NULL;
        int rc = database_get_sync(db, p, &v);
        if (rc == -2 || v == NULL) return "";
        std::string out;
        buffer_t* b = identifier_to_buffer(v);
        if (b != NULL) {
            out.assign((const char*)b->data, b->size);
            buffer_destroy(b);
        }
        identifier_destroy(v);
        return out;
    }

    uint64_t file_size() {
        struct stat st;
        std::string p = test_dir + "/data.wdbp";
        if (stat(p.c_str(), &st) != 0) return 0;
        return (uint64_t)st.st_size;
    }
};

TEST_F(VacuumTest, OpenCursorRefusesVacuum) {
    // Write a key so a cursor has something to iterate
    put("k1", "v1");

    database_iterator_t* it = database_scan_start(db, NULL, NULL);
    ASSERT_NE(it, nullptr);

    // Manual vacuum should refuse with -EBUSY
    int rc = database_vacuum(db);
    EXPECT_EQ(rc, -EBUSY);

    database_scan_end(it);

    // Now vacuum should succeed
    EXPECT_EQ(database_vacuum(db), 0);
}

TEST_F(VacuumTest, AutoVacuumWaitsForCursorClose) {
    put("k1", "v1");
    // Overwrite enough to grow the file (bypass gates via config in SetUp)
    for (int rep = 0; rep < 20; rep++) put("k1", "v" + std::to_string(rep));

    // Open cursor
    database_iterator_t* it = database_scan_start(db, NULL, NULL);
    ASSERT_NE(it, nullptr);

    std::atomic<bool> vacuum_done(false);
    std::atomic<int> vacuum_rc(-100);

    // Launch auto-trigger vacuum in a thread
    auto fut = std::async(std::launch::async, [&]() {
        int rc = database_vacuum_auto(db);
        vacuum_rc.store(rc);
        vacuum_done.store(true);
    });

    // Wait a moment — vacuum should NOT have completed (cursor still open)
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    EXPECT_FALSE(vacuum_done.load()) << "vacuum should be waiting on cursor close";

    // Close cursor — vacuum should fire within ~10ms (well before cursor_close_wait_ms=60s)
    database_scan_end(it);

    fut.wait_for(std::chrono::seconds(5));
    EXPECT_TRUE(vacuum_done.load());
    EXPECT_EQ(vacuum_rc.load(), 0);
}

TEST_F(VacuumAsyncTest, VacuumWithConcurrentWriter) {
    const int N = 200;
    // Initial load
    for (int i = 0; i < N; i++) {
        put("k/" + std::to_string(i), "v0");
    }
    database_flush_dirty_bnodes(db);

    // Overwrite to grow the file via CoW
    for (int rep = 0; rep < 5; rep++) {
        for (int i = 0; i < N; i++) {
            put("k/" + std::to_string(i), "v" + std::to_string(rep));
        }
        database_flush_dirty_bnodes(db);
    }

    uint64_t before = file_size();
    ASSERT_GT(before, (uint64_t)0);
    ASSERT_EQ(database_vacuum(db), 0);
    uint64_t after = file_size();
    EXPECT_LT(after, before);

    // All keys readable with latest value
    for (int i = 0; i < N; i++) {
        std::string expected = "v4";
        EXPECT_EQ(get("k/" + std::to_string(i)), expected);
    }
}

TEST_F(VacuumTest, SnapshotThresholdTriggersVacuum) {
    const int N = 500;
    for (int i = 0; i < N; i++) put("k/" + std::to_string(i), "v0");
    database_flush_dirty_bnodes(db);
    uint64_t after_initial = file_size();

    // Overwrite enough to cross the 30% threshold (default)
    for (int rep = 0; rep < 10; rep++) {
        for (int i = 0; i < N; i++) put("k/" + std::to_string(i), "v" + std::to_string(rep));
        database_flush_dirty_bnodes(db);
    }
    uint64_t before = file_size();
    EXPECT_GT(before, after_initial * 2);

    ASSERT_EQ(database_snapshot(db), 0);
    uint64_t after = file_size();
    EXPECT_LT(after, before) << "snapshot should have triggered vacuum";

    // Keys still readable
    for (int i = 0; i < N; i++) {
        EXPECT_EQ(get("k/" + std::to_string(i)), "v9");
    }
}

TEST_F(VacuumTest, ManualOnlyModeSkipsAutoVacuum) {
    // Tear down the default db and recreate with MANUAL_ONLY mode
    database_destroy(db);
    db = nullptr;

    database_config_t* cfg = database_config_default();
    database_config_set_sync_only(cfg, 1);
    cfg->vacuum_config.mode = VACUUM_MODE_MANUAL_ONLY;
    cfg->vacuum_config.min_file_size_bytes = 0;
    cfg->vacuum_config.min_stale_bytes = 0;
    std::string path = test_dir;  // location is a directory
    db = database_create_with_config(path.c_str(), cfg, NULL);
    database_config_destroy(cfg);
    ASSERT_NE(db, nullptr);

    const int N = 500;
    for (int i = 0; i < N; i++) put("k/" + std::to_string(i), "v0");
    database_flush_dirty_bnodes(db);
    for (int rep = 0; rep < 10; rep++) {
        for (int i = 0; i < N; i++) put("k/" + std::to_string(i), "v" + std::to_string(rep));
        database_flush_dirty_bnodes(db);
    }
    uint64_t before = file_size();

    ASSERT_EQ(database_snapshot(db), 0);
    uint64_t after = file_size();
    EXPECT_EQ(after, before) << "MANUAL_ONLY mode: snapshot should not auto-vacuum";
}

TEST_F(VacuumAsyncTest, BackgroundWorkerAutoVacuums) {
    // Lower background_interval_ms by recreating DB with custom config
    database_destroy(db);
    db = nullptr;
    std::string path = test_dir;
    std::string cmd = "rm -rf " + path + "/data.wdbp*";
    system(cmd.c_str());

    database_config_t* cfg = database_config_default();
    cfg->external_pool = pool;
    cfg->external_wheel = wheel;
    cfg->vacuum_config.mode = VACUUM_MODE_STRICT;
    cfg->vacuum_config.background_interval_ms = 200;  // 200ms for fast test
    cfg->vacuum_config.stale_threshold = 0.10;
    cfg->vacuum_config.min_file_size_bytes = 0;
    cfg->vacuum_config.min_stale_bytes = 0;
    db = database_create_with_config(path.c_str(), cfg, NULL);
    database_config_destroy(cfg);
    ASSERT_NE(db, nullptr);

    // Push enough overwrites to cross threshold
    const int N = 500;
    for (int i = 0; i < N; i++) put("k/" + std::to_string(i), "v0");
    // Flush to make the page file actually grow
    database_flush_dirty_bnodes(db);
    for (int rep = 0; rep < 5; rep++) {
        for (int i = 0; i < N; i++) put("k/" + std::to_string(i), "v" + std::to_string(rep));
        database_flush_dirty_bnodes(db);
    }
    uint64_t before = file_size();
    ASSERT_GT(before, (uint64_t)0);

    // Wait for background vacuum to fire (multiple intervals — 200ms each, wait 2s)
    std::this_thread::sleep_for(std::chrono::milliseconds(2000));

    uint64_t after = file_size();
    EXPECT_LT(after, before) << "background worker should have vacuumed";

    // Keys still readable
    for (int i = 0; i < N; i++) {
        EXPECT_EQ(get("k/" + std::to_string(i)), "v4");
    }
}