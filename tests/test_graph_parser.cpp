//
// Tests for Graph DSL Parser
//

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
#endif

extern "C" {
#include "../src/Layers/graph/graph.h"
#include "../src/Layers/graph/graph_internal.h"
}

static int test_counter = 0;

class GraphParserTest : public ::testing::Test {
protected:
    void SetUp() override {
        test_dir = "/tmp/wavedb_graph_parser_test_" + std::to_string(getpid()) + "_" + std::to_string(test_counter++);
        mkdir(test_dir.c_str(), 0700);
        layer = graph_layer_create(test_dir.c_str(), NULL, NULL, NULL);
        ASSERT_NE(layer, nullptr);
    }

    void TearDown() override {
        graph_layer_destroy(layer);
        std::string cmd = "rm -rf " + test_dir;
        system(cmd.c_str());
    }

    void insert_test_data() {
        graph_insert_sync(layer, "clip_abc", "tagged_with", "gaming");
        graph_insert_sync(layer, "clip_abc", "tagged_with", "tutorial");
        graph_insert_sync(layer, "clip_abc", "created_by", "alice");
        graph_insert_sync(layer, "clip_xyz", "tagged_with", "gaming");
        graph_insert_sync(layer, "clip_xyz", "created_by", "bob");
    }

    std::string test_dir;
    graph_layer_t* layer = nullptr;
};

TEST_F(GraphParserTest, SimpleOutTraversal) {
    insert_test_data();

    graph_parse_error_t err;
    graph_result_t* r = graph_parse_execute(
        "g.V(\"clip_abc\").Out(\"tagged_with\").All()",
        layer, &err);
    ASSERT_NE(r, nullptr) << "Error: " << err.message;
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
}

TEST_F(GraphParserTest, SimpleInTraversal) {
    insert_test_data();

    graph_parse_error_t err;
    graph_result_t* r = graph_parse_execute(
        "g.V(\"gaming\").In(\"tagged_with\").All()",
        layer, &err);
    ASSERT_NE(r, nullptr) << "Error: " << err.message;
    ASSERT_EQ(graph_result_count(r), (size_t)2);
    graph_result_destroy(r);
}

TEST_F(GraphParserTest, MultiHopTraversal) {
    graph_insert_sync(layer, "alice", "follows", "bob");
    graph_insert_sync(layer, "bob", "likes", "clip_abc");

    graph_parse_error_t err;
    graph_result_t* r = graph_parse_execute(
        "g.V(\"alice\").Out(\"follows\").Out(\"likes\").All()",
        layer, &err);
    ASSERT_NE(r, nullptr) << "Error: " << err.message;
    ASSERT_EQ(graph_result_count(r), (size_t)1);
    EXPECT_STREQ(graph_result_vertices(r)[0], "clip_abc");
    graph_result_destroy(r);
}

TEST_F(GraphParserTest, Intersection) {
    insert_test_data();

    graph_parse_error_t err;
    graph_result_t* r = graph_parse_execute(
        "g.V(\"gaming\").In(\"tagged_with\").And(g.V(\"tutorial\").In(\"tagged_with\")).All()",
        layer, &err);
    ASSERT_NE(r, nullptr) << "Error: " << err.message;
    ASSERT_EQ(graph_result_count(r), (size_t)1);
    EXPECT_STREQ(graph_result_vertices(r)[0], "clip_abc");
    graph_result_destroy(r);
}

TEST_F(GraphParserTest, Union) {
    insert_test_data();

    graph_parse_error_t err;
    graph_result_t* r = graph_parse_execute(
        "g.V(\"gaming\").In(\"tagged_with\").Or(g.V(\"tutorial\").In(\"tagged_with\")).All()",
        layer, &err);
    ASSERT_NE(r, nullptr) << "Error: " << err.message;
    ASSERT_EQ(graph_result_count(r), (size_t)2);
    graph_result_destroy(r);
}

TEST_F(GraphParserTest, HasFilter) {
    graph_insert_sync(layer, "clip_abc", "name", "My Clip");
    graph_insert_sync(layer, "clip_xyz", "name", "Other Clip");

    graph_parse_error_t err;
    graph_result_t* r = graph_parse_execute(
        "g.Has(\"name\", \"My Clip\").All()",
        layer, &err);
    ASSERT_NE(r, nullptr) << "Error: " << err.message;
    ASSERT_EQ(graph_result_count(r), (size_t)1);
    EXPECT_STREQ(graph_result_vertices(r)[0], "clip_abc");
    graph_result_destroy(r);
}

TEST_F(GraphParserTest, Limit) {
    insert_test_data();

    graph_parse_error_t err;
    graph_result_t* r = graph_parse_execute(
        "g.V(\"gaming\").In(\"tagged_with\").Limit(1).All()",
        layer, &err);
    ASSERT_NE(r, nullptr) << "Error: " << err.message;
    ASSERT_EQ(graph_result_count(r), (size_t)1);
    graph_result_destroy(r);
}

TEST_F(GraphParserTest, Count) {
    insert_test_data();

    graph_parse_error_t err;
    size_t count = 0;
    int rc = graph_parse_count(
        "g.V(\"gaming\").In(\"tagged_with\").All()",
        layer, &count, &err);
    ASSERT_EQ(rc, 0) << "Error: " << err.message;
    ASSERT_EQ(count, (size_t)2);
}

TEST_F(GraphParserTest, MorphismDefinitionAndFollow) {
    graph_insert_sync(layer, "alice", "follows", "bob");
    graph_insert_sync(layer, "bob", "likes", "clip_abc");

    graph_parse_error_t err;

    // Define a morphism
    int rc = graph_morphism_define(layer, "followed_content",
        "g.Morphism(\"followed_content\").Out(\"follows\").Out(\"likes\")", &err);
    ASSERT_EQ(rc, 0) << "Error: " << err.message;

    // Use it
    graph_result_t* r = graph_parse_execute(
        "g.V(\"alice\").Follow(\"followed_content\").All()",
        layer, &err);
    ASSERT_NE(r, nullptr) << "Error: " << err.message;
    ASSERT_EQ(graph_result_count(r), (size_t)1);
    EXPECT_STREQ(graph_result_vertices(r)[0], "clip_abc");
    graph_result_destroy(r);
}

TEST_F(GraphParserTest, ParseErrorUnclosedString) {
    graph_parse_error_t err;
    graph_query_t* q = graph_parse("g.V(\"abc)", layer, &err);
    ASSERT_EQ(q, nullptr);
    ASSERT_EQ(err.ok, 0);
    ASSERT_GT(err.position, 0);
    EXPECT_NE(strstr(err.message, "Unterminated"), nullptr);
}

TEST_F(GraphParserTest, ParseErrorBadMethod) {
    graph_parse_error_t err;
    graph_query_t* q = graph_parse("g.X(\"y\").All()", layer, &err);
    ASSERT_EQ(q, nullptr);
    ASSERT_EQ(err.ok, 0);
    EXPECT_NE(strstr(err.message, "Unknown method"), nullptr);
}

TEST_F(GraphParserTest, ParseErrorMissingDot) {
    graph_parse_error_t err;
    graph_query_t* q = graph_parse("g.V(\"x\")Out(\"y\")", layer, &err);
    ASSERT_EQ(q, nullptr);
}

TEST_F(GraphParserTest, EmptyQuery) {
    graph_parse_error_t err;
    graph_query_t* q = graph_parse("", layer, &err);
    ASSERT_EQ(q, nullptr);
    ASSERT_EQ(err.ok, 0);
}

TEST_F(GraphParserTest, UnknownMorphism) {
    graph_parse_error_t err;
    graph_result_t* r = graph_parse_execute(
        "g.V(\"x\").Follow(\"nonexistent\").All()", layer, &err);
    ASSERT_EQ(r, nullptr);
    EXPECT_NE(strstr(err.message, "Unknown morphism"), nullptr);
}
