// Tests for Vector Layer
// Spec: docs/superpowers/specs/2026-07-12-vector-layer-design.md
//
// Task 1: skeleton — header, lifecycle, config, dispatch, distance functions.
// 4 lifecycle tests: create/destroy (separate + shared), reconfigure, invalid dim.

#include <gtest/gtest.h>
#include <string.h>
#include <stdlib.h>
#include <string>

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