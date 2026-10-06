// test_eviction_debounce.cpp — the eviction-task spin-fix probes.
//
// The recorded defect (Final review, gdb-confirmed on a served SecretAgent
// frame): the eviction task used to re-enqueue itself UNCONDITIONALLY at the
// end of every iteration, even when the drain found n == 0 — an idle open
// database burned ~2.2 cores forever (2220 executions / 10 s, no client).
//
// The fix: the task never self-arms from an empty queue. The open-arm
// enqueue stays (exactly once), and every later arm comes from the
// eviction_debouncer, poked by database_on_bnode_evict when a bnode eviction
// actually pushes an offset (the WAL fsync_debouncer idiom). Bursts coalesce
// into a single fire per quiet window (wait 100 ms, forced at max_wait
// 1000 ms); a backlog larger than one 64-offset drain still walks out
// immediately (reschedule while eviction_queue_size > 0), which keeps a big
// page-fault storm bounded but NOT per-push.
//
// EXPECTED (a healthy database): an idle db executes the eviction task once
// (the open arm) and never again; a burst coalesces to a couple of runs and
// drains its queue to empty.
#include <gtest/gtest.h>

extern "C" {
#include "Database/database.h"
#include "Database/database_iterator.h"
#include "Database/eviction_queue.h"
#include "Storage/bnode_cache.h"
#include "Time/wheel.h"
#include "Workers/pool.h"
#include "Buffer/buffer.h"
}

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <chrono>

using namespace std::chrono_literals;

class EvictionDebounceTest : public ::testing::Test {
   protected:
    void SetUp() override {
        char tmpdir[128];
#if _WIN32
        strcpy(tmpdir, "C:\\Temp\\wavedb_evict_XXXXXX");
#else
        std::string base = std::string(getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp") +
                           "/wavedb_evict_XXXXXX";
        strcpy(tmpdir, base.c_str());
        mkdtemp(tmpdir);
#endif
        location_ = tmpdir;
    }

    void TearDown() override {
        // Destroy the database FIRST — it flushes its debouncers while the
        // wheel is still running (see STYLEGUIDE Debouncer Lifecycle).
        if (db_ != nullptr) {
            database_destroy(db_);
            db_ = nullptr;
        }
        char cmd[512];
#if _WIN32
        snprintf(cmd, sizeof(cmd), "rmdir /s /q %s", location_.c_str());
#else
        snprintf(cmd, sizeof(cmd), "rm -rf %s", location_.c_str());
#endif
        system(cmd);
    }

    // Concurrent mode (pool + wheel owned by the config): this is the shape
    // that runs the background eviction task. Small bnode cache so later
    // tests can force real cache evictions with a bounded dataset.
    database_t* open_db() {
        return open_db_with_cache_mb(1);
    }

    database_t* open_db_with_cache_mb(size_t cache_mb) {
        database_config_t* config = database_config_default();
        config->enable_persist = 1;
        config->sync_only = 0;
        config->worker_threads = 2;
        config->timer_resolution_ms = 10;
        config->bnode_cache_memory_mb = cache_mb;
        config->btree_node_size = 512;
        int error_code = 0;
        database_t* db =
            database_create_with_config(location_.c_str(), config, &error_code);
        database_config_destroy(config);
        if (db != NULL && db->wheel != NULL) {
            // The debouncer's timers live on this wheel; the OWNER starts
            // the wheel thread (same discipline every fixture in this suite
            // uses for an externally-created wheel).
            hierarchical_timing_wheel_run(db->wheel);
        }
        return db;
    }

    void put_one(database_t* db, uint64_t i, size_t value_len) {
        char key[32];
        snprintf(key, sizeof(key), "ev/%08llu", (unsigned long long)i);
        char value[512];
        size_t vlen = value_len < sizeof(value) ? value_len : sizeof(value);
        memset(value, (int)(i & 0xFF), vlen);
        EXPECT_EQ(database_put_sync_raw(db, key, strlen(key), '/',
                                        (const uint8_t*)value, vlen), 0);
    }

    // Batch-write FILL_RECORDS records under "ev/xxxx" (SUB-BATCH records
    // per submission — a batch whose serialized size crosses the WAL max
    // file size errors out with -5). Values are short; the record count is
    // what sizes the trie's bnode population.
    static constexpr size_t FILL_SUB_BATCH = 256;
    void fill_burst(database_t* db) {
        char key[32];
        uint8_t value[FILL_VALUE_LEN];
        for (size_t start = 0; start < FILL_RECORDS; start += FILL_SUB_BATCH) {
            batch_t* batch = batch_create(FILL_SUB_BATCH);
            ASSERT_NE(batch, nullptr);
            for (size_t i = start; i < start + FILL_SUB_BATCH && i < FILL_RECORDS; i++) {
                snprintf(key, sizeof(key), "%08zu", i);
                memset(value, (int)(i & 0xFF), sizeof(value));
                buffer_t* vbuf =
                    buffer_create_from_pointer_copy(value, sizeof(value));
                identifier_t* ident = identifier_create(vbuf, 0);
                buffer_destroy(vbuf);
                path_t* path = path_create();
                buffer_t* prefix =
                    buffer_create_from_pointer_copy((uint8_t*)"ev", 2);
                identifier_t* pid = identifier_create(prefix, 0);
                buffer_destroy(prefix);
                path_append(path, pid);
                identifier_destroy(pid);
                buffer_t* kbuf =
                    buffer_create_from_pointer_copy((uint8_t*)key, strlen(key));
                identifier_t* kid = identifier_create(kbuf, 0);
                buffer_destroy(kbuf);
                path_append(path, kid);
                identifier_destroy(kid);
                EXPECT_EQ(batch_add_put(batch, path, ident), 0);
            }
            EXPECT_EQ(database_write_batch_sync(db, batch), 0);
            batch_destroy(batch);
        }
    }

    // Load every record under the "ev/" prefix. On a REOPENED database the
    // trie is loaded lazily: each accessed subtree pulls its serialized
    // bnodes through the bnode cache (the only path that populates it), so
    // this pass is what drives real cache evictions on a small cache.
    size_t walk_all(database_t* db) {
        raw_result_t* results = nullptr;
        size_t count = 0;
        int rc = database_scan_sync_raw(db, "ev/", 4, '/', &results, &count);
        EXPECT_EQ(rc, 0);
        size_t total = (size_t)count;
        database_raw_results_free(results, (size_t)count);
        return total;
    }

    // Records that produce ~nodes records/bnodes records: kept modest so the
    // eviction backlog stays under two 64-offset drains.
    static constexpr size_t FILL_RECORDS = 9450;
    static constexpr size_t FILL_VALUE_LEN = 64;

    std::string location_;
    database_t* db_ = nullptr;
};

// The spin probe: a freshly opened CONCURRENT+PERSIST database runs the
// eviction task exactly once (the open arm) and then STOPS. Before the fix
// this never held — runs grew by ~220 per second with no writes at all.
TEST_F(EvictionDebounceTest, IdleOpenDoesNotChurn) {
    db_ = open_db();
    ASSERT_NE(db_, nullptr);

    // One full quiet window + margin: the open arm must have executed.
    std::this_thread::sleep_for(600ms);
    EXPECT_EQ(database_eviction_task_runs(db_), 1u)
        << "open arm did not execute (or ran more than once)";

    // And then nothing — no self-churn while idle.
    std::this_thread::sleep_for(600ms);
    EXPECT_EQ(database_eviction_task_runs(db_), 1u)
        << "idle database re-ran the eviction task (the spin is back)";

    // The in-flight counter must be back to rest: no leaked work items.
    std::this_thread::sleep_for(200ms);
    EXPECT_EQ(eviction_queue_size(&db_->eviction_queue), 0u);
}

// The coalescing probe: fill the db, reopen it (fresh trie lazily loads each
// bnode through the cache), then sweep the whole namespace — the small cache
// evicts as the walk inserts new entries, which pushes offsets and pokes the
// debouncer. The whole sweep is one burst: it must coalesce into one arm
// fire (plus at most the backlog it left behind), and the queue must drain
// back to empty.
//
// The runs bound encodes "coalesced, not per-push": every eviction push
// coalesced into the debouncer's quiet window; the only legitimate
// multiplier is the 64-offset drain cap walking out the backlog. The
// high-water mark sampled during the sweep bounds the pushes that could
// have sat in the 256-cap queue, so the assertion self-adjusts to dataset/
// platform shifts while still failing hard on per-push churning (runs
// proportional to pushes instead of to ceil(backlog / 64)).
TEST_F(EvictionDebounceTest, EvictionBurstCoalesces) {
    // Fill + persist: a write burst dirtying many bnodes, then flush.
    db_ = open_db();
    ASSERT_NE(db_, nullptr);
    fill_burst(db_);
    EXPECT_EQ(database_flush_dirty_bnodes(db_), 0);
    database_destroy(db_);
    db_ = nullptr;

    // Reopen: lazy-trie, everything loaded through the cache on access.
    db_ = open_db();
    ASSERT_NE(db_, nullptr);
    uint64_t runs_before = database_eviction_task_runs(db_);

    // The sweep: one tight burst of cache loads + evictions, with a watcher
    // sampling the queue's high-water mark concurrently.
    struct QueueWatcher {
        std::thread thread_;
        std::atomic<bool> stop_{false};
        std::atomic<size_t> high_water_{0};

        QueueWatcher(eviction_queue_t* queue) {
            thread_ = std::thread([this, queue]() {
                while (!stop_.load(std::memory_order_relaxed)) {
                    size_t size = eviction_queue_size(queue);
                    if (size > high_water_.load(std::memory_order_relaxed)) {
                        high_water_.store(size, std::memory_order_relaxed);
                    }
                    std::this_thread::sleep_for(2ms);
                }
            });
        }
        void stop() {
            stop_.store(true, std::memory_order_relaxed);
            thread_.join();
        }
    } watcher(&db_->eviction_queue);

    size_t walked = walk_all(db_);
    EXPECT_GT(walked, 0u);
    watcher.stop();
    size_t backlog_hwm = watcher.high_water_.load();

    // Debouncer quiet window (100 ms) + force-fire cap (1000 ms) + margin —
    // everything the burst pushed must have been armed and drained by now.
    std::this_thread::sleep_for(1500ms);

    uint64_t runs = database_eviction_task_runs(db_);
    EXPECT_EQ(eviction_queue_size(&db_->eviction_queue), 0u)
        << "evicted offsets left queued after the burst window";
    EXPECT_GT(runs, runs_before) << "eviction burst never armed a task";
    EXPECT_LE(runs - runs_before, backlog_hwm / 64 + 3u)
        << "runs grew per-push — the debounced coalescing is lost";
}

// The accounting probe: the runs counter counts ACTUAL EXECUTIONS, not
// enqueues. The open arm pins that exactly: one enqueued open task whose
// body ran = +1, and an idle database (no pushes) contributes nothing more.
// The abort path (a work item dropped before running) must contribute +0 —
// pinned implicitly here and by the destroy-clean drain in IdleOpenDoesNot
// Churn: if a dropped enqueue leaked a +1, the two 600 ms probes could not
// both read exactly 1.
TEST_F(EvictionDebounceTest, RunsCounterCountsExecutionsNotEnqueues) {
    db_ = open_db();
    ASSERT_NE(db_, nullptr);
    std::this_thread::sleep_for(600ms);
    EXPECT_EQ(database_eviction_task_runs(db_), 1u);
    std::this_thread::sleep_for(600ms);
    // No pushes arrived — a second execution here would be the spin.
    EXPECT_EQ(database_eviction_task_runs(db_), 1u);
}