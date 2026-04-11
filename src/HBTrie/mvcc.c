//
// MVCC Transaction Manager Implementation
//
// Created by victor on 3/17/26.
//

#include "mvcc.h"
#include "bnode.h"
#include "../Util/allocator.h"
#include "../Util/log.h"
#include <string.h>
#include <stdatomic.h>

#define DEFAULT_GC_INTERVAL_MS 100

// Forward declarations
static void tx_manager_gc_callback(void* arg);
static void recompute_global_min(tx_manager_t* manager);

tx_manager_t* tx_manager_create(hbtrie_t* trie,
                                 work_pool_t* pool,
                                 hierarchical_timing_wheel_t* wheel,
                                 uint64_t gc_interval_ms) {
    if (trie == NULL) return NULL;

    tx_manager_t* manager = get_clear_memory(sizeof(tx_manager_t));
    if (manager == NULL) return NULL;

    manager->trie = trie;
    manager->pool = pool;
    manager->wheel = wheel;
    manager->gc_interval_ms = (gc_interval_ms == 0) ? DEFAULT_GC_INTERVAL_MS : gc_interval_ms;
    manager->last_gc_time = 0;

    // Initialize shards
    for (int i = 0; i < TX_MANAGER_SHARDS; i++) {
        vec_init(&manager->shards[i].txns);
        atomic_init(&manager->shards[i].min_txn_id, TXN_ID_SENTINEL);
        atomic_init(&manager->shards[i].count, 0);
        platform_lock_init(&manager->shards[i].lock);
    }

    transaction_id_t last_committed_init = {0, 0, 0};
    atomic_init(&manager->min_active_txn_id, TXN_ID_SENTINEL);
    atomic_init(&manager->last_committed_txn_id, last_committed_init);

    refcounter_init((refcounter_t*)manager);

    return manager;
}

void tx_manager_destroy(tx_manager_t* manager) {
    if (manager == NULL) return;

    refcounter_dereference((refcounter_t*)manager);
    if (refcounter_count((refcounter_t*)manager) == 0) {
        // Acquire all shard locks in order
        for (int i = 0; i < TX_MANAGER_SHARDS; i++) {
            platform_lock(&manager->shards[i].lock);
        }

        // Destroy all active transactions and deinit shard vecs
        for (int i = 0; i < TX_MANAGER_SHARDS; i++) {
            for (int j = 0; j < manager->shards[i].txns.length; j++) {
                txn_desc_t* txn = manager->shards[i].txns.data[j];
                if (txn != NULL) {
                    txn_desc_destroy(txn);
                }
            }
            vec_deinit(&manager->shards[i].txns);
        }

        // Release and destroy all shard locks
        for (int i = 0; i < TX_MANAGER_SHARDS; i++) {
            platform_unlock(&manager->shards[i].lock);
            platform_lock_destroy(&manager->shards[i].lock);
        }

        free(manager);
    }
}

txn_desc_t* tx_manager_begin(tx_manager_t* manager) {
    if (manager == NULL) return NULL;

    txn_desc_t* txn = get_clear_memory(sizeof(txn_desc_t));
    if (txn == NULL) return NULL;

    // Get new transaction ID (thread-safe, doesn't need any lock)
    txn->txn_id = transaction_id_get_next();
    txn->state = TXN_ACTIVE;
    txn->shard_index = txn_shard_index(&txn->txn_id);

    // Initialize lock and refcounter BEFORE making visible to other threads
    platform_lock_init(&txn->lock);
    refcounter_init((refcounter_t*)txn);

    // Lock only the shard this txn belongs to
    tx_shard_t* shard = &manager->shards[txn->shard_index];

    platform_lock(&shard->lock);

    // Add to shard's active list
    vec_push(&shard->txns, txn);
    atomic_fetch_add(&shard->count, 1);

    // Update shard min if this shard was empty
    // Note: new IDs are always >= current shard min because IDs are monotonically
    // increasing, so we only need to handle the empty-shard (SENTINEL) case.
    transaction_id_t current_shard_min = atomic_load(&shard->min_txn_id);
    if (is_txn_id_sentinel(&current_shard_min)) {
        atomic_store(&shard->min_txn_id, txn->txn_id);
    }

    // Global min_active_txn_id does NOT need updating on begin.
    // The new ID is >= any existing min, so the GC safety invariant holds.

    platform_unlock(&shard->lock);

    return txn;
}

int tx_manager_commit(tx_manager_t* manager, txn_desc_t* txn) {
    if (manager == NULL || txn == NULL) return -1;

    tx_shard_t* shard = &manager->shards[txn->shard_index];

    platform_lock(&shard->lock);
    platform_lock(&txn->lock);

    if (txn->state != TXN_ACTIVE) {
        platform_unlock(&txn->lock);
        platform_unlock(&shard->lock);
        return -1;  // Already committed or aborted
    }

    txn->state = TXN_COMMITTED;

    // Update last_committed (lock-free CAS loop)
    transaction_id_t current_last = atomic_load(&manager->last_committed_txn_id);
    while (transaction_id_compare(&txn->txn_id, &current_last) > 0) {
        if (atomic_compare_exchange_weak(&manager->last_committed_txn_id,
                                           &current_last, txn->txn_id)) {
            break;
        }
    }

    // Remove from shard's active list (swap-and-pop for O(1) removal)
    int found = -1;
    for (int i = 0; i < shard->txns.length; i++) {
        if (shard->txns.data[i] == txn) {
            found = i;
            break;
        }
    }
    if (found >= 0) {
        shard->txns.data[found] = shard->txns.data[shard->txns.length - 1];
        shard->txns.length--;
    }

    atomic_fetch_sub(&shard->count, 1);

    // Check if we were the shard minimum
    transaction_id_t current_shard_min = atomic_load(&shard->min_txn_id);
    int was_shard_min = (transaction_id_compare(&txn->txn_id, &current_shard_min) == 0);
    int shard_now_empty = (shard->txns.length == 0);

    if (was_shard_min || shard_now_empty) {
        // Recompute shard minimum
        if (shard->txns.length > 0) {
            transaction_id_t new_shard_min = shard->txns.data[0]->txn_id;
            for (int i = 1; i < shard->txns.length; i++) {
                if (transaction_id_compare(&shard->txns.data[i]->txn_id, &new_shard_min) < 0) {
                    new_shard_min = shard->txns.data[i]->txn_id;
                }
            }
            atomic_store(&shard->min_txn_id, new_shard_min);
        } else {
            atomic_store(&shard->min_txn_id, TXN_ID_SENTINEL);
        }
    }

    platform_unlock(&txn->lock);
    platform_unlock(&shard->lock);

    // Update global min_active_txn_id if necessary
    // Must recompute if: (a) we were the global minimum, or (b) global min is sentinel
    transaction_id_t current_global_min = atomic_load(&manager->min_active_txn_id);
    if (transaction_id_compare(&txn->txn_id, &current_global_min) == 0 ||
        is_txn_id_sentinel(&current_global_min)) {
        recompute_global_min(manager);
    }

    return 0;
}

int tx_manager_abort(tx_manager_t* manager, txn_desc_t* txn) {
    if (manager == NULL || txn == NULL) return -1;

    tx_shard_t* shard = &manager->shards[txn->shard_index];

    platform_lock(&shard->lock);
    platform_lock(&txn->lock);

    if (txn->state != TXN_ACTIVE) {
        platform_unlock(&txn->lock);
        platform_unlock(&shard->lock);
        return -1;  // Already committed or aborted
    }

    txn->state = TXN_ABORTED;

    // Remove from shard's active list (swap-and-pop for O(1) removal)
    int found = -1;
    for (int i = 0; i < shard->txns.length; i++) {
        if (shard->txns.data[i] == txn) {
            found = i;
            break;
        }
    }
    if (found >= 0) {
        shard->txns.data[found] = shard->txns.data[shard->txns.length - 1];
        shard->txns.length--;
    }

    atomic_fetch_sub(&shard->count, 1);

    // Check if we were the shard minimum
    transaction_id_t current_shard_min = atomic_load(&shard->min_txn_id);
    int was_shard_min = (transaction_id_compare(&txn->txn_id, &current_shard_min) == 0);
    int shard_now_empty = (shard->txns.length == 0);

    if (was_shard_min || shard_now_empty) {
        // Recompute shard minimum
        if (shard->txns.length > 0) {
            transaction_id_t new_shard_min = shard->txns.data[0]->txn_id;
            for (int i = 1; i < shard->txns.length; i++) {
                if (transaction_id_compare(&shard->txns.data[i]->txn_id, &new_shard_min) < 0) {
                    new_shard_min = shard->txns.data[i]->txn_id;
                }
            }
            atomic_store(&shard->min_txn_id, new_shard_min);
        } else {
            atomic_store(&shard->min_txn_id, TXN_ID_SENTINEL);
        }
    }

    platform_unlock(&txn->lock);
    platform_unlock(&shard->lock);

    // Update global min_active_txn_id if necessary
    // (Same logic as commit, except we don't update last_committed)
    transaction_id_t current_global_min = atomic_load(&manager->min_active_txn_id);
    if (transaction_id_compare(&txn->txn_id, &current_global_min) == 0 ||
        is_txn_id_sentinel(&current_global_min)) {
        recompute_global_min(manager);
    }

    return 0;
}

transaction_id_t tx_manager_get_min_active(tx_manager_t* manager) {
    if (manager == NULL) {
        transaction_id_t empty;
        memset(&empty, 0, sizeof(transaction_id_t));
        return empty;
    }

    // Atomic load - no lock needed
    transaction_id_t result = atomic_load(&manager->min_active_txn_id);

    // If min_active hasn't been computed yet (SENTINEL), recompute from shards.
    // This happens when transactions have started but no commit has triggered
    // a global min update yet.
    if (is_txn_id_sentinel(&result)) {
        recompute_global_min(manager);
        result = atomic_load(&manager->min_active_txn_id);
    }

    return result;
}

transaction_id_t tx_manager_get_last_committed(tx_manager_t* manager) {
    if (manager == NULL) {
        transaction_id_t empty;
        memset(&empty, 0, sizeof(transaction_id_t));
        return empty;
    }

    // Atomic load - no lock needed
    return atomic_load(&manager->last_committed_txn_id);
}

static void tx_manager_gc_callback(void* arg) {
    tx_manager_t* manager = (tx_manager_t*)arg;
    tx_manager_gc(manager);
}

size_t tx_manager_gc(tx_manager_t* manager) {
    if (manager == NULL || manager->trie == NULL) return 0;

    // Get minimum active transaction ID (GC cutoff)
    transaction_id_t min_active = tx_manager_get_min_active(manager);

    // Traverse trie and clean version chains
    size_t total_removed = hbtrie_gc(manager->trie, min_active);

    return total_removed;
}

void txn_desc_destroy(txn_desc_t* txn) {
    if (txn == NULL) return;

    refcounter_dereference((refcounter_t*)txn);
    if (refcounter_count((refcounter_t*)txn) == 0) {
        platform_lock_destroy(&txn->lock);
        free(txn);
    }
}

/**
 * Recompute the global minimum active transaction ID
 * by scanning all shard minimums.
 *
 * This is called when a transaction that is the current global
 * minimum commits or aborts. Reads shard mins atomically without
 * holding any lock — this is safe because a stale shard min is
 * always <= the true minimum, preserving the GC safety invariant.
 */
static void recompute_global_min(tx_manager_t* manager) {
    transaction_id_t new_global_min = TXN_ID_SENTINEL;

    for (int i = 0; i < TX_MANAGER_SHARDS; i++) {
        transaction_id_t shard_min = atomic_load(&manager->shards[i].min_txn_id);
        if (!is_txn_id_sentinel(&shard_min)) {
            if (is_txn_id_sentinel(&new_global_min) ||
                transaction_id_compare(&shard_min, &new_global_min) < 0) {
                new_global_min = shard_min;
            }
        }
    }

    if (is_txn_id_sentinel(&new_global_min)) {
        // No active transactions -- min_active = last_committed
        transaction_id_t last_committed = atomic_load(&manager->last_committed_txn_id);
        atomic_store(&manager->min_active_txn_id, last_committed);
    } else {
        atomic_store(&manager->min_active_txn_id, new_global_min);
    }
}