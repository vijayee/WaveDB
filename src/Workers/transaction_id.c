//
// Transaction ID implementation
//

#include "transaction_id.h"
#include <time.h>
#include <string.h>
#include <arpa/inet.h>
#include "../Util/threadding.h"

// Global state for transaction ID generation
static PLATFORMLOCKTYPE(g_txn_id_lock);
static transaction_id_t g_current_txn_id = {0, 0, 0};

// Initialize global transaction ID generator
void transaction_id_init(void) {
    platform_lock_init(&g_txn_id_lock);
}

// Generate unique transaction ID (thread-safe)
transaction_id_t transaction_id_get_next(void) {
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
        platform_unlock(&g_txn_id_lock);
        return next;
    }

    // Compare with current
    int cmp = transaction_id_compare(&next, &g_current_txn_id);

    if (cmp == 1) {
        // New timestamp is strictly greater - use it
        g_current_txn_id = next;
        platform_unlock(&g_txn_id_lock);
        return next;
    } else {
        // Same time or clock went backward - increment count
        next.count = g_current_txn_id.count;
        while (cmp == 0 || cmp == -1) {
            next.count++;
            cmp = transaction_id_compare(&next, &g_current_txn_id);
        }
        g_current_txn_id = next;
        platform_unlock(&g_txn_id_lock);
        return next;
    }
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