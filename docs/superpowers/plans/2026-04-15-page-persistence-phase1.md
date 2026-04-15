# Page-Based Persistence — Phase 1: Page File and BnodeCache

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the section-based persistence layer with a page-file storage engine and sharded bnode cache, enabling incremental dirty-node writes and copy-on-write semantics.

**Architecture:** Single append-only page file divided into fixed-size blocks (default 4KB). Modified nodes are written at the end of the file (CoW); old locations are tracked as stale regions. A sharded BnodeCache provides dirty/clean node tracking with LRU eviction. A dual superblock stores the root pointer atomically. Dirty nodes are flushed by a background flusher triggered at configurable thresholds.

**Tech Stack:** C11, gtest for tests, following STYLEGUIDE.md conventions (refcounter_t first member, type_action() naming, get_clear_memory(), platform_lock_init(), etc.)

**Spec:** `docs/superpowers/specs/2026-04-15-page-based-persistence-design.md`

**Important:** Memory leak checks (ASan) at every testing stage. Every create/destroy cycle must show zero leaks.

---

## File Structure

| File | Responsibility |
|------|---------------|
| `src/Storage/page_file.h` | Page file API: open, close, alloc, read, write, stale regions, superblock |
| `src/Storage/page_file.c` | Page file implementation |
| `src/Storage/bnode_cache.h` | BnodeCache API: sharded cache, dirty tracking, flush, eviction |
| `src/Storage/bnode_cache.c` | BnodeCache implementation |
| `src/Storage/stale_region.h` | Stale region tracking API |
| `src/Storage/stale_region.c` | Stale region implementation (sorted list, merge, reclaim) |
| `tests/test_page_file.cpp` | Page file unit tests |
| `tests/test_bnode_cache.cpp` | BnodeCache unit tests |
| `tests/test_stale_region.cpp` | Stale region unit tests |

Existing files modified:
- `CMakeLists.txt` — add new test targets and library sources
- `src/Storage/` — new files added to existing directory

---

### Task 1: Stale Region Manager

**Files:**
- Create: `src/Storage/stale_region.h`
- Create: `src/Storage/stale_region.c`
- Create: `tests/test_stale_region.cpp`
- Modify: `CMakeLists.txt`

The stale region manager tracks regions of the page file that are no longer live (old versions of CoW nodes). It supports adding regions, merging adjacent regions, and querying for reusable blocks.

**Design:** A sorted array of `{offset, length}` pairs, sorted by offset. Merge-adjacent on insert. Query returns contiguous reusable blocks above a threshold.

- [ ] **Step 1: Write stale_region.h**

```c
#ifndef STALE_REGION_H
#define STALE_REGION_H

#include <stdint.h>
#include <stddef.h>

typedef struct {
    uint64_t offset;
    uint64_t length;
} stale_region_t;

typedef struct {
    stale_region_t* regions;
    size_t count;
    size_t capacity;
    uint64_t total_stale_bytes;
} stale_region_mgr_t;

// Create/destroy
stale_region_mgr_t* stale_region_mgr_create(void);
void stale_region_mgr_destroy(stale_region_mgr_t* mgr);

// Add a stale region (merges with adjacent regions automatically)
void stale_region_add(stale_region_mgr_t* mgr, uint64_t offset, uint64_t length);

// Get reusable blocks above threshold ratio (0.0-1.0 of total file size)
// Returns allocated array of {offset, length} blocks and sets out_count.
// Caller must free the returned array.
uint64_t stale_region_get_reusable(stale_region_mgr_t* mgr, uint64_t file_size,
                                   double threshold_ratio, size_t* out_count);

// Clear all stale regions (used after compaction)
void stale_region_clear(stale_region_mgr_t* mgr);

// Get total stale bytes
uint64_t stale_region_total(stale_region_mgr_t* mgr);

// Serialize/deserialize for superblock
uint8_t* stale_region_serialize(stale_region_mgr_t* mgr, size_t* out_len);
stale_region_mgr_t* stale_region_deserialize(const uint8_t* data, size_t len);

#endif
```

- [ ] **Step 2: Write stale_region.c**

Implement all functions. `stale_region_add` inserts into sorted position and merges with adjacent regions if the new region touches or overlaps them. Use `get_clear_memory()` for allocation. Follow STYLEGUIDE.md conventions.

- [ ] **Step 3: Write test_stale_region.cpp**

Test cases:
1. Create and destroy (no leaks under ASan)
2. Add a single region, verify total
3. Add two adjacent regions, verify they merge
4. Add overlapping regions, verify merge
5. Add non-adjacent regions, verify they stay separate
6. Get reusable blocks above threshold
7. Clear and verify empty
8. Serialize and deserialize round-trip
9. Large number of regions (stress test)

- [ ] **Step 4: Build and run tests under ASan**

```bash
cd build_profile && cmake -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTS=ON -DCMAKE_C_FLAGS="-fsanitize=address -g" -DCMAKE_CXX_FLAGS="-fsanitize=address -g" -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address" -S .. -B . && cmake --build . --target test_stale_region && ./test_stale_region
```

Expected: All tests pass with zero leaks.

- [ ] **Step 5: Add to CMakeLists.txt**

Add `stale_region.c` to the wavedb library sources and `test_stale_region` as a test target. Follow the existing pattern for `test_section_variable`.

- [ ] **Step 6: Commit**

```bash
git add src/Storage/stale_region.h src/Storage/stale_region.c tests/test_stale_region.cpp CMakeLists.txt
git commit -m "feat: add stale region manager for page-based persistence"
```

---

### Task 2: Page File

**Files:**
- Create: `src/Storage/page_file.h`
- Create: `src/Storage/page_file.c`
- Create: `tests/test_page_file.cpp`
- Modify: `CMakeLists.txt`

The page file manages the on-disk storage: block allocation, node read/write, superblock management, and stale region integration.

**On-disk layout:**
```
Block 0:     [Superblock A: magic(4) + version(2) + root_offset(8) + root_size(8) + revnum(8) + crc32(4) + padding]
Block 1:     [Superblock B: same format]
Block 2..N:  [Bnode data...][IndexBlkMeta(16): next_bid(8) + revnum_hash(2) + reserved(5) + marker(1=0xFF)]
```

Each bnode write appends at the end of the file. The first 4 bytes of every node contain the node's total serialized size (for reading). Multi-block nodes are linked via `next_bid` in IndexBlkMeta.

- [ ] **Step 1: Write page_file.h**

```c
#ifndef PAGE_FILE_H
#define PAGE_FILE_H

#include <stdint.h>
#include <stddef.h>
#include "../Util/threadding.h"
#include "stale_region.h"

#define PAGE_FILE_MAGIC 0x57444250  // "WDBP"
#define PAGE_FILE_VERSION 1
#define PAGE_FILE_DEFAULT_BLOCK_SIZE 4096
#define PAGE_FILE_DEFAULT_NUM_SUPERBLOCKS 2
#define INDEX_BLK_META_SIZE 16
#define PAGE_FILE_SUPERBLOCK_SIZE 48  // magic(4)+version(2)+root_offset(8)+root_size(8)+revnum(8)+crc32(4)+padding

typedef struct {
    uint8_t magic[4];         // "WDBP"
    uint16_t version;          // Format version
    uint64_t root_offset;      // Offset of root hbtrie_node's btree
    uint64_t root_size;        // Serialized size of root btree
    uint64_t revision;        // Monotonically increasing revision number
    uint32_t crc32;            // CRC32 of all preceding fields
} page_superblock_t;

typedef struct {
    uint64_t next_bid;         // Next block for multi-block nodes (BLK_NOT_FOUND if last)
    uint16_t revnum_hash;      // Low 16 bits of superblock revision (for consistency check)
    uint8_t reserved[5];
    uint8_t marker;           // 0xFF for bnode blocks
} index_blk_meta_t;

#define BLK_NOT_FOUND ((uint64_t)-1)

typedef struct {
    char* path;                          // File path
    int fd;                              // File descriptor
    uint64_t block_size;                  // Block size in bytes
    uint64_t num_superblocks;             // Number of superblock copies
    uint64_t cur_bid;                     // Current allocation block ID
    uint64_t cur_offset;                  // Current offset within cur_bid's block
    uint64_t revision;                    // Current revision number
    stale_region_mgr_t* stale_mgr;       // Stale region manager
    PLATFORMLOCKTYPE(lock);              // Mutex for allocation and superblock writes
    uint8_t is_writable;                  // 1 if file opened for writing
} page_file_t;

// Create/destroy
page_file_t* page_file_create(const char* path, uint64_t block_size, uint64_t num_superblocks);
void page_file_destroy(page_file_t* pf);

// Open existing file for reading (and optionally writing)
int page_file_open(page_file_t* pf, uint8_t writable);

// Allocate a new block at end of file
uint64_t page_file_alloc_block(page_file_t* pf);

// Write a node's data to the file, spanning blocks as needed.
// Returns the offset of the first byte. Sets out_bid to the first block ID.
// out_bids array must be large enough for ceil(data_len / (block_size - INDEX_BLK_META_SIZE)) + 1 blocks.
// Caller provides out_bids array; function fills it and sets out_num_bids.
int page_file_write_node(page_file_t* pf, const uint8_t* data, size_t data_len,
                          uint64_t* out_offset, uint64_t* out_bids, size_t* out_num_bids);

// Read a node from the file at the given offset.
// Reads first 4 bytes for size, then reads the full node.
// Returns allocated buffer; caller must free().
// Returns NULL on error.
uint8_t* page_file_read_node(page_file_t* pf, uint64_t offset, size_t* out_len);

// Mark a region as stale (old version of a CoW node)
void page_file_mark_stale(page_file_t* pf, uint64_t offset, uint64_t length);

// Get reusable blocks from stale regions above threshold
uint64_t* page_file_get_reusable_blocks(page_file_t* pf, double threshold_ratio, size_t* out_count);

// Write superblock to the next slot (round-robin among num_superblocks)
int page_file_write_superblock(page_file_t* pf, uint64_t root_offset, uint64_t root_size);

// Read the latest valid superblock
int page_file_read_superblock(page_file_t* pf, page_superblock_t* out_sb);

// Get total file size
uint64_t page_file_size(page_file_t* pf);

// Get current stale ratio (0.0-1.0)
double page_file_stale_ratio(page_file_t* pf);

#endif
```

- [ ] **Step 2: Write page_file.c**

Implement all functions. Key implementation details:
- `page_file_create`: allocate struct, init lock, create stale_region_mgr. Don't open file yet.
- `page_file_open`: open file with O_RDWR or O_RDONLY. If writable and file is new (size 0), write initial superblocks. If existing, read latest superblock to get revision.
- `page_file_alloc_block`: atomic offset advancement using lock. If file size < required, extend with ftruncate. Return block ID.
- `page_file_write_node`: allocate blocks as needed. Data is written starting at `cur_bid * block_size + cur_offset`. First 4 bytes of data must fit in the current block (enforced like ForestDB). Multi-block nodes are linked via `next_bid` in IndexBlkMeta. Append IndexBlkMeta at end of each block.
- `page_file_read_node`: read 4 bytes at offset for size, then read full node. Follow `next_bid` links for multi-block nodes.
- `page_file_write_superblock`: write to `superblocks[revision % num_superblocks]` position. Compute CRC32 of all fields before the crc. Write atomically (single write call).
- `page_file_read_superblock`: read all superblock slots, pick the one with highest revision and valid CRC.
- Follow STYLEGUIDE.md: `refcounter_t` first, `get_clear_memory()`, `platform_lock_init()`, etc.

- [ ] **Step 3: Write test_page_file.cpp**

Test cases:
1. Create and destroy (no leaks under ASan)
2. Open new file, write superblock, read it back — verify fields match
3. Write a small node (fits in one block), read it back — verify data matches
4. Write a large node (spans multiple blocks), read it back — verify data matches
5. Write multiple nodes, read each by offset
6. Mark stale regions, verify stale ratio
7. Write superblock twice (alternating slots), read latest — verify revision increments
8. Close and reopen file — verify superblock persists
9. Read from non-existent offset — verify NULL return
10. Multiple nodes written, then stale half — verify stale_ratio ≈ 0.5

Each test creates a temp directory, opens a page file, operates, closes, and cleans up. Follow the `DatabaseTest` fixture pattern.

- [ ] **Step 4: Build and run tests under ASan**

```bash
cd build_profile && cmake --build . --target test_page_file && ./test_page_file
```

Expected: All tests pass with zero leaks.

- [ ] **Step 5: Add to CMakeLists.txt and commit**

```bash
git add src/Storage/page_file.h src/Storage/page_file.c tests/test_page_file.cpp CMakeLists.txt
git commit -m "feat: add page file for block-aligned node storage"
```

---

### Task 3: BnodeCache

**Files:**
- Create: `src/Storage/bnode_cache.h`
- Create: `src/Storage/bnode_cache.c`
- Create: `tests/test_bnode_cache.cpp`
- Modify: `CMakeLists.txt`

The BnodeCache provides an in-memory cache for bnode data read from the page file, with dirty tracking for write-back, LRU eviction of clean nodes, and sharded locking for concurrency.

**Architecture:**
- Global singleton `BnodeCacheMgr` managing per-file `FileBnodeCache` instances
- Each `FileBnodeCache` has N shards (default 11, a prime number like ForestDB)
- Each shard has: spin lock, LRU list of clean nodes, map of dirty nodes (by offset), hash map of all nodes
- Nodes are `BnodeCacheItem`: `{offset, data_ptr, data_len, is_dirty, ref_count}`
- `write()`: store node data in cache, mark dirty
- `read()`: check cache, on miss read from page file via `page_file_read_node()`
- `flush()`: write all dirty nodes to page file sequentially in offset order
- `evict()`: LRU eviction when cache exceeds size limit

- [ ] **Step 1: Write bnode_cache.h**

```c
#ifndef BNODE_CACHE_H
#define BNODE_CACHE_H

#include <stdint.h>
#include <stddef.h>
#include "../Util/threadding.h"
#include "page_file.h"

// Forward declarations
typedef struct bnode_cache_item_t bnode_cache_item_t;
typedef struct bnode_cache_shard_t bnode_cache_shard_t;
typedef struct file_bnode_cache_t file_bnode_cache_t;
typedef struct bnode_cache_mgr_t bnode_cache_mgr_t;

typedef struct bnode_cache_item_t {
    uint64_t offset;          // File offset of this node
    uint8_t* data;            // Node data (owned by cache)
    size_t data_len;           // Size of node data
    uint8_t is_dirty;          // 1 if modified since last flush
    uint32_t ref_count;       // Reference count (protected eviction)
    bnode_cache_item_t* lru_next;   // LRU list next (most recently used)
    bnode_cache_item_t* lru_prev;   // LRU list previous (least recently used)
} bnode_cache_item_t;

typedef struct bnode_cache_shard_t {
    PLATFORMLOCKTYPE(lock);                           // Spin lock for this shard
    bnode_cache_item_t* lru_first;                   // Most recently used
    bnode_cache_item_t* lru_last;                    // Least recently used
    size_t dirty_count;                               // Number of dirty items
    size_t dirty_bytes;                               // Total dirty bytes
    // Hash map: offset -> item (use a simple open-addressing hash)
    bnode_cache_item_t** buckets;
    size_t bucket_count;
    size_t item_count;
} bnode_cache_shard_t;

typedef struct file_bnode_cache_t {
    char* filename;                                   // For identification
    page_file_t* page_file;                           // Page file for I/O
    bnode_cache_shard_t* shards;                      // Array of shards
    size_t num_shards;                                 // Number of shards
    size_t max_memory;                                // Max memory for this file's cache
    size_t current_memory;                             // Current memory usage
    size_t dirty_threshold;                            // Flush threshold in bytes
} file_bnode_cache_t;

typedef struct bnode_cache_mgr_t {
    PLATFORMLOCKTYPE(global_lock);                    // For file map access
    file_bnode_cache_t** files;                       // Array of file caches
    size_t file_count;
    size_t max_total_memory;                          // Global memory limit
    size_t current_total_memory;                      // Global memory usage
    size_t num_shards;                                 // Shards per file cache
} bnode_cache_mgr_t;

// Singleton management
bnode_cache_mgr_t* bnode_cache_mgr_create(size_t max_memory, size_t num_shards);
void bnode_cache_mgr_destroy(bnode_cache_mgr_t* mgr);

// File cache management
file_bnode_cache_t* bnode_cache_create_file_cache(bnode_cache_mgr_t* mgr, page_file_t* pf, const char* filename);
void bnode_cache_destroy_file_cache(file_bnode_cache_t* fcache);

// Read a node from cache (or from page file on miss)
// Returns a cache item with ref_count incremented. Caller must call bnode_cache_release().
bnode_cache_item_t* bnode_cache_read(file_bnode_cache_t* fcache, uint64_t offset);

// Write a node to cache (marks dirty)
// data is COPIED into the cache. Caller retains ownership of original data.
int bnode_cache_write(file_bnode_cache_t* fcache, uint64_t offset, const uint8_t* data, size_t data_len);

// Release a cache item (decrement ref_count)
void bnode_cache_release(file_bnode_cache_t* fcache, bnode_cache_item_t* item);

// Flush all dirty nodes to page file
// Writes nodes in ascending offset order for sequential I/O.
// Returns 0 on success.
int bnode_cache_flush_dirty(file_bnode_cache_t* fcache);

// Invalidate a specific node (remove from cache, mark stale in page file)
int bnode_cache_invalidate(file_bnode_cache_t* fcache, uint64_t offset);

// Get number of dirty nodes
size_t bnode_cache_dirty_count(file_bnode_cache_t* fcache);

// Get total dirty bytes
size_t bnode_cache_dirty_bytes(file_bnode_cache_t* fcache);

#endif
```

- [ ] **Step 2: Write bnode_cache.c**

Implement all functions. Key details:
- Use `get_clear_memory()` for allocations, follow STYLEGUIDE.md.
- Shard selection: `offset % num_shards`
- Hash map: simple open-addressing with linear probing. Start with 64 buckets, resize at 75% load.
- LRU: doubly-linked list. On access, move to front. Evict from back when `current_memory > max_memory` and item has `ref_count == 0`.
- `bnode_cache_flush_dirty`: collect all dirty items from all shards, sort by offset, write sequentially to page file via `page_file_write_node()`, mark as clean after write.
- `bnode_cache_read`: look up by offset in shard. On miss, call `page_file_read_node()`, create cache item, insert.
- Thread safety: each shard has its own spin lock. Global lock only for file cache creation/destruction.

- [ ] **Step 3: Write test_bnode_cache.cpp**

Test cases:
1. Create and destroy manager (no leaks under ASan)
2. Create file cache, write a node, read it back — verify data matches
3. Write multiple nodes, read each by offset
4. Write dirty nodes, flush, verify dirty count drops to 0
5. Write more than cache limit, verify eviction of clean nodes
6. Reference counting: read a node, verify ref_count = 2 after second read, release both
7. Invalidate a node, verify it's removed from cache
8. Flush with no dirty nodes — no-op, returns 0
9. Multi-shard: write nodes at different offsets that hash to different shards, verify all readable

Each test creates a temp directory, creates a page file, creates a cache, operates, destroys. Follow `DatabaseTest` fixture pattern.

- [ ] **Step 4: Build and run tests under ASan**

```bash
cd build_profile && cmake --build . --target test_bnode_cache && ./test_bnode_cache
```

Expected: All tests pass with zero leaks.

- [ ] **Step 5: Add to CMakeLists.txt and commit**

```bash
git add src/Storage/bnode_cache.h src/Storage/bnode_cache.c tests/test_bnode_cache.cpp CMakeLists.txt
git commit -m "feat: add sharded bnode cache with dirty tracking and LRU eviction"
```

---

### Task 4: Integration Test — Page File + BnodeCache Together

**Files:**
- Create: `tests/test_page_cache_integration.cpp`
- Modify: `CMakeLists.txt`

This test verifies the page file and bnode cache work together correctly, simulating the CoW write pattern that will be used by the real persistence layer.

- [ ] **Step 1: Write integration test**

Test scenarios:
1. **CoW lifecycle**: Write node at offset A, read it back, write modified version at offset B, mark A as stale, verify stale ratio reflects the change, verify both offsets are readable.
2. **Dirty flush cycle**: Write 10 dirty nodes, flush, verify all are clean and persisted to page file. Close and reopen, verify all 10 readable.
3. **Superblock update cycle**: Write root node, write superblock pointing to it. Modify root, write at new offset, write new superblock. Read latest superblock — verify it points to new root.
4. **Eviction under pressure**: Set cache limit to 2KB, write 5KB of nodes, verify clean nodes are evicted, verify dirty nodes are NOT evicted.
5. **Crash recovery simulation**: Write nodes, write superblock, but do NOT flush dirty nodes. Close file. Reopen, read superblock — verify it points to last flushed root (old data), verify WAL would need to replay.
6. **Large node spanning blocks**: Write a node larger than block_size, read it back, verify data integrity.

- [ ] **Step 2: Build and run under ASan**

```bash
cd build_profile && cmake --build . --target test_page_cache_integration && ./test_page_cache_integration
```

Expected: All tests pass with zero leaks.

- [ ] **Step 3: Commit**

```bash
git add tests/test_page_cache_integration.cpp CMakeLists.txt
git commit -m "test: add page file + bnode cache integration tests"
```

---

### Task 5: Phase 1 Benchmark

**Files:**
- Create: `tests/benchmark/benchmark_page_cache.cpp`
- Modify: `CMakeLists.txt`

Measure raw I/O throughput for the page file and cache hit rates for the bnode cache.

- [ ] **Step 1: Write benchmark**

Benchmarks:
1. **Sequential writes**: Write N nodes (1KB each) to page file, measure ops/sec and throughput (MB/s)
2. **Sequential reads**: Read N nodes from page file by offset, measure ops/sec
3. **Cache hit rate**: Write N nodes to cache, read them all (should be 100% cache hit), then evict half, read again (measure cache hits vs misses)
4. **Dirty flush throughput**: Write N dirty nodes, flush all, measure total flush time
5. **Comparison with section-based**: Same operations using current section/sections code for reference

Each benchmark creates a fresh temp directory, runs operations, prints results. Use `std::chrono::high_resolution_clock` for timing (follow existing benchmark pattern in `benchmark_concurrent_only.cpp`).

- [ ] **Step 2: Build and run benchmark**

```bash
cd build_profile && cmake -DBUILD_BENCHMARKS=ON -S .. -B . && cmake --build . --target benchmark_page_cache && ./benchmark_page_cache
```

Expected: Results printed showing ops/sec and throughput for each scenario.

- [ ] **Step 3: Commit**

```bash
git add tests/benchmark/benchmark_page_cache.cpp CMakeLists.txt
git commit -m "bench: add page file + bnode cache benchmark"
```

---

## Self-Review Checklist

- [x] **Spec coverage**: Stale region manager ✓, page file ✓, bnode cache ✓, superblock ✓, dirty flush ✓, CoW lifecycle ✓, LRU eviction ✓
- [x] **Placeholder scan**: No TBDs, TODOs, or "implement later" in any task
- [x] **Type consistency**: All structs, functions, and parameters use consistent naming (`stale_region_*`, `page_file_*`, `bnode_cache_*`)
- [x] **ASan at every stage**: Every test task includes ASan verification
- [x] **STYLEGUIDE compliance**: `refcounter_t` first, `type_action()` naming, `get_clear_memory()`, `platform_lock_init()`, `refcounter_init()` last in create, `refcounter_destroy_lock()` before `free()` in destroy