//
// Integration tests for Phase 2 persistence (CoW flush, data persistence
// across restart, lazy loading).
//

#include <gtest/gtest.h>

extern "C" {
#include "Database/database.h"
#include "Database/database_config.h"
#include "Storage/page_file.h"
#include "HBTrie/hbtrie.h"
#include "HBTrie/path.h"
#include "HBTrie/identifier.h"
#include "Buffer/buffer.h"
#include "Util/allocator.h"
}

#include <cstdlib>
#include <cstring>
#include <cstdio>

class PersistencePhase2Test : public ::testing::Test {
protected:
    char tmpdir[256];
    database_t* db = nullptr;

    void SetUp() override {
        strcpy(tmpdir, "/tmp/wavedb_phase2_test_XXXXXX");
        mkdtemp(tmpdir);
    }

    void TearDown() override {
        if (db != nullptr) {
            database_destroy(db);
            db = nullptr;
        }
        char cmd[512];
        snprintf(cmd, sizeof(cmd), "rm -rf %s", tmpdir);
        system(cmd);
    }

    database_t* create_db(int enable_persist = 1) {
        database_config_t* config = database_config_default();
        config->enable_persist = (uint8_t)enable_persist;
        config->chunk_size = 4;
        config->btree_node_size = 4096;
        config->worker_threads = 2;
        config->timer_resolution_ms = 100;
        int error_code = 0;
        database_t* result = database_create_with_config(tmpdir, config, &error_code);
        database_config_destroy(config);
        return result;
    }

    // Create a path from string subscripts
    path_t* make_path(std::initializer_list<const char*> subscripts) {
        path_t* p = path_create();
        for (const char* sub : subscripts) {
            buffer_t* buf = buffer_create_from_pointer_copy(
                (uint8_t*)sub, strlen(sub));
            identifier_t* id = identifier_create(buf, 0);
            buffer_destroy(buf);
            path_append(p, id);
            identifier_destroy(id);
        }
        return p;
    }

    // Create an identifier value from a C string
    identifier_t* make_value(const char* data) {
        buffer_t* buf = buffer_create_from_pointer_copy(
            (uint8_t*)data, strlen(data));
        identifier_t* id = identifier_create(buf, 0);
        buffer_destroy(buf);
        return id;
    }

    // Check identifier content against expected string
    void expect_identifier_eq(identifier_t* id, const char* expected) {
        ASSERT_NE(id, nullptr);
        buffer_t* buf = identifier_to_buffer(id);
        ASSERT_NE(buf, nullptr);
        EXPECT_EQ(memcmp(buf->data, expected, strlen(expected)), 0);
        buffer_destroy(buf);
    }
};

// Test 1: Create, put, flush, destroy -- verify no leaks (ASan checks this)
TEST_F(PersistencePhase2Test, PutFlushDestroyNoLeaks) {
    db = create_db(1);
    ASSERT_NE(db, nullptr);

    path_t* key = make_path({"test"});
    identifier_t* val = make_value("value");
    database_put_sync(db, key, val);
    // key and val are consumed by put_sync

    int rc = database_snapshot(db);
    EXPECT_EQ(rc, 0);

    database_destroy(db);
    db = nullptr;
    // ASan will verify no leaks
}

// Test 2: Write, flush, reopen -- verify data persists across restart
TEST_F(PersistencePhase2Test, DataPersistsAcrossRestart) {
    db = create_db(1);
    ASSERT_NE(db, nullptr);

    path_t* key = make_path({"persist"});
    identifier_t* val = make_value("hello");
    database_put_sync(db, key, val);

    int rc = database_snapshot(db);
    EXPECT_EQ(rc, 0);

    database_destroy(db);
    db = nullptr;

    // Reopen the database
    db = create_db(1);
    ASSERT_NE(db, nullptr);

    path_t* key2 = make_path({"persist"});
    identifier_t* result = nullptr;
    int get_rc = database_get_sync(db, key2, &result);
    // key2 is consumed by get_sync

    // Verify data survived restart
    ASSERT_EQ(get_rc, 0);
    ASSERT_NE(result, nullptr);
    expect_identifier_eq(result, "hello");
    identifier_destroy(result);
}

// Test 3: Multiple puts with snapshots, verify CoW stale regions
TEST_F(PersistencePhase2Test, CoWCreatesStaleRegions) {
    db = create_db(1);
    ASSERT_NE(db, nullptr);

    // First write
    path_t* key1 = make_path({"key1"});
    identifier_t* val1 = make_value("value1");
    database_put_sync(db, key1, val1);

    int rc = database_snapshot(db);
    EXPECT_EQ(rc, 0);

    // Second write to same key (triggers CoW -- old bnode location becomes stale)
    path_t* key2 = make_path({"key1"});
    identifier_t* val2 = make_value("value2");
    database_put_sync(db, key2, val2);

    rc = database_snapshot(db);
    EXPECT_EQ(rc, 0);

    // Verify stale ratio is non-zero (old bnode data is stale)
    if (db->page_file != nullptr) {
        double ratio = page_file_stale_ratio(db->page_file);
        EXPECT_GT(ratio, 0.0);
    }

    // Verify the latest value is correct
    path_t* key3 = make_path({"key1"});
    identifier_t* result = nullptr;
    int get_rc = database_get_sync(db, key3, &result);
    ASSERT_EQ(get_rc, 0);
    ASSERT_NE(result, nullptr);
    expect_identifier_eq(result, "value2");
    identifier_destroy(result);
}

// Test 4: Lazy loading -- verify children loaded on demand after restart
TEST_F(PersistencePhase2Test, LazyLoadingOnDemand) {
    db = create_db(1);
    ASSERT_NE(db, nullptr);

    // Create a multi-level trie (3 subscripts = 3 levels deep)
    path_t* key = make_path({"users", "alice", "name"});
    identifier_t* val = make_value("Alice");
    database_put_sync(db, key, val);

    int rc = database_snapshot(db);
    EXPECT_EQ(rc, 0);

    database_destroy(db);
    db = nullptr;

    // Reopen -- root should be lazy-loaded from superblock
    db = create_db(1);
    ASSERT_NE(db, nullptr);

    // Access the deep key -- should trigger lazy loading of intermediate nodes
    path_t* key2 = make_path({"users", "alice", "name"});
    identifier_t* result = nullptr;
    int get_rc = database_get_sync(db, key2, &result);

    ASSERT_EQ(get_rc, 0);
    ASSERT_NE(result, nullptr);
    expect_identifier_eq(result, "Alice");
    identifier_destroy(result);
}

// Test 5: Destroy without snapshot still persists via database_persist
TEST_F(PersistencePhase2Test, PersistOnDestroy) {
    db = create_db(1);
    ASSERT_NE(db, nullptr);

    path_t* key = make_path({"auto_persist"});
    identifier_t* val = make_value("survives");
    database_put_sync(db, key, val);

    // Do NOT call snapshot -- destroy should persist
    database_destroy(db);
    db = nullptr;

    // Reopen
    db = create_db(1);
    ASSERT_NE(db, nullptr);

    path_t* key2 = make_path({"auto_persist"});
    identifier_t* result = nullptr;
    int get_rc = database_get_sync(db, key2, &result);

    ASSERT_EQ(get_rc, 0);
    ASSERT_NE(result, nullptr);
    expect_identifier_eq(result, "survives");
    identifier_destroy(result);
}

// Test 6: Multiple keys persist and are all readable after restart
TEST_F(PersistencePhase2Test, MultipleKeysPersistAcrossRestart) {
    db = create_db(1);
    ASSERT_NE(db, nullptr);

    const int NUM_KEYS = 10;
    for (int i = 0; i < NUM_KEYS; i++) {
        char key_buf[32];
        char val_buf[32];
        snprintf(key_buf, sizeof(key_buf), "key%d", i);
        snprintf(val_buf, sizeof(val_buf), "value%d", i);

        path_t* key = make_path({key_buf});
        identifier_t* val = make_value(val_buf);
        database_put_sync(db, key, val);
    }

    int rc = database_snapshot(db);
    EXPECT_EQ(rc, 0);

    database_destroy(db);
    db = nullptr;

    // Reopen and verify all keys
    db = create_db(1);
    ASSERT_NE(db, nullptr);

    for (int i = 0; i < NUM_KEYS; i++) {
        char key_buf[32];
        char val_buf[32];
        snprintf(key_buf, sizeof(key_buf), "key%d", i);
        snprintf(val_buf, sizeof(val_buf), "value%d", i);

        path_t* key = make_path({key_buf});
        identifier_t* result = nullptr;
        int get_rc = database_get_sync(db, key, &result);

        ASSERT_EQ(get_rc, 0) << "Failed to find key " << key_buf;
        ASSERT_NE(result, nullptr) << "Result null for key " << key_buf;
        expect_identifier_eq(result, val_buf);
        identifier_destroy(result);
    }
}

// Test 7: Delete persists across restart
TEST_F(PersistencePhase2Test, DeletePersistsAcrossRestart) {
    db = create_db(1);
    ASSERT_NE(db, nullptr);

    // Put a key
    path_t* key = make_path({"deleteme"});
    identifier_t* val = make_value("here");
    database_put_sync(db, key, val);

    int rc = database_snapshot(db);
    EXPECT_EQ(rc, 0);

    // Delete the key
    path_t* del_key = make_path({"deleteme"});
    int del_rc = database_delete_sync(db, del_key);
    EXPECT_EQ(del_rc, 0);

    rc = database_snapshot(db);
    EXPECT_EQ(rc, 0);

    database_destroy(db);
    db = nullptr;

    // Reopen and verify the key is gone
    db = create_db(1);
    ASSERT_NE(db, nullptr);

    path_t* key2 = make_path({"deleteme"});
    identifier_t* result = nullptr;
    int get_rc = database_get_sync(db, key2, &result);

    EXPECT_EQ(get_rc, -2);  // -2 = not found
}

// Test 8: Page file is created at the expected path
TEST_F(PersistencePhase2Test, PageFileCreated) {
    db = create_db(1);
    ASSERT_NE(db, nullptr);

    // Write something to ensure the page file gets data
    path_t* key = make_path({"check"});
    identifier_t* val = make_value("pagefile");
    database_put_sync(db, key, val);

    int rc = database_snapshot(db);
    EXPECT_EQ(rc, 0);

    // Verify page file exists at the expected path
    char page_path[512];
    snprintf(page_path, sizeof(page_path), "%s/data.wdbp", tmpdir);
    struct stat st;
    EXPECT_EQ(stat(page_path, &st), 0) << "Page file not found at " << page_path;
    EXPECT_GT(st.st_size, 0) << "Page file is empty";
}

// Test 9: In-memory mode does not create a page file
TEST_F(PersistencePhase2Test, InMemoryModeNoPageFile) {
    db = create_db(0);  // enable_persist = 0
    ASSERT_NE(db, nullptr);

    path_t* key = make_path({"ephemeral"});
    identifier_t* val = make_value("data");
    database_put_sync(db, key, val);

    // Verify page_file is NULL
    EXPECT_EQ(db->page_file, nullptr);

    database_destroy(db);
    db = nullptr;

    // Verify no page file on disk
    char page_path[512];
    snprintf(page_path, sizeof(page_path), "%s/data.wdbp", tmpdir);
    struct stat st;
    EXPECT_NE(stat(page_path, &st), 0) << "Page file should not exist in memory-only mode";
}

// Test 10: Persistence without WAL — verify page file load works independently
TEST_F(PersistencePhase2Test, PersistenceWithoutWAL) {
    db = create_db(1);
    ASSERT_NE(db, nullptr);

    // Write keys
    path_t* key1 = make_path({"no_wal_key"});
    identifier_t* val1 = make_value("no_wal_val");
    database_put_sync(db, key1, val1);

    // Snapshot to flush to page file
    int rc = database_snapshot(db);
    EXPECT_EQ(rc, 0);

    database_destroy(db);
    db = nullptr;

    // Delete all WAL files to force page-file-only load
    // WAL files are named thread_*.wal and current.wal and <sequence>.wal
    // Also delete manifest.dat which tracks WAL files
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -f %s/*.wal %s/manifest.dat", tmpdir, tmpdir);
    system(cmd);

    // Reopen — must load from page file, not WAL
    db = create_db(1);
    ASSERT_NE(db, nullptr);

    path_t* key2 = make_path({"no_wal_key"});
    identifier_t* result = nullptr;
    int get_rc = database_get_sync(db, key2, &result);

    ASSERT_EQ(get_rc, 0) << "Key not found — page file load failed without WAL";
    ASSERT_NE(result, nullptr);
    expect_identifier_eq(result, "no_wal_val");
    identifier_destroy(result);
}