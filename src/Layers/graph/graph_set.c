//
// Created by victor on 05/30/26.
//

#include "graph_internal.h"

// FNV-1a hash for strings
static uint32_t str_hash(const char* s) {
    uint32_t h = 2166136261u;
    while (*s) {
        h ^= (unsigned char)*s++;
        h *= 16777619u;
    }
    return h;
}

// Power-of-2 ceiling
static size_t round_pow2(size_t v) {
    v--;
    v |= v >> 1; v |= v >> 2; v |= v >> 4;
    v |= v >> 8; v |= v >> 16;
    return v + 1;
}

// Find bucket index for a vertex. Returns the first empty or matching bucket.
// Uses open addressing with linear probing.
static uint32_t find_bucket(const vertex_set_t* set, const char* vertex) {
    if (set->num_buckets == 0) return VERTEX_SET_EMPTY;
    uint32_t h = str_hash(vertex);
    uint32_t idx = h & (set->num_buckets - 1);
    for (uint32_t i = 0; i < set->num_buckets; i++) {
        uint32_t b = (idx + i) & (set->num_buckets - 1);
        if (set->buckets[b] == VERTEX_SET_EMPTY) return b;
        if (strcmp(set->vertices[set->buckets[b]], vertex) == 0) return b;
    }
    return VERTEX_SET_EMPTY;
}

void vertex_set_init(vertex_set_t* set, size_t initial_capacity) {
    memset(set, 0, sizeof(*set));
    if (initial_capacity < 8) initial_capacity = 8;
    set->capacity = round_pow2(initial_capacity);
    set->vertices = (char**)get_clear_memory(set->capacity * sizeof(char*));
    set->num_buckets = (uint32_t)(set->capacity * 2);
    set->buckets = (uint32_t*)get_clear_memory(set->num_buckets * sizeof(uint32_t));
    for (uint32_t i = 0; i < set->num_buckets; i++) {
        set->buckets[i] = VERTEX_SET_EMPTY;
    }
    set->count = 0;
}

void vertex_set_destroy(vertex_set_t* set) {
    if (!set) return;
    for (size_t i = 0; i < set->count; i++) {
        free(set->vertices[i]);
    }
    free(set->vertices);
    free(set->buckets);
    memset(set, 0, sizeof(*set));
}

static int vertex_set_resize(vertex_set_t* set, size_t new_capacity) {
    char** old_vertices = set->vertices;
    size_t old_count = set->count;
    size_t old_capacity = set->capacity;
    uint32_t* old_buckets = set->buckets;
    uint32_t old_num_buckets = set->num_buckets;

    set->capacity = round_pow2(new_capacity);
    set->vertices = (char**)get_clear_memory(set->capacity * sizeof(char*));
    set->num_buckets = (uint32_t)(set->capacity * 2);
    set->buckets = (uint32_t*)get_clear_memory(set->num_buckets * sizeof(uint32_t));
    for (uint32_t i = 0; i < set->num_buckets; i++) {
        set->buckets[i] = VERTEX_SET_EMPTY;
    }
    set->count = 0;

    for (size_t i = 0; i < old_count; i++) {
        vertex_set_add(set, old_vertices[i]);
        free(old_vertices[i]);
    }
    free(old_vertices);
    free(old_buckets);
    (void)old_capacity; // unused
    return 0;
}

int vertex_set_add(vertex_set_t* set, const char* vertex) {
    if (!set || !vertex) return -1;

    // Check duplicates
    uint32_t b = find_bucket(set, vertex);
    if (b != VERTEX_SET_EMPTY && set->buckets[b] != VERTEX_SET_EMPTY) {
        return 0; // Already present
    }

    // Resize if needed (load factor > 0.75)
    if (set->count >= set->capacity * 3 / 4) {
        vertex_set_resize(set, set->capacity * 2);
        b = find_bucket(set, vertex);
    }

    // Store string
    size_t len = strlen(vertex);
    set->vertices[set->count] = (char*)get_memory(len + 1);
    memcpy(set->vertices[set->count], vertex, len + 1);

    // Insert into hash table
    if (b == VERTEX_SET_EMPTY) {
        uint32_t h = str_hash(vertex);
        uint32_t idx = h & (set->num_buckets - 1);
        for (uint32_t i = 0; i < set->num_buckets; i++) {
            b = (idx + i) & (set->num_buckets - 1);
            if (set->buckets[b] == VERTEX_SET_EMPTY) break;
        }
    }
    set->buckets[b] = (uint32_t)set->count;
    set->count++;
    return 0;
}

int vertex_set_contains(vertex_set_t* set, const char* vertex) {
    if (!set || !vertex || set->count == 0) return 0;
    uint32_t h = str_hash(vertex);
    uint32_t idx = h & (set->num_buckets - 1);
    for (uint32_t i = 0; i < set->num_buckets; i++) {
        uint32_t b = (idx + i) & (set->num_buckets - 1);
        if (set->buckets[b] == VERTEX_SET_EMPTY) return 0;
        if (strcmp(set->vertices[set->buckets[b]], vertex) == 0) return 1;
    }
    return 0;
}

int vertex_set_intersect(vertex_set_t* result, const vertex_set_t* a, const vertex_set_t* b) {
    if (!result || !a || !b) return -1;
    vertex_set_clear(result);
    const vertex_set_t* smaller = (a->count <= b->count) ? a : b;
    const vertex_set_t* larger  = (a->count <= b->count) ? b : a;
    for (size_t i = 0; i < smaller->count; i++) {
        if (vertex_set_contains((vertex_set_t*)larger, smaller->vertices[i])) {
            vertex_set_add(result, smaller->vertices[i]);
        }
    }
    return 0;
}

int vertex_set_union(vertex_set_t* result, const vertex_set_t* a, const vertex_set_t* b) {
    if (!result || !a || !b) return -1;
    vertex_set_clear(result);
    for (size_t i = 0; i < a->count; i++) {
        vertex_set_add(result, a->vertices[i]);
    }
    for (size_t i = 0; i < b->count; i++) {
        vertex_set_add(result, b->vertices[i]);
    }
    return 0;
}

void vertex_set_clear(vertex_set_t* set) {
    if (!set) return;
    for (size_t i = 0; i < set->count; i++) {
        free(set->vertices[i]);
        set->vertices[i] = NULL;
    }
    set->count = 0;
    for (uint32_t i = 0; i < set->num_buckets; i++) {
        set->buckets[i] = VERTEX_SET_EMPTY;
    }
}

int vertex_set_copy(vertex_set_t* result, const vertex_set_t* src) {
    if (!result || !src) return -1;
    vertex_set_clear(result);
    for (size_t i = 0; i < src->count; i++) {
        if (vertex_set_add(result, src->vertices[i]) != 0) return -1;
    }
    return 0;
}
