# HBTrie Visualization Example

This example demonstrates how to use the HBTrie visualization feature.

## Prerequisites

1. **Build the project first:**
   ```bash
   cd /home/victor/Workspace/src/github.com/vijayee/WaveDB
   mkdir -p build && cd build
   cmake ..
   make
   ```

2. **Generate D3.js base64 file:**
   ```bash
   curl -s https://d3js.org/d3.v7.min.js | base64 -w 0 > /tmp/d3.min.js.base64
   ```

## Build and Run

### Option 1: Compile standalone

```bash
gcc -o example_viz example_viz.c \
    -I./src \
    -I./deps/libcbor/src \
    -I./deps/hashmap \
    -I./deps/xxhash \
    -L./build \
    -lwavedb \
    -lcbor \
    -lxxhash
```

### Option 2: Use CMake (recommended)

Add to CMakeLists.txt:
```cmake
add_executable(example_viz example_viz.c)
target_link_libraries(example_viz PRIVATE wavedb)
```

Then build:
```bash
cd build
make example_viz
```

## Run

```bash
./example_viz
```

This will create `example_output.html` in the current directory.

## View the Visualization

Open `example_output.html` in your browser:
```bash
# On Linux with a browser installed:
xdg-open example_output.html

# On macOS:
open example_output.html

# Or just open the file in your browser
```

## Expected Output

```
Creating HBTrie visualization example...
  Inserted: [users, alice] -> admin
  Inserted: [users, bob] -> user
  Inserted: [products, 123] -> widget

Generating visualization to example_output.html...
Success! Open example_output.html in your browser to see the visualization.
```

## Example Visualization Features

The generated HTML file includes:
- **Interactive tree structure** showing the HBTrie hierarchy
- **Zoom and pan controls** for exploring large trees
- **Node details** displayed on hover/click
- **Hex-encoded keys** for each chunk
- **Search functionality** for finding specific paths
- **Expand/collapse controls** for nodes

## What You'll See

The visualization will show a tree structure like:

```
root
├── users (0x7573657273)
│   ├── alice (0x616c696365) → "admin"
│   └── bob (0x626f62) → "user"
└── products (0x70726f6475637473)
    └── 123 (0x313233) → "widget"
```

Each node shows:
- **Chunk key** in hexadecimal
- **Node statistics** (entry count, size)
- **Child nodes** (expandable)
- **Leaf values** (if applicable)

## Troubleshooting

**Error: "D3.js base64 not found"**
```bash
# Generate the D3.js file:
curl -s https://d3js.org/d3.v7.min.js | base64 -w 0 > /tmp/d3.min.js.base64
```

**Blank page or no visualization:**
- Check browser console for JavaScript errors
- Ensure D3.js base64 file exists and is readable
- Verify the HTML file was generated (check file size > 150KB)

**Compilation errors:**
- Ensure you've built the main library first
- Check that all include paths are correct
- Verify you have all dependencies installed