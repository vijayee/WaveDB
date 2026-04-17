#ifndef WAVEDB_WAL_MANAGER_H
#define WAVEDB_WAL_MANAGER_H

#include <stdint.h>
#include <stddef.h>
#include "../RefCounter/refcounter.h"
#include "../Buffer/buffer.h"
#include "../Util/threadding.h"
#include "../Workers/transaction_id.h"
#include "../Time/wheel.h"
#include "wal.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Manifest entry status
 */
typedef enum {
    WAL_FILE_ACTIVE = 0x01,      // File is being written to
    WAL_FILE_SEALED = 0x02,      // File complete, ready for compaction
    WAL_FILE_COMPACTED = 0x03    // File merged, can be deleted
} wal_file_status_e;

/**
 * Migration state
 */
typedef enum {
    MIGRATION_NONE = 0,          // Fresh database
    MIGRATION_PENDING = 1,       // Legacy detected, needs migration
    MIGRATION_IN_PROGRESS = 2,   // Migration started but not completed
    MIGRATION_COMPLETE = 3,      // Successfully migrated
    MIGRATION_FAILED = 4         // Migration failed, can rollback
} migration_state_e;

/**
 * Manifest header
 */
typedef struct {
    uint32_t version;                    // Manifest format version
    uint32_t migration_state;            // Migration status
    uint64_t migration_timestamp;        // When migration happened
    char backup_file[256];               // Path to backed-up legacy WAL
} manifest_header_t;

#define MANIFEST_VERSION 1

/**
 * Manifest entry (fixed size for atomic appends)
 */
typedef struct {
    uint64_t thread_id;                  // Thread identifier
    char file_path[256];                 // Path to thread-local file
    wal_file_status_e status;            // File status
    transaction_id_t oldest_txn_id;      // First transaction in file
    transaction_id_t newest_txn_id;      // Last transaction in file
    uint32_t checksum;                   // CRC32 of entry
} manifest_entry_t;

/**
 * Thread-local WAL configuration
 */
typedef struct {
    wal_sync_mode_e sync_mode;           // IMMEDIATE, DEBOUNCED, ASYNC
    uint64_t debounce_ms;                // Debounce window (default 100ms)
    uint64_t idle_threshold_ms;          // Compaction idle trigger (default 10s)
    uint64_t compact_interval_ms;        // Compaction interval (default 60s)
    size_t max_file_size;                // Max file size before seal (default 128KB)
    size_t max_sealed_wals;              // Max sealed WALs before writes block (default 10, 0 = unlimited)
} wal_config_t;

/**
 * Thread-local WAL state
 */
typedef struct wal_manager wal_manager_t;  // Forward declaration

typedef struct {
    refcounter_t refcounter;
    PLATFORMLOCKTYPE(lock);              // Lock for this thread's WAL
    uint64_t thread_id;                  // Thread identifier
    char* file_path;                     // Path to thread-local file
    int fd;                              // File descriptor
    wal_sync_mode_e sync_mode;          // Durability mode
    hierarchical_timing_wheel_t* wheel; // Timing wheel for one-shot timer
    transaction_id_t oldest_txn_id;      // First transaction in file
    transaction_id_t newest_txn_id;      // Last transaction in file
    size_t current_size;                 // Current file size
    size_t max_size;                     // Max before seal
    uint64_t pending_writes;             // Count of writes since last fsync

    // Batch write buffer (replaces debouncer)
    uint8_t entry_buf[4096];            // Pre-allocated entry buffer
    size_t entry_buf_used;              // Bytes used in entry_buf
    uint8_t batch_count;                // Entries accumulated in current batch
    uint8_t batch_size;                 // Max entries before flush (1=IMMEDIATE, 4=DEBOUNCED)
    int timer_active;                   // 1 if one-shot timer is pending
    uint64_t timer_id;                  // ID of the pending one-shot timer
    uint64_t debounce_ms;               // Timer delay in milliseconds

    wal_manager_t* manager;              // Back-reference to manager
} thread_wal_t;

/**
 * WAL manager (global state)
 */
struct wal_manager {
    refcounter_t refcounter;
    PLATFORMLOCKTYPE(manifest_lock);     // Lock for manifest operations
    char* location;                      // WAL directory
    char* manifest_path;                 // Path to manifest file
    int manifest_fd;                     // Manifest file descriptor
    wal_config_t config;                 // Configuration
    thread_wal_t** threads;              // Array of thread-local WALs
    size_t thread_count;                 // Number of threads
    size_t thread_capacity;              // Capacity of threads array
    PLATFORMLOCKTYPE(threads_lock);      // Lock for threads array
    hierarchical_timing_wheel_t* wheel; // Timing wheel for one-shot timers
    size_t sealed_count;                 // Number of sealed WAL files not yet compacted
};

/**
 * Recovery options
 */
typedef struct {
    int force_legacy;                    // Force use of legacy WAL
    int force_migration;                 // Force re-migration
    int rollback_on_failure;             // Auto-rollback if failed
    int keep_backup;                      // Keep backup after migration
} wal_recovery_options_t;

// Default configuration
#define WAL_DEFAULT_DEBOUNCE_MS 100
#define WAL_DEFAULT_IDLE_THRESHOLD_MS 10000
#define WAL_DEFAULT_COMPACT_INTERVAL_MS 60000
#define WAL_DEFAULT_MAX_FILE_SIZE (128 * 1024)
#define WAL_DEFAULT_MAX_SEALED_WALS 10

/**
 * Create WAL manager
 */
wal_manager_t* wal_manager_create(const char* location, wal_config_t* config, hierarchical_timing_wheel_t* wheel, int* error_code);

/**
 * Load or create WAL manager with recovery options
 */
wal_manager_t* wal_manager_load_with_options(const char* location, wal_config_t* config,
                                              wal_recovery_options_t* options, int* error_code);

/**
 * Destroy WAL manager
 */
void wal_manager_destroy(wal_manager_t* manager);

/**
 * Get or create thread-local WAL
 */
thread_wal_t* get_thread_wal(wal_manager_t* manager);

/**
 * Create thread-local WAL for a specific thread ID (for testing)
 */
thread_wal_t* create_thread_wal(wal_manager_t* manager, uint64_t thread_id);

/**
 * Write to thread-local WAL
 */
int thread_wal_write(thread_wal_t* twal, transaction_id_t txn_id,
                     wal_type_e type, buffer_t* data);

/**
 * Seal thread-local WAL
 */
int thread_wal_seal(thread_wal_t* twal);

/**
 * Recover from all WAL files
 */
int wal_manager_recover(wal_manager_t* manager, void* db);

/**
 * Compact sealed WAL files
 */
int compact_wal_files(wal_manager_t* manager);

/**
 * Seal all active thread-local WALs and compact all sealed WALs.
 * After this, all WAL entries are in COMPACTED files and will be
 * skipped by wal_manager_recover() on next database creation.
 */
int wal_manager_seal_and_compact(wal_manager_t* manager);

/**
 * Flush pending operations
 */
int wal_manager_flush(wal_manager_t* manager);

/**
 * Read manifest entries from disk.
 *
 * @param manager The WAL manager
 * @param entries Output array of manifest entries (caller must free)
 * @param count Output count of entries
 * @return 0 on success, negative error code on failure
 *
 * On success, *entries is allocated and must be freed by caller.
 * On failure, *entries is set to NULL and *count to 0.
 *
 * Error codes:
 *   WAL_ERROR_INVALID_ARG (-1) - Invalid arguments
 *   WAL_ERROR_IO (-2) - I/O error
 *   WAL_ERROR_MEMORY (-3) - Memory allocation failed
 *   WAL_ERROR_CORRUPT (-4) - Corrupted manifest data
 *   WAL_ERROR_LIMIT (-5) - Maximum entries limit exceeded
 */
int read_manifest(wal_manager_t* manager, manifest_entry_t** entries, size_t* count);

/**
 * Clear thread-local WAL reference (for testing).
 *
 * This clears the thread-local WAL pointer without freeing it.
 * Used for test cleanup to avoid memory leak reports.
 *
 * WARNING: Only use in tests where you know the WAL has been
 * properly destroyed by its manager.
 */
void clear_thread_wal_reference(void);

#ifdef __cplusplus
}
#endif

#endif // WAVEDB_WAL_MANAGER_H