#ifndef WAVEDB_TRIESHARD_ACTOR_H
#define WAVEDB_TRIESHARD_ACTOR_H

#include <stdint.h>
#include <stddef.h>
#include "../Actor/actor.h"
#include "../Actor/message.h"
#include "../HBTrie/hbtrie.h"
#include "../HBTrie/mvcc.h"
#include "../Platform/platform_thread.h"
#include "wal_actor.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct trie_shard_actor_t {
    actor_t actor;
    hbtrie_t* trie;
    tx_manager_t* tx_manager;
    wal_actor_t* wal;
    ATOMIC(hbtrie_node_t*) root;
    platform_mutex_t* sync_lock;       /* Lock for sync operations */
    uint32_t chunk_size;
    uint32_t btree_node_size;
} trie_shard_actor_t;

trie_shard_actor_t** trie_shard_actors_create(size_t count, uint8_t chunk_size,
                                               uint32_t btree_node_size,
                                               tx_manager_t* tx_manager,
                                               wal_actor_t* wal);
void trie_shard_actors_destroy(trie_shard_actor_t** shards, size_t count);
void trie_shard_put(trie_shard_actor_t** shards, size_t count, path_t* path, identifier_t* value, promise_t* promise);
void trie_shard_get(trie_shard_actor_t** shards, size_t count, path_t* path, promise_t* promise);
void trie_shard_delete(trie_shard_actor_t** shards, size_t count, path_t* path, promise_t* promise);
identifier_t* trie_shard_read_sync(trie_shard_actor_t** shards, size_t count,
                                    path_t* path, transaction_id_t txn_id);

#ifdef __cplusplus
}
#endif

#endif /* WAVEDB_TRIESHARD_ACTOR_H */
