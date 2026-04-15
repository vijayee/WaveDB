# Page-Based Persistence Layer Redesign

**Date:** 2026-04-15
**Branch:** experimental-persistence
**Status:** Design

## Problem Statement

WaveDB's current persistence layer has fundamental architectural flaws:

1. **Batch-only persistence**: Section writes only happen on `database_snapshot()` or `database_destroy()`. Normal PUT/GET/DELETE operations only touch the WAL; the in-memory HBTrie is never incrementally persisted.
2. **No lazy loading**: `load_index_sections()` eagerly loads ALL hbtrie_nodes from disk on startup. The `is_loaded`/`is_dirty` flags exist but are never used for eviction or incremental persistence.
3. **V2 inline format prevents independent node I/O**: The V2 serializer recursively inlines child bnodes, making it impossible to load or store a single bnode independently.
4. **Fragment list overhead**: Variable-size section records with CBOR-based free-space metadata, sorted by size (not position), with no coalescing, creates fragmentation and O(n) operations for defrag/remap.
5. **Full-trie-walk on snapshot**: `save_index_sections()` must collect and serialize every node, making it O(N) in total trie size.
6. **Section-level mutex**: Each section has a single mutex held for the entire I/O duration, serializing all reads and writes to the same section.

## ForestDB Reference Architecture

ForestDB (the origin of the HBTrie data structure) provides a proven model for persistence and concurrency. Key takeaways:

### Concurrency Model
- **Single writer, multiple readers** with MVCC
- WAL is an in-memory structure with sharded spin locks for concurrent inserts
- WAL flush is single-threaded: drains committed items and applies to the HB+trie
- The on-disk commit log provides durability (mmap'd append-only files)
- Writers and readers never block each other

### Persistence Model (Copy-on-Write)
- **Append-only file**: Modified B+tree nodes are written at the END of the file, not in place. Old nodes are marked stale.
- **Copy-on-write from leaf to root**: When a leaf node is modified, all nodes along the path from leaf to root are also written (with new offsets). The root's new location is stored in a commit header.
- **BnodeCache**: A global singleton cache with per-file, per-shard dirty/clean tracking. Nodes are variable-size (not block-aligned in BnodeCache). Dirty nodes are flushed sequentially in offset order for maximum write throughput.
- **BnodeMgr**: Manages dirty node set, clean node references, and offset assignment. Nodes are allocated within blocks (4KB), with the first 4 bytes of each node guaranteed to start in the same block for size-reading.
- **Stale block management**: Instead of fragment lists, ForestDB uses a stale region system. When a node is replaced (copy-on-write), its old disk region is marked stale. A `StaleDataManager` tracks stale regions per commit revision. When `block_reusing_threshold` (default 65%) is exceeded, stale regions are reclaimed for new allocations.
- **Block metadata**: Each block has a 16-byte `IndexBlkMeta` suffix with a next-block pointer and a marker byte (0xFF for bnodes). Multi-block nodes are linked via `nextBid`.
- **Recovery**: On crash, scan blocks in reverse order to find the last valid header, then reconstruct WAL entries.

### Key Configuration
| Parameter | Default | Notes |
|-----------|---------|-------|
| Block size | 4KB | Aligned with disk page size |
| BnodeCache size | 128MB | Global across all files |
| BnodeCache flush limit | 1MB | Dirty data threshold for flush |
| WAL threshold | 4096 entries | Triggers WAL flush |
| Block reuse threshold | 65% | Stale data % for reclaim |
| Compaction threshold | 30% | Stale data % for file compaction |
| Superblock sync period | 4MB | Sync superblock interval |

### What ForestDB Does Not Have
- No fine-grained row-level locking (single writer model)
- No lazy loading of hbtrie nodes in the current code (BnodeCache loads on demand via `readNode`, but all accessed nodes stay cached)
- No concurrent multi-writer support

## Design: Page-Based Persistence for WaveDB

### Core Principles

1. **Copy-on-Write (CoW)**: Modified nodes are written to new locations; old locations are marked stale. No in-place updates.
2. **Incremental persistence**: Dirty nodes are flushed incrementally, not in a batch on snapshot/destroy.
3. **Lazy loading**: hbtrie_nodes are loaded from disk on demand; cold nodes can be evicted.
4. **Block-aligned storage**: Nodes are stored in a single database file divided into fixed-size blocks (4KB). Variable-size nodes can span multiple blocks.
5. **Stale region tracking**: Replaces fragment lists. Stale regions are tracked per revision; reclaimed when threshold is exceeded.
6. **Node-level granularity**: Each bnode is independently stored and loaded. This aligns with the per-node seqlock granularity in the in-memory data structure.

### Architecture

```
User API (database_put / database_get / database_delete)
         |
         v
  +------ WAL (thread-local, append-only) ------+
  |  Per-operation durability (as today)        |
  |  CBOR-encode [path, value]                  |
  |  writev() + debounce/immmediate fsync       |
  +----------------------------------------------+
         |
         |  WAL flush or incremental dirty write
         v
  +------ HBTrie (in-memory) ------------------+
  |  hbtrie_node_t (seqlock per node)           |
  |  bnode_t (write_lock per node)              |
  |  is_dirty flag per node                     |
  +----------------------------------------------+
         |
         |  Dirty node flush (write-behind)
         v
  +------ BnodeCache ---------------------------+
  |  Sharded cache (clean + dirty nodes)         |
  |  Per-shard spin locks                        |
  |  LRU eviction of clean nodes                 |
  |  Dirty nodes flushed sequentially            |
  +----------------------------------------------+
         |
         |  Flush to disk
         v
  +------ Page File (append-only) --------------+
  |  Block-aligned (4KB)                        |
  |  Nodes written at end of file (CoW)          |
  |  Stale regions tracked per revision          |
  |  Block metadata suffix (16B per block)       |
  +----------------------------------------------+
         |
         v
  +------ Index Pointer (superblock) ----------+
  |  Root node location                         |
  |  Revision number                            |
  |  CRC32 checksum                             |
  +----------------------------------------------+
```

### Components

#### 1. Page File (`page_file.h` / `page_file.c`)

A single append-only file divided into fixed-size blocks.

**On-disk layout:**
```
Block 0:  [Superblock: magic + version + root_offset + root_size + revnum + CRC32]
Block 1:  [Bnode data...] [IndexBlkMeta: next_bid + revnum_hash + marker=0xFF]
Block 2:  [Bnode data (cont'd if node spans blocks)...] [IndexBlkMeta]
Block 3:  [Bnode data...] [IndexBlkMeta]
...
Block N:  [Header: root_offset + revnum + stale_stats + CRC32]
```

- **Block size**: 4096 bytes (configurable 1KB-128KB)
- **Block metadata**: 16-byte `IndexBlkMeta` suffix at end of each block:
  - `next_bid` (8 bytes): Links to next block for multi-block nodes
  - `revnum_hash` (2 bytes): Superblock revision hash
  - `reserved` (5 bytes)
  - `marker` (1 byte): 0xFF for bnode blocks
- **Superblock**: First block, contains current root location and revision number. Written atomically on each commit.
- **Header blocks**: Periodic commit markers with root location. ForestDB keeps multiple (5 by default) for crash recovery.

**Operations:**
- `page_file_alloc_block()`: Allocate a new block at end of file (increment block count)
- `page_file_write_node()`: Write a bnode's serialized data spanning 1+ blocks, returning the starting offset
- `page_file_read_node()`: Read a bnode from a given offset (read first 4 bytes for size, then read remaining)
- `page_file_mark_stale()`: Mark a region as stale (offset, length)
- `page_file_get_reusable_blocks()`: Return list of stale regions above reuse threshold

#### 2. BnodeCache (`bnode_cache.h` / `bnode_cache.c`)

Replaces `sections_t` + `section_t` + `database_lru_cache_t` with a unified cache.

**Architecture:**
- **Global singleton** (or per-database instance) managing cached bnodes
- **Per-file shard** with per-shard spin locks
- Each shard contains:
  - `clean_nodes`: LRU list of clean (loaded from disk) nodes
  - `dirty_nodes`: map of offset -> Bnode* for dirty (modified) nodes
  - `all_nodes`: hash map of offset -> Bnode* for fast lookup
- **Dirty flush**: Nodes are flushed sequentially in ascending offset order via `writeCachedData()`
- **Eviction**: LRU with second-chance algorithm. Selects least-recently-accessed file, evicts clean nodes. Nodes with refcount > 0 are protected.

**Key difference from current LRU cache**: The BnodeCache caches **bnode_t** objects (serialized from disk), not just value identifiers. This enables lazy loading: when an hbtrie_node is not in memory, it can be loaded from the BnodeCache (or from disk on cache miss).

#### 3. Dirty Node Tracking

**In `hbtrie_node_t`:**
- `is_dirty` flag: Set to 1 when the node's btree is modified (insert, delete, split, GC)
- `disk_offset`: File offset where this node was last written (or BLK_NOT_FOUND if not yet persisted)

**In `bnode_t`:**
- `is_dirty` flag: Set when the bnode is modified

**Flush trigger:**
- **Write-behind**: After a write operation completes, if the dirty node count exceeds a threshold (e.g., 64 nodes), flush dirty nodes asynchronously
- **On commit/snapshot**: Flush all dirty nodes synchronously
- **On destroy**: Flush all dirty nodes, then seal WAL

**Flush algorithm:**
1. Collect all dirty nodes from `hbtrie_node_t->is_dirty` (walk the trie)
2. For each dirty node:
   a. Assign a new offset via `BnodeMgr::assignDirtyNodeOffset()` (append to page file)
   b. Serialize the node's btree (V1 format: section_id + block_index references for child hbtrie_nodes)
   c. Write to BnodeCache as dirty
   d. Mark old offset as stale
   e. Update `disk_offset` and clear `is_dirty`
3. Flush BnodeCache dirty nodes to disk (sequential write)
4. Write superblock with new root location

#### 4. Lazy Loading

**When a child hbtrie_node is accessed:**
1. Check `entry->child != NULL && entry->child->is_loaded`
2. If loaded: use it directly
3. If not loaded (`entry->child == NULL` and `entry->child_disk_offset != 0`):
   a. Read from BnodeCache / page file at `entry->child_disk_offset`
   b. Deserialize bnode tree
   c. Create `hbtrie_node_t` wrapper, set `is_loaded = 1`
   d. Wire up `entry->child` pointer

**Eviction:**
1. When memory exceeds a threshold, select LRU hbtrie_nodes
2. For each victim:
   a. If `is_dirty`: flush to disk first
   b. Set `is_loaded = 0`, record `disk_offset`
   c. Free the btree, set `entry->child = NULL`
   d. Keep `child_disk_offset` in parent entry for future reload

**V1 format requirement**: Lazy loading requires that child hbtrie_nodes are stored as offset references (not inlined). We must revert from V2 inline format to V1 offset-reference format, or use a hybrid: inline internal bnodes (which are always loaded together anyway) but reference child hbtrie_nodes by offset.

#### 5. Stale Region Management

Replaces fragment lists with a simpler stale region tracker.

**`stale_region_t`**: `{ offset, length }`

**StaleDataManager:**
- Maintains a list of stale regions per commit revision
- When a node is replaced (CoW), its old region is added as stale
- Stale regions are gathered and merged into reusable block lists when the stale ratio exceeds a threshold (e.g., 50%)
- Reusable blocks are returned from `getReusableBlocks()` for new allocations
- On compaction: all live data is copied to a new file, stale regions disappear

**Key advantage over fragment lists:**
- No CBOR serialization of free-space metadata
- No sorting by size (stale regions are position-based for sequential I/O)
- Coalescing is done lazily during `gatherRegions()` (merge adjacent regions)
- No separate meta file per section

#### 6. Serializer Changes

**V1 format (magic 0xB3)** with modifications:
- Each bnode is independently serialized (no recursive inline of child bnodes)
- Child hbtrie_nodes are stored as `{file_offset: uint64}` instead of `{section_id + block_index}`
- Internal child bnodes (is_bnode_child) are still serialized inline (they're part of the same hbtrie_node and always loaded together)
- This enables:
  - Lazy loading of hbtrie_nodes (read a single serialized blob from a known offset)
  - Independent persistence of each hbtrie_node's btree

#### 7. Recovery

1. Read superblock (block 0) for last valid root offset and revision
2. If superblock is corrupt, scan blocks in reverse to find last valid header
3. Load root node from root offset
4. Lazy-load child nodes on access (or eagerly if desired)
5. Replay WAL entries with txn_id > last committed revision

### Granularity Alignment

| Component | Granularity | Lock |
|-----------|-------------|------|
| User writes | Per-path hash (64 shards) | `write_locks[shard]` |
| HBTrie node reads | Per-node | Seqlock (optimistic) |
| HBTrie node writes | Per-node | `write_lock` |
| Bnode persistence | Per-hbtrie-node (one btree blob) | Node write_lock held during serialization |
| Bnode cache | Per-shard (11 shards) | Spin lock per shard |
| Page file writes | Sequential append | No lock (single writer or atomic offset) |
| Stale region tracking | Per-revision | No lock needed (append-only) |

**Key alignment**: Node-level persistence matches node-level locking. A write that modifies one hbtrie_node only triggers persistence of that one node. This eliminates the full-trie-walk bottleneck.

### Concurrent Write Model

**Current WaveDB**: Multiple concurrent writers with sharded write locks (64 shards). This is more concurrent than ForestDB's single-writer model.

**Proposed**: Keep multiple concurrent writers, but serialize persistence:

1. **Writes**: Multiple threads can write to the WAL and modify in-memory HBTrie concurrently (as today, via sharded write locks)
2. **Dirty marking**: When a thread modifies an hbtrie_node, it sets `is_dirty = 1` under the node's existing write_lock
3. **Flush serialization**: A single flusher thread (or the commit path) collects and writes dirty nodes. This is safe because:
   - Node serialization happens under the node's write_lock
   - Page file writes are append-only (atomic offset advancement)
   - Superblock writes are atomic (single block)
4. **Reads during flush**: Readers using optimistic seqlocks continue unimpeded. They see the in-memory version (which is always at least as fresh as the on-disk version).

This gives us **concurrent writers for in-memory operations** while maintaining **serializable persistence** — a significant improvement over ForestDB's single-writer model.

### Migration Plan

#### Phase 1: Page File and BnodeCache (Infrastructure)
1. Implement `page_file.h` / `page_file.c` (block allocation, node read/write, stale regions)
2. Implement `bnode_cache.h` / `bnode_cache.c` (sharded cache, dirty tracking, flush)
3. Write unit tests for page file and bnode cache
4. Benchmark: raw page file I/O throughput, cache hit/miss rates

#### Phase 2: Serializer and Dirty Tracking
1. Modify `node_serializer.c` to use V1 format with file_offset references
2. Add `disk_offset` field to `hbtrie_node_t` and `bnode_entry_t`
3. Set `is_dirty = 1` in all write paths (hbtrie_insert, hbtrie_delete, bnode_split, GC)
4. Implement dirty node collection and flush
5. Update `save_index_sections()` to use page file instead of sections
6. Write unit tests for serialization round-trip
7. Benchmark: snapshot cost, flush throughput

#### Phase 3: Lazy Loading
1. Implement lazy loading in `load_node_from_section()` (renamed to `load_node_from_page()`)
2. Implement node eviction in BnodeCache
3. Update all traversal code to check `is_loaded` before accessing child nodes
4. Write unit tests for lazy loading and eviction
5. Benchmark: memory usage with lazy loading, startup time

#### Phase 4: Integration and Cleanup
1. Replace sections with page file in `database_create` / `database_destroy`
2. Update WAL recovery to work with new page file format
3. Remove old section infrastructure (`section.c`, `sections.c`, fragment lists)
4. Add stale region compaction
5. Full test suite pass
6. Benchmark: concurrent throughput, memory usage, snapshot latency

### Testing Requirements

Memory leak detection must be integrated at every testing stage:

1. **Unit tests**: Every test for page_file, bnode_cache, stale_region, and serializer must run under AddressSanitizer (ASan) with leak detection enabled. Tests must create and destroy structures in isolation and verify zero leaks.
2. **Integration tests**: Full database lifecycle tests (create → put → get → snapshot → destroy) must be run under ASan. Every `database_create` must have a matching `database_destroy` that produces zero leaks.
3. **Benchmark tests**: Before and after each benchmark run, verify heap growth is bounded. Use Valgrind or ASan leak checks on benchmark binaries.
4. **Lazy loading tests**: Eviction + reload cycles must show zero net allocations. Load a trie, evict nodes, reload them, destroy — all with leak checking.
5. **CoW tests**: Modify a node, flush, verify old offset is stale, verify new offset is correct, destroy — check for leaks in stale region tracking.
6. **Crash recovery tests**: Write dirty nodes, write superblock, simulate crash (skip destroy), reopen and verify data integrity with zero leaks.

### Expected Improvements

| Metric | Current | Target | Method |
|--------|---------|--------|--------|
| Snapshot cost | ~50ms (10K keys) | <5ms | Incremental dirty node flush |
| Destroy cost | ~100ms (10K keys) | <10ms | No full-trie-walk needed |
| Memory usage | Unbounded (eager load) | Bounded (LRU eviction) | Lazy loading + BnodeCache |
| Write throughput during flush | Drops ~17% post-snapshot | No impact | Dirty nodes flushed async |
| Concurrent write model | 64 shards | 64 shards (unchanged) | Persistence is async |
| Stale space reclamation | Fragment list + defrag (O(n) walk) | Stale regions + block reuse | Simpler, more efficient |

### Resolved Design Decisions

1. **Block size**: Configurable with 4KB default. Aligns with filesystem page size and SSD erase blocks. Node size limit should be expressed as serialized size (not in-memory size) and default to a value that keeps most nodes within a single block (~3KB serialized, leaving room for block metadata). Both `page_file_block_size` and `btree_node_size` should be configurable at database creation time.

2. **Superblock atomicity**: Configurable superblock count, default 2 (dual alternating superblocks). Superblocks are written round-robin at fixed positions in the file (blocks 0..N-1). Recovery reads all copies and picks the one with the highest revision number and valid CRC. The space cost is negligible (2 × 4KB = 8KB).

3. **Dirty flush frequency**: Background write-behind with dirty threshold. A flusher (running on the existing timing wheel worker pool) triggers when dirty nodes exceed a threshold (default: 64 nodes or 1MB of dirty data). Explicit `database_snapshot()` flushes remaining dirty nodes synchronously. The WAL provides per-operation durability between flushes, so unflushed dirty nodes are recoverable via WAL replay.

4. **Serializer format (Phase 1)**: Hybrid format — internal bnodes (`is_bnode_child`) within a single hbtrie_node are inlined (V2-style), while child hbtrie_nodes are stored as file offset references (V1-style). This gives one blob per hbtrie_node (one read per trie level). **Phase 2** (future) will implement fully flat per-bnode storage where every bnode is independent, enabling per-bnode CoW and per-bnode locking. Phase 2 will be benchmarked against Phase 1 to measure write amplification reduction and hot-prefix concurrency gains.

5. **Concurrent page file writes**: Single flusher thread for Phase 1. The flusher collects dirty nodes, assigns offsets, and writes them sequentially (optimal for both HDD and SSD I/O patterns). Runs on the existing timing wheel worker pool. If profiling shows the flusher is a bottleneck, we can evolve to atomic offset advancement with `compare_exchange_strong()` for concurrent allocation