//
// GraphQL Parser - Recursive descent parser for GraphQL SDL and queries
// Created: 2026-04-12
//

#include "graphql_parser.h"
#include "graphql_lexer.h"
#include "../../Util/allocator.h"
#include <stdlib.h>
#include <string.h>

// ============================================================
// Parser state
// ============================================================

typedef struct {
    graphql_lexer_t* lexer;
    bool has_error;
    char* error_message;
} graphql_parser_state_t;

// ============================================================
// Forward declarations
// ============================================================

static graphql_ast_node_t* parse_document(graphql_parser_state_t* state);
static graphql_ast_node_t* parse_definition(graphql_parser_state_t* state);
static graphql_ast_node_t* parse_type_definition(graphql_parser_state_t* state);
static graphql_ast_node_t* parse_type_extension(graphql_parser_state_t* state);
static graphql_ast_node_t* parse_scalar_definition(graphql_parser_state_t* state);
static graphql_ast_node_t* parse_enum_definition(graphql_parser_state_t* state);
static graphql_ast_node_t* parse_schema_definition(graphql_parser_state_t* state);
static graphql_ast_node_t* parse_operation_definition(graphql_parser_state_t* state);
static graphql_ast_node_t* parse_selection_set(graphql_parser_state_t* state);
static graphql_ast_node_t* parse_field(graphql_parser_state_t* state);
static graphql_ast_node_t* parse_fragment_definition(graphql_parser_state_t* state);
static graphql_ast_node_t* parse_fragment_spread(graphql_parser_state_t* state);
static graphql_ast_node_t* parse_inline_fragment(graphql_parser_state_t* state);
static graphql_ast_node_t* parse_field_definition(graphql_parser_state_t* state);
static graphql_type_ref_t* parse_type_reference(graphql_parser_state_t* state);
static graphql_directive_t* parse_directive(graphql_parser_state_t* state);
static graphql_literal_t* parse_literal(graphql_parser_state_t* state);
static graphql_literal_t* parse_object_literal(graphql_parser_state_t* state);
static graphql_literal_t* parse_list_literal(graphql_parser_state_t* state);
static char* token_to_string(graphql_token_t* token);

// ============================================================
// Helper functions
// ============================================================

static char* token_to_string(graphql_token_t* token) {
    if (token == NULL || token->start == NULL || token->length == 0) {
        return strdup("");
    }
    char* str = malloc(token->length + 1);
    if (str == NULL) return NULL;
    memcpy(str, token->start, token->length);
    str[token->length] = '\0';
    return str;
}

// Decode escape sequences in a string literal token value.
// Processes: \n \t \r \" \\ \/ \b \f \uXXXX
// Returns a newly allocated string with escapes decoded.
static char* decode_string_literal(const char* src, size_t len) {
    char* result = malloc(len + 1);
    if (result == NULL) return NULL;
    size_t j = 0;
    for (size_t i = 0; i < len; i++) {
        if (src[i] == '\\' && i + 1 < len) {
            char next = src[i + 1];
            switch (next) {
                case 'n':  result[j++] = '\n'; i++; break;
                case 't':  result[j++] = '\t'; i++; break;
                case 'r':  result[j++] = '\r'; i++; break;
                case '"':  result[j++] = '"';  i++; break;
                case '\\': result[j++] = '\\'; i++; break;
                case '/':  result[j++] = '/';  i++; break;
                case 'b':  result[j++] = '\b'; i++; break;
                case 'f':  result[j++] = '\f'; i++; break;
                case 'u': {
                    // \uXXXX unicode escape
                    if (i + 5 < len) {
                        char hex[5] = {src[i+2], src[i+3], src[i+4], src[i+5], '\0'};
                        unsigned long cp = strtoul(hex, NULL, 16);
                        if (cp < 0x80) {
                            result[j++] = (char)cp;
                        } else if (cp < 0x800) {
                            result[j++] = (char)(0xC0 | (cp >> 6));
                            result[j++] = (char)(0x80 | (cp & 0x3F));
                        } else {
                            result[j++] = (char)(0xE0 | (cp >> 12));
                            result[j++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                            result[j++] = (char)(0x80 | (cp & 0x3F));
                        }
                        i += 5;
                    } else {
                        result[j++] = src[i];  // Not enough hex chars, keep as-is
                    }
                    break;
                }
                default:
                    result[j++] = src[i];  // Unknown escape, keep as-is
                    break;
            }
        } else {
            result[j++] = src[i];
        }
    }
    result[j] = '\0';
    return result;
}

static void parser_error(graphql_parser_state_t* state, const char* message) {
    state->has_error = true;
    free(state->error_message);
    state->error_message = strdup(message);
}

static bool is_type_name(graphql_token_kind_t kind) {
    return kind == GRAPHQL_TOKEN_NAME || kind == GRAPHQL_TOKEN_NULL ||
           kind == GRAPHQL_TOKEN_BOOL;
}

// ============================================================
// Public API
// ============================================================

graphql_ast_node_t* graphql_parse(const char* source, size_t length, char** error_out) {
    if (source == NULL || length == 0) {
        if (error_out != NULL) *error_out = strdup("Empty or null input");
        return NULL;
    }

    graphql_parser_state_t state;
    memset(&state, 0, sizeof(state));
    state.lexer = graphql_lexer_create(source, length);
    if (state.lexer == NULL) {
        if (error_out != NULL) *error_out = strdup("Failed to create lexer");
        return NULL;
    }

    graphql_ast_node_t* doc = parse_document(&state);

    if (state.has_error) {
        graphql_ast_destroy(doc);
        graphql_lexer_destroy(state.lexer);
        if (error_out != NULL) {
            *error_out = state.error_message;
        } else {
            free(state.error_message);
        }
        return NULL;
    }

    graphql_lexer_destroy(state.lexer);
    free(state.error_message);
    return doc;
}

void graphql_ast_destroy(graphql_ast_node_t* ast) {
    if (ast == NULL) return;
    free(ast->name);
    free(ast->alias);
    int i;
    graphql_ast_node_t* child;
    vec_foreach(&ast->children, child, i) {
        graphql_ast_destroy(child);
    }
    vec_deinit(&ast->children);
    graphql_ast_node_t* arg;
    vec_foreach(&ast->arguments, arg, i) {
        graphql_ast_destroy(arg);
    }
    vec_deinit(&ast->arguments);
    if (ast->literal) {
        switch (ast->literal->kind) {
            case GRAPHQL_LITERAL_STRING:
            case GRAPHQL_LITERAL_ENUM:
                free(ast->literal->string_val);
                break;
            case GRAPHQL_LITERAL_OBJECT: {
                int i;
                graphql_object_field_t* f;
                vec_foreach(&ast->literal->object_fields, f, i) {
                    free(f->name);
                    // Recursively destroy nested literal
                    if (f->value) {
                        // Use a dummy node to destroy the literal
                        graphql_ast_node_t dummy;
                        memset(&dummy, 0, sizeof(dummy));
                        dummy.literal = f->value;
                        graphql_ast_destroy(&dummy);
                    }
                    free(f);
                }
                vec_deinit(&ast->literal->object_fields);
                break;
            }
            case GRAPHQL_LITERAL_LIST: {
                int i;
                graphql_literal_t* item;
                vec_foreach(&ast->literal->list_items, item, i) {
                    graphql_ast_node_t dummy;
                    memset(&dummy, 0, sizeof(dummy));
                    dummy.literal = item;
                    graphql_ast_destroy(&dummy);
                }
                vec_deinit(&ast->literal->list_items);
                break;
            }
            default:
                break;
        }
        free(ast->literal);
    }
    graphql_directive_t* dir;
    vec_foreach(&ast->directives, dir, i) {
        graphql_directive_destroy(dir);
    }
    vec_deinit(&ast->directives);
    graphql_type_ref_destroy(ast->type_ref);
    free(ast);
}

graphql_ast_node_t* graphql_ast_node_create(graphql_ast_kind_t kind,
                                             const char* name,
                                             size_t line,
                                             size_t column) {
    graphql_ast_node_t* node = get_clear_memory(sizeof(graphql_ast_node_t));
    if (node == NULL) return NULL;
    node->kind = kind;
    if (name) {
        node->name = strdup(name);
        if (node->name == NULL) {
            free(node);
            return NULL;
        }
    }
    node->line = line;
    node->column = column;
    return node;
}

// ============================================================
// Document parsing
// ============================================================

static graphql_ast_node_t* parse_document(graphql_parser_state_t* state) {
    graphql_ast_node_t* doc = graphql_ast_node_create(GRAPHQL_AST_DOCUMENT, NULL, 1, 1);
    if (doc == NULL) return NULL;

    while (graphql_lexer_peek(state->lexer).kind != GRAPHQL_TOKEN_EOF) {
        graphql_ast_node_t* def = parse_definition(state);
        if (def == NULL) {
            if (state->has_error) {
                graphql_ast_destroy(doc);
                return NULL;
            }
            break;
        }
        def->parent = doc;
        vec_push(&doc->children, def);
    }

    return doc;
}

static graphql_ast_node_t* parse_definition(graphql_parser_state_t* state) {
    graphql_token_t token = graphql_lexer_peek(state->lexer);

    if (token.kind == GRAPHQL_TOKEN_NAME) {
        if (token.length == 4 && strncmp(token.start, "type", 4) == 0) {
            return parse_type_definition(state);
        }
        if (token.length == 4 && strncmp(token.start, "enum", 4) == 0) {
            return parse_enum_definition(state);
        }
        if (token.length == 6 && strncmp(token.start, "scalar", 6) == 0) {
            return parse_scalar_definition(state);
        }
        if (token.length == 6 && strncmp(token.start, "extend", 6) == 0) {
            return parse_type_extension(state);
        }
        if (token.length == 6 && strncmp(token.start, "schema", 6) == 0) {
            return parse_schema_definition(state);
        }
        if (token.length == 5 && strncmp(token.start, "query", 5) == 0) {
            return parse_operation_definition(state);
        }
        if (token.length == 8 && strncmp(token.start, "mutation", 8) == 0) {
            return parse_operation_definition(state);
        }
        // "fragment" keyword
        if (token.length == 8 && strncmp(token.start, "fragment", 8) == 0) {
            return parse_fragment_definition(state);
        }
    }

    // Anonymous query: { ... }
    if (token.kind == GRAPHQL_TOKEN_LBRACE) {
        return parse_operation_definition(state);
    }

    parser_error(state, "Expected type, enum, schema, query, mutation, fragment, or '{'");
    return NULL;
}

// ============================================================
// Type definition: type Name { field: Type ... }
// ============================================================

static graphql_ast_node_t* parse_type_definition(graphql_parser_state_t* state) {
    graphql_token_t type_kw = graphql_lexer_expect(state->lexer, GRAPHQL_TOKEN_NAME);
    if (strncmp(type_kw.start, "type", 4) != 0) {
        parser_error(state, "Expected 'type' keyword");
        return NULL;
    }

    graphql_token_t name_tok = graphql_lexer_expect(state->lexer, GRAPHQL_TOKEN_NAME);
    if (name_tok.kind != GRAPHQL_TOKEN_NAME) {
        parser_error(state, "Expected type name");
        return NULL;
    }

    char* name = token_to_string(&name_tok);
    graphql_ast_node_t* type_def = graphql_ast_node_create(GRAPHQL_AST_TYPE_DEFINITION,
                                                             name, name_tok.line, name_tok.column);
    free(name);

    if (type_def == NULL) return NULL;

    // Parse optional directives (e.g., @plural)
    while (graphql_lexer_peek(state->lexer).kind == GRAPHQL_TOKEN_AT) {
        graphql_directive_t* dir = parse_directive(state);
        if (dir == NULL) {
            graphql_ast_destroy(type_def);
            return NULL;
        }
        vec_push(&type_def->directives, dir);
    }

    // Parse { fields }
    if (!graphql_lexer_accept(state->lexer, GRAPHQL_TOKEN_LBRACE)) {
        parser_error(state, "Expected '{' after type name");
        graphql_ast_destroy(type_def);
        return NULL;
    }

    while (graphql_lexer_peek(state->lexer).kind != GRAPHQL_TOKEN_RBRACE &&
           graphql_lexer_peek(state->lexer).kind != GRAPHQL_TOKEN_EOF) {
        graphql_ast_node_t* field = parse_field_definition(state);
        if (field == NULL) {
            graphql_ast_destroy(type_def);
            return NULL;
        }
        field->parent = type_def;
        vec_push(&type_def->children, field);
    }

    if (!graphql_lexer_accept(state->lexer, GRAPHQL_TOKEN_RBRACE)) {
        parser_error(state, "Expected '}' after type fields");
        graphql_ast_destroy(type_def);
        return NULL;
    }

    return type_def;
}

// ============================================================
// Type extension: extend type Name { fields... }
// ============================================================

static graphql_ast_node_t* parse_type_extension(graphql_parser_state_t* state) {
    graphql_token_t extend_kw = graphql_lexer_expect(state->lexer, GRAPHQL_TOKEN_NAME);
    if (strncmp(extend_kw.start, "extend", 6) != 0) {
        parser_error(state, "Expected 'extend' keyword");
        return NULL;
    }

    // Expect "type" keyword after "extend"
    graphql_token_t type_kw = graphql_lexer_expect(state->lexer, GRAPHQL_TOKEN_NAME);
    if (type_kw.kind != GRAPHQL_TOKEN_NAME || strncmp(type_kw.start, "type", 4) != 0) {
        parser_error(state, "Expected 'type' after 'extend'");
        return NULL;
    }

    graphql_token_t name_tok = graphql_lexer_expect(state->lexer, GRAPHQL_TOKEN_NAME);
    if (name_tok.kind != GRAPHQL_TOKEN_NAME) {
        parser_error(state, "Expected type name after 'extend type'");
        return NULL;
    }

    char* name = token_to_string(&name_tok);
    graphql_ast_node_t* ext_def = graphql_ast_node_create(GRAPHQL_AST_TYPE_EXTENSION,
                                                            name, name_tok.line, name_tok.column);
    free(name);

    if (ext_def == NULL) return NULL;

    // Parse { fields }
    if (!graphql_lexer_accept(state->lexer, GRAPHQL_TOKEN_LBRACE)) {
        parser_error(state, "Expected '{' after extend type name");
        graphql_ast_destroy(ext_def);
        return NULL;
    }

    while (graphql_lexer_peek(state->lexer).kind != GRAPHQL_TOKEN_RBRACE &&
           graphql_lexer_peek(state->lexer).kind != GRAPHQL_TOKEN_EOF) {
        graphql_ast_node_t* field = parse_field_definition(state);
        if (field == NULL) {
            graphql_ast_destroy(ext_def);
            return NULL;
        }
        field->parent = ext_def;
        vec_push(&ext_def->children, field);
    }

    if (!graphql_lexer_accept(state->lexer, GRAPHQL_TOKEN_RBRACE)) {
        parser_error(state, "Expected '}' after extend type fields");
        graphql_ast_destroy(ext_def);
        return NULL;
    }

    return ext_def;
}

// ============================================================
// Scalar definition: scalar Name
// ============================================================

static graphql_ast_node_t* parse_scalar_definition(graphql_parser_state_t* state) {
    graphql_token_t scalar_kw = graphql_lexer_expect(state->lexer, GRAPHQL_TOKEN_NAME);
    if (strncmp(scalar_kw.start, "scalar", 6) != 0) {
        parser_error(state, "Expected 'scalar' keyword");
        return NULL;
    }

    graphql_token_t name_tok = graphql_lexer_expect(state->lexer, GRAPHQL_TOKEN_NAME);
    if (name_tok.kind != GRAPHQL_TOKEN_NAME) {
        parser_error(state, "Expected scalar name");
        return NULL;
    }

    char* name = token_to_string(&name_tok);
    graphql_ast_node_t* scalar_def = graphql_ast_node_create(GRAPHQL_AST_SCALAR_DEFINITION,
                                                              name, name_tok.line, name_tok.column);
    free(name);

    return scalar_def;
}

// ============================================================
// Enum definition: enum Name { VALUE1 VALUE2 ... }
// ============================================================

static graphql_ast_node_t* parse_enum_definition(graphql_parser_state_t* state) {
    graphql_token_t enum_kw = graphql_lexer_expect(state->lexer, GRAPHQL_TOKEN_NAME);
    if (strncmp(enum_kw.start, "enum", 4) != 0) {
        parser_error(state, "Expected 'enum' keyword");
        return NULL;
    }

    graphql_token_t name_tok = graphql_lexer_expect(state->lexer, GRAPHQL_TOKEN_NAME);
    if (name_tok.kind != GRAPHQL_TOKEN_NAME) {
        parser_error(state, "Expected enum name");
        return NULL;
    }

    char* name = token_to_string(&name_tok);
    graphql_ast_node_t* enum_def = graphql_ast_node_create(GRAPHQL_AST_ENUM_DEFINITION,
                                                             name, name_tok.line, name_tok.column);
    free(name);

    if (enum_def == NULL) return NULL;

    // Parse { values }
    if (!graphql_lexer_accept(state->lexer, GRAPHQL_TOKEN_LBRACE)) {
        parser_error(state, "Expected '{' after enum name");
        graphql_ast_destroy(enum_def);
        return NULL;
    }

    while (graphql_lexer_peek(state->lexer).kind != GRAPHQL_TOKEN_RBRACE &&
           graphql_lexer_peek(state->lexer).kind != GRAPHQL_TOKEN_EOF) {
        graphql_token_t value_tok = graphql_lexer_next(state->lexer);
        if (value_tok.kind != GRAPHQL_TOKEN_NAME) {
            parser_error(state, "Expected enum value name");
            graphql_ast_destroy(enum_def);
            return NULL;
        }
        char* val = token_to_string(&value_tok);
        vec_push(&enum_def->children,
                 graphql_ast_node_create(GRAPHQL_AST_FIELD_DEFINITION, val,
                                         value_tok.line, value_tok.column));
        free(val);
    }

    if (!graphql_lexer_accept(state->lexer, GRAPHQL_TOKEN_RBRACE)) {
        parser_error(state, "Expected '}' after enum values");
        graphql_ast_destroy(enum_def);
        return NULL;
    }

    return enum_def;
}

// ============================================================
// Schema definition: schema { query: Query mutation: Mutation }
// ============================================================

static graphql_ast_node_t* parse_schema_definition(graphql_parser_state_t* state) {
    graphql_token_t schema_kw = graphql_lexer_expect(state->lexer, GRAPHQL_TOKEN_NAME);
    if (strncmp(schema_kw.start, "schema", 6) != 0) {
        parser_error(state, "Expected 'schema' keyword");
        return NULL;
    }

    graphql_ast_node_t* schema_def = graphql_ast_node_create(GRAPHQL_AST_SCHEMA_DEFINITION,
                                                               "schema", schema_kw.line, schema_kw.column);
    if (schema_def == NULL) return NULL;

    if (!graphql_lexer_accept(state->lexer, GRAPHQL_TOKEN_LBRACE)) {
        parser_error(state, "Expected '{' after schema");
        graphql_ast_destroy(schema_def);
        return NULL;
    }

    while (graphql_lexer_peek(state->lexer).kind != GRAPHQL_TOKEN_RBRACE &&
           graphql_lexer_peek(state->lexer).kind != GRAPHQL_TOKEN_EOF) {
        graphql_token_t name_tok = graphql_lexer_expect(state->lexer, GRAPHQL_TOKEN_NAME);
        if (name_tok.kind != GRAPHQL_TOKEN_NAME) {
            parser_error(state, "Expected schema field name");
            graphql_ast_destroy(schema_def);
            return NULL;
        }

        if (!graphql_lexer_accept(state->lexer, GRAPHQL_TOKEN_COLON)) {
            parser_error(state, "Expected ':' after schema field name");
            graphql_ast_destroy(schema_def);
            return NULL;
        }

        graphql_type_ref_t* type_ref = parse_type_reference(state);
        if (type_ref == NULL) {
            graphql_ast_destroy(schema_def);
            return NULL;
        }

        char* field_name = token_to_string(&name_tok);
        graphql_ast_node_t* field = graphql_ast_node_create(GRAPHQL_AST_FIELD_DEFINITION,
                                                              field_name, name_tok.line, name_tok.column);
        free(field_name);
        if (field == NULL) {
            graphql_type_ref_destroy(type_ref);
            graphql_ast_destroy(schema_def);
            return NULL;
        }
        field->type_ref = type_ref;
        vec_push(&schema_def->children, field);
    }

    if (!graphql_lexer_accept(state->lexer, GRAPHQL_TOKEN_RBRACE)) {
        parser_error(state, "Expected '}' after schema fields");
        graphql_ast_destroy(schema_def);
        return NULL;
    }

    return schema_def;
}

// ============================================================
// Field definition: name: Type [!] or name(args): Type [!]
// ============================================================

static graphql_ast_node_t* parse_field_definition(graphql_parser_state_t* state) {
    graphql_token_t name_tok = graphql_lexer_expect(state->lexer, GRAPHQL_TOKEN_NAME);
    if (name_tok.kind != GRAPHQL_TOKEN_NAME) {
        parser_error(state, "Expected field name");
        return NULL;
    }

    char* name = token_to_string(&name_tok);
    graphql_ast_node_t* field = graphql_ast_node_create(GRAPHQL_AST_FIELD_DEFINITION,
                                                         name, name_tok.line, name_tok.column);
    free(name);
    if (field == NULL) return NULL;

    // Parse optional arguments: (arg: Type, ...)
    if (graphql_lexer_accept(state->lexer, GRAPHQL_TOKEN_LPAREN)) {
        while (graphql_lexer_peek(state->lexer).kind != GRAPHQL_TOKEN_RPAREN &&
               graphql_lexer_peek(state->lexer).kind != GRAPHQL_TOKEN_EOF) {
            graphql_token_t arg_name = graphql_lexer_expect(state->lexer, GRAPHQL_TOKEN_NAME);
            if (arg_name.kind != GRAPHQL_TOKEN_NAME) {
                parser_error(state, "Expected argument name");
                graphql_ast_destroy(field);
                return NULL;
            }

            if (graphql_lexer_accept(state->lexer, GRAPHQL_TOKEN_COLON)) {
                graphql_type_ref_t* arg_type = parse_type_reference(state);
                if (arg_type == NULL) {
                    graphql_ast_destroy(field);
                    return NULL;
                }
                graphql_type_ref_destroy(arg_type);  // Arguments not fully supported in v1
            }

            // Commas are ignored by the lexer, no need to consume them
        }
        if (!graphql_lexer_accept(state->lexer, GRAPHQL_TOKEN_RPAREN)) {
            parser_error(state, "Expected ')' after arguments");
            graphql_ast_destroy(field);
            return NULL;
        }
    }

    // Parse colon
    if (!graphql_lexer_accept(state->lexer, GRAPHQL_TOKEN_COLON)) {
        parser_error(state, "Expected ':' after field name");
        graphql_ast_destroy(field);
        return NULL;
    }

    // Parse type reference
    graphql_type_ref_t* type_ref = parse_type_reference(state);
    if (type_ref == NULL) {
        graphql_ast_destroy(field);
        return NULL;
    }
    field->type_ref = type_ref;

    // Propagate NON_NULL modifier to is_required flag
    if (type_ref->kind == GRAPHQL_TYPE_NON_NULL) {
        field->is_required = true;
    }

    // Parse optional default value: = literal
    if (graphql_lexer_accept(state->lexer, GRAPHQL_TOKEN_EQUALS)) {
        graphql_literal_t* default_val = parse_literal(state);
        if (default_val != NULL) {
            field->literal = default_val;
        } else {
            parser_error(state, "Expected default value after '='");
            graphql_ast_destroy(field);
            return NULL;
        }
    }

    // Parse optional directives
    while (graphql_lexer_peek(state->lexer).kind == GRAPHQL_TOKEN_AT) {
        graphql_directive_t* dir = parse_directive(state);
        if (dir == NULL) {
            graphql_ast_destroy(field);
            return NULL;
        }
        vec_push(&field->directives, dir);
    }

    return field;
}

// ============================================================
// Type reference: Name, [Type], Type!
// ============================================================

static graphql_type_ref_t* parse_type_reference(graphql_parser_state_t* state) {
    graphql_token_t token = graphql_lexer_peek(state->lexer);

    graphql_type_ref_t* inner = NULL;

    if (token.kind == GRAPHQL_TOKEN_LBRACKET) {
        // List type: [Type]
        graphql_lexer_next(state->lexer);  // consume [
        inner = parse_type_reference(state);  // recursive for nested lists
        if (inner == NULL) return NULL;
        if (!graphql_lexer_accept(state->lexer, GRAPHQL_TOKEN_RBRACKET)) {
            parser_error(state, "Expected ']' after list type");
            graphql_type_ref_destroy(inner);
            return NULL;
        }
        inner = graphql_type_ref_create_list(inner);
    } else if (token.kind == GRAPHQL_TOKEN_NAME) {
        // Named type
        graphql_lexer_next(state->lexer);  // consume name
        char* name = token_to_string(&token);

        // Map built-in scalar names to kinds
        graphql_type_kind_t kind;
        if (strcmp(name, "String") == 0) kind = GRAPHQL_TYPE_SCALAR;
        else if (strcmp(name, "Int") == 0) kind = GRAPHQL_TYPE_SCALAR;
        else if (strcmp(name, "Float") == 0) kind = GRAPHQL_TYPE_SCALAR;
        else if (strcmp(name, "Boolean") == 0) kind = GRAPHQL_TYPE_SCALAR;
        else if (strcmp(name, "ID") == 0) kind = GRAPHQL_TYPE_SCALAR;
        else kind = GRAPHQL_TYPE_OBJECT;  // Custom type

        inner = graphql_type_ref_create_named(kind, name);
        free(name);
    } else {
        parser_error(state, "Expected type name or '['");
        return NULL;
    }

    if (inner == NULL) return NULL;

    // Check for non-null modifier (!)
    if (graphql_lexer_accept(state->lexer, GRAPHQL_TOKEN_BANG)) {
        inner = graphql_type_ref_create_non_null(inner);
    }

    return inner;
}

// ============================================================
// Directive: @name(arg: value, ...)
// ============================================================

static graphql_directive_t* parse_directive(graphql_parser_state_t* state) {
    if (!graphql_lexer_accept(state->lexer, GRAPHQL_TOKEN_AT)) {
        return NULL;
    }

    graphql_token_t name_tok = graphql_lexer_expect(state->lexer, GRAPHQL_TOKEN_NAME);
    if (name_tok.kind != GRAPHQL_TOKEN_NAME) {
        parser_error(state, "Expected directive name");
        return NULL;
    }

    char* name = token_to_string(&name_tok);
    graphql_directive_t* directive = graphql_directive_create(name);
    free(name);
    if (directive == NULL) return NULL;

    // Parse optional arguments: (arg: value, ...)
    if (graphql_lexer_accept(state->lexer, GRAPHQL_TOKEN_LPAREN)) {
        while (graphql_lexer_peek(state->lexer).kind != GRAPHQL_TOKEN_RPAREN &&
               graphql_lexer_peek(state->lexer).kind != GRAPHQL_TOKEN_EOF) {
            graphql_token_t arg_name = graphql_lexer_expect(state->lexer, GRAPHQL_TOKEN_NAME);
            if (arg_name.kind != GRAPHQL_TOKEN_NAME) {
                parser_error(state, "Expected directive argument name");
                graphql_directive_destroy(directive);
                return NULL;
            }

            if (graphql_lexer_accept(state->lexer, GRAPHQL_TOKEN_COLON)) {
                // Parse argument value
                graphql_token_t value_tok = graphql_lexer_next(state->lexer);
                char* arg_name_str = token_to_string(&arg_name);
                char* arg_value_str = token_to_string(&value_tok);
                vec_push(&directive->arg_names, arg_name_str);
                vec_push(&directive->arg_values, arg_value_str);
            }
        }
        if (!graphql_lexer_accept(state->lexer, GRAPHQL_TOKEN_RPAREN)) {
            parser_error(state, "Expected ')' after directive arguments");
            graphql_directive_destroy(directive);
            return NULL;
        }
    }

    return directive;
}

// ============================================================
// Literal value parsing (for arguments and defaults)
// ============================================================

static graphql_literal_t* parse_literal(graphql_parser_state_t* state) {
    graphql_token_t token = graphql_lexer_peek(state->lexer);

    switch (token.kind) {
        case GRAPHQL_TOKEN_STRING: {
            graphql_lexer_next(state->lexer);
            graphql_literal_t* lit = get_clear_memory(sizeof(graphql_literal_t));
            if (lit == NULL) return NULL;
            lit->kind = GRAPHQL_LITERAL_STRING;
            lit->string_val = decode_string_literal(token.start, token.length);
            return lit;
        }
        case GRAPHQL_TOKEN_INT: {
            graphql_lexer_next(state->lexer);
            graphql_literal_t* lit = get_clear_memory(sizeof(graphql_literal_t));
            if (lit == NULL) return NULL;
            lit->kind = GRAPHQL_LITERAL_INT;
            lit->int_val = strtoll(token.start, NULL, 10);
            return lit;
        }
        case GRAPHQL_TOKEN_FLOAT: {
            graphql_lexer_next(state->lexer);
            graphql_literal_t* lit = get_clear_memory(sizeof(graphql_literal_t));
            if (lit == NULL) return NULL;
            lit->kind = GRAPHQL_LITERAL_FLOAT;
            lit->float_val = strtod(token.start, NULL);
            return lit;
        }
        case GRAPHQL_TOKEN_BOOL: {
            graphql_lexer_next(state->lexer);
            graphql_literal_t* lit = get_clear_memory(sizeof(graphql_literal_t));
            if (lit == NULL) return NULL;
            lit->kind = GRAPHQL_LITERAL_BOOL;
            lit->bool_val = (token.length == 4 && strncmp(token.start, "true", 4) == 0);
            return lit;
        }
        case GRAPHQL_TOKEN_NULL: {
            graphql_lexer_next(state->lexer);
            graphql_literal_t* lit = get_clear_memory(sizeof(graphql_literal_t));
            if (lit == NULL) return NULL;
            lit->kind = GRAPHQL_LITERAL_NULL;
            return lit;
        }
        case GRAPHQL_TOKEN_NAME: {
            // Enum value
            graphql_lexer_next(state->lexer);
            graphql_literal_t* lit = get_clear_memory(sizeof(graphql_literal_t));
            if (lit == NULL) return NULL;
            lit->kind = GRAPHQL_LITERAL_ENUM;
            lit->string_val = token_to_string(&token);
            return lit;
        }
        case GRAPHQL_TOKEN_LBRACE: {
            return parse_object_literal(state);
        }
        case GRAPHQL_TOKEN_LBRACKET: {
            return parse_list_literal(state);
        }
        default:
            parser_error(state, "Expected literal value");
            return NULL;
    }
}

static graphql_literal_t* parse_object_literal(graphql_parser_state_t* state) {
    if (!graphql_lexer_accept(state->lexer, GRAPHQL_TOKEN_LBRACE)) {
        parser_error(state, "Expected '{' for object literal");
        return NULL;
    }

    graphql_literal_t* lit = get_clear_memory(sizeof(graphql_literal_t));
    if (lit == NULL) return NULL;
    lit->kind = GRAPHQL_LITERAL_OBJECT;

    while (graphql_lexer_peek(state->lexer).kind != GRAPHQL_TOKEN_RBRACE &&
           graphql_lexer_peek(state->lexer).kind != GRAPHQL_TOKEN_EOF) {
        graphql_token_t key = graphql_lexer_expect(state->lexer, GRAPHQL_TOKEN_NAME);
        if (key.kind != GRAPHQL_TOKEN_NAME) {
            parser_error(state, "Expected object field name");
            // Clean up any fields already parsed
            for (size_t i = 0; i < lit->object_fields.length; i++) {
                graphql_object_field_t* f = lit->object_fields.data[i];
                if (f) {
                    free(f->name);
                    if (f->value) {
                        // Destroy literal using a dummy AST node
                        graphql_ast_node_t dummy;
                        memset(&dummy, 0, sizeof(dummy));
                        dummy.literal = f->value;
                        graphql_ast_destroy(&dummy);
                    }
                    free(f);
                }
            }
            vec_deinit(&lit->object_fields);
            free(lit);
            return NULL;
        }

        if (!graphql_lexer_accept(state->lexer, GRAPHQL_TOKEN_COLON)) {
            parser_error(state, "Expected ':' after object field name");
            return NULL;
        }

        graphql_literal_t* value = parse_literal(state);
        if (value == NULL) {
            // Clean up already-parsed fields
            for (size_t i = 0; i < lit->object_fields.length; i++) {
                graphql_object_field_t* f = lit->object_fields.data[i];
                if (f) {
                    free(f->name);
                    if (f->value) {
                        graphql_ast_node_t dummy;
                        memset(&dummy, 0, sizeof(dummy));
                        dummy.literal = f->value;
                        graphql_ast_destroy(&dummy);
                    }
                    free(f);
                }
            }
            vec_deinit(&lit->object_fields);
            free(lit);
            return NULL;
        }

        graphql_object_field_t* field = get_clear_memory(sizeof(graphql_object_field_t));
        if (field == NULL) {
            // Clean up value and already-parsed fields
            graphql_ast_node_t dummy;
            memset(&dummy, 0, sizeof(dummy));
            dummy.literal = value;
            graphql_ast_destroy(&dummy);
            for (size_t i = 0; i < lit->object_fields.length; i++) {
                graphql_object_field_t* f = lit->object_fields.data[i];
                if (f) {
                    free(f->name);
                    if (f->value) {
                        memset(&dummy, 0, sizeof(dummy));
                        dummy.literal = f->value;
                        graphql_ast_destroy(&dummy);
                    }
                    free(f);
                }
            }
            vec_deinit(&lit->object_fields);
            free(lit);
            return NULL;
        }
        field->name = token_to_string(&key);
        field->value = value;
        vec_push(&lit->object_fields, field);
    }

    if (!graphql_lexer_accept(state->lexer, GRAPHQL_TOKEN_RBRACE)) {
        parser_error(state, "Expected '}' after object literal");
        return NULL;
    }

    return lit;
}

static graphql_literal_t* parse_list_literal(graphql_parser_state_t* state) {
    if (!graphql_lexer_accept(state->lexer, GRAPHQL_TOKEN_LBRACKET)) {
        parser_error(state, "Expected '[' for list literal");
        return NULL;
    }

    graphql_literal_t* lit = get_clear_memory(sizeof(graphql_literal_t));
    if (lit == NULL) return NULL;
    lit->kind = GRAPHQL_LITERAL_LIST;

    while (graphql_lexer_peek(state->lexer).kind != GRAPHQL_TOKEN_RBRACKET &&
           graphql_lexer_peek(state->lexer).kind != GRAPHQL_TOKEN_EOF) {
        graphql_literal_t* item = parse_literal(state);
        if (item == NULL) {
            // Clean up already-parsed list items
            for (size_t i = 0; i < lit->list_items.length; i++) {
                graphql_ast_node_t dummy;
                memset(&dummy, 0, sizeof(dummy));
                dummy.literal = lit->list_items.data[i];
                graphql_ast_destroy(&dummy);
            }
            vec_deinit(&lit->list_items);
            free(lit);
            return NULL;
        }
        vec_push(&lit->list_items, item);
    }

    if (!graphql_lexer_accept(state->lexer, GRAPHQL_TOKEN_RBRACKET)) {
        parser_error(state, "Expected ']' after list literal");
        return NULL;
    }

    return lit;
}

// ============================================================
// Operation definition: query Name { ... } or mutation Name { ... }
// Also handles anonymous queries: { ... }
// ============================================================

static graphql_ast_node_t* parse_operation_definition(graphql_parser_state_t* state) {
    graphql_token_t token = graphql_lexer_peek(state->lexer);

    bool is_mutation = false;
    char* op_name = NULL;
    size_t line = token.line;
    size_t column = token.column;

    if (token.kind == GRAPHQL_TOKEN_NAME) {
        if (token.length == 8 && strncmp(token.start, "mutation", 8) == 0) {
            is_mutation = true;
        } else if (token.length == 5 && strncmp(token.start, "query", 5) == 0) {
            is_mutation = false;
        } else {
            parser_error(state, "Expected 'query' or 'mutation'");
            return NULL;
        }
        graphql_lexer_next(state->lexer);  // consume query/mutation keyword

        // Optional operation name
        graphql_token_t name_tok = graphql_lexer_peek(state->lexer);
        if (name_tok.kind == GRAPHQL_TOKEN_NAME) {
            op_name = token_to_string(&name_tok);
            graphql_lexer_next(state->lexer);
        }
    }
    // else: anonymous query starting with {

    graphql_ast_node_t* op = graphql_ast_node_create(GRAPHQL_AST_OPERATION,
                                                       op_name, line, column);
    free(op_name);
    if (op == NULL) return NULL;

    if (is_mutation) {
        // Mark as mutation - use alias field as a flag
        op->alias = strdup("mutation");
    }

    // Parse selection set { ... }
    graphql_ast_node_t* selection = parse_selection_set(state);
    if (selection == NULL) {
        graphql_ast_destroy(op);
        return NULL;
    }
    // Merge selection children into operation
    int i;
    graphql_ast_node_t* child;
    vec_foreach(&selection->children, child, i) {
        child->parent = op;
        vec_push(&op->children, child);
    }
    // Don't double-free: transfer children, then destroy shell
    selection->children.length = 0;
    graphql_ast_destroy(selection);

    return op;
}

// ============================================================
// Selection set: { field1 field2 ... }
// ============================================================

static graphql_ast_node_t* parse_selection_set(graphql_parser_state_t* state) {
    if (!graphql_lexer_accept(state->lexer, GRAPHQL_TOKEN_LBRACE)) {
        parser_error(state, "Expected '{' for selection set");
        return NULL;
    }

    graphql_ast_node_t* set = graphql_ast_node_create(GRAPHQL_AST_DOCUMENT, NULL, 0, 0);
    if (set == NULL) return NULL;

    while (graphql_lexer_peek(state->lexer).kind != GRAPHQL_TOKEN_RBRACE &&
           graphql_lexer_peek(state->lexer).kind != GRAPHQL_TOKEN_EOF) {
        graphql_token_t peek = graphql_lexer_peek(state->lexer);

        graphql_ast_node_t* child = NULL;
        if (peek.kind == GRAPHQL_TOKEN_DOTDOTDOT) {
            // Fragment spread or inline fragment
            graphql_lexer_next(state->lexer);  // consume ...
            graphql_token_t next = graphql_lexer_peek(state->lexer);
            if (next.kind == GRAPHQL_TOKEN_NAME &&
                !(next.length == 2 && strncmp(next.start, "on", 2) == 0)) {
                // Fragment spread: ...FragmentName
                child = parse_fragment_spread(state);
            } else {
                // Inline fragment: ... on Type { ... }
                child = parse_inline_fragment(state);
            }
        } else if (peek.kind == GRAPHQL_TOKEN_NAME) {
            // Field selection
            child = parse_field(state);
        } else {
            parser_error(state, "Expected field name, fragment spread, or inline fragment");
            graphql_ast_destroy(set);
            return NULL;
        }

        if (child == NULL) {
            graphql_ast_destroy(set);
            return NULL;
        }
        child->parent = set;
        vec_push(&set->children, child);
    }

    if (!graphql_lexer_accept(state->lexer, GRAPHQL_TOKEN_RBRACE)) {
        parser_error(state, "Expected '}' after selection set");
        graphql_ast_destroy(set);
        return NULL;
    }

    return set;
}

// ============================================================
// Field selection: name, alias: name, name(args) { subfields }
// ============================================================

static graphql_ast_node_t* parse_field(graphql_parser_state_t* state) {
    graphql_token_t first = graphql_lexer_next(state->lexer);
    if (first.kind != GRAPHQL_TOKEN_NAME) {
        parser_error(state, "Expected field name");
        return NULL;
    }

    char* name_or_alias = token_to_string(&first);
    char* field_name = NULL;
    char* alias = NULL;

    // Check for alias: name: field
    if (graphql_lexer_peek(state->lexer).kind == GRAPHQL_TOKEN_COLON) {
        // This is an alias
        alias = name_or_alias;
        graphql_lexer_next(state->lexer);  // consume :
        graphql_token_t name_tok = graphql_lexer_next(state->lexer);
        if (name_tok.kind != GRAPHQL_TOKEN_NAME) {
            parser_error(state, "Expected field name after alias");
            free(alias);
            return NULL;
        }
        field_name = token_to_string(&name_tok);
    } else {
        field_name = name_or_alias;
    }

    graphql_ast_node_t* field = graphql_ast_node_create(GRAPHQL_AST_FIELD,
                                                         field_name, first.line, first.column);
    free(field_name);
    if (field == NULL) {
        free(alias);
        return NULL;
    }
    field->alias = alias;

    // Parse optional arguments: (arg: value, ...)
    if (graphql_lexer_accept(state->lexer, GRAPHQL_TOKEN_LPAREN)) {
        while (graphql_lexer_peek(state->lexer).kind != GRAPHQL_TOKEN_RPAREN &&
               graphql_lexer_peek(state->lexer).kind != GRAPHQL_TOKEN_EOF) {
            graphql_token_t arg_name = graphql_lexer_expect(state->lexer, GRAPHQL_TOKEN_NAME);
            if (arg_name.kind != GRAPHQL_TOKEN_NAME) {
                parser_error(state, "Expected argument name");
                graphql_ast_destroy(field);
                return NULL;
            }

            if (!graphql_lexer_accept(state->lexer, GRAPHQL_TOKEN_COLON)) {
                parser_error(state, "Expected ':' after argument name");
                graphql_ast_destroy(field);
                return NULL;
            }

            graphql_literal_t* value = parse_literal(state);
            if (value == NULL) {
                graphql_ast_destroy(field);
                return NULL;
            }

            // Store argument as a child node with the literal value
            char* arg_name_str = token_to_string(&arg_name);
            graphql_ast_node_t* arg = graphql_ast_node_create(GRAPHQL_AST_ARGUMENT,
                                                               arg_name_str, arg_name.line, arg_name.column);
            free(arg_name_str);
            if (arg == NULL) {
                graphql_ast_destroy(field);
                return NULL;
            }
            arg->literal = value;
            arg->parent = field;
            vec_push(&field->arguments, arg);
        }
        if (!graphql_lexer_accept(state->lexer, GRAPHQL_TOKEN_RPAREN)) {
            parser_error(state, "Expected ')' after arguments");
            graphql_ast_destroy(field);
            return NULL;
        }
    }

    // Parse optional directives
    while (graphql_lexer_peek(state->lexer).kind == GRAPHQL_TOKEN_AT) {
        graphql_directive_t* dir = parse_directive(state);
        if (dir == NULL) {
            graphql_ast_destroy(field);
            return NULL;
        }
        vec_push(&field->directives, dir);
    }

    // Parse optional sub-selection set
    if (graphql_lexer_peek(state->lexer).kind == GRAPHQL_TOKEN_LBRACE) {
        graphql_ast_node_t* sub_selection = parse_selection_set(state);
        if (sub_selection == NULL) {
            graphql_ast_destroy(field);
            return NULL;
        }
        // Merge children into field
        int i;
        graphql_ast_node_t* child;
        vec_foreach(&sub_selection->children, child, i) {
            child->parent = field;
            vec_push(&field->children, child);
        }
        sub_selection->children.length = 0;
        graphql_ast_destroy(sub_selection);
    }

    return field;
}

// ============================================================
// Fragment definition: fragment Name on Type { ... }
// ============================================================

static graphql_ast_node_t* parse_fragment_definition(graphql_parser_state_t* state) {
    graphql_token_t frag_kw = graphql_lexer_next(state->lexer);  // consume 'fragment'
    if (frag_kw.kind != GRAPHQL_TOKEN_NAME ||
        !(frag_kw.length == 8 && strncmp(frag_kw.start, "fragment", 8) == 0)) {
        parser_error(state, "Expected 'fragment' keyword");
        return NULL;
    }

    graphql_token_t name_tok = graphql_lexer_next(state->lexer);
    if (name_tok.kind != GRAPHQL_TOKEN_NAME) {
        parser_error(state, "Expected fragment name");
        return NULL;
    }

    // Expect "on" keyword
    graphql_token_t on_kw = graphql_lexer_next(state->lexer);
    if (on_kw.kind != GRAPHQL_TOKEN_NAME ||
        !(on_kw.length == 2 && strncmp(on_kw.start, "on", 2) == 0)) {
        parser_error(state, "Expected 'on' after fragment name");
        return NULL;
    }

    // Type condition
    graphql_token_t type_tok = graphql_lexer_next(state->lexer);
    if (type_tok.kind != GRAPHQL_TOKEN_NAME) {
        parser_error(state, "Expected type name after 'on'");
        return NULL;
    }

    char* name = token_to_string(&name_tok);
    graphql_ast_node_t* frag = graphql_ast_node_create(GRAPHQL_AST_FRAGMENT,
                                                         name, name_tok.line, name_tok.column);
    free(name);
    if (frag == NULL) return NULL;

    // Store type condition in type_ref
    char* type_name = token_to_string(&type_tok);
    frag->type_ref = graphql_type_ref_create_named(GRAPHQL_TYPE_OBJECT, type_name);
    free(type_name);

    // Parse selection set
    graphql_ast_node_t* selection = parse_selection_set(state);
    if (selection == NULL) {
        graphql_ast_destroy(frag);
        return NULL;
    }
    int i;
    graphql_ast_node_t* child;
    vec_foreach(&selection->children, child, i) {
        child->parent = frag;
        vec_push(&frag->children, child);
    }
    selection->children.length = 0;
    graphql_ast_destroy(selection);

    return frag;
}

// ============================================================
// Fragment spread: ...FragmentName (already consumed the ...)
// ============================================================

static graphql_ast_node_t* parse_fragment_spread(graphql_parser_state_t* state) {
    // ... was already consumed, now parse the fragment name
    graphql_token_t name_tok = graphql_lexer_next(state->lexer);
    if (name_tok.kind != GRAPHQL_TOKEN_NAME) {
        parser_error(state, "Expected fragment name after '...'");
        return NULL;
    }

    char* name = token_to_string(&name_tok);
    graphql_ast_node_t* spread = graphql_ast_node_create(GRAPHQL_AST_FRAGMENT_SPREAD,
                                                          name, name_tok.line, name_tok.column);
    free(name);

    // Parse optional directives
    if (spread != NULL) {
        while (graphql_lexer_peek(state->lexer).kind == GRAPHQL_TOKEN_AT) {
            graphql_directive_t* dir = parse_directive(state);
            if (dir == NULL) {
                graphql_ast_destroy(spread);
                return NULL;
            }
            vec_push(&spread->directives, dir);
        }
    }

    return spread;
}

// ============================================================
// Inline fragment: ... on Type { ... } (already consumed the ...)
// ============================================================

static graphql_ast_node_t* parse_inline_fragment(graphql_parser_state_t* state) {
    // ... was already consumed
    // Check for "on" keyword
    graphql_token_t peek = graphql_lexer_peek(state->lexer);

    char* type_name = NULL;
    if (peek.kind == GRAPHQL_TOKEN_NAME &&
        peek.length == 2 && strncmp(peek.start, "on", 2) == 0) {
        graphql_lexer_next(state->lexer);  // consume 'on'
        graphql_token_t type_tok = graphql_lexer_next(state->lexer);
        if (type_tok.kind != GRAPHQL_TOKEN_NAME) {
            parser_error(state, "Expected type name after 'on'");
            return NULL;
        }
        type_name = token_to_string(&type_tok);
    }

    graphql_ast_node_t* inline_frag = graphql_ast_node_create(GRAPHQL_AST_INLINE_FRAGMENT,
                                                                type_name, 0, 0);
    if (inline_frag == NULL) {
        free(type_name);
        return NULL;
    }

    if (type_name != NULL) {
        inline_frag->type_ref = graphql_type_ref_create_named(GRAPHQL_TYPE_OBJECT, type_name);
        free(type_name);
    }

    // Parse optional directives
    while (graphql_lexer_peek(state->lexer).kind == GRAPHQL_TOKEN_AT) {
        graphql_directive_t* dir = parse_directive(state);
        if (dir == NULL) {
            graphql_ast_destroy(inline_frag);
            return NULL;
        }
        vec_push(&inline_frag->directives, dir);
    }

    // Parse selection set
    graphql_ast_node_t* selection = parse_selection_set(state);
    if (selection == NULL) {
        graphql_ast_destroy(inline_frag);
        return NULL;
    }
    int i;
    graphql_ast_node_t* child;
    vec_foreach(&selection->children, child, i) {
        child->parent = inline_frag;
        vec_push(&inline_frag->children, child);
    }
    selection->children.length = 0;
    graphql_ast_destroy(selection);

    return inline_frag;
}