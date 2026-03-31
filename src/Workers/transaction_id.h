//
// Transaction ID with nanosecond precision for storage layer
//

#ifndef WAVEDB_TRANSACTION_ID_H
#define WAVEDB_TRANSACTION_ID_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint64_t time;    // Seconds since epoch
    uint64_t nanos;   // Nanoseconds within second
    uint64_t count;   // Sequence counter for same-timestamp ordering
} transaction_id_t;

// Initialize global transaction ID generator (call once at startup)
void transaction_id_init(void);

// Generate unique transaction ID (thread-safe)
transaction_id_t transaction_id_get_next(void);

// Compare two transaction IDs lexicographically (time, nanos, count)
// Returns: 1 if id1 > id2, -1 if id1 < id2, 0 if equal
int transaction_id_compare(const transaction_id_t* id1, const transaction_id_t* id2);

// Serialize transaction ID to network byte order (24 bytes)
// buf must have space for at least 24 bytes
void transaction_id_serialize(const transaction_id_t* id, uint8_t* buf);

// Deserialize transaction ID from network byte order (24 bytes)
void transaction_id_deserialize(transaction_id_t* id, const uint8_t* buf);

// Advance the global transaction ID generator to at least the given ID
// This is needed after WAL recovery to prevent transaction ID collisions
void transaction_id_advance_to(const transaction_id_t* target);

#ifdef __cplusplus
}
#endif

#endif // WAVEDB_TRANSACTION_ID_H