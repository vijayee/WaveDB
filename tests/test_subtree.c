//
// test_subtree.c - Minimal lifecycle tests for database_subtree_t
//

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if _WIN32
#include <io.h>
#include <direct.h>
#include <process.h>
#define getpid() _getpid()
#else
#include <unistd.h>
#endif

#include "Database/database.h"
#include "Database/database_config.h"
#include "Database/database_subtree.h"
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

/* ---- Helper: create a temp directory path ---- */

static void make_tmpdir(char* buf, size_t bufsize, const char* prefix) {
#if _WIN32
    const char* tmpbase = getenv("TEMP");
    if (!tmpbase) tmpbase = ".";
    snprintf(buf, bufsize, "%s\\%s_%d", tmpbase, prefix, getpid());
#else
    snprintf(buf, bufsize, "/tmp/%s_%d", prefix, getpid());
#endif
}

/* ---- Test 1: Open and close subtree, verify fields ---- */

static void test_subtree_lifecycle(void) {
    printf("\n=== test_subtree_lifecycle ===\n");

    char tmpdir[256];
    make_tmpdir(tmpdir, sizeof(tmpdir), "wavedb_subtree_test1");

    /* Create an in-memory sync_only database */
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
    ASSERT(db != NULL, "Create database");
    ASSERT(error == 0, "No error on creation");

    /* Open a subtree */
    database_subtree_t* st = database_subtree_open(db, "layer/graphql", '/');
    ASSERT(st != NULL, "Open subtree");
    ASSERT(st->prefix != NULL, "Subtree prefix is non-NULL");
    ASSERT(strcmp(st->prefix, "layer/graphql") == 0, "Subtree prefix matches");
    ASSERT(st->prefix_len == strlen("layer/graphql"), "Subtree prefix_len matches");
    ASSERT(st->delimiter == '/', "Subtree delimiter is '/'");
    ASSERT(st->db == db, "Subtree db pointer matches");
    ASSERT(st->chunk_size == db->chunk_size, "Subtree chunk_size matches db");

    /* Test accessors */
    database_t* db_out = database_subtree_get_db(st);
    ASSERT(db_out == db, "database_subtree_get_db returns db");

    work_pool_t* pool_out = database_subtree_get_pool(st);
    ASSERT(pool_out == db->pool, "database_subtree_get_pool returns db->pool");

    /* Close subtree */
    database_subtree_close(st);
    printf("  OK: Subtree closed without error\n");

    /* Destroy database */
    database_destroy(db);

    /* Clean up test directory */
    {
        char cmd[512];
#if _WIN32
        snprintf(cmd, sizeof(cmd), "rmdir /s /q %s", tmpdir);
#else
        snprintf(cmd, sizeof(cmd), "rm -rf %s", tmpdir);
#endif
        system(cmd);
    }

    printf("=== test_subtree_lifecycle DONE ===\n");
}

/* ---- Test 2: Prepend key ---- */

static void test_subtree_prepend_key(void) {
    printf("\n=== test_subtree_prepend_key ===\n");

    char tmpdir[256];
    make_tmpdir(tmpdir, sizeof(tmpdir), "wavedb_subtree_test2");

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
    ASSERT(db != NULL, "Create database");

    database_subtree_t* st = database_subtree_open(db, "layer/graphql", '/');
    ASSERT(st != NULL, "Open subtree");

    /* Prepend key: prefix="layer/graphql", delimiter='/', key="users" */
    size_t out_len = 0;
    char* full_key = database_subtree_prepend_key(st, "users", strlen("users"), &out_len);
    ASSERT(full_key != NULL, "Prepend key returns non-NULL");
    ASSERT(out_len == strlen("layer/graphql") + 1 + strlen("users"),
           "Prepend key out_len is correct");
    ASSERT(memcmp(full_key, "layer/graphql/users", out_len) == 0,
           "Prepend key result is 'layer/graphql/users'");

    free(full_key);

    /* Prepend key with empty key */
    full_key = database_subtree_prepend_key(st, "", 0, &out_len);
    ASSERT(full_key != NULL, "Prepend key with empty key returns non-NULL");
    ASSERT(memcmp(full_key, "layer/graphql/", strlen("layer/graphql/")) == 0,
           "Prepend key with empty key produces 'layer/graphql/'");

    free(full_key);

    database_subtree_close(st);
    database_destroy(db);

    {
        char cmd[512];
#if _WIN32
        snprintf(cmd, sizeof(cmd), "rmdir /s /q %s", tmpdir);
#else
        snprintf(cmd, sizeof(cmd), "rm -rf %s", tmpdir);
#endif
        system(cmd);
    }

    printf("=== test_subtree_prepend_key DONE ===\n");
}

/* ---- Test 3: Prepend path ---- */

static void test_subtree_prepend_path(void) {
    printf("\n=== test_subtree_prepend_path ===\n");

    char tmpdir[256];
    make_tmpdir(tmpdir, sizeof(tmpdir), "wavedb_subtree_test3");

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
    ASSERT(db != NULL, "Create database");

    database_subtree_t* st = database_subtree_open(db, "layer/graphql", '/');
    ASSERT(st != NULL, "Open subtree");

    /* Create a path: ["name"] */
    path_t* original = path_create_from_raw("name", strlen("name"), '/', 4);
    ASSERT(original != NULL, "Create original path");

    /* Prepend prefix: result should be ["layer", "graphql", "name"] */
    path_t* full_path = database_subtree_prepend_path(st, original);
    ASSERT(full_path != NULL, "Prepend path returns non-NULL");
    ASSERT(path_length(full_path) == 3, "Prepended path has 3 identifiers");

    path_destroy(original);
    path_destroy(full_path);

    database_subtree_close(st);
    database_destroy(db);

    {
        char cmd[512];
#if _WIN32
        snprintf(cmd, sizeof(cmd), "rmdir /s /q %s", tmpdir);
#else
        snprintf(cmd, sizeof(cmd), "rm -rf %s", tmpdir);
#endif
        system(cmd);
    }

    printf("=== test_subtree_prepend_path DONE ===\n");
}

/* ---- Main ---- */

int main(void) {
    printf("Running subtree tests...\n");

    test_subtree_lifecycle();
    test_subtree_prepend_key();
    test_subtree_prepend_path();

    printf("\n%d test(s) failed.\n", failures);
    return failures > 0 ? 1 : 0;
}