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

    // Encryption defaults
    config->encryption.type = DATABASE_CONFIG_DEFAULT_ENCRYPTION_TYPE;
    config->encryption.key = NULL;
    config->encryption.key_length = 0;
    config->encryption.private_key_der = NULL;
    config->encryption.private_key_len = 0;
    config->encryption.public_key_der = NULL;
    config->encryption.public_key_len = 0;
    memset(config->encryption.salt, 0, sizeof(config->encryption.salt));
    memset(config->encryption.check, 0, sizeof(config->encryption.check));
    config->encryption.has_encryption = 0;

    // Mutable settings
    config->lru_memory_mb = DATABASE_CONFIG_DEFAULT_LRU_MEMORY_MB;
    config->lru_shards = DATABASE_CONFIG_DEFAULT_LRU_SHARDS;  // 0 = auto-scale
    config->bnode_cache_memory_mb = DATABASE_CONFIG_DEFAULT_BNODE_CACHE_MEMORY_MB;
    config->bnode_cache_shards = DATABASE_CONFIG_DEFAULT_BNODE_CACHE_SHARDS;

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

    // Deep copy encryption key material
    if (config->encryption.key != NULL) {
        copy->encryption.key = get_clear_memory(config->encryption.key_length);
        if (copy->encryption.key == NULL) {
            free(copy);
            return NULL;
        }
        memcpy(copy->encryption.key, config->encryption.key, config->encryption.key_length);
    }
    if (config->encryption.private_key_der != NULL) {
        copy->encryption.private_key_der = get_clear_memory(config->encryption.private_key_len);
        if (copy->encryption.private_key_der == NULL) {
            free(copy->encryption.key);
            free(copy);
            return NULL;
        }
        memcpy(copy->encryption.private_key_der, config->encryption.private_key_der, config->encryption.private_key_len);
    }
    if (config->encryption.public_key_der != NULL) {
        copy->encryption.public_key_der = get_clear_memory(config->encryption.public_key_len);
        if (copy->encryption.public_key_der == NULL) {
            free(copy->encryption.key);
            free(copy->encryption.private_key_der);
            free(copy);
            return NULL;
        }
        memcpy(copy->encryption.public_key_der, config->encryption.public_key_der, config->encryption.public_key_len);
    }

    return copy;
}

void database_config_destroy(database_config_t* config) {
    if (config == NULL) {
        return;
    }

    // Free encryption key material
    if (config->encryption.key != NULL) {
        free(config->encryption.key);
    }
    if (config->encryption.private_key_der != NULL) {
        free(config->encryption.private_key_der);
    }
    if (config->encryption.public_key_der != NULL) {
        free(config->encryption.public_key_der);
    }

    free(config);
}

int database_config_save(const char* location, const database_config_t* config) {
    if (location == NULL || config == NULL) {
        return -1;
    }

    // Build path: <location>/.config
    size_t path_len = strlen(location) + strlen("/.config") + 1;
    char* config_path = malloc(path_len);
    if (config_path == NULL) {
        return -1;
    }
    snprintf(config_path, path_len, "%s/.config", location);

    // Create CBOR map
    cbor_item_t* root = cbor_new_definite_map(14);
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
        .key = cbor_move(cbor_build_string("lru_shards")),
        .value = cbor_move(cbor_build_uint16(config->lru_shards))
    });

    cbor_map_add(root, (struct cbor_pair) {
        .key = cbor_move(cbor_build_string("bnode_cache_memory_mb")),
        .value = cbor_move(cbor_build_uint64(config->bnode_cache_memory_mb))
    });

    cbor_map_add(root, (struct cbor_pair) {
        .key = cbor_move(cbor_build_string("bnode_cache_shards")),
        .value = cbor_move(cbor_build_uint16(config->bnode_cache_shards))
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

    // Add encryption settings
    cbor_map_add(root, (struct cbor_pair) {
        .key = cbor_move(cbor_build_string("encryption_type")),
        .value = cbor_move(cbor_build_uint8((uint8_t)config->encryption.type))
    });

    cbor_map_add(root, (struct cbor_pair) {
        .key = cbor_move(cbor_build_string("encryption_salt")),
        .value = cbor_move(cbor_build_bytestring(config->encryption.salt, 16))
    });

    cbor_map_add(root, (struct cbor_pair) {
        .key = cbor_move(cbor_build_string("encryption_check")),
        .value = cbor_move(cbor_build_bytestring(config->encryption.check, 44))
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
        return cbor_get_int(value);
    }
    return default_val;
}

// Helper to get bytestring from CBOR map
static cbor_item_t* get_map_bytestring(cbor_item_t* map, const char* key) {
    cbor_item_t* key_item = cbor_build_string(key);
    cbor_item_t* value = NULL;

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

    if (value != NULL && cbor_isa_bytestring(value)) {
        return value;
    }
    return NULL;
}

database_config_t* database_config_load(const char* location) {
    if (location == NULL) {
        return NULL;
    }

    // Build path: <location>/.config
    size_t path_len = strlen(location) + strlen("/.config") + 1;
    char* config_path = malloc(path_len);
    if (config_path == NULL) {
        return NULL;
    }
    snprintf(config_path, path_len, "%s/.config", location);

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
    config->lru_shards = (uint16_t)get_map_uint(root, "lru_shards", DATABASE_CONFIG_DEFAULT_LRU_SHARDS);
    config->bnode_cache_memory_mb = get_map_uint(root, "bnode_cache_memory_mb", DATABASE_CONFIG_DEFAULT_BNODE_CACHE_MEMORY_MB);
    config->bnode_cache_shards = (uint16_t)get_map_uint(root, "bnode_cache_shards", DATABASE_CONFIG_DEFAULT_BNODE_CACHE_SHARDS);

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

    // Read encryption settings
    config->encryption.type = (encryption_type_t)get_map_uint(root, "encryption_type", DATABASE_CONFIG_DEFAULT_ENCRYPTION_TYPE);

    cbor_item_t* salt_item = get_map_bytestring(root, "encryption_salt");
    if (salt_item != NULL && cbor_bytestring_length(salt_item) == 16) {
        memcpy(config->encryption.salt, cbor_bytestring_handle(salt_item), 16);
    }

    cbor_item_t* check_item = get_map_bytestring(root, "encryption_check");
    if (check_item != NULL && cbor_bytestring_length(check_item) == 44) {
        memcpy(config->encryption.check, cbor_bytestring_handle(check_item), 44);
    }

    if (config->encryption.type != ENCRYPTION_NONE) {
        config->encryption.has_encryption = 1;
    }

    cbor_decref(&root);
    return config;
}

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

    // ENCRYPTION: Immutable type and persisted verification data come from saved config.
    // Key material (not persisted) comes from passed config at open time.
    merged->encryption.type = saved->encryption.type;
    memcpy(merged->encryption.salt, saved->encryption.salt, sizeof(saved->encryption.salt));
    memcpy(merged->encryption.check, saved->encryption.check, sizeof(saved->encryption.check));
    merged->encryption.has_encryption = saved->encryption.has_encryption;
    // Deep copy key material from passed config (not persisted, supplied at open time)
    if (passed->encryption.key != NULL) {
        merged->encryption.key = get_clear_memory(passed->encryption.key_length);
        if (merged->encryption.key != NULL) {
            memcpy(merged->encryption.key, passed->encryption.key, passed->encryption.key_length);
        }
        merged->encryption.key_length = passed->encryption.key_length;
    }
    if (passed->encryption.private_key_der != NULL) {
        merged->encryption.private_key_der = get_clear_memory(passed->encryption.private_key_len);
        if (merged->encryption.private_key_der != NULL) {
            memcpy(merged->encryption.private_key_der, passed->encryption.private_key_der, passed->encryption.private_key_len);
        }
        merged->encryption.private_key_len = passed->encryption.private_key_len;
    }
    if (passed->encryption.public_key_der != NULL) {
        merged->encryption.public_key_der = get_clear_memory(passed->encryption.public_key_len);
        if (merged->encryption.public_key_der != NULL) {
            memcpy(merged->encryption.public_key_der, passed->encryption.public_key_der, passed->encryption.public_key_len);
        }
        merged->encryption.public_key_len = passed->encryption.public_key_len;
    }

    // MUTABLE: Use passed values (user can change these)
    merged->lru_memory_mb = passed->lru_memory_mb;
    merged->lru_shards = passed->lru_shards;
    merged->bnode_cache_memory_mb = passed->bnode_cache_memory_mb;
    merged->bnode_cache_shards = passed->bnode_cache_shards;
    merged->wal_config = passed->wal_config;

    // THREADING: Use passed values
    merged->worker_threads = passed->worker_threads;
    merged->timer_resolution_ms = passed->timer_resolution_ms;

    // EXTERNAL: Use passed values (runtime only)
    merged->external_pool = passed->external_pool;
    merged->external_wheel = passed->external_wheel;

    return merged;
}

// === Configuration setters ===

void database_config_set_chunk_size(database_config_t* config, uint8_t chunk_size) {
    if (config == NULL) return;
    config->chunk_size = chunk_size;
}

void database_config_set_btree_node_size(database_config_t* config, uint32_t node_size) {
    if (config == NULL) return;
    config->btree_node_size = node_size;
}

void database_config_set_enable_persist(database_config_t* config, uint8_t enable) {
    if (config == NULL) return;
    config->enable_persist = enable;
}

void database_config_set_lru_memory_mb(database_config_t* config, size_t mb) {
    if (config == NULL) return;
    config->lru_memory_mb = mb;
}

void database_config_set_lru_shards(database_config_t* config, uint16_t shards) {
    if (config == NULL) return;
    config->lru_shards = shards;
}

void database_config_set_bnode_cache_memory_mb(database_config_t* config, size_t mb) {
    if (config == NULL) return;
    config->bnode_cache_memory_mb = mb;
}

void database_config_set_bnode_cache_shards(database_config_t* config, uint16_t shards) {
    if (config == NULL) return;
    config->bnode_cache_shards = shards;
}

void database_config_set_worker_threads(database_config_t* config, uint8_t threads) {
    if (config == NULL) return;
    config->worker_threads = threads;
}

void database_config_set_wal_sync_mode(database_config_t* config, uint8_t mode) {
    if (config == NULL) return;
    config->wal_config.sync_mode = (wal_sync_mode_e)mode;
}

void database_config_set_wal_debounce_ms(database_config_t* config, uint64_t ms) {
    if (config == NULL) return;
    config->wal_config.debounce_ms = ms;
}

void database_config_set_wal_max_file_size(database_config_t* config, size_t size) {
    if (config == NULL) return;
    config->wal_config.max_file_size = size;
}

void database_config_set_timer_resolution_ms(database_config_t* config, uint16_t ms) {
    if (config == NULL) return;
    config->timer_resolution_ms = ms;
}

// === Encrypted database config ===

encrypted_database_config_t* encrypted_database_config_default(void) {
    encrypted_database_config_t* config = get_clear_memory(sizeof(encrypted_database_config_t));
    if (config == NULL) {
        return NULL;
    }

    // Initialize the embedded database config
    database_config_t* db_config = database_config_default();
    if (db_config == NULL) {
        free(config);
        return NULL;
    }
    memcpy(&config->config, db_config, sizeof(database_config_t));
    free(db_config);

    // Initialize encryption fields
    config->type = ENCRYPTION_NONE;
    config->symmetric.key = NULL;
    config->symmetric.key_length = 0;
    config->asymmetric.private_key_der = NULL;
    config->asymmetric.private_key_len = 0;
    config->asymmetric.public_key_der = NULL;
    config->asymmetric.public_key_len = 0;

    return config;
}

void encrypted_database_config_destroy(encrypted_database_config_t* config) {
    if (config == NULL) return;

    // Free encryption key material from the embedded config
    if (config->config.encryption.key != NULL) {
        free(config->config.encryption.key);
    }
    if (config->config.encryption.private_key_der != NULL) {
        free(config->config.encryption.private_key_der);
    }
    if (config->config.encryption.public_key_der != NULL) {
        free(config->config.encryption.public_key_der);
    }

    // Free the outer struct (embedded config is part of this allocation)
    free(config);
}

void encrypted_database_config_set_type(encrypted_database_config_t* config, encryption_type_t type) {
    if (config == NULL) return;
    config->type = type;
    config->config.encryption.type = type;
    if (type != ENCRYPTION_NONE) {
        config->config.encryption.has_encryption = 1;
    } else {
        config->config.encryption.has_encryption = 0;
    }
}

void encrypted_database_config_set_symmetric_key(encrypted_database_config_t* config, const uint8_t* key, size_t key_length) {
    if (config == NULL) return;

    // Free previous copy if any
    if (config->config.encryption.key != NULL) {
        free(config->config.encryption.key);
        config->config.encryption.key = NULL;
    }

    uint8_t* copy = get_clear_memory(key_length);
    if (copy == NULL) return;
    memcpy(copy, key, key_length);

    config->symmetric.key = copy;
    config->symmetric.key_length = key_length;
    config->config.encryption.key = copy;
    config->config.encryption.key_length = key_length;
}

void encrypted_database_config_set_asymmetric_private_key(encrypted_database_config_t* config, const uint8_t* key, size_t key_length) {
    if (config == NULL) return;

    // Free previous copy if any
    if (config->config.encryption.private_key_der != NULL) {
        free(config->config.encryption.private_key_der);
        config->config.encryption.private_key_der = NULL;
    }

    if (key == NULL || key_length == 0) {
        config->asymmetric.private_key_der = NULL;
        config->asymmetric.private_key_len = 0;
        config->config.encryption.private_key_der = NULL;
        config->config.encryption.private_key_len = 0;
        return;
    }

    uint8_t* copy = get_clear_memory(key_length);
    if (copy == NULL) return;
    memcpy(copy, key, key_length);

    config->asymmetric.private_key_der = copy;
    config->asymmetric.private_key_len = key_length;
    config->config.encryption.private_key_der = copy;
    config->config.encryption.private_key_len = key_length;
}

void encrypted_database_config_set_asymmetric_public_key(encrypted_database_config_t* config, const uint8_t* key, size_t key_length) {
    if (config == NULL) return;

    // Free previous copy if any
    if (config->config.encryption.public_key_der != NULL) {
        free(config->config.encryption.public_key_der);
        config->config.encryption.public_key_der = NULL;
    }

    uint8_t* copy = get_clear_memory(key_length);
    if (copy == NULL) return;
    memcpy(copy, key, key_length);

    config->asymmetric.public_key_der = copy;
    config->asymmetric.public_key_len = key_length;
    config->config.encryption.public_key_der = copy;
    config->config.encryption.public_key_len = key_length;
}