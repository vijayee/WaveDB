# Database Config System - Phase 2: CBOR Serialization

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superagents:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement CBOR serialization/deserialization for config persistence.

**Architecture:** Use existing libcbor dependency to save/load config to `<location>/config.cbor`.

**Tech Stack:** C, libcbor, stdio

**Spec Reference:** `docs/superpowers/specs/2026-04-04-database-config-design.md` lines 212-246

**Prerequisite:** Phase 1 completed

---

## File Structure

**Modified Files:**
- `src/Database/database_config.h` - Add load/save/merge declarations
- `src/Database/database_config.c` - Add implementation
- `tests/test_database_config.cpp` - Add serialization tests

---

## Task 1: Add API Declarations

**Files:**
- Modify: `src/Database/database_config.h`

- [ ] **Step 1: Add load/save/merge function declarations**

In `src/Database/database_config.h`, add after `database_config_destroy`:

```c
/**
 * Load configuration from database directory.
 * 
 * @param location  Database directory path
 * @return Config or NULL if not found or on error
 */
database_config_t* database_config_load(const char* location);

/**
 * Save configuration to database directory.
 * 
 * Saves to <location>/config.cbor
 * 
 * @param location  Database directory path
 * @param config    Configuration to save
 * @return 0 on success, -1 on failure
 */
int database_config_save(const char* location, const database_config_t* config);

/**
 * Merge two configurations.
 * 
 * Merge rules:
 * - If saved is NULL: return passed (or defaults if both NULL)
 * - If passed is NULL: return copy of saved
 * - Immutable settings: use saved values (ignore passed)
 * - Mutable settings: use passed values (override saved)
 * 
 * @param saved  Config loaded from disk (may be NULL)
 * @param passed Config passed by user (may be NULL)
 * @return Merged config (caller must destroy)
 */
database_config_t* database_config_merge(const database_config_t* saved,
                                         const database_config_t* passed);
```

- [ ] **Step 2: Commit declaration**

```bash
git add src/Database/database_config.h
git commit -m "feat(config): add load/save/merge API declarations"
```

---

## Task 2: Implement Config Save

**Files:**
- Modify: `src/Database/database_config.c`

- [ ] **Step 1: Add CBOR include**

At the top of `src/Database/database_config.c`, add:

```c
#include <cbor.h>
#include <stdio.h>
#include <sys/stat.h>
#include <errno.h>
```

- [ ] **Step 2: Implement database_config_save**

Add to `src/Database/database_config.c`:

```c
int database_config_save(const char* location, const database_config_t* config) {
    if (location == NULL || config == NULL) {
        return -1;
    }
    
    // Build path: <location>/config.cbor
    size_t path_len = strlen(location) + strlen("/config.cbor") + 1;
    char* config_path = malloc(path_len);
    if (config_path == NULL) {
        return -1;
    }
    snprintf(config_path, path_len, "%s/config.cbor", location);
    
    // Create CBOR map
    cbor_item_t* root = cbor_new_definite_map(10);
    if (root == NULL) {
        free(config_path);
        return -1;
    }
    
    // Add version
    cbor_map_add(root, (struct cbor_pair) {
        .key = cbor_move(cbor_build_string("version")),
        .value = cbor_move(cbor_build_uint8(1))
    });
    
    // Add immutable settings
    cbor_map_add(root, (struct cbor_pair) {
        .key = cbor_move(cbor_build_string("chunk_size")),
        .value = cbor_move(cbor_build_uint8(config->chunk_size))
    });
    
    cbor_map_add(root, (struct cbor_pair) {
        .key = cbor_move(cbor_build_string("btree_node_size")),
        .value = cbor_move(cbor_build_uint32(config->btree_node_size))
    });
    
    cbor_map_add(root, (struct cbor_pair) {
        .key = cbor_move(cbor_build_string("enable_persist")),
        .value = cbor_move(cbor_build_uint8(config->enable_persist))
    });
    
    // Add mutable settings
    cbor_map_add(root, (struct cbor_pair) {
        .key = cbor_move(cbor_build_string("lru_memory_mb")),
        .value = cbor_move(cbor_build_uint64(config->lru_memory_mb))
    });
    
    cbor_map_add(root, (struct cbor_pair) {
        .key = cbor_move(cbor_build_string("storage_cache_size")),
        .value = cbor_move(cbor_build_uint64(config->storage_cache_size))
    });
    
    // Add WAL config as nested map
    cbor_item_t* wal = cbor_new_definite_map(5);
    cbor_map_add(wal, (struct cbor_pair) {
        .key = cbor_move(cbor_build_string("sync_mode")),
        .value = cbor_move(cbor_build_uint8(config->wal_config.sync_mode))
    });
    cbor_map_add(wal, (struct cbor_pair) {
        .key = cbor_move(cbor_build_string("debounce_ms")),
        .value = cbor_move(cbor_build_uint64(config->wal_config.debounce_ms))
    });
    cbor_map_add(wal, (struct cbor_pair) {
        .key = cbor_move(cbor_build_string("idle_threshold_ms")),
        .value = cbor_move(cbor_build_uint64(config->wal_config.idle_threshold_ms))
    });
    cbor_map_add(wal, (struct cbor_pair) {
        .key = cbor_move(cbor_build_string("compact_interval_ms")),
        .value = cbor_move(cbor_build_uint64(config->wal_config.compact_interval_ms))
    });
    cbor_map_add(wal, (struct cbor_pair) {
        .key = cbor_move(cbor_build_string("max_file_size")),
        .value = cbor_move(cbor_build_uint64(config->wal_config.max_file_size))
    });
    
    cbor_map_add(root, (struct cbor_pair) {
        .key = cbor_move(cbor_build_string("wal")),
        .value = cbor_move(wal)
    });
    
    // Add threading settings
    cbor_map_add(root, (struct cbor_pair) {
        .key = cbor_move(cbor_build_string("worker_threads")),
        .value = cbor_move(cbor_build_uint8(config->worker_threads))
    });
    
    cbor_map_add(root, (struct cbor_pair) {
        .key = cbor_move(cbor_build_string("timer_resolution_ms")),
        .value = cbor_move(cbor_build_uint16(config->timer_resolution_ms))
    });
    
    // Serialize to bytes
    unsigned char* buffer = NULL;
    size_t buffer_size = 0;
    size_t bytes_written = cbor_serialize_alloc(root, &buffer, &buffer_size);
    cbor_decref(&root);
    
    if (bytes_written == 0 || buffer == NULL) {
        free(config_path);
        return -1;
    }
    
    // Write to file
    FILE* fp = fopen(config_path, "wb");
    if (fp == NULL) {
        free(buffer);
        free(config_path);
        return -1;
    }
    
    size_t written = fwrite(buffer, 1, bytes_written, fp);
    fclose(fp);
    free(buffer);
    free(config_path);
    
    return (written == bytes_written) ? 0 : -1;
}
```

- [ ] **Step 3: Build and verify**

```bash
cd /home/victor/Workspace/src/github.com/vijayee/WaveDB/build
cmake --build . -j$(nproc)
```

- [ ] **Step 4: Commit**

```bash
git add src/Database/database_config.c
git commit -m "feat(config): implement database_config_save with CBOR"
```

---

## Task 3: Implement Config Load

**Files:**
- Modify: `src/Database/database_config.c`

- [ ] **Step 1: Implement database_config_load**

Add to `src/Database/database_config.c`:

```c
// Helper to get uint from CBOR map
static uint64_t get_map_uint(cbor_item_t* map, const char* key, uint64_t default_val) {
    cbor_item_t* key_item = cbor_build_string(key);
    cbor_item_t* value = NULL;
    
    // Find key in map
    for (size_t i = 0; i < cbor_map_size(map); i++) {
        struct cbor_pair pair = cbor_map_handle(map)[i];
        if (cbor_isa_string(pair.key) &&
            cbor_string_length(pair.key) == strlen(key) &&
            memcmp(cbor_string_handle(pair.key), key, strlen(key)) == 0) {
            value = pair.value;
            break;
        }
    }
    cbor_decref(&key_item);
    
    if (value == NULL) {
        return default_val;
    }
    
    if (cbor_isa_uint(value)) {
        return cbor_get_uint64(value);
    }
    return default_val;
}

database_config_t* database_config_load(const char* location) {
    if (location == NULL) {
        return NULL;
    }
    
    // Build path: <location>/config.cbor
    size_t path_len = strlen(location) + strlen("/config.cbor") + 1;
    char* config_path = malloc(path_len);
    if (config_path == NULL) {
        return NULL;
    }
    snprintf(config_path, path_len, "%s/config.cbor", location);
    
    // Check if file exists
    struct stat st;
    if (stat(config_path, &st) != 0) {
        free(config_path);
        return NULL;  // File doesn't exist
    }
    
    // Read file
    FILE* fp = fopen(config_path, "rb");
    free(config_path);
    if (fp == NULL) {
        return NULL;
    }
    
    fseek(fp, 0, SEEK_END);
    long file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    
    if (file_size <= 0 || file_size > 1024 * 1024) {  // Max 1MB
        fclose(fp);
        return NULL;
    }
    
    unsigned char* buffer = malloc(file_size);
    if (buffer == NULL) {
        fclose(fp);
        return NULL;
    }
    
    size_t bytes_read = fread(buffer, 1, file_size, fp);
    fclose(fp);
    
    if (bytes_read != (size_t)file_size) {
        free(buffer);
        return NULL;
    }
    
    // Parse CBOR
    struct cbor_load_result result;
    cbor_item_t* root = cbor_load(buffer, file_size, &result);
    free(buffer);
    
    if (root == NULL || result.error.code != CBOR_ERR_NONE) {
        if (root) cbor_decref(&root);
        return NULL;
    }
    
    if (!cbor_isa_map(root)) {
        cbor_decref(&root);
        return NULL;
    }
    
    // Create config from loaded values
    database_config_t* config = database_config_default();
    if (config == NULL) {
        cbor_decref(&root);
        return NULL;
    }
    
    // Read immutable settings
    config->chunk_size = (uint8_t)get_map_uint(root, "chunk_size", DATABASE_CONFIG_DEFAULT_CHUNK_SIZE);
    config->btree_node_size = (uint32_t)get_map_uint(root, "btree_node_size", DATABASE_CONFIG_DEFAULT_BTREE_NODE_SIZE);
    config->enable_persist = (uint8_t)get_map_uint(root, "enable_persist", 1);
    
    // Read mutable settings
    config->lru_memory_mb = get_map_uint(root, "lru_memory_mb", DATABASE_CONFIG_DEFAULT_LRU_MEMORY_MB);
    config->storage_cache_size = get_map_uint(root, "storage_cache_size", DATABASE_CONFIG_DEFAULT_STORAGE_CACHE_SIZE);
    
    // Read threading settings
    config->worker_threads = (uint8_t)get_map_uint(root, "worker_threads", DATABASE_CONFIG_DEFAULT_WORKER_THREADS);
    config->timer_resolution_ms = (uint16_t)get_map_uint(root, "timer_resolution_ms", DATABASE_CONFIG_DEFAULT_TIMER_RESOLUTION_MS);
    
    // Read WAL config from nested map
    cbor_item_t* key_item = cbor_build_string("wal");
    cbor_item_t* wal_map = NULL;
    for (size_t i = 0; i < cbor_map_size(root); i++) {
        struct cbor_pair pair = cbor_map_handle(root)[i];
        if (cbor_isa_string(pair.key) &&
            cbor_string_length(pair.key) == 3 &&
            memcmp(cbor_string_handle(pair.key), "wal", 3) == 0) {
            wal_map = pair.value;
            break;
        }
    }
    cbor_decref(&key_item);
    
    if (wal_map != NULL && cbor_isa_map(wal_map)) {
        config->wal_config.sync_mode = (wal_sync_mode_e)get_map_uint(wal_map, "sync_mode", WAL_SYNC_IMMEDIATE);
        config->wal_config.debounce_ms = get_map_uint(wal_map, "debounce_ms", WAL_DEFAULT_DEBOUNCE_MS);
        config->wal_config.idle_threshold_ms = get_map_uint(wal_map, "idle_threshold_ms", WAL_DEFAULT_IDLE_THRESHOLD_MS);
        config->wal_config.compact_interval_ms = get_map_uint(wal_map, "compact_interval_ms", WAL_DEFAULT_COMPACT_INTERVAL_MS);
        config->wal_config.max_file_size = (size_t)get_map_uint(wal_map, "max_file_size", WAL_DEFAULT_MAX_FILE_SIZE);
    }
    
    cbor_decref(&root);
    return config;
}
```

- [ ] **Step 2: Build and verify**

```bash
cd /home/victor/Workspace/src/github.com/vijayee/WaveDB/build
cmake --build . -j$(nproc)
```

- [ ] **Step 3: Commit**

```bash
git add src/Database/database_config.c
git commit -m "feat(config): implement database_config_load from CBOR"
```

---

## Task 4: Implement Config Merge

**Files:**
- Modify: `src/Database/database_config.c`

- [ ] **Step 1: Implement database_config_merge**

Add to `src/Database/database_config.c`:

```c
database_config_t* database_config_merge(const database_config_t* saved,
                                         const database_config_t* passed) {
    // Both NULL: return defaults
    if (saved == NULL && passed == NULL) {
        return database_config_default();
    }
    
    // Saved NULL: return copy of passed
    if (saved == NULL) {
        return database_config_copy(passed);
    }
    
    // Passed NULL: return copy of saved
    if (passed == NULL) {
        return database_config_copy(saved);
    }
    
    // Both present: merge according to rules
    database_config_t* merged = database_config_default();
    if (merged == NULL) {
        return NULL;
    }
    
    // IMMUTABLE: Always use saved values (database was created with these)
    merged->chunk_size = saved->chunk_size;
    merged->btree_node_size = saved->btree_node_size;
    merged->enable_persist = saved->enable_persist;
    
    // MUTABLE: Use passed values (user can change these)
    merged->lru_memory_mb = passed->lru_memory_mb;
    merged->storage_cache_size = passed->storage_cache_size;
    merged->wal_config = passed->wal_config;
    
    // THREADING: Use passed values
    merged->worker_threads = passed->worker_threads;
    merged->timer_resolution_ms = passed->timer_resolution_ms;
    
    // EXTERNAL: Use passed values (runtime only)
    merged->external_pool = passed->external_pool;
    merged->external_wheel = passed->external_wheel;
    
    return merged;
}
```

- [ ] **Step 2: Build and verify**

```bash
cd /home/victor/Workspace/src/github.com/vijayee/WaveDB/build
cmake --build . -j$(nproc)
```

- [ ] **Step 3: Commit**

```bash
git add src/Database/database_config.c
git commit -m "feat(config): implement database_config_merge"
```

---

## Task 5: Add Serialization Tests

**Files:**
- Modify: `tests/test_database_config.cpp`

- [ ] **Step 1: Add save/load tests**

Add to `tests/test_database_config.cpp`:

```cpp
#include <cstdio>
#include <unistd.h>
#include <sys/stat.h>

// Test: Save and load preserves values
TEST(DatabaseConfig, SaveAndLoadPreservesValues) {
    // Create temp directory
    char temp_dir[] = "/tmp/wavedb_config_test_XXXXXX";
    mkdtemp(temp_dir);
    
    // Create config with custom values
    database_config_t* original = database_config_default();
    original->lru_memory_mb = 100;
    original->worker_threads = 8;
    original->wal_config.sync_mode = WAL_SYNC_DEBOUNCED;
    original->wal_config.debounce_ms = 200;
    
    // Save
    int result = database_config_save(temp_dir, original);
    ASSERT_EQ(result, 0);
    
    // Load
    database_config_t* loaded = database_config_load(temp_dir);
    ASSERT_NE(loaded, nullptr);
    
    // Verify
    EXPECT_EQ(loaded->lru_memory_mb, 100u);
    EXPECT_EQ(loaded->worker_threads, 8u);
    EXPECT_EQ(loaded->wal_config.sync_mode, WAL_SYNC_DEBOUNCED);
    EXPECT_EQ(loaded->wal_config.debounce_ms, 200u);
    
    // Verify immutable settings preserved
    EXPECT_EQ(loaded->chunk_size, original->chunk_size);
    EXPECT_EQ(loaded->btree_node_size, original->btree_node_size);
    EXPECT_EQ(loaded->enable_persist, original->enable_persist);
    
    database_config_destroy(original);
    database_config_destroy(loaded);
    
    // Cleanup
    char config_path[256];
    snprintf(config_path, sizeof(config_path), "%s/config.cbor", temp_dir);
    unlink(config_path);
    rmdir(temp_dir);
}

// Test: Load returns NULL for non-existent config
TEST(DatabaseConfig, LoadNonExistentReturnsNull) {
    database_config_t* config = database_config_load("/nonexistent/path/12345");
    EXPECT_EQ(config, nullptr);
}

// Test: Merge uses saved for immutable, passed for mutable
TEST(DatabaseConfig, MergeRules) {
    database_config_t* saved = database_config_default();
    saved->chunk_size = 8;  // Different immutable
    saved->lru_memory_mb = 50;  // Original mutable
    
    database_config_t* passed = database_config_default();
    passed->chunk_size = 4;  // Different immutable
    passed->lru_memory_mb = 100;  // New mutable
    
    database_config_t* merged = database_config_merge(saved, passed);
    ASSERT_NE(merged, nullptr);
    
    // Immutable: use saved
    EXPECT_EQ(merged->chunk_size, 8u);
    
    // Mutable: use passed
    EXPECT_EQ(merged->lru_memory_mb, 100u);
    
    database_config_destroy(saved);
    database_config_destroy(passed);
    database_config_destroy(merged);
}

// Test: Merge handles NULL saved
TEST(DatabaseConfig, MergeNullSaved) {
    database_config_t* passed = database_config_default();
    passed->lru_memory_mb = 100;
    
    database_config_t* merged = database_config_merge(nullptr, passed);
    ASSERT_NE(merged, nullptr);
    EXPECT_EQ(merged->lru_memory_mb, 100u);
    
    database_config_destroy(passed);
    database_config_destroy(merged);
}

// Test: Merge handles NULL passed
TEST(DatabaseConfig, MergeNullPassed) {
    database_config_t* saved = database_config_default();
    saved->lru_memory_mb = 100;
    
    database_config_t* merged = database_config_merge(saved, nullptr);
    ASSERT_NE(merged, nullptr);
    EXPECT_EQ(merged->lru_memory_mb, 100u);
    
    database_config_destroy(saved);
    database_config_destroy(merged);
}

// Test: Merge handles both NULL
TEST(DatabaseConfig, MergeBothNull) {
    database_config_t* merged = database_config_merge(nullptr, nullptr);
    ASSERT_NE(merged, nullptr);
    
    // Should return defaults
    EXPECT_EQ(merged->chunk_size, DATABASE_CONFIG_DEFAULT_CHUNK_SIZE);
    EXPECT_EQ(merged->lru_memory_mb, DATABASE_CONFIG_DEFAULT_LRU_MEMORY_MB);
    
    database_config_destroy(merged);
}
```

- [ ] **Step 2: Build and run tests**

```bash
cd /home/victor/Workspace/src/github.com/vijayee/WaveDB/build
cmake --build . -j$(nproc)
ctest -R test_database_config --output-on-failure
```

Expected: All tests pass.

- [ ] **Step 3: Commit tests**

```bash
git add tests/test_database_config.cpp
git commit -m "test(config): add serialization tests for save/load/merge"
```

---

## Summary

Phase 2 adds CBOR persistence:
- `database_config_save()` - Serialize config to CBOR file
- `database_config_load()` - Deserialize config from CBOR file
- `database_config_merge()` - Combine saved and passed configs with proper rules

**Next Phase:** Database integration (`database_create_with_config`, pool/wheel ownership)