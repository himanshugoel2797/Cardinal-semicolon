// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

// Wasm binary decoder: raw .wasm bytes -> wasm_module_t (sections). STUB --
// implemented in the decode workstream against the frozen wasm_internal.h.

#include "wasm_internal.h"
#include <stdlib.h>

wasm_module_t *wasm_decode_impl(const uint8_t *buf, size_t len, wasm_result_t *err) {
    (void)buf;
    (void)len;
    if (err) *err = WASM_ERR_DECODE;
    return NULL;
}

void wasm_module_free_impl(wasm_module_t *m) {
    (void)m;
}
