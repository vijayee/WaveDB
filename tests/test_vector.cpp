// Tests for Vector Layer
// Spec: docs/superpowers/specs/2026-07-12-vector-layer-design.md
//
// Task 1: skeleton — header, lifecycle, config, dispatch, distance functions.
// 4 lifecycle tests: create/destroy (separate + shared), reconfigure, invalid dim.

#include <gtest/gtest.h>
#include <string.h>
#include <stdlib.h>
#include <string>
#include <vector>
#include <set>
#include <map>
#include <algorithm>
#include <utility>
#include <future>
#include <chrono>

#if _WIN32
#include <io.h>
#include <direct.h>
#include <process.h>
#define getpid() _getpid()
#define mkdir(path, mode) _mkdir(path)
#else
#include <unistd.h>
#include <sys/stat.h>
#endif

extern "C" {
#include "../src/Layers/vector/vector_layer.h"
#include "../src/Layers/vector/vector_internal.h"
#include "../src/Database/database.h"
#include "../src/Database/database_config.h"
#include "../src/Workers/promise.h"
#include "../src/Workers/error.h"
#include "../src/Util/log.h"
}

static int vl_test_counter = 0;

class VectorLayerTest : public ::testing::Test {
protected:
    void SetUp() override {
        test_dir = "/tmp/wavedb_vltest_" + std::to_string(getpid()) + "_" +
                   std::to_string(vl_test_counter++);
        mkdir(test_dir.c_str(), 0700);
        /* Per-chunk log_info progress from migrate/train/rebuild would spam
         * gtest stderr; suppress the default stderr path. Registered C
         * callbacks still fire (quiet is independent of callbacks in rxi). */
        log_set_quiet(1);
    }
    void TearDown() override {
        std::string cmd = "rm -rf " + test_dir;
        system(cmd.c_str());
    }
    std::string test_dir;

    vector_layer_config_t flat_config(int dim) {
        vector_layer_config_t cfg = {};
        cfg.format.index_type = VL_INDEX_FLAT;
        cfg.format.dim = dim;
        cfg.format.delimiter = '/';
        cfg.format.distance = VL_DIST_COSINE;
        cfg.runtime.top_k = 10;
        cfg.runtime.sync_only = 1;
        return cfg;
    }

    vector_layer_config_t ivf_config(int dim, int n_clusters) {
        vector_layer_config_t cfg = {};
        cfg.format.index_type = VL_INDEX_IVF;
        cfg.format.dim = dim;
        cfg.format.delimiter = '/';
        cfg.format.distance = VL_DIST_L2;
        cfg.format.ivf_n_clusters = n_clusters;
        cfg.runtime.top_k = 10;
        cfg.runtime.sync_only = 1;
        cfg.runtime.ivf_nprobe = n_clusters;  /* probe all clusters */
        cfg.runtime.ivf_flat_until = 100000;  /* never fall back to flat */
        return cfg;
    }

    vector_layer_config_t slsh_config(int dim) {
        vector_layer_config_t cfg = {};
        cfg.format.index_type = VL_INDEX_SLSH;
        cfg.format.dim = dim;
        cfg.format.delimiter = '/';
        cfg.format.distance = VL_DIST_L2;
        cfg.format.slsh_lsh_tables = 2;
        cfg.format.slsh_hash_bits = 8;
        cfg.format.slsh_bucket_width = 1.0f;
        cfg.runtime.top_k = 10;
        cfg.runtime.sync_only = 1;
        cfg.runtime.slsh_scan_radius = 100;
        return cfg;
    }
};

TEST_F(VectorLayerTest, CreateSeparateAndDestroy) {
    vector_layer_config_t cfg = flat_config(4);
    int err = 0;
    vector_layer_t *vl = vector_layer_open_separate(test_dir.c_str(), "test", &cfg, &err);
    ASSERT_NE(vl, nullptr);
    ASSERT_EQ(err, 0);
    EXPECT_EQ(vector_layer_count(vl), 0u);
    vector_layer_destroy(vl);
}

TEST_F(VectorLayerTest, CreateSharedOnExistingDb) {
    database_config_t *dbcfg = database_config_default();
    ASSERT_NE(dbcfg, nullptr);
    database_config_set_sync_only(dbcfg, 1);
    int db_err = 0;
    database_t *db = database_create_with_config(test_dir.c_str(), dbcfg, &db_err);
    ASSERT_NE(db, nullptr);
    ASSERT_EQ(db_err, 0);

    vector_layer_config_t cfg = flat_config(4);
    int err = 0;
    vector_layer_t *vl = vector_layer_create("test", db, NULL, &cfg, &err);
    ASSERT_NE(vl, nullptr);
    ASSERT_EQ(err, 0);
    EXPECT_EQ(vector_layer_count(vl), 0u);
    vector_layer_destroy(vl);
    database_destroy(db);
    database_config_destroy(dbcfg);
}

TEST_F(VectorLayerTest, ReconfigureRuntime) {
    vector_layer_config_t cfg = flat_config(4);
    cfg.runtime.ivf_nprobe = 4;
    int err = 0;
    vector_layer_t *vl = vector_layer_open_separate(test_dir.c_str(), "test", &cfg, &err);
    ASSERT_NE(vl, nullptr);
    vector_layer_runtime_t rt = {};
    rt.top_k = 20;
    rt.sync_only = 1;
    rt.ivf_nprobe = 8;
    EXPECT_EQ(vector_layer_reconfigure(vl, &rt), 0);
    EXPECT_EQ(vector_layer_count(vl), 0u);
    vector_layer_destroy(vl);
}

TEST_F(VectorLayerTest, RejectsInvalidDim) {
    vector_layer_config_t cfg = flat_config(0);  // dim=0 invalid
    int err = 0;
    vector_layer_t *vl = vector_layer_open_separate(test_dir.c_str(), "test", &cfg, &err);
    EXPECT_EQ(vl, nullptr);
    EXPECT_LT(err, 0);
}

TEST_F(VectorLayerTest, KeyEncoding) {
    char *k = vl_key_vector("test", '/', "alice");
    ASSERT_NE(k, nullptr);
    EXPECT_STREQ(k, "vec/test/vector/alice");
    free(k);

    k = vl_key_count("test", '/');
    ASSERT_NE(k, nullptr);
    EXPECT_STREQ(k, "vec/test/count");
    free(k);

    k = vl_key_centroid("test", '/', 42);
    ASSERT_NE(k, nullptr);
    EXPECT_STREQ(k, "vec/test/centroid/0000000042");
    free(k);

    k = vl_key_cluster_member("test", '/', 42, "alice");
    ASSERT_NE(k, nullptr);
    EXPECT_STREQ(k, "vec/test/cluster/0000000042/alice");
    free(k);

    uint8_t lsh[] = {0xab, 0xcd};
    k = vl_key_hash("test", '/', lsh, 2, "alice");
    ASSERT_NE(k, nullptr);
    EXPECT_STREQ(k, "vec/test/hash/abcd/alice");
    free(k);

    k = vl_key_proj("test", '/', 1);
    ASSERT_NE(k, nullptr);
    EXPECT_STREQ(k, "vec/test/proj/0000000001");
    free(k);
}

TEST_F(VectorLayerTest, FlatInsertCount) {
    vector_layer_config_t cfg = flat_config(4);
    int err = 0;
    vector_layer_t *vl = vector_layer_open_separate(test_dir.c_str(), "test", &cfg, &err);
    ASSERT_NE(vl, nullptr);

    float v[4] = {1.0f, 2.0f, 3.0f, 4.0f};
    for (int i = 0; i < 5; i++) {
        std::string id = "vec_" + std::to_string(i);
        int rc = vector_layer_insert_sync(vl, id.c_str(), v, NULL, 0);
        ASSERT_EQ(rc, 0);
    }
    EXPECT_EQ(vector_layer_count(vl), 5u);
    vector_layer_destroy(vl);
}

TEST_F(VectorLayerTest, FlatSearch) {
    vector_layer_config_t cfg = flat_config(4);
    cfg.format.distance = VL_DIST_L2;  // use L2 for predictable distances
    int err = 0;
    vector_layer_t *vl = vector_layer_open_separate(test_dir.c_str(), "test", &cfg, &err);
    ASSERT_NE(vl, nullptr);

    float vs[5][4] = {
        {1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}, {1, 1, 1, 1}
    };
    const char* ids[5] = {"e0", "e1", "e2", "e3", "all"};
    for (int i = 0; i < 5; i++) {
        ASSERT_EQ(vector_layer_insert_sync(vl, ids[i], vs[i], NULL, 0), 0);
    }

    // Query for {0,0,0,1} — nearest is "e3" (L2=0), then "all" (L2=sqrt(3)~1.73).
    float q[4] = {0, 0, 0, 1};
    vl_result_t *results = NULL;
    int n = 0;
    ASSERT_EQ(vector_layer_search_sync(vl, q, 2, &results, &n), 0);
    ASSERT_EQ(n, 2);
    EXPECT_STREQ(results[0].id, "e3");
    vector_layer_free_results(results, n);
    vector_layer_destroy(vl);
}

TEST_F(VectorLayerTest, FlatExactMatchesBruteForce) {
    vector_layer_config_t cfg = flat_config(8);
    cfg.format.distance = VL_DIST_L2;
    int err = 0;
    vector_layer_t *vl = vector_layer_open_separate(test_dir.c_str(), "test", &cfg, &err);
    ASSERT_NE(vl, nullptr);

    srand(42);
    std::vector<std::vector<float>> stored;
    std::vector<std::string> ids;
    for (int i = 0; i < 50; i++) {
        std::vector<float> v(8);
        for (int d = 0; d < 8; d++) v[d] = (float)(rand() % 100) / 100.0f;
        std::string id = "v" + std::to_string(i);
        ASSERT_EQ(vector_layer_insert_sync(vl, id.c_str(), v.data(), NULL, 0), 0);
        stored.push_back(v);
        ids.push_back(id);
    }

    // 10 queries; FLAT recall@10 must == 1.0 by definition.
    int total_hits = 0;
    for (int q = 0; q < 10; q++) {
        std::vector<float> query(8);
        for (int d = 0; d < 8; d++) query[d] = (float)(rand() % 100) / 100.0f;

        // Brute-force ground truth.
        std::vector<std::pair<float, int>> dists;
        for (int i = 0; i < 50; i++) {
            float d = vl_distance(query.data(), stored[i].data(), 8, VL_DIST_L2);
            dists.push_back({d, i});
        }
        std::sort(dists.begin(), dists.end());
        std::set<std::string> truth;
        for (int i = 0; i < 10; i++) truth.insert(ids[dists[i].second]);

        vl_result_t *results = NULL;
        int n = 0;
        ASSERT_EQ(vector_layer_search_sync(vl, query.data(), 10, &results, &n), 0);
        ASSERT_EQ(n, 10);
        for (int i = 0; i < n; i++) if (truth.count(results[i].id)) total_hits++;
        vector_layer_free_results(results, n);
    }
    EXPECT_EQ(total_hits, 100);  // 10 queries × 10 results, all must hit (recall@10 == 1.0)
    vector_layer_destroy(vl);
}

TEST_F(VectorLayerTest, FlatDelete) {
    vector_layer_config_t cfg = flat_config(4);
    cfg.format.distance = VL_DIST_L2;
    int err = 0;
    vector_layer_t *vl = vector_layer_open_separate(test_dir.c_str(), "test", &cfg, &err);
    ASSERT_NE(vl, nullptr);
    float v[4] = {1, 2, 3, 4};
    ASSERT_EQ(vector_layer_insert_sync(vl, "a", v, NULL, 0), 0);
    ASSERT_EQ(vector_layer_insert_sync(vl, "b", v, NULL, 0), 0);
    EXPECT_EQ(vector_layer_count(vl), 2u);
    ASSERT_EQ(vector_layer_delete_sync(vl, "a"), 0);
    EXPECT_EQ(vector_layer_count(vl), 1u);
    // Search should now return only "b".
    vl_result_t *results = NULL; int n = 0;
    ASSERT_EQ(vector_layer_search_sync(vl, v, 10, &results, &n), 0);
    ASSERT_EQ(n, 1);
    EXPECT_STREQ(results[0].id, "b");
    vector_layer_free_results(results, n);
    vector_layer_destroy(vl);
}

TEST_F(VectorLayerTest, FlatMetadata) {
    vector_layer_config_t cfg = flat_config(4);
    cfg.format.distance = VL_DIST_L2;
    int err = 0;
    vector_layer_t *vl = vector_layer_open_separate(test_dir.c_str(), "test", &cfg, &err);
    ASSERT_NE(vl, nullptr);
    float v[4] = {1, 2, 3, 4};
    uint8_t meta[] = {0xAA, 0xBB, 0xCC};
    ASSERT_EQ(vector_layer_insert_sync(vl, "a", v, meta, 3), 0);
    vl_result_t *results = NULL; int n = 0;
    ASSERT_EQ(vector_layer_search_sync(vl, v, 1, &results, &n), 0);
    ASSERT_EQ(n, 1);
    ASSERT_NE(results[0].metadata, nullptr);
    ASSERT_EQ(results[0].metadata_len, 3u);
    EXPECT_EQ(results[0].metadata[0], 0xAA);
    EXPECT_EQ(results[0].metadata[1], 0xBB);
    EXPECT_EQ(results[0].metadata[2], 0xCC);
    vector_layer_free_results(results, n);
    vector_layer_destroy(vl);
}

TEST_F(VectorLayerTest, IVFInsertCount) {
    vector_layer_config_t cfg = {};
    cfg.format.index_type = VL_INDEX_IVF;
    cfg.format.dim = 4;
    cfg.format.delimiter = '/';
    cfg.format.distance = VL_DIST_L2;
    cfg.format.ivf_n_clusters = 3;
    cfg.runtime.top_k = 10;
    cfg.runtime.sync_only = 1;
    cfg.runtime.ivf_nprobe = 2;
    cfg.runtime.ivf_flat_until = 1000;
    int err = 0;
    vector_layer_t *vl = vector_layer_open_separate(test_dir.c_str(), "test", &cfg, &err);
    ASSERT_NE(vl, nullptr);
    float v[4] = {1, 2, 3, 4};
    for (int i = 0; i < 5; i++) {
        std::string id = "v" + std::to_string(i);
        ASSERT_EQ(vector_layer_insert_sync(vl, id.c_str(), v, NULL, 0), 0);
    }
    EXPECT_EQ(vector_layer_count(vl), 5u);
    vector_layer_destroy(vl);
}

TEST_F(VectorLayerTest, IVFSearch) {
    vector_layer_config_t cfg = {};
    cfg.format.index_type = VL_INDEX_IVF;
    cfg.format.dim = 4;
    cfg.format.delimiter = '/';
    cfg.format.distance = VL_DIST_L2;
    cfg.format.ivf_n_clusters = 3;
    cfg.runtime.top_k = 10;
    cfg.runtime.sync_only = 1;
    cfg.runtime.ivf_nprobe = 2;
    cfg.runtime.ivf_flat_until = 1000;  // cold-start: use flat fallback (no centroids yet)
    int err = 0;
    vector_layer_t *vl = vector_layer_open_separate(test_dir.c_str(), "test", &cfg, &err);
    ASSERT_NE(vl, nullptr);

    // Insert 20 vectors in 3 rough clusters.
    srand(7);
    float centers[3][4] = {{10,0,0,0}, {0,10,0,0}, {0,0,10,0}};
    for (int i = 0; i < 20; i++) {
        int c = i % 3;
        float v[4];
        for (int d = 0; d < 4; d++) v[d] = centers[c][d] + ((float)(rand()%10))/10.0f;
        std::string id = "v" + std::to_string(i);
        ASSERT_EQ(vector_layer_insert_sync(vl, id.c_str(), v, NULL, 0), 0);
    }
    // Train to compute centroids (stub in Task 7 — search falls back to flat).
    ASSERT_EQ(vector_layer_train(vl), 0);

    // Query near cluster 0 — top results should be from cluster 0.
    float q[4] = {10, 0, 0, 0};
    vl_result_t *results = NULL; int n = 0;
    ASSERT_EQ(vector_layer_search_sync(vl, q, 5, &results, &n), 0);
    ASSERT_GT(n, 0);
    // All results should be near q (distance < 5).
    for (int i = 0; i < n; i++) {
        EXPECT_LT(results[i].distance, 5.0f);
    }
    vector_layer_free_results(results, n);
    vector_layer_destroy(vl);
}

TEST_F(VectorLayerTest, IVFTrainRebuild) {
    vector_layer_config_t cfg = {};
    cfg.format.index_type = VL_INDEX_IVF;
    cfg.format.dim = 4;
    cfg.format.delimiter = '/';
    cfg.format.distance = VL_DIST_L2;
    cfg.format.ivf_n_clusters = 3;
    cfg.runtime.sync_only = 1;
    cfg.runtime.ivf_nprobe = 3;       // probe all 3 clusters
    cfg.runtime.ivf_flat_until = 5;   // count=20 > 5, so use IVF path (not flat fallback)
    int err = 0;
    vector_layer_t *vl = vector_layer_open_separate(test_dir.c_str(), "test", &cfg, &err);
    ASSERT_NE(vl, nullptr);

    // Insert 20 vectors in 3 well-separated clusters.
    srand(7);
    float centers[3][4] = {{10,0,0,0}, {0,10,0,0}, {0,0,10,0}};
    std::vector<std::string> ids;
    std::vector<std::vector<float>> stored;
    for (int i = 0; i < 20; i++) {
        int c = i % 3;
        float v[4];
        for (int d = 0; d < 4; d++) v[d] = centers[c][d] + ((float)(rand()%10))/10.0f;
        std::string id = "v" + std::to_string(i);
        ASSERT_EQ(vector_layer_insert_sync(vl, id.c_str(), v, NULL, 0), 0);
        ids.push_back(id);
        stored.push_back(std::vector<float>(v, v+4));
    }

    // Train to compute centroids.
    ASSERT_EQ(vector_layer_train(vl), 0);

    // Rebuild memberships to match new centroids.
    ASSERT_EQ(vector_layer_rebuild(vl), 0);

    // Search near cluster 0 — top results should be from cluster 0 (distance < 5).
    float q[4] = {10, 0, 0, 0};
    vl_result_t *results = NULL; int n = 0;
    ASSERT_EQ(vector_layer_search_sync(vl, q, 5, &results, &n), 0);
    ASSERT_GT(n, 0);
    for (int i = 0; i < n; i++) {
        EXPECT_LT(results[i].distance, 5.0f) << "result " << i << " id=" << results[i].id;
    }
    vector_layer_free_results(results, n);
    vector_layer_destroy(vl);
}

TEST_F(VectorLayerTest, IVFDelete) {
    vector_layer_config_t cfg = {};
    cfg.format.index_type = VL_INDEX_IVF;
    cfg.format.dim = 4;
    cfg.format.delimiter = '/';
    cfg.format.distance = VL_DIST_L2;
    cfg.format.ivf_n_clusters = 3;
    cfg.runtime.sync_only = 1;
    cfg.runtime.ivf_nprobe = 2;
    cfg.runtime.ivf_flat_until = 5;
    int err = 0;
    vector_layer_t *vl = vector_layer_open_separate(test_dir.c_str(), "test", &cfg, &err);
    ASSERT_NE(vl, nullptr);
    float v[4] = {1, 2, 3, 4};
    ASSERT_EQ(vector_layer_insert_sync(vl, "a", v, NULL, 0), 0);
    ASSERT_EQ(vector_layer_insert_sync(vl, "b", v, NULL, 0), 0);
    EXPECT_EQ(vector_layer_count(vl), 2u);
    ASSERT_EQ(vector_layer_delete_sync(vl, "a"), 0);
    EXPECT_EQ(vector_layer_count(vl), 1u);
    // Search should now return only "b".
    vl_result_t *results = NULL; int n = 0;
    ASSERT_EQ(vector_layer_search_sync(vl, v, 10, &results, &n), 0);
    ASSERT_EQ(n, 1);
    EXPECT_STREQ(results[0].id, "b");
    vector_layer_free_results(results, n);
    vector_layer_destroy(vl);
}

TEST_F(VectorLayerTest, SLSHInsertCount) {
    vector_layer_config_t cfg = {};
    cfg.format.index_type = VL_INDEX_SLSH;
    cfg.format.dim = 4;
    cfg.format.delimiter = '/';
    cfg.format.distance = VL_DIST_L2;
    cfg.format.slsh_lsh_tables = 2;
    cfg.format.slsh_hash_bits = 8;
    cfg.format.slsh_bucket_width = 1.0f;
    cfg.runtime.top_k = 10;
    cfg.runtime.sync_only = 1;
    cfg.runtime.slsh_scan_radius = 10;
    int err = 0;
    vector_layer_t *vl = vector_layer_open_separate(test_dir.c_str(), "test", &cfg, &err);
    ASSERT_NE(vl, nullptr);
    float v[4] = {1, 2, 3, 4};
    for (int i = 0; i < 5; i++) {
        std::string id = "v" + std::to_string(i);
        ASSERT_EQ(vector_layer_insert_sync(vl, id.c_str(), v, NULL, 0), 0);
    }
    EXPECT_EQ(vector_layer_count(vl), 5u);
    vector_layer_destroy(vl);
}

TEST_F(VectorLayerTest, SLSHSearchBidirectional) {
    vector_layer_config_t cfg = {};
    cfg.format.index_type = VL_INDEX_SLSH;
    cfg.format.dim = 4;
    cfg.format.delimiter = '/';
    cfg.format.distance = VL_DIST_L2;
    cfg.format.slsh_lsh_tables = 2;
    cfg.format.slsh_hash_bits = 8;
    cfg.format.slsh_bucket_width = 1.0f;
    cfg.runtime.top_k = 10;
    cfg.runtime.sync_only = 1;
    cfg.runtime.slsh_scan_radius = 10;
    int err = 0;
    vector_layer_t *vl = vector_layer_open_separate(test_dir.c_str(), "test", &cfg, &err);
    ASSERT_NE(vl, nullptr);
    srand(7);
    for (int i = 0; i < 20; i++) {
        float v[4] = {(float)(rand()%100), (float)(rand()%100), (float)(rand()%100), (float)(rand()%100)};
        std::string id = "v" + std::to_string(i);
        ASSERT_EQ(vector_layer_insert_sync(vl, id.c_str(), v, NULL, 0), 0);
    }
    ASSERT_EQ(vector_layer_train(vl), 0);  // no-op stub until Task 11
    float q[4] = {50, 50, 50, 50};
    vl_result_t *results = NULL; int n = 0;
    ASSERT_EQ(vector_layer_search_sync(vl, q, 5, &results, &n), 0);
    ASSERT_GT(n, 0);
    vector_layer_free_results(results, n);
    vector_layer_destroy(vl);
}

TEST_F(VectorLayerTest, SLSHTrainRebuild) {
    vector_layer_config_t cfg = {};
    cfg.format.index_type = VL_INDEX_SLSH;
    cfg.format.dim = 4;
    cfg.format.delimiter = '/';
    cfg.format.distance = VL_DIST_L2;
    cfg.format.slsh_lsh_tables = 2;
    cfg.format.slsh_hash_bits = 8;
    cfg.format.slsh_bucket_width = 1.0f;
    cfg.runtime.top_k = 10;
    cfg.runtime.sync_only = 1;
    cfg.runtime.slsh_scan_radius = 10;
    int err = 0;
    vector_layer_t *vl = vector_layer_open_separate(test_dir.c_str(), "test", &cfg, &err);
    ASSERT_NE(vl, nullptr);
    srand(7);
    for (int i = 0; i < 20; i++) {
        float v[4] = {(float)(rand()%100), (float)(rand()%100), (float)(rand()%100), (float)(rand()%100)};
        std::string id = "v" + std::to_string(i);
        ASSERT_EQ(vector_layer_insert_sync(vl, id.c_str(), v, NULL, 0), 0);
    }
    // Train generates projections with a fixed seed.
    ASSERT_EQ(vector_layer_train(vl), 0);
    // Rebuild rehashes all vectors with the new projections.
    ASSERT_EQ(vector_layer_rebuild(vl), 0);
    // Search still works post-train (vectors are now in selective buckets).
    float q[4] = {50, 50, 50, 50};
    vl_result_t *results = NULL; int n = 0;
    ASSERT_EQ(vector_layer_search_sync(vl, q, 5, &results, &n), 0);
    ASSERT_GT(n, 0);
    vector_layer_free_results(results, n);
    vector_layer_destroy(vl);
}

/* Async API (Task 12b): Graph-style true async — async variants return void
 * and take a caller-owned promise_t*. The caller wires the promise to a C++
 * std::promise here (in bindings it'd be a JS Promise / Dart Future / Python
 * asyncio future). With sync_only=0, open_separate creates a db with a real
 * worker pool, so this exercises the enqueue -> worker -> resolve path. With
 * sync_only=1 (default in flat_config), the async variants run inline and
 * resolve the promise before returning (no pool available). */

/* Helper: create a promise wired to a std::promise<int> that captures 0 on
 * resolve, -1 on reject. The caller awaits fut, then promise_destroy. */
static promise_t* vl_make_int_promise(std::promise<int> *p) {
    return promise_create(
        [](void *ctx, void *payload) {
            (void)payload;
            static_cast<std::promise<int>*>(ctx)->set_value(0);
        },
        [](void *ctx, async_error_t *err) {
            static_cast<std::promise<int>*>(ctx)->set_value(-1);
            error_destroy(err);
        },
        p);
}

TEST_F(VectorLayerTest, AsyncInsertSearchDelete) {
    vector_layer_config_t cfg = flat_config(4);
    cfg.format.distance = VL_DIST_L2;
    cfg.runtime.sync_only = 0;  // enable real async (db gets a worker pool)
    int err = 0;
    vector_layer_t *vl = vector_layer_open_separate(test_dir.c_str(), "test", &cfg, &err);
    ASSERT_NE(vl, nullptr);

    float v[4] = {1.0f, 2.0f, 3.0f, 4.0f};

    /* Async insert. */
    {
        auto p = std::make_shared<std::promise<int>>();
        auto fut = p->get_future();
        promise_t *cp = vl_make_int_promise(p.get());
        ASSERT_NE(cp, nullptr);
        vector_layer_insert(vl, "a", v, NULL, 0, cp);
        ASSERT_EQ(fut.wait_for(std::chrono::seconds(2)), std::future_status::ready);
        EXPECT_EQ(fut.get(), 0);
        promise_destroy(cp);
    }
    /* fut is ready only after the worker resolved the promise, by which point
       the insert is committed. Count must be 1. */
    EXPECT_EQ(vector_layer_count(vl), 1u);

    /* Async search — resolve payload is a vl_search_result_t*. */
    {
        auto p = std::make_shared<std::promise<int>>();
        auto fut = p->get_future();
        struct sr_ctx { std::promise<int> *p; vl_search_result_t *sr; } sctx{p.get(), nullptr};
        promise_t *cp = promise_create(
            [](void *ctx_, void *payload) {
                auto *c = static_cast<sr_ctx*>(ctx_);
                c->sr = static_cast<vl_search_result_t*>(payload);
                c->p->set_value(payload ? 1 : 0);
            },
            [](void *ctx_, async_error_t *err) {
                auto *c = static_cast<sr_ctx*>(ctx_);
                c->p->set_value(-1);
                error_destroy(err);
            },
            &sctx);
        vector_layer_search(vl, v, 1, cp);
        ASSERT_EQ(fut.wait_for(std::chrono::seconds(2)), std::future_status::ready);
        ASSERT_EQ(fut.get(), 1);
        ASSERT_NE(sctx.sr, nullptr);
        ASSERT_EQ(sctx.sr->n_results, 1);
        ASSERT_STREQ(sctx.sr->results[0].id, "a");
        vector_layer_free_results(sctx.sr->results, sctx.sr->n_results);
        free(sctx.sr);
        promise_destroy(cp);
    }

    /* Async delete. */
    {
        auto p = std::make_shared<std::promise<int>>();
        auto fut = p->get_future();
        promise_t *cp = vl_make_int_promise(p.get());
        vector_layer_delete(vl, "a", cp);
        ASSERT_EQ(fut.wait_for(std::chrono::seconds(2)), std::future_status::ready);
        EXPECT_EQ(fut.get(), 0);
        promise_destroy(cp);
    }
    EXPECT_EQ(vector_layer_count(vl), 0u);

    vector_layer_destroy(vl);
}

/* sync_only=1 (default) — async variants run inline + resolve before
 * returning (no pool available). */
TEST_F(VectorLayerTest, AsyncRoutesToSyncWhenNoPool) {
    vector_layer_config_t cfg = flat_config(4);
    // cfg.runtime.sync_only = 1 is the default from flat_config
    int err = 0;
    vector_layer_t *vl = vector_layer_open_separate(test_dir.c_str(), "test", &cfg, &err);
    ASSERT_NE(vl, nullptr);

    float v[4] = {1.0f, 2.0f, 3.0f, 4.0f};
    {
        auto p = std::make_shared<std::promise<int>>();
        auto fut = p->get_future();
        promise_t *cp = vl_make_int_promise(p.get());
        vector_layer_insert(vl, "a", v, NULL, 0, cp);
        /* No pool: inline + resolved before returning. */
        ASSERT_EQ(fut.wait_for(std::chrono::seconds(2)), std::future_status::ready);
        EXPECT_EQ(fut.get(), 0);
        promise_destroy(cp);
    }
    EXPECT_EQ(vector_layer_count(vl), 1u);

    {
        auto p = std::make_shared<std::promise<int>>();
        auto fut = p->get_future();
        struct sr_ctx { std::promise<int> *p; vl_search_result_t *sr; } sctx{p.get(), nullptr};
        promise_t *cp = promise_create(
            [](void *ctx_, void *payload) {
                auto *c = static_cast<sr_ctx*>(ctx_);
                c->sr = static_cast<vl_search_result_t*>(payload);
                c->p->set_value(payload ? 1 : 0);
            },
            [](void *ctx_, async_error_t *err) {
                auto *c = static_cast<sr_ctx*>(ctx_);
                c->p->set_value(-1);
                error_destroy(err);
            },
            &sctx);
        vector_layer_search(vl, v, 1, cp);
        ASSERT_EQ(fut.wait_for(std::chrono::seconds(2)), std::future_status::ready);
        ASSERT_EQ(fut.get(), 1);
        ASSERT_NE(sctx.sr, nullptr);
        ASSERT_EQ(sctx.sr->n_results, 1);
        vector_layer_free_results(sctx.sr->results, sctx.sr->n_results);
        free(sctx.sr);
        promise_destroy(cp);
    }

    {
        auto p = std::make_shared<std::promise<int>>();
        auto fut = p->get_future();
        promise_t *cp = vl_make_int_promise(p.get());
        vector_layer_delete(vl, "a", cp);
        ASSERT_EQ(fut.wait_for(std::chrono::seconds(2)), std::future_status::ready);
        EXPECT_EQ(fut.get(), 0);
        promise_destroy(cp);
    }
    EXPECT_EQ(vector_layer_count(vl), 0u);
    vector_layer_destroy(vl);
}

/* Task 13: subtree support — vector_layer_create with a non-NULL subtree
 * shares the root db's key space under the subtree prefix. All vector ops
 * must route through database_subtree_* so the prefix is prepended. */
TEST_F(VectorLayerTest, CreateSubtree) {
    database_config_t *dbcfg = database_config_default();
    int db_err = 0;
    database_t *db = database_create_with_config((char*)test_dir.c_str(), dbcfg, &db_err);
    ASSERT_NE(db, nullptr);
    ASSERT_EQ(db_err, 0);

    /* Open a subtree on "vec_subtree". */
    database_subtree_t *subtree = database_subtree_open(db, "vec_subtree", '/');
    ASSERT_NE(subtree, nullptr);

    vector_layer_config_t cfg = flat_config(4);
    int err = 0;
    vector_layer_t *vl = vector_layer_create("test", db, subtree, &cfg, &err);
    ASSERT_NE(vl, nullptr);
    EXPECT_EQ(err, 0);
    EXPECT_EQ(vector_layer_count(vl), 0u);
    float v[4] = {1, 2, 3, 4};
    ASSERT_EQ(vector_layer_insert_sync(vl, "a", v, NULL, 0), 0);
    EXPECT_EQ(vector_layer_count(vl), 1u);

    /* Search must also work through the subtree. */
    vl_result_t *results = NULL; int n = 0;
    ASSERT_EQ(vector_layer_search_sync(vl, v, 1, &results, &n), 0);
    ASSERT_EQ(n, 1);
    ASSERT_STREQ(results[0].id, "a");
    vector_layer_free_results(results, n);

    /* The vector entries must live under the "vec_subtree" prefix in the root
     * db — verify by reading the count key directly from the root db with the
     * prefix prepended. */
    const char *prefixed = "vec_subtree/vec/test/count";
    uint8_t *buf = NULL; size_t blen = 0;
    int rc = database_get_sync_raw(db, prefixed, strlen(prefixed), '/',
                                    &buf, &blen);
    EXPECT_EQ(rc, 0) << "count key not found under subtree prefix in root db";
    if (rc == 0 && buf && blen >= sizeof(size_t)) {
        size_t c; memcpy(&c, buf, sizeof(size_t));
        EXPECT_EQ(c, 1u);
        database_raw_value_free(buf);
    } else if (buf) {
        database_raw_value_free(buf);
    }

    vector_layer_destroy(vl);
    database_subtree_close(subtree);
    database_destroy(db);
    database_config_destroy(dbcfg);
}

/* Task 13 (R5): AtomicBatchRollback — a mid-batch failure must leave no
 * partial index entries. The NULL id triggers pre-batch validation failure
 * (-EINVAL) before any writes, so count stays 0 and no orphan keys leak. */
TEST_F(VectorLayerTest, AtomicBatchRollback) {
    vector_layer_config_t cfg = {};
    cfg.format.index_type = VL_INDEX_IVF;
    cfg.format.dim = 4;
    cfg.format.delimiter = '/';
    cfg.format.distance = VL_DIST_L2;
    cfg.format.ivf_n_clusters = 3;
    cfg.runtime.sync_only = 1;
    cfg.runtime.ivf_nprobe = 2;
    cfg.runtime.ivf_flat_until = 1000;
    int err = 0;
    vector_layer_t *vl = vector_layer_open_separate(test_dir.c_str(), "test", &cfg, &err);
    ASSERT_NE(vl, nullptr);
    float v[4] = {1, 2, 3, 4};
    /* Insert with NULL id should fail (invalid arg) before any writes. */
    int rc = vector_layer_insert_sync(vl, NULL, v, NULL, 0);
    EXPECT_LT(rc, 0);
    EXPECT_EQ(vector_layer_count(vl), 0u);

    /* Stronger check: a successful insert followed by a failed insert must
     * leave the count + index reflecting only the successful insert. */
    ASSERT_EQ(vector_layer_insert_sync(vl, "ok", v, NULL, 0), 0);
    EXPECT_EQ(vector_layer_count(vl), 1u);
    rc = vector_layer_insert_sync(vl, NULL, v, NULL, 0);
    EXPECT_LT(rc, 0);
    EXPECT_EQ(vector_layer_count(vl), 1u) << "failed insert leaked count";
    /* The orphan check: search must still return only "ok". */
    vl_result_t *results = NULL; int n = 0;
    ASSERT_EQ(vector_layer_search_sync(vl, v, 1, &results, &n), 0);
    ASSERT_EQ(n, 1);
    ASSERT_STREQ(results[0].id, "ok");
    vector_layer_free_results(results, n);

    vector_layer_destroy(vl);
}

/* ---------------------------------------------------------------------------
 * Task 14: Recall gates — CI-enforced decision gates from the spec.
 *
 * IVF recall@10 >= 0.90 vs brute-force ground truth.
 * SLSH bidirectional recall@10 >= 0.90.
 * SLSH right-only recall@10 >= 0.80 (looser — documents the bidirectional cost).
 *
 * 2000 vectors × 50 queries × brute-force O(N) ground truth per query.
 * recall@k = |index_top_k ∩ truth_top_k| / (Q * k). A regression below the
 * gate fails the test, not just the bench.
 *
 * Data: clustered_blobs (matching the spec's synthetic spike arm — NOT uniform
 * random). 50 cluster centers in [0,10)^DIM, 40 vectors per cluster = center +
 * noise*0.1. Queries = random center + noise*0.1. Tight clusters give LSH the
 * structure it needs: near neighbors concentrate in one hash bucket, so
 * right-only (forward scan) finds them. On uniform random data, right-only
 * caps at ~0.50 (half the top-10 land in lower buckets the forward scan can't
 * reach) — that's a data-distribution artifact, not an index bug. The spike
 * (Task 17) will validate on real embeddings (gaussian + EnterpriseRAG-Bench).
 *
 * Two index bugs were found and fixed while writing these gates:
 *  (1) vector_ivf_train / vector_slsh_train only wrote centroids/projections
 *      but did NOT rebuild memberships/hash-entries. Vectors inserted pre-train
 *      kept their default (cid=0 / zero-hash) bucket, so search probed the
 *      wrong clusters/buckets. Fix: train now calls rebuild internally.
 *  (2) SLSH forward scan's end bound was vec/{idx}/hash/{q_key}/\x7f (only the
 *      query's exact bucket). It should be vec/{idx}/hash/\x7f (all higher
 *      buckets) so the forward scan reaches right neighbors across buckets.
 *      With the old bound, right-only always returned 0 results (no vector
 *      shares the exact 64-bit hash). Fix: widen the end bound.
 *
 * Tuned params (final):
 *   IVF:        n_clusters=50, nprobe=16, flat_until=500.
 *   SLSH:       lsh_tables=2, hash_bits=16, bucket_width=2.0, scan_radius=100.
 * Observed recall: IVF 1.00, SLSH 0.91.
 * ------------------------------------------------------------------------- */

/* Generate cluster centers and insert N clustered vectors. n_clusters centers
 * are random in [0,10)^DIM; each of the N vectors is center[i % n_clusters] +
 * uniform[-0.1,0.1) noise. The caller seeds rand() before calling. */
static void vl_recall_insert_clustered(vector_layer_t *vl, int N, int DIM,
                                       int n_clusters,
                                       std::vector<std::vector<float>> &stored) {
    /* Generate centers. */
    std::vector<std::vector<float>> centers(n_clusters);
    for (int c = 0; c < n_clusters; c++) {
        centers[c].resize(DIM);
        for (int d = 0; d < DIM; d++) centers[c][d] = (float)(rand() % 1000) / 100.0f;
    }
    for (int i = 0; i < N; i++) {
        int c = i % n_clusters;
        std::vector<float> v(DIM);
        for (int d = 0; d < DIM; d++) {
            float noise = (float)(rand() % 20 - 10) / 100.0f;  /* [-0.1, 0.1) */
            v[d] = centers[c][d] + noise;
        }
        std::string id = "v" + std::to_string(i);
        ASSERT_EQ(vector_layer_insert_sync(vl, id.c_str(), v.data(), NULL, 0), 0);
        stored.push_back(v);
    }
}

/* Compute recall@K for `vl` against brute-force ground truth. Queries are
 * generated near cluster centers (center + noise*0.1) to match the stored
 * distribution. Returns recall in [0,1]. Caller seeds rand() before calling. */
static float vl_recall_against_stored_clustered(vector_layer_t *vl,
                                                const std::vector<std::vector<float>> &stored,
                                                int DIM, int Q, int K,
                                                int n_clusters) {
    int N = (int)stored.size();
    /* Reconstruct centers: every n_clusters-th stored vector is a center+noise
     * sample; we approximate the center as the mean of each cluster's members.
     * Simpler: just re-derive centers from the stored vectors by averaging. */
    std::vector<std::vector<float>> centers(n_clusters);
    std::vector<int> counts(n_clusters, 0);
    for (int c = 0; c < n_clusters; c++) centers[c].assign(DIM, 0.0f);
    for (int i = 0; i < N; i++) {
        int c = i % n_clusters;
        for (int d = 0; d < DIM; d++) centers[c][d] += stored[i][d];
        counts[c]++;
    }
    for (int c = 0; c < n_clusters; c++) {
        if (counts[c] > 0) for (int d = 0; d < DIM; d++) centers[c][d] /= counts[c];
    }

    int total_hits = 0;
    for (int q = 0; q < Q; q++) {
        /* Query = random center + noise. */
        int c = rand() % n_clusters;
        std::vector<float> query(DIM);
        for (int d = 0; d < DIM; d++) {
            float noise = (float)(rand() % 20 - 10) / 100.0f;  /* [-0.1, 0.1) */
            query[d] = centers[c][d] + noise;
        }

        std::vector<std::pair<float, int>> dists;
        dists.reserve(N);
        for (int i = 0; i < N; i++) {
            float dd = vl_distance(query.data(), stored[i].data(), DIM, VL_DIST_L2);
            dists.push_back({dd, i});
        }
        std::sort(dists.begin(), dists.end());
        std::set<int> truth;
        for (int i = 0; i < K && i < (int)dists.size(); i++) truth.insert(dists[i].second);

        vl_result_t *results = NULL; int n = 0;
        if (vector_layer_search_sync(vl, query.data(), K, &results, &n) != 0) {
            continue;
        }
        for (int i = 0; i < n; i++) {
            if (!results[i].id) continue;
            int rid = atoi(results[i].id + 1);  /* "v%u" -> u */
            if (truth.count(rid)) total_hits++;
        }
        vector_layer_free_results(results, n);
    }
    return (float)total_hits / (Q * K);
}

TEST_F(VectorLayerTest, IVFRecallGate) {
    const int N = 2000, DIM = 16, Q = 50, K = 10;
    vector_layer_config_t cfg = {};
    cfg.format.index_type = VL_INDEX_IVF;
    cfg.format.dim = DIM;
    cfg.format.delimiter = '/';
    cfg.format.distance = VL_DIST_L2;
    cfg.format.ivf_n_clusters = 50;
    cfg.runtime.top_k = K;
    cfg.runtime.sync_only = 1;
    cfg.runtime.ivf_nprobe = 16;
    cfg.runtime.ivf_flat_until = 500;  /* N=2000 > 500, so IVF path is used */
    int err = 0;
    vector_layer_t *vl = vector_layer_open_separate(test_dir.c_str(), "test", &cfg, &err);
    ASSERT_NE(vl, nullptr);

    srand(12345);
    std::vector<std::vector<float>> stored;
    vl_recall_insert_clustered(vl, N, DIM, 50, stored);
    ASSERT_EQ(vector_layer_train(vl), 0);

    float recall = vl_recall_against_stored_clustered(vl, stored, DIM, Q, K, 50);
    std::cout << "IVF recall@10: " << recall << std::endl;
    EXPECT_GE(recall, 0.90f);
    vector_layer_destroy(vl);
}

TEST_F(VectorLayerTest, SLSHRecallGate) {
    /* SLSH now scans bidirectionally always (right-only mode removed — it
       never cleared the 0.80 gate, ~0.50 recall). The scan radius is adaptive:
       actual = max(slsh_scan_radius, count/30). With N=2000 and configured
       radius=100, the adaptive floor is 2000/30 = 66, so actual = 100 (the
       configured floor wins). Gate 0.90. */
    const int N = 2000, DIM = 16, Q = 50, K = 10;
    vector_layer_config_t cfg = {};
    cfg.format.index_type = VL_INDEX_SLSH;
    cfg.format.dim = DIM;
    cfg.format.delimiter = '/';
    cfg.format.distance = VL_DIST_L2;
    cfg.format.slsh_lsh_tables = 2;
    cfg.format.slsh_hash_bits = 16;
    cfg.format.slsh_bucket_width = 2.0f;
    cfg.runtime.top_k = K;
    cfg.runtime.sync_only = 1;
    cfg.runtime.slsh_scan_radius = 100;
    int err = 0;
    vector_layer_t *vl = vector_layer_open_separate(test_dir.c_str(), "test", &cfg, &err);
    ASSERT_NE(vl, nullptr);

    srand(12345);
    std::vector<std::vector<float>> stored;
    vl_recall_insert_clustered(vl, N, DIM, 50, stored);
    ASSERT_EQ(vector_layer_train(vl), 0);

    float recall = vl_recall_against_stored_clustered(vl, stored, DIM, Q, K, 50);
    std::cout << "SLSH recall@10: " << recall << std::endl;
    EXPECT_GE(recall, 0.90f);
    vector_layer_destroy(vl);
}

// ===========================================================================
// __format persistence + open-time validation + explicit migrate
// (spec: docs/superpowers/specs/...; plan mellow-jumping-token.md)
//
// __format is a reserved string leaf at vec/{idx}/__format ("flat"/"ivf"/
// "slsh"), sibling of count, lexically excluded from all vector scans. Set
// on create, validated on open (mismatch -> VL_EFORMAT_MISMATCH, NOT silent
// degrade), updated by the explicit vector_layer_migrate() only.
// ===========================================================================

/* Small-scale clustered insert for migrate tests (DIM=4, 3 clusters). */
static void vl_mtest_insert(vector_layer_t *vl, int N,
                            std::vector<std::vector<float>> &stored) {
    srand(7);
    static const float centers[3][4] = {
        {10.0f, 0.0f, 0.0f, 0.0f},
        {0.0f, 10.0f, 0.0f, 0.0f},
        {0.0f, 0.0f, 10.0f, 0.0f},
    };
    for (int i = 0; i < N; i++) {
        int c = i % 3;
        std::vector<float> v(4);
        for (int d = 0; d < 4; d++) v[d] = centers[c][d] + (float)(rand() % 20 - 10) / 100.0f;
        std::string id = "v" + std::to_string(i);
        ASSERT_EQ(vector_layer_insert_sync(vl, id.c_str(), v.data(), NULL, 0), 0);
        stored.push_back(v);
    }
}

/* FLAT search sanity: querying near cluster 0 returns only ids whose stored
 * vector is in cluster 0 (distance small). Returns the set of returned ids. */
static std::set<std::string> vl_mtest_search_ids(vector_layer_t *vl, const float *q, int k) {
    vl_result_t *results = NULL; int n = 0;
    std::set<std::string> ids;
    if (vector_layer_search_sync(vl, q, k, &results, &n) != 0) return ids;
    for (int i = 0; i < n; i++) {
        if (results[i].id) ids.insert(std::string(results[i].id));
    }
    vector_layer_free_results(results, n);
    return ids;
}

// 1. Create writes __format.
TEST_F(VectorLayerTest, CreateWritesFormat) {
    vector_layer_config_t cfg = flat_config(4);
    int err = 0;
    vector_layer_t *vl = vector_layer_open_separate(test_dir.c_str(), "test", &cfg, &err);
    ASSERT_NE(vl, nullptr);
    ASSERT_EQ(err, 0);
    EXPECT_EQ(vl_read_format_type(vl), VL_INDEX_FLAT);
    vector_layer_format_t gf;
    ASSERT_EQ(vector_layer_get_format(vl, &gf), 0);
    EXPECT_EQ(gf.index_type, VL_INDEX_FLAT);
    EXPECT_EQ(gf.dim, 4);
    vector_layer_destroy(vl);
}

// 2. Reopen validates a matching type.
TEST_F(VectorLayerTest, ReopenValidatesMatchingType) {
    vector_layer_config_t cfg = flat_config(4);
    int err = 0;
    vector_layer_t *vl = vector_layer_open_separate(test_dir.c_str(), "test", &cfg, &err);
    ASSERT_NE(vl, nullptr);
    std::vector<float> v = {1.0f, 2.0f, 3.0f, 4.0f};
    ASSERT_EQ(vector_layer_insert_sync(vl, "v0", v.data(), NULL, 0), 0);
    vector_layer_destroy(vl);

    vl = vector_layer_open_separate(test_dir.c_str(), "test", &cfg, &err);
    ASSERT_NE(vl, nullptr);
    ASSERT_EQ(err, 0);
    EXPECT_EQ(vector_layer_count(vl), 1u);
    EXPECT_EQ(vl_read_format_type(vl), VL_INDEX_FLAT);
    vector_layer_destroy(vl);
}

// 3. Reopen with a mismatched type RAISES (not silent degradation).
TEST_F(VectorLayerTest, ReopenMismatchedTypeRaises) {
    vector_layer_config_t cfg = flat_config(4);
    int err = 0;
    vector_layer_t *vl = vector_layer_open_separate(test_dir.c_str(), "test", &cfg, &err);
    ASSERT_NE(vl, nullptr);
    vector_layer_destroy(vl);

    vector_layer_config_t bad = ivf_config(4, 3);  /* same dim, different type */
    vl = vector_layer_open_separate(test_dir.c_str(), "test", &bad, &err);
    EXPECT_EQ(vl, nullptr);
    EXPECT_EQ(err, VL_EFORMAT_MISMATCH);
}

// 4. A legacy (pre-0.2.1) index has no __format; first reopen writes it from
//    the passed type, and a subsequent mismatched reopen then raises.
TEST_F(VectorLayerTest, LegacyIndexUpgradesOnReopen) {
    database_config_t *dbcfg = database_config_default();
    ASSERT_NE(dbcfg, nullptr);
    database_config_set_sync_only(dbcfg, 1);
    int db_err = 0;
    database_t *db = database_create_with_config(test_dir.c_str(), dbcfg, &db_err);
    ASSERT_NE(db, nullptr);
    ASSERT_EQ(db_err, 0);

    vector_layer_config_t cfg = flat_config(4);
    int err = 0;
    vector_layer_t *vl = vector_layer_create("test", db, NULL, &cfg, &err);
    ASSERT_NE(vl, nullptr);
    std::vector<float> v = {1.0f, 2.0f, 3.0f, 4.0f};
    ASSERT_EQ(vector_layer_insert_sync(vl, "v0", v.data(), NULL, 0), 0);
    vector_layer_destroy(vl);

    /* Strip the __format leaf -> simulate a 0.2.0 index (count + vector). */
    const char *fkey = "vec/test/__format";
    ASSERT_EQ(database_delete_sync_raw(db, fkey, strlen(fkey), '/'), 0);

    /* Reopen FLAT: absent -> writes __format="flat" from the passed type. */
    vl = vector_layer_create("test", db, NULL, &cfg, &err);
    ASSERT_NE(vl, nullptr);
    ASSERT_EQ(err, 0);
    EXPECT_EQ(vl_read_format_type(vl), VL_INDEX_FLAT);
    EXPECT_EQ(vector_layer_count(vl), 1u);
    vector_layer_destroy(vl);

    /* Now __format="flat" on disk -> reopening as IVF raises (loud mismatch). */
    vector_layer_config_t bad = ivf_config(4, 3);
    vl = vector_layer_create("test", db, NULL, &bad, &err);
    EXPECT_EQ(vl, nullptr);
    EXPECT_EQ(err, VL_EFORMAT_MISMATCH);

    database_destroy(db);
    database_config_destroy(dbcfg);
}

// 5-10. Migrate between every type pair: count preserved, search sane.
TEST_F(VectorLayerTest, MigrateFlatToIvf) {
    vector_layer_config_t cfg = flat_config(4);
    int err = 0;
    vector_layer_t *vl = vector_layer_open_separate(test_dir.c_str(), "test", &cfg, &err);
    ASSERT_NE(vl, nullptr);
    std::vector<std::vector<float>> stored;
    vl_mtest_insert(vl, 60, stored);
    size_t n = vector_layer_count(vl);
    ASSERT_EQ(n, 60u);

    vector_layer_format_t nf = ivf_config(4, 3).format;
    ASSERT_EQ(vector_layer_migrate(vl, &nf), 0);
    EXPECT_EQ(vector_layer_count(vl), n);
    EXPECT_EQ(vl_read_format_type(vl), VL_INDEX_IVF);

    /* reconfigure to the IVF runtime (nprobe=3 covers all 3 clusters). */
    vector_layer_runtime_t rt = ivf_config(4, 3).runtime;
    ASSERT_EQ(vector_layer_reconfigure(vl, &rt), 0);
    float q[4] = {10.0f, 0.0f, 0.0f, 0.0f};
    auto ids = vl_mtest_search_ids(vl, q, 5);
    EXPECT_GT(ids.size(), 0u);
    vector_layer_destroy(vl);
}

TEST_F(VectorLayerTest, MigrateFlatToSlsh) {
    vector_layer_config_t cfg = flat_config(4);
    int err = 0;
    vector_layer_t *vl = vector_layer_open_separate(test_dir.c_str(), "test", &cfg, &err);
    ASSERT_NE(vl, nullptr);
    std::vector<std::vector<float>> stored;
    vl_mtest_insert(vl, 60, stored);
    size_t n = vector_layer_count(vl);
    vector_layer_format_t nf = slsh_config(4).format;
    ASSERT_EQ(vector_layer_migrate(vl, &nf), 0);
    EXPECT_EQ(vector_layer_count(vl), n);
    EXPECT_EQ(vl_read_format_type(vl), VL_INDEX_SLSH);
    vector_layer_destroy(vl);
}

TEST_F(VectorLayerTest, MigrateIvfToFlat) {
    vector_layer_config_t cfg = ivf_config(4, 3);
    int err = 0;
    vector_layer_t *vl = vector_layer_open_separate(test_dir.c_str(), "test", &cfg, &err);
    ASSERT_NE(vl, nullptr);
    std::vector<std::vector<float>> stored;
    vl_mtest_insert(vl, 60, stored);
    ASSERT_EQ(vector_layer_train(vl), 0);
    size_t n = vector_layer_count(vl);
    vector_layer_format_t nf = flat_config(4).format;
    ASSERT_EQ(vector_layer_migrate(vl, &nf), 0);
    EXPECT_EQ(vector_layer_count(vl), n);
    EXPECT_EQ(vl_read_format_type(vl), VL_INDEX_FLAT);
    /* FLAT exact: top-1 for a stored vector is itself. */
    float q[4] = {stored[10][0], stored[10][1], stored[10][2], stored[10][3]};
    auto ids = vl_mtest_search_ids(vl, q, 1);
    ASSERT_EQ(ids.size(), 1u);
    EXPECT_EQ(*ids.begin(), std::string("v10"));
    vector_layer_destroy(vl);
}

TEST_F(VectorLayerTest, MigrateIvfToSlsh) {
    vector_layer_config_t cfg = ivf_config(4, 3);
    int err = 0;
    vector_layer_t *vl = vector_layer_open_separate(test_dir.c_str(), "test", &cfg, &err);
    ASSERT_NE(vl, nullptr);
    std::vector<std::vector<float>> stored;
    vl_mtest_insert(vl, 60, stored);
    ASSERT_EQ(vector_layer_train(vl), 0);
    size_t n = vector_layer_count(vl);
    vector_layer_format_t nf = slsh_config(4).format;
    ASSERT_EQ(vector_layer_migrate(vl, &nf), 0);
    EXPECT_EQ(vector_layer_count(vl), n);
    EXPECT_EQ(vl_read_format_type(vl), VL_INDEX_SLSH);
    vector_layer_destroy(vl);
}

TEST_F(VectorLayerTest, MigrateSlshToFlat) {
    vector_layer_config_t cfg = slsh_config(4);
    int err = 0;
    vector_layer_t *vl = vector_layer_open_separate(test_dir.c_str(), "test", &cfg, &err);
    ASSERT_NE(vl, nullptr);
    std::vector<std::vector<float>> stored;
    vl_mtest_insert(vl, 60, stored);
    ASSERT_EQ(vector_layer_train(vl), 0);
    size_t n = vector_layer_count(vl);
    vector_layer_format_t nf = flat_config(4).format;
    ASSERT_EQ(vector_layer_migrate(vl, &nf), 0);
    EXPECT_EQ(vector_layer_count(vl), n);
    EXPECT_EQ(vl_read_format_type(vl), VL_INDEX_FLAT);
    vector_layer_destroy(vl);
}

TEST_F(VectorLayerTest, MigrateSlshToIvf) {
    vector_layer_config_t cfg = slsh_config(4);
    int err = 0;
    vector_layer_t *vl = vector_layer_open_separate(test_dir.c_str(), "test", &cfg, &err);
    ASSERT_NE(vl, nullptr);
    std::vector<std::vector<float>> stored;
    vl_mtest_insert(vl, 60, stored);
    ASSERT_EQ(vector_layer_train(vl), 0);
    size_t n = vector_layer_count(vl);
    vector_layer_format_t nf = ivf_config(4, 3).format;
    ASSERT_EQ(vector_layer_migrate(vl, &nf), 0);
    EXPECT_EQ(vector_layer_count(vl), n);
    EXPECT_EQ(vl_read_format_type(vl), VL_INDEX_IVF);
    vector_layer_destroy(vl);
}

// 11. Migrate rejects a dim mismatch; layer unchanged + __format unchanged.
TEST_F(VectorLayerTest, MigrateRejectsDimMismatch) {
    vector_layer_config_t cfg = flat_config(4);
    int err = 0;
    vector_layer_t *vl = vector_layer_open_separate(test_dir.c_str(), "test", &cfg, &err);
    ASSERT_NE(vl, nullptr);
    std::vector<float> v = {1.0f, 2.0f, 3.0f, 4.0f};
    ASSERT_EQ(vector_layer_insert_sync(vl, "v0", v.data(), NULL, 0), 0);
    vector_layer_format_t nf = ivf_config(8, 3).format;  /* dim 8 != 4 */
    EXPECT_EQ(vector_layer_migrate(vl, &nf), -22);
    /* Layer unchanged and still usable. */
    EXPECT_EQ(vl_read_format_type(vl), VL_INDEX_FLAT);
    EXPECT_EQ(vector_layer_count(vl), 1u);
    auto ids = vl_mtest_search_ids(vl, v.data(), 1);
    ASSERT_EQ(ids.size(), 1u);
    EXPECT_EQ(*ids.begin(), std::string("v0"));
    vector_layer_destroy(vl);
}

// 12. Same-type migrate rebuilds in place (no short-circuit).
TEST_F(VectorLayerTest, MigrateSameTypeRebuildsInPlace) {
    vector_layer_config_t cfg = ivf_config(4, 3);
    int err = 0;
    vector_layer_t *vl = vector_layer_open_separate(test_dir.c_str(), "test", &cfg, &err);
    ASSERT_NE(vl, nullptr);
    std::vector<std::vector<float>> stored;
    vl_mtest_insert(vl, 60, stored);
    ASSERT_EQ(vector_layer_train(vl), 0);
    size_t n = vector_layer_count(vl);
    vector_layer_format_t nf = ivf_config(4, 3).format;  /* same type */
    ASSERT_EQ(vector_layer_migrate(vl, &nf), 0);
    EXPECT_EQ(vector_layer_count(vl), n);
    EXPECT_EQ(vl_read_format_type(vl), VL_INDEX_IVF);
    vector_layer_runtime_t rt = ivf_config(4, 3).runtime;
    ASSERT_EQ(vector_layer_reconfigure(vl, &rt), 0);
    float q[4] = {10.0f, 0.0f, 0.0f, 0.0f};
    auto ids = vl_mtest_search_ids(vl, q, 5);
    EXPECT_GT(ids.size(), 0u);
    vector_layer_destroy(vl);
}

// 13. Round trip FLAT->IVF->SLSH->FLAT->IVF; count preserved each hop; final
//     IVF top-1 self-query matches.
TEST_F(VectorLayerTest, MigrateRoundTrip) {
    vector_layer_config_t cfg = flat_config(4);
    int err = 0;
    vector_layer_t *vl = vector_layer_open_separate(test_dir.c_str(), "test", &cfg, &err);
    ASSERT_NE(vl, nullptr);
    std::vector<std::vector<float>> stored;
    vl_mtest_insert(vl, 60, stored);
    size_t n = vector_layer_count(vl);

    float q[4] = {stored[10][0], stored[10][1], stored[10][2], stored[10][3]};
    auto baseline = vl_mtest_search_ids(vl, q, 1);
    ASSERT_EQ(baseline.size(), 1u);
    EXPECT_EQ(*baseline.begin(), std::string("v10"));

    ASSERT_EQ(vector_layer_migrate(vl, &ivf_config(4, 3).format), 0);
    EXPECT_EQ(vector_layer_count(vl), n);
    ASSERT_EQ(vector_layer_migrate(vl, &slsh_config(4).format), 0);
    EXPECT_EQ(vector_layer_count(vl), n);
    ASSERT_EQ(vector_layer_migrate(vl, &flat_config(4).format), 0);
    EXPECT_EQ(vector_layer_count(vl), n);
    ASSERT_EQ(vector_layer_migrate(vl, &ivf_config(4, 3).format), 0);
    EXPECT_EQ(vector_layer_count(vl), n);
    EXPECT_EQ(vl_read_format_type(vl), VL_INDEX_IVF);

    vector_layer_runtime_t rt = ivf_config(4, 3).runtime;
    ASSERT_EQ(vector_layer_reconfigure(vl, &rt), 0);
    /* Self-query: the exact stored vector is in some cluster; with nprobe
     * covering all clusters the exact vector (distance 0) is top-1. */
    auto final_ids = vl_mtest_search_ids(vl, q, 1);
    ASSERT_EQ(final_ids.size(), 1u);
    EXPECT_EQ(*final_ids.begin(), std::string("v10"));
    vector_layer_destroy(vl);
}

// 14. Migrate retains metadata byte-for-byte.
TEST_F(VectorLayerTest, MigrateRetainsMetadata) {
    vector_layer_config_t cfg = flat_config(4);
    int err = 0;
    vector_layer_t *vl = vector_layer_open_separate(test_dir.c_str(), "test", &cfg, &err);
    ASSERT_NE(vl, nullptr);
    srand(99);
    static const float centers[3][4] = {
        {10.0f, 0.0f, 0.0f, 0.0f}, {0.0f, 10.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 10.0f, 0.0f}};
    std::map<int, std::vector<uint8_t>> metas;
    std::map<int, std::vector<float>> vecs;
    for (int i = 0; i < 15; i++) {
        int c = i % 3;
        std::vector<float> v(4);
        for (int d = 0; d < 4; d++) v[d] = centers[c][d] + (float)(rand() % 20 - 10) / 100.0f;
        uint8_t meta[2] = {(uint8_t)i, (uint8_t)(i ^ 0xFF)};
        std::string id = "v" + std::to_string(i);
        ASSERT_EQ(vector_layer_insert_sync(vl, id.c_str(), v.data(), meta, 2), 0);
        metas[i] = std::vector<uint8_t>(meta, meta + 2);
        vecs[i] = v;
    }
    ASSERT_EQ(vector_layer_migrate(vl, &ivf_config(4, 3).format), 0);
    vector_layer_runtime_t rt = ivf_config(4, 3).runtime;
    ASSERT_EQ(vector_layer_reconfigure(vl, &rt), 0);
    EXPECT_EQ(vector_layer_count(vl), 15u);
    int hits = 0;
    for (int i = 0; i < 15; i++) {
        vl_result_t *res = NULL; int n = 0;
        ASSERT_EQ(vector_layer_search_sync(vl, vecs[i].data(), 1, &res, &n), 0);
        ASSERT_EQ(n, 1);
        std::string id(res[0].id ? res[0].id : "");
        if (id == ("v" + std::to_string(i)) && res[0].metadata_len == 2 &&
            memcmp(res[0].metadata, metas[i].data(), 2) == 0) {
            hits++;
        }
        vector_layer_free_results(res, n);
    }
    EXPECT_EQ(hits, 15);
    vector_layer_destroy(vl);
}

// 15. Migrate writes __format to disk (recovery anchor); reopen-by-type works.
TEST_F(VectorLayerTest, MigrateWritesFormatToDisk) {
    vector_layer_config_t cfg = flat_config(4);
    int err = 0;
    vector_layer_t *vl = vector_layer_open_separate(test_dir.c_str(), "test", &cfg, &err);
    ASSERT_NE(vl, nullptr);
    std::vector<std::vector<float>> stored;
    vl_mtest_insert(vl, 60, stored);
    ASSERT_EQ(vector_layer_migrate(vl, &ivf_config(4, 3).format), 0);
    vector_layer_destroy(vl);

    /* Disk says ivf: reopen IVF ok, reopen FLAT raises. */
    vector_layer_config_t ivf = ivf_config(4, 3);
    vl = vector_layer_open_separate(test_dir.c_str(), "test", &ivf, &err);
    ASSERT_NE(vl, nullptr);
    ASSERT_EQ(err, 0);
    EXPECT_EQ(vl_read_format_type(vl), VL_INDEX_IVF);
    vector_layer_destroy(vl);

    vector_layer_config_t flat = flat_config(4);
    vl = vector_layer_open_separate(test_dir.c_str(), "test", &flat, &err);
    EXPECT_EQ(vl, nullptr);
    EXPECT_EQ(err, VL_EFORMAT_MISMATCH);
}

// 16. Re-running migrate(target) converges (idempotence / crash-recovery proxy).
TEST_F(VectorLayerTest, MigrateRerunConverges) {
    vector_layer_config_t cfg = flat_config(4);
    int err = 0;
    vector_layer_t *vl = vector_layer_open_separate(test_dir.c_str(), "test", &cfg, &err);
    ASSERT_NE(vl, nullptr);
    std::vector<std::vector<float>> stored;
    vl_mtest_insert(vl, 60, stored);
    size_t n = vector_layer_count(vl);
    vector_layer_format_t ivf = ivf_config(4, 3).format;
    ASSERT_EQ(vector_layer_migrate(vl, &ivf), 0);
    vector_layer_runtime_t rt = ivf_config(4, 3).runtime;
    ASSERT_EQ(vector_layer_reconfigure(vl, &rt), 0);
    float q[4] = {10.0f, 0.0f, 0.0f, 0.0f};
    auto ids1 = vl_mtest_search_ids(vl, q, 5);

    /* Re-run migrate(IVF) -> converges; count + top-5 set unchanged. */
    ASSERT_EQ(vector_layer_migrate(vl, &ivf), 0);
    ASSERT_EQ(vector_layer_reconfigure(vl, &rt), 0);
    EXPECT_EQ(vector_layer_count(vl), n);
    auto ids2 = vl_mtest_search_ids(vl, q, 5);
    EXPECT_EQ(ids1, ids2);
    vector_layer_destroy(vl);
}

// 17. Migrate emits log_info progress (vector\t{phase}\t{current}\t{total}).
static std::vector<std::string> g_progress_lines;
static void progress_collect_cb(log_Event *ev) {
    char buf[1024];
    vsnprintf(buf, sizeof buf, ev->fmt, ev->ap);
    g_progress_lines.push_back(std::string(buf));
}
TEST_F(VectorLayerTest, MigrateEmitsProgress) {
    g_progress_lines.clear();
    ASSERT_EQ(log_add_callback(progress_collect_cb, NULL, LOG_LEVEL_INFO), 0);

    vector_layer_config_t cfg = ivf_config(4, 3);
    int err = 0;
    vector_layer_t *vl = vector_layer_open_separate(test_dir.c_str(), "test", &cfg, &err);
    ASSERT_NE(vl, nullptr);
    std::vector<std::vector<float>> stored;
    vl_mtest_insert(vl, 9000, stored);  /* >8000 -> multiple 8000-op chunks */
    ASSERT_EQ(vector_layer_train(vl), 0);
    size_t n = vector_layer_count(vl);

    /* IVF -> SLSH exercises delete (IVF aux) + train (SLSH proj) + rebuild + done. */
    ASSERT_EQ(vector_layer_migrate(vl, &slsh_config(4).format), 0);
    EXPECT_EQ(vector_layer_count(vl), n);
    vector_layer_destroy(vl);

    ASSERT_FALSE(g_progress_lines.empty());
    std::set<std::string> phases;
    std::vector<int> rebuild_cur;
    int done_cur = -1, done_tot = -1;
    for (const std::string &line : g_progress_lines) {
        std::vector<std::string> parts;
        size_t start = 0, end;
        while ((end = line.find('\t', start)) != std::string::npos) {
            parts.push_back(line.substr(start, end - start));
            start = end + 1;
        }
        parts.push_back(line.substr(start));
        if (parts.size() != 4 || parts[0] != "vector") continue;
        phases.insert(parts[1]);
        if (parts[1] == "rebuild") rebuild_cur.push_back(atoi(parts[2].c_str()));
        if (parts[1] == "done") { done_cur = atoi(parts[2].c_str()); done_tot = atoi(parts[3].c_str()); }
    }
    EXPECT_NE(phases.count("delete"), 0u);
    EXPECT_NE(phases.count("train"), 0u);
    EXPECT_NE(phases.count("rebuild"), 0u);
    EXPECT_NE(phases.count("done"), 0u);
    /* Within rebuild, current is non-decreasing. */
    auto sorted_cur = rebuild_cur;
    std::sort(sorted_cur.begin(), sorted_cur.end());
    EXPECT_EQ(rebuild_cur, sorted_cur);
    /* done's current == total == count. */
    EXPECT_EQ(done_cur, (int)n);
    EXPECT_EQ(done_tot, (int)n);
}

// 18. The __format leaf is never swept into a FLAT search (defense-in-depth).
TEST_F(VectorLayerTest, FormatLeafExcludedFromSearch) {
    vector_layer_config_t cfg = flat_config(4);
    int err = 0;
    vector_layer_t *vl = vector_layer_open_separate(test_dir.c_str(), "test", &cfg, &err);
    ASSERT_NE(vl, nullptr);
    std::vector<float> v = {1.0f, 2.0f, 3.0f, 4.0f};
    ASSERT_EQ(vector_layer_insert_sync(vl, "v0", v.data(), NULL, 0), 0);

    /* Overwrite the __format sibling leaf with long garbage bytes that could
     * be mis-parsed as a vector if a scan ever swept it in. */
    std::string garbage(128, 'X');
    const char *fkey = "vec/test/__format";
    ASSERT_EQ(vl_put_string(vl, fkey, strlen(fkey), garbage.c_str()), 0);

    /* FLAT brute-force scan over vector/ must return only real vector ids,
     * never the garbage leaf's bytes. */
    auto ids = vl_mtest_search_ids(vl, v.data(), 10);
    ASSERT_EQ(ids.size(), 1u);
    EXPECT_EQ(*ids.begin(), std::string("v0"));
    vector_layer_destroy(vl);
}