#include "bnode_cache_actor.h"
#include "../Util/allocator.h"
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <io.h>
#define pread _read
#define pwrite _write
#else
#include <unistd.h>
#endif

typedef struct cache_item_t {
    uint64_t offset;
    uint8_t* data;
    size_t data_len;
    struct cache_item_t* next;
    struct cache_item_t* prev;
} cache_item_t;

struct bnode_cache_actor_t {
    actor_t actor;
    page_file_t* page_file;
    cache_item_t* first;
    cache_item_t* last;
    size_t max_memory;
    size_t current_memory;
};

static void bnode_cache_actor_dispatch(void* state, message_t* msg) {
    bnode_cache_actor_t* cache = (bnode_cache_actor_t*)state;

    switch (msg->type) {
        case BNODE_CACHE_READ: {
            bnode_cache_op_payload_t* p = (bnode_cache_op_payload_t*)msg->payload;
            uint8_t* result_data = NULL;
            size_t result_len = 0;

            for (cache_item_t* item = cache->first; item != NULL; item = item->next) {
                if (item->offset == p->offset) {
                    if (item != cache->first) {
                        if (item->prev) item->prev->next = item->next;
                        if (item->next) item->next->prev = item->prev;
                        if (item == cache->last) cache->last = item->prev;
                        item->next = cache->first;
                        item->prev = NULL;
                        if (cache->first) cache->first->prev = item;
                        cache->first = item;
                    }
                    result_data = malloc(item->data_len);
                    memcpy(result_data, item->data, item->data_len);
                    result_len = item->data_len;
                    break;
                }
            }

            if (result_data == NULL) {
                result_data = page_file_read_node(cache->page_file, p->offset, &result_len);
                if (result_data) {
                    size_t needed = sizeof(cache_item_t) + result_len;
                    while (cache->current_memory + needed > cache->max_memory && cache->last) {
                        cache_item_t* evict = cache->last;
                        if (evict->prev) evict->prev->next = NULL;
                        cache->last = evict->prev;
                        if (cache->first == evict) cache->first = NULL;
                        cache->current_memory -= sizeof(cache_item_t) + evict->data_len;
                        free(evict->data);
                        free(evict);
                    }
                    cache_item_t* item = get_clear_memory(sizeof(cache_item_t));
                    item->offset = p->offset;
                    item->data = malloc(result_len);
                    memcpy(item->data, result_data, result_len);
                    item->data_len = result_len;
                    item->next = cache->first;
                    if (cache->first) cache->first->prev = item;
                    cache->first = item;
                    if (!cache->last) cache->last = item;
                    cache->current_memory += needed;
                }
            }

            if (p->reply_to) {
                bnode_cache_result_payload_t* result = get_clear_memory(sizeof(bnode_cache_result_payload_t));
                result->offset = p->offset;
                result->data = result_data;
                result->data_len = result_len;
                message_t reply = { .type = BNODE_CACHE_READ_RESULT, .payload = result, .payload_destroy = free };
                actor_send(p->reply_to, &reply);
            }
            break;
        }
        case BNODE_CACHE_WRITE: {
            bnode_cache_op_payload_t* p = (bnode_cache_op_payload_t*)msg->payload;
            pwrite(cache->page_file->fd, p->data, p->data_len, (off_t)p->offset);
            for (cache_item_t* item = cache->first; item != NULL; item = item->next) {
                if (item->offset == p->offset) {
                    free(item->data);
                    item->data = malloc(p->data_len);
                    memcpy(item->data, p->data, p->data_len);
                    item->data_len = p->data_len;
                    goto done_write;
                }
            }
            {
                size_t needed = sizeof(cache_item_t) + p->data_len;
                while (cache->current_memory + needed > cache->max_memory && cache->last) {
                    cache_item_t* evict = cache->last;
                    if (evict->prev) evict->prev->next = NULL;
                    cache->last = evict->prev;
                    if (cache->first == evict) cache->first = NULL;
                    cache->current_memory -= sizeof(cache_item_t) + evict->data_len;
                    free(evict->data);
                    free(evict);
                }
                cache_item_t* item = get_clear_memory(sizeof(cache_item_t));
                item->offset = p->offset;
                item->data = malloc(p->data_len);
                memcpy(item->data, p->data, p->data_len);
                item->data_len = p->data_len;
                item->next = cache->first;
                if (cache->first) cache->first->prev = item;
                cache->first = item;
                if (!cache->last) cache->last = item;
                cache->current_memory += needed;
            }
done_write:
            if (p->data) free(p->data);
            break;
        }
        case BNODE_CACHE_INVALIDATE: {
            bnode_cache_op_payload_t* p = (bnode_cache_op_payload_t*)msg->payload;
            for (cache_item_t* item = cache->first; item != NULL; item = item->next) {
                if (item->offset == p->offset) {
                    if (item->prev) item->prev->next = item->next;
                    if (item->next) item->next->prev = item->prev;
                    if (item == cache->first) cache->first = item->next;
                    if (item == cache->last) cache->last = item->prev;
                    cache->current_memory -= sizeof(cache_item_t) + item->data_len;
                    free(item->data);
                    free(item);
                    break;
                }
            }
            break;
        }
        default:
            break;
    }
}

bnode_cache_actor_t* bnode_cache_actor_create(page_file_t* pf, size_t max_memory, size_t num_shards) {
    (void)num_shards;
    bnode_cache_actor_t* cache = get_clear_memory(sizeof(bnode_cache_actor_t));
    cache->page_file = pf;
    cache->max_memory = max_memory;
    actor_init(&cache->actor, cache, bnode_cache_actor_dispatch, NULL);
    return cache;
}

void bnode_cache_actor_destroy(bnode_cache_actor_t* cache) {
    if (!cache) return;
    cache_item_t* item = cache->first;
    while (item) {
        cache_item_t* next = item->next;
        free(item->data);
        free(item);
        item = next;
    }
    actor_destroy(&cache->actor);
    free(cache);
}

void bnode_cache_actor_read(bnode_cache_actor_t* cache, uint64_t offset, actor_t* reply_to) {
    bnode_cache_op_payload_t* p = get_clear_memory(sizeof(bnode_cache_op_payload_t));
    p->offset = offset;
    p->data = NULL;
    p->data_len = 0;
    p->reply_to = reply_to;
    message_t msg = { .type = BNODE_CACHE_READ, .payload = p, .payload_destroy = free };
    actor_send(&cache->actor, &msg);
}

void bnode_cache_actor_write(bnode_cache_actor_t* cache, uint64_t offset, const uint8_t* data, size_t len, actor_t* reply_to) {
    bnode_cache_op_payload_t* p = get_clear_memory(sizeof(bnode_cache_op_payload_t));
    p->offset = offset;
    p->data = malloc(len);
    memcpy(p->data, data, len);
    p->data_len = len;
    p->reply_to = reply_to;
    message_t msg = { .type = BNODE_CACHE_WRITE, .payload = p, .payload_destroy = free };
    actor_send(&cache->actor, &msg);
}

void bnode_cache_actor_release(bnode_cache_actor_t* cache, uint64_t offset) {
    bnode_cache_op_payload_t* p = get_clear_memory(sizeof(bnode_cache_op_payload_t));
    p->offset = offset;
    p->data = NULL;
    p->data_len = 0;
    p->reply_to = NULL;
    message_t msg = { .type = BNODE_CACHE_RELEASE, .payload = p, .payload_destroy = free };
    actor_send(&cache->actor, &msg);
}

void bnode_cache_actor_invalidate(bnode_cache_actor_t* cache, uint64_t offset) {
    bnode_cache_op_payload_t* p = get_clear_memory(sizeof(bnode_cache_op_payload_t));
    p->offset = offset;
    p->data = NULL;
    p->data_len = 0;
    p->reply_to = NULL;
    message_t msg = { .type = BNODE_CACHE_INVALIDATE, .payload = p, .payload_destroy = free };
    actor_send(&cache->actor, &msg);
}

void bnode_cache_actor_set_pool(bnode_cache_actor_t* cache, scheduler_pool_t* pool) {
    if (cache) cache->actor.pool = pool;
}
