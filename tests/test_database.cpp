//
// Created by victor on 3/11/26.
//

#include <gtest/gtest.h>
#include <future>
#include <vector>
#include <string>
#include <chrono>
#include <chrono>
extern "C" {
#include "Database/database.h"
#include "Time/wheel.h"
#include "Workers/pool.h"
#include "HBTrie/path.h"
#include "HBTrie/identifier.h"
#include "Buffer/buffer.h"
#include "Util/allocator.h"
}

#define TEST_COUNT 100

class DatabaseTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Create test directory
        test_dir = "/tmp/wavedb_test_" + std::to_string(getpid()) + "_" + std::to_string(test_counter++);
        mkdir(test_dir.c_str(), 0700);

        // Create work pool (match reference pattern)
        pool = work_pool_create(platform_core_count());
        work_pool_launch(pool);

        // Create timing wheel (match reference pattern)
        wheel = hierarchical_timing_wheel_create(8, pool);
        hierarchical_timing_wheel_run(wheel);
    }

    void TearDown() override {
        // Destroy database first
        if (db) {
            database_destroy(db);
            db = nullptr;
        }

        // Wait for timing wheel to be idle BEFORE stopping
        if (wheel) {
            hierarchical_timing_wheel_wait_for_idle_signal(wheel);
            hierarchical_timing_wheel_stop(wheel);
        }

        // Shutdown and join pool BEFORE destroying
        if (pool) {
            work_pool_shutdown(pool);
            work_pool_join_all(pool);
        }

        // Destroy wheel then pool
        if (wheel) {
            hierarchical_timing_wheel_destroy(wheel);
            wheel = nullptr;
        }
        if (pool) {
            work_pool_destroy(pool);
            pool = nullptr;
        }

        // Cleanup test directory
        std::string cmd = std::string("rm -rf ") + test_dir;
        system(cmd.c_str());
    }

    // Helper to create a path from string subscripts
    path_t* make_path(std::initializer_list<const char*> subscripts) {
        path_t* path = path_create();
        for (const char* sub : subscripts) {
            buffer_t* buf = buffer_create_from_pointer_copy((uint8_t*)sub, strlen(sub));
            identifier_t* id = identifier_create(buf, 0);
            buffer_destroy(buf);
            path_append(path, id);
            identifier_destroy(id);
        }
        return path;
    }

    // Helper to create an identifier value
    identifier_t* make_value(const char* data) {
        buffer_t* buf = buffer_create_from_pointer_copy((uint8_t*)data, strlen(data));
        identifier_t* id = identifier_create(buf, 0);
        buffer_destroy(buf);
        return id;
    }

    // Helper to verify identifier content
    void expect_identifier_eq(identifier_t* id, const char* expected) {
        ASSERT_NE(id, nullptr);
        buffer_t* buf = identifier_to_buffer(id);
        ASSERT_NE(buf, nullptr);
        EXPECT_EQ(memcmp(buf->data, expected, strlen(expected)), 0);
        buffer_destroy(buf);
    }

    database_t* db = nullptr;
    work_pool_t* pool = nullptr;
    hierarchical_timing_wheel_t* wheel = nullptr;
    std::string test_dir;
    static int test_counter;

public:
    // Promise arrays for async operations (public for callback access)
    std::promise<void> put_promise[TEST_COUNT];
    std::promise<identifier_t*> get_promise[TEST_COUNT];
    std::promise<void> delete_promise[TEST_COUNT];
};

int DatabaseTest::test_counter = 0;

// Context struct for async callbacks - includes test pointer for promise access
typedef struct {
    size_t i;
    DatabaseTest* test;
} db_test_ctx;

// Callback wrappers for put operations
extern "C" void put_callback_wrapper(void* ctx, void* payload) {
    auto dbtc = static_cast<db_test_ctx*>(ctx);
    dbtc->test->put_promise[dbtc->i].set_value();
    free(ctx);
}

extern "C" void put_callback_err_wrapper(void* ctx, async_error_t* payload) {
    auto dbtc = static_cast<db_test_ctx*>(ctx);
    try {
        throw std::runtime_error((const char*)payload->message);
    } catch(...) {
        dbtc->test->put_promise[dbtc->i].set_exception(std::current_exception());
    }
    error_destroy(payload);
    free(ctx);
}

// Callback wrappers for get operations
extern "C" void get_callback_wrapper(void* ctx, void* payload) {
    auto dbtc = static_cast<db_test_ctx*>(ctx);
    // CONSUME'd values have yield=1, REFERENCE consumes the yield ticket
    // and adjusts the refcount so that identifier_destroy works correctly
    identifier_t* value = (identifier_t*)REFERENCE(payload, identifier_t);
    dbtc->test->get_promise[dbtc->i].set_value(value);
    free(ctx);
}

extern "C" void get_callback_err_wrapper(void* ctx, async_error_t* payload) {
    auto dbtc = static_cast<db_test_ctx*>(ctx);
    try {
        throw std::runtime_error((const char*)payload->message);
    } catch(...) {
        dbtc->test->get_promise[dbtc->i].set_exception(std::current_exception());
    }
    error_destroy(payload);
    free(ctx);
}

// Callback wrappers for delete operations
extern "C" void delete_callback_wrapper(void* ctx, void* payload) {
    auto dbtc = static_cast<db_test_ctx*>(ctx);
    dbtc->test->delete_promise[dbtc->i].set_value();
    free(ctx);
}

extern "C" void delete_callback_err_wrapper(void* ctx, async_error_t* payload) {
    auto dbtc = static_cast<db_test_ctx*>(ctx);
    try {
        throw std::runtime_error((const char*)payload->message);
    } catch(...) {
        dbtc->test->delete_promise[dbtc->i].set_exception(std::current_exception());
    }
    error_destroy(payload);
    free(ctx);
}

TEST_F(DatabaseTest, CreateDestroy) {
    int error = 0;
    db = database_create(test_dir.c_str(), 0, NULL, 0, 0, 0, 0, pool, wheel, &error);
    ASSERT_NE(db, nullptr);
    EXPECT_EQ(error, 0);
}

TEST_F(DatabaseTest, PutGet) {
    int error = 0;
    db = database_create(test_dir.c_str(), 0, NULL, 0, 0, 0, 0, pool, wheel, &error);
    ASSERT_NE(db, nullptr);
    ASSERT_EQ(error, 0);

    // Create path and value
    path_t* path = make_path({"users", "alice", "name"});
    identifier_t* value = make_value("Alice Smith");

    // Create promise for put
    db_test_ctx* ctx = (db_test_ctx*)get_memory(sizeof(db_test_ctx));
    ctx->i = 0;
    ctx->test = this;
    promise_t* put_prom = promise_create(put_callback_wrapper, put_callback_err_wrapper, ctx);

    // Execute put
    database_put(db, path, value, put_prom);

    // Wait for completion
    std::future<void> put_future = put_promise[0].get_future();
    EXPECT_NO_THROW(put_future.get());

    promise_destroy(put_prom);

    // Create promise for get
    path_t* get_path = make_path({"users", "alice", "name"});
    ctx = (db_test_ctx*)get_memory(sizeof(db_test_ctx));
    ctx->i = 0;
    ctx->test = this;
    promise_t* get_prom = promise_create(get_callback_wrapper, get_callback_err_wrapper, ctx);

    // Execute get
    database_get(db, get_path, get_prom);

    // Wait for result
    std::future<identifier_t*> get_future = get_promise[0].get_future();
    identifier_t* result = nullptr;
    EXPECT_NO_THROW({ result = get_future.get(); });

    // Verify result
    ASSERT_NE(result, nullptr);
    expect_identifier_eq(result, "Alice Smith");
    identifier_destroy(result);

    promise_destroy(get_prom);
}

TEST_F(DatabaseTest, PutGetMultiple) {
    int error = 0;
    db = database_create(test_dir.c_str(), 0, NULL, 0, 0, 0, 0, pool, wheel, &error);
    ASSERT_NE(db, nullptr);
    ASSERT_EQ(error, 0);

    const int COUNT = 25;
    

    // Store multiple values sequentially
    for (int i = 0; i < COUNT; i++) {
        char sub1[32], sub2[32], val[32];
        snprintf(sub1, sizeof(sub1), "user%d", i);
        snprintf(sub2, sizeof(sub2), "field%d", i);
        snprintf(val, sizeof(val), "value%d", i);

        path_t* path = make_path({"users", sub1, sub2});
        identifier_t* value = make_value(val);

        db_test_ctx* ctx = (db_test_ctx*)get_memory(sizeof(db_test_ctx));
        ctx->i = i;
        ctx->test = this;
        promise_t* put_prom = promise_create(put_callback_wrapper, put_callback_err_wrapper, ctx);
        database_put(db, path, value, put_prom);

        std::future<void> put_future = put_promise[i].get_future();
        EXPECT_NO_THROW(put_future.get());
        promise_destroy(put_prom);
    }

    if (HasFailure()) {
        GTEST_SKIP();
    }

    // Retrieve and verify all values sequentially
    for (int i = 0; i < COUNT; i++) {
        char sub1[32], sub2[32], val[32];
        snprintf(sub1, sizeof(sub1), "user%d", i);
        snprintf(sub2, sizeof(sub2), "field%d", i);
        snprintf(val, sizeof(val), "value%d", i);

        path_t* get_path = make_path({"users", sub1, sub2});

        db_test_ctx* ctx = (db_test_ctx*)get_memory(sizeof(db_test_ctx));
        ctx->i = i;
        ctx->test = this;
        promise_t* get_prom = promise_create(get_callback_wrapper, get_callback_err_wrapper, ctx);
        database_get(db, get_path, get_prom);

        std::future<identifier_t*> get_future = get_promise[i].get_future();
        identifier_t* result = nullptr;
        EXPECT_NO_THROW({ result = get_future.get(); });

        ASSERT_NE(result, nullptr);
        expect_identifier_eq(result, val);
        identifier_destroy(result);
        promise_destroy(get_prom);
    }
}

TEST_F(DatabaseTest, GetNonExistent) {
    int error = 0;
    db = database_create(test_dir.c_str(), 0, NULL, 0, 0, 0, 0, pool, wheel, &error);
    ASSERT_NE(db, nullptr);
    ASSERT_EQ(error, 0);

    // Try to get a non-existent key
    path_t* path = make_path({"nonexistent", "key"});

    db_test_ctx* ctx = (db_test_ctx*)get_memory(sizeof(db_test_ctx));
    ctx->i = 0;
    ctx->test = this;
    promise_t* get_prom = promise_create(get_callback_wrapper, get_callback_err_wrapper, ctx);

    
    database_get(db, path, get_prom);

    std::future<identifier_t*> get_future = get_promise[0].get_future();
    identifier_t* result = nullptr;
    EXPECT_NO_THROW({ result = get_future.get(); });

    // Result should be NULL for non-existent key
    EXPECT_EQ(result, nullptr);

    promise_destroy(get_prom);
}

TEST_F(DatabaseTest, UpdateValue) {
    int error = 0;
    db = database_create(test_dir.c_str(), 0, NULL, 0, 0, 0, 0, pool, wheel, &error);
    ASSERT_NE(db, nullptr);
    ASSERT_EQ(error, 0);

    

    // Insert initial value
    path_t* path = make_path({"key"});
    identifier_t* value1 = make_value("value1");

    db_test_ctx* ctx = (db_test_ctx*)get_memory(sizeof(db_test_ctx));
    ctx->i = 0;
    ctx->test = this;
    promise_t* put_prom = promise_create(put_callback_wrapper, put_callback_err_wrapper, ctx);
    database_put(db, path, value1, put_prom);

    std::future<void> put_future = put_promise[0].get_future();
    EXPECT_NO_THROW(put_future.get());
    promise_destroy(put_prom);

    if (HasFailure()) {
        GTEST_SKIP();
    }

    // Verify initial value
    path_t* get_path1 = make_path({"key"});
    ctx = (db_test_ctx*)get_memory(sizeof(db_test_ctx));
    ctx->i = 0;
    ctx->test = this;
    promise_t* get_prom1 = promise_create(get_callback_wrapper, get_callback_err_wrapper, ctx);
    database_get(db, get_path1, get_prom1);

    std::future<identifier_t*> get_future1 = get_promise[0].get_future();
    identifier_t* result1 = nullptr;
    EXPECT_NO_THROW({ result1 = get_future1.get(); });

    ASSERT_NE(result1, nullptr);
    expect_identifier_eq(result1, "value1");
    identifier_destroy(result1);
    promise_destroy(get_prom1);

    if (HasFailure()) {
        GTEST_SKIP();
    }

    // Update with new value
    path_t* path2 = make_path({"key"});
    identifier_t* value2 = make_value("value2");

    ctx = (db_test_ctx*)get_memory(sizeof(db_test_ctx));
    ctx->i = 1;
    ctx->test = this;
    promise_t* put_prom2 = promise_create(put_callback_wrapper, put_callback_err_wrapper, ctx);
    database_put(db, path2, value2, put_prom2);

    std::future<void> put_future2 = put_promise[1].get_future();
    EXPECT_NO_THROW(put_future2.get());
    promise_destroy(put_prom2);

    if (HasFailure()) {
        GTEST_SKIP();
    }

    // Verify updated value
    path_t* get_path2 = make_path({"key"});
    ctx = (db_test_ctx*)get_memory(sizeof(db_test_ctx));
    ctx->i = 1;
    ctx->test = this;
    promise_t* get_prom2 = promise_create(get_callback_wrapper, get_callback_err_wrapper, ctx);
    database_get(db, get_path2, get_prom2);

    std::future<identifier_t*> get_future2 = get_promise[1].get_future();
    identifier_t* result2 = nullptr;
    EXPECT_NO_THROW({ result2 = get_future2.get(); });

    ASSERT_NE(result2, nullptr);
    expect_identifier_eq(result2, "value2");
    identifier_destroy(result2);
    promise_destroy(get_prom2);
}

TEST_F(DatabaseTest, Delete) {
    int error = 0;
    db = database_create(test_dir.c_str(), 0, NULL, 0, 0, 0, 0, pool, wheel, &error);
    ASSERT_NE(db, nullptr);
    ASSERT_EQ(error, 0);

    

    // Insert a value
    path_t* path = make_path({"key", "to", "delete"});
    identifier_t* value = make_value("deleteme");

    db_test_ctx* ctx = (db_test_ctx*)get_memory(sizeof(db_test_ctx));
    ctx->i = 0;
    ctx->test = this;
    promise_t* put_prom = promise_create(put_callback_wrapper, put_callback_err_wrapper, ctx);
    database_put(db, path, value, put_prom);

    std::future<void> put_future = put_promise[0].get_future();
    EXPECT_NO_THROW(put_future.get());
    promise_destroy(put_prom);

    if (HasFailure()) {
        GTEST_SKIP();
    }

    // Verify it exists
    path_t* get_path1 = make_path({"key", "to", "delete"});
    ctx = (db_test_ctx*)get_memory(sizeof(db_test_ctx));
    ctx->i = 0;
    ctx->test = this;
    promise_t* get_prom1 = promise_create(get_callback_wrapper, get_callback_err_wrapper, ctx);
    database_get(db, get_path1, get_prom1);

    std::future<identifier_t*> get_future1 = get_promise[0].get_future();
    identifier_t* result1 = nullptr;
    EXPECT_NO_THROW({ result1 = get_future1.get(); });
    ASSERT_NE(result1, nullptr);
    identifier_destroy(result1);
    promise_destroy(get_prom1);

    if (HasFailure()) {
        GTEST_SKIP();
    }

    // Delete the value
    path_t* del_path = make_path({"key", "to", "delete"});
    ctx = (db_test_ctx*)get_memory(sizeof(db_test_ctx));
    ctx->i = 0;
    ctx->test = this;
    promise_t* del_prom = promise_create(delete_callback_wrapper, delete_callback_err_wrapper, ctx);
    database_delete(db, del_path, del_prom);

    std::future<void> del_future = delete_promise[0].get_future();
    EXPECT_NO_THROW(del_future.get());
    promise_destroy(del_prom);

    if (HasFailure()) {
        GTEST_SKIP();
    }

    // Verify it's gone
    path_t* get_path2 = make_path({"key", "to", "delete"});
    ctx = (db_test_ctx*)get_memory(sizeof(db_test_ctx));
    ctx->i = 1;
    ctx->test = this;
    promise_t* get_prom2 = promise_create(get_callback_wrapper, get_callback_err_wrapper, ctx);
    database_get(db, get_path2, get_prom2);

    std::future<identifier_t*> get_future2 = get_promise[1].get_future();
    identifier_t* result2 = nullptr;
    EXPECT_NO_THROW({ result2 = get_future2.get(); });
    EXPECT_EQ(result2, nullptr);
    promise_destroy(get_prom2);
}

TEST_F(DatabaseTest, DeleteNonExistent) {
    int error = 0;
    db = database_create(test_dir.c_str(), 0, NULL, 0, 0, 0, 0, pool, wheel, &error);
    ASSERT_NE(db, nullptr);
    ASSERT_EQ(error, 0);

    

    // Delete a key that doesn't exist (should succeed without error)
    path_t* del_path = make_path({"nonexistent", "key"});

    db_test_ctx* ctx = (db_test_ctx*)get_memory(sizeof(db_test_ctx));
    ctx->i = 0;
    ctx->test = this;
    promise_t* del_prom = promise_create(delete_callback_wrapper, delete_callback_err_wrapper, ctx);
    database_delete(db, del_path, del_prom);

    std::future<void> del_future = delete_promise[0].get_future();
    EXPECT_NO_THROW(del_future.get());
    promise_destroy(del_prom);
}

TEST_F(DatabaseTest, ConcurrentOperations) {
    int error = 0;
    db = database_create(test_dir.c_str(), 0, NULL, 0, 0, 0, 0, pool, wheel, &error);
    ASSERT_NE(db, nullptr);
    ASSERT_EQ(error, 0);

    const int COUNT = 50;
    

    // Insert values sequentially
    for (int i = 0; i < COUNT; i++) {
        char sub[32], val[32];
        snprintf(sub, sizeof(sub), "concurrent%d", i);
        snprintf(val, sizeof(val), "concurrent_value%d", i);

        path_t* path = make_path({"concurrent", sub});
        identifier_t* value = make_value(val);

        db_test_ctx* ctx = (db_test_ctx*)get_memory(sizeof(db_test_ctx));
        ctx->i = i;
        ctx->test = this;
        promise_t* put_prom = promise_create(put_callback_wrapper, put_callback_err_wrapper, ctx);
        database_put(db, path, value, put_prom);

        std::future<void> put_future = put_promise[i].get_future();
        EXPECT_NO_THROW(put_future.get());
        promise_destroy(put_prom);
    }

    if (HasFailure()) {
        GTEST_SKIP();
    }

    // Verify all values exist
    for (int i = 0; i < COUNT; i++) {
        char sub[32], val[32];
        snprintf(sub, sizeof(sub), "concurrent%d", i);
        snprintf(val, sizeof(val), "concurrent_value%d", i);

        path_t* get_path = make_path({"concurrent", sub});

        db_test_ctx* ctx = (db_test_ctx*)get_memory(sizeof(db_test_ctx));
        ctx->i = i;
        ctx->test = this;
        promise_t* get_prom = promise_create(get_callback_wrapper, get_callback_err_wrapper, ctx);
        database_get(db, get_path, get_prom);

        std::future<identifier_t*> get_future = get_promise[i].get_future();
        identifier_t* result = nullptr;
        EXPECT_NO_THROW({ result = get_future.get(); });

        ASSERT_NE(result, nullptr);
        expect_identifier_eq(result, val);
        identifier_destroy(result);
        promise_destroy(get_prom);
    }
}

TEST_F(DatabaseTest, Persistence) {
    int error = 0;

    // First instance: create and insert
    {
        db = database_create(test_dir.c_str(), 0, NULL, 0, 0, 0, 0, pool, wheel, &error);
        ASSERT_NE(db, nullptr);
        ASSERT_EQ(error, 0);

        

        // Insert values
        path_t* path1 = make_path({"persistent", "key1"});
        identifier_t* value1 = make_value("persistent_value1");

        db_test_ctx* ctx = (db_test_ctx*)get_memory(sizeof(db_test_ctx));
        ctx->i = 0;
        ctx->test = this;
        promise_t* put_prom = promise_create(put_callback_wrapper, put_callback_err_wrapper, ctx);
        database_put(db, path1, value1, put_prom);

        std::future<void> put_future = put_promise[0].get_future();
        EXPECT_NO_THROW(put_future.get());
        promise_destroy(put_prom);

        path_t* path2 = make_path({"persistent", "key2"});
        identifier_t* value2 = make_value("persistent_value2");

        ctx = (db_test_ctx*)get_memory(sizeof(db_test_ctx));
        ctx->i = 1;
        ctx->test = this;
        promise_t* put_prom2 = promise_create(put_callback_wrapper, put_callback_err_wrapper, ctx);
        database_put(db, path2, value2, put_prom2);

        std::future<void> put_future2 = put_promise[1].get_future();
        EXPECT_NO_THROW(put_future2.get());
        promise_destroy(put_prom2);

        // Force snapshot
        database_snapshot(db);

        // Destroy database
        database_destroy(db);
        db = nullptr;
    }

    // Second instance: reopen and verify
    {
        db = database_create(test_dir.c_str(), 0, NULL, 0, 0, 0, 0, pool, wheel, &error);
        ASSERT_NE(db, nullptr);
        ASSERT_EQ(error, 0);

        

        // Verify first value
        path_t* get_path1 = make_path({"persistent", "key1"});

        db_test_ctx* ctx = (db_test_ctx*)get_memory(sizeof(db_test_ctx));
        ctx->i = 0;
        ctx->test = this;
        promise_t* get_prom1 = promise_create(get_callback_wrapper, get_callback_err_wrapper, ctx);
        database_get(db, get_path1, get_prom1);

        std::future<identifier_t*> get_future1 = get_promise[0].get_future();
        identifier_t* result1 = nullptr;
        EXPECT_NO_THROW({ result1 = get_future1.get(); });

        ASSERT_NE(result1, nullptr);
        expect_identifier_eq(result1, "persistent_value1");
        identifier_destroy(result1);
        promise_destroy(get_prom1);

        // Verify second value
        path_t* get_path2 = make_path({"persistent", "key2"});
        ctx = (db_test_ctx*)get_memory(sizeof(db_test_ctx));
        ctx->i = 1;
        ctx->test = this;
        promise_t* get_prom2 = promise_create(get_callback_wrapper, get_callback_err_wrapper, ctx);
        database_get(db, get_path2, get_prom2);

        std::future<identifier_t*> get_future2 = get_promise[1].get_future();
        identifier_t* result2 = nullptr;
        EXPECT_NO_THROW({ result2 = get_future2.get(); });

        ASSERT_NE(result2, nullptr);
        expect_identifier_eq(result2, "persistent_value2");
        identifier_destroy(result2);
        promise_destroy(get_prom2);
    }
}

TEST_F(DatabaseTest, VaryingPathDepths) {
    int error = 0;
    db = database_create(test_dir.c_str(), 0, NULL, 0, 0, 0, 0, pool, wheel, &error);
    ASSERT_NE(db, nullptr);
    ASSERT_EQ(error, 0);

    

    // Test paths with depths 1 through 15 sequentially
    for (int depth = 1; depth <= 15; depth++) {
        path_t* path = path_create();
        for (int i = 0; i < depth; i++) {
            char sub[16];
            snprintf(sub, sizeof(sub), "d%d_l%d", depth, i);
            buffer_t* buf = buffer_create_from_pointer_copy((uint8_t*)sub, strlen(sub));
            identifier_t* id = identifier_create(buf, 0);
            buffer_destroy(buf);
            path_append(path, id);
            identifier_destroy(id);
        }

        char val[16];
        snprintf(val, sizeof(val), "v%d", depth);
        identifier_t* value = make_value(val);

        db_test_ctx* ctx = (db_test_ctx*)get_memory(sizeof(db_test_ctx));
        ctx->i = depth - 1;
        ctx->test = this;
        promise_t* put_prom = promise_create(put_callback_wrapper, put_callback_err_wrapper, ctx);
        database_put(db, path, value, put_prom);

        std::future<void> put_future = put_promise[depth - 1].get_future();
        EXPECT_NO_THROW(put_future.get());
        promise_destroy(put_prom);
    }

    if (HasFailure()) {
        GTEST_SKIP();
    }

    // Verify all depths can be found
    for (int depth = 1; depth <= 15; depth++) {
        path_t* path = path_create();
        for (int i = 0; i < depth; i++) {
            char sub[16];
            snprintf(sub, sizeof(sub), "d%d_l%d", depth, i);
            buffer_t* buf = buffer_create_from_pointer_copy((uint8_t*)sub, strlen(sub));
            identifier_t* id = identifier_create(buf, 0);
            buffer_destroy(buf);
            path_append(path, id);
            identifier_destroy(id);
        }

        db_test_ctx* ctx = (db_test_ctx*)get_memory(sizeof(db_test_ctx));
        ctx->i = depth - 1;
        ctx->test = this;
        promise_t* get_prom = promise_create(get_callback_wrapper, get_callback_err_wrapper, ctx);
        database_get(db, path, get_prom);

        std::future<identifier_t*> get_future = get_promise[depth - 1].get_future();
        identifier_t* result = nullptr;
        EXPECT_NO_THROW({ result = get_future.get(); });

        ASSERT_NE(result, nullptr);

        char expected[16];
        snprintf(expected, sizeof(expected), "v%d", depth);
        expect_identifier_eq(result, expected);
        identifier_destroy(result);
        promise_destroy(get_prom);

        // Note: database_get consumed the path reference
    }
}

TEST_F(DatabaseTest, WriteBatchSyncBasic) {
    int error = 0;
    db = database_create(test_dir.c_str(), 0, NULL, 0, 0, 0, 0, pool, wheel, &error);
    ASSERT_NE(db, nullptr);
    ASSERT_EQ(error, 0);

    // Create batch
    batch_t* batch = batch_create(10);
    ASSERT_NE(batch, nullptr);

    // Add operations to batch
    path_t* path1 = make_path({"batch", "key1"});
    identifier_t* value1 = make_value("batch_value1");
    EXPECT_EQ(batch_add_put(batch, path1, value1), 0);

    path_t* path2 = make_path({"batch", "key2"});
    identifier_t* value2 = make_value("batch_value2");
    EXPECT_EQ(batch_add_put(batch, path2, value2), 0);

    // Submit batch synchronously
    int result = database_write_batch_sync(db, batch);
    EXPECT_EQ(result, 0);

    // Verify both values exist
    path_t* get_path1 = make_path({"batch", "key1"});
    identifier_t* result1 = nullptr;
    EXPECT_EQ(database_get_sync(db, get_path1, &result1), 0);
    ASSERT_NE(result1, nullptr);
    expect_identifier_eq(result1, "batch_value1");
    identifier_destroy(result1);

    path_t* get_path2 = make_path({"batch", "key2"});
    identifier_t* result2 = nullptr;
    EXPECT_EQ(database_get_sync(db, get_path2, &result2), 0);
    ASSERT_NE(result2, nullptr);
    expect_identifier_eq(result2, "batch_value2");
    identifier_destroy(result2);

    // Cleanup
    batch_destroy(batch);
}

TEST_F(DatabaseTest, WriteBatchSyncEmpty) {
    int error = 0;
    db = database_create(test_dir.c_str(), 0, NULL, 0, 0, 0, 0, pool, wheel, &error);
    ASSERT_NE(db, nullptr);
    ASSERT_EQ(error, 0);

    // Create empty batch
    batch_t* batch = batch_create(10);
    ASSERT_NE(batch, nullptr);

    // Submit empty batch - should return error -3
    int result = database_write_batch_sync(db, batch);
    EXPECT_EQ(result, -3);

    // Cleanup
    batch_destroy(batch);
}

TEST_F(DatabaseTest, WriteBatchSyncTooLarge) {
    int error = 0;
    db = database_create(test_dir.c_str(), 0, NULL, 0, 0, 0, 0, pool, wheel, &error);
    ASSERT_NE(db, nullptr);
    ASSERT_EQ(error, 0);

    // Create batch
    batch_t* batch = batch_create(10000);
    ASSERT_NE(batch, nullptr);

    // Add operations to exceed WAL max size (128KB default)
    // Each operation is ~100 bytes, so add 2000 operations to exceed 128KB
    for (int i = 0; i < 2000; i++) {
        char sub1[32], sub2[32], val[256];
        snprintf(sub1, sizeof(sub1), "large%d", i);
        snprintf(sub2, sizeof(sub2), "field%d", i);
        snprintf(val, sizeof(val), "very_long_value_string_to_increase_size_%d", i);

        path_t* path = make_path({sub1, sub2});
        identifier_t* value = make_value(val);

        int result = batch_add_put(batch, path, value);
        if (result == -2) {
            // Batch is full, that's fine for this test
            break;
        }
        EXPECT_EQ(result, 0);
    }

    // Submit oversized batch - should return error -5
    int result = database_write_batch_sync(db, batch);
    EXPECT_EQ(result, -5);

    // Cleanup
    batch_destroy(batch);
}

TEST_F(DatabaseTest, WriteBatchSyncDoubleSubmit) {
    int error = 0;
    db = database_create(test_dir.c_str(), 0, NULL, 0, 0, 0, 0, pool, wheel, &error);
    ASSERT_NE(db, nullptr);
    ASSERT_EQ(error, 0);

    // Create batch
    batch_t* batch = batch_create(10);
    ASSERT_NE(batch, nullptr);

    // Add operation
    path_t* path1 = make_path({"batch", "key"});
    identifier_t* value1 = make_value("batch_value");
    EXPECT_EQ(batch_add_put(batch, path1, value1), 0);

    // First submission should succeed
    int result = database_write_batch_sync(db, batch);
    EXPECT_EQ(result, 0);

    // Second submission should fail with error -6
    result = database_write_batch_sync(db, batch);
    EXPECT_EQ(result, -6);

    // Cleanup
    batch_destroy(batch);
}

TEST_F(DatabaseTest, WriteBatchSyncWithDelete) {
    int error = 0;
    db = database_create(test_dir.c_str(), 0, NULL, 0, 0, 0, 0, pool, wheel, &error);
    ASSERT_NE(db, nullptr);
    ASSERT_EQ(error, 0);

    // Put key1 and key2 individually
    path_t* path1 = make_path({"batch", "key1"});
    identifier_t* value1 = make_value("batch_value1");
    EXPECT_EQ(database_put_sync(db, path1, value1), 0);

    path_t* path2 = make_path({"batch", "key2"});
    identifier_t* value2 = make_value("batch_value2");
    EXPECT_EQ(database_put_sync(db, path2, value2), 0);

    // Verify both exist
    path_t* get_path1 = make_path({"batch", "key1"});
    identifier_t* result1 = nullptr;
    EXPECT_EQ(database_get_sync(db, get_path1, &result1), 0);
    ASSERT_NE(result1, nullptr);
    expect_identifier_eq(result1, "batch_value1");
    identifier_destroy(result1);

    // Create batch with put and delete
    batch_t* batch = batch_create(10);
    ASSERT_NE(batch, nullptr);

    path_t* path3 = make_path({"batch", "key3"});
    identifier_t* value3 = make_value("batch_value3");
    EXPECT_EQ(batch_add_put(batch, path3, value3), 0);

    path_t* del_path1 = make_path({"batch", "key1"});
    EXPECT_EQ(batch_add_delete(batch, del_path1), 0);

    // Submit batch
    int result = database_write_batch_sync(db, batch);
    EXPECT_EQ(result, 0);

    // Verify key1 is deleted
    path_t* get_path1_after = make_path({"batch", "key1"});
    identifier_t* result1_after = nullptr;
    int get_rc = database_get_sync(db, get_path1_after, &result1_after);
    EXPECT_EQ(result1_after, nullptr) << "key1 should be deleted after batch delete";

    // Verify key2 still exists
    path_t* get_path2 = make_path({"batch", "key2"});
    identifier_t* result2 = nullptr;
    EXPECT_EQ(database_get_sync(db, get_path2, &result2), 0);
    ASSERT_NE(result2, nullptr);
    expect_identifier_eq(result2, "batch_value2");
    identifier_destroy(result2);

    // Verify key3 was put by batch
    path_t* get_path3 = make_path({"batch", "key3"});
    identifier_t* result3 = nullptr;
    EXPECT_EQ(database_get_sync(db, get_path3, &result3), 0);
    ASSERT_NE(result3, nullptr);
    expect_identifier_eq(result3, "batch_value3");
    identifier_destroy(result3);

    // Cleanup
    batch_destroy(batch);
}

TEST_F(DatabaseTest, Snapshot) {
    int error = 0;
    db = database_create(test_dir.c_str(), 0, NULL, 0, 0, 0, 0, pool, wheel, &error);
    ASSERT_NE(db, nullptr);
    ASSERT_EQ(error, 0);



    // Insert values
    const int COUNT = 10;
    for (int i = 0; i < COUNT; i++) {
        char sub[32], val[32];
        snprintf(sub, sizeof(sub), "snap%d", i);
        snprintf(val, sizeof(val), "snap_value%d", i);

        path_t* path = make_path({"snapshot", sub});
        identifier_t* value = make_value(val);

        db_test_ctx* ctx = (db_test_ctx*)get_memory(sizeof(db_test_ctx));
        ctx->i = i;
        ctx->test = this;
        promise_t* put_prom = promise_create(put_callback_wrapper, put_callback_err_wrapper, ctx);
        database_put(db, path, value, put_prom);

        std::future<void> put_future = put_promise[i].get_future();
        EXPECT_NO_THROW(put_future.get());
        promise_destroy(put_prom);
    }

    if (HasFailure()) {
        GTEST_SKIP();
    }

    // Force snapshot
    EXPECT_EQ(database_snapshot(db), 0);

    // Wait a bit for the timing wheel to process
    usleep(100000);  // 100ms

    if (HasFailure()) {
        GTEST_SKIP();
    }

    // Verify all values still exist after snapshot
    for (int i = 0; i < COUNT; i++) {
        char sub[32], val[32];
        snprintf(sub, sizeof(sub), "snap%d", i);
        snprintf(val, sizeof(val), "snap_value%d", i);

        path_t* get_path = make_path({"snapshot", sub});

        db_test_ctx* ctx = (db_test_ctx*)get_memory(sizeof(db_test_ctx));
        ctx->i = i;
        ctx->test = this;
        promise_t* get_prom = promise_create(get_callback_wrapper, get_callback_err_wrapper, ctx);
        database_get(db, get_path, get_prom);

        std::future<identifier_t*> get_future = get_promise[i].get_future();
        identifier_t* result = nullptr;
        EXPECT_NO_THROW({ result = get_future.get(); });

        ASSERT_NE(result, nullptr);
        expect_identifier_eq(result, val);
        identifier_destroy(result);
        promise_destroy(get_prom);
    }
}

TEST_F(DatabaseTest, BatchVsIndividualPutPerformance) {
    int error = 0;
    db = database_create(test_dir.c_str(), 0, NULL, 0, 0, 0, 0, pool, wheel, &error);
    ASSERT_NE(db, nullptr);
    ASSERT_EQ(error, 0);

    const int COUNT = 100;  // Use smaller count to stay within TEST_COUNT

    // Measure individual put operations using synchronous API
    auto individual_start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < COUNT; i++) {
        char key[32], val[32];
        snprintf(key, sizeof(key), "ind_key_%d", i);
        snprintf(val, sizeof(val), "ind_val_%d", i);

        path_t* path = make_path({key});
        identifier_t* value = make_value(val);

        int result = database_put_sync(db, path, value);
        EXPECT_EQ(result, 0);
    }

    auto individual_end = std::chrono::high_resolution_clock::now();
    auto individual_ms = std::chrono::duration_cast<std::chrono::milliseconds>(individual_end - individual_start).count();

    if (HasFailure()) {
        GTEST_SKIP();
    }

    // Measure batch put operations
    batch_t* batch = batch_create(COUNT);
    ASSERT_NE(batch, nullptr);

    // Add all operations to batch
    for (int i = 0; i < COUNT; i++) {
        char key[32], val[32];
        snprintf(key, sizeof(key), "batch_key_%d", i);
        snprintf(val, sizeof(val), "batch_val_%d", i);

        path_t* path = make_path({key});
        identifier_t* value = make_value(val);

        int result = batch_add_put(batch, path, value);
        EXPECT_EQ(result, 0);
        if (result != 0) {
            // Clean up on error
            path_destroy(path);
            identifier_destroy(value);
        }
    }

    auto batch_start = std::chrono::high_resolution_clock::now();

    // Submit batch
    int result = database_write_batch_sync(db, batch);
    EXPECT_EQ(result, 0);

    auto batch_end = std::chrono::high_resolution_clock::now();
    auto batch_ms = std::chrono::duration_cast<std::chrono::milliseconds>(batch_end - batch_start).count();

    batch_destroy(batch);

    if (HasFailure()) {
        GTEST_SKIP();
    }

    // Verify all batch values exist
    for (int i = 0; i < COUNT; i++) {
        char key[32], val[32];
        snprintf(key, sizeof(key), "batch_key_%d", i);
        snprintf(val, sizeof(val), "batch_val_%d", i);

        path_t* get_path = make_path({key});

        identifier_t* result_val = nullptr;
        EXPECT_EQ(database_get_sync(db, get_path, &result_val), 0);
        ASSERT_NE(result_val, nullptr);
        expect_identifier_eq(result_val, val);
        identifier_destroy(result_val);
    }

    // Log performance comparison
    if (individual_ms > 0 && batch_ms > 0) {
        double individual_ops_per_sec = (double)COUNT / (individual_ms / 1000.0);
        double batch_ops_per_sec = (double)COUNT / (batch_ms / 1000.0);

        // Performance metrics logged for analysis
        // Individual puts: individual_ms ms (individual_ops_per_sec ops/sec)
        // Batch put: batch_ms ms (batch_ops_per_sec ops/sec)
        // Speedup: (individual_ops_per_sec / batch_ops_per_sec)x

        SUCCEED() << "Individual: " << individual_ms << "ms (" << individual_ops_per_sec
                  << " ops/sec), Batch: " << batch_ms << "ms (" << batch_ops_per_sec << " ops/sec)";
    } else {
        SUCCEED() << "Individual: " << individual_ms << "ms, Batch: " << batch_ms << "ms";
    }
}