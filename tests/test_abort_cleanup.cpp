//
// Test that database operation abort functions properly clean up resources
// when work items are queued but not executed during shutdown
//

#include <gtest/gtest.h>
#include "../src/Database/database.h"
#include "../src/Workers/pool.h"
#include "../src/Time/wheel.h"
#include "../src/HBTrie/path.h"
#include "../src/HBTrie/identifier.h"
#include "../src/Buffer/buffer.h"
#include <memory>
#include <string>
#include <vector>
#include <cstring>

class AbortCleanupTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Create test directory
        test_dir = "/tmp/wavedb_abort_test_" + std::to_string(getpid());
        mkdir(test_dir.c_str(), 0755);

        // Create worker pool with only 1 thread
        pool = work_pool_create(1);
        ASSERT_NE(pool, nullptr);

        // Create timing wheel
        wheel = hierarchical_timing_wheel_create(1000, pool);
        ASSERT_NE(wheel, nullptr);

        // Create database
        int error = 0;
        db = database_create(test_dir.c_str(), 10, 1024*1024, 4, 4096, 0, 0, pool, wheel, &error);
        ASSERT_NE(db, nullptr);
        ASSERT_EQ(error, 0);
    }

    void TearDown() override {
        // Force cleanup of queued work items (triggers abort path)
        if (pool) {
            work_pool_destroy(pool);
        }

        // Destroy database
        if (db) {
            database_destroy(db);
        }

        // Destroy timing wheel
        if (wheel) {
            hierarchical_timing_wheel_destroy(wheel);
        }

        // Clean up test directory
        std::string cmd = "rm -rf " + test_dir;
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

    // Helper to create an identifier from a string
    identifier_t* make_identifier(const char* str) {
        buffer_t* buf = buffer_create_from_pointer_copy((uint8_t*)str, strlen(str));
        identifier_t* id = identifier_create(buf, 0);
        buffer_destroy(buf);
        return id;
    }

    std::string test_dir;
    work_pool_t* pool = nullptr;
    hierarchical_timing_wheel_t* wheel = nullptr;
    database_t* db = nullptr;
};

// Dummy callback for promise
static void dummy_callback(void* ctx, void* result) {
    // Do nothing
}

static void dummy_error_callback(void* ctx, async_error_t* error) {
    // Do nothing
}

// Test that queued put operations don't leak when pool is destroyed
TEST_F(AbortCleanupTest, QueuedPutNoLeak) {
    // Create path and value
    path_t* path = make_path({"users", "alice", "name"});
    identifier_t* value = make_identifier("Alice Smith");

    // Create promise
    promise_t* promise = promise_create(dummy_callback, dummy_error_callback, nullptr);

    // Queue put operation but don't wait for it
    // The work item will be queued but not executed
    database_put(db, path, value, promise);

    // Destroy pool immediately - triggers abort path
    // This should call abort_database_put which destroys path and value
    work_pool_destroy(pool);
    pool = nullptr;

    // Destroy promise (should already be resolved or aborted)
    promise_destroy(promise);

    // If there's a leak, valgrind/ASan will catch it
    // Test passes if no memory leaks are detected
}

// Test that queued get operations don't leak when pool is destroyed
TEST_F(AbortCleanupTest, QueuedGetNoLeak) {
    // Create path
    path_t* path = make_path({"users", "bob"});

    // Create promise
    promise_t* promise = promise_create(dummy_callback, dummy_error_callback, nullptr);

    // Queue get operation
    database_get(db, path, promise);

    // Destroy pool - triggers abort path
    work_pool_destroy(pool);
    pool = nullptr;

    // Destroy promise
    promise_destroy(promise);
}

// Test that queued delete operations don't leak when pool is destroyed
TEST_F(AbortCleanupTest, QueuedDeleteNoLeak) {
    // Create path
    path_t* path = make_path({"users", "charlie"});

    // Create promise
    promise_t* promise = promise_create(dummy_callback, dummy_error_callback, nullptr);

    // Queue delete operation
    database_delete(db, path, promise);

    // Destroy pool - triggers abort path
    work_pool_destroy(pool);
    pool = nullptr;

    // Destroy promise
    promise_destroy(promise);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}