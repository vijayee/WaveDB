//
// GraphQL Lexer - Tokenizer implementation
// Created: 2026-04-12
//

#include "graphql_lexer.h"
#include "../../Util/allocator.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

// Lexer state (internal)
struct graphql_lexer_t {
    const char* source;        // Source string (not owned)
    size_t length;            // Source length
    size_t pos;               // Current position
    size_t line;              // Current line (1-based)
    size_t column;            // Current column (1-based)

    // Peek buffer
    graphql_token_t peeked;   // Buffered peek token
    bool has_peeked;          // Whether peeked token is valid

    // Error state
    bool has_error;
    char* error_message;
};

// Forward declarations
static graphql_token_t lexer_next_token(graphql_lexer_t* lexer);
static void skip_whitespace_and_comments(graphql_lexer_t* lexer);
static graphql_token_t make_token(graphql_lexer_t* lexer, graphql_token_kind_t kind, const char* start, size_t length);

graphql_lexer_t* graphql_lexer_create(const char* source, size_t length) {
    graphql_lexer_t* lexer = get_clear_memory(sizeof(graphql_lexer_t));
    if (lexer == NULL) return NULL;
    lexer->source = source;
    lexer->length = length;
    lexer->pos = 0;
    lexer->line = 1;
    lexer->column = 1;
    lexer->has_peeked = false;
    lexer->has_error = false;
    lexer->error_message = NULL;
    return lexer;
}

void graphql_lexer_destroy(graphql_lexer_t* lexer) {
    if (lexer == NULL) return;
    free(lexer->error_message);
    free(lexer);
}

graphql_token_t graphql_lexer_next(graphql_lexer_t* lexer) {
    if (lexer == NULL) {
        graphql_token_t eof = {GRAPHQL_TOKEN_EOF, NULL, 0, 0, 0};
        return eof;
    }
    if (lexer->has_peeked) {
        lexer->has_peeked = false;
        return lexer->peeked;
    }
    return lexer_next_token(lexer);
}

graphql_token_t graphql_lexer_peek(graphql_lexer_t* lexer) {
    if (lexer == NULL) {
        graphql_token_t eof = {GRAPHQL_TOKEN_EOF, NULL, 0, 0, 0};
        return eof;
    }
    if (lexer->has_peeked) {
        return lexer->peeked;
    }
    lexer->peeked = lexer_next_token(lexer);
    lexer->has_peeked = true;
    return lexer->peeked;
}

bool graphql_lexer_accept(graphql_lexer_t* lexer, graphql_token_kind_t kind) {
    graphql_token_t token = graphql_lexer_peek(lexer);
    if (token.kind == kind) {
        lexer->has_peeked = false;
        return true;
    }
    return false;
}

graphql_token_t graphql_lexer_expect(graphql_lexer_t* lexer, graphql_token_kind_t kind) {
    graphql_token_t token = graphql_lexer_next(lexer);
    if (token.kind != kind) {
        lexer->has_error = true;
        free(lexer->error_message);
        static const char* kind_names[] = {
            "NAME", "STRING", "INT", "FLOAT", "BOOL", "NULL",
            "LBRACE", "RBRACE", "LPAREN", "RPAREN",
            "LBRACKET", "RBRACKET", "COLON", "BANG", "AT",
            "DOTDOTDOT", "EQUALS", "AMP", "PIPE", "ERROR", "EOF"
        };
        char msg[128];
        snprintf(msg, sizeof(msg), "Expected %s but got %s",
                 kind_names[kind], kind_names[token.kind]);
        lexer->error_message = strdup(msg);
    }
    return token;
}

bool graphql_lexer_has_error(graphql_lexer_t* lexer) {
    return lexer != NULL && lexer->has_error;
}

const char* graphql_lexer_error_message(graphql_lexer_t* lexer) {
    if (lexer == NULL) return NULL;
    return lexer->error_message;
}

// ============================================================
// Internal lexer functions
// ============================================================

static void skip_whitespace_and_comments(graphql_lexer_t* lexer) {
    while (lexer->pos < lexer->length) {
        char c = lexer->source[lexer->pos];
        // Skip whitespace
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            if (c == '\n') {
                lexer->line++;
                lexer->column = 1;
            } else {
                lexer->column++;
            }
            lexer->pos++;
            continue;
        }
        // Skip comments (# to end of line)
        if (c == '#') {
            while (lexer->pos < lexer->length && lexer->source[lexer->pos] != '\n') {
                lexer->pos++;
            }
            continue;
        }
        // Skip comma (ignored in GraphQL)
        if (c == ',') {
            lexer->pos++;
            lexer->column++;
            continue;
        }
        break;
    }
}

static graphql_token_t make_token(graphql_lexer_t* lexer, graphql_token_kind_t kind,
                                    const char* start, size_t length) {
    graphql_token_t token;
    token.kind = kind;
    token.start = start;
    token.length = length;
    token.line = lexer->line;
    token.column = lexer->column;
    return token;
}

static graphql_token_t lexer_next_token(graphql_lexer_t* lexer) {
    skip_whitespace_and_comments(lexer);

    if (lexer->pos >= lexer->length) {
        return make_token(lexer, GRAPHQL_TOKEN_EOF, lexer->source + lexer->pos, 0);
    }

    size_t start_pos = lexer->pos;
    size_t start_line = lexer->line;
    size_t start_col = lexer->column;
    char c = lexer->source[lexer->pos];

    // String literal
    if (c == '"') {
        // Lexer captures raw string content including escape sequences.
        // The parser's decode_string_literal() function processes \n, \t, etc.
        lexer->pos++;
        size_t str_start = lexer->pos;
        while (lexer->pos < lexer->length && lexer->source[lexer->pos] != '"') {
            if (lexer->source[lexer->pos] == '\\') {
                lexer->pos++;  // Skip escaped character
            }
            lexer->pos++;
        }
        size_t str_len = lexer->pos - str_start;
        if (lexer->pos < lexer->length) lexer->pos++;  // Skip closing quote
        return make_token(lexer, GRAPHQL_TOKEN_STRING, lexer->source + str_start, str_len);
    }

    // Punctuation
    switch (c) {
        case '{': lexer->pos++; lexer->column++; return make_token(lexer, GRAPHQL_TOKEN_LBRACE, lexer->source + start_pos, 1);
        case '}': lexer->pos++; lexer->column++; return make_token(lexer, GRAPHQL_TOKEN_RBRACE, lexer->source + start_pos, 1);
        case '(': lexer->pos++; lexer->column++; return make_token(lexer, GRAPHQL_TOKEN_LPAREN, lexer->source + start_pos, 1);
        case ')': lexer->pos++; lexer->column++; return make_token(lexer, GRAPHQL_TOKEN_RPAREN, lexer->source + start_pos, 1);
        case '[': lexer->pos++; lexer->column++; return make_token(lexer, GRAPHQL_TOKEN_LBRACKET, lexer->source + start_pos, 1);
        case ']': lexer->pos++; lexer->column++; return make_token(lexer, GRAPHQL_TOKEN_RBRACKET, lexer->source + start_pos, 1);
        case ':': lexer->pos++; lexer->column++; return make_token(lexer, GRAPHQL_TOKEN_COLON, lexer->source + start_pos, 1);
        case '!': lexer->pos++; lexer->column++; return make_token(lexer, GRAPHQL_TOKEN_BANG, lexer->source + start_pos, 1);
        case '@': lexer->pos++; lexer->column++; return make_token(lexer, GRAPHQL_TOKEN_AT, lexer->source + start_pos, 1);
        case '=': lexer->pos++; lexer->column++; return make_token(lexer, GRAPHQL_TOKEN_EQUALS, lexer->source + start_pos, 1);
        case '&': lexer->pos++; lexer->column++; return make_token(lexer, GRAPHQL_TOKEN_AMP, lexer->source + start_pos, 1);
        case '|': lexer->pos++; lexer->column++; return make_token(lexer, GRAPHQL_TOKEN_PIPE, lexer->source + start_pos, 1);
        case '.': {
            // Check for ... (spread operator)
            if (lexer->pos + 2 < lexer->length &&
                lexer->source[lexer->pos + 1] == '.' &&
                lexer->source[lexer->pos + 2] == '.') {
                lexer->pos += 3;
                lexer->column += 3;
                return make_token(lexer, GRAPHQL_TOKEN_DOTDOTDOT, lexer->source + start_pos, 3);
            }
            // Single dot is an error in GraphQL
            lexer->has_error = true;
            lexer->error_message = strdup("Unexpected character '.'");
            return make_token(lexer, GRAPHQL_TOKEN_ERROR, lexer->source + start_pos, 1);
        }
        default:
            break;
    }

    // Number (INT or FLOAT)
    if (c == '-' || (c >= '0' && c <= '9')) {
        size_t num_start = lexer->pos;
        if (c == '-') lexer->pos++;
        bool is_float = false;
        while (lexer->pos < lexer->length && lexer->source[lexer->pos] >= '0' && lexer->source[lexer->pos] <= '9') {
            lexer->pos++;
        }
        if (lexer->pos < lexer->length && lexer->source[lexer->pos] == '.') {
            is_float = true;
            lexer->pos++;
            while (lexer->pos < lexer->length && lexer->source[lexer->pos] >= '0' && lexer->source[lexer->pos] <= '9') {
                lexer->pos++;
            }
        }
        // Check for exponent
        if (lexer->pos < lexer->length && (lexer->source[lexer->pos] == 'e' || lexer->source[lexer->pos] == 'E')) {
            is_float = true;
            lexer->pos++;
            if (lexer->pos < lexer->length && (lexer->source[lexer->pos] == '+' || lexer->source[lexer->pos] == '-')) {
                lexer->pos++;
            }
            while (lexer->pos < lexer->length && lexer->source[lexer->pos] >= '0' && lexer->source[lexer->pos] <= '9') {
                lexer->pos++;
            }
        }
        lexer->column += lexer->pos - num_start;
        return make_token(lexer, is_float ? GRAPHQL_TOKEN_FLOAT : GRAPHQL_TOKEN_INT,
                          lexer->source + num_start, lexer->pos - num_start);
    }

    // Name / keyword (true, false, null)
    if (c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
        size_t name_start = lexer->pos;
        while (lexer->pos < lexer->length) {
            char nc = lexer->source[lexer->pos];
            if (nc == '_' || (nc >= 'a' && nc <= 'z') || (nc >= 'A' && nc <= 'Z') || (nc >= '0' && nc <= '9')) {
                lexer->pos++;
            } else {
                break;
            }
        }
        size_t name_len = lexer->pos - name_start;
        lexer->column += name_len;

        // Check for keywords
        if (name_len == 4 && strncmp(lexer->source + name_start, "true", 4) == 0) {
            return make_token(lexer, GRAPHQL_TOKEN_BOOL, lexer->source + name_start, 4);
        }
        if (name_len == 5 && strncmp(lexer->source + name_start, "false", 5) == 0) {
            return make_token(lexer, GRAPHQL_TOKEN_BOOL, lexer->source + name_start, 5);
        }
        if (name_len == 4 && strncmp(lexer->source + name_start, "null", 4) == 0) {
            return make_token(lexer, GRAPHQL_TOKEN_NULL, lexer->source + name_start, 4);
        }
        return make_token(lexer, GRAPHQL_TOKEN_NAME, lexer->source + name_start, name_len);
    }

    // Unknown character
    lexer->has_error = true;
    char* err_msg = malloc(64);
    snprintf(err_msg, 64, "Unexpected character '%c' at line %zu, column %zu",
             c, start_line, start_col);
    lexer->error_message = err_msg;
    lexer->pos++;
    lexer->column++;
    return make_token(lexer, GRAPHQL_TOKEN_ERROR, lexer->source + start_pos, 1);
}