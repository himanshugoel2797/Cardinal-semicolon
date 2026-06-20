// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

// UNIT C -- the reference interpreter (the semantic oracle). A tree-walker over
// the VERIFIED typed AST. Every vector op is defined by a lane loop here (the
// mandatory scalar lowering -- SIMD is a perf property of a future backend,
// never correctness). Every region access is bounds-checked at runtime.
//
// The public sh_invoke (arg validation + dispatch) lives here; shi_invoke is the
// internal entry. STUB until implemented. See
// notes/scratch/shader-proposal-minimalist.md sections 1 and 8.

#include "sh_internal.h"

sh_status shi_invoke(const sh_program *p, const sh_value *args, uint32_t argc,
                     sh_value *out, sh_error *err) {
    (void)p;
    (void)args;
    (void)argc;
    (void)out;
    return sh_set_error(err, SH_ERR_INTERNAL, -1, -1, "interpreter not implemented");
}

uint64_t shi_cost_for_args(const sh_program *p, const sh_value *args, uint32_t argc) {
    (void)args;
    (void)argc;
    return p ? p->cost.const_cost : 0;
}

// Public entry: validate the program is verified and the args match the declared
// signature, then run. (Argument-type validation against p->params can move into
// Unit C's real implementation; kept minimal here.)
sh_status sh_invoke(const sh_program *p, const sh_value *args, uint32_t argc,
                    sh_value *out, sh_error *err) {
    if (!p || !p->verified)
        return sh_set_error(err, SH_ERR_INTERNAL, -1, -1, "invoke of unverified program");
    if (argc != p->nparams)
        return sh_set_error(err, SH_ERR_ARITY, -1, -1,
                            "shader expects %u args, got %u", p->nparams, argc);
    return shi_invoke(p, args, argc, out, err);
}
