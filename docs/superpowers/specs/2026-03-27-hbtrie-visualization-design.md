# HBTrie Visualization Design

**Date**: 2026-03-27
**Status**: Approved
**Purpose**: Development debugging and data exploration

## Overview

Interactive HTML visualization for HBTrie structure with hex-encoded chunks. Single self-contained HTML file with embedded D3.js library for offline use.

## C API

### Public Function

```c
/**
 * Visualize an HBTrie structure as interactive HTML.
 *
 * Generates a JSON representation of the trie and embeds it
 * in a self-contained HTML file with JavaScript viewer.
 *
 * @param trie    HBTrie to visualize
 * @param path    Output file path (e.g., "hbtrie_viz.html")
 * @return 0 on success, -1 on failure
 */
int hbtrie_visualize(hbtrie_t* trie, const char* path);
```

### Usage Examples

```c
// In tests
hbtrie_t* trie = create_test_trie();
hbtrie_visualize(trie, "test_output.html");

// In debugger
(lldb) call (int) hbtrie_visualize(trie, "/tmp/trie_debug.html")

// In application code
if (debug_enabled) {
    hbtrie_visualize(db->index, "index_snapshot.html");
}
```

## JSON Data Structure

### Schema

```json
{
  "chunk_size": 4,
  "btree_node_size": 4096,
  "root": {
    "id": "node_1",
    "entries": [
      {
        "key_hex": "484f4d45",
        "has_value": false,
        "child": {
          "id": "node_2",
          "entries": [...]
        }
      },
      {
        "key_hex": "00000001",
        "has_value": true,
        "value": {
          "length": 4,
          "data_hex": "484f4d45",
          "text": "HOME"
        }
      }
    ],
    "stats": {
      "entry_count": 3,
      "node_size_bytes": 128
    }
  },
  "stats": {
    "total_nodes": 15,
    "total_entries": 42,
    "max_depth": 5
  }
}
```

### Key Design Decisions

- **Hex encoding**: All chunk keys shown as hex strings (`"key_hex": "484f4d45"`)
- **Node IDs**: Unique IDs for breadcrumb navigation (`"id": "node_1"`)
- **Value representation**: Both hex and ASCII text when printable
- **Stats**: Aggregated stats at root + per-node stats
- **Minimal data**: Only essential structure (no MVCC versions or storage metadata)

### Chunk to Hex Conversion

```c
// Each byte becomes 2 hex chars
// Example: chunk with bytes [0x48, 0x4F, 0x4D, 0x45] -> "484f4d45"
// Full identifier: concatenation of all chunks with original length
```

## HTML/JavaScript Viewer

### File Structure

```c
static const char* HTML_VIEWER = "<!DOCTYPE html>\n"
  "<html>\n"
  "<head>\n"
  "  <meta charset=\"UTF-8\">\n"
  "  <title>HBTrie Visualizer</title>\n"
  "  <script src=\"data:text/javascript;base64,...\"</script>\n"  // Embedded D3
  "  <style>\n"
  "    /* CSS for tree layout, nodes, links */\n"
  "  </style>\n"
  "</head>\n"
  "<body>\n"
  "  <div id=\"controls\">\n"
  "    <input type=\"text\" id=\"search\" placeholder=\"Search path...\">\n"
  "    <button id=\"expand-all\">Expand All</button>\n"
  "    <button id=\"collapse-all\">Collapse All</button>\n"
  "    <button id=\"zoom-in\">Zoom In</button>\n"
  "    <button id=\"zoom-out\">Zoom Out</button>\n"
  "    <button id=\"zoom-reset\">Fit to View</button>\n"
  "  </div>\n"
  "  <div id=\"breadcrumbs\"></div>\n"
  "  <svg id=\"tree\"></svg>\n"
  "  <script>\n"
  "    const TRIE_DATA = %s;  // JSON injected here\n"
  "    // D3.js tree layout code\n"
  "  </script>\n"
  "</body>\n"
  "</html>";
```

### D3.js Features

1. **Tree layout**: D3 hierarchical layout with automatic node positioning
2. **Expand/collapse**: Click handlers to show/hide children with smooth transitions
3. **Search**: Filter tree to show matching paths
4. **Breadcrumbs**: Track current position with clickable path back to root
5. **Hex display**: Show chunk keys in hex with ASCII tooltip when printable
6. **Viewport zoom/pan**:
   - Mouse wheel: Zoom in/out centered on cursor
   - Pinch: Zoom on touch devices
   - Drag: Pan around the tree
   - Double-click: Reset to fit entire tree
7. **Zoom controls**: Zoom In, Zoom Out, Fit to View buttons

### CSS Styling

- Tree nodes indented with vertical lines
- Collapsed nodes show `[+]`, expanded nodes show `[-]`
- Internal nodes: Blue circles with hex key label
- Leaf nodes: Green squares with value preview
- Search results: Yellow highlight border
- Breadcrumbs: Clickable with `›` separators

### D3 Configuration

- Node size: 25px spacing
- Orientation: Vertical (root at top)
- Link style: Curved Bezier paths
- Animation duration: 500ms
- Zoom extent: 10% to 400%
- Initial view: Centered and scaled to fit

## C Implementation

### File Structure

```
src/HBTrie/hbtrie_viz.c
src/HBTrie/hbtrie_viz.h  (public API, optional)
```

### Function Breakdown

```c
// Main public API
int hbtrie_visualize(hbtrie_t* trie, const char* path);

// Internal helpers (static)
static cJSON* serialize_hbtrie_node(hbtrie_node_t* node);  // Recursive serialization
static cJSON* serialize_bnode(bnode_t* bnode);             // B+tree to JSON array
static cJSON* serialize_entry(bnode_entry_t* entry);       // Single entry to JSON
static cJSON* serialize_identifier(identifier_t* id);      // Value to JSON
static char* chunk_to_hex(chunk_t* chunk);                  // Bytes to hex string
static char* identifier_to_hex(identifier_t* id);           // Full identifier hex

// D3.js embedded viewer (static constant)
static const char* HTML_VIEWER = "...";  // ~160KB total
```

### JSON Serialization Flow

```
hbtrie_visualize()
  └─> serialize_hbtrie_node(root)
      └─> serialize_bnode(btree)
          └─> serialize_entry(entry)
              ├─> chunk_to_hex(key)
              └─> serialize_identifier(value) OR serialize_hbtrie_node(child)
```

### Dependencies

- **Existing**: `jansson` library (already in project)
- **Existing**: `hbtrie.h`, `bnode.h`, `chunk.h`, `identifier.h`

### Error Handling

- Check file write permissions
- Validate trie != NULL
- Clean up JSON objects on error
- Return -1 with error log on failure

### Build Integration

```cmake
# Add to src/HBTrie/CMakeLists.txt
set(HBTRIE_SOURCES
    ...
    hbtrie_viz.c
)
```

## Testing Strategy

### Unit Tests

```c
// In tests/test_hbtrie_viz.cpp

TEST(HbtrieViz, EmptyTrie) {
    hbtrie_t* trie = hbtrie_create(0, 0);
    ASSERT_EQ(0, hbtrie_visualize(trie, "/tmp/empty.html"));
    // Verify file exists and contains valid HTML
}

TEST(HbtrieViz, SingleNode) {
    hbtrie_t* trie = create_trie_with_path("test");
    ASSERT_EQ(0, hbtrie_visualize(trie, "/tmp/single.html"));
    // Verify JSON structure
}

TEST(HbtrieViz, DeepTree) {
    hbtrie_t* trie = create_deep_trie(5);  // 5 levels
    ASSERT_EQ(0, hbtrie_visualize(trie, "/tmp/deep.html"));
    // Verify recursion handles depth
}
```

### Integration Tests

- Generate visualization from existing test tries
- Manually verify HTML files render correctly in browser
- Test with large trees (1000+ nodes) for performance
- Test zoom/pan on mobile touch devices

## File Size Estimate

- D3.min.js: ~150KB minified
- Viewer code: ~5KB
- JSON data: Varies by trie size
- HTML wrapper: ~1KB
- **Total**: ~160KB base + JSON data

## Performance Considerations

- **Serialization**: O(n) where n = total nodes
- **JSON size**: ~100 bytes per node (estimate)
- **Browser rendering**: D3 handles 1000+ nodes smoothly
- **Memory**: All data in browser memory (no lazy loading for MVP)

## Future Enhancements (Not in MVP)

- Subtree focus (drill-down into branch)
- MVCC version visualization
- Storage metadata display
- JSON export without HTML wrapper
- Lazy loading for very large trees
- Export to PNG/SVG

## Success Criteria

1. ✅ Single C function API: `hbtrie_visualize()`
2. ✅ Self-contained HTML file with embedded D3.js
3. ✅ Interactive tree with expand/collapse
4. ✅ Breadcrumb navigation
5. ✅ Path search/filter
6. ✅ Hex display with ASCII tooltips
7. ✅ Viewport zoom/pan
8. ✅ Works offline (no external dependencies)
9. ✅ Clean separation: C for data, JS for presentation