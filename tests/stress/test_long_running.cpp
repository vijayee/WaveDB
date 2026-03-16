//
// Long-running stability tests
//

#include <gtest/gtest.h>
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>
#include <cstring>
#include <algorithm>
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
#define SUSTAINED_OPS 100000

class LongRunningTest : public ::testing::Test {
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
        test_dir = strdup("/tmp/wavedb_longrun_test_XXXXXX");
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
        memset(data, 0xCD, size);
        return buffer_create_from_existing_memory(data, size);
    }
};

TEST_F(LongRunningTest, SustainedOperations) {
    printf("\n=== Sustained Operations Test ===\n");
    printf("Target: %d operations\n", SUSTAINED_OPS);

    sections_t* sections = sections_create(test_dir, SECTION_SIZE, CACHE_SIZE,
                                         SECTION_CONCURRENCY, wheel, 100, 1000);
    ASSERT_NE(sections, nullptr);

    std::atomic<uint64_t> total_ops{0};
    std::atomic<uint64_t> total_errors{0};
    std::atomic<uint64_t> total_bytes{0};

    auto start = std::chrono::high_resolution_clock::now();
    auto last_report = start;

    for (int i = 0; i < SUSTAINED_OPS; i++) {
        size_t data_size = 64 + (i % 1024);
        buffer_t* data = generate_data(data_size);
        transaction_id_t txn_id = transaction_id_get_next();
        size_t section_id, offset;

        int result = sections_write(sections, txn_id, data, &section_id, &offset);
        if (result == 0) {
            total_ops++;
            total_bytes += data_size;
        } else {
            total_errors++;
        }

        buffer_destroy(data);

        if (i % 10000 == 0 && i > 0) {
            auto now = std::chrono::high_resolution_clock::now();
            auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_report).count();
            double throughput = 10000.0 / (elapsed_ms / 1000.0);
            printf("Progress: %d/%d ops (%.1f%%) | Throughput: %.0f ops/sec\n",
                   i, SUSTAINED_OPS, 100.0 * i / SUSTAINED_OPS, throughput);
            last_report = now;
        }
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    printf("\nFinal Results:\n");
    printf("Total ops: %lu\n", total_ops.load());
    printf("Total errors: %lu\n", total_errors.load());
    printf("Total bytes: %lu (%.2f MB)\n", total_bytes.load(),
           (double)total_bytes.load() / (1024 * 1024));
    printf("Duration: %ld ms\n", duration.count());
    printf("Throughput: %.2f ops/sec\n",
           (double)total_ops.load() / (duration.count() / 1000.0));

    sections_destroy(sections);

    EXPECT_EQ(total_errors.load(), 0);
    EXPECT_EQ(total_ops.load(), (uint64_t)SUSTAINED_OPS);
}

TEST_F(LongRunningTest, ConcurrentSustainedOperations) {
    printf("\n=== Concurrent Sustained Operations Test ===\n");

    sections_t* sections = sections_create(test_dir, SECTION_SIZE, CACHE_SIZE,
                                         SECTION_CONCURRENCY, wheel, 100, 1000);
    ASSERT_NE(sections, nullptr);

    const int NUM_THREADS = 4;
    const int OPS_PER_THREAD = SUSTAINED_OPS / NUM_THREADS;

    std::atomic<uint64_t> total_ops{0};
    std::atomic<uint64_t> total_errors{0};

    auto start = std::chrono::high_resolution_clock::now();

    std::vector<std::thread> threads;
    for (int t = 0; t < NUM_THREADS; t++) {
        threads.emplace_back([&, t]() {
            for (int i = 0; i < OPS_PER_THREAD; i++) {
                size_t data_size = 64 + (i % 512);
                buffer_t* data = generate_data(data_size);
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

    printf("Threads: %d\n", NUM_THREADS);
    printf("Ops per thread: %d\n", OPS_PER_THREAD);
    printf("Total ops: %lu\n", total_ops.load());
    printf("Total errors: %lu\n", total_errors.load());
    printf("Duration: %ld ms\n", duration.count());
    printf("Throughput: %.2f ops/sec\n",
           (double)total_ops.load() / (duration.count() / 1000.0));

    sections_destroy(sections);

    EXPECT_EQ(total_errors.load(), 0);
    EXPECT_EQ(total_ops.load(), (uint64_t)SUSTAINED_OPS);
}

TEST_F(LongRunningTest, PerformanceDegradation) {
    printf("\n=== Performance Degradation Detection Test ===\n");

    sections_t* sections = sections_create(test_dir, SECTION_SIZE, CACHE_SIZE,
                                         SECTION_CONCURRENCY, wheel, 100, 1000);
    ASSERT_NE(sections, nullptr);

    const int BATCH_SIZE = 1000;
    const int NUM_BATCHES = 10;
    std::vector<double> batch_times;

    for (int batch = 0; batch < NUM_BATCHES; batch++) {
        auto start = std::chrono::high_resolution_clock::now();

        for (int i = 0; i < BATCH_SIZE; i++) {
            buffer_t* data = generate_data(64);
            transaction_id_t txn_id = transaction_id_get_next();
            size_t section_id, offset;

            sections_write(sections, txn_id, data, &section_id, &offset);
            buffer_destroy(data);
        }

        auto end = std::chrono::high_resolution_clock::now();
        auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
        batch_times.push_back((double)BATCH_SIZE / (duration_ms / 1000.0));

        printf("Batch %2d: %.0f ops/sec\n", batch + 1, batch_times.back());
    }

    double first_half_avg = 0, second_half_avg = 0;
    for (int i = 0; i < NUM_BATCHES / 2; i++) {
        first_half_avg += batch_times[i];
        second_half_avg += batch_times[i + NUM_BATCHES / 2];
    }
    first_half_avg /= NUM_BATCHES / 2;
    second_half_avg /= NUM_BATCHES / 2;

    double degradation = (first_half_avg - second_half_avg) / first_half_avg * 100.0;

    printf("\nFirst half avg: %.0f ops/sec\n", first_half_avg);
    printf("Second half avg: %.0f ops/sec\n", second_half_avg);
    printf("Performance change: %.1f%%\n", -degradation);

    sections_destroy(sections);

    // Allow up to 20% degradation
    EXPECT_LT(degradation, 20.0);
}

TEST_F(LongRunningTest, TransactionIDUniqueness) {
    printf("\n=== Transaction ID Uniqueness Test ===\n");

    const int NUM_THREADS = 8;
    const int OPS_PER_THREAD = 10000;
    std::vector<std::vector<transaction_id_t>> thread_ids(NUM_THREADS);

    auto start = std::chrono::high_resolution_clock::now();

    std::vector<std::thread> threads;
    for (int t = 0; t < NUM_THREADS; t++) {
        threads.emplace_back([&, t]() {
            for (int i = 0; i < OPS_PER_THREAD; i++) {
                transaction_id_t id = transaction_id_get_next();
                thread_ids[t].push_back(id);
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    // Collect all IDs
    std::vector<transaction_id_t> all_ids;
    for (int t = 0; t < NUM_THREADS; t++) {
        for (const auto& id : thread_ids[t]) {
            all_ids.push_back(id);
        }
    }

    printf("Total IDs generated: %zu\n", all_ids.size());
    printf("Duration: %ld ms\n", duration.count());
    printf("Throughput: %.0f IDs/sec\n",
           (double)all_ids.size() / (duration.count() / 1000.0));

    // Check uniqueness
    printf("Verifying uniqueness...\n");
    std::sort(all_ids.begin(), all_ids.end(), [](const transaction_id_t& a, const transaction_id_t& b) {
        return transaction_id_compare(&a, &b) < 0;
    });

    int duplicates = 0;
    for (size_t i = 1; i < all_ids.size(); i++) {
        if (transaction_id_compare(&all_ids[i], &all_ids[i-1]) == 0) {
            duplicates++;
        }
    }

    printf("Duplicates found: %d\n", duplicates);

    EXPECT_EQ(all_ids.size(), (size_t)NUM_THREADS * OPS_PER_THREAD);
    EXPECT_EQ(duplicates, 0);
}

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
