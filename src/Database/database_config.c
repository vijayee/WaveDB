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