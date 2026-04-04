# Database Config System - Phase 1: Core Structure and Basic API

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superagents:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Create the database_config_t structure with basic create/destroy/default functions.

**Architecture:** New header/source files for config, minimal integration with existing database.h to add new fields to database_t.

**Tech Stack:** C, existing allocator patterns

**Spec Reference:** `docs/superpowers/specs/2026-04-04-database-config-design.md`

---

## File Structure

**New Files:**
- `src/Database/database_config.h` - Config structure and API declarations
- `src/Database/database_config.c` - Implementation
- `tests/test_database_config.cpp` - Unit tests

**Modified Files:**
- `src/Database/database.h` - Add owns_pool, owns_wheel, active_config fields
- `src/Database/CMakeLists.txt` - Add new source file

---

## Task 1: Create Header File with Structure and Defaults

**Files:**
- Create: `src/Database/database_config.h`

- [ ] **Step 1: Create database_config.h with structure definition**

Create `src/Database/database_config.h`:

```c
//
// Database Configuration
// Created: 2026-04-04
//

#ifndef WAVEDB_DATABASE_CONFIG_H
#define WAVEDB_DATABASE_CONFIG_H

#include <stdint.h>
#include <stddef.h>
#include "wal_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

// Forward declarations
struct work_pool;
struct hierarchical_timing_wheel;
typedef struct work_pool work_pool_t;
typedef struct hierarchical_timing_wheel hierarchical_timing_wheel_t;

/**
 * Default values
 */
#define DATABASE_CONFIG_DEFAULT_CHUNK_SIZE 4
#define DATABASE_CONFIG_DEFAULT_BTREE_NODE_SIZE 4096
#define DATABASE_CONFIG_DEFAULT_LRU_MEMORY_MB 50
#define DATABASE_CONFIG_DEFAULT_STORAGE_CACHE_SIZE 1024
#define DATABASE_CONFIG_DEFAULT_WORKER_THREADS 4
#define DATABASE_CONFIG_DEFAULT_TIMER_RESOLUTION_MS 10

/**
 * Database configuration structure.
 * 
 * Immutable settings are fixed at database creation.
 * Mutable settings can be changed when reopening.
 * External resources are not persisted.
 */
typedef struct {
    // === IMMUTABLE SETTINGS ===
    uint8_t chunk_size;           // HBTrie chunk size (default: 4)
    uint32_t btree_node_size;     // B+tree node size (default: 4096)
    uint8_t enable_persist;       // 0 = in-memory, 1 = persistent
    
    // === MUTABLE SETTINGS ===
    size_t lru_memory_mb;         // LRU cache size in MB (default: 50)
    size_t storage_cache_size;    // Section cache size (default: 1024)
    wal_config_t wal_config;      // WAL settings
    
    // === THREADING SETTINGS ===
    uint8_t worker_threads;       // Number of workers (default: 4)
    uint16_t timer_resolution_ms; // Timer resolution (default: 10)
    
    // === EXTERNAL RESOURCES (not saved) ===
    work_pool_t* external_pool;
    hierarchical_timing_wheel_t* external_wheel;
} database_config_t;

/**
 * Create default configuration.
 * 
 * Returns a config with all defaults set. Caller must destroy.
 * 
 * @return New config or NULL on failure
 */
database_config_t* database_config_default(void);

/**
 * Create a copy of a configuration.
 * 
 * @param config  Config to copy
 * @return New config or NULL on failure
 */
database_config_t* database_config_copy(const database_config_t* config);

/**
 * Destroy a configuration.
 * 
 * @param config  Config to destroy
 */
void database_config_destroy(database_config_t* config);

#ifdef __cplusplus
}
#endif

#endif // WAVEDB_DATABASE_CONFIG_H
```

- [ ] **Step 2: Commit header file**

```bash
git add src/Database/database_config.h
git commit -m "feat(config): add database_config_t structure and basic API"
```

---

## Task 2: Implement Basic Functions

**Files:**
- Create: `src/Database/database_config.c`

- [ ] **Step 1: Create database_config.c with implementation**

Create `src/Database/database_config.c`:

```c
//
// Database Configuration Implementation
// Created: 2026-04-04
//

#include "database_config.h"
#include "../Util/allocator.h"
#include <string.h>

database_config_t* database_config_default(void) {
    database_config_t* config = get_clear_memory(sizeof(database_config_t));
    if (config == NULL) {
        return NULL;
    }
    
    // Immutable settings
    config->chunk_size = DATABASE_CONFIG_DEFAULT_CHUNK_SIZE;
    config->btree_node_size = DATABASE_CONFIG_DEFAULT_BTREE_NODE_SIZE;
    config->enable_persist = 1;  // Default to persistent
    
    // Mutable settings
    config->lru_memory_mb = DATABASE_CONFIG_DEFAULT_LRU_MEMORY_MB;
    config->storage_cache_size = DATABASE_CONFIG_DEFAULT_STORAGE_CACHE_SIZE;
    
    // WAL config defaults (use existing defaults from wal_manager.h)
    config->wal_config.sync_mode = WAL_SYNC_IMMEDIATE;
    config->wal_config.debounce_ms = WAL_DEFAULT_DEBOUNCE_MS;
    config->wal_config.idle_threshold_ms = WAL_DEFAULT_IDLE_THRESHOLD_MS;
    config->wal_config.compact_interval_ms = WAL_DEFAULT_COMPACT_INTERVAL_MS;
    config->wal_config.max_file_size = WAL_DEFAULT_MAX_FILE_SIZE;
    
    // Threading settings
    config->worker_threads = DATABASE_CONFIG_DEFAULT_WORKER_THREADS;
    config->timer_resolution_ms = DATABASE_CONFIG_DEFAULT_TIMER_RESOLUTION_MS;
    
    // External resources (NULL = create own)
    config->external_pool = NULL;
    config->external_wheel = NULL;
    
    return config;
}

database_config_t* database_config_copy(const database_config_t* config) {
    if (config == NULL) {
        return NULL;
    }
    
    database_config_t* copy = get_clear_memory(sizeof(database_config_t));
    if (copy == NULL) {
        return NULL;
    }
    
    memcpy(copy, config, sizeof(database_config_t));
    // External resources are not owned, so just copy pointers
    copy->external_pool = config->external_pool;
    copy->external_wheel = config->external_wheel;
    
    return copy;
}

void database_config_destroy(database_config_t* config) {
    if (config == NULL) {
        return;
    }
    
    // No internal allocations to free - just the struct itself
    free(config);
}
```

- [ ] **Step 2: Commit implementation**

```bash
git add src/Database/database_config.c
git commit -m "feat(config): implement database_config_default/copy/destroy"
```

---

## Task 3: Add to CMake Build

**Files:**
- Modify: `src/Database/CMakeLists.txt`

- [ ] **Step 1: Add database_config.c to CMakeLists.txt**

Find the source file list in `src/Database/CMakeLists.txt` and add `database_config.c`.

- [ ] **Step 2: Build and verify**

```bash
cd /home/victor/Workspace/src/github.com/vijayee/WaveDB/build
cmake --build . -j$(nproc)
```

Expected: Build succeeds.

- [ ] **Step 3: Commit**

```bash
git add src/Database/CMakeLists.txt
git commit -m "build(config): add database_config to build"
```

---

## Task 4: Write Unit Tests

**Files:**
- Create: `tests/test_database_config.cpp`

- [ ] **Step 1: Create test file**

Create `tests/test_database_config.cpp`:

```cpp
#include <gtest/gtest.h>
extern "C" {
#include "Database/database_config.h"
}

// Test: database_config_default creates valid config
TEST(DatabaseConfig, DefaultCreatesValidConfig) {
    database_config_t* config = database_config_default();
    ASSERT_NE(config, nullptr);
    
    // Check defaults
    EXPECT_EQ(config->chunk_size, 4);
    EXPECT_EQ(config->btree_node_size, 4096u);
    EXPECT_EQ(config->enable_persist, 1);
    EXPECT_EQ(config->lru_memory_mb, 50u);
    EXPECT_EQ(config->storage_cache_size, 1024u);
    EXPECT_EQ(config->worker_threads, 4u);
    EXPECT_EQ(config->timer_resolution_ms, 10u);
    EXPECT_EQ(config->external_pool, nullptr);
    EXPECT_EQ(config->external_wheel, nullptr);
    
    database_config_destroy(config);
}

// Test: database_config_copy creates exact copy
TEST(DatabaseConfig, CopyCreatesExactCopy) {
    database_config_t* config = database_config_default();
    ASSERT_NE(config, nullptr);
    
    // Modify some values
    config->lru_memory_mb = 100;
    config->worker_threads = 8;
    
    database_config_t* copy = database_config_copy(config);
    ASSERT_NE(copy, nullptr);
    
    EXPECT_EQ(copy->lru_memory_mb, 100u);
    EXPECT_EQ(copy->worker_threads, 8u);
    EXPECT_EQ(copy->chunk_size, config->chunk_size);
    
    database_config_destroy(config);
    database_config_destroy(copy);
}

// Test: database_config_destroy handles NULL
TEST(DatabaseConfig, DestroyHandlesNull) {
    // Should not crash
    database_config_destroy(nullptr);
}

// Test: database_config_copy handles NULL
TEST(DatabaseConfig, CopyHandlesNull) {
    database_config_t* copy = database_config_copy(nullptr);
    EXPECT_EQ(copy, nullptr);
}

// Test: WAL config defaults are reasonable
TEST(DatabaseConfig, WalConfigDefaults) {
    database_config_t* config = database_config_default();
    ASSERT_NE(config, nullptr);
    
    EXPECT_EQ(config->wal_config.sync_mode, WAL_SYNC_IMMEDIATE);
    EXPECT_GT(config->wal_config.debounce_ms, 0u);
    EXPECT_GT(config->wal_config.max_file_size, 0u);
    
    database_config_destroy(config);
}
```

- [ ] **Step 2: Add test to CMakeLists.txt**

In `tests/CMakeLists.txt`, add the test executable:

```cmake
add_executable(test_database_config test_database_config.cpp)
target_link_libraries(test_database_config PRIVATE wavedb GTest::gtest_main)
add_test(NAME test_database_config COMMAND test_database_config)
```

- [ ] **Step 3: Build and run tests**

```bash
cd /home/victor/Workspace/src/github.com/vijayee/WaveDB/build
cmake --build . -j$(nproc)
ctest -R test_database_config --output-on-failure
```

Expected: All tests pass.

- [ ] **Step 4: Commit tests**

```bash
git add tests/test_database_config.cpp tests/CMakeLists.txt
git commit -m "test(config): add unit tests for database_config_t"
```

---

## Task 5: Add Fields to database_t

**Files:**
- Modify: `src/Database/database.h`

- [ ] **Step 1: Add ownership fields to database_t**

In `src/Database/database.h`, add to the `database_t` struct (after `storage_max_tuple`):

```c
    // Config ownership tracking
    bool owns_pool;                     // True if database created the pool
    bool owns_wheel;                   // True if database created the wheel
    
    // Active configuration
    database_config_t* active_config;   // Current config (for runtime queries)
```

Also add forward declaration before the struct:

```c
// Forward declaration
typedef struct database_config database_config_t;
```

- [ ] **Step 2: Include config header**

Add at the top of `src/Database/database.h`:

```c
#include "database_config.h"
```

- [ ] **Step 3: Build and verify**

```bash
cd /home/victor/Workspace/src/github.com/vijayee/WaveDB/build
cmake --build . -j$(nproc)
```

Expected: Build succeeds.

- [ ] **Step 4: Commit**

```bash
git add src/Database/database.h
git commit -m "feat(database): add config ownership fields to database_t"
```

---

## Summary

Phase 1 creates the foundation:
- `database_config_t` structure with all settings
- Basic API: `database_config_default()`, `database_config_copy()`, `database_config_destroy()`
- Unit tests for core functionality
- Database struct prepared for ownership tracking

**Next Phase:** CBOR serialization (`database_config_load`, `database_config_save`, `database_config_merge`)