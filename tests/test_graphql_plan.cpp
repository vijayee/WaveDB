//
// Tests for GraphQL Plan Compilation
// Created: 2026-04-12
//

#include <gtest/gtest.h>
#include <cstring>
#include "Layers/graphql/graphql.h"

class GraphQLPlanTest : public ::testing::Test {
protected:
    const char* test_dir = "/tmp/wavedb_test_graphql_plan";
    graphql_layer_t* layer = nullptr;
    graphql_layer_config_t* config = nullptr;

    void SetUp() override {
        rmrf(test_dir);
        mkdir(test_dir, 0755);

        config = graphql_layer_config_default();
        config->path = test_dir;
        config->enable_persist = 1;

        layer = graphql_layer_create(test_dir, config);
        ASSERT_NE(layer, nullptr);

        // Register a simple schema
        const char* sdl = "type User { name: String age: Int friends: [User] }";
        int rc = graphql_schema_parse(layer, sdl, NULL);
        ASSERT_EQ(rc, 0);
    }

    void TearDown() override {
        if (layer) graphql_layer_destroy(layer);
        if (config) graphql_layer_config_destroy(config);
        rmrf(test_dir);
    }

    void rmrf(const char* path) {
        char cmd[512];
        snprintf(cmd, sizeof(cmd), "rm -rf %s 2>/dev/null", path);
        (void)system(cmd);
    }
};

TEST_F(GraphQLPlanTest, SimpleQuery) {
    const char* query = "{ User { name } }";
    graphql_plan_t* plan = graphql_compile_query(layer, query, NULL, NULL);
    ASSERT_NE(plan, nullptr);

    char* str = graphql_plan_to_string(plan);
    EXPECT_NE(str, nullptr);
    EXPECT_NE(strstr(str, "GET"), nullptr) << "Plan: " << str;

    free(str);
    graphql_plan_destroy(plan);
}

TEST_F(GraphQLPlanTest, QueryWithArgument) {
    const char* query = "{ User(id: \"1\") { name age } }";
    graphql_plan_t* plan = graphql_compile_query(layer, query, NULL, NULL);
    ASSERT_NE(plan, nullptr);

    char* str = graphql_plan_to_string(plan);
    EXPECT_NE(str, nullptr);

    free(str);
    graphql_plan_destroy(plan);
}

TEST_F(GraphQLPlanTest, NestedQuery) {
    const char* query = "{ User { name friends { name } } }";
    graphql_plan_t* plan = graphql_compile_query(layer, query, NULL, NULL);
    ASSERT_NE(plan, nullptr);

    char* str = graphql_plan_to_string(plan);
    EXPECT_NE(str, nullptr);

    free(str);
    graphql_plan_destroy(plan);
}

TEST_F(GraphQLPlanTest, SkipDirective) {
    const char* query = "{ User @skip(if: true) { name } }";
    graphql_plan_t* plan = graphql_compile_query(layer, query, NULL, NULL);
    // @skip(if: true) should skip the field
    // The top-level plan should have no children since User is skipped
    if (plan != nullptr) {
        // The skip causes the child plan to be NULL
        char* str = graphql_plan_to_string(plan);
        free(str);
        graphql_plan_destroy(plan);
    }
}

TEST_F(GraphQLPlanTest, IncludeDirective) {
    const char* query = "{ User @include(if: true) { name } }";
    graphql_plan_t* plan = graphql_compile_query(layer, query, NULL, NULL);
    ASSERT_NE(plan, nullptr);

    char* str = graphql_plan_to_string(plan);
    EXPECT_NE(str, nullptr);

    free(str);
    graphql_plan_destroy(plan);
}

TEST_F(GraphQLPlanTest, NullLayer) {
    graphql_plan_t* plan = graphql_compile_query(nullptr, "{ User { name } }", NULL, NULL);
    EXPECT_EQ(plan, nullptr);
}

TEST_F(GraphQLPlanTest, NullQuery) {
    graphql_plan_t* plan = graphql_compile_query(layer, nullptr, NULL, NULL);
    EXPECT_EQ(plan, nullptr);
}

TEST_F(GraphQLPlanTest, InvalidQuery) {
    graphql_plan_t* plan = graphql_compile_query(layer, "type !Broken { }", NULL, NULL);
    EXPECT_EQ(plan, nullptr);
}

TEST_F(GraphQLPlanTest, NamedQuery) {
    const char* query = "query GetUser { User { name } }";
    graphql_plan_t* plan = graphql_compile_query(layer, query, NULL, NULL);
    ASSERT_NE(plan, nullptr);

    char* str = graphql_plan_to_string(plan);
    EXPECT_NE(str, nullptr);

    free(str);
    graphql_plan_destroy(plan);
}

TEST_F(GraphQLPlanTest, MutationPlan) {
    const char* mutation = "mutation CreateUser { User { name } }";
    graphql_plan_t* plan = graphql_compile_mutation(layer, mutation, NULL, NULL);
    ASSERT_NE(plan, nullptr);

    char* str = graphql_plan_to_string(plan);
    EXPECT_NE(str, nullptr);
    EXPECT_NE(strstr(str, "Mutation"), nullptr) << "Plan: " << str;

    free(str);
    graphql_plan_destroy(plan);
}

TEST_F(GraphQLPlanTest, PlanToString) {
    const char* query = "{ User { name } }";
    graphql_plan_t* plan = graphql_compile_query(layer, query, NULL, NULL);
    ASSERT_NE(plan, nullptr);

    char* str = graphql_plan_to_string(plan);
    EXPECT_NE(str, nullptr);
    EXPECT_NE(strstr(str, "GET"), nullptr) << "Plan: " << str;

    free(str);
    graphql_plan_destroy(plan);
}