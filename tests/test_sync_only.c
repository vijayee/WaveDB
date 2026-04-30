//
// test_sync_only.c - Functional and cross-mode tests for sync_only mode
//

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "Database/database.h"
#include "Database/database_config.h"
#include "HBTrie/path.h"
#include "HBTrie/identifier.h"
#include "Buffer/buffer.h"

static int failures = 0;

#define ASSERT(cond, msg) do { \
    if (!(cond)) { \
        printf("  FAIL: %s\n", msg); \
        failures++; \
    } else { \
        printf("  OK: %s\n", msg); \
    } \
} while (0)

/* ---- Helper functions ---- */

static path_t* make_path(const char* sub) {
    path_t* path = path_create();
    if (!path) return NULL;
    buffer_t* buf = buffer_create_from_pointer_copy((uint8_t*)sub, strlen(sub));
    if (!buf) { path_destroy(path); return NULL; }
    identifier_t* id = identifier_create(buf, 0);
    buffer_destroy(buf);
    if (!id) { path_destroy(path); return NULL; }
    path_append(path, id);
    identifier_destroy(id);
    return path;
}

static identifier_t* make_value(const char* data) {
    buffer_t* buf = buffer_create_from_pointer_copy((uint8_t*)data, strlen(data));
    if (!buf) return NULL;
    identifier_t* id = identifier_create(buf, 0);
    buffer_destroy(buf);
    return id;
}

static int verify_value(identifier_t* id, const char* expected) {
    if (!id) return 0;
    size_t len;
    uint8_t* data = identifier_get_data_copy(id, &len);
    if (!data) return 0;
    size_t elen = strlen(expected);
    int match = (len == elen && memcmp(data, expected, elen) == 0);
    free(data);
    return match;
}

/* ---- Test 1: Basic put/get/delete in sync_only mode ---- */

static void test_sync_only_put_get(void) {
    printf("\n=== test_sync_only_put_get ===\n");

    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/wavedb_sync_test1_%d", getpid());

    /* Create sync_only config */
    database_config_t* config = database_config_default();
    config->enable_persist = 1;
    config->sync_only = 1;
    config->worker_threads = 0;
    config->timer_resolution_ms = 0;
    config->chunk_size = 4;
    config->btree_node_size = 4096;

    int error = 0;
    database_t* db = database_create_with_config(tmpdir, config, &error);
    database_config_destroy(config);
    ASSERT(db != NULL, "Create sync_only database");
    ASSERT(error == 0, "No error on creation");
    ASSERT(db->sync_only == 1, "db->sync_only is 1");

    /* Put "hello" = "world" */
    path_t* key = make_path("hello");
    identifier_t* value = make_value("world");
    int rc = database_put_sync(db, key, value);
    ASSERT(rc == 0, "Put hello=world");

    /* Get "hello" and verify value */
    identifier_t* result = NULL;
    key = make_path("hello");
    rc = database_get_sync(db, key, &result);
    ASSERT(rc == 0, "Get hello returns 0");
    ASSERT(result != NULL, "Get hello returns non-NULL");
    ASSERT(verify_value(result, "world"), "Get hello value is 'world'");
    if (result) identifier_destroy(result);

    /* Delete "hello" */
    key = make_path("hello");
    rc = database_delete_sync(db, key);
    ASSERT(rc == 0, "Delete hello");

    /* Verify deleted */
    key = make_path("hello");
    result = NULL;
    rc = database_get_sync(db, key, &result);
    ASSERT(rc == -2, "Get hello after delete returns -2");
    ASSERT(result == NULL, "Get hello after delete returns NULL");

    /* Clean up */
    database_destroy(db);

    /* Remove test directory */
    {
        char cmd[512];
        snprintf(cmd, sizeof(cmd), "rm -rf %s", tmpdir);
        system(cmd);
    }

    printf("=== test_sync_only_put_get DONE ===\n");
}

/* ---- Test 2: Cross-mode transition (sync_only -> async -> sync_only) ---- */

static void test_sync_only_cross_mode(void) {
    printf("\n=== test_sync_only_cross_mode ===\n");

    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/wavedb_sync_cross_%d", getpid());

    /* ===== Phase 1: sync_only mode ===== */
    printf("  Phase 1: Create DB in sync_only mode\n");

    database_config_t* config = database_config_default();
    config->enable_persist = 1;
    config->sync_only = 1;
    config->worker_threads = 0;
    config->timer_resolution_ms = 0;
    config->chunk_size = 4;
    config->btree_node_size = 4096;

    int error = 0;
    database_t* db = database_create_with_config(tmpdir, config, &error);
    database_config_destroy(config);
    ASSERT(db != NULL, "Phase 1: Create sync_only database");
    ASSERT(error == 0, "Phase 1: No error on creation");
    ASSERT(db->sync_only == 1, "Phase 1: db->sync_only is 1");

    /* Put "alpha" = "sync_data" */
    {
        path_t* key = make_path("alpha");
        identifier_t* val = make_value("sync_data");
        int rc = database_put_sync(db, key, val);
        ASSERT(rc == 0, "Phase 1: Put alpha=sync_data");
    }

    /* Flush and close */
    {
        int rc = database_flush_dirty_bnodes(db);
        ASSERT(rc == 0, "Phase 1: Flush dirty bnodes");
    }
    database_destroy(db);
    printf("  Phase 1 COMPLETE: Wrote 'alpha'='sync_data' in sync_only mode\n");

    /* ===== Phase 2: async (normal concurrent) mode ===== */
    printf("  Phase 2: Reopen with normal config (async/concurrent mode)\n");

    config = database_config_default();
    config->enable_persist = 1;
    config->sync_only = 0;
    config->chunk_size = 4;
    config->btree_node_size = 4096;
    config->worker_threads = 2;
    config->timer_resolution_ms = 100;
    config->wal_config.sync_mode = WAL_SYNC_ASYNC;
    config->wal_config.debounce_ms = 100;

    error = 0;
    db = database_create_with_config(tmpdir, config, &error);
    database_config_destroy(config);
    ASSERT(db != NULL, "Phase 2: Reopen database in async mode");
    ASSERT(error == 0, "Phase 2: No error on reopen");
    ASSERT(db->sync_only == 0, "Phase 2: db->sync_only is 0");

    /* Phase 2a: Verify "alpha" = "sync_data" readable */
    {
        path_t* key = make_path("alpha");
        identifier_t* result = NULL;
        int rc = database_get_sync(db, key, &result);
        ASSERT(rc == 0, "Phase 2a: Get alpha returns 0");
        ASSERT(result != NULL, "Phase 2a: Get alpha returns non-NULL");
        ASSERT(verify_value(result, "sync_data"), "Phase 2a: alpha value is 'sync_data' (legacy format)");
        if (result) identifier_destroy(result);
        printf("  Phase 2a OK: read 'alpha'='sync_data' in async mode (legacy format)\n");
    }

    /* Phase 2b: Put "beta" = "async_new" */
    {
        path_t* key = make_path("beta");
        identifier_t* val = make_value("async_new");
        int rc = database_put_sync(db, key, val);
        ASSERT(rc == 0, "Phase 2b: Put beta=async_new");
        printf("  Phase 2b OK: wrote 'beta'='async_new' (versioned)\n");
    }

    /* Phase 2c: Overwrite "alpha" = "async_overwrite" (upgrades to version chain) */
    {
        path_t* key = make_path("alpha");
        identifier_t* val = make_value("async_overwrite");
        int rc = database_put_sync(db, key, val);
        ASSERT(rc == 0, "Phase 2c: Put alpha=async_overwrite");
        printf("  Phase 2c OK: overwrote 'alpha'='async_overwrite' (upgrades to version chain)\n");
    }

    /* Phase 2d: Verify overwrite */
    {
        path_t* key = make_path("alpha");
        identifier_t* result = NULL;
        int rc = database_get_sync(db, key, &result);
        ASSERT(rc == 0, "Phase 2d: Get alpha returns 0");
        ASSERT(result != NULL, "Phase 2d: Get alpha returns non-NULL");
        ASSERT(verify_value(result, "async_overwrite"), "Phase 2d: alpha value is 'async_overwrite'");
        if (result) identifier_destroy(result);
        printf("  Phase 2d OK: verified 'alpha'='async_overwrite'\n");
    }

    /* Phase 2e: Delete "beta" (tombstone) */
    {
        path_t* key = make_path("beta");
        int rc = database_delete_sync(db, key);
        ASSERT(rc == 0, "Phase 2e: Delete beta");
        printf("  Phase 2e OK: deleted 'beta' (tombstone)\n");
    }

    /* Phase 2f: Verify "beta" deleted */
    {
        path_t* key = make_path("beta");
        identifier_t* result = NULL;
        int rc = database_get_sync(db, key, &result);
        ASSERT(rc == -2, "Phase 2f: Get beta after delete returns -2");
        ASSERT(result == NULL, "Phase 2f: Get beta after delete returns NULL");
        printf("  Phase 2f OK: verified 'beta' deleted\n");
    }

    /* Flush and close */
    {
        int rc = database_flush_dirty_bnodes(db);
        ASSERT(rc == 0, "Phase 2: Flush dirty bnodes");
    }
    database_destroy(db);
    printf("  Phase 2 COMPLETE: Flushed async mode changes and closed\n");

    /* ===== Phase 3: sync_only mode again ===== */
    printf("  Phase 3: Reopen with sync_only config\n");

    config = database_config_default();
    config->enable_persist = 1;
    config->sync_only = 1;
    config->worker_threads = 0;
    config->timer_resolution_ms = 0;
    config->chunk_size = 4;
    config->btree_node_size = 4096;

    error = 0;
    db = database_create_with_config(tmpdir, config, &error);
    database_config_destroy(config);
    ASSERT(db != NULL, "Phase 3: Reopen database in sync_only mode");
    ASSERT(error == 0, "Phase 3: No error on reopen");
    ASSERT(db->sync_only == 1, "Phase 3: db->sync_only is 1");

    /* Phase 3a: Verify "alpha" = "async_overwrite" (versioned data readable in unsafe mode) */
    {
        path_t* key = make_path("alpha");
        identifier_t* result = NULL;
        int rc = database_get_sync(db, key, &result);
        ASSERT(rc == 0, "Phase 3a: Get alpha returns 0");
        ASSERT(result != NULL, "Phase 3a: Get alpha returns non-NULL");
        ASSERT(verify_value(result, "async_overwrite"), "Phase 3a: alpha value is 'async_overwrite' (versioned data readable in unsafe mode)");
        if (result) identifier_destroy(result);
        printf("  Phase 3a OK: read 'alpha'='async_overwrite' (versioned data readable in unsafe mode)\n");
    }

    /* Phase 3b: Verify "beta" still deleted */
    {
        path_t* key = make_path("beta");
        identifier_t* result = NULL;
        int rc = database_get_sync(db, key, &result);
        ASSERT(rc == -2, "Phase 3b: Get beta returns -2");
        ASSERT(result == NULL, "Phase 3b: Get beta returns NULL");
        printf("  Phase 3b OK: verified 'beta' still deleted\n");
    }

    /* Phase 3c: Put "gamma" = "sync_after_async" */
    {
        path_t* key = make_path("gamma");
        identifier_t* val = make_value("sync_after_async");
        int rc = database_put_sync(db, key, val);
        ASSERT(rc == 0, "Phase 3c: Put gamma=sync_after_async");
        printf("  Phase 3c OK: wrote 'gamma'='sync_after_async'\n");
    }

    /* Phase 3d: Verify "gamma" */
    {
        path_t* key = make_path("gamma");
        identifier_t* result = NULL;
        int rc = database_get_sync(db, key, &result);
        ASSERT(rc == 0, "Phase 3d: Get gamma returns 0");
        ASSERT(result != NULL, "Phase 3d: Get gamma returns non-NULL");
        ASSERT(verify_value(result, "sync_after_async"), "Phase 3d: gamma value is 'sync_after_async'");
        if (result) identifier_destroy(result);
        printf("  Phase 3d OK: verified 'gamma'='sync_after_async'\n");
    }

    /* Flush and close */
    {
        int rc = database_flush_dirty_bnodes(db);
        ASSERT(rc == 0, "Phase 3: Flush dirty bnodes");
    }
    database_destroy(db);
    printf("  Phase 3 COMPLETE: Flushed sync_only changes and closed\n");

    /* Clean up test directory */
    {
        char cmd[512];
        snprintf(cmd, sizeof(cmd), "rm -rf %s", tmpdir);
        system(cmd);
    }

    printf("=== test_sync_only_cross_mode DONE ===\n");
}

/* ---- Main ---- */

int main(void) {
    printf("Running sync_only tests...\n");

    test_sync_only_put_get();
    test_sync_only_cross_mode();

    printf("\n%d test(s) failed.\n", failures);
    return failures > 0 ? 1 : 0;
}
