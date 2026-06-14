// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT
//
// HMAC is the module-signing MAC (libs/crypto/hmac.c). NOTE: this is NOT the
// RFC 2104 / RFC 4231 HMAC -- it hashes a fixed 32 bytes of key and uses a
// 32-byte (not block-size) pad, so it will not match standard test vectors and
// must not be changed without re-signing every module. These tests therefore
// pin behaviour rather than a standard: determinism plus sensitivity to key and
// message. See notes/AUDIT.md for the short-key out-of-bounds caveat.

#include <stdint.h>
#include <string.h>

#include "test_framework.h"
#include "hmac.h"

static void hmac_compute(uint8_t key[32], const void *msg, size_t len,
                         uint8_t out[32]) {
    hmac_ctx ctx;
    hmac_init(&ctx, key);
    hmac_update(&ctx, (uint8_t *)msg, len);
    hmac_final(&ctx, out);
}

void test_hmac(void) {
    printf("[hmac]\n");

    uint8_t key[32];
    for (int i = 0; i < 32; i++)
        key[i] = (uint8_t)i;

    const char *msg = "The quick brown fox";

    uint8_t a[32], b[32];
    hmac_compute(key, msg, strlen(msg), a);
    hmac_compute(key, msg, strlen(msg), b);
    CHECK(memcmp(a, b, 32) == 0);  // deterministic

    // Output must not be trivially zero/constant.
    int nonzero = 0;
    for (int i = 0; i < 32; i++)
        nonzero |= a[i];
    CHECK(nonzero != 0);

    // Sensitivity to the message.
    uint8_t c[32];
    hmac_compute(key, "The quick brown foX", strlen(msg), c);
    CHECK(memcmp(a, c, 32) != 0);

    // Sensitivity to the key.
    uint8_t key2[32];
    memcpy(key2, key, 32);
    key2[0] ^= 0x80;
    uint8_t d[32];
    hmac_compute(key2, msg, strlen(msg), d);
    CHECK(memcmp(a, d, 32) != 0);

    // Incremental update equals one-shot.
    hmac_ctx ictx;
    uint8_t e[32];
    hmac_init(&ictx, key);
    hmac_update(&ictx, (uint8_t *)"The quick ", 10);
    hmac_update(&ictx, (uint8_t *)"brown fox", 9);
    hmac_final(&ictx, e);
    CHECK(memcmp(a, e, 32) == 0);
}
