// src/Util/xxhash.h - Minimal xxHash implementation
#ifndef WAVEDB_XXHASH_H
#define WAVEDB_XXHASH_H

#include <stdint.h>
#include <stddef.h>

static inline uint64_t xxh64_rotl(uint64_t x, int r) {
    return (x << r) | (x >> (64 - r));
}

static inline uint64_t xxh64_round(uint64_t acc, uint64_t val) {
    acc += val * 0x9E3779B97F4A7C15ULL;
    acc = xxh64_rotl(acc, 31);
    acc *= 0xBF58476D1CE4E5B9ULL;
    return acc;
}

static inline uint64_t xxh64_merge(uint64_t h, uint64_t val) {
    h ^= xxh64_round(0, val);
    h *= 0x9E3779B97F4A7C15ULL;
    return h;
}

static inline uint64_t xxhash64(const void* data, size_t len, uint64_t seed) {
    const uint8_t* p = (const uint8_t*)data;
    const uint8_t* end = p + len;
    uint64_t h;

    if (len >= 32) {
        uint64_t v1 = seed + 0x9E3779B97F4A7C15ULL + 0x9E3779B97F4A7C15ULL;
        uint64_t v2 = seed + 0x9E3779B97F4A7C15ULL;
        uint64_t v3 = seed;
        uint64_t v4 = seed - 0x9E3779B97F4A7C15ULL;

        do {
            v1 = xxh64_round(v1, *(const uint64_t*)(void*)p); p += 8;
            v2 = xxh64_round(v2, *(const uint64_t*)(void*)p); p += 8;
            v3 = xxh64_round(v3, *(const uint64_t*)(void*)p); p += 8;
            v4 = xxh64_round(v4, *(const uint64_t*)(void*)p); p += 8;
        } while (p <= end - 32);

        h = xxh64_rotl(v1, 1) + xxh64_rotl(v2, 7) + xxh64_rotl(v3, 12) + xxh64_rotl(v4, 18);
        h = xxh64_merge(h, v1);
        h = xxh64_merge(h, v2);
        h = xxh64_merge(h, v3);
        h = xxh64_merge(h, v4);
    } else {
        h = seed + 0x9E3779B97F4A7C15ULL;
    }

    h ^= len;

    while (p + 8 <= end) {
        h ^= xxh64_round(0, *(const uint64_t*)(void*)p);
        h = xxh64_rotl(h, 27) * 0x9E3779B97F4A7C15ULL;
        p += 8;
    }

    while (p + 4 <= end) {
        h ^= *(const uint32_t*)(void*)p * 0x9E3779B97F4A7C15ULL;
        h = xxh64_rotl(h, 23) * 0xBF58476D1CE4E5B9ULL;
        p += 4;
    }

    while (p < end) {
        h ^= (*p++) * 0x9E3779B97F4A7C15ULL;
        h = xxh64_rotl(h, 5) * 0xBF58476D1CE4E5B9ULL;
    }

    h ^= h >> 33;
    h *= 0x62A9D5E4B79D9B49ULL;
    h ^= h >> 29;
    h *= 0x4CF5AD432745937FULL;
    h ^= h >> 32;

    return h;
}

#endif