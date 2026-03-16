//
// Created by victor on 03/13/26.
//

#include "section.h"
#include "../Util/allocator.h"
#include "../Util/log.h"
#include "../Util/mkdir_p.h"
#include "../Util/path_join.h"
#include <cbor.h>
#include <fcntl.h>
#include <stdio.h>
#include <arpa/inet.h>

#ifdef _WIN32
#include <io.h>
#define F_OK 0
#define access _access
#else
#include <unistd.h>
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
    fragment_t* fragment = get_memory(sizeof(fragment_t));
    fragment->start = start;
    fragment->end = end;
    return fragment;
}

void fragment_destroy(fragment_t* fragment) {
    free(fragment);
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

// Fragment list node functions
fragment_list_node_t* fragment_list_node_create(fragment_t* fragment,
                                                   fragment_list_node_t* next,
                                                   fragment_list_node_t* previous) {
    fragment_list_node_t* node = get_memory(sizeof(fragment_list_node_t));
    node->fragment = fragment;
    node->next = next;
    node->previous = previous;
    return node;
}

// Fragment list functions
fragment_list_t* fragment_list_create(void) {
    fragment_list_t* list = get_clear_memory(sizeof(fragment_list_t));
    list->first = NULL;
    list->last = NULL;
    return list;
}

void fragment_list_destroy(fragment_list_t* list) {
    fragment_list_node_t* current = list->first;
    fragment_list_node_t* next = NULL;
    while (current != NULL) {
        next = current->next;
        fragment_destroy(current->fragment);
        free(current);
        current = next;
    }
    free(list);
}

void fragment_list_enqueue(fragment_list_t* list, fragment_t* fragment) {
    fragment_list_node_t* node = fragment_list_node_create(fragment, NULL, NULL);
    if ((list->last == NULL) && (list->first == NULL)) {
        list->first = node;
        list->last = node;
    } else {
        node->previous = list->last;
        list->last->next = node;
        list->last = node;
    }
    list->count++;
}

fragment_t* fragment_list_dequeue(fragment_list_t* list) {
    if ((list->last == NULL) && (list->first == NULL)) {
        return NULL;
    } else {
        fragment_list_node_t* node = list->first;
        list->first = node->next;
        if (node->next != NULL) {
            list->first->previous = NULL;
        }
        if (list->last == node) {
            list->last = NULL;
        }
        fragment_t* fragment = node->fragment;
        free(node);
        list->count--;
        return fragment;
    }
}

fragment_t* fragment_list_remove(fragment_list_t* list, fragment_list_node_t* node) {
    if ((list->last == NULL) && (list->first == NULL)) {
        return NULL;
    }
    if (list->last == node) {
        list->last = node->previous;
    }
    if (list->first == node) {
        list->first = node->next;
    }
    if (node->previous != NULL) {
        node->previous->next = node->next;
    }
    if (node->next != NULL) {
        node->next->previous = node->previous;
    }
    fragment_t* fragment = node->fragment;
    list->count--;
    free(node);
    return fragment;
}

cbor_item_t* fragment_list_to_cbor(fragment_list_t* list) {
    cbor_item_t* array = cbor_new_definite_array(list->count);
    fragment_list_node_t* current = list->first;
    bool success = true;
    while (current != NULL) {
        success = cbor_array_push(array, cbor_move(fragment_to_cbor(current->fragment)));
        if (!success) {
            cbor_decref(&array);
            return NULL;
        }
        current = current->next;
    }
    return array;
}

fragment_list_t* cbor_to_fragment_list(cbor_item_t* cbor) {
    fragment_list_t* list = fragment_list_create();
    size_t size = cbor_array_size(cbor);
    for (size_t i = 0; i < size; i++) {
        fragment_list_enqueue(list, cbor_to_fragment(cbor_move(cbor_array_get(cbor, i))));
    }
    list->count = size;
    return list;
}

// Section functions
section_t* section_create(char* path, char* meta_path, size_t size, size_t id) {
    section_t* section = get_clear_memory(sizeof(section_t));
    refcounter_init((refcounter_t*) section);
    platform_lock_init(&section->lock);

    char section_id[20];
    sprintf(section_id, "%lu", id);
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
        fragment_list_enqueue(section->fragments, fragment);
    }

    return section;
}

void section_destroy(section_t* section) {
    refcounter_dereference((refcounter_t*) section);
    if (refcounter_count((refcounter_t*) section) == 0) {
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
    if (section->fragments->count == 0) {
        return 1;  // No free space
    }

    fragment_list_node_t* current = section->fragments->first;
    while (current != NULL) {
        size_t fragment_size = current->fragment->end - current->fragment->start + 1;
        if (fragment_size >= total_bytes) {
            // Found a fit!
            *offset = current->fragment->start;

            if (fragment_size == total_bytes) {
                // Exact fit - remove fragment
                fragment_destroy(fragment_list_remove(section->fragments, current));
            } else {
                // Partial fit - shrink fragment
                current->fragment->start += total_bytes;
            }
            return 0;
        }
        current = current->next;
    }

    return 1;  // No suitable fragment found
}

uint8_t section_full(section_t* section) {
    uint8_t result;
    platform_lock(&section->lock);
    result = section->fragments->count == 0;
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
        *full = section->fragments->count == 0;
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
            *full = section->fragments->count == 0;
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

    // Write header: transaction_id (24 bytes)
    uint8_t txn_buf[24];
    transaction_id_serialize(&txn_id, txn_buf);
    ssize_t written = write(section->fd, txn_buf, 24);
    if (written != 24) {
        _section_deallocate(section, *offset, total_bytes);
        *full = section->fragments->count == 0;
        platform_unlock(&section->lock);
        return 4;
    }

    // Write data size (8 bytes, network order)
    uint8_t size_buf[8];
    write_uint64(size_buf, data->size);
    written = write(section->fd, size_buf, 8);
    if (written != 8) {
        _section_deallocate(section, *offset, total_bytes);
        *full = section->fragments->count == 0;
        platform_unlock(&section->lock);
        return 5;
    }

    // Write data
    written = write(section->fd, data->data, data->size);
    if ((size_t)written != data->size) {
        _section_deallocate(section, *offset, total_bytes);
        *full = section->fragments->count == 0;
        platform_unlock(&section->lock);
        return 6;
    }

    section_save_fragments(section);
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

// Deallocate variable-size record (merge adjacent fragments)
static int _section_deallocate(section_t* section, size_t offset, size_t total_bytes) {
    size_t end_offset = offset + total_bytes - 1;

    if (section->fragments->count == 0) {
        // Section is full, add first fragment
        fragment_list_enqueue(section->fragments, fragment_create(offset, end_offset));
        section_save_fragments(section);
        return 0;
    }

    fragment_list_node_t* current = section->fragments->first;
    fragment_list_node_t* greater_than = NULL;
    fragment_list_node_t* less_than = NULL;

    // Find adjacent fragments
    while (current != NULL) {
        if (offset > current->fragment->end) {
            greater_than = current;
        }
        if (end_offset < current->fragment->start) {
            less_than = current;
        }
        // Check for overlap
        if ((offset <= current->fragment->end) && (end_offset >= current->fragment->start)) {
            // Overlap with existing fragment - error
            return 1;
        }
        current = current->next;
    }

    // Merge with adjacent fragments or create new fragment
    if ((greater_than == NULL) && (less_than == NULL)) {
        return 2;  // Invalid state
    }

    if (greater_than == less_than) {
        return 3;  // Invalid state
    }

    if ((greater_than == NULL) && (less_than != NULL)) {
        // Before first fragment
        if (less_than->fragment->start == end_offset + 1) {
            // Merge with less_than
            less_than->fragment->start = offset;
        } else {
            // Create new fragment
            fragment_t* fragment = fragment_create(offset, end_offset);
            fragment_list_node_t* node = fragment_list_node_create(fragment, less_than, NULL);
            less_than->previous = node;
            section->fragments->first = node;
            section->fragments->count++;
        }
    } else if ((greater_than != NULL) && (less_than == NULL)) {
        // After last fragment
        if (greater_than->fragment->end == offset - 1) {
            // Merge with greater_than
            greater_than->fragment->end = end_offset;
        } else {
            // Create new fragment
            fragment_t* fragment = fragment_create(offset, end_offset);
            fragment_list_node_t* node = fragment_list_node_create(fragment, NULL, greater_than);
            greater_than->next = node;
            section->fragments->last = node;
            section->fragments->count++;
        }
    } else {
        // Between two fragments
        size_t gt_diff = offset - greater_than->fragment->end - 1;
        size_t lt_diff = less_than->fragment->start - end_offset - 1;

        if ((gt_diff == 0) && (lt_diff == 0)) {
            // Merge both fragments into one
            greater_than->fragment->end = less_than->fragment->end;
            fragment_list_remove(section->fragments, less_than);
        } else if (gt_diff == 0) {
            // Merge with greater_than
            greater_than->fragment->end = end_offset;
        } else if (lt_diff == 0) {
            // Merge with less_than
            less_than->fragment->start = offset;
        } else {
            // Create new fragment between them
            fragment_t* fragment = fragment_create(offset, end_offset);
            fragment_list_node_t* node = fragment_list_node_create(fragment, less_than, greater_than);
            greater_than->next = node;
            less_than->previous = node;
            section->fragments->count++;
        }
    }

    section_save_fragments(section);
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
}