//
// Created by victor on 3/27/26.
//

#include "hbtrie_viz.h"
#include "bnode.h"
#include "chunk.h"
#include "identifier.h"
#include "../Util/log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdatomic.h>

// Static counter for unique node IDs
static atomic_size_t g_node_id_counter = ATOMIC_VAR_INIT(0);

// Forward declarations
static int serialize_hbtrie_node(hbtrie_node_t* node, FILE* fp, uint8_t chunk_size, size_t* node_count, int depth);
static int serialize_bnode(bnode_t* bnode, FILE* fp, uint8_t chunk_size, size_t* node_count, int depth);

// Helper: Convert binary data to hex string
// Returns newly allocated string, caller must free
static char* bytes_to_hex(const uint8_t* data, size_t len) {
    if (data == NULL || len == 0) return strdup("");

    char* hex = malloc(len * 2 + 1);
    if (hex == NULL) return NULL;

    for (size_t i = 0; i < len; i++) {
        sprintf(hex + i * 2, "%02x", data[i]);
    }
    hex[len * 2] = '\0';
    return hex;
}

// Helper: Convert chunk to hex string
static char* chunk_to_hex(chunk_t* chunk) {
    if (chunk == NULL) return strdup("");
    return bytes_to_hex(chunk_data_const(chunk), chunk->data->size);
}

// Helper: Convert identifier to hex string (all chunks concatenated)
static char* identifier_to_hex(identifier_t* id) {
    if (id == NULL) return strdup("");

    // Calculate total length
    size_t total_len = 0;
    for (size_t i = 0; i < vec_length(&id->chunks); i++) {
        chunk_t* chunk = vec_at(&id->chunks, i);
        total_len += chunk->data->size;
    }

    // Allocate buffer for all data
    uint8_t* buffer = malloc(total_len);
    if (buffer == NULL && total_len > 0) return NULL;

    // Copy chunk data
    size_t offset = 0;
    for (size_t i = 0; i < vec_length(&id->chunks); i++) {
        chunk_t* chunk = vec_at(&id->chunks, i);
        memcpy(buffer + offset, chunk_data_const(chunk), chunk->data->size);
        offset += chunk->data->size;
    }

    char* hex = bytes_to_hex(buffer, total_len);
    free(buffer);
    return hex;
}

// Helper: Check if data is printable ASCII
static int is_printable_ascii(const uint8_t* data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (!isprint(data[i]) && data[i] != '\t' && data[i] != '\n' && data[i] != '\r') {
            return 0;
        }
    }
    return 1;
}

// Helper: Escape string for JSON
static char* escape_json_string(const char* str) {
    if (str == NULL) return strdup("");

    size_t len = strlen(str);
    char* escaped = malloc(len * 6 + 1);  // Worst case: \uXXXX for control chars
    if (escaped == NULL) return NULL;

    size_t j = 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = str[i];
        if (c == '"' || c == '\\') {
            escaped[j++] = '\\';
            escaped[j++] = c;
        } else if (c == '\n') {
            escaped[j++] = '\\';
            escaped[j++] = 'n';
        } else if (c == '\r') {
            escaped[j++] = '\\';
            escaped[j++] = 'r';
        } else if (c == '\t') {
            escaped[j++] = '\\';
            escaped[j++] = 't';
        } else if (c == '\b') {
            escaped[j++] = '\\';
            escaped[j++] = 'b';
        } else if (c == '\f') {
            escaped[j++] = '\\';
            escaped[j++] = 'f';
        } else if (c < 0x20) {
            // Control characters: \uXXXX
            j += sprintf(escaped + j, "\\u%04x", c);
        } else {
            escaped[j++] = c;
        }
    }
    escaped[j] = '\0';
    return escaped;
}

// Helper: Write JSON string to buffer
static int write_json_string(FILE* fp, const char* str) {
    char* escaped = escape_json_string(str);
    if (escaped == NULL) return -1;

    fprintf(fp, "\"%s\"", escaped);
    free(escaped);
    return 0;
}

// Serialize HBTrie node to JSON
static int serialize_hbtrie_node(hbtrie_node_t* node, FILE* fp, uint8_t chunk_size, size_t* node_count, int depth) {
    if (depth > 1000) {
        log_error("Recursion depth exceeded in HBTrie serialization");
        return -1;
    }

    if (node == NULL) {
        fprintf(fp, "null");
        return 0;
    }

    (*node_count)++;

    fprintf(fp, "{\n");
    fprintf(fp, "      \"id\": \"node_%zu\",\n", atomic_fetch_add(&g_node_id_counter, 1));

    // Serialize B+tree
    fprintf(fp, "      \"entries\": ");
    if (serialize_bnode(node->btree, fp, chunk_size, node_count, depth + 1) != 0) {
        return -1;
    }

    // Stats
    fprintf(fp, ",\n");
    fprintf(fp, "      \"stats\": {\n");
    fprintf(fp, "        \"entry_count\": %zu,\n", bnode_count(node->btree));
    fprintf(fp, "        \"node_size_bytes\": %zu\n", bnode_size(node->btree, chunk_size));
    fprintf(fp, "      }\n");
    fprintf(fp, "    }");

    return 0;
}

// Serialize B+tree node to JSON
static int serialize_bnode(bnode_t* bnode, FILE* fp, uint8_t chunk_size, size_t* node_count, int depth) {
    if (bnode == NULL || bnode_is_empty(bnode)) {
        fprintf(fp, "[]");
        return 0;
    }

    fprintf(fp, "[\n");

    size_t count = bnode_count(bnode);
    for (size_t i = 0; i < count; i++) {
        bnode_entry_t* entry = bnode_get(bnode, i);
        if (entry == NULL) continue;

        fprintf(fp, "      {\n");

        // Key (chunk) as hex
        char* key_hex = chunk_to_hex(entry->key);
        fprintf(fp, "        \"key_hex\": \"%s\",\n", key_hex ? key_hex : "");
        free(key_hex);

        // has_value flag
        fprintf(fp, "        \"has_value\": %s,\n", entry->has_value ? "true" : "false");

        if (entry->has_value) {
            // Value (leaf)
            if (entry->has_versions) {
                // For now, serialize only the latest version
                version_entry_t* latest = entry->versions;
                if (latest && latest->value) {
                    char* value_hex = identifier_to_hex(latest->value);
                    fprintf(fp, "        \"value\": {\n");
                    fprintf(fp, "          \"length\": %zu,\n", latest->value->length);
                    fprintf(fp, "          \"data_hex\": \"%s\"\n", value_hex ? value_hex : "");
                    fprintf(fp, "        }\n");
                    free(value_hex);
                } else {
                    fprintf(fp, "        \"value\": null\n");
                }
            } else {
                // Legacy single value
                if (entry->value) {
                    char* value_hex = identifier_to_hex(entry->value);
                    fprintf(fp, "        \"value\": {\n");
                    fprintf(fp, "          \"length\": %zu,\n", entry->value->length);
                    fprintf(fp, "          \"data_hex\": \"%s\"\n", value_hex ? value_hex : "");
                    fprintf(fp, "        }\n");
                    free(value_hex);
                } else {
                    fprintf(fp, "        \"value\": null\n");
                }
            }
        } else {
            // Child node
            fprintf(fp, "        \"child\": ");
            if (serialize_hbtrie_node(entry->child, fp, chunk_size, node_count, depth + 1) != 0) {
                return -1;
            }
            fprintf(fp, "\n");
        }

        fprintf(fp, "      }%s\n", (i < count - 1) ? "," : "");
    }

    fprintf(fp, "    ]");
    return 0;
}