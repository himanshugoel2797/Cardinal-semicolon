// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT
//
// SHA-256 against published FIPS 180-2 test vectors. This is the hash under the
// module-signing HMAC, so a regression here is a security-relevant break.

#include <stdint.h>
#include <string.h>

#include "test_framework.h"
#include "sha256.h"

static void sha256_buf(const void *data, size_t len, uint8_t out[32]) {
    SHA256_CTX ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, (const BYTE *)data, len);
    sha256_final(&ctx, out);
}

static void check_digest(const char *msg, const uint8_t expect[32]) {
    uint8_t got[32];
    sha256_buf(msg, strlen(msg), got);
    CHECK(memcmp(got, expect, 32) == 0);
}

void test_sha256(void) {
    printf("[sha256]\n");

    static const uint8_t empty[32] = {
        0xe3, 0xb0, 0xc4, 0x42, 0x98, 0xfc, 0x1c, 0x14,
        0x9a, 0xfb, 0xf4, 0xc8, 0x99, 0x6f, 0xb9, 0x24,
        0x27, 0xae, 0x41, 0xe4, 0x64, 0x9b, 0x93, 0x4c,
        0xa4, 0x95, 0x99, 0x1b, 0x78, 0x52, 0xb8, 0x55
    };
    check_digest("", empty);

    static const uint8_t abc[32] = {
        0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea,
        0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
        0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c,
        0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad
    };
    check_digest("abc", abc);

    static const uint8_t two_block[32] = {
        0x24, 0x8d, 0x6a, 0x61, 0xd2, 0x06, 0x38, 0xb8,
        0xe5, 0xc0, 0x26, 0x93, 0x0c, 0x3e, 0x60, 0x39,
        0xa3, 0x3c, 0xe4, 0x59, 0x64, 0xff, 0x21, 0x67,
        0xf6, 0xec, 0xed, 0xd4, 0x19, 0xdb, 0x06, 0xc1
    };
    check_digest("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
                 two_block);

    // Incremental updates must equal a one-shot update.
    uint8_t one_shot[32], incremental[32];
    sha256_buf("abc", 3, one_shot);
    SHA256_CTX ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, (const BYTE *)"a", 1);
    sha256_update(&ctx, (const BYTE *)"b", 1);
    sha256_update(&ctx, (const BYTE *)"c", 1);
    sha256_final(&ctx, incremental);
    CHECK(memcmp(one_shot, incremental, 32) == 0);
}
