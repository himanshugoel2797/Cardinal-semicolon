// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#ifndef CARDINAL_HASH_H
#define CARDINAL_HASH_H

#include <stddef.h>
#include <stdint.h>

// FNV-1a 32-bit hash. Shared so the kernel symbol DB and libs/kvs don't each
// carry their own copy (the kernel can't link against libs/).
#define FNV1A_BASIS 2166136261u
#define FNV1A_PRIME 16777619u

static inline uint32_t fnv1a_hash(const char *src, size_t src_len) {
    uint32_t h = FNV1A_BASIS;
    for (size_t i = 0; i < src_len; i++) {
        h ^= (uint8_t)src[i];
        h *= FNV1A_PRIME;
    }
    return h;
}

#endif
