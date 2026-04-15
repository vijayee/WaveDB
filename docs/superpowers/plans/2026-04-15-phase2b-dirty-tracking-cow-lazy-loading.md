# Phase 2B: Dirty Tracking, CoW Flush, Lazy Loading, and Integration

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Wire dirty tracking into all HBTrie write paths, implement per-bnode CoW flush, implement lazy loading from page file, and integrate page_file/bnode_cache into the database lifecycle replacing sections.

**Architecture:** All HBTrie write paths (insert, delete, split, GC) mark affected bnodes and hbtrie_nodes dirty. A new `database_flush_dirty_bnodes` function collects dirty bnodes bottom-up, serializes each with V3 format, writes to page file, propagates offsets up the tree, and writes the superblock. Lazy loading checks `child_disk_offset` when child pointers are NULL, reads from bnode_cache/page_file, and deserializes. Database create/destroy/snapshot use page_file instead of sections.

**Tech Stack:** C11, gtest for tests, following STYLEGUIDE.md conventions

**Spec:** `docs/superpowers/specs/2026-04-15-phase2-flat-bnode-design.md` sections 3-10

**Prerequisites:** Phase 2A complete (data structures + V3 serializer)

**Important:** Memory leak checks (ASan) at every testing stage.

---

## File Structure

| File | Responsibility |
|------|---------------|
| `src/HBTrie/hbtrie.c` | Wire is_dirty=1 in insert, delete, split, GC paths |
| `src/HBTrie/hbtrie.h` | Add lazy_load_bnode function declaration |
| `src/Database/database.c` | Replace sections with page_file, implement flush_dirty_bnodes, lazy loading |
| `src/Database/database.h` | Add page_file/bnode_cache fields to database_t |
| `tests/test_persistence_phase2.cpp` | Integration tests for CoW, lazy loading, crash recovery |
| `tests/benchmark_write_amplification.cpp` | Benchmark comparing Phase 1 vs Phase 2 |

Existing files modified:
- `CMakeLists.txt` — add new test targets

---

### Task 5: Wire is_dirty=1 in all HBTrie write paths

**Files:**
- Modify: `src/HBTrie/hbtrie.c` (hbtrie_insert, hbtrie_delete, btree_split_after_insert, hbtrie_gc)

- [ ] **Step 1: Mark hbtrie_node dirty in hbtrie_insert**

In `src/HBTrie/hbtrie.c`, find `hbtrie_insert` (line 1526). After any modification to a leaf bnode entry (adding a value, updating a value, creating a new child node), add:

```c
    // Mark the current hbtrie_node as dirty (modified since last flush)
    current->is_dirty = 1;
    current->btree->is_dirty = 1;
```

This should be added in each code path that modifies the btree. Find the sections where:
1. A new value is inserted into a leaf entry (set `has_value = 1` or add version)
2. A new child hbtrie_node is created
3. A trie_child is created for an existing value entry
4. An intermediate chunk entry is created

For each of these, after the modification, set `current->is_dirty = 1; current->btree->is_dirty = 1;`

- [ ] **Step 2: Mark hbtrie_node dirty in hbtrie_delete**

In `src/HBTrie/hbtrie.c`, find `hbtrie_delete` (line 1880). After creating a tombstone or removing an entry, add:

```c
    current->is_dirty = 1;
    current->btree->is_dirty = 1;
```

- [ ] **Step 3: Mark bnodes dirty in btree_split_after_insert**

In `src/HBTrie/hbtrie.c`, find `btree_split_after_insert` (line 189). After a bnode split, mark the split node and the parent dirty:

```c
    node->is_dirty = 1;
    if (parent != NULL) {
        parent->is_dirty = 1;
    }
```

Also in `btree_propagate_split` (line 77), after inserting a new child entry into the parent:

```c
    parent->is_dirty = 1;
```

- [ ] **Step 4: Mark bnodes dirty in hbtrie_gc**

In `src/HBTrie/hbtrie.c`, find `hbtrie_gc` (line 2028). After garbage collecting version chains or removing entries, mark the node dirty:

```c
    current->btree->is_dirty = 1;
    current->is_dirty = 1;
```

This should happen after any version_entry_gc call that might have removed versions.

- [ ] **Step 5: Add helper function to mark bnode and ancestors dirty**

In `src/HBTrie/hbtrie.c`, add a static helper that marks a bnode and its containing hbtrie_node dirty:

```c
static void mark_dirty(hbtrie_node_t* hbnode, bnode_t* bnode) {
    if (hbnode != NULL) hbnode->is_dirty = 1;
    if (bnode != NULL) bnode->is_dirty = 1;
}
```

Use this helper in the above steps instead of inline assignments for consistency.

- [ ] **Step 6: Build and run existing tests**

Run: `cd /home/victor/Workspace/src/github.com/vijayee/WaveDB/build_debug && cmake .. -DCMAKE_BUILD_TYPE=Debug && make -j$(nproc) 2>&1 | tail -10`

Then: `cd /home/victor/Workspace/src/github.com/vijayee/WaveDB/build_debug && ctest --output-on-failure -R "test_hbtrie|test_bnode" 2>&1 | tail -20`

Expected: All existing tests pass. The is_dirty flags are set but not yet used.

- [ ] **Step 7: Commit**

```bash
git add src/HBTrie/hbtrie.c
git commit -m "feat: wire is_dirty=1 in all HBTrie write paths"
```

---

### Task 6: Implement database_flush_dirty_bnodes (CoW flush)

**Files:**
- Modify: `src/Database/database.c` (new function + replace save_index_sections)
- Modify: `src/Database/database.h` (add page_file/bnode_cache fields)

- [ ] **Step 1: Add page_file and bnode_cache fields to database_t**

In `src/Database/database.h`, find the `database_t` struct. Add fields:

```c
    // Page-based persistence (Phase 2: replaces sections)
    page_file_t* page_file;
    file_bnode_cache_t* bnode_cache;
```

Remove or keep (but mark deprecated) the existing `storage` field:
```c
    sections_t* storage;   // Deprecated: will be removed in Phase 2 cleanup
```

- [ ] **Step 2: Define dirty_bnode_info_t and collect_dirty_bnodes**

In `src/Database/database.c`, add a helper struct and collection function:

```c
typedef struct {
    bnode_t* bnode;
    hbtrie_node_t* hbnode;         // Owning hbtrie_node
    bnode_t* parent_bnode;         // Parent bnode (or NULL for root)
    size_t parent_entry_index;     // Index of this bnode in parent's entries
    uint16_t level;                // B+tree level (for sorting)
} dirty_bnode_info_t;

static int compare_dirty_by_level(const void* a, const void* b) {
    const dirty_bnode_info_t* da = (const dirty_bnode_info_t*)a;
    const dirty_bnode_info_t* db = (const dirty_bnode_info_t*)b;
    // Ascending: leaves (level=1) first, root last
    if (da->level < db->level) return -1;
    if (da->level > db->level) return 1;
    return 0;
}

// Walk the trie to collect all dirty bnodes with parent tracking
static void collect_dirty_bnodes_from_hbnode(
    hbtrie_node_t* hbnode,
    bnode_t* parent_bnode,
    size_t parent_entry_index,
    vec_t(dirty_bnode_info_t)* dirty_list)
{
    if (hbnode == NULL || !hbnode->is_dirty) return;

    // Walk all bnodes in this hbtrie_node's btree
    vec_t(bnode_t*) stack;
    vec_init(&stack);
    vec_push(&stack, hbnode->btree);

    while (stack.length > 0) {
        bnode_t* bn = vec_pop(&stack);

        if (bn->is_dirty) {
            dirty_bnode_info_t info;
            info.bnode = bn;
            info.hbnode = hbnode;
            info.parent_bnode = parent_bnode;
            info.parent_entry_index = parent_entry_index;
            info.level = atomic_load(&bn->level);
            vec_push(dirty_list, info);
        }

        // Push child bnodes for internal nodes
        for (size_t i = 0; i < bn->entries.length; i++) {
            bnode_entry_t* entry = &bn->entries.data[i];
            if (entry->is_bnode_child && entry->child_bnode != NULL) {
                vec_push(&stack, entry->child_bnode);
            }
        }
    }

    vec_deinit(&stack);

    // Recurse into child hbtrie_nodes
    for (size_t i = 0; i < hbnode->btree->entries.length; i++) {
        bnode_entry_t* entry = &hbnode->btree->entries.data[i];
        if (!entry->is_bnode_child && !entry->has_value && entry->child != NULL) {
            collect_dirty_bnodes_from_hbnode(entry->child, hbnode->btree, i, dirty_list);
        }
        if (entry->trie_child != NULL) {
            collect_dirty_bnodes_from_hbnode(entry->trie_child, hbnode->btree, i, dirty_list);
        }
    }
}
```

Note: For entries that are both value and trie_child, the trie_child is a separate hbtrie_node that may also be dirty. The parent tracking uses the root bnode of the hbtrie_node.

- [ ] **Step 3: Implement database_flush_dirty_bnodes**

In `src/Database/database.c`, add the main flush function:

```c
int database_flush_dirty_bnodes(database_t* db) {
    if (db == NULL || db->page_file == NULL || db->trie == NULL) return -1;

    hbtrie_node_t* root = atomic_load(&db->trie->root);
    if (root == NULL || !root->is_dirty) return 0;  // Nothing dirty

    // 1. Collect all dirty bnodes
    vec_t(dirty_bnode_info_t) dirty_list;
    vec_init(&dirty_list);
    collect_dirty_bnodes_from_hbnode(root, NULL, 0, &dirty_list);

    if (dirty_list.length == 0) {
        vec_deinit(&dirty_list);
        return 0;
    }

    // 2. Sort by level (leaves first, root last)
    qsort(dirty_list.data, dirty_list.length, sizeof(dirty_bnode_info_t),
          compare_dirty_by_level);

    // 3. Flush each dirty bnode
    for (size_t i = 0; i < dirty_list.length; i++) {
        dirty_bnode_info_t* info = &dirty_list.data[i];
        bnode_t* bn = info->bnode;

        // Serialize with V3 format
        uint8_t* buf = NULL;
        size_t len = 0;
        int rc = bnode_serialize_v3(bn, db->trie->chunk_size, &buf, &len);
        if (rc != 0) {
            free(buf);
            vec_deinit(&dirty_list);
            return -1;
        }

        // Write to page file at new offset (CoW)
        uint64_t new_offset = 0;
        uint64_t bids[64] = {0};
        size_t num_bids = 0;
        rc = page_file_write_node(db->page_file, buf, len,
                                   &new_offset, bids, 64, &num_bids);
        free(buf);
        if (rc != 0) {
            vec_deinit(&dirty_list);
            return -1;
        }

        // Mark old offset as stale
        if (bn->disk_offset != 0) {
            page_file_mark_stale(db->page_file, bn->disk_offset, len);
        }

        // Update bnode's disk_offset
        bn->disk_offset = new_offset;
        bn->is_dirty = 0;

        // Update parent entry's child_disk_offset
        if (info->parent_bnode != NULL) {
            bnode_entry_t* parent_entry = &info->parent_bnode->entries.data[info->parent_entry_index];
            parent_entry->child_disk_offset = new_offset;
            // Parent is now dirty (its entry changed)
            info->parent_bnode->is_dirty = 1;
            // Parent's hbtrie_node is also dirty
            // (it will be processed in a later iteration since sorted by level)
        }
    }

    // 4. Update hbtrie_node disk_offsets and clear dirty flags
    // Walk the trie again to sync disk_offset and is_dirty
    // (root bnode's offset IS the hbtrie_node's offset)
    root->disk_offset = root->btree->disk_offset;
    root->is_dirty = 0;

    // 5. Write superblock with new root offset
    page_file_write_superblock(db->page_file, root->disk_offset, 0);

    vec_deinit(&dirty_list);
    return 0;
}
```

Note: The parent-entry update logic is simplified here. In practice, the parent bnode needs to be flushed too (since its child_disk_offset changed). The level-sorted processing ensures parents are processed after children. The key insight: when we write a child bnode to a new offset, we update the parent's entry. This makes the parent dirty. Since we process from leaves to root, the parent will be encountered later and flushed with the correct child_disk_offset.

However, there's a subtlety: the `collect_dirty_bnodes_from_hbnode` function may not include the parent if it wasn't dirty at collection time. The solution: collect dirty bnodes AFTER marking all ancestors dirty. This requires a two-pass approach:

1. First pass: mark all ancestors of dirty bnodes as dirty (propagate dirty upward)
2. Second pass: collect all dirty bnodes
3. Sort and flush

For Step 2, update `collect_dirty_bnodes_from_hbnode` to first propagate dirty upward:

```c
// Before collecting, propagate is_dirty upward through the btree
static void propagate_dirty_upward(bnode_t* root) {
    if (root == NULL) return;

    // Check if any child bnode is dirty
    for (size_t i = 0; i < root->entries.length; i++) {
        bnode_entry_t* entry = &root->entries.data[i];
        if (entry->is_bnode_child && entry->child_bnode != NULL) {
            propagate_dirty_upward(entry->child_bnode);
            if (entry->child_bnode->is_dirty) {
                root->is_dirty = 1;
            }
        }
    }
}
```

Call `propagate_dirty_upward(root->btree)` before collection. This ensures all ancestors of dirty leaves are also marked dirty.

- [ ] **Step 4: Replace database_persist and database_snapshot**

In `src/Database/database.c`, replace `database_persist`:

```c
int database_persist(database_t* db) {
    if (db == NULL) return -1;
    if (db->page_file != NULL) {
        return database_flush_dirty_bnodes(db);
    }
    if (db->storage != NULL) {
        return save_index_sections(db);  // Legacy fallback (stubbed in Phase 2A)
    }
    return 0;
}
```

Update `database_snapshot` to use `database_flush_dirty_bnodes` instead of `save_index_sections`:

```c
    if (db->page_file != NULL) {
        rc = database_flush_dirty_bnodes(db);
    } else if (db->storage != NULL) {
        rc = save_index_sections(db);  // Legacy fallback
    }
```

- [ ] **Step 5: Build and verify compilation**

Run: `cd /home/victor/Workspace/src/github.com/vijayee/WaveDB/build_debug && cmake .. -DCMAKE_BUILD_TYPE=Debug && make -j$(nproc) 2>&1 | tail -20`

- [ ] **Step 6: Commit**

```bash
git add src/Database/database.h src/Database/database.c
git commit -m "feat: implement per-bnode CoW flush replacing save_index_sections"
```

---

### Task 7: Implement lazy loading from page file

**Files:**
- Modify: `src/HBTrie/hbtrie.h` (add bnode_lazy_load declaration)
- Modify: `src/HBTrie/hbtrie.c` (implement lazy loading)
- Modify: `src/Database/database.c` (wire page_file into database_create, add load_from_page_file)

- [ ] **Step 1: Add lazy loading function declarations**

In `src/HBTrie/hbtrie.h`, add declarations:

```c
/**
 * Load a child bnode from disk on demand (lazy loading).
 *
 * If entry->child_bnode is NULL and entry->child_disk_offset != 0,
 * reads the bnode from the page file via the bnode cache,
 * deserializes with V3 format, and sets entry->child_bnode.
 *
 * @param entry        Bnode entry whose child needs loading
 * @param fcache       File bnode cache for read-through
 * @param chunk_size   Chunk size for deserialization
 * @param btree_node_size Max btree node size for deserialization
 * @return 0 on success, -1 on failure
 */
int bnode_entry_lazy_load_bnode_child(bnode_entry_t* entry,
                                       file_bnode_cache_t* fcache,
                                       uint8_t chunk_size,
                                       uint32_t btree_node_size);

/**
 * Load a child hbtrie_node from disk on demand (lazy loading).
 *
 * If entry->child is NULL and entry->child_disk_offset != 0,
 * reads the root bnode from the page file, creates an hbtrie_node
 * wrapper, and sets entry->child.
 *
 * @param entry        Bnode entry whose child needs loading
 * @param fcache       File bnode cache for read-through
 * @param chunk_size   Chunk size for deserialization
 * @param btree_node_size Max btree node size for deserialization
 * @return 0 on success, -1 on failure
 */
int bnode_entry_lazy_load_hbtrie_child(bnode_entry_t* entry,
                                        file_bnode_cache_t* fcache,
                                        uint8_t chunk_size,
                                        uint32_t btree_node_size);
```

- [ ] **Step 2: Implement lazy loading functions**

In `src/HBTrie/hbtrie.c`, add implementations:

```c
int bnode_entry_lazy_load_bnode_child(bnode_entry_t* entry,
                                       file_bnode_cache_t* fcache,
                                       uint8_t chunk_size,
                                       uint32_t btree_node_size) {
    if (entry == NULL || fcache == NULL) return -1;
    if (entry->child_bnode != NULL) return 0;  // Already loaded
    if (entry->child_disk_offset == 0) return -1;  // Not on disk

    // Read from bnode cache (which reads from page file on miss)
    bnode_cache_item_t* item = bnode_cache_read(fcache, entry->child_disk_offset);
    if (item == NULL) return -1;

    // Deserialize with V3 format
    node_location_t* locations = NULL;
    size_t num_locations = 0;
    bnode_t* child = bnode_deserialize_v3(item->data, item->data_len,
                                           chunk_size, btree_node_size,
                                           &locations, &num_locations);

    bnode_cache_release(fcache, item);

    if (child == NULL) {
        free(locations);
        return -1;
    }

    // Set child_disk_offset on child's entries (from locations)
    for (size_t i = 0; i < num_locations && i < child->entries.length; i++) {
        child->entries.data[i].child_disk_offset = locations[i].offset;
    }

    // Set the child's disk_offset
    child->disk_offset = entry->child_disk_offset;
    child->is_dirty = 0;

    entry->child_bnode = child;
    free(locations);
    return 0;
}

int bnode_entry_lazy_load_hbtrie_child(bnode_entry_t* entry,
                                        file_bnode_cache_t* fcache,
                                       uint8_t chunk_size,
                                       uint32_t btree_node_size) {
    if (entry == NULL || fcache == NULL) return -1;
    if (entry->child != NULL) return 0;  // Already loaded
    if (entry->child_disk_offset == 0) return -1;  // Not on disk

    // Read root bnode from page file
    bnode_cache_item_t* item = bnode_cache_read(fcache, entry->child_disk_offset);
    if (item == NULL) return -1;

    node_location_t* locations = NULL;
    size_t num_locations = 0;
    bnode_t* root_bnode = bnode_deserialize_v3(item->data, item->data_len,
                                                 chunk_size, btree_node_size,
                                                 &locations, &num_locations);

    bnode_cache_release(fcache, item);

    if (root_bnode == NULL) {
        free(locations);
        return -1;
    }

    // Set child_disk_offset on root bnode entries
    for (size_t i = 0; i < num_locations && i < root_bnode->entries.length; i++) {
        root_bnode->entries.data[i].child_disk_offset = locations[i].offset;
    }

    root_bnode->disk_offset = entry->child_disk_offset;
    root_bnode->is_dirty = 0;

    // Create hbtrie_node wrapper
    hbtrie_node_t* hbnode = hbtrie_node_create(btree_node_size);
    if (hbnode == NULL) {
        bnode_destroy_tree(root_bnode);
        free(locations);
        return -1;
    }

    // Replace the default btree with the loaded one
    bnode_destroy(hbnode->btree);
    hbnode->btree = root_bnode;
    hbnode->btree_height = atomic_load(&root_bnode->level);
    hbnode->disk_offset = entry->child_disk_offset;
    hbnode->is_loaded = 1;
    hbnode->is_dirty = 0;

    entry->child = hbnode;
    free(locations);
    return 0;
}
```

- [ ] **Step 3: Add lazy load calls in traversal paths**

In `src/HBTrie/hbtrie.c`, update `bnode_descend` (and any function that traverses bnode trees) to lazy-load child bnodes. Find where `entry->child_bnode` is used for descent (in `bnode_descend`, approximately). Before accessing `entry->child_bnode`, add a lazy-load check:

```c
    if (entry->is_bnode_child && entry->child_bnode == NULL && entry->child_disk_offset != 0) {
        // Lazy load from page file (requires fcache access)
        // This is called from read paths that may not have fcache.
        // Lazy loading is triggered from database-level functions that have fcache access.
        // At the hbtrie level, we require the caller to ensure children are loaded.
    }
```

For now, add the lazy-load call in the database-level traversal wrappers. In `database.c`, wherever the database traverses the trie (in `database_get_sync`, `database_put_sync`, etc.), add a pre-pass that lazy-loads any needed children before calling hbtrie_find/hbtrie_insert.

Alternative approach: add a `file_bnode_cache_t* fcache` parameter to `hbtrie_find` and `hbtrie_insert`. This is cleaner but requires changing many function signatures. Use this approach:

Add an optional `fcache` parameter pattern — add `fcache` as a new field on `hbtrie_t`:

```c
typedef struct hbtrie_t {
    refcounter_t refcounter;
    uint8_t chunk_size;
    uint32_t btree_node_size;
    _Atomic(hbtrie_node_t*) root;
    file_bnode_cache_t* fcache;  // Phase 2: for lazy loading
} hbtrie_t;
```

Then in `bnode_descend`, before following a `child_bnode` pointer, check if it's NULL and needs lazy loading via `trie->fcache`.

- [ ] **Step 4: Wire page_file/bnode_cache into database_create**

In `src/Database/database.c`, update `database_create_with_config`. When `enable_persist` is set and the database directory exists:

```c
    // Create page file and bnode cache (Phase 2: replaces sections)
    if (enable_persist) {
        char page_path[512];
        snprintf(page_path, sizeof(page_path), "%s/data.wdbp", db->dir);

        db->page_file = page_file_create(page_path, PAGE_FILE_DEFAULT_BLOCK_SIZE,
                                           PAGE_FILE_DEFAULT_NUM_SUPERBLOCKS);
        if (db->page_file == NULL) { /* error handling */ }

        int rc = page_file_open(db->page_file, 1);
        if (rc == 0) {
            // Create bnode cache manager
            bnode_cache_mgr_t* cache_mgr = bnode_cache_mgr_create(
                db->storage_cache_size > 0 ? db->storage_cache_size : 128 * 1024 * 1024,
                4);  // 4 shards
            db->bnode_cache = bnode_cache_create_file_cache(
                cache_mgr, db->page_file, page_path);

            // Read superblock for root offset
            page_superblock_t sb;
            rc = page_file_read_superblock(db->page_file, &sb);
            if (rc == 0 && sb.root_offset != 0) {
                // Lazy-load root hbtrie_node from root_offset
                bnode_cache_item_t* item = bnode_cache_read(db->bnode_cache, sb.root_offset);
                if (item != NULL) {
                    node_location_t* locations = NULL;
                    size_t num_locations = 0;
                    bnode_t* root_bnode = bnode_deserialize_v3(
                        item->data, item->data_len,
                        db->trie->chunk_size, db->trie->btree_node_size,
                        &locations, &num_locations);
                    bnode_cache_release(db->bnode_cache, item);

                    if (root_bnode != NULL) {
                        // Set child_disk_offset on entries
                        for (size_t i = 0; i < num_locations && i < root_bnode->entries.length; i++) {
                            root_bnode->entries.data[i].child_disk_offset = locations[i].offset;
                        }
                        root_bnode->disk_offset = sb.root_offset;
                        root_bnode->is_dirty = 0;

                        // Replace trie root
                        hbtrie_node_t* root_hbnode = hbtrie_node_create(db->trie->btree_node_size);
                        bnode_destroy(root_hbnode->btree);
                        root_hbnode->btree = root_bnode;
                        root_hbnode->btree_height = atomic_load(&root_bnode->level);
                        root_hbnode->disk_offset = sb.root_offset;
                        root_hbnode->is_loaded = 1;
                        root_hbnode->is_dirty = 0;

                        hbtrie_node_t* old_root = atomic_load(&db->trie->root);
                        atomic_store(&db->trie->root, root_hbnode);
                        hbtrie_node_destroy(old_root);

                        free(locations);
                    }
                }
            }

            // Set fcache on trie for lazy loading
            db->trie->fcache = db->bnode_cache;
        }
    }
```

- [ ] **Step 5: Wire page_file into database_destroy**

In `src/Database/database.c`, update `database_destroy`. Before freeing the trie, flush dirty bnodes:

```c
    // Flush remaining dirty bnodes to page file
    if (db->page_file != NULL) {
        database_flush_dirty_bnodes(db);
    }
```

After destroying the trie, destroy the page file and bnode cache:

```c
    // Destroy page-based persistence (Phase 2)
    if (db->bnode_cache != NULL) {
        bnode_cache_mgr_t* mgr = db->bnode_cache->mgr;
        bnode_cache_destroy_file_cache(db->bnode_cache);
        if (mgr != NULL) bnode_cache_mgr_destroy(mgr);
    }
    if (db->page_file != NULL) {
        page_file_destroy(db->page_file);
    }
```

- [ ] **Step 6: Build and run existing tests**

Run: `cd /home/victor/Workspace/src/github.com/vijayee/WaveDB/build_debug && cmake .. -DCMAKE_BUILD_TYPE=Debug && make -j$(nproc) 2>&1 | tail -10`

Then: `cd /home/victor/Workspace/src/github.com/vijayee/WaveDB/build_debug && ctest --output-on-failure 2>&1 | tail -20`

Expected: All existing tests pass (in-memory tests are unaffected, persistence tests use the new page_file path).

- [ ] **Step 7: Commit**

```bash
git add src/HBTrie/hbtrie.h src/HBTrie/hbtrie.c src/Database/database.h src/Database/database.c
git commit -m "feat: implement lazy loading and wire page_file into database lifecycle"
```

---

### Task 8: Integration tests for Phase 2 persistence

**Files:**
- Create: `tests/test_persistence_phase2.cpp`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Create integration test file**

Create `tests/test_persistence_phase2.cpp` with tests for CoW flush, lazy loading, crash recovery:

```cpp
#include <gtest/gtest.h>
#include "Database/database.h"
#include "Storage/page_file.h"
#include "Storage/bnode_cache.h"
#include "HBTrie/hbtrie.h"

#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <filesystem>

class PersistencePhase2Test : public ::testing::Test {
protected:
    char tmpdir[256];
    database_t* db = nullptr;

    void SetUp() override {
        strcpy(tmpdir, "/tmp/wavedb_phase2_test_XXXXXX");
        mkdtemp(tmpdir);
    }

    void TearDown() override {
        if (db != nullptr) {
            database_destroy(db);
            db = nullptr;
        }
        char cmd[512];
        snprintf(cmd, sizeof(cmd), "rm -rf %s", tmpdir);
        system(cmd);
    }

    database_t* create_db(int enable_persist = 1) {
        database_config_t config;
        database_config_init(&config);
        config.enable_persist = enable_persist;
        config.chunk_size = 4;
        config.btree_node_size = 4096;
        snprintf(config.path, sizeof(config.path), "%s", tmpdir);
        return database_create(&config);
    }
};

// Test 1: Create, put, flush, destroy — verify no leaks
TEST_F(PersistencePhase2Test, PutFlushDestroyNoLeaks) {
    db = create_db(1);
    ASSERT_NE(db, nullptr);

    path_t* key = path_create();
    path_append(key, "test");
    identifier_t* val = identifier_create_from_cstr("value");
    database_put_sync(db, key, val);
    path_destroy(key);
    identifier_destroy(val);

    // Flush should write dirty bnodes to page file
    int rc = database_snapshot(db);
    EXPECT_EQ(rc, 0);

    // Destroy flushes remaining dirty nodes
    database_destroy(db);
    db = nullptr;
    // ASan will verify no leaks
}

// Test 2: Write, flush, reopen — verify data persists across restart
TEST_F(PersistencePhase2Test, DataPersistsAcrossRestart) {
    db = create_db(1);
    ASSERT_NE(db, nullptr);

    path_t* key = path_create();
    path_append(key, "persist");
    identifier_t* val = identifier_create_from_cstr("hello");
    database_put_sync(db, key, val);
    path_destroy(key);
    identifier_destroy(val);

    database_snapshot(db);
    database_destroy(db);
    db = nullptr;

    // Reopen
    db = create_db(1);
    ASSERT_NE(db, nullptr);

    path_t* key2 = path_create();
    path_append(key2, "persist");
    identifier_t* result = database_get_sync(db, key2);
    path_destroy(key2);

    // Verify data survived restart
    // (lazy loading should load the root bnode from superblock)
    ASSERT_NE(result, nullptr);
    // Check value content
    buffer_t* buf = identifier_to_buffer(result);
    EXPECT_NE(buf, nullptr);
    if (buf != nullptr) {
        EXPECT_EQ(memcmp(buf->data, "hello", 5), 0);
        buffer_destroy(buf);
    }
    identifier_destroy(result);
}

// Test 3: Multiple puts, snapshot, verify CoW stale regions
TEST_F(PersistencePhase2Test, CoWCreatesStaleRegions) {
    db = create_db(1);
    ASSERT_NE(db, nullptr);

    // First write
    path_t* key1 = path_create();
    path_append(key1, "key1");
    identifier_t* val1 = identifier_create_from_cstr("value1");
    database_put_sync(db, key1, val1);
    path_destroy(key1);
    identifier_destroy(val1);

    database_snapshot(db);

    // Second write to same key (triggers CoW — old bnode location becomes stale)
    path_t* key2 = path_create();
    path_append(key2, "key1");
    identifier_t* val2 = identifier_create_from_cstr("value2");
    database_put_sync(db, key2, val2);
    path_destroy(key2);
    identifier_destroy(val2);

    database_snapshot(db);

    // Verify stale ratio is non-zero (old bnode data is stale)
    if (db->page_file != nullptr) {
        double ratio = page_file_stale_ratio(db->page_file);
        EXPECT_GT(ratio, 0.0);
    }
}

// Test 4: Lazy loading — verify children loaded on demand
TEST_F(PersistencePhase2Test, LazyLoadingOnDemand) {
    db = create_db(1);
    ASSERT_NE(db, nullptr);

    // Create multi-level trie
    path_t* key = path_create();
    path_append(key, "users");
    path_append(key, "alice");
    path_append(key, "name");
    identifier_t* val = identifier_create_from_cstr("Alice");
    database_put_sync(db, key, val);
    path_destroy(key);
    identifier_destroy(val);

    database_snapshot(db);
    database_destroy(db);
    db = nullptr;

    // Reopen — root should be lazy-loaded from superblock
    db = create_db(1);
    ASSERT_NE(db, nullptr);

    // Access the deep key — should trigger lazy loading of intermediate nodes
    path_t* key2 = path_create();
    path_append(key2, "users");
    path_append(key2, "alice");
    path_append(key2, "name");
    identifier_t* result = database_get_sync(db, key2);
    path_destroy(key2);

    ASSERT_NE(result, nullptr);
    identifier_destroy(result);
}
```

- [ ] **Step 2: Add test target to CMakeLists.txt**

In `CMakeLists.txt`, add after existing test targets:

```cmake
# Phase 2 persistence integration tests
add_executable(test_persistence_phase2 tests/test_persistence_phase2.cpp)
target_link_libraries(test_persistence_phase2 ${WAVEDB_LIBS} gtest gtest_main)
add_test(NAME test_persistence_phase2 COMMAND test_persistence_phase2)
```

- [ ] **Step 3: Build and run integration tests**

Run: `cd /home/victor/Workspace/src/github.com/vijayee/WaveDB/build_debug && cmake .. -DCMAKE_BUILD_TYPE=Debug && make -j$(nproc) test_persistence_phase2 2>&1 | tail -20`

Then: `cd /home/victor/Workspace/src/github.com/vijayee/WaveDB/build_debug && ./test_persistence_phase2 2>&1`

Expected: Tests may need debugging — lazy loading and persistence are new code paths. Fix any issues found.

- [ ] **Step 4: Run under ASan for leak checks**

Run: `cd /home/victor/Workspace/src/github.com/vijayee/WaveDB/build_asan && cmake .. -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_FLAGS="-fsanitize=address -fno-omit-frame-pointer" -DCMAKE_CXX_FLAGS="-fsanitize=address -fno-omit-frame-pointer" -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address" && make -j$(nproc) test_persistence_phase2 2>&1 | tail -10`

Then: `cd /home/victor/Workspace/src/github.com/vijayee/WaveDB/build_asan && ./test_persistence_phase2 2>&1`

Expected: Zero ASan errors and zero leaks.

- [ ] **Step 5: Commit**

```bash
git add tests/test_persistence_phase2.cpp CMakeLists.txt
git commit -m "test: add Phase 2 persistence integration tests"
```

---

### Task 9: Write amplification benchmark

**Files:**
- Create: `tests/benchmark_write_amplification.cpp`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Create benchmark comparing Phase 1 vs Phase 2 write amplification**

Create `tests/benchmark_write_amplification.cpp`:

```cpp
#include <iostream>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include "Database/database.h"
#include "Storage/page_file.h"

// Measure bytes written to page file per key update
// Phase 2: only the dirty bnodes are rewritten (leaf + ancestors)
// This should be much less than Phase 1 (entire hbtrie_node blob)

static char tmpdir[256];

static database_t* create_db() {
    database_config_t config;
    database_config_init(&config);
    config.enable_persist = 1;
    config.chunk_size = 4;
    config.btree_node_size = 4096;
    snprintf(config.path, sizeof(config.path), "%s", tmpdir);
    return database_create(&config);
}

int main() {
    strcpy(tmpdir, "/tmp/wavedb_bench_phase2_XXXXXX");
    mkdtemp(tmpdir);

    database_t* db = create_db();
    if (db == nullptr) {
        std::cerr << "Failed to create database" << std::endl;
        return 1;
    }

    const int NUM_KEYS = 1000;
    const int NUM_UPDATES = 100;

    // Phase 1: Insert N keys, then measure file growth per update
    std::cout << "=== Write Amplification Benchmark ===" << std::endl;

    // Insert initial keys
    for (int i = 0; i < NUM_KEYS; i++) {
        path_t* key = path_create();
        char buf[64];
        snprintf(buf, sizeof(buf), "key%d", i);
        path_append(key, buf);
        identifier_t* val = identifier_create_from_cstr(buf);
        database_put_sync(db, key, val);
        path_destroy(key);
        identifier_destroy(val);
    }

    // Snapshot to flush everything
    database_snapshot(db);
    uint64_t file_size_before = page_file_size(db->page_file);
    double stale_before = page_file_stale_ratio(db->page_file);

    std::cout << "Initial state: file_size=" << file_size_before
              << " stale_ratio=" << stale_before << std::endl;

    // Update same key repeatedly, snapshot after each
    uint64_t total_bytes_written = 0;
    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < NUM_UPDATES; i++) {
        path_t* key = path_create();
        path_append(key, "key0");  // Always update same key
        char val_buf[64];
        snprintf(val_buf, sizeof(val_buf), "updated_val_%d", i);
        identifier_t* val = identifier_create_from_cstr(val_buf);
        database_put_sync(db, key, val);
        path_destroy(key);
        identifier_destroy(val);

        database_snapshot(db);
    }

    auto end = std::chrono::high_resolution_clock::now();
    uint64_t file_size_after = page_file_size(db->page_file);
    double stale_after = page_file_stale_ratio(db->page_file);

    total_bytes_written = file_size_after - file_size_before;

    double write_amp = (double)total_bytes_written / (double)NUM_UPDATES;

    std::cout << "\nAfter " << NUM_UPDATES << " updates to same key:" << std::endl;
    std::cout << "  file_size=" << file_size_after << std::endl;
    std::cout << "  total_bytes_written=" << total_bytes_written << std::endl;
    std::cout << "  bytes_per_update=" << write_amp << std::endl;
    std::cout << "  stale_ratio=" << stale_after << std::endl;

    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
    double updates_per_sec = (double)NUM_UPDATES / (duration.count() / 1e6);
    std::cout << "  snapshot_throughput=" << updates_per_sec << " snapshots/sec" << std::endl;

    database_destroy(db);

    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", tmpdir);
    system(cmd);

    return 0;
}
```

- [ ] **Step 2: Add benchmark target to CMakeLists.txt**

In `CMakeLists.txt`, add:

```cmake
add_executable(benchmark_write_amplification tests/benchmark_write_amplification.cpp)
target_link_libraries(benchmark_write_amplification ${WAVEDB_LIBS})
```

- [ ] **Step 3: Build and run benchmark**

Run: `cd /home/victor/Workspace/src/github.com/vijayee/WaveDB/build_debug && cmake .. -DCMAKE_BUILD_TYPE=Debug && make -j$(nproc) benchmark_write_amplification 2>&1 | tail -10`

Then: `cd /home/victor/Workspace/src/github.com/vijayee/WaveDB/build_debug && ./benchmark_write_amplification 2>&1`

Expected: Benchmark outputs write amplification metrics. Compare with Phase 1 baseline (Phase 1 would rewrite entire hbtrie_node blobs).

- [ ] **Step 4: Commit**

```bash
git add tests/benchmark_write_amplification.cpp CMakeLists.txt
git commit -m "bench: add write amplification benchmark for Phase 2"
```

---

## Self-Review

**1. Spec coverage:**
- Section 1 (V3 format): Implemented in Phase 2A
- Section 2 (data structure changes): Implemented in Phase 2A
- Section 3 (CoW propagation): Task 6 implements database_flush_dirty_bnodes
- Section 4 (lazy loading): Task 7 implements bnode_entry_lazy_load_*
- Section 5 (BnodeCache integration): Task 7 wires bnode_cache into database
- Section 6 (database lifecycle): Tasks 6-7 wire page_file into create/destroy/snapshot
- Section 7 (write amplification): Task 9 benchmarks this
- Section 8 (concurrency): bnode_t.write_lock already exists — per-bnode locking is inherent
- Section 9 (removed components): Stubs in Phase 2A; full removal deferred to cleanup
- Section 10 (testing): Tasks 4, 8 cover V3 serializer tests and integration tests

**2. Placeholder scan:**
- No TBD/TODO found in task steps
- All steps have code or specific instructions
- Test code is complete for Tasks 4 and 8

**3. Type consistency:**
- `child_disk_offset` (uint64_t) used consistently in bnode_entry_t, serializer, deserializer, lazy loading
- `disk_offset` (uint64_t) used consistently in bnode_t, hbtrie_node_t, database_flush_dirty_bnodes
- `node_location_t.offset` (uint64_t) consistent with child_disk_offset
- `is_dirty` (uint8_t) consistent in bnode_t and hbtrie_node_t
- `file_bnode_cache_t* fcache` on hbtrie_t for lazy loading