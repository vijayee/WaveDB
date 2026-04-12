//
// Tests for GraphQL Parser
// Created: 2026-04-12
//

#include <gtest/gtest.h>
#include <cstring>
#include "Layers/graphql/graphql_parser.h"
#include "Layers/graphql/graphql_types.h"

class GraphQLParserTest : public ::testing::Test {
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
// Type definition parsing
// ============================================================

TEST_F(GraphQLParserTest, SimpleTypeDefinition) {
    const char* sdl = "type User { name: String age: Int }";
    ast = graphql_parse(sdl, strlen(sdl));
    ASSERT_NE(ast, nullptr);
    EXPECT_EQ(ast->kind, GRAPHQL_AST_DOCUMENT);
    EXPECT_EQ(ast->children.length, 1);

    graphql_ast_node_t* type_def = ast->children.data[0];
    EXPECT_EQ(type_def->kind, GRAPHQL_AST_TYPE_DEFINITION);
    EXPECT_STREQ(type_def->name, "User");
    EXPECT_EQ(type_def->children.length, 2);

    // First field: name: String
    graphql_ast_node_t* field1 = type_def->children.data[0];
    EXPECT_EQ(field1->kind, GRAPHQL_AST_FIELD_DEFINITION);
    EXPECT_STREQ(field1->name, "name");
    ASSERT_NE(field1->type_ref, nullptr);
    EXPECT_EQ(field1->type_ref->kind, GRAPHQL_TYPE_SCALAR);

    // Second field: age: Int
    graphql_ast_node_t* field2 = type_def->children.data[1];
    EXPECT_EQ(field2->kind, GRAPHQL_AST_FIELD_DEFINITION);
    EXPECT_STREQ(field2->name, "age");
    ASSERT_NE(field2->type_ref, nullptr);
    EXPECT_EQ(field2->type_ref->kind, GRAPHQL_TYPE_SCALAR);
}

TEST_F(GraphQLParserTest, TypeWithRequiredField) {
    const char* sdl = "type User { id: ID! }";
    ast = graphql_parse(sdl, strlen(sdl));
    ASSERT_NE(ast, nullptr);

    graphql_ast_node_t* type_def = ast->children.data[0];
    graphql_ast_node_t* field = type_def->children.data[0];
    EXPECT_STREQ(field->name, "id");
    ASSERT_NE(field->type_ref, nullptr);
    EXPECT_EQ(field->type_ref->kind, GRAPHQL_TYPE_NON_NULL);
    ASSERT_NE(field->type_ref->of_type, nullptr);
    EXPECT_EQ(field->type_ref->of_type->kind, GRAPHQL_TYPE_SCALAR);
}

TEST_F(GraphQLParserTest, TypeWithListField) {
    const char* sdl = "type User { friends: [User] }";
    ast = graphql_parse(sdl, strlen(sdl));
    ASSERT_NE(ast, nullptr);

    graphql_ast_node_t* type_def = ast->children.data[0];
    graphql_ast_node_t* field = type_def->children.data[0];
    EXPECT_STREQ(field->name, "friends");
    ASSERT_NE(field->type_ref, nullptr);
    EXPECT_EQ(field->type_ref->kind, GRAPHQL_TYPE_LIST);
    ASSERT_NE(field->type_ref->of_type, nullptr);
    EXPECT_EQ(field->type_ref->of_type->kind, GRAPHQL_TYPE_OBJECT);
}

TEST_F(GraphQLParserTest, TypeWithRequiredListField) {
    const char* sdl = "type User { friends: [User]! }";
    ast = graphql_parse(sdl, strlen(sdl));
    ASSERT_NE(ast, nullptr);

    graphql_ast_node_t* type_def = ast->children.data[0];
    graphql_ast_node_t* field = type_def->children.data[0];
    EXPECT_EQ(field->type_ref->kind, GRAPHQL_TYPE_NON_NULL);
    EXPECT_EQ(field->type_ref->of_type->kind, GRAPHQL_TYPE_LIST);
}

TEST_F(GraphQLParserTest, MultipleTypes) {
    const char* sdl = "type User { name: String } type Post { title: String }";
    ast = graphql_parse(sdl, strlen(sdl));
    ASSERT_NE(ast, nullptr);
    EXPECT_EQ(ast->children.length, 2);

    EXPECT_STREQ(ast->children.data[0]->name, "User");
    EXPECT_STREQ(ast->children.data[1]->name, "Post");
}

// ============================================================
// Enum definition parsing
// ============================================================

TEST_F(GraphQLParserTest, EnumDefinition) {
    const char* sdl = "enum Role { ADMIN USER }";
    ast = graphql_parse(sdl, strlen(sdl));
    ASSERT_NE(ast, nullptr);

    graphql_ast_node_t* enum_def = ast->children.data[0];
    EXPECT_EQ(enum_def->kind, GRAPHQL_AST_ENUM_DEFINITION);
    EXPECT_STREQ(enum_def->name, "Role");
    EXPECT_EQ(enum_def->children.length, 2);
}

// ============================================================
// Schema definition parsing
// ============================================================

TEST_F(GraphQLParserTest, SchemaDefinition) {
    const char* sdl = "schema { query: Query mutation: Mutation }";
    ast = graphql_parse(sdl, strlen(sdl));
    ASSERT_NE(ast, nullptr);

    graphql_ast_node_t* schema_def = ast->children.data[0];
    EXPECT_EQ(schema_def->kind, GRAPHQL_AST_SCHEMA_DEFINITION);
    EXPECT_EQ(schema_def->children.length, 2);

    graphql_ast_node_t* query_field = schema_def->children.data[0];
    EXPECT_STREQ(query_field->name, "query");
    EXPECT_EQ(query_field->type_ref->kind, GRAPHQL_TYPE_OBJECT);
}

// ============================================================
// Directive parsing
// ============================================================

TEST_F(GraphQLParserTest, TypeDirective) {
    const char* sdl = "type Person @plural(name: \"People\") { name: String }";
    ast = graphql_parse(sdl, strlen(sdl));
    ASSERT_NE(ast, nullptr);

    graphql_ast_node_t* type_def = ast->children.data[0];
    EXPECT_EQ(type_def->directives.length, 1);
    graphql_directive_t* dir = type_def->directives.data[0];
    EXPECT_STREQ(dir->name, "plural");
    EXPECT_EQ(dir->arg_names.length, 1);
    EXPECT_EQ(dir->arg_values.length, 1);
    EXPECT_STREQ(dir->arg_names.data[0], "name");
    // The value is the string content without quotes
}

TEST_F(GraphQLParserTest, FieldDirective) {
    const char* sdl = "type User { email: String @skip(if: true) }";
    ast = graphql_parse(sdl, strlen(sdl));
    ASSERT_NE(ast, nullptr);

    graphql_ast_node_t* type_def = ast->children.data[0];
    graphql_ast_node_t* field = type_def->children.data[0];
    EXPECT_STREQ(field->name, "email");
    EXPECT_EQ(field->directives.length, 1);
    EXPECT_STREQ(field->directives.data[0]->name, "skip");
}

// ============================================================
// Error handling
// ============================================================

TEST_F(GraphQLParserTest, InvalidInput) {
    // This is now valid as an anonymous query: { invalid }
    // Use something that's actually invalid in both SDL and query syntax
    const char* sdl = "type !Invalid { }";
    ast = graphql_parse(sdl, strlen(sdl));
    EXPECT_EQ(ast, nullptr);
}

TEST_F(GraphQLParserTest, EmptyInput) {
    const char* sdl = "";
    ast = graphql_parse(sdl, strlen(sdl));
    EXPECT_EQ(ast, nullptr);
}

TEST_F(GraphQLParserTest, NullInput) {
    ast = graphql_parse(NULL, 0);
    EXPECT_EQ(ast, nullptr);
}