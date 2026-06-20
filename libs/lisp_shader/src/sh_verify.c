// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

// UNIT B -- THE VERIFIER (the trusted moat; no MMU firewall stands behind it).
// Type-checks the parsed AST in place, derives the bounded-loop trip bounds and
// worst-case cost, discharges static region bounds (SH_NF_BOUNDS_PROVEN), and
// enforces the capability whitelist. No allocation, no I/O.
//
// STUB: returns SH_ERR_INTERNAL until implemented. See
// notes/scratch/shader-proposal-minimalist.md sections 2-5.

#include "sh_internal.h"

sh_status shv_verify(sh_program *p, const sh_prim_set *prims, uint32_t flags,
                     sh_error *err) {
    (void)p;
    (void)prims;
    (void)flags;
    return sh_set_error(err, SH_ERR_INTERNAL, -1, -1, "verifier not implemented");
}
