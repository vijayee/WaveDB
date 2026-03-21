//
// Created by victor on 3/11/26.
//

#include "database.h"
#include "../Util/allocator.h"
#include "../Util/mkdir_p.h"
#include "../Util/path_join.h"
#include "../Util/log.h"
#include "../Storage/sections.h"
#include "../Storage/node_serializer.h"
#include <cbor.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>

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
    if (index_file == NULL) return -1;

    uint8_t* buf = NULL;
    size_t len = 0;
    if (hbtrie_serialize(db->trie, &buf, &len) != 0) {
        free(index_file);
        return -1;
    }

    int fd = open(index_file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        free(buf);
        free(index_file);
        return -1;
    }

    ssize_t written = write(fd, buf, len);
    close(fd);
    free(buf);
    free(index_file);

    return (written == (ssize_t)len) ? 0 : -1;
}

// Load HBTrie from disk
static hbtrie_t* load_index(const char* location, uint8_t chunk_size, uint32_t btree_node_size) {
    // Find most recent index file
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

// Replay WAL entries
static int replay_wal(database_t* db) {
    if (db->wal == NULL) return 0;

    // List all WAL sequence files
    uint64_t* sequences = NULL;
    size_t seq_count = 0;
    if (wal_list_sequences(db->location, &sequences, &seq_count) != 0) {
        return 0;  // No WAL files
    }

    // Replay each sequence in order
    for (size_t i = 0; i < seq_count; i++) {
        char* wal_path = wal_sequence_path(db->location, sequences[i]);
        if (wal_path == NULL) continue;

        int fd = open(wal_path, O_RDONLY);
        free(wal_path);
        if (fd < 0) continue;

        // Set wal's fd for reading
        int old_fd = db->wal->fd;
        db->wal->fd = fd;

        uint64_t cursor = 0;
        while (1) {
            transaction_id_t txn_id;
            wal_type_e type;
            buffer_t* data = NULL;
            int result = wal_read(db->wal, &txn_id, &type, &data, &cursor);
            if (result != 0) break;  // EOF or error

            // Decode and apply
            struct cbor_load_result cbor_result;
            cbor_item_t* item = cbor_load(data->data, data->size, &cbor_result);
            if (item == NULL || cbor_result.error.code != CBOR_ERR_NONE) {
                if (item) cbor_decref(&item);
                buffer_destroy(data);
                continue;
            }

            if (type == WAL_PUT && cbor_isa_array(item) && cbor_array_size(item) == 2) {
                cbor_item_t* path_cbor = cbor_array_get(item, 0);
                cbor_item_t* value_cbor = cbor_array_get(item, 1);
                path_t* path = cbor_to_path(path_cbor, db->chunk_size);
                identifier_t* value = cbor_to_identifier(value_cbor, db->chunk_size);
                if (path && value) {
                    hbtrie_insert(db->trie, path, value);
                    identifier_destroy(value);
                    path_destroy(path);
                }
                cbor_decref(&path_cbor);
                cbor_decref(&value_cbor);
            } else if (type == WAL_DELETE && cbor_isa_array(item) && cbor_array_size(item) == 1) {
                cbor_item_t* path_cbor = cbor_array_get(item, 0);
                path_t* path = cbor_to_path(path_cbor, db->chunk_size);
                if (path) {
                    identifier_t* removed = hbtrie_remove(db->trie, path);
                    if (removed) identifier_destroy(removed);
                    path_destroy(path);
                }
                cbor_decref(&path_cbor);
            }

            cbor_decref(&item);
            buffer_destroy(data);
        }

        close(fd);
        db->wal->fd = old_fd;
    }

    free(sequences);
    return 0;
}

database_t* database_create(const char* location, size_t lru_memory_mb, size_t wal_max_size,
                            uint8_t chunk_size, uint32_t btree_node_size,
                            uint8_t enable_persist, size_t storage_cache_size,
                            work_pool_t* pool, hierarchical_timing_wheel_t* wheel,
                            int* error_code) {
    if (error_code) *error_code = 0;

    // Create directory if needed
    if (mkdir_p((char*)location) != 0) {
        if (error_code) *error_code = errno;
        return NULL;
    }

    // Initialize transaction ID generator (call once per process)
    transaction_id_init();

    database_t* db = get_clear_memory(sizeof(database_t));
    if (db == NULL) {
        if (error_code) *error_code = ENOMEM;
        return NULL;
    }

    db->location = strdup(location);
    db->lru_size = (lru_memory_mb == 0) ? DATABASE_DEFAULT_LRU_MEMORY_MB : lru_memory_mb;
    db->wal_max_size = (wal_max_size == 0) ? DATABASE_DEFAULT_WAL_MAX_SIZE : wal_max_size;
    db->chunk_size = (chunk_size == 0) ? DEFAULT_CHUNK_SIZE : chunk_size;
    db->btree_node_size = btree_node_size;
    db->pool = pool;
    db->wheel = wheel;
    db->is_rebuilding = 0;

    // Convert MB to bytes
    size_t lru_memory_bytes = (lru_memory_mb == 0) ?
        DATABASE_DEFAULT_LRU_MEMORY_MB * 1024 * 1024 :
        lru_memory_mb * 1024 * 1024;

    // Create LRU cache with memory budget
    db->lru = database_lru_cache_create(lru_memory_bytes);
    if (db->lru == NULL) {
        free(db->location);
        free(db);
        if (error_code) *error_code = ENOMEM;
        return NULL;
    }

    // Initialize write lock shards
    for (size_t i = 0; i < WRITE_LOCK_SHARDS; i++) {
        platform_lock_init(&db->write_locks[i]);
    }

    // Initialize storage if persistence is enabled
    if (enable_persist) {
        // Create data and meta directories for sections
        char* data_path = path_join(location, "data");
        char* meta_path = path_join(location, "meta");
        mkdir_p(data_path);
        mkdir_p(meta_path);

        // Determine storage parameters
        size_t cache_size = (storage_cache_size == 0) ? 64 : storage_cache_size;  // Default: 64 sections in cache
        size_t section_concurrency = 8;  // Keep 8 sections open at once
        size_t sections_size = 1024 * 1024;  // 1MB per section

        db->storage = sections_create(location, sections_size, cache_size, section_concurrency,
                                      wheel, DATABASE_DEBOUNCE_WAIT_MS, DATABASE_DEBOUNCE_MAX_WAIT_MS);

        free(data_path);
        free(meta_path);

        if (db->storage == NULL) {
            // Storage initialization failed, continue in-memory only
            log_warn("Failed to initialize persistent storage, continuing in-memory only");
        } else {
            db->storage_cache_size = cache_size;
            db->storage_max_tuple = section_concurrency;

            // Attach storage to trie
            db->trie->root->storage = db->storage;
        }
    } else {
        db->storage = NULL;
    }

    // Load or create trie
    db->trie = load_index(location, db->chunk_size, db->btree_node_size);
    if (db->trie == NULL) {
        db->trie = hbtrie_create(db->chunk_size, db->btree_node_size);
    }

    // If using storage, attach it to trie
    if (db->storage != NULL && db->trie->root != NULL) {
        db->trie->root->storage = db->storage;
    }

    if (db->trie == NULL) {
        if (db->storage) sections_destroy(db->storage);
        database_lru_cache_destroy(db->lru);
        for (size_t i = 0; i < WRITE_LOCK_SHARDS; i++) {
            platform_lock_destroy(&db->write_locks[i]);
        }
        free(db->location);
        free(db);
        if (error_code) *error_code = ENOMEM;
        return NULL;
    }

    // Create transaction manager for MVCC
    db->tx_manager = tx_manager_create(db->trie, pool, wheel, 100);  // 100ms GC interval
    if (db->tx_manager == NULL) {
        hbtrie_destroy(db->trie);
        if (db->storage) sections_destroy(db->storage);
        database_lru_cache_destroy(db->lru);
        for (size_t i = 0; i < WRITE_LOCK_SHARDS; i++) {
            platform_lock_destroy(&db->write_locks[i]);
        }
        free(db->location);
        free(db);
        if (error_code) *error_code = ENOMEM;
        return NULL;
    }

    // Create WAL
    db->wal = wal_create(db->location, db->wal_max_size, error_code);

    // Replay WAL for recovery
    db->is_rebuilding = 1;
    replay_wal(db);
    db->is_rebuilding = 0;


    refcounter_init((refcounter_t*)db);
    return db;
}

void database_destroy(database_t* db) {
    if (db == NULL) return;

    refcounter_dereference((refcounter_t*)db);
    if (refcounter_count((refcounter_t*)db) == 0) {
        if (db->wal) wal_destroy(db->wal);

        // Destroy trie
        if (db->trie) hbtrie_destroy(db->trie);

        // Destroy transaction manager
        if (db->tx_manager) tx_manager_destroy(db->tx_manager);

        // Destroy storage if present
        if (db->storage) {
            sections_destroy(db->storage);
        }

        if (db->lru) database_lru_cache_destroy(db->lru);

        // Destroy write lock shards
        for (size_t i = 0; i < WRITE_LOCK_SHARDS; i++) {
            platform_lock_destroy(&db->write_locks[i]);
        }

        free(db->location);
        refcounter_destroy_lock((refcounter_t*)db);
        free(db);
    }
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
        if (chunk != NULL && chunk->data != NULL) {
            for (size_t j = 0; j < chunk->data->size; j++) {
                hash = hash * 31 + chunk->data->data[j];
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

    // Write to WAL first (durability)
    buffer_t* entry = encode_put_entry(path, value);
    if (entry != NULL) {
        wal_write(db->wal, txn->txn_id, WAL_PUT, entry);
        buffer_destroy(entry);
    }

    // Acquire sharded write lock (allows concurrent writes to different paths)
    size_t shard = get_write_lock_shard(path);
    platform_lock(&db->write_locks[shard]);

    // Apply to trie with MVCC
    hbtrie_insert_mvcc(db->trie, path, value, txn->txn_id);

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
        promise_resolve(promise, CONSUME(value, identifier_t));
        return;
    }

    // Get last committed transaction ID (lock-free read)
    transaction_id_t read_txn_id = tx_manager_get_last_committed(db->tx_manager);

    // Look up in trie with MVCC (lock-free!)
    value = hbtrie_find_mvcc(db->trie, path, read_txn_id);

    // Add to LRU cache if found
    if (value != NULL) {
        path_t* copied_path = path_copy(path);
        identifier_t* cached = REFERENCE(value, identifier_t);
        database_lru_cache_put(db->lru, copied_path, cached);
    }

    path_destroy(path);
    free(ctx);

    promise_resolve(promise, value ? CONSUME(value, identifier_t) : NULL);
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

    // Write to WAL
    buffer_t* entry = encode_delete_entry(path);
    if (entry != NULL) {
        wal_write(db->wal, txn->txn_id, WAL_DELETE, entry);
        buffer_destroy(entry);
    }

    // Acquire sharded write lock (allows concurrent writes to different paths)
    size_t shard = get_write_lock_shard(path);
    platform_lock(&db->write_locks[shard]);

    // Remove from trie with MVCC (creates tombstone)
    identifier_t* removed = hbtrie_delete_mvcc(db->trie, path, txn->txn_id);

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

    // MVCC: Trigger GC to clean up old versions
    tx_manager_gc(db->tx_manager);

    // Save index to disk
    return save_index(db);
}

size_t database_count(database_t* db) {
    if (db == NULL) return 0;

    // Return LRU cache size as approximation
    return database_lru_cache_size(db->lru);
}