#include "offset_remap.h"
#include "allocator.h"
#include <stdlib.h>
#include <string.h>

#define SENTINEL_EMPTY 0xFFFFFFFFFFFFFFFFULL
#define SENTINEL_TOMB  0xFFFFFFFFFFFFFFFEULL

struct offset_remap_t {
    uint64_t* keys;
    uint64_t* vals;
    size_t    capacity;
    size_t    size;
    size_t    tombstones;
};

static size_t hash_u64(uint64_t key, size_t capacity) {
    // FNV-1a-ish mix; capacity is power of two
    key ^= key >> 33;
    key *= 0xff51afd7ed558ccdULL;
    key ^= key >> 33;
    return (size_t)(key & (capacity - 1));
}

offset_remap_t* offset_remap_create(size_t initial_capacity) {
    if (initial_capacity < 16) initial_capacity = 16;
    // round up to power of two
    size_t cap = 16;
    while (cap < initial_capacity) cap <<= 1;
    offset_remap_t* m = get_clear_memory(sizeof(*m));
    m->keys = get_clear_memory(sizeof(uint64_t) * cap);
    m->vals = get_clear_memory(sizeof(uint64_t) * cap);
    // initialize keys to SENTINEL_EMPTY
    for (size_t i = 0; i < cap; i++) m->keys[i] = SENTINEL_EMPTY;
    m->capacity = cap;
    m->size = 0;
    m->tombstones = 0;
    return m;
}

void offset_remap_destroy(offset_remap_t* m) {
    if (m == NULL) return;
    free(m->keys);
    free(m->vals);
    free(m);
}

static void rehash(offset_remap_t* m, size_t new_capacity) {
    uint64_t* old_keys = m->keys;
    uint64_t* old_vals = m->vals;
    size_t old_cap = m->capacity;
    m->keys = get_clear_memory(sizeof(uint64_t) * new_capacity);
    m->vals = get_clear_memory(sizeof(uint64_t) * new_capacity);
    for (size_t i = 0; i < new_capacity; i++) m->keys[i] = SENTINEL_EMPTY;
    m->capacity = new_capacity;
    m->size = 0;
    m->tombstones = 0;
    for (size_t i = 0; i < old_cap; i++) {
        if (old_keys[i] != SENTINEL_EMPTY && old_keys[i] != SENTINEL_TOMB) {
            offset_remap_put(m, old_keys[i], old_vals[i]);
        }
    }
    free(old_keys);
    free(old_vals);
}

void offset_remap_put(offset_remap_t* m, uint64_t key, uint64_t value) {
    if (m == NULL || key == SENTINEL_EMPTY || key == SENTINEL_TOMB) return;

    // Resize if load factor > 0.7
    if ((m->size + m->tombstones) * 10 >= m->capacity * 7) {
        rehash(m, m->capacity << 1);
    }

    size_t idx = hash_u64(key, m->capacity);
    size_t first_tomb = (size_t)-1;
    for (;;) {
        if (m->keys[idx] == key) {
            m->vals[idx] = value;
            return;
        }
        if (m->keys[idx] == SENTINEL_EMPTY) {
            if (first_tomb != (size_t)-1) {
                m->keys[first_tomb] = key;
                m->vals[first_tomb] = value;
                m->tombstones--;
            } else {
                m->keys[idx] = key;
                m->vals[idx] = value;
            }
            m->size++;
            return;
        }
        if (m->keys[idx] == SENTINEL_TOMB && first_tomb == (size_t)-1) {
            first_tomb = idx;
        }
        idx = (idx + 1) & (m->capacity - 1);
    }
}

uint64_t offset_remap_get(offset_remap_t* m, uint64_t key) {
    if (m == NULL) return SENTINEL_EMPTY;
    size_t idx = hash_u64(key, m->capacity);
    for (size_t probes = 0; probes < m->capacity; probes++) {
        if (m->keys[idx] == SENTINEL_EMPTY) return SENTINEL_EMPTY;
        if (m->keys[idx] == key) return m->vals[idx];
        idx = (idx + 1) & (m->capacity - 1);
    }
    return SENTINEL_EMPTY;
}

size_t offset_remap_size(offset_remap_t* m) {
    return m == NULL ? 0 : m->size;
}