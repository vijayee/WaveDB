// test_empty_value_reopen.cpp — the persisted empty-value point-get reproducer.
//
// The recorded defect (0.2.6/0.2.7 smoke probe): a put of a zero-length
// value round-trips in-session, and the key survives close/reopen (scans
// still list it), but the SAME key's point-get returns a MISS after reopen.
//
// Root cause (page-file binary deserializers, v1/v2/v3 alike): a legacy
// entry with has_value set serialized a present-but-EMPTY value as a
// uint32 length of 0 with no payload — byte-identical to "no value" — so
// the deserializer reconstructed value == NULL with has_value still set,
// and hbtrie_find's entry->value == NULL check turned it into a miss.
// Deletes are immune by construction: they always upgrade to a version
// chain with an is_deleted tombstone.
//
// Fix: the deserializers now materialize identifier_create_empty() for
// has_value + zero-length (non-deleted) values, in all four decode sites.
#include <gtest/gtest.h>

extern "C" {
#include "Database/database.h"
#include "Database/database_iterator.h"
#include "Buffer/buffer.h"
}

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

class EmptyValueReopenTest : public ::testing::Test {
   protected:
    void SetUp() override {
        char tmpdir[128];
#if _WIN32
        strcpy(tmpdir, "C:\\Temp\\wavedb_emptyval_XXXXXX");
#else
        std::string base = std::string(getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp") +
                           "/wavedb_emptyval_XXXXXX";
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

    // Same CONCURRENT-mode shape as the reopen-amnesia suite.
    database_t* open_db() {
        database_config_t* config = database_config_default();
        config->enable_persist = 1;
        config->sync_only = 0;
        config->chunk_size = 4;
        config->btree_node_size = 4096;
        config->worker_threads = 2;
        config->timer_resolution_ms = 100;
        int error_code = 0;
        database_t* db = database_create_with_config(location_.c_str(), config, &error_code);
        database_config_destroy(config);
        return db;
    }

    void put(database_t* db, const char* key, const char* value, size_t value_len) {
        EXPECT_EQ(database_put_sync_raw(db, key, strlen(key), '/',
                                        (const uint8_t*)value, value_len), 0);
    }

    // A hit returns the value bytes (empty allowed); a miss returns nullopt
    // by setting has_value = false. Distinguishes "stored empty" from "absent".
    struct GetResult {
        bool hit = false;
        std::string value;
    };

    GetResult probe(database_t* db, const char* key) {
        uint8_t* value = nullptr;
        size_t len = 0;
        GetResult out;
        int rc = database_get_sync_raw(db, key, strlen(key), '/', &value, &len);
        if (rc == 0) {
            out.hit = true;
            if (value != nullptr && len > 0) {
                out.value.assign((char*)value, len);
            }
            if (value != nullptr) free(value);
        }
        return out;
    }

    // All VALUES in ascending scan order over the whole tree (empty values
    // materialize as "" via identifier_get_data_copy's len-0 contract). This
    // guards the "scan still lists the key" side of the contract.
    std::vector<std::string> scan_values(database_t* db, const char* start_s,
                                         const char* end_s) {
        std::vector<std::string> out;
        path_t* start = path_create_from_raw(start_s, strlen(start_s), '/', 0);
        path_t* end = path_create_from_raw(end_s, strlen(end_s), '/', 0);
        database_iterator_t* iter = database_scan_start(db, start, end);
        path_destroy(start);
        path_destroy(end);
        if (iter == nullptr) return out;
        path_t* k = nullptr;
        identifier_t* v = nullptr;
        while (database_scan_next(iter, &k, &v) == 0) {
            if (v != nullptr) {
                size_t len = 0;
                uint8_t* data = identifier_get_data_copy(v, &len);
                if (data != nullptr) {
                    out.emplace_back((char*)data, len);
                    free(data);
                } else {
                    out.emplace_back("");  // valid empty value
                }
            } else {
                out.emplace_back("");
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

// --- In-session: the empty value round-trips before any persistence -------
TEST_F(EmptyValueReopenTest, InSessionEmptyValueRoundTrip) {
    db_ = open_db();
    ASSERT_NE(db_, nullptr);
    put(db_, "kv/empty", "", 0);
    put(db_, "kv/normal", "xyz", 3);

    GetResult empty = probe(db_, "kv/empty");
    EXPECT_TRUE(empty.hit);
    EXPECT_EQ(empty.value, "");
    GetResult normal = probe(db_, "kv/normal");
    EXPECT_TRUE(normal.hit);
    EXPECT_EQ(normal.value, "xyz");
}

// --- THE PROBE: persisted empty value survives close/reopen ---------------
TEST_F(EmptyValueReopenTest, PersistedEmptyValueSurvivesReopen) {
    db_ = open_db();
    ASSERT_NE(db_, nullptr);
    put(db_, "kv/empty", "", 0);
    put(db_, "kv/normal", "xyz", 3);
    put(db_, "kv/short", "ab", 2);
    database_destroy(db_);
    db_ = nullptr;

    db_ = open_db();
    ASSERT_NE(db_, nullptr);

    GetResult empty = probe(db_, "kv/empty");
    EXPECT_TRUE(empty.hit)
        << "THE PROBE: a persisted empty value must point-get after reopen";
    EXPECT_EQ(empty.value, "");

    GetResult normal = probe(db_, "kv/normal");
    EXPECT_TRUE(normal.hit);
    EXPECT_EQ(normal.value, "xyz");

    GetResult short_v = probe(db_, "kv/short");
    EXPECT_TRUE(short_v.hit);
    EXPECT_EQ(short_v.value, "ab");
}

// --- Tombstones must stay tombstones across the same reopen ---------------
// The fix materializes empty identifiers for has_value + zero-length. A
// deleted key must STILL be a miss: deletes take the version-chain path
// with an is_deleted tombstone, which the deserializer preserves.
TEST_F(EmptyValueReopenTest, DeletedKeysStillMissAfterReopen) {
    db_ = open_db();
    ASSERT_NE(db_, nullptr);
    put(db_, "kv/gone", "before-delete", 13);
    put(db_, "kv/empty_gone", "", 0);
    EXPECT_EQ(database_delete_sync_raw(db_, "kv/gone", 7, '/'), 0);

    // In-session: already deleted.
    GetResult gone = probe(db_, "kv/gone");
    EXPECT_FALSE(gone.hit);

    database_destroy(db_);
    db_ = nullptr;

    db_ = open_db();
    ASSERT_NE(db_, nullptr);
    GetResult gone_after = probe(db_, "kv/gone");
    EXPECT_FALSE(gone_after.hit)
        << "a deleted key must not resurrect as a (possibly empty) hit";
    GetResult empty_gone_after = probe(db_, "kv/empty_gone");
    EXPECT_TRUE(empty_gone_after.hit) << "an EMPTY value that was never "
                                         "deleted must still be present";
    EXPECT_EQ(empty_gone_after.value, "");
}

// --- Scan must list the key AND carry the empty value (not a tombstone) ---
TEST_F(EmptyValueReopenTest, ScanCarriesEmptyValueAfterReopen) {
    db_ = open_db();
    ASSERT_NE(db_, nullptr);
    put(db_, "kv/ahollow", "", 0);
    put(db_, "kv/bfull", "xyz", 3);
    put(db_, "kv/clast", "ab", 2);
    database_destroy(db_);
    db_ = nullptr;

    db_ = open_db();
    ASSERT_NE(db_, nullptr);
    std::vector<std::string> values = scan_values(db_, "kv", "kv0");
    ASSERT_EQ(values.size(), 3u);
    // Ascending key order: ahollow (""), bfull ("xyz"), clast ("ab").
    EXPECT_EQ(values[0], "");
    EXPECT_EQ(values[1], "xyz");
    EXPECT_EQ(values[2], "ab");
}

// --- Overwrite cycles: empty -> valued -> empty, across reopen ------------
TEST_F(EmptyValueReopenTest, EmptyValuedCycleSurvivesReopen) {
    db_ = open_db();
    ASSERT_NE(db_, nullptr);
    put(db_, "kv/cycle", "", 0);
    database_destroy(db_);
    db_ = nullptr;

    db_ = open_db();
    ASSERT_NE(db_, nullptr);
    put(db_, "kv/cycle", "full", 4);
    database_destroy(db_);
    db_ = nullptr;

    db_ = open_db();
    ASSERT_NE(db_, nullptr);
    GetResult valued = probe(db_, "kv/cycle");
    EXPECT_TRUE(valued.hit);
    EXPECT_EQ(valued.value, "full");

    put(db_, "kv/cycle", "", 0);
    database_destroy(db_);
    db_ = nullptr;

    db_ = open_db();
    ASSERT_NE(db_, nullptr);
    GetResult emptied_again = probe(db_, "kv/cycle");
    EXPECT_TRUE(emptied_again.hit);
    EXPECT_EQ(emptied_again.value, "");
}