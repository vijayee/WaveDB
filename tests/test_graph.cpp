//
// Tests for Graph Schema Layer
//

#include <gtest/gtest.h>
#include <string.h>
#include <stdlib.h>
#include <string>
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
#endif

extern "C" {
#include "../src/Layers/graph/graph.h"
#include "../src/Layers/graph/graph_internal.h"
}

static int test_counter = 0;

class GraphLayerTest : public ::testing::Test {
protected:
    void SetUp() override {
        test_dir = "/tmp/wavedb_graph_test_" + std::to_string(getpid()) + "_" + std::to_string(test_counter++);
        mkdir(test_dir.c_str(), 0700);

        // graph_layer_create uses the default config path; with pool/wheel it works.
        // We need to provide pool and wheel because the default config doesn't set
        // worker_threads > 0 or sync_only.
        layer = graph_layer_create(test_dir.c_str(), NULL);
        ASSERT_NE(layer, nullptr);
    }

    void TearDown() override {
        graph_layer_destroy(layer);

        // Clean up test directory
        std::string cmd = "rm -rf " + test_dir;
        system(cmd.c_str());
    }

    std::string test_dir;
    graph_layer_t* layer = nullptr;
};

TEST_F(GraphLayerTest, CreateDestroy) {
    ASSERT_NE(layer, nullptr);
    database_t* db = graph_layer_get_db(layer);
    ASSERT_NE(db, nullptr);
}

TEST_F(GraphLayerTest, InsertAndDelete) {
    int rc = graph_insert_sync(layer, "clip_abc", "tagged_with", "gaming");
    ASSERT_EQ(rc, 0);

    rc = graph_insert_sync(layer, "clip_abc", "tagged_with", "tutorial");
    ASSERT_EQ(rc, 0);

    rc = graph_insert_sync(layer, "clip_abc", "created_by", "alice");
    ASSERT_EQ(rc, 0);

    rc = graph_delete_sync(layer, "clip_abc", "tagged_with", "tutorial");
    ASSERT_EQ(rc, 0);
}

TEST_F(GraphLayerTest, SimpleOutTraversal) {
    graph_insert_sync(layer, "clip_abc", "tagged_with", "gaming");
    graph_insert_sync(layer, "clip_abc", "tagged_with", "tutorial");
    graph_insert_sync(layer, "clip_abc", "created_by", "alice");

    graph_query_t* q = graph_query_create(layer);
    graph_query_vertex(q, "clip_abc");
    graph_query_out(q, "tagged_with");

    graph_result_t* r = graph_query_execute_sync(q);
    ASSERT_NE(r, nullptr);
    ASSERT_EQ(graph_result_count(r), (size_t)2);

    const char* const* verts = graph_result_vertices(r);
    bool found_gaming = false, found_tutorial = false;
    for (size_t i = 0; i < graph_result_count(r); i++) {
        if (strcmp(verts[i], "gaming") == 0) found_gaming = true;
        if (strcmp(verts[i], "tutorial") == 0) found_tutorial = true;
    }
    EXPECT_TRUE(found_gaming);
    EXPECT_TRUE(found_tutorial);

    graph_result_destroy(r);
    graph_query_destroy(q);
}

TEST_F(GraphLayerTest, SimpleInTraversal) {
    graph_insert_sync(layer, "clip_abc", "tagged_with", "gaming");
    graph_insert_sync(layer, "clip_xyz", "tagged_with", "gaming");

    graph_query_t* q = graph_query_create(layer);
    graph_query_vertex(q, "gaming");
    graph_query_in(q, "tagged_with");

    graph_result_t* r = graph_query_execute_sync(q);
    ASSERT_NE(r, nullptr);
    ASSERT_EQ(graph_result_count(r), (size_t)2);

    const char* const* verts = graph_result_vertices(r);
    bool found_abc = false, found_xyz = false;
    for (size_t i = 0; i < graph_result_count(r); i++) {
        if (strcmp(verts[i], "clip_abc") == 0) found_abc = true;
        if (strcmp(verts[i], "clip_xyz") == 0) found_xyz = true;
    }
    EXPECT_TRUE(found_abc);
    EXPECT_TRUE(found_xyz);

    graph_result_destroy(r);
    graph_query_destroy(q);
}

TEST_F(GraphLayerTest, MultiHopTraversal) {
    graph_insert_sync(layer, "alice", "follows", "bob");
    graph_insert_sync(layer, "bob", "likes", "clip_abc");

    graph_query_t* q = graph_query_create(layer);
    graph_query_vertex(q, "alice");
    graph_query_out(q, "follows");
    graph_query_out(q, "likes");

    graph_result_t* r = graph_query_execute_sync(q);
    ASSERT_NE(r, nullptr);
    ASSERT_EQ(graph_result_count(r), (size_t)1);
    EXPECT_STREQ(graph_result_vertices(r)[0], "clip_abc");

    graph_result_destroy(r);
    graph_query_destroy(q);
}

TEST_F(GraphLayerTest, Intersection) {
    graph_insert_sync(layer, "clip_abc", "tagged_with", "gaming");
    graph_insert_sync(layer, "clip_abc", "tagged_with", "tutorial");
    graph_insert_sync(layer, "clip_xyz", "tagged_with", "gaming");

    graph_query_t* q1 = graph_query_create(layer);
    graph_query_vertex(q1, "gaming");
    graph_query_in(q1, "tagged_with");

    graph_query_t* q2 = graph_query_create(layer);
    graph_query_vertex(q2, "tutorial");
    graph_query_in(q2, "tagged_with");

    graph_query_t* q = graph_query_create(layer);
    graph_query_intersect(q, q1, q2);

    graph_result_t* r = graph_query_execute_sync(q);
    ASSERT_NE(r, nullptr);
    ASSERT_EQ(graph_result_count(r), (size_t)1);
    EXPECT_STREQ(graph_result_vertices(r)[0], "clip_abc");

    graph_result_destroy(r);
    graph_query_destroy(q1);
    graph_query_destroy(q2);
    graph_query_destroy(q);
}

TEST_F(GraphLayerTest, Union) {
    graph_insert_sync(layer, "clip_abc", "tagged_with", "gaming");
    graph_insert_sync(layer, "clip_xyz", "tagged_with", "tutorial");

    graph_query_t* q1 = graph_query_create(layer);
    graph_query_vertex(q1, "gaming");
    graph_query_in(q1, "tagged_with");

    graph_query_t* q2 = graph_query_create(layer);
    graph_query_vertex(q2, "tutorial");
    graph_query_in(q2, "tagged_with");

    graph_query_t* q = graph_query_create(layer);
    graph_query_union(q, q1, q2);

    graph_result_t* r = graph_query_execute_sync(q);
    ASSERT_NE(r, nullptr);
    ASSERT_EQ(graph_result_count(r), (size_t)2);

    graph_result_destroy(r);
    graph_query_destroy(q1);
    graph_query_destroy(q2);
    graph_query_destroy(q);
}

TEST_F(GraphLayerTest, Limit) {
    graph_insert_sync(layer, "clip_abc", "tagged_with", "gaming");
    graph_insert_sync(layer, "clip_xyz", "tagged_with", "gaming");

    graph_query_t* q = graph_query_create(layer);
    graph_query_vertex(q, "gaming");
    graph_query_in(q, "tagged_with");
    graph_query_limit(q, 1);

    graph_result_t* r = graph_query_execute_sync(q);
    ASSERT_NE(r, nullptr);
    ASSERT_EQ(graph_result_count(r), (size_t)1);

    graph_result_destroy(r);
    graph_query_destroy(q);
}

TEST_F(GraphLayerTest, EmptyResult) {
    graph_query_t* q = graph_query_create(layer);
    graph_query_vertex(q, "nonexistent");
    graph_query_out(q, "unknown");

    graph_result_t* r = graph_query_execute_sync(q);
    ASSERT_NE(r, nullptr);
    ASSERT_EQ(graph_result_count(r), (size_t)0);

    graph_result_destroy(r);
    graph_query_destroy(q);
}

TEST_F(GraphLayerTest, IntersectionEmpty) {
    graph_insert_sync(layer, "clip_abc", "tagged_with", "gaming");

    graph_query_t* q1 = graph_query_create(layer);
    graph_query_vertex(q1, "gaming");
    graph_query_in(q1, "tagged_with");

    graph_query_t* q2 = graph_query_create(layer);
    graph_query_vertex(q2, "nonexistent");
    graph_query_in(q2, "tagged_with");

    graph_query_t* q = graph_query_create(layer);
    graph_query_intersect(q, q1, q2);

    graph_result_t* r = graph_query_execute_sync(q);
    ASSERT_NE(r, nullptr);
    ASSERT_EQ(graph_result_count(r), (size_t)0);

    graph_result_destroy(r);
    graph_query_destroy(q1);
    graph_query_destroy(q2);
    graph_query_destroy(q);
}

TEST_F(GraphLayerTest, IntersectionThenOut) {
    graph_insert_sync(layer, "clip_abc", "tagged_with", "gaming");
    graph_insert_sync(layer, "clip_abc", "created_by", "alice");
    graph_insert_sync(layer, "clip_xyz", "tagged_with", "gaming");
    graph_insert_sync(layer, "clip_xyz", "created_by", "bob");

    graph_query_t* q1 = graph_query_create(layer);
    graph_query_vertex(q1, "gaming");
    graph_query_in(q1, "tagged_with");

    graph_query_t* q2 = graph_query_create(layer);
    graph_query_vertex(q2, "tutorial");
    graph_query_in(q2, "tagged_with");

    graph_query_t* q = graph_query_create(layer);
    graph_query_intersect(q, q1, q2);
    graph_query_out(q, "created_by");

    graph_result_t* r = graph_query_execute_sync(q);
    ASSERT_NE(r, nullptr);
    ASSERT_EQ(graph_result_count(r), (size_t)0);

    graph_result_destroy(r);
    graph_query_destroy(q1);
    graph_query_destroy(q2);
    graph_query_destroy(q);
}

TEST_F(GraphLayerTest, AsyncInsertExecutesInline) {
    auto p = std::make_shared<std::promise<int>>();
    auto fut = p->get_future();

    promise_t* cpromise = promise_create(
        [](void* ctx, void* payload) {
            auto* prom = static_cast<std::promise<int>*>(ctx);
            prom->set_value(*(int*)payload);
        },
        [](void* ctx, async_error_t* err) {
            auto* prom = static_cast<std::promise<int>*>(ctx);
            prom->set_value(-1);
            error_destroy(err);
        },
        p.get()
    );

    graph_insert(layer, "clip_abc", "tagged_with", "gaming", cpromise);

    auto status = fut.wait_for(std::chrono::seconds(1));
    ASSERT_EQ(status, std::future_status::ready);
    int result = fut.get();
    EXPECT_EQ(result, 0);
    promise_destroy(cpromise);

    graph_query_t* q = graph_query_create(layer);
    graph_query_vertex(q, "clip_abc");
    graph_query_out(q, "tagged_with");
    graph_result_t* r = graph_query_execute_sync(q);
    ASSERT_NE(r, nullptr);
    ASSERT_EQ(graph_result_count(r), (size_t)1);
    EXPECT_STREQ(graph_result_vertices(r)[0], "gaming");
    graph_result_destroy(r);
    graph_query_destroy(q);
}
