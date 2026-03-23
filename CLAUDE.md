# WaveDB Project

Always read and follow the coding conventions in [STYLEGUIDE.md](./STYLEGUIDE.md) when writing or modifying C code in this project.

## Git Commit Conventions

- **Do NOT add "Co-Authored-By" lines to commit messages.** All commits should have only the author's information.
- Use clear, descriptive commit messages following conventional commit format (e.g., "feat:", "fix:", "docs:", "test:", "refactor:", "chore:")
- Keep commits focused and atomic - one logical change per commit

## Key Patterns
- Reference-counted structs have `refcounter_t refcounter` as the first member
- Types use `_t` suffix, functions follow `type_action()` naming
- Create functions use `get_clear_memory()` and call `refcounter_init()` last
- Destroy functions check count==0 before freeing
- Directories in `src/` are organized by semantic purpose (Buffer/, Workers/, Time/, etc.)

## HBTrie Data Structures

### Core Primitives

**chunk_t** (`src/HBTrie/chunk.h`) - Fixed-size buffer for key comparison
- Each chunk holds exactly `chunk_size` bytes (default 4)
- Chunks are the atomic units for B+tree comparison
- Use `chunk_create()`, `chunk_destroy()`, `chunk_compare()`

**identifier_t** (`src/HBTrie/identifier.h`) - Sequence of chunks
- `refcounter_t` first, then `vec_t chunks`, then `size_t length`
- `length` tracks original byte count (not padded)
- `nchunk = (length - 1) / chunk_size + 1`
- Use `identifier_create()`, `identifier_destroy()`, `identifier_compare()`

**path_t** (`src/HBTrie/path.h`) - Sequence of identifiers (MUMPS subscripts)
- `refcounter_t` first, then `vec_t identifiers`
- Represents hierarchical key: `['homes', 'trailers', 'type']`
- Use `path_create()`, `path_append()`, `path_destroy()`

### B+Tree Structures

**bnode_entry_t** (`src/HBTrie/bnode.h`) - Entry in B+tree node
- `chunk_t* key` - Single chunk for comparison
- `union { hbtrie_node_t* child; identifier_t* value; }` - Either subtree or leaf
- `uint8_t has_value` - 0=child node, 1=value

**bnode_t** (`src/HBTrie/bnode.h`) - B+tree node
- `refcounter_t` first
- `vec_t entries` - Sorted by chunk key
- Use `bnode_create()`, `bnode_destroy()`, `bnode_find()`, `bnode_insert()`

**hbtrie_node_t** (`src/HBTrie/hbtrie.h`) - HBTrie node
- `refcounter_t` first
- `bnode_t* btree` - B+tree comparing chunks at this level
- Use `hbtrie_node_create()`, `hbtrie_node_destroy()`

**hbtrie_t** (`src/HBTrie/hbtrie.h`) - Top-level HBTrie
- `refcounter_t` first
- `uint8_t chunk_size` - Configurable (default 4)
- `hbtrie_node_t* root` - Root node
- Use `hbtrie_create()`, `hbtrie_destroy()`, `hbtrie_insert()`, `hbtrie_find()`

### Traversal

**hbtrie_cursor_t** - Tracks position during traversal
- `hbtrie_node_t* current` - Current node
- `identifier_t* identifier` - Being traversed
- `size_t chunk_pos` - Current chunk position

## Chunk Padding
- Last chunk may not be full: `last_len = length - (nchunk - 1) * chunk_size`
- Original length stored in `identifier_t.length`
- No null padding - use length field for reconstruction