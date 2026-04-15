//
// Created by victor on 04/15/26.
//

#ifndef PAGE_FILE_H
#define PAGE_FILE_H

#include <stdint.h>
#include <stddef.h>
#include "../Util/threadding.h"
#include "stale_region.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PAGE_FILE_MAGIC 0x57444250  // "WDBP"
#define PAGE_FILE_VERSION 1
#define PAGE_FILE_DEFAULT_BLOCK_SIZE 4096
#define PAGE_FILE_DEFAULT_NUM_SUPERBLOCKS 2
#define INDEX_BLK_META_SIZE 16
#define PAGE_FILE_SUPERBLOCK_SIZE 48  // magic(4)+version(2)+root_offset(8)+root_size(8)+revnum(8)+crc32(4)+padding

typedef struct {
    uint8_t magic[4];         // "WDBP"
    uint16_t version;          // Format version
    uint64_t root_offset;      // Offset of root hbtrie_node's btree
    uint64_t root_size;        // Serialized size of root btree
    uint64_t revision;        // Monotonically increasing revision number
    uint32_t crc32;            // CRC32 of all preceding fields
} page_superblock_t;

typedef struct {
    uint64_t next_bid;         // Next block for multi-block nodes (BLK_NOT_FOUND if last)
    uint16_t revnum_hash;      // Low 16 bits of superblock revision (for consistency check)
    uint8_t reserved[5];
    uint8_t marker;           // 0xFF for bnode blocks
} index_blk_meta_t;

#define BLK_NOT_FOUND ((uint64_t)-1)

typedef struct {
    char* path;                          // File path
    int fd;                              // File descriptor
    uint64_t block_size;                  // Block size in bytes
    uint64_t num_superblocks;             // Number of superblock copies
    uint64_t cur_bid;                     // Current allocation block ID
    uint64_t cur_offset;                  // Current offset within cur_bid's block
    uint64_t revision;                    // Current revision number
    stale_region_mgr_t* stale_mgr;       // Stale region manager
    PLATFORMLOCKTYPE(lock);              // Mutex for allocation and superblock writes
    uint8_t is_writable;                  // 1 if file opened for writing
} page_file_t;

// Create/destroy
page_file_t* page_file_create(const char* path, uint64_t block_size, uint64_t num_superblocks);
void page_file_destroy(page_file_t* pf);

// Open existing file for reading (and optionally writing)
int page_file_open(page_file_t* pf, uint8_t writable);

// Allocate a new block at end of file
uint64_t page_file_alloc_block(page_file_t* pf);

// Write a node's data to the file, spanning blocks as needed.
// Returns the offset of the first byte. Sets out_bid to the first block ID.
// out_bids array must be large enough for ceil(data_len / (block_size - INDEX_BLK_META_SIZE)) + 1 blocks.
// Caller provides out_bids array; function fills it and sets out_num_bids.
int page_file_write_node(page_file_t* pf, const uint8_t* data, size_t data_len,
                          uint64_t* out_offset, uint64_t* out_bids, size_t* out_num_bids);

// Read a node from the file at the given offset.
// Reads first 4 bytes for size, then reads the full node.
// Returns allocated buffer; caller must free().
// Returns NULL on error.
uint8_t* page_file_read_node(page_file_t* pf, uint64_t offset, size_t* out_len);

// Mark a region as stale (old version of a CoW node)
void page_file_mark_stale(page_file_t* pf, uint64_t offset, uint64_t length);

// Get reusable blocks from stale regions above threshold
uint64_t* page_file_get_reusable_blocks(page_file_t* pf, double threshold_ratio, size_t* out_count);

// Write superblock to the next slot (round-robin among num_superblocks)
int page_file_write_superblock(page_file_t* pf, uint64_t root_offset, uint64_t root_size);

// Read the latest valid superblock
int page_file_read_superblock(page_file_t* pf, page_superblock_t* out_sb);

// Get total file size
uint64_t page_file_size(page_file_t* pf);

// Get current stale ratio (0.0-1.0)
double page_file_stale_ratio(page_file_t* pf);

#ifdef __cplusplus
}
#endif

#endif