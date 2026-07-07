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
#include <vector>
#include <string>
#include <cstdio>
#include <sys/stat.h>
extern "C" {
#include "Database/database.h"
#include "Database/database_config.h"
#include "HBTrie/path.h"
#include "HBTrie/identifier.h"
#include "Buffer/buffer.h"
#include "Storage/page_file.h"
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