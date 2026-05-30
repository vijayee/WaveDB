#include "lru_actor.h"
#include "../Util/allocator.h"
#include <stdlib.h>
#include <string.h>

typedef struct lru_entry_t {
    path_t* path;
    uint64_t key_hash;
    identifier_t* value;
    size_t memory_size;
    struct lru_entry_t* next;
    struct lru_entry_t* prev;
} lru_entry_t;

static size_t estimate_entry_size(path_t* path, identifier_t* value) {
    size_t s = sizeof(lru_entry_t);
    /* path_compare and path_hash operate on identifiers; use path_length for
       a rough byte-count estimate. Each identifier's length is already counted. */
    for (int i = 0; i < path->identifiers.length; i++) {
        s += path->identifiers.data[i]->length;
    }
    s += value->length;
    return s;
}

static void lru_actor_dispatch(void* state, message_t* msg) {
    lru_actor_t* lru = (lru_actor_t*)state;

    switch (msg->type) {
        case LRU_GET: {
            lru_op_payload_t* p = (lru_op_payload_t*)msg->payload;
            identifier_t* found = NULL;
            uint64_t hash = path_hash(p->path);
            for (lru_entry_t* e = lru->first; e != NULL; e = e->next) {
                if (e->key_hash == hash && path_compare(e->path, p->path) == 0) {
                    found = REFERENCE(e->value, identifier_t);
                    /* Move to front */
                    if (e != lru->first) {
                        if (e->prev) e->prev->next = e->next;
                        if (e->next) e->next->prev = e->prev;
                        if (e == lru->last) lru->last = e->prev;
                        e->next = lru->first;
                        e->prev = NULL;
                        if (lru->first) lru->first->prev = e;
                        lru->first = e;
                    }
                    break;
                }
            }
            if (p->reply_to) {
                lru_result_payload_t* result = get_clear_memory(sizeof(lru_result_payload_t));
                result->path = NULL;
                result->value = found;
                message_t reply = { .type = LRU_GET_RESULT, .payload = result, .payload_destroy = free };
                actor_send(p->reply_to, &reply);
            }
            DESTROY(p->path, path);
            break;
        }
        case LRU_PUT: {
            lru_op_payload_t* p = (lru_op_payload_t*)msg->payload;
            size_t mem = estimate_entry_size(p->path, p->value);
            while (lru->current_memory + mem > lru->max_memory && lru->last != NULL) {
                lru_entry_t* oldest = lru->last;
                if (oldest->prev) oldest->prev->next = NULL;
                lru->last = oldest->prev;
                if (lru->first == oldest) lru->first = NULL;
                lru->current_memory -= oldest->memory_size;
                lru->entry_count--;
                DESTROY(oldest->path, path);
                DESTROY(oldest->value, identifier);
                free(oldest);
            }
            lru_entry_t* entry = get_clear_memory(sizeof(lru_entry_t));
            entry->path = REFERENCE(p->path, path_t);
            entry->key_hash = path_hash(p->path);
            entry->value = REFERENCE(p->value, identifier_t);
            entry->memory_size = mem;
            entry->next = lru->first;
            if (lru->first) lru->first->prev = entry;
            lru->first = entry;
            if (!lru->last) lru->last = entry;
            lru->current_memory += mem;
            lru->entry_count++;
            DESTROY(p->path, path);
            DESTROY(p->value, identifier);
            break;
        }
        case LRU_DELETE: {
            lru_op_payload_t* p = (lru_op_payload_t*)msg->payload;
            uint64_t hash = path_hash(p->path);
            for (lru_entry_t* e = lru->first; e != NULL; e = e->next) {
                if (e->key_hash == hash && path_compare(e->path, p->path) == 0) {
                    if (e->prev) e->prev->next = e->next;
                    if (e->next) e->next->prev = e->prev;
                    if (e == lru->first) lru->first = e->next;
                    if (e == lru->last) lru->last = e->prev;
                    lru->current_memory -= e->memory_size;
                    lru->entry_count--;
                    DESTROY(e->path, path);
                    DESTROY(e->value, identifier);
                    free(e);
                    break;
                }
            }
            DESTROY(p->path, path);
            break;
        }
        default:
            break;
    }
}

lru_actor_t* lru_actor_create(size_t max_memory_bytes) {
    lru_actor_t* lru = get_clear_memory(sizeof(lru_actor_t));
    lru->max_memory = max_memory_bytes ? max_memory_bytes : (size_t)-1;
    actor_init(&lru->actor, lru, lru_actor_dispatch, NULL);
    return lru;
}

void lru_actor_destroy(lru_actor_t* lru) {
    if (!lru) return;
    lru_entry_t* e = lru->first;
    while (e) {
        lru_entry_t* next = e->next;
        DESTROY(e->path, path);
        DESTROY(e->value, identifier);
        free(e);
        e = next;
    }
    actor_destroy(&lru->actor);
    free(lru);
}

void lru_actor_get(lru_actor_t* lru, path_t* path, actor_t* reply_to) {
    lru_op_payload_t* p = get_clear_memory(sizeof(lru_op_payload_t));
    p->path = REFERENCE(path, path_t);
    p->value = NULL;
    p->reply_to = reply_to;
    message_t msg = { .type = LRU_GET, .payload = p, .payload_destroy = free };
    actor_send(&lru->actor, &msg);
}

void lru_actor_put(lru_actor_t* lru, path_t* path, identifier_t* value, actor_t* reply_to) {
    lru_op_payload_t* p = get_clear_memory(sizeof(lru_op_payload_t));
    p->path = REFERENCE(path, path_t);
    p->value = REFERENCE(value, identifier_t);
    p->reply_to = reply_to;
    message_t msg = { .type = LRU_PUT, .payload = p, .payload_destroy = free };
    actor_send(&lru->actor, &msg);
}

void lru_actor_delete(lru_actor_t* lru, path_t* path) {
    lru_op_payload_t* p = get_clear_memory(sizeof(lru_op_payload_t));
    p->path = REFERENCE(path, path_t);
    p->value = NULL;
    p->reply_to = NULL;
    message_t msg = { .type = LRU_DELETE, .payload = p, .payload_destroy = free };
    actor_send(&lru->actor, &msg);
}
