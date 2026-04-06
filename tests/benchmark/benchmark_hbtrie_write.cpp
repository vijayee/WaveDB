//
// Focused HBTrie Write Benchmark for Profiling
//

#include <thread>
#include <vector>
#include <atomic>
#include <chrono>
#include <cstring>
#include <cstdio>
extern "C" {
#include "HBTrie/hbtrie.h"
#include "HBTrie/path.h"
#include "HBTrie/identifier.h"
#include "Buffer/buffer.h"
#include "Util/allocator.h"
}

// Helper to create a simple path from string
static path_t* make_simple_path(const char* key) {
    path_t* path = path_create();
    buffer_t* buf = buffer_create_from_pointer_copy((uint8_t*)key, strlen(key));
    identifier_t* id = identifier_create(buf, 0);
    buffer_destroy(buf);
    path_append(path, id);
    identifier_destroy(id);
    return path;
}

// Helper to create a simple value from string
static identifier_t* make_simple_value(const char* data) {
    buffer_t* buf = buffer_create_from_pointer_copy((uint8_t*)data, strlen(data));
    identifier_t* id = identifier_create(buf, 0);
    buffer_destroy(buf);
    return id;
}

int main(int argc, char** argv) {
    int thread_count = argc > 1 ? atoi(argv[1]) : 8;
    int ops_per_thread = argc > 2 ? atoi(argv[2]) : 10000;

    printf("HBTrie Write Benchmark\n");
    printf("Threads: %d, Ops per thread: %d\n", thread_count, ops_per_thread);

    // Create HBTrie
    hbtrie_t* trie = hbtrie_create(4, 4096);
    if (trie == NULL) {
        fprintf(stderr, "Failed to create HBTrie\n");
        return 1;
    }

    // Test single insert first
    {
        path_t* test_path = make_simple_path("test_key_0");
        identifier_t* test_value = make_simple_value("test_value");
        int test_result = hbtrie_insert(trie, test_path, test_value);
        printf("Test insert result: %d\n", test_result);
        if (test_result != 0) {
            printf("Test insert FAILED - debugging...\n");
            printf("  path_length: %zu\n", path_length(test_path));
        }
    }

    std::atomic<uint64_t> total_ops{0};
    std::atomic<uint64_t> total_errors{0};

    auto start = std::chrono::high_resolution_clock::now();

    std::vector<std::thread> threads;
    for (int t = 0; t < thread_count; t++) {
        threads.emplace_back([&, t]() {
            for (int i = 0; i < ops_per_thread; i++) {
                // Each thread writes to unique keys
                char key[64];
                snprintf(key, sizeof(key), "key_%d_%d", t, i);

                path_t* path = make_simple_path(key);
                identifier_t* value = make_simple_value("value_data_for_testing");

                int result = hbtrie_insert(trie, path, value);

                if (result == 0) {
                    total_ops++;
                } else {
                    total_errors++;
                }

                // Note: hbtrie_insert takes ownership of path and value
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);

    double ops_per_sec = (double)total_ops.load() / (duration.count() / 1000000.0);

    printf("\nResults:\n");
    printf("  Total operations: %lu\n", total_ops.load());
    printf("  Total errors: %lu\n", total_errors.load());
    printf("  Duration: %.2f ms\n", duration.count() / 1000.0);
    printf("  Throughput: %.0f ops/sec\n", ops_per_sec);
    if (total_ops.load() > 0) {
        printf("  Avg latency: %.0f ns\n", (double)duration.count() * 1000 / total_ops.load());
    }

    hbtrie_destroy(trie);

    return 0;
}