//
// Created by victor on 04/15/26.
//

#include "stale_region.h"
#include "../Util/allocator.h"
#include <stdlib.h>
#include <string.h>

#define INITIAL_CAPACITY 16

stale_region_mgr_t* stale_region_mgr_create(void) {
    stale_region_mgr_t* mgr = get_clear_memory(sizeof(stale_region_mgr_t));
    mgr->capacity = INITIAL_CAPACITY;
    mgr->regions = get_clear_memory(sizeof(stale_region_t) * mgr->capacity);
    mgr->count = 0;
    mgr->total_stale_bytes = 0;
    return mgr;
}

void stale_region_mgr_destroy(stale_region_mgr_t* mgr) {
    if (mgr == NULL) return;
    free(mgr->regions);
    free(mgr);
}

static void ensure_capacity(stale_region_mgr_t* mgr) {
    if (mgr->count < mgr->capacity) return;
    size_t new_capacity = mgr->capacity * 2;
    stale_region_t* new_regions = get_clear_memory(sizeof(stale_region_t) * new_capacity);
    memcpy(new_regions, mgr->regions, sizeof(stale_region_t) * mgr->count);
    free(mgr->regions);
    mgr->regions = new_regions;
    mgr->capacity = new_capacity;
}

void stale_region_add(stale_region_mgr_t* mgr, uint64_t offset, uint64_t length) {
    if (length == 0) return;

    // Find insertion position and check for merge with existing regions
    // We merge if the new region touches or overlaps an existing region.
    // Two regions touch if: new_start <= existing_end AND existing_start <= new_end

    uint64_t new_start = offset;
    uint64_t new_end = offset + length;

    // Find the leftmost region that could overlap (region.end >= new_start)
    size_t left = 0;
    while (left < mgr->count && mgr->regions[left].offset + mgr->regions[left].length < new_start) {
        left++;
    }

    // Find the rightmost region that could overlap (region.start <= new_end)
    size_t right = left;
    while (right < mgr->count && mgr->regions[right].offset <= new_end) {
        right++;
    }

    if (left == right) {
        // No overlapping regions - insert at position left
        ensure_capacity(mgr);
        memmove(&mgr->regions[left + 1], &mgr->regions[left],
                sizeof(stale_region_t) * (mgr->count - left));
        mgr->regions[left].offset = offset;
        mgr->regions[left].length = length;
        mgr->count++;
        mgr->total_stale_bytes += length;
    } else {
        // Merge regions [left, right) with the new region
        uint64_t merged_start = mgr->regions[left].offset;
        if (new_start < merged_start) merged_start = new_start;

        uint64_t merged_end = mgr->regions[right - 1].offset + mgr->regions[right - 1].length;
        if (new_end > merged_end) merged_end = new_end;

        // Subtract old regions from total
        for (size_t i = left; i < right; i++) {
            mgr->total_stale_bytes -= mgr->regions[i].length;
        }

        // Add merged region to total
        uint64_t merged_length = merged_end - merged_start;
        mgr->total_stale_bytes += merged_length;

        // Collapse: place merged region at left, shift remaining down
        mgr->regions[left].offset = merged_start;
        mgr->regions[left].length = merged_length;

        size_t remaining = mgr->count - right;
        if (remaining > 0) {
            memmove(&mgr->regions[left + 1], &mgr->regions[right],
                    sizeof(stale_region_t) * remaining);
        }
        mgr->count -= (right - left - 1);
    }
}

uint64_t stale_region_get_reusable(stale_region_mgr_t* mgr, uint64_t file_size,
                                   double threshold_ratio, size_t* out_count) {
    *out_count = 0;
    if (mgr->count == 0 || file_size == 0) return 0;

    uint64_t threshold_bytes = (uint64_t)(file_size * threshold_ratio);

    // Count how many regions are above threshold
    size_t result_count = 0;
    for (size_t i = 0; i < mgr->count; i++) {
        if (mgr->regions[i].length >= threshold_bytes) {
            result_count++;
        }
    }

    if (result_count == 0) return 0;

    // Allocate result array and fill it
    stale_region_t* result = get_clear_memory(sizeof(stale_region_t) * result_count);
    size_t idx = 0;
    for (size_t i = 0; i < mgr->count; i++) {
        if (mgr->regions[i].length >= threshold_bytes) {
            result[idx].offset = mgr->regions[i].offset;
            result[idx].length = mgr->regions[i].length;
            idx++;
        }
    }

    *out_count = result_count;
    return (uint64_t)(uintptr_t)result;
}

void stale_region_clear(stale_region_mgr_t* mgr) {
    mgr->count = 0;
    mgr->total_stale_bytes = 0;
}

uint64_t stale_region_total(stale_region_mgr_t* mgr) {
    if (mgr == NULL) return 0;
    return mgr->total_stale_bytes;
}

uint8_t* stale_region_serialize(stale_region_mgr_t* mgr, size_t* out_len) {
    // Format:
    //   [8 bytes] count (uint64_t)
    //   [8 bytes] total_stale_bytes (uint64_t)
    //   [16 bytes per region] offset (uint64_t) + length (uint64_t)
    size_t len = 16 + sizeof(stale_region_t) * mgr->count;
    uint8_t* data = get_clear_memory(len);

    // Write count
    memcpy(data, &mgr->count, sizeof(uint64_t));
    // Write total_stale_bytes
    memcpy(data + 8, &mgr->total_stale_bytes, sizeof(uint64_t));
    // Write regions
    if (mgr->count > 0) {
        memcpy(data + 16, mgr->regions, sizeof(stale_region_t) * mgr->count);
    }

    *out_len = len;
    return data;
}

stale_region_mgr_t* stale_region_deserialize(const uint8_t* data, size_t len) {
    if (len < 16) return NULL;

    stale_region_mgr_t* mgr = get_clear_memory(sizeof(stale_region_mgr_t));

    // Read count
    memcpy(&mgr->count, data, sizeof(uint64_t));
    // Read total_stale_bytes
    memcpy(&mgr->total_stale_bytes, data + 8, sizeof(uint64_t));

    // Validate: data must be large enough for all regions
    size_t expected_len = 16 + sizeof(stale_region_t) * mgr->count;
    if (len < expected_len) {
        free(mgr);
        return NULL;
    }

    // Allocate and copy regions
    if (mgr->count > 0) {
        mgr->capacity = mgr->count;
        // Round up to INITIAL_CAPACITY
        if (mgr->capacity < INITIAL_CAPACITY) mgr->capacity = INITIAL_CAPACITY;
        mgr->regions = get_clear_memory(sizeof(stale_region_t) * mgr->capacity);
        memcpy(mgr->regions, data + 16, sizeof(stale_region_t) * mgr->count);
    } else {
        mgr->capacity = INITIAL_CAPACITY;
        mgr->regions = get_clear_memory(sizeof(stale_region_t) * mgr->capacity);
    }

    return mgr;
}