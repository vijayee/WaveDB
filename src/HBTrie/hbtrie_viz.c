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
#include <unistd.h>  // for close(), unlink()

// D3.js will be embedded as base64 in HTML output
// Size: ~274KB minified, ~365KB base64 encoded
// Generated with: curl -s https://d3js.org/d3.v7.min.js | base64 -w 0 > /tmp/d3.min.js.base64
static const char* D3_JS_BASE64 = "";  // Placeholder - will be loaded from file at runtime

// HTML template for D3.js visualization
// %s placeholders: 1) D3 base64, 2) JSON data
static const char* HTML_TEMPLATE =
"<!DOCTYPE html>\n"
"<html>\n"
"<head>\n"
"  <meta charset=\"UTF-8\">\n"
"  <title>HBTrie Visualizer</title>\n"
"  <script src=\"data:text/javascript;base64,%s\"></script>\n"
"  <style>\n"
"    body { font-family: Arial, sans-serif; margin: 0; padding: 0; }\n"
"    #controls { padding: 10px; background: #f5f5f5; border-bottom: 1px solid #ccc; }\n"
"    #controls input { padding: 5px; margin-right: 10px; }\n"
"    #controls button { padding: 5px 10px; margin-right: 5px; cursor: pointer; }\n"
"    #breadcrumbs { padding: 10px; background: #e8e8e8; }\n"
"    #breadcrumbs span { cursor: pointer; color: #0066cc; }\n"
"    #breadcrumbs span:hover { text-decoration: underline; }\n"
"    #tree { width: 100%%; height: 800px; }\n"
"    .node circle { fill: #fff; stroke: #4682b4; stroke-width: 2px; cursor: pointer; }\n"
"    .node.leaf rect { fill: #90EE90; stroke: #228B22; stroke-width: 2px; }\n"
"    .node text { font-size: 12px; font-family: monospace; }\n"
"    .link { fill: none; stroke: #ccc; stroke-width: 1px; }\n"
"    .tooltip { position: absolute; background: white; border: 1px solid black; padding: 5px; font-size: 12px; }\n"
"  </style>\n"
"</head>\n"
"<body>\n"
"  <div id=\"controls\">\n"
"    <input type=\"text\" id=\"search\" placeholder=\"Search path (hex)...\">\n"
"    <button id=\"expand-all\">Expand All</button>\n"
"    <button id=\"collapse-all\">Collapse All</button>\n"
"    <button id=\"zoom-in\">Zoom In</button>\n"
"    <button id=\"zoom-out\">Zoom Out</button>\n"
"    <button id=\"zoom-reset\">Fit to View</button>\n"
"  </div>\n"
"  <div id=\"breadcrumbs\"></div>\n"
"  <svg id=\"tree\"></svg>\n"
"  <script>\n"
"    const TRIE_DATA = %s;\n"
"\n"
"    // D3 visualization code\n"
"    const width = window.innerWidth - 40;\n"
"    const height = 800;\n"
"    const margin = { top: 40, right: 120, bottom: 40, left: 120 };\n"
"\n"
"    let svg = d3.select(\"#tree\")\n"
"      .attr(\"width\", width)\n"
"      .attr(\"height\", height);\n"
"\n"
"    let g = svg.append(\"g\")\n"
"      .attr(\"transform\", `translate(${margin.left},${margin.top})`);\n"
"\n"
"    // Zoom behavior\n"
"    const zoom = d3.zoom()\n"
"      .scaleExtent([0.1, 4])\n"
"      .on(\"zoom\", (event) => {\n"
"        g.attr(\"transform\", event.transform);\n"
"      });\n"
"    svg.call(zoom);\n"
"\n"
"    // Convert trie data to D3 hierarchy\n"
"    function buildHierarchy(node) {\n"
"      if (!node) return null;\n"
"      \n"
"      let children = [];\n"
"      if (node.entries) {\n"
"        node.entries.forEach(entry => {\n"
"          if (!entry.has_value && entry.child) {\n"
"            children.push(buildHierarchy(entry.child));\n"
"          }\n"
"        });\n"
"      }\n"
"\n"
"      return {\n"
"        name: node.id,\n"
"        data: node,\n"
"        children: children.length > 0 ? children : undefined\n"
"      };\n"
"    }\n"
"\n"
"    let root = d3.hierarchy(buildHierarchy(TRIE_DATA.root));\n"
"    let treeLayout = d3.tree().size([height - margin.top - margin.bottom, width - margin.left - margin.right]);\n"
"    treeLayout(root);\n"
"\n"
"    // Draw links\n"
"    g.selectAll(\".link\")\n"
"      .data(root.links())\n"
"      .enter()\n"
"      .append(\"path\")\n"
"      .attr(\"class\", \"link\")\n"
"      .attr(\"d\", d3.linkHorizontal()\n"
"        .x(d => d.y)\n"
"        .y(d => d.x));\n"
"\n"
"    // Draw nodes\n"
"    let nodes = g.selectAll(\".node\")\n"
"      .data(root.descendants())\n"
"      .enter()\n"
"      .append(\"g\")\n"
"      .attr(\"class\", \"node\")\n"
"      .attr(\"transform\", d => `translate(${d.y},${d.x})`)\n"
"      .on(\"click\", (event, d) => {\n"
"        // Toggle collapse/expand\n"
"        if (d.children) {\n"
"          d._children = d.children;\n"
"          d.children = null;\n"
"        } else if (d._children) {\n"
"          d.children = d._children;\n"
"          d._children = null;\n"
"        }\n"
"        update();\n"
"      });\n"
"\n"
"    // Draw circles for internal nodes, rects for leaves\n"
"    nodes.each(function(d) {\n"
"      let node = d3.select(this);\n"
"      let data = d.data.data;\n"
"      \n"
"      if (d.children === undefined && d._children === undefined) {\n"
"        // Leaf node\n"
"        node.append(\"rect\")\n"
"          .attr(\"width\", 20)\n"
"          .attr(\"height\", 20)\n"
"          .attr(\"x\", -10)\n"
"          .attr(\"y\", -10)\n"
"          .attr(\"class\", \"leaf\");\n"
"      } else {\n"
"        // Internal node\n"
"        node.append(\"circle\")\n"
"          .attr(\"r\", 10);\n"
"      }\n"
"    });\n"
"\n"
"    // Add labels\n"
"    nodes.append(\"text\")\n"
"      .attr(\"dy\", 3)\n"
"      .attr(\"x\", d => d.children || d._children ? -15 : 25)\n"
"      .style(\"text-anchor\", d => d.children || d._children ? \"end\" : \"start\")\n"
"      .text(d => {\n"
"        let data = d.data.data;\n"
"        if (data.entries && data.entries.length > 0) {\n"
"          return data.entries[0].key_hex.substring(0, 8);\n"
"        }\n"
"        return d.data.name;\n"
"      });\n"
"\n"
"    function update() {\n"
"      // Re-render tree after collapse/expand\n"
"      // Simplified for MVP - full re-render\n"
"      location.reload();\n"
"    }\n"
"\n"
"    // Zoom controls\n"
"    d3.select(\"#zoom-in\").on(\"click\", () => {\n"
"      svg.transition().call(zoom.scaleBy, 1.3);\n"
"    });\n"
"\n"
"    d3.select(\"#zoom-out\").on(\"click\", () => {\n"
"      svg.transition().call(zoom.scaleBy, 0.7);\n"
"    });\n"
"\n"
"    d3.select(\"#zoom-reset\").on(\"click\", () => {\n"
"      svg.transition().call(zoom.transform, d3.zoomIdentity);\n"
"    });\n"
"\n"
"    // Initial fit\n"
"    svg.call(zoom.transform, d3.zoomIdentity);\n"
"  </script>\n"
"</body>\n"
"</html>";

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
    for (int i = 0; i < id->chunks.length; i++) {
        chunk_t* chunk = id->chunks.data[i];
        total_len += chunk->data->size;
    }

    // Allocate buffer for all data
    uint8_t* buffer = malloc(total_len);
    if (buffer == NULL && total_len > 0) return NULL;

    // Copy chunk data
    size_t offset = 0;
    for (int i = 0; i < id->chunks.length; i++) {
        chunk_t* chunk = id->chunks.data[i];
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

// Main visualization function
int hbtrie_visualize(hbtrie_t* trie, const char* path) {
    if (trie == NULL || path == NULL) {
        log_error("Invalid parameters: trie=%p, path=%p", trie, path);
        return -1;
    }

    // Open output file
    FILE* fp = fopen(path, "w");
    if (fp == NULL) {
        log_error("Failed to open file: %s", path);
        return -1;
    }

    // Reset node ID counter
    atomic_store(&g_node_id_counter, 0);

    // Create temporary file for JSON
    char temp_path[] = "/tmp/hbtrie_viz_json_XXXXXX";
    int temp_fd = mkstemp(temp_path);
    if (temp_fd < 0) {
        log_error("Failed to create temp file");
        fclose(fp);
        return -1;
    }

    FILE* json_fp = fdopen(temp_fd, "w");
    if (json_fp == NULL) {
        log_error("Failed to open temp stream");
        close(temp_fd);
        fclose(fp);
        return -1;
    }

    // Build JSON
    size_t node_count = 0;
    fprintf(json_fp, "{\n");
    fprintf(json_fp, "  \"chunk_size\": %u,\n", trie->chunk_size);
    fprintf(json_fp, "  \"btree_node_size\": %u,\n", trie->btree_node_size);
    fprintf(json_fp, "  \"root\": ");

    if (serialize_hbtrie_node(trie->root, json_fp, trie->chunk_size, &node_count, 0) != 0) {
        log_error("Failed to serialize root node");
        fclose(json_fp);
        unlink(temp_path);
        fclose(fp);
        return -1;
    }

    fprintf(json_fp, ",\n");
    fprintf(json_fp, "  \"stats\": {\n");
    fprintf(json_fp, "    \"total_nodes\": %zu,\n", node_count);
    fprintf(json_fp, "    \"total_entries\": 0,\n");
    fprintf(json_fp, "    \"max_depth\": 0\n");
    fprintf(json_fp, "  }\n");
    fprintf(json_fp, "}\n");
    fclose(json_fp);

    // Read JSON back
    FILE* read_fp = fopen(temp_path, "r");
    if (read_fp == NULL) {
        log_error("Failed to read temp file");
        unlink(temp_path);
        fclose(fp);
        return -1;
    }

    fseek(read_fp, 0, SEEK_END);
    long json_size = ftell(read_fp);
    fseek(read_fp, 0, SEEK_SET);

    char* json_data = malloc(json_size + 1);
    if (json_data == NULL) {
        log_error("Failed to allocate JSON buffer");
        fclose(read_fp);
        unlink(temp_path);
        fclose(fp);
        return -1;
    }

    fread(json_data, 1, json_size, read_fp);
    json_data[json_size] = '\0';
    fclose(read_fp);
    unlink(temp_path);

    // Load D3.js base64
    FILE* d3_fp = fopen("/tmp/d3.min.js.base64", "r");
    char* d3_base64 = NULL;
    if (d3_fp) {
        fseek(d3_fp, 0, SEEK_END);
        long d3_size = ftell(d3_fp);
        fseek(d3_fp, 0, SEEK_SET);

        d3_base64 = malloc(d3_size + 1);
        if (d3_base64 == NULL) {
            log_error("Failed to allocate D3 buffer");
            fclose(d3_fp);
            free(json_data);
            fclose(fp);
            return -1;
        }
        fread(d3_base64, 1, d3_size, d3_fp);
        d3_base64[d3_size] = '\0';
        fclose(d3_fp);
    }

    if (d3_base64 == NULL) {
        log_error("D3.js base64 not found. Run: curl -s https://d3js.org/d3.v7.min.js | base64 -w 0 > /tmp/d3.min.js.base64");
        free(json_data);
        fclose(fp);
        return -1;
    }

    // Write HTML
    fprintf(fp, HTML_TEMPLATE, d3_base64, json_data);

    // Cleanup
    free(d3_base64);
    free(json_data);
    fclose(fp);

    return 0;
}