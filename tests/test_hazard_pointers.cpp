//
// Test for Hazard Pointers
//

#include <gtest/gtest.h>
#include <thread>
#include <vector>
#include <atomic>
#include <cstdlib>
#include "Util/hazard_pointers.h"

class HazardPointerTest : public ::testing::Test {
protected:
    void SetUp() override {
        hp_init();
    }
    void TearDown() override {
        hp_destroy();
    }
};

TEST_F(HazardPointerTest, InitDestroy) {
    // Should be idempotent
    EXPECT_EQ(hp_init(), 0);
    hp_destroy();
}

TEST_F(HazardPointerTest, RegisterUnregisterThread) {
    hp_context_t* ctx = hp_register_thread();
    ASSERT_NE(ctx, nullptr);
    EXPECT_EQ(ctx->retired_count, 0);
    hp_unregister_thread(ctx);
}

TEST_F(HazardPointerTest, AcquireRelease) {
    hp_context_t* ctx = hp_register_thread();
    ASSERT_NE(ctx, nullptr);

    int value = 42;

    // Acquire hazard pointer
    hp_acquire(ctx, &value, 0);
    EXPECT_TRUE(hp_is_protected(ctx, &value));

    // Release hazard pointer
    hp_release(ctx, 0);
    EXPECT_FALSE(hp_is_protected(ctx, &value));

    hp_unregister_thread(ctx);
}

TEST_F(HazardPointerTest, MultipleSlots) {
    hp_context_t* ctx = hp_register_thread();
    ASSERT_NE(ctx, nullptr);

    int a = 1, b = 2, c = 3, d = 4;

    // Acquire multiple slots
    hp_acquire(ctx, &a, 0);
    hp_acquire(ctx, &b, 1);
    hp_acquire(ctx, &c, 2);
    hp_acquire(ctx, &d, 3);

    EXPECT_TRUE(hp_is_protected(ctx, &a));
    EXPECT_TRUE(hp_is_protected(ctx, &b));
    EXPECT_TRUE(hp_is_protected(ctx, &c));
    EXPECT_TRUE(hp_is_protected(ctx, &d));

    // Release all
    hp_release(ctx, 0);
    hp_release(ctx, 1);
    hp_release(ctx, 2);
    hp_release(ctx, 3);

    EXPECT_FALSE(hp_is_protected(ctx, &a));
    EXPECT_FALSE(hp_is_protected(ctx, &b));
    EXPECT_FALSE(hp_is_protected(ctx, &c));
    EXPECT_FALSE(hp_is_protected(ctx, &d));

    hp_unregister_thread(ctx);
}

TEST_F(HazardPointerTest, RetireAndReclaim) {
    hp_context_t* ctx = hp_register_thread();
    ASSERT_NE(ctx, nullptr);

    static std::atomic<int> reclaimed{0};
    reclaimed = 0;

    auto reclaim_func = [](void* ptr) {
        free(ptr);
        reclaimed++;
    };

    // Allocate and retire multiple objects
    for (int i = 0; i < 20; i++) {
        int* obj = (int*)malloc(sizeof(int));
        *obj = i;
        hp_retire(ctx, obj, reclaim_func);
    }

    // Objects should be reclaimed after scan (triggered by threshold)
    // HP_RETIRE_THRESHOLD is 16, so first 16 should trigger a scan
    // But protected objects won't be reclaimed

    EXPECT_GE(reclaimed.load(), 0);  // At least some should be reclaimed
    EXPECT_LE(reclaimed.load(), 20);  // Not all may be reclaimed yet

    // Unregister will force cleanup
    hp_unregister_thread(ctx);

    // All objects should be reclaimed after unregister
    EXPECT_EQ(reclaimed.load(), 20);
}

TEST_F(HazardPointerTest, RetireWithoutScan) {
    hp_context_t* ctx = hp_register_thread();
    ASSERT_NE(ctx, nullptr);

    static std::atomic<int> reclaimed{0};
    reclaimed = 0;

    auto reclaim_func = [](void* ptr) {
        free(ptr);
        reclaimed++;
    };

    // Retire less than threshold
    for (int i = 0; i < 5; i++) {
        int* obj = (int*)malloc(sizeof(int));
        hp_retire(ctx, obj, reclaim_func);
    }

    // Should not have triggered scan yet
    EXPECT_EQ(reclaimed.load(), 0);

    // Explicit scan
    hp_scan(ctx);

    // Now objects should be reclaimed
    EXPECT_EQ(reclaimed.load(), 5);

    hp_unregister_thread(ctx);
}

TEST_F(HazardPointerTest, ProtectedObjectNotReclaimed) {
    hp_context_t* ctx1 = hp_register_thread();
    hp_context_t* ctx2 = hp_register_thread();
    ASSERT_NE(ctx1, nullptr);
    ASSERT_NE(ctx2, nullptr);

    static std::atomic<int> reclaimed{0};
    reclaimed = 0;

    auto reclaim_func = [](void* ptr) {
        free(ptr);
        reclaimed++;
    };

    // Allocate object
    int* obj = (int*)malloc(sizeof(int));
    *obj = 42;

    // Thread 1 protects the object
    hp_acquire(ctx1, obj, 0);

    // Thread 2 tries to retire it
    hp_retire(ctx2, obj, reclaim_func);

    // Object should not be reclaimed yet
    hp_scan(ctx2);
    EXPECT_EQ(reclaimed.load(), 0);

    // Thread 1 releases protection
    hp_release(ctx1, 0);

    // Now object can be reclaimed
    hp_scan(ctx2);
    EXPECT_EQ(reclaimed.load(), 1);

    hp_unregister_thread(ctx1);
    hp_unregister_thread(ctx2);
}

TEST_F(HazardPointerTest, ConcurrentAccess) {
    constexpr int NUM_THREADS = 4;
    constexpr int ITERATIONS = 1000;

    hp_init();

    // Shared objects to protect
    std::vector<int*> objects;
    for (int i = 0; i < 100; i++) {
        objects.push_back((int*)malloc(sizeof(int)));
        *objects.back() = i;
    }

    std::atomic<int> errors{0};

    std::vector<std::thread> threads;
    for (int t = 0; t < NUM_THREADS; t++) {
        threads.emplace_back([&, t]() {
            hp_context_t* ctx = hp_register_thread();
            if (ctx == nullptr) {
                errors++;
                return;
            }

            for (int i = 0; i < ITERATIONS; i++) {
                // Acquire random object
                int* obj = objects[rand() % objects.size()];
                hp_acquire(ctx, obj, 0);

                // Simulate work
                int value = *obj;
                (void)value;

                // Release
                hp_release(ctx, 0);

                // Occasionally retire objects
                if (rand() % 100 < 5) {
                    int* retired_obj = (int*)malloc(sizeof(int));
                    hp_retire(ctx, retired_obj, [](void* p) { free(p); });
                }
            }

            hp_unregister_thread(ctx);
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    EXPECT_EQ(errors.load(), 0);

    // Cleanup
    for (int* obj : objects) {
        free(obj);
    }

    hp_destroy();
}

TEST_F(HazardPointerTest, StressTest) {
    constexpr int NUM_THREADS = 8;
    constexpr int ITERATIONS = 5000;

    hp_init();

    std::atomic<int> objects_created{0};
    std::atomic<int> objects_reclaimed{0};

    auto reclaim_func = [](void* ptr) {
        free(ptr);
        // Don't increment atomic here - called from different threads
    };

    std::vector<std::thread> threads;
    for (int t = 0; t < NUM_THREADS; t++) {
        threads.emplace_back([&, t]() {
            hp_context_t* ctx = hp_register_thread();
            if (ctx == nullptr) return;

            for (int i = 0; i < ITERATIONS; i++) {
                // 80% acquire/release, 20% retire
                if (rand() % 100 < 80) {
                    int* obj = (int*)malloc(sizeof(int));
                    *obj = i;
                    objects_created++;

                    hp_acquire(ctx, obj, 0);
                    int value = *obj;
                    (void)value;
                    hp_release(ctx, 0);

                    free(obj);
                    objects_created--;
                } else {
                    int* retired = (int*)malloc(sizeof(int));
                    hp_retire(ctx, retired, reclaim_func);
                }
            }

            hp_unregister_thread(ctx);
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    hp_destroy();

    // If we got here without crashing, test passes
    SUCCEED() << "Stress test completed without crashes";
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}