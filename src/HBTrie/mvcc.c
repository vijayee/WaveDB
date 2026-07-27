//
// MVCC Transaction Manager Implementation
//
// Created by victor on 3/17/26.
//

#include "mvcc.h"
#include "bnode.h"
#include "../Util/allocator.h"
#include "../Util/memory_pool.h"
#include "../Util/log.h"
#include <string.h>
#include "Util/atomic_compat.h"

#define DEFAULT_GC_INTERVAL_MS 100

/* Seqlock write_lock is a plain volatile int, not a _wdb_atomic wrapper.
 * Provide small helper that works on MSVC (InterlockedExchange) and
 * C11/GCC/Clang (atomic_exchange_explicit). */

#if defined(_MSC_VER)
  #include <windows.h>
  static inline int seqlock_acquire(volatile int* lock) {
    while (InterlockedExchange((LONG*)lock, 1)) {
      /* spin */
    }
    return 0;
  }
  static inline void seqlock_release(volatile int* lock) {
    InterlockedExchange((LONG*)lock, 0);
  }
  static inline uint64_t seqlock_seq_load(atomic_uint_fast64_t* seq) {
    return (uint64_t)InterlockedCompareExchange64(&seq->v, 0, 0);
  }
  static inline void seqlock_seq_store(atomic_uint_fast64_t* seq, uint64_t val) {
    InterlockedExchange64(&seq->v, (LONG64)val);
  }
  static inline uint64_t seqlock_seq_add(atomic_uint_fast64_t* seq, int64_t delta) {
    return (uint64_t)InterlockedExchangeAdd64(&seq->v, (LONG64)delta);
  }
#else
  #include <stdatomic.h>
  static inline int seqlock_acquire(volatile int* lock) {
    while (atomic_exchange_explicit((atomic_int*)lock, 1, memory_order_acquire)) {
      /* spin */
    }
    return 0;
  }
  static inline void seqlock_release(volatile int* lock) {
    atomic_store_explicit((atomic_int*)lock, 0, memory_order_release);
  }
  static inline uint64_t seqlock_seq_load(atomic_uint_fast64_t* seq) {
    return (uint64_t)atomic_load(seq);
  }
  static inline void seqlock_seq_store(atomic_uint_fast64_t* seq, uint64_t val) {
    atomic_store(seq, (uint_fast64_t)val);
  }
  static inline uint64_t seqlock_seq_add(atomic_uint_fast64_t* seq, int64_t delta) {
    return (uint64_t)atomic_fetch_add(seq, (uint_fast64_t)delta);
  }
#endif

/* ── Seqlock helpers for txn_id_seqlock_t ──
 *
 * Readers are lock-free: they read the sequence counter, memcpy the
 * value, re-read the sequence, and retry only if it changed or was odd
 * (write in progress). Writers serialize via a spinlock and bump the
 * sequence odd → even around the write so readers see a consistent
 * value. This replaces the 24-byte _Atomic(transaction_id_t) which
 * had no hardware CAS on x86-64 and fell back to libatomic locks. */

void txn_id_seqlock_init(txn_id_seqlock_t* sl, transaction_id_t val) {
    seqlock_seq_store(&sl->seq, 0);
    sl->value = val;
    sl->write_lock = 0;
    /* Release the seq to 0 (even = stable) so the first read succeeds. */
    seqlock_seq_store(&sl->seq, 0);
}

void txn_id_seqlock_read(const txn_id_seqlock_t* sl, transaction_id_t* out) {
    uint64_t seq1, seq2;
    do {
        seq1 = seqlock_seq_load(&sl->seq);
        /* Make sure the read of the value happens after the read of seq
         * (acquire). */
        memcpy(out, (const void*)&sl->value, sizeof(transaction_id_t));
        seq2 = seqlock_seq_load(&sl->seq);
    } while (seq1 != seq2 || (seq1 & 1));
}

void txn_id_seqlock_write(txn_id_seqlock_t* sl, transaction_id_t val) {
    /* Acquire the writer spinlock so concurrent writers don't clobber
     * each other's sequence bumps. */
    seqlock_acquire(&sl->write_lock);
    seqlock_seq_add(&sl->seq, 1);  /* odd: write in progress */
    /* Store the value. */
    memcpy((void*)&sl->value, &val, sizeof(transaction_id_t));
    seqlock_seq_add(&sl->seq, 1);  /* even: stable */
    seqlock_release(&sl->write_lock);
}

int txn_id_seqlock_cas(txn_id_seqlock_t* sl, const transaction_id_t* expected, transaction_id_t desired) {
    /* Serialize with other writers. */
    seqlock_acquire(&sl->write_lock);
    transaction_id_t current;
    txn_id_seqlock_read(sl, &current);
    int swapped = 0;
    if (transaction_id_compare(&current, expected) == 0) {
        seqlock_seq_add(&sl->seq, 1);  /* odd */
        memcpy((void*)&sl->value, &desired, sizeof(transaction_id_t));
        seqlock_seq_add(&sl->seq, 1);  /* even */
        swapped = 1;
    }
    seqlock_release(&sl->write_lock);
    return swapped;
}

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
        txn_id_seqlock_init(&manager->shards[i].min_txn_id, TXN_ID_SENTINEL);
        atomic_init(&manager->shards[i].count, 0);
        platform_lock_init(&manager->shards[i].lock);
    }

    transaction_id_t last_committed_init = {0, 0, 0};
    txn_id_seqlock_init(&manager->min_active_txn_id, TXN_ID_SENTINEL);
    txn_id_seqlock_init(&manager->last_committed_txn_id, last_committed_init);

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

    txn_desc_t* txn = memory_pool_alloc(sizeof(txn_desc_t));
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
    transaction_id_t current_shard_min;
    txn_id_seqlock_read(&shard->min_txn_id, &current_shard_min);
    if (is_txn_id_sentinel(&current_shard_min)) {
        txn_id_seqlock_write(&shard->min_txn_id, txn->txn_id);
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
    transaction_id_t current_last;
    txn_id_seqlock_read(&manager->last_committed_txn_id, &current_last);
    while (transaction_id_compare(&txn->txn_id, &current_last) > 0) {
        if (txn_id_seqlock_cas(&manager->last_committed_txn_id,
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
    transaction_id_t current_shard_min;
    txn_id_seqlock_read(&shard->min_txn_id, &current_shard_min);
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
            txn_id_seqlock_write(&shard->min_txn_id, new_shard_min);
        } else {
            txn_id_seqlock_write(&shard->min_txn_id, TXN_ID_SENTINEL);
        }
    }

    platform_unlock(&txn->lock);
    platform_unlock(&shard->lock);

    // Update global min_active_txn_id if necessary
    // Must recompute if: (a) we were the global minimum, or (b) global min is sentinel
    transaction_id_t current_global_min;
    txn_id_seqlock_read(&manager->min_active_txn_id, &current_global_min);
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
    transaction_id_t current_shard_min;
    txn_id_seqlock_read(&shard->min_txn_id, &current_shard_min);
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
            txn_id_seqlock_write(&shard->min_txn_id, new_shard_min);
        } else {
            txn_id_seqlock_write(&shard->min_txn_id, TXN_ID_SENTINEL);
        }
    }

    platform_unlock(&txn->lock);
    platform_unlock(&shard->lock);

    // Update global min_active_txn_id if necessary
    // (Same logic as commit, except we don't update last_committed)
    transaction_id_t current_global_min;
    txn_id_seqlock_read(&manager->min_active_txn_id, &current_global_min);
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
    transaction_id_t result;
    txn_id_seqlock_read(&manager->min_active_txn_id, &result);

    // If min_active hasn't been computed yet (SENTINEL), recompute from shards.
    // This happens when transactions have started but no commit has triggered
    // a global min update yet.
    if (is_txn_id_sentinel(&result)) {
        recompute_global_min(manager);
        txn_id_seqlock_read(&manager->min_active_txn_id, &result);
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
    transaction_id_t result;
    txn_id_seqlock_read(&manager->last_committed_txn_id, &result);
    return result;
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
        memory_pool_free(txn, sizeof(txn_desc_t));
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
        transaction_id_t shard_min;
        txn_id_seqlock_read(&manager->shards[i].min_txn_id, &shard_min);
        if (!is_txn_id_sentinel(&shard_min)) {
            if (is_txn_id_sentinel(&new_global_min) ||
                transaction_id_compare(&shard_min, &new_global_min) < 0) {
                new_global_min = shard_min;
            }
        }
    }

    if (is_txn_id_sentinel(&new_global_min)) {
        // No active transactions -- min_active = last_committed
        transaction_id_t last_committed;
        txn_id_seqlock_read(&manager->last_committed_txn_id, &last_committed);
        txn_id_seqlock_write(&manager->min_active_txn_id, last_committed);
    } else {
        txn_id_seqlock_write(&manager->min_active_txn_id, new_global_min);
    }
}