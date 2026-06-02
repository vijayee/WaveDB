# Sync-Only Mode — Phase 1: Core C Infrastructure

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a `sync_only` flag to `database_config_t` that causes `database_put_sync`/`database_get_sync`/`database_delete_sync` to skip all concurrency control overhead (MVCC tx_manager, spinlocks, hbtrie per-node locks, LRU mutexes) while preserving WAL durability and cross-mode compatibility.

**Architecture:** The `sync_only` flag is stored in `database_config_t` and persisted to `.config`. When set, `database_create_with_config` skips creating the pool, wheel, tx_manager, and write_locks. The sync API functions branch on `db->sync_only` to take a fast path: generate txn IDs directly (no tx_manager), call `hbtrie_*_unsafe` variants (no per-node locks), and use unsynchronized LRU access (no mutex). WAL writes are preserved. Values are stored in legacy mode (`has_versions=0`) for compatibility with async mode.

**Tech Stack:** C11, pthreads, HBTrie, WAL

---

## File Structure

| File | Role |
|------|------|
| `src/Database/database_config.h` | Add `sync_only` field to `database_config_t` |
| `src/Database/database_config.c` | Serialize/deserialize/merge/setter for `sync_only` |
| `src/Database/database.h` | Add `sync_only` to `database_t` inner struct |
| `src/Database/database.c` | Modify `database_create_with_config` + fast-path sync functions |
| `src/HBTrie/hbtrie.h` | Declare `hbtrie_insert_unsafe`, `hbtrie_find_unsafe`, `hbtrie_delete_unsafe` |
| `src/HBTrie/hbtrie.c` | Implement unsafe variants (same logic, no locks) |
| `src/Database/database_lru.h` | Declare `database_lru_cache_get_unsafe`, `database_lru_cache_put_unsafe` |
| `src/Database/database_lru.c` | Implement unsafe LRU variants (no mutex) |
| `tests/test_sync_only.c` | New test file — functional and benchmark tests |

---

### Task 1: Add `sync_only` to `database_config_t`

**Files:**
- Modify: `src/Database/database_config.h:59-80`
- Modify: `src/Database/database_config.c:14-48,127-200,321-400,451-500`
- Create: (none — extend existing functions)

**Context:** The `database_config_t` struct has three sections: immutable settings, mutable settings, and threading settings. `sync_only` is an immutable setting — once set at creation, it cannot be changed on reopen. It should be persisted to `.config` so recovery/reopen knows the database was created in sync-only mode.

- [ ] **Step 1: Add field to struct**

In `src/Database/database_config.h`, add after the `encryption_config_t` line (line 64):

```c
    uint8_t sync_only;            // 1 = sync-only (no MVCC, no locks), 0 = concurrent
```

- [ ] **Step 2: Set default in `database_config_default()`**

In `src/Database/database_config.c`, in `database_config_default()`, add after the `encryption` default:

```c
    config->sync_only = 0;  // Default: concurrent mode
```

- [ ] **Step 3: Add CBOR serialization**

In `database_config_save()`, add a new map entry (increment the map size). There are currently 11 entries. Add as entry 12:

```c
    cbor_item_t* sync_only_key = cbor_build_string("sync_only");
    cbor_item_t* sync_only_val = cbor_build_uint8(config->sync_only);
    ok = cbor_map_add(config_map, (struct cbor_pair){
        .key = cbor_move(sync_only_key),
        .value = cbor_move(sync_only_val)
    });
    if (!ok) { cbor_decref(&sync_only_key); cbor_decref(&sync_only_val); }
```

- [ ] **Step 4: Add CBOR deserialization**

In `database_config_load()`, add a case for reading `sync_only` from the CBOR map. In the loop that iterates map pairs, add:

```c
    if (strcmp(key_str, "sync_only") == 0 && cbor_isa_uint(value)) {
        config->sync_only = cbor_get_uint8(value);
    }
```

- [ ] **Step 5: Handle in `database_config_merge()`**

`sync_only` is immutable — once persisted, it can't change. In `database_config_merge()`, add after the existing immutable field copies:

```c
    merged->sync_only = saved->sync_only;  // Immutable: use saved value
```

- [ ] **Step 6: Add setter function**

At the end of `src/Database/database_config.c`, add:

```c
void database_config_set_sync_only(database_config_t* config, uint8_t sync_only) {
    if (config == NULL) return;
    config->sync_only = sync_only ? 1 : 0;
}
```

In `src/Database/database_config.h`, add the declaration after the other setter declarations:

```c
void database_config_set_sync_only(database_config_t* config, uint8_t sync_only);
```

- [ ] **Step 7: Build and verify compilation**

```bash
cd build && cmake .. && make -j$(nproc) 2>&1 | head -30
```

Expected: Clean build, no errors.

- [ ] **Step 8: Commit**

```bash
git add src/Database/database_config.h src/Database/database_config.c
git commit -m "feat: add sync_only field to database_config_t with serialization"
```

---

### Task 2: Modify `database_create_with_config` for sync-only

**Files:**
- Modify: `src/Database/database.h:53-91`
- Modify: `src/Database/database.c:700-1050`

**Context:** Currently `database_create_with_config` requires `worker_threads > 0` and creates a pool + wheel. For sync-only mode, we skip pool, wheel, tx_manager, and write_locks init. We still create LRU, trie, page file, bnode cache, and WAL manager.

- [ ] **Step 1: Add `sync_only` field to `database_t`**

In `src/Database/database.h`, add at the end of the `database_t` struct (after `encryption_t* encryption`):

```c
    uint8_t sync_only;                  // 1 = sync-only mode (no concurrency control)
```

- [ ] **Step 2: Store sync_only flag in database_t**

In `database_create_with_config()`, after setting `db->chunk_size` etc. (around line 774), add:

```c
    db->sync_only = effective_config->sync_only;
```

- [ ] **Step 3: Allow worker_threads=0 when sync_only**

Replace the pool creation block (lines 777-799). The current code:

```c
    if (effective_config->external_pool != NULL) {
        db->pool = effective_config->external_pool;
        db->owns_pool = false;
    } else if (effective_config->worker_threads > 0) {
        db->pool = work_pool_create(effective_config->worker_threads);
        ...
    } else {
        // No pool available → error
        ...
    }
```

Change the `else` branch to only error when NOT sync_only:

```c
    } else if (effective_config->sync_only) {
        // Sync-only mode: no pool needed
        db->pool = NULL;
        db->owns_pool = false;
    } else {
        // No pool available
        if (error_code) *error_code = EINVAL;
        free(db->location);
        free(db);
        if (owns_config) database_config_destroy(effective_config);
        return NULL;
    }
```

- [ ] **Step 4: Allow timer_resolution_ms=0 when sync_only**

Replace the wheel creation block (lines 801-824). Change the final `else` to:

```c
    } else if (effective_config->sync_only) {
        // Sync-only mode: no wheel needed
        db->wheel = NULL;
        db->owns_wheel = false;
    } else {
        // No wheel available → error
        ...
    }
```

- [ ] **Step 5: Skip write_locks init when sync_only**

Wrap the write_locks init block (lines 838-841):

```c
    if (!db->sync_only) {
        for (int i = 0; i < WRITE_LOCK_SHARDS; i++) {
            spinlock_init(&db->write_locks[i]);
        }
    }
```

- [ ] **Step 6: Skip tx_manager creation when sync_only**

Wrap the tx_manager creation block (lines 981-1005):

```c
    if (!db->sync_only) {
        db->tx_manager = tx_manager_create(db->trie, db->pool, db->wheel, 100);
        if (db->has_pending_txn_id && db->tx_manager != NULL) {
            transaction_id_advance_to(&db->pending_txn_id);
            atomic_store(&db->tx_manager->last_committed_txn_id, db->pending_txn_id);
            db->has_pending_txn_id = 0;
        }
        if (db->tx_manager == NULL) {
            // ... error cleanup ...
            return NULL;
        }
    } else {
        db->tx_manager = NULL;
    }
```

- [ ] **Step 7: Skip eviction task start when sync_only**

Wrap the eviction task enqueue (lines 948-956) in `if (!db->sync_only && db->pool != NULL)`.

- [ ] **Step 8: Update database_destroy for sync_only**

In `database_destroy()`, guard pool/wheel/tx_manager destruction with `!db->sync_only` or NULL checks. The existing NULL checks on `db->owns_pool` and `db->owns_wheel` already handle sync_only (since `owns_pool=false` and `owns_wheel=false`). Add a guard for `tx_manager_destroy`:

```c
    if (db->tx_manager != NULL) {
        tx_manager_destroy(db->tx_manager);
    }
```

(Should already be present — verify.)

- [ ] **Step 9: Build and verify compilation**

```bash
cd build && cmake .. && make -j$(nproc) 2>&1 | grep -i error
```

Expected: No errors.

- [ ] **Step 10: Commit**

```bash
git add src/Database/database.h src/Database/database.c
git commit -m "feat: allow sync_only mode in database_create_with_config (skip pool/wheel/tx_manager)"
```

---

### Task 3: Add unsafe hbtrie variants

**Files:**
- Modify: `src/HBTrie/hbtrie.h:281-318`
- Modify: `src/HBTrie/hbtrie.c:1926-2300,2090-2400,2553-2650`

**Context:** The unsafe variants skip per-node spinlocks and seqlock validation. `hbtrie_find_unsafe` does a direct traversal without the seqlock retry loop. `hbtrie_insert_unsafe` skips `spinlock_lock`/`spinlock_unlock` + `atomic_fetch_add` on seq. `hbtrie_delete_unsafe` likewise skips per-node locks. All three still handle `has_versions` for cross-mode compatibility (reading version chains written by async mode).

- [ ] **Step 1: Declare unsafe functions in hbtrie.h**

After the existing function declarations (line ~318), add:

```c
/**
 * Unsafe variants for sync-only mode.
 * Skip all per-node spinlocks and seqlock validation.
 * Still handle has_versions for cross-mode compatibility.
 */
identifier_t* hbtrie_find_unsafe(hbtrie_t* trie, path_t* path);
int hbtrie_insert_unsafe(hbtrie_t* trie, path_t* path, identifier_t* value, transaction_id_t txn_id);
identifier_t* hbtrie_delete_unsafe(hbtrie_t* trie, path_t* path, transaction_id_t txn_id);
```

- [ ] **Step 2: Implement `hbtrie_find_unsafe`**

Copy `hbtrie_find` (lines 1926-2004) and simplify:
- Remove the outer `for (;;)` seqlock retry loop
- Remove `seq_before`/`seq_after` reads and validation
- Keep the `has_versions` check and version chain traversal (for compat with async-written data)
- Keep `bnode_find_leaf_lazy` calls

```c
identifier_t* hbtrie_find_unsafe(hbtrie_t* trie, path_t* path) {
    if (trie == NULL || path == NULL) return NULL;
    
    size_t path_len_ids = path_length(path);
    if (path_len_ids == 0) return NULL;
    
    hbtrie_node_t* current = atomic_load(&trie->root);
    
    for (size_t i = 0; i < path_len_ids; i++) {
        identifier_t* identifier = path_get(path, i);
        if (identifier == NULL) return NULL;
        
        size_t nchunk = identifier_chunk_count(identifier);
        for (size_t j = 0; j < nchunk; j++) {
            chunk_t* chunk = identifier_get_chunk(identifier, j);
            if (chunk == NULL) return NULL;
            
            size_t index;
            bnode_entry_t* entry = bnode_find_leaf_lazy(current->btree, chunk, &index,
                                                          trie->fcache, trie->chunk_size,
                                                          trie->btree_node_size);
            
            int is_last_chunk = (j == nchunk - 1);
            int is_last_identifier = (i == path_len_ids - 1);
            
            if (is_last_chunk && is_last_identifier) {
                if (entry == NULL || !entry->has_value) return NULL;
                
                if (entry->has_versions) {
                    // MVCC data from prior async use — return most recent version
                    // In sync-only there are no concurrent txns, so take the first
                    // non-deleted version
                    version_entry_t* ver = entry->versions;
                    while (ver != NULL) {
                        if (ver->value != NULL) {
                            return (identifier_t*)refcounter_reference((refcounter_t*)ver->value);
                        }
                        ver = ver->next;
                    }
                    return NULL;  // All versions are tombstones
                } else {
                    if (entry->value == NULL) return NULL;
                    return (identifier_t*)refcounter_reference((refcounter_t*)entry->value);
                }
            } else if (is_last_chunk) {
                if (entry != NULL && entry->has_value) {
                    if (entry->trie_child == NULL && entry->child_disk_offset != 0 && trie->fcache != NULL) {
                        bnode_entry_lazy_load_trie_child(entry, trie->fcache,
                                                          trie->chunk_size, trie->btree_node_size);
                    }
                    current = entry->trie_child;
                } else {
                    current = NULL;
                }
                if (current == NULL) return NULL;
            } else {
                if (entry != NULL && entry->has_value) {
                    if (entry->trie_child == NULL && entry->child_disk_offset != 0 && trie->fcache != NULL) {
                        bnode_entry_lazy_load_trie_child(entry, trie->fcache,
                                                          trie->chunk_size, trie->btree_node_size);
                    }
                    current = entry->trie_child;
                }
                if (current == NULL) return NULL;
            }
        }
    }
    return NULL;
}
```

- [ ] **Step 3: Implement `hbtrie_insert_unsafe`**

Copy `hbtrie_insert` (lines 2090-2300) and:
- Remove all `spinlock_lock(&current->write_lock)` / `spinlock_unlock(&current->write_lock)`
- Remove all `atomic_fetch_add(&current->seq, ...)` calls
- Keep `has_versions=0` (legacy mode) for the value — this is the key compat feature
- Keep MVCC version chain upgrade for existing has_versions=1 entries (compat with async)

The pattern everywhere: replace `spinlock_lock(&current->write_lock)` + `atomic_fetch_add(&current->seq, 1)` with nothing, and replace `atomic_fetch_add(&current->seq, 1)` + `spinlock_unlock(&current->write_lock)` with nothing.

- [ ] **Step 4: Implement `hbtrie_delete_unsafe`**

Copy `hbtrie_delete` (line 2553+) and apply the same lock removal pattern as Step 3.

- [ ] **Step 5: Build and fix compilation errors**

```bash
cd build && cmake .. && make -j$(nproc) 2>&1 | grep -E "error|warning"
```

Expected: No errors. Fix any missing includes or undeclared functions.

- [ ] **Step 6: Commit**

```bash
git add src/HBTrie/hbtrie.h src/HBTrie/hbtrie.c
git commit -m "feat: add hbtrie unsafe variants for sync-only mode (no per-node locks)"
```

---

### Task 4: Add unsafe LRU variants

**Files:**
- Modify: `src/Database/database_lru.h:90-103`
- Modify: `src/Database/database_lru.c:266-330`

**Context:** The unsafe variants skip `platform_lock(&shard->lock)` / `platform_unlock(&shard->lock)`. They also skip `path_copy` in `_put_unsafe` — the caller transfers path ownership directly.

- [ ] **Step 1: Declare unsafe functions in database_lru.h**

After the existing function declarations (line ~103), add:

```c
/**
 * Unsafe variants for sync-only mode.
 * Skip all mutex locking. Not thread-safe.
 */
identifier_t* database_lru_cache_get_unsafe(database_lru_cache_t* lru, path_t* path);
identifier_t* database_lru_cache_put_unsafe(database_lru_cache_t* lru, path_t* path, identifier_t* value);
void database_lru_cache_delete_unsafe(database_lru_cache_t* lru, path_t* path);
```

- [ ] **Step 2: Implement `database_lru_cache_get_unsafe`**

Copy `database_lru_cache_get` (line 266), remove `platform_lock(&shard->lock)` and `platform_unlock(&shard->lock)`:

```c
identifier_t* database_lru_cache_get_unsafe(database_lru_cache_t* lru, path_t* path) {
    if (lru == NULL || path == NULL) return NULL;
    
    size_t shard_idx = get_shard_index(lru, path);
    database_lru_shard_t* shard = &lru->shards[shard_idx];
    
    database_lru_node_t* node = hashmap_get(&shard->cache, path);
    if (node == NULL) return NULL;
    
    lru_move_to_front(shard, node);
    return (identifier_t*)refcounter_reference((refcounter_t*)node->value);
}
```

- [ ] **Step 3: Implement `database_lru_cache_put_unsafe`**

Copy `database_lru_cache_put` (line 292), remove locking. **Do NOT call `path_copy`** — in sync-only mode, the caller transfers path ownership:

```c
identifier_t* database_lru_cache_put_unsafe(database_lru_cache_t* lru, path_t* path, identifier_t* value) {
    if (lru == NULL || path == NULL) {
        if (path != NULL) path_destroy(path);
        if (value != NULL) identifier_destroy(value);
        return NULL;
    }
    
    size_t shard_idx = get_shard_index(lru, path);
    database_lru_shard_t* shard = &lru->shards[shard_idx];
    
    database_lru_node_t* existing = hashmap_get(&shard->cache, path);
    identifier_t* ejected = NULL;
    
    if (existing != NULL) {
        identifier_t* old_value = existing->value;
        existing->value = value;
        size_t old_memory = existing->memory_size;
        existing->memory_size = calculate_entry_memory(path, value);
        shard->current_memory += (existing->memory_size - old_memory);
        path_destroy(path);
        lru_move_to_front(shard, existing);
        return old_value;  // Caller must destroy
    }
    
    // Evict if needed
    while (shard->current_memory + calculate_entry_memory(path, value) > shard->max_memory
           && shard->last != NULL) {
        database_lru_node_t* lru_node = shard->last;
        hashmap_remove(&shard->cache, lru_node->path);
        shard->current_memory -= lru_node->memory_size;
        lru_remove(shard, lru_node);
        path_destroy(lru_node->path);
        identifier_destroy(lru_node->value);
        free(lru_node);
        shard->entry_count--;
    }
    
    // Create new node
    database_lru_node_t* new_node = get_clear_memory(sizeof(database_lru_node_t));
    if (new_node == NULL) {
        path_destroy(path);
        identifier_destroy(value);
        return NULL;
    }
    
    new_node->path = path;  // Take ownership (no path_copy!)
    new_node->key_hash = hash_path(path);
    new_node->value = value;  // Take ownership
    new_node->memory_size = calculate_entry_memory(path, value);
    
    hashmap_put(&shard->cache, path, new_node);
    shard->current_memory += new_node->memory_size;
    shard->entry_count++;
    lru_move_to_front(shard, new_node);
    return NULL;
}
```

- [ ] **Step 4: Implement `database_lru_cache_delete_unsafe`**

Same pattern — copy `database_lru_cache_delete`, remove locking.

- [ ] **Step 5: Build and verify**

```bash
cd build && cmake .. && make -j$(nproc) 2>&1 | grep -i error
```

Expected: No errors.

- [ ] **Step 6: Commit**

```bash
git add src/Database/database_lru.h src/Database/database_lru.c
git commit -m "feat: add unsafe LRU cache variants for sync-only mode (no mutex)"
```

---

### Task 5: Add fast-path in sync API functions

**Files:**
- Modify: `src/Database/database.c:1697-1860`

**Context:** `database_put_sync` and `database_delete_sync` branch on `db->sync_only`. In sync-only mode they: generate txn_id directly, skip tx_manager, skip write_lock spinlock, call `hbtrie_*_unsafe`, use unsafe LRU access. `database_get_sync` also takes a fast path using unsafe LRU and `hbtrie_find_unsafe`.

- [ ] **Step 1: Implement `database_put_sync` fast path**

Replace `database_put_sync` (lines 1697-1765) with a version that branches on `db->sync_only`:

```c
int database_put_sync(database_t* db, path_t* path, identifier_t* value) {
    if (db == NULL || path == NULL || value == NULL) {
        if (path) path_destroy(path);
        if (value) identifier_destroy(value);
        return -1;
    }
    
    // Generate transaction ID (needed for WAL)
    transaction_id_t txn_id = transaction_id_get_next();
    
    // Sync-only fast path: no tx_manager, no locks, unsafe hbtrie + LRU
    if (db->sync_only) {
        // Write WAL for durability
        if (db->wal_manager != NULL) {
            buffer_t* entry = encode_put_entry_binary(path, value);
            if (entry != NULL) {
                thread_wal_t* twal = get_thread_wal(db->wal_manager);
                if (twal != NULL) {
                    thread_wal_write(twal, txn_id, WAL_PUT, entry);
                }
                buffer_destroy(entry);
            }
        }
        
        // Insert into trie (no locks, legacy mode has_versions=0)
        hbtrie_insert_unsafe(db->trie, path, value, txn_id);
        
        // Update LRU cache (no lock, no path_copy — transfers ownership)
        path_t* cache_path = path_copy(path);
        identifier_t* value_ref = REFERENCE(value, identifier_t);
        identifier_t* ejected = database_lru_cache_put_unsafe(db->lru, cache_path, value_ref);
        if (ejected) identifier_destroy(ejected);
        
        path_destroy(path);
        identifier_destroy(value);
        return 0;
    }
    
    // Original concurrent path follows...
    txn_desc_t* txn = tx_manager_begin(db->tx_manager);
    ...
```

- [ ] **Step 2: Implement `database_get_sync` fast path**

Replace `database_get_sync` (lines 1767-1817) with a version that branches on `db->sync_only`:

```c
int database_get_sync(database_t* db, path_t* path, identifier_t** result) {
    if (result == NULL) {
        if (path) path_destroy(path);
        return -1;
    }
    *result = NULL;
    if (db == NULL || path == NULL) {
        if (path) path_destroy(path);
        return -1;
    }
    
    // Sync-only fast path: unsafe LRU + unsafe trie find (no locks, no MVCC)
    if (db->sync_only) {
        // Check LRU first (no lock)
        identifier_t* value = database_lru_cache_get_unsafe(db->lru, path);
        if (value != NULL) {
            path_destroy(path);
            *result = value;
            return 0;
        }
        
        // Trie lookup (no seqlock, no MVCC txn)
        value = hbtrie_find_unsafe(db->trie, path);
        if (value != NULL) {
            // Add to LRU (no lock, no path_copy race)
            path_t* cache_path = path_copy(path);
            identifier_t* cached = REFERENCE(value, identifier_t);
            identifier_t* ejected = database_lru_cache_put_unsafe(db->lru, cache_path, cached);
            if (ejected) identifier_destroy(ejected);
        }
        
        path_destroy(path);
        if (value != NULL) {
            *result = value;
            return 0;
        }
        return -2;  // Not found
    }
    
    // Original concurrent path follows...
    identifier_t* value = database_lru_cache_get(db->lru, path);
    ...
```

- [ ] **Step 3: Implement `database_delete_sync` fast path**

Same pattern as `database_put_sync` — branch on `db->sync_only`, call `hbtrie_delete_unsafe` instead of `hbtrie_delete`, skip tx_manager and write_locks spinlock.

- [ ] **Step 4: Add `database_flush_dirty_bnodes` sync-only guard**

In `database_flush_dirty_bnodes()`, if `db->sync_only`, skip any pool-dependent operations (eviction scheduling).

- [ ] **Step 5: Build and verify**

```bash
cd build && cmake .. && make -j$(nproc) 2>&1 | grep -i error
```

Expected: No errors.

- [ ] **Step 6: Commit**

```bash
git add src/Database/database.c
git commit -m "feat: add sync-only fast path to database_put/get/delete_sync"
```

---

### Task 6: Add sync-only functional and benchmark tests

**Files:**
- Create: `tests/test_sync_only.c`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Write functional test**

Create `tests/test_sync_only.c`:

```c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "Database/database.h"
#include "Database/database_config.h"
#include "HBTrie/path.h"
#include "HBTrie/identifier.h"
#include "Buffer/buffer.h"

static path_t* make_path(const char* key) {
    path_t* path = path_create();
    buffer_t* buf = buffer_create_from_pointer_copy((uint8_t*)key, strlen(key));
    identifier_t* id = identifier_create(buf, 0);
    buffer_destroy(buf);
    path_append(path, id);
    identifier_destroy(id);
    return path;
}

static identifier_t* make_value(const char* data) {
    buffer_t* buf = buffer_create_from_pointer_copy((uint8_t*)data, strlen(data));
    identifier_t* id = identifier_create(buf, 0);
    buffer_destroy(buf);
    return id;
}

static int test_sync_only_put_get(void) {
    printf("Test: sync_only put/get ... ");
    
    mkdir("/tmp/test_sync_only", 0755);
    
    database_config_t* config = database_config_default();
    config->sync_only = 1;
    config->worker_threads = 0;
    config->timer_resolution_ms = 0;
    config->enable_persist = 1;
    wal_config_t wal_cfg = {.sync_mode = WAL_SYNC_ASYNC, .debounce_ms = 100,
                            .idle_threshold_ms = 10000, .compact_interval_ms = 60000,
                            .max_file_size = 100*1024*1024, .max_sealed_wals = 0};
    config->wal_config = wal_cfg;
    
    int error_code = 0;
    database_t* db = database_create_with_config("/tmp/test_sync_only", config, &error_code);
    if (db == NULL) {
        printf("FAIL (create failed, error=%d)\n", error_code);
        database_config_destroy(config);
        return 1;
    }
    
    // Put
    path_t* p = make_path("hello");
    identifier_t* v = make_value("world");
    int rc = database_put_sync(db, p, v);
    if (rc != 0) {
        printf("FAIL (put returned %d)\n", rc);
        database_destroy(db);
        database_config_destroy(config);
        return 1;
    }
    
    // Get
    path_t* gp = make_path("hello");
    identifier_t* result = NULL;
    rc = database_get_sync(db, gp, &result);
    if (rc != 0 || result == NULL) {
        printf("FAIL (get returned %d, result=%p)\n", rc, (void*)result);
        database_destroy(db);
        database_config_destroy(config);
        return 1;
    }
    
    size_t len = 0;
    uint8_t* data = identifier_get_data_copy(result, &len);
    int ok = (len == 5 && memcmp(data, "world", 5) == 0);
    free(data);
    identifier_destroy(result);
    
    if (!ok) {
        printf("FAIL (value mismatch)\n");
        database_destroy(db);
        database_config_destroy(config);
        return 1;
    }
    
    // Delete
    path_t* dp = make_path("hello");
    rc = database_delete_sync(db, dp);
    if (rc != 0) {
        printf("FAIL (delete returned %d)\n", rc);
        database_destroy(db);
        database_config_destroy(config);
        return 1;
    }
    
    // Verify deleted
    path_t* gp2 = make_path("hello");
    identifier_t* r2 = NULL;
    rc = database_get_sync(db, gp2, &r2);
    if (rc != -2) {
        printf("FAIL (get after delete returned %d, expected -2)\n", rc);
        database_destroy(db);
        database_config_destroy(config);
        return 1;
    }
    
    database_destroy(db);
    database_config_destroy(config);
    system("rm -rf /tmp/test_sync_only");
    printf("PASS\n");
    return 0;
}

// Verify a sync_only database can be closed and reopened by the async
// (concurrent) API, perform normal operations, then switch back to sync_only.
// This tests the full cross-mode transition chain:
//   sync_only → async → sync_only
static int test_sync_only_cross_mode(void) {
    printf("Test: sync_only → async → sync_only cross-mode ...\n");
    
    const char* db_path = "/tmp/test_sync_only_xmode";
    system("rm -rf /tmp/test_sync_only_xmode");
    mkdir(db_path, 0755);
    
    // === Phase 1: Create in sync-only mode ===
    printf("  Phase 1: Creating sync-only DB ...\n");
    database_config_t* sync_config = database_config_default();
    sync_config->sync_only = 1;
    sync_config->worker_threads = 0;
    sync_config->timer_resolution_ms = 0;
    sync_config->enable_persist = 1;
    wal_config_t wal_cfg = {.sync_mode = WAL_SYNC_ASYNC, .debounce_ms = 100,
                            .idle_threshold_ms = 10000, .compact_interval_ms = 60000,
                            .max_file_size = 100*1024*1024, .max_sealed_wals = 0};
    sync_config->wal_config = wal_cfg;
    
    int ec = 0;
    database_t* db_sync = database_create_with_config(db_path, sync_config, &ec);
    if (db_sync == NULL) {
        printf("  FAIL (sync_only create failed, error=%d)\n", ec);
        database_config_destroy(sync_config);
        return 1;
    }
    
    // Write data in sync-only mode
    path_t* p1 = make_path("alpha");
    identifier_t* v1 = make_value("sync_data");
    if (database_put_sync(db_sync, p1, v1) != 0) {
        printf("  FAIL (sync put)\n"); database_destroy(db_sync);
        database_config_destroy(sync_config); return 1;
    }
    printf("  Phase 1 OK: wrote 'alpha'='sync_data' in sync-only mode\n");
    
    database_flush_dirty_bnodes(db_sync);
    database_destroy(db_sync);
    database_config_destroy(sync_config);
    
    // === Phase 2: Reopen in async (concurrent) mode ===
    printf("  Phase 2: Reopening in async mode ...\n");
    database_config_t* async_config = database_config_default();
    async_config->sync_only = 0;  // Normal concurrent mode
    async_config->worker_threads = 2;
    async_config->timer_resolution_ms = 10;
    async_config->lru_memory_mb = 50;
    async_config->enable_persist = 1;
    async_config->wal_config = wal_cfg;
    
    ec = 0;
    database_t* db_async = database_create_with_config(db_path, async_config, &ec);
    if (db_async == NULL) {
        printf("  FAIL (async reopen failed, error=%d)\n", ec);
        database_config_destroy(async_config);
        return 1;
    }
    if (db_async->sync_only != 0) {
        printf("  FAIL (expected sync_only=0 in async mode, got %d)\n", db_async->sync_only);
        database_destroy(db_async);
        database_config_destroy(async_config);
        return 1;
    }
    
    // Read sync-written data in async mode (has_versions=0 legacy format)
    path_t* gp1 = make_path("alpha");
    identifier_t* result = NULL;
    int rc = database_get_sync(db_async, gp1, &result);
    if (rc != 0 || result == NULL) {
        printf("  FAIL (async read of sync-written key returned %d)\n", rc);
        database_destroy(db_async);
        database_config_destroy(async_config);
        return 1;
    }
    size_t len = 0;
    uint8_t* data = identifier_get_data_copy(result, &len);
    int data_ok = (len == 9 && memcmp(data, "sync_data", 9) == 0);
    free(data);
    identifier_destroy(result);
    if (!data_ok) {
        printf("  FAIL (sync-written data corrupted in async mode)\n");
        database_destroy(db_async);
        database_config_destroy(async_config);
        return 1;
    }
    printf("  Phase 2a OK: read 'alpha'='sync_data' in async mode (legacy format)\n");
    
    // Write new data in async mode (creates has_versions=1 version chains)
    path_t* p2 = make_path("beta");
    identifier_t* v2 = make_value("async_new");
    if (database_put_sync(db_async, p2, v2) != 0) {
        printf("  FAIL (async put new key)\n"); database_destroy(db_async);
        database_config_destroy(async_config); return 1;
    }
    printf("  Phase 2b OK: wrote 'beta'='async_new' in async mode (versioned)\n");
    
    // Overwrite sync-written key in async mode (upgrades to version chain)
    path_t* p1b = make_path("alpha");
    identifier_t* v1b = make_value("async_overwrite");
    if (database_put_sync(db_async, p1b, v1b) != 0) {
        printf("  FAIL (async overwrite of sync key)\n"); database_destroy(db_async);
        database_config_destroy(async_config); return 1;
    }
    
    // Verify the overwrite took effect
    path_t* gp1b = make_path("alpha");
    identifier_t* r1b = NULL;
    rc = database_get_sync(db_async, gp1b, &r1b);
    if (rc != 0 || r1b == NULL) {
        printf("  FAIL (async read of overwritten key)\n"); database_destroy(db_async);
        database_config_destroy(async_config); return 1;
    }
    len = 0;
    data = identifier_get_data_copy(r1b, &len);
    data_ok = (len == 15 && memcmp(data, "async_overwrite", 15) == 0);
    free(data);
    identifier_destroy(r1b);
    if (!data_ok) {
        printf("  FAIL (overwritten data wrong in async mode)\n");
        database_destroy(db_async);
        database_config_destroy(async_config);
        return 1;
    }
    printf("  Phase 2c OK: overwrote 'alpha'→'async_overwrite' in async mode (upgraded to versioned)\n");
    
    // Delete a key in async mode (creates tombstone in version chain)
    path_t* dp = make_path("beta");
    if (database_delete_sync(db_async, dp) != 0) {
        printf("  FAIL (async delete)\n"); database_destroy(db_async);
        database_config_destroy(async_config); return 1;
    }
    
    // Verify delete
    path_t* gp_del = make_path("beta");
    identifier_t* r_del = NULL;
    rc = database_get_sync(db_async, gp_del, &r_del);
    if (rc != -2) {
        printf("  FAIL (async delete not reflected, rc=%d)\n", rc);
        database_destroy(db_async);
        database_config_destroy(async_config);
        return 1;
    }
    printf("  Phase 2d OK: deleted 'beta' in async mode (tombstone)\n");
    
    database_flush_dirty_bnodes(db_async);
    database_destroy(db_async);
    database_config_destroy(async_config);
    
    // === Phase 3: Reopen in sync-only mode ===
    printf("  Phase 3: Reopening in sync-only mode ...\n");
    database_config_t* sync_config2 = database_config_default();
    // sync_only=1 is loaded from saved config; just pass minimal mutable settings
    sync_config2->worker_threads = 0;
    sync_config2->timer_resolution_ms = 0;
    sync_config2->lru_memory_mb = 50;
    sync_config2->enable_persist = 1;
    sync_config2->wal_config = wal_cfg;
    
    ec = 0;
    database_t* db_sync2 = database_create_with_config(db_path, sync_config2, &ec);
    if (db_sync2 == NULL) {
        printf("  FAIL (sync_only reopen failed, error=%d)\n", ec);
        database_config_destroy(sync_config2);
        return 1;
    }
    if (db_sync2->sync_only != 1) {
        printf("  FAIL (expected sync_only=1 from saved config, got %d)\n", db_sync2->sync_only);
        database_destroy(db_sync2);
        database_config_destroy(sync_config2);
        return 1;
    }
    
    // Read the overwritten key (has_versions=1 from async upgrade)
    path_t* gp3 = make_path("alpha");
    identifier_t* r3 = NULL;
    rc = database_get_sync(db_sync2, gp3, &r3);
    if (rc != 0 || r3 == NULL) {
        printf("  FAIL (sync read of async-overwritten key returned %d)\n", rc);
        database_destroy(db_sync2);
        database_config_destroy(sync_config2);
        return 1;
    }
    len = 0;
    data = identifier_get_data_copy(r3, &len);
    data_ok = (len == 15 && memcmp(data, "async_overwrite", 15) == 0);
    free(data);
    identifier_destroy(r3);
    if (!data_ok) {
        printf("  FAIL (async-overwritten data not visible in sync mode)\n");
        database_destroy(db_sync2);
        database_config_destroy(sync_config2);
        return 1;
    }
    printf("  Phase 3a OK: read 'alpha'='async_overwrite' (versioned) in sync mode\n");
    
    // Verify deleted key is still deleted
    path_t* gp_del2 = make_path("beta");
    identifier_t* r_del2 = NULL;
    rc = database_get_sync(db_sync2, gp_del2, &r_del2);
    if (rc != -2) {
        printf("  FAIL (async-deleted key still visible in sync mode, rc=%d)\n", rc);
        database_destroy(db_sync2);
        database_config_destroy(sync_config2);
        return 1;
    }
    printf("  Phase 3b OK: 'beta' still deleted in sync mode\n");
    
    // Write more data in sync-only mode after async use
    path_t* p3 = make_path("gamma");
    identifier_t* v3 = make_value("sync_after_async");
    if (database_put_sync(db_sync2, p3, v3) != 0) {
        printf("  FAIL (sync put after async use)\n"); database_destroy(db_sync2);
        database_config_destroy(sync_config2); return 1;
    }
    printf("  Phase 3c OK: wrote 'gamma'='sync_after_async' in sync mode after async use\n");
    
    database_flush_dirty_bnodes(db_sync2);
    database_destroy(db_sync2);
    database_config_destroy(sync_config2);
    system("rm -rf /tmp/test_sync_only_xmode");
    printf("  PASS\n");
    return 0;
}

int main(void) {
    int failures = 0;
    failures += test_sync_only_put_get();
    failures += test_sync_only_cross_mode();
    
    printf("\n%d test(s) failed\n", failures);
    return failures;
}
```

- [ ] **Step 2: Add test target to CMakeLists.txt**

In `tests/CMakeLists.txt`, add:

```cmake
add_executable(test_sync_only test_sync_only.c)
target_link_libraries(test_sync_only PRIVATE wavedb)
target_include_directories(test_sync_only PRIVATE ${CMAKE_SOURCE_DIR}/src)
```

- [ ] **Step 3: Run tests**

```bash
cd build && cmake .. && make test_sync_only -j$(nproc) && ./test_sync_only
```

Expected: `Test: sync_only put/get ... PASS`, `Test: sync_only → async → sync_only cross-mode ... PASS` with phase-by-phase output, `0 test(s) failed`

- [ ] **Step 4: Run existing tests to check for regressions**

```bash
cd build && cmake .. && make -j$(nproc) && ctest --output-on-failure
```

Expected: All existing tests pass.

- [ ] **Step 5: Commit**

```bash
git add tests/test_sync_only.c tests/CMakeLists.txt
git commit -m "test: add sync-only mode functional tests (put/get/delete/reopen)"
```
