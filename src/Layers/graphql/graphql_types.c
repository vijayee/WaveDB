//
// GraphQL Layer Core Types - Implementation
// Created: 2026-04-12
//

#include "graphql_types.h"
#include "../../Util/allocator.h"
#include <stdlib.h>
#include <string.h>

// ============================================================
// Type reference functions
// ============================================================

graphql_type_ref_t* graphql_type_ref_create_named(graphql_type_kind_t kind,
                                                     const char* name) {
    graphql_type_ref_t* ref = get_clear_memory(sizeof(graphql_type_ref_t));
    if (ref == NULL) return NULL;
    ref->kind = kind;
    if (name) {
        ref->name = strdup(name);
        if (ref->name == NULL) {
            free(ref);
            return NULL;
        }
    }
    return ref;
}

graphql_type_ref_t* graphql_type_ref_create_list(graphql_type_ref_t* inner) {
    graphql_type_ref_t* ref = get_clear_memory(sizeof(graphql_type_ref_t));
    if (ref == NULL) return NULL;
    ref->kind = GRAPHQL_TYPE_LIST;
    ref->of_type = inner;
    return ref;
}

graphql_type_ref_t* graphql_type_ref_create_non_null(graphql_type_ref_t* inner) {
    graphql_type_ref_t* ref = get_clear_memory(sizeof(graphql_type_ref_t));
    if (ref == NULL) return NULL;
    ref->kind = GRAPHQL_TYPE_NON_NULL;
    ref->of_type = inner;
    return ref;
}

void graphql_type_ref_destroy(graphql_type_ref_t* ref) {
    if (ref == NULL) return;
    free(ref->name);
    graphql_type_ref_destroy(ref->of_type);
    free(ref);
}

// ============================================================
// Literal functions
// ============================================================

void graphql_literal_destroy(graphql_literal_t* literal) {
    if (literal == NULL) return;
    switch (literal->kind) {
        case GRAPHQL_LITERAL_STRING:
        case GRAPHQL_LITERAL_ENUM:
            free(literal->string_val);
            break;
        case GRAPHQL_LITERAL_OBJECT: {
            int i;
            graphql_object_field_t* f;
            vec_foreach(&literal->object_fields, f, i) {
                free(f->name);
                graphql_literal_destroy(f->value);
                free(f);
            }
            vec_deinit(&literal->object_fields);
            break;
        }
        case GRAPHQL_LITERAL_LIST: {
            int i;
            graphql_literal_t* item;
            vec_foreach(&literal->list_items, item, i) {
                graphql_literal_destroy(item);
            }
            vec_deinit(&literal->list_items);
            break;
        }
        default:
            break;
    }
    free(literal);
}

graphql_literal_t* graphql_literal_copy(const graphql_literal_t* literal) {
    if (literal == NULL) return NULL;

    graphql_literal_t* copy = get_clear_memory(sizeof(graphql_literal_t));
    if (copy == NULL) return NULL;
    copy->kind = literal->kind;

    switch (literal->kind) {
        case GRAPHQL_LITERAL_STRING:
        case GRAPHQL_LITERAL_ENUM:
            if (literal->string_val) {
                copy->string_val = strdup(literal->string_val);
            }
            break;
        case GRAPHQL_LITERAL_INT:
            copy->int_val = literal->int_val;
            break;
        case GRAPHQL_LITERAL_FLOAT:
            copy->float_val = literal->float_val;
            break;
        case GRAPHQL_LITERAL_BOOL:
            copy->bool_val = literal->bool_val;
            break;
        case GRAPHQL_LITERAL_OBJECT: {
            int i;
            graphql_object_field_t* f;
            vec_foreach(&literal->object_fields, f, i) {
                graphql_object_field_t* fcopy = get_clear_memory(sizeof(graphql_object_field_t));
                if (fcopy == NULL) break;
                fcopy->name = f->name ? strdup(f->name) : NULL;
                fcopy->value = graphql_literal_copy(f->value);
                vec_push(&copy->object_fields, fcopy);
            }
            break;
        }
        case GRAPHQL_LITERAL_LIST: {
            int i;
            graphql_literal_t* item;
            vec_foreach(&literal->list_items, item, i) {
                graphql_literal_t* itemcopy = graphql_literal_copy(item);
                if (itemcopy) vec_push(&copy->list_items, itemcopy);
            }
            break;
        }
        default:
            break;
    }
    return copy;
}

// ============================================================
// Field functions
// ============================================================

graphql_field_t* graphql_field_create(const char* name,
                                       graphql_type_ref_t* type,
                                       bool is_required) {
    graphql_field_t* field = get_clear_memory(sizeof(graphql_field_t));
    if (field == NULL) return NULL;
    if (name) {
        field->name = strdup(name);
        if (field->name == NULL) {
            free(field);
            return NULL;
        }
    }
    field->type = type;
    field->is_required = is_required;
    return field;
}

void graphql_field_destroy(graphql_field_t* field) {
    if (field == NULL) return;
    free(field->name);
    graphql_type_ref_destroy(field->type);
    graphql_literal_destroy(field->default_value);
    int i;
    graphql_directive_t* d;
    vec_foreach(&field->directives, d, i) {
        graphql_directive_destroy(d);
    }
    vec_deinit(&field->directives);
    free(field);
}

// ============================================================
// Type functions
// ============================================================

graphql_type_t* graphql_type_create(const char* name, graphql_type_kind_t kind) {
    graphql_type_t* type = get_clear_memory(sizeof(graphql_type_t));
    if (type == NULL) return NULL;
    if (name) {
        type->name = strdup(name);
        if (type->name == NULL) {
            free(type);
            return NULL;
        }
    }
    type->kind = kind;
    return type;
}

void graphql_type_destroy(graphql_type_t* type) {
    if (type == NULL) return;
    free(type->name);
    free(type->plural_name);
    int i;
    graphql_field_t* f;
    vec_foreach(&type->fields, f, i) {
        graphql_field_destroy(f);
    }
    vec_deinit(&type->fields);
    char* ev;
    vec_foreach(&type->enum_values, ev, i) {
        free(ev);
    }
    vec_deinit(&type->enum_values);
    graphql_directive_t* d;
    vec_foreach(&type->directives, d, i) {
        graphql_directive_destroy(d);
    }
    vec_deinit(&type->directives);
    free(type);
}

// ============================================================
// Result node functions
// ============================================================

graphql_result_node_t* graphql_result_node_create(graphql_result_kind_t kind,
                                                    const char* name) {
    graphql_result_node_t* node = get_clear_memory(sizeof(graphql_result_node_t));
    if (node == NULL) return NULL;
    node->kind = kind;
    if (name) {
        node->name = strdup(name);
        if (node->name == NULL) {
            free(node);
            return NULL;
        }
    }
    return node;
}

void graphql_result_node_destroy(graphql_result_node_t* node) {
    if (node == NULL) return;
    free(node->name);
    switch (node->kind) {
        case RESULT_STRING:
        case RESULT_ID:
            free(node->string_val);
            break;
        case RESULT_LIST:
        case RESULT_OBJECT: {
            int i;
            graphql_result_node_t* child;
            vec_foreach(&node->children, child, i) {
                graphql_result_node_destroy(child);
            }
            vec_deinit(&node->children);
            break;
        }
        default:
            break;
    }
    free(node);
}

int graphql_result_node_add_child(graphql_result_node_t* parent,
                                   graphql_result_node_t* child) {
    if (parent == NULL || child == NULL) return -1;
    if (parent->kind != RESULT_OBJECT && parent->kind != RESULT_LIST) return -1;
    return vec_push(&parent->children, child);
}

// ============================================================
// Result functions
// ============================================================

void graphql_result_destroy(graphql_result_t* result) {
    if (result == NULL) return;
    graphql_result_node_destroy(result->data);
    int i;
    for (i = 0; i < result->errors.length; i++) {
        graphql_error_contents_destroy(&result->errors.data[i]);
    }
    vec_deinit(&result->errors);
    free(result);
}

// ============================================================
// Error functions
// ============================================================

graphql_error_t graphql_error_create(const char* message, const char* path) {
    graphql_error_t error;
    memset(&error, 0, sizeof(error));
    error.message = message ? strdup(message) : NULL;
    error.path = path ? strdup(path) : NULL;
    return error;
}

void graphql_error_contents_destroy(graphql_error_t* error) {
    if (error == NULL) return;
    free(error->message);
    free(error->path);
    vec_deinit(&error->locations);
}

// ============================================================
// Plan functions
// ============================================================

graphql_plan_t* graphql_plan_create(graphql_plan_kind_t kind) {
    graphql_plan_t* plan = get_clear_memory(sizeof(graphql_plan_t));
    if (plan == NULL) return NULL;
    plan->kind = kind;
    return plan;
}

void graphql_plan_destroy(graphql_plan_t* plan) {
    if (plan == NULL) return;
    free(plan->type_name);
    free(plan->field_name);
    free(plan->alias);
    if (plan->base_path) path_destroy(plan->base_path);
    if (plan->scan_start) path_destroy(plan->scan_start);
    if (plan->scan_end) path_destroy(plan->scan_end);
    if (plan->get_path) path_destroy(plan->get_path);
    free(plan->ref_type);
    if (plan->ref_id) identifier_destroy(plan->ref_id);
    int i;
    for (i = 0; i < plan->args.length; i++) {
        free(plan->args.data[i].name);
        free(plan->args.data[i].value);
    }
    vec_deinit(&plan->args);
    graphql_plan_t* child;
    vec_foreach(&plan->children, child, i) {
        graphql_plan_destroy(child);
    }
    vec_deinit(&plan->children);
    // Don't destroy result - it's owned by the caller
    free(plan);
}

// ============================================================
// Directive functions
// ============================================================

graphql_directive_t* graphql_directive_create(const char* name) {
    graphql_directive_t* directive = get_clear_memory(sizeof(graphql_directive_t));
    if (directive == NULL) return NULL;
    if (name) {
        directive->name = strdup(name);
        if (directive->name == NULL) {
            free(directive);
            return NULL;
        }
    }
    return directive;
}

void graphql_directive_destroy(graphql_directive_t* directive) {
    if (directive == NULL) return;
    free(directive->name);
    int i;
    char* name;
    vec_foreach(&directive->arg_names, name, i) {
        free(name);
    }
    vec_deinit(&directive->arg_names);
    char* value;
    vec_foreach(&directive->arg_values, value, i) {
        free(value);
    }
    vec_deinit(&directive->arg_values);
    free(directive);
}

// ============================================================
// Type registry functions
// ============================================================

graphql_type_registry_t* graphql_type_registry_create(void) {
    graphql_type_registry_t* registry = get_clear_memory(sizeof(graphql_type_registry_t));
    if (registry == NULL) return NULL;
    return registry;
}

void graphql_type_registry_destroy(graphql_type_registry_t* registry) {
    if (registry == NULL) return;
    int i;
    graphql_type_t* type;
    vec_foreach(&registry->types, type, i) {
        graphql_type_destroy(type);
    }
    vec_deinit(&registry->types);
    free(registry);
}

int graphql_type_registry_register(graphql_type_registry_t* registry,
                                     graphql_type_t* type) {
    if (registry == NULL || type == NULL) return -1;
    return vec_push(&registry->types, type);
}

graphql_type_t* graphql_type_registry_get(graphql_type_registry_t* registry,
                                           const char* name) {
    if (registry == NULL || name == NULL) return NULL;
    int i;
    graphql_type_t* type;
    vec_foreach(&registry->types, type, i) {
        if (strcmp(type->name, name) == 0) {
            return type;
        }
    }
    return NULL;
}

// ============================================================
// Layer config functions
// ============================================================

graphql_layer_config_t* graphql_layer_config_default(void) {
    graphql_layer_config_t* config = get_clear_memory(sizeof(graphql_layer_config_t));
    if (config == NULL) return NULL;
    config->chunk_size = DATABASE_CONFIG_DEFAULT_CHUNK_SIZE;
    config->btree_node_size = DATABASE_CONFIG_DEFAULT_BTREE_NODE_SIZE;
    config->lru_memory_mb = DATABASE_CONFIG_DEFAULT_LRU_MEMORY_MB;
    config->lru_shards = DATABASE_CONFIG_DEFAULT_LRU_SHARDS;
    config->worker_threads = DATABASE_CONFIG_DEFAULT_WORKER_THREADS;
    config->enable_persist = 1;
    config->delimiter = GRAPHQL_LAYER_DEFAULT_DELIMITER;
    return config;
}

void graphql_layer_config_destroy(graphql_layer_config_t* config) {
    free(config);
}

const char* graphql_type_get_plural(graphql_type_t* type) {
    if (type == NULL) return NULL;
    if (type->plural_name != NULL) return type->plural_name;
    return type->name;  // Default: use type name as-is, append 's' at path construction
}