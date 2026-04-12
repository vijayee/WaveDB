//
// Tests for GraphQL Query Parser
// Created: 2026-04-12
//

#include <gtest/gtest.h>
#include <cstring>
#include "Layers/graphql/graphql_parser.h"
#include "Layers/graphql/graphql_types.h"

class GraphQLQueryTest : public ::testing::Test {
protected:
    void TearDown() override {
        if (ast) {
            graphql_ast_destroy(ast);
            ast = nullptr;
        }
    }

    graphql_ast_node_t* ast = nullptr;
};

// ============================================================
// Anonymous queries
// ============================================================

TEST_F(GraphQLQueryTest, AnonymousQuery) {
    const char* query = "{ user { name } }";
    ast = graphql_parse(query, strlen(query));
    ASSERT_NE(ast, nullptr);
    EXPECT_EQ(ast->kind, GRAPHQL_AST_DOCUMENT);
    EXPECT_EQ(ast->children.length, 1);

    graphql_ast_node_t* op = ast->children.data[0];
    EXPECT_EQ(op->kind, GRAPHQL_AST_OPERATION);
    EXPECT_EQ(op->name, nullptr);  // anonymous query has no name
    EXPECT_EQ(op->children.length, 1);

    graphql_ast_node_t* field = op->children.data[0];
    EXPECT_EQ(field->kind, GRAPHQL_AST_FIELD);
    EXPECT_STREQ(field->name, "user");
    EXPECT_EQ(field->children.length, 1);

    graphql_ast_node_t* subfield = field->children.data[0];
    EXPECT_EQ(subfield->kind, GRAPHQL_AST_FIELD);
    EXPECT_STREQ(subfield->name, "name");
}

TEST_F(GraphQLQueryTest, NamedQuery) {
    const char* query = "query GetUser { user { name } }";
    ast = graphql_parse(query, strlen(query));
    ASSERT_NE(ast, nullptr);

    graphql_ast_node_t* op = ast->children.data[0];
    EXPECT_EQ(op->kind, GRAPHQL_AST_OPERATION);
    EXPECT_STREQ(op->name, "GetUser");
    EXPECT_EQ(op->alias, nullptr);  // not a mutation
}

TEST_F(GraphQLQueryTest, MutationOperation) {
    const char* query = "mutation CreateUser { createUser { id } }";
    ast = graphql_parse(query, strlen(query));
    ASSERT_NE(ast, nullptr);

    graphql_ast_node_t* op = ast->children.data[0];
    EXPECT_EQ(op->kind, GRAPHQL_AST_OPERATION);
    EXPECT_STREQ(op->alias, "mutation");
    EXPECT_STREQ(op->name, "CreateUser");
}

// ============================================================
// Field arguments
// ============================================================

TEST_F(GraphQLQueryTest, FieldWithArguments) {
    const char* query = "{ user(id: \"1\") { name } }";
    ast = graphql_parse(query, strlen(query));
    ASSERT_NE(ast, nullptr);

    graphql_ast_node_t* op = ast->children.data[0];
    graphql_ast_node_t* field = op->children.data[0];
    EXPECT_STREQ(field->name, "user");
    EXPECT_EQ(field->arguments.length, 1);

    graphql_ast_node_t* arg = field->arguments.data[0];
    EXPECT_EQ(arg->kind, GRAPHQL_AST_ARGUMENT);
    EXPECT_STREQ(arg->name, "id");
    ASSERT_NE(arg->literal, nullptr);
    EXPECT_EQ(arg->literal->kind, GRAPHQL_LITERAL_STRING);
}

TEST_F(GraphQLQueryTest, MultipleArguments) {
    const char* query = "{ users(limit: 10, offset: 0) { name } }";
    ast = graphql_parse(query, strlen(query));
    ASSERT_NE(ast, nullptr);

    graphql_ast_node_t* field = ast->children.data[0]->children.data[0];
    EXPECT_EQ(field->arguments.length, 2);
    EXPECT_STREQ(field->arguments.data[0]->name, "limit");
    EXPECT_EQ(field->arguments.data[0]->literal->kind, GRAPHQL_LITERAL_INT);
    EXPECT_STREQ(field->arguments.data[1]->name, "offset");
    EXPECT_EQ(field->arguments.data[1]->literal->kind, GRAPHQL_LITERAL_INT);
}

// ============================================================
// Field aliases
// ============================================================

TEST_F(GraphQLQueryTest, FieldAlias) {
    const char* query = "{ admin: user(id: \"1\") { name } }";
    ast = graphql_parse(query, strlen(query));
    ASSERT_NE(ast, nullptr);

    graphql_ast_node_t* field = ast->children.data[0]->children.data[0];
    EXPECT_STREQ(field->name, "user");
    EXPECT_STREQ(field->alias, "admin");
}

// ============================================================
// Directives on query fields
// ============================================================

TEST_F(GraphQLQueryTest, SkipDirective) {
    const char* query = "{ user @skip(if: true) { name } }";
    ast = graphql_parse(query, strlen(query));
    ASSERT_NE(ast, nullptr);

    graphql_ast_node_t* field = ast->children.data[0]->children.data[0];
    EXPECT_STREQ(field->name, "user");
    EXPECT_EQ(field->directives.length, 1);
    EXPECT_STREQ(field->directives.data[0]->name, "skip");
}

TEST_F(GraphQLQueryTest, IncludeDirective) {
    const char* query = "{ user @include(if: false) { name } }";
    ast = graphql_parse(query, strlen(query));
    ASSERT_NE(ast, nullptr);

    graphql_ast_node_t* field = ast->children.data[0]->children.data[0];
    EXPECT_EQ(field->directives.length, 1);
    EXPECT_STREQ(field->directives.data[0]->name, "include");
}

// ============================================================
// Nested selections
// ============================================================

TEST_F(GraphQLQueryTest, NestedSelection) {
    const char* query = "{ user { name friends { name email } } }";
    ast = graphql_parse(query, strlen(query));
    ASSERT_NE(ast, nullptr);

    graphql_ast_node_t* user_field = ast->children.data[0]->children.data[0];
    EXPECT_STREQ(user_field->name, "user");
    EXPECT_EQ(user_field->children.length, 2);

    EXPECT_STREQ(user_field->children.data[0]->name, "name");
    EXPECT_STREQ(user_field->children.data[1]->name, "friends");

    graphql_ast_node_t* friends_field = user_field->children.data[1];
    EXPECT_EQ(friends_field->children.length, 2);
    EXPECT_STREQ(friends_field->children.data[0]->name, "name");
    EXPECT_STREQ(friends_field->children.data[1]->name, "email");
}

// ============================================================
// Fragments
// ============================================================

TEST_F(GraphQLQueryTest, FragmentSpread) {
    const char* query = "{ user { ...UserFields } }";
    ast = graphql_parse(query, strlen(query));
    ASSERT_NE(ast, nullptr);

    graphql_ast_node_t* user_field = ast->children.data[0]->children.data[0];
    EXPECT_EQ(user_field->children.length, 1);

    graphql_ast_node_t* spread = user_field->children.data[0];
    EXPECT_EQ(spread->kind, GRAPHQL_AST_FRAGMENT_SPREAD);
    EXPECT_STREQ(spread->name, "UserFields");
}

TEST_F(GraphQLQueryTest, FragmentDefinition) {
    const char* query = "fragment UserFields on User { name email }";
    ast = graphql_parse(query, strlen(query));
    ASSERT_NE(ast, nullptr);

    graphql_ast_node_t* frag = ast->children.data[0];
    EXPECT_EQ(frag->kind, GRAPHQL_AST_FRAGMENT);
    EXPECT_STREQ(frag->name, "UserFields");
    ASSERT_NE(frag->type_ref, nullptr);
    EXPECT_STREQ(frag->type_ref->name, "User");
    EXPECT_EQ(frag->children.length, 2);
}

TEST_F(GraphQLQueryTest, InlineFragment) {
    const char* query = "{ user { ... on Admin { role } } }";
    ast = graphql_parse(query, strlen(query));
    ASSERT_NE(ast, nullptr);

    graphql_ast_node_t* user_field = ast->children.data[0]->children.data[0];
    EXPECT_EQ(user_field->children.length, 1);

    graphql_ast_node_t* inline_frag = user_field->children.data[0];
    EXPECT_EQ(inline_frag->kind, GRAPHQL_AST_INLINE_FRAGMENT);
    EXPECT_EQ(inline_frag->children.length, 1);
    EXPECT_STREQ(inline_frag->children.data[0]->name, "role");
}

// ============================================================
// Argument value types
// ============================================================

TEST_F(GraphQLQueryTest, IntArgument) {
    const char* query = "{ user(id: 42) { name } }";
    ast = graphql_parse(query, strlen(query));
    ASSERT_NE(ast, nullptr);

    graphql_ast_node_t* arg = ast->children.data[0]->children.data[0]->arguments.data[0];
    EXPECT_EQ(arg->literal->kind, GRAPHQL_LITERAL_INT);
    EXPECT_EQ(arg->literal->int_val, 42);
}

TEST_F(GraphQLQueryTest, BoolArgument) {
    const char* query = "{ users(active: true) { name } }";
    ast = graphql_parse(query, strlen(query));
    ASSERT_NE(ast, nullptr);

    graphql_ast_node_t* arg = ast->children.data[0]->children.data[0]->arguments.data[0];
    EXPECT_EQ(arg->literal->kind, GRAPHQL_LITERAL_BOOL);
    EXPECT_EQ(arg->literal->bool_val, true);
}

TEST_F(GraphQLQueryTest, NullArgument) {
    const char* query = "{ users(filter: null) { name } }";
    ast = graphql_parse(query, strlen(query));
    ASSERT_NE(ast, nullptr);

    graphql_ast_node_t* arg = ast->children.data[0]->children.data[0]->arguments.data[0];
    EXPECT_EQ(arg->literal->kind, GRAPHQL_LITERAL_NULL);
}

// ============================================================
// Mixed SDL and queries
// ============================================================

TEST_F(GraphQLQueryTest, SDLAndQueryInSameDocument) {
    const char* doc = "type User { name: String } query GetUser { user { name } }";
    ast = graphql_parse(doc, strlen(doc));
    ASSERT_NE(ast, nullptr);
    EXPECT_EQ(ast->children.length, 2);

    EXPECT_EQ(ast->children.data[0]->kind, GRAPHQL_AST_TYPE_DEFINITION);
    EXPECT_EQ(ast->children.data[1]->kind, GRAPHQL_AST_OPERATION);
}