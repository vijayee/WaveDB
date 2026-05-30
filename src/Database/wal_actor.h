#ifndef WAVEDB_WAL_ACTOR_H
#define WAVEDB_WAL_ACTOR_H

#include <stdint.h>
#include <stddef.h>
#include "../Actor/actor.h"
#include "../Actor/message.h"
#include "../Buffer/buffer.h"
#include "../Storage/encryption.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct wal_actor_t {
    actor_t actor;
    char* location;
    int fd;
    uint64_t thread_id;
    void* wheel;
    encryption_t* encryption;
    size_t pending_writes;
    size_t current_size;
    size_t max_file_size;
    transaction_id_t oldest_txn_id;
    transaction_id_t newest_txn_id;
    size_t rotation_count;
    uint8_t entry_buf[4096];
    size_t entry_buf_used;
} wal_actor_t;

wal_actor_t* wal_actor_create(const char* location, void* wheel,
                               encryption_t* encryption, int* error_code);
void wal_actor_destroy(wal_actor_t* wal);
void wal_actor_write(wal_actor_t* wal, uint64_t thread_id, transaction_id_t txn_id,
                     uint8_t type, buffer_t* data, actor_t* reply_to);

#ifdef __cplusplus
}
#endif

#endif /* WAVEDB_WAL_ACTOR_H */
