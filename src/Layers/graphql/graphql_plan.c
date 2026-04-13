//
// GraphQL Plan - Query plan compilation
// Created: 2026-04-12
//

#include "graphql_plan.h"
#include "graphql_parser.h"
#include "graphql_lexer.h"
#include "graphql_schema.h"
#include "../../Util/allocator.h"
#include "../../HBTrie/path.h"
#include "../../HBTrie/identifier.h"
#include "../../HBTrie/chunk.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// ============================================================
// Forward declarations
// ============================================================

// Fragment lookup table
typedef struct {
    const char* name;            // Fragment name (points into AST, not owned)
    graphql_ast_node_t* node;    // AST node for the fragment definition
} fragment_entry_t;

typedef vec_t(fragment_entry_t) fragment_table_t;

#define MAX_FRAGMENT_DEPTH 10

static graphql_plan_t* compile_field(graphql_layer_t* layer,
                                      graphql_ast_node_t* field_node,
                                      graphql_type_t* parent_type,
                                      const char* parent_path,
                                      fragment_table_t* fragments,
                                      const char** visited,
                                      int visited_depth,
                                      bool parent_is_entity_scan);
static bool should_skip_field(graphql_ast_node_t* field_node);

// ============================================================
// Helper: evaluate @skip/@include directives
// ============================================================

static bool should_skip_field(graphql_ast_node_t* field_node) {
    for (int i = 0; i < field_node->directives.length; i++) {
        graphql_directive_t* dir = field_node->directives.data[i];

        if (strcmp(dir->name, "skip") == 0) {
            // @skip(if: true) → skip this field
            for (int j = 0; j < dir->arg_names.length; j++) {
                if (strcmp(dir->arg_names.data[j], "if") == 0) {
                    if (strcmp(dir->arg_values.data[j], "true") == 0) {
                        return true;
                    }
                }
            }
        }

        if (strcmp(dir->name, "include") == 0) {
            // @include(if: false) → skip this field
            for (int j = 0; j < dir->arg_names.length; j++) {
                if (strcmp(dir->arg_names.data[j], "if") == 0) {
                    if (strcmp(dir->arg_values.data[j], "false") == 0) {
                        return true;
                    }
                }
            }
        }
    }
    return false;
}

// ============================================================
// Helper: build path string
// ============================================================

static char* build_path(const char* parent, const char* child) {
    if (parent == NULL || parent[0] == '\0') {
        return strdup(child);
    }
    size_t len = strlen(parent) + 1 + strlen(child) + 1;
    char* path = malloc(len);
    if (path == NULL) return NULL;
    snprintf(path, len, "%s/%s", parent, child);
    return path;
}

// ============================================================
// Compile a single field into a plan node
// ============================================================

static graphql_plan_t* compile_field(graphql_layer_t* layer,
                                      graphql_ast_node_t* field_node,
                                      graphql_type_t* parent_type,
                                      const char* parent_path,
                                      fragment_table_t* fragments,
                                      const char** visited,
                                      int visited_depth,
                                      bool parent_is_entity_scan) {
    if (field_node == NULL) return NULL;

    // Check @skip/@include
    if (should_skip_field(field_node)) {
        return NULL;
    }

    const char* field_name = field_node->name;
    if (field_name == NULL) return NULL;

    // __typename is a virtual field — does not resolve from the database
    if (strcmp(field_name, "__typename") == 0) {
        graphql_plan_t* plan = graphql_plan_create(PLAN_RESOLVE_FIELD);
        if (plan == NULL) return NULL;
        plan->field_name = strdup("__typename");
        plan->type_name = parent_type ? strdup(parent_type->name) : NULL;
        plan->alias = field_node->alias ? strdup(field_node->alias) : NULL;
        return plan;
    }

    // Look up field in parent type
    graphql_field_t* type_field = NULL;
    graphql_type_ref_t* field_type_ref = NULL;

    if (parent_type != NULL) {
        for (int i = 0; i < parent_type->fields.length; i++) {
            graphql_field_t* f = parent_type->fields.data[i];
            if (strcmp(f->name, field_name) == 0) {
                type_field = f;
                field_type_ref = f->type;
                break;
            }
        }
    }

    // Determine the target type for nested selections
    graphql_type_t* target_type = NULL;
    const char* target_type_name = NULL;
    bool is_list = false;

    if (field_type_ref != NULL) {
        // Unwrap LIST and NON_NULL to find the base type name
        graphql_type_ref_t* ref = field_type_ref;
        while (ref != NULL) {
            if (ref->kind == GRAPHQL_TYPE_LIST) {
                is_list = true;
                ref = ref->of_type;
            } else if (ref->kind == GRAPHQL_TYPE_NON_NULL) {
                ref = ref->of_type;
            } else {
                target_type_name = ref->name;
                break;
            }
        }

        if (target_type_name != NULL && layer != NULL) {
            target_type = graphql_schema_get_type(layer, target_type_name);
        }
    } else if (layer != NULL && layer->registry != NULL) {
        // Field not found in parent type — check if the field name directly
        // references a known type (root query fields like "User" or "Users")
        for (int k = 0; k < layer->registry->types.length; k++) {
            graphql_type_t* t = layer->registry->types.data[k];
            const char* plural = graphql_type_get_plural(t);
            if ((plural != NULL && strcmp(plural, field_name) == 0) ||
                strcmp(t->name, field_name) == 0) {
                target_type = t;
                target_type_name = t->name;
                // Root query fields that reference types always return a list
                is_list = true;
                break;
            }
        }
    }

    // Get plural name for path construction
    const char* plural = target_type ? graphql_type_get_plural(target_type) : field_name;

    // Build the path for this field
    // For root query fields that reference a type (fallback lookup), use the
    // type's plural directly as the path prefix, not parent_path/field_name
    char* field_path;
    if (type_field == NULL && field_type_ref == NULL && target_type != NULL) {
        field_path = strdup(plural);
    } else {
        field_path = build_path(parent_path, field_name);
    }

    // Determine if the target type is a scalar (no nested fields to resolve)
    bool is_scalar = (target_type == NULL) || (target_type->kind == GRAPHQL_TYPE_SCALAR) || (target_type->kind == GRAPHQL_TYPE_ENUM);

    // Determine if the target type is a reference to another object type
    bool is_reference = (!is_scalar && !is_list);
    bool is_list_reference = (!is_scalar && is_list);

    // Determine plan kind
    graphql_plan_kind_t plan_kind;

    if (is_list && field_node->arguments.length > 0) {
        // List type with arguments (e.g., User(id: "1")) → batch get specific entities
        plan_kind = PLAN_BATCH_GET;
    } else if (parent_is_entity_scan && is_scalar) {
        // Scalar field under an entity scan → resolve by parent entity ID
        plan_kind = PLAN_RESOLVE_FIELD;
    } else if (parent_is_entity_scan && is_reference) {
        // Single reference field under an entity scan → follow the reference
        plan_kind = PLAN_RESOLVE_REF;
    } else if (parent_is_entity_scan && is_list_reference) {
        // List reference under an entity scan → scan the reference path
        // This stays as PLAN_SCAN but the path is entity-relative
        plan_kind = PLAN_SCAN;
    } else if (is_list) {
        plan_kind = PLAN_SCAN;
    } else {
        plan_kind = PLAN_GET;
    }

    graphql_plan_t* plan = graphql_plan_create(plan_kind);
    if (plan == NULL) {
        free(field_path);
        return NULL;
    }

    plan->type_name = target_type_name ? strdup(target_type_name) : NULL;
    plan->field_name = strdup(field_name);
    plan->alias = field_node->alias ? strdup(field_node->alias) : NULL;

    // Set up the path for this plan
    if (field_path != NULL) {
        // Parse path string into path_t
        path_t* path = path_create();
        if (path != NULL) {
            const char* start = field_path;
            while (*start) {
                const char* end = strchr(start, '/');
                size_t len = end ? (size_t)(end - start) : strlen(start);
                if (len > 0) {
                    buffer_t* buf = buffer_create(len);
                    if (buf != NULL) {
                        memcpy(buf->data, start, len);
                        buf->size = len;
                        identifier_t* id = identifier_create(buf, 0);
                        buffer_destroy(buf);
                        if (id != NULL) {
                            path_append(path, id);
                            identifier_destroy(id);
                        }
                    }
                }
                if (end) {
                    start = end + 1;
                } else {
                    break;
                }
            }
        }

        switch (plan_kind) {
            case PLAN_SCAN:
            case PLAN_BATCH_GET:
                plan->scan_start = path;
                plan->scan_end = NULL;
                break;
            case PLAN_GET:
                plan->get_path = path;
                break;
            case PLAN_RESOLVE_FIELD:
            case PLAN_RESOLVE_REF:
                // RESOLVE_FIELD and RESOLVE_REF build paths dynamically
                // from parent_plural/parent_id/field_name, so we don't
                // need a static path. Store as base_path for reference.
                plan->base_path = path;
                break;
            default:
                // Other plan kinds: free the path
                if (path) path_destroy(path);
                break;
        }
    }
    free(field_path);

    // Store arguments
    for (int i = 0; i < field_node->arguments.length; i++) {
        graphql_ast_node_t* arg = field_node->arguments.data[i];
        graphql_arg_t plan_arg;
        plan_arg.name = strdup(arg->name);
        // Convert literal to string representation
        if (arg->literal != NULL) {
            switch (arg->literal->kind) {
                case GRAPHQL_LITERAL_STRING:
                    plan_arg.value = strdup(arg->literal->string_val ? arg->literal->string_val : "");
                    break;
                case GRAPHQL_LITERAL_INT: {
                    char buf[32];
                    snprintf(buf, sizeof(buf), "%lld", (long long)arg->literal->int_val);
                    plan_arg.value = strdup(buf);
                    break;
                }
                case GRAPHQL_LITERAL_FLOAT: {
                    char buf[64];
                    snprintf(buf, sizeof(buf), "%g", arg->literal->float_val);
                    plan_arg.value = strdup(buf);
                    break;
                }
                case GRAPHQL_LITERAL_BOOL:
                    plan_arg.value = strdup(arg->literal->bool_val ? "true" : "false");
                    break;
                case GRAPHQL_LITERAL_NULL:
                    plan_arg.value = strdup("null");
                    break;
                case GRAPHQL_LITERAL_ENUM:
                    plan_arg.value = strdup(arg->literal->string_val ? arg->literal->string_val : "");
                    break;
                default:
                    plan_arg.value = strdup("");
                    break;
            }
        } else {
            plan_arg.value = strdup("");
        }
        vec_push(&plan->args, plan_arg);
    }

    // For RESOLVE_REF, set the referenced type name
    if (plan_kind == PLAN_RESOLVE_REF && target_type_name != NULL) {
        plan->ref_type = strdup(target_type_name);
    }

    // For arguments like id=xxx, construct a specific get path
    // instead of a scan
    if (is_list && plan->args.length > 0) {
        // This is like users(id: "1") - should be a batch get
        // Keep as PLAN_SCAN but with args that identify specific entries
    }

    // Determine if child fields should be resolved per-entity
    bool child_is_entity_scan = (plan_kind == PLAN_SCAN || plan_kind == PLAN_BATCH_GET);

    // Compile nested selections
    for (int i = 0; i < field_node->children.length; i++) {
        graphql_ast_node_t* child = field_node->children.data[i];

        if (child->kind == GRAPHQL_AST_FRAGMENT_SPREAD) {
            // Fragment spread: look up the fragment definition and inline it
            const char* spread_name = child->name;
            graphql_ast_node_t* frag_def = NULL;

            if (fragments != NULL) {
                for (int fi = 0; fi < fragments->length; fi++) {
                    if (strcmp(fragments->data[fi].name, spread_name) == 0) {
                        frag_def = fragments->data[fi].node;
                        break;
                    }
                }
            }

            if (frag_def == NULL) {
                // Unknown fragment - skip silently
                continue;
            }

            // Cycle detection
            bool is_cycle = false;
            for (int vi = 0; vi < visited_depth; vi++) {
                if (strcmp(visited[vi], spread_name) == 0) {
                    is_cycle = true;
                    break;
                }
            }
            if (is_cycle) continue;

            // Build new visited stack for recursive calls
            const char* new_visited[MAX_FRAGMENT_DEPTH];
            for (int vi = 0; vi < visited_depth && vi < MAX_FRAGMENT_DEPTH; vi++) {
                new_visited[vi] = visited[vi];
            }
            int new_depth = visited_depth < MAX_FRAGMENT_DEPTH ? visited_depth + 1 : visited_depth;
            if (visited_depth < MAX_FRAGMENT_DEPTH) {
                new_visited[visited_depth] = spread_name;
            }

            // Determine fragment's type condition
            graphql_type_t* frag_type = NULL;
            if (frag_def->type_ref && frag_def->type_ref->name && layer != NULL) {
                frag_type = graphql_schema_get_type(layer, frag_def->type_ref->name);
            }
            graphql_type_t* frag_parent_type = frag_type ? frag_type : target_type;

            // Inline fragment's selection fields
            for (int j = 0; j < frag_def->children.length; j++) {
                graphql_plan_t* child_plan = compile_field(layer, frag_def->children.data[j],
                                                            frag_parent_type, parent_path,
                                                            fragments, new_visited, new_depth,
                                                            child_is_entity_scan);
                if (child_plan != NULL) {
                    vec_push(&plan->children, child_plan);
                }
            }
            continue;
        }

        if (child->kind == GRAPHQL_AST_INLINE_FRAGMENT) {
            // Inline fragment: compile children with type condition
            graphql_type_t* inline_type = NULL;
            if (child->type_ref && child->type_ref->name && layer != NULL) {
                inline_type = graphql_schema_get_type(layer, child->type_ref->name);
            }

            // Build path for inline fragment selections
            const char* inline_path = parent_path;
            // Use the inline type or fall back to target type
            graphql_type_t* inline_parent_type = inline_type ? inline_type : target_type;
            if (inline_parent_type != NULL) {
                const char* inline_plural = graphql_type_get_plural(inline_parent_type);
                char* inline_path_str = build_path(parent_path, inline_plural);
                for (int j = 0; j < child->children.length; j++) {
                    graphql_plan_t* child_plan = compile_field(layer, child->children.data[j],
                                                                inline_parent_type, inline_path_str,
                                                                fragments, visited, visited_depth,
                                                                child_is_entity_scan);
                    if (child_plan != NULL) {
                        vec_push(&plan->children, child_plan);
                    }
                }
                free(inline_path_str);
            } else {
                for (int j = 0; j < child->children.length; j++) {
                    graphql_plan_t* child_plan = compile_field(layer, child->children.data[j],
                                                                NULL, parent_path,
                                                                fragments, visited, visited_depth,
                                                                child_is_entity_scan);
                    if (child_plan != NULL) {
                        vec_push(&plan->children, child_plan);
                    }
                }
            }
            continue;
        }

        if (child->kind == GRAPHQL_AST_FIELD) {
            graphql_plan_t* child_plan = compile_field(layer, child,
                                                        target_type, plural,
                                                        fragments, visited, visited_depth,
                                                        child_is_entity_scan);
            if (child_plan != NULL) {
                vec_push(&plan->children, child_plan);
            }
        }
    }

    // Copy custom resolver if available
    if (type_field != NULL && type_field->resolver != NULL) {
        plan->kind = PLAN_CUSTOM;
        plan->resolver = type_field->resolver;
        plan->resolver_ctx = type_field->resolver_ctx;
    }

    return plan;
}

// ============================================================
// Public API
// ============================================================

graphql_plan_t* graphql_compile_query(graphql_layer_t* layer, const char* query) {
    if (layer == NULL || query == NULL) return NULL;

    // Parse the query
    graphql_ast_node_t* ast = graphql_parse(query, strlen(query));
    if (ast == NULL) return NULL;

    // Collect fragment definitions
    fragment_table_t fragments;
    vec_init(&fragments);
    for (int i = 0; i < ast->children.length; i++) {
        graphql_ast_node_t* child = ast->children.data[i];
        if (child->kind == GRAPHQL_AST_FRAGMENT) {
            fragment_entry_t entry;
            entry.name = child->name;
            entry.node = child;
            vec_push(&fragments, entry);
        }
    }

    graphql_plan_t* root_plan = NULL;

    for (int i = 0; i < ast->children.length; i++) {
        graphql_ast_node_t* def = ast->children.data[i];

        if (def->kind == GRAPHQL_AST_OPERATION) {
            // Determine root type from schema
            // For queries, use the query type; for mutations, use the mutation type
            bool is_mutation = (def->alias != NULL && strcmp(def->alias, "mutation") == 0);
            (void)is_mutation;  // Used later for mutation-specific planning

            // Compile each top-level field selection
            for (int j = 0; j < def->children.length; j++) {
                graphql_ast_node_t* field = def->children.data[j];

                if (field->kind == GRAPHQL_AST_FIELD) {
                    // Look up the type for this field in the schema
                    const char* field_name = field->name;
                    graphql_type_t* root_type = NULL;

                    if (layer->registry != NULL) {
                        // Try to find the type matching this field name
                        // (e.g., "user" -> look up "User" type)
                        for (int k = 0; k < layer->registry->types.length; k++) {
                            graphql_type_t* t = layer->registry->types.data[k];
                            const char* plural = graphql_type_get_plural(t);
                            if (strcmp(plural, field_name) == 0 ||
                                strcmp(t->name, field_name) == 0) {
                                root_type = t;
                                break;
                            }
                        }
                    }

                    const char* path_prefix = root_type ? graphql_type_get_plural(root_type) : field_name;

                    graphql_plan_t* child_plan = compile_field(layer, field,
                                                                root_type, path_prefix,
                                                                &fragments, NULL, 0, false);
                    if (child_plan != NULL) {
                        if (root_plan == NULL) {
                            root_plan = graphql_plan_create(PLAN_GET);
                            if (root_plan == NULL) {
                                graphql_plan_destroy(child_plan);
                                break;
                            }
                            root_plan->type_name = strdup(is_mutation ? "Mutation" : "Query");
                        }
                        vec_push(&root_plan->children, child_plan);
                    }
                }
            }
            break;  // Process only the first operation
        }
    }

    vec_deinit(&fragments);
    graphql_ast_destroy(ast);
    return root_plan;
}

graphql_plan_t* graphql_compile_mutation(graphql_layer_t* layer, const char* mutation) {
    if (layer == NULL || mutation == NULL) return NULL;

    // Parse the mutation
    graphql_ast_node_t* ast = graphql_parse(mutation, strlen(mutation));
    if (ast == NULL) return NULL;

    // Collect fragment definitions
    fragment_table_t fragments;
    vec_init(&fragments);
    for (int i = 0; i < ast->children.length; i++) {
        graphql_ast_node_t* child = ast->children.data[i];
        if (child->kind == GRAPHQL_AST_FRAGMENT) {
            fragment_entry_t entry;
            entry.name = child->name;
            entry.node = child;
            vec_push(&fragments, entry);
        }
    }

    graphql_plan_t* root_plan = NULL;

    for (int i = 0; i < ast->children.length; i++) {
        graphql_ast_node_t* def = ast->children.data[i];

        if (def->kind == GRAPHQL_AST_OPERATION &&
            def->alias != NULL && strcmp(def->alias, "mutation") == 0) {
            root_plan = graphql_plan_create(PLAN_GET);
            if (root_plan == NULL) break;
            root_plan->type_name = strdup("Mutation");

            for (int j = 0; j < def->children.length; j++) {
                graphql_ast_node_t* field = def->children.data[j];
                if (field->kind == GRAPHQL_AST_FIELD) {
                    const char* field_name = field->name;

                    // For mutations, look up the type
                    graphql_type_t* root_type = NULL;
                    if (layer->registry != NULL) {
                        for (int k = 0; k < layer->registry->types.length; k++) {
                            graphql_type_t* t = layer->registry->types.data[k];
                            const char* plural = graphql_type_get_plural(t);
                            if (strcmp(plural, field_name) == 0 ||
                                strcmp(t->name, field_name) == 0) {
                                root_type = t;
                                break;
                            }
                        }
                    }

                    const char* path_prefix = root_type ? graphql_type_get_plural(root_type) : field_name;
                    graphql_plan_t* child_plan = compile_field(layer, field,
                                                                root_type, path_prefix,
                                                                &fragments, NULL, 0, false);
                    if (child_plan != NULL) {
                        vec_push(&root_plan->children, child_plan);
                    }
                }
            }
            break;
        }
    }

    vec_deinit(&fragments);
    graphql_ast_destroy(ast);
    return root_plan;
}

char* graphql_plan_to_string(graphql_plan_t* plan) {
    if (plan == NULL) return strdup("(null plan)");

    const char* kind_names[] = {
        "SCAN", "GET", "BATCH_GET", "RESOLVE_FIELD",
        "RESOLVE_REF", "FILTER", "CUSTOM"
    };

    char buf[4096];
    int pos = 0;

    pos += snprintf(buf + pos, sizeof(buf) - pos, "%s(", kind_names[plan->kind]);
    if (plan->type_name) {
        pos += snprintf(buf + pos, sizeof(buf) - pos, "type=%s", plan->type_name);
    }
    if (plan->args.length > 0) {
        pos += snprintf(buf + pos, sizeof(buf) - pos, ", args=[");
        for (int i = 0; i < plan->args.length; i++) {
            if (i > 0) pos += snprintf(buf + pos, sizeof(buf) - pos, ", ");
            pos += snprintf(buf + pos, sizeof(buf) - pos, "%s=%s",
                          plan->args.data[i].name, plan->args.data[i].value);
        }
        pos += snprintf(buf + pos, sizeof(buf) - pos, "]");
    }
    pos += snprintf(buf + pos, sizeof(buf) - pos, ")");
    if (plan->children.length > 0) {
        pos += snprintf(buf + pos, sizeof(buf) - pos, " { ");
        for (int i = 0; i < plan->children.length; i++) {
            char* child_str = graphql_plan_to_string(plan->children.data[i]);
            if (child_str) {
                pos += snprintf(buf + pos, sizeof(buf) - pos, "%s ", child_str);
                free(child_str);
            }
        }
        pos += snprintf(buf + pos, sizeof(buf) - pos, "}");
    }

    return strdup(buf);
}