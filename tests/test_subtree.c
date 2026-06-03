//
// test_subtree.c - Tests for database_subtree_t
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
#include "Workers/promise.h"
#include "Workers/error.h"
#include "Workers/pool.h"
#include "Time/wheel.h"

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

/* ---- Helper: create a sync_only database ---- */

static database_t* create_test_db(char* tmpdir, size_t bufsize, const char* prefix) {
    make_tmpdir(tmpdir, bufsize, prefix);

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
    return db;
}

/* ---- Helper: cleanup test directory ---- */

static void cleanup_tmpdir(const char* tmpdir) {
    char cmd[512];
#if _WIN32
    snprintf(cmd, sizeof(cmd), "rmdir /s /q %s", tmpdir);
#else
    snprintf(cmd, sizeof(cmd), "rm -rf %s", tmpdir);
#endif
    system(cmd);
}

/* ---- Test 1: Open and close subtree, verify fields ---- */

static void test_subtree_lifecycle(void) {
    printf("\n=== test_subtree_lifecycle ===\n");

    char tmpdir[256];
    database_t* db = create_test_db(tmpdir, sizeof(tmpdir), "wavedb_subtree_test1");
    ASSERT(db != NULL, "Create database");

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
    cleanup_tmpdir(tmpdir);

    printf("=== test_subtree_lifecycle DONE ===\n");
}

/* ---- Test 2: Prepend key ---- */

static void test_subtree_prepend_key(void) {
    printf("\n=== test_subtree_prepend_key ===\n");

    char tmpdir[256];
    database_t* db = create_test_db(tmpdir, sizeof(tmpdir), "wavedb_subtree_test2");
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
    cleanup_tmpdir(tmpdir);

    printf("=== test_subtree_prepend_key DONE ===\n");
}

/* ---- Test 3: Prepend path ---- */

static void test_subtree_prepend_path(void) {
    printf("\n=== test_subtree_prepend_path ===\n");

    char tmpdir[256];
    database_t* db = create_test_db(tmpdir, sizeof(tmpdir), "wavedb_subtree_test3");
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
    cleanup_tmpdir(tmpdir);

    printf("=== test_subtree_prepend_path DONE ===\n");
}

/* ---- Test 4: Path-based sync CRUD ---- */

static void test_subtree_sync_crud(void) {
    printf("\n=== test_subtree_sync_crud ===\n");

    char tmpdir[256];
    database_t* db = create_test_db(tmpdir, sizeof(tmpdir), "wavedb_subtree_crud");
    ASSERT(db != NULL, "Create database");

    database_subtree_t* st = database_subtree_open(db, "graphql", '/');
    ASSERT(st != NULL, "Open subtree on 'graphql'");

    /* Put a value via subtree: path "Users/1/name", value "Alice" */
    path_t* put_path = path_create_from_raw("Users/1/name", strlen("Users/1/name"), '/', 4);
    ASSERT(put_path != NULL, "Create put path");

    identifier_t* put_value = identifier_create_from_raw((const uint8_t*)"Alice", strlen("Alice"), 4);
    ASSERT(put_value != NULL, "Create put value");

    int rc = database_subtree_put_sync(st, put_path, put_value);
    ASSERT(rc == 0, "Put via subtree succeeds");

    /* Get the value back via subtree */
    path_t* get_path = path_create_from_raw("Users/1/name", strlen("Users/1/name"), '/', 4);
    ASSERT(get_path != NULL, "Create get path");

    identifier_t* result = NULL;
    rc = database_subtree_get_sync(st, get_path, &result);
    ASSERT(rc == 0, "Get via subtree succeeds");
    ASSERT(result != NULL, "Get returns non-NULL result");

    /* Verify value content */
    if (result != NULL) {
        buffer_t* result_buf = identifier_to_buffer(result);
        if (result_buf != NULL) {
            ASSERT(memcmp(result_buf->data, "Alice", result_buf->size) == 0,
                   "Get value matches put value 'Alice'");
            buffer_destroy(result_buf);
        }
        identifier_destroy(result);
    }

    /* Verify that the raw database sees the prefixed key */
    {
        uint8_t* raw_val = NULL;
        size_t raw_val_len = 0;
        int raw_rc = database_get_sync_raw(db, "graphql/Users/1/name",
                                           strlen("graphql/Users/1/name"), '/',
                                           &raw_val, &raw_val_len);
        ASSERT(raw_rc == 0, "Raw get on prefixed key succeeds");
        if (raw_rc == 0) {
            ASSERT(raw_val != NULL, "Raw get returns non-NULL value");
            ASSERT(memcmp(raw_val, "Alice", raw_val_len) == 0,
                   "Raw value matches 'Alice'");
            database_raw_value_free(raw_val);
        }
    }

    /* Delete via subtree */
    path_t* del_path = path_create_from_raw("Users/1/name", strlen("Users/1/name"), '/', 4);
    ASSERT(del_path != NULL, "Create delete path");

    rc = database_subtree_delete_sync(st, del_path);
    ASSERT(rc == 0, "Delete via subtree succeeds");

    /* Verify get returns not found */
    path_t* get_path2 = path_create_from_raw("Users/1/name", strlen("Users/1/name"), '/', 4);
    ASSERT(get_path2 != NULL, "Create second get path");

    identifier_t* result2 = NULL;
    rc = database_subtree_get_sync(st, get_path2, &result2);
    ASSERT(rc == -2, "Get after delete returns not found (-2)");
    ASSERT(result2 == NULL, "Result is NULL after delete");

    /* Test increment */
    path_t* inc_path = path_create_from_raw("counter", strlen("counter"), '/', 4);
    ASSERT(inc_path != NULL, "Create increment path");

    int64_t new_val = database_subtree_increment_sync(st, inc_path, 10);
    ASSERT(new_val == 10, "Increment from 0 by 10 returns 10");

    path_t* inc_path2 = path_create_from_raw("counter", strlen("counter"), '/', 4);
    ASSERT(inc_path2 != NULL, "Create second increment path");

    new_val = database_subtree_increment_sync(st, inc_path2, 5);
    ASSERT(new_val == 15, "Increment from 10 by 5 returns 15");

    database_subtree_close(st);
    database_destroy(db);
    cleanup_tmpdir(tmpdir);

    printf("=== test_subtree_sync_crud DONE ===\n");
}

/* ---- Test 5: Raw sync CRUD ---- */

static void test_subtree_sync_crud_raw(void) {
    printf("\n=== test_subtree_sync_crud_raw ===\n");

    char tmpdir[256];
    database_t* db = create_test_db(tmpdir, sizeof(tmpdir), "wavedb_subtree_raw");
    ASSERT(db != NULL, "Create database");

    database_subtree_t* st = database_subtree_open(db, "layer", '/');
    ASSERT(st != NULL, "Open subtree on 'layer'");

    /* Put via raw */
    const uint8_t* val = (const uint8_t*) "hello";
    int rc = database_subtree_put_sync_raw(st, "key1", strlen("key1"), '/',
                                           val, strlen("hello"));
    ASSERT(rc == 0, "Raw put via subtree succeeds");

    /* Get via raw */
    uint8_t* out_val = NULL;
    size_t out_val_len = 0;
    rc = database_subtree_get_sync_raw(st, "key1", strlen("key1"), '/',
                                       &out_val, &out_val_len);
    ASSERT(rc == 0, "Raw get via subtree succeeds");
    ASSERT(out_val != NULL, "Raw get returns non-NULL value");
    if (out_val != NULL) {
        ASSERT(out_val_len == strlen("hello"), "Raw get value length matches");
        ASSERT(memcmp(out_val, "hello", out_val_len) == 0,
               "Raw get value matches 'hello'");
        database_raw_value_free(out_val);
    }

    /* Delete via raw */
    rc = database_subtree_delete_sync_raw(st, "key1", strlen("key1"), '/');
    ASSERT(rc == 0, "Raw delete via subtree succeeds");

    /* Verify get returns not found */
    out_val = NULL;
    out_val_len = 0;
    rc = database_subtree_get_sync_raw(st, "key1", strlen("key1"), '/',
                                       &out_val, &out_val_len);
    ASSERT(rc == -2, "Raw get after delete returns not found (-2)");
    ASSERT(out_val == NULL, "Raw value is NULL after delete");

    database_subtree_close(st);
    database_destroy(db);
    cleanup_tmpdir(tmpdir);

    printf("=== test_subtree_sync_crud_raw DONE ===\n");
}

/* ---- Test 6: Subtree isolation ---- */

static void test_subtree_isolation(void) {
    printf("\n=== test_subtree_isolation ===\n");

    char tmpdir[256];
    database_t* db = create_test_db(tmpdir, sizeof(tmpdir), "wavedb_subtree_iso");
    ASSERT(db != NULL, "Create database");

    database_subtree_t* st_a = database_subtree_open(db, "graphql", '/');
    ASSERT(st_a != NULL, "Open subtree 'graphql'");

    database_subtree_t* st_b = database_subtree_open(db, "graph", '/');
    ASSERT(st_b != NULL, "Open subtree 'graph'");

    /* Put different values at the same key in each subtree */
    path_t* path_a = path_create_from_raw("Users/1/name", strlen("Users/1/name"), '/', 4);
    ASSERT(path_a != NULL, "Create path for subtree A");

    identifier_t* val_a = identifier_create_from_raw((const uint8_t*)"Alice", strlen("Alice"), 4);
    ASSERT(val_a != NULL, "Create value for subtree A");

    int rc = database_subtree_put_sync(st_a, path_a, val_a);
    ASSERT(rc == 0, "Put in subtree 'graphql' succeeds");

    path_t* path_b = path_create_from_raw("Users/1/name", strlen("Users/1/name"), '/', 4);
    ASSERT(path_b != NULL, "Create path for subtree B");

    identifier_t* val_b = identifier_create_from_raw((const uint8_t*)"Bob", strlen("Bob"), 4);
    ASSERT(val_b != NULL, "Create value for subtree B");

    rc = database_subtree_put_sync(st_b, path_b, val_b);
    ASSERT(rc == 0, "Put in subtree 'graph' succeeds");

    /* Get from subtree A — should return "Alice" */
    path_t* get_a = path_create_from_raw("Users/1/name", strlen("Users/1/name"), '/', 4);
    ASSERT(get_a != NULL, "Create get path for subtree A");

    identifier_t* result_a = NULL;
    rc = database_subtree_get_sync(st_a, get_a, &result_a);
    ASSERT(rc == 0, "Get from subtree 'graphql' succeeds");
    if (result_a != NULL) {
        buffer_t* buf_a = identifier_to_buffer(result_a);
        if (buf_a != NULL) {
            ASSERT(memcmp(buf_a->data, "Alice", buf_a->size) == 0,
                   "Subtree 'graphql' returns 'Alice'");
            buffer_destroy(buf_a);
        }
        identifier_destroy(result_a);
    }

    /* Get from subtree B — should return "Bob" */
    path_t* get_b = path_create_from_raw("Users/1/name", strlen("Users/1/name"), '/', 4);
    ASSERT(get_b != NULL, "Create get path for subtree B");

    identifier_t* result_b = NULL;
    rc = database_subtree_get_sync(st_b, get_b, &result_b);
    ASSERT(rc == 0, "Get from subtree 'graph' succeeds");
    if (result_b != NULL) {
        buffer_t* buf_b = identifier_to_buffer(result_b);
        if (buf_b != NULL) {
            ASSERT(memcmp(buf_b->data, "Bob", buf_b->size) == 0,
                   "Subtree 'graph' returns 'Bob'");
            buffer_destroy(buf_b);
        }
        identifier_destroy(result_b);
    }

    database_subtree_close(st_a);
    database_subtree_close(st_b);
    database_destroy(db);
    cleanup_tmpdir(tmpdir);

    printf("=== test_subtree_isolation DONE ===\n");
}

/* ---- Async test helpers ---- */

typedef struct {
    volatile int completed;
    int status;
    identifier_t* result;
    uint8_t* raw_result;
    size_t raw_result_len;
} test_async_result_t;

static void test_on_resolve(void* ctx, void* payload) {
    test_async_result_t* r = (test_async_result_t*)ctx;
    r->status = 0;
    r->completed = 1;
    (void)payload;
}

static void test_on_resolve_get(void* ctx, void* payload) {
    test_async_result_t* r = (test_async_result_t*)ctx;
    if (payload != NULL) {
        identifier_t* id = (identifier_t*)payload;
        REFERENCE(id, identifier_t);
        r->result = id;
        r->status = 0;
    } else {
        r->status = -2;
        r->result = NULL;
    }
    r->completed = 1;
}

static void test_on_reject(void* ctx, async_error_t* error) {
    test_async_result_t* r = (test_async_result_t*)ctx;
    r->status = -1;
    r->completed = 1;
    error_destroy(error);
}

/* ---- Helper: create a database with worker pool for async tests ---- */

static database_t* create_async_db(char* tmpdir, size_t bufsize, const char* prefix) {
    make_tmpdir(tmpdir, bufsize, prefix);

    database_config_t* config = database_config_default();
    config->enable_persist = 1;
    config->sync_only = 0;
    config->worker_threads = 1;
    config->timer_resolution_ms = 100;
    config->chunk_size = 4;
    config->btree_node_size = 4096;

    int error = 0;
    database_t* db = database_create_with_config(tmpdir, config, &error);
    database_config_destroy(config);
    return db;
}

/* ---- Test 7: Path-based async CRUD ---- */

static void test_subtree_async_crud(void) {
    printf("\n=== test_subtree_async_crud ===\n");

    char tmpdir[256];
    database_t* db = create_async_db(tmpdir, sizeof(tmpdir), "wavedb_subtree_async");
    ASSERT(db != NULL, "Create async database");
    ASSERT(db->sync_only == 0, "Database is not sync_only");

    database_subtree_t* st = database_subtree_open(db, "async_ns", '/');
    ASSERT(st != NULL, "Open subtree on 'async_ns'");

    /* --- Async put --- */
    {
        path_t* put_path = path_create_from_raw("Users/1/name", strlen("Users/1/name"), '/', 4);
        ASSERT(put_path != NULL, "Create async put path");

        identifier_t* put_value = identifier_create_from_raw((const uint8_t*)"AsyncAlice", strlen("AsyncAlice"), 4);
        ASSERT(put_value != NULL, "Create async put value");

        test_async_result_t result = {0};
        promise_t* promise = promise_create(test_on_resolve, test_on_reject, &result);
        ASSERT(promise != NULL, "Create put promise");

        database_subtree_put(st, put_path, put_value, promise);

        /* Wait for completion */
        int max_wait = 10000;  /* 10 seconds in ms */
        while (!result.completed && max_wait > 0) {
            usleep(1000);
            max_wait--;
        }
        ASSERT(result.completed, "Async put completed");
        ASSERT(result.status == 0, "Async put succeeded");

        promise_destroy(promise);
    }

    /* --- Async get --- */
    {
        path_t* get_path = path_create_from_raw("Users/1/name", strlen("Users/1/name"), '/', 4);
        ASSERT(get_path != NULL, "Create async get path");

        test_async_result_t result = {0};
        promise_t* promise = promise_create(test_on_resolve_get, test_on_reject, &result);
        ASSERT(promise != NULL, "Create get promise");

        database_subtree_get(st, get_path, promise);

        int max_wait = 10000;
        while (!result.completed && max_wait > 0) {
            usleep(1000);
            max_wait--;
        }
        ASSERT(result.completed, "Async get completed");
        ASSERT(result.status == 0, "Async get succeeded");
        ASSERT(result.result != NULL, "Async get returned non-NULL result");

        if (result.result != NULL) {
            buffer_t* buf = identifier_to_buffer(result.result);
            if (buf != NULL) {
                ASSERT(memcmp(buf->data, "AsyncAlice", buf->size) == 0,
                       "Async get value matches 'AsyncAlice'");
                buffer_destroy(buf);
            }
            identifier_destroy(result.result);
        }

        promise_destroy(promise);
    }

    /* --- Async delete --- */
    {
        path_t* del_path = path_create_from_raw("Users/1/name", strlen("Users/1/name"), '/', 4);
        ASSERT(del_path != NULL, "Create async delete path");

        test_async_result_t result = {0};
        promise_t* promise = promise_create(test_on_resolve, test_on_reject, &result);
        ASSERT(promise != NULL, "Create delete promise");

        database_subtree_delete(st, del_path, promise);

        int max_wait = 10000;
        while (!result.completed && max_wait > 0) {
            usleep(1000);
            max_wait--;
        }
        ASSERT(result.completed, "Async delete completed");
        ASSERT(result.status == 0, "Async delete succeeded");

        promise_destroy(promise);
    }

    /* --- Verify deletion with async get --- */
    {
        path_t* get_path2 = path_create_from_raw("Users/1/name", strlen("Users/1/name"), '/', 4);
        ASSERT(get_path2 != NULL, "Create second async get path");

        test_async_result_t result = {0};
        promise_t* promise = promise_create(test_on_resolve_get, test_on_reject, &result);
        ASSERT(promise != NULL, "Create second get promise");

        database_subtree_get(st, get_path2, promise);

        int max_wait = 10000;
        while (!result.completed && max_wait > 0) {
            usleep(1000);
            max_wait--;
        }
        ASSERT(result.completed, "Async get after delete completed");
        ASSERT(result.result == NULL, "Async get after delete returns NULL");

        promise_destroy(promise);
    }

    database_subtree_close(st);
    database_destroy(db);
    cleanup_tmpdir(tmpdir);

    printf("=== test_subtree_async_crud DONE ===\n");
}

/* ---- Test 8: Raw async CRUD ---- */

static void test_subtree_async_crud_raw(void) {
    printf("\n=== test_subtree_async_crud_raw ===\n");

    char tmpdir[256];
    database_t* db = create_async_db(tmpdir, sizeof(tmpdir), "wavedb_subtree_async_raw");
    ASSERT(db != NULL, "Create async database for raw test");

    database_subtree_t* st = database_subtree_open(db, "raw_ns", '/');
    ASSERT(st != NULL, "Open subtree on 'raw_ns'");

    /* --- Async raw put --- */
    {
        const uint8_t* val = (const uint8_t*)"hello_async";
        test_async_result_t result = {0};
        promise_t* promise = promise_create(test_on_resolve, test_on_reject, &result);
        ASSERT(promise != NULL, "Create raw put promise");

        int rc = database_subtree_put_raw(st, "key1", strlen("key1"), '/',
                                           val, strlen("hello_async"), promise);
        ASSERT(rc == 0, "Async raw put returns 0");

        int max_wait = 10000;
        while (!result.completed && max_wait > 0) {
            usleep(1000);
            max_wait--;
        }
        ASSERT(result.completed, "Async raw put completed");
        ASSERT(result.status == 0, "Async raw put succeeded");

        promise_destroy(promise);
    }

    /* --- Async raw get --- */
    {
        test_async_result_t result = {0};
        promise_t* promise = promise_create(test_on_resolve_get, test_on_reject, &result);
        ASSERT(promise != NULL, "Create raw get promise");

        int rc = database_subtree_get_raw(st, "key1", strlen("key1"), '/', promise);
        ASSERT(rc == 0, "Async raw get returns 0");

        int max_wait = 10000;
        while (!result.completed && max_wait > 0) {
            usleep(1000);
            max_wait--;
        }
        ASSERT(result.completed, "Async raw get completed");

        if (result.result != NULL) {
            buffer_t* buf = identifier_to_buffer(result.result);
            if (buf != NULL) {
                ASSERT(memcmp(buf->data, "hello_async", buf->size) == 0,
                       "Async raw get value matches 'hello_async'");
                buffer_destroy(buf);
            }
            identifier_destroy(result.result);
        } else {
            printf("  FAIL: Async raw get returned NULL result\n");
            failures++;
        }

        promise_destroy(promise);
    }

    /* --- Async raw delete --- */
    {
        test_async_result_t result = {0};
        promise_t* promise = promise_create(test_on_resolve, test_on_reject, &result);
        ASSERT(promise != NULL, "Create raw delete promise");

        int rc = database_subtree_delete_raw(st, "key1", strlen("key1"), '/', promise);
        ASSERT(rc == 0, "Async raw delete returns 0");

        int max_wait = 10000;
        while (!result.completed && max_wait > 0) {
            usleep(1000);
            max_wait--;
        }
        ASSERT(result.completed, "Async raw delete completed");
        ASSERT(result.status == 0, "Async raw delete succeeded");

        promise_destroy(promise);
    }

    database_subtree_close(st);
    database_destroy(db);
    cleanup_tmpdir(tmpdir);

    printf("=== test_subtree_async_crud_raw DONE ===\n");
}

/* ---- Test 9: Async CRUD in sync_only mode ---- */

static void test_subtree_async_sync_only(void) {
    printf("\n=== test_subtree_async_sync_only ===\n");

    /* In sync_only mode, async functions should call sync variants */
    char tmpdir[256];
    database_t* db = create_test_db(tmpdir, sizeof(tmpdir), "wavedb_subtree_async_sync");
    ASSERT(db != NULL, "Create sync_only database");
    ASSERT(db->sync_only == 1, "Database is sync_only");

    database_subtree_t* st = database_subtree_open(db, "sync_ns", '/');
    ASSERT(st != NULL, "Open subtree on 'sync_ns'");

    /* Async put in sync_only mode */
    {
        path_t* put_path = path_create_from_raw("counter", strlen("counter"), '/', 4);
        ASSERT(put_path != NULL, "Create put path");

        identifier_t* put_value = identifier_create_from_raw((const uint8_t*)"42", strlen("42"), 4);
        ASSERT(put_value != NULL, "Create put value");

        test_async_result_t result = {0};
        promise_t* promise = promise_create(test_on_resolve, test_on_reject, &result);
        ASSERT(promise != NULL, "Create put promise");

        database_subtree_put(st, put_path, put_value, promise);

        /* In sync_only mode, promise should resolve immediately */
        ASSERT(result.completed, "Sync-only put completed immediately");
        ASSERT(result.status == 0, "Sync-only put succeeded");

        promise_destroy(promise);
    }

    /* Async get in sync_only mode */
    {
        path_t* get_path = path_create_from_raw("counter", strlen("counter"), '/', 4);
        ASSERT(get_path != NULL, "Create get path");

        test_async_result_t result = {0};
        promise_t* promise = promise_create(test_on_resolve_get, test_on_reject, &result);
        ASSERT(promise != NULL, "Create get promise");

        database_subtree_get(st, get_path, promise);

        ASSERT(result.completed, "Sync-only get completed immediately");
        ASSERT(result.result != NULL, "Sync-only get returned non-NULL result");

        if (result.result != NULL) {
            buffer_t* buf = identifier_to_buffer(result.result);
            if (buf != NULL) {
                ASSERT(memcmp(buf->data, "42", buf->size) == 0,
                       "Sync-only get value matches '42'");
                buffer_destroy(buf);
            }
            identifier_destroy(result.result);
        }

        promise_destroy(promise);
    }

    /* Async delete in sync_only mode */
    {
        path_t* del_path = path_create_from_raw("counter", strlen("counter"), '/', 4);
        ASSERT(del_path != NULL, "Create delete path");

        test_async_result_t result = {0};
        promise_t* promise = promise_create(test_on_resolve, test_on_reject, &result);
        ASSERT(promise != NULL, "Create delete promise");

        database_subtree_delete(st, del_path, promise);

        ASSERT(result.completed, "Sync-only delete completed immediately");
        ASSERT(result.status == 0, "Sync-only delete succeeded");

        promise_destroy(promise);
    }

    database_subtree_close(st);
    database_destroy(db);
    cleanup_tmpdir(tmpdir);

    printf("=== test_subtree_async_sync_only DONE ===\n");
}

/* ---- Main ---- */

int main(void) {
    printf("Running subtree tests...\n");

    test_subtree_lifecycle();
    test_subtree_prepend_key();
    test_subtree_prepend_path();
    test_subtree_sync_crud();
    test_subtree_sync_crud_raw();
    test_subtree_isolation();
    test_subtree_async_crud();
    test_subtree_async_crud_raw();
    test_subtree_async_sync_only();

    printf("\n%d test(s) failed.\n", failures);
    return failures > 0 ? 1 : 0;
}