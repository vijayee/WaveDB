# Database Config System - Phase 3: Database Integration

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superagents:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Integrate config with database creation, implement pool/wheel ownership.

**Architecture:** Modify `database_create` to use config, add `database_create_with_config`, handle pool/wheel creation/ownership.

**Tech Stack:** C, existing work_pool and timing_wheel

**Spec Reference:** `docs/superpowers/specs/2026-04-04-database-config-design.md` lines 288-330

**Prerequisites:** Phase 1 and 2 completed

---

## File Structure

**Modified Files:**
- `src/Database/database.h` - Add new API
- `src/Database/database.c` - Implement integration
- `tests/test_database_config.cpp` - Integration tests

---

## Task 1: Add database_create_with_config Declaration

**Files:**
- Modify: `src/Database/database.h`

- [ ] **Step 1: Add new function declaration**

In `src/Database/database.h`, add after the existing `database_create` declaration:

```c
/**
 * Create a database with configuration.
 * 
 * @param location    Directory path for database files
 * @param config      Configuration (NULL for defaults)
 * @param error_code  Output error code (0 on success)
 * @return Database or NULL on failure
 * 
 * Behavior:
 * - If database doesn't exist: use config (or defaults)
 * - If database exists: load saved config, merge with passed config
 * - Immutable settings from passed config are ignored for existing DB
 * - If config->worker_threads > 0 and external_pool/wheel are NULL,
 *   creates its own pool and wheel
 */
database_t* database_create_with_config(const char* location,
                                        database_config_t* config,
                                        int* error_code);
```

- [ ] **Step 2: Commit**

```bash
git add src/Database/database.h
git commit -m "feat(database): add database_create_with_config declaration"
```

---

## Task 2: Implement database_create_with_config

**Files:**
- Modify: `src/Database/database.c`

- [ ] **Step 1: Add includes at top of database.c**

Add after existing includes:

```c
#include "database_config.h"
```

- [ ] **Step 2: Implement database_create_with_config**

Add before the existing `database_create` function:

```c
database_t* database_create_with_config(const char* location,
                                        database_config_t* config,
                                        int* error_code) {
    if (error_code) *error_code = 0;
    
    // Use defaults if no config provided
    database_config_t* effective_config = NULL;
    bool owns_config = false;
    
    if (config == NULL) {
        effective_config = database_config_default();
        owns_config = true;
    } else {
        effective_config = config;
    }
    
    // Check if database already exists
    char config_path[1024];
    snprintf(config_path, sizeof(config_path), "%s/config.cbor", location);
    struct stat st;
    bool db_exists = (stat(config_path, &st) == 0);
    
    if (db_exists) {
        // Load saved config and merge
        database_config_t* saved_config = database_config_load(location);
        if (saved_config != NULL) {
            database_config_t* merged = database_config_merge(saved_config, effective_config);
            if (owns_config) {
                database_config_destroy(effective_config);
            }
            database_config_destroy(saved_config);
            effective_config = merged;
            owns_config = true;
        }
    }
    
    // Create directory if needed
    if (mkdir_p((char*)location) != 0) {
        if (error_code) *error_code = errno;
        if (owns_config) database_config_destroy(effective_config);
        return NULL;
    }
    
    // Initialize transaction ID generator (call once per process)
    transaction_id_init();
    
    database_t* db = get_clear_memory(sizeof(database_t));
    if (db == NULL) {
        if (error_code) *error_code = ENOMEM;
        if (owns_config) database_config_destroy(effective_config);
        return NULL;
    }
    
    db->location = strdup(location);
    db->lru_size = effective_config->lru_memory_mb;
    db->chunk_size = effective_config->chunk_size;
    db->btree_node_size = effective_config->btree_node_size;
    db->is_rebuilding = 0;
    
    // Handle pool ownership
    if (effective_config->external_pool != NULL) {
        db->pool = effective_config->external_pool;
        db->owns_pool = false;
    } else if (effective_config->worker_threads > 0) {
        db->pool = work_pool_create(effective_config->worker_threads);
        db->owns_pool = (db->pool != NULL);
        if (db->pool == NULL) {
            if (error_code) *error_code = ENOMEM;
            free(db->location);
            free(db);
            if (owns_config) database_config_destroy(effective_config);
            return NULL;
        }
    } else {
        // No pool available
        if (error_code) *error_code = EINVAL;
        free(db->location);
        free(db);
        if (owns_config) database_config_destroy(effective_config);
        return NULL;
    }
    
    // Handle wheel ownership
    if (effective_config->external_wheel != NULL) {
        db->wheel = effective_config->external_wheel;
        db->owns_wheel = false;
    } else if (effective_config->timer_resolution_ms > 0) {
        db->wheel = hierarchical_timing_wheel_create(effective_config->timer_resolution_ms);
        db->owns_wheel = (db->wheel != NULL);
        if (db->wheel == NULL) {
            if (db->owns_pool) work_pool_destroy(db->pool);
            free(db->location);
            free(db);
            if (owns_config) database_config_destroy(effective_config);
            if (error_code) *error_code = ENOMEM;
            return NULL;
        }
    } else {
        // No wheel available
        if (error_code) *error_code = EINVAL;
        if (db->owns_pool) work_pool_destroy(db->pool);
        free(db->location);
        free(db);
        if (owns_config) database_config_destroy(effective_config);
        return NULL;
    }
    
    // Create LRU cache
    db->lru = database_lru_cache_create(db->lru_size * 1024 * 1024);
    if (db->lru == NULL) {
        if (db->owns_pool) work_pool_destroy(db->pool);
        if (db->owns_wheel) hierarchical_timing_wheel_destroy(db->wheel);
        free(db->location);
        free(db);
        if (owns_config) database_config_destroy(effective_config);
        if (error_code) *error_code = ENOMEM;
        return NULL;
    }
    
    // Create HBTrie
    db->trie = hbtrie_create(effective_config->chunk_size, effective_config->btree_node_size);
    if (db->trie == NULL) {
        database_lru_cache_destroy(db->lru);
        if (db->owns_pool) work_pool_destroy(db->pool);
        if (db->owns_wheel) hierarchical_timing_wheel_destroy(db->wheel);
        free(db->location);
        free(db);
        if (owns_config) database_config_destroy(effective_config);
        if (error_code) *error_code = ENOMEM;
        return NULL;
    }
    
    // Create transaction manager
    db->tx_manager = tx_manager_create(db->trie, db->pool, db->wheel, 100);
    if (db->tx_manager == NULL) {
        hbtrie_destroy(db->trie);
        database_lru_cache_destroy(db->lru);
        if (db->owns_pool) work_pool_destroy(db->pool);
        if (db->owns_wheel) hierarchical_timing_wheel_destroy(db->wheel);
        free(db->location);
        free(db);
        if (owns_config) database_config_destroy(effective_config);
        if (error_code) *error_code = ENOMEM;
        return NULL;
    }
    
    // Create WAL manager
    db->wal_manager = wal_manager_create(db->location, &effective_config->wal_config, db->pool, db->wheel);
    if (db->wal_manager == NULL) {
        tx_manager_destroy(db->tx_manager);
        hbtrie_destroy(db->trie);
        database_lru_cache_destroy(db->lru);
        if (db->owns_pool) work_pool_destroy(db->pool);
        if (db->owns_wheel) hierarchical_timing_wheel_destroy(db->wheel);
        free(db->location);
        free(db);
        if (owns_config) database_config_destroy(effective_config);
        if (error_code) *error_code = ENOMEM;
        return NULL;
    }
    
    // Create storage if persistent
    if (effective_config->enable_persist) {
        db->storage_cache_size = effective_config->storage_cache_size;
        db->storage = sections_create(location, db->storage_cache_size);
        // Storage is optional - can be NULL for in-memory
    }
    
    // Store active config
    db->active_config = database_config_copy(effective_config);
    
    // Save config
    database_config_save(location, db->active_config);
    
    // Initialize write locks
    for (int i = 0; i < WRITE_LOCK_SHARDS; i++) {
        platform_lock_init(&db->write_locks[i]);
    }
    
    refcounter_init((refcounter_t*)db);
    
    if (owns_config) {
        database_config_destroy(effective_config);
    }
    
    return db;
}
```

- [ ] **Step 3: Build and verify**

```bash
cd /home/victor/Workspace/src/github.com/vijayee/WaveDB/build
cmake --build . -j$(nproc)
```

- [ ] **Step 4: Commit**

```bash
git add src/Database/database.c
git commit -m "feat(database): implement database_create_with_config"
```

---

## Task 3: Update database_destroy for Ownership

**Files:**
- Modify: `src/Database/database.c`

- [ ] **Step 1: Update database_destroy**

Find the `database_destroy` function and add pool/wheel cleanup before the final `free(db)`:

```c
    // Destroy active config
    if (db->active_config != NULL) {
        database_config_destroy(db->active_config);
    }
    
    // Destroy owned pool/wheel
    if (db->owns_pool && db->pool != NULL) {
        work_pool_destroy(db->pool);
    }
    if (db->owns_wheel && db->wheel != NULL) {
        hierarchical_timing_wheel_destroy(db->wheel);
    }
```

- [ ] **Step 2: Build and verify**

```bash
cd /home/victor/Workspace/src/github.com/vijayee/WaveDB/build
cmake --build . -j$(nproc)
```

- [ ] **Step 3: Commit**

```bash
git add src/Database/database.c
git commit -m "feat(database): handle pool/wheel ownership in destroy"
```

---

## Task 4: Update database_create for Backward Compatibility

**Files:**
- Modify: `src/Database/database.c`

- [ ] **Step 1: Rewrite database_create to use config**

Replace the existing `database_create` function with:

```c
database_t* database_create(const char* location, size_t lru_memory_mb,
                            wal_config_t* wal_config,
                            uint8_t chunk_size, uint32_t btree_node_size,
                            uint8_t enable_persist, size_t storage_cache_size,
                            work_pool_t* pool, hierarchical_timing_wheel_t* wheel,
                            int* error_code) {
    // Create config from parameters
    database_config_t* config = database_config_default();
    if (config == NULL) {
        if (error_code) *error_code = ENOMEM;
        return NULL;
    }
    
    // Set values from parameters
    if (lru_memory_mb > 0) config->lru_memory_mb = lru_memory_mb;
    if (chunk_size > 0) config->chunk_size = chunk_size;
    if (btree_node_size > 0) config->btree_node_size = btree_node_size;
    config->enable_persist = enable_persist;
    if (storage_cache_size > 0) config->storage_cache_size = storage_cache_size;
    
    // Set WAL config if provided
    if (wal_config != NULL) {
        config->wal_config = *wal_config;
    }
    
    // Set external resources
    config->external_pool = pool;
    config->external_wheel = wheel;
    if (pool != NULL) config->worker_threads = 0;  // Using external pool
    if (wheel != NULL) config->timer_resolution_ms = 0;  // Using external wheel
    
    database_t* db = database_create_with_config(location, config, error_code);
    database_config_destroy(config);
    
    return db;
}
```

- [ ] **Step 2: Build and run existing tests**

```bash
cd /home/victor/Workspace/src/github.com/vijayee/WaveDB/build
cmake --build . -j$(nproc)
ctest --output-on-failure
```

Expected: All existing tests pass.

- [ ] **Step 3: Commit**

```bash
git add src/Database/database.c
git commit -m "refactor(database): use config in legacy database_create"
```

---

## Task 5: Add Integration Tests

**Files:**
- Modify: `tests/test_database_config.cpp`

- [ ] **Step 1: Add integration tests**

Add to `tests/test_database_config.cpp`:

```cpp
// Test: Create database with config
TEST(DatabaseConfig, CreateWithConfig) {
    char temp_dir[] = "/tmp/wavedb_config_test_XXXXXX";
    mkdtemp(temp_dir);
    
    database_config_t* config = database_config_default();
    config->lru_memory_mb = 10;
    config->worker_threads = 2;
    
    int error = 0;
    database_t* db = database_create_with_config(temp_dir, config, &error);
    ASSERT_NE(db, nullptr);
    EXPECT_EQ(error, 0);
    
    // Verify owns_pool is true (created own)
    EXPECT_TRUE(db->owns_pool);
    EXPECT_TRUE(db->owns_wheel);
    
    database_destroy(db);
    database_config_destroy(config);
    
    // Cleanup
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", temp_dir);
    system(cmd);
}

// Test: Reopen preserves immutable settings
TEST(DatabaseConfig, ReopenPreservesImmutable) {
    char temp_dir[] = "/tmp/wavedb_config_test_XXXXXX";
    mkdtemp(temp_dir);
    
    // Create with chunk_size=8
    database_config_t* config1 = database_config_default();
    config1->chunk_size = 8;
    config1->worker_threads = 2;
    
    int error = 0;
    database_t* db1 = database_create_with_config(temp_dir, config1, &error);
    ASSERT_NE(db1, nullptr);
    database_destroy(db1);
    database_config_destroy(config1);
    
    // Reopen with chunk_size=4 (should be ignored)
    database_config_t* config2 = database_config_default();
    config2->chunk_size = 4;  // Different - should be ignored
    config2->lru_memory_mb = 100;  // Mutable - should change
    config2->worker_threads = 2;
    
    database_t* db2 = database_create_with_config(temp_dir, config2, &error);
    ASSERT_NE(db2, nullptr);
    
    // Verify immutable setting preserved
    EXPECT_EQ(db2->chunk_size, 8u);  // Original value preserved
    
    database_destroy(db2);
    database_config_destroy(config2);
    
    // Cleanup
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", temp_dir);
    system(cmd);
}

// Test: External pool/wheel not owned
TEST(DatabaseConfig, ExternalResourcesNotOwned) {
    char temp_dir[] = "/tmp/wavedb_config_test_XXXXXX";
    mkdtemp(temp_dir);
    
    // Create external resources
    work_pool_t* pool = work_pool_create(2);
    hierarchical_timing_wheel_t* wheel = hierarchical_timing_wheel_create(10);
    
    database_config_t* config = database_config_default();
    config->external_pool = pool;
    config->external_wheel = wheel;
    
    int error = 0;
    database_t* db = database_create_with_config(temp_dir, config, &error);
    ASSERT_NE(db, nullptr);
    
    // Verify not owned
    EXPECT_FALSE(db->owns_pool);
    EXPECT_FALSE(db->owns_wheel);
    
    database_destroy(db);
    database_config_destroy(config);
    
    // External resources still valid after database destroy
    // (Cleaned up below)
    work_pool_destroy(pool);
    hierarchical_timing_wheel_destroy(wheel);
    
    // Cleanup
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", temp_dir);
    system(cmd);
}
```

- [ ] **Step 2: Build and run tests**

```bash
cd /home/victor/Workspace/src/github.com/vijayee/WaveDB/build
cmake --build . -j$(nproc)
ctest -R test_database_config --output-on-failure
```

- [ ] **Step 3: Commit tests**

```bash
git add tests/test_database_config.cpp
git commit -m "test(config): add database integration tests"
```

---

## Summary

Phase 3 completes database integration:
- `database_create_with_config()` - New API using config
- Pool/wheel ownership tracking
- Proper cleanup in `database_destroy`
- Backward compatible `database_create`
- Config persistence on database creation

**Next Phase:** Dart/NodeJS bindings update (if needed)