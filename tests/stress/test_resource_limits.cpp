//
// Stress test for resource limits and exhaustion
//

#include <gtest/gtest.h>
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>
extern "C" {
#include "Storage/sections.h"
#include "Database/wal.h"
#include "Buffer/buffer.h"
#include "Time/wheel.h"
#include "Workers/pool.h"
#include "Workers/transaction_id.h"
#include "Util/path_join.h"
#include "Util/mkdir_p.h"
}

#define SECTION_SIZE (1024 * 1024)
#define CACHE_SIZE 100
#define SECTION_CONCURRENCY 10

class ResourceLimitsTest : public ::testing::Test {
protected:
    hierarchical_timing_wheel_t* wheel;
    work_pool_t* pool;
    char* test_dir;

    void SetUp() override {
        transaction_id_init();
        pool = work_pool_create(8);
        work_pool_launch(pool);
        wheel = hierarchical_timing_wheel_create(8, pool);
        hierarchical_timing_wheel_run(wheel);
        test_dir = strdup("/tmp/wavedb_resource_test_XXXXXX");
        mkdtemp(test_dir);
    }

    void TearDown() override {
        hierarchical_timing_wheel_destroy(wheel);
        work_pool_destroy(pool);
        char cmd[512];
        snprintf(cmd, sizeof(cmd), "rm -rf %s", test_dir);
        system(cmd);
        free(test_dir);
    }

    buffer_t* generate_data(size_t size) {
        uint8_t* data = (uint8_t*)malloc(size);
        memset(data, 0xAB, size);
        return buffer_create_from_existing_memory(data, size);
    }
};

TEST_F(ResourceLimitsTest, MaximumOpenSections) {
    printf("\n=== Maximum Open Sections Test ===\n");

    sections_t* sections = sections_create(test_dir, SECTION_SIZE, CACHE_SIZE,
                                         SECTION_CONCURRENCY, wheel, 100, 1000);
    ASSERT_NE(sections, nullptr);

    std::vector<size_t> section_ids;
    std::vector<section_t*> checked_out_sections;
    int max_sections = 0;

    for (int i = 0; i < 1000; i++) {
        buffer_t* data = generate_data(1024);
        transaction_id_t txn_id = transaction_id_get_next();
        size_t section_id, offset;

        int result = sections_write(sections, txn_id, data, &section_id, &offset);
        buffer_destroy(data);

        if (result != 0) {
            printf("Write failed after %d sections\n", max_sections);
            break;
        }

        section_t* section = sections_checkout(sections, section_id);
        if (section != nullptr) {
            checked_out_sections.push_back(section);
            section_ids.push_back(section_id);
            max_sections++;
        }

        if (max_sections >= SECTION_CONCURRENCY * 2) break;
    }

    printf("Successfully opened %d sections\n", max_sections);

    for (size_t i = 0; i < checked_out_sections.size(); i++) {
        sections_checkin(sections, checked_out_sections[i]);
    }

    sections_destroy(sections);
    EXPECT_GT(max_sections, 0);
}

TEST_F(ResourceLimitsTest, WALRotationUnderLoad) {
    printf("\n=== WAL Rotation Under Load Test ===\n");

    char* wal_path = path_join(test_dir, "wal");
    int error_code;
    wal_t* wal = wal_create(wal_path, 1024 * 1024, &error_code);
    ASSERT_NE(wal, nullptr);

    const int NUM_WRITERS = 4;
    const int OPS_PER_WRITER = 500;
    std::atomic<uint64_t> total_ops{0};
    std::atomic<uint64_t> total_bytes{0};

    auto start = std::chrono::high_resolution_clock::now();

    std::vector<std::thread> threads;
    for (int t = 0; t < NUM_WRITERS; t++) {
        threads.emplace_back([&, t]() {
            for (int i = 0; i < OPS_PER_WRITER; i++) {
                size_t data_size = 64 + (i % 1024);
                buffer_t* data = generate_data(data_size);
                transaction_id_t txn_id = transaction_id_get_next();

                int result = wal_write(wal, txn_id, WAL_PUT, data);
                if (result == 0) {
                    total_ops++;
                    total_bytes += data_size;
                }

                buffer_destroy(data);
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    printf("Total ops: %lu\n", total_ops.load());
    printf("Total bytes: %lu (%.2f MB)\n", total_bytes.load(),
           (double)total_bytes.load() / (1024 * 1024));
    printf("Duration: %ld ms\n", duration.count());
    printf("Throughput: %.2f ops/sec\n",
           (double)total_ops.load() / (duration.count() / 1000.0));

    wal_destroy(wal);
    free(wal_path);

    EXPECT_EQ(total_ops.load(), (uint64_t)NUM_WRITERS * OPS_PER_WRITER);
}

TEST_F(ResourceLimitsTest, MemoryPressureSmallSections) {
    printf("\n=== Memory Pressure (Small Sections) Test ===\n");

    sections_t* sections = sections_create(test_dir, SECTION_SIZE, CACHE_SIZE,
                                         SECTION_CONCURRENCY, wheel, 100, 1000);
    ASSERT_NE(sections, nullptr);

    const int NUM_WRITERS = 8;
    const int OPS_PER_WRITER = 1000;
    std::atomic<uint64_t> total_ops{0};
    std::atomic<uint64_t> total_errors{0};

    auto start = std::chrono::high_resolution_clock::now();

    std::vector<std::thread> threads;
    for (int t = 0; t < NUM_WRITERS; t++) {
        threads.emplace_back([&, t]() {
            for (int i = 0; i < OPS_PER_WRITER; i++) {
                buffer_t* data = generate_data(32 + (i % 64));
                transaction_id_t txn_id = transaction_id_get_next();
                size_t section_id, offset;

                int result = sections_write(sections, txn_id, data, &section_id, &offset);
                if (result == 0) {
                    total_ops++;
                } else {
                    total_errors++;
                }

                buffer_destroy(data);
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    printf("Total ops: %lu\n", total_ops.load());
    printf("Total errors: %lu\n", total_errors.load());
    printf("Duration: %ld ms\n", duration.count());
    printf("Throughput: %.2f ops/sec\n",
           (double)total_ops.load() / (duration.count() / 1000.0));

    sections_destroy(sections);

    EXPECT_EQ(total_errors.load(), 0);
}

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
