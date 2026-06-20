// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

// S3 UNIT 1 -- structural tests for the lowerer (sh_lower).
//
// Strategy: compile a shader via sh_compile_string, lower it with sh_lower,
// then assert on the STRUCTURE of the emitted sh_chunk -- opcodes, operand
// vreg wiring, jump targets, nvregs, aux contents. We do NOT execute the chunk
// (sh_vm_run is still a stub). Correctness against the interpreter is the job
// of the S3-2 differential harness.
//
// Self-check applied to every lowered chunk:
//   - every instruction's dst/a/b/c (when not SH_VREG_NONE) is < nvregs
//   - every jump target is in [0, ncode)
//   - every aux range [aux_off, aux_off+aux_len) is within [0, naux)
//   - chunk->result < nvregs
//
// Run with: bash libs/lisp_shader/test/build-and-run.sh

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lisp.h"
#include "sh_bytecode.h"
#include "sh_internal.h"

// ---------------------------------------------------------------------------
// Test harness
// ---------------------------------------------------------------------------

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond, msg)                                              \
  do {                                                                \
    if (cond) {                                                       \
      g_pass++;                                                       \
      printf("  ok   [%d] %s\n", __LINE__, (msg));                   \
    } else {                                                          \
      g_fail++;                                                       \
      printf("  FAIL [%d] %s\n", __LINE__, (msg));                   \
    }                                                                 \
  } while (0)

// ---------------------------------------------------------------------------
// Self-check: verify every vreg reference and jump target in a chunk.
// Returns 1 if all checks pass, 0 on the first violation (prints diagnosis).
// ---------------------------------------------------------------------------

static int chunk_self_check(const sh_chunk *c) {
  if (!c) { printf("  [self_check] chunk is NULL\n"); return 0; }
  if (c->nvregs == 0 && c->ncode > 0) {
    printf("  [self_check] nvregs=0 but ncode=%u\n", c->ncode);
    return 0;
  }
  if (c->result != SH_VREG_NONE && c->result >= c->nvregs) {
    printf("  [self_check] result vreg %u >= nvregs %u\n",
           c->result, c->nvregs);
    return 0;
  }
  for (uint32_t i = 0; i < c->ncode; i++) {
    const sh_instr *ins = &c->code[i];
    sh_bc_op op = (sh_bc_op)ins->op;
    // Check vreg operands
#define CHK_VREG(fld) \
    if (ins->fld != SH_VREG_NONE && ins->fld >= c->nvregs) { \
      printf("  [self_check] pc=%u op=%u: " #fld "=%u >= nvregs=%u\n", \
             i, ins->op, ins->fld, c->nvregs); \
      return 0; \
    }
    CHK_VREG(dst)
    CHK_VREG(a)
    CHK_VREG(b)
    CHK_VREG(c)
#undef CHK_VREG
    // Jump targets: forward jumps may target ncode (past last instr = exit),
    // backward jumps must target a valid instruction index.
    if (op == SHB_JMP || op == SHB_JMP_IFNOT) {
      if (ins->imm > c->ncode) {
        printf("  [self_check] pc=%u: jump target %u > ncode %u\n",
               i, ins->imm, c->ncode);
        return 0;
      }
    }
    // Aux ranges
    if (ins->aux_len > 0) {
      if ((uint64_t)ins->aux_off + ins->aux_len > (uint64_t)c->naux) {
        printf("  [self_check] pc=%u: aux[%u..+%u) out of naux=%u\n",
               i, ins->aux_off, ins->aux_len, c->naux);
        return 0;
      }
    }
  }
  return 1;
}

// ---------------------------------------------------------------------------
// Helper: compile a shader string and lower it.
// Returns the chunk (caller owns it; free with sh_chunk_free).
// On any failure prints an error and returns NULL.
// ---------------------------------------------------------------------------

static sh_chunk *compile_and_lower(const char *src, const sh_prim_set *prims) {
  sh_program *p = NULL;
  sh_error err = {0};
  sh_status s = sh_compile_string(src, prims, 0, &p, &err);
  if (s != SH_OK) {
    printf("  [compile] FAILED: %s\n", err.msg);
    return NULL;
  }
  sh_chunk *c = NULL;
  err = (sh_error){0};
  s = sh_lower(p, &c, &err);
  sh_free(p);
  if (s != SH_OK) {
    printf("  [lower] FAILED: %s\n", err.msg);
    return NULL;
  }
  return c;
}

// Find the first instruction with the given opcode; returns its pc or -1.
static int find_op(const sh_chunk *c, sh_bc_op op) {
  for (uint32_t i = 0; i < c->ncode; i++)
    if ((sh_bc_op)c->code[i].op == op) return (int)i;
  return -1;
}

// Count instructions with the given opcode.
static uint32_t count_op(const sh_chunk *c, sh_bc_op op) {
  uint32_t n = 0;
  for (uint32_t i = 0; i < c->ncode; i++)
    if ((sh_bc_op)c->code[i].op == op) n++;
  return n;
}

// ---------------------------------------------------------------------------
// Test 1: identity shader (u32 param passthrough)
// ---------------------------------------------------------------------------
static void test_identity(void) {
  printf("--- 1. identity shader ---\n");
  sh_chunk *c = compile_and_lower(
    "(defshader id ((x u32)) -> u32 x)", NULL);
  CHECK(c != NULL, "lowers without error");
  if (!c) return;
  CHECK(chunk_self_check(c), "self-check: all vreg refs valid");
  // Must have exactly one PARAM instruction.
  CHECK(count_op(c, SHB_PARAM) == 1, "exactly 1 PARAM");
  // result vreg == the PARAM dst vreg.
  int p_idx = find_op(c, SHB_PARAM);
  if (p_idx >= 0) {
    CHECK(c->code[p_idx].imm == 0, "PARAM.imm = 0 (first param)");
    // The result might be the PARAM dst directly (if the slot MOV was
    // elided) or a slot vreg that was MOV'd from PARAM.
    CHECK(c->result < c->nvregs, "result vreg in range");
  }
  CHECK(c->nvregs >= 1, "nvregs >= 1");
  sh_chunk_free(c);
}

// ---------------------------------------------------------------------------
// Test 2: integer constant
// ---------------------------------------------------------------------------
static void test_const(void) {
  printf("--- 2. constant ---\n");
  // Integer literals default to i64; use -> i64 to keep it a bare CONST.
  sh_chunk *c = compile_and_lower(
    "(defshader k () -> i64 42)", NULL);
  CHECK(c != NULL, "lowers without error");
  if (!c) return;
  CHECK(chunk_self_check(c), "self-check");
  CHECK(count_op(c, SHB_CONST) == 1, "exactly 1 CONST");
  int k_idx = find_op(c, SHB_CONST);
  if (k_idx >= 0) {
    CHECK(c->code[k_idx].sub == 0, "CONST.sub=0 (int literal)");
    CHECK(c->code[k_idx].imm64 == 42, "CONST.imm64 = 42");
    // Integer literal gets kind = I64 (default scalar int kind)
    CHECK(c->code[k_idx].kind == (uint8_t)SH_K_I64, "CONST.kind = I64");
  }
  sh_chunk_free(c);
}

// ---------------------------------------------------------------------------
// Test 3: arithmetic (BINOP wiring)
// ---------------------------------------------------------------------------
static void test_binop(void) {
  printf("--- 3. arithmetic BINOP ---\n");
  sh_chunk *c = compile_and_lower(
    "(defshader add ((a u32)(b u32)) -> u32 (+ a b))", NULL);
  CHECK(c != NULL, "lowers without error");
  if (!c) return;
  CHECK(chunk_self_check(c), "self-check");
  CHECK(count_op(c, SHB_PARAM) == 2, "2 PARAM instructions");
  CHECK(count_op(c, SHB_BINOP) == 1, "1 BINOP instruction");
  int bp_idx = find_op(c, SHB_BINOP);
  if (bp_idx >= 0) {
    const sh_instr *bp = &c->code[bp_idx];
    CHECK(bp->sub == (uint16_t)SH_BIN_ADD, "BINOP.sub = SH_BIN_ADD");
    CHECK(bp->kind == (uint8_t)SH_K_U32, "BINOP.kind = U32");
    CHECK(bp->a != SH_VREG_NONE, "BINOP.a valid");
    CHECK(bp->b != SH_VREG_NONE, "BINOP.b valid");
    CHECK(bp->a != bp->b, "BINOP.a != BINOP.b (different operand vregs)");
    CHECK(bp->dst == c->result, "BINOP.dst == chunk->result");
  }
  sh_chunk_free(c);
}

// ---------------------------------------------------------------------------
// Test 4: comparison (CMP kind = operand kind, result = bool)
// ---------------------------------------------------------------------------
static void test_cmp(void) {
  printf("--- 4. comparison CMP ---\n");
  sh_chunk *c = compile_and_lower(
    "(defshader lt ((a u32)(b u32)) -> bool (< a b))", NULL);
  CHECK(c != NULL, "lowers without error");
  if (!c) return;
  CHECK(chunk_self_check(c), "self-check");
  CHECK(count_op(c, SHB_CMP) == 1, "1 CMP instruction");
  int cmp_idx = find_op(c, SHB_CMP);
  if (cmp_idx >= 0) {
    const sh_instr *ci = &c->code[cmp_idx];
    CHECK(ci->sub == (uint16_t)SH_CMP_LT, "CMP.sub = SH_CMP_LT");
    // kind carries the OPERAND kind (u32), not the result kind (bool)
    CHECK(ci->kind == (uint8_t)SH_K_U32, "CMP.kind = operand kind U32");
    CHECK(ci->dst != SH_VREG_NONE, "CMP.dst valid");
  }
  sh_chunk_free(c);
}

// ---------------------------------------------------------------------------
// Test 5: IF -- JMP_IFNOT and JMP wiring, both arms write same result vreg
// ---------------------------------------------------------------------------
static void test_if(void) {
  printf("--- 5. IF control flow ---\n");
  sh_chunk *c = compile_and_lower(
    "(defshader max2 ((a u32)(b u32)) -> u32 (if (> a b) a b))", NULL);
  CHECK(c != NULL, "lowers without error");
  if (!c) return;
  CHECK(chunk_self_check(c), "self-check");

  // Must have exactly 1 JMP_IFNOT and 1 JMP
  CHECK(count_op(c, SHB_JMP_IFNOT) == 1, "1 JMP_IFNOT");
  CHECK(count_op(c, SHB_JMP) == 1, "1 JMP");

  int jif_idx = find_op(c, SHB_JMP_IFNOT);
  int jmp_idx = find_op(c, SHB_JMP);
  CHECK(jif_idx >= 0 && jmp_idx >= 0, "both jump instructions present");

  if (jif_idx >= 0 && jmp_idx >= 0) {
    const sh_instr *jif = &c->code[jif_idx];
    const sh_instr *jmp = &c->code[jmp_idx];

    // JMP_IFNOT must come before JMP (then-arm is between them)
    CHECK((uint32_t)jif_idx < (uint32_t)jmp_idx, "JMP_IFNOT before JMP");

    // JMP_IFNOT target must be past the JMP (== else-label pc)
    uint32_t else_pc = jif->imm;
    CHECK(else_pc > (uint32_t)jmp_idx, "JMP_IFNOT targets else-label");

    // JMP target must be >= else_pc (== end-label pc, at or after else block)
    uint32_t end_pc = jmp->imm;
    CHECK(end_pc >= else_pc, "JMP targets end-label (>= else-label)");
    CHECK(end_pc <= c->ncode, "JMP target in range (<=ncode)");  // ncode = past end ok

    // Both arms should write the same result vreg via MOV.
    // Find MOVs around the join point.
    uint32_t join_vreg = c->result;
    int found_then_mov = 0, found_else_mov = 0;
    for (uint32_t i = 0; i < c->ncode; i++) {
      if ((sh_bc_op)c->code[i].op == SHB_MOV &&
          c->code[i].dst == join_vreg) {
        if (i > (uint32_t)jif_idx && i < (uint32_t)jmp_idx)
          found_then_mov = 1;
        if (i > (uint32_t)jmp_idx)
          found_else_mov = 1;
      }
    }
    CHECK(found_then_mov, "then-arm writes result vreg via MOV");
    CHECK(found_else_mov, "else-arm writes result vreg via MOV");
  }
  sh_chunk_free(c);
}

// ---------------------------------------------------------------------------
// Test 6: LET binding
// ---------------------------------------------------------------------------
static void test_let(void) {
  printf("--- 6. LET binding ---\n");
  sh_chunk *c = compile_and_lower(
    "(defshader sq ((x u32)) -> u32 (let ((y (* x x))) y))", NULL);
  CHECK(c != NULL, "lowers without error");
  if (!c) return;
  CHECK(chunk_self_check(c), "self-check");
  // The MUL BINOP and at least one MOV (LET binding)
  CHECK(count_op(c, SHB_BINOP) >= 1, "BINOP for MUL");
  CHECK(count_op(c, SHB_MOV) >= 1, "MOV for LET binding");
  CHECK(c->nvregs >= 2, "nvregs >= 2 (param + let slot + result)");
  sh_chunk_free(c);
}

// ---------------------------------------------------------------------------
// Test 7: LOOP (named-let) -- backward JMP, induction MOVs, parallel RECUR
// ---------------------------------------------------------------------------
static void test_loop_sum(void) {
  printf("--- 7. LOOP named-let (sum 0..9) ---\n");
  const char *src =
    "(defshader sum10 () -> i64"
    " (let loop ((i 0) (acc 0))"
    "   (if (>= i 10) acc (loop (+ i 1) (+ acc i)))))";
  sh_chunk *c = compile_and_lower(src, NULL);
  CHECK(c != NULL, "lowers without error");
  if (!c) return;
  CHECK(chunk_self_check(c), "self-check");

  // Must have a backward JMP (back-edge to loop header).
  // The JMP_IFNOT is the loop exit test; its target is the recur arm.
  // The backward JMP is the recur's back-edge (target < its own pc).
  int found_backward_jmp = 0;
  for (uint32_t i = 0; i < c->ncode; i++) {
    if ((sh_bc_op)c->code[i].op == SHB_JMP && c->code[i].imm < i)
      found_backward_jmp = 1;
  }
  CHECK(found_backward_jmp, "backward JMP for loop back-edge");

  // Induction vars: 2 vars (i, acc) -> at least 2 MOVs for init
  CHECK(count_op(c, SHB_MOV) >= 2, "at least 2 MOVs (induction var inits)");

  // The parallel recur: 2 new induction values + 2 MOVs into induction slots.
  // Total MOVs include: 2 induction inits + 2 recur parallel-assign + IF joins.
  // Just verify the backward JMP and MOV count is sane.
  CHECK(c->nvregs >= 4, "nvregs covers induction vars + tmps + result");

  sh_chunk_free(c);
}

// ---------------------------------------------------------------------------
// Test 8: RECUR parallel-assignment semantics
// Use a proper bounded loop (counter i) that also carries two induction vars
// (a, b) that get swapped. The verifier requires the exit test to be a CMP
// on i, and the recur must advance i by a constant positive step.
// ---------------------------------------------------------------------------
static void test_loop_swap(void) {
  printf("--- 8. RECUR parallel assignment (swap) ---\n");
  // Loop: 2 iterations, returning the final 'a' value.
  // i=0, a=10, b=20 -> recur(1, b, a) -> i=1, a=20, b=10 -> exit: a (=20)
  // The recur (loop (+ i 1) b a) evaluates new_a=b, new_b=a into temps first
  // (parallel), then MOVs into the induction slots. If not parallel, new_b
  // would see the already-overwritten slot_a instead of the original a.
  // Use u32 for all vars to match the bounded-loop verifier's type requirements.
  const char *src =
    "(defshader swap () -> u32"
    " (let loop ((i 0) (a (u32 10)) (b (u32 20)))"
    "   (if (>= i 2)"
    "     a"
    "     (loop (+ i 1) b a))))";
  sh_chunk *c = compile_and_lower(src, NULL);
  CHECK(c != NULL, "lowers without error");
  if (!c) return;
  CHECK(chunk_self_check(c), "self-check");
  // 3 induction vars -> 3 init MOVs. Recur has 3 new args -> 3 MOVs.
  // Plus IF join MOVs. Total >= 6.
  CHECK(count_op(c, SHB_MOV) >= 6, ">= 6 MOVs (3 init + 3 recur parallel)");
  // Backward JMP for back-edge
  int found_backward = 0;
  for (uint32_t i = 0; i < c->ncode; i++) {
    if ((sh_bc_op)c->code[i].op == SHB_JMP && c->code[i].imm < i)
      found_backward = 1;
  }
  CHECK(found_backward, "backward JMP for loop back-edge");
  sh_chunk_free(c);
}

// ---------------------------------------------------------------------------
// Test 9: REGION_LOAD emits SHB_RLOAD with correct elem kind
// ---------------------------------------------------------------------------
static void test_region_load(void) {
  printf("--- 9. REGION_LOAD elem kind ---\n");
  sh_chunk *c = compile_and_lower(
    "(defshader rload ((buf (bytes u32)) (i u32)) -> u32"
    " (region-ref buf i))", NULL);
  CHECK(c != NULL, "lowers without error");
  if (!c) return;
  CHECK(chunk_self_check(c), "self-check");
  CHECK(count_op(c, SHB_RLOAD) == 1, "1 RLOAD");
  int rl_idx = find_op(c, SHB_RLOAD);
  if (rl_idx >= 0) {
    const sh_instr *ri = &c->code[rl_idx];
    CHECK(ri->kind == (uint8_t)SH_K_U32, "RLOAD.kind = U32 (elem kind)");
    CHECK(ri->a != SH_VREG_NONE, "RLOAD.a = region vreg");
    CHECK(ri->b != SH_VREG_NONE, "RLOAD.b = index vreg");
  }
  sh_chunk_free(c);
}

// ---------------------------------------------------------------------------
// Test 10: REGION_STORE emits SHB_RSTORE
// ---------------------------------------------------------------------------
static void test_region_store(void) {
  printf("--- 10. REGION_STORE ---\n");
  sh_chunk *c = compile_and_lower(
    "(defshader rstore ((buf (bytes-mut u8)) (i u32)) -> u8"
    " (region-set! buf i 42))", NULL);
  CHECK(c != NULL, "lowers without error");
  if (!c) return;
  CHECK(chunk_self_check(c), "self-check");
  CHECK(count_op(c, SHB_RSTORE) == 1, "1 RSTORE");
  int rs_idx = find_op(c, SHB_RSTORE);
  if (rs_idx >= 0) {
    const sh_instr *ri = &c->code[rs_idx];
    CHECK(ri->kind == (uint8_t)SH_K_U8, "RSTORE.kind = U8");
    CHECK(ri->a != SH_VREG_NONE, "RSTORE.a = region");
    CHECK(ri->b != SH_VREG_NONE, "RSTORE.b = index");
    CHECK(ri->c != SH_VREG_NONE, "RSTORE.c = value");
  }
  sh_chunk_free(c);
}

// ---------------------------------------------------------------------------
// Test 11: REGION_LEN emits SHB_RLEN (result kind = U32)
// ---------------------------------------------------------------------------
static void test_region_len(void) {
  printf("--- 11. REGION_LEN ---\n");
  sh_chunk *c = compile_and_lower(
    "(defshader rlen ((buf (bytes u32))) -> u32"
    " (region-len buf))", NULL);
  CHECK(c != NULL, "lowers without error");
  if (!c) return;
  CHECK(chunk_self_check(c), "self-check");
  CHECK(count_op(c, SHB_RLEN) == 1, "1 RLEN");
  int rl_idx = find_op(c, SHB_RLEN);
  if (rl_idx >= 0) {
    CHECK(c->code[rl_idx].kind == (uint8_t)SH_K_U32, "RLEN.kind = U32");
    CHECK(c->code[rl_idx].a != SH_VREG_NONE, "RLEN.a = region vreg");
  }
  sh_chunk_free(c);
}

// ---------------------------------------------------------------------------
// Test 12: CALL primitive -- args in aux
// ---------------------------------------------------------------------------

static sh_value prim_add3(const sh_value *args, uint32_t argc) {
  (void)argc;
  return sh_val_u32((uint32_t)args[0].u + (uint32_t)args[1].u +
                    (uint32_t)args[2].u);
}

static void test_call(void) {
  printf("--- 12. CALL primitive ---\n");
  sh_type u32t = sh_type_scalar(SH_K_U32);
  sh_prim add3_prim = {
    .name = "add3",
    .ret  = u32t,
    .nparams = 3,
    .params  = {u32t, u32t, u32t},
    .fn = prim_add3,
  };
  sh_prim_set prims = {&add3_prim, 1};

  sh_chunk *c = compile_and_lower(
    "(defshader t ((x u32)(y u32)(z u32)) -> u32 (add3 x y z))",
    &prims);
  CHECK(c != NULL, "lowers without error");
  if (!c) return;
  CHECK(chunk_self_check(c), "self-check");
  CHECK(count_op(c, SHB_CALL) == 1, "1 CALL");
  int call_idx = find_op(c, SHB_CALL);
  if (call_idx >= 0) {
    const sh_instr *ci = &c->code[call_idx];
    CHECK(ci->imm == 0, "CALL.imm = 0 (prim index 0)");
    CHECK(ci->aux_len == 3, "CALL.aux_len = 3 (three args)");
    // Aux must contain 3 valid vreg indices
    if (ci->aux_len == 3) {
      uint32_t a0 = c->aux[ci->aux_off + 0];
      uint32_t a1 = c->aux[ci->aux_off + 1];
      uint32_t a2 = c->aux[ci->aux_off + 2];
      CHECK(a0 < c->nvregs && a1 < c->nvregs && a2 < c->nvregs,
            "all call arg vregs in range");
      // The three args should be distinct vregs (x, y, z are different params)
      CHECK(a0 != a1 && a1 != a2 && a0 != a2,
            "call arg vregs are distinct");
    }
  }
  sh_chunk_free(c);
}

// ---------------------------------------------------------------------------
// Test 13: VSPLAT emits SHB_VSPLAT with lanes/kind
// ---------------------------------------------------------------------------
static void test_vsplat(void) {
  printf("--- 13. VSPLAT ---\n");
  sh_chunk *c = compile_and_lower(
    "(defshader sp ((x f32)) -> vec4 (splat x))", NULL);
  CHECK(c != NULL, "lowers without error");
  if (!c) return;
  CHECK(chunk_self_check(c), "self-check");
  CHECK(count_op(c, SHB_VSPLAT) == 1, "1 VSPLAT");
  int vs_idx = find_op(c, SHB_VSPLAT);
  if (vs_idx >= 0) {
    const sh_instr *vi = &c->code[vs_idx];
    CHECK(vi->kind  == (uint8_t)SH_K_F32, "VSPLAT.kind = F32");
    CHECK(vi->lanes == 4, "VSPLAT.lanes = 4");
    CHECK(vi->a != SH_VREG_NONE, "VSPLAT.a = scalar source vreg");
  }
  sh_chunk_free(c);
}

// ---------------------------------------------------------------------------
// Test 14: VBINOP emits SHB_VBINOP with correct sub/kind/lanes
// ---------------------------------------------------------------------------
static void test_vbinop(void) {
  printf("--- 14. VBINOP ---\n");
  sh_chunk *c = compile_and_lower(
    "(defshader vadd ((a f32x4)(b f32x4)) -> f32x4 (+ a b))", NULL);
  CHECK(c != NULL, "lowers without error");
  if (!c) return;
  CHECK(chunk_self_check(c), "self-check");
  CHECK(count_op(c, SHB_VBINOP) == 1, "1 VBINOP");
  int vb_idx = find_op(c, SHB_VBINOP);
  if (vb_idx >= 0) {
    const sh_instr *vi = &c->code[vb_idx];
    CHECK(vi->sub   == (uint16_t)SH_BIN_ADD, "VBINOP.sub = ADD");
    CHECK(vi->kind  == (uint8_t)SH_K_F32, "VBINOP.kind = F32 (lane kind)");
    CHECK(vi->lanes == 4, "VBINOP.lanes = 4");
    CHECK(vi->a != SH_VREG_NONE, "VBINOP.a valid");
    CHECK(vi->b != SH_VREG_NONE, "VBINOP.b valid");
  }
  sh_chunk_free(c);
}

// ---------------------------------------------------------------------------
// Test 15: VCMP emits SHB_VCMP with operand lane kind
// ---------------------------------------------------------------------------
static void test_vcmp(void) {
  printf("--- 15. VCMP ---\n");
  // Extract lane 0 of the compare result so the program has a scalar return.
  sh_chunk *c = compile_and_lower(
    "(defshader vcmp ((a f32x4)(b f32x4)) -> bool"
    " (lane (> a b) 0))", NULL);
  CHECK(c != NULL, "lowers without error");
  if (!c) return;
  CHECK(chunk_self_check(c), "self-check");
  CHECK(count_op(c, SHB_VCMP) == 1, "1 VCMP");
  int vc_idx = find_op(c, SHB_VCMP);
  if (vc_idx >= 0) {
    const sh_instr *vi = &c->code[vc_idx];
    // sub = SH_CMP_GT
    CHECK(vi->sub   == (uint16_t)SH_CMP_GT, "VCMP.sub = GT");
    // kind = operand lane kind = F32
    CHECK(vi->kind  == (uint8_t)SH_K_F32, "VCMP.kind = operand lane kind F32");
    CHECK(vi->lanes == 4, "VCMP.lanes = 4");
  }
  sh_chunk_free(c);
}

// ---------------------------------------------------------------------------
// Test 16: VSHUFFLE emits SHB_VSHUFFLE with indices in aux
// ---------------------------------------------------------------------------
static void test_vshuffle(void) {
  printf("--- 16. VSHUFFLE ---\n");
  sh_chunk *c = compile_and_lower(
    "(defshader vrev ((v f32x4)) -> f32x4 (shuffle v 3 2 1 0))", NULL);
  CHECK(c != NULL, "lowers without error");
  if (!c) return;
  CHECK(chunk_self_check(c), "self-check");
  CHECK(count_op(c, SHB_VSHUFFLE) == 1, "1 VSHUFFLE");
  int sh_idx = find_op(c, SHB_VSHUFFLE);
  if (sh_idx >= 0) {
    const sh_instr *si = &c->code[sh_idx];
    CHECK(si->aux_len == 4, "VSHUFFLE.aux_len = 4");
    if (si->aux_len == 4) {
      CHECK(c->aux[si->aux_off + 0] == 3, "shuffle index 0 = 3");
      CHECK(c->aux[si->aux_off + 1] == 2, "shuffle index 1 = 2");
      CHECK(c->aux[si->aux_off + 2] == 1, "shuffle index 2 = 1");
      CHECK(c->aux[si->aux_off + 3] == 0, "shuffle index 3 = 0");
    }
  }
  sh_chunk_free(c);
}

// ---------------------------------------------------------------------------
// Test 17: VREDUCE emits SHB_VREDUCE with lane kind from input
// ---------------------------------------------------------------------------
static void test_vreduce(void) {
  printf("--- 17. VREDUCE ---\n");
  sh_chunk *c = compile_and_lower(
    "(defshader vsum ((v f32x4)) -> f32 (vreduce-add v))", NULL);
  CHECK(c != NULL, "lowers without error");
  if (!c) return;
  CHECK(chunk_self_check(c), "self-check");
  CHECK(count_op(c, SHB_VREDUCE) == 1, "1 VREDUCE");
  int vr_idx = find_op(c, SHB_VREDUCE);
  if (vr_idx >= 0) {
    const sh_instr *vi = &c->code[vr_idx];
    CHECK(vi->sub   == (uint16_t)SH_RED_ADD, "VREDUCE.sub = ADD");
    CHECK(vi->kind  == (uint8_t)SH_K_F32,   "VREDUCE.kind = F32 (lane kind)");
    CHECK(vi->lanes == 4, "VREDUCE.lanes = 4 (input lane count)");
    CHECK(vi->a != SH_VREG_NONE, "VREDUCE.a = input vector vreg");
  }
  sh_chunk_free(c);
}

// ---------------------------------------------------------------------------
// Test 18: dot product (VREDUCE with SH_RED_DOT uses two inputs)
// ---------------------------------------------------------------------------
static void test_dot(void) {
  printf("--- 18. VREDUCE dot product ---\n");
  sh_chunk *c = compile_and_lower(
    "(defshader vdot ((a f32x4)(b f32x4)) -> f32 (dot a b))", NULL);
  CHECK(c != NULL, "lowers without error");
  if (!c) return;
  CHECK(chunk_self_check(c), "self-check");
  int vr_idx = find_op(c, SHB_VREDUCE);
  if (vr_idx >= 0) {
    const sh_instr *vi = &c->code[vr_idx];
    CHECK(vi->sub == (uint16_t)SH_RED_DOT, "VREDUCE.sub = DOT");
    CHECK(vi->b != SH_VREG_NONE, "VREDUCE.b = second vector vreg");
    CHECK(vi->a != vi->b, "VREDUCE.a != b (different input vregs)");
  }
  sh_chunk_free(c);
}

// ---------------------------------------------------------------------------
// Test 19: VLANE emits SHB_VLANE with constant lane index in imm
// ---------------------------------------------------------------------------
static void test_vlane(void) {
  printf("--- 19. VLANE ---\n");
  sh_chunk *c = compile_and_lower(
    "(defshader vl2 ((v f32x4)) -> f32 (lane v 2))", NULL);
  CHECK(c != NULL, "lowers without error");
  if (!c) return;
  CHECK(chunk_self_check(c), "self-check");
  CHECK(count_op(c, SHB_VLANE) == 1, "1 VLANE");
  int vl_idx = find_op(c, SHB_VLANE);
  if (vl_idx >= 0) {
    const sh_instr *vi = &c->code[vl_idx];
    CHECK(vi->imm  == 2, "VLANE.imm = 2 (lane index)");
    CHECK(vi->kind == (uint8_t)SH_K_F32, "VLANE.kind = F32 (lane kind)");
    CHECK(vi->a != SH_VREG_NONE, "VLANE.a = vector vreg");
  }
  sh_chunk_free(c);
}

// ---------------------------------------------------------------------------
// Test 20: UNOP -- NEG and NOT
// ---------------------------------------------------------------------------
static void test_unop(void) {
  printf("--- 20. UNOP (neg / not) ---\n");
  {
    sh_chunk *c = compile_and_lower(
      "(defshader neg ((x i64)) -> i64 (- x))", NULL);
    CHECK(c != NULL, "neg lowers");
    if (c) {
      CHECK(chunk_self_check(c), "self-check neg");
      CHECK(count_op(c, SHB_UNOP) == 1, "1 UNOP for neg");
      int u_idx = find_op(c, SHB_UNOP);
      if (u_idx >= 0)
        CHECK(c->code[u_idx].sub == (uint16_t)SH_UN_NEG, "UNOP.sub = NEG");
      sh_chunk_free(c);
    }
  }
  {
    sh_chunk *c = compile_and_lower(
      "(defshader nt ((x bool)) -> bool (not x))", NULL);
    CHECK(c != NULL, "not lowers");
    if (c) {
      CHECK(chunk_self_check(c), "self-check not");
      CHECK(count_op(c, SHB_UNOP) == 1, "1 UNOP for not");
      int u_idx = find_op(c, SHB_UNOP);
      if (u_idx >= 0)
        CHECK(c->code[u_idx].sub == (uint16_t)SH_UN_NOT, "UNOP.sub = NOT");
      sh_chunk_free(c);
    }
  }
}

// ---------------------------------------------------------------------------
// Test 21: UNOP CVT -- cast carries target kind
// ---------------------------------------------------------------------------
static void test_cvt(void) {
  printf("--- 21. UNOP CVT (cast) ---\n");
  sh_chunk *c = compile_and_lower(
    "(defshader cvt ((x u32)) -> f32 (f32 x))", NULL);
  CHECK(c != NULL, "cvt lowers");
  if (!c) return;
  CHECK(chunk_self_check(c), "self-check");
  int u_idx = find_op(c, SHB_UNOP);
  CHECK(u_idx >= 0, "UNOP for CVT exists");
  if (u_idx >= 0) {
    CHECK(c->code[u_idx].sub  == (uint16_t)SH_UN_CVT, "UNOP.sub = CVT");
    CHECK(c->code[u_idx].kind == (uint8_t)SH_K_F32,   "UNOP.kind = F32 (target)");
  }
  sh_chunk_free(c);
}

// ---------------------------------------------------------------------------
// Test 22: nested let* (multiple LET levels)
// ---------------------------------------------------------------------------
static void test_nested_let(void) {
  printf("--- 22. nested let* ---\n");
  sh_chunk *c = compile_and_lower(
    "(defshader deep ((x u32)) -> u32"
    " (let* ((a (+ x 1)) (b (+ a 1)) (c (+ b 1))) c))", NULL);
  CHECK(c != NULL, "nested let* lowers");
  if (!c) return;
  CHECK(chunk_self_check(c), "self-check");
  // Three BINOPs for the three additions
  CHECK(count_op(c, SHB_BINOP) == 3, "3 BINOPs for let* chain");
  // nvregs must cover: param + 3 local slots + 3 init result vregs + lets +
  // result -- just check it's sane
  CHECK(c->nvregs >= 4, "nvregs >= 4 for nested let*");
  sh_chunk_free(c);
}

// ---------------------------------------------------------------------------
// Test 23: region-sum loop (exercises RLEN, RLOAD, LOOP, RECUR together)
// ---------------------------------------------------------------------------
static void test_region_sum_loop(void) {
  printf("--- 23. region-sum loop ---\n");
  const char *src =
    "(defshader sum ((buf (bytes u32))) -> u32"
    " (let loop ((i 0) (acc 0))"
    "   (if (>= i (region-len buf))"
    "     acc"
    "     (loop (+ i 1) (+ acc (region-ref buf i))))))";
  sh_chunk *c = compile_and_lower(src, NULL);
  CHECK(c != NULL, "region-sum loop lowers");
  if (!c) return;
  CHECK(chunk_self_check(c), "self-check");

  CHECK(count_op(c, SHB_RLEN)  >= 1, "at least 1 RLEN");
  CHECK(count_op(c, SHB_RLOAD) >= 1, "at least 1 RLOAD");
  // Backward JMP for loop back-edge
  int found_backward = 0;
  for (uint32_t i = 0; i < c->ncode; i++) {
    if ((sh_bc_op)c->code[i].op == SHB_JMP && c->code[i].imm < i)
      found_backward = 1;
  }
  CHECK(found_backward, "backward JMP for loop back-edge");

  // RLOAD elem kind must be U32
  int rl_idx = find_op(c, SHB_RLOAD);
  if (rl_idx >= 0)
    CHECK(c->code[rl_idx].kind == (uint8_t)SH_K_U32,
          "RLOAD.kind = U32 (elem kind)");
  sh_chunk_free(c);
}

// ---------------------------------------------------------------------------
// Test 24: bool CONST (sub=2)
// ---------------------------------------------------------------------------
static void test_bool_const(void) {
  printf("--- 24. bool CONST sub=2 ---\n");
  sh_chunk *c = compile_and_lower(
    "(defshader bt () -> u32 (if #t (u32 1) (u32 0)))", NULL);
  CHECK(c != NULL, "bool-const lowers");
  if (!c) return;
  CHECK(chunk_self_check(c), "self-check");
  // Find a CONST with sub=2 (bool)
  int found_bool_const = 0;
  for (uint32_t i = 0; i < c->ncode; i++) {
    if ((sh_bc_op)c->code[i].op == SHB_CONST && c->code[i].sub == 2)
      found_bool_const = 1;
  }
  CHECK(found_bool_const, "CONST with sub=2 (bool) present");
  sh_chunk_free(c);
}

// ---------------------------------------------------------------------------
// Test 25: float CONST (sub=1)
// ---------------------------------------------------------------------------
static void test_float_const(void) {
  printf("--- 25. float CONST sub=1 ---\n");
  sh_chunk *c = compile_and_lower(
    "(defshader fc ((x f32)) -> f32 (+ x 1.0))", NULL);
  CHECK(c != NULL, "float-const lowers");
  if (!c) return;
  CHECK(chunk_self_check(c), "self-check");
  int found_float_const = 0;
  for (uint32_t i = 0; i < c->ncode; i++) {
    if ((sh_bc_op)c->code[i].op == SHB_CONST && c->code[i].sub == 1)
      found_float_const = 1;
  }
  CHECK(found_float_const, "CONST with sub=1 (float bits) present");
  sh_chunk_free(c);
}

// ---------------------------------------------------------------------------
// Test 26: nvregs covers ALL written vregs (no instruction writes dst >= nvregs)
// This is guaranteed by chunk_self_check, but we call it once more as a
// named test so the count shows up clearly.
// ---------------------------------------------------------------------------
static void test_nvregs_covers_all(void) {
  printf("--- 26. nvregs covers all written vregs ---\n");
  // Use a complex shader to exercise many vregs.
  const char *src =
    "(defshader complex ((a u32)(b u32)) -> u32"
    " (let* ((x (+ a b))"
    "        (y (* x 2))"
    "        (z (if (> y 100) y x)))"
    "  z))";
  sh_chunk *c = compile_and_lower(src, NULL);
  CHECK(c != NULL, "complex shader lowers");
  if (!c) return;
  // chunk_self_check already verifies this; fail here if it finds any issue.
  CHECK(chunk_self_check(c), "self-check: no vreg >= nvregs");
  sh_chunk_free(c);
}

// ---------------------------------------------------------------------------
// Test 27: sh_chunk_free(NULL) is safe
// ---------------------------------------------------------------------------
static void test_free_null(void) {
  printf("--- 27. sh_chunk_free(NULL) is safe ---\n");
  sh_chunk_free(NULL);
  CHECK(1, "sh_chunk_free(NULL) did not crash");
}

// ---------------------------------------------------------------------------
// Test 28: error on unverified program
// ---------------------------------------------------------------------------
static void test_unverified_rejected(void) {
  printf("--- 28. unverified program rejected ---\n");
  sh_program fake;
  memset(&fake, 0, sizeof(fake));
  // verified = false (already zero)
  sh_chunk *c = NULL;
  sh_error err = {0};
  sh_status s = sh_lower(&fake, &c, &err);
  CHECK(s != SH_OK, "sh_lower rejects unverified program");
  CHECK(c == NULL, "out is NULL on error");
  sh_chunk_free(c);
}

// ---------------------------------------------------------------------------
// Test 29: IF with vector arms (scalar bool cond) -- exercises vector IF
// ---------------------------------------------------------------------------
static void test_vector_if(void) {
  printf("--- 29. IF with vector arms (scalar bool cond) ---\n");
  // IF node: scalar bool cond, vector then/else -- this is a valid IF (not
  // VSELECT; VSELECT has no frontend syntax and is IR-only for future use).
  sh_chunk *c = compile_and_lower(
    "(defshader ifvec ((a f32x4)(b f32x4)) -> f32x4"
    " (if (< (lane a 0) (lane b 0)) a b))", NULL);
  CHECK(c != NULL, "vector IF lowers");
  if (!c) return;
  CHECK(chunk_self_check(c), "self-check");
  // Must have JMP_IFNOT for the IF
  CHECK(count_op(c, SHB_JMP_IFNOT) == 1, "1 JMP_IFNOT for vector IF");
  // Must have VLANE (extracting lane 0 for the scalar cond)
  CHECK(count_op(c, SHB_VLANE) >= 2, ">= 2 VLANE (lane a 0, lane b 0)");
  // Must have CMP for the <
  CHECK(count_op(c, SHB_CMP) == 1, "1 CMP for < on scalars");
  sh_chunk_free(c);
}

// ---------------------------------------------------------------------------
// Test 30: chunk->name matches program name
// ---------------------------------------------------------------------------
static void test_chunk_metadata(void) {
  printf("--- 30. chunk metadata ---\n");
  sh_chunk *c = compile_and_lower(
    "(defshader myprog ((x u32)) -> u32 x)", NULL);
  CHECK(c != NULL, "metadata shader lowers");
  if (!c) return;
  CHECK(strcmp(c->name, "myprog") == 0, "chunk->name = 'myprog'");
  CHECK(c->nparams == 1, "chunk->nparams = 1");
  CHECK(c->params[0].kind == (uint8_t)SH_K_U32, "chunk->params[0].kind = U32");
  CHECK(c->ret.kind == (uint8_t)SH_K_U32, "chunk->ret.kind = U32");
  sh_chunk_free(c);
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(void) {
  (void)lisp_default_env();

  printf("[test_lower] Lowerer structural tests\n\n");

  test_identity();
  test_const();
  test_binop();
  test_cmp();
  test_if();
  test_let();
  test_loop_sum();
  test_loop_swap();
  test_region_load();
  test_region_store();
  test_region_len();
  test_call();
  test_vsplat();
  test_vbinop();
  test_vcmp();
  test_vshuffle();
  test_vreduce();
  test_dot();
  test_vlane();
  test_unop();
  test_cvt();
  test_nested_let();
  test_region_sum_loop();
  test_bool_const();
  test_float_const();
  test_nvregs_covers_all();
  test_free_null();
  test_unverified_rejected();
  test_vector_if();
  test_chunk_metadata();

  int total = g_pass + g_fail;
  printf("\n[test_lower] %s (%d/%d)\n",
         g_fail == 0 ? "ALL TESTS PASSED" : "SOME TESTS FAILED",
         g_pass, total);
  return g_fail ? 1 : 0;
}
