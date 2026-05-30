#ifndef WAVEDB_LRU_ACTOR_H
#define WAVEDB_LRU_ACTOR_H

#include <stdint.h>
#include <stddef.h>
#include "../RefCounter/refcounter.h"
#include "../HBTrie/path.h"
#include "../HBTrie/identifier.h"
#include "../Actor/actor.h"
#include "../Actor/message.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct lru_actor_t {
    actor_t actor;
    struct lru_entry_t* first;     /* Most recently used */
    struct lru_entry_t* last;      /* Least recently used */
    size_t current_memory;
    size_t max_memory;
    size_t entry_count;
} lru_actor_t;

lru_actor_t* lru_actor_create(size_t max_memory_bytes);
void lru_actor_destroy(lru_actor_t* lru);

void lru_actor_get(lru_actor_t* lru, path_t* path, actor_t* reply_to);
void lru_actor_put(lru_actor_t* lru, path_t* path, identifier_t* value, actor_t* reply_to);
void lru_actor_delete(lru_actor_t* lru, path_t* path);

#ifdef __cplusplus
}
#endif

#endif /* WAVEDB_LRU_ACTOR_H */
