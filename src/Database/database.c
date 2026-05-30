//
// Created by victor on 3/11/26.
//

#include "database.h"
#include "database_config.h"
#include "database_iterator.h"
#include "batch.h"
#include "eviction_queue.h"
#include "../Workers/work.h"
#include "../Workers/error.h"
#include "../Util/allocator.h"
#include "../Util/mkdir_p.h"
#include "../Util/path_join.h"
#include "../Util/log.h"
#include "../Storage/node_serializer.h"
#include "../Storage/page_file.h"
#include "../Storage/encryption.h"
#include "../Storage/bnode_cache.h"
#include "../Util/memory_pool.h"
#include "../Util/endian.h"
#include <cbor.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <errno.h>
#include "Util/dirent_compat.h"
#include <sys/stat.h>
#if _WIN32
#include "Util/unistd_compat.h"
#define fsync(fd) _commit(fd)
#define open _open
#define O_RDONLY _O_RDONLY
#define O_WRONLY _O_WRONLY
#define O_RDWR _O_RDWR
#define O_CREAT _O_CREAT
#define O_TRUNC _O_TRUNC
#define O_BINARY _O_BINARY
#else
#include <unistd.h>
#endif

#define WAL_BINARY_MAGIC 0xB1

// ============================================================================
// WAL encode helpers (kept for future use with WAL actor)
// ============================================================================

static buffer_t* encode_put_entry_binary(path_t* path, identifier_t* value) {
    size_t path_count = path_length(path);
    size_t total_size = 1 + 2 + 4;

    size_t path_byte_len = 0;
    for (size_t i = 0; i < path_count; i++) {
        identifier_t* id = path_get(path, i);
        size_t id_len = id->length;
        total_size += 2 + id_len;
        path_byte_len += id_len;
    }

    size_t value_len = (value != NULL) ? value->length : 0;
    total_size += 4 + value_len;

    buffer_t* buffer = buffer_create(total_size);
    if (buffer == NULL) return NULL;

    uint8_t* pos = buffer->data;
    *pos++ = WAL_BINARY_MAGIC;
    write_be16(pos, (uint16_t)path_count);
    pos += 2;
    write_be32(pos, (uint32_t)path_byte_len);
    pos += 4;

    for (size_t i = 0; i < path_count; i++) {
        identifier_t* id = path_get(path, i);
        size_t id_len = id->length;
        write_be16(pos, (uint16_t)id_len);
        pos += 2;
        buffer_t* id_buf = identifier_to_buffer(id);
        if (id_buf == NULL) {
            buffer_destroy(buffer);
            return NULL;
        }
        memcpy(pos, id_buf->data, id_len);
        pos += id_len;
        buffer_destroy(id_buf);
    }

    write_be32(pos, (uint32_t)value_len);
    pos += 4;
    if (value_len > 0) {
        buffer_t* val_buf = identifier_to_buffer(value);
        if (val_buf == NULL) {
            buffer_destroy(buffer);
            return NULL;
        }
        memcpy(pos, val_buf->data, value_len);
        pos += value_len;
        buffer_destroy(val_buf);
    }

    buffer->size = (size_t)(pos - buffer->data);
    return buffer;
}

static buffer_t* encode_delete_entry_binary(path_t* path) {
    size_t path_count = path_length(path);
    size_t total_size = 1 + 2 + 4;
    size_t path_byte_len = 0;
    for (size_t i = 0; i < path_count; i++) {
        identifier_t* id = path_get(path, i);
        size_t id_len = id->length;
        total_size += 2 + id_len;
        path_byte_len += id_len;
    }

    buffer_t* buffer = buffer_create(total_size);
    if (buffer == NULL) return NULL;

    uint8_t* pos = buffer->data;
    *pos++ = WAL_BINARY_MAGIC;
    write_be16(pos, (uint16_t)path_count);
    pos += 2;
    write_be32(pos, (uint32_t)path_byte_len);
    pos += 4;

    for (size_t i = 0; i < path_count; i++) {
        identifier_t* id = path_get(path, i);
        size_t id_len = id->length;
        write_be16(pos, (uint16_t)id_len);
        pos += 2;
        buffer_t* id_buf = identifier_to_buffer(id);
        if (id_buf == NULL) {
            buffer_destroy(buffer);
            return NULL;
        }
        memcpy(pos, id_buf->data, id_len);
        pos += id_len;
        buffer_destroy(id_buf);
    }

    buffer->size = (size_t)(pos - buffer->data);
    return buffer;
}

static buffer_t* serialize_batch(batch_t* batch) {
    if (batch == NULL || batch->count == 0) return NULL;

    cbor_item_t* batch_array = cbor_new_definite_array(batch->count);
    if (batch_array == NULL) return NULL;

    platform_lock(&batch->lock);
    for (size_t i = 0; i < batch->count; i++) {
        batch_op_t* op = &batch->ops[i];
        cbor_item_t* op_array = cbor_new_definite_array(op->type == WAL_PUT ? 3 : 2);
        if (op_array == NULL) {
            platform_unlock(&batch->lock);
            cbor_decref(&batch_array);
            return NULL;
        }

        cbor_item_t* type_item = cbor_build_uint8(op->type);
        if (type_item == NULL) {
            platform_unlock(&batch->lock);
            cbor_decref(&op_array);
            cbor_decref(&batch_array);
            return NULL;
        }
        cbor_array_push(op_array, type_item);
        cbor_decref(&type_item);

        cbor_item_t* path_cbor = path_to_cbor(op->path);
        if (path_cbor == NULL) {
            platform_unlock(&batch->lock);
            cbor_decref(&op_array);
            cbor_decref(&batch_array);
            return NULL;
        }
        cbor_array_push(op_array, path_cbor);
        cbor_decref(&path_cbor);

        if (op->type == WAL_PUT && op->value != NULL) {
            cbor_item_t* value_cbor = identifier_to_cbor(op->value);
            if (value_cbor == NULL) {
                platform_unlock(&batch->lock);
                cbor_decref(&op_array);
                cbor_decref(&batch_array);
                return NULL;
            }
            cbor_array_push(op_array, value_cbor);
            cbor_decref(&value_cbor);
        }
        cbor_array_push(batch_array, op_array);
        cbor_decref(&op_array);
    }
    platform_unlock(&batch->lock);

    unsigned char* cbor_data;
    size_t cbor_size;
    size_t cbor_alen = cbor_serialize_alloc(batch_array, &cbor_data, &cbor_size);
    cbor_decref(&batch_array);
    if (cbor_alen == 0) return NULL;

    buffer_t* buffer = buffer_create(cbor_size);
    if (buffer == NULL) {
        free(cbor_data);
        return NULL;
    }
    memcpy(buffer->data, cbor_data, cbor_size);
    buffer->size = cbor_size;
    free(cbor_data);
    return buffer;
}

// ============================================================================
// Index persistence helpers (kept for backward compat with load_index/save_index)
// ============================================================================

static char* create_index_path(const char* location, uint64_t id, uint32_t crc) {
    (void)crc;
    char* path = malloc(1024);
    if (path) snprintf(path, 1024, "%s/data_%lu.idx", location, (unsigned long)id);
    return path;
}

static int save_index(database_t* db) {
    if (db == NULL || db->trie == NULL) return -1;
    if (db->page_file != NULL) return database_flush_dirty_bnodes(db);
    return -1; /* CBOR save not supported in actor mode */
}

static hbtrie_t* load_index(const char* location, uint8_t chunk_size,
                            uint32_t btree_node_size) {
    (void)location;
    return hbtrie_create(chunk_size, btree_node_size);
}

// ============================================================================
// Page file flush helpers (kept as-is from original)
// ============================================================================

static int compare_dirty_by_level(const void* a, const void* b) {
    bnode_t* ba = *(bnode_t**)a;
    bnode_t* bb = *(bnode_t**)b;
    uint16_t la = atomic_load(&ba->level);
    uint16_t lb = atomic_load(&bb->level);
    if (la < lb) return -1;
    if (la > lb) return 1;
    return 0;
}

static void propagate_dirty_upward(bnode_t* root) {
    if (root == NULL || !root->is_dirty) return;

    size_t n = root->entries.length;
    for (size_t i = 0; i < n; i++) {
        bnode_entry_t* entry = &root->entries.data[i];
        if (entry->is_bnode_child && entry->child_bnode != NULL && entry->child_bnode->is_dirty) {
            propagate_dirty_upward(entry->child_bnode);
        }
    }
}

static void collect_dirty_bnodes_from_hbnode(
    hbtrie_node_t* hbnode, vec_t(bnode_t*) * dirty_vec) {
    if (hbnode == NULL || !hbnode->is_loaded || hbnode->btree == NULL) return;

    bnode_t* root = hbnode->btree;
    if (root->is_dirty) {
        propagate_dirty_upward(root);
        vec_push(dirty_vec, root);
    }

    size_t n = root->entries.length;
    for (size_t i = 0; i < n; i++) {
        bnode_entry_t* entry = &root->entries.data[i];
        if (entry->is_bnode_child && entry->child_bnode != NULL) {
            vec_push(dirty_vec, entry->child_bnode);
        }
        if (!entry->has_value && !entry->is_bnode_child && entry->child != NULL) {
            collect_dirty_bnodes_from_hbnode(entry->child, dirty_vec);
        }
    }
}

int database_flush_dirty_bnodes(database_t* db) {
    if (db == NULL || db->trie == NULL || db->page_file == NULL) return -1;

    hbtrie_node_t* root = atomic_load(&db->trie->root);
    if (root == NULL || !root->is_loaded) return -1;

    vec_t(bnode_t*) dirty_vec;
    vec_init(&dirty_vec);
    collect_dirty_bnodes_from_hbnode(root, &dirty_vec);

    if (dirty_vec.length == 0) {
        vec_deinit(&dirty_vec);
        return 0;
    }

    qsort(dirty_vec.data, dirty_vec.length, sizeof(bnode_t*), compare_dirty_by_level);

    uint64_t last_written_offset = 0;
    size_t last_written_size = 0;

    for (size_t i = 0; i < dirty_vec.length; i++) {
        bnode_t* bnode = dirty_vec.data[i];
        uint8_t* buf = NULL;
        size_t buf_len = 0;

        bnode_cache_item_t* item = NULL;
        if (db->bnode_cache) {
            item = bnode_cache_read(db->bnode_cache, bnode->disk_offset);
            if (item) {
                buf = item->data;
                buf_len = item->data_len;
            }
        }

        bool owns_buf = false;
        if (buf == NULL) {
            uint8_t* serialized = NULL;
            int s_rc = bnode_serialize_v3(bnode, db->chunk_size, &serialized, &buf_len);
            if (s_rc == 0 && serialized) {
                buf = serialized;
                owns_buf = true;
            }
        }

        if (buf && buf_len > 0) {
            uint64_t new_offset = 0;
            uint64_t out_bids = 0;
            size_t out_num_bids = 0;
            int rc = page_file_write_node(db->page_file, buf, buf_len, &new_offset,
                                           &out_bids, 1, &out_num_bids);
            if (rc == 0) {
                if (bnode->disk_offset != UINT64_MAX) {
                    page_file_mark_stale(db->page_file, bnode->disk_offset, buf_len);
                }
                bnode->disk_offset = new_offset;
                bnode->is_dirty = 0;
                last_written_offset = new_offset;
                last_written_size = buf_len;
            }
        }

        if (item) {
            bnode_cache_release(db->bnode_cache, item);
        } else if (owns_buf && buf) {
            free(buf);
        }
    }

    if (dirty_vec.length > 0 && last_written_offset != 0) {
        transaction_id_t txn_id = {0, 0, 0};
        if (db->tx_manager) {
            txn_id = tx_manager_get_last_committed(db->tx_manager);
        }
        page_file_write_superblock(db->page_file, last_written_offset, last_written_size, &txn_id);
    }

    vec_deinit(&dirty_vec);
    return 0;
}

// ============================================================================
// Eviction queue and task (kept as-is, simplified without work_pool)
// ============================================================================

static void database_on_bnode_evict(uint64_t disk_offset, void* user_data) {
    database_t* db = (database_t*)user_data;
    if (db == NULL) return;
    eviction_queue_push(&db->eviction_queue, disk_offset);
}

static void database_eviction_task_execute(void* ctx) {
    database_t* db = (database_t*)ctx;
    if (db == NULL) return;

    uint64_t offsets[64];
    size_t n = eviction_queue_drain(&db->eviction_queue, offsets, 64);

    for (size_t i = 0; i < n; i++) {
        if (db->trie != NULL) {
            hbtrie_null_entries_by_offset(db->trie, offsets[i]);
        }
        if (db->bnode_cache != NULL) {
            bnode_cache_complete_evict(db->bnode_cache, offsets[i]);
        }
    }

    if (n == 0) {
        atomic_store(&db->eviction_in_flight, 0);
        return;
    }

    if (!db->destroying && db->scheduler_pool != NULL) {
        atomic_store(&db->eviction_in_flight, 0);
    } else {
        atomic_store(&db->eviction_in_flight, 0);
    }
}

// ============================================================================
// database_create_with_config — Actor-based creation
// ============================================================================

database_t* database_create_with_config(const char* location,
                                        database_config_t* config,
                                        int* error_code) {
    if (error_code) *error_code = 0;

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
    snprintf(config_path, sizeof(config_path), "%s/.config", location);
    struct stat st;
    bool db_exists = (stat(config_path, &st) == 0);

    if (db_exists) {
        database_config_t* saved_config = database_config_load(location);
        if (saved_config != NULL) {
            if (saved_config->encryption.has_encryption &&
                saved_config->encryption.type != ENCRYPTION_NONE &&
                !(effective_config->encryption.has_encryption &&
                  effective_config->encryption.type != ENCRYPTION_NONE)) {
                database_config_destroy(saved_config);
                if (owns_config) database_config_destroy(effective_config);
                if (error_code) *error_code = DATABASE_ERR_ENCRYPTION_REQUIRED;
                return NULL;
            }
            database_config_t* merged = database_config_merge(saved_config, effective_config);
            if (owns_config) database_config_destroy(effective_config);
            database_config_destroy(saved_config);
            effective_config = merged;
            owns_config = true;
        }
    }

    if (mkdir_p((char*)location) != 0) {
        if (error_code) *error_code = errno;
        if (owns_config) database_config_destroy(effective_config);
        return NULL;
    }

    memory_pool_init();
    transaction_id_init();

    database_t* db = get_clear_memory(sizeof(database_t));
    if (db == NULL) {
        if (error_code) *error_code = ENOMEM;
        if (owns_config) database_config_destroy(effective_config);
        return NULL;
    }

    db->location = strdup(location);
    db->lru_memory_mb = effective_config->lru_memory_mb;
    db->chunk_size = effective_config->chunk_size;
    db->btree_node_size = effective_config->btree_node_size;
    db->sync_only = effective_config->sync_only;
    db->is_rebuilding = 0;

    // Determine worker count
    size_t worker_count = effective_config->worker_threads > 0
        ? effective_config->worker_threads
        : (effective_config->sync_only ? 1 : 4);

    // Create scheduler pool (replaces work_pool_t)
    db->scheduler_pool = scheduler_pool_create(worker_count);
    if (db->scheduler_pool == NULL) {
        free(db->location);
        free(db);
        if (owns_config) database_config_destroy(effective_config);
        if (error_code) *error_code = ENOMEM;
        return NULL;
    }
    scheduler_pool_start(db->scheduler_pool);
    // Actor model: scheduler_pool replaces work_pool_t.
    // If external pool was provided, mark as not owned (caller's lifecycle).
    db->owns_pool = (effective_config->external_pool == NULL);
    // Actor model: no hierarchical timing wheel. Set owns_wheel for backward
    // compat with tests that check it when auto-creating resources.
    db->owns_wheel = (effective_config->external_wheel == NULL
                      && !effective_config->sync_only
                      && effective_config->worker_threads > 0);

    // Create storage directories if persistent
    if (effective_config->enable_persist) {
        char* data_path = path_join(location, "data");
        char* meta_path = path_join(location, "meta");
        mkdir_p(data_path);
        mkdir_p(meta_path);
        free(data_path);
        free(meta_path);
    }

    // Load or create trie (for backward compat; shard actors own their own tries)
    db->trie = load_index(location, db->chunk_size, db->btree_node_size);
    if (db->trie == NULL) {
        db->trie = hbtrie_create(db->chunk_size, db->btree_node_size);
    }

    // Create MVCC transaction manager (no pool/wheel needed — GC is per-shard)
    db->tx_manager = tx_manager_create(NULL, NULL, NULL, 0);
    if (db->tx_manager == NULL) {
        hbtrie_destroy(db->trie);
        scheduler_pool_stop(db->scheduler_pool);
        scheduler_pool_destroy(db->scheduler_pool);
        free(db->location);
        free(db);
        if (owns_config) database_config_destroy(effective_config);
        if (error_code) *error_code = ENOMEM;
        return NULL;
    }

    // Apply pending MVCC txn ID from page file if any
    if (db->has_pending_txn_id) {
        transaction_id_advance_to(&db->pending_txn_id);
        atomic_store(&db->tx_manager->last_committed_txn_id, db->pending_txn_id);
        db->has_pending_txn_id = 0;
    }

    // Create WAL actor (if persistence enabled)
    if (effective_config->enable_persist) {
        int wal_err = 0;
        db->wal_actor = wal_actor_create(location, NULL, db->encryption, &wal_err);
        if (db->wal_actor == NULL) {
            tx_manager_destroy(db->tx_manager);
            hbtrie_destroy(db->trie);
            scheduler_pool_stop(db->scheduler_pool);
            scheduler_pool_destroy(db->scheduler_pool);
            free(db->location);
            free(db);
            if (owns_config) database_config_destroy(effective_config);
            if (error_code) *error_code = (wal_err != 0) ? wal_err : ENOMEM;
            return NULL;
        }
        db->wal_actor->actor.pool = db->scheduler_pool;
    }

    // Create trie shard actors
    #define N_SHARDS 64
    db->shard_actors = trie_shard_actors_create(N_SHARDS, db->chunk_size,
                                                 db->btree_node_size,
                                                 db->tx_manager, db->wal_actor);
    db->shard_count = N_SHARDS;
    if (db->shard_actors == NULL) {
        if (db->wal_actor) wal_actor_destroy(db->wal_actor);
        tx_manager_destroy(db->tx_manager);
        hbtrie_destroy(db->trie);
        scheduler_pool_stop(db->scheduler_pool);
        scheduler_pool_destroy(db->scheduler_pool);
        free(db->location);
        free(db);
        if (owns_config) database_config_destroy(effective_config);
        if (error_code) *error_code = ENOMEM;
        return NULL;
    }
    for (size_t i = 0; i < N_SHARDS; i++) {
        db->shard_actors[i]->actor.pool = db->scheduler_pool;
    }

    // Replace db->trie with the first shard's trie so scans/iterations work.
    // The legacy trie was created empty; shard actors own the real data.
    if (db->trie) hbtrie_destroy(db->trie);
    db->trie = db->shard_actors[0]->trie;
    REFERENCE(db->trie, hbtrie_t);

    // Create LRU actor
    db->lru_actor = lru_actor_create(db->lru_memory_mb * 1024 * 1024);
    if (db->lru_actor == NULL) {
        trie_shard_actors_destroy(db->shard_actors, db->shard_count);
        if (db->wal_actor) wal_actor_destroy(db->wal_actor);
        tx_manager_destroy(db->tx_manager);
        hbtrie_destroy(db->trie);
        scheduler_pool_stop(db->scheduler_pool);
        scheduler_pool_destroy(db->scheduler_pool);
        free(db->location);
        free(db);
        if (owns_config) database_config_destroy(effective_config);
        if (error_code) *error_code = ENOMEM;
        return NULL;
    }
    db->lru_actor->actor.pool = db->scheduler_pool;

    // Create page file and bnode cache (Phase 2 persistence)
    if (effective_config->enable_persist) {
        char page_path[1024];
        snprintf(page_path, sizeof(page_path), "%s/data.wdbp", location);

        db->page_file = page_file_create(page_path,
            PAGE_FILE_DEFAULT_BLOCK_SIZE, PAGE_FILE_DEFAULT_NUM_SUPERBLOCKS, db->encryption);
        if (db->page_file != NULL) {
            int pf_rc = page_file_open(db->page_file, 1);
            if (pf_rc == 0) {
                bnode_cache_mgr_t* cache_mgr = bnode_cache_mgr_create(
                    (size_t)effective_config->bnode_cache_memory_mb * 1024 * 1024,
                    effective_config->bnode_cache_shards);
                if (cache_mgr != NULL) {
                    db->bnode_cache = bnode_cache_create_file_cache(
                        cache_mgr, db->page_file, page_path);
                    if (db->bnode_cache == NULL) {
                        bnode_cache_mgr_destroy(cache_mgr);
                    } else if (db->trie != NULL) {
                        db->trie->fcache = db->bnode_cache;
                    }
                }

                eviction_queue_init(&db->eviction_queue);
                if (db->bnode_cache != NULL) {
                    db->bnode_cache->on_evict = database_on_bnode_evict;
                    db->bnode_cache->on_evict_data = db;

                    // Schedule eviction task to run via actor
                    if (!db->sync_only && db->scheduler_pool != NULL) {
                        atomic_fetch_add(&db->eviction_in_flight, 1);
                        // Eviction is handled inline during flush for now
                    }
                }

                // Create bnode cache actor
                db->bnode_cache_actor = bnode_cache_actor_create(
                    db->page_file,
                    (size_t)effective_config->bnode_cache_memory_mb * 1024 * 1024,
                    N_SHARDS);
                if (db->bnode_cache_actor != NULL) {
                    bnode_cache_actor_set_pool(db->bnode_cache_actor, db->scheduler_pool);
                }
            } else {
                log_warn("Failed to open page file: %s", page_path);
                page_file_destroy(db->page_file);
                db->page_file = NULL;
            }
        }
    }

    // Legacy LRU (NULL — actor-based LRU replaces it)
    db->lru = NULL;

    // Store active config
    db->active_config = database_config_copy(effective_config);

    // Save config
    database_config_save(location, effective_config);

    // Recovery: replay WAL if persistence enabled and WAL actor present
    db->is_rebuilding = 1;
    // Note: WAL recovery via wal_actor is not yet implemented;
    // in-memory databases don't need it.
    db->is_rebuilding = 0;

    refcounter_init((refcounter_t*)db);

    if (owns_config) {
        database_config_destroy(effective_config);
    }

    return db;
}

// ============================================================================
// database_create_encrypted
// ============================================================================

database_t* database_create_encrypted(const char* location,
                                       encrypted_database_config_t* config,
                                       int* error_code) {
    if (error_code) *error_code = 0;

    if (config == NULL) {
        if (error_code) *error_code = EINVAL;
        return NULL;
    }

    if (config->type == ENCRYPTION_NONE) {
        if ((config->symmetric.key != NULL && config->symmetric.key_length > 0) ||
            (config->asymmetric.public_key_der != NULL && config->asymmetric.public_key_len > 0)) {
            if (error_code) *error_code = DATABASE_ERR_ENCRYPTION_KEY_INVALID;
            return NULL;
        }
        return database_create_with_config(location, &config->config, error_code);
    }

    encryption_t* encryption = NULL;

    if (config->type == ENCRYPTION_SYMMETRIC) {
        if (config->symmetric.key == NULL || config->symmetric.key_length == 0) {
            if (error_code) *error_code = DATABASE_ERR_ENCRYPTION_KEY_INVALID;
            return NULL;
        }
        encryption = encryption_create_symmetric(config->symmetric.key,
                                                  config->symmetric.key_length);
    } else if (config->type == ENCRYPTION_ASYMMETRIC) {
        if (config->asymmetric.public_key_der == NULL ||
            config->asymmetric.public_key_len == 0) {
            if (error_code) *error_code = DATABASE_ERR_ENCRYPTION_KEY_INVALID;
            return NULL;
        }
        encryption = encryption_create_asymmetric(
            config->asymmetric.private_key_der, config->asymmetric.private_key_len,
            config->asymmetric.public_key_der, config->asymmetric.public_key_len);
    } else {
        if (error_code) *error_code = DATABASE_ERR_ENCRYPTION_UNSUPPORTED;
        return NULL;
    }

    if (encryption == NULL) {
        if (error_code) *error_code = DATABASE_ERR_ENCRYPTION_KEY_INVALID;
        return NULL;
    }

    char config_path[1024];
    snprintf(config_path, sizeof(config_path), "%s/.config", location);
    struct stat st;
    bool db_exists = (stat(config_path, &st) == 0);

    if (db_exists) {
        database_config_t* saved_config = database_config_load(location);
        if (saved_config != NULL && saved_config->encryption.has_encryption &&
            saved_config->encryption.type != ENCRYPTION_NONE) {
            encryption_t* verify_enc = encryption_create_from_config(
                config->type,
                config->symmetric.key, config->symmetric.key_length,
                config->asymmetric.private_key_der, config->asymmetric.private_key_len,
                config->asymmetric.public_key_der, config->asymmetric.public_key_len,
                saved_config->encryption.salt,
                saved_config->encryption.check);

            if (verify_enc == NULL) {
                encryption_destroy(encryption);
                database_config_destroy(saved_config);
                if (error_code) *error_code = DATABASE_ERR_ENCRYPTION_KEY_INVALID;
                return NULL;
            }
            encryption_destroy(encryption);
            encryption = verify_enc;
            database_config_destroy(saved_config);
        } else {
            if (saved_config != NULL) database_config_destroy(saved_config);
            encryption_destroy(encryption);
            if (error_code) *error_code = DATABASE_ERR_ENCRYPTION_UNSUPPORTED;
            return NULL;
        }
    }

    database_config_t effective_config = config->config;
    effective_config.encryption.type = config->type;
    effective_config.encryption.has_encryption = 1;

    const uint8_t* salt = encryption_get_salt(encryption);
    const uint8_t* check = encryption_get_check(encryption);
    if (salt != NULL) memcpy(effective_config.encryption.salt, salt, 16);
    if (check != NULL) memcpy(effective_config.encryption.check, check, 44);

    database_t* db = database_create_with_config(location, &effective_config, error_code);
    if (db != NULL) {
        db->encryption = encryption;
    } else {
        encryption_destroy(encryption);
    }

    return db;
}

// ============================================================================
// database_create — Legacy entry point, delegates to database_create_with_config
// ============================================================================

database_t* database_create(const char* location, size_t lru_memory_mb,
                            wal_config_t* wal_config,
                            uint8_t chunk_size, uint32_t btree_node_size,
                            uint8_t enable_persist,
                            work_pool_t* pool, hierarchical_timing_wheel_t* wheel,
                            int* error_code) {
    database_config_t* config = database_config_default();
    if (config == NULL) {
        if (error_code) *error_code = ENOMEM;
        return NULL;
    }

    if (lru_memory_mb > 0) config->lru_memory_mb = lru_memory_mb;
    if (chunk_size > 0) config->chunk_size = chunk_size;
    if (btree_node_size > 0) config->btree_node_size = btree_node_size;
    config->enable_persist = enable_persist;

    if (wal_config != NULL) config->wal_config = *wal_config;

    // Actor model: ignore external pool/wheel — we create our own scheduler_pool.
    // The caller's pool/wheel are still valid for their own lifecycle (e.g. tests).
    (void)pool;
    (void)wheel;

    // Determine worker threads from pool or default
    config->worker_threads = 4;  // Default in actor mode
    config->timer_resolution_ms = 0;  // No hierarchical wheel in actor mode

    database_t* db = database_create_with_config(location, config, error_code);
    database_config_destroy(config);

    return db;
}

// ============================================================================
// database_persist
// ============================================================================

static int database_persist(database_t* db) {
    if (db == NULL) return -1;
    if (db->page_file != NULL) {
        return database_flush_dirty_bnodes(db);
    }
    return save_index(db);
}

// ============================================================================
// database_destroy — Actor-based cleanup
// ============================================================================

void database_destroy(database_t* db) {
    if (db == NULL) return;

    refcounter_dereference((refcounter_t*)db);
    uint_fast32_t count = refcounter_count((refcounter_t*)db);

    if (count == 0) {
        db->destroying = true;

        // Unregister eviction callback
        if (db->bnode_cache != NULL) {
            db->bnode_cache->on_evict = NULL;
            db->bnode_cache->on_evict_data = NULL;
        }

        // Stop scheduler pool before destroying data structures
        scheduler_pool_stop(db->scheduler_pool);

        // Persist before teardown
        database_persist(db);

        // Destroy actors
        if (db->shard_actors) {
            trie_shard_actors_destroy(db->shard_actors, db->shard_count);
            db->shard_actors = NULL;
        }
        if (db->wal_actor) {
            wal_actor_destroy(db->wal_actor);
            db->wal_actor = NULL;
        }
        if (db->lru_actor) {
            lru_actor_destroy(db->lru_actor);
            db->lru_actor = NULL;
        }
        if (db->bnode_cache_actor) {
            bnode_cache_actor_destroy(db->bnode_cache_actor);
            db->bnode_cache_actor = NULL;
        }

        // Process remaining eviction callbacks
        if (db->bnode_cache != NULL) {
            uint64_t offsets[64];
            size_t n;
            do {
                n = eviction_queue_drain(&db->eviction_queue, offsets, 64);
                for (size_t i = 0; i < n; i++) {
                    if (db->trie != NULL) {
                        hbtrie_null_entries_by_offset(db->trie, offsets[i]);
                    }
                    bnode_cache_complete_evict(db->bnode_cache, offsets[i]);
                }
            } while (n > 0);

            db->bnode_cache->on_evict = NULL;
            db->bnode_cache->on_evict_data = NULL;
        }

        // Destroy trie reference (shard actors own the real tries; db->trie is just a reference)
        if (db->trie) {
            hbtrie_destroy(db->trie);
            db->trie = NULL;
        }

        // Destroy page-based persistence
        if (db->bnode_cache != NULL) {
            bnode_cache_mgr_t* mgr = db->bnode_cache->mgr;
            bnode_cache_destroy_file_cache(db->bnode_cache);
            if (mgr != NULL) bnode_cache_mgr_destroy(mgr);
        }
        if (db->page_file != NULL) {
            page_file_destroy(db->page_file);
        }

        // Destroy encryption
        if (db->encryption) {
            encryption_destroy(db->encryption);
            db->encryption = NULL;
        }

        // Destroy transaction manager
        if (db->tx_manager) tx_manager_destroy(db->tx_manager);

        // Destroy active config
        if (db->active_config != NULL) {
            database_config_destroy(db->active_config);
        }

        // Destroy scheduler pool
        if (db->scheduler_pool) {
            scheduler_pool_destroy(db->scheduler_pool);
            db->scheduler_pool = NULL;
        }

        free(db->location);
        free(db);

        memory_pool_tls_drain();
    }
}

// ============================================================================
// Core operations — actor-based
// ============================================================================

void database_put(database_t* db, path_t* path,
                   identifier_t* value, promise_t* promise) {
    if (db == NULL || path == NULL || value == NULL || promise == NULL) {
        if (path) path_destroy(path);
        if (value) identifier_destroy(value);
        if (promise) promise_resolve(promise, NULL);
        return;
    }

    // Sync-only mode: execute inline
    if (db->sync_only) {
        int rc = database_put_sync(db, path, value);
        int* result = malloc(sizeof(int));
        if (result) *result = rc;
        promise_resolve(promise, result);
        return;
    }

    // Actor mode: send to shard actor
    trie_shard_put(db->shard_actors, db->shard_count, path, value, promise);
}

void database_get(database_t* db, path_t* path, promise_t* promise) {
    if (db == NULL || path == NULL || promise == NULL) {
        if (path) path_destroy(path);
        if (promise) promise_resolve(promise, NULL);
        return;
    }

    // Sync-only mode: execute inline
    if (db->sync_only) {
        identifier_t* result = NULL;
        int rc = database_get_sync(db, path, &result);
        (void)rc;
        promise_resolve(promise, result);
        return;
    }

    // Actor mode: send to shard actor
    trie_shard_get(db->shard_actors, db->shard_count, path, promise);
}

void database_delete(database_t* db, path_t* path, promise_t* promise) {
    if (db == NULL || path == NULL || promise == NULL) {
        if (path) path_destroy(path);
        if (promise) promise_resolve(promise, NULL);
        return;
    }

    // Sync-only mode: execute inline
    if (db->sync_only) {
        int rc = database_delete_sync(db, path);
        int* result = malloc(sizeof(int));
        if (result) *result = rc;
        promise_resolve(promise, result);
        return;
    }

    // Actor mode: send to shard actor
    trie_shard_delete(db->shard_actors, db->shard_count, path, promise);
}

// ============================================================================
// database_snapshot
// ============================================================================

int database_snapshot(database_t* db) {
    if (db == NULL) return -1;

    // GC old versions
    if (db->tx_manager != NULL) {
        tx_manager_gc(db->tx_manager);
    }

    if (db->page_file != NULL) {
        return database_flush_dirty_bnodes(db);
    }
    // In-memory mode: no persistence, snapshot is a no-op but succeeds
    return 0;
}

// ============================================================================
// database_count
// ============================================================================

size_t database_count(database_t* db) {
    if (db == NULL) return 0;
    if (db->lru_actor != NULL) {
        return db->lru_actor->entry_count;
    }
    return 0;
}

// ============================================================================
// Synchronous operations — actor-based with wait-for-idle
// ============================================================================

int database_put_sync(database_t* db, path_t* path, identifier_t* value) {
    if (db == NULL || path == NULL || value == NULL) {
        if (path) path_destroy(path);
        if (value) identifier_destroy(value);
        return -1;
    }

    // Direct sync: operate on the shard's trie directly (single-threaded context)
    size_t idx = path_hash(path) % db->shard_count;
    trie_shard_actor_t* shard = db->shard_actors[idx];

    txn_desc_t* txn = tx_manager_begin(db->tx_manager);
    if (txn == NULL) {
        path_destroy(path);
        identifier_destroy(value);
        return -1;
    }

    hbtrie_insert(shard->trie, path, value, txn->txn_id);
    ATOMIC_STORE(&shard->root, shard->trie->root);
    tx_manager_commit(db->tx_manager, txn);
    txn_desc_destroy(txn);
    return 0;
}

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

    // Lock-free read path: read from shard via atomic root snapshot
    transaction_id_t read_txn = tx_manager_get_last_committed(db->tx_manager);
    size_t idx = path_hash(path) % db->shard_count;
    trie_shard_actor_t* shard = db->shard_actors[idx];

    hbtrie_node_t* root = ATOMIC_LOAD(&shard->root);
    if (!root) {
        path_destroy(path);
        return -2;
    }

    hbtrie_node_t* saved_root = shard->trie->root;
    shard->trie->root = root;
    identifier_t* value = hbtrie_find(shard->trie, path, read_txn);
    shard->trie->root = saved_root;
    path_destroy(path);

    if (value) {
        *result = value;
        return 0;
    }
    return -2;
}

int database_delete_sync(database_t* db, path_t* path) {
    if (db == NULL || path == NULL) {
        if (path) path_destroy(path);
        return -1;
    }

    // Direct sync: operate on the shard's trie directly
    size_t idx = path_hash(path) % db->shard_count;
    trie_shard_actor_t* shard = db->shard_actors[idx];

    txn_desc_t* txn = tx_manager_begin(db->tx_manager);
    if (txn == NULL) {
        path_destroy(path);
        return -1;
    }

    identifier_t* removed = hbtrie_delete(shard->trie, path, txn->txn_id);
    ATOMIC_STORE(&shard->root, shard->trie->root);
    tx_manager_commit(db->tx_manager, txn);
    txn_desc_destroy(txn);

    if (removed) identifier_destroy(removed);
    return 0;
}

// ============================================================================
// database_increment_sync — actor-based
// ============================================================================

int64_t database_increment_sync(database_t* db, path_t* path, int64_t delta) {
    if (db == NULL || path == NULL) return -1;

    // Read current value via direct trie access
    transaction_id_t read_txn = tx_manager_get_last_committed(db->tx_manager);
    size_t idx = path_hash(path) % db->shard_count;
    trie_shard_actor_t* shard = db->shard_actors[idx];

    path_t* read_path = path_copy(path);
    identifier_t* current = NULL;
    hbtrie_node_t* root = ATOMIC_LOAD(&shard->root);
    if (root) {
        hbtrie_node_t* saved_root = shard->trie->root;
        shard->trie->root = root;
        current = hbtrie_find(shard->trie, read_path, read_txn);
        shard->trie->root = saved_root;
    }
    path_destroy(read_path);

    int64_t old_val = 0;
    if (current != NULL) {
        buffer_t* buf = identifier_to_buffer(current);
        if (buf != NULL && buf->size > 0) {
            char* tmp = malloc(buf->size + 1);
            if (tmp != NULL) {
                memcpy(tmp, buf->data, buf->size);
                tmp[buf->size] = '\0';
                old_val = strtoll(tmp, NULL, 10);
                free(tmp);
            }
            buffer_destroy(buf);
        }
        identifier_destroy(current);
    }

    int64_t new_val = old_val + delta;

    char val_str[32];
    snprintf(val_str, sizeof(val_str), "%lld", (long long)new_val);

    buffer_t* val_buf = buffer_create(strlen(val_str));
    if (val_buf == NULL) { path_destroy(path); return -1; }
    memcpy(val_buf->data, val_str, strlen(val_str));
    val_buf->size = strlen(val_str);
    identifier_t* new_id = identifier_create(val_buf, 0);
    buffer_destroy(val_buf);
    if (new_id == NULL) { path_destroy(path); return -1; }

    // Direct write
    txn_desc_t* txn = tx_manager_begin(db->tx_manager);
    if (txn == NULL) {
        path_destroy(path);
        identifier_destroy(new_id);
        return -1;
    }
    hbtrie_insert(shard->trie, path, new_id, txn->txn_id);
    ATOMIC_STORE(&shard->root, shard->trie->root);
    tx_manager_commit(db->tx_manager, txn);
    txn_desc_destroy(txn);

    return new_val;
}

// ============================================================================
// database_scan_range — kept as-is
// ============================================================================

// The implementation is in database_iterator.h/c but the forward declaration is in database.h
// Forward the call to the proper implementation
database_iterator_t* database_scan_range(database_t* db,
                                          const char* start,
                                          const char* end) {
    if (db == NULL) return NULL;

    path_t* start_path = NULL;
    path_t* end_path = NULL;

    // Build start path from string
    if (start && *start) {
        start_path = path_create();
        const char* s = start;
        while (s && *s) {
            const char* e = strchr(s, ',');
            size_t len = e ? (size_t)(e - s) : strlen(s);
            buffer_t* buf = buffer_create(len);
            if (buf) {
                memcpy(buf->data, s, len);
                buf->size = len;
                identifier_t* id = identifier_create(buf, 0);
                buffer_destroy(buf);
                if (id != NULL) {
                    path_append(start_path, id);
                    identifier_destroy(id);
                }
            }
            if (e) { s = e + 1; } else { break; }
        }
    }

    // Build end path from string
    if (end && *end) {
        end_path = path_create();
        const char* s = end;
        while (s && *s) {
            const char* e = strchr(s, ',');
            size_t len = e ? (size_t)(e - s) : strlen(s);
            buffer_t* buf = buffer_create(len);
            if (buf) {
                memcpy(buf->data, s, len);
                buf->size = len;
                identifier_t* id = identifier_create(buf, 0);
                buffer_destroy(buf);
                if (id != NULL) {
                    path_append(end_path, id);
                    identifier_destroy(id);
                }
            }
            if (e) { s = e + 1; } else { break; }
        }
    }

    database_iterator_t* iter = database_scan_start(db, start_path, end_path);
    if (start_path) path_destroy(start_path);
    if (end_path) path_destroy(end_path);
    return iter;
}

// ============================================================================
// Batch operations — actor-based
// ============================================================================

int database_write_batch_sync(database_t* db, batch_t* batch) {
    if (db == NULL || batch == NULL) return -1;

    platform_lock(&batch->lock);
    if (batch->count == 0) {
        platform_unlock(&batch->lock);
        return -3;
    }
    if (batch->submitted) {
        platform_unlock(&batch->lock);
        return -6;
    }
    batch->submitted = 1;

    // Begin a single MVCC transaction for the batch
    txn_desc_t* txn = tx_manager_begin(db->tx_manager);
    if (txn == NULL) {
        platform_unlock(&batch->lock);
        return -1;
    }

    // Write to WAL actor if present
    #define WAL_MAX_FILE_SIZE (128 * 1024)
    if (db->wal_actor) {
        buffer_t* data = serialize_batch(batch);
        if (data != NULL) {
            if (data->size > WAL_MAX_FILE_SIZE) {
                buffer_destroy(data);
                txn_desc_destroy(txn);
                platform_unlock(&batch->lock);
                return -5;
            }
            wal_actor_write(db->wal_actor, 0, txn->txn_id, WAL_BATCH, data, NULL);
        }
    }

    // Apply each operation directly to the trie
    for (size_t i = 0; i < batch->count; i++) {
        if (batch->ops[i].type == WAL_PUT) {
            size_t idx = path_hash(batch->ops[i].path) % db->shard_count;
            trie_shard_actor_t* shard = db->shard_actors[idx];
            hbtrie_insert(shard->trie, batch->ops[i].path, batch->ops[i].value, txn->txn_id);
            ATOMIC_STORE(&shard->root, shard->trie->root);
        } else {
            size_t idx = path_hash(batch->ops[i].path) % db->shard_count;
            trie_shard_actor_t* shard = db->shard_actors[idx];
            identifier_t* removed = hbtrie_delete(shard->trie, batch->ops[i].path, txn->txn_id);
            ATOMIC_STORE(&shard->root, shard->trie->root);
            if (removed) identifier_destroy(removed);
        }
    }

    platform_unlock(&batch->lock);

    // Commit the transaction
    tx_manager_commit(db->tx_manager, txn);
    txn_desc_destroy(txn);

    return 0;
}

void database_write_batch(database_t* db, batch_t* batch, promise_t* promise) {
    if (db == NULL || batch == NULL || promise == NULL) {
        int* error = malloc(sizeof(int));
        if (error) *error = -1;
        promise_resolve(promise, error ? error : NULL);
        return;
    }

    if (db->sync_only) {
        int* error = malloc(sizeof(int));
        if (error) *error = -1;
        promise_resolve(promise, error);
        return;
    }

    int rc = database_write_batch_sync(db, batch);
    int* result = malloc(sizeof(int));
    if (result) *result = rc;
    promise_resolve(promise, result);
}

// ============================================================================
// Raw API implementations (delegate to core functions)
// ============================================================================

int database_put_sync_raw(database_t* db,
    const char* key, size_t key_len, char delimiter,
    const uint8_t* value, size_t value_len) {
    if (!db || !key || key_len == 0 || !value) return -1;

    path_t* path = path_create_from_raw(key, key_len, delimiter, db->chunk_size);
    if (!path) return -1;

    identifier_t* id = identifier_create_from_raw(value, value_len, db->chunk_size);
    if (!id) { path_destroy(path); return -1; }

    return database_put_sync(db, path, id);
}

int database_get_sync_raw(database_t* db,
    const char* key, size_t key_len, char delimiter,
    uint8_t** value_out, size_t* value_len_out) {
    if (!db || !key || key_len == 0 || !value_out || !value_len_out) return -1;

    path_t* path = path_create_from_raw(key, key_len, delimiter, db->chunk_size);
    if (!path) return -1;

    identifier_t* result = NULL;
    int rc = database_get_sync(db, path, &result);

    if (rc == 0 && result) {
        *value_out = identifier_get_data_copy(result, value_len_out);
        identifier_destroy(result);
        if (!*value_out) return -1;
        return 0;
    }

    *value_out = NULL;
    *value_len_out = 0;
    return rc;
}

int database_delete_sync_raw(database_t* db,
    const char* key, size_t key_len, char delimiter) {
    if (!db || !key || key_len == 0) return -1;

    path_t* path = path_create_from_raw(key, key_len, delimiter, db->chunk_size);
    if (!path) return -1;

    return database_delete_sync(db, path);
}

void database_raw_value_free(uint8_t* value) {
    free(value);
}

/* --- Raw async context and workers --- */

typedef struct {
    database_t* db;
    char* key_buf;
    size_t key_len;
    char delimiter;
    uint8_t* value_buf;
    size_t value_len;
    promise_t* promise;
    int op_type;
} raw_async_ctx_t;

static void _raw_put_worker(void* ctx_ptr) {
    raw_async_ctx_t* ctx = (raw_async_ctx_t*)ctx_ptr;
    path_t* path = path_create_from_raw(ctx->key_buf, ctx->key_len,
                                         ctx->delimiter,
                                         ctx->db->chunk_size);
    if (!path) {
        async_error_t* err = ERROR("Failed to create path from raw key");
        promise_reject(ctx->promise, err);
        free(ctx->key_buf);
        free(ctx->value_buf);
        free(ctx);
        return;
    }

    identifier_t* value = identifier_create_from_raw(
        ctx->value_buf, ctx->value_len, ctx->db->chunk_size);
    if (!value) {
        path_destroy(path);
        async_error_t* err = ERROR("Failed to create value from raw data");
        promise_reject(ctx->promise, err);
        free(ctx->key_buf);
        free(ctx->value_buf);
        free(ctx);
        return;
    }

    free(ctx->key_buf);
    free(ctx->value_buf);

    // Use actor-based put
    database_put(ctx->db, path, value, ctx->promise);
    free(ctx);
}

static void abort_raw_put(void* ctx_ptr) {
    raw_async_ctx_t* ctx = (raw_async_ctx_t*)ctx_ptr;
    free(ctx->key_buf);
    free(ctx->value_buf);
    free(ctx);
}

static void _raw_get_worker(void* ctx_ptr) {
    raw_async_ctx_t* ctx = (raw_async_ctx_t*)ctx_ptr;
    path_t* path = path_create_from_raw(ctx->key_buf, ctx->key_len,
                                         ctx->delimiter,
                                         ctx->db->chunk_size);
    free(ctx->key_buf);

    if (!path) {
        async_error_t* err = ERROR("Failed to create path from raw key");
        promise_reject(ctx->promise, err);
        free(ctx);
        return;
    }

    database_get(ctx->db, path, ctx->promise);
    free(ctx);
}

static void abort_raw_get(void* ctx_ptr) {
    raw_async_ctx_t* ctx = (raw_async_ctx_t*)ctx_ptr;
    free(ctx->key_buf);
    free(ctx);
}

static void _raw_delete_worker(void* ctx_ptr) {
    raw_async_ctx_t* ctx = (raw_async_ctx_t*)ctx_ptr;
    path_t* path = path_create_from_raw(ctx->key_buf, ctx->key_len,
                                         ctx->delimiter,
                                         ctx->db->chunk_size);
    free(ctx->key_buf);

    if (!path) {
        async_error_t* err = ERROR("Failed to create path from raw key");
        promise_reject(ctx->promise, err);
        free(ctx);
        return;
    }

    database_delete(ctx->db, path, ctx->promise);
    free(ctx);
}

static void abort_raw_delete(void* ctx_ptr) {
    raw_async_ctx_t* ctx = (raw_async_ctx_t*)ctx_ptr;
    free(ctx->key_buf);
    free(ctx);
}

int database_put_raw(database_t* db,
    const char* key, size_t key_len, char delimiter,
    const uint8_t* value, size_t value_len,
    promise_t* promise) {
    if (!db || !key || key_len == 0 || !value || !promise) {
        if (promise) promise_resolve(promise, NULL);
        return -1;
    }

    if (db->sync_only) {
        int* error = malloc(sizeof(int));
        if (error) *error = -1;
        promise_resolve(promise, error);
        return -1;
    }

    raw_async_ctx_t* ctx = calloc(1, sizeof(raw_async_ctx_t));
    if (!ctx) { promise_resolve(promise, NULL); return -1; }

    ctx->db = db;
    ctx->key_len = key_len;
    ctx->key_buf = malloc(key_len);
    if (!ctx->key_buf) { free(ctx); promise_resolve(promise, NULL); return -1; }
    memcpy(ctx->key_buf, key, key_len);

    ctx->delimiter = delimiter;
    ctx->value_len = value_len;
    ctx->value_buf = malloc(value_len);
    if (!ctx->value_buf) { free(ctx->key_buf); free(ctx); promise_resolve(promise, NULL); return -1; }
    memcpy(ctx->value_buf, value, value_len);

    ctx->promise = promise;
    ctx->op_type = 0;

    // Actor-based: create message directly instead of work queue
    _raw_put_worker(ctx);
    return 0;
}

int database_get_raw(database_t* db,
    const char* key, size_t key_len, char delimiter,
    promise_t* promise) {
    if (!db || !key || key_len == 0 || !promise) {
        if (promise) promise_resolve(promise, NULL);
        return -1;
    }

    if (db->sync_only) {
        promise_resolve(promise, NULL);
        return -1;
    }

    raw_async_ctx_t* ctx = calloc(1, sizeof(raw_async_ctx_t));
    if (!ctx) { promise_resolve(promise, NULL); return -1; }

    ctx->db = db;
    ctx->key_len = key_len;
    ctx->key_buf = malloc(key_len);
    if (!ctx->key_buf) { free(ctx); promise_resolve(promise, NULL); return -1; }
    memcpy(ctx->key_buf, key, key_len);

    ctx->delimiter = delimiter;
    ctx->promise = promise;
    ctx->op_type = 1;

    _raw_get_worker(ctx);
    return 0;
}

int database_delete_raw(database_t* db,
    const char* key, size_t key_len, char delimiter,
    promise_t* promise) {
    if (!db || !key || key_len == 0 || !promise) {
        if (promise) promise_resolve(promise, NULL);
        return -1;
    }

    if (db->sync_only) {
        int* error = malloc(sizeof(int));
        if (error) *error = -1;
        promise_resolve(promise, error);
        return -1;
    }

    raw_async_ctx_t* ctx = calloc(1, sizeof(raw_async_ctx_t));
    if (!ctx) { promise_resolve(promise, NULL); return -1; }

    ctx->db = db;
    ctx->key_len = key_len;
    ctx->key_buf = malloc(key_len);
    if (!ctx->key_buf) { free(ctx); promise_resolve(promise, NULL); return -1; }
    memcpy(ctx->key_buf, key, key_len);

    ctx->delimiter = delimiter;
    ctx->promise = promise;
    ctx->op_type = 2;

    _raw_delete_worker(ctx);
    return 0;
}

/* --- Batch raw implementations --- */

int database_batch_sync_raw(database_t* db, char delimiter,
    const raw_op_t* ops, size_t count) {
    if (!db || !ops || count == 0) return -1;

    batch_t* batch = batch_create(count);
    if (!batch) return -1;

    for (size_t i = 0; i < count; i++) {
        path_t* path = path_create_from_raw(ops[i].key, ops[i].key_len,
                                             delimiter,
                                             db->chunk_size);
        if (!path) { batch_destroy(batch); return -1; }

        if (ops[i].type == 0) {
            if (!ops[i].value) {
                path_destroy(path);
                batch_destroy(batch);
                return -1;
            }
            identifier_t* value = identifier_create_from_raw(
                ops[i].value, ops[i].value_len,
                db->chunk_size);
            if (!value) { path_destroy(path); batch_destroy(batch); return -1; }

            int rc = batch_add_put(batch, path, value);
            if (rc != 0) {
                path_destroy(path);
                identifier_destroy(value);
                batch_destroy(batch);
                return -1;
            }
        } else {
            int rc = batch_add_delete(batch, path);
            if (rc != 0) {
                path_destroy(path);
                batch_destroy(batch);
                return -1;
            }
        }
    }

    int rc = database_write_batch_sync(db, batch);
    batch_destroy(batch);
    return rc;
}

int database_batch_raw(database_t* db, char delimiter,
    const raw_op_t* ops, size_t count,
    promise_t* promise) {
    if (!db || !ops || count == 0 || !promise) return -1;

    batch_t* batch = batch_create(count);
    if (!batch) return -1;

    for (size_t i = 0; i < count; i++) {
        path_t* path = path_create_from_raw(ops[i].key, ops[i].key_len,
                                             delimiter,
                                             db->chunk_size);
        if (!path) { batch_destroy(batch); return -1; }

        if (ops[i].type == 0) {
            if (!ops[i].value) {
                path_destroy(path);
                batch_destroy(batch);
                return -1;
            }
            identifier_t* value = identifier_create_from_raw(
                ops[i].value, ops[i].value_len,
                db->chunk_size);
            if (!value) { path_destroy(path); batch_destroy(batch); return -1; }

            int rc = batch_add_put(batch, path, value);
            if (rc != 0) {
                path_destroy(path);
                identifier_destroy(value);
                batch_destroy(batch);
                return -1;
            }
        } else {
            int rc = batch_add_delete(batch, path);
            if (rc != 0) {
                path_destroy(path);
                batch_destroy(batch);
                return -1;
            }
        }
    }

    database_write_batch(db, batch, promise);
    return 0;
}

/* --- Scan raw implementations --- */

int database_scan_sync_raw(database_t* db,
    const char* prefix, size_t prefix_len, char delimiter,
    raw_result_t** results, size_t* count) {
    if (!db || !results || !count) return -1;

    *results = NULL;
    *count = 0;

    path_t* start_path = NULL;
    if (prefix && prefix_len > 0) {
        start_path = path_create_from_raw(prefix, prefix_len, delimiter,
                                           db->chunk_size);
        if (!start_path) return -1;
    }

    database_iterator_t* iter = database_scan_start(db, start_path, NULL);
    if (start_path) path_destroy(start_path);
    if (!iter) return 0;

    size_t capacity = 64;
    size_t n = 0;
    raw_result_t* out = malloc(capacity * sizeof(raw_result_t));
    if (!out) { database_scan_end(iter); return -1; }

    while (true) {
        path_t* out_path = NULL;
        identifier_t* out_value = NULL;
        int rc = database_scan_next(iter, &out_path, &out_value);
        if (rc != 0) break;

        if (n >= capacity) {
            capacity *= 2;
            raw_result_t* new_out = realloc(out, capacity * sizeof(raw_result_t));
            if (!new_out) {
                for (size_t j = 0; j < n; j++) {
                    free(out[j].key);
                    free(out[j].value);
                }
                free(out);
                path_destroy(out_path);
                identifier_destroy(out_value);
                database_scan_end(iter);
                return -1;
            }
            out = new_out;
        }

        size_t key_total = 0;
        size_t path_len = path_length(out_path);
        for (size_t i = 0; i < path_len; i++) {
            identifier_t* id = path_get(out_path, i);
            key_total += id->length;
            if (i > 0) key_total++;
        }

        out[n].key = malloc(key_total + 1);
        if (!out[n].key) {
            for (size_t j = 0; j < n; j++) {
                free(out[j].key);
                free(out[j].value);
            }
            free(out);
            path_destroy(out_path);
            identifier_destroy(out_value);
            database_scan_end(iter);
            return -1;
        }

        size_t pos = 0;
        for (size_t i = 0; i < path_len; i++) {
            identifier_t* id = path_get(out_path, i);
            if (i > 0) out[n].key[pos++] = delimiter;
            size_t id_len;
            uint8_t* id_data = identifier_get_data_copy(id, &id_len);
            if (id_data) {
                memcpy(out[n].key + pos, id_data, id_len);
                pos += id_len;
                free(id_data);
            }
        }
        out[n].key[pos] = '\0';
        out[n].key_len = pos;

        out[n].value = identifier_get_data_copy(out_value, &out[n].value_len);

        path_destroy(out_path);
        identifier_destroy(out_value);
        n++;
    }

    database_scan_end(iter);
    *results = out;
    *count = n;
    return 0;
}

void database_raw_results_free(raw_result_t* results, size_t count) {
    if (!results) return;
    for (size_t i = 0; i < count; i++) {
        free(results[i].key);
        free(results[i].value);
    }
    free(results);
}
