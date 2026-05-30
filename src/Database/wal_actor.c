#include "wal_actor.h"
#include "../RefCounter/refcounter.h"
#include "../Util/allocator.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

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
