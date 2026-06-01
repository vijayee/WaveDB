//
// Tests for Graph Query Optimizer
//

#include <gtest/gtest.h>
#include <string.h>
#include <string>

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
#include "../src/Layers/graph/graph_internal.h"
}

// Helper: allocate a step directly (alloc_step is static in graph.c,
// and graph_query_intersect always sets num_children=2, so we need
// direct construction for single-child tests.)
static query_step_t* make_step(graph_step_type_t type) {
    query_step_t* s = (query_step_t*)get_clear_memory(sizeof(query_step_t));
    s->type = type;
    return s;
}

static int test_counter = 0;

class GraphOptimizerTest : public ::testing::Test {
protected:
    std::string test_dir;
    graph_layer_t* layer;
    void SetUp() override {
        test_dir = "/tmp/wavedb_opt_test_" + std::to_string(getpid()) + "_" + std::to_string(test_counter++);
        mkdir(test_dir.c_str(), 0700);
        layer = graph_layer_create(test_dir.c_str(), NULL);
        ASSERT_NE(layer, nullptr);
    }
    void TearDown() override {
        graph_layer_destroy(layer);
        // Clean up test directory
        std::string cmd = "rm -rf " + test_dir;
        system(cmd.c_str());
    }
};

TEST_F(GraphOptimizerTest, FoldSingleChildIntersect) {
    // Build: VERTEX("clip_abc") -> INTERSECT(1 child: IN("tagged_with"))
    // After folding: VERTEX("clip_abc") -> IN("tagged_with")
    // Child does NOT start with VERTEX, so the fold is valid.
    query_step_t* v1 = make_step(GRAPH_STEP_VERTEX);
    v1->vertex_id = strdup("clip_abc");

    query_step_t* in_step = make_step(GRAPH_STEP_IN);
    in_step->predicate = strdup("tagged_with");

    query_step_t* intersect = make_step(GRAPH_STEP_INTERSECT);
    intersect->num_children = 1;
    intersect->children = (query_step_t**)get_clear_memory(sizeof(query_step_t*));
    intersect->children[0] = in_step;

    v1->next = intersect;

    // Build query for cleanup (graph_query_destroy walks head next chain)
    graph_query_t* q = graph_query_create(layer);
    q->head = v1;
    q->tail = in_step;

    graph_optimize(&q->head);

    ASSERT_NE(q->head, nullptr);
    ASSERT_EQ(q->head->type, GRAPH_STEP_VERTEX);
    ASSERT_NE(q->head->next, nullptr);
    ASSERT_EQ(q->head->next->type, GRAPH_STEP_IN);
    ASSERT_STREQ(q->head->next->predicate, "tagged_with");
    ASSERT_EQ(q->head->next->next, nullptr);

    graph_query_destroy(q);
}

TEST_F(GraphOptimizerTest, NoChangeForTwoChildIntersect) {
    // Two different children: optimizer should leave the INTERSECT alone
    graph_query_t* q = graph_query_create(layer);
    graph_query_vertex(q, "clip_abc");

    graph_query_t* left = graph_query_create(layer);
    graph_query_vertex(left, "gaming");
    graph_query_in(left, "tagged_with");

    graph_query_t* right = graph_query_create(layer);
    graph_query_vertex(right, "tutorial");
    graph_query_in(right, "tagged_with");

    graph_query_intersect(q, left, right);

    ASSERT_EQ(q->head->next->type, GRAPH_STEP_INTERSECT);
    ASSERT_EQ(q->head->next->num_children, (size_t)2);

    graph_optimize(&q->head);

    ASSERT_EQ(q->head->next->type, GRAPH_STEP_INTERSECT);
    ASSERT_EQ(q->head->next->num_children, (size_t)2);

    graph_query_destroy(q);
    // left and right are now empty shells (head/tail = NULL), safe to destroy
    graph_query_destroy(left);
    graph_query_destroy(right);
}

TEST_F(GraphOptimizerTest, MultipleSingleChildIntersects) {
    // Nested single-child INTERSECTs where no child starts with VERTEX:
    // VERTEX("clip_abc") -> INTERSECT( INTERSECT( IN("tagged_with") ) )
    // After folding: VERTEX("clip_abc") -> IN("tagged_with")  (2 main-chain steps)

    query_step_t* in_step = make_step(GRAPH_STEP_IN);
    in_step->predicate = strdup("tagged_with");

    // Inner INTERSECT wrapping in_step
    query_step_t* inner = make_step(GRAPH_STEP_INTERSECT);
    inner->num_children = 1;
    inner->children = (query_step_t**)get_clear_memory(sizeof(query_step_t*));
    inner->children[0] = in_step;

    // Outer INTERSECT wrapping inner
    query_step_t* outer = make_step(GRAPH_STEP_INTERSECT);
    outer->num_children = 1;
    outer->children = (query_step_t**)get_clear_memory(sizeof(query_step_t*));
    outer->children[0] = inner;

    query_step_t* v_clip = make_step(GRAPH_STEP_VERTEX);
    v_clip->vertex_id = strdup("clip_abc");
    v_clip->next = outer;

    graph_query_t* main = graph_query_create(layer);
    main->head = v_clip;
    main->tail = in_step;

    graph_optimize(&main->head);

    // After optimization: VERTEX("clip_abc") -> IN("tagged_with")
    int steps = 0;
    query_step_t* s = main->head;
    while (s) { steps++; s = s->next; }
    ASSERT_EQ(steps, 2);

    s = main->head;
    ASSERT_EQ(s->type, GRAPH_STEP_VERTEX);
    ASSERT_STREQ(s->vertex_id, "clip_abc");

    s = s->next;
    ASSERT_EQ(s->type, GRAPH_STEP_IN);
    ASSERT_STREQ(s->predicate, "tagged_with");
    ASSERT_EQ(s->next, nullptr);

    graph_query_destroy(main);
}

TEST_F(GraphOptimizerTest, EmptyChainDoesNotCrash) {
    query_step_t* null_steps = NULL;
    int rc = graph_optimize(&null_steps);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(null_steps, nullptr);
}

TEST_F(GraphOptimizerTest, ExecutionStillWorks) {
    // Insert test data
    graph_insert_sync(layer, "clip_abc", "tagged_with", "gaming");
    graph_insert_sync(layer, "clip_abc", "tagged_with", "tutorial");

    // Build query with two different children (num_children=2, won't be folded).
    // Both children return the same set as the main traversal so the final
    // intersect is non-empty.
    graph_query_t* left = graph_query_create(layer);
    graph_query_vertex(left, "clip_abc");
    graph_query_out(left, "tagged_with");

    graph_query_t* right = graph_query_create(layer);
    graph_query_vertex(right, "clip_abc");
    graph_query_out(right, "tagged_with");

    graph_query_t* q = graph_query_create(layer);
    graph_query_vertex(q, "clip_abc");
    graph_query_out(q, "tagged_with");
    graph_query_intersect(q, left, right);

    // Execute through graph_query_execute_sync which calls graph_optimize
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
    // left and right are empty shells after intersect took their heads
    graph_query_destroy(left);
    graph_query_destroy(right);
}

TEST_F(GraphOptimizerTest, FoldSingleChildUnion) {
    // Build: VERTEX("clip_abc") -> UNION(1 child: IN("tagged_with"))
    // After folding: VERTEX("clip_abc") -> IN("tagged_with")
    // Child does NOT start with VERTEX, so the fold is valid.
    query_step_t* v1 = make_step(GRAPH_STEP_VERTEX);
    v1->vertex_id = strdup("clip_abc");

    query_step_t* in_step = make_step(GRAPH_STEP_IN);
    in_step->predicate = strdup("tagged_with");

    query_step_t* union_step = make_step(GRAPH_STEP_UNION);
    union_step->num_children = 1;
    union_step->children = (query_step_t**)get_clear_memory(sizeof(query_step_t*));
    union_step->children[0] = in_step;

    v1->next = union_step;

    graph_query_t* q = graph_query_create(layer);
    q->head = v1;
    q->tail = in_step;

    ASSERT_EQ(q->head->next->type, GRAPH_STEP_UNION);
    ASSERT_EQ(q->head->next->num_children, (size_t)1);

    graph_optimize(&q->head);

    query_step_t* s = q->head;
    while (s) {
        ASSERT_NE(s->type, GRAPH_STEP_UNION);
        s = s->next;
    }

    // Should now be: VERTEX("clip_abc") -> IN("tagged_with")
    ASSERT_EQ(q->head->next->type, GRAPH_STEP_IN);
    ASSERT_STREQ(q->head->next->predicate, "tagged_with");
    ASSERT_EQ(q->head->next->next, nullptr);

    graph_query_destroy(q);
}
