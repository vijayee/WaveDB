# Phase 2: Fully Flat Per-Bnode Storage — Design Spec

**Date:** 2026-04-15
**Branch:** experimental-persistence-phase2
**Depends on:** Phase 1 (page_file, bnode_cache, stale_region — complete)
**Parent spec:** `docs/superpowers/specs/2026-04-15-page-based-persistence-design.md`

## Goal

Replace Phase 1's hybrid format (one blob per hbtrie_node with inlined internal bnodes) with fully flat per-bnode storage where **every bnode is independently stored** on disk. This enables:

1. **Per-bnode CoW** — modifying one leaf bnode rewrites only the bnodes on the path from leaf to root, not the entire hbtrie_node blob
2. **Per-bnode locking** — finer-grained concurrency for hot-prefix scenarios
3. **Per-bnode lazy loading** — load only the bnodes actually traversed, not entire btree blobs
4. **Reduced write amplification** — from O(total_bnodes_per_hbtrie_node) to O(btree_height + trie_depth) per modification

## Current State (Phase 1)

Phase 1 implemented the infrastructure (page_file, bnode_cache, stale_region) but the database still uses the section-based persistence layer. The serializer uses V2 format (magic 0xB4) which recursively inlines internal bnodes, producing one blob per hbtrie_node.

## Phase 2 Changes

### 1. V3 Serialization Format (Flat Per-Bnode)

Each bnode is independently serialized as a self-contained blob. No bnode is inlined inside another.

**Wire format:**
```
[1 byte: magic 0xB5]
[2 bytes: level (uint16, network order)]
[2 bytes: num_entries (uint16, network order)]
For each entry:
  [chunk_size bytes: key data]
  [1 byte: flags]
    bit 0: has_value
    bit 1: is_bnode_child
    bit 2: has_versions
  if has_value:
    if has_versions:
      [2 bytes: version_count]
      for each version:
        [1 byte: is_deleted]
        [24 bytes: txn_id (time:u64 + nanos:u64 + count:u64)]
        [4 bytes: value_len] [value_len bytes: value data]
    else:
      [4 bytes: value_len] [value_len bytes: value data]
  else if is_bnode_child:
    [8 bytes: child_disk_offset]   // NEW: file offset of child bnode
  else:
    [8 bytes: child_disk_offset]   // NEW: file offset of child hbtrie_node's root bnode
```

**Key differences from V2:**
- `is_bnode_child` entries store `child_disk_offset` (8 bytes) instead of inline child bnode data
- Child hbtrie_node references use `child_disk_offset` (8 bytes) instead of `section_id` + `block_index` (16 bytes)
- No recursive inline serialization — every bnode is a self-contained blob
- The page file's 4-byte size prefix (written by `page_file_write_node`) wraps each serialized bnode

### 2. Data Structure Changes

**hbtrie_node_t** — add disk tracking fields:
```c
typedef struct hbtrie_node_t {
    refcounter_t refcounter;
    _Atomic(uint64_t) seq;
    PLATFORMLOCKTYPE(write_lock);
    bnode_t* btree;
    uint16_t btree_height;

    // NEW: page file storage tracking (replaces section_id + block_index)
    uint64_t disk_offset;       // File offset of root bnode (BLK_NOT_FOUND if not persisted)
    uint8_t is_dirty;           // 1 if this node's btree has been modified since last flush
    uint8_t is_loaded;          // 1 if in memory, 0 if on-disk stub

    // REMOVED: storage, section_id, block_index, data_size
} hbtrie_node_t;
```

The hbtrie_node's `disk_offset` is the same as its root bnode's `disk_offset`. There is no separate serialized hbtrie_node record on disk — the hbtrie_node is just a wrapper around its root bnode.

**bnode_t** — add disk tracking fields:
```c
typedef struct bnode_t {
    refcounter_t refcounter;
    _Atomic(uint16_t) level;
    uint32_t node_size;
    vec_t(bnode_entry_t) entries;
    _Atomic(uint64_t) seq;
    PLATFORMLOCKTYPE(write_lock);

    // NEW: per-bnode disk tracking
    uint64_t disk_offset;       // File offset of this bnode (BLK_NOT_FOUND if not persisted)
    uint8_t is_dirty;           // 1 if modified since last write
} bnode_t;
```

**bnode_entry_t** — add child_disk_offset, remove section-based fields:
```c
typedef struct bnode_entry_t {
    // ... existing fields unchanged ...

    // CHANGED: replace section_id + block_index with single offset
    uint64_t child_disk_offset;  // File offset of child bnode or hbtrie_node root (0 = not on disk)

    // REMOVED: child_section_id, child_block_index
} bnode_entry_t;
```

**node_location_t** — updated to use single offset:
```c
typedef struct {
    uint64_t offset;     // File offset in page file (replaces section_id + block_index)
} node_location_t;
```

### 3. CoW Propagation (Per-Bnode)

When a bnode is modified, CoW propagates from the modified bnode up to the database root:

**Write path:**
1. Modify a leaf bnode → `bnode.is_dirty = 1`
2. Flush phase:
   a. Process dirty bnodes from leaves to root (sorted by level ascending)
   b. For each dirty bnode:
      - Serialize with V3 format (children already have correct `child_disk_offset`)
      - Write to page file at new offset via `page_file_write_node`
      - Mark old offset as stale via `page_file_mark_stale`
      - Update `bnode.disk_offset = new_offset`
      - Clear `bnode.is_dirty`
   c. After writing a bnode, update the parent entry's `child_disk_offset = new_offset`
   d. The parent bnode is now dirty (its entry changed) — it will be processed in the next level
3. After all bnodes are flushed:
   - Update `hbtrie_node.disk_offset = root_bnode.disk_offset`
   - Update parent hbtrie_node entry's `child_disk_offset`
   - Continue up the trie
4. Write superblock with new root offset

**Dirty marking in write paths:**
- `hbtrie_insert` → mark target leaf bnode + all ancestor bnodes dirty
- `hbtrie_delete` → mark target leaf bnode + all ancestor bnodes dirty
- `bnode_split` → mark split node + new sibling + parent dirty
- `bnode_propagate_split` → mark all affected bnodes dirty
- MVCC version updates → mark leaf bnode dirty

### 4. Lazy Loading (Per-Bnode)

**When a child bnode is accessed during traversal:**
1. Check `entry->child_bnode != NULL` → use it directly
2. If NULL and `entry->child_disk_offset != 0`:
   a. Read from bnode_cache/page_file at `entry->child_disk_offset`
   b. Deserialize with V3 deserializer → get bnode_t
   c. Set `entry->child_bnode = bnode`
   d. The bnode's entries may also have NULL children with non-zero `child_disk_offset`
   e. Those will be lazily loaded on demand when traversed

**When a child hbtrie_node is accessed:**
1. Check `entry->child != NULL && entry->child->is_loaded` → use it
2. If NULL and `entry->child_disk_offset != 0`:
   a. Read root bnode from `entry->child_disk_offset`
   b. Create hbtrie_node_t wrapper with `btree = root_bnode`, `is_loaded = 1`
   c. Set `entry->child = hbtrie_node`

**Eviction:**
1. Select LRU bnodes for eviction (clean only, ref_count == 0)
2. For each victim bnode:
   a. If dirty: flush to disk first
   b. The parent entry's `child_disk_offset` already has the correct value
   c. Free the bnode_t
   d. Set parent entry's `child_bnode = NULL` (or `child = NULL` for hbtrie_node children)
3. For evicted hbtrie_nodes:
   a. Set `is_loaded = 0`
   b. Free btree (all bnodes in the btree)
   c. Set parent entry's `child = NULL`
   d. `child_disk_offset` still valid for future reload

### 5. BnodeCache Integration

The Phase 1 BnodeCache already stores serialized byte buffers keyed by file offset. Phase 2 uses it identically but at finer granularity:

- Each bnode gets its own cache entry
- Cache keys are file offsets (from `bnode.disk_offset` or `entry.child_disk_offset`)
- Cache values are serialized bnode bytes (including 4-byte size prefix from page_file)
- On read: `bnode_cache_read` → deserialize → bnode_t
- On write: serialize → `bnode_cache_write` → flush to page file
- Cache eviction evicts individual bnodes (not entire hbtrie_node blobs)

### 6. Database Lifecycle Changes

**database_create (with existing DB):**
1. Create page_file and bnode_cache (instead of sections)
2. Read superblock for root offset
3. Load root bnode from root offset
4. Create root hbtrie_node wrapper
5. Child bnodes/hbtrie_nodes loaded lazily on access

**database_snapshot:**
1. Flush all dirty bnodes (incremental CoW)
2. Write superblock with new root offset
3. No full-trie-walk needed

**database_destroy:**
1. Flush remaining dirty bnodes
2. Write final superblock
3. Destroy bnode_cache and page_file
4. Free in-memory trie

### 7. Write Amplification Comparison

| Scenario | Phase 1 (hybrid blob) | Phase 2 (flat per-bnode) |
|----------|----------------------|--------------------------|
| Modify 1 leaf in btree of height 3, 100 bnodes | Rewrite entire btree blob (~100 bnodes) | Rewrite 3 bnodes (leaf → internal → root) |
| Modify 1 leaf at trie depth 4 | Rewrite 4 hbtrie_node blobs | Rewrite 4 bnodes (one per trie level) |
| Split a full leaf | Rewrite entire parent btree blob | Rewrite leaf + sibling + parent (3 bnodes) |

For btrees with many bnodes, Phase 2 reduces write amplification from O(N) to O(H) where N = total bnodes in btree and H = btree height.

### 8. Concurrency Benefits

Phase 2 enables per-bnode locking for writes:
- In Phase 1, all writes to an hbtrie_node's btree are serialized by the node's write_lock
- In Phase 2, writes to different bnodes in the same btree can proceed concurrently
- Hot-prefix contention (many writes targeting the same hbtrie_node but different leaf bnodes) is reduced
- The bnode_t's existing `write_lock` provides the per-bnode serialization

### 9. Removed Components

After Phase 2 integration:
- `sections_t` / `section_t` — replaced by page_file
- `section_id` / `block_index` fields — replaced by `disk_offset`
- `child_section_id` / `child_block_index` — replaced by `child_disk_offset`
- `save_index_sections` — replaced by `database_flush_dirty_bnodes`
- `load_index_sections` / `load_node_from_section` — replaced by lazy loading
- V2 serializer (inline bnodes) — replaced by V3 flat serializer

### 10. Testing Requirements

All Phase 2 tests must run under ASan with zero leaks:

1. **V3 serializer**: Round-trip serialize/deserialize for all bnode types (leaf, internal, with versions, with hbtrie_node children)
2. **CoW propagation**: Modify a leaf, flush, verify old offsets are stale, verify new offsets are correct
3. **Lazy loading**: Load a bnode from disk on demand, verify data matches
4. **Eviction**: Evict a clean bnode, reload it, verify data matches, zero net allocations
5. **Crash recovery**: Write dirty bnodes, write superblock, skip destroy, reopen, verify data
6. **Benchmarks**: Compare Phase 1 vs Phase 2 write amplification and concurrent throughput