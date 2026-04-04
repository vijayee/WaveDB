//
// Database Configuration Implementation
// Created: 2026-04-04
//

#include "database_config.h"
#include "../Util/allocator.h"
#include <cbor.h>
#include <stdio.h>
#include <sys/stat.h>
#include <errno.h>
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