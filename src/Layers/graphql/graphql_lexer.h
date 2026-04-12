//
// GraphQL Lexer - Tokenizer for GraphQL SDL and query language
// Created: 2026-04-12
//

#ifndef WAVEDB_GRAPHQL_LEXER_H
#define WAVEDB_GRAPHQL_LEXER_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================
// Token types
// ============================================================

typedef enum {
    // Value tokens
    GRAPHQL_TOKEN_NAME,          // Identifier: name starts with letter/_, followed by letter/digit/_
    GRAPHQL_TOKEN_STRING,        // String literal: "hello"
    GRAPHQL_TOKEN_INT,           // Integer literal: 42, -7
    GRAPHQL_TOKEN_FLOAT,         // Float literal: 3.14, -0.5
    GRAPHQL_TOKEN_BOOL,          // Boolean literal: true, false
    GRAPHQL_TOKEN_NULL,          // Null literal: null

    // Punctuation tokens
    GRAPHQL_TOKEN_LBRACE,        // {
    GRAPHQL_TOKEN_RBRACE,        // }
    GRAPHQL_TOKEN_LPAREN,        // (
    GRAPHQL_TOKEN_RPAREN,        // )
    GRAPHQL_TOKEN_LBRACKET,      // [
    GRAPHQL_TOKEN_RBRACKET,      // ]
    GRAPHQL_TOKEN_COLON,         // :
    GRAPHQL_TOKEN_BANG,          // !
    GRAPHQL_TOKEN_AT,            // @
    GRAPHQL_TOKEN_DOTDOTDOT,     // ...
    GRAPHQL_TOKEN_EQUALS,        // =
    GRAPHQL_TOKEN_AMP,           // &
    GRAPHQL_TOKEN_PIPE,          // |

    // Special
    GRAPHQL_TOKEN_EOF,           // End of input
    GRAPHQL_TOKEN_ERROR,         // Lexer error
} graphql_token_kind_t;

// ============================================================
// Token
// ============================================================

typedef struct {
    graphql_token_kind_t kind;
    const char* start;            // Pointer into source string
    size_t length;               // Token length
    size_t line;                 // 1-based line number
    size_t column;               // 1-based column number
} graphql_token_t;

// ============================================================
// Lexer state
// ============================================================

typedef struct graphql_lexer_t graphql_lexer_t;

/**
 * Create a lexer for a GraphQL source string.
 *
 * The source string must remain valid for the lifetime of the lexer.
 *
 * @param source  GraphQL source string
 * @param length  Source string length
 * @return New lexer or NULL on failure
 */
graphql_lexer_t* graphql_lexer_create(const char* source, size_t length);

/**
 * Destroy a lexer.
 *
 * @param lexer  Lexer to destroy
 */
void graphql_lexer_destroy(graphql_lexer_t* lexer);

/**
 * Get the next token from the lexer.
 *
 * Advances the lexer position past the token.
 *
 * @param lexer  Lexer
 * @return Next token
 */
graphql_token_t graphql_lexer_next(graphql_lexer_t* lexer);

/**
 * Peek at the next token without advancing.
 *
 * @param lexer  Lexer
 * @return Next token (without advancing position)
 */
graphql_token_t graphql_lexer_peek(graphql_lexer_t* lexer);

/**
 * Accept a token of the given kind if it matches.
 *
 * If the next token matches, advances past it and returns true.
 * Otherwise, returns false without advancing.
 *
 * @param lexer  Lexer
 * @param kind  Expected token kind
 * @return true if token was accepted, false otherwise
 */
bool graphql_lexer_accept(graphql_lexer_t* lexer, graphql_token_kind_t kind);

/**
 * Expect a token of the given kind.
 *
 * If the next token doesn't match, sets an error state.
 *
 * @param lexer  Lexer
 * @param kind  Expected token kind
 * @return The consumed token
 */
graphql_token_t graphql_lexer_expect(graphql_lexer_t* lexer, graphql_token_kind_t kind);

/**
 * Check if the lexer has encountered an error.
 *
 * @param lexer  Lexer
 * @return true if there's an error
 */
bool graphql_lexer_has_error(graphql_lexer_t* lexer);

/**
 * Get the lexer's error message (if any).
 *
 * @param lexer  Lexer
 * @return Error message or NULL
 */
const char* graphql_lexer_error_message(graphql_lexer_t* lexer);

#ifdef __cplusplus
}
#endif

#endif // WAVEDB_GRAPHQL_LEXER_H