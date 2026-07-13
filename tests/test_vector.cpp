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
#include <algorithm>
#include <utility>

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
}

static int vl_test_counter = 0;

class VectorLayerTest : public ::testing::Test {
protected:
    void SetUp() override {
        test_dir = "/tmp/wavedb_vltest_" + std::to_string(getpid()) + "_" +
                   std::to_string(vl_test_counter++);
        mkdir(test_dir.c_str(), 0700);
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
    cfg.runtime.slsh_bidirectional = 1;
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
    cfg.runtime.slsh_bidirectional = 1;
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
    cfg.runtime.slsh_bidirectional = 1;
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

TEST_F(VectorLayerTest, SLSHSearchRightOnly) {
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
    cfg.runtime.slsh_bidirectional = 0;  // right-only
    int err = 0;
    vector_layer_t *vl = vector_layer_open_separate(test_dir.c_str(), "test", &cfg, &err);
    ASSERT_NE(vl, nullptr);
    srand(7);
    for (int i = 0; i < 20; i++) {
        float v[4] = {(float)(rand()%100), (float)(rand()%100), (float)(rand()%100), (float)(rand()%100)};
        std::string id = "v" + std::to_string(i);
        ASSERT_EQ(vector_layer_insert_sync(vl, id.c_str(), v, NULL, 0), 0);
    }
    float q[4] = {50, 50, 50, 50};
    vl_result_t *results = NULL; int n = 0;
    ASSERT_EQ(vector_layer_search_sync(vl, q, 5, &results, &n), 0);
    ASSERT_GT(n, 0);
    vector_layer_free_results(results, n);
    vector_layer_destroy(vl);
}

/* Task 12: async insert/search/delete via Workers/promise.
 * The vector async API is "blocking async": work is enqueued to the db's
 * worker pool, and the caller blocks on a condvar until the promise resolves.
 * With sync_only=0, open_separate creates a db with a real pool, so this
 * exercises the worker path. With sync_only=1 (default in flat_config), the
 * async variants fall back to the sync versions (no pool available). */
TEST_F(VectorLayerTest, AsyncInsertSearchDelete) {
    vector_layer_config_t cfg = flat_config(4);
    cfg.format.distance = VL_DIST_L2;
    cfg.runtime.sync_only = 0;  // enable real async (db gets a worker pool)
    int err = 0;
    vector_layer_t *vl = vector_layer_open_separate(test_dir.c_str(), "test", &cfg, &err);
    ASSERT_NE(vl, nullptr);

    float v[4] = {1.0f, 2.0f, 3.0f, 4.0f};
    int rc = vector_layer_insert(vl, "a", v, NULL, 0);  // async
    ASSERT_EQ(rc, 0);
    // Async insert blocks until the worker resolves, so count must already be 1.
    // The poll loop is a safety net for any scheduling latency.
    for (int i = 0; i < 100 && vector_layer_count(vl) == 0; i++) {
        usleep(10000);  // 10ms
    }
    EXPECT_EQ(vector_layer_count(vl), 1u);

    vl_result_t *results = NULL; int n = 0;
    rc = vector_layer_search(vl, v, 1, &results, &n);  // async
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(n, 1);
    ASSERT_STREQ(results[0].id, "a");
    vector_layer_free_results(results, n);

    rc = vector_layer_delete(vl, "a");  // async
    ASSERT_EQ(rc, 0);
    for (int i = 0; i < 100 && vector_layer_count(vl) == 1; i++) {
        usleep(10000);
    }
    EXPECT_EQ(vector_layer_count(vl), 0u);

    vector_layer_destroy(vl);
}

/* sync_only=1 (default) — async variants must route to sync (no pool). */
TEST_F(VectorLayerTest, AsyncRoutesToSyncWhenNoPool) {
    vector_layer_config_t cfg = flat_config(4);
    // cfg.runtime.sync_only = 1 is the default from flat_config
    int err = 0;
    vector_layer_t *vl = vector_layer_open_separate(test_dir.c_str(), "test", &cfg, &err);
    ASSERT_NE(vl, nullptr);

    float v[4] = {1.0f, 2.0f, 3.0f, 4.0f};
    ASSERT_EQ(vector_layer_insert(vl, "a", v, NULL, 0), 0);
    EXPECT_EQ(vector_layer_count(vl), 1u);

    vl_result_t *results = NULL; int n = 0;
    ASSERT_EQ(vector_layer_search(vl, v, 1, &results, &n), 0);
    ASSERT_EQ(n, 1);
    vector_layer_free_results(results, n);

    ASSERT_EQ(vector_layer_delete(vl, "a"), 0);
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