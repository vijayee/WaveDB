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
#include <stdatomic.h>

#define DEFAULT_GC_INTERVAL_MS 100

// Forward declarations
static void tx_manager_gc_callback(void* arg);
static void gc_traverse_node(hbtrie_node_t* node, transaction_id_t min_active);

tx_manager_t* tx_manager_create(hbtrie_t* trie,
                                 work_pool_t* pool,
                                 hierarchical_timing_wheel_t* wheel,
                                 uint64_t gc_interval_ms) {
    if (trie == NULL || pool == NULL || wheel == NULL) return NULL;

    tx_manager_t* manager = get_clear_memory(sizeof(tx_manager_t));
    if (manager == NULL) return NULL;

    manager->trie = trie;
    manager->pool = pool;
    manager->wheel = wheel;
    manager->gc_interval_ms = (gc_interval_ms == 0) ? DEFAULT_GC_INTERVAL_MS : gc_interval_ms;
    manager->last_gc_time = 0;

    vec_init(&manager->active_txns);
    transaction_id_t min_active_init = {0, 0, 0};
    transaction_id_t last_committed_init = {0, 0, 0};
    atomic_init(&manager->min_active_txn_id, min_active_init);
    atomic_init(&manager->last_committed_txn_id, last_committed_init);

    platform_lock_init(&manager->lock);
    refcounter_init((refcounter_t*)manager);

    return manager;
}

void tx_manager_destroy(tx_manager_t* manager) {
    if (manager == NULL) return;

    refcounter_dereference((refcounter_t*)manager);
    if (refcounter_count((refcounter_t*)manager) == 0) {
        platform_lock(&manager->lock);

        // Destroy all active transactions
        for (int i = 0; i < manager->active_txns.length; i++) {
            txn_desc_t* txn = manager->active_txns.data[i];
            if (txn != NULL) {
                txn_desc_destroy(txn);
            }
        }
        vec_deinit(&manager->active_txns);

        platform_unlock(&manager->lock);
        platform_lock_destroy(&manager->lock);
        free(manager);
    }
}

txn_desc_t* tx_manager_begin(tx_manager_t* manager) {
    if (manager == NULL) return NULL;

    txn_desc_t* txn = get_clear_memory(sizeof(txn_desc_t));
    if (txn == NULL) return NULL;

    // Get new transaction ID (thread-safe, doesn't need manager lock)
    txn->txn_id = transaction_id_get_next();
    txn->state = TXN_ACTIVE;

    // Initialize lock and refcounter BEFORE making visible to other threads
    platform_lock_init(&txn->lock);
    refcounter_init((refcounter_t*)txn);

    platform_lock(&manager->lock);

    // Add to active list (now txn is fully initialized)
    vec_push(&manager->active_txns, txn);

    // Update min_active if needed
    transaction_id_t current_min = atomic_load(&manager->min_active_txn_id);
    if (manager->active_txns.length == 1 ||
        transaction_id_compare(&txn->txn_id, &current_min) < 0) {
        atomic_store(&manager->min_active_txn_id, txn->txn_id);
    }

    platform_unlock(&manager->lock);

    return txn;
}

int tx_manager_commit(tx_manager_t* manager, txn_desc_t* txn) {
    if (manager == NULL || txn == NULL) return -1;

    platform_lock(&manager->lock);
    platform_lock(&txn->lock);

    if (txn->state != TXN_ACTIVE) {
        platform_unlock(&txn->lock);
        platform_unlock(&manager->lock);
        return -1;  // Already committed or aborted
    }

    txn->state = TXN_COMMITTED;

    // Update last_committed
    transaction_id_t current_last_committed = atomic_load(&manager->last_committed_txn_id);
    if (transaction_id_compare(&txn->txn_id, &current_last_committed) > 0) {
        atomic_store(&manager->last_committed_txn_id, txn->txn_id);
    }

    // Remove from active list
    for (int i = 0; i < manager->active_txns.length; i++) {
        if (manager->active_txns.data[i] == txn) {
            vec_splice(&manager->active_txns, i, 1);
            break;
        }
    }

    // Update min_active
    if (manager->active_txns.length > 0) {
        transaction_id_t new_min = manager->active_txns.data[0]->txn_id;
        for (int i = 1; i < manager->active_txns.length; i++) {
            if (transaction_id_compare(&manager->active_txns.data[i]->txn_id, &new_min) < 0) {
                new_min = manager->active_txns.data[i]->txn_id;
            }
        }
        atomic_store(&manager->min_active_txn_id, new_min);
    } else {
        // No active transactions - min_active = last_committed
        // Reload last_committed as it may have been updated above
        transaction_id_t final_last_committed = atomic_load(&manager->last_committed_txn_id);
        atomic_store(&manager->min_active_txn_id, final_last_committed);
    }

    platform_unlock(&txn->lock);
    platform_unlock(&manager->lock);

    return 0;
}

int tx_manager_abort(tx_manager_t* manager, txn_desc_t* txn) {
    if (manager == NULL || txn == NULL) return -1;

    platform_lock(&manager->lock);
    platform_lock(&txn->lock);

    if (txn->state != TXN_ACTIVE) {
        platform_unlock(&txn->lock);
        platform_unlock(&manager->lock);
        return -1;  // Already committed or aborted
    }

    txn->state = TXN_ABORTED;

    // Remove from active list
    for (int i = 0; i < manager->active_txns.length; i++) {
        if (manager->active_txns.data[i] == txn) {
            vec_splice(&manager->active_txns, i, 1);
            break;
        }
    }

    // Update min_active (same logic as commit)
    if (manager->active_txns.length > 0) {
        transaction_id_t new_min = manager->active_txns.data[0]->txn_id;
        for (int i = 1; i < manager->active_txns.length; i++) {
            if (transaction_id_compare(&manager->active_txns.data[i]->txn_id, &new_min) < 0) {
                new_min = manager->active_txns.data[i]->txn_id;
            }
        }
        atomic_store(&manager->min_active_txn_id, new_min);
    } else {
        transaction_id_t last = atomic_load(&manager->last_committed_txn_id);
        atomic_store(&manager->min_active_txn_id, last);
    }

    platform_unlock(&txn->lock);
    platform_unlock(&manager->lock);

    return 0;
}

transaction_id_t tx_manager_get_min_active(tx_manager_t* manager) {
    if (manager == NULL) {
        transaction_id_t empty;
        memset(&empty, 0, sizeof(transaction_id_t));
        return empty;
    }

    // Atomic load - no lock needed
    return atomic_load(&manager->min_active_txn_id);
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