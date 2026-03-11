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

### Dependencies

- C11 compiler
- CMake 3.14+
- pthreads (POSIX) or Windows threads

Submodules:
- [googletest](https://github.com/google/googletest) - Testing framework
- [xxhash](https://github.com/Cyan4973/xxHash) - Hashing library
- [hashmap](https://github.com/DavidLeeds/hashmap) - Hash map implementation

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