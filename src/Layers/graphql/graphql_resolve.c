//
// GraphQL Resolve - Query and mutation execution
// Created: 2026-04-12
//

#include "graphql_resolve.h"
#include "graphql_parser.h"
#include "graphql_lexer.h"
#include "graphql_plan.h"
#include "graphql_result.h"
#include "graphql_schema.h"
#include "../../Util/allocator.h"
#include "../../HBTrie/path.h"
#include "../../HBTrie/identifier.h"
#include "../../HBTrie/chunk.h"
#include "../../Database/database.h"
#include "../../Database/database_iterator.h"
#include "../../Database/batch.h"
#include "../../Workers/pool.h"
#include "../../Workers/work.h"
#include "../../Workers/promise.h"
#include "../../Workers/error.h"
#include "../../RefCounter/refcounter.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// ============================================================
// Constants
// ============================================================

#define MAX_RESOLVE_DEPTH 15

// Effective name for result nodes: prefer alias over type_name
#define PLAN_NAME(p) ((p)->alias ? (p)->alias : ((p)->field_name ? (p)->field_name : (p)->type_name))

// ============================================================
// Error collection helpers
// ============================================================

static void push_error(graphql_result_t* result, const char* message, const char* path) {
    if (result == NULL) return;
    graphql_error_t error;
    memset(&error, 0, sizeof(error));
    error.message = strdup(message);
    error.path = path ? strdup(path) : NULL;
    vec_push(&result->errors, error);
}

static char* build_error_path(const char* parent, const char* child) {
    if (parent == NULL || parent[0] == '\0') {
        return child ? strdup(child) : strdup("");
    }
    if (child == NULL || child[0] == '\0') return strdup(parent);
    size_t len = strlen(parent) + 1 + strlen(child) + 1;
    char* path = malloc(len);
    if (path == NULL) return strdup(child ? child : "");
    snprintf(path, len, "%s.%s", parent, child);
    return path;
}

// ============================================================
// Forward declarations
// ============================================================

static graphql_result_node_t* execute_plan(graphql_layer_t* layer,
                                            graphql_plan_t* plan,
                                            graphql_type_t* type,
                                            const char* parent_id,
                                            int depth,
                                            graphql_result_t* result,
                                            const char* path_prefix);
static graphql_result_node_t* resolve_field(graphql_layer_t* layer,
                                             graphql_plan_t* plan,
                                             graphql_type_t* parent_type,
                                             const char* parent_id,
                                             int depth,
                                             graphql_result_t* result,
                                             const char* path_prefix);
static graphql_result_node_t* execute_get(graphql_layer_t* layer,
                                           graphql_plan_t* plan,
                                           graphql_type_t* type,
                                           graphql_result_t* result,
                                           const char* path_prefix);
static graphql_result_node_t* execute_scan(graphql_layer_t* layer,
                                            graphql_plan_t* plan,
                                            graphql_type_t* type,
                                            int depth,
                                            graphql_result_t* result,
                                            const char* path_prefix);
static char* db_get_string(database_t* db, const char* path_str);
static int db_put_string(database_t* db, const char* path_str, const char* value);

// Re-declare from schema.c — these are static there, so we implement local versions
static char* local_db_get_string(database_t* db, const char* path_str);
static int local_db_put_string(database_t* db, const char* path_str, const char* value);

// ============================================================
// Database helpers (local copies for resolve module)
// ============================================================

static char* local_db_get_string(database_t* db, const char* path_str) {
    path_t* path = path_create();
    if (path == NULL) return NULL;

    const char* start = path_str;
    while (*start) {
        const char* end = strchr(start, '/');
        size_t len = end ? (size_t)(end - start) : strlen(start);
        if (len > 0) {
            buffer_t* buf = buffer_create(len);
            if (buf == NULL) {
                path_destroy(path);
                return NULL;
            }
            memcpy(buf->data, start, len);
            buf->size = len;
            identifier_t* id = identifier_create(buf, 0);
            buffer_destroy(buf);
            if (id == NULL) {
                path_destroy(path);
                return NULL;
            }
            path_append(path, id);
            identifier_destroy(id);
        }
        if (end) {
            start = end + 1;
        } else {
            break;
        }
    }

    identifier_t* value = NULL;
    int rc = database_get_sync(db, path, &value);
    // path is consumed by database_get_sync, don't destroy it
    if (rc != 0) return NULL;

    buffer_t* val_buf = identifier_to_buffer(value);
    identifier_destroy(value);
    if (val_buf == NULL) return NULL;

    char* result = malloc(val_buf->size + 1);
    if (result == NULL) {
        buffer_destroy(val_buf);
        return NULL;
    }
    memcpy(result, val_buf->data, val_buf->size);
    result[val_buf->size] = '\0';
    buffer_destroy(val_buf);
    return result;
}

static int local_db_put_string(database_t* db, const char* path_str, const char* value) {
    path_t* path = path_create();
    if (path == NULL) return -1;

    const char* start = path_str;
    while (*start) {
        const char* end = strchr(start, '/');
        size_t len = end ? (size_t)(end - start) : strlen(start);
        if (len > 0) {
            buffer_t* buf = buffer_create(len);
            if (buf == NULL) {
                path_destroy(path);
                return -1;
            }
            memcpy(buf->data, start, len);
            buf->size = len;
            identifier_t* id = identifier_create(buf, 0);
            buffer_destroy(buf);
            if (id == NULL) {
                path_destroy(path);
                return -1;
            }
            path_append(path, id);
            identifier_destroy(id);
        }
        if (end) {
            start = end + 1;
        } else {
            break;
        }
    }

    buffer_t* val_buf = buffer_create(strlen(value));
    if (val_buf == NULL) {
        path_destroy(path);
        return -1;
    }
    memcpy(val_buf->data, value, strlen(value));
    val_buf->size = strlen(value);
    identifier_t* val_id = identifier_create(val_buf, 0);
    buffer_destroy(val_buf);
    if (val_id == NULL) {
        path_destroy(path);
        return -1;
    }

    // database_put_sync takes ownership of both path and val_id
    return database_put_sync(db, path, val_id);
}

// ============================================================
// Plan execution
// ============================================================

static graphql_result_node_t* execute_get(graphql_layer_t* layer,
                                           graphql_plan_t* plan,
                                           graphql_type_t* type,
                                           graphql_result_t* result,
                                           const char* path_prefix) {
    if (plan->get_path == NULL) {
        graphql_result_node_t* null_node = graphql_result_node_create(RESULT_NULL, PLAN_NAME(plan));
        return null_node;
    }

    // Build path string from get_path
    char path_buf[1024] = {0};
    int pos = 0;
    for (size_t i = 0; i < path_length(plan->get_path); i++) {
        identifier_t* id = path_get(plan->get_path, i);
        if (id == NULL) break;
        buffer_t* buf = identifier_to_buffer(id);
        if (buf == NULL) break;
        if (pos > 0) pos += snprintf(path_buf + pos, sizeof(path_buf) - pos, "/");
        pos += snprintf(path_buf + pos, sizeof(path_buf) - pos, "%.*s", (int)buf->size, buf->data);
        buffer_destroy(buf);
    }

    char* value = local_db_get_string(layer->db, path_buf);
    if (value == NULL) {
        return graphql_result_node_create(RESULT_NULL, PLAN_NAME(plan));
    }

    // Determine result kind based on type
    graphql_result_kind_t kind = RESULT_STRING;
    if (type != NULL && type->kind == GRAPHQL_TYPE_ENUM) {
        kind = RESULT_STRING;  // Enums serialize as strings
    }

    graphql_result_node_t* node = graphql_result_node_create(kind, PLAN_NAME(plan));
    if (node != NULL) {
        node->string_val = value;
    } else {
        free(value);
    }
    return node;
}

static graphql_result_node_t* execute_scan(graphql_layer_t* layer,
                                            graphql_plan_t* plan,
                                            graphql_type_t* type,
                                            int depth,
                                            graphql_result_t* result,
                                            const char* path_prefix) {
    // Look up the type from plan's type_name if not provided
    graphql_type_t* scan_type = type;
    if (scan_type == NULL && plan->type_name != NULL && layer->registry != NULL) {
        scan_type = graphql_schema_get_type(layer, plan->type_name);
    }

    graphql_result_node_t* list = graphql_result_node_create(RESULT_LIST, PLAN_NAME(plan));
    if (list == NULL) return NULL;

    if (depth > MAX_RESOLVE_DEPTH) {
        return list;
    }

    // Scan for all entries under the type's path prefix
    path_t* scan_start = plan->scan_start;
    if (scan_start == NULL) {
        return list;
    }

    database_iterator_t* iter = database_scan_start(layer->db, scan_start, NULL);
    if (iter == NULL) {
        return list;
    }

    path_t* out_path = NULL;
    identifier_t* out_value = NULL;

    // Collect unique entity IDs from scan
    vec_t(char*) ids;
    vec_init(&ids);

    while (database_scan_next(iter, &out_path, &out_value) == 0) {
        if (out_path == NULL) break;

        // Extract ID from path: {PluralType}/{id}/...
        size_t path_len = path_length(out_path);
        if (path_len >= 2) {
            identifier_t* id_id = path_get(out_path, 1);
            if (id_id != NULL) {
                buffer_t* buf = identifier_to_buffer(id_id);
                if (buf != NULL) {
                    char* id_str = malloc(buf->size + 1);
                    if (id_str != NULL) {
                        memcpy(id_str, buf->data, buf->size);
                        id_str[buf->size] = '\0';

                        // Skip internal/schema entries (prefixed with __)
                        if (id_str[0] == '_' && id_str[1] == '_') {
                            free(id_str);
                            buffer_destroy(buf);
                            // Skip this entry, clean up below
                        } else {
                            // Check for duplicates
                            bool found = false;
                            for (size_t i = 0; i < ids.length; i++) {
                                if (strcmp(ids.data[i], id_str) == 0) {
                                    found = true;
                                    break;
                                }
                            }

                            if (!found) {
                                vec_push(&ids, id_str);
                            } else {
                                free(id_str);
                            }
                            buffer_destroy(buf);
                        }
                    }
                }
            }
        }

        if (out_path) { path_destroy(out_path); out_path = NULL; }
        if (out_value) { identifier_destroy(out_value); out_value = NULL; }
    }

    database_scan_end(iter);

    // For each entity ID, resolve child plans to build a complete object
    for (size_t i = 0; i < ids.length; i++) {
        const char* entity_id = ids.data[i];

        if (plan->children.length == 0) {
            // No child selections — return just the ID
            graphql_result_node_t* obj = graphql_result_node_create(RESULT_OBJECT, PLAN_NAME(plan));
            if (obj != NULL) {
                graphql_result_node_t* id_node = graphql_result_node_create(RESULT_STRING, "id");
                if (id_node != NULL) {
                    id_node->string_val = strdup(entity_id);
                    graphql_result_node_add_child(obj, id_node);
                }
                graphql_result_node_add_child(list, obj);
            }
        } else {
            // Resolve child plans for this entity
            graphql_result_node_t* obj = graphql_result_node_create(RESULT_OBJECT, PLAN_NAME(plan));
            if (obj != NULL) {
                // Always include the entity ID
                graphql_result_node_t* id_node = graphql_result_node_create(RESULT_STRING, "id");
                if (id_node != NULL) {
                    id_node->string_val = strdup(entity_id);
                    graphql_result_node_add_child(obj, id_node);
                }

                for (int j = 0; j < plan->children.length; j++) {
                    graphql_plan_t* child = plan->children.data[j];
                    graphql_type_t* child_type = NULL;
                    if (child->type_name != NULL && layer->registry != NULL) {
                        child_type = graphql_schema_get_type(layer, child->type_name);
                    }

                    char* child_path = build_error_path(path_prefix, PLAN_NAME(child));
                    graphql_result_node_t* child_result = resolve_field(layer, child, scan_type, entity_id, depth + 1, result, child_path);
                    free(child_path);
                    if (child_result != NULL) {
                        graphql_result_node_add_child(obj, child_result);
                    }
                }
                graphql_result_node_add_child(list, obj);
            }
        }
    }

    // Free collected IDs
    for (size_t i = 0; i < ids.length; i++) {
        free(ids.data[i]);
    }
    vec_deinit(&ids);

    return list;
}

static graphql_result_node_t* resolve_field(graphql_layer_t* layer,
                                             graphql_plan_t* plan,
                                             graphql_type_t* parent_type,
                                             const char* parent_id,
                                             int depth,
                                             graphql_result_t* result,
                                             const char* path_prefix) {
    if (depth > MAX_RESOLVE_DEPTH) {
        return graphql_result_node_create(RESULT_NULL, PLAN_NAME(plan));
    }

    switch (plan->kind) {
        case PLAN_GET:
            return execute_get(layer, plan, parent_type, result, path_prefix);


        case PLAN_SCAN:
            return execute_scan(layer, plan, parent_type, depth, result, path_prefix);

        case PLAN_RESOLVE_FIELD: {
            // __typename: return parent type name without DB lookup
            if (plan->field_name && strcmp(plan->field_name, "__typename") == 0) {
                const char* type_name = plan->type_name ? plan->type_name : "Unknown";
                graphql_result_node_t* node = graphql_result_node_create(RESULT_STRING, PLAN_NAME(plan));
                if (node != NULL) {
                    node->string_val = strdup(type_name);
                }
                return node;
            }

            // Resolve a single field from a parent object
            char path_buf[512];
            const char* plural = parent_type ? graphql_type_get_plural(parent_type) : "Unknown";
            const char* resolve_name = plan->field_name ? plan->field_name : plan->type_name;
            if (parent_id != NULL) {
                snprintf(path_buf, sizeof(path_buf), "%s/%s/%s", plural, parent_id, resolve_name);
            } else {
                snprintf(path_buf, sizeof(path_buf), "%s/%s", plural, resolve_name);
            }

            char* value = local_db_get_string(layer->db, path_buf);
            if (value == NULL) {
                return graphql_result_node_create(RESULT_NULL, PLAN_NAME(plan));
            }

            // If this field has child selections and resolves to a reference type,
            // look up the referenced type and resolve nested fields
            if (plan->children.length > 0) {
                // This is a reference field — value is an ID pointing to another type
                graphql_type_t* ref_type = NULL;
                if (plan->type_name != NULL && layer->registry != NULL) {
                    ref_type = graphql_schema_get_type(layer, plan->type_name);
                }

                graphql_result_node_t* obj = graphql_result_node_create(RESULT_OBJECT, PLAN_NAME(plan));
                if (obj != NULL) {
                    // Include the referenced ID
                    graphql_result_node_t* ref_id_node = graphql_result_node_create(RESULT_STRING, "id");
                    if (ref_id_node != NULL) {
                        ref_id_node->string_val = strdup(value);
                        graphql_result_node_add_child(obj, ref_id_node);
                    }

                    // Resolve child fields using the referenced ID as parent
                    for (int i = 0; i < plan->children.length; i++) {
                        graphql_plan_t* child = plan->children.data[i];
                        graphql_type_t* child_type = NULL;
                        if (child->type_name != NULL && layer->registry != NULL) {
                            child_type = graphql_schema_get_type(layer, child->type_name);
                        }

                        char* child_path = build_error_path(path_prefix, PLAN_NAME(child));
                        graphql_result_node_t* child_result = resolve_field(layer, child, ref_type, value, depth + 1, result, child_path);
                        free(child_path);
                        if (child_result != NULL) {
                            graphql_result_node_add_child(obj, child_result);
                        }
                    }
                }
                free(value);
                return obj;
            }

            // Scalar field — return the value directly
            graphql_result_node_t* node = graphql_result_node_create(RESULT_STRING, PLAN_NAME(plan));
            if (node != NULL) {
                node->string_val = value;
            } else {
                free(value);
            }
            return node;
        }

        case PLAN_RESOLVE_REF: {
            // Follow a reference to another type
            // The reference field value is stored as an ID in the parent
            // Look up the referenced type and resolve its fields
            if (plan->ref_type == NULL || parent_id == NULL) {
                return graphql_result_node_create(RESULT_NULL, PLAN_NAME(plan));
            }

            graphql_type_t* ref_type = NULL;
            if (layer->registry != NULL) {
                ref_type = graphql_schema_get_type(layer, plan->ref_type);
            }
            const char* ref_plural = ref_type ? graphql_type_get_plural(ref_type) : plan->ref_type;

            // If we have a specific ref_id, resolve that entity
            if (plan->ref_id != NULL) {
                buffer_t* buf = identifier_to_buffer(plan->ref_id);
                if (buf == NULL) {
                    return graphql_result_node_create(RESULT_NULL, PLAN_NAME(plan));
                }
                char* ref_id_str = malloc(buf->size + 1);
                if (ref_id_str == NULL) {
                    buffer_destroy(buf);
                    return graphql_result_node_create(RESULT_NULL, PLAN_NAME(plan));
                }
                memcpy(ref_id_str, buf->data, buf->size);
                ref_id_str[buf->size] = '\0';
                buffer_destroy(buf);

                // Build object with referenced entity's fields
                graphql_result_node_t* obj = graphql_result_node_create(RESULT_OBJECT, PLAN_NAME(plan));
                if (obj != NULL) {
                    graphql_result_node_t* id_node = graphql_result_node_create(RESULT_STRING, "id");
                    if (id_node != NULL) {
                        id_node->string_val = strdup(ref_id_str);
                        graphql_result_node_add_child(obj, id_node);
                    }

                    for (int i = 0; i < plan->children.length; i++) {
                        graphql_plan_t* child = plan->children.data[i];
                        graphql_type_t* child_type = NULL;
                        if (child->type_name != NULL && layer->registry != NULL) {
                            child_type = graphql_schema_get_type(layer, child->type_name);
                        }

                        char* child_path = build_error_path(path_prefix, PLAN_NAME(child));
                        graphql_result_node_t* child_result = resolve_field(layer, child, ref_type, ref_id_str, depth + 1, result, child_path);
                        free(child_path);
                        if (child_result != NULL) {
                            graphql_result_node_add_child(obj, child_result);
                        }
                    }
                }
                free(ref_id_str);
                return obj;
            }

            // No specific ref_id — this might be a list reference
            // Scan for all referenced IDs under parent_type/parent_id/field_name
            char scan_path[512];
            const char* plural = parent_type ? graphql_type_get_plural(parent_type) : "Unknown";
            const char* ref_field_name = plan->field_name ? plan->field_name : plan->type_name;
            snprintf(scan_path, sizeof(scan_path), "%s/%s/%s", plural, parent_id, ref_field_name);

            // Get the field value(s) — could be a single ID or a list
            char* ref_value = local_db_get_string(layer->db, scan_path);
            if (ref_value != NULL) {
                // Single reference — resolve it
                graphql_result_node_t* ref_result = graphql_result_node_create(RESULT_OBJECT, PLAN_NAME(plan));
                if (ref_result != NULL) {
                    graphql_result_node_t* id_node = graphql_result_node_create(RESULT_STRING, "id");
                    if (id_node != NULL) {
                        id_node->string_val = ref_value;
                        graphql_result_node_add_child(ref_result, id_node);
                    } else {
                        free(ref_value);
                    }

                    for (int i = 0; i < plan->children.length; i++) {
                        graphql_plan_t* child = plan->children.data[i];
                        graphql_type_t* child_type = NULL;
                        if (child->type_name != NULL && layer->registry != NULL) {
                            child_type = graphql_schema_get_type(layer, child->type_name);
                        }

                        char* child_path = build_error_path(path_prefix, PLAN_NAME(child));
                        graphql_result_node_t* child_result = resolve_field(layer, child, ref_type, ref_value, depth + 1, result, child_path);
                        free(child_path);
                        if (child_result != NULL) {
                            graphql_result_node_add_child(ref_result, child_result);
                        }
                    }
                }
                return ref_result;
            }

            // Try scanning for multiple references under the field path
            path_t* field_path = path_create();
            if (field_path == NULL) {
                return graphql_result_node_create(RESULT_NULL, PLAN_NAME(plan));
            }
            const char* s = scan_path;
            while (*s) {
                const char* e = strchr(s, '/');
                size_t len = e ? (size_t)(e - s) : strlen(s);
                if (len > 0) {
                    buffer_t* buf = buffer_create(len);
                    if (buf != NULL) {
                        memcpy(buf->data, s, len);
                        buf->size = len;
                        identifier_t* id = identifier_create(buf, 0);
                        buffer_destroy(buf);
                        if (id != NULL) {
                            path_append(field_path, id);
                            identifier_destroy(id);
                        }
                    }
                }
                if (e) { s = e + 1; } else { break; }
            }

            database_iterator_t* iter = database_scan_start(layer->db, field_path, NULL);
            // field_path consumed by scan_start

            if (iter == NULL) {
                return graphql_result_node_create(RESULT_NULL, PLAN_NAME(plan));
            }

            graphql_result_node_t* list = graphql_result_node_create(RESULT_LIST, PLAN_NAME(plan));
            path_t* out_path = NULL;
            identifier_t* out_val = NULL;

            while (database_scan_next(iter, &out_path, &out_val) == 0) {
                if (out_path == NULL) break;

                // Extract referenced ID from the path or value
                if (out_val != NULL) {
                    buffer_t* val_buf = identifier_to_buffer(out_val);
                    if (val_buf != NULL) {
                        char* ref_id = malloc(val_buf->size + 1);
                        if (ref_id != NULL) {
                            memcpy(ref_id, val_buf->data, val_buf->size);
                            ref_id[val_buf->size] = '\0';

                            graphql_result_node_t* ref_obj = graphql_result_node_create(RESULT_OBJECT, PLAN_NAME(plan));
                            if (ref_obj != NULL) {
                                graphql_result_node_t* id_node = graphql_result_node_create(RESULT_STRING, "id");
                                if (id_node != NULL) {
                                    id_node->string_val = strdup(ref_id);
                                    graphql_result_node_add_child(ref_obj, id_node);
                                }

                                for (int i = 0; i < plan->children.length; i++) {
                                    graphql_plan_t* child = plan->children.data[i];
                                    graphql_type_t* child_type = NULL;
                                    if (child->type_name != NULL && layer->registry != NULL) {
                                        child_type = graphql_schema_get_type(layer, child->type_name);
                                    }

                                    char* child_path = build_error_path(path_prefix, PLAN_NAME(child));
                                    graphql_result_node_t* child_result = resolve_field(layer, child, ref_type, ref_id, depth + 1, result, child_path);
                                    free(child_path);
                                    if (child_result != NULL) {
                                        graphql_result_node_add_child(ref_obj, child_result);
                                    }
                                }
                                graphql_result_node_add_child(list, ref_obj);
                            }
                            free(ref_id);
                        }
                        buffer_destroy(val_buf);
                    }
                }

                if (out_path) { path_destroy(out_path); out_path = NULL; }
                if (out_val) { identifier_destroy(out_val); out_val = NULL; }
            }

            database_scan_end(iter);
            return list;
        }

        case PLAN_CUSTOM:
            if (plan->resolver != NULL) {
                // Build args_json from plan args
                char args_buf[1024];
                int apos = 0;
                apos += snprintf(args_buf + apos, sizeof(args_buf) - apos, "{");
                for (int i = 0; i < plan->args.length; i++) {
                    if (i > 0) apos += snprintf(args_buf + apos, sizeof(args_buf) - apos, ",");
                    apos += snprintf(args_buf + apos, sizeof(args_buf) - apos, "\"%s\":\"%s\"",
                                     plan->args.data[i].name, plan->args.data[i].value);
                }
                apos += snprintf(args_buf + apos, sizeof(args_buf) - apos, "}");

                // Build parent_path from plan
                path_t* parent_path = plan->base_path ? plan->base_path : NULL;

                return plan->resolver(layer, NULL, parent_path, NULL, args_buf, plan->resolver_ctx);
            }
            return graphql_result_node_create(RESULT_NULL, PLAN_NAME(plan));

        case PLAN_FILTER:
            // Filter is already applied during compilation (@skip/@include)
            return graphql_result_node_create(RESULT_NULL, PLAN_NAME(plan));

        case PLAN_BATCH_GET: {
            // Batch get: resolve specific entities identified by id arguments
            graphql_result_node_t* list = graphql_result_node_create(RESULT_LIST, PLAN_NAME(plan));
            if (list == NULL) return NULL;

            if (depth > MAX_RESOLVE_DEPTH) {
                return list;
            }

            // Look up the type from the plan's type_name if parent_type is not available
            graphql_type_t* batch_type = parent_type;
            if (batch_type == NULL && plan->type_name != NULL && layer->registry != NULL) {
                batch_type = graphql_schema_get_type(layer, plan->type_name);
            }
            const char* parent_plural = batch_type ? graphql_type_get_plural(batch_type) : "Unknown";

            for (int ai = 0; ai < plan->args.length; ai++) {
                if (strcmp(plan->args.data[ai].name, "id") != 0) continue;
                const char* entity_id = plan->args.data[ai].value;

                // Build object for this entity
                graphql_result_node_t* obj = graphql_result_node_create(RESULT_OBJECT, PLAN_NAME(plan));
                if (obj == NULL) continue;

                // Include the entity ID
                graphql_result_node_t* id_node = graphql_result_node_create(RESULT_STRING, "id");
                if (id_node != NULL) {
                    id_node->string_val = strdup(entity_id);
                    graphql_result_node_add_child(obj, id_node);
                }

                // Resolve child plans for this entity using RESOLVE_FIELD-style path construction
                for (int j = 0; j < plan->children.length; j++) {
                    graphql_plan_t* child = plan->children.data[j];
                    char* child_path = build_error_path(path_prefix, PLAN_NAME(child));
                    graphql_result_node_t* child_result = resolve_field(layer, child, batch_type, entity_id, depth + 1, result, child_path);
                    free(child_path);
                    if (child_result != NULL) {
                        graphql_result_node_add_child(obj, child_result);
                    }
                }

                graphql_result_node_add_child(list, obj);
            }

            return list;
        }

        default:
            return graphql_result_node_create(RESULT_NULL, PLAN_NAME(plan));
    }
}

static graphql_result_node_t* execute_plan(graphql_layer_t* layer,
                                            graphql_plan_t* plan,
                                            graphql_type_t* type,
                                            const char* parent_id,
                                            int depth,
                                            graphql_result_t* result,
                                            const char* path_prefix) {
    if (plan == NULL) return NULL;

    if (depth > MAX_RESOLVE_DEPTH) {
        return graphql_result_node_create(RESULT_NULL, PLAN_NAME(plan));
    }

    // For SCAN and RESOLVE_REF, resolve_field handles child resolution internally
    // (since scan iterates per-entity and resolves children for each)
    if (plan->kind == PLAN_SCAN || plan->kind == PLAN_RESOLVE_REF) {
        return resolve_field(layer, plan, type, parent_id, depth, result, path_prefix);
    }

    // For other plan kinds, resolve the field and then attach child results
    graphql_result_node_t* node_result = resolve_field(layer, plan, type, parent_id, depth, result, path_prefix);

    // If resolve_field returned NULL or a non-container type but we have children,
    // upgrade to an object node so children can be attached
    if (node_result != NULL && plan->children.length > 0 &&
        node_result->kind != RESULT_OBJECT && node_result->kind != RESULT_LIST) {
        graphql_result_node_t* obj = graphql_result_node_create(RESULT_OBJECT, PLAN_NAME(plan));
        if (obj != NULL) {
            // Transfer the name from the original node
            if (node_result->name) {
                free(obj->name);
                obj->name = node_result->name;
                node_result->name = NULL;
            }
            graphql_result_node_destroy(node_result);
            node_result = obj;
        }
    }
    if (node_result == NULL) {
        if (plan->children.length > 0) {
            node_result = graphql_result_node_create(RESULT_OBJECT, PLAN_NAME(plan));
        } else {
            return NULL;
        }
    }

    // Execute child plans and attach to result
    for (int i = 0; i < plan->children.length; i++) {
        graphql_plan_t* child = plan->children.data[i];
        graphql_type_t* child_type = NULL;
        if (child->type_name != NULL && layer->registry != NULL) {
            child_type = graphql_schema_get_type(layer, child->type_name);
        }

        char* child_path = build_error_path(path_prefix, PLAN_NAME(child));
        graphql_result_node_t* child_result = resolve_field(layer, child, child_type, parent_id, depth + 1, result, child_path);
        if (child_result == NULL) {
            push_error(result, "Failed to resolve field", child_path);
            free(child_path);
            graphql_result_node_t* null_node = graphql_result_node_create(RESULT_NULL, PLAN_NAME(child));
            if (null_node != NULL && node_result != NULL) {
                if (node_result->kind == RESULT_OBJECT || node_result->kind == RESULT_LIST) {
                    graphql_result_node_add_child(node_result, null_node);
                }
            }
            continue;
        }
        free(child_path);
        if (node_result != NULL) {
            if (node_result->kind == RESULT_OBJECT || node_result->kind == RESULT_LIST) {
                graphql_result_node_add_child(node_result, child_result);
            }
        }
    }

    return node_result;
}

// ============================================================
// Introspection
// ============================================================

static graphql_result_node_t* introspect_schema(graphql_layer_t* layer) {
    if (layer == NULL || layer->registry == NULL) {
        return graphql_result_node_create(RESULT_NULL, "__schema");
    }

    graphql_result_node_t* schema_node = graphql_result_node_create(RESULT_OBJECT, "__schema");
    if (schema_node == NULL) return NULL;

    // types: list of all registered types
    graphql_result_node_t* types_list = graphql_result_node_create(RESULT_LIST, "types");
    if (types_list != NULL) {
        for (int i = 0; i < layer->registry->types.length; i++) {
            graphql_type_t* type = layer->registry->types.data[i];
            graphql_result_node_t* type_obj = graphql_result_node_create(RESULT_OBJECT, NULL);
            if (type_obj != NULL) {
                graphql_result_node_t* name_node = graphql_result_node_create(RESULT_STRING, "name");
                if (name_node != NULL) {
                    name_node->string_val = strdup(type->name);
                    graphql_result_node_add_child(type_obj, name_node);
                }

                graphql_result_node_t* kind_node = graphql_result_node_create(RESULT_STRING, "kind");
                if (kind_node != NULL) {
                    const char* kind_str = "OBJECT";
                    switch (type->kind) {
                        case GRAPHQL_TYPE_SCALAR: kind_str = "SCALAR"; break;
                        case GRAPHQL_TYPE_OBJECT: kind_str = "OBJECT"; break;
                        case GRAPHQL_TYPE_ENUM: kind_str = "ENUM"; break;
                        case GRAPHQL_TYPE_LIST: kind_str = "LIST"; break;
                        case GRAPHQL_TYPE_NON_NULL: kind_str = "NON_NULL"; break;
                        case GRAPHQL_TYPE_INPUT: kind_str = "INPUT_OBJECT"; break;
                        case GRAPHQL_TYPE_INTERFACE: kind_str = "INTERFACE"; break;
                        case GRAPHQL_TYPE_UNION: kind_str = "UNION"; break;
                    }
                    kind_node->string_val = strdup(kind_str);
                    graphql_result_node_add_child(type_obj, kind_node);
                }

                // __typename on introspection type objects
                graphql_result_node_t* typename_node = graphql_result_node_create(RESULT_STRING, "__typename");
                if (typename_node != NULL) {
                    typename_node->string_val = strdup("__Type");
                    graphql_result_node_add_child(type_obj, typename_node);
                }

                // fields: list of field objects
                if (type->fields.length > 0) {
                    graphql_result_node_t* fields_list = graphql_result_node_create(RESULT_LIST, "fields");
                    if (fields_list != NULL) {
                        for (int j = 0; j < type->fields.length; j++) {
                            graphql_field_t* field = type->fields.data[j];
                            graphql_result_node_t* field_obj = graphql_result_node_create(RESULT_OBJECT, NULL);
                            if (field_obj != NULL) {
                                graphql_result_node_t* fname = graphql_result_node_create(RESULT_STRING, "name");
                                if (fname != NULL) {
                                    fname->string_val = strdup(field->name);
                                    graphql_result_node_add_child(field_obj, fname);
                                }

                                // Resolve type name from type ref
                                graphql_result_node_t* ftype = graphql_result_node_create(RESULT_STRING, "type");
                                if (ftype != NULL) {
                                    const char* type_name = "Unknown";
                                    if (field->type != NULL) {
                                        // Unwrap to base type name
                                        graphql_type_ref_t* ref = field->type;
                                        while (ref->of_type != NULL) ref = ref->of_type;
                                        if (ref->name != NULL) type_name = ref->name;
                                    }
                                    ftype->string_val = strdup(type_name);
                                    graphql_result_node_add_child(field_obj, ftype);
                                }

                                graphql_result_node_add_child(fields_list, field_obj);
                            }
                        }
                        graphql_result_node_add_child(type_obj, fields_list);
                    }
                }

                // enumValues: list of enum value strings
                if (type->kind == GRAPHQL_TYPE_ENUM && type->enum_values.length > 0) {
                    graphql_result_node_t* enum_list = graphql_result_node_create(RESULT_LIST, "enumValues");
                    if (enum_list != NULL) {
                        for (int j = 0; j < type->enum_values.length; j++) {
                            graphql_result_node_t* val = graphql_result_node_create(RESULT_STRING, NULL);
                            if (val != NULL) {
                                val->string_val = strdup(type->enum_values.data[j]);
                                graphql_result_node_add_child(enum_list, val);
                            }
                        }
                        graphql_result_node_add_child(type_obj, enum_list);
                    }
                }

                graphql_result_node_add_child(types_list, type_obj);
            }
        }
        graphql_result_node_add_child(schema_node, types_list);
    }

    return schema_node;
}

static graphql_result_node_t* introspect_type(graphql_layer_t* layer, const char* type_name) {
    if (layer == NULL || type_name == NULL) {
        return graphql_result_node_create(RESULT_NULL, "__type");
    }

    graphql_type_t* type = graphql_schema_get_type(layer, type_name);
    if (type == NULL) {
        return graphql_result_node_create(RESULT_NULL, "__type");
    }

    graphql_result_node_t* type_obj = graphql_result_node_create(RESULT_OBJECT, "__type");
    if (type_obj == NULL) return NULL;

    // name
    graphql_result_node_t* name_node = graphql_result_node_create(RESULT_STRING, "name");
    if (name_node != NULL) {
        name_node->string_val = strdup(type->name);
        graphql_result_node_add_child(type_obj, name_node);
    }

    // kind
    graphql_result_node_t* kind_node = graphql_result_node_create(RESULT_STRING, "kind");
    if (kind_node != NULL) {
        const char* kind_str = "OBJECT";
        switch (type->kind) {
            case GRAPHQL_TYPE_SCALAR: kind_str = "SCALAR"; break;
            case GRAPHQL_TYPE_OBJECT: kind_str = "OBJECT"; break;
            case GRAPHQL_TYPE_ENUM: kind_str = "ENUM"; break;
            case GRAPHQL_TYPE_LIST: kind_str = "LIST"; break;
            case GRAPHQL_TYPE_NON_NULL: kind_str = "NON_NULL"; break;
            case GRAPHQL_TYPE_INPUT: kind_str = "INPUT_OBJECT"; break;
            case GRAPHQL_TYPE_INTERFACE: kind_str = "INTERFACE"; break;
            case GRAPHQL_TYPE_UNION: kind_str = "UNION"; break;
        }
        kind_node->string_val = strdup(kind_str);
        graphql_result_node_add_child(type_obj, kind_node);
    }

    // fields
    if (type->fields.length > 0) {
        graphql_result_node_t* fields_list = graphql_result_node_create(RESULT_LIST, "fields");
        if (fields_list != NULL) {
            for (int i = 0; i < type->fields.length; i++) {
                graphql_field_t* field = type->fields.data[i];
                graphql_result_node_t* field_obj = graphql_result_node_create(RESULT_OBJECT, NULL);
                if (field_obj != NULL) {
                    graphql_result_node_t* fname = graphql_result_node_create(RESULT_STRING, "name");
                    if (fname != NULL) {
                        fname->string_val = strdup(field->name);
                        graphql_result_node_add_child(field_obj, fname);
                    }
                    graphql_result_node_add_child(fields_list, field_obj);
                }
            }
            graphql_result_node_add_child(type_obj, fields_list);
        }
    }

    // enumValues
    if (type->kind == GRAPHQL_TYPE_ENUM && type->enum_values.length > 0) {
        graphql_result_node_t* enum_list = graphql_result_node_create(RESULT_LIST, "enumValues");
        if (enum_list != NULL) {
            for (int i = 0; i < type->enum_values.length; i++) {
                graphql_result_node_t* val = graphql_result_node_create(RESULT_STRING, NULL);
                if (val != NULL) {
                    val->string_val = strdup(type->enum_values.data[i]);
                    graphql_result_node_add_child(enum_list, val);
                }
            }
            graphql_result_node_add_child(type_obj, enum_list);
        }
    }

    return type_obj;
}

// ============================================================
// Helper: create error result
// ============================================================

static graphql_result_t* make_error_result(const char* message, const char* path) {
    graphql_result_t* result = get_clear_memory(sizeof(graphql_result_t));
    if (result == NULL) return NULL;
    result->success = false;
    result->data = graphql_result_node_create(RESULT_NULL, NULL);
    graphql_error_t error = graphql_error_create(message, path);
    vec_push(&result->errors, error);
    return result;
}

// ============================================================
// Shared implementation (used by both sync and async paths)
// ============================================================

static graphql_result_t* graphql_query_impl(graphql_layer_t* layer, const char* query) {
    if (layer == NULL || query == NULL) {
        return make_error_result("Invalid arguments", NULL);
    }

    // Check for introspection queries before compiling
    // __schema and __type are handled directly without plan compilation
    // Use "__type(" to avoid matching __typename
    if (strstr(query, "__schema") != NULL || strstr(query, "__type(") != NULL) {
        graphql_result_t* result = get_clear_memory(sizeof(graphql_result_t));
        if (result == NULL) return make_error_result("Out of memory", NULL);

        result->data = graphql_result_node_create(RESULT_OBJECT, "data");
        result->success = true;
        if (result->data == NULL) {
            free(result);
            return make_error_result("Out of memory", NULL);
        }

        // Check for __schema
        if (strstr(query, "__schema") != NULL) {
            graphql_result_node_t* schema_result = introspect_schema(layer);
            if (schema_result != NULL) {
                graphql_result_node_add_child(result->data, schema_result);
            }
        }

        // Check for __type(name: "...")
        const char* type_pos = strstr(query, "__type(");
        if (type_pos != NULL) {
            // Extract the name argument from __type(name: "...")
            const char* name_start = strstr(type_pos, "name:");
            if (name_start == NULL) name_start = strstr(type_pos, "name :");
            if (name_start != NULL) {
                const char* quote = strchr(name_start, '"');
                if (quote != NULL) {
                    const char* quote_end = strchr(quote + 1, '"');
                    if (quote_end != NULL) {
                        size_t name_len = quote_end - quote - 1;
                        char* type_name = malloc(name_len + 1);
                        if (type_name != NULL) {
                            memcpy(type_name, quote + 1, name_len);
                            type_name[name_len] = '\0';
                            graphql_result_node_t* type_result = introspect_type(layer, type_name);
                            if (type_result != NULL) {
                                graphql_result_node_add_child(result->data, type_result);
                            }
                            free(type_name);
                        }
                    }
                }
            }
        }

        return result;
    }

    // Compile the query into a plan
    graphql_plan_t* plan = graphql_compile_query(layer, query);
    if (plan == NULL) {
        return make_error_result("Failed to compile query", NULL);
    }

    // Execute the plan
    graphql_result_t* result = get_clear_memory(sizeof(graphql_result_t));
    if (result == NULL) {
        graphql_plan_destroy(plan);
        return make_error_result("Out of memory", NULL);
    }

    result->data = execute_plan(layer, plan, NULL, NULL, 0, result, "data");
    result->success = true;

    graphql_plan_destroy(plan);
    return result;
}

// Check whether a type's required fields are present in a mutation's arguments.
// Returns NULL if validation passes, or an error result listing missing fields.
static graphql_result_t* validate_required_fields(graphql_type_t* type, graphql_ast_node_t* field) {
    if (type == NULL || type->kind != GRAPHQL_TYPE_OBJECT) return NULL;

    // Build list of missing required fields
    vec_t(char*) missing = {0};

    for (size_t i = 0; i < type->fields.length; i++) {
        graphql_field_t* f = type->fields.data[i];
        if (!f->is_required && !(f->type && f->type->kind == GRAPHQL_TYPE_NON_NULL)) continue;

        // Check if this required field is provided in the mutation arguments
        bool found = false;
        for (int j = 0; j < field->arguments.length; j++) {
            graphql_ast_node_t* arg = field->arguments.data[j];
            if (arg->name && strcmp(arg->name, f->name) == 0) {
                found = true;
                break;
            }
        }

        if (!found) {
            char* name_copy = strdup(f->name);
            vec_push(&missing, name_copy);
        }
    }

    if (missing.length == 0) return NULL;

    // Build error message
    size_t msg_len = 64 + missing.length * 64;
    for (size_t i = 0; i < missing.length; i++) msg_len += strlen(missing.data[i]);
    char* msg = get_memory(msg_len);
    if (msg == NULL) {
        for (size_t i = 0; i < missing.length; i++) free(missing.data[i]);
        return make_error_result("Out of memory", NULL);
    }

    strcpy(msg, "Missing required fields: ");
    size_t pos = strlen(msg);
    for (size_t i = 0; i < missing.length; i++) {
        if (i > 0) {
            msg[pos++] = ',';
            msg[pos++] = ' ';
        }
        size_t name_len = strlen(missing.data[i]);
        memcpy(msg + pos, missing.data[i], name_len);
        pos += name_len;
    }
    msg[pos] = '\0';

    for (size_t i = 0; i < missing.length; i++) free(missing.data[i]);
    vec_deinit(&missing);

    graphql_result_t* result = make_error_result(msg, NULL);
    free(msg);
    return result;
}

static graphql_result_t* graphql_mutate_impl(graphql_layer_t* layer, const char* mutation) {
    if (layer == NULL || mutation == NULL) {
        return make_error_result("Invalid arguments", NULL);
    }

    // Parse the mutation AST
    graphql_ast_node_t* ast = graphql_parse(mutation, strlen(mutation));
    if (ast == NULL) {
        return make_error_result("Failed to parse mutation", NULL);
    }

    // Find the operation
    graphql_ast_node_t* op = NULL;
    for (int i = 0; i < ast->children.length; i++) {
        if (ast->children.data[i]->kind == GRAPHQL_AST_OPERATION) {
            op = ast->children.data[i];
            break;
        }
    }

    if (op == NULL) {
        graphql_ast_destroy(ast);
        return make_error_result("No mutation operation found", NULL);
    }

    graphql_result_t* result = get_clear_memory(sizeof(graphql_result_t));
    if (result == NULL) {
        graphql_ast_destroy(ast);
        return make_error_result("Out of memory", NULL);
    }

    result->data = graphql_result_node_create(RESULT_OBJECT, "mutation");
    result->success = true;

    // Execute each mutation field
    for (int i = 0; i < op->children.length; i++) {
        graphql_ast_node_t* field = op->children.data[i];
        if (field->kind != GRAPHQL_AST_FIELD) continue;

        const char* field_name = field->name;
        bool is_create = (strncmp(field_name, "create", 6) == 0);
        bool is_update = (strncmp(field_name, "update", 6) == 0);
        bool is_delete = (strncmp(field_name, "delete", 6) == 0);

        // Determine the type name from the field name
        // e.g., "createUser" -> "User"
        const char* type_name_start = field_name + 6;  // Skip "create"/"update"/"delete"
        char type_name[256];
        if (strlen(type_name_start) > 0) {
            // Capitalize first letter
            type_name[0] = type_name_start[0];  // Keep original case for now
            strncpy(type_name + 1, type_name_start + 1, sizeof(type_name) - 2);
            type_name[sizeof(type_name) - 1] = '\0';
        } else {
            strncpy(type_name, field_name, sizeof(type_name) - 1);
            type_name[sizeof(type_name) - 1] = '\0';
        }

        // Look up the type
        graphql_type_t* type = graphql_schema_get_type(layer, type_name);
        const char* plural = type ? graphql_type_get_plural(type) : type_name;

        if (is_create) {
            // Validate required fields for create mutations
            graphql_result_t* validation_error = validate_required_fields(type, field);
            if (validation_error != NULL) {
                // Copy error messages to the mutation result
                for (size_t ei = 0; ei < validation_error->errors.length; ei++) {
                    graphql_error_t src = validation_error->errors.data[ei];
                    graphql_error_t new_err = graphql_error_create(src.message, src.path);
                    vec_push(&result->errors, new_err);
                }
                result->success = false;
                graphql_result_destroy(validation_error);
                continue;
            }
            // Generate next ID
            char path_buf[512];
            snprintf(path_buf, sizeof(path_buf), "%s/__meta/next_id", plural);
            char* next_id_str = local_db_get_string(layer->db, path_buf);
            long long next_id = 1;
            if (next_id_str != NULL) {
                next_id = strtoll(next_id_str, NULL, 10);
                free(next_id_str);
            }

            // Write fields from arguments
            graphql_result_node_t* create_result = graphql_result_node_create(RESULT_OBJECT, field_name);
            if (create_result != NULL) {
                char id_str[32];
                snprintf(id_str, sizeof(id_str), "%lld", (long long)next_id);
                graphql_result_node_t* id_node = graphql_result_node_create(RESULT_STRING, "id");
                if (id_node != NULL) {
                    id_node->string_val = strdup(id_str);
                    vec_push(&create_result->children, id_node);
                }

                // Write each argument as a field
                for (int j = 0; j < field->arguments.length; j++) {
                    graphql_ast_node_t* arg = field->arguments.data[j];
                    if (arg->name && arg->literal) {
                        char field_path[512];
                        snprintf(field_path, sizeof(field_path), "%s/%s/%s", plural, id_str, arg->name);

                        // Convert literal to string
                        char* value_str = NULL;
                        switch (arg->literal->kind) {
                            case GRAPHQL_LITERAL_STRING:
                                value_str = arg->literal->string_val ? strdup(arg->literal->string_val) : strdup("");
                                break;
                            case GRAPHQL_LITERAL_INT: {
                                char buf[32];
                                snprintf(buf, sizeof(buf), "%lld", (long long)arg->literal->int_val);
                                value_str = strdup(buf);
                                break;
                            }
                            case GRAPHQL_LITERAL_FLOAT: {
                                char buf[64];
                                snprintf(buf, sizeof(buf), "%g", arg->literal->float_val);
                                value_str = strdup(buf);
                                break;
                            }
                            case GRAPHQL_LITERAL_BOOL:
                                value_str = strdup(arg->literal->bool_val ? "true" : "false");
                                break;
                            default:
                                value_str = strdup("");
                                break;
                        }

                        if (value_str != NULL) {
                            local_db_put_string(layer->db, field_path, value_str);

                            // Add to result
                            graphql_result_node_t* field_node = graphql_result_node_create(RESULT_STRING, arg->name);
                            if (field_node != NULL) {
                                field_node->string_val = value_str;
                                vec_push(&create_result->children, field_node);
                            } else {
                                free(value_str);
                            }
                        }
                    }
                }

                // Increment next_id
                char new_id[32];
                snprintf(new_id, sizeof(new_id), "%lld", (long long)(next_id + 1));
                local_db_put_string(layer->db, path_buf, new_id);

                vec_push(&result->data->children, create_result);
            }
        } else if (is_update) {
            // Update: find id argument and update fields
            char* target_id = NULL;
            for (int j = 0; j < field->arguments.length; j++) {
                graphql_ast_node_t* arg = field->arguments.data[j];
                if (arg->name && strcmp(arg->name, "id") == 0 && arg->literal) {
                    if (arg->literal->kind == GRAPHQL_LITERAL_STRING && arg->literal->string_val) {
                        target_id = strdup(arg->literal->string_val);
                    } else if (arg->literal->kind == GRAPHQL_LITERAL_INT) {
                        char buf[32];
                        snprintf(buf, sizeof(buf), "%lld", (long long)arg->literal->int_val);
                        target_id = strdup(buf);
                    }
                    break;
                }
            }

            if (target_id != NULL) {
                graphql_result_node_t* update_result = graphql_result_node_create(RESULT_OBJECT, field_name);
                if (update_result != NULL) {
                    // Add id to result
                    graphql_result_node_t* id_node = graphql_result_node_create(RESULT_STRING, "id");
                    if (id_node != NULL) {
                        id_node->string_val = target_id;
                        vec_push(&update_result->children, id_node);
                    }

                    for (int j = 0; j < field->arguments.length; j++) {
                        graphql_ast_node_t* arg = field->arguments.data[j];
                        if (arg->name && strcmp(arg->name, "id") != 0 && arg->literal) {
                            char field_path[512];
                            snprintf(field_path, sizeof(field_path), "%s/%s/%s", plural, target_id, arg->name);

                            char* value_str = NULL;
                            switch (arg->literal->kind) {
                                case GRAPHQL_LITERAL_STRING:
                                    value_str = arg->literal->string_val ? strdup(arg->literal->string_val) : strdup("");
                                    break;
                                case GRAPHQL_LITERAL_INT: {
                                    char buf[32];
                                    snprintf(buf, sizeof(buf), "%lld", (long long)arg->literal->int_val);
                                    value_str = strdup(buf);
                                    break;
                                }
                                default:
                                    value_str = strdup("");
                                    break;
                            }

                            if (value_str != NULL) {
                                local_db_put_string(layer->db, field_path, value_str);
                                graphql_result_node_t* f_node = graphql_result_node_create(RESULT_STRING, arg->name);
                                if (f_node != NULL) {
                                    f_node->string_val = value_str;
                                    vec_push(&update_result->children, f_node);
                                } else {
                                    free(value_str);
                                }
                            }
                        }
                    }
                    vec_push(&result->data->children, update_result);
                }
                free(target_id);
            }
        } else if (is_delete) {
            // Delete: find id argument
            char* target_id = NULL;
            for (int j = 0; j < field->arguments.length; j++) {
                graphql_ast_node_t* arg = field->arguments.data[j];
                if (arg->name && strcmp(arg->name, "id") == 0 && arg->literal) {
                    if (arg->literal->kind == GRAPHQL_LITERAL_STRING && arg->literal->string_val) {
                        target_id = strdup(arg->literal->string_val);
                    } else if (arg->literal->kind == GRAPHQL_LITERAL_INT) {
                        char buf[32];
                        snprintf(buf, sizeof(buf), "%lld", (long long)arg->literal->int_val);
                        target_id = strdup(buf);
                    }
                    break;
                }
            }

            if (target_id != NULL) {
                // Scan and delete all fields under type/id
                char prefix[256];
                snprintf(prefix, sizeof(prefix), "%s/%s", plural, target_id);

                path_t* start = path_create();
                if (start != NULL) {
                    const char* s = prefix;
                    while (*s) {
                        const char* e = strchr(s, '/');
                        size_t len = e ? (size_t)(e - s) : strlen(s);
                        if (len > 0) {
                            buffer_t* buf = buffer_create(len);
                            if (buf != NULL) {
                                memcpy(buf->data, s, len);
                                buf->size = len;
                                identifier_t* id = identifier_create(buf, 0);
                                buffer_destroy(buf);
                                if (id != NULL) {
                                    path_append(start, id);
                                    identifier_destroy(id);
                                }
                            }
                        }
                        if (e) { s = e + 1; } else { break; }
                    }

                    database_iterator_t* iter = database_scan_start(layer->db, start, NULL);
                    path_destroy(start);

                    if (iter != NULL) {
                        path_t* out_path = NULL;
                        identifier_t* out_value = NULL;
                        while (database_scan_next(iter, &out_path, &out_value) == 0) {
                            if (out_path == NULL) break;
                            database_delete_sync(layer->db, out_path);
                            // out_path consumed by delete
                            if (out_value) identifier_destroy(out_value);
                            out_path = NULL;
                            out_value = NULL;
                        }
                        database_scan_end(iter);
                    }
                }

                graphql_result_node_t* del_result = graphql_result_node_create(RESULT_OBJECT, field_name);
                if (del_result != NULL) {
                    graphql_result_node_t* id_node = graphql_result_node_create(RESULT_STRING, "id");
                    if (id_node != NULL) {
                        id_node->string_val = target_id;
                        vec_push(&del_result->children, id_node);
                    }
                    vec_push(&result->data->children, del_result);
                } else {
                    free(target_id);
                }
            }
        }
    }

    graphql_ast_destroy(ast);
    return result;
}

// ============================================================
// Public API - Synchronous
// ============================================================

graphql_result_t* graphql_query_sync(graphql_layer_t* layer, const char* query) {
    return graphql_query_impl(layer, query);
}

graphql_result_t* graphql_mutate_sync(graphql_layer_t* layer, const char* mutation) {
    return graphql_mutate_impl(layer, mutation);
}

// ============================================================
// Public API - Asynchronous (worker pool dispatch)
// ============================================================

typedef struct {
    graphql_layer_t* layer;
    char* query;
    promise_t* promise;
    void* user_data;
} graphql_query_ctx_t;

typedef struct {
    graphql_layer_t* layer;
    char* mutation;
    promise_t* promise;
    void* user_data;
} graphql_mutate_ctx_t;

static void graphql_query_execute(void* arg) {
    graphql_query_ctx_t* ctx = (graphql_query_ctx_t*)arg;
    graphql_result_t* result = graphql_query_impl(ctx->layer, ctx->query);
    promise_resolve(ctx->promise, result);
    free(ctx->query);
    refcounter_dereference((refcounter_t*)ctx->layer);
    refcounter_dereference((refcounter_t*)ctx->promise);
    free(ctx);
}

static void graphql_query_abort(void* arg) {
    graphql_query_ctx_t* ctx = (graphql_query_ctx_t*)arg;
    async_error_t* error = ERROR("Query aborted due to shutdown");
    promise_reject(ctx->promise, error);
    free(ctx->query);
    refcounter_dereference((refcounter_t*)ctx->layer);
    refcounter_dereference((refcounter_t*)ctx->promise);
    free(ctx);
}

static void graphql_mutate_execute(void* arg) {
    graphql_mutate_ctx_t* ctx = (graphql_mutate_ctx_t*)arg;
    graphql_result_t* result = graphql_mutate_impl(ctx->layer, ctx->mutation);
    promise_resolve(ctx->promise, result);
    free(ctx->mutation);
    refcounter_dereference((refcounter_t*)ctx->layer);
    refcounter_dereference((refcounter_t*)ctx->promise);
    free(ctx);
}

static void graphql_mutate_abort(void* arg) {
    graphql_mutate_ctx_t* ctx = (graphql_mutate_ctx_t*)arg;
    async_error_t* error = ERROR("Mutation aborted due to shutdown");
    promise_reject(ctx->promise, error);
    free(ctx->mutation);
    refcounter_dereference((refcounter_t*)ctx->layer);
    refcounter_dereference((refcounter_t*)ctx->promise);
    free(ctx);
}

void graphql_query(graphql_layer_t* layer, const char* query,
                   promise_t* promise, void* user_data) {
    if (layer == NULL || query == NULL || promise == NULL) {
        if (promise != NULL) {
            promise_resolve(promise, make_error_result("Invalid arguments", NULL));
        }
        return;
    }

    // No pool — execute synchronously and resolve inline
    if (layer->pool == NULL) {
        graphql_result_t* result = graphql_query_impl(layer, query);
        promise_resolve(promise, result);
        return;
    }

    // Dispatch to worker pool
    graphql_query_ctx_t* ctx = get_clear_memory(sizeof(graphql_query_ctx_t));
    if (ctx == NULL) {
        promise_resolve(promise, make_error_result("Out of memory", NULL));
        return;
    }

    ctx->layer = (graphql_layer_t*)refcounter_reference((refcounter_t*)layer);
    ctx->query = strdup(query);
    ctx->promise = (promise_t*)refcounter_reference((refcounter_t*)promise);
    ctx->user_data = user_data;

    if (ctx->query == NULL) {
        refcounter_dereference((refcounter_t*)ctx->layer);
        refcounter_dereference((refcounter_t*)ctx->promise);
        free(ctx);
        promise_resolve(promise, make_error_result("Out of memory", NULL));
        return;
    }

    work_t* work = work_create(graphql_query_execute, graphql_query_abort, ctx);
    if (work == NULL) {
        free(ctx->query);
        refcounter_dereference((refcounter_t*)ctx->layer);
        refcounter_dereference((refcounter_t*)ctx->promise);
        free(ctx);
        promise_resolve(promise, make_error_result("Out of memory", NULL));
        return;
    }

    refcounter_yield((refcounter_t*)work);
    work_pool_enqueue(layer->pool, work);
}

void graphql_mutate(graphql_layer_t* layer, const char* mutation,
                    promise_t* promise, void* user_data) {
    if (layer == NULL || mutation == NULL || promise == NULL) {
        if (promise != NULL) {
            promise_resolve(promise, make_error_result("Invalid arguments", NULL));
        }
        return;
    }

    // No pool — execute synchronously and resolve inline
    if (layer->pool == NULL) {
        graphql_result_t* result = graphql_mutate_impl(layer, mutation);
        promise_resolve(promise, result);
        return;
    }

    // Dispatch to worker pool
    graphql_mutate_ctx_t* ctx = get_clear_memory(sizeof(graphql_mutate_ctx_t));
    if (ctx == NULL) {
        promise_resolve(promise, make_error_result("Out of memory", NULL));
        return;
    }

    ctx->layer = (graphql_layer_t*)refcounter_reference((refcounter_t*)layer);
    ctx->mutation = strdup(mutation);
    ctx->promise = (promise_t*)refcounter_reference((refcounter_t*)promise);
    ctx->user_data = user_data;

    if (ctx->mutation == NULL) {
        refcounter_dereference((refcounter_t*)ctx->layer);
        refcounter_dereference((refcounter_t*)ctx->promise);
        free(ctx);
        promise_resolve(promise, make_error_result("Out of memory", NULL));
        return;
    }

    work_t* work = work_create(graphql_mutate_execute, graphql_mutate_abort, ctx);
    if (work == NULL) {
        free(ctx->mutation);
        refcounter_dereference((refcounter_t*)ctx->layer);
        refcounter_dereference((refcounter_t*)ctx->promise);
        free(ctx);
        promise_resolve(promise, make_error_result("Out of memory", NULL));
        return;
    }

    refcounter_yield((refcounter_t*)work);
    work_pool_enqueue(layer->pool, work);
}