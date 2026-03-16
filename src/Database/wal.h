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
 * @param wal  WAL to destroy
 */
void wal_destroy(wal_t* wal);

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