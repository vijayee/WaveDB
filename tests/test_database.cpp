//
// Created by victor on 3/11/26.
//

#include <gtest/gtest.h>
#include "Database/database.h"
#include "Time/wheel.h"
#include "Workers/pool.h"

class DatabaseTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Create test directory
        test_dir = "/tmp/wavedb_test_" + std::to_string(getpid());
        mkdir(test_dir.c_str(), 0700);

        // Create work pool
        pool = work_pool_create(4);
        work_pool_launch(pool);

        // Create timing wheel
        wheel = hierarchical_timing_wheel_create(100, pool);
        hierarchical_timing_wheel_run(wheel);
    }

    void TearDown() override {
        if (db) {
            database_destroy(db);
            db = nullptr;
        }
        if (wheel) {
            hierarchical_timing_wheel_stop(wheel);
            hierarchical_timing_wheel_destroy(wheel);
            wheel = nullptr;
        }
        if (pool) {
            work_pool_shutdown(pool);
            work_pool_join_all(pool);
            work_pool_destroy(pool);
            pool = nullptr;
        }

        // Cleanup test directory
        std::string cmd = std::string("rm -rf ") + test_dir;
        system(cmd.c_str());
    }

    database_t* db = nullptr;
    work_pool_t* pool = nullptr;
    hierarchical_timing_wheel_t* wheel = nullptr;
    std::string test_dir;
};

TEST_F(DatabaseTest, CreateDestroy) {
    int error = 0;
    db = database_create(test_dir.c_str(), 0, 0, 0, 0, pool, wheel, &error);
    ASSERT_NE(db, nullptr);
    EXPECT_EQ(error, 0);
}

// TODO: Add more tests for put/get/delete operations


