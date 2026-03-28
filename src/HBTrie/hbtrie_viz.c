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
    char* escaped = malloc(len * 2 + 1);  // Worst case: every char needs escaping
    if (escaped == NULL) return NULL;

    size_t j = 0;
    for (size_t i = 0; i < len; i++) {
        char c = str[i];
        if (c == '"' || c == '\\') {
            escaped[j++] = '\\';
        }
        escaped[j++] = c;
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