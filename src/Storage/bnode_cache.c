//
// Created by victor on 04/15/26.
//

#include "bnode_cache.h"
#include "../Util/allocator.h"

#include <stdlib.h>
#include <string.h>

#define BNODE_CACHE_INITIAL_BUCKET_COUNT 64
#define BNODE_CACHE_TOMBSTONE ((bnode_cache_item_t*)(uintptr_t)-1)
#define BNODE_CACHE_FILES_INITIAL_CAPACITY 4

/* ---- Hash map helpers ---- */

static size_t hash_key(uint64_t offset, size_t bucket_count) {
    return (size_t)(offset % bucket_count);
}

static bnode_cache_item_t* shard_find(bnode_cache_shard_t* shard, uint64_t offset) {
    size_t idx = hash_key(offset, shard->bucket_count);
    for (size_t i = 0; i < shard->bucket_count; i++) {
        size_t pos = (idx + i) % shard->bucket_count;
        if (shard->buckets[pos] == NULL) return NULL;
        if (shard->buckets[pos] == BNODE_CACHE_TOMBSTONE) continue;
        if (shard->buckets[pos]->offset == offset) return shard->buckets[pos];
    }
    return NULL;
}

static void shard_resize(bnode_cache_shard_t* shard) {
    size_t new_count = shard->bucket_count * 2;
    bnode_cache_item_t** new_buckets = get_clear_memory(new_count * sizeof(bnode_cache_item_t*));

    for (size_t i = 0; i < shard->bucket_count; i++) {
        if (shard->buckets[i] != NULL && shard->buckets[i] != BNODE_CACHE_TOMBSTONE) {
            bnode_cache_item_t* item = shard->buckets[i];
            size_t idx = hash_key(item->offset, new_count);
            for (size_t j = 0; j < new_count; j++) {
                size_t pos = (idx + j) % new_count;
                if (new_buckets[pos] == NULL) {
                    new_buckets[pos] = item;
                    break;
                }
            }
        }
    }

    free(shard->buckets);
    shard->buckets = new_buckets;
    shard->bucket_count = new_count;
}

static int shard_insert(bnode_cache_shard_t* shard, bnode_cache_item_t* item) {
    /* Resize at 75% load factor */
    if (shard->item_count * 4 >= shard->bucket_count * 3) {
        shard_resize(shard);
    }

    size_t idx = hash_key(item->offset, shard->bucket_count);
    for (size_t i = 0; i < shard->bucket_count; i++) {
        size_t pos = (idx + i) % shard->bucket_count;
        if (shard->buckets[pos] == NULL || shard->buckets[pos] == BNODE_CACHE_TOMBSTONE) {
            shard->buckets[pos] = item;
            shard->item_count++;
            return 0;
        }
    }
    return -1;
}

static int shard_remove(bnode_cache_shard_t* shard, uint64_t offset) {
    size_t idx = hash_key(offset, shard->bucket_count);
    for (size_t i = 0; i < shard->bucket_count; i++) {
        size_t pos = (idx + i) % shard->bucket_count;
        if (shard->buckets[pos] == NULL) return -1;
        if (shard->buckets[pos] == BNODE_CACHE_TOMBSTONE) continue;
        if (shard->buckets[pos]->offset == offset) {
            shard->buckets[pos] = BNODE_CACHE_TOMBSTONE;
            shard->item_count--;
            return 0;
        }
    }
    return -1;
}

/* ---- LRU helpers ---- */

static void lru_remove(bnode_cache_shard_t* shard, bnode_cache_item_t* item) {
    if (item->lru_prev != NULL) {
        item->lru_prev->lru_next = item->lru_next;
    } else {
        shard->lru_first = item->lru_next;
    }
    if (item->lru_next != NULL) {
        item->lru_next->lru_prev = item->lru_prev;
    } else {
        shard->lru_last = item->lru_prev;
    }
    item->lru_next = NULL;
    item->lru_prev = NULL;
}

static void lru_push_front(bnode_cache_shard_t* shard, bnode_cache_item_t* item) {
    item->lru_prev = NULL;
    item->lru_next = shard->lru_first;
    if (shard->lru_first != NULL) {
        shard->lru_first->lru_prev = item;
    }
    shard->lru_first = item;
    if (shard->lru_last == NULL) {
        shard->lru_last = item;
    }
}

static void lru_move_to_front(bnode_cache_shard_t* shard, bnode_cache_item_t* item) {
    if (item == shard->lru_first) return;
    lru_remove(shard, item);
    lru_push_front(shard, item);
}

/* ---- Shard init/destroy ---- */

static void shard_init(bnode_cache_shard_t* shard) {
    platform_lock_init(&shard->lock);
    shard->lru_first = NULL;
    shard->lru_last = NULL;
    shard->dirty_count = 0;
    shard->dirty_bytes = 0;
    shard->bucket_count = BNODE_CACHE_INITIAL_BUCKET_COUNT;
    shard->buckets = get_clear_memory(shard->bucket_count * sizeof(bnode_cache_item_t*));
    shard->item_count = 0;
    shard->deferred_first = NULL;
}

static void shard_destroy(bnode_cache_shard_t* shard) {
    /* Free all items in the shard */
    bnode_cache_item_t* item = shard->lru_first;
    while (item != NULL) {
        bnode_cache_item_t* next = item->lru_next;
        if (item->data != NULL) {
            free(item->data);
        }
        free(item);
        item = next;
    }
    /* Free deferred-evict items */
    bnode_cache_item_t* deferred = shard->deferred_first;
    while (deferred != NULL) {
        bnode_cache_item_t* next = deferred->lru_next;
        if (deferred->data != NULL) {
            free(deferred->data);
        }
        free(deferred);
        deferred = next;
    }
    if (shard->buckets != NULL) {
        free(shard->buckets);
    }
    platform_lock_destroy(&shard->lock);
}

/* ---- Eviction ---- */

static void evict_if_needed(file_bnode_cache_t* fcache, bnode_cache_shard_t* shard) {
    /* Caller must hold shard->lock */
    while (fcache->current_memory > fcache->max_memory && shard->lru_last != NULL) {
        bnode_cache_item_t* victim = shard->lru_last;
        if (victim->ref_count > 0 || victim->is_dirty) {
            break;
        }
        lru_remove(shard, victim);
        shard_remove(shard, victim->offset);
        shard->item_count--;
        fcache->current_memory -= victim->data_len;
        if (fcache->mgr != NULL) {
            fcache->mgr->current_total_memory -= victim->data_len;
        }

        // Deferred free: mark as evict_pending, add to deferred list, call callback
        victim->evict_pending = 1;
        victim->lru_next = shard->deferred_first;
        victim->lru_prev = NULL;
        shard->deferred_first = victim;

        if (fcache->on_evict != NULL) {
            fcache->on_evict(victim->offset, fcache->on_evict_data);
        }
    }
}

/* ---- Public API ---- */

bnode_cache_mgr_t* bnode_cache_mgr_create(size_t max_memory, size_t num_shards) {
    bnode_cache_mgr_t* mgr = get_clear_memory(sizeof(bnode_cache_mgr_t));
    platform_lock_init(&mgr->global_lock);
    mgr->files = get_clear_memory(BNODE_CACHE_FILES_INITIAL_CAPACITY * sizeof(file_bnode_cache_t*));
    mgr->file_count = 0;
    mgr->max_total_memory = max_memory;
    mgr->current_total_memory = 0;
    mgr->num_shards = num_shards > 0 ? num_shards : 4;
    return mgr;
}

void bnode_cache_mgr_destroy(bnode_cache_mgr_t* mgr) {
    if (mgr == NULL) return;

    /* Destroy all file caches */
    for (size_t i = 0; i < mgr->file_count; i++) {
        if (mgr->files[i] != NULL) {
            file_bnode_cache_t* fcache = mgr->files[i];
            fcache->mgr = NULL; /* Prevent double-update of current_total_memory */
            for (size_t s = 0; s < fcache->num_shards; s++) {
                shard_destroy(&fcache->shards[s]);
            }
            if (fcache->shards != NULL) {
                free(fcache->shards);
            }
            if (fcache->filename != NULL) {
                free(fcache->filename);
            }
            free(fcache);
        }
    }
    if (mgr->files != NULL) {
        free(mgr->files);
    }
    platform_lock_destroy(&mgr->global_lock);
    free(mgr);
}

file_bnode_cache_t* bnode_cache_create_file_cache(bnode_cache_mgr_t* mgr, page_file_t* pf, const char* filename) {
    if (mgr == NULL || pf == NULL || filename == NULL) return NULL;

    platform_lock(&mgr->global_lock);

    file_bnode_cache_t* fcache = get_clear_memory(sizeof(file_bnode_cache_t));
    fcache->filename = strdup(filename);
    fcache->page_file = pf;
    fcache->num_shards = mgr->num_shards;
    fcache->max_memory = mgr->max_total_memory;
    fcache->current_memory = 0;
    fcache->dirty_threshold = mgr->max_total_memory / 2;
    fcache->mgr = mgr;

    fcache->shards = get_clear_memory(fcache->num_shards * sizeof(bnode_cache_shard_t));
    for (size_t i = 0; i < fcache->num_shards; i++) {
        shard_init(&fcache->shards[i]);
    }

    /* Grow files array if needed */
    size_t capacity = BNODE_CACHE_FILES_INITIAL_CAPACITY;
    while (mgr->file_count >= capacity) {
        capacity *= 2;
    }
    /* Re-allocate if we are at capacity */
    if (mgr->file_count >= BNODE_CACHE_FILES_INITIAL_CAPACITY) {
        /* Check if we need to grow */
        size_t current_cap = BNODE_CACHE_FILES_INITIAL_CAPACITY;
        while (current_cap <= mgr->file_count) {
            current_cap *= 2;
        }
        if (current_cap > BNODE_CACHE_FILES_INITIAL_CAPACITY) {
            file_bnode_cache_t** new_files = get_clear_memory(current_cap * sizeof(file_bnode_cache_t*));
            memcpy(new_files, mgr->files, mgr->file_count * sizeof(file_bnode_cache_t*));
            free(mgr->files);
            mgr->files = new_files;
        }
    }

    mgr->files[mgr->file_count] = fcache;
    mgr->file_count++;

    platform_unlock(&mgr->global_lock);
    return fcache;
}

void bnode_cache_destroy_file_cache(file_bnode_cache_t* fcache) {
    if (fcache == NULL) return;

    bnode_cache_mgr_t* mgr = fcache->mgr;

    if (mgr != NULL) {
        platform_lock(&mgr->global_lock);

        /* Update total memory before destroying */
        mgr->current_total_memory -= fcache->current_memory;

        /* Remove from files array */
        for (size_t i = 0; i < mgr->file_count; i++) {
            if (mgr->files[i] == fcache) {
                mgr->files[i] = mgr->files[mgr->file_count - 1];
                mgr->files[mgr->file_count - 1] = NULL;
                mgr->file_count--;
                break;
            }
        }
        fcache->mgr = NULL;

        platform_unlock(&mgr->global_lock);
    }

    /* Destroy all shards (frees items) */
    for (size_t s = 0; s < fcache->num_shards; s++) {
        shard_destroy(&fcache->shards[s]);
    }
    if (fcache->shards != NULL) {
        free(fcache->shards);
    }
    if (fcache->filename != NULL) {
        free(fcache->filename);
    }
    free(fcache);
}

bnode_cache_item_t* bnode_cache_read(file_bnode_cache_t* fcache, uint64_t offset) {
    if (fcache == NULL) return NULL;

    size_t shard_idx = (size_t)(offset % fcache->num_shards);
    bnode_cache_shard_t* shard = &fcache->shards[shard_idx];

    platform_lock(&shard->lock);

    /* Look up in hash map */
    bnode_cache_item_t* item = shard_find(shard, offset);
    if (item != NULL) {
        item->ref_count++;
        lru_move_to_front(shard, item);
        platform_unlock(&shard->lock);
        return item;
    }

    /* Cache miss: read from page file */
    if (fcache->page_file == NULL || fcache->page_file->fd < 0) {
        platform_unlock(&shard->lock);
        return NULL;
    }

    size_t data_len = 0;
    uint8_t* data = page_file_read_node(fcache->page_file, offset, &data_len);
    if (data == NULL) {
        platform_unlock(&shard->lock);
        return NULL;
    }

    /* page_file_read_node returns buffer with 4-byte size prefix.
       data_len is the size WITHOUT the prefix. Total = 4 + data_len. */
    size_t total_len = 4 + data_len;

    item = get_clear_memory(sizeof(bnode_cache_item_t));
    item->offset = offset;
    item->data = data;
    item->data_len = total_len;
    item->is_dirty = 0;
    item->ref_count = 1;

    shard_insert(shard, item);
    lru_push_front(shard, item);

    fcache->current_memory += total_len;
    if (fcache->mgr != NULL) {
        fcache->mgr->current_total_memory += total_len;
    }

    evict_if_needed(fcache, shard);

    platform_unlock(&shard->lock);
    return item;
}

int bnode_cache_write(file_bnode_cache_t* fcache, uint64_t offset, const uint8_t* data, size_t data_len) {
    if (fcache == NULL || data == NULL || data_len == 0) return -1;

    size_t shard_idx = (size_t)(offset % fcache->num_shards);
    bnode_cache_shard_t* shard = &fcache->shards[shard_idx];

    platform_lock(&shard->lock);

    /* Check if offset already exists */
    bnode_cache_item_t* item = shard_find(shard, offset);
    if (item != NULL) {
        /* Replace data */
        size_t old_len = item->data_len;

        /* Update dirty tracking */
        if (!item->is_dirty) {
            shard->dirty_count++;
            shard->dirty_bytes += data_len;
            item->is_dirty = 1;
        } else {
            shard->dirty_bytes = shard->dirty_bytes - old_len + data_len;
        }

        /* Update memory tracking */
        fcache->current_memory = fcache->current_memory - old_len + data_len;
        if (fcache->mgr != NULL) {
            fcache->mgr->current_total_memory = fcache->mgr->current_total_memory - old_len + data_len;
        }

        free(item->data);
        item->data = get_clear_memory(data_len);
        memcpy(item->data, data, data_len);
        item->data_len = data_len;

        lru_move_to_front(shard, item);
    } else {
        /* Create new item */
        item = get_clear_memory(sizeof(bnode_cache_item_t));
        item->offset = offset;
        item->data = get_clear_memory(data_len);
        memcpy(item->data, data, data_len);
        item->data_len = data_len;
        item->is_dirty = 1;
        item->ref_count = 0;

        shard_insert(shard, item);
        lru_push_front(shard, item);

        shard->dirty_count++;
        shard->dirty_bytes += data_len;

        fcache->current_memory += data_len;
        if (fcache->mgr != NULL) {
            fcache->mgr->current_total_memory += data_len;
        }
    }

    evict_if_needed(fcache, shard);

    platform_unlock(&shard->lock);
    return 0;
}

void bnode_cache_release(file_bnode_cache_t* fcache, bnode_cache_item_t* item) {
    if (fcache == NULL || item == NULL) return;

    size_t shard_idx = (size_t)(item->offset % fcache->num_shards);
    bnode_cache_shard_t* shard = &fcache->shards[shard_idx];

    platform_lock(&shard->lock);

    if (item->ref_count > 0) {
        item->ref_count--;
    }

    /* If invalidated while we held a reference, free it now */
    if (item->ref_count == 0 && item->invalidate_pending) {
        shard_remove(shard, item->offset);
        lru_remove(shard, item);
        if (item->is_dirty) {
            shard->dirty_count--;
            shard->dirty_bytes -= item->data_len;
        }
        shard->item_count--;
        fcache->current_memory -= item->data_len;
        if (fcache->mgr != NULL) {
            fcache->mgr->current_total_memory -= item->data_len;
        }
        if (fcache->page_file != NULL) {
            page_file_mark_stale(fcache->page_file, item->offset, item->data_len);
        }
        if (item->data != NULL) {
            free(item->data);
        }
        free(item);
        platform_unlock(&shard->lock);
        return;
    }

    /* Move to front on release so recently released items are last to evict */
    if (item->ref_count == 0 && shard_find(shard, item->offset) != NULL) {
        lru_move_to_front(shard, item);
    }

    platform_unlock(&shard->lock);
}

static int compare_items_by_offset(const void* a, const void* b) {
    const bnode_cache_item_t* item_a = *(const bnode_cache_item_t* const*)a;
    const bnode_cache_item_t* item_b = *(const bnode_cache_item_t* const*)b;
    if (item_a->offset < item_b->offset) return -1;
    if (item_a->offset > item_b->offset) return 1;
    return 0;
}

int bnode_cache_flush_dirty(file_bnode_cache_t* fcache) {
    if (fcache == NULL) return -1;

    /* 1. Collect all dirty items from all shards */
    size_t total_dirty = 0;
    for (size_t s = 0; s < fcache->num_shards; s++) {
        platform_lock(&fcache->shards[s].lock);
        total_dirty += fcache->shards[s].dirty_count;
        platform_unlock(&fcache->shards[s].lock);
    }
    if (total_dirty == 0) return 0;

    bnode_cache_item_t** dirty_items = get_clear_memory(total_dirty * sizeof(bnode_cache_item_t*));
    size_t dirty_idx = 0;

    for (size_t s = 0; s < fcache->num_shards; s++) {
        bnode_cache_shard_t* shard = &fcache->shards[s];
        platform_lock(&shard->lock);

        /* Walk the LRU to find dirty items, pin them to prevent UAF */
        bnode_cache_item_t* item = shard->lru_first;
        while (item != NULL) {
            if (item->is_dirty && !item->invalidate_pending) {
                if (dirty_idx < total_dirty) {
                    item->ref_count++;
                    dirty_items[dirty_idx++] = item;
                }
            }
            item = item->lru_next;
        }

        platform_unlock(&shard->lock);
    }

    if (dirty_idx == 0) {
        free(dirty_items);
        return 0;
    }

    /* 2. Sort by offset (ascending) for sequential I/O */
    qsort(dirty_items, dirty_idx, sizeof(bnode_cache_item_t*), compare_items_by_offset);

    /* 3. Write each dirty item */
    for (size_t i = 0; i < dirty_idx; i++) {
        bnode_cache_item_t* item = dirty_items[i];
        uint64_t old_offset = item->offset;

        /* Write to page file (skip 4-byte size prefix) */
        uint64_t new_offset = 0;
        uint64_t bids[64] = {0};
        size_t num_bids = 0;

        int rc = page_file_write_node(fcache->page_file,
                                       item->data + 4, item->data_len - 4,
                                       &new_offset, bids, 64, &num_bids);
        if (rc != 0) {
            /* Release pins for remaining items (including this one) */
            for (size_t j = i; j < dirty_idx; j++) {
                bnode_cache_release(fcache, dirty_items[j]);
            }
            free(dirty_items);
            return -1;
        }

        /* Mark old offset as stale */
        page_file_mark_stale(fcache->page_file, old_offset, item->data_len);

        /* Determine old and new shard indices */
        size_t old_shard_idx = (size_t)(old_offset % fcache->num_shards);
        size_t new_shard_idx = (size_t)(new_offset % fcache->num_shards);

        if (old_shard_idx == new_shard_idx) {
            /* Same shard: update in place */
            bnode_cache_shard_t* shard = &fcache->shards[old_shard_idx];
            platform_lock(&shard->lock);

            /* Remove from hash map at old offset, insert at new offset */
            shard_remove(shard, old_offset);
            item->offset = new_offset;
            item->is_dirty = 0;
            shard_insert(shard, item);

            shard->dirty_count--;
            shard->dirty_bytes -= item->data_len;

            /* Release our pin (under lock) */
            if (item->ref_count > 0) item->ref_count--;

            platform_unlock(&shard->lock);
        } else {
            /* Different shards: move between shards */
            bnode_cache_shard_t* old_shard = &fcache->shards[old_shard_idx];
            bnode_cache_shard_t* new_shard = &fcache->shards[new_shard_idx];

            platform_lock(&old_shard->lock);
            shard_remove(old_shard, old_offset);
            lru_remove(old_shard, item);
            old_shard->item_count--;
            old_shard->dirty_count--;
            old_shard->dirty_bytes -= item->data_len;
            if (item->ref_count > 0) item->ref_count--;
            platform_unlock(&old_shard->lock);

            item->offset = new_offset;
            item->is_dirty = 0;

            platform_lock(&new_shard->lock);
            shard_insert(new_shard, item);
            lru_push_front(new_shard, item);
            new_shard->item_count++;
            platform_unlock(&new_shard->lock);
        }
    }

    free(dirty_items);
    return 0;
}

int bnode_cache_invalidate(file_bnode_cache_t* fcache, uint64_t offset) {
    if (fcache == NULL) return -1;

    size_t shard_idx = (size_t)(offset % fcache->num_shards);
    bnode_cache_shard_t* shard = &fcache->shards[shard_idx];

    platform_lock(&shard->lock);

    bnode_cache_item_t* item = shard_find(shard, offset);
    if (item == NULL) {
        platform_unlock(&shard->lock);
        return -1;
    }

    if (item->ref_count > 0) {
        // Item is in use by another thread — mark for deletion on release
        // rather than freeing it now (would cause use-after-free).
        // bnode_cache_release will check this flag and free the item.
        item->invalidate_pending = 1;
        item->is_dirty = 0;  // Don't flush a doomed item
        platform_unlock(&shard->lock);
        return 0;  // Deferred invalidation
    }

    /* Remove from hash map */
    shard_remove(shard, offset);

    /* Remove from LRU list */
    lru_remove(shard, item);

    /* Update dirty tracking */
    if (item->is_dirty) {
        shard->dirty_count--;
        shard->dirty_bytes -= item->data_len;
    }

    shard->item_count--;

    /* Update memory tracking */
    fcache->current_memory -= item->data_len;
    if (fcache->mgr != NULL) {
        fcache->mgr->current_total_memory -= item->data_len;
    }

    /* Mark stale in page file */
    if (fcache->page_file != NULL) {
        page_file_mark_stale(fcache->page_file, offset, item->data_len);
    }

    /* Free item */
    if (item->data != NULL) {
        free(item->data);
    }
    free(item);

    platform_unlock(&shard->lock);
    return 0;
}

size_t bnode_cache_dirty_count(file_bnode_cache_t* fcache) {
    if (fcache == NULL) return 0;

    size_t total = 0;
    for (size_t i = 0; i < fcache->num_shards; i++) {
        total += fcache->shards[i].dirty_count;
    }
    return total;
}

size_t bnode_cache_dirty_bytes(file_bnode_cache_t* fcache) {
    if (fcache == NULL) return 0;

    size_t total = 0;
    for (size_t i = 0; i < fcache->num_shards; i++) {
        total += fcache->shards[i].dirty_bytes;
    }
    return total;
}

void bnode_cache_complete_evict(file_bnode_cache_t* fcache, uint64_t offset) {
    if (fcache == NULL) return;

    size_t shard_idx = (size_t)(offset % fcache->num_shards);
    bnode_cache_shard_t* shard = &fcache->shards[shard_idx];

    platform_lock(&shard->lock);

    bnode_cache_item_t** pp = &shard->deferred_first;
    while (*pp != NULL) {
        if ((*pp)->offset == offset && (*pp)->evict_pending) {
            bnode_cache_item_t* item = *pp;
            *pp = item->lru_next;
            if (item->data != NULL) {
                free(item->data);
            }
            free(item);
            platform_unlock(&shard->lock);
            return;
        }
        pp = &(*pp)->lru_next;
    }

    platform_unlock(&shard->lock);
}
