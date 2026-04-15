# Phase 2A: Data Structure Changes + V3 Flat Serializer

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add page-file disk tracking fields to bnode_t, hbtrie_node_t, and bnode_entry_t; implement the V3 flat per-bnode serializer where every bnode is independently serialized with file_offset references instead of inline child bnodes.

**Architecture:** Add `disk_offset` (uint64_t) and `is_dirty` (uint8_t) to bnode_t; add `disk_offset` to hbtrie_node_t replacing section-based fields; add `child_disk_offset` (uint64_t) to bnode_entry_t replacing `child_section_id`/`child_block_index`; update `node_location_t` to use single offset. Implement V3 serializer (magic 0xB5) that stores child bnodes as 8-byte file offsets instead of inline data. V3 deserializer reads child offsets and leaves child pointers NULL for lazy loading. All existing V1/V2 format reading preserved for backward compatibility.

**Tech Stack:** C11, gtest for tests, following STYLEGUIDE.md conventions (refcounter_t first member, type_action() naming, get_clear_memory()/memory_pool_alloc(), platform_lock_init(), etc.)

**Spec:** `docs/superpowers/specs/2026-04-15-phase2-flat-bnode-design.md` sections 1-2

**Important:** Memory leak checks (ASan) at every testing stage. Every create/destroy cycle must show zero leaks.

---

## File Structure

| File | Responsibility |
|------|---------------|
| `src/HBTrie/bnode.h` | bnode_t and bnode_entry_t struct definitions with new fields |
| `src/HBTrie/bnode.c` | bnode_create/bnode_destroy/bnode_split updated for new fields |
| `src/HBTrie/hbtrie.h` | hbtrie_node_t struct definition with disk_offset, removed section fields |
| `src/HBTrie/hbtrie.c` | hbtrie_node_create/destroy/copy updated for new fields |
| `src/Storage/node_serializer.h` | node_location_t updated, V3 serialize/deserialize declarations |
| `src/Storage/node_serializer.c` | V3 serializer and deserializer implementation |
| `tests/test_v3_serializer.cpp` | V3 serializer round-trip tests |

Existing files modified:
- `CMakeLists.txt` — add test_v3_serializer target

---

### Task 1: Add disk_offset to bnode_t and bnode_entry_t

**Files:**
- Modify: `src/HBTrie/bnode.h:121-128` (bnode_t struct)
- Modify: `src/HBTrie/bnode.h:72-108` (bnode_entry_t struct)
- Modify: `src/HBTrie/bnode.c:22-42` (bnode_create_with_level)

- [ ] **Step 1: Add disk_offset and is_dirty to bnode_t**

In `src/HBTrie/bnode.h`, add two fields after `PLATFORMLOCKTYPE(write_lock)`:

```c
typedef struct bnode_t {
    refcounter_t refcounter;          // MUST be first member (16-48 bytes)
    _Atomic(uint16_t) level;           // B+tree level: 1 = leaf, > 1 = internal
    uint32_t node_size;                // Configurable max size in bytes
    vec_t(bnode_entry_t) entries;      // Sorted by chunk key
    _Atomic(uint64_t) seq;             // Seqlock: even=stable, odd=writing
    PLATFORMLOCKTYPE(write_lock);       // Writer mutual exclusion

    // Per-bnode disk tracking (Phase 2: flat per-bnode persistence)
    uint64_t disk_offset;              // File offset of this bnode (0 if not persisted)
    uint8_t is_dirty;                  // 1 if modified since last write
} bnode_t;
```

Note: `memset(node, 0, sizeof(bnode_t))` in `bnode_create_with_level` already zero-initializes these new fields, so `disk_offset=0` and `is_dirty=0` by default. No code change needed in create function.

- [ ] **Step 2: Add child_disk_offset to bnode_entry_t, remove child_section_id/child_block_index**

In `src/HBTrie/bnode.h`, replace `child_section_id` and `child_block_index` with `child_disk_offset`:

```c
typedef struct bnode_entry_t {
    // ... all existing fields before line 104 unchanged ...

    // Storage location for lazy-loaded children (Phase 2: page file offset)
    uint64_t child_disk_offset;        // File offset of child bnode or hbtrie_node root (0 = not on disk)

    // REMOVED: child_section_id, child_block_index (replaced by child_disk_offset)
} bnode_entry_t;
```

- [ ] **Step 3: Update bnode_split to copy child_disk_offset instead of child_section_id/child_block_index**

In `src/HBTrie/bnode.c`, find the `bnode_split` function where it copies `child_section_id` and `child_block_index` (approximately line 398-399). Replace with:

```c
right_entry.child_disk_offset = entry->child_disk_offset;
```

Remove the two lines copying `child_section_id` and `child_block_index`.

- [ ] **Step 4: Build to verify struct changes compile**

Run: `cd /home/victor/Workspace/src/github.com/vijayee/WaveDB/build_debug && cmake .. -DCMAKE_BUILD_TYPE=Debug && make -j$(nproc) 2>&1 | head -100`

Expected: Compilation errors in files that reference `child_section_id`, `child_block_index`, `section_id`, `block_index`. These will be fixed in subsequent tasks.

- [ ] **Step 5: Commit**

```bash
git add src/HBTrie/bnode.h src/HBTrie/bnode.c
git commit -m "feat: add disk_offset to bnode_t and child_disk_offset to bnode_entry_t"
```

---

### Task 2: Update hbtrie_node_t and hbtrie.c for page file fields

**Files:**
- Modify: `src/HBTrie/hbtrie.h:39-54` (hbtrie_node_t struct)
- Modify: `src/HBTrie/hbtrie.c:305-326` (hbtrie_node_create)
- Modify: `src/HBTrie/hbtrie.c:370-389` (hbtrie_node_destroy)
- Modify: `src/HBTrie/hbtrie.c:460-517` (hbtrie_node_copy)

- [ ] **Step 1: Update hbtrie_node_t struct**

In `src/HBTrie/hbtrie.h`, replace the section-based fields with page file fields:

```c
typedef struct hbtrie_node_t {
    refcounter_t refcounter;          // MUST be first member
    _Atomic(uint64_t) seq;            // Seqlock: even=stable, odd=writing
    PLATFORMLOCKTYPE(write_lock);      // Writer mutual exclusion

    bnode_t* btree;                   // Root bnode of multi-level B+tree at this level
    uint16_t btree_height;            // Height of B+tree (1 = single leaf, > 1 = has internal nodes)

    // Page file storage tracking (Phase 2: replaces section_id + block_index)
    uint64_t disk_offset;             // File offset of root bnode (0 if not persisted)
    uint8_t is_loaded;                // 1 if in memory, 0 if on-disk stub
    uint8_t is_dirty;                 // 1 if modified since last save
} hbtrie_node_t;
```

Removed: `storage`, `section_id`, `block_index`, `data_size` fields.
Added: `disk_offset` (uint64_t).
Kept: `is_loaded`, `is_dirty`.

- [ ] **Step 2: Update hbtrie_node_create**

In `src/HBTrie/hbtrie.c`, update `hbtrie_node_create` (line 305). Replace the section-based initialization:

```c
  // Initialize page file storage tracking (in-memory by default)
  node->disk_offset = 0;            // 0 = not yet persisted
  node->is_loaded = 1;              // Newly created nodes are in memory
  node->is_dirty = 0;               // Not modified yet
```

Remove the old `node->storage = NULL;`, `node->data_size = 0;` lines.

- [ ] **Step 3: Update hbtrie_node_destroy**

In `src/HBTrie/hbtrie.c`, update `hbtrie_node_destroy` (line 378). Replace the section deallocation:

```c
      // Page file stale regions are tracked separately by stale_region_mgr
      // No explicit deallocation needed here - CoW handles stale marking
```

Remove the `sections_deallocate` call and the condition checking `storage != NULL && section_id != 0`.

- [ ] **Step 4: Update hbtrie_node_copy**

In `src/HBTrie/hbtrie.c`, update `hbtrie_node_copy` (line 508-514). Replace storage metadata copy:

```c
  // Copy page file storage metadata
  copy->disk_offset = node->disk_offset;
  copy->is_loaded = node->is_loaded;
  copy->is_dirty = node->is_dirty;
```

Remove the old `copy->storage`, `copy->section_id`, `copy->block_index`, `copy->data_size` lines.

- [ ] **Step 5: Update hbtrie_insert child->storage references**

In `src/HBTrie/hbtrie.c`, find all lines in `hbtrie_insert` that set `child->storage = current->storage` (approximately lines 1712, 1753, 1774, 1795, 1836, 1857). These set the `storage` field which no longer exists. Remove all these lines — the `disk_offset` field is initialized to 0 by `hbtrie_node_create`, which is correct for new nodes.

- [ ] **Step 6: Update hbtrie_gc section references**

In `src/HBTrie/hbtrie.c`, find `hbtrie_gc` function. It references `node->storage` (lines ~2071, 2103-2108). Replace the `sections_deallocate` call with a no-op comment (stale regions are tracked by the stale_region_mgr in page_file, not by explicit deallocation). The `version_entry_gc` function also takes a `sections_t*` parameter — pass NULL for now (will be wired later in Phase 2B).

- [ ] **Step 7: Build and verify compilation**

Run: `cd /home/victor/Workspace/src/github.com/vijayee/WaveDB/build_debug && cmake .. -DCMAKE_BUILD_TYPE=Debug && make -j$(nproc) 2>&1 | head -100`

Expected: Errors in `database.c`, `sections.c`, and `node_serializer.c` referencing removed fields. These are expected and will be fixed in subsequent tasks.

- [ ] **Step 8: Commit**

```bash
git add src/HBTrie/hbtrie.h src/HBTrie/hbtrie.c
git commit -m "feat: update hbtrie_node_t for page file disk tracking"
```

---

### Task 3: Update node_serializer.h/c and fix compilation

**Files:**
- Modify: `src/Storage/node_serializer.h:24-27` (node_location_t)
- Modify: `src/Storage/node_serializer.c:238-340` (V2 serializer — update entry serialization)
- Modify: `src/Storage/node_serializer.c:369-622` (deserializer — update for new fields)
- Modify: `src/Database/database.c` (fix section-based references)
- Modify: `src/Storage/sections.c` (fix defrag_remap_callback)

- [ ] **Step 1: Update node_location_t**

In `src/Storage/node_serializer.h`, replace `node_location_t`:

```c
typedef struct {
    uint64_t offset;     // File offset in page file (0 if not on disk)
} node_location_t;
```

- [ ] **Step 2: Add V3 magic constant and serialize/deserialize declarations**

In `src/Storage/node_serializer.h`, add after the existing declarations:

```c
#define BNODE_SERIALIZE_MAGIC_V3 0xB5  // V3: flat per-bnode with file_offset references

/**
 * Serialize a B+tree node using V3 flat format.
 *
 * V3 format (magic 0xB5): Every bnode is self-contained.
 * Child bnodes (is_bnode_child) store 8-byte child_disk_offset
 * instead of inline data. Child hbtrie_nodes also store 8-byte
 * child_disk_offset instead of section_id + block_index.
 *
 * @param node       B+tree node to serialize
 * @param chunk_size Size of each chunk in bytes
 * @param buf        Output: allocated buffer (caller must free)
 * @param len        Output: buffer length
 * @return 0 on success, -1 on failure
 */
int bnode_serialize_v3(bnode_t* node, uint8_t chunk_size, uint8_t** buf, size_t* len);

/**
 * Deserialize a B+tree node from V3 flat format.
 *
 * Child entries are NOT loaded — child_bnode/child pointers are NULL
 * and child_disk_offset is populated for lazy loading.
 *
 * @param buf        Binary buffer
 * @param len        Buffer length
 * @param chunk_size Size of each chunk in bytes
 * @param btree_node_size Max B+tree node size
 * @param locations  Array of child offsets (output)
 * @param num_locations Output: number of child offsets
 * @return New B+tree node or NULL on failure
 */
bnode_t* bnode_deserialize_v3(uint8_t* buf, size_t len, uint8_t chunk_size,
                               uint32_t btree_node_size,
                               node_location_t** locations, size_t* num_locations);
```

- [ ] **Step 3: Implement bnode_serialize_v3**

In `src/Storage/node_serializer.c`, add the V3 serializer. It follows the same pattern as `bnode_serialize` (V2) but for `is_bnode_child` entries, writes `child_disk_offset` (8 bytes) instead of inline child bnode data:

```c
static void bnode_serialize_entries_v3(bnode_t* node, uint8_t chunk_size, serialize_buf_t* sb) {
    for (int i = 0; i < node->entries.length; i++) {
        bnode_entry_t* entry = &node->entries.data[i];

        // Write chunk
        chunk_t* key = bnode_entry_get_key(entry);
        if (key != NULL) {
            sbuf_write_bytes(sb, key->data, chunk_size);
        } else {
            uint8_t zero[8] = {0};
            sbuf_write_bytes(sb, zero, chunk_size);
        }

        // Write flags byte
        uint8_t flags = 0;
        if (entry->has_value) flags |= 0x01;
        if (entry->is_bnode_child) flags |= 0x02;
        if (entry->has_value && entry->has_versions) flags |= 0x04;
        sbuf_write_uint8(sb, flags);

        if (entry->has_value) {
            // Value/version serialization: identical to V2 (see spec section 1)
            // Copy the existing value/version serialization logic from
            // bnode_serialize_entries lines 258-309
            if (entry->has_versions && entry->versions != NULL) {
                size_t version_count = 0;
                version_entry_t* current = entry->versions;
                while (current != NULL) { version_count++; current = current->next; }
                sbuf_write_uint16(sb, (uint16_t)version_count);
                current = entry->versions;
                while (current != NULL) {
                    sbuf_write_uint8(sb, current->is_deleted);
                    sbuf_write_uint64(sb, current->txn_id.time);
                    sbuf_write_uint64(sb, current->txn_id.nanos);
                    sbuf_write_uint64(sb, current->txn_id.count);
                    if (!current->is_deleted && current->value != NULL) {
                        buffer_t* ident_buf = identifier_to_buffer(current->value);
                        if (ident_buf != NULL) {
                            sbuf_write_uint32(sb, (uint32_t)ident_buf->size);
                            if (ident_buf->size > 0)
                                sbuf_write_bytes(sb, ident_buf->data, ident_buf->size);
                            buffer_destroy(ident_buf);
                        } else {
                            sbuf_write_uint32(sb, 0);
                        }
                    } else {
                        sbuf_write_uint32(sb, 0);
                    }
                    current = current->next;
                }
            } else {
                identifier_t* ident = entry->value;
                if (ident != NULL) {
                    buffer_t* ident_buf = identifier_to_buffer(ident);
                    if (ident_buf != NULL) {
                        sbuf_write_uint32(sb, (uint32_t)ident_buf->size);
                        if (ident_buf->size > 0)
                            sbuf_write_bytes(sb, ident_buf->data, ident_buf->size);
                        buffer_destroy(ident_buf);
                    } else {
                        sbuf_write_uint32(sb, 0);
                    }
                } else {
                    sbuf_write_uint32(sb, 0);
                }
            }
        } else {
            // V3: both is_bnode_child and hbtrie_node children store child_disk_offset
            sbuf_write_uint64(sb, entry->child_disk_offset);
        }
    }
}

int bnode_serialize_v3(bnode_t* node, uint8_t chunk_size, uint8_t** buf, size_t* len) {
    if (node == NULL || buf == NULL || len == NULL) return -1;

    serialize_buf_t sb;
    sbuf_init(&sb);

    sbuf_write_uint8(&sb, BNODE_SERIALIZE_MAGIC_V3);
    sbuf_write_uint16(&sb, atomic_load(&node->level));
    sbuf_write_uint16(&sb, (uint16_t)node->entries.length);
    bnode_serialize_entries_v3(node, chunk_size, &sb);

    *buf = sb.data;
    *len = sb.size;
    return 0;
}
```

- [ ] **Step 4: Implement bnode_deserialize_v3**

In `src/Storage/node_serializer.c`, add the V3 deserializer. For non-value entries, it reads `child_disk_offset` and sets `child_disk_offset` on the entry, leaving `child_bnode`/`child` as NULL for lazy loading:

```c
bnode_t* bnode_deserialize_v3(uint8_t* buf, size_t len, uint8_t chunk_size,
                               uint32_t btree_node_size,
                               node_location_t** locations, size_t* num_locations) {
    if (buf == NULL || len < 1) return NULL;

    uint8_t* ptr = buf;
    size_t remaining = len;

    uint8_t magic = read_uint8(&ptr);
    remaining--;
    if (magic != BNODE_SERIALIZE_MAGIC_V3) return NULL;

    if (remaining < 4) return NULL;
    uint16_t level = read_uint16(&ptr);
    remaining -= 2;
    uint16_t num_entries = read_uint16(&ptr);
    remaining -= 2;

    *locations = get_clear_memory(num_entries * sizeof(node_location_t));
    *num_locations = num_entries;

    bnode_t* node = bnode_create_with_level(btree_node_size, level);
    if (node == NULL) { free(*locations); *locations = NULL; *num_locations = 0; return NULL; }

    for (uint16_t i = 0; i < num_entries; i++) {
        bnode_entry_t entry;
        memset(&entry, 0, sizeof(entry));

        // Read chunk
        if (remaining < chunk_size) goto fail;
        uint8_t* chunk_buf = get_memory(chunk_size);
        read_bytes(&ptr, chunk_buf, chunk_size);
        remaining -= chunk_size;
        chunk_t* key = chunk_deserialize(chunk_buf, chunk_size);
        free(chunk_buf);
        if (key == NULL) goto fail;
        bnode_entry_set_key(&entry, key);
        chunk_destroy(key);

        // Read flags
        if (remaining < 1) goto fail;
        uint8_t flags = read_uint8(&ptr);
        remaining--;
        entry.has_value = (flags & 0x01) != 0;
        entry.is_bnode_child = (flags & 0x02) != 0;
        entry.has_versions = (flags & 0x04) != 0;

        if (entry.has_value) {
            // Value/version deserialization: identical logic to V2 deserializer
            // Copy the existing value deserialization code from
            // bnode_deserialize_recursive lines 438-520
            if (entry.has_versions) {
                if (remaining < 2) goto fail;
                uint16_t version_count = read_uint16(&ptr);
                remaining -= 2;
                entry.versions = NULL;
                version_entry_t* prev_version = NULL;
                for (uint16_t j = 0; j < version_count; j++) {
                    if (remaining < 1 + 24 + 4) goto fail;
                    uint8_t is_deleted = read_uint8(&ptr);
                    remaining--;
                    transaction_id_t txn_id;
                    txn_id.time = read_uint64(&ptr);
                    txn_id.nanos = read_uint64(&ptr);
                    txn_id.count = read_uint64(&ptr);
                    remaining -= 24;
                    identifier_t* val = NULL;
                    uint32_t ident_len = read_uint32(&ptr);
                    remaining -= 4;
                    if (ident_len > 0 && !is_deleted) {
                        if (remaining < ident_len) goto fail;
                        uint8_t* ident_buf = get_memory(ident_len);
                        read_bytes(&ptr, ident_buf, ident_len);
                        remaining -= ident_len;
                        buffer_t* data_buf = buffer_create_from_existing_memory(ident_buf, ident_len);
                        if (data_buf == NULL) { free(ident_buf); goto fail; }
                        val = identifier_create(data_buf, chunk_size);
                        buffer_destroy(data_buf);
                        if (val == NULL) goto fail;
                    }
                    version_entry_t* version = version_entry_create(txn_id, val, is_deleted);
                    if (version == NULL) { if (val != NULL) identifier_destroy(val); goto fail; }
                    if (entry.versions == NULL) {
                        entry.versions = version;
                    } else {
                        prev_version->next = version;
                        version->prev = prev_version;
                    }
                    prev_version = version;
                }
                (*locations)[i].offset = 0;
            } else {
                if (remaining < 4) goto fail;
                uint32_t ident_len = read_uint32(&ptr);
                remaining -= 4;
                if (ident_len > 0) {
                    if (remaining < ident_len) goto fail;
                    uint8_t* ident_buf = get_memory(ident_len);
                    read_bytes(&ptr, ident_buf, ident_len);
                    remaining -= ident_len;
                    buffer_t* data_buf = buffer_create_from_existing_memory(ident_buf, ident_len);
                    if (data_buf == NULL) { free(ident_buf); goto fail; }
                    entry.value = identifier_create(data_buf, chunk_size);
                    buffer_destroy(data_buf);
                    if (entry.value == NULL) goto fail;
                }
                (*locations)[i].offset = 0;
            }
        } else {
            // V3: read child_disk_offset (8 bytes) for both bnode_child and hbtrie_node children
            if (remaining < 8) goto fail;
            entry.child_disk_offset = read_uint64(&ptr);
            remaining -= 8;
            // child_bnode and child pointers remain NULL (lazy loading)
            (*locations)[i].offset = entry.child_disk_offset;
        }

        bnode_insert(node, &entry);
    }

    return node;

fail:
    bnode_destroy_tree(node);
    free(*locations);
    *locations = NULL;
    *num_locations = 0;
    return NULL;
}
```

- [ ] **Step 5: Update existing V2 serializer to use child_disk_offset**

In `src/Storage/node_serializer.c`, update `bnode_serialize_entries` (V2) to use `child_disk_offset` instead of `child_section_id`/`child_block_index` for hbtrie_node children. Find the `else` branch at the end of the entry serialization (approximately line 335-339) that writes `entry->child_section_id` and `entry->child_block_index`. Replace with:

```c
        } else {
            // Child hbtrie_node location (V2 still stores offset pair for backward compat)
            sbuf_write_uint64(sb, entry->child_disk_offset);
            sbuf_write_uint64(sb, 0);  // Reserved (was block_index, now unused)
        }
```

- [ ] **Step 6: Update existing V2 deserializer to use child_disk_offset**

In `src/Storage/node_serializer.c`, update `bnode_deserialize_recursive` for V1/V2 hbtrie_node child references. Find the `else` branch that reads `child_section_id` and `child_block_index` (approximately lines 561-568). Replace with:

```c
            } else {
                // Child hbtrie_node location
                if (*remaining < 16) goto fail;
                entry.child_disk_offset = read_uint64(ptr);
                *remaining -= 8;
                uint64_t reserved = read_uint64(ptr);  // Was block_index, now unused
                *remaining -= 8;
                (*locations)[i].offset = entry.child_disk_offset;
            }
```

Also update the V1 is_bnode_child branch (approximately line 552-558) similarly:
```c
                } else {
                    // V1: offset reference
                    if (*remaining < 16) goto fail;
                    entry.child_disk_offset = read_uint64(ptr);
                    *remaining -= 8;
                    uint64_t reserved = read_uint64(ptr);
                    *remaining -= 8;
                }
```

And the old-format branch (lines ~601-606):
```c
            } else {
                if (*remaining < 16) goto fail;
                entry.child_disk_offset = read_uint64(ptr);
                *remaining -= 8;
                uint64_t reserved = read_uint64(ptr);
                *remaining -= 8;
                entry.child = NULL;
            }
```

- [ ] **Step 7: Fix database.c section-based references**

In `src/Database/database.c`, the `save_index_sections` function (line 335) and `load_node_from_section` function (line 435) reference `section_id`, `block_index`, `data_size`, `child_section_id`, `child_block_index`. These functions will be replaced in Phase 2B. For now, add stub implementations that return 0 (success) to allow compilation:

```c
int save_index_sections(database_t* db) {
    // Phase 2A stub: persistence will be reimplemented in Phase 2B
    return 0;
}

int load_index_sections(database_t* db) {
    // Phase 2A stub: lazy loading will be reimplemented in Phase 2B
    return -1;  // Return failure to trigger CBOR fallback
}
```

Also fix `load_node_from_section` — replace its body with a return NULL stub.

- [ ] **Step 8: Fix sections.c defrag_remap_callback**

In `src/Storage/sections.c`, find `defrag_remap_callback` (line 700) which directly accesses `hbtrie_node_t.section_id` and `hbtrie_node_t.block_index`. Since these fields no longer exist, update the callback to use `disk_offset`:

Replace `node->section_id == section_id && node->block_index == old_offset` checks with:
```c
if (node->disk_offset != 0) {  // Node has been persisted
    // Note: defrag_remap_callback is specific to section-based storage.
    // With page file storage, defrag is handled differently.
    // This callback will be removed in Phase 2B integration.
}
```

For the `bnode_entry_t` field updates, replace `entry->child_section_id == section_id && entry->child_block_index == old_offset` with a check on `entry->child_disk_offset`.

- [ ] **Step 9: Fix hbtrie_gc version_entry_gc calls**

In `src/HBTrie/hbtrie.c`, find calls to `version_entry_gc` (approximately line 2071). This function takes a `sections_t*` parameter. Pass NULL since sections are being replaced:

```c
version_entry_gc(&entry->versions, min_active_txn_id, NULL);
```

Also in the loop that deallocates section storage (lines 2103-2108), replace with a no-op or remove:

```c
// Section storage deallocation handled by stale_region_mgr in page file
```

- [ ] **Step 10: Build and verify all tests pass**

Run: `cd /home/victor/Workspace/src/github.com/vijayee/WaveDB/build_debug && cmake .. -DCMAKE_BUILD_TYPE=Debug && make -j$(nproc) 2>&1 | tail -20`

Then run existing tests:
```bash
cd /home/victor/Workspace/src/github.com/vijayee/WaveDB/build_debug && ctest --output-on-failure -R "test_bnode|test_hbtrie|test_database" 2>&1 | tail -20
```

Expected: All existing tests pass (persistence is stubbed out, in-memory paths unchanged).

- [ ] **Step 11: Commit**

```bash
git add src/Storage/node_serializer.h src/Storage/node_serializer.c src/Database/database.c src/Storage/sections.c src/HBTrie/hbtrie.c
git commit -m "feat: implement V3 flat serializer and update data structures for page file"
```

---

### Task 4: V3 Serializer Tests

**Files:**
- Create: `tests/test_v3_serializer.cpp`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Create test file with failing test for V3 leaf bnode round-trip**

Create `tests/test_v3_serializer.cpp`:

```cpp
#include <gtest/gtest.h>
#include "Storage/node_serializer.h"
#include "HBTrie/bnode.h"
#include "HBTrie/chunk.h"
#include "HBTrie/identifier.h"
#include "Buffer/buffer.h"

class V3SerializerTest : public ::testing::Test {
protected:
    uint8_t chunk_size = 4;
    uint32_t btree_node_size = 4096;

    chunk_t* make_chunk(const char* data) {
        return chunk_create((const uint8_t*)data, chunk_size);
    }

    identifier_t* make_ident(const char* data) {
        size_t len = strlen(data);
        buffer_t* buf = buffer_create(len);
        memcpy(buf->data, data, len);
        buf->size = len;
        identifier_t* ident = identifier_create(buf, chunk_size);
        buffer_destroy(buf);
        return ident;
    }
};

// Test 1: Serialize/deserialize a leaf bnode with value entries
TEST_F(V3SerializerTest, LeafNodeWithValueRoundTrip) {
    bnode_t* node = bnode_create_with_level(btree_node_size, 1);
    ASSERT_NE(node, nullptr);

    // Add entry with value
    bnode_entry_t entry1;
    memset(&entry1, 0, sizeof(entry1));
    chunk_t* key1 = make_chunk("abcd");
    bnode_entry_set_key(&entry1, key1);
    entry1.has_value = 1;
    entry1.value = make_ident("value1");
    bnode_insert(node, &entry1);
    chunk_destroy(key1);

    // Add another entry with value
    bnode_entry_t entry2;
    memset(&entry2, 0, sizeof(entry2));
    chunk_t* key2 = make_chunk("efgh");
    bnode_entry_set_key(&entry2, key2);
    entry2.has_value = 1;
    entry2.value = make_ident("value2");
    bnode_insert(node, &entry2);
    chunk_destroy(key2);

    // Serialize with V3
    uint8_t* buf = nullptr;
    size_t len = 0;
    int rc = bnode_serialize_v3(node, chunk_size, &buf, &len);
    EXPECT_EQ(rc, 0);
    EXPECT_GT(len, 0u);
    EXPECT_NE(buf, nullptr);

    // Verify magic byte
    EXPECT_EQ(buf[0], 0xB5);

    // Deserialize
    node_location_t* locations = nullptr;
    size_t num_locations = 0;
    bnode_t* result = bnode_deserialize_v3(buf, len, chunk_size, btree_node_size,
                                            &locations, &num_locations);
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(atomic_load(&result->level), 1u);
    EXPECT_EQ(result->entries.length, 2u);

    // Verify value entries have child_disk_offset = 0 (no children)
    EXPECT_EQ(locations[0].offset, 0u);
    EXPECT_EQ(locations[1].offset, 0u);

    // Verify the values survived round-trip
    bnode_entry_t* r_entry1 = bnode_find(result, make_chunk("abcd"), nullptr);
    ASSERT_NE(r_entry1, nullptr);
    EXPECT_EQ(r_entry1->has_value, 1);

    free(locations);
    free(buf);
    bnode_destroy(result);
    bnode_destroy(node);
}

// Test 2: Serialize/deserialize internal bnode with child_disk_offset
TEST_F(V3SerializerTest, InternalNodeWithChildOffsetRoundTrip) {
    bnode_t* node = bnode_create_with_level(btree_node_size, 2);
    ASSERT_NE(node, nullptr);

    // Add entry with child bnode offset
    bnode_entry_t entry1;
    memset(&entry1, 0, sizeof(entry1));
    chunk_t* key1 = make_chunk("abcd");
    bnode_entry_set_key(&entry1, key1);
    entry1.is_bnode_child = 1;
    entry1.child_disk_offset = 4096;  // Points to child bnode at offset 4096
    bnode_insert(node, &entry1);
    chunk_destroy(key1);

    // Add entry with hbtrie_node child offset
    bnode_entry_t entry2;
    memset(&entry2, 0, sizeof(entry2));
    chunk_t* key2 = make_chunk("efgh");
    bnode_entry_set_key(&entry2, key2);
    entry2.child_disk_offset = 8192;  // Points to child hbtrie_node at offset 8192
    bnode_insert(node, &entry2);
    chunk_destroy(key2);

    // Serialize with V3
    uint8_t* buf = nullptr;
    size_t len = 0;
    int rc = bnode_serialize_v3(node, chunk_size, &buf, &len);
    EXPECT_EQ(rc, 0);

    // Deserialize
    node_location_t* locations = nullptr;
    size_t num_locations = 0;
    bnode_t* result = bnode_deserialize_v3(buf, len, chunk_size, btree_node_size,
                                            &locations, &num_locations);
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(atomic_load(&result->level), 2u);
    EXPECT_EQ(result->entries.length, 2u);

    // Verify child_disk_offset values preserved
    EXPECT_EQ(locations[0].offset, 4096u);  // bnode_child offset
    EXPECT_EQ(locations[1].offset, 8192u);  // hbtrie_node offset

    // Verify entries have child_disk_offset set, pointers NULL
    bnode_entry_t* r_entry1 = bnode_find(result, make_chunk("abcd"), nullptr);
    ASSERT_NE(r_entry1, nullptr);
    EXPECT_EQ(r_entry1->is_bnode_child, 1);
    EXPECT_EQ(r_entry1->child_disk_offset, 4096u);
    EXPECT_EQ(r_entry1->child_bnode, nullptr);  // Lazy: not loaded

    bnode_entry_t* r_entry2 = bnode_find(result, make_chunk("efgh"), nullptr);
    ASSERT_NE(r_entry2, nullptr);
    EXPECT_EQ(r_entry2->child_disk_offset, 8192u);
    EXPECT_EQ(r_entry2->child, nullptr);  // Lazy: not loaded

    free(locations);
    free(buf);
    bnode_destroy(result);
    bnode_destroy(node);
}

// Test 3: V3 cannot deserialize V2 data (magic mismatch)
TEST_F(V3SerializerTest, V3RejectsV2Data) {
    // Create a V2 serialized bnode
    bnode_t* node = bnode_create_with_level(btree_node_size, 1);
    bnode_entry_t entry;
    memset(&entry, 0, sizeof(entry));
    chunk_t* key = make_chunk("abcd");
    bnode_entry_set_key(&entry, key);
    entry.has_value = 1;
    entry.value = make_ident("val");
    bnode_insert(node, &entry);
    chunk_destroy(key);

    uint8_t* buf = nullptr;
    size_t len = 0;
    bnode_serialize(node, chunk_size, &buf, &len);  // V2 serialize
    EXPECT_EQ(buf[0], 0xB4);  // V2 magic

    // V3 deserializer should reject V2 data
    node_location_t* locations = nullptr;
    size_t num_locations = 0;
    bnode_t* result = bnode_deserialize_v3(buf, len, chunk_size, btree_node_size,
                                            &locations, &num_locations);
    EXPECT_EQ(result, nullptr);  // Should fail: wrong magic

    free(buf);
    bnode_destroy(node);
}

// Test 4: Zero-offset children (new node, not yet persisted)
TEST_F(V3SerializerTest, ZeroOffsetChildren) {
    bnode_t* node = bnode_create_with_level(btree_node_size, 2);
    ASSERT_NE(node, nullptr);

    bnode_entry_t entry;
    memset(&entry, 0, sizeof(entry));
    chunk_t* key = make_chunk("abcd");
    bnode_entry_set_key(&entry, key);
    entry.is_bnode_child = 1;
    entry.child_disk_offset = 0;  // Not yet persisted
    bnode_insert(node, &entry);
    chunk_destroy(key);

    uint8_t* buf = nullptr;
    size_t len = 0;
    bnode_serialize_v3(node, chunk_size, &buf, &len);
    EXPECT_EQ(buf[0], 0xB5);

    node_location_t* locations = nullptr;
    size_t num_locations = 0;
    bnode_t* result = bnode_deserialize_v3(buf, len, chunk_size, btree_node_size,
                                            &locations, &num_locations);
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(locations[0].offset, 0u);

    free(locations);
    free(buf);
    bnode_destroy(result);
    bnode_destroy(node);
}
```

- [ ] **Step 2: Add test_v3_serializer target to CMakeLists.txt**

In `CMakeLists.txt`, add after the `test_bnode_cache` target block (around line 256):

```cmake
# V3 serializer tests
add_executable(test_v3_serializer tests/test_v3_serializer.cpp)
target_link_libraries(test_v3_serializer ${WAVEDB_LIBS} gtest gtest_main)
add_test(NAME test_v3_serializer COMMAND test_v3_serializer)
```

- [ ] **Step 3: Build and run V3 serializer tests**

Run: `cd /home/victor/Workspace/src/github.com/vijayee/WaveDB/build_debug && cmake .. -DCMAKE_BUILD_TYPE=Debug && make -j$(nproc) test_v3_serializer 2>&1 | tail -20`

Then: `cd /home/victor/Workspace/src/github.com/vijayee/WaveDB/build_debug && ./test_v3_serializer 2>&1`

Expected: All 4 tests PASS.

- [ ] **Step 4: Run full test suite under ASan**

Run: `cd /home/victor/Workspace/src/github.com/vijayee/WaveDB && mkdir -p build_asan && cd build_asan && cmake .. -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_FLAGS="-fsanitize=address -fno-omit-frame-pointer" -DCMAKE_CXX_FLAGS="-fsanitize=address -fno-omit-frame-pointer" -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address" && make -j$(nproc) 2>&1 | tail -10`

Then: `cd /home/victor/Workspace/src/github.com/vijayee/WaveDB/build_asan && ./test_v3_serializer 2>&1`

Expected: All 4 tests PASS with zero ASan errors/leaks.

- [ ] **Step 5: Commit**

```bash
git add tests/test_v3_serializer.cpp CMakeLists.txt
git commit -m "test: add V3 flat serializer round-trip tests"
```

---

## Self-Review

**1. Spec coverage:**
- Section 1 (V3 format): Task 3 implements serializer, Task 4 tests it
- Section 2 (data structure changes): Tasks 1-2 add all new fields
- Sections 3-10 (CoW, lazy loading, integration): Not in Phase 2A — deferred to Phase 2B plan

**2. Placeholder scan:**
- No TBD/TODO found
- All steps have exact code or specific instructions
- Test code is complete

**3. Type consistency:**
- `child_disk_offset` (uint64_t) used consistently in bnode_entry_t, serializer, deserializer
- `disk_offset` (uint64_t) used in bnode_t and hbtrie_node_t
- `node_location_t.offset` (uint64_t) consistent with `child_disk_offset`
- `is_dirty` (uint8_t) consistent in bnode_t and hbtrie_node_t