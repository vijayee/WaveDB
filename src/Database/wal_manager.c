#include "wal_manager.h"
#include "../Util/allocator.h"
#include "../Util/mkdir_p.h"
#include "../Util/path_join.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>

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

// Write manifest entry atomically
static void write_manifest_entry(wal_manager_t* manager, uint64_t thread_id,
                                 const char* file_path, wal_file_status_e status,
                                 transaction_id_t* txn_id) {
    manifest_entry_t entry;
    memset(&entry, 0, sizeof(entry));
    entry.thread_id = thread_id;
    strncpy(entry.file_path, file_path, sizeof(entry.file_path) - 1);
    entry.status = status;
    if (txn_id) {
        entry.oldest_txn_id = *txn_id;
        entry.newest_txn_id = *txn_id;
    }
    entry.checksum = wal_crc32((const uint8_t*)&entry, sizeof(entry) - sizeof(entry.checksum));

    // Atomic append (O_APPEND ensures atomicity for < PIPE_BUF)
    write(manager->manifest_fd, &entry, sizeof(entry));

    // Debounced fsync (manifest doesn't need immediate sync)
    // Fsync is handled by background thread
}

// Helper to create thread-local WAL
static thread_wal_t* create_thread_wal(wal_manager_t* manager, uint64_t thread_id) {
    thread_wal_t* twal = get_clear_memory(sizeof(thread_wal_t));
    if (twal == NULL) {
        return NULL;
    }

    // Generate file path
    char filename[64];
    snprintf(filename, sizeof(filename), "thread_%lu.wal", (unsigned long)thread_id);
    twal->file_path = path_join(manager->location, filename);
    twal->thread_id = thread_id;
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
        twal->wheel = NULL;  // Will be set by caller if needed
        twal->fsync_debouncer = debouncer_create(NULL, twal,
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

    // Write initial manifest entry (ACTIVE)
    write_manifest_entry(manager, twal->thread_id, twal->file_path,
                        WAL_FILE_ACTIVE, &twal->newest_txn_id);

    return twal;
}

// Skeleton implementations (to be filled in next tasks)
wal_manager_t* wal_manager_create(const char* location, wal_config_t* config, int* error_code) {
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

    // Create manifest file
    manager->manifest_fd = open(manager->manifest_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (manager->manifest_fd < 0) {
        free(manager->location);
        free(manager->manifest_path);
        free(manager);
        if (error_code) *error_code = errno;
        return NULL;
    }

    // Write manifest header
    manifest_header_t header;
    memset(&header, 0, sizeof(header));
    header.version = MANIFEST_VERSION;
    header.migration_state = 0;  // MIGRATION_NONE
    write(manager->manifest_fd, &header, sizeof(header));

    platform_lock_init(&manager->manifest_lock);
    platform_lock_init(&manager->threads_lock);
    refcounter_init((refcounter_t*)manager);

    return manager;
}

wal_manager_t* wal_manager_load_with_options(const char* location, wal_config_t* config,
                                              wal_recovery_options_t* options, int* error_code) {
    // TODO: Implement
    return NULL;
}

void wal_manager_destroy(wal_manager_t* manager) {
    if (manager == NULL) return;

    refcounter_dereference((refcounter_t*)manager);
    if (refcounter_count((refcounter_t*)manager) == 0) {
        // Destroy all thread-local WALs
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

                    // Destroy debouncer if present
                    if (twal->fsync_debouncer != NULL) {
                        debouncer_destroy(twal->fsync_debouncer);
                    }

                    // Free file path
                    free(twal->file_path);

                    // Clear thread-local storage if this is the current thread's WAL
                    if (thread_local_wal == twal) {
                        thread_local_wal = NULL;
                    }

                    free(twal);
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
    if (thread_local_wal != NULL && thread_local_wal->manager == manager) {
        return thread_local_wal;
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
    // TODO: Implement
    return -1;
}

int thread_wal_seal(thread_wal_t* twal) {
    // TODO: Implement
    return -1;
}

int wal_manager_recover(wal_manager_t* manager, void* db) {
    // TODO: Implement
    return -1;
}

int compact_wal_files(wal_manager_t* manager) {
    // TODO: Implement
    return -1;
}

int wal_manager_flush(wal_manager_t* manager) {
    // TODO: Implement
    return -1;
}