# HBTrie Visualization

## Overview

The HBTrie library includes a built-in visualization feature that generates interactive HTML visualizations of trie structures. This is useful for debugging and understanding trie behavior.

## Usage

### Basic Usage

```c
#include "HBTrie/hbtrie_viz.h"

hbtrie_t* trie = create_my_trie();
int result = hbtrie_visualize(trie, "output.html");
```

### From Debugger

```lldb
(lldb) call (int) hbtrie_visualize(trie, "/tmp/trie_debug.html")
```

### From Tests

```cpp
TEST_F(MyTest, VisualizeState) {
    hbtrie_visualize(trie, "test_state.html");
}
```

## Features

- **Interactive tree**: Click nodes to expand/collapse
- **Viewport zoom/pan**: Mouse wheel to zoom, drag to pan
- **Search**: Find paths in the tree
- **Hex display**: All chunk keys shown in hexadecimal
- **Offline**: Self-contained HTML with embedded D3.js

## Output

The visualization generates a single HTML file (~372KB base + JSON data) containing:
- Embedded D3.js library (v7)
- JSON representation of trie structure
- Interactive SVG visualization

## Prerequisites

D3.js must be available as base64:
```bash
curl -s https://d3js.org/d3.v7.min.js | base64 -w 0 > /tmp/d3.min.js.base64
```

This needs to be run once before generating visualizations.

## Performance

- **Serialization**: O(n) where n = total nodes
- **JSON size**: ~100 bytes per node
- **Browser rendering**: Handles 1000+ nodes smoothly

## Limitations

- MVCC versions not shown (shows latest only)
- Storage metadata not included
- No lazy loading (full tree in memory)

## Example

```c
#include "HBTrie/hbtrie.h"
#include "HBTrie/hbtrie_viz.h"

int main() {
    // Create trie
    hbtrie_t* trie = hbtrie_create(4, 4096);

    // Insert some data
    path_t* path = path_create();
    buffer_t* buf = buffer_create_from_pointer_copy((uint8_t*)"test", 4);
    identifier_t* id = identifier_create(buf, 0);
    path_append(path, id);

    buffer_t* val = buffer_create_from_pointer_copy((uint8_t*)"value", 5);
    identifier_t* val_id = identifier_create(val, 0);
    hbtrie_insert(trie, path, val_id);

    // Visualize
    hbtrie_visualize(trie, "trie.html");

    // Cleanup
    buffer_destroy(buf);
    buffer_destroy(val);
    identifier_destroy(id);
    identifier_destroy(val_id);
    path_destroy(path);
    hbtrie_destroy(trie);

    return 0;
}
```