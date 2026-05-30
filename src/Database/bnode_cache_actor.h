#ifndef WAVEDB_BNODE_CACHE_ACTOR_H
#define WAVEDB_BNODE_CACHE_ACTOR_H

#include <stdint.h>
#include <stddef.h>
#include "../Actor/actor.h"
#include "../Actor/message.h"
#include "../Storage/page_file.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct bnode_cache_actor_t bnode_cache_actor_t;

bnode_cache_actor_t* bnode_cache_actor_create(page_file_t* pf, size_t max_memory, size_t num_shards);
void bnode_cache_actor_destroy(bnode_cache_actor_t* cache);
void bnode_cache_actor_set_pool(bnode_cache_actor_t* cache, scheduler_pool_t* pool);

void bnode_cache_actor_read(bnode_cache_actor_t* cache, uint64_t offset, actor_t* reply_to);
void bnode_cache_actor_write(bnode_cache_actor_t* cache, uint64_t offset, const uint8_t* data, size_t len, actor_t* reply_to);
void bnode_cache_actor_release(bnode_cache_actor_t* cache, uint64_t offset);
void bnode_cache_actor_invalidate(bnode_cache_actor_t* cache, uint64_t offset);

#ifdef __cplusplus
}
#endif

#endif /* WAVEDB_BNODE_CACHE_ACTOR_H */
