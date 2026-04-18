//
// Created by victor on 3/11/26.
//

#ifndef WAVEDB_DATABASE_H
#define WAVEDB_DATABASE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "../RefCounter/refcounter.h"
#include "../Util/threadding.h"
#include "../HBTrie/hbtrie.h"
#include "../HBTrie/mvcc.h"
#include "../Time/wheel.h"
#include "../Workers/pool.h"
#include "../Workers/promise.h"
#include "database_lru.h"
#include "wal.h"
#include "wal_manager.h"
#include "batch.h"
#include "database_config.h"
#include "../Storage/page_file.h"
#include "../Storage/bnode_cache.h"
#include "eviction_queue.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * database_t - Async key-value storage with MVCC.
 *
 * Uses MVCC (Multi-Version Concurrency Control) for concurrent access:
 * - Single trie with version chains
 * - Transaction manager tracks active transactions
 * - Lock-free reads (no read lock needed)
 * - Single writer (write lock serializes writers)
 * - WAL provides durability for writes
 *
 * Storage modes:
 * - In-memory only: storage = NULL, no persistence
 */
// Number of shards for write locks (reduces contention)
#define WRITE_LOCK_SHARDS 64

typedef struct {
    refcounter_t refcounter;
    spinlock_t write_locks[WRITE_LOCK_SHARDS];  // Sharded write locks (adaptive spinlocks)
    hbtrie_t* trie;                      // Single trie with MVCC (renamed from write_trie)
    tx_manager_t* tx_manager;           // Transaction manager for MVCC
    database_lru_cache_t* lru;          // In-memory LRU cache
    wal_t* wal;                         // Legacy WAL (for migration)
    wal_manager_t* wal_manager;         // Thread-local WAL manager
    work_pool_t* pool;                  // Thread pool for async ops
    hierarchical_timing_wheel_t* wheel; // Timing wheel
    char* location;                      // Storage directory
    size_t lru_memory_mb;                // LRU cache max size in megabytes
    uint8_t chunk_size;                  // HBTrie chunk size
    uint32_t btree_node_size;            // B+tree node size
    uint8_t is_rebuilding;               // Flag for recovery mode

    // Page-based persistence (Phase 2: replaces CBOR index files)
    page_file_t* page_file;
    file_bnode_cache_t* bnode_cache;
    eviction_queue_t eviction_queue;     // Lock-free ring buffer for eviction offsets
    uint64_t next_index_id;              // Incrementing ID for index files

    // Pending MVCC transaction ID from page file superblock
    // Applied after tx_manager is created (page file loads before tx_manager)
    transaction_id_t pending_txn_id;
    uint8_t has_pending_txn_id;

    // Config ownership tracking
    bool owns_pool;                     // True if database created the pool
    bool owns_wheel;                   // True if database created the wheel

    // Active configuration
    database_config_t* active_config;   // Current config (for runtime queries)
} database_t;

/**
 * Default sizes
 */
#define DATABASE_DEFAULT_LRU_MEMORY_MB 50       // 50 MB default
#define DATABASE_DEFAULT_WAL_MAX_SIZE (128 * 1024)  // 128KB
#define DATABASE_DEBOUNCE_WAIT_MS 100                // Wait 100ms before save
#define DATABASE_DEBOUNCE_MAX_WAIT_MS 1000           // Force save after 1 second

/**
 * Create a database.
 *
 * Creates or loads a database from the specified location.
 * If existing data is found, replays WAL to recover state.
 *
 * @param location          Directory path for database files
 * @param lru_memory_mb     LRU cache memory budget in MB (0 for default 50 MB)
 * @param wal_config        WAL configuration (NULL for defaults)
 * @param chunk_size        HBTrie chunk size (0 for default)
 * @param btree_node_size   B+tree node size (0 for default)
 * @param enable_persist   Enable persistent storage (0 = in-memory only, 1 = persistent)
 * @param pool              Work pool for async operations
 * @param wheel             Timing wheel for debouncer
 * @param error_code        Output error code (0 on success)
 * @return New database or NULL on failure
 */
database_t* database_create(const char* location, size_t lru_memory_mb,
                            wal_config_t* wal_config,
                            uint8_t chunk_size, uint32_t btree_node_size,
                            uint8_t enable_persist,
                            work_pool_t* pool, hierarchical_timing_wheel_t* wheel,
                            int* error_code);

/**
 * Create a database with configuration.
 *
 * @param location    Directory path for database files
 * @param config      Configuration (NULL for defaults)
 * @param error_code  Output error code (0 on success)
 * @return Database or NULL on failure
 *
 * Behavior:
 * - If database doesn't exist: use config (or defaults)
 * - If database exists: load saved config, merge with passed config
 * - Immutable settings from passed config are ignored for existing DB
 * - If config->worker_threads > 0 and external_pool/wheel are NULL,
 *   creates its own pool and wheel
 */
database_t* database_create_with_config(const char* location,
                                        database_config_t* config,
                                        int* error_code);

/**
 * Destroy a database.
 *
 * Flushes pending writes, closes files, and frees resources.
 *
 * @param db  Database to destroy
 */
void database_destroy(database_t* db);

/**
 * Asynchronously insert a value.
 *
 * @param db       Database to modify
 * @param path     Path key (takes ownership of reference)
 * @param value    Value to store (takes ownership of reference)
 * @param promise  Promise to resolve on completion
 */
void database_put(database_t* db, path_t* path,
                   identifier_t* value, promise_t* promise);

/**
 * Asynchronously get a value.
 *
 * Checks LRU cache first, then read_trie.
 *
 * @param db       Database to query
 * @param path     Path key to find
 * @param promise  Promise to resolve with value (or NULL if not found)
 */
void database_get(database_t* db, path_t* path, promise_t* promise);

/**
 * Asynchronously delete a value.
 *
 * @param db       Database to modify
 * @param path     Path key to delete
 * @param promise  Promise to resolve on completion
 */
void database_delete(database_t* db, path_t* path, promise_t* promise);

/**
 * Force an immediate snapshot.
 *
 * Writes write_trie to disk and updates read_trie.
 * Waits for completion.
 *
 * @param db  Database to snapshot
 * @return 0 on success, -1 on failure
 */
int database_snapshot(database_t* db);

/**
 * Get current entry count (approximate).
 *
 * @param db  Database to query
 * @return Approximate entry count
 */
size_t database_count(database_t* db);

/**
 * Synchronously insert a value.
 *
 * @param db       Database to modify
 * @param path     Path key (takes ownership of reference)
 * @param value    Value to store (takes ownership of reference)
 * @return 0 on success, -1 on error
 */
int database_put_sync(database_t* db, path_t* path, identifier_t* value);

/**
 * Synchronously get a value.
 *
 * Checks LRU cache first, then trie.
 *
 * @param db       Database to query
 * @param path     Path key to find (takes ownership, always consumed)
 * @param result   Output: found value (caller must destroy) or NULL if not found
 *                 Caller should NOT initialize *result before calling
 * @return 0 on success (result found), -1 on error, -2 on not found
 */
int database_get_sync(database_t* db, path_t* path, identifier_t** result);

/**
 * Synchronously delete a value.
 *
 * @param db       Database to modify
 * @param path     Path key to delete (takes ownership)
 * @return 0 on success, -1 on error
 */
int database_delete_sync(database_t* db, path_t* path);

/**
 * Atomically increment a numeric value at the given path.
 *
 * Reads the current value, increments by delta, and writes the result back.
 * Uses sharded write locks for atomicity under concurrent access.
 * If the path doesn't exist, starts from 0.
 *
 * @param db     Database to modify
 * @param path   Path key (takes ownership)
 * @param delta  Amount to increment by
 * @return New value after increment, or -1 on error
 */
int64_t database_increment_sync(database_t* db, path_t* path, int64_t delta);

// Forward declaration (see database_iterator.h for full definition and scan functions)
typedef struct database_iterator_t database_iterator_t;

/**
 * Start a database scan using string-based path bounds.
 *
 * Convenience wrapper that converts string bounds to path_t
 * and calls database_scan_start.
 *
 * @param db     Database to scan
 * @param start  Start path string (NULL = beginning)
 * @param end    End path string (NULL = no upper bound)
 * @return Iterator handle, or NULL on failure
 */
database_iterator_t* database_scan_range(database_t* db,
                                          const char* start,
                                          const char* end);

/**
 * Submit batch synchronously.
 *
 * @param db Database to modify
 * @param batch Batch to submit
 * @return 0 on success, error code on failure
 */
int database_write_batch_sync(database_t* db, batch_t* batch);

/**
 * Submit batch asynchronously.
 *
 * @param db Database to modify
 * @param batch Batch to submit
 * @param promise Promise to resolve with result
 */
void database_write_batch(database_t* db, batch_t* batch, promise_t* promise);

/**
 * Flush all dirty bnodes to the page file using CoW.
 *
 * Collects all dirty bnodes bottom-up, serializes each with V3 format,
 * writes to page file at new offset, marks old offset stale, and
 * propagates new offsets up to parent entries.
 *
 * @param db  Database to flush
 * @return 0 on success, -1 on failure
 */
int database_flush_dirty_bnodes(database_t* db);

#ifdef __cplusplus
}
#endif

#endif //WAVEDB_DATABASE_H