// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

// UNIT C: runtime value model helpers (non-inline; the scalar constructors are
// inline in lisp_shader.h). Region wrapping + f32 narrowing live here. The
// interpreter (sh_interp.c) is the other half of Unit C.

#include "sh_internal.h"

sh_value sh_val_region_raw(void *base, uint32_t len, sh_kind elem, bool mutable_) {
    sh_value v = {0};
    v.kind = SH_K_REGION;
    v.lanes = 1;
    v.region.base = (uint8_t *)base;
    v.region.len = len;
    v.region.elem = elem;
    v.region.mutable_ = mutable_ ? 1 : 0;
    return v;
}

sh_value sh_val_region(lisp_value bytes, sh_kind elem, bool mutable_) {
    uint8_t *base = (uint8_t *)lisp_bytes_data(bytes);
    size_t bytelen = lisp_bytes_len(bytes);
    uint32_t esz = sh_kind_size(elem);
    uint32_t len = esz ? (uint32_t)(bytelen / esz) : 0;
    return sh_val_region_raw(base, len, elem, mutable_);
}
