//
// Transaction ID implementation
//

#include "transaction_id.h"
#include <time.h>
#include <string.h>
#include <stdatomic.h>
#include <arpa/inet.h>
#include "../Util/threadding.h"

// Thread-local pool size (number of IDs per batch)
#define TXN_ID_BATCH_SIZE 1000

// Global state for transaction ID generation
static transaction_id_t g_current_txn_id = {0, 0, 0};
static PLATFORMLOCKTYPE(g_txn_id_lock);
static atomic_uint_fast64_t g_global_count = ATOMIC_VAR_INIT(0);

// One-time initialization control
#if _WIN32
static INIT_ONCE g_init_once = INIT_ONCE_STATIC_INIT;
#else
static pthread_once_t g_init_once = PTHREAD_ONCE_INIT;
#endif

// Actual initialization function (called once)
static void transaction_id_do_init(void) {
    platform_lock_init(&g_txn_id_lock);
}

// Initialize global transaction ID generator (safe to call multiple times)
void transaction_id_init(void) {
#if _WIN32
    InitOnceExecuteOnce(&g_init_once, transaction_id_do_init, NULL, NULL);
#else
    pthread_once(&g_init_once, transaction_id_do_init);
#endif
}

// Generate unique transaction ID (thread-safe with atomic counter)
transaction_id_t transaction_id_get_next(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);

    // Atomic increment - much faster than mutex lock
    // Count rollover is handled by timestamp-first ordering in comparison
    uint64_t count = atomic_fetch_add(&g_global_count, 1);

    transaction_id_t next = {
        .time = (uint64_t)ts.tv_sec,
        .nanos = (uint64_t)ts.tv_nsec,
        .count = count
    };

    return next;
}

// Compare two transaction IDs
// Primary ordering: timestamp (CLOCK_MONOTONIC is always increasing)
// Secondary ordering: nanoseconds (within same second)
// Tertiary ordering: count (within same timestamp, ensures uniqueness)
// This handles count rollover correctly - when count wraps, timestamp will be greater
int transaction_id_compare(const transaction_id_t* id1, const transaction_id_t* id2) {
    // Primary: timestamp seconds
    if (id1->time > id2->time) {
        return 1;
    } else if (id1->time < id2->time) {
        return -1;
    }

    // Secondary: timestamp nanoseconds
    if (id1->nanos > id2->nanos) {
        return 1;
    } else if (id1->nanos < id2->nanos) {
        return -1;
    }

    // Tertiary: count (guarantees uniqueness within same timestamp)
    if (id1->count > id2->count) {
        return 1;
    } else if (id1->count < id2->count) {
        return -1;
    }

    return 0;
}

// Helper: write uint64_t in network byte order
static void write_uint64(uint8_t* buf, uint64_t val) {
    uint32_t high = htonl((uint32_t)(val >> 32));
    uint32_t low = htonl((uint32_t)(val & 0xFFFFFFFF));
    memcpy(buf, &high, sizeof(uint32_t));
    memcpy(buf + 4, &low, sizeof(uint32_t));
}

// Helper: read uint64_t in network byte order
static uint64_t read_uint64(const uint8_t* buf) {
    uint32_t high, low;
    memcpy(&high, buf, sizeof(uint32_t));
    memcpy(&low, buf + 4, sizeof(uint32_t));
    return ((uint64_t)ntohl(high) << 32) | ntohl(low);
}

// Serialize transaction ID to network byte order (24 bytes)
void transaction_id_serialize(const transaction_id_t* id, uint8_t* buf) {
    write_uint64(buf, id->time);
    write_uint64(buf + 8, id->nanos);
    write_uint64(buf + 16, id->count);
}

// Deserialize transaction ID from network byte order (24 bytes)
void transaction_id_deserialize(transaction_id_t* id, const uint8_t* buf) {
    id->time = read_uint64(buf);
    id->nanos = read_uint64(buf + 8);
    id->count = read_uint64(buf + 16);
}

// Advance the global transaction ID generator to at least the given ID
// This is needed after WAL recovery to prevent transaction ID collisions
void transaction_id_advance_to(const transaction_id_t* target) {
    transaction_id_init();

    // Advance the global count atomically
    uint64_t new_count = target->count + TXN_ID_BATCH_SIZE;  // Add buffer to ensure uniqueness
    uint64_t current = atomic_load(&g_global_count);

    // Use atomic compare-and-swap to ensure we only advance forward
    while (new_count > current) {
        if (atomic_compare_exchange_weak(&g_global_count, &current, new_count)) {
            // Successfully updated
            platform_lock(&g_txn_id_lock);
            g_current_txn_id = *target;
            platform_unlock(&g_txn_id_lock);
            break;
        }
        // Another thread updated current, retry the comparison
    }
}