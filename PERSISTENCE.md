# WaveDB Persistence Architecture

## Overview

WaveDB uses two co-existing persistence mechanisms:

1. **WAL (Write-Ahead Log)** — per-operation, append-only durability for crash recovery
2. **Section-based index persistence** — periodic snapshots of the HBTrie tree structure

Normal operations (PUT/DELETE) write only to the WAL. Section persistence happens only on explicit `database_snapshot()` or `database_destroy()`. Crash recovery replays the WAL.

## WAL (Write-Ahead Log)

### Architecture

- **Thread-local WAL files** (`thread_<tid>.wal`) — one per thread, eliminating write contention
- **Manifest file** (`manifest.dat`) tracks WAL states: ACTIVE, SEALED, COMPACTED
- **Background compaction** merges sealed WALs and marks them COMPACTED

### WAL Entry Format (per operation)

```
[type:1B][txn_id:24B][crc32:4B][data_len:4B][cbor_data:variable]
```

- type: `'p'` (PUT), `'d'` (DELETE), `'b'` (BATCH)
- txn_id: 24 bytes (time:8, nanos:8, count:8) in network byte order
- crc32: CRC32 of the CBOR data payload
- data_len: big-endian uint32
- cbor_data: CBOR-encoded [path, value] for PUT; [path] for DELETE

### WAL Sync Modes

| Mode | Behavior | Safety | Performance |
|------|----------|--------|-------------|
| IMMEDIATE | fsync() after every write | Maximum | Lowest |
| DEBOUNCED | fsync debounced via timing wheel (~100ms) | Good | Medium |
| ASYNC | No fsync, relies on OS page cache | Minimal | Highest |

### WAL Write Path

```
database_put_sync()
  -> tx_manager_begin()
  -> encode_put_entry()          // CBOR-encode [path, value]
  -> get_thread_wal()            // Get thread-local WAL
  -> thread_wal_write()
       -> wal_crc32(data)
       -> writev(fd, [header, data])  // Atomic append via O_APPEND
       -> fsync or debounce
  -> hbtrie_insert()             // Apply to in-memory trie
  -> tx_manager_commit()
```

### WAL Rotation

When a thread-local WAL exceeds `max_file_size` (default 128KB):
1. Current file is fsynced and closed
2. A new file is opened for that thread
3. Manifest entry updated to mark old file as SEALED
4. If sealed WALs exceed `max_sealed_wals` (default 10), writes block until compaction runs

### WAL Compaction

Background `wal_compactor_t` thread checks every 1 second. Compaction triggers when:
- Time since last write > `idle_threshold_ms` (default 10s), OR
- Time since last compaction > `compact_interval_ms` (default 60s)

Compaction merges sealed WAL files and marks them COMPACTED so they are skipped during recovery.

### WAL Recovery

At startup, `wal_manager_recover()`:
1. Reads manifest entries (skips COMPACTED files)
2. Scans directory for `thread_*.wal` files not in manifest
3. Reads all entries, sorts by `transaction_id`
4. Replays: deserialize CBOR, call `hbtrie_insert()` / `hbtrie_delete()`

## Section-Based Index Persistence

### Architecture

- **Section** — a single file containing variable-size records, with a separate CBOR metadata file for free-space tracking
- **Sections pool** — manages multiple sections with round-robin distribution, LRU cache, sharded checkout tracking
- **Index pointer file** (`index.wdbs`) — 37-byte binary file pointing to the root hbtrie_node

### On-Disk Record Format (within a section)

```
[transaction_id: 24B][data_size: 8B network-order][data: variable]
```

Header overhead: 32 bytes per record.

### Section Metadata

Fragment lists (free-space tracking) are stored in separate CBOR files per section. Each fragment is `[start, end]` as a CBOR array of two uint64s. The fragment list is a CBOR array of these pairs.

Metadata is **lazily persisted**: `meta_dirty` flag is set on writes, flushed on checkin/destroy.

### Fragment List

- Sorted array of fragments, sorted by **size ascending** (not position)
- O(log n) binary-search first-fit allocation
- No adjacent-fragment coalescing (TODO)
- Partial fragments are removed and re-inserted to maintain sort order

### Index Pointer File Format (index.wdbs)

```
[magic:4B "WDBS"][chunk_size:1B][btree_node_size:4B]
[root_section_id:8B][root_block_index:8B][root_data_size:8B]
[crc32:4B]
```

## Save Path: `save_index_sections()`

This is the **critical path** for persistence. Called on `database_snapshot()` and `database_destroy()`.

### Algorithm

1. **BFS collection**: Collect ALL hbtrie_nodes via breadth-first search from root, tracking parent-child relationships
2. **Reverse processing**: Process nodes in reverse order (children first, then parents):
   - If previously persisted: `sections_deallocate(old section, offset, data_size)`
   - Serialize the node's bnode tree: `bnode_serialize(node->btree, ...)`
   - Write to section: `sections_write(storage, txn_id, data_buf, &section_id, &offset)`
   - Update node: `section_id`, `block_index`, `data_size`, `is_dirty=0`
   - Update parent entry: `parent_entry->child_section_id`, `parent_entry->child_block_index`
3. **Save index pointer**: Write `index.wdbs` with root location

The reverse order is crucial: children must be written before parents so parent entries can store correct child locations in their serialized form.

### Granularity

**Per-HBTrie-node**: Each hbtrie_node's btree is serialized as a single blob and written to a section as a single record. A single `bnode_serialize` call serializes the **entire btree** under that hbtrie_node (V2 format inlines child bnodes recursively).

### Cost

For N hbtrie_nodes:
- N deallocations of old section records
- N serializations (each is a full btree walk)
- N section writes (each involves: lock, fragment allocation, lseek, writev, unlock)
- 1 index pointer write
- Plus: round-robin lock acquisition, section checkout/checkin, LRU cache operations

## Load Path: `load_index_sections()`

### Algorithm

1. Read `index.wdbs` to get root location
2. `load_node_from_section()` recursively:
   - Read serialized bnode from section
   - Deserialize (V2 format: recursively reconstructs inline child bnodes)
   - Create `hbtrie_node_t` wrapper
   - For each child hbtrie_node location (section_id != 0): load recursively
3. Wire up entry->child pointers

### Loading Strategy

**Eager**: All nodes in the tree are loaded into memory at startup. The `is_loaded` flag and `child_section_id`/`child_block_index` fields exist to support future lazy loading but it is not yet implemented.

## Bnode Serialization Format (V2 — Magic 0xB4)

### Structure

```
[magic:0xB4][level:uint16][num_entries:uint16]
For each entry:
  [chunk_data: chunk_size bytes][flags:uint8]
  flags: bit0=has_value, bit1=is_bnode_child, bit2=has_versions

  if has_value and has_versions:
    [version_count:uint16]
    for each version:
      [is_deleted:uint8][txn_id:24B][ident_len:uint32][ident_data]
  elif has_value and !has_versions:
    [ident_len:uint32][ident_data]
  elif !has_value and is_bnode_child (V2 inline):
    [child_bnode_size:uint32][child_bnode_data: recursive V2 bnode]
  elif !has_value and !is_bnode_child:
    [child_section_id:uint64][child_block_index:uint64]
```

### V2 Key Design: Inline Child Bnodes

V2 recursively serializes child bnodes (internal B+tree nodes) inline with a length prefix. This means one `bnode_serialize` call produces a single blob containing the **entire btree subtree** of internal nodes. Leaf-side child hbtrie_nodes are still stored as section_id + block_index references.

### V1 Format (Magic 0xB3)

V1 uses section_id + block_index references for bnode children instead of inlining. Not used in current writes.

## Sections Pool Concurrency

### Checkout/Checkin

- 16 sharded checkout locks (`section_id % 16`), each with a hashmap of `checkout_t` (section ref + count)
- `sections_checkout()`: increment refcount via sharded lock
- `sections_checkin()`: decrement refcount, destroy if zero

### Write Distribution

- **Round-robin**: cyclic linked list of section IDs, `round_robin_next()` picks the next section
- **Section rotation**: if a section is full, it's removed from round-robin and a new section is added
- All round-robin operations are protected by a single lock

### LRU Cache

- Evicts least-recently-used section references when full
- `sections_lru_cache_move()` on access promotes to front
- Not thread-safe (assumes single-threaded access via checkout)

## Defragmentation

### Section-Level Defragmentation

`section_defragment()`:
1. Acquire section lock
2. Build snapshot of current free fragments
3. Scan entire section file sequentially, reading record headers
4. Determine "dead" records (offset range falls within a free fragment)
5. Collect all live records
6. If already contiguous: just rebuild fragment list for the tail
7. Otherwise: read entire file into memory buffer, `memmove` live records to beginning, write buffer back, `ftruncate` file, rebuild fragment list

### Idle-Triggered Defrag

After 30 seconds of inactivity (no writes/deallocations), the defrag debouncer fires `sections_defrag_check()`. It scans all LRU-cached sections and defragments any where `free_space_ratio >= 0.5` (50% threshold).

### Offset Remapping

After defrag moves a record, `defrag_remap_callback()` walks the **entire in-memory HBTrie** to find and update matching `hbtrie_node_t->block_index` or `bnode_entry_t->child_block_index` entries. This is O(N) in the total number of trie entries per defragmented section.

## MVCC GC and Section Deallocation

- `version_entry_gc()` accepts a `sections_t*` parameter and deallocates section storage for removed versions
- `hbtrie_node_destroy()` deallocates from section storage if the node was previously persisted
- `hbtrie_gc()` deallocates section storage for all versions in removed entries

## Complete Persistence Lifecycle

### Startup

```
1. Create work pool and timing wheel
2. Create sections pool (loads/creates sections, round-robin, LRU cache)
3. Load index:
   a. If storage available: load_index_sections() -> read index.wdbs -> recursively load nodes
   b. Fallback: load monolithic CBOR index
4. Attach storage to trie root
5. Create transaction manager and WAL manager
6. Replay WAL: sort entries by txn_id, apply to trie
```

### Normal PUT

```
1. Begin MVCC transaction
2. CBOR-encode entry
3. Write to thread-local WAL (with chosen sync mode)
4. Acquire sharded write lock (64 shards, by path hash)
5. hbtrie_insert() -> modifies in-memory trie
6. Release write lock
7. Commit transaction
8. Update LRU cache
```

### Shutdown

```
1. Stop timing wheel and worker pool
2. Flush all thread-local WALs
3. database_persist() -> save_index_sections()
   a. Walk entire trie, serialize each node's btree
   b. Write each to a section (deallocating old location first)
   c. Save index pointer file
4. WAL seal and compact (mark entries as captured by snapshot)
5. Destroy WAL, LRU, trie, sections
```

## Identified Bottlenecks

### 1. Section-Level Mutex Contention

Each `section_t` has a single mutex protecting all operations (read, write, deallocate, metadata flush, defragmentation). The lock is held for the **entire I/O duration** including `lseek` + `writev`/`read` syscalls. Under concurrent workloads, multiple threads accessing the same section serialize completely.

### 2. Round-Robin Lock Contention

All `sections_write()` calls go through `round_robin_next()` which acquires a single global lock. Every concurrent write competes for this lock.

### 3. Save-Index Full Trie Walk

`save_index_sections()` is O(N) in total trie entries. It must collect all nodes (BFS), then serialize + write each. The V2 serializer inlines child bnodes, making each serialization a full btree walk. For large databases, this is extremely expensive.

### 4. V2 Serializer Blob Size

V2 inlines child bnodes into the parent's serialized blob. A single hbtrie_node's serialized output can be very large (entire btree subtree). This means larger section writes, more fragmentation, and more expensive deallocation/re-write cycles.

### 5. Eager Loading

`load_index_sections()` recursively loads **all** child hbtrie_nodes from sections on startup. No lazy loading. For large databases, this is slow and memory-intensive.

### 6. Defrag Full Trie Traversal

The `defrag_remap_callback()` walks the entire HBTrie for **each** defragmented section. If multiple sections are defragmented, this is O(sections × trie_size).

### 7. Fragment List Sorted by Size (Not Position)

Fragments are sorted by size ascending for first-fit allocation. This prevents efficient coalescing of adjacent fragments and makes position-based operations (like defrag scanning) require O(n) linear scans.

### 8. No Incremental Section Writes During Normal Operation

Section writes only happen on snapshot/shutdown. All per-key durability relies on the WAL. Section persistence is an all-or-nothing batch operation.