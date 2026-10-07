#ifndef HASH_H
#define HASH_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ht.h"

/* murmur3 32-bit (public domain, Austin Appleby) — byte-wise, alignment-safe */
static inline uint32_t murmur3_32(const uint8_t *key, size_t len, uint32_t seed) {
    uint32_t h = seed;
    const uint32_t c1 = 0xcc9e2d51, c2 = 0x1b873593;
    size_t i = 0;
    for (; i + 4 <= len; i += 4) {
        uint32_t k = (uint32_t)key[i] | ((uint32_t)key[i + 1] << 8) |
                     ((uint32_t)key[i + 2] << 16) | ((uint32_t)key[i + 3] << 24);
        k *= c1;
        k = (k << 15) | (k >> 17);
        k *= c2;
        h ^= k;
        h = (h << 13) | (h >> 19);
        h = h * 5 + 0xe6546b64;
    }
    uint32_t k = 0;
    switch (len & 3) {
    case 3: k ^= (uint32_t)key[i + 2] << 16; /* fallthrough */
    case 2: k ^= (uint32_t)key[i + 1] << 8;  /* fallthrough */
    case 1: k ^= (uint32_t)key[i];
        k *= c1;
        k = (k << 15) | (k >> 17);
        k *= c2;
        h ^= k;
    }
    h ^= (uint32_t)len;
    h ^= h >> 16;
    h *= 0x85ebca6b;
    h ^= h >> 13;
    h *= 0xc2b2ae35;
    h ^= h >> 16;
    return h;
}

/* hasheq for C-string keys, hashed with murmur3 (for ht.h) */
static inline uintptr_t str_hasheq(Ht_Op op, void const *a_, void const *b_, size_t n) {
    (void)n;
    char const *const *a = (char const *const *)a_;
    switch (op) {
    case HT_HASH:
        return (uintptr_t)murmur3_32((const uint8_t *)*a, strlen(*a), 0x51ed2701);
    case HT_EQ: {
        char const *const *b = (char const *const *)b_;
        return strcmp(*a, *b) == 0;
    }
    }
    return 0;
}

#endif /* HASH_H */
