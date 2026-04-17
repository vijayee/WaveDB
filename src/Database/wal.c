//
// Created by victor on 3/11/26.
//

#include "wal.h"
#include "batch.h"
#include "crc32.h"
#include "../Util/endian.h"
#include "../Util/allocator.h"
#include "../Util/mkdir_p.h"
#include "../Util/path_join.h"
#include "../Time/debouncer.h"
#include <cbor.h>
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

        // Flush and destroy debouncer if present
        if (wal->fsync_debouncer != NULL) {
            debouncer_flush(wal->fsync_debouncer);
            debouncer_destroy(wal->fsync_debouncer);
            wal->fsync_debouncer = NULL;
        }

        if (wal->fd >= 0) {
            close(wal->fd);
        }
        free(wal->location);
        free(wal->current_file);
        platform_lock_destroy(&wal->lock);
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
    write_be32(header + 25, crc);
    write_be32(header + 29, (uint32_t)data->size);

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
    uint32_t expected_crc = read_be32(header + 25);
    uint32_t data_len = read_be32(header + 29);

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

