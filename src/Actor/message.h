#ifndef WAVEDB_ACTOR_MESSAGE_H
#define WAVEDB_ACTOR_MESSAGE_H

#include <stdint.h>
#include "../HBTrie/path.h"
#include "../HBTrie/identifier.h"
#include "../Buffer/buffer.h"
#include "../Workers/promise.h"
#include "../Workers/transaction_id.h"
#include "../Storage/bnode_cache.h"
#include "../Time/wheel.h"

/* Forward declarations */
typedef struct actor_t actor_t;

/* Database operations */
typedef struct {
    path_t* path;
    identifier_t* value;     /* NULL for reads */
    promise_t* promise;     /* NULL for fire-and-forget */
} db_op_payload_t;

/* WAL write record */
typedef struct {
    uint64_t thread_id;
    transaction_id_t txn_id;
    uint8_t type;            /* 0=put, 1=delete */
    buffer_t* data;          /* serialized entry */
    actor_t* reply_to;
} wal_record_payload_t;

/* LRU cache operations */
typedef struct {
    path_t* path;
    identifier_t* value;     /* For put; NULL for get */
    actor_t* reply_to;
} lru_op_payload_t;

typedef struct {
    path_t* path;
    identifier_t* value;     /* NULL if not found */
} lru_result_payload_t;

/* BNode cache operations */
typedef struct {
    uint64_t offset;         /* Disk offset */
    uint8_t* data;           /* For write; NULL for read */
    size_t data_len;
    actor_t* reply_to;
} bnode_cache_op_payload_t;

typedef struct {
    uint64_t offset;
    uint8_t* data;
    size_t data_len;
} bnode_cache_result_payload_t;

typedef enum {
    /* Database operations */
    DB_PUT = 0,
    DB_GET,
    DB_DELETE,
    DB_SNAPSHOT,
    /* WAL operations */
    WAL_WRITE,
    WAL_FLUSH,
    WAL_SEAL_AND_COMPACT,
    /* LRU operations */
    LRU_GET,
    LRU_PUT,
    LRU_DELETE,
    LRU_CLEAR,
    /* BNode cache operations */
    BNODE_CACHE_READ,
    BNODE_CACHE_WRITE,
    BNODE_CACHE_RELEASE,
    BNODE_CACHE_INVALIDATE,
    BNODE_CACHE_FLUSH,
    /* Result messages */
    DB_GET_RESULT,
    DB_OP_RESULT,
    WAL_FLUSH_RESULT,
    LRU_GET_RESULT,
    BNODE_CACHE_READ_RESULT,
    /* Shard actor internal */
    SHARD_PUT,
    SHARD_GET,
    SHARD_DELETE,
    SHARD_GC,
} wavedb_message_type_e;

typedef struct message_t {
    uint32_t type;
    void* payload;
    void (*payload_destroy)(void*);
} message_t;

#endif /* WAVEDB_ACTOR_MESSAGE_H */
