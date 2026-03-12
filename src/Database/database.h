//
// Created by victor on 3/11/26.
//

#ifndef WAVEDB_DATABASE_H
#define WAVEDB_DATABASE_H

#include <stdint.h>
#include <stddef.h>
#include "../RefCounter/refcounter.h"
#include "../HBTrie/hbtrie.h"
#include "../Time/wheel.h"
#include "../Time/debouncer.h"
#include "../Workers/pool.h"
#include "../Workers/promise.h"
#include "database_lru.h"
#include "wal.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * database_t - Async key-value storage with HBTrie backend.
 *
 * Uses copy-on-write pattern for concurrent access:
 * - Single writer thread modifies write_trie
 * - Multiple readers access read_trie (snapshot)
 * - Debouncer triggers periodic snapshots
 * - WAL provides durability for writes
 */
typedef struct {
    refcounter_t refcounter;
    PLATFORMLOCKTYPE(write_lock);       // Single-writer lock
    PLATFORMRWLOCKTYPE(read_lock);      // Multiple-reader lock for read_trie
    PLATFORMLOCKTYPE(callback_lock);    // Protects callback_in_progress
    PLATFORMCONDITIONTYPE(callback_done); // Signal when callback completes
    int callback_in_progress;           // Flag: snapshot callback is running
    int destroy_requested;               // Flag: destroy in progress
    hbtrie_t* write_trie;               // Mutable trie (single writer)
    hbtrie_t* read_trie;                // Read-only snapshot (multiple readers)
    database_lru_cache_t* lru;          // In-memory LRU cache
    wal_t* wal;                         // Write-ahead log
    debouncer_t* debouncer;             // Timed save debouncer
    work_pool_t* pool;                  // Thread pool for async ops
    hierarchical_timing_wheel_t* wheel; // Timing wheel for debouncer
    char* location;                      // Storage directory
    size_t lru_size;                     // LRU cache max size
    size_t wal_max_size;                 // WAL max size before rotation
    uint8_t chunk_size;                  // HBTrie chunk size
    uint32_t btree_node_size;            // B+tree node size
    uint8_t is_rebuilding;               // Flag for recovery mode
} database_t;

/**
 * Default sizes
 */
#define DATABASE_DEFAULT_LRU_SIZE 1000
#define DATABASE_DEFAULT_WAL_MAX_SIZE (128 * 1024)  // 128KB
#define DATABASE_DEBOUNCE_WAIT_MS 100                // Wait 100ms before save
#define DATABASE_DEBOUNCE_MAX_WAIT_MS 1000           // Force save after 1 second

/**
 * Create a database.
 *
 * Creates or loads a database from the specified location.
 * If existing data is found, replays WAL to recover state.
 *
 * @param location       Directory path for database files
 * @param lru_size       Max LRU cache entries (0 for default)
 * @param wal_max_size   Max WAL file size before rotation (0 for default)
 * @param chunk_size     HBTrie chunk size (0 for default)
 * @param btree_node_size B+tree node size (0 for default)
 * @param pool           Work pool for async operations
 * @param wheel          Timing wheel for debouncer
 * @param error_code     Output error code (0 on success)
 * @return New database or NULL on failure
 */
database_t* database_create(const char* location, size_t lru_size, size_t wal_max_size,
                            uint8_t chunk_size, uint32_t btree_node_size,
                            work_pool_t* pool, hierarchical_timing_wheel_t* wheel,
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
 * @param priority Priority for async execution
 * @param path     Path key (takes ownership of reference)
 * @param value    Value to store (takes ownership of reference)
 * @param promise  Promise to resolve on completion
 */
void database_put(database_t* db, priority_t priority, path_t* path,
                   identifier_t* value, promise_t* promise);

/**
 * Asynchronously get a value.
 *
 * Checks LRU cache first, then read_trie.
 *
 * @param db       Database to query
 * @param priority Priority for async execution
 * @param path     Path key to find
 * @param promise  Promise to resolve with value (or NULL if not found)
 */
void database_get(database_t* db, priority_t priority, path_t* path, promise_t* promise);

/**
 * Asynchronously delete a value.
 *
 * @param db       Database to modify
 * @param priority Priority for async execution
 * @param path     Path key to delete
 * @param promise  Promise to resolve on completion
 */
void database_delete(database_t* db, priority_t priority, path_t* path, promise_t* promise);

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

#ifdef __cplusplus
}
#endif

#endif //WAVEDB_DATABASE_H