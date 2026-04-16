//
// Created by victor on 04/15/26.
//

#include "page_file.h"
#include "../Util/allocator.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>

#include <xxhash.h>

// posix_fadvise availability: Linux has it, macOS/BSD may not
#if defined(__linux__) || defined(__FreeBSD__) || defined(__OpenBSD__)
#define HAS_POSIX_FADVISE 1
#endif

// Forward declaration for multi-block read fallback
static uint8_t* page_file_read_node_multiblock(page_file_t* pf, uint64_t offset, size_t* out_len);

// Write a superblock to a buffer (block_size bytes), returns the buffer
static void serialize_superblock(const page_superblock_t* sb, uint8_t* buf, uint64_t block_size) {
    memset(buf, 0, block_size);
    memcpy(buf, sb->magic, 4);
    memcpy(buf + 4, &sb->version, 2);
    memcpy(buf + 6, &sb->root_offset, 8);
    memcpy(buf + 14, &sb->root_size, 8);
    memcpy(buf + 22, &sb->revision, 8);
    memcpy(buf + 30, &sb->last_txn_time, 8);
    memcpy(buf + 38, &sb->last_txn_nanos, 8);
    memcpy(buf + 46, &sb->last_txn_count, 8);
    // Compute CRC over the first 54 bytes
    uint32_t crc = XXH32(buf, 54, 0);
    memcpy(buf + 54, &crc, 4);
}

// Read a superblock from a buffer, returns 0 on success, -1 on CRC failure
static int deserialize_superblock(const uint8_t* buf, page_superblock_t* out_sb) {
    memcpy(out_sb->magic, buf, 4);
    memcpy(&out_sb->version, buf + 4, 2);
    memcpy(&out_sb->root_offset, buf + 6, 8);
    memcpy(&out_sb->root_size, buf + 14, 8);
    memcpy(&out_sb->revision, buf + 22, 8);
    memcpy(&out_sb->last_txn_time, buf + 30, 8);
    memcpy(&out_sb->last_txn_nanos, buf + 38, 8);
    memcpy(&out_sb->last_txn_count, buf + 46, 8);
    memcpy(&out_sb->crc32, buf + 54, 4);

    // Verify CRC over the first 54 bytes
    uint32_t computed = XXH32(buf, 54, 0);
    if (computed != out_sb->crc32) {
        return -1;
    }
    return 0;
}

page_file_t* page_file_create(const char* path, uint64_t block_size, uint64_t num_superblocks) {
    if (path == NULL) return NULL;

    page_file_t* pf = get_clear_memory(sizeof(page_file_t));
    pf->path = strdup(path);
    pf->fd = -1;
    pf->block_size = block_size > 0 ? block_size : PAGE_FILE_DEFAULT_BLOCK_SIZE;
    pf->num_superblocks = num_superblocks > 0 ? num_superblocks : PAGE_FILE_DEFAULT_NUM_SUPERBLOCKS;
    pf->cur_bid = pf->num_superblocks;  // First data block is after superblocks
    pf->cur_offset = 0;
    pf->revision = 0;
    pf->stale_mgr = stale_region_mgr_create();
    pf->is_writable = 0;
    platform_lock_init(&pf->lock);

    return pf;
}

void page_file_destroy(page_file_t* pf) {
    if (pf == NULL) return;

    if (pf->fd >= 0) {
        close(pf->fd);
    }
    if (pf->path != NULL) {
        free(pf->path);
    }
    if (pf->stale_mgr != NULL) {
        stale_region_mgr_destroy(pf->stale_mgr);
    }
    platform_lock_destroy(&pf->lock);
    free(pf);
}

int page_file_open(page_file_t* pf, uint8_t writable) {
    if (pf == NULL) return -1;

    if (pf->fd >= 0) {
        // Already open, close the old fd first
        close(pf->fd);
    }

    int flags = writable ? (O_RDWR | O_CREAT) : O_RDONLY;
    int mode = S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH;  // 0644

    pf->fd = open(pf->path, flags, mode);
    if (pf->fd < 0) {
        return -1;
    }
    pf->is_writable = writable;

    // Hint sequential access for better readahead
#if HAS_POSIX_FADVISE
    posix_fadvise(pf->fd, 0, 0, POSIX_FADV_SEQUENTIAL);
#endif

    // Check file size
    off_t file_size = lseek(pf->fd, 0, SEEK_END);
    if (file_size < 0) {
        close(pf->fd);
        pf->fd = -1;
        return -1;
    }

    if (file_size == 0 && writable) {
        // New file: extend to fit superblocks and write initial superblocks
        uint64_t required_size = pf->num_superblocks * pf->block_size;
        if (ftruncate(pf->fd, (off_t)required_size) != 0) {
            close(pf->fd);
            pf->fd = -1;
            return -1;
        }

        // Write initial superblocks with revision 0
        page_superblock_t sb;
        memset(&sb, 0, sizeof(sb));
        sb.magic[0] = 'W';
        sb.magic[1] = 'D';
        sb.magic[2] = 'B';
        sb.magic[3] = 'P';
        sb.version = PAGE_FILE_VERSION;
        sb.root_offset = 0;
        sb.root_size = 0;
        sb.revision = 0;

        uint8_t* blk_buf = get_clear_memory(pf->block_size);
        serialize_superblock(&sb, blk_buf, pf->block_size);

        for (uint64_t i = 0; i < pf->num_superblocks; i++) {
            ssize_t written = pwrite(pf->fd, blk_buf, pf->block_size, (off_t)(i * pf->block_size));
            if (written != (ssize_t)pf->block_size) {
                free(blk_buf);
                close(pf->fd);
                pf->fd = -1;
                return -1;
            }
        }
        free(blk_buf);

        pf->cur_bid = pf->num_superblocks;
        pf->cur_offset = 0;
        pf->revision = 0;
    } else if (file_size > 0) {
        // Existing file: read latest superblock to get revision
        page_superblock_t sb;
        if (page_file_read_superblock(pf, &sb) == 0) {
            pf->revision = sb.revision;
        }

        // Set cur_bid/cur_offset to end of file
        uint64_t total_blocks = (uint64_t)file_size / pf->block_size;
        uint64_t remainder = (uint64_t)file_size % pf->block_size;
        if (remainder > 0) {
            pf->cur_bid = total_blocks;
            pf->cur_offset = remainder;
        } else {
            pf->cur_bid = total_blocks;
            pf->cur_offset = 0;
        }
    }

    return 0;
}

uint64_t page_file_alloc_block(page_file_t* pf) {
    if (pf == NULL || !pf->is_writable) return BLK_NOT_FOUND;

    platform_lock(&pf->lock);

    // If current block is fully used, move to next block
    if (pf->cur_offset >= pf->block_size) {
        pf->cur_bid++;
        pf->cur_offset = 0;
    }

    uint64_t bid = pf->cur_bid;

    // Ensure file is large enough
    uint64_t required_end = (bid + 1) * pf->block_size;
    off_t current_size = lseek(pf->fd, 0, SEEK_END);
    if (current_size < 0) {
        platform_unlock(&pf->lock);
        return BLK_NOT_FOUND;
    }

    if ((uint64_t)current_size < required_end) {
        if (ftruncate(pf->fd, (off_t)required_end) != 0) {
            platform_unlock(&pf->lock);
            return BLK_NOT_FOUND;
        }
    }

    // Reserve the full block
    pf->cur_offset = pf->block_size;  // Mark block as fully used

    platform_unlock(&pf->lock);
    return bid;
}

int page_file_write_node(page_file_t* pf, const uint8_t* data, size_t data_len,
                          uint64_t* out_offset, uint64_t* out_bids, size_t max_bids,
                          size_t* out_num_bids) {
    if (pf == NULL || data == NULL || data_len == 0 || out_offset == NULL ||
        out_bids == NULL || max_bids == 0 || out_num_bids == NULL || !pf->is_writable) {
        return -1;
    }

    // Guard: data_len must fit in uint32_t for the size prefix
    if (data_len > UINT32_MAX) {
        return -1;
    }

    // Total write = 4-byte size prefix + data
    size_t total_len = 4 + data_len;

    platform_lock(&pf->lock);

    // Record the starting offset
    uint64_t start_bid = pf->cur_bid;
    uint64_t start_offset_in_block = pf->cur_offset;
    *out_offset = start_bid * pf->block_size + start_offset_in_block;

    size_t written = 0;
    size_t bids_count = 0;
    const uint8_t* read_ptr = data;

    // Write the 4-byte size prefix first
    uint32_t node_size = (uint32_t)data_len;

    while (written < total_len) {
        // How much space is left in the current block
        uint64_t space = pf->block_size - pf->cur_offset;
        if (space <= INDEX_BLK_META_SIZE) {
            // Not enough room for data + meta in this block, move to next
            pf->cur_bid++;
            pf->cur_offset = 0;
            space = pf->block_size;
        }

        // The usable space in this block (must leave room for IndexBlkMeta at end)
        uint64_t usable = space - INDEX_BLK_META_SIZE;
        if (usable > total_len - written) {
            usable = total_len - written;
        }

        // Write data to current position
        off_t write_pos = (off_t)(pf->cur_bid * pf->block_size + pf->cur_offset);

        // If we haven't written the size prefix yet, prepend it
        if (written < 4) {
            // We need to write the 4-byte size prefix first
            uint8_t size_buf[4];
            memcpy(size_buf, &node_size, 4);
            size_t prefix_remaining = 4 - written;
            size_t prefix_to_write = prefix_remaining < usable ? prefix_remaining : usable;

            ssize_t w = pwrite(pf->fd, size_buf + written, prefix_to_write, write_pos);
            if (w != (ssize_t)prefix_to_write) {
                platform_unlock(&pf->lock);
                return -1;
            }
            pf->cur_offset += prefix_to_write;
            written += prefix_to_write;
            usable -= prefix_to_write;

            // Update write position for remaining data
            write_pos = (off_t)(pf->cur_bid * pf->block_size + pf->cur_offset);

            if (usable == 0) {
                // Write IndexBlkMeta and continue to next block
                // Will be handled below
            }
        }

        if (usable > 0 && written >= 4) {
            // Write actual data
            size_t data_written_so_far = written - 4;
            size_t data_to_write = usable;
            if (data_to_write > data_len - data_written_so_far) {
                data_to_write = data_len - data_written_so_far;
            }

            ssize_t w = pwrite(pf->fd, read_ptr, data_to_write, write_pos);
            if (w != (ssize_t)data_to_write) {
                platform_unlock(&pf->lock);
                return -1;
            }
            read_ptr += data_to_write;
            pf->cur_offset += data_to_write;
            written += data_to_write;
            usable -= data_to_write;
        }

        // Record this block ID
        if (bids_count == 0 || out_bids[bids_count - 1] != pf->cur_bid) {
            // Check for overflow of bids array
            if (bids_count >= max_bids) {
                platform_unlock(&pf->lock);
                return -1;
            }
            out_bids[bids_count] = pf->cur_bid;
            bids_count++;
        }

        // Write IndexBlkMeta at the end of this block if we used space in it
        // Only if we've consumed data or the block is "full"
        if (pf->cur_offset > 0 || written >= total_len) {
            // Check if we need to move to the next block (current block is full)
            uint64_t remaining_space = pf->block_size - pf->cur_offset;

            if (remaining_space <= INDEX_BLK_META_SIZE || written >= total_len) {
                // Write IndexBlkMeta at the end of this block
                index_blk_meta_t meta;
                memset(&meta, 0, sizeof(meta));
                meta.next_bid = (written < total_len) ? pf->cur_bid + 1 : BLK_NOT_FOUND;
                meta.revnum_hash = (uint16_t)(pf->revision & 0xFFFF);
                meta.marker = 0xFF;

                off_t meta_pos = (off_t)((pf->cur_bid + 1) * pf->block_size - INDEX_BLK_META_SIZE);
                ssize_t w = pwrite(pf->fd, &meta, INDEX_BLK_META_SIZE, meta_pos);
                if (w != INDEX_BLK_META_SIZE) {
                    platform_unlock(&pf->lock);
                    return -1;
                }

                // Move to next block
                pf->cur_bid++;
                pf->cur_offset = 0;
            }
        }
    }

    *out_num_bids = bids_count;

    platform_unlock(&pf->lock);
    return 0;
}

uint8_t* page_file_read_node(page_file_t* pf, uint64_t offset, size_t* out_len) {
    if (pf == NULL || pf->fd < 0 || out_len == NULL) return NULL;

    *out_len = 0;

    // Optimized read path: read the entire block in one pread, then extract
    // the node data in-memory. For single-block nodes (the common case), this
    // reduces syscalls from 3 to 1.
    uint64_t bid = offset / pf->block_size;
    uint64_t block_start = bid * pf->block_size;
    uint64_t data_start_in_block = offset - block_start;

    // Read the entire block into a temporary buffer
    uint8_t* block_buf = (uint8_t*)malloc(pf->block_size);
    if (block_buf == NULL) return NULL;

    ssize_t rd = pread(pf->fd, block_buf, (size_t)pf->block_size, (off_t)block_start);
    if (rd < 0) {
        free(block_buf);
        return NULL;
    }

    size_t bytes_in_block = (size_t)rd;
    uint64_t block_data_end = (bid + 1) * pf->block_size - INDEX_BLK_META_SIZE;
    size_t data_area_len = (size_t)(block_data_end - block_start);
    if (data_area_len > bytes_in_block) {
        data_area_len = bytes_in_block;
    }

    // Extract the 4-byte size prefix from the block buffer
    if (data_start_in_block + 4 > data_area_len) {
        // Size prefix spans block boundary - fall back to multi-block read
        free(block_buf);
        return page_file_read_node_multiblock(pf, offset, out_len);
    }

    uint32_t node_size;
    memcpy(&node_size, block_buf + data_start_in_block, 4);

    // Guard against unreasonable sizes
    if (node_size == 0 || node_size > UINT32_MAX - 4) {
        free(block_buf);
        return NULL;
    }

    size_t total = 4 + (size_t)node_size;

    // Check if the entire node fits within this block's data area
    if (data_start_in_block + total <= data_area_len) {
        // Single-block node: copy directly from block buffer (0 additional preads)
        uint8_t* result = (uint8_t*)malloc(total);
        if (result == NULL) {
            free(block_buf);
            return NULL;
        }
        memcpy(result, block_buf + data_start_in_block, total);
        free(block_buf);
        *out_len = node_size;
        return result;
    }

    // Multi-block node: use block buffer for the first part, then follow
    // IndexBlkMeta links for the remainder.
    free(block_buf);
    return page_file_read_node_multiblock(pf, offset, out_len);
}

// Multi-block read: follows IndexBlkMeta links across blocks.
// Called when a node spans multiple blocks (rare for small nodes).
static uint8_t* page_file_read_node_multiblock(page_file_t* pf, uint64_t offset, size_t* out_len) {
    // Read the 4-byte size prefix first
    uint32_t node_size = 0;
    uint64_t first_bid = offset / pf->block_size;
    uint64_t first_data_start = first_bid * pf->block_size;
    uint64_t first_data_end = (first_bid + 1) * pf->block_size - INDEX_BLK_META_SIZE;
    size_t first_data_len = (size_t)(first_data_end - first_data_start);
    uint64_t data_offset_in_block = offset - first_data_start;

    if (data_offset_in_block + 4 > first_data_len) {
        // Size prefix spans block boundary
        uint8_t size_buf[4];
        size_t can_read = (size_t)(first_data_len - data_offset_in_block);
        ssize_t rd = pread(pf->fd, size_buf, can_read, (off_t)offset);
        if (rd < (ssize_t)can_read) return NULL;
        index_blk_meta_t meta;
        rd = pread(pf->fd, &meta, sizeof(meta), (off_t)((first_bid + 1) * pf->block_size - INDEX_BLK_META_SIZE));
        if (rd != sizeof(meta) || meta.next_bid == BLK_NOT_FOUND || meta.marker != 0xFF) return NULL;
        uint64_t next_data_start = meta.next_bid * pf->block_size;
        rd = pread(pf->fd, size_buf + can_read, 4 - can_read, (off_t)next_data_start);
        if (rd < (ssize_t)(4 - can_read)) return NULL;
        memcpy(&node_size, size_buf, 4);
    } else {
        ssize_t rd = pread(pf->fd, &node_size, 4, (off_t)offset);
        if (rd < 4) return NULL;
    }

    if (node_size == 0 || node_size > UINT32_MAX - 4) return NULL;

    size_t total = 4 + (size_t)node_size;
    uint8_t* result = (uint8_t*)malloc(total);
    if (result == NULL) return NULL;

    uint64_t read_offset = offset;
    uint64_t bid = offset / pf->block_size;
    size_t total_read = 0;

    while (total_read < total) {
        uint64_t block_data_end_local = (bid + 1) * pf->block_size - INDEX_BLK_META_SIZE;
        uint64_t block_start_local = bid * pf->block_size;
        uint64_t can_read = block_data_end_local - (block_start_local + (read_offset - block_start_local));

        if (can_read > total - total_read) {
            can_read = total - total_read;
        }

        if (can_read == 0) {
            free(result);
            return NULL;
        }

        ssize_t rd = pread(pf->fd, result + total_read, can_read, (off_t)read_offset);
        if (rd != (ssize_t)can_read) {
            free(result);
            return NULL;
        }
        total_read += (size_t)can_read;
        read_offset += can_read;

        if (total_read < total) {
            index_blk_meta_t meta;
            off_t meta_pos = (off_t)((bid + 1) * pf->block_size - INDEX_BLK_META_SIZE);
            rd = pread(pf->fd, &meta, INDEX_BLK_META_SIZE, meta_pos);
            if (rd != INDEX_BLK_META_SIZE || meta.next_bid == BLK_NOT_FOUND || meta.marker != 0xFF) {
                free(result);
                return NULL;
            }
            bid = meta.next_bid;
            read_offset = bid * pf->block_size;
        }
    }

    *out_len = node_size;
    return result;
}

void page_file_mark_stale(page_file_t* pf, uint64_t offset, uint64_t length) {
    if (pf == NULL || length == 0) return;
    platform_lock(&pf->lock);
    stale_region_add(pf->stale_mgr, offset, length);
    platform_unlock(&pf->lock);
}

uint64_t* page_file_get_reusable_blocks(page_file_t* pf, double threshold_ratio, size_t* out_count) {
    if (out_count == NULL) return NULL;
    *out_count = 0;
    if (pf == NULL) return NULL;

    uint64_t file_sz = page_file_size(pf);
    if (file_sz == 0) return NULL;

    platform_lock(&pf->lock);
    stale_region_t* regions = stale_region_get_reusable(pf->stale_mgr, file_sz,
                                                         threshold_ratio, out_count);
    platform_unlock(&pf->lock);

    if (regions == NULL || *out_count == 0) {
        if (regions) free(regions);
        return NULL;
    }

    // Convert stale_region_t array to uint64_t offsets array
    uint64_t* offsets = get_clear_memory(sizeof(uint64_t) * (*out_count));
    for (size_t i = 0; i < *out_count; i++) {
        offsets[i] = regions[i].offset;
    }

    free(regions);
    return offsets;
}

int page_file_write_superblock(page_file_t* pf, uint64_t root_offset, uint64_t root_size,
                               const transaction_id_t* last_txn_id) {
    if (pf == NULL || !pf->is_writable) return -1;

    platform_lock(&pf->lock);

    pf->revision++;

    page_superblock_t sb;
    memset(&sb, 0, sizeof(sb));
    sb.magic[0] = 'W';
    sb.magic[1] = 'D';
    sb.magic[2] = 'B';
    sb.magic[3] = 'P';
    sb.version = PAGE_FILE_VERSION;
    sb.root_offset = root_offset;
    sb.root_size = root_size;
    sb.revision = pf->revision;
    if (last_txn_id != NULL) {
        sb.last_txn_time = last_txn_id->time;
        sb.last_txn_nanos = last_txn_id->nanos;
        sb.last_txn_count = last_txn_id->count;
    }

    // Write to the round-robin slot
    uint64_t slot = pf->revision % pf->num_superblocks;
    uint64_t slot_offset = slot * pf->block_size;

    uint8_t* blk_buf = get_clear_memory(pf->block_size);
    serialize_superblock(&sb, blk_buf, pf->block_size);

    ssize_t written = pwrite(pf->fd, blk_buf, pf->block_size, (off_t)slot_offset);
    free(blk_buf);

    if (written != (ssize_t)pf->block_size) {
        // Rollback revision
        pf->revision--;
        platform_unlock(&pf->lock);
        return -1;
    }

    platform_unlock(&pf->lock);
    return 0;
}

int page_file_read_superblock(page_file_t* pf, page_superblock_t* out_sb) {
    if (pf == NULL || out_sb == NULL) return -1;

    memset(out_sb, 0, sizeof(page_superblock_t));

    uint8_t* blk_buf = get_clear_memory(pf->block_size);
    page_superblock_t best_sb;
    memset(&best_sb, 0, sizeof(best_sb));
    int found = 0;

    for (uint64_t i = 0; i < pf->num_superblocks; i++) {
        off_t offset = (off_t)(i * pf->block_size);
        ssize_t rd = pread(pf->fd, blk_buf, pf->block_size, offset);
        if (rd != (ssize_t)pf->block_size) {
            continue;
        }

        page_superblock_t candidate;
        if (deserialize_superblock(blk_buf, &candidate) != 0) {
            continue;  // CRC mismatch
        }

        // Check magic
        if (candidate.magic[0] != 'W' || candidate.magic[1] != 'D' ||
            candidate.magic[2] != 'B' || candidate.magic[3] != 'P') {
            continue;
        }

        // Check version
        if (candidate.version > PAGE_FILE_VERSION) {
            continue;
        }

        if (!found || candidate.revision > best_sb.revision) {
            best_sb = candidate;
            found = 1;
        }
    }

    free(blk_buf);

    if (!found) {
        return -1;
    }

    *out_sb = best_sb;
    return 0;
}

uint64_t page_file_size(page_file_t* pf) {
    if (pf == NULL || pf->fd < 0) return 0;

    struct stat st;
    if (fstat(pf->fd, &st) != 0) return 0;
    return (uint64_t)st.st_size;
}

double page_file_stale_ratio(page_file_t* pf) {
    if (pf == NULL) return 0.0;

    uint64_t file_sz = page_file_size(pf);
    if (file_sz == 0) return 0.0;

    platform_lock(&pf->lock);
    uint64_t stale = stale_region_total(pf->stale_mgr);
    platform_unlock(&pf->lock);
    return (double)stale / (double)file_sz;
}