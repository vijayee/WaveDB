//
// Transaction ID implementation
//

#include "transaction_id.h"
#include <time.h>
#include <string.h>
#include <arpa/inet.h>
#include "../Util/threadding.h"

// Thread-local pool size (number of IDs per batch)
#define TXN_ID_BATCH_SIZE 1000

// Thread-local pool for transaction ID generation
typedef struct {
    transaction_id_t current;    // Current ID in the pool
    transaction_id_t max;        // Maximum ID in the pool
    uint64_t remaining;          // Number of IDs remaining in pool
    int initialized;             // Whether this thread pool is initialized
} txn_id_pool_t;

// Global state for transaction ID generation
static PLATFORMLOCKTYPE(g_txn_id_lock);
static transaction_id_t g_current_txn_id = {0, 0, 0};

// Thread-local pool
static __thread txn_id_pool_t thread_pool = {0};

// Initialize global transaction ID generator
void transaction_id_init(void) {
    platform_lock_init(&g_txn_id_lock);
}

// Allocate a new batch of transaction IDs from global pool
static transaction_id_t allocate_batch(void) {
    platform_lock(&g_txn_id_lock);

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);

    transaction_id_t next = {
        .time = (uint64_t)ts.tv_sec,
        .nanos = (uint64_t)ts.tv_nsec,
        .count = 0
    };

    // First call ever
    if (g_current_txn_id.time == 0 && g_current_txn_id.nanos == 0 && g_current_txn_id.count == 0) {
        g_current_txn_id = next;
        g_current_txn_id.count = TXN_ID_BATCH_SIZE;
        platform_unlock(&g_txn_id_lock);
        return next;
    }

    // Compare with current
    int cmp = transaction_id_compare(&next, &g_current_txn_id);

    if (cmp == 1) {
        // New timestamp is strictly greater - use it
        g_current_txn_id = next;
        g_current_txn_id.count = TXN_ID_BATCH_SIZE;
        platform_unlock(&g_txn_id_lock);
        return next;
    } else {
        // Same time or clock went backward - use current + batch size
        next = g_current_txn_id;
        next.count += TXN_ID_BATCH_SIZE;
        g_current_txn_id.count += TXN_ID_BATCH_SIZE;
        platform_unlock(&g_txn_id_lock);
        return next;
    }
}

// Generate unique transaction ID (thread-safe with thread-local pool)
transaction_id_t transaction_id_get_next(void) {
    // Fast path: use thread-local pool if available
    if (thread_pool.initialized && thread_pool.remaining > 0) {
        thread_pool.remaining--;
        thread_pool.current.count++;
        return thread_pool.current;
    }

    // Slow path: allocate new batch from global pool
    transaction_id_t batch_start = allocate_batch();

    // Initialize thread-local pool
    thread_pool.current = batch_start;
    thread_pool.max.time = batch_start.time;
    thread_pool.max.nanos = batch_start.nanos;
    thread_pool.max.count = batch_start.count + TXN_ID_BATCH_SIZE - 1;
    thread_pool.remaining = TXN_ID_BATCH_SIZE;
    thread_pool.initialized = 1;

    // Return first ID in batch
    thread_pool.remaining--;
    return thread_pool.current;
}

// Compare two transaction IDs lexicographically
int transaction_id_compare(const transaction_id_t* id1, const transaction_id_t* id2) {
    if (id1->time > id2->time) {
        return 1;
    } else if (id1->time < id2->time) {
        return -1;
    } else if (id1->nanos > id2->nanos) {
        return 1;
    } else if (id1->nanos < id2->nanos) {
        return -1;
    } else if (id1->count > id2->count) {
        return 1;
    } else if (id1->count < id2->count) {
        return -1;
    } else {
        return 0;
    }
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