//
// MVCC Transaction Manager for HBTrie
//
// Created by victor on 3/17/26.
//

#ifndef WAVEDB_MVCC_H
#define WAVEDB_MVCC_H

#include <stdint.h>
#include <stddef.h>
#include "../RefCounter/refcounter.h"
#include "../Util/atomic_compat.h"
#include "../Util/vec.h"
#include "../Util/threadding.h"
#include "../Workers/transaction_id.h"
#include "../Workers/pool.h"
#include "../Time/wheel.h"
#include "hbtrie.h"

#ifdef __cplusplus
extern "C" {
#endif

// Forward declarations
struct hbtrie_t;
typedef struct hbtrie_t hbtrie_t;

/* Seqlock for 24-byte transaction_id_t.
 *
 * _Atomic(transaction_id_t) is 24 bytes, which exceeds the platform's
 * hardware CAS width (8 or 16 bytes on x86-64), so C11 atomics fall
 * back to libatomic lock-based operations — every "lock-free" read
 * takes a lock. The seqlock gives genuinely lock-free reads: readers
 * read the sequence counter, memcpy the value, re-read the sequence,
 * and retry only if it changed (or was odd, meaning a write is in
 * progress). Writers serialize via a spinlock and bump the sequence
 * odd → even around the write. */
typedef struct txn_id_seqlock_t {
    atomic_uint_fast64_t seq;    /* even = stable, odd = write in progress */
    transaction_id_t value;      /* the 24-byte protected value */
    volatile int write_lock;      /* spinlock serializing writers */
} txn_id_seqlock_t;

void txn_id_seqlock_init(txn_id_seqlock_t* sl, transaction_id_t val);
void txn_id_seqlock_read(const txn_id_seqlock_t* sl, transaction_id_t* out);
void txn_id_seqlock_write(txn_id_seqlock_t* sl, transaction_id_t val);
int txn_id_seqlock_cas(txn_id_seqlock_t* sl, const transaction_id_t* expected, transaction_id_t desired);

/**
 * Transaction state.
 */
typedef enum {
    TXN_ACTIVE,      // Transaction is active
    TXN_COMMITTED,   // Transaction committed successfully
    TXN_ABORTED      // Transaction was aborted
} txn_state_e;

/**
 * Transaction descriptor.
 *
 * Tracks the state and metadata of a single transaction.
 */
typedef struct txn_desc_t {
    refcounter_t refcounter;           // MUST be first member
    transaction_id_t txn_id;             // Unique transaction ID
    txn_state_e state;                   // Current state
    uint32_t shard_index;                // Which shard this txn belongs to
    PLATFORMLOCKTYPE(lock);              // Lock for state changes
} txn_desc_t;

/**
 * Shard of the transaction manager.
 *
 * Each shard has its own lock and active transaction list,
 * reducing contention under high concurrency.
 */
typedef struct tx_shard_t {
    PLATFORMLOCKTYPE(lock);              // Per-shard lock
    vec_t(txn_desc_t*) txns;            // Active transactions in this shard
    txn_id_seqlock_t min_txn_id; // Minimum txn ID in this shard (SENTINEL if empty)
    ATOMIC_TYPE(uint32_t) count;              // Number of active transactions (for fast empty check)
} tx_shard_t;

/**
 * Transaction manager.
 *
 * Coordinates MVCC transactions, tracks active transactions,
 * and manages garbage collection. Uses sharded tracking for
 * reduced contention under high concurrency.
 */
typedef struct tx_manager_t {
    refcounter_t refcounter;           // MUST be first member

    tx_shard_t shards[TX_MANAGER_SHARDS]; // Sharded active transaction tracking
    txn_id_seqlock_t min_active_txn_id;  // Oldest active transaction (GC cutoff) - seqlock for lock-free reads
    txn_id_seqlock_t last_committed_txn_id;  // Last committed transaction - seqlock for lock-free reads

    hbtrie_t* trie;                      // Reference to trie
    work_pool_t* pool;                   // Work pool for async GC
    hierarchical_timing_wheel_t* wheel;  // Timing wheel for GC scheduling

    uint64_t gc_interval_ms;             // GC interval in milliseconds
    uint64_t last_gc_time;               // Last GC execution time
} tx_manager_t;

/**
 * Create a transaction manager.
 *
 * @param trie     HBTrie to manage
 * @param pool     Work pool for async operations
 * @param wheel    Timing wheel for scheduling
 * @param gc_interval_ms  GC interval in milliseconds (0 for default: 100ms)
 * @return New transaction manager or NULL on failure
 */
tx_manager_t* tx_manager_create(hbtrie_t* trie,
                                 work_pool_t* pool,
                                 hierarchical_timing_wheel_t* wheel,
                                 uint64_t gc_interval_ms);

/**
 * Destroy a transaction manager.
 *
 * @param manager  Transaction manager to destroy
 */
void tx_manager_destroy(tx_manager_t* manager);

/**
 * Begin a new transaction.
 *
 * @param manager  Transaction manager
 * @return New transaction descriptor or NULL on failure
 */
txn_desc_t* tx_manager_begin(tx_manager_t* manager);

/**
 * Commit a transaction.
 *
 * @param manager  Transaction manager
 * @param txn      Transaction to commit
 * @return 0 on success, -1 on failure
 */
int tx_manager_commit(tx_manager_t* manager, txn_desc_t* txn);

/**
 * Abort a transaction.
 *
 * @param manager  Transaction manager
 * @param txn      Transaction to abort
 * @return 0 on success, -1 on failure
 */
int tx_manager_abort(tx_manager_t* manager, txn_desc_t* txn);

/**
 * Get the minimum active transaction ID.
 *
 * This is used as the GC cutoff - versions older than this can be removed.
 *
 * @param manager  Transaction manager
 * @return Minimum active transaction ID (or last_committed if no active txns)
 */
transaction_id_t tx_manager_get_min_active(tx_manager_t* manager);

/**
 * Get the last committed transaction ID.
 *
 * This is used as the read point for new transactions.
 *
 * @param manager  Transaction manager
 * @return Last committed transaction ID
 */
transaction_id_t tx_manager_get_last_committed(tx_manager_t* manager);

/**
 * Garbage collect old versions.
 *
 * Removes version chain entries older than min_active_txn_id.
 * This is typically called asynchronously via the work pool.
 *
 * @param manager  Transaction manager
 * @return Number of versions removed
 */
size_t tx_manager_gc(tx_manager_t* manager);

/**
 * Destroy a transaction descriptor.
 *
 * @param txn  Transaction descriptor to destroy
 */
void txn_desc_destroy(txn_desc_t* txn);

#ifdef __cplusplus
}
#endif

#endif // WAVEDB_MVCC_H