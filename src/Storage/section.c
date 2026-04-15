//
// Created by victor on 03/13/26.
//

#include "section.h"
#include "../Util/allocator.h"
#include "../Util/log.h"
#include "../Util/mkdir_p.h"
#include "../Util/path_join.h"
#include "../Util/memory_pool.h"
#include <cbor.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>  // For memcpy
#include <arpa/inet.h>

#ifdef _WIN32
#include <io.h>
#define F_OK 0
#define access _access
#else
#include <unistd.h>
#include <sys/uio.h>  // For writev()
#endif

// Helper: write uint64_t in network byte order
static void write_uint64(uint8_t* buf, uint64_t val) {
    uint32_t high = htonl((uint32_t)(val >> 32));
    uint32_t low = htonl((uint32_t)(val & 0xFFFFFFFF));
    memcpy(buf, &high, sizeof(uint32_t));
    memcpy(buf + 4, &low, sizeof(uint32_t));
}

// Helper: read uint64_t in network byte order
static uint64_t read_uint64(const uint8_t* buf) {
    uint32_t high, low;
    memcpy(&high, buf, sizeof(uint32_t));
    memcpy(&low, buf + 4, sizeof(uint32_t));
    return ((uint64_t)ntohl(high) << 32) | ntohl(low);
}

// Forward declarations
static int section_next_offset(section_t* section, size_t total_bytes, size_t* offset);
static void section_save_fragments(section_t* section);
static int _section_deallocate(section_t* section, size_t offset, size_t total_bytes);

// Fragment functions
fragment_t* fragment_create(size_t start, size_t end) {
    fragment_t* fragment = (fragment_t*)memory_pool_alloc(sizeof(fragment_t));
    if (fragment) {
        fragment->start = start;
        fragment->end = end;
    }
    return fragment;
}

void fragment_destroy(fragment_t* fragment) {
    memory_pool_free(fragment, sizeof(fragment_t));
}

cbor_item_t* fragment_to_cbor(fragment_t* fragment) {
    cbor_item_t* array = cbor_new_definite_array(2);
    bool success = cbor_array_push(array, cbor_move(cbor_build_uint64(fragment->start)));
    success &= cbor_array_push(array, cbor_move(cbor_build_uint64(fragment->end)));
    if (!success) {
        cbor_decref(&array);
        return NULL;
    }
    return array;
}

fragment_t* cbor_to_fragment(cbor_item_t* cbor) {
    fragment_t* fragment = get_clear_memory(sizeof(fragment_t));
    fragment->start = cbor_get_uint64(cbor_move(cbor_array_get(cbor, 0)));
    fragment->end = cbor_get_uint64(cbor_move(cbor_array_get(cbor, 1)));
    return fragment;
}

// Fragment list functions
fragment_list_t* fragment_list_create(void) {
    fragment_list_t* list = get_clear_memory(sizeof(fragment_list_t));
    list->fragments = NULL;
    list->count = 0;
    list->capacity = 0;
    list->total_free_space = 0;
    return list;
}

void fragment_list_destroy(fragment_list_t* list) {
    if (list->fragments != NULL) {
        free(list->fragments);
    }
    free(list);
}

// Helper: calculate fragment size
static inline size_t fragment_size(const fragment_t* frag) {
    return frag->end - frag->start + 1;
}

// Helper: grow capacity if needed
static void fragment_list_ensure_capacity(fragment_list_t* list, size_t needed) {
    if (list->capacity < needed) {
        size_t new_capacity = list->capacity == 0 ? 8 : list->capacity * 2;
        if (new_capacity < needed) {
            new_capacity = needed;
        }
        list->fragments = realloc(list->fragments, new_capacity * sizeof(fragment_t));
        list->capacity = new_capacity;
    }
}

// Insert fragment maintaining sorted order by size (ascending)
void fragment_list_insert(fragment_list_t* list, fragment_t* fragment) {
    // Grow array if needed
    fragment_list_ensure_capacity(list, list->count + 1);

    // Find insertion point using binary search
    size_t frag_size = fragment_size(fragment);
    size_t left = 0, right = list->count;

    while (left < right) {
        size_t mid = left + (right - left) / 2;
        size_t mid_size = fragment_size(&list->fragments[mid]);

        if (mid_size < frag_size) {
            left = mid + 1;
        } else {
            right = mid;
        }
    }

    // Shift elements to make room
    if (left < list->count) {
        memmove(&list->fragments[left + 1], &list->fragments[left],
                (list->count - left) * sizeof(fragment_t));
    }

    // Insert the fragment
    list->fragments[left] = *fragment;
    list->count++;
    list->total_free_space += frag_size;

    // Free the original fragment (we copied it)
    free(fragment);
}

// Binary search for first-fit fragment
int fragment_list_find_fit(fragment_list_t* list, size_t size, size_t* offset) {
    // Quick rejection
    if (list->count == 0 || list->total_free_space < size) {
        return 1;  // No fit
    }

    // Binary search for smallest fragment >= size
    size_t left = 0, right = list->count;
    while (left < right) {
        size_t mid = left + (right - left) / 2;
        size_t mid_size = fragment_size(&list->fragments[mid]);

        if (mid_size < size) {
            left = mid + 1;
        } else {
            right = mid;
        }
    }

    // Check if we found a fit
    if (left >= list->count) {
        return 1;  // No fit
    }

    // Found a fit - extract the fragment
    fragment_t* frag = &list->fragments[left];
    *offset = frag->start;

    size_t frag_size = fragment_size(frag);
    size_t remaining = frag_size - size;

    // Update total free space
    list->total_free_space -= size;

    if (remaining == 0) {
        // Remove fragment from array
        if (left < list->count - 1) {
            memmove(&list->fragments[left], &list->fragments[left + 1],
                    (list->count - left - 1) * sizeof(fragment_t));
        }
        list->count--;
    } else {
        // Shrink fragment (keep remaining space)
        // Fragment size changed - need to re-sort to maintain sorted order
        size_t new_start = frag->start + size;
        size_t new_end = frag->end;

        // Remove fragment from current position
        if (left < list->count - 1) {
            memmove(&list->fragments[left], &list->fragments[left + 1],
                    (list->count - left - 1) * sizeof(fragment_t));
        }
        list->count--;

        // Re-insert with new size to maintain sorted order
        fragment_t* new_frag = fragment_create(new_start, new_end);
        fragment_list_insert(list, new_frag);
    }

    return 0;  // Success
}

cbor_item_t* fragment_list_to_cbor(fragment_list_t* list) {
    cbor_item_t* array = cbor_new_definite_array(list->count);
    bool success = true;

    // Iterate through sorted array
    for (size_t i = 0; i < list->count; i++) {
        success = cbor_array_push(array, cbor_move(fragment_to_cbor(&list->fragments[i])));
        if (!success) {
            cbor_decref(&array);
            return NULL;
        }
    }
    return array;
}

fragment_list_t* cbor_to_fragment_list(cbor_item_t* cbor) {
    fragment_list_t* list = fragment_list_create();
    size_t size = cbor_array_size(cbor);

    for (size_t i = 0; i < size; i++) {
        fragment_t* frag = cbor_to_fragment(cbor_move(cbor_array_get(cbor, i)));
        fragment_list_insert(list, frag);  // Will sort by size
    }

    return list;
}

// Section functions
section_t* section_create(char* path, char* meta_path, size_t size, size_t id) {
    section_t* section = get_clear_memory(sizeof(section_t));
    refcounter_init((refcounter_t*) section);
    platform_lock_init(&section->lock);

    char section_id[24];
    snprintf(section_id, sizeof(section_id), "%lu", id);
    section->fd = -1;
    section->path = path_join(path, section_id);
    section->meta_path = path_join(meta_path, section_id);
    section->size = size;
    section->id = id;

    if (access(section->meta_path, F_OK) == 0) {
        // Existing section - load metadata
#ifdef _WIN32
        int meta_fd = _open(section->meta_path, _O_RDONLY | _O_BINARY, 0644);
#else
        int meta_fd = open(section->meta_path, O_RDONLY, 0644);
#endif

        int32_t file_size = lseek(meta_fd, 0, SEEK_END);
        if (file_size < 0) {
            log_error("Failed to read section meta file size");
            abort();
        }

        if (lseek(meta_fd, 0, SEEK_SET) < 0) {
            log_error("Failed to read section meta file");
            close(meta_fd);
            abort();
        }

        uint8_t* buffer = get_memory(file_size);
        int32_t read_size = read(meta_fd, buffer, file_size);
        close(meta_fd);
        if (file_size != read_size) {
            log_error("Failed to read section meta file");
            free(buffer);
            abort();
        }

        struct cbor_load_result result;
        cbor_item_t* cbor = cbor_load(buffer, file_size, &result);
        free(buffer);

        if (result.error.code != CBOR_ERR_NONE) {
            log_error("Failed to parse section meta file");
            abort();
        }
        if (!cbor_isa_array(cbor)) {
            log_error("Failed to parse section meta file: Malformed data");
            cbor_decref(&cbor);
            abort();
        }

        fragment_list_t* fragments = cbor_to_fragment_list(cbor);
        if (fragments == NULL) {
            log_error("Failed to parse section meta file: Malformed data");
            cbor_decref(&cbor);
            abort();
        }
        section->fragments = fragments;
        cbor_decref(&cbor);
    } else {
        // New section - initialize empty
        section->fragments = fragment_list_create();
        fragment_t* fragment = fragment_create(0, section->size - 1);
        fragment_list_insert(section->fragments, fragment);
    }

    return section;
}

void section_destroy(section_t* section) {
    refcounter_dereference((refcounter_t*) section);
    if (refcounter_count((refcounter_t*) section) == 0) {
        // Flush dirty metadata before destroying
        if (section->meta_dirty) {
            section_save_fragments(section);
        }

        refcounter_destroy_lock((refcounter_t*) section);
        platform_lock_destroy(&section->lock);

        if (section->fd != -1) {
            close(section->fd);
            section->fd = -1;
        }

        fragment_list_destroy(section->fragments);
        free(section->path);
        free(section->meta_path);
        free(section);
    }
}

// Find first-fit fragment that can fit total_bytes
static int section_next_offset(section_t* section, size_t total_bytes, size_t* offset) {
    // Use binary search for O(log n) fragment lookup
    return fragment_list_find_fit(section->fragments, total_bytes, offset);
}

uint8_t section_full(section_t* section) {
    uint8_t result;
    platform_lock(&section->lock);
    result = section->fragments->count == 0;
    platform_unlock(&section->lock);
    return result;
}

uint8_t section_can_fit(section_t* section, size_t required_bytes) {
    uint8_t result;
    platform_lock(&section->lock);
    // Check if any fragment can accommodate the required size
    size_t offset;
    result = fragment_list_find_fit(section->fragments, required_bytes, &offset) == 0;
    platform_unlock(&section->lock);
    return result;
}

// Write variable-size record: [transaction_id (24B)] [data_size (8B)] [data]
int section_write(section_t* section, transaction_id_t txn_id, buffer_t* data, size_t* offset, uint8_t* full) {
    platform_lock(&section->lock);

    // Total bytes needed: transaction_id (24) + size (8) + data
    size_t total_bytes = 24 + 8 + data->size;

    // Find space
    if (section_next_offset(section, total_bytes, offset)) {
        // Section cannot fit this write. Check if it's truly full
        // (no fragments at all) or just fragmented (fragments too small).
        *full = section->fragments->count == 0 || section->fragments->total_free_space < total_bytes;
        platform_unlock(&section->lock);
        return 1;  // No space
    }

    // Open file if needed
    if (section->fd == -1) {
#ifdef _WIN32
        section->fd = _open(section->path, _O_RDWR | _O_BINARY | _O_CREAT, 0644);
#else
        section->fd = open(section->path, O_RDWR | O_CREAT, 0644);
#endif
        if (section->fd < 0) {
            _section_deallocate(section, *offset, total_bytes);
            *full = section->fragments->count == 0 || section->fragments->total_free_space < total_bytes;
            platform_unlock(&section->lock);
            return 2;
        }
    }

    // Seek to offset
    if (lseek(section->fd, *offset, SEEK_SET) != (off_t)*offset) {
        _section_deallocate(section, *offset, total_bytes);
        *full = section->fragments->count == 0;
        platform_unlock(&section->lock);
        return 3;
    }

    // Prepare header: transaction_id (24 bytes) + data size (8 bytes)
    uint8_t txn_buf[24];
    uint8_t size_buf[8];
    transaction_id_serialize(&txn_id, txn_buf);
    write_uint64(size_buf, data->size);

    // Use writev() for single syscall instead of 3 separate writes
    struct iovec iov[3];
    iov[0].iov_base = txn_buf;
    iov[0].iov_len = 24;
    iov[1].iov_base = size_buf;
    iov[1].iov_len = 8;
    iov[2].iov_base = data->data;
    iov[2].iov_len = data->size;

    ssize_t written = writev(section->fd, iov, 3);
    if (written != (ssize_t)(24 + 8 + data->size)) {
        _section_deallocate(section, *offset, total_bytes);
        *full = section->fragments->count == 0;
        platform_unlock(&section->lock);
        return 4;
    }

    // Mark metadata as dirty (will be saved on checkin/close)
    section->meta_dirty = 1;
    *full = section->fragments->count == 0;
    platform_unlock(&section->lock);
    return 0;
}

// Read variable-size record
int section_read(section_t* section, size_t offset, transaction_id_t* txn_id, buffer_t** data) {
    platform_lock(&section->lock);

    // Open file if needed
    if (section->fd == -1) {
#ifdef _WIN32
        section->fd = _open(section->path, _O_RDWR | _O_BINARY | _O_CREAT, 0644);
#else
        section->fd = open(section->path, O_RDWR | O_CREAT, 0644);
#endif
    }

    if (section->fd < 0) {
        platform_unlock(&section->lock);
        return 1;
    }

    // Seek to offset
    if (lseek(section->fd, offset, SEEK_SET) != (off_t)offset) {
        platform_unlock(&section->lock);
        return 2;
    }

    // Read transaction_id (24 bytes)
    uint8_t txn_buf[24];
    ssize_t bytes_read = read(section->fd, txn_buf, 24);
    if (bytes_read != 24) {
        platform_unlock(&section->lock);
        return 3;
    }
    transaction_id_deserialize(txn_id, txn_buf);

    // Read data size (8 bytes)
    uint8_t size_buf[8];
    bytes_read = read(section->fd, size_buf, 8);
    if (bytes_read != 8) {
        platform_unlock(&section->lock);
        return 4;
    }
    uint64_t data_size = read_uint64(size_buf);

    // Read data
    uint8_t* data_buf = get_memory(data_size);
    bytes_read = read(section->fd, data_buf, data_size);
    if ((size_t)bytes_read != data_size) {
        free(data_buf);
        platform_unlock(&section->lock);
        return 5;
    }

    platform_unlock(&section->lock);
    *data = buffer_create_from_existing_memory(data_buf, data_size);
    return 0;
}

// Deallocate variable-size record (add fragment back to list)
static int _section_deallocate(section_t* section, size_t offset, size_t total_bytes) {
    size_t end_offset = offset + total_bytes - 1;

    // Simple deallocation: just add the fragment back
    // TODO: Could optimize by merging adjacent fragments in the future
    fragment_t* frag = fragment_create(offset, end_offset);
    if (frag == NULL) {
        return 1;  // Memory allocation failed
    }

    fragment_list_insert(section->fragments, frag);
    return 0;
}

int section_deallocate(section_t* section, size_t offset, size_t data_size) {
    platform_lock(&section->lock);
    // Total bytes: transaction_id (24) + size (8) + data_size
    size_t total_bytes = 24 + 8 + data_size;
    int result = _section_deallocate(section, offset, total_bytes);
    platform_unlock(&section->lock);
    return result;
}

void section_flush_metadata(section_t* section) {
    platform_lock(&section->lock);
    if (section->meta_dirty) {
        section_save_fragments(section);
        section->meta_dirty = 0;
    }
    platform_unlock(&section->lock);
}

static void section_save_fragments(section_t* section) {
    cbor_item_t* cbor = fragment_list_to_cbor(section->fragments);
    uint8_t* cbor_data;
    size_t cbor_size;
    cbor_serialize_alloc(cbor, &cbor_data, &cbor_size);

#ifdef _WIN32
    int meta_fd = _open(section->meta_path, _O_WRONLY | _O_TRUNC | _O_BINARY | _O_CREAT, 0644);
#else
    int meta_fd = open(section->meta_path, O_WRONLY | O_TRUNC | O_CREAT, 0644);
#endif
    if (meta_fd < 0) {
        log_error("Failed to save section metadata");
        free(cbor_data);
        cbor_decref(&cbor);
        return;
    }

    write(meta_fd, cbor_data, cbor_size);
    close(meta_fd);
    free(cbor_data);
    cbor_decref(&cbor);
    section->meta_dirty = 0;  // Mark as clean after successful save
}

// Flush dirty metadata to disk
void section_flush(section_t* section) {
    if (section == NULL) return;

    platform_lock(&section->lock);
    if (section->meta_dirty) {
        section_save_fragments(section);
    }
    platform_unlock(&section->lock);
}

// Helper: check if an offset range is entirely within a free fragment
static int offset_in_free_fragments(fragment_list_t* fragments, size_t offset, size_t total_bytes) {
    size_t end = offset + total_bytes - 1;
    for (size_t i = 0; i < fragments->count; i++) {
        fragment_t* frag = &fragments->fragments[i];
        if (offset >= frag->start && end <= frag->end) {
            return 1;  // This record is in a free fragment (deallocated)
        }
    }
    return 0;
}

uint8_t section_is_fragmented(section_t* section, double threshold_ratio) {
    uint8_t result;
    platform_lock(&section->lock);
    if (section->size == 0) {
        platform_unlock(&section->lock);
        return 0;
    }
    double free_ratio = (double)section->fragments->total_free_space / (double)section->size;
    result = (free_ratio >= threshold_ratio) ? 1 : 0;
    platform_unlock(&section->lock);
    return result;
}

int section_defragment(section_t* section,
                       void (*remap_cb)(size_t old_offset, size_t new_offset, void* ctx),
                       void* remap_ctx,
                       size_t* remap_count) {
    if (section == NULL) return -1;

    platform_lock(&section->lock);

    // Open file if needed
    if (section->fd == -1) {
#ifdef _WIN32
        section->fd = _open(section->path, _O_RDWR | _O_BINARY | _O_CREAT, 0644);
#else
        section->fd = open(section->path, O_RDWR | O_CREAT, 0644);
#endif
        if (section->fd < 0) {
            platform_unlock(&section->lock);
            return -2;
        }
    }

    // Get current file size
    off_t file_size = lseek(section->fd, 0, SEEK_END);
    if (file_size <= 0) {
        platform_unlock(&section->lock);
        if (remap_count) *remap_count = 0;
        return 0;  // Empty or unseekable — nothing to defragment
    }

    // Build a snapshot of free fragments for checking dead records.
    // Copy the fragment data so we can check without holding the list stable.
    size_t frag_count = section->fragments->count;
    fragment_t* frag_snapshot = NULL;
    if (frag_count > 0) {
        frag_snapshot = malloc(frag_count * sizeof(fragment_t));
        if (frag_snapshot == NULL) {
            platform_unlock(&section->lock);
            return -3;
        }
        memcpy(frag_snapshot, section->fragments->fragments, frag_count * sizeof(fragment_t));
    }

    // Scan the file sequentially, reading record headers to find live records
    typedef struct {
        size_t offset;      // Original offset
        size_t total_bytes;  // Total record size (header + data)
    } live_record_t;

    // We don't know how many records there are, so use a dynamic array
    size_t live_cap = 64;
    size_t live_count = 0;
    live_record_t* live_records = malloc(live_cap * sizeof(live_record_t));
    if (live_records == NULL) {
        free(frag_snapshot);
        platform_unlock(&section->lock);
        return -3;
    }

    size_t scan_offset = 0;
    while (scan_offset < (size_t)file_size) {
        // Read transaction_id header (24 bytes) + data_size (8 bytes) = 32 bytes
        uint8_t header[32];
        if (lseek(section->fd, scan_offset, SEEK_SET) != (off_t)scan_offset) {
            break;  // Can't seek — stop scanning
        }
        ssize_t hdr_read = read(section->fd, header, 32);
        if (hdr_read != 32) {
            break;  // Can't read full header — stop scanning
        }

        uint64_t data_size = read_uint64(header + 24);
        size_t total_bytes = 24 + 8 + data_size;

        // Check if this record is in a free fragment (deallocated)
        int is_dead = 0;
        if (frag_snapshot != NULL) {
            size_t rec_end = scan_offset + total_bytes - 1;
            for (size_t i = 0; i < frag_count; i++) {
                if (scan_offset >= frag_snapshot[i].start && rec_end <= frag_snapshot[i].end) {
                    is_dead = 1;
                    break;
                }
            }
        }

        if (!is_dead) {
            // Live record — track it
            if (live_count >= live_cap) {
                live_cap *= 2;
                live_record_t* new_arr = realloc(live_records, live_cap * sizeof(live_record_t));
                if (new_arr == NULL) {
                    free(live_records);
                    free(frag_snapshot);
                    platform_unlock(&section->lock);
                    return -3;
                }
                live_records = new_arr;
            }
            live_records[live_count].offset = scan_offset;
            live_records[live_count].total_bytes = total_bytes;
            live_count++;
        }

        scan_offset += total_bytes;
    }

    free(frag_snapshot);

    // No live records, or all records already at offset 0 contiguously — nothing to do
    if (live_count == 0) {
        free(live_records);
        // Section is entirely free — replace fragments with single fragment covering whole section
        fragment_list_destroy(section->fragments);
        section->fragments = fragment_list_create();
        fragment_t* whole = fragment_create(0, section->size - 1);
        fragment_list_insert(section->fragments, whole);
        section->meta_dirty = 1;
        platform_unlock(&section->lock);
        if (remap_count) *remap_count = 0;
        return 0;
    }

    // Check if already contiguous (no gaps between live records starting at offset 0)
    int already_contiguous = 1;
    size_t expected_offset = 0;
    for (size_t i = 0; i < live_count; i++) {
        if (live_records[i].offset != expected_offset) {
            already_contiguous = 0;
            break;
        }
        expected_offset += live_records[i].total_bytes;
    }

    if (already_contiguous) {
        free(live_records);
        // Already compact — just rebuild fragment list for the tail
        fragment_list_destroy(section->fragments);
        section->fragments = fragment_list_create();
        if (expected_offset < section->size) {
            fragment_t* tail = fragment_create(expected_offset, section->size - 1);
            fragment_list_insert(section->fragments, tail);
        }
        section->meta_dirty = 1;
        platform_unlock(&section->lock);
        if (remap_count) *remap_count = 0;
        return 0;
    }

    // Need to compact: read each live record and write it contiguously
    // Read the entire file into memory for efficient compaction
    size_t buf_size = (size_t)file_size;
    uint8_t* file_buf = malloc(buf_size);
    if (file_buf == NULL) {
        free(live_records);
        platform_unlock(&section->lock);
        return -3;
    }

    // Read entire file
    if (lseek(section->fd, 0, SEEK_SET) != 0) {
        free(file_buf);
        free(live_records);
        platform_unlock(&section->lock);
        return -4;
    }
    ssize_t total_read = read(section->fd, file_buf, buf_size);
    if ((size_t)total_read != buf_size) {
        free(file_buf);
        free(live_records);
        platform_unlock(&section->lock);
        return -4;
    }

    // Write live records contiguously from the beginning
    size_t write_offset = 0;
    size_t moved_count = 0;

    for (size_t i = 0; i < live_count; i++) {
        size_t old_off = live_records[i].offset;
        size_t rec_size = live_records[i].total_bytes;

        if (old_off != write_offset) {
            // Move record to new position
            memmove(file_buf + write_offset, file_buf + old_off, rec_size);

            // Notify caller of offset change
            if (remap_cb) {
                remap_cb(old_off, write_offset, remap_ctx);
            }
            moved_count++;
        }

        write_offset += rec_size;
    }

    // Write the compacted data back to the file
    if (lseek(section->fd, 0, SEEK_SET) != 0) {
        free(file_buf);
        free(live_records);
        platform_unlock(&section->lock);
        return -4;
    }
    ssize_t written = write(section->fd, file_buf, write_offset);
    if ((size_t)written != write_offset) {
        free(file_buf);
        free(live_records);
        platform_unlock(&section->lock);
        return -4;
    }

    // Truncate the file to the new size
#ifdef _WIN32
    // On Windows, use _chsize to truncate
    _chsize(section->fd, write_offset);
#else
    if (ftruncate(section->fd, write_offset) != 0) {
        log_warn("Failed to truncate section file after defragmentation");
    }
#endif

    free(file_buf);
    free(live_records);

    // Rebuild fragment list with single free fragment for the tail
    fragment_list_destroy(section->fragments);
    section->fragments = fragment_list_create();
    if (write_offset < section->size) {
        fragment_t* tail = fragment_create(write_offset, section->size - 1);
        fragment_list_insert(section->fragments, tail);
    }

    section->meta_dirty = 1;
    platform_unlock(&section->lock);

    if (remap_count) *remap_count = moved_count;
    return 0;
}