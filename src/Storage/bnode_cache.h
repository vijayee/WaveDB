//
// Created by victor on 04/15/26.
//

#ifndef BNODE_CACHE_H
#define BNODE_CACHE_H

#include <stdint.h>
#include <stddef.h>
#include "../Util/threadding.h"
#include "page_file.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct bnode_cache_item_t bnode_cache_item_t;
typedef struct bnode_cache_shard_t bnode_cache_shard_t;
typedef struct file_bnode_cache_t file_bnode_cache_t;
typedef struct bnode_cache_mgr_t bnode_cache_mgr_t;

struct bnode_cache_item_t {
    uint64_t offset;
    uint8_t* data;
    size_t data_len;
    uint8_t is_dirty;
    uint8_t invalidate_pending;  // Set when invalidated while ref_count > 0
    uint32_t ref_count;
    bnode_cache_item_t* lru_next;
    bnode_cache_item_t* lru_prev;
};

struct bnode_cache_shard_t {
    PLATFORMLOCKTYPE(lock);
    bnode_cache_item_t* lru_first;
    bnode_cache_item_t* lru_last;
    size_t dirty_count;
    size_t dirty_bytes;
    bnode_cache_item_t** buckets;
    size_t bucket_count;
    size_t item_count;
};

struct file_bnode_cache_t {
    char* filename;
    page_file_t* page_file;
    bnode_cache_shard_t* shards;
    size_t num_shards;
    size_t max_memory;
    size_t current_memory;
    size_t dirty_threshold;
    bnode_cache_mgr_t* mgr;
};

struct bnode_cache_mgr_t {
    PLATFORMLOCKTYPE(global_lock);
    file_bnode_cache_t** files;
    size_t file_count;
    size_t max_total_memory;
    size_t current_total_memory;
    size_t num_shards;
};

bnode_cache_mgr_t* bnode_cache_mgr_create(size_t max_memory, size_t num_shards);
void bnode_cache_mgr_destroy(bnode_cache_mgr_t* mgr);

file_bnode_cache_t* bnode_cache_create_file_cache(bnode_cache_mgr_t* mgr, page_file_t* pf, const char* filename);
void bnode_cache_destroy_file_cache(file_bnode_cache_t* fcache);

bnode_cache_item_t* bnode_cache_read(file_bnode_cache_t* fcache, uint64_t offset);
int bnode_cache_write(file_bnode_cache_t* fcache, uint64_t offset, const uint8_t* data, size_t data_len);
void bnode_cache_release(file_bnode_cache_t* fcache, bnode_cache_item_t* item);
int bnode_cache_flush_dirty(file_bnode_cache_t* fcache);
int bnode_cache_invalidate(file_bnode_cache_t* fcache, uint64_t offset);
size_t bnode_cache_dirty_count(file_bnode_cache_t* fcache);
size_t bnode_cache_dirty_bytes(file_bnode_cache_t* fcache);

#ifdef __cplusplus
}
#endif

#endif