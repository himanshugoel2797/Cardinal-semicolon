#ifndef CARDINAL_AES_HMAC_H
#define CARDINAL_AES_HMAC_H

#include "sha256.h"
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t o_key_pad[256 / 8];
    SHA256_CTX hash_ctx;
} hmac_ctx;

int hmac_init(hmac_ctx *ctx, uint8_t *key);

int hmac_update(hmac_ctx *ctx, uint8_t *message, size_t message_len);

int hmac_final(hmac_ctx *ctx, uint8_t *result);

#endif