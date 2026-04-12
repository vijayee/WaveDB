//
// Tests for GraphQL Lexer
// Created: 2026-04-12
//

#include <gtest/gtest.h>
#include <cstring>
#include "Layers/graphql/graphql_lexer.h"

class GraphQLLexerTest : public ::testing::Test {
protected:
    void TearDown() override {
        if (lexer) {
            graphql_lexer_destroy(lexer);
            lexer = nullptr;
        }
    }

    graphql_lexer_t* lexer = nullptr;
};

// ============================================================
// Basic tokenization
// ============================================================

TEST_F(GraphQLLexerTest, EmptyInput) {
    lexer = graphql_lexer_create("", 0);
    ASSERT_NE(lexer, nullptr);
    graphql_token_t token = graphql_lexer_next(lexer);
    EXPECT_EQ(token.kind, GRAPHQL_TOKEN_EOF);
}

TEST_F(GraphQLLexerTest, WhitespaceOnly) {
    const char* src = "   \t\n\r  ";
    lexer = graphql_lexer_create(src, strlen(src));
    ASSERT_NE(lexer, nullptr);
    graphql_token_t token = graphql_lexer_next(lexer);
    EXPECT_EQ(token.kind, GRAPHQL_TOKEN_EOF);
}

TEST_F(GraphQLLexerTest, Comments) {
    const char* src = "type # this is a comment\nUser";
    lexer = graphql_lexer_create(src, strlen(src));
    ASSERT_NE(lexer, nullptr);
    graphql_token_t token = graphql_lexer_next(lexer);
    EXPECT_EQ(token.kind, GRAPHQL_TOKEN_NAME);
    EXPECT_EQ(std::string(token.start, token.length), "type");
    token = graphql_lexer_next(lexer);
    EXPECT_EQ(token.kind, GRAPHQL_TOKEN_NAME);
    EXPECT_EQ(std::string(token.start, token.length), "User");
    token = graphql_lexer_next(lexer);
    EXPECT_EQ(token.kind, GRAPHQL_TOKEN_EOF);
}

TEST_F(GraphQLLexerTest, CommaIgnored) {
    const char* src = "a, b, c";
    lexer = graphql_lexer_create(src, strlen(src));
    ASSERT_NE(lexer, nullptr);
    graphql_token_t token = graphql_lexer_next(lexer);
    EXPECT_EQ(std::string(token.start, token.length), "a");
    token = graphql_lexer_next(lexer);
    EXPECT_EQ(std::string(token.start, token.length), "b");
    token = graphql_lexer_next(lexer);
    EXPECT_EQ(std::string(token.start, token.length), "c");
}

// ============================================================
// Punctuation
// ============================================================

TEST_F(GraphQLLexerTest, Punctuation) {
    const char* src = "{ } ( ) [ ] : ! @ = & |";
    lexer = graphql_lexer_create(src, strlen(src));
    ASSERT_NE(lexer, nullptr);

    graphql_token_kind_t expected[] = {
        GRAPHQL_TOKEN_LBRACE, GRAPHQL_TOKEN_RBRACE,
        GRAPHQL_TOKEN_LPAREN, GRAPHQL_TOKEN_RPAREN,
        GRAPHQL_TOKEN_LBRACKET, GRAPHQL_TOKEN_RBRACKET,
        GRAPHQL_TOKEN_COLON, GRAPHQL_TOKEN_BANG,
        GRAPHQL_TOKEN_AT, GRAPHQL_TOKEN_EQUALS,
        GRAPHQL_TOKEN_AMP, GRAPHQL_TOKEN_PIPE
    };

    for (int i = 0; i < 12; i++) {
        graphql_token_t token = graphql_lexer_next(lexer);
        EXPECT_EQ(token.kind, expected[i]) << "Token " << i << " wrong kind, got "
            << token.kind << " expected " << expected[i];
    }
    EXPECT_EQ(graphql_lexer_next(lexer).kind, GRAPHQL_TOKEN_EOF);
}

TEST_F(GraphQLLexerTest, SpreadOperator) {
    const char* src = "...";
    lexer = graphql_lexer_create(src, strlen(src));
    ASSERT_NE(lexer, nullptr);
    graphql_token_t token = graphql_lexer_next(lexer);
    EXPECT_EQ(token.kind, GRAPHQL_TOKEN_DOTDOTDOT);
}

// ============================================================
// Names and keywords
// ============================================================

TEST_F(GraphQLLexerTest, Names) {
    const char* src = "type User hello_world __schema";
    lexer = graphql_lexer_create(src, strlen(src));
    ASSERT_NE(lexer, nullptr);

    graphql_token_t token = graphql_lexer_next(lexer);
    EXPECT_EQ(token.kind, GRAPHQL_TOKEN_NAME);
    EXPECT_EQ(std::string(token.start, token.length), "type");

    token = graphql_lexer_next(lexer);
    EXPECT_EQ(token.kind, GRAPHQL_TOKEN_NAME);
    EXPECT_EQ(std::string(token.start, token.length), "User");

    token = graphql_lexer_next(lexer);
    EXPECT_EQ(token.kind, GRAPHQL_TOKEN_NAME);
    EXPECT_EQ(std::string(token.start, token.length), "hello_world");

    token = graphql_lexer_next(lexer);
    EXPECT_EQ(token.kind, GRAPHQL_TOKEN_NAME);
    EXPECT_EQ(std::string(token.start, token.length), "__schema");
}

TEST_F(GraphQLLexerTest, BooleanKeywords) {
    const char* src = "true false";
    lexer = graphql_lexer_create(src, strlen(src));
    ASSERT_NE(lexer, nullptr);
    graphql_token_t token = graphql_lexer_next(lexer);
    EXPECT_EQ(token.kind, GRAPHQL_TOKEN_BOOL);
    EXPECT_EQ(std::string(token.start, token.length), "true");

    token = graphql_lexer_next(lexer);
    EXPECT_EQ(token.kind, GRAPHQL_TOKEN_BOOL);
    EXPECT_EQ(std::string(token.start, token.length), "false");
}

TEST_F(GraphQLLexerTest, NullKeyword) {
    const char* src = "null";
    lexer = graphql_lexer_create(src, strlen(src));
    ASSERT_NE(lexer, nullptr);
    graphql_token_t token = graphql_lexer_next(lexer);
    EXPECT_EQ(token.kind, GRAPHQL_TOKEN_NULL);
}

// ============================================================
// Numbers
// ============================================================

TEST_F(GraphQLLexerTest, Integers) {
    const char* src = "42 0 -7 100";
    lexer = graphql_lexer_create(src, strlen(src));
    ASSERT_NE(lexer, nullptr);

    graphql_token_t token = graphql_lexer_next(lexer);
    EXPECT_EQ(token.kind, GRAPHQL_TOKEN_INT);
    EXPECT_EQ(std::string(token.start, token.length), "42");

    token = graphql_lexer_next(lexer);
    EXPECT_EQ(token.kind, GRAPHQL_TOKEN_INT);
    EXPECT_EQ(std::string(token.start, token.length), "0");

    token = graphql_lexer_next(lexer);
    EXPECT_EQ(token.kind, GRAPHQL_TOKEN_INT);
    EXPECT_EQ(std::string(token.start, token.length), "-7");

    token = graphql_lexer_next(lexer);
    EXPECT_EQ(token.kind, GRAPHQL_TOKEN_INT);
    EXPECT_EQ(std::string(token.start, token.length), "100");
}

TEST_F(GraphQLLexerTest, Floats) {
    const char* src = "3.14 -0.5 1e10 2.5E-3";
    lexer = graphql_lexer_create(src, strlen(src));
    ASSERT_NE(lexer, nullptr);

    graphql_token_t token = graphql_lexer_next(lexer);
    EXPECT_EQ(token.kind, GRAPHQL_TOKEN_FLOAT);
    EXPECT_EQ(std::string(token.start, token.length), "3.14");

    token = graphql_lexer_next(lexer);
    EXPECT_EQ(token.kind, GRAPHQL_TOKEN_FLOAT);
    EXPECT_EQ(std::string(token.start, token.length), "-0.5");

    token = graphql_lexer_next(lexer);
    EXPECT_EQ(token.kind, GRAPHQL_TOKEN_FLOAT);
    EXPECT_EQ(std::string(token.start, token.length), "1e10");

    token = graphql_lexer_next(lexer);
    EXPECT_EQ(token.kind, GRAPHQL_TOKEN_FLOAT);
    EXPECT_EQ(std::string(token.start, token.length), "2.5E-3");
}

// ============================================================
// Strings
// ============================================================

TEST_F(GraphQLLexerTest, Strings) {
    const char* src = "\"hello\" \"world\"";
    lexer = graphql_lexer_create(src, strlen(src));
    ASSERT_NE(lexer, nullptr);

    graphql_token_t token = graphql_lexer_next(lexer);
    EXPECT_EQ(token.kind, GRAPHQL_TOKEN_STRING);
    EXPECT_EQ(std::string(token.start, token.length), "hello");

    token = graphql_lexer_next(lexer);
    EXPECT_EQ(token.kind, GRAPHQL_TOKEN_STRING);
    EXPECT_EQ(std::string(token.start, token.length), "world");
}

TEST_F(GraphQLLexerTest, StringWithEscapes) {
    const char* src = "\"hello\\nworld\"";
    lexer = graphql_lexer_create(src, strlen(src));
    ASSERT_NE(lexer, nullptr);
    graphql_token_t token = graphql_lexer_next(lexer);
    EXPECT_EQ(token.kind, GRAPHQL_TOKEN_STRING);
}

// ============================================================
// Peek and accept
// ============================================================

TEST_F(GraphQLLexerTest, PeekDoesNotAdvance) {
    const char* src = "type User";
    lexer = graphql_lexer_create(src, strlen(src));
    ASSERT_NE(lexer, nullptr);

    graphql_token_t token = graphql_lexer_peek(lexer);
    EXPECT_EQ(token.kind, GRAPHQL_TOKEN_NAME);
    EXPECT_EQ(std::string(token.start, token.length), "type");

    // Peek again - should return same token without advancing
    token = graphql_lexer_peek(lexer);
    EXPECT_EQ(token.kind, GRAPHQL_TOKEN_NAME);
    EXPECT_EQ(std::string(token.start, token.length), "type");

    // Next should return the peeked token
    token = graphql_lexer_next(lexer);
    EXPECT_EQ(token.kind, GRAPHQL_TOKEN_NAME);
    EXPECT_EQ(std::string(token.start, token.length), "type");

    // Next should advance to second token
    token = graphql_lexer_next(lexer);
    EXPECT_EQ(token.kind, GRAPHQL_TOKEN_NAME);
    EXPECT_EQ(std::string(token.start, token.length), "User");
}

TEST_F(GraphQLLexerTest, AcceptConsumesToken) {
    const char* src = "type User";
    lexer = graphql_lexer_create(src, strlen(src));
    ASSERT_NE(lexer, nullptr);

    // Accept "type" as NAME
    EXPECT_TRUE(graphql_lexer_accept(lexer, GRAPHQL_TOKEN_NAME));
    // Accept "User" as NAME
    EXPECT_TRUE(graphql_lexer_accept(lexer, GRAPHQL_TOKEN_NAME));
    // No more NAME tokens
    EXPECT_FALSE(graphql_lexer_accept(lexer, GRAPHQL_TOKEN_NAME));
    // Should be at EOF
    EXPECT_EQ(graphql_lexer_next(lexer).kind, GRAPHQL_TOKEN_EOF);
}

// ============================================================
// Position tracking
// ============================================================

TEST_F(GraphQLLexerTest, LineColumnTracking) {
    const char* src = "type\n  User";
    lexer = graphql_lexer_create(src, strlen(src));
    ASSERT_NE(lexer, nullptr);

    graphql_token_t token = graphql_lexer_next(lexer);
    EXPECT_EQ(token.kind, GRAPHQL_TOKEN_NAME);
    EXPECT_EQ(token.line, 1);

    token = graphql_lexer_next(lexer);
    EXPECT_EQ(token.kind, GRAPHQL_TOKEN_NAME);
    EXPECT_EQ(token.line, 2);
}

// ============================================================
// SDL-like input
// ============================================================

TEST_F(GraphQLLexerTest, TypeDefinition) {
    const char* sdl = "type User { name: String age: Int }";
    lexer = graphql_lexer_create(sdl, strlen(sdl));
    ASSERT_NE(lexer, nullptr);

    graphql_token_kind_t expected[] = {
        GRAPHQL_TOKEN_NAME,     // type
        GRAPHQL_TOKEN_NAME,     // User
        GRAPHQL_TOKEN_LBRACE,
        GRAPHQL_TOKEN_NAME,     // name
        GRAPHQL_TOKEN_COLON,
        GRAPHQL_TOKEN_NAME,     // String
        GRAPHQL_TOKEN_NAME,     // age
        GRAPHQL_TOKEN_COLON,
        GRAPHQL_TOKEN_NAME,     // Int
        GRAPHQL_TOKEN_RBRACE,
        GRAPHQL_TOKEN_EOF
    };

    for (int i = 0; i < 11; i++) {
        graphql_token_t token = graphql_lexer_next(lexer);
        EXPECT_EQ(token.kind, expected[i]) << "Token " << i << " wrong kind, got "
            << token.kind << " expected " << expected[i];
    }
}

TEST_F(GraphQLLexerTest, QueryWithDirectives) {
    const char* query = "query { user(id: \"1\") { name @skip(if: true) } }";
    lexer = graphql_lexer_create(query, strlen(query));
    ASSERT_NE(lexer, nullptr);

    // Check key tokens
    EXPECT_EQ(graphql_lexer_next(lexer).kind, GRAPHQL_TOKEN_NAME);     // query
    EXPECT_EQ(graphql_lexer_next(lexer).kind, GRAPHQL_TOKEN_LBRACE);
    EXPECT_EQ(graphql_lexer_next(lexer).kind, GRAPHQL_TOKEN_NAME);     // user
    EXPECT_EQ(graphql_lexer_next(lexer).kind, GRAPHQL_TOKEN_LPAREN);
    EXPECT_EQ(graphql_lexer_next(lexer).kind, GRAPHQL_TOKEN_NAME);     // id
    EXPECT_EQ(graphql_lexer_next(lexer).kind, GRAPHQL_TOKEN_COLON);
    EXPECT_EQ(graphql_lexer_next(lexer).kind, GRAPHQL_TOKEN_STRING);   // "1"
    EXPECT_EQ(graphql_lexer_next(lexer).kind, GRAPHQL_TOKEN_RPAREN);
    EXPECT_EQ(graphql_lexer_next(lexer).kind, GRAPHQL_TOKEN_LBRACE);
    EXPECT_EQ(graphql_lexer_next(lexer).kind, GRAPHQL_TOKEN_NAME);     // name
    EXPECT_EQ(graphql_lexer_next(lexer).kind, GRAPHQL_TOKEN_AT);       // @
    EXPECT_EQ(graphql_lexer_next(lexer).kind, GRAPHQL_TOKEN_NAME);     // skip
    EXPECT_EQ(graphql_lexer_next(lexer).kind, GRAPHQL_TOKEN_LPAREN);
    EXPECT_EQ(graphql_lexer_next(lexer).kind, GRAPHQL_TOKEN_NAME);     // if
    EXPECT_EQ(graphql_lexer_next(lexer).kind, GRAPHQL_TOKEN_COLON);
    EXPECT_EQ(graphql_lexer_next(lexer).kind, GRAPHQL_TOKEN_BOOL);     // true
}

TEST_F(GraphQLLexerTest, ListType) {
    const char* sdl = "type User { friends: [User] }";
    lexer = graphql_lexer_create(sdl, strlen(sdl));
    ASSERT_NE(lexer, nullptr);

    // Skip to [User]
    for (int i = 0; i < 5; i++) graphql_lexer_next(lexer);  // type User { friends :
    EXPECT_EQ(graphql_lexer_next(lexer).kind, GRAPHQL_TOKEN_LBRACKET);
    EXPECT_EQ(graphql_lexer_next(lexer).kind, GRAPHQL_TOKEN_NAME);     // User
    EXPECT_EQ(graphql_lexer_next(lexer).kind, GRAPHQL_TOKEN_RBRACKET);
}

TEST_F(GraphQLLexerTest, NonNullType) {
    const char* sdl = "type User { id: ID! }";
    lexer = graphql_lexer_create(sdl, strlen(sdl));
    ASSERT_NE(lexer, nullptr);

    // Skip to ID!
    for (int i = 0; i < 5; i++) graphql_lexer_next(lexer);  // type User { id :
    EXPECT_EQ(graphql_lexer_next(lexer).kind, GRAPHQL_TOKEN_NAME);     // ID
    EXPECT_EQ(graphql_lexer_next(lexer).kind, GRAPHQL_TOKEN_BANG);    // !
}