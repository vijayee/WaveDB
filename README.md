<div align="center">
  <img src="wave_fuji.svg" alt="WaveDB Logo" width="200">
</div>

# WaveDB

A C implementation of an HBTrie (Hierarchical B+Tree) data structure for efficient hierarchical key-value storage.

## Overview

WaveDB provides an HBTrie data structure inspired by MUMPS-style multi-dimensional subscript access patterns. It enables efficient storage and retrieval of values at hierarchical key paths like `['users', 'alice', 'name']`.

## Architecture

### Core Data Structures

| Structure | Description |
|-----------|-------------|
| `chunk_t` | Fixed-size buffer (default 4 bytes) - atomic comparison unit |
| `identifier_t` | Variable-length sequence of chunks - represents a single key or value |
| `path_t` | Sequence of identifiers - represents a hierarchical key path |
| `bnode_t` | B+tree node containing sorted entries |
| `bnode_entry_t` | Entry mapping a chunk key to either a child node or leaf value |
| `hbtrie_node_t` | HBTrie node containing a B+tree for one level |
| `hbtrie_t` | Top-level trie with root node |

### Key Concepts

**Chunk-based Comparison:** Keys are split into fixed-size chunks (default 4 bytes). Each chunk is compared at a corresponding level in the B+tree, enabling efficient prefix-based lookups.

**Hierarchical Paths:** A path is a sequence of identifiers, similar to MUMPS subscripts:
```
path: ["users", "alice", "name"]
       ↓
identifier → split into chunks
       ↓
traverse B+tree at each level
       ↓
final position stores value
```

**Reference Counting:** All major structures use thread-safe reference counting with `refcounter_t` as the first member, enabling safe shared ownership.

## Building

```bash
# Clone with submodules
git clone --recursive https://github.com/vijayee/WaveDB.git

# Build
mkdir build && cd build
cmake ..
make -j4

# Run tests
make test
```

## Installation

### From Source

```bash
# Build and install
mkdir build && cd build
cmake -DCMAKE_INSTALL_PREFIX=/usr/local ..
make
sudo make install
```

This installs:
- Headers: `/usr/local/include/HBTrie/*.h`, `/usr/local/include/Buffer/*.h`, etc.
- Libraries: `/usr/local/lib/libwavedb.a`, `/usr/local/lib/libxxhash.a`, `/usr/local/lib/libhashmap.a`
- CMake config: `/usr/local/lib/cmake/WaveDB/WaveDBConfig.cmake`

### Using as a CMake Dependency

**Method 1: find_package (installed)**

```cmake
cmake_minimum_required(VERSION 3.14)
project(myapp C)

find_package(WaveDB REQUIRED)

add_executable(myapp main.c)
target_link_libraries(myapp WaveDB::wavedb)
```

**Method 2: add_subdirectory (embedded)**

```cmake
cmake_minimum_required(VERSION 3.14)
project(myapp C)

add_subdirectory(path/to/WaveDB)

add_executable(myapp main.c)
target_link_libraries(myapp WaveDB::wavedb)
```

### Dependencies

WaveDB bundles the following dependencies:
- **xxhash** - Fast hashing library
- **hashmap** - Hash map implementation
- **libcbor** - CBOR serialization

These are installed alongside WaveDB and require no additional setup.

## Installation

### From Source

```bash
# Clone with submodules
git clone --recursive https://github.com/vijayee/WaveDB.git
cd WaveDB

# Build and install
mkdir build && cd build
cmake -DCMAKE_INSTALL_PREFIX=/usr/local ..
make
make install
```

This installs:
- Libraries: `libwavedb.a`, `libxxhash.a`, `libhashmap.a` in `/usr/local/lib`
- Headers: All headers in `/usr/local/include/` (preserving directory structure)
- CMake config: `/usr/local/lib/cmake/WaveDB/WaveDBConfig.cmake`

### Using as CMake Dependency

#### Method 1: find_package (installed)

After installing WaveDB system-wide or to a custom prefix:

```cmake
cmake_minimum_required(VERSION 3.14)
project(myapp C)

find_package(WaveDB REQUIRED)

add_executable(myapp main.c)
target_link_libraries(myapp WaveDB::wavedb)
```

Build with:
```bash
cmake -DCMAKE_PREFIX_PATH=/usr/local ..
make
```

#### Method 2: add_subdirectory (embedded)

For embedding WaveDB in your project:

```cmake
cmake_minimum_required(VERSION 3.14)
project(myapp C)

add_subdirectory(path/to/WaveDB)

add_executable(myapp main.c)
target_link_libraries(myapp WaveDB::wavedb)
```

### Dependencies

WaveDB bundles the following dependencies:
- **xxhash** - Fast hash algorithm
- **hashmap** - Hash map implementation
- **libcbor** - CBOR encoding/decoding

These are installed alongside WaveDB and require no additional setup.

## Usage

```c
#include "HBTrie/hbtrie.h"
#include "HBTrie/path.h"
#include "HBTrie/identifier.h"
#include "Buffer/buffer.h"

// Create trie with default chunk size (4 bytes)
hbtrie_t* trie = hbtrie_create(0, 0);

// Create a path: ["users", "alice", "name"]
path_t* path = path_create();
buffer_t* buf1 = buffer_create_from_pointer_copy((uint8_t*)"users", 5);
identifier_t* id1 = identifier_create(buf1, 0);
path_append(path, id1);

buffer_t* buf2 = buffer_create_from_pointer_copy((uint8_t*)"alice", 5);
identifier_t* id2 = identifier_create(buf2, 0);
path_append(path, id2);

buffer_t* buf3 = buffer_create_from_pointer_copy((uint8_t*)"name", 4);
identifier_t* id3 = identifier_create(buf3, 0);
path_append(path, id3);

// Create value
buffer_t* val_buf = buffer_create_from_pointer_copy((uint8_t*)"Alice Smith", 11);
identifier_t* value = identifier_create(val_buf, 0);

// Insert
hbtrie_insert(trie, path, value);

// Find
identifier_t* found = hbtrie_find(trie, path);
if (found) {
    buffer_t* result = identifier_to_buffer(found);
    // use result...
    buffer_destroy(result);
    identifier_destroy(found);
}

// Remove
identifier_t* removed = hbtrie_remove(trie, path);
if (removed) {
    identifier_destroy(removed);
}

// Cleanup
path_destroy(path);
identifier_destroy(value);
hbtrie_destroy(trie);
```

## API Reference

### HBTrie

```c
hbtrie_t* hbtrie_create(uint8_t chunk_size, uint32_t btree_node_size);
void hbtrie_destroy(hbtrie_t* trie);

int hbtrie_insert(hbtrie_t* trie, path_t* path, identifier_t* value);
identifier_t* hbtrie_find(hbtrie_t* trie, path_t* path);
identifier_t* hbtrie_remove(hbtrie_t* trie, path_t* path);
```

### Path

```c
path_t* path_create(void);
path_t* path_create_from_identifier(identifier_t* id);
void path_destroy(path_t* path);
int path_append(path_t* path, identifier_t* id);
identifier_t* path_get(path_t* path, size_t index);
size_t path_length(path_t* path);
```

### Identifier

```c
identifier_t* identifier_create(buffer_t* buf, size_t chunk_size);
identifier_t* identifier_create_empty(size_t chunk_size);
void identifier_destroy(identifier_t* id);
int identifier_compare(identifier_t* a, identifier_t* b);
chunk_t* identifier_get_chunk(identifier_t* id, size_t index);
size_t identifier_chunk_count(identifier_t* id);
buffer_t* identifier_to_buffer(identifier_t* id);
```

## Thread Safety

All HBTrie operations are thread-safe using platform-abstracted locking. Define `REFCOUNTER_ATOMIC` at compile time to use C11 atomics instead of mutex-based reference counting.

## Performance

Benchmarks run on Linux x86_64 with the following configuration:
- CPU: 8-core processor
- Memory: 50MB LRU cache
- Pre-populated: 10,000 keys for read benchmarks

### Single-Threaded Operations

| Operation | Throughput | Avg Latency | P99 Latency |
|-----------|------------|-------------|-------------|
| Put (single) | 36,169 ops/sec | 27.6 µs | 53.0 µs |
| Get (single) | 70,772 ops/sec | 14.1 µs | 112.6 µs |
| Batch (1K ops) | 27,944 ops/sec | 35.8 µs | 89.1 µs |
| Mixed (70% read) | 50,542 ops/sec | 19.8 µs | 65.8 µs |
| Delete | 32,780 ops/sec | 30.5 µs | 83.0 µs |

### Concurrent Operations (Multi-Threaded)

| Threads | Write ops/sec | Read ops/sec | Mixed ops/sec |
|---------|---------------|--------------|---------------|
| 1 | 26,839 | 111,927 | 50,090 |
| 2 | 64,908 | 190,248 | 84,652 |
| 4 | 147,573 | 191,212 | 148,334 |
| 8 | 148,767 | 210,073 | 142,025 |
| 16 | 200,344 | 209,424 | 158,298 |

### Memory Efficiency

- LRU Cache Budget: 50.00 MB
- Typical Memory Usage: ~11 MB for 35K entries
- Average Entry Size: ~318 bytes

### Performance Features

- **Memory Pool**: Thread-local caches for lock-free allocation
- **xxHash**: Fast path hashing (3-5x improvement)
- **MVCC Fast-Path**: 90%+ visibility check hit rate
- **Fragment Re-sorting**: Maintains O(log n) lookup

## Code Organization

```
src/
├── HBTrie/        # Core trie implementation
│   ├── hbtrie.c/h     - Top-level trie operations
│   ├── bnode.c/h      - B+tree node implementation
│   ├── chunk.c/h      - Fixed-size comparison unit
│   ├── identifier.c/h - Chunk sequence (key/value)
│   ├── path.c/h       - Hierarchical key path
│   └── bs_array.c/h   - B-sorted array helper
├── Buffer/        # Binary data handling
├── RefCounter/    # Reference counting infrastructure
├── Time/          # Timing utilities (wheel, ticker, debouncer)
├── Workers/       # Async execution (pool, work, promise, queue)
└── Util/          # Utilities (allocator, log, vec, hash, threading)
```

## Coding Conventions

See [STYLEGUIDE.md](./STYLEGUIDE.md) for detailed coding conventions including:

- Reference counting patterns
- Create/destroy function patterns
- Naming conventions (`type_action()`, `_t` suffix)
- Platform abstraction macros

## License

[Specify license here]