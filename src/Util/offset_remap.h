#ifndef OFFSET_REMAP_H
#define OFFSET_REMAP_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Open-addressing hashmap: uint64_t key -> uint64_t value.
// UINT64_MAX is the "not found" sentinel; keys equal to UINT64_MAX are rejected.
typedef struct offset_remap_t offset_remap_t;

offset_remap_t* offset_remap_create(size_t initial_capacity);
void offset_remap_destroy(offset_remap_t* m);

void offset_remap_put(offset_remap_t* m, uint64_t key, uint64_t value);
uint64_t offset_remap_get(offset_remap_t* m, uint64_t key);  // returns UINT64_MAX if not found

size_t offset_remap_size(offset_remap_t* m);

#ifdef __cplusplus
}
#endif

#endif