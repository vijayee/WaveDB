//
// Example: Generate complex HBTrie visualization with many nodes
// This creates a trie with multiple levels and branches for demonstration
//

#include "HBTrie/hbtrie.h"
#include "HBTrie/hbtrie_viz.h"
#include "HBTrie/path.h"
#include "HBTrie/identifier.h"
#include "Buffer/buffer.h"
#include <stdio.h>
#include <string.h>

// Helper to create path from multiple strings
path_t* create_path(const char** parts, int count) {
    path_t* path = path_create();
    for (int i = 0; i < count; i++) {
        buffer_t* buf = buffer_create_from_pointer_copy(
            (uint8_t*)parts[i], strlen(parts[i]));
        identifier_t* id = identifier_create(buf, 0);
        buffer_destroy(buf);
        path_append(path, id);
        identifier_destroy(id);
    }
    return path;
}

// Helper to create value
identifier_t* create_value(const char* data) {
    buffer_t* buf = buffer_create_from_pointer_copy(
        (uint8_t*)data, strlen(data));
    identifier_t* id = identifier_create(buf, 0);
    buffer_destroy(buf);
    return id;
}

int main() {
    printf("Creating complex HBTrie visualization...\n\n");

    // Create trie
    hbtrie_t* trie = hbtrie_create(4, 4096);
    if (!trie) {
        fprintf(stderr, "Failed to create trie\n");
        return 1;
    }

    // Create a tree structure with multiple branches:
    // database/
    //   ├── users/
    //   │   ├── alice -> admin
    //   │   ├── bob -> user
    //   │   └── charlie -> guest
    //   ├── products/
    //   │   ├── widget -> available
    //   │   └── gadget -> discontinued
    //   └── config/
    //       ├── theme -> dark
    //       └── language -> en

    printf("Inserting data:\n");

    // Users branch
    path_t* path = create_path((const char*[]){"database", "users", "alice"}, 3);
    identifier_t* val = create_value("admin");
    hbtrie_insert(trie, path, val);
    printf("  [database, users, alice] -> admin\n");
    path_destroy(path);
    identifier_destroy(val);

    path = create_path((const char*[]){"database", "users", "bob"}, 3);
    val = create_value("user");
    hbtrie_insert(trie, path, val);
    printf("  [database, users, bob] -> user\n");
    path_destroy(path);
    identifier_destroy(val);

    path = create_path((const char*[]){"database", "users", "charlie"}, 3);
    val = create_value("guest");
    hbtrie_insert(trie, path, val);
    printf("  [database, users, charlie] -> guest\n");
    path_destroy(path);
    identifier_destroy(val);

    // Products branch
    path = create_path((const char*[]){"database", "products", "widget"}, 3);
    val = create_value("available");
    hbtrie_insert(trie, path, val);
    printf("  [database, products, widget] -> available\n");
    path_destroy(path);
    identifier_destroy(val);

    path = create_path((const char*[]){"database", "products", "gadget"}, 3);
    val = create_value("discontinued");
    hbtrie_insert(trie, path, val);
    printf("  [database, products, gadget] -> discontinued\n");
    path_destroy(path);
    identifier_destroy(val);

    // Config branch
    path = create_path((const char*[]){"database", "config", "theme"}, 3);
    val = create_value("dark");
    hbtrie_insert(trie, path, val);
    printf("  [database, config, theme] -> dark\n");
    path_destroy(path);
    identifier_destroy(val);

    path = create_path((const char*[]){"database", "config", "language"}, 3);
    val = create_value("en");
    hbtrie_insert(trie, path, val);
    printf("  [database, config, language] -> en\n");
    path_destroy(path);
    identifier_destroy(val);

    // Add some top-level entries
    path = create_path((const char*[]){"version"}, 1);
    val = create_value("1.0.0");
    hbtrie_insert(trie, path, val);
    printf("  [version] -> 1.0.0\n");
    path_destroy(path);
    identifier_destroy(val);

    path = create_path((const char*[]){"status"}, 1);
    val = create_value("running");
    hbtrie_insert(trie, path, val);
    printf("  [status] -> running\n");
    path_destroy(path);
    identifier_destroy(val);

    // Generate visualization
    const char* output_file = "complex_example.html";
    printf("\nGenerating visualization to %s...\n", output_file);

    int result = hbtrie_visualize(trie, output_file);
    if (result != 0) {
        fprintf(stderr, "Failed to generate visualization\n");
        hbtrie_destroy(trie);
        return 1;
    }

    printf("✓ Success!\n\n");
    printf("Tree structure created:\n");
    printf("  database/\n");
    printf("    ├── users/\n");
    printf("    │   ├── alice -> admin\n");
    printf("    │   ├── bob -> user\n");
    printf("    │   └── charlie -> guest\n");
    printf("    ├── products/\n");
    printf("    │   ├── widget -> available\n");
    printf("    │   └── gadget -> discontinued\n");
    printf("    └── config/\n");
    printf("        ├── theme -> dark\n");
    printf("        └── language -> en\n");
    printf("  version -> 1.0.0\n");
    printf("  status -> running\n\n");

    printf("Open %s in your browser to see the interactive visualization.\n", output_file);
    printf("\nNote: Make sure /tmp/d3.min.js.base64 exists:\n");
    printf("  curl -s https://d3js.org/d3.v7.min.js | base64 -w 0 > /tmp/d3.min.js.base64\n");

    // Cleanup
    hbtrie_destroy(trie);

    return 0;
}