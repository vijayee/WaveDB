//
// Created by victor on 03/13/26.
//

#include "sections.h"
#include "../Util/allocator.h"
#include "../Util/log.h"
#include "../Util/mkdir_p.h"
#include "../Util/path_join.h"
#include "../Util/get_dir.h"
#include "../Util/hash.h"
#include <cbor.h>

#ifdef _WIN32
#include <io.h>
#define F_OK 0
#define access _access
#else
#include <unistd.h>
#endif

// Forward declarations
static void sections_lru_cache_move(sections_lru_cache_t* lru, sections_lru_node_t* node);
static void round_robin_save(void* ctx);
static void sections_full(sections_t* sections, size_t section_id);
static size_t sections_get_next_id(sections_t* sections);
static void sections_free(sections_t* sections, size_t section_id);

// LRU cache functions
sections_lru_cache_t* sections_lru_cache_create(size_t size) {
    sections_lru_cache_t* lru = get_clear_memory(sizeof(sections_lru_cache_t));
    lru->size = size;
    lru->first = NULL;
    lru->last = NULL;
    hashmap_init(&lru->cache, (void*) hash_size_t, (void*) compare_size_t);
    hashmap_set_key_alloc_funcs(&lru->cache, duplicate_size_t, (void*) free);
    return lru;
}

void sections_lru_cache_destroy(sections_lru_cache_t* lru) {
    sections_lru_node_t* node;
    hashmap_foreach_data(node, &lru->cache) {
        section_destroy(node->value);
        free(node);
    }
    hashmap_cleanup(&lru->cache);
    free(lru);
}

section_t* sections_lru_cache_get(sections_lru_cache_t* lru, size_t section_id) {
    sections_lru_node_t* node = hashmap_get(&lru->cache, &section_id);
    if (node == NULL) {
        return NULL;
    } else {
        sections_lru_cache_move(lru, node);
        return node->value;
    }
}

void sections_lru_cache_delete(sections_lru_cache_t* lru, size_t section_id) {
    sections_lru_node_t* node = hashmap_get(&lru->cache, &section_id);
    if (node != NULL) {
        if (node->previous == NULL) {
            // Node is at the start
            if (node->next == NULL) {
                // Only one node
                lru->first = NULL;
                lru->last = NULL;
            } else {
                sections_lru_node_t* next_node = node->next;
                next_node->previous = NULL;
                lru->first = node->next;
            }
        } else {
            // Node is not at start
            sections_lru_node_t* previous_node = node->previous;
            if (node->next == NULL) {
                // Node is at end
                previous_node->next = NULL;
                lru->last = previous_node;
            } else {
                // Node is in middle
                sections_lru_node_t* next_node = node->next;
                next_node->previous = node->previous;
                previous_node->next = node->next;
            }
        }
        hashmap_remove(&lru->cache, &section_id);
        section_destroy(node->value);
        free(node);
    }
}

void sections_lru_cache_put(sections_lru_cache_t* lru, section_t* section) {
    if (lru->size == 0) {
        return;
    }

    sections_lru_node_t* node = hashmap_get(&lru->cache, &section->id);
    // Add new node if none exists
    if (node == NULL) {
        node = get_clear_memory(sizeof(sections_lru_node_t));
        node->previous = NULL;
        node->next = NULL;
        node->value = (section_t*) refcounter_reference((refcounter_t*) section);
    }

    // Cache eviction
    if (hashmap_size(&lru->cache) == lru->size) {
        if (lru->last != NULL) {
            sections_lru_node_t* last_node = lru->last;
            if (last_node->previous == NULL) {
                // Only one node
                lru->last = NULL;
                lru->first = NULL;
            } else {
                sections_lru_node_t* new_last_node = last_node->previous;
                new_last_node->next = NULL;
                lru->last = last_node->previous;
            }
            hashmap_remove(&lru->cache, &last_node->value->id);
            section_destroy(last_node->value);
            free(last_node);
        }
    }

    hashmap_put(&lru->cache, &section->id, node);
    sections_lru_cache_move(lru, node);
}

uint8_t sections_lru_cache_contains(sections_lru_cache_t* lru, size_t section_id) {
    sections_lru_node_t* node = hashmap_get(&lru->cache, &section_id);
    return node != NULL;
}

static void sections_lru_cache_move(sections_lru_cache_t* lru, sections_lru_node_t* node) {
    if (lru->first == NULL) {
        lru->first = node;
        lru->last = node;
    } else {
        if (lru->first == node) {
            // Already most recently used
            return;
        }
        if (lru->first == lru->last) {
            // Only one node
            node->next = lru->first;
            sections_lru_node_t* first_node = lru->first;
            first_node->previous = node;
            lru->last = first_node;
            lru->first = node;
        } else if (lru->last == node) {
            // Node is LRU
            lru->last = node->previous;
            sections_lru_node_t* last_node = lru->last;
            last_node->next = NULL;
            node->next = lru->first;
            node->previous = NULL;
            sections_lru_node_t* first_node = lru->first;
            first_node->previous = node;
            lru->first = node;
        } else {
            // Node is in middle
            if ((node->next == NULL) && (node->previous == NULL)) {
                // New node
                sections_lru_node_t* first_node = lru->first;
                first_node->previous = node;
                node->next = first_node;
                lru->first = node;
            } else {
                if (node->previous != NULL) {
                    sections_lru_node_t* previous_node = node->previous;
                    previous_node->next = node->next;
                }
                if (node->next != NULL) {
                    sections_lru_node_t* next_node = node->next;
                    next_node->previous = node->previous;
                }
                sections_lru_node_t* first_node = lru->first;
                first_node->previous = node;
                node->next = first_node;
                lru->first = node;
            }
        }
    }
}

// Round-robin functions
round_robin_t* round_robin_create(char* robin_path, hierarchical_timing_wheel_t* wheel) {
    round_robin_t* robin = get_clear_memory(sizeof(round_robin_t));
    platform_lock_init(&robin->lock);
    robin->debouncer = debouncer_create(wheel, (void*) robin, round_robin_save, round_robin_save, 5, 5000);
    robin->path = robin_path;
    return robin;
}

void round_robin_destroy(round_robin_t* robin) {
    debouncer_flush(robin->debouncer);
    debouncer_destroy(robin->debouncer);
    free(robin->path);
    platform_lock_destroy(&robin->lock);

    round_robin_node_t* current = robin->first;
    while (current != NULL) {
        round_robin_node_t* next = current->next;
        free(current);
        current = next;
    }
    free(robin);
}

void round_robin_add(round_robin_t* robin, size_t id) {
    round_robin_node_t* node = get_clear_memory(sizeof(round_robin_node_t));
    node->id = id;
    node->previous = NULL;
    node->next = NULL;

    platform_lock(&robin->lock);
    if ((robin->last == NULL) && (robin->first == NULL)) {
        robin->first = node;
        robin->last = node;
    } else {
        node->previous = robin->last;
        robin->last->next = node;
        robin->last = node;
    }
    debouncer_debounce(robin->debouncer);
    robin->size++;
    platform_unlock(&robin->lock);
}

size_t round_robin_next(round_robin_t* robin) {
    platform_lock(&robin->lock);

    if ((robin->last == NULL) && (robin->first == NULL)) {
        platform_unlock(&robin->lock);
        return 0;
    } else {
        round_robin_node_t* node = robin->first;
        if (robin->last == node) {
            // Only one node
            size_t id = node->id;
            platform_unlock(&robin->lock);
            return id;
        } else {
            robin->first = node->next;
            if (node->next != NULL) {
                node->next->previous = NULL;
                node->next = NULL;
            }
            size_t id = node->id;
            node->previous = robin->last;
            robin->last->next = node;
            robin->last = node;
            platform_unlock(&robin->lock);
            return id;
        }
    }
}

void round_robin_remove(round_robin_t* robin, size_t id) {
    platform_lock(&robin->lock);

    if ((robin->last == NULL) && (robin->first == NULL)) {
        platform_unlock(&robin->lock);
        return;
    }

    round_robin_node_t* current = robin->first;
    while (current != NULL) {
        if (current->id == id) {
            if (robin->last == current) {
                robin->last = current->previous;
            }
            if (robin->first == current) {
                robin->first = current->next;
            }
            if (current->previous != NULL) {
                current->previous->next = current->next;
            }
            if (current->next != NULL) {
                current->next->previous = current->previous;
            }
            free(current);
            debouncer_debounce(robin->debouncer);
            robin->size--;
            platform_unlock(&robin->lock);
            return;
        } else {
            current = current->next;
        }
    }
    platform_unlock(&robin->lock);
}

uint8_t round_robin_contains(round_robin_t* robin, size_t id) {
    platform_lock(&robin->lock);

    if ((robin->last == NULL) && (robin->first == NULL)) {
        platform_unlock(&robin->lock);
        return 0;
    }

    round_robin_node_t* current = robin->first;
    while (current != NULL) {
        if (current->id == id) {
            platform_unlock(&robin->lock);
            return 1;
        } else {
            current = current->next;
        }
    }
    platform_unlock(&robin->lock);
    return 0;
}

cbor_item_t* round_robin_to_cbor(round_robin_t* robin) {
    cbor_item_t* array = cbor_new_definite_array(robin->size);
    round_robin_node_t* current = robin->first;
    bool success = true;
    while (current != NULL) {
        success &= cbor_array_push(array, cbor_move(cbor_build_uint64(current->id)));
        current = current->next;
    }
    if (!success) {
        cbor_decref(&array);
        return NULL;
    }
    return array;
}

round_robin_t* cbor_to_round_robin(cbor_item_t* cbor, char* robin_path, hierarchical_timing_wheel_t* wheel) {
    if (!cbor_isa_array(cbor)) {
        return NULL;
    }

    round_robin_t* robin = round_robin_create(robin_path, wheel);
    size_t size = cbor_array_size(cbor);
    for (size_t i = 0; i < size; i++) {
        cbor_item_t* cbor_id = cbor_array_get(cbor, i);
        round_robin_add(robin, cbor_get_uint64(cbor_id));
    }
    return robin;
}

static void round_robin_save(void* ctx) {
    round_robin_t* robin = (round_robin_t*) ctx;
    platform_lock(&robin->lock);
    cbor_item_t* cbor = round_robin_to_cbor(robin);
    platform_unlock(&robin->lock);

    if (cbor == NULL) {
        log_error("Failed to save robin file");
        return;
    }

    uint8_t* cbor_data;
    size_t cbor_size;
    cbor_serialize_alloc(cbor, &cbor_data, &cbor_size);

    FILE* robin_file = fopen(robin->path, "wb");
    if (robin_file == NULL) {
        log_error("Failed to save robin file");
        free(cbor_data);
        cbor_decref(&cbor);
        return;
    }

    fwrite(cbor_data, cbor_size, 1, robin_file);
    fflush(robin_file);
    fclose(robin_file);
    free(cbor_data);
    cbor_decref(&cbor);
}

// Sections functions
sections_t* sections_create(char* path, size_t size, size_t cache_size, size_t max_tuple_size,
                            block_size_e type, hierarchical_timing_wheel_t* wheel,
                            size_t wait, size_t max_wait) {
    char* robin_folder = path_join(path, "robin");
    mkdir_p(robin_folder);
    char* robin_path = path_join(robin_folder, ".robin");
    free(robin_folder);

    sections_t* sections = get_clear_memory(sizeof(sections_t));
    sections->wheel = (hierarchical_timing_wheel_t*) refcounter_reference((refcounter_t*) wheel);
    sections->wait = wait;
    sections->max_wait = max_wait;
    sections->type = type;
    sections->max_tuple_size = max_tuple_size;
    sections->size = size;

    platform_lock_init(&sections->lock);
    platform_lock_init(&sections->checkout.lock);
    hashmap_init(&sections->checkout.sections, (void*) hash_size_t, (void*) compare_size_t);
    hashmap_set_key_alloc_funcs(&sections->checkout.sections, duplicate_size_t, (void*) free);

    if (access(robin_path, F_OK) == 0) {
        // Existing round-robin file
        FILE* robin_file = fopen(robin_path, "rb");
        if (fseek(robin_file, 0, SEEK_END)) {
            log_error("Failed to read round robin file size");
            abort();
        }
        int32_t file_size = ftell(robin_file);
        rewind(robin_file);
        uint8_t* buffer = get_memory(file_size);
        int32_t read_size = fread(buffer, sizeof(uint8_t), file_size, robin_file);
        fclose(robin_file);

        if (file_size != read_size) {
            log_error("Failed to read round robin file");
            free(buffer);
            abort();
        }

        struct cbor_load_result result;
        cbor_item_t* cbor = cbor_load(buffer, file_size, &result);
        free(buffer);

        if (result.error.code != CBOR_ERR_NONE) {
            log_error("Failed to parse round robin file");
            cbor_decref(&cbor);
            abort();
        }
        if (!cbor_isa_array(cbor)) {
            log_error("Failed to parse round robin file: Malformed data");
            cbor_decref(&cbor);
            abort();
        }

        sections->robin = cbor_to_round_robin(cbor, robin_path, wheel);
        cbor_decref(&cbor);
    } else {
        // New round-robin
        sections->robin = round_robin_create(robin_path, wheel);
    }

    sections->lru = sections_lru_cache_create(cache_size);
    sections->data_path = path_join(path, "data");
    mkdir_p(sections->data_path);
    sections->meta_path = path_join(path, "meta");
    mkdir_p(sections->meta_path);

    // Find next section ID
    vec_str_t* files = get_dir(sections->meta_path);
    if (files->length) {
        char* last = vec_last(files);
        uint64_t last_id = strtoull(last, NULL, 10);
        sections->next_id = last_id + 1;
    } else {
        sections->next_id = 0;
    }
    vec_deinit(files);
    free(files);

    // Create initial sections if needed
    while (sections->robin->size < sections->max_tuple_size) {
        section_t* section = section_create(sections->data_path, sections->meta_path,
                                           sections->size, sections_get_next_id(sections), sections->type);
        refcounter_yield((refcounter_t*) section);
        sections_lru_cache_put(sections->lru, section);
        round_robin_add(sections->robin, section->id);
    }

    return sections;
}

void sections_destroy(sections_t* sections) {
    platform_lock_destroy(&sections->checkout.lock);
    hashmap_cleanup(&sections->checkout.sections);
    sections_lru_cache_destroy(sections->lru);
    round_robin_destroy(sections->robin);
    hierarchical_timing_wheel_destroy(sections->wheel);
    free(sections->meta_path);
    free(sections->data_path);
    free(sections->robin_path);
    free(sections);
}

int sections_write(sections_t* sections, buffer_t* data, size_t* section_id, size_t* section_index) {
    section_t* section;
    uint8_t full;
    uint8_t tries = 0;
    int result;

    do {
        *section_id = round_robin_next(sections->robin);
        section = sections_checkout(sections, *section_id);
        if (section == NULL) {
            section = section_create(sections->data_path, sections->meta_path,
                                    sections->size, *section_id, sections->type);
        }
        result = section_write(section, data, section_index, &full);

        if ((result == 2) || full) {
            sections_full(sections, *section_id);
        }
        tries++;
        sections_checkin(sections, section);
    } while ((result == 2) && (tries < sections->max_tuple_size));

    return result;
}

buffer_t* sections_read(sections_t* sections, size_t section_id, size_t section_index) {
    section_t* section = sections_checkout(sections, section_id);
    buffer_t* data = section_read(section, section_index);
    sections_checkin(sections, section);
    return data;
}

int sections_deallocate(sections_t* sections, size_t section_id, size_t section_index) {
    section_t* section = sections_checkout(sections, section_id);
    int result = section_deallocate(section, section_index);
    if (result == 0) {
        sections_free(sections, section_id);
    }
    sections_checkin(sections, section);
    return result;
}

section_t* sections_checkout(sections_t* sections, size_t section_id) {
    section_t* section = sections_lru_cache_get(sections->lru, section_id);
    if (section == NULL) {
        section = section_create(sections->data_path, sections->meta_path,
                                sections->size, section_id, sections->type);
        refcounter_yield((refcounter_t*) section);
        if (section_full(section) == 0) {
            round_robin_add(sections->robin, section_id);
        }
    }

    platform_lock(&sections->checkout.lock);
    checkout_t* checkout = hashmap_get(&sections->checkout.sections, &section_id);
    if (checkout == NULL) {
        checkout = get_clear_memory(sizeof(checkout_t));
        checkout->section = (section_t*) refcounter_reference((refcounter_t*) section);
        checkout->count = 1;
        hashmap_put(&sections->checkout.sections, &section_id, checkout);
    } else {
        checkout->count++;
    }
    platform_unlock(&sections->checkout.lock);

    return (section_t*) refcounter_reference((refcounter_t*) section);
}

void sections_checkin(sections_t* sections, section_t* section) {
    platform_lock(&sections->checkout.lock);
    checkout_t* checkout = hashmap_get(&sections->checkout.sections, &section->id);
    if (checkout != NULL) {
        checkout->count--;
        if (checkout->count == 0) {
            hashmap_remove(&sections->checkout.sections, &section->id);
            free(checkout);
            section_destroy(section);
        }
    }
    platform_unlock(&sections->checkout.lock);
    section_destroy(section);
}

static size_t sections_get_next_id(sections_t* sections) {
    size_t id;
    platform_lock(&sections->lock);
    id = sections->next_id++;
    platform_unlock(&sections->lock);
    return id;
}

static void sections_full(sections_t* sections, size_t section_id) {
    round_robin_remove(sections->robin, section_id);

    while (sections->robin->size < sections->max_tuple_size) {
        section_t* section = section_create(sections->data_path, sections->meta_path,
                                            sections->size, sections_get_next_id(sections), sections->type);
        refcounter_yield((refcounter_t*) section);
        sections_lru_cache_put(sections->lru, section);
        round_robin_add(sections->robin, section->id);
    }
}

static void sections_free(sections_t* sections, size_t section_id) {
    if (round_robin_contains(sections->robin, section_id) == 0) {
        round_robin_add(sections->robin, section_id);
    }
}