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

TEST_F(GraphLayerTest, Difference) {
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
    graph_query_difference(q, q1, q2);

    graph_result_t* r = graph_query_execute_sync(q);
    ASSERT_NE(r, nullptr);
    ASSERT_EQ(graph_result_count(r), (size_t)1);
    EXPECT_STREQ(graph_result_vertices(r)[0], "clip_xyz");

    graph_result_destroy(r);
    graph_query_destroy(q1);
    graph_query_destroy(q2);
    graph_query_destroy(q);
}

TEST_F(GraphLayerTest, DifferenceEmptyResult) {
    graph_insert_sync(layer, "clip_abc", "tagged_with", "gaming");
    graph_insert_sync(layer, "clip_abc", "tagged_with", "tutorial");

    graph_query_t* q1 = graph_query_create(layer);
    graph_query_vertex(q1, "gaming");
    graph_query_in(q1, "tagged_with");

    graph_query_t* q2 = graph_query_create(layer);
    graph_query_vertex(q2, "tutorial");
    graph_query_in(q2, "tagged_with");

    graph_query_t* q = graph_query_create(layer);
    graph_query_difference(q, q1, q2);

    graph_result_t* r = graph_query_execute_sync(q);
    ASSERT_NE(r, nullptr);
    ASSERT_EQ(graph_result_count(r), (size_t)0);

    graph_result_destroy(r);
    graph_query_destroy(q1);
    graph_query_destroy(q2);
    graph_query_destroy(q);
}

TEST_F(GraphLayerTest, MorphismFollow) {
    graph_insert_sync(layer, "alice", "follows", "bob");
    graph_insert_sync(layer, "bob", "likes", "clip_abc");

    graph_parse_error_t err;
    int rc = graph_morphism_define(layer, "friends_content",
        "g.Morphism(\"friends_content\").Out(\"follows\").Out(\"likes\")", &err);
    ASSERT_EQ(rc, 0);

    graph_query_t* q = graph_query_create(layer);
    graph_query_vertex(q, "alice");
    graph_query_follow(q, "friends_content");

    graph_result_t* r = graph_query_execute_sync(q);
    ASSERT_NE(r, nullptr);
    ASSERT_EQ(graph_result_count(r), (size_t)1);
    EXPECT_STREQ(graph_result_vertices(r)[0], "clip_abc");

    graph_result_destroy(r);
    graph_query_destroy(q);
}

TEST_F(GraphLayerTest, MorphismFollowMultipleResults) {
    graph_insert_sync(layer, "alice", "follows", "bob");
    graph_insert_sync(layer, "bob", "likes", "clip_abc");
    graph_insert_sync(layer, "bob", "likes", "clip_xyz");

    graph_parse_error_t err;
    int rc = graph_morphism_define(layer, "friends_content",
        "g.Morphism(\"friends_content\").Out(\"follows\").Out(\"likes\")", &err);
    ASSERT_EQ(rc, 0);

    graph_query_t* q = graph_query_create(layer);
    graph_query_vertex(q, "alice");
    graph_query_follow(q, "friends_content");

    graph_result_t* r = graph_query_execute_sync(q);
    ASSERT_NE(r, nullptr);
    ASSERT_EQ(graph_result_count(r), (size_t)2);

    graph_result_destroy(r);
    graph_query_destroy(q);
}

TEST_F(GraphLayerTest, AsyncInsertExecutesInline) {
    auto p = std::make_shared<std::promise<int>>();
    auto fut = p->get_future();

    promise_t* cpromise = promise_create(
        [](void* ctx, void* payload) {
            auto* prom = static_cast<std::promise<int>*>(ctx);
            // Async insert/delete resolves with NULL on success
            prom->set_value(payload ? *(int*)payload : 0);
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

TEST_F(GraphLayerTest, HasEqualityFilter) {
    graph_insert_sync(layer, "clip_abc", "tagged_with", "gaming");
    graph_insert_sync(layer, "clip_abc", "tagged_with", "tutorial");
    graph_insert_sync(layer, "clip_xyz", "tagged_with", "gaming");

    graph_query_t* q = graph_query_create(layer);
    graph_query_has(q, "tagged_with", "gaming");

    graph_result_t* r = graph_query_execute_sync(q);
    ASSERT_NE(r, nullptr);
    ASSERT_EQ(graph_result_count(r), (size_t)2);

    bool found_abc = false, found_xyz = false;
    const char* const* verts = graph_result_vertices(r);
    for (size_t i = 0; i < graph_result_count(r); i++) {
        if (strcmp(verts[i], "clip_abc") == 0) found_abc = true;
        if (strcmp(verts[i], "clip_xyz") == 0) found_xyz = true;
    }
    EXPECT_TRUE(found_abc);
    EXPECT_TRUE(found_xyz);

    graph_result_destroy(r);
    graph_query_destroy(q);
}

TEST_F(GraphLayerTest, HasGtFilter) {
    graph_insert_sync(layer, "clip_abc", "version", "001");
    graph_insert_sync(layer, "clip_xyz", "version", "003");
    graph_insert_sync(layer, "clip_def", "version", "005");

    graph_query_t* q = graph_query_create(layer);
    graph_query_has_cmp(q, "version", "001", GRAPH_CMP_GT);

    graph_result_t* r = graph_query_execute_sync(q);
    ASSERT_NE(r, nullptr);
    ASSERT_EQ(graph_result_count(r), (size_t)2);

    bool found_xyz = false, found_def = false;
    const char* const* verts = graph_result_vertices(r);
    for (size_t i = 0; i < graph_result_count(r); i++) {
        if (strcmp(verts[i], "clip_xyz") == 0) found_xyz = true;
        if (strcmp(verts[i], "clip_def") == 0) found_def = true;
    }
    EXPECT_TRUE(found_xyz);
    EXPECT_TRUE(found_def);

    graph_result_destroy(r);
    graph_query_destroy(q);
}

TEST_F(GraphLayerTest, HasLtFilter) {
    graph_insert_sync(layer, "clip_abc", "version", "001");
    graph_insert_sync(layer, "clip_xyz", "version", "003");
    graph_insert_sync(layer, "clip_def", "version", "005");

    graph_query_t* q = graph_query_create(layer);
    graph_query_has_cmp(q, "version", "005", GRAPH_CMP_LT);

    graph_result_t* r = graph_query_execute_sync(q);
    ASSERT_NE(r, nullptr);
    ASSERT_EQ(graph_result_count(r), (size_t)2);

    bool found_abc = false, found_xyz = false;
    const char* const* verts = graph_result_vertices(r);
    for (size_t i = 0; i < graph_result_count(r); i++) {
        if (strcmp(verts[i], "clip_abc") == 0) found_abc = true;
        if (strcmp(verts[i], "clip_xyz") == 0) found_xyz = true;
    }
    EXPECT_TRUE(found_abc);
    EXPECT_TRUE(found_xyz);

    graph_result_destroy(r);
    graph_query_destroy(q);
}

TEST_F(GraphLayerTest, HasGteFilter) {
    graph_insert_sync(layer, "clip_abc", "version", "001");
    graph_insert_sync(layer, "clip_xyz", "version", "003");

    graph_query_t* q = graph_query_create(layer);
    graph_query_has_cmp(q, "version", "003", GRAPH_CMP_GTE);

    graph_result_t* r = graph_query_execute_sync(q);
    ASSERT_NE(r, nullptr);
    ASSERT_EQ(graph_result_count(r), (size_t)1);
    EXPECT_STREQ(graph_result_vertices(r)[0], "clip_xyz");

    graph_result_destroy(r);
    graph_query_destroy(q);
}

TEST_F(GraphLayerTest, HasLteFilter) {
    graph_insert_sync(layer, "clip_abc", "version", "001");
    graph_insert_sync(layer, "clip_xyz", "version", "003");

    graph_query_t* q = graph_query_create(layer);
    graph_query_has_cmp(q, "version", "001", GRAPH_CMP_LTE);

    graph_result_t* r = graph_query_execute_sync(q);
    ASSERT_NE(r, nullptr);
    ASSERT_EQ(graph_result_count(r), (size_t)1);
    EXPECT_STREQ(graph_result_vertices(r)[0], "clip_abc");

    graph_result_destroy(r);
    graph_query_destroy(q);
}

TEST_F(GraphLayerTest, HasRangeWithInput) {
    graph_insert_sync(layer, "clip_abc", "version", "001");
    graph_insert_sync(layer, "clip_xyz", "version", "003");
    graph_insert_sync(layer, "clip_def", "version", "005");

    // V("clip_abc").Has("version", >, "000") should intersect {clip_abc} with {clip_abc, clip_xyz, clip_def}
    graph_query_t* q = graph_query_create(layer);
    graph_query_vertex(q, "clip_abc");
    graph_query_has_cmp(q, "version", "000", GRAPH_CMP_GT);

    graph_result_t* r = graph_query_execute_sync(q);
    ASSERT_NE(r, nullptr);
    ASSERT_EQ(graph_result_count(r), (size_t)1);
    EXPECT_STREQ(graph_result_vertices(r)[0], "clip_abc");

    graph_result_destroy(r);
    graph_query_destroy(q);
}

TEST_F(GraphLayerTest, HasRangeParserGt) {
    graph_insert_sync(layer, "clip_abc", "version", "001");
    graph_insert_sync(layer, "clip_xyz", "version", "003");

    graph_parse_error_t err;
    graph_query_t* q = graph_parse("g.Has(\"version\", >, \"002\")", layer, &err);
    ASSERT_NE(q, nullptr);

    graph_result_t* r = graph_query_execute_sync(q);
    ASSERT_NE(r, nullptr);
    ASSERT_EQ(graph_result_count(r), (size_t)1);
    EXPECT_STREQ(graph_result_vertices(r)[0], "clip_xyz");

    graph_result_destroy(r);
    graph_query_destroy(q);
}

TEST_F(GraphLayerTest, HasRangeParserGte) {
    graph_insert_sync(layer, "clip_abc", "version", "001");
    graph_insert_sync(layer, "clip_xyz", "version", "003");

    graph_parse_error_t err;
    graph_query_t* q = graph_parse("g.Has(\"version\", >=, \"003\")", layer, &err);
    ASSERT_NE(q, nullptr);

    graph_result_t* r = graph_query_execute_sync(q);
    ASSERT_NE(r, nullptr);
    ASSERT_EQ(graph_result_count(r), (size_t)1);
    EXPECT_STREQ(graph_result_vertices(r)[0], "clip_xyz");

    graph_result_destroy(r);
    graph_query_destroy(q);
}

TEST_F(GraphLayerTest, HasRangeParserLt) {
    graph_insert_sync(layer, "clip_abc", "version", "001");
    graph_insert_sync(layer, "clip_xyz", "version", "003");

    graph_parse_error_t err;
    graph_query_t* q = graph_parse("g.Has(\"version\", <, \"003\")", layer, &err);
    ASSERT_NE(q, nullptr);

    graph_result_t* r = graph_query_execute_sync(q);
    ASSERT_NE(r, nullptr);
    ASSERT_EQ(graph_result_count(r), (size_t)1);
    EXPECT_STREQ(graph_result_vertices(r)[0], "clip_abc");

    graph_result_destroy(r);
    graph_query_destroy(q);
}

TEST_F(GraphLayerTest, HasRangeParserLte) {
    graph_insert_sync(layer, "clip_abc", "version", "001");
    graph_insert_sync(layer, "clip_xyz", "version", "003");

    graph_parse_error_t err;
    graph_query_t* q = graph_parse("g.Has(\"version\", <=, \"001\")", layer, &err);
    ASSERT_NE(q, nullptr);

    graph_result_t* r = graph_query_execute_sync(q);
    ASSERT_NE(r, nullptr);
    ASSERT_EQ(graph_result_count(r), (size_t)1);
    EXPECT_STREQ(graph_result_vertices(r)[0], "clip_abc");

    graph_result_destroy(r);
    graph_query_destroy(q);
}

TEST_F(GraphLayerTest, HasOutFusion) {
    graph_insert_sync(layer, "clip_abc", "tagged_with", "gaming");
    graph_insert_sync(layer, "clip_abc", "created_by", "alice");
    graph_insert_sync(layer, "clip_xyz", "tagged_with", "gaming");
    graph_insert_sync(layer, "clip_xyz", "created_by", "bob");
    graph_insert_sync(layer, "clip_def", "tagged_with", "tutorial");
    graph_insert_sync(layer, "clip_def", "created_by", "charlie");

    graph_query_t* q = graph_query_create(layer);
    graph_query_vertex(q, "clip_abc");
    graph_query_has(q, "tagged_with", "gaming");
    graph_query_out(q, "created_by");

    graph_result_t* r = graph_query_execute_sync(q);
    ASSERT_NE(r, nullptr);
    ASSERT_EQ(graph_result_count(r), (size_t)1);
    EXPECT_STREQ(graph_result_vertices(r)[0], "alice");

    graph_result_destroy(r);
    graph_query_destroy(q);
}

TEST_F(GraphLayerTest, HasInFusion) {
    // Verify that fused Has+In produces the same result as separate Has then In.
    // Data: alice follows bob, bob likes gaming, alice likes gaming
    graph_insert_sync(layer, "alice", "follows", "bob");
    graph_insert_sync(layer, "bob", "likes", "gaming");
    graph_insert_sync(layer, "alice", "likes", "gaming");

    // Unfused path: Has("likes", "gaming") -> In("follows")
    // Has finds {bob, alice} (both like gaming)
    // In("follows") from {bob, alice} finds who follows them
    //   /pos/follows/bob/ -> alice, /pos/follows/alice/ -> nobody
    // Result: {alice}
    graph_query_t* q = graph_query_create(layer);
    graph_query_has(q, "likes", "gaming");
    graph_query_in(q, "follows");

    graph_result_t* r = graph_query_execute_sync(q);
    ASSERT_NE(r, nullptr);
    ASSERT_EQ(graph_result_count(r), (size_t)1);
    EXPECT_STREQ(graph_result_vertices(r)[0], "alice");

    graph_result_destroy(r);
    graph_query_destroy(q);
}

TEST_F(GraphLayerTest, HasOutFusionSameResultsAsUnfused) {
    graph_insert_sync(layer, "a", "status", "active");
    graph_insert_sync(layer, "a", "edge", "x");
    graph_insert_sync(layer, "b", "status", "inactive");
    graph_insert_sync(layer, "b", "edge", "x");
    graph_insert_sync(layer, "c", "status", "active");
    graph_insert_sync(layer, "c", "edge", "y");

    graph_query_t* q = graph_query_create(layer);
    graph_query_has(q, "status", "active");
    graph_query_out(q, "edge");

    graph_result_t* r = graph_query_execute_sync(q);
    ASSERT_NE(r, nullptr);
    ASSERT_EQ(graph_result_count(r), (size_t)2);

    bool found_x = false, found_y = false;
    for (size_t i = 0; i < graph_result_count(r); i++) {
        if (strcmp(graph_result_vertices(r)[i], "x") == 0) found_x = true;
        if (strcmp(graph_result_vertices(r)[i], "y") == 0) found_y = true;
    }
    EXPECT_TRUE(found_x);
    EXPECT_TRUE(found_y);

    graph_result_destroy(r);
    graph_query_destroy(q);
}
// This test was replaced - see above for the corrected version

TEST_F(GraphLayerTest, HasReorderMostSelectiveFirst) {
    // Insert data where "name" is very selective and "type" is very common
    graph_insert_sync(layer, "clip_abc", "type", "video");
    graph_insert_sync(layer, "clip_xyz", "type", "video");
    graph_insert_sync(layer, "clip_def", "type", "video");
    graph_insert_sync(layer, "clip_abc", "name", "MyClip");
    graph_insert_sync(layer, "clip_abc", "tagged_with", "gaming");

    // g.Has("name", "MyClip").Has("type", "video") should produce {clip_abc}
    // The optimizer should reorder so the more selective Has runs first
    graph_parse_error_t err;
    graph_query_t* q = graph_parse("g.Has(\"name\", \"MyClip\").Has(\"type\", \"video\")", layer, &err);
    ASSERT_NE(q, nullptr);
    graph_result_t* r = graph_query_execute_sync(q);
    ASSERT_NE(r, nullptr);
    ASSERT_EQ(graph_result_count(r), (size_t)1);
    EXPECT_STREQ(graph_result_vertices(r)[0], "clip_abc");
    graph_result_destroy(r);
    graph_query_destroy(q);

    // Also test the reverse order (optimizer should produce same result)
    q = graph_parse("g.Has(\"type\", \"video\").Has(\"name\", \"MyClip\")", layer, &err);
    ASSERT_NE(q, nullptr);
    r = graph_query_execute_sync(q);
    ASSERT_NE(r, nullptr);
    ASSERT_EQ(graph_result_count(r), (size_t)1);
    EXPECT_STREQ(graph_result_vertices(r)[0], "clip_abc");
    graph_result_destroy(r);
    graph_query_destroy(q);
}
