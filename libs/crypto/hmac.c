#include "hmac.h"

int hmac_init(hmac_ctx *ctx, uint8_t *key) {

    uint8_t h_key_full[256 / 8];
    uint8_t i_key_pad[256 / 8];

    {
        SHA256_CTX sha_ctxt;
        sha256_init(&sha_ctxt);
        sha256_update(&sha_ctxt, key, 256 / 8);
        sha256_final(&sha_ctxt, h_key_full);
    }

    for (int i = 0; i < 256 / 8; i++) {
        i_key_pad[i] = 0x36 ^ h_key_full[i];
        ctx->o_key_pad[i] = 0x5c ^ h_key_full[i];
    }

    // hash(o_key_pad || hash(i_key_pad || message))
    sha256_init(&ctx->hash_ctx);
    sha256_update(&ctx->hash_ctx, i_key_pad, sizeof(i_key_pad));
    return 0;
}

int hmac_update(hmac_ctx *ctx, uint8_t *message, size_t message_len) {
    sha256_update(&ctx->hash_ctx, message, message_len);
    return 0;
}

int hmac_final(hmac_ctx *ctx, uint8_t *result) {

    uint8_t hash_0[256 / 8];

    sha256_final(&ctx->hash_ctx, hash_0);

    SHA256_CTX sha_ctxt;
    sha256_init(&sha_ctxt);
    sha256_update(&sha_ctxt, ctx->o_key_pad, sizeof(ctx->o_key_pad));
    sha256_update(&sha_ctxt, hash_0, sizeof(hash_0));
    sha256_final(&sha_ctxt, result);

    return 0;
}