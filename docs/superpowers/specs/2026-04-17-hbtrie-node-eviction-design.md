# hbtrie_node Eviction — Callback-Driven Lazy Unloading

**Date:** 2026-04-17
**Status:** Design

## Problem

The `is_loaded` field on `hbtrie_node_t` is always set to 1 and never read — a write-only stub. There is no mechanism to unload hbtrie_nodes from memory. Once loaded, nodes stay in memory forever, consuming unbounded memory for large databases.

The bnode_cache has an LRU eviction mechanism, but it operates independently of the in-memory hbtrie tree. When the bnode_cache evicts a bnode that a parent entry still points to via `entry->child_bnode`, that pointer becomes dangling.

## Solution

Use the bnode_cache's existing LRU as the eviction policy. Add an eviction callback that queues evicted offsets for deferred processing. A background task nulls parent entry pointers and frees bnode memory. The `is_loaded` flag becomes a real indicator of whether an hbtrie_node's btree is valid in memory.

**Key design principle:** No new lock contention on read or write paths. The bnode_cache's existing shard locks drive eviction. Tree pointer cleanup happens asynchronously.

## Architecture

```
                    Database
                      |
        +-------------+-------------+
        |             |             |
   hbtrie_t      bnode_cache_t   work_pool_t
   (tree)        (raw data)     (async tasks)
        |             |              |
   hbtrie_node_t  evict_cb --> eviction_queue
   (is_loaded)    (existing    (lock-free
                   LRU)        ring buffer)
                                  |
                          background task
                          (null pointers,
                           deferred free)
```

## Components

### 1. Eviction callback on `file_bnode_cache_t`

Add a callback to `file_bnode_cache_t` that fires when a bnode is evicted from the cache's LRU:

```c
typedef void (*bnode_evict_fn)(uint64_t disk_offset, void* user_data);

struct file_bnode_cache_t {
    // ... existing fields ...
    bnode_evict_fn on_evict;         // Called when a bnode is evicted
    void* on_evict_data;             // User data for the callback
};
```

When `evict_if_needed` selects a victim, it calls `fcache->on_evict(victim->offset, fcache->on_evict_data)` **before** freeing the item. The callback does NOT free memory — it defers that to the background task.

**Why a callback:** Keeps `src/Storage/bnode_cache.c` decoupled from hbtrie. The database layer wires the callback during initialization.

### 2. Deferred free — eviction queue

The callback pushes evicted offsets into a lock-free ring buffer. The bnode_cache **does not free** the evicted item's data immediately — it marks it as `evict_pending` and leaves it in the shard. The background task confirms when pointers are nulled, then the data is freed.

```c
#define EVICTION_QUEUE_CAPACITY 256

typedef struct eviction_queue_t {
    _Atomic uint64_t head;
    _Atomic uint64_t tail;
    uint64_t offsets[EVICTION_QUEUE_CAPACITY];
} eviction_queue_t;
```

Operations:
- `eviction_queue_push(queue, offset)` — lock-free, called from bnode_cache callback
- `eviction_queue_drain(queue, out, max)` — bulk drain for background task

### 3. Deferred free tracking on bnode_cache_item_t

Add a flag to `bnode_cache_item_t`:

```c
uint8_t evict_pending;    // 1 when evicted but data not yet freed
```

When `evict_if_needed` selects a victim:
1. Remove from hash map and LRU list (existing behavior)
2. Set `evict_pending = 1`
3. Call `on_evict` callback
4. **Do NOT free** `item->data` or `item` — leave them allocated

The background task, after nulling all parent pointers:
1. Calls `bnode_cache_complete_evict(fcache, offset)` which frees the data and item

This ensures no dangling pointers: the bnode memory stays allocated until the tree no longer references it.

### 4. Background eviction task in the work pool

A background task that runs periodically (scheduled via the timing wheel):

1. Drain the eviction queue
2. For each evicted offset, walk the hbtrie tree to find the parent entry:
   - Check `entry->child_disk_offset == evicted_offset`
   - If match and `entry->child != NULL` (hbtrie child): NULL `entry->child`, set `entry->child->is_loaded = 0`
   - If match and `entry->child_bnode != NULL` (bnode child): NULL `entry->child_bnode`
   - If match and `entry->trie_child != NULL`: NULL `entry->trie_child`, set `entry->trie_child->is_loaded = 0`
3. After nulling all references, call `bnode_cache_complete_evict(fcache, offset)` to free the deferred data

**Parent finding:** Walk from the root. For each hbtrie_node, check all entries. O(n) but eviction is infrequent and runs in a background thread. No lock on the hot path.

**Locking:** The background task acquires the parent hbtrie_node's write_lock (seqlock) only when modifying an entry. The tree walk itself uses optimistic reads (seqlock even=stable check).

### 5. Access-time checks

On every traversal (find, insert, delete, cursor), before dereferencing `entry->child` or `entry->child_bnode`:

- If `entry->child == NULL && entry->child_disk_offset != 0`: call the existing `bnode_entry_lazy_load_hbtrie_child` or `bnode_entry_lazy_load_trie_child` function
- If `entry->child_bnode == NULL && entry->child_disk_offset != 0`: call `bnode_entry_lazy_load_bnode_child`

These checks already exist in the lazy-load functions. The key change is making sure all traversal paths call the lazy-load functions before accessing child pointers.

**Current gaps:**
- Read paths (hbtrie_find_mvcc) need to check `is_loaded` after getting a child reference
- Cursor paths need to handle unloaded nodes during DFS traversal
- Write paths (hbtrie_insert, hbtrie_delete) already use `ensure_btree_loaded` which handles this

### 6. `is_loaded` semantics

| Value | Meaning | btree field | disk_offset |
|-------|---------|-------------|-------------|
| 1 | In memory, valid | Valid pointer | May be set (persisted) or UINT64_MAX (not yet) |
| 0 | Evicted, on disk only | NULL | Valid file offset |

Setting `is_loaded = 0`:
- Background eviction task: after nulling parent entry's child pointer and freeing the btree

Setting `is_loaded = 1`:
- `hbtrie_node_create`: newly created node
- `bnode_entry_lazy_load_*`: after loading from disk
- `hbtrie_load_child_node`: after loading from disk

Reading `is_loaded`:
- Before accessing `hbnode->btree` in traversal paths
- In `hbtrie_node_destroy` to skip btree walk for unloaded nodes

## No New Lock Contention

This design adds **zero lock acquisitions** on the read or write paths:

- The bnode_cache's existing shard locks drive eviction (no change)
- The eviction callback pushes to a lock-free ring buffer (no lock)
- The background task processes the queue asynchronously (no hot-path lock)
- `is_loaded` checks on traversal use the existing seqlock protocol (already in place)
- Lazy-load calls already exist and use the existing spinlock/seqlock protocol

## Files to Modify

1. **`src/Storage/bnode_cache.h`** — Add `on_evict`/`on_evict_data` to `file_bnode_cache_t`, add `evict_pending` to `bnode_cache_item_t`
2. **`src/Storage/bnode_cache.c`** — Wire callback in `evict_if_needed`, add `bnode_cache_complete_evict`, defer free when `evict_pending`
3. **`src/Database/database.h`** — Add `eviction_queue_t` field
4. **`src/Database/database.c`** — Create eviction queue, wire callback, submit background task
5. **`src/HBTrie/hbtrie.c`** — Add `is_loaded` checks in traversal paths, add tree-walk-for-eviction helper
6. **`src/HBTrie/hbtrie.h`** — Declare `hbtrie_null_entries_by_offset` helper
7. **New: `src/Database/eviction_queue.h`** — Lock-free ring buffer header
8. **New: `src/Database/eviction_queue.c`** — Lock-free ring buffer implementation

## Edge Cases

- **Root node:** The root hbtrie_node should never have its btree evicted (entry point). The bnode_cache's LRU naturally protects it since it's frequently accessed.
- **Dirty nodes:** `evict_if_needed` already skips dirty items (`victim->is_dirty`). The bnode_cache flushes dirty items before they become eviction candidates.
- **Concurrent access:** The background task acquires the parent node's write_lock before nulling the child pointer. Between eviction and nulling, the deferred free ensures the bnode memory is still valid (no crash), just stale.
- **Cursor traversal:** Cursors hold references (refcount on hbtrie_nodes), which prevents the background task from freeing a node that a cursor is visiting.
- **Queue overflow:** If the ring buffer fills, the callback falls back to blocking (rare case). The queue drains every few seconds so overflow is unlikely.

## Performance Impact

- **Read/write path overhead:** Zero — no new locks, no LRU touch calls
- **Eviction callback cost:** Lock-free push to ring buffer (~5ns)
- **Background task cost:** Tree walk to null pointers, runs asynchronously
- **Deferred free cost:** Small memory overhead for evicted-but-not-yet-freed bnodes
- **Reload cost:** First access after eviction pays a disk read + deserialize. Subsequent accesses hit the bnode_cache.
- **Memory savings:** Evicted hbtrie_nodes free their btree memory, bounded by bnode_cache max_memory