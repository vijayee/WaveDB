#include "wal_manager.h"
#include "database.h"
#include "batch.h"
#include "crc32.h"
#include "wal.h"
#include "../HBTrie/hbtrie.h"
#include "../HBTrie/path.h"
#include "../HBTrie/identifier.h"
#include "../HBTrie/mvcc.h"
#include "../Workers/transaction_id.h"
#include "../Storage/encryption.h"
#include "../Util/allocator.h"
#include "../Util/log.h"
#include "../Util/mkdir_p.h"
#include "../Util/path_join.h"
#include "../Util/endian.h"
#include "../Time/wheel.h"
#include <cbor.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <stdatomic.h>
#include <sys/uio.h>
#include <dirent.h>

// Error codes
#define WAL_ERROR_INVALID_ARG -1
#define WAL_ERROR_IO -2
#define WAL_ERROR_MEMORY -3
#define WAL_ERROR_CORRUPT -4
#define WAL_ERROR_LIMIT -5
#define WAL_ERROR_ENCRYPTION -6

#define WAL_ENCRYPTED_MAGIC 0xE1

// Maximum entries to prevent DoS
#define MANIFEST_MAX_ENTRIES 10000

/**
 * Detect whether WAL payload data is binary or CBOR format.
 *
 * Binary payloads start with WAL_BINARY_MAGIC (0xB1).
 * CBOR arrays start with 0x80-0x9F (definite) or 0x9F (indefinite).
 * This is unambiguous since 0xB1 is not a valid CBOR array header.
 */
static int wal_detect_format(const uint8_t* data, size_t data_len) {
    if (data_len < 1) return WAL_FORMAT_CBOR;  // Too short, try CBOR
    if (data[0] == WAL_BINARY_MAGIC) {
        return WAL_FORMAT_BINARY;
    }
    return WAL_FORMAT_CBOR;
}

/**
 * Decode a binary-encoded PUT entry payload.
 *
 * Binary format:
 *   [0xB1 magic][path_count:2B BE][path_len:4B BE]
 *   For each identifier: [id_len:2B BE][id_data:id_len bytes]
 *   [value_len:4B BE][value:value_len bytes]
 *
 * Returns 0 on success, -1 on error.
 */
static int decode_put_entry_binary(uint8_t* data, size_t data_len,
                                    path_t** out_path, identifier_t** out_value,
                                    uint8_t chunk_size) {
    size_t pos = 0;

    // Skip magic byte
    if (pos + 1 > data_len) return -1;
    if (data[pos] != WAL_BINARY_MAGIC) return -1;
    pos += 1;

    // Read path_count
    if (pos + 2 > data_len) return -1;
    uint16_t path_count = read_be16(data + pos);
    pos += 2;

    // Read path_len (validation)
    if (pos + 4 > data_len) return -1;
    uint32_t path_len = read_be32(data + pos);
    pos += 4;

    // path_len validated against total_id_bytes below

    // Build path
    path_t* path = path_create();
    if (path == NULL) return -1;

    size_t total_id_bytes = 0;
    for (uint16_t i = 0; i < path_count; i++) {
        if (pos + 2 > data_len) { path_destroy(path); return -1; }
        uint16_t id_len = read_be16(data + pos);
        pos += 2;

        if (pos + id_len > data_len) { path_destroy(path); return -1; }
        buffer_t* id_buf = buffer_create_from_pointer_copy(data + pos, id_len);
        if (id_buf == NULL) { path_destroy(path); return -1; }
        identifier_t* ident = identifier_create(id_buf, chunk_size);
        buffer_destroy(id_buf);
        if (ident == NULL) { path_destroy(path); return -1; }
        path_append(path, ident);
        DEREFERENCE(ident);
        pos += id_len;
        total_id_bytes += id_len;
    }

    // Validate path_len matches
    if (total_id_bytes != path_len) {
        log_error("WAL Recovery: Binary path_len mismatch (expected=%u, actual=%zu)",
                  path_len, total_id_bytes);
        path_destroy(path);
        return -1;
    }

    // Read value
    if (pos + 4 > data_len) { path_destroy(path); return -1; }
    uint32_t value_len = read_be32(data + pos);
    pos += 4;

    identifier_t* value = NULL;
    if (value_len > 0) {
        if (pos + value_len > data_len) { path_destroy(path); return -1; }
        buffer_t* val_buf = buffer_create_from_pointer_copy(data + pos, value_len);
        if (val_buf == NULL) { path_destroy(path); return -1; }
        value = identifier_create(val_buf, chunk_size);
        buffer_destroy(val_buf);
        if (value == NULL) { path_destroy(path); return -1; }
        pos += value_len;
    }

    *out_path = path;
    *out_value = value;
    return 0;
}

/**
 * Decode a binary-encoded DELETE entry payload (path only, no value).
 *
 * Binary format:
 *   [0xB1 magic][path_count:2B BE][path_len:4B BE]
 *   For each identifier: [id_len:2B BE][id_data:id_len bytes]
 *
 * Returns 0 on success, -1 on error.
 */
static int decode_delete_entry_binary(uint8_t* data, size_t data_len,
                                       path_t** out_path,
                                       uint8_t chunk_size) {
    size_t pos = 0;

    // Skip magic byte
    if (pos + 1 > data_len) return -1;
    if (data[pos] != WAL_BINARY_MAGIC) return -1;
    pos += 1;

    // Read path_count
    if (pos + 2 > data_len) return -1;
    uint16_t path_count = read_be16(data + pos);
    pos += 2;

    // Read path_len (validation)
    if (pos + 4 > data_len) return -1;
    uint32_t path_len = read_be32(data + pos);
    pos += 4;

    // path_len validated against total_id_bytes below

    // Build path
    path_t* path = path_create();
    if (path == NULL) return -1;

    size_t total_id_bytes = 0;
    for (uint16_t i = 0; i < path_count; i++) {
        if (pos + 2 > data_len) { path_destroy(path); return -1; }
        uint16_t id_len = read_be16(data + pos);
        pos += 2;

        if (pos + id_len > data_len) { path_destroy(path); return -1; }
        buffer_t* id_buf = buffer_create_from_pointer_copy(data + pos, id_len);
        if (id_buf == NULL) { path_destroy(path); return -1; }
        identifier_t* ident = identifier_create(id_buf, chunk_size);
        buffer_destroy(id_buf);
        if (ident == NULL) { path_destroy(path); return -1; }
        path_append(path, ident);
        DEREFERENCE(ident);
        pos += id_len;
        total_id_bytes += id_len;
    }

    // Validate path_len matches
    if (total_id_bytes != path_len) {
        log_error("WAL Recovery: Binary delete path_len mismatch (expected=%u, actual=%zu)",
                  path_len, total_id_bytes);
        path_destroy(path);
        return -1;
    }

    *out_path = path;
    return 0;
}

// Thread-local storage for WAL
static __thread thread_wal_t* thread_local_wal = NULL;

// Default configuration
static void init_default_config(wal_config_t* config) {
    config->sync_mode = WAL_SYNC_IMMEDIATE;
    config->debounce_ms = WAL_DEFAULT_DEBOUNCE_MS;
    config->idle_threshold_ms = WAL_DEFAULT_IDLE_THRESHOLD_MS;
    config->compact_interval_ms = WAL_DEFAULT_COMPACT_INTERVAL_MS;
    config->max_file_size = WAL_DEFAULT_MAX_FILE_SIZE;
    config->max_sealed_wals = WAL_DEFAULT_MAX_SEALED_WALS;
}

// Forward declaration — defined below
static int thread_wal_flush_buffer_locked(thread_wal_t* twal);

// One-shot timer callback: flush buffer if entries exist
static void wal_flush_timer_callback(void* ctx) {
    thread_wal_t* twal = (thread_wal_t*)ctx;
    if (twal == NULL) return;

    platform_lock(&twal->lock);
    if (twal->batch_count > 0) {
        thread_wal_flush_buffer_locked(twal);
    }

    // DEBOUNCED mode: fsync all pending writes to disk
    if (twal->sync_mode == WAL_SYNC_DEBOUNCED && twal->fd >= 0) {
        if (fsync(twal->fd) != 0) {
            log_warn("fsync failed on WAL timer (DEBOUNCED, fd=%d, errno=%d)", twal->fd, errno);
        }
        twal->pending_writes = 0;
    }

    // ASYNC mode: data is now in kernel buffer after write
    if (twal->sync_mode == WAL_SYNC_ASYNC) {
        twal->pending_writes = 0;
    }

    twal->timer_active = 0;
    platform_unlock(&twal->lock);
}

// Flush all buffered entries to disk (called under twal->lock)
static int thread_wal_flush_buffer_locked(thread_wal_t* twal) {
    if (twal == NULL || twal->batch_count == 0 || twal->entry_buf_used == 0) return 0;

    // Write all buffered entries
    ssize_t written = write(twal->fd, twal->entry_buf, twal->entry_buf_used);
    if (written != (ssize_t)twal->entry_buf_used) {
        log_error("WAL: Failed to write buffered entries (expected %zu, wrote %zd)",
                  twal->entry_buf_used, written);
        return -1;
    }

    twal->current_size += twal->entry_buf_used;

    // Handle sync modes:
    // - IMMEDIATE: fsync after every flush (data is on disk)
    // - DEBOUNCED: defer fsync to the timer; just track pending writes
    // - ASYNC: data is in kernel buffer after write(), no fsync needed
    if (twal->sync_mode == WAL_SYNC_IMMEDIATE) {
        if (fsync(twal->fd) != 0) {
            log_warn("fsync failed on WAL flush (IMMEDIATE, fd=%d, errno=%d)", twal->fd, errno);
        }
        twal->pending_writes = 0;
    } else if (twal->sync_mode == WAL_SYNC_ASYNC) {
        twal->pending_writes = 0;  // Data is in kernel buffer, safe from process crash
    } else {
        twal->pending_writes += twal->batch_count;  // DEBOUNCED: track for timer fsync
    }

    // Reset buffer
    twal->entry_buf_used = 0;
    twal->batch_count = 0;

    return 0;
}

// Structure to hold WAL entry during recovery
typedef struct {
    transaction_id_t txn_id;
    wal_type_e type;
    buffer_t* data;
    char* file_path;
    uint64_t offset;
} recovery_entry_t;

// Compare function for qsort
static int compare_recovery_entries(const void* a, const void* b) {
    const recovery_entry_t* ea = (const recovery_entry_t*)a;
    const recovery_entry_t* eb = (const recovery_entry_t*)b;
    return transaction_id_compare(&ea->txn_id, &eb->txn_id);
}

// Read entries from a single WAL file
static int read_wal_file(const char* path, recovery_entry_t** entries, size_t* count, encryption_t* encryption) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return -1;
    }

    size_t capacity = 256;
    recovery_entry_t* list = get_clear_memory(capacity * sizeof(recovery_entry_t));
    size_t n = 0;
    uint64_t cursor = 0;

    while (1) {
        // Read header
        uint8_t header[33];
        ssize_t bytes = read(fd, header, 33);

        if (bytes == 0) {
            break;  // EOF
        }
        if (bytes != 33) {
            break;  // Partial read
        }

        // Parse header
        wal_type_e type = (wal_type_e)header[0];
        transaction_id_t txn_id;
        transaction_id_deserialize(&txn_id, header + 1);
        uint32_t expected_crc = read_be32(header + 25);
        uint32_t data_len = read_be32(header + 29);

        // Read data
        buffer_t* data = buffer_create(data_len);
        if (data == NULL) {
            break;
        }
        bytes = read(fd, data->data, data_len);
        if (bytes != (ssize_t)data_len) {
            buffer_destroy(data);
            break;
        }

        // Verify CRC over the on-disk payload (encrypted or plaintext)
        uint32_t actual_crc = wal_crc32(data->data, data_len);
        if (actual_crc != expected_crc) {
            buffer_destroy(data);
            break;
        }

        // Handle encrypted payload
        if (type == WAL_ENCRYPTED_MAGIC) {
            if (encryption == NULL) {
                log_error("WAL Recovery: Encrypted entry but no encryption context provided");
                buffer_destroy(data);
                break;
            }
            uint8_t* plaintext = NULL;
            size_t pt_len = 0;
            if (encryption_decrypt(encryption, data->data, data_len, &plaintext, &pt_len) != 0) {
                log_error("WAL Recovery: Failed to decrypt WAL entry");
                buffer_destroy(data);
                break;
            }
            if (pt_len < 1) {
                log_error("WAL Recovery: Decrypted payload too short");
                free(plaintext);
                buffer_destroy(data);
                break;
            }
            // First byte of decrypted payload is the original wal_type_e
            type = (wal_type_e)plaintext[0];
            buffer_destroy(data);
            data = buffer_create(pt_len - 1);
            if (data == NULL) {
                free(plaintext);
                break;
            }
            memcpy(data->data, plaintext + 1, pt_len - 1);
            data->size = pt_len - 1;
            free(plaintext);
        }

        // Add to array
        if (n >= capacity) {
            capacity *= 2;
            recovery_entry_t* new_list = get_clear_memory(capacity * sizeof(recovery_entry_t));
            if (new_list == NULL) {
                buffer_destroy(data);
                break;
            }
            memcpy(new_list, list, n * sizeof(recovery_entry_t));
            free(list);
            list = new_list;
        }

        list[n].txn_id = txn_id;
        list[n].type = type;
        list[n].data = data;
        list[n].file_path = strdup(path);
        list[n].offset = cursor;
        n++;

        cursor += 33 + data_len;
    }

    close(fd);
    *entries = list;
    *count = n;
    return 0;
}

// Helper to scan directory for WAL files
static int scan_wal_directory(const char* location, char*** wal_files, size_t* count) {
    DIR* dir = opendir(location);
    if (dir == NULL) {
        return -1;
    }

    size_t capacity = 16;
    char** files = get_clear_memory(capacity * sizeof(char*));
    if (files == NULL) {
        closedir(dir);
        return -1;
    }
    size_t n = 0;

    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        // Check for thread_*.wal pattern
        if (strncmp(entry->d_name, "thread_", 7) == 0) {
            char* dot = strrchr(entry->d_name, '.');
            if (dot != NULL && strcmp(dot, ".wal") == 0) {
                // Found a thread WAL file
                if (n >= capacity) {
                    capacity *= 2;
                    char** new_files = get_clear_memory(capacity * sizeof(char*));
                    if (new_files == NULL) {
                        // Cleanup on error
                        for (size_t i = 0; i < n; i++) {
                            free(files[i]);
                        }
                        free(files);
                        closedir(dir);
                        return -1;
                    }
                    memcpy(new_files, files, n * sizeof(char*));
                    free(files);
                    files = new_files;
                }
                files[n] = path_join(location, entry->d_name);
                if (files[n] == NULL) {
                    // Continue on allocation failure
                    continue;
                }
                n++;
            }
        }
    }

    closedir(dir);
    *wal_files = files;
    *count = n;
    return 0;
}

// Write manifest entry atomically
static int write_manifest_entry(wal_manager_t* manager, uint64_t thread_id,
                                 const char* file_path, wal_file_status_e status,
                                 transaction_id_t* txn_id) {
    manifest_entry_t entry;
    memset(&entry, 0, sizeof(entry));  // Zero all bytes including padding
    entry.thread_id = thread_id;
    strncpy(entry.file_path, file_path, sizeof(entry.file_path) - 1);
    entry.status = status;
    if (txn_id) {
        entry.oldest_txn_id = *txn_id;
        entry.newest_txn_id = *txn_id;
    }
    // Compute checksum over entire entry (padding is already zeroed by memset)
    entry.checksum = wal_crc32((const uint8_t*)&entry, sizeof(entry) - sizeof(entry.checksum));

    // Atomic append (O_APPEND ensures atomicity for < PIPE_BUF)
    ssize_t bytes_written = write(manager->manifest_fd, &entry, sizeof(entry));
    if (bytes_written != (ssize_t)sizeof(entry)) {
        log_error("Manifest write failed: expected %zu, wrote %zd",
                  sizeof(entry), bytes_written);
        return -1;
    }

    // Fsync for correctness (manifest must be durable)
    if (fsync(manager->manifest_fd) != 0) {
        log_error("Manifest fsync failed: fd=%d, errno=%d", manager->manifest_fd, errno);
        return -1;
    }

    return 0;
}

// Read manifest file
int read_manifest(wal_manager_t* manager, manifest_entry_t** entries, size_t* count) {
    if (manager == NULL || entries == NULL || count == NULL) {
        return WAL_ERROR_INVALID_ARG;
    }

    *entries = NULL;
    *count = 0;

    // Acquire manifest lock for thread safety
    platform_lock(&manager->manifest_lock);

    // Open manifest file for reading
    int fd = open(manager->manifest_path, O_RDONLY);
    if (fd < 0) {
        platform_unlock(&manager->manifest_lock);
        return WAL_ERROR_IO;
    }

    // Read header
    manifest_header_t header;
    ssize_t bytes_read = read(fd, &header, sizeof(header));
    if (bytes_read != sizeof(header)) {
        close(fd);
        platform_unlock(&manager->manifest_lock);
        return WAL_ERROR_IO;
    }

    // Check version
    if (header.version != MANIFEST_VERSION) {
        close(fd);
        platform_unlock(&manager->manifest_lock);
        return WAL_ERROR_CORRUPT;
    }

    // Allocate entries array (start with capacity 16, double as needed)
    size_t capacity = 16;
    manifest_entry_t* result = get_clear_memory(capacity * sizeof(manifest_entry_t));
    if (result == NULL) {
        close(fd);
        platform_unlock(&manager->manifest_lock);
        return WAL_ERROR_MEMORY;
    }

    size_t num_entries = 0;

    // Read entries in loop
    while (1) {
        manifest_entry_t entry;

        // Read entry
        bytes_read = read(fd, &entry, sizeof(entry));

        // Stop on EOF
        if (bytes_read == 0) {
            break;
        }

        // Stop on error or partial read
        if (bytes_read != sizeof(entry)) {
            free(result);
            close(fd);
            platform_unlock(&manager->manifest_lock);
            return WAL_ERROR_IO;
        }

        // Check maximum entries limit (DoS protection)
        if (num_entries >= MANIFEST_MAX_ENTRIES) {
            free(result);
            close(fd);
            platform_unlock(&manager->manifest_lock);
            return WAL_ERROR_LIMIT;
        }

        // Preserve the stored checksum before computing
        uint32_t stored_checksum = entry.checksum;

        // Zero padding bytes (and checksum field) before computing checksum
        memset(&entry.checksum, 0, sizeof(entry.checksum));
        // Note: Any padding bytes between newest_txn_id and checksum are already
        // included in the structure, and the memset above zeros them too

        // Verify CRC32 checksum
        uint32_t computed_checksum = wal_crc32((const uint8_t*)&entry,
                                               sizeof(entry) - sizeof(entry.checksum));

        // Restore the checksum for the entry that will be returned
        entry.checksum = stored_checksum;

        // Stop on corrupted entry (checksum mismatch)
        if (computed_checksum != stored_checksum) {
            free(result);
            close(fd);
            platform_unlock(&manager->manifest_lock);
            return WAL_ERROR_CORRUPT;
        }

        // Grow array if needed
        if (num_entries >= capacity) {
            size_t new_capacity = capacity * 2;
            manifest_entry_t* new_result = get_clear_memory(new_capacity * sizeof(manifest_entry_t));
            if (new_result == NULL) {
                free(result);
                close(fd);
                platform_unlock(&manager->manifest_lock);
                return WAL_ERROR_MEMORY;
            }
            // Copy existing entries
            memcpy(new_result, result, num_entries * sizeof(manifest_entry_t));
            free(result);
            result = new_result;
            capacity = new_capacity;
        }

        // Copy valid entry
        result[num_entries++] = entry;
    }

    close(fd);
    platform_unlock(&manager->manifest_lock);

    *entries = result;
    *count = num_entries;

    return 0;
}

// Helper to create thread-local WAL
thread_wal_t* create_thread_wal(wal_manager_t* manager, uint64_t thread_id) {
    thread_wal_t* twal = get_clear_memory(sizeof(thread_wal_t));
    if (twal == NULL) {
        return NULL;
    }

    // Generate file path
    char filename[64];
    snprintf(filename, sizeof(filename), "thread_%lu.wal", (unsigned long)thread_id);
    twal->file_path = path_join(manager->location, filename);
    twal->thread_id = thread_id;
    twal->manager = manager;  // Set back-reference
    twal->sync_mode = manager->config.sync_mode;
    twal->max_size = manager->config.max_file_size;
    twal->oldest_txn_id = (transaction_id_t){0, 0, 0};
    twal->newest_txn_id = (transaction_id_t){0, 0, 0};

    // Open file with O_APPEND for atomic writes
    twal->fd = open(twal->file_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (twal->fd < 0) {
        log_error("Failed to create WAL file: %s (errno=%d)", twal->file_path, errno);
        free(twal->file_path);
        free(twal);
        return NULL;
    }
    log_info("Created WAL file: %s (thread_id=%lu, fd=%d)", twal->file_path, (unsigned long)thread_id, twal->fd);

    // Initialize batch write buffer
    twal->wheel = manager->wheel;
    twal->entry_buf_used = 0;
    twal->batch_count = 0;
    // IMMEDIATE mode requires batch_size=1 (fsync after every entry).
    // Defensive: if someone passes batch_size=0 with IMMEDIATE, force it to 1.
    if (twal->sync_mode == WAL_SYNC_IMMEDIATE) {
        twal->batch_size = 1;
    } else {
        twal->batch_size = 0;  // 0 = flush when buffer full
    }
    twal->timer_active = 0;
    twal->timer_id = 0;
    twal->debounce_ms = manager->config.debounce_ms;

    platform_lock_init(&twal->lock);
    refcounter_init((refcounter_t*)twal);

    // Write initial manifest entry (ACTIVE) - protected by manifest lock
    platform_lock(&manager->manifest_lock);
    int rc = write_manifest_entry(manager, twal->thread_id, twal->file_path,
                        WAL_FILE_ACTIVE, &twal->newest_txn_id);
    platform_unlock(&manager->manifest_lock);

    if (rc != 0) {
        log_warn("Failed to write ACTIVE manifest entry for thread %lu", twal->thread_id);
    }

    return twal;
}

// Skeleton implementations (to be filled in next tasks)
wal_manager_t* wal_manager_create(const char* location, wal_config_t* config,
                                   hierarchical_timing_wheel_t* wheel, encryption_t* encryption, int* error_code) {
    if (error_code) *error_code = 0;

    // Create directory if needed
    if (mkdir_p((char*)location) != 0) {
        if (error_code) *error_code = errno;
        return NULL;
    }

    wal_manager_t* manager = get_clear_memory(sizeof(wal_manager_t));
    if (manager == NULL) {
        if (error_code) *error_code = ENOMEM;
        return NULL;
    }

    // Copy configuration
    if (config) {
        manager->config = *config;
    } else {
        init_default_config(&manager->config);
    }

    // Initialize fields
    manager->location = strdup(location);
    manager->manifest_path = path_join(location, "manifest.dat");
    manager->manifest_fd = -1;
    manager->threads = NULL;
    manager->thread_count = 0;
    manager->thread_capacity = 0;
    manager->sealed_count = 0;
    manager->wheel = wheel;  // Store timing wheel for one-shot timers
    manager->encryption = encryption;

    // Create manifest file
    manager->manifest_fd = open(manager->manifest_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (manager->manifest_fd < 0) {
        free(manager->location);
        free(manager->manifest_path);
        free(manager);
        if (error_code) *error_code = errno;
        return NULL;
    }

    // Check if manifest already has a header (file exists and is not empty)
    struct stat st;
    int has_header = (stat(manager->manifest_path, &st) == 0 && st.st_size >= sizeof(manifest_header_t));

    // Write manifest header only if file is new/empty
    if (!has_header) {
        manifest_header_t header;
        memset(&header, 0, sizeof(header));
        header.version = MANIFEST_VERSION;
        header.migration_state = 0;  // MIGRATION_NONE
        write(manager->manifest_fd, &header, sizeof(header));
    }

    platform_lock_init(&manager->manifest_lock);
    platform_lock_init(&manager->threads_lock);
    refcounter_init((refcounter_t*)manager);

    return manager;
}

// Helper function to migrate legacy WAL
static wal_manager_t* migrate_legacy_wal(const char* location,
                                          wal_config_t* config,
                                          wal_recovery_options_t* options,
                                          int* error_code) {
    // 1. Create manager
    wal_manager_t* manager = wal_manager_create(location, config, NULL, NULL, error_code);
    if (manager == NULL) {
        return NULL;
    }

    // 2. Update manifest header with migration state
    platform_lock(&manager->manifest_lock);

    manifest_header_t header;
    memset(&header, 0, sizeof(header));
    header.version = MANIFEST_VERSION;
    header.migration_state = MIGRATION_IN_PROGRESS;

    // Get current timestamp in milliseconds
    timeval_t now;
    get_time(&now);
    header.migration_timestamp = (now.tv_sec * 1000) + (now.tv_usec / 1000);

    // Seek to beginning and write header
    if (lseek(manager->manifest_fd, 0, SEEK_SET) < 0) {
        platform_unlock(&manager->manifest_lock);
        wal_manager_destroy(manager);
        if (error_code) *error_code = errno;
        return NULL;
    }

    ssize_t bytes_written = write(manager->manifest_fd, &header, sizeof(header));
    if (bytes_written != (ssize_t)sizeof(header)) {
        platform_unlock(&manager->manifest_lock);
        wal_manager_destroy(manager);
        if (error_code) *error_code = EIO;
        return NULL;
    }
    if (fsync(manager->manifest_fd) != 0) {
        log_warn("Manifest header fsync failed (fd=%d, errno=%d)", manager->manifest_fd, errno);
    }

    // 3. Backup legacy WAL
    char* legacy_wal = path_join(location, "current.wal");
    if (legacy_wal == NULL) {
        platform_unlock(&manager->manifest_lock);
        wal_manager_destroy(manager);
        if (error_code) *error_code = ENOMEM;
        return NULL;
    }

    char* backup_wal = path_join(location, "current.wal.backup");
    if (backup_wal == NULL) {
        free(legacy_wal);
        platform_unlock(&manager->manifest_lock);
        wal_manager_destroy(manager);
        if (error_code) *error_code = ENOMEM;
        return NULL;
    }

    int rename_result = rename(legacy_wal, backup_wal);
    if (rename_result != 0) {
        platform_unlock(&manager->manifest_lock);
        wal_manager_destroy(manager);
        free(legacy_wal);
        free(backup_wal);
        if (error_code) *error_code = errno;
        return NULL;
    }

    // Store backup file path in header
    strncpy(header.backup_file, backup_wal, sizeof(header.backup_file) - 1);

    // Seek to beginning and update header with backup path
    if (lseek(manager->manifest_fd, 0, SEEK_SET) < 0) {
        platform_unlock(&manager->manifest_lock);
        rename(backup_wal, legacy_wal);
        wal_manager_destroy(manager);
        free(legacy_wal);
        free(backup_wal);
        if (error_code) *error_code = errno;
        return NULL;
    }

    bytes_written = write(manager->manifest_fd, &header, sizeof(header));
    if (bytes_written != sizeof(header)) {
        platform_unlock(&manager->manifest_lock);
        rename(backup_wal, legacy_wal);
        wal_manager_destroy(manager);
        free(legacy_wal);
        free(backup_wal);
        if (error_code) *error_code = EIO;
        return NULL;
    }

    fsync(manager->manifest_fd);  // Best-effort: migration path
    platform_unlock(&manager->manifest_lock);

    // 4. Read legacy WAL and write to thread-local WAL
    // For this implementation, we'll create an empty thread-local WAL
    // A full implementation would read entries from the backup and
    // write them using thread_wal_write()

    // Create a thread WAL for migration
    thread_wal_t* twal = get_thread_wal(manager);
    if (twal == NULL) {
        // Release lock before cleanup
        // (Note: lock already released before calling get_thread_wal)
        // Rollback: restore backup
        rename(backup_wal, legacy_wal);
        wal_manager_destroy(manager);
        free(legacy_wal);
        free(backup_wal);
        if (error_code) *error_code = ENOMEM;
        return NULL;
    }

    // 5. Mark migration complete
    platform_lock(&manager->manifest_lock);
    header.migration_state = MIGRATION_COMPLETE;

    if (lseek(manager->manifest_fd, 0, SEEK_SET) < 0) {
        platform_unlock(&manager->manifest_lock);
        rename(backup_wal, legacy_wal);
        wal_manager_destroy(manager);
        free(legacy_wal);
        free(backup_wal);
        if (error_code) *error_code = errno;
        return NULL;
    }

    bytes_written = write(manager->manifest_fd, &header, sizeof(header));
    if (bytes_written != sizeof(header)) {
        platform_unlock(&manager->manifest_lock);
        rename(backup_wal, legacy_wal);
        wal_manager_destroy(manager);
        free(legacy_wal);
        free(backup_wal);
        if (error_code) *error_code = EIO;
        return NULL;
    }

    fsync(manager->manifest_fd);  // Best-effort: migration path

    platform_unlock(&manager->manifest_lock);

    // 6. Optionally delete backup
    if (options && !options->keep_backup) {
        unlink(backup_wal);
    }

    free(legacy_wal);
    free(backup_wal);

    return manager;
}

wal_manager_t* wal_manager_load_with_options(const char* location, wal_config_t* config,
                                              wal_recovery_options_t* options, int* error_code) {
    if (error_code) *error_code = 0;

    // Check for legacy WAL
    char* legacy_wal = path_join(location, "current.wal");
    char* manifest = path_join(location, "manifest.dat");

    int has_legacy = (access(legacy_wal, F_OK) == 0);
    int has_manifest = (access(manifest, F_OK) == 0);

    free(legacy_wal);
    free(manifest);

    // Handle force options
    if (options && options->force_legacy) {
        log_warn("force_legacy option is not supported — legacy WAL format has been removed");
        if (error_code) *error_code = ENOTSUP;
        return NULL;
    }

    if (has_legacy && !has_manifest) {
        // Migration needed
        return migrate_legacy_wal(location, config, options, error_code);
    }

    // No migration needed, create or load normally
    return wal_manager_create(location, config, NULL, NULL, error_code);
}

void wal_manager_destroy(wal_manager_t* manager) {
    if (manager == NULL) return;

    refcounter_dereference((refcounter_t*)manager);
    if (refcounter_count((refcounter_t*)manager) == 0) {
        // Destroy all thread-local WALs
        // Note: Thread-local WALs created by async workers may still be in use by those threads.
        // We cannot safely free them here without causing crashes.
        // The OS will clean up all resources when the process exits.
        // This is a known limitation of the thread-local WAL architecture.
        if (manager->threads != NULL) {
            for (size_t i = 0; i < manager->thread_count; i++) {
                thread_wal_t* twal = manager->threads[i];
                if (twal != NULL) {
                    // Clear thread-local reference if this is the current thread's WAL
                    bool is_current_thread = (thread_local_wal == twal);
                    if (is_current_thread) {
                        thread_local_wal = NULL;
                    }

                    // Flush buffer and cancel timer before closing fd
                    platform_lock(&twal->lock);
                    if (twal->batch_count > 0) {
                        thread_wal_flush_buffer_locked(twal);
                    }
                    if (twal->timer_active && twal->wheel != NULL) {
                        hierarchical_timing_wheel_cancel_timer(twal->wheel, twal->timer_id);
                        twal->timer_active = 0;
                    }
                    platform_unlock(&twal->lock);

                    // Close file descriptor (after flushing buffered entries)
                    if (twal->fd >= 0) {
                        fsync(twal->fd);
                        close(twal->fd);
                    }

                    // Free file path
                    free(twal->file_path);

                    // Destroy lock
                    platform_lock_destroy(&twal->lock);

                    // Free the WAL struct
                    // Safe because database_destroy ensures all async operations
                    // have completed before calling wal_manager_destroy
                    free(twal);
                }
            }
            free(manager->threads);
        }

        // Close manifest file
        if (manager->manifest_fd >= 0) {
            fsync(manager->manifest_fd);  // Best-effort: destroy path
            close(manager->manifest_fd);
        }

        // Free paths
        free(manager->location);
        free(manager->manifest_path);

        // Destroy locks
        platform_lock_destroy(&manager->manifest_lock);
        platform_lock_destroy(&manager->threads_lock);

        free(manager);
    }
}

thread_wal_t* get_thread_wal(wal_manager_t* manager) {
    if (manager == NULL) {
        return NULL;
    }

    // Check if thread-local WAL already exists and belongs to this manager
    if (thread_local_wal != NULL) {
        // Check if it belongs to this manager
        if (thread_local_wal->manager == manager) {
            // Validate that the WAL is still in the manager's thread list
            // This catches cases where the WAL was destroyed but thread_local_wal wasn't cleared
            platform_lock(&manager->threads_lock);
            int found = 0;
            for (size_t i = 0; i < manager->thread_count; i++) {
                if (manager->threads[i] == thread_local_wal) {
                    found = 1;
                    break;
                }
            }
            platform_unlock(&manager->threads_lock);

            if (found) {
                return thread_local_wal;
            }
            // WAL was destroyed but thread_local_wal still points to it, clear it
            thread_local_wal = NULL;
        } else {
            // WAL belongs to a different (possibly destroyed) manager, clear it
            thread_local_wal = NULL;
        }
    }

    // Get thread ID
    uint64_t thread_id = (uint64_t)pthread_self();

    // Create thread-local WAL
    thread_local_wal = create_thread_wal(manager, thread_id);
    if (thread_local_wal == NULL) {
        return NULL;
    }

    // Add to manager's thread array
    platform_lock(&manager->threads_lock);
    if (manager->thread_count >= manager->thread_capacity) {
        size_t new_capacity = manager->thread_capacity == 0 ? 16 : manager->thread_capacity * 2;
        thread_wal_t** new_threads = realloc(manager->threads,
                                              new_capacity * sizeof(thread_wal_t*));
        if (new_threads == NULL) {
            // Cleanup the created thread WAL on realloc failure
            if (thread_local_wal != NULL) {
                if (thread_local_wal->fd >= 0) {
                    close(thread_local_wal->fd);
                }
                if (thread_local_wal->file_path != NULL) {
                    free(thread_local_wal->file_path);
                }
                // Cancel timer if active
                if (thread_local_wal->timer_active && thread_local_wal->wheel != NULL) {
                    hierarchical_timing_wheel_cancel_timer(thread_local_wal->wheel, thread_local_wal->timer_id);
                }
                platform_lock_destroy(&thread_local_wal->lock);
                free(thread_local_wal);
                thread_local_wal = NULL;
            }
            platform_unlock(&manager->threads_lock);
            return NULL;
        }
        manager->threads = new_threads;
        manager->thread_capacity = new_capacity;
    }
    manager->threads[manager->thread_count++] = thread_local_wal;
    log_info("Registered thread WAL (manager=%p, total_threads=%zu, thread_id=%lu)",
             (void*)manager, manager->thread_count, (unsigned long)thread_id);
    platform_unlock(&manager->threads_lock);

    return thread_local_wal;
}

/**
 * Rotate the thread WAL: seal the current file and open a new one.
 * Must be called with twal->lock held. Returns 0 on success, -1 on failure.
 * On failure, the old file may already be sealed but no new file is opened.
 */
static int thread_wal_rotate(thread_wal_t* twal) {
    if (twal == NULL) return -1;

    // Save old file info for potential rollback
    char* old_file_path = twal->file_path;
    int old_fd = twal->fd;

    // Seal the current file (flush, close, mark SEALED in manifest)
    if (old_fd >= 0) {
        if (fsync(old_fd) != 0) {
            log_warn("fsync failed on WAL seal (fd=%d, errno=%d)", old_fd, errno);
        }
        close(old_fd);
        twal->fd = -1;
    }

    // Update manifest to mark old file as SEALED (under manifest_lock)
    platform_lock(&twal->manager->manifest_lock);
    int rc = write_manifest_entry(twal->manager, twal->thread_id, old_file_path,
                        WAL_FILE_SEALED, &twal->newest_txn_id);
    platform_unlock(&twal->manager->manifest_lock);

    if (rc != 0) {
        log_error("Failed to write SEALED manifest entry for %s", old_file_path);
        // Attempt rollback: reopen old file
        twal->fd = open(old_file_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (twal->fd < 0) {
            log_error("Rollback failed: cannot reopen old WAL file %s", old_file_path);
        }
        free(old_file_path);
        return -1;
    }

    // Track sealed file count
    twal->manager->sealed_count++;

    // Free old file path (now committed as SEALED)
    free(old_file_path);
    twal->file_path = NULL;

    // Generate new file path with a unique suffix (timestamp-based)
    char filename[96];
    snprintf(filename, sizeof(filename), "thread_%lu_%lu.wal",
             (unsigned long)twal->thread_id,
             (unsigned long)twal->newest_txn_id.time);
    twal->file_path = path_join(twal->manager->location, filename);
    if (twal->file_path == NULL) {
        log_error("Failed to allocate path for rotated WAL file");
        return -1;
    }

    // Open new file
    twal->fd = open(twal->file_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (twal->fd < 0) {
        log_error("Failed to create rotated WAL file: %s (errno=%d)", twal->file_path, errno);
        free(twal->file_path);
        twal->file_path = NULL;
        return -1;
    }

    log_info("Rotated WAL file: %s (thread_id=%lu, fd=%d)",
             twal->file_path, (unsigned long)twal->thread_id, twal->fd);

    // Reset size tracking
    twal->current_size = 0;

    // Reset transaction ID tracking
    twal->oldest_txn_id = (transaction_id_t){0, 0, 0};
    twal->newest_txn_id = (transaction_id_t){0, 0, 0};

    // Write initial manifest entry (ACTIVE) for new file
    platform_lock(&twal->manager->manifest_lock);
    rc = write_manifest_entry(twal->manager, twal->thread_id, twal->file_path,
                        WAL_FILE_ACTIVE, &twal->newest_txn_id);
    platform_unlock(&twal->manager->manifest_lock);

    if (rc != 0) {
        log_warn("Failed to write ACTIVE manifest entry for new WAL file %s", twal->file_path);
    }

    return 0;
}

int thread_wal_write(thread_wal_t* twal, transaction_id_t txn_id,
                     wal_type_e type, buffer_t* data) {
    if (twal == NULL || data == NULL) {
        return -1;
    }

    encryption_t* encryption = twal->manager->encryption;
    uint8_t* payload = data->data;
    size_t payload_len = data->size;
    wal_type_e header_type = type;
    uint8_t* encrypted_buf = NULL;

    if (encryption != NULL) {
        // Prepend original type byte to payload before encryption
        size_t plain_len = 1 + data->size;
        uint8_t* plain_buf = get_memory(plain_len);
        if (plain_buf == NULL) {
            return -1;
        }
        plain_buf[0] = (uint8_t)type;
        memcpy(plain_buf + 1, data->data, data->size);

        size_t ct_len = 0;
        if (encryption_encrypt(encryption, plain_buf, plain_len, &encrypted_buf, &ct_len) != 0) {
            free(plain_buf);
            log_error("WAL: Failed to encrypt entry payload");
            return -1;
        }
        free(plain_buf);

        payload = encrypted_buf;
        payload_len = ct_len;
        header_type = WAL_ENCRYPTED_MAGIC;
    }

    // Entry format: type(1) + txn_id(24) + crc32(4) + data_len(4) + data
    size_t entry_size = 33 + payload_len;

    // Calculate CRC32 of payload (encrypted or plaintext)
    uint32_t crc = wal_crc32(payload, payload_len);

    // Prepare header buffer (before acquiring lock)
    uint8_t header_buf[33];
    uint8_t* ptr = header_buf;
    *ptr++ = (uint8_t)header_type;
    transaction_id_serialize(&txn_id, ptr);
    ptr += 24;
    write_be32(ptr, crc);
    ptr += 4;
    write_be32(ptr, (uint32_t)payload_len);

    // Acquire lock for thread safety
    platform_lock(&twal->lock);

    // Check file size limit — rotate if needed (accounting for buffered data)
    if (twal->current_size + twal->entry_buf_used + entry_size >= twal->max_size) {
        // Flush buffer first
        if (twal->batch_count > 0) {
            if (thread_wal_flush_buffer_locked(twal) != 0) {
                platform_unlock(&twal->lock);
                free(encrypted_buf);
                return -1;
            }
        }

        // Check sealed WAL limit before rotating
        size_t max_sealed = twal->manager->config.max_sealed_wals;
        if (max_sealed > 0 && twal->manager->sealed_count >= max_sealed) {
            platform_unlock(&twal->lock);
            int compact_rc = compact_wal_files(twal->manager);
            platform_lock(&twal->lock);
            if (compact_rc != 0 || twal->manager->sealed_count >= max_sealed) {
                log_warn("WAL sealed limit reached (%zu/%zu), compaction failed or insufficient",
                         twal->manager->sealed_count, max_sealed);
                platform_unlock(&twal->lock);
                free(encrypted_buf);
                return -1;
            }
        }

        if (thread_wal_rotate(twal) != 0) {
            log_error("WAL rotation failed (thread_id=%lu, current_size=%zu, max_size=%zu)",
                     (unsigned long)twal->thread_id, twal->current_size, twal->max_size);
            platform_unlock(&twal->lock);
            free(encrypted_buf);
            return -1;
        }
    }

    // Check if entry fits in buffer — if not, flush first
    if (twal->entry_buf_used + entry_size > sizeof(twal->entry_buf)) {
        // Buffer full — flush before adding
        if (thread_wal_flush_buffer_locked(twal) != 0) {
            platform_unlock(&twal->lock);
            free(encrypted_buf);
            return -1;
        }

        // If entry itself is too large for the buffer, write directly
        if (entry_size > sizeof(twal->entry_buf)) {
            struct iovec iov[2];
            iov[0].iov_base = header_buf;
            iov[0].iov_len = sizeof(header_buf);
            iov[1].iov_base = payload;
            iov[1].iov_len = payload_len;
            ssize_t bytes_written = writev(twal->fd, iov, 2);
            if (bytes_written != (ssize_t)(sizeof(header_buf) + payload_len)) {
                platform_unlock(&twal->lock);
                free(encrypted_buf);
                return -1;
            }
            twal->current_size += sizeof(header_buf) + payload_len;

            // Handle sync for direct write with error checking
            int result = 0;
            if (twal->sync_mode == WAL_SYNC_IMMEDIATE) {
                if (fsync(twal->fd) != 0) {
                    log_warn("fsync failed on WAL direct write (IMMEDIATE, fd=%d, errno=%d)", twal->fd, errno);
                    result = -1;
                }
                twal->pending_writes = 0;
            } else if (twal->sync_mode == WAL_SYNC_DEBOUNCED) {
                if (fsync(twal->fd) != 0) {
                    log_warn("fsync failed on WAL direct write (DEBOUNCED, fd=%d, errno=%d)", twal->fd, errno);
                    result = -1;
                }
                twal->pending_writes = 0;
            } else {
                // ASYNC: data is in kernel buffer
                twal->pending_writes = 0;
            }

            // Update transaction ID tracking
            if (twal->oldest_txn_id.time == 0 && twal->oldest_txn_id.count == 0) {
                twal->oldest_txn_id = txn_id;
            }
            twal->newest_txn_id = txn_id;

            platform_unlock(&twal->lock);
            free(encrypted_buf);
            return result;
        }
    }

    // Copy header + payload to buffer
    memcpy(twal->entry_buf + twal->entry_buf_used, header_buf, sizeof(header_buf));
    twal->entry_buf_used += sizeof(header_buf);
    memcpy(twal->entry_buf + twal->entry_buf_used, payload, payload_len);
    twal->entry_buf_used += payload_len;
    twal->batch_count++;

    // Update transaction ID tracking
    if (twal->oldest_txn_id.time == 0 && twal->oldest_txn_id.count == 0) {
        twal->oldest_txn_id = txn_id;
    }
    twal->newest_txn_id = txn_id;

    // Safety: flush if buffer is more than half full (prevents entry_buf overflow)
    if (twal->entry_buf_used > sizeof(twal->entry_buf) / 2) {
        if (thread_wal_flush_buffer_locked(twal) != 0) {
            platform_unlock(&twal->lock);
            free(encrypted_buf);
            return -1;
        }
        platform_unlock(&twal->lock);
        free(encrypted_buf);
        return 0;
    }

    // Handle flush triggers
    if (twal->batch_size > 0 && twal->batch_count >= twal->batch_size) {
        // Batch limit reached — flush immediately (IMMEDIATE mode)
        if (thread_wal_flush_buffer_locked(twal) != 0) {
            platform_unlock(&twal->lock);
            free(encrypted_buf);
            return -1;
        }
    } else if (twal->batch_count == 1 && !twal->timer_active) {
        // First entry — start one-shot idle drain timer.
        // DEBOUNCED: flush buffer + fsync on timer fire.
        // ASYNC: flush buffer to kernel only (survives process crash, not power failure).
        // IMMEDIATE: no timer needed (batch_size=1 flushes every entry).
        if (twal->sync_mode != WAL_SYNC_IMMEDIATE && twal->wheel != NULL && twal->debounce_ms > 0) {
            twal->timer_active = 1;
            twal->timer_id = hierarchical_timing_wheel_set_timer(
                twal->wheel, twal,
                wal_flush_timer_callback, NULL,
                (timer_duration_t){.milliseconds = twal->debounce_ms});
        }
    }

    platform_unlock(&twal->lock);
    free(encrypted_buf);
    return 0;
}

int thread_wal_seal(thread_wal_t* twal) {
    if (twal == NULL) {
        return WAL_ERROR_INVALID_ARG;
    }

    platform_lock(&twal->lock);

    // Flush buffer and cancel timer before sealing
    if (twal->batch_count > 0) {
        thread_wal_flush_buffer_locked(twal);
    }

    // Cancel one-shot timer if active
    if (twal->timer_active && twal->wheel != NULL) {
        hierarchical_timing_wheel_cancel_timer(twal->wheel, twal->timer_id);
        twal->timer_active = 0;
    }

    if (twal->fd >= 0) {
        // Flush pending writes
        if (fsync(twal->fd) != 0) {
            log_warn("fsync failed on WAL seal (fd=%d, errno=%d)", twal->fd, errno);
        }
        close(twal->fd);
        twal->fd = -1;
    }

    // Update manifest to mark file as SEALED
    platform_lock(&twal->manager->manifest_lock);
    int rc = write_manifest_entry(twal->manager, twal->thread_id, twal->file_path,
                        WAL_FILE_SEALED, &twal->newest_txn_id);
    platform_unlock(&twal->manager->manifest_lock);

    if (rc != 0) {
        log_warn("Failed to write SEALED manifest entry for %s", twal->file_path);
    }

    platform_unlock(&twal->lock);

    return rc;
}

int wal_manager_recover(wal_manager_t* manager, void* db) {
    if (manager == NULL) {
        return WAL_ERROR_INVALID_ARG;
    }

    log_info("WAL Recovery: Starting recovery");

    // Suppress unused parameter warning
    (void)db;

    // 1. Read manifest
    manifest_entry_t* manifest_entries = NULL;
    size_t manifest_count = 0;
    int rc = read_manifest(manager, &manifest_entries, &manifest_count);
    if (rc != 0) {
        log_warn("WAL Recovery: Failed to read manifest (error %d), continuing with directory scan", rc);
        // Manifest read failed - continue with directory scan
        manifest_entries = NULL;
        manifest_count = 0;
    } else {
        log_info("WAL Recovery: Read %zu manifest entries", manifest_count);
    }

    // 2. Scan directory for all thread_*.wal files
    char** wal_files = NULL;
    size_t wal_count = 0;
    scan_wal_directory(manager->location, &wal_files, &wal_count);
    log_info("WAL Recovery: Found %zu WAL files in directory", wal_count);

    // 3. Read all entries from all files
    recovery_entry_t* all_entries = NULL;
    size_t entry_count = 0;
    size_t entry_capacity = 1024;
    all_entries = get_clear_memory(entry_capacity * sizeof(recovery_entry_t));
    if (all_entries == NULL) {
        if (manifest_entries) {
            free(manifest_entries);
        }
        for (size_t i = 0; i < wal_count; i++) {
            free(wal_files[i]);
        }
        free(wal_files);
        return WAL_ERROR_MEMORY;
    }

    // Read from manifest entries
    for (size_t i = 0; i < manifest_count; i++) {
        if (manifest_entries[i].status == WAL_FILE_COMPACTED) {
            continue;  // Skip compacted files
        }

        recovery_entry_t* file_entries = NULL;
        size_t file_count = 0;
        if (read_wal_file(manifest_entries[i].file_path, &file_entries, &file_count, manager->encryption) == 0) {
            // Add to all_entries
            if (entry_count + file_count >= entry_capacity) {
                entry_capacity = (entry_count + file_count) * 2;
                recovery_entry_t* new_entries = get_clear_memory(entry_capacity * sizeof(recovery_entry_t));
                if (new_entries == NULL) {
                    free(all_entries);
                    if (manifest_entries) {
                        free(manifest_entries);
                    }
                    for (size_t j = 0; j < wal_count; j++) {
                        free(wal_files[j]);
                    }
                    free(wal_files);
                    return WAL_ERROR_MEMORY;
                }
                memcpy(new_entries, all_entries, entry_count * sizeof(recovery_entry_t));
                free(all_entries);
                all_entries = new_entries;
            }
            memcpy(&all_entries[entry_count], file_entries, file_count * sizeof(recovery_entry_t));
            entry_count += file_count;
            free(file_entries);
        }
    }

    // Read from directory scan (files not in manifest)
    for (size_t i = 0; i < wal_count; i++) {
        // Check if file is already in manifest
        int in_manifest = 0;
        for (size_t j = 0; j < manifest_count; j++) {
            if (strcmp(wal_files[i], manifest_entries[j].file_path) == 0) {
                in_manifest = 1;
                break;
            }
        }

        if (!in_manifest) {
            recovery_entry_t* file_entries = NULL;
            size_t file_count = 0;
            if (read_wal_file(wal_files[i], &file_entries, &file_count, manager->encryption) == 0) {
                if (entry_count + file_count >= entry_capacity) {
                    entry_capacity = (entry_count + file_count) * 2;
                    recovery_entry_t* new_entries = get_clear_memory(entry_capacity * sizeof(recovery_entry_t));
                    if (new_entries == NULL) {
                        free(all_entries);
                        if (manifest_entries) {
                            free(manifest_entries);
                        }
                        for (size_t k = 0; k < wal_count; k++) {
                            free(wal_files[k]);
                        }
                        free(wal_files);
                        return WAL_ERROR_MEMORY;
                    }
                    memcpy(new_entries, all_entries, entry_count * sizeof(recovery_entry_t));
                    free(all_entries);
                    all_entries = new_entries;
                }
                memcpy(&all_entries[entry_count], file_entries, file_count * sizeof(recovery_entry_t));
                entry_count += file_count;
                free(file_entries);
            }
        }
    }

    // 4. Sort by transaction ID
    log_info("WAL Recovery: Sorting %zu entries by transaction ID", entry_count);

    if (entry_count > 0) {
        qsort(all_entries, entry_count, sizeof(recovery_entry_t),
              compare_recovery_entries);
    }

    log_info("WAL Recovery: Total %zu entries to replay", entry_count);

    // Check entries array
    if (all_entries == NULL && entry_count > 0) {
        log_error("all_entries is NULL but entry_count=%zu", entry_count);
        // Clean up and return error
        if (manifest_entries) free(manifest_entries);
        for (size_t i = 0; i < wal_count; i++) free(wal_files[i]);
        free(wal_files);
        return WAL_ERROR_MEMORY;
    }

    // Track highest transaction ID for transaction manager update
    transaction_id_t max_txn_id;
    memset(&max_txn_id, 0, sizeof(transaction_id_t));
    log_info("WAL Recovery: Initialized max_txn_id");

    // 5. Replay in order
    database_t* database = (database_t*)db;
    log_info("WAL Recovery: Cast database pointer");

    log_info("WAL Recovery: Starting replay loop, entry_count=%zu, all_entries=%p", entry_count, (void*)all_entries);

    for (size_t i = 0; i < entry_count; i++) {
        log_info("WAL Recovery: Accessing entry %zu of %zu", i, entry_count);
        recovery_entry_t* entry = &all_entries[i];

        // Track highest transaction ID
        if (transaction_id_compare(&entry->txn_id, &max_txn_id) > 0) {
            max_txn_id = entry->txn_id;
        }
        buffer_t* data = entry->data;

        log_info("WAL Recovery: Processing entry %zu: type=%d, data_size=%zu, txn_id=%lu.%09lu.%lu",
                 i, entry->type, data ? data->size : 0,
                 entry->txn_id.time, entry->txn_id.nanos, entry->txn_id.count);

        // Skip actual database operations if no database provided (testing mode)
        if (database == NULL) {
            log_info("WAL Recovery: No database provided, skipping entry application");
            continue;
        }

        switch (entry->type) {
            case WAL_BATCH: {
                // Deserialize batch from CBOR format
                // Format: CBOR array of [type_uint8, path_cbor, value_cbor?] entries
                struct cbor_load_result result;
                cbor_item_t* batch_cbor = cbor_load(data->data, data->size, &result);
                if (batch_cbor == NULL || result.error.code != CBOR_ERR_NONE) {
                    if (batch_cbor) cbor_decref(&batch_cbor);
                    log_error("WAL Recovery: Failed to load batch CBOR (error=%d)", result.error.code);
                    break;
                }

                if (!cbor_isa_array(batch_cbor)) {
                    cbor_decref(&batch_cbor);
                    log_error("WAL Recovery: Batch CBOR is not an array");
                    break;
                }

                size_t op_count = cbor_array_size(batch_cbor);
                batch_op_t* ops = get_clear_memory(op_count * sizeof(batch_op_t));
                if (ops == NULL) {
                    cbor_decref(&batch_cbor);
                    log_error("WAL Recovery: Failed to allocate batch ops");
                    break;
                }

                memset(ops, 0, op_count * sizeof(batch_op_t));
                cbor_item_t** entries = cbor_array_handle(batch_cbor);
                size_t valid_ops = 0;

                for (size_t j = 0; j < op_count; j++) {
                    cbor_item_t* op_array = entries[j];
                    if (!cbor_isa_array(op_array)) {
                        log_error("WAL Recovery: Batch entry %zu is not an array", j);
                        continue;
                    }

                    size_t array_size = cbor_array_size(op_array);
                    if (array_size < 2) {
                        log_error("WAL Recovery: Batch entry %zu has too few elements", j);
                        continue;
                    }

                    cbor_item_t** op_items = cbor_array_handle(op_array);

                    // Type (uint8)
                    uint8_t op_type = cbor_get_uint8(op_items[0]);
                    ops[valid_ops].type = (wal_type_e)op_type;

                    // Path
                    path_t* path = cbor_to_path(op_items[1], database->chunk_size);
                    if (path == NULL) {
                        log_error("WAL Recovery: Failed to deserialize path in batch entry %zu", j);
                        continue;
                    }
                    ops[valid_ops].path = path;

                    // Value (for PUT operations)
                    if (op_type == WAL_PUT && array_size >= 3) {
                        identifier_t* value = cbor_to_identifier(op_items[2], database->chunk_size);
                        if (value == NULL) {
                            log_error("WAL Recovery: Failed to deserialize value in batch entry %zu", j);
                            path_destroy(path);
                            ops[valid_ops].path = NULL;
                            continue;
                        }
                        ops[valid_ops].value = value;
                    } else {
                        ops[valid_ops].value = NULL;
                    }

                    valid_ops++;
                }

                // Apply each operation to trie
                for (size_t j = 0; j < valid_ops; j++) {
                    if (ops[j].type == WAL_PUT) {
                        hbtrie_insert(database->trie, ops[j].path, ops[j].value, entry->txn_id);
                    } else {
                        identifier_t* removed = hbtrie_delete(database->trie, ops[j].path, entry->txn_id);
                        if (removed) identifier_destroy(removed);
                    }

                    // Clean up
                    path_destroy(ops[j].path);
                    if (ops[j].value) {
                        identifier_destroy(ops[j].value);
                    }
                }

                free(ops);
                cbor_decref(&batch_cbor);
                break;
            }
            case WAL_PUT:
            case WAL_DELETE: {
                // Detect payload format: binary or CBOR
                int format = wal_detect_format(data->data, data->size);
                path_t* path = NULL;
                identifier_t* value = NULL;

                log_info("WAL Recovery: Deserializing entry (type=%s, data_size=%zu, format=%s)",
                         entry->type == WAL_PUT ? "PUT" : "DELETE",
                         data ? data->size : 0,
                         format == WAL_FORMAT_BINARY ? "binary" : "CBOR");

                if (format == WAL_FORMAT_BINARY) {
                    // Binary format
                    if (entry->type == WAL_PUT) {
                        if (decode_put_entry_binary(data->data, data->size,
                                                    &path, &value,
                                                    database->chunk_size) != 0) {
                            log_error("WAL Recovery: Failed to decode binary PUT entry");
                            break;
                        }
                    } else {
                        if (decode_delete_entry_binary(data->data, data->size,
                                                       &path,
                                                       database->chunk_size) != 0) {
                            log_error("WAL Recovery: Failed to decode binary DELETE entry");
                            break;
                        }
                    }
                } else {
                    // Legacy CBOR format
                    struct cbor_load_result result;
                    cbor_item_t* entry_cbor;

                    entry_cbor = cbor_load(data->data, data->size, &result);
                    if (entry_cbor == NULL || result.error.code != CBOR_ERR_NONE) {
                        if (entry_cbor) cbor_decref(&entry_cbor);
                        log_error("WAL Recovery: Failed to load entry CBOR (error=%d)", result.error.code);
                        break;
                    }

                    // Verify it's an array with correct number of elements
                    // PUT operations have 2 elements [path, value]
                    // DELETE operations have 1 element [path]
                    size_t expected_size = (entry->type == WAL_PUT) ? 2 : 1;
                    if (!cbor_isa_array(entry_cbor) || cbor_array_size(entry_cbor) != expected_size) {
                        cbor_decref(&entry_cbor);
                        log_error("WAL Recovery: Invalid entry format (expected %zu elements, got %zu)",
                                  expected_size, cbor_isa_array(entry_cbor) ? cbor_array_size(entry_cbor) : 0);
                        break;
                    }

                    // Get path
                    cbor_item_t* path_cbor = cbor_array_handle(entry_cbor)[0];
                    path = cbor_to_path(path_cbor, database->chunk_size);
                    if (path == NULL) {
                        cbor_decref(&entry_cbor);
                        log_error("WAL Recovery: Failed to deserialize path");
                        break;
                    }

                    // Get value (for PUT operations)
                    if (entry->type == WAL_PUT) {
                        cbor_item_t* value_cbor = cbor_array_handle(entry_cbor)[1];
                        value = cbor_to_identifier(value_cbor, database->chunk_size);
                        if (value == NULL) {
                            path_destroy(path);
                            cbor_decref(&entry_cbor);
                            log_error("WAL Recovery: Failed to deserialize value");
                            break;
                        }
                    }

                    cbor_decref(&entry_cbor);
                }

                // Apply operation to trie
                if (entry->type == WAL_PUT) {
                    if (path == NULL) {
                        log_error("WAL Recovery: Path is NULL, skipping entry");
                        if (value) identifier_destroy(value);
                        break;
                    }

                    // Convert first identifier to string for logging
                    char key_str[256] = {0};
                    if (path->identifiers.length > 0) {
                        identifier_t* id = path->identifiers.data[0];
                        if (id->chunks.length > 0) {
                            chunk_t* chunk = (chunk_t*)id->chunks.data[0];
                            if (chunk) {
                                size_t copy_len = (chunk->size < 255) ? chunk->size : 255;
                                memcpy(key_str, chunk->data, copy_len);
                                key_str[copy_len] = '\0';
                            }
                        }
                    }

                    log_info("WAL Recovery: Applying PUT key='%s' (path_depth=%zu, value_len=%zu, txn_id=%lu.%09lu.%lu)",
                             key_str, path->identifiers.length, value ? value->length : 0,
                             entry->txn_id.time, entry->txn_id.nanos, entry->txn_id.count);

                    hbtrie_insert(database->trie, path, value, entry->txn_id);
                } else {
                    if (path == NULL) {
                        log_error("WAL Recovery: Path is NULL for DELETE, skipping entry");
                        break;
                    }

                    identifier_t* removed = hbtrie_delete(database->trie, path, entry->txn_id);
                    if (removed) identifier_destroy(removed);
                    log_info("WAL Recovery: Applied DELETE operation (txn_id=%lu.%09lu.%lu)",
                             entry->txn_id.time, entry->txn_id.nanos, entry->txn_id.count);
                }

                // Clean up
                path_destroy(path);
                if (value) identifier_destroy(value);
                break;
            }
            default:
                log_warn("Unknown WAL entry type: %c", entry->type);
                break;
        }
    }

    // Update transaction manager with highest transaction ID
    // This makes recovered MVCC entries visible to subsequent reads
    if (database != NULL && entry_count > 0 && database->tx_manager != NULL) {
        // Use atomic store to safely update the transaction ID
        atomic_store(&database->tx_manager->last_committed_txn_id, max_txn_id);
        log_info("WAL Recovery: Updated last_committed_txn_id (count=%lu)", max_txn_id.count);

        // Advance global transaction ID generator to prevent collisions
        transaction_id_advance_to(&max_txn_id);
    }

    // 6. Clean up
    for (size_t i = 0; i < entry_count; i++) {
        buffer_destroy(all_entries[i].data);
        free(all_entries[i].file_path);
    }
    free(all_entries);
    if (manifest_entries) {
        free(manifest_entries);
    }
    for (size_t i = 0; i < wal_count; i++) {
        free(wal_files[i]);
    }
    free(wal_files);

    return 0;
}

int wal_manager_seal_and_compact(wal_manager_t* manager) {
    if (manager == NULL) return -1;

    platform_lock(&manager->threads_lock);
    // Seal all active thread-local WALs
    for (size_t i = 0; i < manager->thread_count; i++) {
        thread_wal_t* twal = manager->threads[i];
        if (twal != NULL && twal->fd >= 0) {
            platform_unlock(&manager->threads_lock);
            thread_wal_seal(twal);
            platform_lock(&manager->threads_lock);
        }
    }
    platform_unlock(&manager->threads_lock);

    // Compact all sealed WAL files
    return compact_wal_files(manager);
}

int compact_wal_files(wal_manager_t* manager) {
    if (manager == NULL) {
        return WAL_ERROR_INVALID_ARG;
    }

    // Find all SEALED files
    manifest_entry_t* entries = NULL;
    size_t count = 0;
    int rc = read_manifest(manager, &entries, &count);
    if (rc != 0) {
        return rc;
    }

    size_t sealed_count = 0;
    for (size_t i = 0; i < count; i++) {
        if (entries[i].status == WAL_FILE_SEALED) {
            sealed_count++;
        }
    }

    if (sealed_count == 0) {
        free(entries);
        return 0;  // Nothing to compact
    }

    // Read all entries from sealed files
    recovery_entry_t* all_entries = get_clear_memory(1024 * sizeof(recovery_entry_t));
    if (all_entries == NULL) {
        free(entries);
        return WAL_ERROR_MEMORY;
    }

    size_t entry_count = 0;
    size_t entry_capacity = 1024;

    for (size_t i = 0; i < count; i++) {
        if (entries[i].status == WAL_FILE_SEALED) {
            recovery_entry_t* file_entries = NULL;
            size_t file_count = 0;
            int result = read_wal_file(entries[i].file_path, &file_entries, &file_count, manager->encryption);
            if (result == 0 && file_count > 0) {
                // Expand array if needed
                if (entry_count + file_count >= entry_capacity) {
                    entry_capacity = (entry_count + file_count) * 2;
                    recovery_entry_t* new_entries = get_clear_memory(entry_capacity * sizeof(recovery_entry_t));
                    if (new_entries == NULL) {
                        // Cleanup on error
                        for (size_t j = 0; j < entry_count; j++) {
                            buffer_destroy(all_entries[j].data);
                            free(all_entries[j].file_path);
                        }
                        free(all_entries);
                        free(file_entries);
                        free(entries);
                        return WAL_ERROR_MEMORY;
                    }
                    memcpy(new_entries, all_entries, entry_count * sizeof(recovery_entry_t));
                    free(all_entries);
                    all_entries = new_entries;
                }
                // Copy entries
                memcpy(&all_entries[entry_count], file_entries, file_count * sizeof(recovery_entry_t));
                entry_count += file_count;
            }
            free(file_entries);
        }
    }

    // Sort by transaction ID
    qsort(all_entries, entry_count, sizeof(recovery_entry_t),
          compare_recovery_entries);

    // Generate compacted file path
    char compacted_path[512];
    snprintf(compacted_path, sizeof(compacted_path),
             "%s/compacted_%lu.wal", manager->location, (unsigned long)time(NULL));

    int fd = open(compacted_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) {
        for (size_t i = 0; i < entry_count; i++) {
            buffer_destroy(all_entries[i].data);
            free(all_entries[i].file_path);
        }
        free(all_entries);
        free(entries);
        return WAL_ERROR_IO;
    }

    // Write all entries to compacted file
    for (size_t i = 0; i < entry_count; i++) {
        // Write header
        uint8_t header[33];
        header[0] = (uint8_t)all_entries[i].type;
        transaction_id_serialize(&all_entries[i].txn_id, header + 1);
        uint32_t crc = wal_crc32(all_entries[i].data->data, all_entries[i].data->size);
        write_be32(header + 25, crc);
        write_be32(header + 29, (uint32_t)all_entries[i].data->size);
        ssize_t bytes_written = write(fd, header, 33);
        if (bytes_written != 33) {
            close(fd);
            // Cleanup
            for (size_t j = 0; j < entry_count; j++) {
                buffer_destroy(all_entries[j].data);
                free(all_entries[j].file_path);
            }
            free(all_entries);
            free(entries);
            return WAL_ERROR_IO;
        }

        // Write data
        bytes_written = write(fd, all_entries[i].data->data, all_entries[i].data->size);
        if (bytes_written != (ssize_t)all_entries[i].data->size) {
            close(fd);
            // Cleanup
            for (size_t j = 0; j < entry_count; j++) {
                buffer_destroy(all_entries[j].data);
                free(all_entries[j].file_path);
            }
            free(all_entries);
            free(entries);
            return WAL_ERROR_IO;
        }
    }
    if (fsync(fd) != 0) {
        log_warn("fsync failed on compacted WAL file (errno=%d)", errno);
    }
    close(fd);

    // Update manifest: mark sealed files as COMPACTED, add compacted file as SEALED
    platform_lock(&manager->manifest_lock);
    size_t compacted_count = 0;
    for (size_t i = 0; i < count; i++) {
        if (entries[i].status == WAL_FILE_SEALED) {
            int rc = write_manifest_entry(manager, entries[i].thread_id,
                                entries[i].file_path, WAL_FILE_COMPACTED,
                                &entries[i].newest_txn_id);
            if (rc != 0) {
                log_warn("Failed to write COMPACTED manifest entry for %s", entries[i].file_path);
            }
            compacted_count++;
        }
    }

    // Update sealed count
    if (manager->sealed_count >= compacted_count) {
        manager->sealed_count -= compacted_count;
    } else {
        manager->sealed_count = 0;
    }

    // Add compacted file
    transaction_id_t empty_txn = {0, 0, 0};
    rc = write_manifest_entry(manager, 0, compacted_path, WAL_FILE_SEALED, &empty_txn);
    if (rc != 0) {
        log_warn("Failed to write SEALED manifest entry for compacted file %s", compacted_path);
    }

    platform_unlock(&manager->manifest_lock);

    // Delete old sealed files
    for (size_t i = 0; i < count; i++) {
        if (entries[i].status == WAL_FILE_SEALED) {
            unlink(entries[i].file_path);
        }
    }

    // Cleanup
    for (size_t i = 0; i < entry_count; i++) {
        buffer_destroy(all_entries[i].data);
        free(all_entries[i].file_path);
    }
    free(all_entries);
    free(entries);

    return 0;
}

int wal_manager_flush(wal_manager_t* manager) {
    if (manager == NULL) return -1;

    platform_lock(&manager->threads_lock);
    log_info("WAL Manager flush: manager=%p, thread_count=%zu", (void*)manager, manager->thread_count);

    // Flush all thread-local WALs
    for (size_t i = 0; i < manager->thread_count; i++) {
        thread_wal_t* twal = manager->threads[i];
        log_info("  Thread %zu: twal=%p, fd=%d, file_path=%s",
                 i, (void*)twal, twal ? twal->fd : -1, twal ? twal->file_path : "NULL");
        if (twal != NULL) {
            platform_lock(&twal->lock);

            // Flush buffer first (writes buffered entries to kernel),
            // then fsync (ensures data is on disk for DEBOUNCED/IMMEDIATE).
            // Order matters: fsync before buffer flush would miss the
            // most recently buffered entries.
            if (twal->batch_count > 0) {
                thread_wal_flush_buffer_locked(twal);
            }
            if (twal->fd >= 0) {
                if (fsync(twal->fd) != 0) {
                    log_warn("fsync failed on WAL flush (fd=%d, errno=%d)", twal->fd, errno);
                }
                log_info("  Flushed WAL file: %s", twal->file_path);
                twal->pending_writes = 0;
            }
            // Cancel timer if active
            if (twal->timer_active && twal->wheel != NULL) {
                hierarchical_timing_wheel_cancel_timer(twal->wheel, twal->timer_id);
                twal->timer_active = 0;
            }

            platform_unlock(&twal->lock);
        }
    }

    // Flush manifest
    platform_lock(&manager->manifest_lock);
    if (manager->manifest_fd >= 0) {
        fsync(manager->manifest_fd);  // Best-effort: flush path
    }
    platform_unlock(&manager->manifest_lock);

    platform_unlock(&manager->threads_lock);

    return 0;
}

void clear_thread_wal_reference(void) {
    // Free the thread-local WAL if it exists
    if (thread_local_wal != NULL) {
        // Close file descriptor if open
        if (thread_local_wal->fd >= 0) {
            fsync(thread_local_wal->fd);
            close(thread_local_wal->fd);
        }
        // Free file path
        free(thread_local_wal->file_path);
        // Destroy lock
        platform_lock_destroy(&thread_local_wal->lock);
        // Free the struct
        free(thread_local_wal);
        thread_local_wal = NULL;
    }
}