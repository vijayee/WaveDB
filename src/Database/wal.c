//
// Created by victor on 3/11/26.
//

#include "wal.h"
#include "../Util/allocator.h"
#include "../Util/mkdir_p.h"
#include "../Util/path_join.h"
#include "../Time/debouncer.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <dirent.h>

// Default debounce wait time for fsync (100ms)
#define WAL_DEFAULT_DEBOUNCE_MS 100

// Fsync callback for debounced fsync
static void wal_fsync_callback(void* ctx) {
    wal_t* wal = (wal_t*)ctx;
    if (wal && wal->fd >= 0) {
        fsync(wal->fd);
        wal->pending_writes = 0;
    }
}

// CRC32 lookup table
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
static uint32_t read_uint32_be(const uint8_t* buf) {
    return ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16) |
           ((uint32_t)buf[2] << 8) | (uint32_t)buf[3];
}

// Open or create WAL file
static int wal_open_file(wal_t* wal, const char* path, int create) {
    if (create) {
        // Create new file for writing
        wal->fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    } else {
        // Open existing file for reading
        wal->fd = open(path, O_RDONLY);
    }

    if (wal->fd < 0) {
        return -1;
    }

    // Get current size
    struct stat st;
    if (fstat(wal->fd, &st) == 0) {
        wal->current_size = (size_t)st.st_size;
    } else {
        wal->current_size = 0;
    }

    return 0;
}

// Get current WAL file path
static char* wal_current_path(const char* location) {
    return path_join(location, "current.wal");
}

char* wal_sequence_path(const char* location, uint64_t sequence) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%lu.wal", (unsigned long)sequence);
    return path_join(location, buf);
}

// Find highest sequence number in directory
static int wal_find_highest_sequence(const char* location, uint64_t* sequence) {
    DIR* dir = opendir(location);
    if (dir == NULL) {
        return -1;
    }

    *sequence = 0;
    int found = 0;

    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        // Check for <seq>.wal pattern
        char* dot = strrchr(entry->d_name, '.');
        if (dot == NULL || strcmp(dot, ".wal") != 0) {
            continue;
        }

        // Parse sequence number
        char* end;
        uint64_t seq = strtoull(entry->d_name, &end, 10);
        if (end == dot) {
            if (!found || seq > *sequence) {
                *sequence = seq;
                found = 1;
            }
        }
    }

    closedir(dir);
    return found ? 0 : -1;
}

wal_t* wal_create(char* location, size_t max_size, int* error_code) {
    if (error_code) *error_code = 0;

    // Create directory if needed
    if (mkdir_p(location) != 0) {
        if (error_code) *error_code = errno;
        return NULL;
    }

    wal_t* wal = get_clear_memory(sizeof(wal_t));
    if (wal == NULL) {
        if (error_code) *error_code = ENOMEM;
        return NULL;
    }

    wal->location = strdup(location);
    wal->max_size = (max_size == 0) ? WAL_DEFAULT_MAX_SIZE : max_size;
    wal->sequence = 0;
    wal->current_file = wal_current_path(location);
    wal->oldest_txn_id = (transaction_id_t){0, 0, 0};
    wal->newest_txn_id = (transaction_id_t){0, 0, 0};

    // Initialize sync mode to immediate (safest default)
    wal->sync_mode = WAL_SYNC_IMMEDIATE;
    wal->fsync_debouncer = NULL;
    wal->pending_writes = 0;
    wal->wheel = NULL;

    // Open new file for writing
    if (wal_open_file(wal, wal->current_file, 1) != 0) {
        free(wal->location);
        free(wal->current_file);
        free(wal);
        if (error_code) *error_code = errno;
        return NULL;
    }

    platform_lock_init(&wal->lock);
    refcounter_init((refcounter_t*)wal);

    // Initialize sync mode to immediate (safest default)
    wal->sync_mode = WAL_SYNC_IMMEDIATE;
    wal->fsync_debouncer = NULL;
    wal->pending_writes = 0;
    wal->wheel = NULL;

    return wal;
}

wal_t* wal_create_with_sync(char* location, size_t max_size, wal_sync_mode_e sync_mode,
                             hierarchical_timing_wheel_t* wheel, uint64_t debounce_ms, int* error_code) {
    // Create WAL with default settings
    wal_t* wal = wal_create(location, max_size, error_code);
    if (wal == NULL) {
        return NULL;
    }

    // Configure sync mode
    wal->sync_mode = sync_mode;
    wal->wheel = wheel;

    // Create debouncer if needed
    if (sync_mode == WAL_SYNC_DEBOUNCED) {
        if (wheel == NULL) {
            // Debounced mode requires a timing wheel
            if (error_code) *error_code = EINVAL;
            wal_destroy(wal);
            return NULL;
        }

        // Use default debounce time if not specified
        if (debounce_ms == 0) {
            debounce_ms = WAL_DEFAULT_DEBOUNCE_MS;
        }

        // Create debouncer with reference to this WAL
        wal->fsync_debouncer = debouncer_create(wheel, wal, wal_fsync_callback, NULL, debounce_ms, debounce_ms);
        if (wal->fsync_debouncer == NULL) {
            if (error_code) *error_code = ENOMEM;
            wal_destroy(wal);
            return NULL;
        }
    }

    return wal;
}

wal_t* wal_load(char* location, size_t max_size, int* error_code) {
    if (error_code) *error_code = 0;

    // Find highest sequence
    uint64_t seq = 0;
    if (wal_find_highest_sequence(location, &seq) != 0) {
        // No existing files, create new
        return wal_create(location, max_size, error_code);
    }

    wal_t* wal = get_clear_memory(sizeof(wal_t));
    if (wal == NULL) {
        if (error_code) *error_code = ENOMEM;
        return NULL;
    }

    wal->location = strdup(location);
    wal->max_size = (max_size == 0) ? WAL_DEFAULT_MAX_SIZE : max_size;
    wal->sequence = seq;
    wal->current_file = wal_current_path(location);
    wal->oldest_txn_id = (transaction_id_t){0, 0, 0};
    wal->newest_txn_id = (transaction_id_t){0, 0, 0};

    // Check if current.wal exists
    struct stat st;
    if (stat(wal->current_file, &st) == 0) {
        // current.wal exists, open for reading
        if (wal_open_file(wal, wal->current_file, 0) != 0) {
            free(wal->location);
            free(wal->current_file);
            free(wal);
            if (error_code) *error_code = errno;
            return NULL;
        }
    } else {
        // Open highest sequence file for reading
        char* seq_path = wal_sequence_path(location, seq);
        if (wal_open_file(wal, seq_path, 0) != 0) {
            free(seq_path);
            free(wal->location);
            free(wal->current_file);
            free(wal);
            if (error_code) *error_code = errno;
            return NULL;
        }
        free(seq_path);
    }

    platform_lock_init(&wal->lock);
    refcounter_init((refcounter_t*)wal);

    return wal;
}

void wal_destroy(wal_t* wal) {
    if (wal == NULL) return;

    refcounter_dereference((refcounter_t*)wal);
    if (refcounter_count((refcounter_t*)wal) == 0) {
        // Flush any pending writes before destroying
        if (wal->fd >= 0 && wal->sync_mode == WAL_SYNC_DEBOUNCED && wal->pending_writes > 0) {
            fsync(wal->fd);
            wal->pending_writes = 0;
        }

        // Destroy debouncer if present
        if (wal->fsync_debouncer != NULL) {
            debouncer_destroy(wal->fsync_debouncer);
            wal->fsync_debouncer = NULL;
        }

        if (wal->fd >= 0) {
            close(wal->fd);
        }
        free(wal->location);
        free(wal->current_file);
        platform_lock_destroy(&wal->lock);
        refcounter_destroy_lock((refcounter_t*)wal);
        free(wal);
    }
}

int wal_flush(wal_t* wal) {
    if (wal == NULL) {
        return -1;
    }

    platform_lock(&wal->lock);

    // Flush any pending fsync operations
    if (wal->sync_mode == WAL_SYNC_DEBOUNCED && wal->fsync_debouncer != NULL) {
        debouncer_flush(wal->fsync_debouncer);
    } else if (wal->sync_mode == WAL_SYNC_IMMEDIATE && wal->fd >= 0) {
        // For immediate mode, just fsync directly
        fsync(wal->fd);
    }

    platform_unlock(&wal->lock);
    return 0;
}

int wal_write(wal_t* wal, transaction_id_t txn_id, wal_type_e type, buffer_t* data) {
    if (wal == NULL || data == NULL) {
        return -1;
    }

    platform_lock(&wal->lock);

    // Entry size: 1 byte type + 24 bytes txn_id + 4 bytes CRC + 4 bytes len + data
    size_t entry_size = 1 + 24 + 4 + 4 + data->size;

    // Check if we need to rotate
    int rotated = 0;
    if (wal->current_size + entry_size > wal->max_size) {
        if (wal_swap(wal) != 0) {
            platform_unlock(&wal->lock);
            return -1;
        }
        rotated = 1;
    }

    // Compute CRC
    uint32_t crc = wal_crc32(data->data, data->size);

    // Write header: type (1) + txn_id (24) + CRC (4) + length (4) = 33 bytes
    uint8_t header[33];
    header[0] = (uint8_t)type;
    transaction_id_serialize(&txn_id, header + 1);
    write_uint32_be(header + 25, crc);
    write_uint32_be(header + 29, (uint32_t)data->size);

    ssize_t written = write(wal->fd, header, 33);
    if (written != 33) {
        platform_unlock(&wal->lock);
        return -1;
    }

    // Write data
    written = write(wal->fd, data->data, data->size);
    if (written != (ssize_t)data->size) {
        platform_unlock(&wal->lock);
        return -1;
    }

    // Sync to disk based on sync mode
    wal->pending_writes++;
    if (wal->sync_mode == WAL_SYNC_IMMEDIATE) {
        // Immediate fsync (safest, but slower)
        fsync(wal->fd);
        wal->pending_writes = 0;
    } else if (wal->sync_mode == WAL_SYNC_DEBOUNCED && wal->fsync_debouncer != NULL) {
        // Debounced fsync (high performance)
        debouncer_debounce(wal->fsync_debouncer);
    }
    // WAL_SYNC_ASYNC: No fsync (fastest, least durable)

    // Update transaction tracking
    if (wal->oldest_txn_id.time == 0 && wal->oldest_txn_id.nanos == 0 && wal->oldest_txn_id.count == 0) {
        wal->oldest_txn_id = txn_id;
    }
    wal->newest_txn_id = txn_id;

    wal->current_size += entry_size;
    platform_unlock(&wal->lock);

    return rotated ? 1 : 0;
}

int wal_read(wal_t* wal, transaction_id_t* txn_id, wal_type_e* type, buffer_t** data, uint64_t* cursor) {
    if (wal == NULL || txn_id == NULL || type == NULL || data == NULL || cursor == NULL) {
        return -1;
    }

    platform_lock(&wal->lock);

    // Seek to cursor position
    if (lseek(wal->fd, (off_t)*cursor, SEEK_SET) < 0) {
        platform_unlock(&wal->lock);
        return -1;
    }

    // Read header: type (1) + txn_id (24) + CRC (4) + length (4) = 33 bytes
    uint8_t header[33];
    ssize_t bytes_read = read(wal->fd, header, 33);
    if (bytes_read == 0) {
        // EOF
        platform_unlock(&wal->lock);
        return 1;
    }
    if (bytes_read != 33) {
        platform_unlock(&wal->lock);
        return -1;
    }

    // Parse header
    *type = (wal_type_e)header[0];
    transaction_id_deserialize(txn_id, header + 1);
    uint32_t expected_crc = read_uint32_be(header + 25);
    uint32_t data_len = read_uint32_be(header + 29);

    // Read data
    buffer_t* buf = buffer_create(data_len);
    if (buf == NULL) {
        platform_unlock(&wal->lock);
        return -1;
    }

    bytes_read = read(wal->fd, buf->data, data_len);
    if (bytes_read != (ssize_t)data_len) {
        buffer_destroy(buf);
        platform_unlock(&wal->lock);
        return -1;
    }

    // Verify CRC
    uint32_t actual_crc = wal_crc32(buf->data, data_len);
    if (actual_crc != expected_crc) {
        buffer_destroy(buf);
        platform_unlock(&wal->lock);
        return -1;
    }

    *data = buf;
    *cursor += 33 + data_len;

    platform_unlock(&wal->lock);
    return 0;
}

int wal_swap(wal_t* wal) {
    if (wal == NULL) {
        return -1;
    }

    // Close current file
    if (wal->fd >= 0) {
        fsync(wal->fd);
        close(wal->fd);
    }

    // Rename current.wal to <seq>.wal
    char* seq_path = wal_sequence_path(wal->location, wal->sequence);
    if (rename(wal->current_file, seq_path) != 0 && errno != ENOENT) {
        free(seq_path);
        return -1;
    }
    free(seq_path);

    // Increment sequence
    wal->sequence++;

    // Open new current.wal
    if (wal_open_file(wal, wal->current_file, 1) != 0) {
        return -1;
    }

    return 0;
}

int wal_list_sequences(const char* location, uint64_t** sequences, size_t* count) {
    if (location == NULL || sequences == NULL || count == NULL) {
        return -1;
    }

    DIR* dir = opendir(location);
    if (dir == NULL) {
        return -1;
    }

    // First pass: count sequences
    size_t capacity = 16;
    size_t n = 0;
    uint64_t* seqs = get_clear_memory(capacity * sizeof(uint64_t));
    if (seqs == NULL) {
        closedir(dir);
        return -1;
    }

    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        char* dot = strrchr(entry->d_name, '.');
        if (dot == NULL || strcmp(dot, ".wal") != 0) {
            continue;
        }

        char* end;
        uint64_t seq = strtoull(entry->d_name, &end, 10);
        if (end == dot) {
            if (n >= capacity) {
                capacity *= 2;
                uint64_t* new_seqs = realloc(seqs, capacity * sizeof(uint64_t));
                if (new_seqs == NULL) {
                    free(seqs);
                    closedir(dir);
                    return -1;
                }
                seqs = new_seqs;
            }
            seqs[n++] = seq;
        }
    }

    closedir(dir);

    // Sort sequences
    for (size_t i = 0; i < n; i++) {
        for (size_t j = i + 1; j < n; j++) {
            if (seqs[j] < seqs[i]) {
                uint64_t tmp = seqs[i];
                seqs[i] = seqs[j];
                seqs[j] = tmp;
            }
        }
    }

    *sequences = seqs;
    *count = n;
    return 0;
}