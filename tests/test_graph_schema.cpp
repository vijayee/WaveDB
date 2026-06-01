//
// Tests for Graph Schema Definition and Index Selection
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
#include "../src/Layers/graph/graph.h"
}

static int test_counter = 0;

class GraphSchemaTest : public ::testing::Test {
protected:
    void SetUp() override {
        test_dir = "/tmp/wavedb_schema_test_" + std::to_string(getpid()) + "_" + std::to_string(test_counter++);
        mkdir(test_dir.c_str(), 0700);
        layer = graph_layer_create(test_dir.c_str(), NULL);
        ASSERT_NE(layer, nullptr);
    }

    void TearDown() override {
        graph_layer_destroy(layer);
        std::string cmd = "rm -rf " + test_dir;
        system(cmd.c_str());
    }

    std::string test_dir;
    graph_layer_t* layer = nullptr;
};

TEST_F(GraphSchemaTest, ParseSchema) {
    char* error = NULL;
    int rc = graph_schema_parse(layer,
        "type Clip @index(spo, pos) {\n"
        "  tagged_with: [Tag]\n"
        "  created_by: User\n"
        "  name: String @index(pos)\n"
        "}\n"
        "type User @index(spo) {\n"
        "  follows: [User]\n"
        "  likes: [Clip]\n"
        "  name: String @index(pos)\n"
        "}", &error);
    ASSERT_EQ(rc, 0) << (error ? error : "parse failed");
    if (error) free(error);
}

TEST_F(GraphSchemaTest, IndexSelectionWithoutSchema) {
    // Without schema, all indices are maintained
    EXPECT_EQ(graph_schema_needs_index(layer, "anything", GRAPH_INDEX_SPO), 1);
    EXPECT_EQ(graph_schema_needs_index(layer, "anything", GRAPH_INDEX_POS), 1);
    EXPECT_EQ(graph_schema_needs_index(layer, "anything", GRAPH_INDEX_OSP), 1);
    EXPECT_EQ(graph_schema_needs_index(layer, "anything", GRAPH_INDEX_PSO), 1);
}

TEST_F(GraphSchemaTest, IndexSelectionWithSchema) {
    char* error = NULL;
    int rc = graph_schema_parse(layer,
        "type Clip @index(spo, pos) {\n"
        "  name: String @index(pos)\n"
        "  tagged_with: [Tag]\n"
        "}", &error);
    ASSERT_EQ(rc, 0) << (error ? error : "parse failed");
    if (error) free(error);

    // name is @index(pos) only
    EXPECT_EQ(graph_schema_needs_index(layer, "name", GRAPH_INDEX_SPO), 0);
    EXPECT_EQ(graph_schema_needs_index(layer, "name", GRAPH_INDEX_POS), 1);
    EXPECT_EQ(graph_schema_needs_index(layer, "name", GRAPH_INDEX_OSP), 0);
    EXPECT_EQ(graph_schema_needs_index(layer, "name", GRAPH_INDEX_PSO), 0);

    // tagged_with uses type default @index(spo, pos)
    EXPECT_EQ(graph_schema_needs_index(layer, "tagged_with", GRAPH_INDEX_SPO), 1);
    EXPECT_EQ(graph_schema_needs_index(layer, "tagged_with", GRAPH_INDEX_POS), 1);
}

TEST_F(GraphSchemaTest, UnknownPredicateUsesBackwardCompat) {
    char* error = NULL;
    graph_schema_parse(layer,
        "type Clip @index(spo) { name: String @index(pos) }", &error);
    if (error) free(error);

    // Unknown predicates should still maintain all indices (backward compat)
    EXPECT_EQ(graph_schema_needs_index(layer, "unknown_pred", GRAPH_INDEX_SPO), 1);
    EXPECT_EQ(graph_schema_needs_index(layer, "unknown_pred", GRAPH_INDEX_POS), 1);
    EXPECT_EQ(graph_schema_needs_index(layer, "unknown_pred", GRAPH_INDEX_OSP), 1);
    EXPECT_EQ(graph_schema_needs_index(layer, "unknown_pred", GRAPH_INDEX_PSO), 1);
}

TEST_F(GraphSchemaTest, ParseError) {
    char* error = NULL;
    int rc = graph_schema_parse(layer,
        "type Clip @index(invalid) {}", &error);
    ASSERT_LT(rc, 0);
    ASSERT_NE(error, nullptr);
    EXPECT_NE(strstr(error, "Unknown index"), nullptr);
    free(error);
}

TEST_F(GraphSchemaTest, InsertUsesSchema) {
    char* error = NULL;
    graph_schema_parse(layer,
        "type Clip @index(spo) { name: String @index(spo, pos) }", &error);
    if (error) free(error);

    // Insert with schema-specified indices
    int rc = graph_insert_sync(layer, "clip_abc", "name", "My Clip");
    ASSERT_EQ(rc, 0);

    // Query via SPO scan (written because @index(spo, pos) includes SPO)
    graph_query_t* q = graph_query_create(layer);
    graph_query_vertex(q, "clip_abc");
    graph_query_out(q, "name");
    graph_result_t* r = graph_query_execute_sync(q);
    ASSERT_NE(r, nullptr);
    ASSERT_EQ(graph_result_count(r), (size_t)1);
    EXPECT_STREQ(graph_result_vertices(r)[0], "My Clip");
    graph_result_destroy(r);
    graph_query_destroy(q);
}

TEST_F(GraphSchemaTest, AllFourIndices) {
    // Test with OSP and PSO enabled
    char* error = NULL;
    graph_schema_parse(layer,
        "type Clip @index(spo, pos, osp, pso) {\n"
        "  tagged_with: [Tag] @index(spo, pos, osp, pso)\n"
        "}", &error);
    if (error) free(error);

    // Insert
    int rc = graph_insert_sync(layer, "clip_abc", "tagged_with", "gaming");
    ASSERT_EQ(rc, 0);

    // SPO scan still works
    graph_query_t* q = graph_query_create(layer);
    graph_query_vertex(q, "clip_abc");
    graph_query_out(q, "tagged_with");
    graph_result_t* r = graph_query_execute_sync(q);
    ASSERT_NE(r, nullptr);
    ASSERT_EQ(graph_result_count(r), (size_t)1);
    EXPECT_STREQ(graph_result_vertices(r)[0], "gaming");
    graph_result_destroy(r);
    graph_query_destroy(q);

    // Delete
    rc = graph_delete_sync(layer, "clip_abc", "tagged_with", "gaming");
    ASSERT_EQ(rc, 0);
}

TEST_F(GraphSchemaTest, ParseMultipleTypes) {
    char* error = NULL;
    int rc = graph_schema_parse(layer,
        "type Clip @index(spo, pos) { name: String }\n"
        "type User @index(spo) { name: String }\n"
        "type Tag @index(spo, pos) { name: String }", &error);
    ASSERT_EQ(rc, 0) << (error ? error : "parse failed");
    if (error) free(error);
}
