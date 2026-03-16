//
// Created by victor on 3/11/26.
//

#ifndef WAVEDB_WAL_H
#define WAVEDB_WAL_H

#include <stdint.h>
#include <stddef.h>
#include "../RefCounter/refcounter.h"
#include "../Buffer/buffer.h"
#include "../Util/threadding.h"
#include "../Workers/transaction_id.h"
#include "../Time/debouncer.h"  // For debouncer_t and timing wheel

#ifdef __cplusplus
extern "C" {
#endif

/**
 * WAL entry types
 */
typedef enum {
    WAL_PUT = 'p',      // Insert/update operation
    WAL_DELETE = 'd'    // Delete operation
} wal_type_e;

/**
 * WAL entry header structure (written before CBOR data)
 */
typedef struct {
    wal_type_e type;        // Entry type
    uint32_t crc32;         // CRC32 of the CBOR data (big-endian)
    uint32_t data_len;      // Length of CBOR data (big-endian)
} wal_entry_header_t;

/**
 * WAL sync modes
 */
typedef enum {
    WAL_SYNC_IMMEDIATE = 0,  // fsync after every write (default, safest)
    WAL_SYNC_DEBOUNCED = 1,  // debounced fsync (configurable wait, good balance)
    WAL_SYNC_ASYNC = 2       // no fsync (fastest, less durable)
} wal_sync_mode_e;

/**
 * WAL state
 */
typedef struct {
    refcounter_t refcounter;
    PLATFORMLOCKTYPE(lock);
    int fd;                 // File descriptor for current WAL file
    char* location;         // Directory path for WAL files
    char* current_file;      // Path to current WAL file
    uint64_t sequence;       // Current sequence number
    size_t current_size;     // Current file size
    size_t max_size;         // Max size before rotation
    transaction_id_t oldest_txn_id;  // Oldest transaction since last compaction
    transaction_id_t newest_txn_id;   // Newest transaction written to disk

    // Fsync batching support
    wal_sync_mode_e sync_mode;     // Sync mode (immediate/debounced/async)
    debouncer_t* fsync_debouncer;   // Debouncer for batched fsync
    uint64_t pending_writes;         // Count of writes since last fsync
    hierarchical_timing_wheel_t* wheel; // Timing wheel for debouncer
} wal_t;

/**
 * Default maximum WAL file size (128KB)
 */
#define WAL_DEFAULT_MAX_SIZE (128 * 1024)

/**
 * Create a new WAL at sequence 0.
 *
 * Creates the WAL directory if needed and opens a new WAL file.
 *
 * @param location  Directory path for WAL files
 * @param max_size  Maximum file size before rotation (0 for default)
 * @param error_code Output parameter for error code (0 on success)
 * @return New WAL or NULL on failure
 */
wal_t* wal_create(char* location, size_t max_size, int* error_code);

/**
 * Create a new WAL with configurable sync mode.
 *
 * Creates the WAL directory if needed and opens a new WAL file.
 * Allows configuration of fsync behavior for performance optimization.
 *
 * @param location   Directory path for WAL files
 * @param max_size    Maximum file size before rotation (0 for default)
 * @param sync_mode   Sync mode (IMMEDIATE/DEBOUNCED/ASYNC)
 * @param wheel       Timing wheel for debounced fsync (required if DEBOUNCED)
 * @param debounce_ms Debounce wait time in milliseconds (0 for default 100ms)
 * @param error_code  Output parameter for error code (0 on success)
 * @return New WAL or NULL on failure
 */
wal_t* wal_create_with_sync(char* location, size_t max_size, wal_sync_mode_e sync_mode,
                             hierarchical_timing_wheel_t* wheel, uint64_t debounce_ms, int* error_code);

/**
 * Load existing WAL from directory.
 *
 * Finds the most recent WAL file and opens it for reading.
 * Used for recovery to replay entries.
 *
 * @param location   Directory path for WAL files
 * @param max_size   Maximum file size before rotation (0 for default)
 * @param error_code Output parameter for error code (0 on success)
 * @return WAL or NULL on failure
 */
wal_t* wal_load(char* location, size_t max_size, int* error_code);

/**
 * Destroy a WAL.
 *
 * Flushes any pending fsync operations and frees resources.
 *
 * @param wal  WAL to destroy
 */
void wal_destroy(wal_t* wal);

/**
 * Flush pending fsync operations.
 *
 * For DEBOUNCED mode, immediately flushes all pending writes.
 * For IMMEDIATE and ASYNC modes, this is a no-op.
 *
 * @param wal  WAL to flush
 * @return 0 on success, -1 on error
 */
int wal_flush(wal_t* wal);

/**
 * Write an entry to the WAL.
 *
 * Writes type + txn_id + CRC32 + CBOR data. May trigger rotation if max_size exceeded.
 *
 * @param wal   WAL to write to
 * @param txn_id Transaction ID for this entry
 * @param type  Entry type
 * @param data  CBOR-encoded data buffer
 * @return 0 on success, -1 on error, 1 if WAL was rotated
 */
int wal_write(wal_t* wal, transaction_id_t txn_id, wal_type_e type, buffer_t* data);

/**
 * Read the next entry from the WAL.
 *
 * Used during recovery to replay entries.
 *
 * @param wal    WAL to read from
 * @param txn_id Output parameter for transaction ID
 * @param type   Output parameter for entry type
 * @param data   Output parameter for CBOR data (caller must destroy)
 * @param cursor Position to read from (updated after read)
 * @return 0 on success, -1 on error, 1 on EOF
 */
int wal_read(wal_t* wal, transaction_id_t* txn_id, wal_type_e* type, buffer_t** data, uint64_t* cursor);

/**
 * Swap current WAL for a new file.
 *
 * Closes current file, renames to sequence.wal, opens new current.wal.
 *
 * @param wal  WAL to swap
 * @return 0 on success, -1 on failure
 */
int wal_swap(wal_t* wal);

/**
 * Get the list of WAL sequence files.
 *
 * @param location  WAL directory path
 * @param sequences Output array for sequence numbers (caller must free)
 * @param count     Output count of sequences
 * @return 0 on success, -1 on failure
 */
int wal_list_sequences(const char* location, uint64_t** sequences, size_t* count);

/**
 * Get the path for a sequence WAL file.
 *
 * @param location  WAL directory path
 * @param sequence  Sequence number
 * @return Allocated path string (caller must free)
 */
char* wal_sequence_path(const char* location, uint64_t sequence);

#ifdef __cplusplus
}
#endif

#endif //WAVEDB_WAL_H