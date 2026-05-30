#include "wal_actor.h"
#include "trie_shard_actor.h"
#include "../RefCounter/refcounter.h"
#include "../Util/allocator.h"
#include "../Util/log.h"
#include "../HBTrie/hbtrie.h"
#include "../HBTrie/path.h"
#include "../HBTrie/identifier.h"
#include "../Buffer/buffer.h"
#include "../Workers/transaction_id.h"
#include <cbor.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <dirent.h>

static void _flush_batch(wal_actor_t* wal) {
    if (wal->entry_buf_used == 0) return;
    ssize_t written = write(wal->fd, wal->entry_buf, wal->entry_buf_used);
    if (written > 0) {
        wal->pending_writes++;
        if (wal->pending_writes >= 16) {
            fsync(wal->fd);
            wal->pending_writes = 0;
        }
        wal->current_size += (size_t)written;
        if (wal->current_size >= wal->max_file_size) {
            fsync(wal->fd);
            close(wal->fd);
            wal->rotation_count++;
            char new_path[512];
            snprintf(new_path, sizeof(new_path), "%s/wal_%zu.wal", wal->location, wal->rotation_count);
            wal->fd = open(new_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
            wal->current_size = 0;
        }
    }
    wal->entry_buf_used = 0;
}

static void wal_actor_dispatch(void* state, message_t* msg) {
    wal_actor_t* wal = (wal_actor_t*)state;

    switch (msg->type) {
        case WAL_WRITE: {
            wal_record_payload_t* p = (wal_record_payload_t*)msg->payload;
            size_t needed = sizeof(uint64_t) + sizeof(transaction_id_t) + 1 + sizeof(uint32_t) + p->data->size;
            if (wal->entry_buf_used + needed > sizeof(wal->entry_buf)) {
                _flush_batch(wal);
            }
            if (needed > sizeof(wal->entry_buf)) {
                write(wal->fd, &p->thread_id, sizeof(uint64_t));
                write(wal->fd, &p->txn_id, sizeof(transaction_id_t));
                write(wal->fd, &p->type, 1);
                uint32_t len = (uint32_t)p->data->size;
                write(wal->fd, &len, sizeof(uint32_t));
                write(wal->fd, p->data->data, p->data->size);
            } else {
                memcpy(wal->entry_buf + wal->entry_buf_used, &p->thread_id, sizeof(uint64_t));
                wal->entry_buf_used += sizeof(uint64_t);
                memcpy(wal->entry_buf + wal->entry_buf_used, &p->txn_id, sizeof(transaction_id_t));
                wal->entry_buf_used += sizeof(transaction_id_t);
                wal->entry_buf[wal->entry_buf_used++] = p->type;
                uint32_t len = (uint32_t)p->data->size;
                memcpy(wal->entry_buf + wal->entry_buf_used, &len, sizeof(uint32_t));
                wal->entry_buf_used += sizeof(uint32_t);
                memcpy(wal->entry_buf + wal->entry_buf_used, p->data->data, p->data->size);
                wal->entry_buf_used += p->data->size;
            }
            if (transaction_id_compare(&p->txn_id, &wal->newest_txn_id) > 0) {
                wal->newest_txn_id = p->txn_id;
            }
            if (wal->oldest_txn_id.time == 0 && wal->oldest_txn_id.nanos == 0 && wal->oldest_txn_id.count == 0) {
                wal->oldest_txn_id = p->txn_id;
            }
            DESTROY(p->data, buffer);
            break;
        }
        case WAL_FLUSH: {
            _flush_batch(wal);
            if (wal->pending_writes > 0) {
                fsync(wal->fd);
                wal->pending_writes = 0;
            }
            break;
        }
        default:
            break;
    }
}

wal_actor_t* wal_actor_create(const char* location, void* wheel,
                               encryption_t* encryption, int* error_code) {
    *error_code = 0;
    wal_actor_t* wal = get_clear_memory(sizeof(wal_actor_t));
    wal->location = strdup(location);
    wal->wheel = wheel;
    wal->encryption = encryption;
    wal->max_file_size = 128 * 1024;

    mkdir(location, 0755);

    char path[512];
    snprintf(path, sizeof(path), "%s/wal_0.wal", location);
    wal->fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (wal->fd < 0) {
        *error_code = -1;
        free(wal->location);
        free(wal);
        return NULL;
    }
    wal->thread_id = platform_thread_self();
    actor_init(&wal->actor, wal, wal_actor_dispatch, NULL);
    return wal;
}

void wal_actor_destroy(wal_actor_t* wal) {
    if (!wal) return;
    if (wal->fd >= 0) {
        _flush_batch(wal);
        if (wal->pending_writes > 0) fsync(wal->fd);
        close(wal->fd);
    }
    free(wal->location);
    actor_destroy(&wal->actor);
    free(wal);
}

void wal_actor_write(wal_actor_t* wal, uint64_t thread_id, transaction_id_t txn_id,
                     uint8_t type, buffer_t* data, actor_t* reply_to) {
    wal_record_payload_t* p = get_clear_memory(sizeof(wal_record_payload_t));
    p->thread_id = thread_id;
    p->txn_id = txn_id;
    p->type = type;
    p->data = REFERENCE(data, buffer_t);
    p->reply_to = reply_to;
    message_t msg = { .type = WAL_WRITE, .payload = p, .payload_destroy = free };
    actor_send(&wal->actor, &msg);
}

// ============================================================================
// WAL recovery: replay wal_*.wal files into shard tries at startup
// ============================================================================

#define WAL_ACTOR_HEADER_SIZE (sizeof(uint64_t) + sizeof(transaction_id_t) + 1 + sizeof(uint32_t))

static int _wal_read_entry(int fd, transaction_id_t* txn_id, uint8_t* type,
                           buffer_t** data) {
    uint8_t header[WAL_ACTOR_HEADER_SIZE];
    ssize_t n = read(fd, header, WAL_ACTOR_HEADER_SIZE);
    if (n == 0) return 1;   // EOF
    if (n != WAL_ACTOR_HEADER_SIZE) return -1;

    // thread_id is at offset 0 (8 bytes) — skip
    memcpy(txn_id, header + 8, sizeof(transaction_id_t));
    *type = header[8 + sizeof(transaction_id_t)];
    uint32_t data_len;
    memcpy(&data_len, header + 8 + sizeof(transaction_id_t) + 1, sizeof(uint32_t));

    if (data_len == 0) {
        *data = NULL;
        return 0;
    }

    *data = buffer_create(data_len);
    if (*data == NULL) return -1;

    n = read(fd, (*data)->data, data_len);
    if (n != (ssize_t)data_len) {
        buffer_destroy(*data);
        *data = NULL;
        return -1;
    }
    (*data)->size = data_len;
    return 0;
}

void wal_actor_recover(wal_actor_t* wal, void* shards_ptr,
                       size_t shard_count) {
    trie_shard_actor_t** shards = (trie_shard_actor_t**)shards_ptr;
    if (wal == NULL || shards == NULL || shard_count == 0) return;

    log_info("WAL actor recovery: scanning %s for wal_*.wal files", wal->location);

    // Scan directory for wal_N.wal files
    DIR* dir = opendir(wal->location);
    if (dir == NULL) {
        log_warn("WAL actor recovery: cannot open directory %s", wal->location);
        return;
    }

    // Collect matching files
    char* wal_files[256];
    size_t file_count = 0;

    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL && file_count < 256) {
        if (strncmp(entry->d_name, "wal_", 4) == 0) {
            const char* dot = strrchr(entry->d_name, '.');
            if (dot != NULL && strcmp(dot, ".wal") == 0) {
                char* full = malloc(strlen(wal->location) + strlen(entry->d_name) + 2);
                if (full) {
                    sprintf(full, "%s/%s", wal->location, entry->d_name);
                    wal_files[file_count++] = full;
                }
            }
        }
    }
    closedir(dir);

    if (file_count == 0) {
        log_info("WAL actor recovery: no wal_*.wal files found");
        return;
    }

    log_info("WAL actor recovery: found %zu WAL files", file_count);

    // Get chunk_size from the shard trie (consistent across all shards)
    size_t csize = (shard_count > 0 && shards[0]) ? shards[0]->chunk_size : 4;

    // Replay entries from each file
    size_t total_replayed = 0;

    for (size_t f = 0; f < file_count; f++) {
        int fd = open(wal_files[f], O_RDONLY);
        if (fd < 0) {
            log_warn("WAL actor recovery: cannot open %s", wal_files[f]);
            free(wal_files[f]);
            continue;
        }

        log_info("WAL actor recovery: replaying %s", wal_files[f]);

        while (1) {
            transaction_id_t txn_id;
            uint8_t type;
            buffer_t* data = NULL;

            int rc = _wal_read_entry(fd, &txn_id, &type, &data);
            if (rc != 0) break;  // EOF or error

            if (data == NULL || data->size == 0) continue;

            // Deserialize CBOR data: [path_cbor] for DELETE, [path_cbor, value_cbor] for PUT
            struct cbor_load_result result;
            cbor_item_t* cbor = cbor_load(data->data, data->size, &result);
            buffer_destroy(data);

            if (cbor == NULL || result.error.code != CBOR_ERR_NONE) {
                if (cbor) cbor_decref(&cbor);
                continue;
            }

            if (!cbor_isa_array(cbor)) {
                cbor_decref(&cbor);
                continue;
            }

            size_t array_sz = cbor_array_size(cbor);
            if (array_sz < 1) {
                cbor_decref(&cbor);
                continue;
            }

            // Get path from first element
            cbor_item_t** items = cbor_array_handle(cbor);
            path_t* path = cbor_to_path(items[0], csize);
            if (path == NULL) {
                cbor_decref(&cbor);
                continue;
            }

            // Route to correct shard
            size_t shard_idx = path_hash(path) % shard_count;

            if (type == 'p' && array_sz >= 2) {
                // PUT: deserialize value and insert
                identifier_t* value = cbor_to_identifier(items[1], csize);
                if (value != NULL) {
                    hbtrie_insert(shards[shard_idx]->trie, path, value, txn_id);
                    ATOMIC_STORE(&shards[shard_idx]->root,
                                 shards[shard_idx]->trie->root);
                } else {
                    path_destroy(path);
                }
            } else if (type == 'd') {
                // DELETE: remove from trie
                identifier_t* removed = hbtrie_delete(shards[shard_idx]->trie,
                                                       path, txn_id);
                if (removed) identifier_destroy(removed);
                ATOMIC_STORE(&shards[shard_idx]->root,
                             shards[shard_idx]->trie->root);
            } else {
                // Unknown type — consume path
                path_destroy(path);
            }

            cbor_decref(&cbor);
            total_replayed++;
        }

        close(fd);
        free(wal_files[f]);
    }

    log_info("WAL actor recovery: replayed %zu entries total", total_replayed);
}
