//
// GraphQL Parser - Recursive descent parser for GraphQL SDL and queries
// Created: 2026-04-12
//

#ifndef WAVEDB_GRAPHQL_PARSER_H
#define WAVEDB_GRAPHQL_PARSER_H

#include "graphql_lexer.h"
#include "graphql_types.h"

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================
// AST node kinds
// ============================================================

typedef enum {
    GRAPHQL_AST_DOCUMENT,
    GRAPHQL_AST_OPERATION,        // query/mutation operation
    GRAPHQL_AST_FIELD,           // field selection
    GRAPHQL_AST_ARGUMENT,        // field argument (name: value)
    GRAPHQL_AST_FRAGMENT,        // fragment definition
    GRAPHQL_AST_FRAGMENT_SPREAD,  // ...FragmentName
    GRAPHQL_AST_INLINE_FRAGMENT,  // ... on Type { }
    GRAPHQL_AST_TYPE_DEFINITION, // type User { ... }
    GRAPHQL_AST_ENUM_DEFINITION,  // enum Role { ... }
    GRAPHQL_AST_SCHEMA_DEFINITION, // schema { query: Query }
    GRAPHQL_AST_FIELD_DEFINITION, // field in a type definition
    GRAPHQL_AST_TYPE_REFERENCE,    // Type, [Type], Type!
    GRAPHQL_AST_SCALAR_DEFINITION, // scalar Date
    GRAPHQL_AST_TYPE_EXTENSION,   // extend type User { ... }
} graphql_ast_kind_t;

// ============================================================
// AST node
// ============================================================

typedef struct graphql_ast_node_t graphql_ast_node_t;

struct graphql_ast_node_t {
    graphql_ast_kind_t kind;
    char* name;                              // Node name (type name, field name, etc.)
    char* alias;                             // Field alias (NULL if none)
    vec_t(graphql_ast_node_t*) children;    // Sub-fields, type fields, etc.
    vec_t(graphql_ast_node_t*) arguments;   // Arguments (key-value pairs)
    graphql_literal_t* literal;             // Argument value (NULL for most nodes)
    vec_t(graphql_directive_t*) directives; // Directives on this node
    graphql_type_ref_t* type_ref;          // Type reference (for field definitions)
    bool is_required;                       // Non-null modifier
    struct graphql_ast_node_t* parent;      // Parent node (NULL for root)
    size_t line;                             // Source line
    size_t column;                           // Source column
};

// ============================================================
// Parser functions
// ============================================================

/**
 * Parse a GraphQL document (SDL, query, or mutation).
 *
 * @param source  GraphQL source string
 * @param length  Source string length
 * @return AST root node or NULL on parse error
 */
graphql_ast_node_t* graphql_parse(const char* source, size_t length);

/**
 * Destroy an AST node (recursively destroys children).
 *
 * @param ast  AST node to destroy
 */
void graphql_ast_destroy(graphql_ast_node_t* ast);

/**
 * Create a new AST node.
 *
 * @param kind    Node kind
 * @param name    Node name (copied, may be NULL)
 * @param line    Source line
 * @param column  Source column
 * @return New AST node or NULL on failure
 */
graphql_ast_node_t* graphql_ast_node_create(graphql_ast_kind_t kind,
                                             const char* name,
                                             size_t line,
                                             size_t column);

#ifdef __cplusplus
}
#endif

#endif // WAVEDB_GRAPHQL_PARSER_H