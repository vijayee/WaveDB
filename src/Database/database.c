//
// Created by victor on 3/11/26.
//

#include "database.h"
#include "../Util/allocator.h"
#include "../Util/mkdir_p.h"
#include "../Util/path_join.h"
#include "../Util/log.h"
#include "../Buffer/buffer.h"
#include "../Time/debouncer.h"
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
static void database_snapshot_callback(void* ctx);
static void _database_put(database_put_ctx_t* ctx);
static void _database_get(database_get_ctx_t* ctx);
static void _database_delete(database_delete_ctx_t* ctx);

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
    uint32_t crc = hbtrie_compute_hash(db->write_trie);

    char* index_file = create_index_path(db->location, 0, crc);  // TODO: use incrementing ID
    if (index_file == NULL) return -1;

    uint8_t* buf = NULL;
    size_t len = 0;
    if (hbtrie_serialize(db->write_trie, &buf, &len) != 0) {
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
            wal_type_e type;
            buffer_t* data = NULL;
            int result = wal_read(db->wal, &type, &data, &cursor);
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
                    hbtrie_insert(db->write_trie, path, value);
                    identifier_destroy(value);
                    path_destroy(path);
                }
                cbor_decref(&path_cbor);
                cbor_decref(&value_cbor);
            } else if (type == WAL_DELETE && cbor_isa_array(item) && cbor_array_size(item) == 1) {
                cbor_item_t* path_cbor = cbor_array_get(item, 0);
                path_t* path = cbor_to_path(path_cbor, db->chunk_size);
                if (path) {
                    identifier_t* removed = hbtrie_remove(db->write_trie, path);
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

database_t* database_create(const char* location, size_t lru_size, size_t wal_max_size,
                            uint8_t chunk_size, uint32_t btree_node_size,
                            work_pool_t* pool, hierarchical_timing_wheel_t* wheel,
                            int* error_code) {
    if (error_code) *error_code = 0;

    // Create directory if needed
    if (mkdir_p((char*)location) != 0) {
        if (error_code) *error_code = errno;
        return NULL;
    }

    database_t* db = get_clear_memory(sizeof(database_t));
    if (db == NULL) {
        if (error_code) *error_code = ENOMEM;
        return NULL;
    }

    db->location = strdup(location);
    db->lru_size = (lru_size == 0) ? DATABASE_DEFAULT_LRU_SIZE : lru_size;
    db->wal_max_size = (wal_max_size == 0) ? DATABASE_DEFAULT_WAL_MAX_SIZE : wal_max_size;
    db->chunk_size = (chunk_size == 0) ? DEFAULT_CHUNK_SIZE : chunk_size;
    db->btree_node_size = btree_node_size;
    db->pool = pool;
    db->wheel = wheel;
    db->is_rebuilding = 0;

    // Create LRU cache
    db->lru = database_lru_cache_create(db->lru_size);
    if (db->lru == NULL) {
        free(db->location);
        free(db);
        if (error_code) *error_code = ENOMEM;
        return NULL;
    }

    // Initialize locks
    platform_lock_init(&db->write_lock);
    platform_rw_lock_init(&db->read_lock);
    platform_lock_init(&db->callback_lock);
    platform_condition_init(&db->callback_done);
    db->callback_in_progress = 0;
    db->destroy_requested = 0;

    // Load or create write_trie
    db->write_trie = load_index(location, db->chunk_size, db->btree_node_size);
    if (db->write_trie == NULL) {
        db->write_trie = hbtrie_create(db->chunk_size, db->btree_node_size);
    }

    // Create read_trie copy
    db->read_trie = hbtrie_copy(db->write_trie);
    if (db->read_trie == NULL) {
        hbtrie_destroy(db->write_trie);
        database_lru_cache_destroy(db->lru);
        platform_lock_destroy(&db->write_lock);
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

    // Create debouncer
    db->debouncer = debouncer_create(wheel, db, database_snapshot_callback, NULL,
                                      DATABASE_DEBOUNCE_WAIT_MS, DATABASE_DEBOUNCE_MAX_WAIT_MS);

    refcounter_init((refcounter_t*)db);
    return db;
}

void database_destroy(database_t* db) {
    if (db == NULL) return;

    refcounter_dereference((refcounter_t*)db);
    if (refcounter_count((refcounter_t*)db) == 0) {
        // Signal that destroy is requested to prevent new callbacks
        platform_lock(&db->callback_lock);
        db->destroy_requested = 1;
        // Wait for any in-progress callback to complete
        while (db->callback_in_progress) {
            platform_condition_wait(&db->callback_lock, &db->callback_done);
        }
        platform_unlock(&db->callback_lock);

        // Flush any pending writes
        if (db->debouncer) {
            debouncer_flush(db->debouncer);
            debouncer_destroy(db->debouncer);
        }

        if (db->wal) wal_destroy(db->wal);
        if (db->read_trie) hbtrie_destroy(db->read_trie);
        if (db->write_trie) hbtrie_destroy(db->write_trie);
        if (db->lru) database_lru_cache_destroy(db->lru);

        platform_lock_destroy(&db->write_lock);
        platform_rw_lock_destroy(&db->read_lock);
        platform_lock_destroy(&db->callback_lock);
        platform_condition_destroy(&db->callback_done);
        refcounter_destroy_lock((refcounter_t*)db);

        free(db->location);
        free(db);
    }
}

static void database_snapshot_callback(void* ctx) {
    database_t* db = (database_t*)ctx;

    // Check if destroy is in progress
    platform_lock(&db->callback_lock);
    if (db->destroy_requested) {
        platform_unlock(&db->callback_lock);
        return;
    }
    db->callback_in_progress = 1;
    platform_unlock(&db->callback_lock);

    // Acquire write lock
    platform_lock(&db->write_lock);

    // Save write_trie to disk
    save_index(db);

    // Copy write_trie to new read_trie
    hbtrie_t* new_read_trie = hbtrie_copy(db->write_trie);

    // Swap read_trie pointer
    platform_rw_lock_w(&db->read_lock);
    hbtrie_t* old_read_trie = db->read_trie;
    db->read_trie = new_read_trie;
    platform_rw_unlock_w(&db->read_lock);

    // Release write lock
    platform_unlock(&db->write_lock);

    // Destroy old read_trie
    if (old_read_trie) {
        hbtrie_destroy(old_read_trie);
    }

    // Signal that callback is complete
    platform_lock(&db->callback_lock);
    db->callback_in_progress = 0;
    platform_signal_condition(&db->callback_done);
    platform_unlock(&db->callback_lock);
}

static void _database_put(database_put_ctx_t* ctx) {
    database_t* db = ctx->db;
    path_t* path = ctx->path;
    identifier_t* value = ctx->value;
    promise_t* promise = ctx->promise;

    // Write to WAL first (durability)
    buffer_t* entry = encode_put_entry(path, value);
    if (entry != NULL) {
        wal_write(db->wal, WAL_PUT, entry);
        buffer_destroy(entry);
    }

    // Acquire write lock
    platform_lock(&db->write_lock);

    // Check write_trie
    if (db->write_trie == NULL) {
        platform_unlock(&db->write_lock);
        promise_resolve(promise, NULL);
        free(ctx);
        return;
    }

    // Apply to write_trie
    hbtrie_insert(db->write_trie, path, value);

    // Release write lock
    platform_unlock(&db->write_lock);

    // Update LRU cache
    path_t* copied_path = path_copy(path);
    identifier_t* value_ref = REFERENCE(value, identifier_t);
    identifier_t* ejected = database_lru_cache_put(db->lru, copied_path, value_ref);
    if (ejected) {
        identifier_destroy(ejected);
    }

    // Trigger debounce
    if (!db->is_rebuilding) {
        debouncer_debounce(db->debouncer);
    }

    // Clean up
    path_destroy(path);
    identifier_destroy(value);
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

    // Acquire read lock on read_trie
    platform_rw_lock_r(&db->read_lock);

    // Look up in read_trie
    value = hbtrie_find(db->read_trie, path);

    // Release read lock
    platform_rw_unlock_r(&db->read_lock);

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

    // Write to WAL
    buffer_t* entry = encode_delete_entry(path);
    if (entry != NULL) {
        wal_write(db->wal, WAL_DELETE, entry);
        buffer_destroy(entry);
    }

    // Acquire write lock
    platform_lock(&db->write_lock);

    // Remove from write_trie
    identifier_t* removed = hbtrie_remove(db->write_trie, path);

    // Release write lock
    platform_unlock(&db->write_lock);

    // Remove from LRU cache
    database_lru_cache_delete(db->lru, path);

    // Trigger debounce
    if (!db->is_rebuilding) {
        debouncer_debounce(db->debouncer);
    }

    path_destroy(path);
    free(ctx);

    if (removed) {
        identifier_destroy(removed);
    }

    promise_resolve(promise, NULL);
}

void database_put(database_t* db, priority_t priority, path_t* path,
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

    work_t* work = work_create(priority, ctx,
        (void (*)(void*))_database_put,
        (void (*)(void*))free);  // abort: free context
    if (work == NULL) {
        free(ctx);
        path_destroy(path);
        identifier_destroy(value);
        promise_resolve(promise, NULL);
        return;
    }

    work_pool_enqueue(db->pool, work);
}

void database_get(database_t* db, priority_t priority, path_t* path, promise_t* promise) {
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

    work_t* work = work_create(priority, ctx,
        (void (*)(void*))_database_get,
        (void (*)(void*))free);
    if (work == NULL) {
        free(ctx);
        path_destroy(path);
        promise_resolve(promise, NULL);
        return;
    }

    work_pool_enqueue(db->pool, work);
}

void database_delete(database_t* db, priority_t priority, path_t* path, promise_t* promise) {
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

    work_t* work = work_create(priority, ctx,
        (void (*)(void*))_database_delete,
        (void (*)(void*))free);
    if (work == NULL) {
        free(ctx);
        path_destroy(path);
        promise_resolve(promise, NULL);
        return;
    }

    work_pool_enqueue(db->pool, work);
}

int database_snapshot(database_t* db) {
    if (db == NULL) return -1;

    database_snapshot_callback(db);
    return 0;
}

size_t database_count(database_t* db) {
    if (db == NULL) return 0;

    // Return LRU cache size as approximation
    return database_lru_cache_size(db->lru);
}