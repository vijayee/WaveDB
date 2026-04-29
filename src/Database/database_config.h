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
#include "../Storage/encryption.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Default values
 */
#define DATABASE_CONFIG_DEFAULT_CHUNK_SIZE 4
#define DATABASE_CONFIG_DEFAULT_BTREE_NODE_SIZE 4096
#define DATABASE_CONFIG_DEFAULT_LRU_MEMORY_MB 50
#define DATABASE_CONFIG_DEFAULT_LRU_SHARDS 64  // Default LRU shard count
#define DATABASE_CONFIG_DEFAULT_BNODE_CACHE_MEMORY_MB 128
#define DATABASE_CONFIG_DEFAULT_BNODE_CACHE_SHARDS 4
#define DATABASE_CONFIG_DEFAULT_WORKER_THREADS 4
#define DATABASE_CONFIG_DEFAULT_TIMER_RESOLUTION_MS 10
#define DATABASE_CONFIG_DEFAULT_ENCRYPTION_TYPE ENCRYPTION_NONE

/**
 * Encryption configuration.
 *
 * Key material (key, private_key_der, public_key_der) is NOT persisted.
 * Salt and check are persisted for key verification on re-open.
 */
typedef struct {
    encryption_type_t type;
    uint8_t* key;                    /* Symmetric: 32-byte key (not persisted) */
    size_t key_length;
    uint8_t* private_key_der;        /* Asymmetric: DER private key (not persisted) */
    size_t private_key_len;
    uint8_t* public_key_der;         /* Asymmetric: DER public key (not persisted) */
    size_t public_key_len;
    uint8_t salt[16];               /* Persisted: verification salt */
    uint8_t check[44];              /* Persisted: verification ciphertext+tag (IV:12 + ct:16 + tag:16) */
    uint8_t has_encryption;          /* Whether encryption is enabled */
} encryption_config_t;

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
    encryption_config_t encryption; // Encryption settings (type + keys are immutable)
    uint8_t sync_only;            // 1 = sync-only (no MVCC, no locks), 0 = concurrent

    // === MUTABLE SETTINGS ===
    size_t lru_memory_mb;         // LRU cache size in MB (default: 50)
    uint16_t lru_shards;          // LRU cache shard count (default: 64, 0 = auto-scale)
    size_t bnode_cache_memory_mb; // Bnode cache size in MB (default: 128)
    uint16_t bnode_cache_shards;  // Bnode cache shard count (default: 4)
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
 * Saves to <location>/.config
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

/**
 * Configuration setters for individual fields.
 */
void database_config_set_chunk_size(database_config_t* config, uint8_t chunk_size);
void database_config_set_btree_node_size(database_config_t* config, uint32_t node_size);
void database_config_set_enable_persist(database_config_t* config, uint8_t enable);
void database_config_set_lru_memory_mb(database_config_t* config, size_t mb);
void database_config_set_lru_shards(database_config_t* config, uint16_t shards);
void database_config_set_bnode_cache_memory_mb(database_config_t* config, size_t mb);
void database_config_set_bnode_cache_shards(database_config_t* config, uint16_t shards);
void database_config_set_worker_threads(database_config_t* config, uint8_t threads);
void database_config_set_wal_sync_mode(database_config_t* config, uint8_t mode);
void database_config_set_wal_debounce_ms(database_config_t* config, uint64_t ms);
void database_config_set_wal_max_file_size(database_config_t* config, size_t size);
void database_config_set_timer_resolution_ms(database_config_t* config, uint16_t ms);
void database_config_set_sync_only(database_config_t* config, uint8_t sync_only);

/**
 * Encrypted database configuration.
 *
 * Wraps database_config_t with encryption key material.
 * Used when opening a database with encryption.
 */
typedef struct {
    database_config_t config;
    encryption_type_t type;
    union {
        struct {
            const uint8_t* key;
            size_t key_length;
        } symmetric;
        struct {
            const uint8_t* private_key_der;
            size_t private_key_len;
            const uint8_t* public_key_der;
            size_t public_key_len;
        } asymmetric;
    };
} encrypted_database_config_t;

/**
 * Create default encrypted configuration.
 *
 * Returns a config with all defaults set and encryption type NONE.
 * Caller must destroy with encrypted_database_config_destroy().
 *
 * @return New config or NULL on failure
 */
encrypted_database_config_t* encrypted_database_config_default(void);

/**
 * Destroy an encrypted configuration.
 *
 * @param config  Config to destroy
 */
void encrypted_database_config_destroy(encrypted_database_config_t* config);

/**
 * Set encryption type.
 */
void encrypted_database_config_set_type(encrypted_database_config_t* config, encryption_type_t type);

/**
 * Set symmetric key (32 bytes for AES-256).
 */
void encrypted_database_config_set_symmetric_key(encrypted_database_config_t* config, const uint8_t* key, size_t key_length);

/**
 * Set asymmetric private key (DER-encoded, may be NULL for write-only mode).
 */
void encrypted_database_config_set_asymmetric_private_key(encrypted_database_config_t* config, const uint8_t* key, size_t key_length);

/**
 * Set asymmetric public key (DER-encoded, required).
 */
void encrypted_database_config_set_asymmetric_public_key(encrypted_database_config_t* config, const uint8_t* key, size_t key_length);

#ifdef __cplusplus
}
#endif

#endif // WAVEDB_DATABASE_CONFIG_H