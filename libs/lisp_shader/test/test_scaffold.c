// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

// Scaffold smoke test: exercises the frozen contract that is REAL today (type
// helpers, arena builders, the error helper, value constructors, the compile
// pipeline reaching the stubbed seams). The per-unit suites (test_frontend.c,
// test_verify.c, test_interp_*.c, test_diff.c) replace/augment this as the units
// land. Its job now: prove the scaffold builds, links, and the seams wire up.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lisp.h"
#include "sh_internal.h"

static int failures = 0;
#define CHECK(cond, msg)                                       \
    do {                                                       \
        if (!(cond)) {                                         \
            printf("  FAIL %s\n", (msg));                      \
            failures++;                                        \
        }                                                      \
    } while (0)

int main(void) {
    // The reader interns symbols; bring the runtime up like the lisp harness.
    (void)lisp_default_env();

    // --- type helpers ---
    sh_type u32 = sh_type_scalar(SH_K_U32);
    sh_type f32x4 = sh_type_vec(SH_K_F32, 4);
    sh_type reg = sh_type_region(SH_K_U16, false);
    CHECK(sh_type_eq(u32, sh_type_scalar(SH_K_U32)), "u32 type eq");
    CHECK(!sh_type_eq(u32, f32x4), "u32 != f32x4");
    CHECK(f32x4.kind == SH_K_VEC && f32x4.lane_kind == SH_K_F32 && f32x4.lanes == 4, "vec encoding");
    CHECK(reg.kind == SH_K_REGION && (reg.flags & SH_TYPE_FLAG_MUTABLE) == 0, "region immutable");
    CHECK(sh_kind_size(SH_K_U16) == 2 && sh_kind_size(SH_K_F64) == 8, "kind sizes");
    CHECK(sh_kind_is_int(SH_K_U32) && sh_kind_is_float(SH_K_F32), "kind predicates");

    // --- value constructors ---
    sh_value v = sh_val_f32(1.5f);
    CHECK(v.kind == SH_K_F32 && v.f == 1.5, "f32 value");
    sh_value vr = sh_val_region_raw((void *)0x1000, 64, SH_K_U32, true);
    CHECK(vr.kind == SH_K_REGION && vr.region.len == 64 && vr.region.mutable_, "region value");

    // --- arena builders ---
    sh_program *p = calloc(1, sizeof(*p));
    sh_nref n0 = sh_node_alloc(p, SH_OP_CONST);
    sh_nref n1 = sh_node_alloc(p, SH_OP_PARAM);
    CHECK(n0 == 0 && n1 == 1 && p->nnodes == 2, "node alloc ids");
    CHECK(p->nodes[n0].a == SH_NREF_NONE, "node children default to NONE");
    uint32_t off = 999;
    CHECK(sh_aux_reserve(p, 3, &off) && off == 0 && p->naux == 3, "aux reserve");
    uint32_t li = 999;
    sh_loop_alloc(p, &li);
    CHECK(li == 0 && p->nloops == 1, "loop alloc");
    free(p->nodes);
    free(p->aux);
    free(p->loops);
    free(p);

    // --- error helper ---
    sh_error err = {0};
    sh_status s = sh_set_error(&err, SH_ERR_TYPE, 3, 7, "expected %s got %s", "u32", "f32");
    CHECK(s == SH_ERR_TYPE && err.line == 3 && err.col == 7, "error fields");
    CHECK(strstr(err.msg, "expected u32 got f32") != NULL, "error message format");

    // --- pipeline: frontend + verifier real; compile succeeds end-to-end ---
    sh_program *prog = NULL;
    err = (sh_error){0};
    s = sh_compile_string("(defshader id ((x u32)) -> u32 x)", NULL, 0, &prog, &err);
    CHECK(s == SH_OK && prog != NULL, "identity shader compiles end-to-end");
    if (prog) {
        CHECK(strcmp(sh_name(prog), "id") == 0, "compiled shader name");
        CHECK(sh_param_count(prog) == 1, "compiled param count");
        sh_free(prog);
    }

    printf("[lisp_shader scaffold] %d failures\n", failures);
    return failures ? 1 : 0;
}
