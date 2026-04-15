//
// Page File + Bnode Cache Performance Benchmarks
// Measures raw I/O throughput and cache hit rates
//

#include "benchmark_base.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <chrono>
#include <vector>

extern "C" {
#include "Storage/page_file.h"
#include "Storage/bnode_cache.h"
}

// ============================================================
// Helpers
// ============================================================

static double now_secs() {
    auto tp = std::chrono::high_resolution_clock::now();
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(tp.time_since_epoch()).count();
    return (double)ns / 1e9;
}

// Build a data buffer with 4-byte size prefix (matching bnode_cache convention)
static uint8_t* make_prefixed_data(const uint8_t* payload, size_t payload_len, size_t* out_total_len) {
    size_t total = 4 + payload_len;
    uint8_t* buf = (uint8_t*)calloc(1, total);
    uint32_t sz = (uint32_t)payload_len;
    memcpy(buf, &sz, 4);
    memcpy(buf + 4, payload, payload_len);
    *out_total_len = total;
    return buf;
}

// Create a temp directory for a benchmark run; caller must rm -rf it
static int make_temp_dir(char* buf, size_t bufsize, const char* prefix) {
    snprintf(buf, bufsize, "/tmp/%s_%d_XXXXXX", prefix, getpid());
    return mkdtemp(buf) ? 0 : -1;
}

// Remove a directory tree
static void remove_dir(const char* path) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", path);
    system(cmd);
}

// ============================================================
// Parameters
// ============================================================

static const size_t N_SEQUENTIAL      = 10000;
static const size_t NODE_SIZE          = 1024;      // 1 KB
static const size_t BLOCK_SIZE         = 4096;
static const size_t NUM_SUPERBLOCKS    = 2;
static const size_t NUM_SHARDS         = 7;
static const size_t CACHE_SMALL_MB     = 1;          // 1 MB for cache-hit benchmark
static const size_t CACHE_LARGE_MB     = 10;         // 10 MB for dirty-flush benchmark

// ============================================================
// Benchmark 1: Sequential Writes
// ============================================================

static void benchmark_sequential_writes() {
    printf("=== Sequential Writes ===\n");

    char tmpdir[256];
    if (make_temp_dir(tmpdir, sizeof(tmpdir), "page_cache_bench_writes") != 0) {
        fprintf(stderr, "Failed to create temp directory\n");
        return;
    }

    char filepath[512];
    snprintf(filepath, sizeof(filepath), "%s/writes.db", tmpdir);

    page_file_t* pf = page_file_create(filepath, BLOCK_SIZE, NUM_SUPERBLOCKS);
    if (!pf) { fprintf(stderr, "page_file_create failed\n"); remove_dir(tmpdir); return; }

    int rc = page_file_open(pf, 1);
    if (rc != 0) { fprintf(stderr, "page_file_open failed\n"); page_file_destroy(pf); remove_dir(tmpdir); return; }

    // Prepare node data (1 KB payload)
    size_t total_len = 0;
    uint8_t* data = make_prefixed_data((const uint8_t*)"X", 1, &total_len);
    // Expand to ~1KB total (including 4-byte prefix)
    size_t payload_len = NODE_SIZE;
    uint8_t* node_data = (uint8_t*)calloc(1, payload_len);
    for (size_t i = 0; i < payload_len; i++) {
        node_data[i] = (uint8_t)(i % 251);
    }
    free(data);

    std::vector<uint64_t> offsets(N_SEQUENTIAL);

    double t0 = now_secs();

    for (size_t i = 0; i < N_SEQUENTIAL; i++) {
        uint64_t offset = 0;
        uint64_t bids[16] = {0};
        size_t num_bids = 0;

        rc = page_file_write_node(pf, node_data, payload_len, &offset, bids, 16, &num_bids);
        if (rc != 0) {
            fprintf(stderr, "page_file_write_node failed at i=%zu\n", i);
            break;
        }
        offsets[i] = offset;
    }

    double elapsed = now_secs() - t0;

    double mb_written = (double)(N_SEQUENTIAL * payload_len) / (1024.0 * 1024.0);
    double ops_sec = (double)N_SEQUENTIAL / elapsed;
    double throughput = mb_written / elapsed;

    printf("  Nodes: %zu\n", N_SEQUENTIAL);
    printf("  Node size: %zu bytes\n", payload_len);
    printf("  Time: %.3f sec\n", elapsed);
    printf("  Ops/sec: %.0f\n", ops_sec);
    printf("  Throughput: %.1f MB/s\n", throughput);
    printf("  Avg latency: %.2f us\n", (elapsed / N_SEQUENTIAL) * 1e6);
    printf("\n");

    free(node_data);
    page_file_destroy(pf);
    remove_dir(tmpdir);
}

// ============================================================
// Benchmark 2: Sequential Reads
// ============================================================

static void benchmark_sequential_reads() {
    printf("=== Sequential Reads ===\n");

    char tmpdir[256];
    if (make_temp_dir(tmpdir, sizeof(tmpdir), "page_cache_bench_reads") != 0) {
        fprintf(stderr, "Failed to create temp directory\n");
        return;
    }

    char filepath[512];
    snprintf(filepath, sizeof(filepath), "%s/reads.db", tmpdir);

    page_file_t* pf = page_file_create(filepath, BLOCK_SIZE, NUM_SUPERBLOCKS);
    if (!pf) { fprintf(stderr, "page_file_create failed\n"); remove_dir(tmpdir); return; }

    int rc = page_file_open(pf, 1);
    if (rc != 0) { fprintf(stderr, "page_file_open failed\n"); page_file_destroy(pf); remove_dir(tmpdir); return; }

    // Write nodes first
    size_t payload_len = NODE_SIZE;
    uint8_t* node_data = (uint8_t*)calloc(1, payload_len);
    for (size_t i = 0; i < payload_len; i++) {
        node_data[i] = (uint8_t)(i % 251);
    }

    std::vector<uint64_t> offsets(N_SEQUENTIAL);
    for (size_t i = 0; i < N_SEQUENTIAL; i++) {
        uint64_t offset = 0;
        uint64_t bids[16] = {0};
        size_t num_bids = 0;
        rc = page_file_write_node(pf, node_data, payload_len, &offset, bids, 16, &num_bids);
        if (rc != 0) {
            fprintf(stderr, "page_file_write_node failed at i=%zu\n", i);
            free(node_data);
            page_file_destroy(pf);
            remove_dir(tmpdir);
            return;
        }
        offsets[i] = offset;
    }

    // Now read them all back
    double t0 = now_secs();

    size_t success_count = 0;
    for (size_t i = 0; i < N_SEQUENTIAL; i++) {
        size_t out_len = 0;
        uint8_t* result = page_file_read_node(pf, offsets[i], &out_len);
        if (result) {
            success_count++;
            free(result);
        } else {
            fprintf(stderr, "page_file_read_node failed at offset %zu\n", (size_t)offsets[i]);
        }
    }

    double elapsed = now_secs() - t0;

    double mb_read = (double)(success_count * payload_len) / (1024.0 * 1024.0);
    double ops_sec = (double)success_count / elapsed;
    double throughput = mb_read / elapsed;

    printf("  Nodes: %zu (read %zu)\n", N_SEQUENTIAL, success_count);
    printf("  Node size: %zu bytes\n", payload_len);
    printf("  Time: %.3f sec\n", elapsed);
    printf("  Ops/sec: %.0f\n", ops_sec);
    printf("  Throughput: %.1f MB/s\n", throughput);
    printf("  Avg latency: %.2f us\n", (elapsed / N_SEQUENTIAL) * 1e6);
    printf("\n");

    free(node_data);
    page_file_destroy(pf);
    remove_dir(tmpdir);
}

// ============================================================
// Benchmark 3: Cache Hit Rate
// ============================================================

static void benchmark_cache_hit_rate() {
    printf("=== Cache Hit Rate ===\n");

    char tmpdir[256];
    if (make_temp_dir(tmpdir, sizeof(tmpdir), "page_cache_bench_hitrate") != 0) {
        fprintf(stderr, "Failed to create temp directory\n");
        return;
    }

    char filepath[512];
    snprintf(filepath, sizeof(filepath), "%s/hitrate.db", tmpdir);

    page_file_t* pf = page_file_create(filepath, BLOCK_SIZE, NUM_SUPERBLOCKS);
    if (!pf) { fprintf(stderr, "page_file_create failed\n"); remove_dir(tmpdir); return; }

    int rc = page_file_open(pf, 1);
    if (rc != 0) { fprintf(stderr, "page_file_open failed\n"); page_file_destroy(pf); remove_dir(tmpdir); return; }

    // Small cache (1 MB) so we can observe eviction
    size_t cache_size = CACHE_SMALL_MB * 1024 * 1024;
    bnode_cache_mgr_t* mgr = bnode_cache_mgr_create(cache_size, NUM_SHARDS);
    if (!mgr) { fprintf(stderr, "bnode_cache_mgr_create failed\n"); page_file_destroy(pf); remove_dir(tmpdir); return; }

    file_bnode_cache_t* fcache = bnode_cache_create_file_cache(mgr, pf, "hitrate.db");
    if (!fcache) { fprintf(stderr, "bnode_cache_create_file_cache failed\n"); bnode_cache_mgr_destroy(mgr); page_file_destroy(pf); remove_dir(tmpdir); return; }

    // Number of nodes to write - enough to fill cache and more
    size_t num_nodes = 1000;
    size_t payload_len = 512;  // 512 bytes each (including 4-byte prefix = 516 bytes per node)
    uint8_t* node_data = (uint8_t*)calloc(1, payload_len);
    for (size_t i = 0; i < payload_len; i++) {
        node_data[i] = (uint8_t)(i % 251);
    }

    // Phase 1: Write all nodes to cache
    std::vector<uint64_t> offsets(num_nodes);
    for (size_t i = 0; i < num_nodes; i++) {
        // Each write goes to the cache with a unique offset
        // Use offsets past the superblocks
        uint64_t offset = (uint64_t)(BLOCK_SIZE * NUM_SUPERBLOCKS + i * BLOCK_SIZE);
        offsets[i] = offset;

        size_t total_len = 0;
        uint8_t* data = make_prefixed_data(node_data, payload_len, &total_len);
        rc = bnode_cache_write(fcache, offset, data, total_len);
        if (rc != 0) {
            fprintf(stderr, "bnode_cache_write failed at i=%zu\n", i);
        }
        free(data);
    }

    // Flush to make them clean (readable from page file)
    rc = bnode_cache_flush_dirty(fcache);
    if (rc != 0) {
        fprintf(stderr, "bnode_cache_flush_dirty failed\n");
    }

    // Release all references from flush
    // After flush, offsets may have changed. Re-read items to release refs.
    // First, release any items we may have referenced
    // (bnode_cache_write doesn't give us references, so this is clean)

    // Phase 2: Read all nodes - should be 100% cache hit (they're in cache)
    size_t hits_phase2 = 0;
    double t0 = now_secs();

    for (size_t i = 0; i < num_nodes; i++) {
        bnode_cache_item_t* item = bnode_cache_read(fcache, offsets[i]);
        if (item != NULL) {
            hits_phase2++;
            bnode_cache_release(fcache, item);
        }
    }

    double elapsed_phase2 = now_secs() - t0;

    // Phase 3: Flush again, then write enough new nodes to evict half the cache
    // Use large writes to force eviction of the earlier nodes
    size_t evict_count = num_nodes / 2;
    for (size_t i = 0; i < evict_count; i++) {
        uint64_t evict_offset = (uint64_t)(BLOCK_SIZE * NUM_SUPERBLOCKS + (num_nodes + i) * BLOCK_SIZE);
        size_t total_len = 0;
        uint8_t* data = make_prefixed_data(node_data, payload_len * 2, &total_len);
        // Use larger data to put pressure on memory
        rc = bnode_cache_write(fcache, evict_offset, data, total_len);
        free(data);
    }

    // Flush and release the evicting nodes
    rc = bnode_cache_flush_dirty(fcache);
    for (size_t i = 0; i < evict_count; i++) {
        uint64_t evict_offset = (uint64_t)(BLOCK_SIZE * NUM_SUPERBLOCKS + (num_nodes + i) * BLOCK_SIZE);
        bnode_cache_item_t* item = bnode_cache_read(fcache, evict_offset);
        if (item) {
            bnode_cache_release(fcache, item);
        }
    }

    // Phase 4: Read the original nodes again - some will be cache misses now
    size_t hits_phase4 = 0;
    size_t misses_phase4 = 0;
    t0 = now_secs();

    for (size_t i = 0; i < num_nodes; i++) {
        bnode_cache_item_t* item = bnode_cache_read(fcache, offsets[i]);
        if (item != NULL) {
            hits_phase4++;
            bnode_cache_release(fcache, item);
        } else {
            misses_phase4++;
        }
    }

    double elapsed_phase4 = now_secs() - t0;

    printf("  Cache size: %zu MB\n", CACHE_SMALL_MB);
    printf("  Nodes: %zu (%zu bytes each)\n", num_nodes, payload_len);
    printf("\n");
    printf("  Phase 1 - 100%% cache hit (freshly written):\n");
    printf("    Hits: %zu / %zu\n", hits_phase2, num_nodes);
    printf("    Hit rate: %.1f%%\n", num_nodes > 0 ? (double)hits_phase4 / num_nodes * 100.0 : 0.0);
    printf("    Time: %.3f sec\n", elapsed_phase2);
    printf("    Ops/sec: %.0f\n", num_nodes > 0 ? hits_phase2 / elapsed_phase2 : 0.0);
    printf("\n");
    printf("  Phase 2 - After eviction pressure:\n");
    printf("    Hits: %zu, Misses: %zu\n", hits_phase4, misses_phase4);
    printf("    Hit rate: %.1f%%\n", (hits_phase4 + misses_phase4) > 0 ? (double)hits_phase4 / (hits_phase4 + misses_phase4) * 100.0 : 0.0);
    printf("    Time: %.3f sec\n", elapsed_phase4);
    printf("    Ops/sec: %.0f\n", (hits_phase4 + misses_phase4) > 0 ? (hits_phase4 + misses_phase4) / elapsed_phase4 : 0.0);
    printf("\n");

    free(node_data);
    bnode_cache_destroy_file_cache(fcache);
    bnode_cache_mgr_destroy(mgr);
    page_file_destroy(pf);
    remove_dir(tmpdir);
}

// ============================================================
// Benchmark 4: Dirty Flush Throughput
// ============================================================

static void benchmark_dirty_flush() {
    printf("=== Dirty Flush Throughput ===\n");

    char tmpdir[256];
    if (make_temp_dir(tmpdir, sizeof(tmpdir), "page_cache_bench_flush") != 0) {
        fprintf(stderr, "Failed to create temp directory\n");
        return;
    }

    char filepath[512];
    snprintf(filepath, sizeof(filepath), "%s/flush.db", tmpdir);

    page_file_t* pf = page_file_create(filepath, BLOCK_SIZE, NUM_SUPERBLOCKS);
    if (!pf) { fprintf(stderr, "page_file_create failed\n"); remove_dir(tmpdir); return; }

    int rc = page_file_open(pf, 1);
    if (rc != 0) { fprintf(stderr, "page_file_open failed\n"); page_file_destroy(pf); remove_dir(tmpdir); return; }

    // 10 MB cache
    size_t cache_size = CACHE_LARGE_MB * 1024 * 1024;
    bnode_cache_mgr_t* mgr = bnode_cache_mgr_create(cache_size, NUM_SHARDS);
    if (!mgr) { fprintf(stderr, "bnode_cache_mgr_create failed\n"); page_file_destroy(pf); remove_dir(tmpdir); return; }

    file_bnode_cache_t* fcache = bnode_cache_create_file_cache(mgr, pf, "flush.db");
    if (!fcache) { fprintf(stderr, "bnode_cache_create_file_cache failed\n"); bnode_cache_mgr_destroy(mgr); page_file_destroy(pf); remove_dir(tmpdir); return; }

    size_t num_nodes = N_SEQUENTIAL;
    size_t payload_len = NODE_SIZE;
    uint8_t* node_data = (uint8_t*)calloc(1, payload_len);
    for (size_t i = 0; i < payload_len; i++) {
        node_data[i] = (uint8_t)(i % 251);
    }

    // Write all nodes as dirty
    double t_write_start = now_secs();
    for (size_t i = 0; i < num_nodes; i++) {
        uint64_t offset = (uint64_t)(BLOCK_SIZE * NUM_SUPERBLOCKS + i * BLOCK_SIZE);
        size_t total_len = 0;
        uint8_t* data = make_prefixed_data(node_data, payload_len, &total_len);
        rc = bnode_cache_write(fcache, offset, data, total_len);
        if (rc != 0) {
            fprintf(stderr, "bnode_cache_write failed at i=%zu\n", i);
        }
        free(data);
    }
    double t_write_end = now_secs();
    double write_time = t_write_end - t_write_start;

    size_t dirty_count = bnode_cache_dirty_count(fcache);
    size_t dirty_bytes = bnode_cache_dirty_bytes(fcache);

    printf("  Dirty nodes written: %zu\n", dirty_count);
    printf("  Dirty bytes: %zu (%.2f MB)\n", dirty_bytes, (double)dirty_bytes / (1024.0 * 1024.0));
    printf("  Write time: %.3f sec\n", write_time);
    printf("  Write ops/sec: %.0f\n", num_nodes / write_time);
    printf("\n");

    // Flush all dirty nodes
    double t_flush_start = now_secs();
    rc = bnode_cache_flush_dirty(fcache);
    double t_flush_end = now_secs();
    double flush_time = t_flush_end - t_flush_start;

    if (rc != 0) {
        fprintf(stderr, "bnode_cache_flush_dirty failed\n");
    }

    size_t dirty_after = bnode_cache_dirty_count(fcache);
    double mb_flushed = (double)dirty_bytes / (1024.0 * 1024.0);

    printf("  Flush time: %.3f sec\n", flush_time);
    printf("  Flush throughput: %.1f MB/s\n", flush_time > 0 ? mb_flushed / flush_time : 0.0);
    printf("  Dirty nodes after flush: %zu\n", dirty_after);
    printf("  Avg flush latency per node: %.2f us\n", flush_time > 0 ? (flush_time / dirty_count) * 1e6 : 0.0);
    printf("\n");

    free(node_data);
    bnode_cache_destroy_file_cache(fcache);
    bnode_cache_mgr_destroy(mgr);
    page_file_destroy(pf);
    remove_dir(tmpdir);
}

// ============================================================
// Benchmark 5: Combined CoW Pattern
// ============================================================

static void benchmark_cow_pattern() {
    printf("=== Combined CoW Pattern ===\n");

    char tmpdir[256];
    if (make_temp_dir(tmpdir, sizeof(tmpdir), "page_cache_bench_cow") != 0) {
        fprintf(stderr, "Failed to create temp directory\n");
        return;
    }

    char filepath[512];
    snprintf(filepath, sizeof(filepath), "%s/cow.db", tmpdir);

    page_file_t* pf = page_file_create(filepath, BLOCK_SIZE, NUM_SUPERBLOCKS);
    if (!pf) { fprintf(stderr, "page_file_create failed\n"); remove_dir(tmpdir); return; }

    int rc = page_file_open(pf, 1);
    if (rc != 0) { fprintf(stderr, "page_file_open failed\n"); page_file_destroy(pf); remove_dir(tmpdir); return; }

    // 10 MB cache
    size_t cache_size = CACHE_LARGE_MB * 1024 * 1024;
    bnode_cache_mgr_t* mgr = bnode_cache_mgr_create(cache_size, NUM_SHARDS);
    if (!mgr) { fprintf(stderr, "bnode_cache_mgr_create failed\n"); page_file_destroy(pf); remove_dir(tmpdir); return; }

    file_bnode_cache_t* fcache = bnode_cache_create_file_cache(mgr, pf, "cow.db");
    if (!fcache) { fprintf(stderr, "bnode_cache_create_file_cache failed\n"); bnode_cache_mgr_destroy(mgr); page_file_destroy(pf); remove_dir(tmpdir); return; }

    size_t num_nodes = N_SEQUENTIAL;
    size_t payload_len = NODE_SIZE;
    uint8_t* node_data = (uint8_t*)calloc(1, payload_len);
    for (size_t i = 0; i < payload_len; i++) {
        node_data[i] = (uint8_t)(i % 251);
    }

    // Phase 1: Initial write
    std::vector<uint64_t> offsets(num_nodes);
    double t0 = now_secs();

    for (size_t i = 0; i < num_nodes; i++) {
        uint64_t offset = (uint64_t)(BLOCK_SIZE * NUM_SUPERBLOCKS + i * BLOCK_SIZE);
        offsets[i] = offset;
        size_t total_len = 0;
        uint8_t* data = make_prefixed_data(node_data, payload_len, &total_len);
        rc = bnode_cache_write(fcache, offset, data, total_len);
        if (rc != 0) {
            fprintf(stderr, "bnode_cache_write failed at i=%zu\n", i);
        }
        free(data);
    }

    double write_time = now_secs() - t0;

    // Phase 2: Flush
    t0 = now_secs();
    rc = bnode_cache_flush_dirty(fcache);
    double flush_time = now_secs() - t0;
    if (rc != 0) {
        fprintf(stderr, "bnode_cache_flush_dirty failed\n");
    }

    // Read back offsets after flush (flush may have changed them)
    // Items in cache have updated offsets after flush
    // We need to read from cache to get current offsets for read-back verification
    std::vector<uint64_t> flushed_offsets(num_nodes);
    for (size_t i = 0; i < num_nodes; i++) {
        bnode_cache_item_t* item = bnode_cache_read(fcache, offsets[i]);
        if (item != NULL) {
            flushed_offsets[i] = item->offset;
            bnode_cache_release(fcache, item);
        } else {
            flushed_offsets[i] = offsets[i];
        }
    }

    // Phase 3: Modify (CoW - write new version)
    // Modify every other node with updated data
    uint8_t* new_data = (uint8_t*)calloc(1, payload_len);
    for (size_t i = 0; i < payload_len; i++) {
        new_data[i] = (uint8_t)((i + 1) % 251);
    }

    size_t modify_count = num_nodes / 2;

    t0 = now_secs();
    for (size_t i = 0; i < modify_count; i++) {
        // Write at the same offset - CoW will mark old data stale and write new
        size_t total_len = 0;
        uint8_t* data = make_prefixed_data(new_data, payload_len, &total_len);
        rc = bnode_cache_write(fcache, flushed_offsets[i * 2], data, total_len);
        if (rc != 0) {
            fprintf(stderr, "bnode_cache_write (modify) failed at i=%zu\n", i * 2);
        }
        free(data);
    }
    double modify_time = now_secs() - t0;

    // Phase 4: Flush modified nodes
    t0 = now_secs();
    rc = bnode_cache_flush_dirty(fcache);
    double flush2_time = now_secs() - t0;
    if (rc != 0) {
        fprintf(stderr, "bnode_cache_flush_dirty (modify) failed\n");
    }

    // Phase 5: Read back all nodes
    t0 = now_secs();
    size_t read_success = 0;
    for (size_t i = 0; i < num_nodes; i++) {
        bnode_cache_item_t* item = bnode_cache_read(fcache, flushed_offsets[i]);
        if (item != NULL) {
            read_success++;
            bnode_cache_release(fcache, item);
        }
    }
    double read_time = now_secs() - t0;

    double total_time = write_time + flush_time + modify_time + flush2_time + read_time;
    double total_mb = (double)(num_nodes * payload_len + modify_count * payload_len) / (1024.0 * 1024.0);

    printf("  Nodes: %zu\n", num_nodes);
    printf("  Node size: %zu bytes\n", payload_len);
    printf("  Modified: %zu nodes\n", modify_count);
    printf("\n");
    printf("  Phase 1 - Initial write:\n");
    printf("    Time: %.3f sec\n", write_time);
    printf("    Ops/sec: %.0f\n", write_time > 0 ? num_nodes / write_time : 0.0);
    printf("\n");
    printf("  Phase 2 - First flush:\n");
    printf("    Time: %.3f sec\n", flush_time);
    printf("\n");
    printf("  Phase 3 - CoW modify:\n");
    printf("    Time: %.3f sec\n", modify_time);
    printf("    Ops/sec: %.0f\n", modify_time > 0 ? modify_count / modify_time : 0.0);
    printf("\n");
    printf("  Phase 4 - Second flush:\n");
    printf("    Time: %.3f sec\n", flush2_time);
    printf("\n");
    printf("  Phase 5 - Read back:\n");
    printf("    Read success: %zu / %zu\n", read_success, num_nodes);
    printf("    Time: %.3f sec\n", read_time);
    printf("    Ops/sec: %.0f\n", read_time > 0 ? read_success / read_time : 0.0);
    printf("\n");
    printf("  Total CoW cycle:\n");
    printf("    Total time: %.3f sec\n", total_time);
    printf("    Total data: %.2f MB\n", total_mb);
    printf("    Throughput: %.1f MB/s\n", total_time > 0 ? total_mb / total_time : 0.0);
    printf("    Cycle ops/sec: %.0f\n", total_time > 0 ? (num_nodes + modify_count + num_nodes) / total_time : 0.0);
    printf("\n");

    free(node_data);
    free(new_data);
    bnode_cache_destroy_file_cache(fcache);
    bnode_cache_mgr_destroy(mgr);
    page_file_destroy(pf);
    remove_dir(tmpdir);
}

// ============================================================
// Main
// ============================================================

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    printf("========================================\n");
    printf("Page File + Bnode Cache Benchmarks\n");
    printf("========================================\n\n");

    printf("Configuration:\n");
    printf("  - Block size: %zu bytes\n", BLOCK_SIZE);
    printf("  - Node size: %zu bytes\n", NODE_SIZE);
    printf("  - Superblocks: %zu\n", NUM_SUPERBLOCKS);
    printf("  - Cache shards: %zu\n", NUM_SHARDS);
    printf("  - Sequential N: %zu\n", N_SEQUENTIAL);
    printf("\n");

    benchmark_sequential_writes();
    benchmark_sequential_reads();
    benchmark_cache_hit_rate();
    benchmark_dirty_flush();
    benchmark_cow_pattern();

    printf("========================================\n");
    printf("All page cache benchmarks complete.\n");
    printf("========================================\n");

    return 0;
}