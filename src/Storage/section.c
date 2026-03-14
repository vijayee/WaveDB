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

#ifdef _WIN32
#include <io.h>
#define F_OK 0
#define access _access
#else
#include <unistd.h>
#endif

// Forward declarations
static int section_next_index(section_t* section, size_t* index);
static void section_save_fragments(section_t* section);
static int _section_deallocate(section_t* section, size_t index);

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
section_t* section_create(char* path, char* meta_path, size_t size, size_t id, block_size_e type) {
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
    section->block_size = (size_t)type;

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

static int section_next_index(section_t* section, size_t* index) {
    if (section->fragments->count == 0) {
        return 1;
    } else {
        fragment_list_node_t* current = section->fragments->first;
        int64_t nxt = -1;
        if (current != NULL) {
            if (current->fragment->start == current->fragment->end) {
                nxt = current->fragment->start;
                fragment_destroy(fragment_list_remove(section->fragments, current));
            } else {
                nxt = current->fragment->start;
                current->fragment->start++;
            }
        }
        if (nxt == -1) {
            return 1;
        } else {
            *index = nxt;
            return 0;
        }
    }
}

uint8_t section_full(section_t* section) {
    uint8_t result;
    platform_lock(&section->lock);
    result = section->fragments->count == 0;
    platform_unlock(&section->lock);
    return result;
}

int section_write(section_t* section, buffer_t* data, size_t* index, uint8_t* full) {
    platform_lock(&section->lock);

    if (data->size != section->block_size) {
        *full = section->fragments->count == 0;
        platform_unlock(&section->lock);
        return 1;
    }

    if (section_next_index(section, index)) {
        *full = section->fragments->count == 0;
        platform_unlock(&section->lock);
        return 2;
    }

    if (section->fd == -1) {
#ifdef _WIN32
        section->fd = _open(section->path, _O_RDWR | _O_BINARY | _O_CREAT, 0644);
#else
        section->fd = open(section->path, O_RDWR | O_CREAT, 0644);
#endif
        if (section->fd < 0) {
            _section_deallocate(section, *index);
            *full = section->fragments->count == 0;
            platform_unlock(&section->lock);
            return 3;
        }
    }

    size_t byte_offset = data->size * (*index);
    if (lseek(section->fd, byte_offset, SEEK_SET) != byte_offset) {
        _section_deallocate(section, *index);
        *full = section->fragments->count == 0;
        platform_unlock(&section->lock);
        return 4;
    }

    size_t result = write(section->fd, data->data, data->size);
    if (result < section->block_size) {
        _section_deallocate(section, *index);
        *full = section->fragments->count == 0;
        platform_unlock(&section->lock);
        return 5;
    }

    section_save_fragments(section);
    *full = section->fragments->count == 0;
    platform_unlock(&section->lock);
    return 0;
}

buffer_t* section_read(section_t* section, size_t index) {
    platform_lock(&section->lock);

    if (section->fd == -1) {
#ifdef _WIN32
        section->fd = _open(section->path, _O_RDWR | _O_BINARY | _O_CREAT, 0644);
#else
        section->fd = open(section->path, O_RDWR | O_CREAT, 0644);
#endif
    }

    if (section->fd < 0) {
        platform_unlock(&section->lock);
        return NULL;
    }

    size_t byte_offset = index * section->block_size;
    if (lseek(section->fd, byte_offset, SEEK_SET) != byte_offset) {
        platform_unlock(&section->lock);
        return NULL;
    }

    uint8_t* data = get_memory(section->block_size);
    int32_t read_size = read(section->fd, data, section->block_size);
    if (read_size < section->block_size) {
        free(data);
        platform_unlock(&section->lock);
        return NULL;
    }

    platform_unlock(&section->lock);
    buffer_t* buf = buffer_create_from_existing_memory(data, section->block_size);
    return buf;
}

static int _section_deallocate(section_t* section, size_t index) {
    if (section->fragments->count == 0) {
        // Section is full, add first fragment
        fragment_list_enqueue(section->fragments, fragment_create(index, index));
        section_save_fragments(section);
        return 0;
    }

    fragment_list_node_t* current = section->fragments->first;
    fragment_list_node_t* greater_than = NULL;
    int64_t greater_than_difference = 0;
    fragment_list_node_t* less_than = NULL;
    int64_t less_than_difference = 0;

    while (current != NULL) {
        if (index == current->fragment->end) {
            // Someone tried to deallocate free space
            return 1;
        } else if ((index < current->fragment->end) && (index >= current->fragment->start)) {
            // Someone tried to deallocate free space
            return 1;
        } else {
            if (index > current->fragment->end) {
                greater_than = current;
                greater_than_difference = index - greater_than->fragment->end;
            }
            if (index < current->fragment->start) {
                less_than = current;
                less_than_difference = less_than->fragment->start - index;
            }
            current = current->next;
        }
    }

    if ((greater_than == NULL) && (less_than == NULL)) {
        return 2;
    }

    if (greater_than == less_than) {
        return 3;
    }

    if ((greater_than == NULL) && (less_than != NULL)) {
        if (less_than_difference > 1) {
            fragment_t* fragment = fragment_create(index, index);
            fragment_list_node_t* node = fragment_list_node_create(fragment, less_than, NULL);
            less_than->previous = node;
            section->fragments->first = node;
            section->fragments->count++;
        } else {
            less_than->fragment->start--;
        }
    } else if ((greater_than != NULL) && (less_than == NULL)) {
        if (greater_than_difference > 1) {
            fragment_t* fragment = fragment_create(index, index);
            fragment_list_node_t* node = fragment_list_node_create(fragment, NULL, greater_than);
            greater_than->next = node;
            section->fragments->last = node;
            section->fragments->count++;
        } else {
            greater_than->fragment->end++;
        }
    } else {
        if ((greater_than_difference > 1) && (less_than_difference > 1)) {
            fragment_t* fragment = fragment_create(index, index);
            fragment_list_node_t* node = fragment_list_node_create(fragment, less_than, greater_than);
            greater_than->next = node;
            less_than->previous = node;
            section->fragments->count++;
        } else if ((greater_than_difference == 1) && (less_than_difference == 1)) {
            greater_than->fragment->end = less_than->fragment->end;
            fragment_list_remove(section->fragments, less_than);
        } else {
            if (less_than_difference > 1) {
                greater_than->fragment->end++;
            } else {
                less_than->fragment->start--;
            }
        }
    }

    section_save_fragments(section);
    return 0;
}

int section_deallocate(section_t* section, size_t index) {
    platform_lock(&section->lock);
    int result = _section_deallocate(section, index);
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