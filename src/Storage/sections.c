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
#include <fcntl.h>

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

// Helper: Get shard index for section_id
static inline size_t get_shard(size_t section_id) {
    return section_id % CHECKOUT_LOCK_SHARDS;
}

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
    // Free all nodes and sections from the LRU list
    sections_lru_node_t* current = lru->first;
    while (current != NULL) {
        sections_lru_node_t* next = current->next;
        section_destroy(current->value);
        free(current);
        current = next;
    }
    // Cleanup the hashmap (this will free all keys)
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
        node = get_memory(sizeof(sections_lru_node_t));
        node->value = section;
        node->next = NULL;
        node->previous = NULL;
        // Consume yield if present (transfers ownership)
        refcounter_reference((refcounter_t*) section);
        if (lru->first == NULL) {
            lru->first = node;
            lru->last = node;
        } else {
            node->next = lru->first;
            lru->first->previous = node;
            lru->first = node;
        }
        // Hashmap will duplicate the key via duplicate_size_t alloc func
        hashmap_put(&lru->cache, &section->id, node);

        // Remove least recently used if cache is full
        size_t count = hashmap_size(&lru->cache);
        if (count > lru->size) {
            sections_lru_node_t* lru_node = lru->last;
            sections_lru_cache_delete(lru, lru_node->value->id);
        }
    } else {
        // Move to front if already exists
        sections_lru_cache_move(lru, node);
    }
}

static void sections_lru_cache_move(sections_lru_cache_t* lru, sections_lru_node_t* node) {
    if (node == lru->first) {
        return;
    }
    if (node->previous != NULL) {
        node->previous->next = node->next;
    }
    if (node->next != NULL) {
        node->next->previous = node->previous;
    }
    lru->first->previous = node;
    lru->first = node;
}

// Forward declaration for callback
static void round_robin_save(void* ctx);

// Round-robin functions
round_robin_t* round_robin_create(char* robin_path, hierarchical_timing_wheel_t* wheel) {
    round_robin_t* robin = get_clear_memory(sizeof(round_robin_t));
    platform_lock_init(&robin->lock);
    robin->path = robin_path;
    robin->first = NULL;
    robin->last = NULL;
    robin->size = 0;
    robin->debouncer = debouncer_create(wheel, robin, round_robin_save, NULL, 1000, 5000);
    return robin;
}

void round_robin_destroy(round_robin_t* robin) {
    // Flush any pending timer before destroying
    debouncer_flush(robin->debouncer);
    debouncer_destroy(robin->debouncer);
    platform_lock_destroy(&robin->lock);

    round_robin_node_t* current = robin->first;
    while (current != NULL) {
        round_robin_node_t* next = current->next;
        free(current);
        current = next;
    }

    free(robin->path);
    free(robin);
}

void round_robin_add(round_robin_t* robin, size_t id) {
    platform_lock(&robin->lock);

    round_robin_node_t* node = get_memory(sizeof(round_robin_node_t));
    node->id = id;
    node->next = NULL;
    node->previous = robin->last;

    if (robin->last == NULL) {
        robin->first = node;
        robin->last = node;
    } else {
        robin->last->next = node;
        robin->last = node;
    }

    robin->size++;
    platform_unlock(&robin->lock);
    debouncer_debounce(robin->debouncer);
}

size_t round_robin_next(round_robin_t* robin) {
    platform_lock(&robin->lock);

    if (robin->first == NULL) {
        platform_unlock(&robin->lock);
        return 0;  // Error: empty round-robin
    }

    round_robin_node_t* node = robin->first;
    size_t id = node->id;

    // Move to end
    robin->first = node->next;
    if (robin->first != NULL) {
        robin->first->previous = NULL;
    } else {
        robin->last = NULL;
    }

    node->next = NULL;
    node->previous = robin->last;

    if (robin->last != NULL) {
        robin->last->next = node;
    }
    robin->last = node;

    platform_unlock(&robin->lock);
    return id;
}

void round_robin_remove(round_robin_t* robin, size_t id) {
    platform_lock(&robin->lock);

    round_robin_node_t* current = robin->first;
    while (current != NULL) {
        if (current->id == id) {
            if (current->previous != NULL) {
                current->previous->next = current->next;
            } else {
                robin->first = current->next;
            }

            if (current->next != NULL) {
                current->next->previous = current->previous;
            } else {
                robin->last = current->previous;
            }

            free(current);
            robin->size--;
            break;
        }
        current = current->next;
    }

    platform_unlock(&robin->lock);
    debouncer_debounce(robin->debouncer);
}

uint8_t round_robin_contains(round_robin_t* robin, size_t id) {
    platform_lock(&robin->lock);

    round_robin_node_t* current = robin->first;
    while (current != NULL) {
        if (current->id == id) {
            platform_unlock(&robin->lock);
            return 1;
        }
        current = current->next;
    }

    platform_unlock(&robin->lock);
    return 0;
}

cbor_item_t* round_robin_to_cbor(round_robin_t* robin) {
    platform_lock(&robin->lock);

    cbor_item_t* array = cbor_new_definite_array(robin->size);
    round_robin_node_t* current = robin->first;
    while (current != NULL) {
        cbor_array_push(array, cbor_move(cbor_build_uint64(current->id)));
        current = current->next;
    }

    platform_unlock(&robin->lock);
    return array;
}

round_robin_t* cbor_to_round_robin(cbor_item_t* cbor, char* robin_path, hierarchical_timing_wheel_t* wheel) {
    round_robin_t* robin = round_robin_create(robin_path, wheel);

    size_t length = cbor_array_size(cbor);
    for (size_t i = 0; i < length; i++) {
        cbor_item_t* item = cbor_move(cbor_array_get(cbor, i));
        round_robin_add(robin, cbor_get_uint64(item));
    }

    return robin;
}

static void round_robin_save(void* ctx) {
    round_robin_t* robin = (round_robin_t*) ctx;
    cbor_item_t* cbor = round_robin_to_cbor(robin);

    uint8_t* cbor_data;
    size_t cbor_size;
    cbor_serialize_alloc(cbor, &cbor_data, &cbor_size);

    FILE* file = fopen(robin->path, "wb");
    if (file) {
        fwrite(cbor_data, sizeof(uint8_t), cbor_size, file);
        fclose(file);
    }

    free(cbor_data);
    cbor_decref(&cbor);
}

// Sections functions
sections_t* sections_create(char* path, size_t size, size_t cache_size, size_t section_concurrency,
                            hierarchical_timing_wheel_t* wheel, size_t wait, size_t max_wait) {
    char* robin_folder = path_join(path, "robin");
    mkdir_p(robin_folder);
    char* robin_path = path_join(robin_folder, ".robin");
    free(robin_folder);

    sections_t* sections = get_clear_memory(sizeof(sections_t));
    sections->wheel = (hierarchical_timing_wheel_t*) refcounter_reference((refcounter_t*) wheel);
    sections->wait = wait;
    sections->max_wait = max_wait;
    sections->section_concurrency = section_concurrency;
    sections->size = size;

    // Initialize transaction range
    sections->oldest_txn_id = (transaction_id_t){0, 0, 0};
    sections->newest_txn_id = (transaction_id_t){0, 0, 0};
    sections->range_path = path_join(path, ".range");

    platform_lock_init(&sections->lock);

    // Initialize sharded checkout locks
    for (size_t i = 0; i < CHECKOUT_LOCK_SHARDS; i++) {
        platform_lock_init(&sections->checkout_shards[i].lock);
        hashmap_init(&sections->checkout_shards[i].sections, (void*) hash_size_t, (void*) compare_size_t);
        hashmap_set_key_alloc_funcs(&sections->checkout_shards[i].sections, duplicate_size_t, (void*) free);
    }

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

    // Load transaction range if exists
    if (access(sections->range_path, F_OK) == 0) {
        sections_load_txn_range(sections);
    }

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
    while (sections->robin->size < sections->section_concurrency) {
        section_t* section = section_create(sections->data_path, sections->meta_path,
                                           sections->size, sections_get_next_id(sections));
        refcounter_yield((refcounter_t*) section);
        sections_lru_cache_put(sections->lru, section);
        round_robin_add(sections->robin, section->id);
    }

    return sections;
}

void sections_destroy(sections_t* sections) {
    // Cleanup sharded checkout locks
    for (size_t i = 0; i < CHECKOUT_LOCK_SHARDS; i++) {
        platform_lock_destroy(&sections->checkout_shards[i].lock);
        hashmap_cleanup(&sections->checkout_shards[i].sections);
    }

    sections_lru_cache_destroy(sections->lru);
    round_robin_destroy(sections->robin);
    if (sections->wheel != NULL) {
        hierarchical_timing_wheel_destroy(sections->wheel);
    }
    free(sections->meta_path);
    free(sections->data_path);
    free(sections->robin_path);
    free(sections->range_path);
    platform_lock_destroy(&sections->lock);
    free(sections);
}

int sections_write(sections_t* sections, transaction_id_t txn_id, buffer_t* data,
                   size_t* section_id, size_t* offset) {
    section_t* section;
    uint8_t full;
    uint8_t tries = 0;
    int result;

    do {
        *section_id = round_robin_next(sections->robin);
        section = sections_checkout(sections, *section_id);
        if (section == NULL) {
            section = section_create(sections->data_path, sections->meta_path,
                                    sections->size, *section_id);
        }
        result = section_write(section, txn_id, data, offset, &full);

        if ((result == 1) || full) {
            sections_full(sections, *section_id);
        }
        tries++;
        sections_checkin(sections, section);
    } while ((result == 1) && (tries < sections->section_concurrency));

    if (result == 0) {
        sections_update_txn_range(sections, txn_id);
    }

    return result;
}

int sections_read(sections_t* sections, size_t section_id, size_t offset,
                  transaction_id_t* txn_id, buffer_t** data) {
    section_t* section = sections_checkout(sections, section_id);
    int result = section_read(section, offset, txn_id, data);
    sections_checkin(sections, section);
    return result;
}

int sections_deallocate(sections_t* sections, size_t section_id, size_t offset, size_t data_size) {
    section_t* section = sections_checkout(sections, section_id);
    int result = section_deallocate(section, offset, data_size);
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
                                sections->size, section_id);
        refcounter_yield((refcounter_t*) section);
        if (section_full(section) == 0) {
            round_robin_add(sections->robin, section_id);
        }
    }

    // Use sharded lock for better concurrency
    size_t shard_idx = get_shard(section_id);
    platform_lock(&sections->checkout_shards[shard_idx].lock);

    checkout_t* checkout = hashmap_get(&sections->checkout_shards[shard_idx].sections, &section_id);
    if (checkout == NULL) {
        checkout = get_clear_memory(sizeof(checkout_t));
        checkout->section = (section_t*) refcounter_reference((refcounter_t*) section);
        checkout->count = 1;
        hashmap_put(&sections->checkout_shards[shard_idx].sections, &section_id, checkout);
    } else {
        checkout->count++;
    }

    platform_unlock(&sections->checkout_shards[shard_idx].lock);

    return (section_t*) refcounter_reference((refcounter_t*) section);
}

void sections_checkin(sections_t* sections, section_t* section) {
    // Use sharded lock for better concurrency
    size_t shard_idx = get_shard(section->id);
    platform_lock(&sections->checkout_shards[shard_idx].lock);

    checkout_t* checkout = hashmap_get(&sections->checkout_shards[shard_idx].sections, &section->id);
    if (checkout != NULL) {
        checkout->count--;
        if (checkout->count == 0) {
            hashmap_remove(&sections->checkout_shards[shard_idx].sections, &section->id);
            free(checkout);
            section_destroy(section);
        }
    }

    platform_unlock(&sections->checkout_shards[shard_idx].lock);
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

    while (sections->robin->size < sections->section_concurrency) {
        section_t* section = section_create(sections->data_path, sections->meta_path,
                                            sections->size, sections_get_next_id(sections));
        refcounter_yield((refcounter_t*) section);
        sections_lru_cache_put(sections->lru, section);
        round_robin_add(sections->robin, section->id);
    }
}

static void sections_free(sections_t* sections, size_t section_id) {
    // Currently no-op - could implement section cleanup here
    (void)sections;
    (void)section_id;
}

void sections_update_txn_range(sections_t* sections, transaction_id_t txn_id) {
    platform_lock(&sections->lock);

    // Update oldest if this is the first transaction or if older than current oldest
    if (sections->oldest_txn_id.time == 0 &&
        sections->oldest_txn_id.nanos == 0 &&
        sections->oldest_txn_id.count == 0) {
        sections->oldest_txn_id = txn_id;
    }

    // Always update newest
    sections->newest_txn_id = txn_id;

    platform_unlock(&sections->lock);
}

void sections_save_txn_range(sections_t* sections) {
    cbor_item_t* array = cbor_new_definite_array(6);
    cbor_array_push(array, cbor_move(cbor_build_uint64(sections->oldest_txn_id.time)));
    cbor_array_push(array, cbor_move(cbor_build_uint64(sections->oldest_txn_id.nanos)));
    cbor_array_push(array, cbor_move(cbor_build_uint64(sections->oldest_txn_id.count)));
    cbor_array_push(array, cbor_move(cbor_build_uint64(sections->newest_txn_id.time)));
    cbor_array_push(array, cbor_move(cbor_build_uint64(sections->newest_txn_id.nanos)));
    cbor_array_push(array, cbor_move(cbor_build_uint64(sections->newest_txn_id.count)));

    uint8_t* cbor_data;
    size_t cbor_size;
    cbor_serialize_alloc(array, &cbor_data, &cbor_size);

    int fd = open(sections->range_path, O_WRONLY | O_TRUNC | O_CREAT, 0644);
    if (fd >= 0) {
        write(fd, cbor_data, cbor_size);
        close(fd);
    }

    free(cbor_data);
    cbor_decref(&array);
}

int sections_load_txn_range(sections_t* sections) {
    int fd = open(sections->range_path, O_RDONLY, 0644);
    if (fd < 0) return 1;

    int32_t file_size = lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);

    uint8_t* buffer = get_memory(file_size);
    read(fd, buffer, file_size);
    close(fd);

    struct cbor_load_result result;
    cbor_item_t* cbor = cbor_load(buffer, file_size, &result);
    free(buffer);

    if (result.error.code != CBOR_ERR_NONE || !cbor_isa_array(cbor)) {
        cbor_decref(&cbor);
        return 1;
    }

    sections->oldest_txn_id.time = cbor_get_uint64(cbor_move(cbor_array_get(cbor, 0)));
    sections->oldest_txn_id.nanos = cbor_get_uint64(cbor_move(cbor_array_get(cbor, 1)));
    sections->oldest_txn_id.count = cbor_get_uint64(cbor_move(cbor_array_get(cbor, 2)));
    sections->newest_txn_id.time = cbor_get_uint64(cbor_move(cbor_array_get(cbor, 3)));
    sections->newest_txn_id.nanos = cbor_get_uint64(cbor_move(cbor_array_get(cbor, 4)));
    sections->newest_txn_id.count = cbor_get_uint64(cbor_move(cbor_array_get(cbor, 5)));

    cbor_decref(&cbor);
    return 0;
}