#include "wal_manager.h"
#include "database.h"
#include "batch.h"
#include "../HBTrie/hbtrie.h"
#include "../HBTrie/path.h"
#include "../HBTrie/identifier.h"
#include "../Util/allocator.h"
#include "../Util/log.h"
#include "../Util/mkdir_p.h"
#include "../Util/path_join.h"
#include "../Util/log.h"
#include "../Time/debouncer.h"
#include <cbor.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <dirent.h>
#include "../Util/log.h"  // For log_warn
#include <cbor.h>

// Forward declaration from wal.c
int deserialize_batch(buffer_t* data, batch_op_t** ops, size_t* count);

// Error codes
#define WAL_ERROR_INVALID_ARG -1
#define WAL_ERROR_IO -2
#define WAL_ERROR_MEMORY -3
#define WAL_ERROR_CORRUPT -4
#define WAL_ERROR_LIMIT -5

// Maximum entries to prevent DoS
#define MANIFEST_MAX_ENTRIES 10000

// Thread-local storage for WAL
static __thread thread_wal_t* thread_local_wal = NULL;

// Default configuration
static void init_default_config(wal_config_t* config) {
    config->sync_mode = WAL_SYNC_IMMEDIATE;
    config->debounce_ms = WAL_DEFAULT_DEBOUNCE_MS;
    config->idle_threshold_ms = WAL_DEFAULT_IDLE_THRESHOLD_MS;
    config->compact_interval_ms = WAL_DEFAULT_COMPACT_INTERVAL_MS;
    config->max_file_size = WAL_DEFAULT_MAX_FILE_SIZE;
}

// Fsync callback for debouncer
static void thread_wal_fsync_callback(void* ctx) {
    thread_wal_t* twal = (thread_wal_t*)ctx;
    if (twal && twal->fd >= 0) {
        fsync(twal->fd);
        twal->pending_writes = 0;
    }
}

// CRC32 lookup table (copy from wal.c)
static const uint32_t crc32_table[256] = {
    0x00000000, 0x77073096, 0xee0e612c, 0x990951ba, 0x076dc419, 0x706af48f,
    0xe963a535, 0x9e6495a3, 0x0edb8832, 0x79dcb8a4, 0xe0d5e91e, 0x97d2d988,
    0x09b64c2b, 0x7eb17cbd, 0xe7b82d07, 0x90bf1d91, 0x1db71064, 0x6ab020f2,
    0xf3b97148, 0x84be41de, 0x1adad47d, 0x6ddde4eb, 0xf4d4b551, 0x83d385c7,
    0x136c9856, 0x646ba8c0, 0xfd62f97a, 0x8a65c9ec, 0x14015c4f, 0x63066cd9,
    0xfa0f3d63, 0x8d080df5, 0x3b6e20c8, 0x4c69105e, 0xd56041e4, 0xa2677172,
    0x3c03e4d1, 0x4b04d447, 0xd20d85fd, 0xa50ab56b, 0x35b5a8fa, 0x42b2986c,
    0xdbbbc9d6, 0xacbcf940, 0x32d86ce3, 0x45df5c75, 0xfcd7c757, 0x8b1ddc93,
    0x19b4a692, 0x6e6b3f45, 0xf77fe7e9, 0x80f0d9aa, 0x0d9d866e, 0xda0be426,
    0xad7fa049, 0x3a3b9d82, 0x4d6e3f2e, 0xd5a5e94f, 0xa2b3c0be, 0x3798a7f1,
    0x4a99a7d9, 0xdd27d263, 0xaa08c3a9, 0x3d6b2b7c, 0x4ae3e13d, 0xd3e1c0c1,
    0xae5c1b1b, 0x398c2b8a, 0x4e4c0b13, 0xd97a7dbf, 0xaaf04c0f, 0x3d2a9d8e,
    0x4a7d0e85, 0xd1d28e5c, 0xa65da570, 0x31b47a8b, 0x4c2d0e87, 0xdb1d3b9e,
    0xac59a35a, 0x3b5e1d71, 0x4c88c3c9, 0xdbb09ed0, 0xad056d8f, 0x3875c7c0,
    0x4f1dc247, 0xd8a6e67a, 0xab07a0bb, 0x3c6e3d8a, 0x498e54f7, 0xde3cb7b4,
    0xa937b96a, 0x3c05a5d1, 0x4b7bb860, 0xd8e03cf3, 0xafd5be6d, 0x38c54aef,
    0x4b8cb63b, 0xd9c1e2b6, 0xae5e89c6, 0x394d18e4, 0x4c0367d3, 0xdb8f6b9f,
    0xa81d3a3a, 0x3d08e57c, 0x4aef0b79, 0xdd7dbd56, 0xa90f3e2e, 0x3c267c0e,
    0x49f61087, 0xddc0d5d2, 0xaa71e9a9, 0x3b629db8, 0x4c8a5b57, 0xd0efdf92,
    0xad9d56c7, 0x38c7e78c, 0x4b0f47c4, 0xd8a7f56e, 0xab03a4d9, 0x3c7ef2d0,
    0x491f8bb2, 0xdd24b8f6, 0xaafa9a3f, 0x3b5fcdd3, 0x4c80f0cd, 0xda7e6ae6,
    0xad67a04c, 0x39452196, 0x4c9b3a8c, 0xd8cb6c9d, 0xabff0385, 0x3b6b41c1,
    0x4c9a5a8b, 0xd8d87c6b, 0xa9e46ec4, 0x3b5e4f7f, 0x4c87b8e6, 0xdd7c6a4a,
    0xaaca0b88, 0x3b9e0c82, 0x4c9e3c9b, 0xd8e36d7f, 0xa9e3b4e9, 0x3b8e0d88,
    0x4c8e10db, 0xd8e27465, 0xa9e4a71e, 0x3b8a0889, 0x4c95b727, 0xd8e54c5f,
    0xa9e1e8e8, 0x3b83d68b, 0x4c90ce89, 0xd8e2747b, 0xa9e4d64e, 0x3b8e1e1c,
    0x4c97a08c, 0xd8e2748a, 0xa9e3a0e8, 0x3b8e1a5e, 0x4c96be96, 0xd8e26e5b,
    0xa9e3c1a7, 0x3b8e078e, 0x4c97e7e2, 0xd8e274f1, 0xa9e4b2ae, 0x3b8bfb6b,
    0x4c9c5e99, 0xd8e37e93, 0xa9e3b9d1, 0x3b8e0d2e, 0x4c97b5ab, 0xd8e27493,
    0xa9e3959b, 0x3b8e076a, 0x4c9b3a9a, 0xd8e3d59c, 0xa9e49b4a, 0x3b8e0c55,
    0x4c96aebc, 0xd8e3b2c9, 0xa9e3a2b1, 0x3b8e0953, 0x4c9b3b14, 0xd8e3c6a5,
    0xa9e4d072, 0x3b8b8ed8, 0x4c9ca5ca, 0xd8e28f98, 0xa9e3f77a, 0x3b8e0b4a,
    0x4c95b9e3, 0xd8e3c4c7, 0xa9e4df9d, 0x3b8e0963, 0x4c98a8c6, 0xd8e398e1,
    0xa9e3b7e3, 0x3b8e0579, 0x4c9b3a1c, 0xd8e274e3, 0xa9e3954f, 0x3b8e0c1f,
    0x4c96b6e4, 0xd8e2746b, 0xa9e3b0d5, 0x3b8e0e0d, 0x4c9b0e2b, 0xd8e37e4e,
    0xa9e49b28, 0x3b8b8e8e, 0x4c9ce6c5, 0xd8e35d8e, 0xa9e4b0ee, 0x3b8e0b5b,
    0x4c95b459, 0xd8e2746c, 0xa9e3e9eb, 0x3b8e0d0d, 0x4c9c5f2e, 0xd8e39f8c,
    0xa9e3e4be, 0x3b8e1e9d, 0x4c97a3cd, 0xd8e37e5d, 0xa9e3b0ce, 0x3b8e088b,
    0x4c96a31a, 0xd8e2d1d7, 0xa9e3a4af, 0x3b8e0a9e, 0x4c9b9b9e, 0xd8e3b4f9,
    0xa9e3d0b4, 0x3b8e08ce, 0x4c9be5d0, 0xd8e3f0a5, 0xa9e4c8de, 0x3b8b8c1e,
    0x4c9c5d2d, 0xd8e3e98d, 0xa9e3c3e8, 0x3b8e1d1a, 0x4c96a4ee, 0xd8e2c0be,
    0xa9e3a1ad, 0x3b8e0d2d, 0x4c9b3dce, 0xd8e39e1d, 0xa9e4e9d1, 0x3b8b8a7e,
    0x4c9dab8e, 0xd8e3a7ad
};

// Compute CRC32
static uint32_t wal_crc32(const uint8_t* data, size_t len) {
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; i++) {
        crc = (crc >> 8) ^ crc32_table[(crc ^ data[i]) & 0xFF];
    }
    return crc ^ 0xFFFFFFFF;
}

// Write uint32 in big-endian
static void write_uint32_be(uint8_t* buf, uint32_t val) {
    buf[0] = (val >> 24) & 0xFF;
    buf[1] = (val >> 16) & 0xFF;
    buf[2] = (val >> 8) & 0xFF;
    buf[3] = val & 0xFF;
}

// Read uint32 from big-endian
static uint32_t read_uint32_be_local(const uint8_t* buf) {
    return ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16) |
           ((uint32_t)buf[2] << 8) | (uint32_t)buf[3];
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
static int read_wal_file(const char* path, recovery_entry_t** entries, size_t* count) {
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
        uint32_t expected_crc = read_uint32_be_local(header + 25);
        uint32_t data_len = read_uint32_be_local(header + 29);

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

        // Verify CRC
        uint32_t actual_crc = wal_crc32(data->data, data_len);
        if (actual_crc != expected_crc) {
            buffer_destroy(data);
            break;
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
static void write_manifest_entry(wal_manager_t* manager, uint64_t thread_id,
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
    (void)bytes_written;  // Suppress unused variable warning

    // Fsync for correctness (manifest must be durable)
    fsync(manager->manifest_fd);
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
        free(twal->file_path);
        free(twal);
        return NULL;
    }

    // Create debouncer if needed
    if (twal->sync_mode == WAL_SYNC_DEBOUNCED && manager->config.debounce_ms > 0) {
        twal->wheel = manager->wheel;  // Use manager's timing wheel
        twal->fsync_debouncer = debouncer_create(manager->wheel, twal,
                                                  thread_wal_fsync_callback,
                                                  NULL,
                                                  manager->config.debounce_ms,
                                                  manager->config.debounce_ms);
        if (twal->fsync_debouncer == NULL) {
            close(twal->fd);
            free(twal->file_path);
            free(twal);
            return NULL;
        }
    }

    platform_lock_init(&twal->lock);
    refcounter_init((refcounter_t*)twal);

    // Write initial manifest entry (ACTIVE) - protected by manifest lock
    platform_lock(&manager->manifest_lock);
    write_manifest_entry(manager, twal->thread_id, twal->file_path,
                        WAL_FILE_ACTIVE, &twal->newest_txn_id);
    platform_unlock(&manager->manifest_lock);

    return twal;
}

// Skeleton implementations (to be filled in next tasks)
wal_manager_t* wal_manager_create(const char* location, wal_config_t* config,
                                   hierarchical_timing_wheel_t* wheel, int* error_code) {
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
    manager->wheel = wheel;  // Store timing wheel for debouncer
    manager->wheel = wheel;  // Store timing wheel for debouncer

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
    wal_manager_t* manager = wal_manager_create(location, config, NULL, error_code);
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
    if (bytes_written != sizeof(header)) {
        platform_unlock(&manager->manifest_lock);
        wal_manager_destroy(manager);
        if (error_code) *error_code = EIO;
        return NULL;
    }
    fsync(manager->manifest_fd);

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

    fsync(manager->manifest_fd);

    // Release lock before calling get_thread_wal (which also acquires manifest_lock)
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

    fsync(manager->manifest_fd);

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
        // Use legacy WAL (not implemented in this task)
        if (error_code) *error_code = ENOTSUP;
        return NULL;
    }

    if (has_legacy && !has_manifest) {
        // Migration needed
        return migrate_legacy_wal(location, config, options, error_code);
    }

    // No migration needed, create or load normally
    return wal_manager_create(location, config, NULL, error_code);
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
                    if (thread_local_wal == twal) {
                        thread_local_wal = NULL;
                    }

                    // Close file descriptor
                    if (twal->fd >= 0) {
                        fsync(twal->fd);
                        close(twal->fd);
                    }

                    // Flush and destroy debouncer if present
                    if (twal->fsync_debouncer != NULL) {
                        debouncer_flush(twal->fsync_debouncer);
                        debouncer_destroy(twal->fsync_debouncer);
                    }

                    // Free file path
                    free(twal->file_path);

                    // Destroy lock
                    platform_lock_destroy(&twal->lock);

                    // Note: twal struct itself is NOT freed to avoid potential use-after-free
                    // by worker threads that may still reference it
                }
            }
            free(manager->threads);
        }

        // Close manifest file
        if (manager->manifest_fd >= 0) {
            fsync(manager->manifest_fd);
            close(manager->manifest_fd);
        }

        // Free paths
        free(manager->location);
        free(manager->manifest_path);

        // Destroy locks
        platform_lock_destroy(&manager->manifest_lock);
        platform_lock_destroy(&manager->threads_lock);
        refcounter_destroy_lock((refcounter_t*)manager);

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
                if (thread_local_wal->fsync_debouncer != NULL) {
                    debouncer_destroy(thread_local_wal->fsync_debouncer);
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
    platform_unlock(&manager->threads_lock);

    return thread_local_wal;
}

int thread_wal_write(thread_wal_t* twal, transaction_id_t txn_id,
                     wal_type_e type, buffer_t* data) {
    if (twal == NULL || data == NULL) {
        return -1;
    }

    // Acquire lock for thread safety
    platform_lock(&twal->lock);

    // Check file size limit (TODO: file rotation not implemented yet)
    if (twal->current_size >= twal->max_size) {
        platform_unlock(&twal->lock);
        return -1;  // File full, needs rotation
    }

    // Entry format: type(1) + txn_id(24) + crc32(4) + data_len(4) + data
    // Calculate CRC32 of data
    uint32_t crc = wal_crc32(data->data, data->size);

    // Prepare header buffer: type(1) + txn_id(24) + crc32(4) + data_len(4)
    uint8_t header_buf[33];  // 1 + 24 + 4 + 4 = 33 bytes
    uint8_t* ptr = header_buf;

    // Write type (1 byte)
    *ptr++ = (uint8_t)type;

    // Write transaction ID (24 bytes, serialized)
    transaction_id_serialize(&txn_id, ptr);
    ptr += 24;

    // Write CRC32 (4 bytes, big-endian)
    write_uint32_be(ptr, crc);
    ptr += 4;

    // Write data length (4 bytes, big-endian)
    write_uint32_be(ptr, (uint32_t)data->size);

    // Write header and data atomically using writev() with O_APPEND
    struct iovec iov[2];
    iov[0].iov_base = header_buf;
    iov[0].iov_len = sizeof(header_buf);
    iov[1].iov_base = data->data;
    iov[1].iov_len = data->size;
    ssize_t bytes_written = writev(twal->fd, iov, 2);
    if (bytes_written != (ssize_t)(sizeof(header_buf) + data->size)) {
        platform_unlock(&twal->lock);
        return -1;
    }

    // Update file size
    twal->current_size += sizeof(header_buf) + data->size;

    // Update transaction ID tracking
    if (twal->oldest_txn_id.time == 0 && twal->oldest_txn_id.count == 0) {
        twal->oldest_txn_id = txn_id;
    }
    twal->newest_txn_id = txn_id;

    // Handle sync modes
    int result = 0;
    switch (twal->sync_mode) {
        case WAL_SYNC_IMMEDIATE:
            // Fsync immediately
            if (fsync(twal->fd) != 0) {
                result = -1;
            }
            twal->pending_writes = 0;
            break;

        case WAL_SYNC_DEBOUNCED:
            // Debounce fsync
            if (twal->fsync_debouncer != NULL) {
                twal->pending_writes++;
                debouncer_debounce(twal->fsync_debouncer);
            }
            break;

        case WAL_SYNC_ASYNC:
            // No fsync, rely on OS page cache
            twal->pending_writes++;
            break;

        default:
            result = -1;
            break;
    }

    platform_unlock(&twal->lock);
    return result;
}

int thread_wal_seal(thread_wal_t* twal) {
    if (twal == NULL) {
        return WAL_ERROR_INVALID_ARG;
    }

    platform_lock(&twal->lock);

    if (twal->fd >= 0) {
        // Flush pending writes
        fsync(twal->fd);
        close(twal->fd);
        twal->fd = -1;
    }

    // Update manifest to mark file as SEALED
    write_manifest_entry(twal->manager, twal->thread_id, twal->file_path,
                        WAL_FILE_SEALED, &twal->newest_txn_id);

    platform_unlock(&twal->lock);

    return 0;
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
        if (read_wal_file(manifest_entries[i].file_path, &file_entries, &file_count) == 0) {
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
            if (read_wal_file(wal_files[i], &file_entries, &file_count) == 0) {
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
    if (entry_count > 0) {
        qsort(all_entries, entry_count, sizeof(recovery_entry_t),
              compare_recovery_entries);
    }

    log_info("WAL Recovery: Total %zu entries to replay", entry_count);

    // 5. Replay in order
    database_t* database = (database_t*)db;
    for (size_t i = 0; i < entry_count; i++) {
        recovery_entry_t* entry = &all_entries[i];
        buffer_t* data = entry->data;

        switch (entry->type) {
            case WAL_BATCH: {
                // Deserialize batch
                batch_op_t* ops = NULL;
                size_t op_count = 0;

                if (deserialize_batch(data, &ops, &op_count) != 0) {
                    fprintf(stderr, "ERROR: Failed to deserialize batch\n");
                    break;
                }

                // Apply each operation to trie
                for (size_t j = 0; j < op_count; j++) {
                    if (ops[j].type == WAL_PUT) {
                        hbtrie_insert_mvcc(database->trie, ops[j].path, ops[j].value, entry->txn_id);
                    } else {
                        hbtrie_delete_mvcc(database->trie, ops[j].path, entry->txn_id);
                    }

                    // Clean up
                    path_destroy(ops[j].path);
                    if (ops[j].value) {
                        identifier_destroy(ops[j].value);
                    }
                }

                free(ops);
                break;
            }
            case WAL_PUT:
            case WAL_DELETE: {
                // Deserialize individual PUT/DELETE entry
                // Format: CBOR array [path, value]
                struct cbor_load_result result;
                cbor_item_t* entry_cbor = cbor_load(data->data, data->size, &result);
                if (entry_cbor == NULL || result.error.code != CBOR_ERR_NONE) {
                    if (entry_cbor) cbor_decref(&entry_cbor);
                    log_error("WAL Recovery: Failed to load entry CBOR");
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
                path_t* path = cbor_to_path(path_cbor, database->chunk_size);
                if (path == NULL) {
                    cbor_decref(&entry_cbor);
                    log_error("WAL Recovery: Failed to deserialize path");
                    break;
                }

                // Get value (for PUT operations)
                identifier_t* value = NULL;
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

                // Apply operation to trie
                if (entry->type == WAL_PUT) {
                    hbtrie_insert_mvcc(database->trie, path, value, entry->txn_id);
                    log_info("WAL Recovery: Applied PUT operation");
                } else {
                    identifier_t* removed = hbtrie_delete_mvcc(database->trie, path, entry->txn_id);
                    if (removed) identifier_destroy(removed);
                    log_info("WAL Recovery: Applied DELETE operation");
                }

                // Clean up
                path_destroy(path);
                if (value) identifier_destroy(value);
                cbor_decref(&entry_cbor);
                break;
            }
            default:
                fprintf(stderr, "WARNING: Unknown WAL entry type: %c\n", entry->type);
                break;
        }
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
            int result = read_wal_file(entries[i].file_path, &file_entries, &file_count);
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
        write_uint32_be(header + 25, crc);
        write_uint32_be(header + 29, (uint32_t)all_entries[i].data->size);
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
    fsync(fd);
    close(fd);

    // Update manifest: mark sealed files as COMPACTED, add compacted file as SEALED
    platform_lock(&manager->manifest_lock);
    for (size_t i = 0; i < count; i++) {
        if (entries[i].status == WAL_FILE_SEALED) {
            write_manifest_entry(manager, entries[i].thread_id,
                                entries[i].file_path, WAL_FILE_COMPACTED,
                                &entries[i].newest_txn_id);
        }
    }

    // Add compacted file
    transaction_id_t empty_txn = {0, 0, 0};
    write_manifest_entry(manager, 0, compacted_path, WAL_FILE_SEALED, &empty_txn);

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

    // Flush all thread-local WALs
    for (size_t i = 0; i < manager->thread_count; i++) {
        thread_wal_t* twal = manager->threads[i];
        if (twal != NULL) {
            platform_lock(&twal->lock);

            // Flush any pending writes to disk
            if (twal->fd >= 0) {
                fsync(twal->fd);
            }

            // Flush debouncer if present
            if (twal->fsync_debouncer != NULL) {
                debouncer_flush(twal->fsync_debouncer);
            }

            platform_unlock(&twal->lock);
        }
    }

    // Flush manifest
    platform_lock(&manager->manifest_lock);
    if (manager->manifest_fd >= 0) {
        fsync(manager->manifest_fd);
    }
    platform_unlock(&manager->manifest_lock);

    platform_unlock(&manager->threads_lock);

    return 0;
}