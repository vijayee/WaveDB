//
// Test: scan iterator returns keys without null padding, both in-memory
// and after close/reopen. Also verifies binary keys with real trailing
// null bytes are preserved exactly.
//

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

class ScanPaddingTest : public ::testing::Test {
protected:
    char tmpdir[256];

    void SetUp() override {
#if _WIN32
        strcpy(tmpdir, getenv("TEMP"));
        strcat(tmpdir, "/wavedb_scan_padding_XXXXXX");
        _mktemp(tmpdir);
        _mkdir(tmpdir);
#else
        strcpy(tmpdir, "/tmp/wavedb_scan_padding_XXXXXX");
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
        config->chunk_size = 4;        // small chunks to force multi-chunk subscripts
        config->btree_node_size = 4096;
        config->worker_threads = 2;
        config->enable_persist = 1;
        config->timer_resolution_ms = 100;
        int error_code = 0;
        database_t* result = database_create_with_config(tmpdir, config, &error_code);
        database_config_destroy(config);
        return result;
    }

    // Scan all entries and collect (key, value) pairs
    void scan_all(database_t* db, raw_result_t** out_results, size_t* out_count) {
        *out_results = NULL;
        *out_count = 0;
        int rc = database_scan_range_sync_raw(db, NULL, 0, NULL, 0, '/',
                                               out_results, out_count);
        ASSERT_EQ(rc, 0);
    }

    // Check that a key has no internal null padding (trailing nulls in each
    // subscript segment). The key should be exactly the original bytes.
    void assert_no_padding(const char* key, size_t key_len, const char* expected, size_t expected_len) {
        ASSERT_EQ(key_len, expected_len) << "Key length mismatch";
        ASSERT_EQ(0, memcmp(key, expected, expected_len)) << "Key data mismatch";
    }
};

// --- In-memory scan: no null padding ---

TEST_F(ScanPaddingTest, InMemoryScanNoPadding) {
    database_t* db = create_db();
    ASSERT_NE(db, nullptr);

    // Insert multi-subscript keys (chunk_size=4 forces multi-chunk subscripts:
    // "users" = 5 bytes = 2 chunks, "alice" = 5 bytes = 2 chunks, etc.)
    ASSERT_EQ(0, database_put_sync_raw(db, "users/alice/name", 16, '/', (const uint8_t*)"Alice", 5));
    ASSERT_EQ(0, database_put_sync_raw(db, "users/bob/age", 13, '/', (const uint8_t*)"30", 2));

    // Scan and verify no padding
    raw_result_t* results = NULL;
    size_t count = 0;
    scan_all(db, &results, &count);

    ASSERT_EQ(count, 2u);
    // Results may be in any order; check both expected keys are present
    bool found_alice = false, found_bob = false;
    for (size_t i = 0; i < count; i++) {
        if (results[i].key_len == 16 && memcmp(results[i].key, "users/alice/name", 16) == 0) {
            found_alice = true;
            ASSERT_EQ(results[i].value_len, 5u);
            ASSERT_EQ(0, memcmp(results[i].value, "Alice", 5));
        }
        if (results[i].key_len == 13 && memcmp(results[i].key, "users/bob/age", 13) == 0) {
            found_bob = true;
            ASSERT_EQ(results[i].value_len, 2u);
            ASSERT_EQ(0, memcmp(results[i].value, "30", 2));
        }
    }
    EXPECT_TRUE(found_alice);
    EXPECT_TRUE(found_bob);

    database_raw_results_free(results, count);
    database_destroy(db);
}

// --- After close/reopen: still no padding ---

TEST_F(ScanPaddingTest, ReopenScanNoPadding) {
    {
        database_t* db = create_db();
        ASSERT_NE(db, nullptr);
        ASSERT_EQ(0, database_put_sync_raw(db, "users/alice/name", 16, '/', (const uint8_t*)"Alice", 5));
        ASSERT_EQ(0, database_put_sync_raw(db, "users/bob/age", 13, '/', (const uint8_t*)"30", 2));
        database_destroy(db);
    }

    // Reopen
    database_t* db = create_db();
    ASSERT_NE(db, nullptr);

    raw_result_t* results = NULL;
    size_t count = 0;
    scan_all(db, &results, &count);

    ASSERT_EQ(count, 2u);
    bool found_alice = false, found_bob = false;
    for (size_t i = 0; i < count; i++) {
        // Verify no null padding: keys should be exact original length
        if (results[i].key_len == 16 && memcmp(results[i].key, "users/alice/name", 16) == 0) {
            found_alice = true;
        }
        if (results[i].key_len == 13 && memcmp(results[i].key, "users/bob/age", 13) == 0) {
            found_bob = true;
        }
        // Explicitly check for null bytes inside the key (padding indicator)
        for (size_t j = 0; j < results[i].key_len; j++) {
            ASSERT_NE(results[i].key[j], '\0')
                << "Null byte found in key at position " << j;
        }
    }
    EXPECT_TRUE(found_alice);
    EXPECT_TRUE(found_bob);

    database_raw_results_free(results, count);
    database_destroy(db);
}

// --- Binary key with real trailing nulls preserved exactly ---

TEST_F(ScanPaddingTest, BinaryKeyWithRealNullsInMemory) {
    database_t* db = create_db();
    ASSERT_NE(db, nullptr);

    // Key: "foo\0bar/baz" — subscript "foo\0bar" has a real null at position 3
    const char binary_key[] = {'f','o','o','\0','b','a','r','/','b','a','z'};
    size_t key_len = sizeof(binary_key);
    ASSERT_EQ(0, database_put_sync_raw(db, binary_key, key_len, '/', (const uint8_t*)"val", 3));

    raw_result_t* results = NULL;
    size_t count = 0;
    scan_all(db, &results, &count);

    ASSERT_EQ(count, 1u);
    ASSERT_EQ(results[0].key_len, key_len);
    ASSERT_EQ(0, memcmp(results[0].key, binary_key, key_len))
        << "Binary key with real trailing nulls must be preserved exactly";

    database_raw_results_free(results, count);
    database_destroy(db);
}

// --- Binary key with real trailing nulls after close/reopen ---

TEST_F(ScanPaddingTest, BinaryKeyWithRealNullsReopen) {
    const char binary_key[] = {'f','o','o','\0','b','a','r','/','b','a','z'};
    size_t key_len = sizeof(binary_key);

    {
        database_t* db = create_db();
        ASSERT_NE(db, nullptr);
        ASSERT_EQ(0, database_put_sync_raw(db, binary_key, key_len, '/', (const uint8_t*)"val", 3));
        database_destroy(db);
    }

    database_t* db = create_db();
    ASSERT_NE(db, nullptr);

    raw_result_t* results = NULL;
    size_t count = 0;
    scan_all(db, &results, &count);

    ASSERT_EQ(count, 1u);
    ASSERT_EQ(results[0].key_len, key_len);
    ASSERT_EQ(0, memcmp(results[0].key, binary_key, key_len))
        << "Binary key with real trailing nulls must survive close/reopen";

    database_raw_results_free(results, count);
    database_destroy(db);
}