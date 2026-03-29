//
// Example: Generate HBTrie visualization HTML
// This creates a simple trie and visualizes it as an interactive HTML file
//

#include "HBTrie/hbtrie.h"
#include "HBTrie/hbtrie_viz.h"
#include "HBTrie/path.h"
#include "HBTrie/identifier.h"
#include "Buffer/buffer.h"
#include <stdio.h>
#include <string.h>

int main() {
    printf("Creating HBTrie visualization example...\n");

    // Create a trie with chunk size 4 and B+tree node size 4096
    hbtrie_t* trie = hbtrie_create(4, 4096);
    if (!trie) {
        fprintf(stderr, "Failed to create trie\n");
        return 1;
    }

    // Insert some sample data
    // Path: ["users", "alice"] -> value: "admin"
    path_t* path1 = path_create();
    buffer_t* buf1 = buffer_create_from_pointer_copy((uint8_t*)"users", 5);
    identifier_t* id1 = identifier_create(buf1, 0);
    path_append(path1, id1);
    buffer_destroy(buf1);
    identifier_destroy(id1);

    buf1 = buffer_create_from_pointer_copy((uint8_t*)"alice", 5);
    id1 = identifier_create(buf1, 0);
    path_append(path1, id1);
    buffer_destroy(buf1);
    identifier_destroy(id1);

    buf1 = buffer_create_from_pointer_copy((uint8_t*)"admin", 5);
    identifier_t* val1 = identifier_create(buf1, 0);
    buffer_destroy(buf1);

    hbtrie_insert(trie, path1, val1);
    printf("  Inserted: [users, alice] -> admin\n");

    // Insert more data
    // Path: ["users", "bob"] -> value: "user"
    path_t* path2 = path_create();
    buffer_t* buf2 = buffer_create_from_pointer_copy((uint8_t*)"users", 5);
    identifier_t* id2 = identifier_create(buf2, 0);
    path_append(path2, id2);
    buffer_destroy(buf2);
    identifier_destroy(id2);

    buf2 = buffer_create_from_pointer_copy((uint8_t*)"bob", 3);
    id2 = identifier_create(buf2, 0);
    path_append(path2, id2);
    buffer_destroy(buf2);
    identifier_destroy(id2);

    buf2 = buffer_create_from_pointer_copy((uint8_t*)"user", 4);
    identifier_t* val2 = identifier_create(buf2, 0);
    buffer_destroy(buf2);

    hbtrie_insert(trie, path2, val2);
    printf("  Inserted: [users, bob] -> user\n");

    // Path: ["products", "123"] -> value: "widget"
    path_t* path3 = path_create();
    buffer_t* buf3 = buffer_create_from_pointer_copy((uint8_t*)"products", 8);
    identifier_t* id3 = identifier_create(buf3, 0);
    path_append(path3, id3);
    buffer_destroy(buf3);
    identifier_destroy(id3);

    buf3 = buffer_create_from_pointer_copy((uint8_t*)"123", 3);
    id3 = identifier_create(buf3, 0);
    path_append(path3, id3);
    buffer_destroy(buf3);
    identifier_destroy(id3);

    buf3 = buffer_create_from_pointer_copy((uint8_t*)"widget", 6);
    identifier_t* val3 = identifier_create(buf3, 0);
    buffer_destroy(buf3);

    hbtrie_insert(trie, path3, val3);
    printf("  Inserted: [products, 123] -> widget\n");

    // Generate visualization
    const char* output_file = "example_output.html";
    printf("\nGenerating visualization to %s...\n", output_file);

    int result = hbtrie_visualize(trie, output_file);
    if (result != 0) {
        fprintf(stderr, "Failed to generate visualization\n");
        path_destroy(path1);
        identifier_destroy(val1);
        path_destroy(path2);
        identifier_destroy(val2);
        path_destroy(path3);
        identifier_destroy(val3);
        hbtrie_destroy(trie);
        return 1;
    }

    printf("Success! Open %s in your browser to see the visualization.\n", output_file);
    printf("Note: Make sure /tmp/d3.min.js.base64 exists. Generate it with:\n");
    printf("  curl -s https://d3js.org/d3.v7.min.js | base64 -w 0 > /tmp/d3.min.js.base64\n");

    // Cleanup
    path_destroy(path1);
    identifier_destroy(val1);
    path_destroy(path2);
    identifier_destroy(val2);
    path_destroy(path3);
    identifier_destroy(val3);
    hbtrie_destroy(trie);

    return 0;
}