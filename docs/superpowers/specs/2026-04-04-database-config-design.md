# Database Configuration System Design

**Date:** 2026-04-04
**Status:** Approved
**Scope:** Configuration management for WaveDB database creation and persistence

---

## Overview

WaveDB currently has a `database_create()` function with 10 parameters, making it unwieldy to call and difficult to persist configuration. This design introduces a `database_config_t` type that:

1. Groups all configurable aspects into a single structure
2. Persists configuration to disk using CBOR
3. Merges passed config with saved config on reopen
4. Allows the database to create its own work pool and timing wheel

---

## Configuration Categories

### Immutable Settings (Fixed at Creation)

These settings cannot change after the database is created because they affect data structure layout:

| Setting | Type | Default | Description |
|---------|------|---------|-------------|
| `chunk_size` | `uint8_t` | 4 | HBTrie chunk size in bytes |
| `btree_node_size` | `uint32_t` | 4096 | B+tree node size in bytes |
| `enable_persist` | `uint8_t` | 1 | 0 = in-memory only, 1 = persistent |

### Mutable Settings (Changeable on Reopen)

These settings can be modified when reopening an existing database:

| Setting | Type | Default | Description |
|---------|------|---------|-------------|
| `lru_memory_mb` | `size_t` | 50 | LRU cache memory budget in MB |
| `storage_cache_size` | `size_t` | 1024 | Section cache size |
| `wal_config` | `wal_config_t` | see below | WAL configuration |

### WAL Configuration (Embedded)

The `wal_config_t` structure is embedded in the config:

```c
typedef struct {
    wal_sync_mode_e sync_mode;      // IMMEDIATE, DEBOUNCED, ASYNC
    uint64_t debounce_ms;           // Debounce window (default: 100)
    uint64_t idle_threshold_ms;     // Compaction trigger (default: 10000)
    uint64_t compact_interval_ms;    // Compaction interval (default: 60000)
    size_t max_file_size;           // Max WAL file size (default: 128KB)
} wal_config_t;
```

### Threading Settings

| Setting | Type | Default | Description |
|---------|------|---------|-------------|
| `worker_threads` | `uint8_t` | 4 | Number of worker threads (0 = use external pool) |
| `timer_resolution_ms` | `uint16_t` | 10 | Timing wheel resolution in ms |

### External Resources (Not Saved)

These are runtime-only, not persisted:

| Setting | Type | Description |
|---------|------|-------------|
| `external_pool` | `work_pool_t*` | External work pool (NULL = create own) |
| `external_wheel` | `hierarchical_timing_wheel_t*` | External timing wheel (NULL = create own) |

---

## Data Structures

### database_config_t

```c
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
```

---

## API

### Creation and Lifecycle

```c
/**
 * Create default configuration.
 * 
 * Returns a config with all defaults set. Caller must destroy.
 */
database_config_t* database_config_default(void);

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
 */
database_t* database_create_with_config(const char* location,
                                        database_config_t* config,
                                        int* error_code);

/**
 * Destroy a configuration.
 */
void database_config_destroy(database_config_t* config);
```

### Persistence

```c
/**
 * Load configuration from existing database.
 * 
 * @param location  Directory path
 * @return Config or NULL if not found
 */
database_config_t* database_config_load(const char* location);

/**
 * Save configuration to disk.
 * 
 * Called automatically on database creation/close.
 * Saved to <location>/config.cbor
 * 
 * @param db  Database to save
 * @return 0 on success, -1 on failure
 */
int database_config_save(database_t* db);
```

### Merge

```c
/**
 * Merge two configurations.
 * 
 * @param saved  Config loaded from disk (may be NULL)
 * @param passed  Config passed by user (may be NULL)
 * @return Merged config (caller must destroy)
 * 
 * Merge rules:
 * - If saved is NULL: return passed (or defaults if both NULL)
 * - If passed is NULL: return saved
 * - Immutable settings: use saved values (ignore passed)
 * - Mutable settings: use passed values (override saved)
 */
database_config_t* database_config_merge(database_config_t* saved,
                                         database_config_t* passed);
```

### Utility

```c
/**
 * Get current configuration from a database.
 * 
 * Returns a copy of the active configuration.
 */
database_config_t* database_config_get(database_t* db);

/**
 * Update mutable settings at runtime.
 * 
 * Only mutable settings are updated; immutable settings are ignored.
 * Returns error if setting cannot be changed.
 * 
 * @param db      Database to update
 * @param config  New settings (only mutable fields used)
 * @return 0 on success, -1 on failure
 */
int database_config_update(database_t* db, database_config_t* config);
```

---

## Config File Format

### Location

```
<location>/config.cbor
```

### CBOR Structure

```cbor
{
    "version": 1,
    "chunk_size": 4,
    "btree_node_size": 4096,
    "enable_persist": 1,
    "lru_memory_mb": 50,
    "storage_cache_size": 1024,
    "wal": {
        "sync_mode": 1,
        "debounce_ms": 100,
        "idle_threshold_ms": 10000,
        "compact_interval_ms": 60000,
        "max_file_size": 131072
    },
    "worker_threads": 4,
    "timer_resolution_ms": 10
}
```

### Versioning

- `version` field allows future format changes
- Version 1 is the initial format
- Unknown fields are ignored (forward compatibility)

---

## Backward Compatibility

### Existing database_create()

The existing `database_create()` function remains for backward compatibility:

```c
// Legacy API - internally converts to config
database_t* database_create(const char* location, size_t lru_memory_mb,
                            wal_config_t* wal_config,
                            uint8_t chunk_size, uint32_t btree_node_size,
                            uint8_t enable_persist, size_t storage_cache_size,
                            work_pool_t* pool, hierarchical_timing_wheel_t* wheel,
                            int* error_code);
```

Implementation:

```c
database_t* database_create(...) {
    database_config_t* config = database_config_default();
    config->lru_memory_mb = lru_memory_mb == 0 ? 50 : lru_memory_mb;
    config->chunk_size = chunk_size;
    config->btree_node_size = btree_node_size;
    config->enable_persist = enable_persist;
    config->storage_cache_size = storage_cache_size;
    config->external_pool = pool;
    config->external_wheel = wheel;
    if (wal_config) config->wal_config = *wal_config;
    
    database_t* db = database_create_with_config(location, config, error_code);
    database_config_destroy(config);
    return db;
}
```

---

## Threading Behavior

### Creating Own Pool/Wheel

When `worker_threads > 0` and external resources are NULL:

```c
// Create work pool
db->pool = work_pool_create(config->worker_threads);

// Create timing wheel  
db->wheel = hierarchical_timing_wheel_create(config->timer_resolution_ms);
db->owns_pool = true;   // Track ownership for cleanup
db->owns_wheel = true;
```

### Using External Resources

When external resources are provided:

```c
db->pool = config->external_pool;
db->wheel = config->external_wheel;
db->owns_pool = false;
db->owns_wheel = false;
```

### Cleanup

```c
void database_destroy(database_t* db) {
    // ... existing cleanup ...
    
    if (db->owns_pool && db->pool) {
        work_pool_destroy(db->pool);
    }
    if (db->owns_wheel && db->wheel) {
        hierarchical_timing_wheel_destroy(db->wheel);
    }
    
    // ... free db ...
}
```

---

## Error Handling

| Error Code | Description |
|------------|-------------|
| 0 | Success |
| ENOENT | Database not found (for load) |
| EEXIST | Database already exists with different immutable config |
| EINVAL | Invalid configuration |
| ENOMEM | Out of memory |
| EIO | Failed to read/write config file |

---

## File Organization

```
src/Database/
├── database_config.h    # Config structure and API
├── database_config.c    # Implementation
├── database.h           # Updated with new API
├── database.c           # Updated implementation
```

---

## Usage Examples

### Simple Creation (Defaults)

```c
database_t* db = database_create_with_config("/path/to/db", NULL, &error);
```

### Custom Configuration

```c
database_config_t* config = database_config_default();
config->lru_memory_mb = 100;
config->worker_threads = 8;
config->wal_config.sync_mode = WAL_SYNC_DEBOUNCED;

database_t* db = database_create_with_config("/path/to/db", config, &error);
database_config_destroy(config);
```

### In-Memory Database

```c
database_config_t* config = database_config_default();
config->enable_persist = 0;
config->lru_memory_mb = 256;

database_t* db = database_create_with_config("/tmp/mydb", config, &error);
```

### Reopening with Settings Change

```c
// Change LRU size on reopen
database_config_t* config = database_config_default();
config->lru_memory_mb = 200;  // Increase cache

database_t* db = database_create_with_config("/path/to/db", config, &error);
// Existing immutable settings preserved, LRU size updated
```

---

## Implementation Notes

### CBOR Serialization

Use existing `libcbor` dependency:
- `cbor_serialize_alloc()` for writing
- `cbor_load()` for reading
- Helper functions for each field type

### Database Structure Changes

Add to `database_t`:

```c
typedef struct {
    // ... existing fields ...
    
    // Config ownership tracking
    bool owns_pool;
    bool owns_wheel;
    
    // Active config (for runtime queries)
    database_config_t* active_config;
} database_t;
```

### Migration

For existing databases without `config.cbor`:
- Create default config on first open
- Infer `enable_persist` from existence of storage files
- Use defaults for other settings
- Save config on close

---

## Summary

- `database_config_t` groups all configurable aspects
- Immutable settings fixed at creation, mutable settings changeable
- Config persisted as CBOR in `<location>/config.cbor`
- Database can create own pool/wheel or use external
- Backward compatible with existing `database_create()` API