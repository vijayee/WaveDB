//
// Created by victor on 3/11/26.
//

#include "database.h"
#include "database_config.h"
#include "wal_manager.h"
#include "batch.h"
#include "../Util/allocator.h"
#include "../Util/mkdir_p.h"
#include "../Util/path_join.h"
#include "../Util/log.h"
#include "../Storage/page_file.h"
#include "../Storage/bnode_cache.h"
#include "../Storage/node_serializer.h"
#include "../Util/memory_pool.h"
#include <cbor.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include <arpa/inet.h>

typedef struct {
    database_t* db;
    path_t* path;
    identifier_t* value;
    promise_t* promise;
} database_put_ctx_t;

typedef struct {
    database_t* db;
    path_t* path;
    promise_t* promise;
} database_get_ctx_t;

typedef struct {
    database_t* db;
    path_t* path;
    promise_t* promise;
} database_delete_ctx_t;

typedef struct {
    database_t* db;
    batch_t* batch;
    promise_t* promise;
} batch_work_t;

// Forward declarations
static void _database_put(database_put_ctx_t* ctx);
static void _database_get(database_get_ctx_t* ctx);
static void _database_delete(database_delete_ctx_t* ctx);

// Abort functions for cleanup when work is not executed
static void abort_database_put(void* ctx) {
    database_put_ctx_t* put_ctx = (database_put_ctx_t*)ctx;
    if (put_ctx->path) path_destroy(put_ctx->path);
    if (put_ctx->value) identifier_destroy(put_ctx->value);
    free(put_ctx);
}

static void abort_database_get(void* ctx) {
    database_get_ctx_t* get_ctx = (database_get_ctx_t*)ctx;
    if (get_ctx->path) path_destroy(get_ctx->path);
    free(get_ctx);
}

static void abort_database_delete(void* ctx) {
    database_delete_ctx_t* del_ctx = (database_delete_ctx_t*)ctx;
    if (del_ctx->path) path_destroy(del_ctx->path);
    free(del_ctx);
}

// Helper to encode path+value for WAL
static buffer_t* encode_put_entry(path_t* path, identifier_t* value) {
    cbor_item_t* array = cbor_new_definite_array(2);
    if (array == NULL) return NULL;

    cbor_item_t* path_cbor = path_to_cbor(path);
    if (path_cbor == NULL) {
        cbor_decref(&array);
        return NULL;
    }
    cbor_array_push(array, path_cbor);
    cbor_decref(&path_cbor);

    cbor_item_t* value_cbor = identifier_to_cbor(value);
    if (value_cbor == NULL) {
        cbor_decref(&array);
        return NULL;
    }
    cbor_array_push(array, value_cbor);
    cbor_decref(&value_cbor);

    unsigned char* buf = NULL;
    size_t buf_size = 0;
    cbor_serialize_alloc(array, &buf, &buf_size);
    cbor_decref(&array);

    if (buf == NULL) return NULL;

    buffer_t* buffer = buffer_create_from_existing_memory(buf, buf_size);
    return buffer;
}

// Helper to encode path for WAL
static buffer_t* encode_delete_entry(path_t* path) {
    cbor_item_t* array = cbor_new_definite_array(1);
    if (array == NULL) return NULL;

    cbor_item_t* path_cbor = path_to_cbor(path);
    if (path_cbor == NULL) {
        cbor_decref(&array);
        return NULL;
    }
    cbor_array_push(array, path_cbor);
    cbor_decref(&path_cbor);

    unsigned char* buf = NULL;
    size_t buf_size = 0;
    cbor_serialize_alloc(array, &buf, &buf_size);
    cbor_decref(&array);

    if (buf == NULL) return NULL;

    buffer_t* buffer = buffer_create_from_existing_memory(buf, buf_size);
    return buffer;
}

// Helper to serialize batch for WAL
static buffer_t* serialize_batch(batch_t* batch) {
    if (batch == NULL || batch->count == 0) {
        return NULL;
    }

    // Create CBOR array: [ [type, path, value?], ... ]
    cbor_item_t* batch_array = cbor_new_definite_array(batch->count);
    if (batch_array == NULL) {
        return NULL;
    }

    // Lock batch for reading
    platform_lock(&batch->lock);

    // Serialize each operation
    for (size_t i = 0; i < batch->count; i++) {
        batch_op_t* op = &batch->ops[i];

        // Create operation entry: [type, path, value?]
        cbor_item_t* op_array = cbor_new_definite_array(op->type == WAL_PUT ? 3 : 2);
        if (op_array == NULL) {
            platform_unlock(&batch->lock);
            cbor_decref(&batch_array);
            return NULL;
        }

        // Add type
        cbor_item_t* type_item = cbor_build_uint8(op->type);
        if (type_item == NULL) {
            platform_unlock(&batch->lock);
            cbor_decref(&op_array);
            cbor_decref(&batch_array);
            return NULL;
        }
        cbor_array_push(op_array, type_item);
        cbor_decref(&type_item);

        // Add path
        cbor_item_t* path_cbor = path_to_cbor(op->path);
        if (path_cbor == NULL) {
            platform_unlock(&batch->lock);
            cbor_decref(&op_array);
            cbor_decref(&batch_array);
            return NULL;
        }
        cbor_array_push(op_array, path_cbor);
        cbor_decref(&path_cbor);

        // Add value for PUT operations
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

    // Serialize to buffer
    unsigned char* buf = NULL;
    size_t buf_size = 0;
    cbor_serialize_alloc(batch_array, &buf, &buf_size);
    cbor_decref(&batch_array);

    if (buf == NULL) {
        return NULL;
    }

    buffer_t* buffer = buffer_create_from_existing_memory(buf, buf_size);
    return buffer;
}

// Create index file path
static char* create_index_path(const char* location, uint64_t id, uint32_t crc) {
    char buf[256];
    snprintf(buf, sizeof(buf), "%s/index_%lu_%08x.cbor", location, (unsigned long)id, crc);
    return strdup(buf);
}

// Save HBTrie to disk
static int save_index(database_t* db) {
    uint32_t crc = hbtrie_compute_hash(db->trie);
    uint64_t index_id = db->next_index_id++;

    char* index_file = create_index_path(db->location, index_id, crc);
    if (index_file == NULL) {
        log_error("Failed to create index path");
        return -1;
    }

    uint8_t* buf = NULL;
    size_t len = 0;
    if (hbtrie_serialize(db->trie, &buf, &len) != 0) {
        log_error("Failed to serialize HBTrie");
        free(index_file);
        return -1;
    }

    int fd = open(index_file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        log_error("Failed to open index file: %s", index_file);
        free(buf);
        free(index_file);
        return -1;
    }

    ssize_t written = write(fd, buf, len);
    close(fd);
    free(buf);
    free(index_file);

    if (written != (ssize_t)len) {
        log_error("Failed to write index file: wrote %zd of %zu bytes", written, len);
        return -1;
    }

    log_info("Saved index file successfully");
    return 0;
}

// Load HBTrie from page file
static hbtrie_t* load_index_from_page_file(const char* location, uint8_t chunk_size,
                                            uint32_t btree_node_size,
                                            page_file_t* page_file,
                                            file_bnode_cache_t* bnode_cache) {
    page_superblock_t sb;
    int rc = page_file_read_superblock(page_file, &sb);
    if (rc != 0 || sb.root_offset == 0) return NULL;

    size_t root_len = 0;
    uint8_t* root_data = page_file_read_node(page_file, sb.root_offset, &root_len);
    if (root_data == NULL) return NULL;

    node_location_t* locations = NULL;
    size_t num_locations = 0;
    bnode_t* root_bnode = bnode_deserialize(root_data, root_len, chunk_size,
                                              btree_node_size, &locations, &num_locations);
    free(root_data);
    if (root_bnode == NULL) {
        free(locations);
        return NULL;
    }

    // Set child_disk_offset on entries from locations
    for (size_t i = 0; i < num_locations && i < root_bnode->entries.length; i++) {
        root_bnode->entries.data[i].child_disk_offset = locations[i].offset;
    }
    root_bnode->disk_offset = sb.root_offset;
    root_bnode->is_dirty = 0;
    free(locations);

    hbtrie_node_t* root_hbnode = hbtrie_node_create(btree_node_size);
    if (root_hbnode == NULL) {
        bnode_destroy_tree(root_bnode);
        return NULL;
    }
    bnode_destroy(root_hbnode->btree);
    root_hbnode->btree = root_bnode;
    root_hbnode->btree_height = atomic_load(&root_bnode->level);
    root_hbnode->disk_offset = sb.root_offset;
    root_hbnode->is_loaded = 1;
    root_hbnode->is_dirty = 0;

    hbtrie_t* trie = hbtrie_create(chunk_size, btree_node_size);
    if (trie == NULL) {
        hbtrie_node_destroy(root_hbnode);
        return NULL;
    }
    trie->fcache = bnode_cache;

    hbtrie_node_t* old_root = atomic_load(&trie->root);
    atomic_store(&trie->root, root_hbnode);
    if (old_root != NULL) hbtrie_node_destroy(old_root);

    return trie;
}

// Flush and persist all dirty bnodes to the page file
int database_flush_persist(database_t* db) {
    if (db == NULL || db->page_file == NULL || db->trie == NULL) return -1;

    hbtrie_node_t* root = atomic_load(&db->trie->root);
    if (root == NULL) return 0;

    // Collect all hbtrie_nodes (children before parents)
    vec_t(hbtrie_node_t*) node_stack;
    vec_init(&node_stack);
    vec_push(&node_stack, root);

    vec_t(hbtrie_node_t*) all_nodes;
    vec_init(&all_nodes);

    while (node_stack.length > 0) {
        hbtrie_node_t* current = vec_pop(&node_stack);
        vec_push(&all_nodes, current);

        if (current->btree == NULL) continue;

        // Walk bnode tree to find child hbtrie_nodes
        vec_t(bnode_t*) bnode_stack;
        vec_init(&bnode_stack);
        vec_push(&bnode_stack, current->btree);

        while (bnode_stack.length > 0) {
            bnode_t* bn = vec_pop(&bnode_stack);
            for (size_t j = 0; j < bnode_count(bn); j++) {
                bnode_entry_t* entry = bnode_get(bn, j);
                if (entry == NULL) continue;

                if (entry->is_bnode_child && entry->child_bnode != NULL) {
                    vec_push(&bnode_stack, entry->child_bnode);
                } else if (!entry->has_value && entry->child != NULL) {
                    vec_push(&node_stack, entry->child);
                }
            }
        }
        vec_deinit(&bnode_stack);
    }
    vec_deinit(&node_stack);

    // Process nodes in reverse order (children before parents)
    for (int i = all_nodes.length - 1; i >= 0; i--) {
        hbtrie_node_t* node = all_nodes.data[i];

        // Serialize the bnode tree
        uint8_t* buf = NULL;
        size_t len = 0;
        if (bnode_serialize(node->btree, db->trie->chunk_size, &buf, &len) != 0) {
            free(buf);
            vec_deinit(&all_nodes);
            return -1;
        }

        // Write to page file at new offset (CoW)
        uint64_t new_offset = 0;
        uint64_t bids[64] = {0};
        size_t num_bids = 0;
        int rc = page_file_write_node(db->page_file, buf, len,
                                       &new_offset, bids, 64, &num_bids);
        free(buf);
        if (rc != 0) {
            vec_deinit(&all_nodes);
            return -1;
        }

        // Mark old offset as stale (if previously persisted)
        if (node->disk_offset != (uint64_t)-1) {
            page_file_mark_stale(db->page_file, node->disk_offset, len);
        }

        // Update node's disk_offset and clear dirty flag
        node->disk_offset = new_offset;
        node->is_dirty = 0;

        // Find and update parent entry's child_disk_offset
        // Walk all other nodes to find entries pointing to this node
        for (size_t j = 0; j < all_nodes.length; j++) {
            hbtrie_node_t* other = all_nodes.data[j];
            if (other->btree == NULL) continue;
            // Walk bnode tree of the other node
            vec_t(bnode_t*) bs;
            vec_init(&bs);
            vec_push(&bs, other->btree);
            while (bs.length > 0) {
                bnode_t* bn = vec_pop(&bs);
                for (size_t k = 0; k < bnode_count(bn); k++) {
                    bnode_entry_t* entry = bnode_get(bn, k);
                    if (entry == NULL) continue;
                    if (!entry->has_value && entry->child == node) {
                        entry->child_disk_offset = new_offset;
                    }
                    if (entry->is_bnode_child && entry->child_bnode != NULL) {
                        vec_push(&bs, entry->child_bnode);
                    }
                }
            }
            vec_deinit(&bs);
        }
    }
    vec_deinit(&all_nodes);

    // Write superblock with new root offset
    int sb_rc = page_file_write_superblock(db->page_file, root->disk_offset, 0);
    if (sb_rc != 0) return -1;

    return 0;
}

// Load HBTrie from CBOR (fallback)
static hbtrie_t* load_index_cbor(const char* location, uint8_t chunk_size,
                                  uint32_t btree_node_size) {
    DIR* dir = opendir(location);
    if (dir == NULL) return NULL;

    char* latest_file = NULL;
    uint64_t latest_id = 0;

    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, "index_", 6) == 0 && strstr(entry->d_name, ".cbor") != NULL) {
            // Parse ID from filename: index_<id>_<crc>.cbor
            uint64_t id = 0;
            if (sscanf(entry->d_name, "index_%lu", &id) == 1) {
                if (id >= latest_id) {
                    latest_id = id;
                    free(latest_file);
                    latest_file = path_join(location, entry->d_name);
                }
            }
        }
    }
    closedir(dir);

    if (latest_file == NULL) {
        return hbtrie_create(chunk_size, btree_node_size);
    }

    // Read file
    int fd = open(latest_file, O_RDONLY);
    if (fd < 0) {
        free(latest_file);
        return hbtrie_create(chunk_size, btree_node_size);
    }

    struct stat st;
    if (fstat(fd, &st) != 0) {
        close(fd);
        free(latest_file);
        return hbtrie_create(chunk_size, btree_node_size);
    }

    uint8_t* buf = malloc(st.st_size);
    if (buf == NULL) {
        close(fd);
        free(latest_file);
        return hbtrie_create(chunk_size, btree_node_size);
    }

    ssize_t bytes_read = read(fd, buf, st.st_size);
    close(fd);
    free(latest_file);

    if (bytes_read != st.st_size) {
        free(buf);
        return hbtrie_create(chunk_size, btree_node_size);
    }

    hbtrie_t* trie = hbtrie_deserialize(buf, bytes_read, chunk_size, btree_node_size);
    free(buf);

    return trie ? trie : hbtrie_create(chunk_size, btree_node_size);
}

// Load HBTrie from disk — tries page_file first, then CBOR fallback
static hbtrie_t* load_index(const char* location, uint8_t chunk_size,
                             uint32_t btree_node_size,
                             page_file_t* page_file,
                             file_bnode_cache_t* bnode_cache) {
    // Try page_file first if available
    if (page_file != NULL) {
        hbtrie_t* trie = load_index_from_page_file(location, chunk_size,
                                                    btree_node_size, page_file,
                                                    bnode_cache);
        if (trie != NULL) return trie;
    }
    // Fall back to CBOR
    return load_index_cbor(location, chunk_size, btree_node_size);
}

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
    snprintf(config_path, sizeof(config_path), "%s/.config", location);
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
    db->lru_memory_mb = effective_config->lru_memory_mb;
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
        work_pool_launch(db->pool);
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
        db->wheel = hierarchical_timing_wheel_create(effective_config->timer_resolution_ms, db->pool);
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
    db->lru = database_lru_cache_create(db->lru_memory_mb * 1024 * 1024, effective_config->lru_shards);
    if (db->lru == NULL) {
        if (db->owns_pool) work_pool_destroy(db->pool);
        if (db->owns_wheel) hierarchical_timing_wheel_destroy(db->wheel);
        free(db->location);
        free(db);
        if (owns_config) database_config_destroy(effective_config);
        if (error_code) *error_code = ENOMEM;
        return NULL;
    }

    // Initialize write locks
    for (int i = 0; i < WRITE_LOCK_SHARDS; i++) {
        platform_lock_init(&db->write_locks[i]);
    }

    // Create storage if persistent
    if (effective_config->enable_persist) {
        char page_path[1024];
        snprintf(page_path, sizeof(page_path), "%s/data.wdbp", location);

        db->page_file = page_file_create(page_path,
            PAGE_FILE_DEFAULT_BLOCK_SIZE, PAGE_FILE_DEFAULT_NUM_SUPERBLOCKS);
        if (db->page_file != NULL) {
            int pf_rc = page_file_open(db->page_file, 1);
            if (pf_rc == 0) {
                // Create bnode cache
                bnode_cache_mgr_t* cache_mgr = bnode_cache_mgr_create(
                    128 * 1024 * 1024, 4);
                if (cache_mgr != NULL) {
                    db->bnode_cache = bnode_cache_create_file_cache(
                        cache_mgr, db->page_file, page_path);
                    if (db->bnode_cache != NULL) {
                        db->bnode_cache_mgr = cache_mgr;
                    } else {
                        bnode_cache_mgr_destroy(cache_mgr);
                    }
                }
            } else {
                page_file_destroy(db->page_file);
                db->page_file = NULL;
            }
        }

        if (db->page_file == NULL) {
            log_warn("Failed to initialize persistent storage, continuing in-memory only");
        }
    }

    // Load or create trie
    db->trie = load_index(location, db->chunk_size, db->btree_node_size, db->page_file, db->bnode_cache);
    if (db->trie == NULL) {
        db->trie = hbtrie_create(db->chunk_size, db->btree_node_size);
    }

    // Wire fcache into trie if page_file is available
    if (db->trie != NULL && db->bnode_cache != NULL) {
        db->trie->fcache = db->bnode_cache;
    }

    if (db->trie == NULL) {
        if (db->bnode_cache_mgr != NULL) bnode_cache_mgr_destroy(db->bnode_cache_mgr);
        if (db->page_file != NULL) page_file_destroy(db->page_file);
        database_lru_cache_destroy(db->lru);
        if (db->owns_pool) work_pool_destroy(db->pool);
        if (db->owns_wheel) hierarchical_timing_wheel_destroy(db->wheel);
        for (int i = 0; i < WRITE_LOCK_SHARDS; i++) {
            platform_lock_destroy(&db->write_locks[i]);
        }
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
        if (db->bnode_cache_mgr != NULL) bnode_cache_mgr_destroy(db->bnode_cache_mgr);
        if (db->page_file != NULL) page_file_destroy(db->page_file);
        database_lru_cache_destroy(db->lru);
        if (db->owns_pool) work_pool_destroy(db->pool);
        if (db->owns_wheel) hierarchical_timing_wheel_destroy(db->wheel);
        for (int i = 0; i < WRITE_LOCK_SHARDS; i++) {
            platform_lock_destroy(&db->write_locks[i]);
        }
        free(db->location);
        free(db);
        if (owns_config) database_config_destroy(effective_config);
        if (error_code) *error_code = ENOMEM;
        return NULL;
    }

    // Create WAL manager
    db->wal_manager = wal_manager_create(db->location, &effective_config->wal_config, db->wheel, error_code);
    if (db->wal_manager == NULL) {
        tx_manager_destroy(db->tx_manager);
        hbtrie_destroy(db->trie);
        if (db->bnode_cache_mgr != NULL) bnode_cache_mgr_destroy(db->bnode_cache_mgr);
        if (db->page_file != NULL) page_file_destroy(db->page_file);
        database_lru_cache_destroy(db->lru);
        if (db->owns_pool) work_pool_destroy(db->pool);
        if (db->owns_wheel) hierarchical_timing_wheel_destroy(db->wheel);
        for (int i = 0; i < WRITE_LOCK_SHARDS; i++) {
            platform_lock_destroy(&db->write_locks[i]);
        }
        free(db->location);
        free(db);
        if (owns_config) database_config_destroy(effective_config);
        if (error_code && *error_code == 0) {
            *error_code = ENOMEM;
        }
        return NULL;
    }

    // Set legacy WAL to NULL
    db->wal = NULL;

    // Store active config
    db->active_config = database_config_copy(effective_config);

    // Save config
    database_config_save(location, effective_config);

    // Replay WAL for recovery
    db->is_rebuilding = 1;
    wal_manager_recover(db->wal_manager, db);
    db->is_rebuilding = 0;

    refcounter_init((refcounter_t*)db);

    if (owns_config) {
        database_config_destroy(effective_config);
    }

    return db;
}

database_t* database_create(const char* location, size_t lru_memory_mb,
                            wal_config_t* wal_config,
                            uint8_t chunk_size, uint32_t btree_node_size,
                            uint8_t enable_persist, size_t storage_cache_size,
                            work_pool_t* pool, hierarchical_timing_wheel_t* wheel,
                            int* error_code) {
    (void)storage_cache_size;  // Kept for ABI compat, no longer used
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
    // storage_cache_size parameter kept for ABI compatibility but no longer used

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

// Persist the trie (page_file if available, otherwise CBOR fallback).
// Returns 0 on success, -1 if storage not available or persist failed.
static int database_persist(database_t* db) {
    if (db == NULL) return -1;
    if (db->page_file != NULL) {
        return database_flush_persist(db);
    }
    return save_index(db);
}

void database_destroy(database_t* db) {
    if (db == NULL) return;

    refcounter_dereference((refcounter_t*)db);
    uint_fast32_t count = refcounter_count((refcounter_t*)db);

    if (count == 0) {
        // Stop the timing wheel and worker pool BEFORE destroying data structures.
        // If we destroy the trie/LRU while workers are still running, a worker
        // thread might access freed memory or hold a lock on a destroyed mutex.
        // Note: Do NOT call wait_for_idle_signal here — debouncers reschedule
        // timers indefinitely, so the idle condition is never reached. The wheel
        // stop cancels all timers and the pool join ensures all workers complete.
        if (db->owns_wheel && db->wheel != NULL) {
            hierarchical_timing_wheel_stop(db->wheel);
        }
        if (db->owns_pool && db->pool != NULL) {
            work_pool_shutdown(db->pool);
            work_pool_join_all(db->pool);
        }

        // Flush all thread-local WALs to disk before destroying
        if (db->wal_manager) {
            wal_manager_flush(db->wal_manager);
        }

        // Persist trie before teardown
        // This ensures data survives across database_destroy/create cycles.
        if (db->page_file != NULL) {
            int persist_rc = database_persist(db);
            if (persist_rc == 0 && db->wal_manager != NULL) {
                // Seal and compact WAL so entries already captured in the
                // snapshot are not replayed on next database creation.
                wal_manager_seal_and_compact(db->wal_manager);
            } else if (persist_rc != 0) {
                log_warn("database_destroy: persist failed (rc=%d), "
                         "WAL entries will be replayed on next open", persist_rc);
            }
        } else if (db->trie != NULL) {
            // No page file — try CBOR fallback
            int persist_rc = database_persist(db);
            if (persist_rc != 0) {
                log_warn("database_destroy: persist failed (rc=%d)", persist_rc);
            }
        }

        // Destroy WAL manager (thread-local WAL)
        if (db->wal_manager) wal_manager_destroy(db->wal_manager);

        // Legacy WAL (should be NULL, but check for safety)
        if (db->wal) wal_destroy(db->wal);

        // Destroy LRU cache before trie — LRU holds REFERENCES to identifiers
        // that are also stored in the trie. Destroying LRU first decrements
        // refcounts (2→1), then trie destroy decrements (1→0) and frees.
        // Reversing the order causes use-after-free since trie frees identifiers
        // that the LRU still references.
        if (db->lru) database_lru_cache_destroy(db->lru);

        // Destroy trie (frees all identifiers and bnode entries)
        if (db->trie) hbtrie_destroy(db->trie);

        // Destroy transaction manager
        if (db->tx_manager) tx_manager_destroy(db->tx_manager);

        // Destroy bnode cache manager (which destroys all file caches) and page file
        if (db->bnode_cache_mgr != NULL) {
            bnode_cache_mgr_destroy(db->bnode_cache_mgr);
        }
        if (db->page_file != NULL) {
            page_file_destroy(db->page_file);
        }

        // Destroy write lock shards
        for (size_t i = 0; i < WRITE_LOCK_SHARDS; i++) {
            platform_lock_destroy(&db->write_locks[i]);
        }

        // Destroy active config
        if (db->active_config != NULL) {
            database_config_destroy(db->active_config);
        }

        // Destroy owned pool/wheel (already stopped/joined above)
        if (db->owns_pool && db->pool != NULL) {
            work_pool_destroy(db->pool);
        }
        if (db->owns_wheel && db->wheel != NULL) {
            hierarchical_timing_wheel_destroy(db->wheel);
        }

        free(db->location);
        refcounter_destroy_lock((refcounter_t*)db);
        free(db);
    }
    // If count > 0, other references exist and will handle cleanup when they dereference
}



// Helper: Get write lock shard from path
static inline size_t get_write_lock_shard(path_t* path) {
    if (path == NULL || path->identifiers.length == 0) {
        return 0;
    }

    // Hash the first identifier to get shard
    identifier_t* first_id = path->identifiers.data[0];
    if (first_id == NULL) {
        return 0;
    }

    // Simple hash: combine all chunks
    size_t hash = 0;
    for (size_t i = 0; i < first_id->chunks.length; i++) {
        chunk_t* chunk = first_id->chunks.data[i];
        if (chunk != NULL) {
            for (size_t j = 0; j < chunk->size; j++) {
                hash = hash * 31 + chunk->data[j];
            }
        }
    }

    return hash % WRITE_LOCK_SHARDS;
}

static void _database_put(database_put_ctx_t* ctx) {
    database_t* db = ctx->db;
    path_t* path = ctx->path;
    identifier_t* value = ctx->value;
    promise_t* promise = ctx->promise;

    // Begin MVCC transaction
    txn_desc_t* txn = tx_manager_begin(db->tx_manager);
    if (txn == NULL) {
        path_destroy(path);
        identifier_destroy(value);
        free(ctx);
        promise_resolve(promise, NULL);
        return;
    }

    // Write to thread-local WAL first (durability)
    buffer_t* entry = encode_put_entry(path, value);
    if (entry != NULL) {
        thread_wal_t* twal = get_thread_wal(db->wal_manager);
        if (twal != NULL) {
            int result = thread_wal_write(twal, txn->txn_id, WAL_PUT, entry);
            if (result != 0) {
                log_warn("Failed to write to thread-local WAL");
            }
        }
        buffer_destroy(entry);
    }

    // Acquire sharded write lock (allows concurrent writes to different paths)
    size_t shard = get_write_lock_shard(path);
    platform_lock(&db->write_locks[shard]);

    // Apply to trie with MVCC
    hbtrie_insert(db->trie, path, value, txn->txn_id);

    // Release write lock
    platform_unlock(&db->write_locks[shard]);

    // Commit transaction
    tx_manager_commit(db->tx_manager, txn);

    // Update LRU cache
    path_t* copied_path = path_copy(path);
    identifier_t* value_ref = REFERENCE(value, identifier_t);
    identifier_t* ejected = database_lru_cache_put(db->lru, copied_path, value_ref);
    if (ejected) {
        identifier_destroy(ejected);
    }

    // Clean up
    path_destroy(path);
    identifier_destroy(value);
    txn_desc_destroy(txn);
    free(ctx);

    promise_resolve(promise, NULL);
}

static void _database_get(database_get_ctx_t* ctx) {
    database_t* db = ctx->db;
    path_t* path = ctx->path;
    promise_t* promise = ctx->promise;

    // Check LRU cache first
    identifier_t* value = database_lru_cache_get(db->lru, path);
    if (value != NULL) {
        path_destroy(path);
        free(ctx);
        // database_lru_cache_get REFERENCE'd the value (count+1) for the
        // caller. CONSUME transfers this reference to the callback: sets
        // yield=1. The callback must REFERENCE (which consumes the yield)
        // then identifier_destroy (which decrements the count).
        identifier_t* consumed = (identifier_t*)CONSUME(value, identifier_t);
        promise_resolve(promise, consumed);
        return;
    }

    // Get last committed transaction ID (lock-free read)
    transaction_id_t read_txn_id = tx_manager_get_last_committed(db->tx_manager);

    // Look up in trie with MVCC (lock-free!)
    value = hbtrie_find(db->trie, path, read_txn_id);

    // Add to LRU cache if found
    if (value != NULL) {
        path_t* copied_path = path_copy(path);
        identifier_t* cached = REFERENCE(value, identifier_t);
        identifier_t* ejected = database_lru_cache_put(db->lru, copied_path, cached);
        if (ejected) {
            identifier_destroy(ejected);
        }
    }

    path_destroy(path);
    free(ctx);

    // hbtrie_find returns a reference the caller owns (count+1).
    // REFERENCE above created a separate reference for LRU.
    // CONSUME transfers the caller's reference to the callback: sets
    // yield=1. The callback must REFERENCE (which consumes the yield)
    // then identifier_destroy (which decrements the count).
    if (value != NULL) {
        identifier_t* consumed = (identifier_t*)CONSUME(value, identifier_t);
        promise_resolve(promise, consumed);
    } else {
        promise_resolve(promise, NULL);
    }
}

static void _database_delete(database_delete_ctx_t* ctx) {
    database_t* db = ctx->db;
    path_t* path = ctx->path;
    promise_t* promise = ctx->promise;

    // Begin transaction
    txn_desc_t* txn = tx_manager_begin(db->tx_manager);
    if (txn == NULL) {
        path_destroy(path);
        promise_resolve(promise, NULL);
        free(ctx);
        return;
    }

    // Write to thread-local WAL
    buffer_t* entry = encode_delete_entry(path);
    if (entry != NULL) {
        thread_wal_t* twal = get_thread_wal(db->wal_manager);
        if (twal != NULL) {
            int result = thread_wal_write(twal, txn->txn_id, WAL_DELETE, entry);
            if (result != 0) {
                log_warn("Failed to write to thread-local WAL");
            }
        }
        buffer_destroy(entry);
    }

    // Acquire sharded write lock (allows concurrent writes to different paths)
    size_t shard = get_write_lock_shard(path);
    platform_lock(&db->write_locks[shard]);

    // Remove from trie with MVCC (creates tombstone)
    identifier_t* removed = hbtrie_delete(db->trie, path, txn->txn_id);

    // Release write lock
    platform_unlock(&db->write_locks[shard]);

    // Commit transaction
    tx_manager_commit(db->tx_manager, txn);
    txn_desc_destroy(txn);

    // Remove from LRU cache
    database_lru_cache_delete(db->lru, path);

    path_destroy(path);
    free(ctx);

    if (removed) {
        identifier_destroy(removed);
    }

    promise_resolve(promise, NULL);
}

void database_put(database_t* db, path_t* path,
                   identifier_t* value, promise_t* promise) {
    if (db == NULL || path == NULL || value == NULL || promise == NULL) {
        if (path) path_destroy(path);
        if (value) identifier_destroy(value);
        if (promise) {
            promise_resolve(promise, NULL);
        }
        return;
    }

    database_put_ctx_t* ctx = get_clear_memory(sizeof(database_put_ctx_t));
    if (ctx == NULL) {
        path_destroy(path);
        identifier_destroy(value);
        promise_resolve(promise, NULL);
        return;
    }

    ctx->db = db;
    ctx->path = path;
    ctx->value = value;
    ctx->promise = promise;

    work_t* work = work_create(
        (void (*)(void*))_database_put,
        (void (*)(void*))abort_database_put,
        ctx);
    if (work == NULL) {
        free(ctx);
        path_destroy(path);
        identifier_destroy(value);
        promise_resolve(promise, NULL);
        return;
    }

    refcounter_yield((refcounter_t*) work);
    work_pool_enqueue(db->pool, work);
}

void database_get(database_t* db, path_t* path, promise_t* promise) {
    if (db == NULL || path == NULL || promise == NULL) {
        if (path) path_destroy(path);
        promise_resolve(promise, NULL);
        return;
    }

    database_get_ctx_t* ctx = get_clear_memory(sizeof(database_get_ctx_t));
    if (ctx == NULL) {
        path_destroy(path);
        promise_resolve(promise, NULL);
        return;
    }

    ctx->db = db;
    ctx->path = path;
    ctx->promise = promise;

    work_t* work = work_create(
        (void (*)(void*))_database_get,
        (void (*)(void*))abort_database_get,
        ctx);
    if (work == NULL) {
        free(ctx);
        path_destroy(path);
        promise_resolve(promise, NULL);
        return;
    }

    refcounter_yield((refcounter_t*) work);
    work_pool_enqueue(db->pool, work);
}

void database_delete(database_t* db, path_t* path, promise_t* promise) {
    if (db == NULL || path == NULL || promise == NULL) {
        if (path) path_destroy(path);
        promise_resolve(promise, NULL);
        return;
    }

    database_delete_ctx_t* ctx = get_clear_memory(sizeof(database_delete_ctx_t));
    if (ctx == NULL) {
        path_destroy(path);
        promise_resolve(promise, NULL);
        return;
    }

    ctx->db = db;
    ctx->path = path;
    ctx->promise = promise;

    work_t* work = work_create(
        (void (*)(void*))_database_delete,
        (void (*)(void*))abort_database_delete,
        ctx);
    if (work == NULL) {
        free(ctx);
        path_destroy(path);
        promise_resolve(promise, NULL);
        return;
    }

    refcounter_yield((refcounter_t*) work);
    work_pool_enqueue(db->pool, work);
}

int database_snapshot(database_t* db) {
    if (db == NULL) return -1;

    // Flush all thread-local WALs to ensure all data is persisted
    // This must be done before saving the index to guarantee consistency
    if (db->wal_manager != NULL) {
        wal_manager_flush(db->wal_manager);
    }

    // MVCC: Trigger GC to clean up old versions
    tx_manager_gc(db->tx_manager);

    // Save index to disk: use page_file if available, otherwise monolithic CBOR
    if (db->page_file != NULL) {
        return database_flush_persist(db);
    } else {
        return save_index(db);
    }
}

size_t database_count(database_t* db) {
    if (db == NULL) return 0;

    // Returns LRU cache entry count, which may undercount after eviction.
    // This is a fast approximation; for exact counts, walk the trie.
    return database_lru_cache_size(db->lru);
}

// ============================================================================
// Synchronous API Implementation
// ============================================================================

int database_put_sync(database_t* db, path_t* path, identifier_t* value) {
    // Validation (same as async)
    if (db == NULL || path == NULL || value == NULL) {
        if (path) path_destroy(path);
        if (value) identifier_destroy(value);
        return -1;
    }

    // Begin MVCC transaction
    txn_desc_t* txn = tx_manager_begin(db->tx_manager);
    if (txn == NULL) {
        path_destroy(path);
        identifier_destroy(value);
        return -1;
    }

    // Write to thread-local WAL
    buffer_t* entry = encode_put_entry(path, value);
    if (entry != NULL) {
        thread_wal_t* twal = get_thread_wal(db->wal_manager);
        if (twal != NULL) {
            int result = thread_wal_write(twal, txn->txn_id, WAL_PUT, entry);
            if (result != 0) {
                log_error("Failed to write to thread-local WAL (result=%d, txn_id=%lu.%09lu.%lu)",
                         result, txn->txn_id.time, txn->txn_id.nanos, txn->txn_id.count);
            } else {
                log_debug("Wrote PUT to WAL (txn_id=%lu.%09lu.%lu)",
                         txn->txn_id.time, txn->txn_id.nanos, txn->txn_id.count);
            }
        } else {
            log_error("get_thread_wal returned NULL - cannot write to WAL");
        }
        buffer_destroy(entry);
    } else {
        log_error("encode_put_entry returned NULL - cannot write to WAL");
    }

    // Acquire sharded write lock
    size_t shard = get_write_lock_shard(path);
    platform_lock(&db->write_locks[shard]);

    // Apply to trie with MVCC
    hbtrie_insert(db->trie, path, value, txn->txn_id);

    // Release write lock
    platform_unlock(&db->write_locks[shard]);

    // Commit transaction
    tx_manager_commit(db->tx_manager, txn);

    // Update LRU cache
    path_t* copied_path = path_copy(path);
    identifier_t* value_ref = REFERENCE(value, identifier_t);
    identifier_t* ejected = database_lru_cache_put(db->lru, copied_path, value_ref);
    if (ejected) {
        identifier_destroy(ejected);
    }

    // Cleanup
    path_destroy(path);
    identifier_destroy(value);
    txn_desc_destroy(txn);

    return 0;
}

int database_get_sync(database_t* db, path_t* path, identifier_t** result) {
    // Initialize output
    if (result == NULL) {
        if (path) path_destroy(path);
        return -1;
    }
    *result = NULL;

    // Validation
    if (db == NULL || path == NULL) {
        if (path) path_destroy(path);
        return -1;
    }

    // Check LRU cache first
    identifier_t* value = database_lru_cache_get(db->lru, path);
    if (value != NULL) {
        path_destroy(path);
        // Transfer ownership of the reference from database_lru_cache_get to the caller.
        // database_lru_cache_get already incremented the refcount, so we pass it directly.
        *result = value;
        return 0;
    }

    // Get last committed transaction ID (lock-free read)
    transaction_id_t read_txn_id = tx_manager_get_last_committed(db->tx_manager);

    // Look up in trie with MVCC (lock-free!)
    value = hbtrie_find(db->trie, path, read_txn_id);

    // Add to LRU cache if found
    if (value != NULL) {
        path_t* copied_path = path_copy(path);
        identifier_t* cached = REFERENCE(value, identifier_t);
        identifier_t* ejected = database_lru_cache_put(db->lru, copied_path, cached);
        if (ejected) {
            identifier_destroy(ejected);
        }
    }

    path_destroy(path);

    if (value != NULL) {
        // Transfer ownership of the reference from hbtrie_find to the caller.
        // hbtrie_find already incremented the refcount, so we pass it directly.
        *result = value;
        return 0;
    } else {
        return -2;  // Not found
    }
}

int database_delete_sync(database_t* db, path_t* path) {
    // Validation
    if (db == NULL || path == NULL) {
        if (path) path_destroy(path);
        return -1;
    }

    // Begin transaction
    txn_desc_t* txn = tx_manager_begin(db->tx_manager);
    if (txn == NULL) {
        path_destroy(path);
        return -1;
    }

    // Write to thread-local WAL
    buffer_t* entry = encode_delete_entry(path);
    if (entry != NULL) {
        thread_wal_t* twal = get_thread_wal(db->wal_manager);
        if (twal != NULL) {
            int result = thread_wal_write(twal, txn->txn_id, WAL_DELETE, entry);
            if (result != 0) {
                log_warn("Failed to write to thread-local WAL");
            }
        }
        buffer_destroy(entry);
    }

    // Acquire sharded write lock
    size_t shard = get_write_lock_shard(path);
    platform_lock(&db->write_locks[shard]);

    // Remove from trie with MVCC (creates tombstone)
    identifier_t* removed = hbtrie_delete(db->trie, path, txn->txn_id);

    // Release write lock
    platform_unlock(&db->write_locks[shard]);

    // Commit transaction
    tx_manager_commit(db->tx_manager, txn);
    txn_desc_destroy(txn);

    // Remove from LRU cache
    database_lru_cache_delete(db->lru, path);

    path_destroy(path);

    if (removed) {
        identifier_destroy(removed);
    }

    return 0;
}

int64_t database_increment_sync(database_t* db, path_t* path, int64_t delta) {
    if (db == NULL || path == NULL) return -1;

    // Acquire sharded write lock for atomic read-modify-write
    size_t shard = get_write_lock_shard(path);
    platform_lock(&db->write_locks[shard]);

    // Read current value
    transaction_id_t read_txn_id = tx_manager_get_last_committed(db->tx_manager);
    identifier_t* current = hbtrie_find(db->trie, path, read_txn_id);
    int64_t old_val = 0;
    if (current != NULL) {
        buffer_t* buf = identifier_to_buffer(current);
        if (buf != NULL && buf->size > 0) {
            // Null-terminate for strtoll
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

    // Write new value
    char val_str[32];
    snprintf(val_str, sizeof(val_str), "%lld", (long long)new_val);

    // Create copies for the insert
    path_t* ins_path = path_copy(path);
    buffer_t* val_buf = buffer_create(strlen(val_str));
    if (val_buf == NULL || ins_path == NULL) {
        if (val_buf) buffer_destroy(val_buf);
        if (ins_path) path_destroy(ins_path);
        platform_unlock(&db->write_locks[shard]);
        return -1;
    }
    memcpy(val_buf->data, val_str, strlen(val_str));
    val_buf->size = strlen(val_str);
    identifier_t* new_id = identifier_create(val_buf, 0);
    buffer_destroy(val_buf);
    if (new_id == NULL) {
        path_destroy(ins_path);
        platform_unlock(&db->write_locks[shard]);
        return -1;
    }

    // Begin transaction
    txn_desc_t* txn = tx_manager_begin(db->tx_manager);
    if (txn == NULL) {
        path_destroy(ins_path);
        identifier_destroy(new_id);
        platform_unlock(&db->write_locks[shard]);
        return -1;
    }

    // Insert into trie
    hbtrie_insert(db->trie, ins_path, new_id, txn->txn_id);

    // Update LRU cache
    path_t* cache_path = path_copy(ins_path);
    identifier_t* cache_val = REFERENCE(new_id, identifier_t);
    identifier_t* ejected = database_lru_cache_put(db->lru, cache_path, cache_val);
    if (ejected) identifier_destroy(ejected);

    // Commit and cleanup
    tx_manager_commit(db->tx_manager, txn);
    txn_desc_destroy(txn);
    path_destroy(ins_path);
    identifier_destroy(new_id);

    platform_unlock(&db->write_locks[shard]);
    return new_val;
}

database_iterator_t* database_scan_range(database_t* db,
                                          const char* start,
                                          const char* end) {
    if (db == NULL) return NULL;

    path_t* start_path = NULL;
    path_t* end_path = NULL;

    // Build start path from string
    if (start != NULL) {
        start_path = path_create();
        if (start_path == NULL) return NULL;
        const char* s = start;
        while (*s) {
            const char* e = strchr(s, '/');
            size_t len = e ? (size_t)(e - s) : strlen(s);
            if (len > 0) {
                buffer_t* buf = buffer_create(len);
                if (buf != NULL) {
                    memcpy(buf->data, s, len);
                    buf->size = len;
                    identifier_t* id = identifier_create(buf, 0);
                    buffer_destroy(buf);
                    if (id != NULL) {
                        path_append(start_path, id);
                        identifier_destroy(id);
                    }
                }
            }
            if (e) { s = e + 1; } else { break; }
        }
    }

    // Build end path from string
    if (end != NULL) {
        end_path = path_create();
        if (end_path == NULL) {
            if (start_path) path_destroy(start_path);
            return NULL;
        }
        const char* s = end;
        while (*s) {
            const char* e = strchr(s, '/');
            size_t len = e ? (size_t)(e - s) : strlen(s);
            if (len > 0) {
                buffer_t* buf = buffer_create(len);
                if (buf != NULL) {
                    memcpy(buf->data, s, len);
                    buf->size = len;
                    identifier_t* id = identifier_create(buf, 0);
                    buffer_destroy(buf);
                    if (id != NULL) {
                        path_append(end_path, id);
                        identifier_destroy(id);
                    }
                }
            }
            if (e) { s = e + 1; } else { break; }
        }
    }

    database_iterator_t* iter = database_scan_start(db, start_path, end_path);
    // database_scan_start copies the paths, so free our local copies
    if (start_path) path_destroy(start_path);
    if (end_path) path_destroy(end_path);
    return iter;
}

int database_write_batch_sync(database_t* db, batch_t* batch) {
    // Validate inputs
    if (db == NULL || batch == NULL) {
        return -1;
    }

    // Check if batch is empty
    platform_lock(&batch->lock);
    if (batch->count == 0) {
        platform_unlock(&batch->lock);
        return -3;
    }

    // Check if already submitted
    if (batch->submitted) {
        platform_unlock(&batch->lock);
        return -6;
    }

    platform_unlock(&batch->lock);

    // Check size against WAL max size
    size_t size = batch_estimate_size(batch);
    if (size > db->wal_manager->config.max_file_size) {
        return -5;
    }

    // Begin MVCC transaction
    txn_desc_t* txn = tx_manager_begin(db->tx_manager);
    if (txn == NULL) {
        return -1;
    }

    // Mark batch as submitted
    platform_lock(&batch->lock);
    batch->submitted = 1;
    platform_unlock(&batch->lock);

    // Acquire ALL write locks (in order: 0-63)
    for (size_t i = 0; i < WRITE_LOCK_SHARDS; i++) {
        platform_lock(&db->write_locks[i]);
    }

    // Serialize batch
    buffer_t* data = serialize_batch(batch);
    if (data == NULL) {
        // Release locks
        for (size_t i = WRITE_LOCK_SHARDS; i > 0; i--) {
            platform_unlock(&db->write_locks[i - 1]);
        }
        txn_desc_destroy(txn);
        return -1;
    }

    // Write to thread-local WAL
    thread_wal_t* twal = get_thread_wal(db->wal_manager);
    if (twal == NULL) {
        buffer_destroy(data);
        // Release locks
        for (size_t i = WRITE_LOCK_SHARDS; i > 0; i--) {
            platform_unlock(&db->write_locks[i - 1]);
        }
        txn_desc_destroy(txn);
        return -1;
    }

    int result = thread_wal_write(twal, txn->txn_id, WAL_BATCH, data);
    buffer_destroy(data);

    if (result != 0) {
        // Release locks
        for (size_t i = WRITE_LOCK_SHARDS; i > 0; i--) {
            platform_unlock(&db->write_locks[i - 1]);
        }
        txn_desc_destroy(txn);
        return result;
    }

    // Apply to trie with MVCC
    platform_lock(&batch->lock);
    for (size_t i = 0; i < batch->count; i++) {
        int op_result;
        if (batch->ops[i].type == WAL_PUT) {
            op_result = hbtrie_insert(db->trie, batch->ops[i].path, batch->ops[i].value, txn->txn_id);
        } else {
            identifier_t* removed = hbtrie_delete(db->trie, batch->ops[i].path, txn->txn_id);
            op_result = 0; // Delete always succeeds
            if (removed) {
                identifier_destroy(removed);
            }
            // Invalidate LRU cache for deleted keys
            database_lru_cache_delete(db->lru, batch->ops[i].path);
        }

        if (op_result != 0) {
            platform_unlock(&batch->lock);
            log_error("Batch apply failed at operation %zu", i);
            // Release locks
            for (size_t j = WRITE_LOCK_SHARDS; j > 0; j--) {
                platform_unlock(&db->write_locks[j - 1]);
            }
            txn_desc_destroy(txn);
            return -1;
        }
    }
    platform_unlock(&batch->lock);

    // Commit transaction
    tx_manager_commit(db->tx_manager, txn);
    txn_desc_destroy(txn);

    // Release locks (in reverse order: 63-0)
    for (size_t i = WRITE_LOCK_SHARDS; i > 0; i--) {
        platform_unlock(&db->write_locks[i - 1]);
    }

    return 0;
}

static void batch_execute_work(void* ctx) {
    batch_work_t* work = (batch_work_t*)ctx;

    int result = database_write_batch_sync(work->db, work->batch);

    // Drop the reference taken by database_write_batch
    refcounter_dereference((refcounter_t*)work->batch);

    // Allocate result on heap for promise handoff
    int* result_ptr = malloc(sizeof(int));
    if (result_ptr) {
        *result_ptr = result;
        promise_resolve(work->promise, result_ptr);
    } else {
        promise_resolve(work->promise, NULL);
    }

    free(work);
}

void database_write_batch(database_t* db, batch_t* batch, promise_t* promise) {
    if (db == NULL || batch == NULL || promise == NULL) {
        int* error = malloc(sizeof(int));
        if (error) {
            *error = -1;
            promise_resolve(promise, error);
        } else {
            promise_resolve(promise, NULL);
        }
        return;
    }

    batch_work_t* work = malloc(sizeof(batch_work_t));
    if (work == NULL) {
        int* error = malloc(sizeof(int));
        if (error) {
            *error = -1;
            promise_resolve(promise, error);
        } else {
            promise_resolve(promise, NULL);
        }
        return;
    }

    work->db = db;
    work->batch = batch;
    work->promise = promise;

    refcounter_reference((refcounter_t*)batch);
    work_t* task = work_create(batch_execute_work, NULL, work);
    refcounter_yield((refcounter_t*) task);
    work_pool_enqueue(db->pool, task);
}