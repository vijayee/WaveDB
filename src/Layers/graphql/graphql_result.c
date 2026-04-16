//
// GraphQL Result - Result construction and JSON serialization
// Created: 2026-04-12
//

#include "graphql_result.h"
#include "graphql_types.h"
#include "../../Util/allocator.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// ============================================================
// Dynamic string builder for JSON output
// ============================================================

typedef struct {
    char* data;
    size_t length;
    size_t capacity;
} json_builder_t;

static void json_builder_init(json_builder_t* b) {
    b->capacity = GRAPHQL_BUF_SIZE;
    b->data = malloc(b->capacity);
    b->length = 0;
    if (b->data) b->data[0] = '\0';
}

static void json_builder_ensure(json_builder_t* b, size_t extra) {
    if (b->length + extra + 1 >= b->capacity) {
        while (b->length + extra + 1 >= b->capacity) {
            b->capacity *= 2;
        }
        b->data = realloc(b->data, b->capacity);
    }
}

static void json_builder_append(json_builder_t* b, const char* str) {
    size_t len = strlen(str);
    json_builder_ensure(b, len);
    memcpy(b->data + b->length, str, len + 1);
    b->length += len;
}

static void json_builder_append_char(json_builder_t* b, char c) {
    json_builder_ensure(b, 1);
    b->data[b->length++] = c;
    b->data[b->length] = '\0';
}

static void json_builder_append_escaped(json_builder_t* b, const char* str) {
    while (*str) {
        switch (*str) {
            case '"':  json_builder_append(b, "\\\""); break;
            case '\\': json_builder_append(b, "\\\\"); break;
            case '\n': json_builder_append(b, "\\n"); break;
            case '\r': json_builder_append(b, "\\r"); break;
            case '\t': json_builder_append(b, "\\t"); break;
            default:
                if ((unsigned char)*str < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", (unsigned char)*str);
                    json_builder_append(b, buf);
                } else {
                    json_builder_append_char(b, *str);
                }
                break;
        }
        str++;
    }
}

// ============================================================
// JSON serialization helpers
// ============================================================

static void result_node_to_json(graphql_result_node_t* node, json_builder_t* b);

static void result_object_to_json(graphql_result_node_t* node, json_builder_t* b) {
    json_builder_append_char(b, '{');
    for (int i = 0; i < node->children.length; i++) {
        if (i > 0) json_builder_append(b, ",");
        graphql_result_node_t* child = node->children.data[i];
        json_builder_append_char(b, '"');
        if (child->name) {
            json_builder_append_escaped(b, child->name);
        }
        json_builder_append(b, "\":");
        result_node_to_json(child, b);
    }
    json_builder_append_char(b, '}');
}

static void result_list_to_json(graphql_result_node_t* node, json_builder_t* b) {
    json_builder_append_char(b, '[');
    for (int i = 0; i < node->children.length; i++) {
        if (i > 0) json_builder_append(b, ",");
        result_node_to_json(node->children.data[i], b);
    }
    json_builder_append_char(b, ']');
}

static void result_node_to_json(graphql_result_node_t* node, json_builder_t* b) {
    if (node == NULL) {
        json_builder_append(b, "null");
        return;
    }

    switch (node->kind) {
        case RESULT_NULL:
            json_builder_append(b, "null");
            break;
        case RESULT_STRING:
            json_builder_append_char(b, '"');
            if (node->string_val) {
                json_builder_append_escaped(b, node->string_val);
            }
            json_builder_append_char(b, '"');
            break;
        case RESULT_INT: {
            char buf[32];
            snprintf(buf, sizeof(buf), "%lld", (long long)node->int_val);
            json_builder_append(b, buf);
            break;
        }
        case RESULT_FLOAT: {
            char buf[64];
            snprintf(buf, sizeof(buf), "%g", node->float_val);
            json_builder_append(b, buf);
            break;
        }
        case RESULT_BOOL:
            json_builder_append(b, node->bool_val ? "true" : "false");
            break;
        case RESULT_ID:
            json_builder_append_char(b, '"');
            if (node->id_val) {
                json_builder_append_escaped(b, node->id_val);
            }
            json_builder_append_char(b, '"');
            break;
        case RESULT_OBJECT:
            result_object_to_json(node, b);
            break;
        case RESULT_LIST:
            result_list_to_json(node, b);
            break;
    }
}

// ============================================================
// Public API
// ============================================================

const char* graphql_result_to_json(graphql_result_t* result) {
    if (result == NULL) return strdup("{\"data\":null}");

    json_builder_t b;
    json_builder_init(&b);
    if (b.data == NULL) return strdup("{\"data\":null}");

    json_builder_append(&b, "{\"data\":");

    if (result->data != NULL) {
        result_node_to_json(result->data, &b);
    } else {
        json_builder_append(&b, "null");
    }

    // Add errors if present
    if (result->errors.length > 0) {
        json_builder_append(&b, ",\"errors\":[");
        for (int i = 0; i < result->errors.length; i++) {
            if (i > 0) json_builder_append(&b, ",");
            graphql_error_t* err = &result->errors.data[i];
            json_builder_append(&b, "{\"message\":");
            json_builder_append_char(&b, '"');
            if (err->message) {
                json_builder_append_escaped(&b, err->message);
            }
            json_builder_append_char(&b, '"');
            if (err->path) {
                json_builder_append(&b, ",\"path\":");
                json_builder_append_char(&b, '"');
                json_builder_append_escaped(&b, err->path);
                json_builder_append_char(&b, '"');
            }
            if (err->locations.length > 0) {
                json_builder_append(&b, ",\"locations\":[");
                for (int li = 0; li < err->locations.length; li++) {
                    if (li > 0) json_builder_append(&b, ",");
                    json_builder_append(&b, "{\"line\":");
                    char loc_buf[32];
                    snprintf(loc_buf, sizeof(loc_buf), "%zu", err->locations.data[li].line);
                    json_builder_append(&b, loc_buf);
                    json_builder_append(&b, ",\"column\":");
                    snprintf(loc_buf, sizeof(loc_buf), "%zu", err->locations.data[li].column);
                    json_builder_append(&b, loc_buf);
                    json_builder_append(&b, "}");
                }
                json_builder_append(&b, "]");
            }
            json_builder_append(&b, "}");
        }
        json_builder_append(&b, "]");
    }

    json_builder_append(&b, "}");
    return b.data;
}