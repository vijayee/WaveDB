//
// Database Configuration
// Created: 2026-04-04
//

#ifndef WAVEDB_DATABASE_CONFIG_H
#define WAVEDB_DATABASE_CONFIG_H

#include <stdint.h>
#include <stddef.h>
#include "wal_manager.h"
#include "../Workers/pool.h"
#include "../Time/wheel.h"

#ifdef __cplusplus
extern "C" {
#endif

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

#ifdef __cplusplus
}
#endif

#endif // WAVEDB_DATABASE_CONFIG_H