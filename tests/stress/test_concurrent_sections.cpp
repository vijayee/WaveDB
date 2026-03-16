//
// Stress test for concurrent section access
//

#include <gtest/gtest.h>
#include <thread>
#include <vector>
#include <atomic>
#include <random>
#include <chrono>
extern "C" {
#include "Storage/sections.h"
#include "Buffer/buffer.h"
#include "Time/wheel.h"
#include "Workers/pool.h"
#include "Workers/transaction_id.h"
#include "Util/path_join.h"
#include "Util/mkdir_p.h"
}

// Test configuration
#define NUM_THREADS 8
#define OPS_PER_THREAD 10000
#define SECTION_SIZE (1024 * 1024)  // 1MB sections
#define CACHE_SIZE 100
#define SECTION_CONCURRENCY 10

class ConcurrentSectionsTest : public ::testing::Test {
protected:
    sections_t* sections;
    hierarchical_timing_wheel_t* wheel;
    work_pool_t* pool;
    char* test_dir;

    void SetUp() override {
        // Initialize transaction ID generator
        transaction_id_init();

        // Create work pool
        pool = work_pool_create(8);
        work_pool_launch(pool);

        // Create timing wheel
        wheel = hierarchical_timing_wheel_create(8, pool);
        ASSERT_NE(wheel, nullptr);
        hierarchical_timing_wheel_run(wheel);

        // Create test directory
        test_dir = strdup("/tmp/wavedb_stress_test_XXXXXX");
        mkdtemp(test_dir);

        // Create subdirectories
        char* data_dir = path_join(test_dir, "data");
        char* meta_dir = path_join(test_dir, "meta");
        mkdir_p(data_dir);
        mkdir_p(meta_dir);
        free(data_dir);
        free(meta_dir);

        // Create sections pool
        sections = sections_create(test_dir, SECTION_SIZE, CACHE_SIZE,
                                  SECTION_CONCURRENCY, wheel, 100, 1000);
        ASSERT_NE(sections, nullptr);
    }

    void TearDown() override {
        sections_destroy(sections);
        hierarchical_timing_wheel_destroy(wheel);
        work_pool_destroy(pool);

        // Cleanup test directory
        char cmd[512];
        snprintf(cmd, sizeof(cmd), "rm -rf %s", test_dir);
        system(cmd);
        free(test_dir);
    }

    buffer_t* generate_random_data(size_t min_size, size_t max_size) {
        static thread_local std::mt19937 rng(std::random_device{}());
        std::uniform_int_distribution<size_t> size_dist(min_size, max_size);
        std::uniform_int_distribution<uint8_t> byte_dist(0, 255);

        size_t size = size_dist(rng);
        uint8_t* data = (uint8_t*)malloc(size);
        for (size_t i = 0; i < size; i++) {
            data[i] = byte_dist(rng);
        }

        return buffer_create_from_existing_memory(data, size);
    }
};

// Test 1: Concurrent writes (8 threads, 10K ops each)
TEST_F(ConcurrentSectionsTest, ConcurrentWrites) {
    std::vector<std::thread> threads;
    std::atomic<uint64_t> total_ops{0};
    std::atomic<uint64_t> total_errors{0};
    std::atomic<uint64_t> total_bytes{0};

    auto start = std::chrono::high_resolution_clock::now();

    // Launch writer threads
    for (int t = 0; t < NUM_THREADS; t++) {
        threads.emplace_back([&, t]() {
            for (int i = 0; i < OPS_PER_THREAD; i++) {
                // Generate random data
                buffer_t* data = generate_random_data(64, 1024);

                // Get transaction ID
                transaction_id_t txn_id = transaction_id_get_next();

                // Write to section
                size_t section_id, offset;
                int result = sections_write(sections, txn_id, data, &section_id, &offset);

                if (result == 0) {
                    total_ops++;
                    total_bytes += data->size;
                } else {
                    total_errors++;
                }

                buffer_destroy(data);
            }
        });
    }

    // Wait for all threads
    for (auto& thread : threads) {
        thread.join();
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    // Print statistics
    printf("\n=== Concurrent Writes Test ===\n");
    printf("Threads: %d\n", NUM_THREADS);
    printf("Ops per thread: %d\n", OPS_PER_THREAD);
    printf("Total ops: %lu\n", total_ops.load());
    printf("Total errors: %lu\n", total_errors.load());
    printf("Total bytes: %lu\n", total_bytes.load());
    printf("Duration: %ld ms\n", duration.count());
    printf("Throughput: %.2f ops/sec\n",
           (double)total_ops.load() / (duration.count() / 1000.0));
    printf("Bandwidth: %.2f MB/sec\n",
           (double)total_bytes.load() / (1024 * 1024) / (duration.count() / 1000.0));

    // Verify all operations succeeded
    EXPECT_EQ(total_errors.load(), 0);
    EXPECT_EQ(total_ops.load(), (uint64_t)NUM_THREADS * OPS_PER_THREAD);
}

// Test 2: Concurrent checkout/checkin
TEST_F(ConcurrentSectionsTest, ConcurrentCheckoutCheckin) {
    std::vector<std::thread> threads;
    std::atomic<uint64_t> checkout_ops{0};
    std::atomic<uint64_t> checkin_ops{0};
    std::atomic<uint64_t> errors{0};

    // Create some sections
    std::vector<size_t> section_ids;
    for (int i = 0; i < 50; i++) {
        buffer_t* data = generate_random_data(64, 512);
        transaction_id_t txn_id = transaction_id_get_next();
        size_t section_id, offset;
        int result = sections_write(sections, txn_id, data, &section_id, &offset);
        if (result == 0) {
            section_ids.push_back(section_id);
        }
        buffer_destroy(data);
    }

    auto start = std::chrono::high_resolution_clock::now();

    // Launch threads that repeatedly checkout/checkin sections
    for (int t = 0; t < NUM_THREADS; t++) {
        threads.emplace_back([&, t]() {
            static thread_local std::mt19937 rng(std::random_device{}());
            std::uniform_int_distribution<size_t> id_dist(0, section_ids.size() - 1);

            for (int i = 0; i < OPS_PER_THREAD; i++) {
                size_t section_id = section_ids[id_dist(rng)];

                // Checkout section
                section_t* section = sections_checkout(sections, section_id);
                if (section != nullptr) {
                    checkout_ops++;

                    // Simulate some work with the section
                    usleep(1);  // 1 microsecond

                    // Checkin section
                    sections_checkin(sections, section);
                    checkin_ops++;
                } else {
                    errors++;
                }
            }
        });
    }

    // Wait for all threads
    for (auto& thread : threads) {
        thread.join();
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    // Print statistics
    printf("\n=== Concurrent Checkout/Checkin Test ===\n");
    printf("Threads: %d\n", NUM_THREADS);
    printf("Ops per thread: %d\n", OPS_PER_THREAD);
    printf("Checkout ops: %lu\n", checkout_ops.load());
    printf("Checkin ops: %lu\n", checkin_ops.load());
    printf("Errors: %lu\n", errors.load());
    printf("Duration: %ld ms\n", duration.count());
    printf("Throughput: %.2f ops/sec\n",
           (double)checkout_ops.load() / (duration.count() / 1000.0));

    EXPECT_EQ(errors.load(), 0);
    EXPECT_EQ(checkout_ops.load(), checkin_ops.load());
}

// Test 3: Measure lock contention under various loads
TEST_F(ConcurrentSectionsTest, LockContentionScaling) {
    printf("\n=== Lock Contention Scaling Test ===\n");

    // Test with increasing thread counts
    for (int thread_count : {1, 2, 4, 8, 16}) {
        std::vector<std::thread> threads;
        std::atomic<uint64_t> total_ops{0};
        std::atomic<uint64_t> total_errors{0};

        // Pre-create some sections
        std::vector<size_t> section_ids;
        for (int i = 0; i < 100; i++) {
            buffer_t* data = generate_random_data(64, 512);
            transaction_id_t txn_id = transaction_id_get_next();
            size_t section_id, offset;
            int result = sections_write(sections, txn_id, data, &section_id, &offset);
            if (result == 0) {
                section_ids.push_back(section_id);
            }
            buffer_destroy(data);
        }

        auto start = std::chrono::high_resolution_clock::now();

        // Launch threads
        for (int t = 0; t < thread_count; t++) {
            threads.emplace_back([&, t]() {
                static thread_local std::mt19937 rng(std::random_device{}());
                std::uniform_int_distribution<size_t> id_dist(0, section_ids.size() - 1);

                for (int i = 0; i < 5000; i++) {
                    size_t section_id = section_ids[id_dist(rng)];
                    section_t* section = sections_checkout(sections, section_id);

                    if (section != nullptr) {
                        // Do minimal work
                        sections_checkin(sections, section);
                        total_ops++;
                    } else {
                        total_errors++;
                    }
                }
            });
        }

        // Wait for all threads
        for (auto& thread : threads) {
            thread.join();
        }

        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

        double throughput = (double)total_ops.load() / (duration.count() / 1000.0);

        printf("Threads: %2d | Ops: %lu | Errors: %lu | Time: %4ld ms | Throughput: %8.0f ops/sec\n",
               thread_count, total_ops.load(), total_errors.load(),
               duration.count(), throughput);

        EXPECT_EQ(total_errors.load(), 0);
    }
}

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}