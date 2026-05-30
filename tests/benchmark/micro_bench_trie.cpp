//
// Micro-benchmark: hbtrie_insert vs hbtrie_insert_unsafe
// Measures ONLY trie insertion without WAL or LRU overhead
//
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>

extern "C" {
#include "HBTrie/hbtrie.h"
#include "HBTrie/path.h"
#include "HBTrie/identifier.h"
#include "Buffer/buffer.h"
#include "Workers/transaction_id.h"
}

static path_t* make_path(const char* key) {
    path_t* path = path_create();
    buffer_t* buf = buffer_create_from_pointer_copy((uint8_t*)key, strlen(key));
    identifier_t* id = identifier_create(buf, 0);
    buffer_destroy(buf);
    path_append(path, id);
    identifier_destroy(id);
    return path;
}

static identifier_t* make_value(const char* val) {
    buffer_t* buf = buffer_create_from_pointer_copy((uint8_t*)val, strlen(val));
    identifier_t* id = identifier_create(buf, 0);
    buffer_destroy(buf);
    return id;
}

static uint64_t now_ns() {
    auto now = std::chrono::high_resolution_clock::now();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        now.time_since_epoch()).count();
}

int main() {
    const int WARMUP = 1000;
    const int ITERATIONS = 50000;

    printf("=== hbtrie_insert vs hbtrie_insert_unsafe micro-benchmark ===\n");
    printf("Warmup: %d, Iterations: %d\n\n", WARMUP, ITERATIONS);

    // --- hbtrie_insert (concurrent path with spinlocks + MVCC) ---
    {
        hbtrie_t* trie = hbtrie_create(4, 4096);
        transaction_id_t txn_id = {0, 0, 1};

        // Warmup
        for (int i = 0; i < WARMUP; i++) {
            char key[64];
            snprintf(key, sizeof(key), "warmup_insert_%d", i);
            path_t* p = make_path(key);
            identifier_t* v = make_value("value");
            hbtrie_insert(trie, p, v, txn_id);
            path_destroy(p);
            identifier_destroy(v);
            txn_id.count++;
        }

        // Benchmark
        uint64_t start = now_ns();
        for (int i = 0; i < ITERATIONS; i++) {
            char key[64];
            snprintf(key, sizeof(key), "bench_insert_%d", i);
            path_t* p = make_path(key);
            identifier_t* v = make_value("value");
            hbtrie_insert(trie, p, v, txn_id);
            path_destroy(p);
            identifier_destroy(v);
            txn_id.count++;
        }
        uint64_t elapsed = now_ns() - start;

        printf("hbtrie_insert:       %8.2f ns/op  (%8.0f ops/sec)\n",
               (double)elapsed / ITERATIONS,
               ITERATIONS / (elapsed / 1e9));

        hbtrie_destroy(trie);
    }

    // --- hbtrie_insert_unsafe (sync_only path, no locks/MVCC) ---
    {
        hbtrie_t* trie = hbtrie_create(4, 4096);
        transaction_id_t txn_id = {0, 0, 1};

        // Warmup
        for (int i = 0; i < WARMUP; i++) {
            char key[64];
            snprintf(key, sizeof(key), "warmup_unsafe_%d", i);
            path_t* p = make_path(key);
            identifier_t* v = make_value("value");
            hbtrie_insert_unsafe(trie, p, v, txn_id);
            path_destroy(p);
            identifier_destroy(v);
            txn_id.count++;
        }

        // Benchmark
        uint64_t start = now_ns();
        for (int i = 0; i < ITERATIONS; i++) {
            char key[64];
            snprintf(key, sizeof(key), "bench_unsafe_%d", i);
            path_t* p = make_path(key);
            identifier_t* v = make_value("value");
            hbtrie_insert_unsafe(trie, p, v, txn_id);
            path_destroy(p);
            identifier_destroy(v);
            txn_id.count++;
        }
        uint64_t elapsed = now_ns() - start;

        printf("hbtrie_insert_unsafe: %8.2f ns/op  (%8.0f ops/sec)\n",
               (double)elapsed / ITERATIONS,
               ITERATIONS / (elapsed / 1e9));

        hbtrie_destroy(trie);
    }

    // --- hbtrie_find vs hbtrie_find_unsafe ---
    printf("\n--- Get path comparison ---\n\n");

    // Pre-populate for get test
    {
        hbtrie_t* trie = hbtrie_create(4, 4096);
        transaction_id_t txn_id = {0, 0, 1};
        for (int i = 0; i < ITERATIONS; i++) {
            char key[64];
            snprintf(key, sizeof(key), "bench_get_%d", i);
            path_t* p = make_path(key);
            identifier_t* v = make_value("value");
            hbtrie_insert(trie, p, v, txn_id);
            path_destroy(p);
            identifier_destroy(v);
            txn_id.count++;
        }

        // Benchmark hbtrie_find
        uint64_t start = now_ns();
        for (int i = 0; i < ITERATIONS; i++) {
            char key[64];
            snprintf(key, sizeof(key), "bench_get_%d", i);
            path_t* p = make_path(key);
            identifier_t* result = NULL;
            transaction_id_t read_id = {0, 0, (uint32_t)(i + 100000)};
            result = hbtrie_find(trie, p, read_id);
            if (result) identifier_destroy(result);
            path_destroy(p);
        }
        uint64_t elapsed = now_ns() - start;

        printf("hbtrie_find:          %8.2f ns/op  (%8.0f ops/sec)\n",
               (double)elapsed / ITERATIONS,
               ITERATIONS / (elapsed / 1e9));

        hbtrie_destroy(trie);
    }

    // Pre-populate with unsafe for unsafe get test
    {
        hbtrie_t* trie = hbtrie_create(4, 4096);
        transaction_id_t txn_id = {0, 0, 1};
        for (int i = 0; i < ITERATIONS; i++) {
            char key[64];
            snprintf(key, sizeof(key), "bench_getu_%d", i);
            path_t* p = make_path(key);
            identifier_t* v = make_value("value");
            hbtrie_insert_unsafe(trie, p, v, txn_id);
            path_destroy(p);
            identifier_destroy(v);
            txn_id.count++;
        }

        // Benchmark hbtrie_find_unsafe
        uint64_t start = now_ns();
        for (int i = 0; i < ITERATIONS; i++) {
            char key[64];
            snprintf(key, sizeof(key), "bench_getu_%d", i);
            path_t* p = make_path(key);
            identifier_t* result = hbtrie_find_unsafe(trie, p);
            if (result) identifier_destroy(result);
            path_destroy(p);
        }
        uint64_t elapsed = now_ns() - start;

        printf("hbtrie_find_unsafe:   %8.2f ns/op  (%8.0f ops/sec)\n",
               (double)elapsed / ITERATIONS,
               ITERATIONS / (elapsed / 1e9));

        hbtrie_destroy(trie);
    }

    return 0;
}
