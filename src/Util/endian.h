//
// endian.h - Big-endian read/write helpers for portable binary formats.
//
// Uses the project's portable_endian.h for cross-platform byte-order
// conversion (htobe16/be16toh/htobe32/be32toh/htobe64/be64toh).
//

#ifndef WAVEDB_ENDIAN_H
#define WAVEDB_ENDIAN_H

#include <stdint.h>
#include <string.h>
#include "portable_endian.h"

/* Write uint16_t in big-endian byte order. Returns val for convenience. */
static inline uint16_t write_be16(uint8_t* buf, uint16_t val) {
    uint16_t net_val = htobe16(val);
    memcpy(buf, &net_val, sizeof(uint16_t));
    return val;
}

/* Read uint16_t from big-endian byte order. */
static inline uint16_t read_be16(const uint8_t* buf) {
    uint16_t net_val;
    memcpy(&net_val, buf, sizeof(uint16_t));
    return be16toh(net_val);
}

/* Write uint32_t in big-endian byte order. Returns val for convenience. */
static inline uint32_t write_be32(uint8_t* buf, uint32_t val) {
    uint32_t net_val = htobe32(val);
    memcpy(buf, &net_val, sizeof(uint32_t));
    return val;
}

/* Read uint32_t from big-endian byte order. */
static inline uint32_t read_be32(const uint8_t* buf) {
    uint32_t net_val;
    memcpy(&net_val, buf, sizeof(uint32_t));
    return be32toh(net_val);
}

/* Write uint64_t in big-endian byte order. Returns val for convenience. */
static inline uint64_t write_be64(uint8_t* buf, uint64_t val) {
    uint32_t high = htobe32((uint32_t)(val >> 32));
    uint32_t low  = htobe32((uint32_t)(val & 0xFFFFFFFF));
    memcpy(buf, &high, sizeof(uint32_t));
    memcpy(buf + sizeof(uint32_t), &low, sizeof(uint32_t));
    return val;
}

/* Read uint64_t from big-endian byte order. */
static inline uint64_t read_be64(const uint8_t* buf) {
    uint32_t high, low;
    memcpy(&high, buf, sizeof(uint32_t));
    memcpy(&low, buf + sizeof(uint32_t), sizeof(uint32_t));
    return ((uint64_t)be32toh(high) << 32) | (uint64_t)be32toh(low);
}

#endif /* WAVEDB_ENDIAN_H */