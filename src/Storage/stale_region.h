//
// Created by victor on 04/15/26.
//

#ifndef STALE_REGION_H
#define STALE_REGION_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint64_t offset;
    uint64_t length;
} stale_region_t;

typedef struct {
    stale_region_t* regions;
    size_t count;
    size_t capacity;
    uint64_t total_stale_bytes;
} stale_region_mgr_t;

// Create/destroy
stale_region_mgr_t* stale_region_mgr_create(void);
void stale_region_mgr_destroy(stale_region_mgr_t* mgr);

// Add a stale region (merges with adjacent regions automatically)
void stale_region_add(stale_region_mgr_t* mgr, uint64_t offset, uint64_t length);

// Get reusable blocks above threshold ratio (0.0-1.0 of total file size)
// Returns allocated array of {offset, length} blocks and sets out_count.
// Caller must free the returned array.
stale_region_t* stale_region_get_reusable(stale_region_mgr_t* mgr, uint64_t file_size,
                                          double threshold_ratio, size_t* out_count);

// Clear all stale regions (used after compaction)
void stale_region_clear(stale_region_mgr_t* mgr);

// Get total stale bytes
uint64_t stale_region_total(stale_region_mgr_t* mgr);

// Serialize/deserialize for superblock
uint8_t* stale_region_serialize(stale_region_mgr_t* mgr, size_t* out_len);
stale_region_mgr_t* stale_region_deserialize(const uint8_t* data, size_t len);

#ifdef __cplusplus
}
#endif

#endif