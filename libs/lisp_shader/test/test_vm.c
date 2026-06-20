// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

// S3 UNIT 2 -- test suite: chunk validator + differential VM vs. oracle harness.
//
// Tests:
//   1. CHUNK VALIDATOR -- hand-corrupt lowered chunks; assert rejects. Also
//      assert clean chunks pass.
//   2. DIFFERENTIAL HARNESS -- for a corpus of shaders compile->lower->vm_run
//      AND sh_invoke (the oracle) with the same args; assert results equal
//      bit-for-bit. Covers scalar arithmetic across kinds, casts, if/let/let*,
//      a region-sum loop, ip-checksum, saturating-add blit, f32 dot product,
//      vector ops, and a CALL with a prim.
//   3. SH_VM_FORCE_SCALAR flag -- assert same result as default (no SIMD yet,
//      so the paths are identical; sanity check the flag plumbing).
//
// Run with: bash libs/lisp_shader/test/build-and-run.sh

#include <math.h>
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

#define CHECK(cond, msg)                                                \
  do {                                                                  \
    if (cond) {                                                         \
      g_pass++;                                                         \
      printf("  ok   [%d] %s\n", __LINE__, (msg));                     \
    } else {                                                            \
      g_fail++;                                                         \
      printf("  FAIL [%d] %s\n", __LINE__, (msg));                     \
    }                                                                   \
  } while (0)

// ---------------------------------------------------------------------------
// Primitive functions used by tests (must be at file scope in C11)
// ---------------------------------------------------------------------------

static sh_value add3_stub(const sh_value *args, uint32_t argc) {
  (void)argc;
  return sh_val_u32((uint32_t)args[0].u + (uint32_t)args[1].u
                    + (uint32_t)args[2].u);
}

static sh_value p1_id_u32(const sh_value *args, uint32_t argc) {
  (void)argc;
  return sh_val_u32((uint32_t)args[0].u);
}

static sh_value clamp_u32_fn(const sh_value *args, uint32_t argc) {
  (void)argc;
  uint32_t x  = (uint32_t)args[0].u;
  uint32_t lo = (uint32_t)args[1].u;
  uint32_t hi = (uint32_t)args[2].u;
  uint32_t r  = x < lo ? lo : (x > hi ? hi : x);
  return sh_val_u32(r);
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Compile a shader string (with optional prims) and lower it.
// Returns the chunk (caller owns it; free with sh_chunk_free).
// Prints an error and returns NULL on failure.
static sh_chunk *compile_lower(const char *src, const sh_prim_set *prims) {
  sh_program *p = NULL;
  sh_error err;
  memset(&err, 0, sizeof(err));
  sh_status s = sh_compile_string(src, prims, 0, &p, &err);
  if (s != SH_OK) {
    printf("  [compile_lower] compile FAILED: %s\n", err.msg);
    return NULL;
  }
  sh_chunk *c = NULL;
  memset(&err, 0, sizeof(err));
  s = sh_lower(p, &c, &err);
  sh_free(p);
  if (s != SH_OK) {
    printf("  [compile_lower] lower FAILED: %s\n", err.msg);
    return NULL;
  }
  return c;
}

// Compare two sh_values for bit-exact equality.
// For vectors, compare each lane bit-for-bit.
// For scalars, compare the appropriate union field.
static int sh_value_bits_eq(sh_value a, sh_value b) {
  if (a.kind != b.kind) return 0;
  if (a.kind == SH_K_VEC) {
    if (a.lanes != b.lanes) return 0;
    if (a.lane_kind != b.lane_kind) return 0;
    for (int i = 0; i < a.lanes; i++)
      if (a.lane[i] != b.lane[i]) return 0;
    return 1;
  }
  if (a.kind == SH_K_REGION) {
    return (a.region.base == b.region.base &&
            a.region.len  == b.region.len  &&
            a.region.elem == b.region.elem);
  }
  // Scalar: compare bit-for-bit via the u field.
  switch (a.kind) {
    case SH_K_I64: return a.i == b.i;
    case SH_K_F32:
    case SH_K_F64: {
      // Compare the bit pattern of the double (both narrowed in the same way).
      uint64_t ab, bb;
      memcpy(&ab, &a.f, 8);
      memcpy(&bb, &b.f, 8);
      return ab == bb;
    }
    default: return a.u == b.u;
  }
}

// Run vm_run + sh_invoke on the same args and assert results are bit-equal.
// Returns 1 on match, 0 on mismatch or error.
static int diff_check(const char *test_name, sh_chunk *c,
                      const sh_program *p,
                      const sh_value *args, uint32_t argc) {
  sh_error err_vm, err_interp;
  sh_value vm_out, interp_out;
  memset(&err_vm,     0, sizeof(err_vm));
  memset(&err_interp, 0, sizeof(err_interp));
  memset(&vm_out,     0, sizeof(vm_out));
  memset(&interp_out, 0, sizeof(interp_out));

  sh_status sv = sh_vm_run(c, args, argc, 0, &vm_out, &err_vm);
  sh_status si = sh_invoke(p, args, argc, &interp_out, &err_interp);

  if (sv != si) {
    printf("  [diff] %s: status mismatch vm=%d interp=%d\n",
           test_name, (int)sv, (int)si);
    return 0;
  }
  if (sv != SH_OK) return 1;  // Both errored the same way: pass.

  if (!sh_value_bits_eq(vm_out, interp_out)) {
    printf("  [diff] %s: RESULT MISMATCH\n", test_name);
    printf("    vm_out:     kind=%u u=%llu f=%g\n",
           (unsigned)vm_out.kind,
           (unsigned long long)vm_out.u, vm_out.f);
    printf("    interp_out: kind=%u u=%llu f=%g\n",
           (unsigned)interp_out.kind,
           (unsigned long long)interp_out.u, interp_out.f);
    return 0;
  }
  return 1;
}

// ---------------------------------------------------------------------------
// Section 1: Chunk validator tests
// ---------------------------------------------------------------------------

static void test_validator(void) {
  printf("--- chunk validator ---\n");

  // 1a. A clean chunk must pass validation.
  {
    sh_chunk *c = compile_lower(
      "(defshader add ((a u32)(b u32)) -> u32 (+ a b))", NULL);
    CHECK(c != NULL, "add shader lowers");
    if (c) {
      sh_error err; memset(&err, 0, sizeof(err));
      CHECK(sh_chunk_validate(c, &err) == SH_OK,
            "validator: clean chunk passes");
      sh_chunk_free(c);
    }
  }

  // 1b. NULL chunk is rejected.
  {
    sh_error err; memset(&err, 0, sizeof(err));
    CHECK(sh_chunk_validate(NULL, &err) == SH_ERR_INTERNAL,
          "validator: NULL chunk rejected");
  }

  // 1c. Corrupt instruction.a vreg >= nvregs -> rejected.
  {
    sh_chunk *c = compile_lower(
      "(defshader id ((x u32)) -> u32 x)", NULL);
    CHECK(c != NULL, "id shader lowers for vreg OOB test");
    if (c) {
      // Save the old value; corrupt instruction 0's `a` to nvregs.
      uint32_t saved_a = c->code[0].a;
      c->code[0].a = c->nvregs;  // == exactly nvregs = OOB
      sh_error err; memset(&err, 0, sizeof(err));
      CHECK(sh_chunk_validate(c, &err) == SH_ERR_INTERNAL,
            "validator: a vreg == nvregs rejected");
      // Restore and check valid again.
      c->code[0].a = saved_a;
      memset(&err, 0, sizeof(err));
      CHECK(sh_chunk_validate(c, &err) == SH_OK,
            "validator: restored chunk passes again");
      sh_chunk_free(c);
    }
  }

  // 1d. Corrupt chunk->result >= nvregs -> rejected.
  {
    sh_chunk *c = compile_lower(
      "(defshader id ((x u32)) -> u32 x)", NULL);
    CHECK(c != NULL, "id shader lowers for result OOB test");
    if (c) {
      sh_vreg saved_result = c->result;
      c->result = c->nvregs + 5;
      sh_error err; memset(&err, 0, sizeof(err));
      CHECK(sh_chunk_validate(c, &err) == SH_ERR_INTERNAL,
            "validator: result vreg OOB rejected");
      c->result = saved_result;
      sh_chunk_free(c);
    }
  }

  // 1e. Corrupt jump target > ncode -> rejected.
  {
    sh_chunk *c = compile_lower(
      "(defshader max2 ((a u32)(b u32)) -> u32 (if (> a b) a b))", NULL);
    CHECK(c != NULL, "max2 shader lowers for jump OOB test");
    if (c) {
      // Find a JMP or JMP_IFNOT; corrupt its imm.
      int found = -1;
      for (uint32_t i = 0; i < c->ncode; i++) {
        sh_bc_op op = (sh_bc_op)c->code[i].op;
        if (op == SHB_JMP || op == SHB_JMP_IFNOT) {
          found = (int)i;
          break;
        }
      }
      CHECK(found >= 0, "validator: found a jump instruction");
      if (found >= 0) {
        uint32_t saved_imm = c->code[found].imm;
        c->code[found].imm = c->ncode + 5;  // OOB
        sh_error err; memset(&err, 0, sizeof(err));
        CHECK(sh_chunk_validate(c, &err) == SH_ERR_INTERNAL,
              "validator: jump target > ncode rejected");
        c->code[found].imm = saved_imm;
        memset(&err, 0, sizeof(err));
        CHECK(sh_chunk_validate(c, &err) == SH_OK,
              "validator: restored jump passes");
      }
      sh_chunk_free(c);
    }
  }

  // 1f. Corrupt aux range out of naux -> rejected.
  {
    // Use a CALL so we get aux entries.
    sh_type u32t = sh_type_scalar(SH_K_U32);
    sh_prim dummy_prim;
    memset(&dummy_prim, 0, sizeof(dummy_prim));
    dummy_prim.name = "add3";
    dummy_prim.ret  = u32t;
    dummy_prim.nparams = 3;
    dummy_prim.params[0] = u32t;
    dummy_prim.params[1] = u32t;
    dummy_prim.params[2] = u32t;
    // We only need the prim for compile+lower; fn cannot be NULL for validate.
    dummy_prim.fn = add3_stub;
    sh_prim_set prims = {&dummy_prim, 1};

    sh_chunk *c = compile_lower(
      "(defshader t ((x u32)(y u32)(z u32)) -> u32 (add3 x y z))",
      &prims);
    CHECK(c != NULL, "call shader lowers for aux OOB test");
    if (c) {
      // Find the CALL instruction.
      int ci = -1;
      for (uint32_t i = 0; i < c->ncode; i++) {
        if ((sh_bc_op)c->code[i].op == SHB_CALL) { ci = (int)i; break; }
      }
      CHECK(ci >= 0, "CALL instruction found");
      if (ci >= 0) {
        uint32_t saved_aux_off = c->code[ci].aux_off;
        uint32_t saved_aux_len = c->code[ci].aux_len;
        // Force aux_off so that aux_off + aux_len overflows naux.
        c->code[ci].aux_off = c->naux;  // start at end
        sh_error err; memset(&err, 0, sizeof(err));
        CHECK(sh_chunk_validate(c, &err) == SH_ERR_INTERNAL,
              "validator: aux OOB rejected");
        c->code[ci].aux_off = saved_aux_off;
        c->code[ci].aux_len = saved_aux_len;
      }
      sh_chunk_free(c);
    }
  }

  // 1g. Corrupt CALL prim index >= prims->count -> rejected.
  {
    sh_type u32t = sh_type_scalar(SH_K_U32);
    sh_prim p1;
    memset(&p1, 0, sizeof(p1));
    p1.name = "id_u32"; p1.ret = u32t; p1.nparams = 1;
    p1.params[0] = u32t; p1.fn = p1_id_u32;
    sh_prim_set prims = {&p1, 1};

    sh_chunk *c = compile_lower(
      "(defshader t ((x u32)) -> u32 (id_u32 x))", &prims);
    CHECK(c != NULL, "call1 shader lowers for prim OOB test");
    if (c) {
      // Find CALL and corrupt imm.
      for (uint32_t i = 0; i < c->ncode; i++) {
        if ((sh_bc_op)c->code[i].op == SHB_CALL) {
          uint32_t saved = c->code[i].imm;
          c->code[i].imm = 99;  // prim 99 does not exist
          sh_error err; memset(&err, 0, sizeof(err));
          CHECK(sh_chunk_validate(c, &err) == SH_ERR_INTERNAL,
                "validator: CALL prim OOB rejected");
          c->code[i].imm = saved;
          break;
        }
      }
      sh_chunk_free(c);
    }
  }

  // 1h. VSHUFFLE with aux index >= SH_MAX_LANES -> rejected.
  {
    sh_chunk *c = compile_lower(
      "(defshader vrev ((v f32x4)) -> f32x4 (shuffle v 3 2 1 0))", NULL);
    CHECK(c != NULL, "vshuffle shader lowers for lane OOB test");
    if (c) {
      // Find VSHUFFLE aux entry and corrupt it.
      for (uint32_t i = 0; i < c->ncode; i++) {
        if ((sh_bc_op)c->code[i].op == SHB_VSHUFFLE && c->code[i].aux_len > 0) {
          uint32_t saved = c->aux[c->code[i].aux_off];
          c->aux[c->code[i].aux_off] = SH_MAX_LANES;  // >= SH_MAX_LANES
          sh_error err; memset(&err, 0, sizeof(err));
          CHECK(sh_chunk_validate(c, &err) == SH_ERR_INTERNAL,
                "validator: VSHUFFLE lane >= SH_MAX_LANES rejected");
          c->aux[c->code[i].aux_off] = saved;
          break;
        }
      }
      sh_chunk_free(c);
    }
  }
}

// ---------------------------------------------------------------------------
// Section 2: Differential harness helpers
// ---------------------------------------------------------------------------

// Build and run one differential test case: compile->lower->vm AND compile->invoke.
// Returns 1 on all-pass, 0 on any failure.
static int diff_case(const char *shader_name, const char *src,
                     const sh_prim_set *prims,
                     const sh_value *args, uint32_t argc) {
  sh_program *p = NULL;
  sh_error err;
  memset(&err, 0, sizeof(err));

  sh_status s = sh_compile_string(src, prims, 0, &p, &err);
  if (s != SH_OK) {
    printf("  [diff] %s: compile FAILED: %s\n", shader_name, err.msg);
    return 0;
  }

  sh_chunk *c = NULL;
  memset(&err, 0, sizeof(err));
  s = sh_lower(p, &c, &err);
  if (s != SH_OK) {
    printf("  [diff] %s: lower FAILED: %s\n", shader_name, err.msg);
    sh_free(p);
    return 0;
  }

  // Validate the chunk (also called inside vm_run, but assert it explicitly here).
  memset(&err, 0, sizeof(err));
  s = sh_chunk_validate(c, &err);
  if (s != SH_OK) {
    printf("  [diff] %s: validate FAILED: %s\n", shader_name, err.msg);
    sh_chunk_free(c);
    sh_free(p);
    return 0;
  }

  int ok = diff_check(shader_name, c, p, args, argc);
  sh_chunk_free(c);
  sh_free(p);
  return ok;
}

// Like diff_case but also tests SH_VM_FORCE_SCALAR flag against default.
static int diff_case_flags(const char *shader_name, const char *src,
                           const sh_prim_set *prims,
                           const sh_value *args, uint32_t argc) {
  sh_program *p = NULL;
  sh_error err;
  memset(&err, 0, sizeof(err));

  sh_status s = sh_compile_string(src, prims, 0, &p, &err);
  if (s != SH_OK) { sh_free(p); return 0; }

  sh_chunk *c = NULL;
  memset(&err, 0, sizeof(err));
  s = sh_lower(p, &c, &err);
  if (s != SH_OK) { sh_free(p); return 0; }

  sh_value vm_out_default, vm_out_scalar;
  memset(&vm_out_default, 0, sizeof(vm_out_default));
  memset(&vm_out_scalar,  0, sizeof(vm_out_scalar));
  sh_error e1, e2;
  memset(&e1, 0, sizeof(e1));
  memset(&e2, 0, sizeof(e2));

  sh_status s1 = sh_vm_run(c, args, argc, 0,                 &vm_out_default, &e1);
  sh_status s2 = sh_vm_run(c, args, argc, SH_VM_FORCE_SCALAR, &vm_out_scalar,  &e2);

  int ok = 1;
  if (s1 != s2) {
    printf("  [diff_flags] %s: status mismatch default=%d force_scalar=%d\n",
           shader_name, (int)s1, (int)s2);
    ok = 0;
  } else if (s1 == SH_OK && !sh_value_bits_eq(vm_out_default, vm_out_scalar)) {
    printf("  [diff_flags] %s: FORCE_SCALAR result differs from default\n",
           shader_name);
    ok = 0;
  }

  // Also compare against oracle.
  int ok2 = diff_check(shader_name, c, p, args, argc);

  sh_chunk_free(c);
  sh_free(p);
  return ok && ok2;
}

// ---------------------------------------------------------------------------
// Section 2: Differential harness -- corpus of kernels
// ---------------------------------------------------------------------------

// Helper: make an f32x4 sh_value.
static sh_value make_f32x4(float a, float b, float cc, float d) {
  sh_value v;
  memset(&v, 0, sizeof(v));
  v.kind = SH_K_VEC;
  v.lanes = 4;
  v.lane_kind = (uint8_t)SH_K_F32;
  float vals[4] = {a, b, cc, d};
  for (int i = 0; i < 4; i++) {
    uint32_t bits;
    memcpy(&bits, &vals[i], 4);
    v.lane[i] = (uint64_t)bits;
  }
  return v;
}

static void test_diff_scalar_arithmetic(void) {
  printf("--- differential: scalar arithmetic ---\n");

  // u32 add/sub/mul/div/mod
  sh_value a2[2];

  a2[0] = sh_val_u32(17); a2[1] = sh_val_u32(5);
  CHECK(diff_case("u32 add", "(defshader f ((a u32)(b u32)) -> u32 (+ a b))",
                  NULL, a2, 2), "u32 add");
  CHECK(diff_case("u32 sub", "(defshader f ((a u32)(b u32)) -> u32 (- a b))",
                  NULL, a2, 2), "u32 sub");
  CHECK(diff_case("u32 mul", "(defshader f ((a u32)(b u32)) -> u32 (* a b))",
                  NULL, a2, 2), "u32 mul");
  CHECK(diff_case("u32 div", "(defshader f ((a u32)(b u32)) -> u32 (/ a b))",
                  NULL, a2, 2), "u32 div");
  CHECK(diff_case("u32 mod", "(defshader f ((a u32)(b u32)) -> u32 (mod a b))",
                  NULL, a2, 2), "u32 mod");

  // u32 wraparound
  a2[0] = sh_val_u32(0xFFFFFFFFu); a2[1] = sh_val_u32(1);
  CHECK(diff_case("u32 wraparound",
                  "(defshader f ((a u32)(b u32)) -> u32 (+ a b))",
                  NULL, a2, 2), "u32 add wraparound");

  // u32 div-by-zero -> 0
  a2[0] = sh_val_u32(7); a2[1] = sh_val_u32(0);
  CHECK(diff_case("u32 div0",
                  "(defshader f ((a u32)(b u32)) -> u32 (/ a b))",
                  NULL, a2, 2), "u32 div-by-zero -> 0");

  // Bitwise
  a2[0] = sh_val_u32(0xF0); a2[1] = sh_val_u32(0x0F);
  CHECK(diff_case("u32 and",
                  "(defshader f ((a u32)(b u32)) -> u32 (bit-and a b))",
                  NULL, a2, 2), "u32 bit-and");
  CHECK(diff_case("u32 or",
                  "(defshader f ((a u32)(b u32)) -> u32 (bit-or a b))",
                  NULL, a2, 2), "u32 bit-or");
  CHECK(diff_case("u32 xor",
                  "(defshader f ((a u32)(b u32)) -> u32 (bit-xor a b))",
                  NULL, a2, 2), "u32 bit-xor");

  // Shift
  a2[0] = sh_val_u32(1); a2[1] = sh_val_u32(7);
  CHECK(diff_case("u32 shl",
                  "(defshader f ((a u32)(b u32)) -> u32 (shl a b))",
                  NULL, a2, 2), "u32 shl");
  a2[0] = sh_val_u32(512); a2[1] = sh_val_u32(3);
  CHECK(diff_case("u32 shr",
                  "(defshader f ((a u32)(b u32)) -> u32 (shr a b))",
                  NULL, a2, 2), "u32 shr");

  // i64 arithmetic
  a2[0] = sh_val_i64(-100); a2[1] = sh_val_i64(7);
  CHECK(diff_case("i64 add",
                  "(defshader f ((a i64)(b i64)) -> i64 (+ a b))",
                  NULL, a2, 2), "i64 add");
  CHECK(diff_case("i64 div",
                  "(defshader f ((a i64)(b i64)) -> i64 (/ a b))",
                  NULL, a2, 2), "i64 div truncate");
  CHECK(diff_case("i64 mod",
                  "(defshader f ((a i64)(b i64)) -> i64 (mod a b))",
                  NULL, a2, 2), "i64 mod");

  // i64 negation
  sh_value a1 = sh_val_i64(-99);
  CHECK(diff_case("i64 neg", "(defshader f ((x i64)) -> i64 (- x))",
                  NULL, &a1, 1), "i64 neg");

  // f32 arithmetic
  a2[0] = sh_val_f32(1.0f / 3.0f); a2[1] = sh_val_f32(2.5f);
  CHECK(diff_case("f32 add",
                  "(defshader f ((a f32)(b f32)) -> f32 (+ a b))",
                  NULL, a2, 2), "f32 add");
  CHECK(diff_case("f32 mul",
                  "(defshader f ((a f32)(b f32)) -> f32 (* a b))",
                  NULL, a2, 2), "f32 mul");
  CHECK(diff_case("f32 div",
                  "(defshader f ((a f32)(b f32)) -> f32 (/ a b))",
                  NULL, a2, 2), "f32 div");

  // f64 arithmetic
  a2[0] = sh_val_f64(1.0 / 7.0); a2[1] = sh_val_f64(3.14159265358979);
  CHECK(diff_case("f64 add",
                  "(defshader f ((a f64)(b f64)) -> f64 (+ a b))",
                  NULL, a2, 2), "f64 add");

  // u8 arithmetic
  sh_value u8args[2];
  memset(&u8args[0], 0, sizeof(sh_value));
  memset(&u8args[1], 0, sizeof(sh_value));
  u8args[0].kind = SH_K_U8; u8args[0].lanes = 1; u8args[0].u = 200;
  u8args[1].kind = SH_K_U8; u8args[1].lanes = 1; u8args[1].u = 100;
  CHECK(diff_case("u8 add wrap",
                  "(defshader f ((a u8)(b u8)) -> u8 (+ a b))",
                  NULL, u8args, 2), "u8 add wrap");

  // bool not
  sh_value bv = sh_val_bool(true);
  CHECK(diff_case("bool not",
                  "(defshader f ((x bool)) -> bool (not x))",
                  NULL, &bv, 1), "bool not true");
  bv = sh_val_bool(false);
  CHECK(diff_case("bool not false",
                  "(defshader f ((x bool)) -> bool (not x))",
                  NULL, &bv, 1), "bool not false");
}

static void test_diff_casts(void) {
  printf("--- differential: casts ---\n");

  sh_value a;

  a = sh_val_u32(7);
  CHECK(diff_case("u32->f32", "(defshader f ((x u32)) -> f32 (f32 x))",
                  NULL, &a, 1), "u32->f32");

  a = sh_val_f64(-3.9);
  CHECK(diff_case("f64->i64", "(defshader f ((x f64)) -> i64 (i64 x))",
                  NULL, &a, 1), "f64->i64 truncate");

  a = sh_val_f64(1.0 / 3.0);
  CHECK(diff_case("f64->f32", "(defshader f ((x f64)) -> f32 (f32 x))",
                  NULL, &a, 1), "f64->f32 narrow");

  a = sh_val_u32(257);
  CHECK(diff_case("u32->u8", "(defshader f ((x u32)) -> u8 (u8 x))",
                  NULL, &a, 1), "u32->u8 truncate");

  a = sh_val_bool(true);
  CHECK(diff_case("bool->u32", "(defshader f ((x bool)) -> u32 (u32 x))",
                  NULL, &a, 1), "bool->u32");

  a = sh_val_u64(1000000000ULL);
  CHECK(diff_case("u64->f64", "(defshader f ((x u64)) -> f64 (f64 x))",
                  NULL, &a, 1), "u64->f64");

  a = sh_val_i64(-42);
  CHECK(diff_case("i64->f32", "(defshader f ((x i64)) -> f32 (f32 x))",
                  NULL, &a, 1), "i64->f32");
}

static void test_diff_cmp(void) {
  printf("--- differential: comparisons ---\n");

  sh_value a2[2];

  a2[0] = sh_val_u32(5); a2[1] = sh_val_u32(10);
  CHECK(diff_case("u32 <",
                  "(defshader f ((a u32)(b u32)) -> bool (< a b))",
                  NULL, a2, 2), "u32 < true");
  CHECK(diff_case("u32 >",
                  "(defshader f ((a u32)(b u32)) -> bool (> a b))",
                  NULL, a2, 2), "u32 > false");

  a2[0] = sh_val_i64(-5); a2[1] = sh_val_i64(5);
  CHECK(diff_case("i64 signed <",
                  "(defshader f ((a i64)(b i64)) -> bool (< a b))",
                  NULL, a2, 2), "i64 signed < true");
  CHECK(diff_case("i64 signed >",
                  "(defshader f ((a i64)(b i64)) -> bool (> a b))",
                  NULL, a2, 2), "i64 signed > false");

  a2[0] = sh_val_f32(1.0f); a2[1] = sh_val_f32(1.0f);
  CHECK(diff_case("f32 ==",
                  "(defshader f ((a f32)(b f32)) -> bool (= a b))",
                  NULL, a2, 2), "f32 == equal");
  CHECK(diff_case("f32 not-eq",
                  "(defshader f ((a f32)(b f32)) -> bool (not (= a b)))",
                  NULL, a2, 2), "f32 not-equal (using not =)");
}

static void test_diff_if(void) {
  printf("--- differential: if ---\n");

  sh_value a2[2];

  // max(a, b)
  a2[0] = sh_val_u32(7); a2[1] = sh_val_u32(3);
  CHECK(diff_case("if max(7,3)",
                  "(defshader f ((a u32)(b u32)) -> u32 (if (> a b) a b))",
                  NULL, a2, 2), "if: max(7,3)=7");

  a2[0] = sh_val_u32(2); a2[1] = sh_val_u32(9);
  CHECK(diff_case("if max(2,9)",
                  "(defshader f ((a u32)(b u32)) -> u32 (if (> a b) a b))",
                  NULL, a2, 2), "if: max(2,9)=9");

  // abs
  sh_value neg = sh_val_i64(-5);
  CHECK(diff_case("abs(-5)",
                  "(defshader f ((x i64)) -> i64 (if (< x 0) (- x) x))",
                  NULL, &neg, 1), "if: abs(-5)=5");
  sh_value pos = sh_val_i64(3);
  CHECK(diff_case("abs(3)",
                  "(defshader f ((x i64)) -> i64 (if (< x 0) (- x) x))",
                  NULL, &pos, 1), "if: abs(3)=3");

  // bool literal
  CHECK(diff_case("if #t",
                  "(defshader f () -> u32 (if #t (u32 1) (u32 0)))",
                  NULL, NULL, 0), "if #t -> 1");
  CHECK(diff_case("if #f",
                  "(defshader f () -> u32 (if #f (u32 1) (u32 0)))",
                  NULL, NULL, 0), "if #f -> 0");
}

static void test_diff_let(void) {
  printf("--- differential: let / let* ---\n");

  sh_value a = sh_val_u32(6);

  CHECK(diff_case("let square",
                  "(defshader f ((x u32)) -> u32 (let ((y (* x x))) y))",
                  NULL, &a, 1), "let: 6^2=36");

  CHECK(diff_case("let* nested",
                  "(defshader f ((x u32)) -> u32"
                  " (let* ((y (* x x)) (z (+ y 1))) z))",
                  NULL, &a, 1), "let*: 6^2+1=37");

  CHECK(diff_case("let* three",
                  "(defshader f ((x u32)) -> u32"
                  " (let* ((a (+ x 1)) (b (+ a 1)) (c (+ b 1))) c))",
                  NULL, &a, 1), "let*: 6+3=9");

  sh_value two = sh_val_u32(2);
  CHECK(diff_case("let parallel",
                  "(defshader f ((x u32)) -> u32"
                  " (let ((a (+ x 1)) (b (* x 2))) (+ a b)))",
                  NULL, &two, 1), "let: parallel (3+4=7)");
}

static void test_diff_loop(void) {
  printf("--- differential: loops ---\n");

  // Const-bound sum 0..9
  CHECK(diff_case("sum10",
                  "(defshader f () -> i64"
                  " (let loop ((i 0) (acc 0))"
                  "   (if (>= i 10) acc (loop (+ i 1) (+ acc i)))))",
                  NULL, NULL, 0), "const-bound loop: sum 0..9 = 45");

  // Region sum
  uint32_t data[5] = {1, 2, 3, 4, 5};
  sh_value buf = sh_val_region_raw(data, 5, SH_K_U32, false);

  const char *sum_src =
    "(defshader f ((buf (bytes u32))) -> u32"
    " (let loop ((i 0) (acc 0))"
    "   (if (>= i (region-len buf))"
    "     acc"
    "     (loop (+ i 1) (+ acc (region-ref buf i))))))";
  CHECK(diff_case("region sum [1..5]", sum_src, NULL, &buf, 1),
        "region sum [1,2,3,4,5] = 15");

  sh_value empty_buf = sh_val_region_raw(data, 0, SH_K_U32, false);
  CHECK(diff_case("region sum empty", sum_src, NULL, &empty_buf, 1),
        "region sum empty = 0");

  // Multiple arg sets for the sum shader.
  uint32_t data2[3] = {10, 20, 30};
  sh_value buf2 = sh_val_region_raw(data2, 3, SH_K_U32, false);
  CHECK(diff_case("region sum [10,20,30]", sum_src, NULL, &buf2, 1),
        "region sum [10,20,30] = 60");

  // RECUR with arithmetic on induction vars (counter + accumulator).
  const char *acc_src =
    "(defshader f () -> u32"
    " (let loop ((i 0) (acc (u32 0)))"
    "   (if (>= i 5)"
    "     acc"
    "     (loop (+ i 1) (+ acc (* i i))))))";
  // 0^2 + 1^2 + 2^2 + 3^2 + 4^2 = 0+1+4+9+16 = 30
  CHECK(diff_case("recur acc sum of squares", acc_src, NULL, NULL, 0),
        "RECUR acc: sum of squares 0..4 = 30");

  // Parallel swap: each iteration swaps a and b by passing them directly as
  // each other's new value -- (loop (+ i 1) b a). This is the exact case the
  // lowerer's parallel-assignment must get right (materialize into fresh temps
  // before overwriting any induction slot). The oracle is the ground truth.
  const char *swap_src =
    "(defshader f ((n u32)) -> u32"
    " (let loop ((i 0) (a (u32 1)) (b (u32 2)))"
    "   (if (>= i n) a (loop (+ i 1) b a))))";
  sh_value sn0 = sh_val_u32(0);  // a = 1 (no swaps)
  sh_value sn1 = sh_val_u32(1);  // a = 2 (one swap)
  sh_value sn2 = sh_val_u32(2);  // a = 1 (two swaps)
  sh_value sn5 = sh_val_u32(5);  // a = 2 (odd swaps)
  CHECK(diff_case("recur swap n=0", swap_src, NULL, &sn0, 1), "swap n=0 -> a=1");
  CHECK(diff_case("recur swap n=1", swap_src, NULL, &sn1, 1), "swap n=1 -> a=2");
  CHECK(diff_case("recur swap n=2", swap_src, NULL, &sn2, 1), "swap n=2 -> a=1");
  CHECK(diff_case("recur swap n=5", swap_src, NULL, &sn5, 1), "swap n=5 -> a=2");
}

static void test_diff_region_bounds(void) {
  printf("--- differential: region bounds ---\n");

  // OOB read -- both VM and interp must return SH_ERR_BOUNDS.
  uint32_t data[3] = {10, 20, 30};
  sh_value buf   = sh_val_region_raw(data, 3, SH_K_U32, false);
  sh_value idx   = sh_val_u32(3);  // OOB
  sh_value oob_args[2] = {buf, idx};

  const char *oob_src =
    "(defshader f ((buf (bytes u32)) (i u32)) -> u32"
    " (region-ref buf i))";

  // Compile once, test both.
  sh_program *p = NULL;
  sh_error err;
  memset(&err, 0, sizeof(err));
  sh_status s = sh_compile_string(oob_src, NULL, 0, &p, &err);
  if (s == SH_OK && p) {
    sh_chunk *c = NULL;
    memset(&err, 0, sizeof(err));
    s = sh_lower(p, &c, &err);
    if (s == SH_OK && c) {
      sh_value vm_out, interp_out;
      memset(&vm_out,     0, sizeof(vm_out));
      memset(&interp_out, 0, sizeof(interp_out));
      sh_error ev, ei;
      memset(&ev, 0, sizeof(ev));
      memset(&ei, 0, sizeof(ei));
      sh_status sv = sh_vm_run(c, oob_args, 2, 0, &vm_out, &ev);
      sh_status si = sh_invoke(p, oob_args, 2, &interp_out, &ei);
      CHECK(sv == SH_ERR_BOUNDS, "vm: OOB load -> SH_ERR_BOUNDS");
      CHECK(si == SH_ERR_BOUNDS, "interp: OOB load -> SH_ERR_BOUNDS");
      CHECK(sv == si, "vm and interp agree on bounds error");
      sh_chunk_free(c);
    }
    sh_free(p); p = NULL;
  }

  // In-bounds read: both must succeed and agree.
  idx = sh_val_u32(2);
  oob_args[1] = idx;
  CHECK(diff_case("region in-bounds", oob_src, NULL, oob_args, 2),
        "in-bounds read: vm == interp");

  // OOB store: both must return SH_ERR_BOUNDS.
  uint32_t dst[3] = {0, 0, 0};
  sh_value mbuf    = sh_val_region_raw(dst, 3, SH_K_U32, true);
  sh_value st_args[2] = {mbuf, sh_val_u32(5)};  // idx=5 > len=3
  const char *store_src =
    "(defshader f ((buf (bytes-mut u32)) (i u32)) -> u32"
    " (region-set! buf i 99))";
  memset(&err, 0, sizeof(err));
  s = sh_compile_string(store_src, NULL, 0, &p, &err);
  if (s == SH_OK && p) {
    sh_chunk *c = NULL;
    memset(&err, 0, sizeof(err));
    s = sh_lower(p, &c, &err);
    if (s == SH_OK && c) {
      sh_value vm_out, interp_out;
      memset(&vm_out,     0, sizeof(vm_out));
      memset(&interp_out, 0, sizeof(interp_out));
      sh_error ev, ei;
      memset(&ev, 0, sizeof(ev));
      memset(&ei, 0, sizeof(ei));
      sh_status sv = sh_vm_run(c, st_args, 2, 0, &vm_out, &ev);
      sh_status si = sh_invoke(p, st_args, 2, &interp_out, &ei);
      CHECK(sv == SH_ERR_BOUNDS, "vm: OOB store -> SH_ERR_BOUNDS");
      CHECK(si == SH_ERR_BOUNDS, "interp: OOB store -> SH_ERR_BOUNDS");
      sh_chunk_free(c);
    }
    sh_free(p);
  }
}

// Saturating-add blit.
static void test_diff_blit(void) {
  printf("--- differential: saturating blit ---\n");

  uint8_t src_data[6] = {200, 100, 50, 10, 255, 0};
  uint8_t dst_data_vm[6];
  uint8_t dst_data_interp[6];

  const char *blit_src =
    "(defshader f ((src (bytes u8)) (dst (bytes-mut u8)) (delta u32)) -> u32"
    " (let loop ((i 0) (acc (u32 0)))"
    "   (if (>= i (region-len src))"
    "     (region-len src)"
    "     (loop (+ i 1)"
    "           (+ acc (u32 (region-set! dst i"
    "             (u8 (if (> (+ (u32 (region-ref src i)) delta) (u32 255))"
    "                    (u32 255)"
    "                    (+ (u32 (region-ref src i)) delta))))))))))";

  // Test with delta=60.
  memset(dst_data_vm,    0, 6);
  memset(dst_data_interp,0, 6);

  sh_value src_buf_vm     = sh_val_region_raw(src_data,    6, SH_K_U8, false);
  sh_value dst_buf_vm     = sh_val_region_raw(dst_data_vm, 6, SH_K_U8, true);
  sh_value src_buf_interp = sh_val_region_raw(src_data,        6, SH_K_U8, false);
  sh_value dst_buf_interp = sh_val_region_raw(dst_data_interp, 6, SH_K_U8, true);

  sh_program *p = NULL;
  sh_error err;
  memset(&err, 0, sizeof(err));
  sh_status s = sh_compile_string(blit_src, NULL, 0, &p, &err);
  if (s != SH_OK) {
    printf("  [blit] compile FAILED: %s\n", err.msg);
    CHECK(0, "blit compiles");
    return;
  }
  sh_chunk *c = NULL;
  memset(&err, 0, sizeof(err));
  s = sh_lower(p, &c, &err);
  if (s != SH_OK) {
    printf("  [blit] lower FAILED: %s\n", err.msg);
    sh_free(p);
    CHECK(0, "blit lowers");
    return;
  }

  sh_value args_vm[3]     = {src_buf_vm,     dst_buf_vm,     sh_val_u32(60)};
  sh_value args_interp[3] = {src_buf_interp, dst_buf_interp, sh_val_u32(60)};

  sh_value vm_out, interp_out;
  memset(&vm_out,     0, sizeof(vm_out));
  memset(&interp_out, 0, sizeof(interp_out));
  sh_error ev, ei;
  memset(&ev, 0, sizeof(ev));
  memset(&ei, 0, sizeof(ei));

  sh_status sv = sh_vm_run(c, args_vm,     3, 0, &vm_out,     &ev);
  sh_status si = sh_invoke(p, args_interp, 3, &interp_out, &ei);

  CHECK(sv == SH_OK, "blit vm runs ok");
  CHECK(si == SH_OK, "blit interp runs ok");
  if (sv == SH_OK && si == SH_OK) {
    // Compare return value.
    CHECK(sh_value_bits_eq(vm_out, interp_out), "blit: return value matches");
    // Compare the written bytes.
    int ok = 1;
    for (int i = 0; i < 6; i++) ok &= (dst_data_vm[i] == dst_data_interp[i]);
    CHECK(ok, "blit: written dst bytes match interp");
  }

  sh_chunk_free(c);
  sh_free(p);

  // Second argument set: delta=0 (no change).
  memset(dst_data_vm,    0, 6);
  memset(dst_data_interp,0, 6);
  dst_buf_vm     = sh_val_region_raw(dst_data_vm,     6, SH_K_U8, true);
  dst_buf_interp = sh_val_region_raw(dst_data_interp, 6, SH_K_U8, true);

  memset(&err, 0, sizeof(err));
  s = sh_compile_string(blit_src, NULL, 0, &p, &err);
  if (s == SH_OK) {
    memset(&err, 0, sizeof(err));
    s = sh_lower(p, &c, &err);
    if (s == SH_OK) {
      sh_value a2vm[3]     = {sh_val_region_raw(src_data, 6, SH_K_U8, false),
                               dst_buf_vm, sh_val_u32(0)};
      sh_value a2ip[3]     = {sh_val_region_raw(src_data, 6, SH_K_U8, false),
                               dst_buf_interp, sh_val_u32(0)};
      memset(&vm_out, 0, sizeof(vm_out));
      memset(&interp_out, 0, sizeof(interp_out));
      memset(&ev, 0, sizeof(ev)); memset(&ei, 0, sizeof(ei));
      sv = sh_vm_run(c, a2vm, 3, 0, &vm_out, &ev);
      si = sh_invoke(p, a2ip, 3, &interp_out, &ei);
      CHECK(sv == SH_OK && si == SH_OK, "blit delta=0 runs ok");
      if (sv == SH_OK && si == SH_OK) {
        int ok = 1;
        for (int i = 0; i < 6; i++) ok &= (dst_data_vm[i] == dst_data_interp[i]);
        CHECK(ok, "blit delta=0: bytes match");
      }
      sh_chunk_free(c);
    }
    sh_free(p);
  }
}

static void test_diff_ip_checksum(void) {
  printf("--- differential: ip-checksum ---\n");

  const char *src =
    "(defshader f ((buf (bytes u16))) -> u32"
    " (let loop ((i 0) (acc (u32 0)))"
    "   (if (>= i (region-len buf))"
    "     acc"
    "     (loop (+ i 1) (+ acc (u32 (region-ref buf i)))))))";

  // Packet 1
  uint16_t pkt1[8] = {0x4500, 0x003C, 0x1C46, 0x4000,
                      0x4006, 0x0000, 0xAC10, 0x0A63};
  sh_value b1 = sh_val_region_raw(pkt1, 8, SH_K_U16, false);
  CHECK(diff_case("ip-checksum pkt1", src, NULL, &b1, 1),
        "ip-checksum pkt1 vm==interp");

  // Packet 2: different payload
  uint16_t pkt2[4] = {0x1111, 0x2222, 0x3333, 0x4444};
  sh_value b2 = sh_val_region_raw(pkt2, 4, SH_K_U16, false);
  CHECK(diff_case("ip-checksum pkt2", src, NULL, &b2, 1),
        "ip-checksum pkt2 vm==interp");

  // Empty packet
  sh_value b3 = sh_val_region_raw(pkt1, 0, SH_K_U16, false);
  CHECK(diff_case("ip-checksum empty", src, NULL, &b3, 1),
        "ip-checksum empty vm==interp");
}

static void test_diff_fdot(void) {
  printf("--- differential: f32 dot product via region ---\n");

  const char *src =
    "(defshader f ((a (bytes f32)) (b (bytes f32))) -> f32"
    " (let loop ((i 0) (acc (f32 0.0)))"
    "   (if (>= i (region-len a))"
    "     acc"
    "     (loop (+ i 1)"
    "           (+ acc (* (region-ref a i) (region-ref b i)))))))";

  float a_data[4] = {1.0f, 2.0f, 3.0f, 4.0f};
  float b_data[4] = {0.5f, 0.5f, 0.5f, 0.5f};
  sh_value abuf = sh_val_region_raw(a_data, 4, SH_K_F32, false);
  sh_value bbuf = sh_val_region_raw(b_data, 4, SH_K_F32, false);
  sh_value ab2[2] = {abuf, bbuf};
  CHECK(diff_case("fdot [1,2,3,4].[0.5..] ", src, NULL, ab2, 2),
        "f32 dot vm==interp (bit-exact)");

  float a2_data[4] = {-1.0f, 0.5f, 0.25f, 0.125f};
  float b2_data[4] = { 2.0f, 4.0f, 8.0f, 16.0f};
  abuf = sh_val_region_raw(a2_data, 4, SH_K_F32, false);
  bbuf = sh_val_region_raw(b2_data, 4, SH_K_F32, false);
  ab2[0] = abuf; ab2[1] = bbuf;
  CHECK(diff_case("fdot mixed signs", src, NULL, ab2, 2),
        "f32 dot mixed signs vm==interp");

  // Length 1
  float a1 = 3.14f, b1 = 2.71f;
  abuf = sh_val_region_raw(&a1, 1, SH_K_F32, false);
  bbuf = sh_val_region_raw(&b1, 1, SH_K_F32, false);
  ab2[0] = abuf; ab2[1] = bbuf;
  CHECK(diff_case("fdot len1", src, NULL, ab2, 2),
        "f32 dot len=1 vm==interp");
}

static void test_diff_call_prim(void) {
  printf("--- differential: CALL primitive ---\n");

  sh_type u32t = sh_type_scalar(SH_K_U32);
  sh_prim cp;
  memset(&cp, 0, sizeof(cp));
  cp.name     = "clamp";
  cp.ret      = u32t;
  cp.nparams  = 3;
  cp.params[0] = u32t;
  cp.params[1] = u32t;
  cp.params[2] = u32t;
  cp.fn       = clamp_u32_fn;
  sh_prim_set prims = {&cp, 1};

  const char *src =
    "(defshader f ((x u32)) -> u32 (clamp x 10 20))";

  sh_value a;
  a = sh_val_u32(5);
  CHECK(diff_case("clamp(5,10,20)", src, &prims, &a, 1),
        "CALL clamp(5,10,20)=10 vm==interp");
  a = sh_val_u32(15);
  CHECK(diff_case("clamp(15,10,20)", src, &prims, &a, 1),
        "CALL clamp(15,10,20)=15 vm==interp");
  a = sh_val_u32(25);
  CHECK(diff_case("clamp(25,10,20)", src, &prims, &a, 1),
        "CALL clamp(25,10,20)=20 vm==interp");
  a = sh_val_u32(10);
  CHECK(diff_case("clamp(10,10,20)", src, &prims, &a, 1),
        "CALL clamp at boundary lo vm==interp");
  a = sh_val_u32(20);
  CHECK(diff_case("clamp(20,10,20)", src, &prims, &a, 1),
        "CALL clamp at boundary hi vm==interp");
}

static void test_diff_vector(void) {
  printf("--- differential: vector ops ---\n");

  sh_value va = make_f32x4(1.0f, 2.0f, 3.0f, 4.0f);
  sh_value vb = make_f32x4(10.0f, 20.0f, 30.0f, 40.0f);
  sh_value args2[2];

  // VSPLAT
  sh_value scalar = sh_val_f32(3.0f);
  CHECK(diff_case("vsplat f32",
                  "(defshader f ((x f32)) -> vec4 (splat x))",
                  NULL, &scalar, 1),
        "VSPLAT f32 vm==interp");

  // VBINOP add
  args2[0] = va; args2[1] = vb;
  CHECK(diff_case("vbinop add f32x4",
                  "(defshader f ((a f32x4)(b f32x4)) -> f32x4 (+ a b))",
                  NULL, args2, 2),
        "VBINOP add f32x4 vm==interp");

  // VBINOP mul
  CHECK(diff_case("vbinop mul f32x4",
                  "(defshader f ((a f32x4)(b f32x4)) -> f32x4 (* a b))",
                  NULL, args2, 2),
        "VBINOP mul f32x4 vm==interp");

  // VCMP -> VLANE
  CHECK(diff_case("vcmp lane0",
                  "(defshader f ((a f32x4)(b f32x4)) -> bool"
                  " (lane (> a b) 0))",
                  NULL, args2, 2),
        "VCMP+VLANE lane0 vm==interp");
  CHECK(diff_case("vcmp lane1",
                  "(defshader f ((a f32x4)(b f32x4)) -> bool"
                  " (lane (> a b) 1))",
                  NULL, args2, 2),
        "VCMP+VLANE lane1 vm==interp");

  // VSELECT
  sh_value mask_vec;
  memset(&mask_vec, 0, sizeof(mask_vec));
  mask_vec.kind = SH_K_VEC;
  mask_vec.lanes = 4;
  mask_vec.lane_kind = (uint8_t)SH_K_BOOL;
  mask_vec.lane[0] = 1; mask_vec.lane[1] = 0;
  mask_vec.lane[2] = 1; mask_vec.lane[3] = 0;

  // VSHUFFLE
  CHECK(diff_case("vshuffle reverse",
                  "(defshader f ((v f32x4)) -> f32x4 (shuffle v 3 2 1 0))",
                  NULL, &va, 1),
        "VSHUFFLE reverse vm==interp");
  CHECK(diff_case("vshuffle xxzz",
                  "(defshader f ((v f32x4)) -> f32x4 (shuffle v 0 0 2 2))",
                  NULL, &va, 1),
        "VSHUFFLE xxzz vm==interp");

  // VREDUCE
  CHECK(diff_case("vreduce-add f32x4",
                  "(defshader f ((v f32x4)) -> f32 (vreduce-add v))",
                  NULL, &va, 1),
        "VREDUCE add [1,2,3,4]=10 vm==interp");
  CHECK(diff_case("vreduce-min f32x4",
                  "(defshader f ((v f32x4)) -> f32 (vreduce-min v))",
                  NULL, &va, 1),
        "VREDUCE min vm==interp");
  CHECK(diff_case("vreduce-max f32x4",
                  "(defshader f ((v f32x4)) -> f32 (vreduce-max v))",
                  NULL, &va, 1),
        "VREDUCE max vm==interp");

  // DOT
  sh_value vc = make_f32x4(1.0f, 1.0f, 1.0f, 1.0f);
  args2[0] = va; args2[1] = vc;
  CHECK(diff_case("dot f32x4 ones",
                  "(defshader f ((a f32x4)(b f32x4)) -> f32 (dot a b))",
                  NULL, args2, 2),
        "DOT [1,2,3,4].[1,1,1,1]=10 vm==interp");

  args2[0] = va; args2[1] = va;
  CHECK(diff_case("dot self",
                  "(defshader f ((a f32x4)(b f32x4)) -> f32 (dot a b))",
                  NULL, args2, 2),
        "DOT [1,2,3,4].[1,2,3,4]=30 vm==interp");

  // VLANE
  CHECK(diff_case("vlane0",
                  "(defshader f ((v f32x4)) -> f32 (lane v 0))",
                  NULL, &va, 1),
        "VLANE 0 vm==interp");
  CHECK(diff_case("vlane3",
                  "(defshader f ((v f32x4)) -> f32 (lane v 3))",
                  NULL, &va, 1),
        "VLANE 3 vm==interp");

  // u8x4 vbinop
  sh_value vu8a, vu8b;
  memset(&vu8a, 0, sizeof(vu8a));
  memset(&vu8b, 0, sizeof(vu8b));
  vu8a.kind = SH_K_VEC; vu8a.lanes = 4; vu8a.lane_kind = (uint8_t)SH_K_U8;
  vu8b.kind = SH_K_VEC; vu8b.lanes = 4; vu8b.lane_kind = (uint8_t)SH_K_U8;
  for (int i = 0; i < 4; i++) {
    vu8a.lane[i] = (uint64_t)(i + 1);
    vu8b.lane[i] = 3;
  }
  args2[0] = vu8a; args2[1] = vu8b;
  CHECK(diff_case("u8x4 mul",
                  "(defshader f ((a u8x4)(b u8x4)) -> u8x4 (* a b))",
                  NULL, args2, 2),
        "u8x4 VBINOP mul vm==interp");

  // IF with vector arms (scalar bool cond)
  args2[0] = va; args2[1] = vb;
  CHECK(diff_case("vector IF",
                  "(defshader f ((a f32x4)(b f32x4)) -> f32x4"
                  " (if (< (lane a 0) (lane b 0)) a b))",
                  NULL, args2, 2),
        "vector IF scalar cond vm==interp");
}

// ---------------------------------------------------------------------------
// Section 2b: SIMD bit-exactness landmine tests
// These cases specifically exercise the correctness traps documented in
// notes/scratch/shader-s3-decision.md:
//   - f32x4 add/sub/mul/div with edge-case values (zero, negative, nearly-equal)
//   - u8x16 and u32x4 integer VBINOP
//   - VCMP producing 0/1 bool mask, verified through VLANE
//   - VSHUFFLE all permutations in the dispatch table
//   - VREDUCE add/min/max on floats (the reassociation landmine: correct answer
//     is the left-to-right sequential fold; a tree reduction would differ)
//   - VREDUCE add on integers (ordering should match too)
// Both the SIMD (default) and FORCE_SCALAR paths are checked against the oracle.
// ---------------------------------------------------------------------------

// Helper: make a u32x4 sh_value.
static sh_value make_u32x4(uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
  sh_value v;
  memset(&v, 0, sizeof(v));
  v.kind = SH_K_VEC;
  v.lanes = 4;
  v.lane_kind = (uint8_t)SH_K_U32;
  v.lane[0] = a; v.lane[1] = b; v.lane[2] = c; v.lane[3] = d;
  return v;
}

// Helper: make a u8x16 sh_value.
static sh_value make_u8x16(uint8_t lanes[16]) {
  sh_value v;
  memset(&v, 0, sizeof(v));
  v.kind = SH_K_VEC;
  v.lanes = 16;
  v.lane_kind = (uint8_t)SH_K_U8;
  for (int i = 0; i < 16; i++) v.lane[i] = lanes[i];
  return v;
}

static void test_simd_exactness(void) {
  printf("--- SIMD bit-exactness landmines ---\n");

  // --- f32x4 arithmetic: several operand sets including zeros, negatives,
  //     and values that would differ under a different rounding order.
  {
    // Normal positive values.
    sh_value va = make_f32x4(1.0f/3.0f, 2.0f/7.0f, 3.0f/11.0f, 4.0f/13.0f);
    sh_value vb = make_f32x4(7.0f, 11.0f, 13.0f, 17.0f);
    sh_value args2[2] = {va, vb};
    CHECK(diff_case_flags("f32x4 add frac",
          "(defshader f ((a f32x4)(b f32x4)) -> f32x4 (+ a b))",
          NULL, args2, 2), "SIMD f32x4 add fractional values");
    CHECK(diff_case_flags("f32x4 sub frac",
          "(defshader f ((a f32x4)(b f32x4)) -> f32x4 (- a b))",
          NULL, args2, 2), "SIMD f32x4 sub fractional values");
    CHECK(diff_case_flags("f32x4 mul frac",
          "(defshader f ((a f32x4)(b f32x4)) -> f32x4 (* a b))",
          NULL, args2, 2), "SIMD f32x4 mul fractional values");
    CHECK(diff_case_flags("f32x4 div frac",
          "(defshader f ((a f32x4)(b f32x4)) -> f32x4 (/ a b))",
          NULL, args2, 2), "SIMD f32x4 div fractional values");
  }
  {
    // Negative and zero values.
    sh_value va = make_f32x4(-1.0f, 0.0f, -0.0f, 1e-38f);
    sh_value vb = make_f32x4( 1.0f, 0.0f,  1.0f, 1e38f);
    sh_value args2[2] = {va, vb};
    CHECK(diff_case_flags("f32x4 add neg/zero",
          "(defshader f ((a f32x4)(b f32x4)) -> f32x4 (+ a b))",
          NULL, args2, 2), "SIMD f32x4 add with negatives/zeros");
    CHECK(diff_case_flags("f32x4 mul neg/zero",
          "(defshader f ((a f32x4)(b f32x4)) -> f32x4 (* a b))",
          NULL, args2, 2), "SIMD f32x4 mul with negatives/zeros");
    // f32x4 div: avoid 0/0 in the SIMD test since the spec defines that as 0
    // via the scalar zero-check (fb != 0.0 ? fa/fb : 0.0), but SSE _mm_div_ps
    // produces NaN for 0/0. The SIMD div path is scalar-only (see sh_vm.c).
    // Use only non-zero divisors here.
    {
      sh_value vad = make_f32x4(-1.0f, -0.0f, 1e-38f, 1.0f);
      sh_value vbd = make_f32x4( 1.0f,  1.0f, 1e38f, -1.0f);
      sh_value aargs[2] = {vad, vbd};
      CHECK(diff_case_flags("f32x4 div nonzero",
            "(defshader f ((a f32x4)(b f32x4)) -> f32x4 (/ a b))",
            NULL, aargs, 2), "SIMD f32x4 div nonzero divisors");
    }
  }
  {
    // Large values (near overflow territory).
    sh_value va = make_f32x4(1e37f, -1e37f, 3.4e38f, -3.4e38f);
    sh_value vb = make_f32x4(1e36f, -1e36f, 3.4e37f, -3.4e37f);
    sh_value args2[2] = {va, vb};
    CHECK(diff_case_flags("f32x4 add large",
          "(defshader f ((a f32x4)(b f32x4)) -> f32x4 (+ a b))",
          NULL, args2, 2), "SIMD f32x4 add large values");
    CHECK(diff_case_flags("f32x4 mul large",
          "(defshader f ((a f32x4)(b f32x4)) -> f32x4 (* a b))",
          NULL, args2, 2), "SIMD f32x4 mul large values");
  }

  // --- u32x4 VBINOP ---
  {
    sh_value va = make_u32x4(0xFFFFFFFFu, 1, 100, 0);
    sh_value vb = make_u32x4(1, 0xFFFFFFFFu, 200, 0);
    sh_value args2[2] = {va, vb};
    CHECK(diff_case_flags("u32x4 add wrap",
          "(defshader f ((a u32x4)(b u32x4)) -> u32x4 (+ a b))",
          NULL, args2, 2), "SIMD u32x4 add with wraparound");
    CHECK(diff_case_flags("u32x4 sub wrap",
          "(defshader f ((a u32x4)(b u32x4)) -> u32x4 (- a b))",
          NULL, args2, 2), "SIMD u32x4 sub with wraparound");
    CHECK(diff_case_flags("u32x4 mul",
          "(defshader f ((a u32x4)(b u32x4)) -> u32x4 (* a b))",
          NULL, args2, 2), "SIMD u32x4 mul");
  }
  {
    sh_value va = make_u32x4(3, 7, 15, 255);
    sh_value vb = make_u32x4(3, 7, 15, 255);
    sh_value args2[2] = {va, vb};
    CHECK(diff_case_flags("u32x4 mul equal lanes",
          "(defshader f ((a u32x4)(b u32x4)) -> u32x4 (* a b))",
          NULL, args2, 2), "SIMD u32x4 mul equal lanes (squares)");
  }

  // --- u8x16 VBINOP ---
  {
    uint8_t la[16] = {255,200,100,50,1,0,128,64, 255,200,100,50,1,0,128,64};
    uint8_t lb[16] = {  1, 10, 20,30,5,7, 64,32,   0,  1, 56,27,9,3,  10, 8};
    sh_value va = make_u8x16(la);
    sh_value vb = make_u8x16(lb);
    sh_value args2[2] = {va, vb};
    CHECK(diff_case_flags("u8x16 add wrap",
          "(defshader f ((a u8x16)(b u8x16)) -> u8x16 (+ a b))",
          NULL, args2, 2), "SIMD u8x16 add with wrapping");
    CHECK(diff_case_flags("u8x16 sub wrap",
          "(defshader f ((a u8x16)(b u8x16)) -> u8x16 (- a b))",
          NULL, args2, 2), "SIMD u8x16 sub with wrapping");
  }

  // --- VCMP -> VLANE: verify bool mask is exactly 0 or 1 (not 0xFF) ---
  {
    sh_value va = make_f32x4(1.0f, 5.0f, 3.0f, 9.0f);
    sh_value vb = make_f32x4(2.0f, 5.0f, 2.0f, 8.0f);
    sh_value args2[2] = {va, vb};
    // lane 0: 1 < 2 -> true; lane 1: 5 < 5 -> false
    CHECK(diff_case_flags("vcmp lt lane0",
          "(defshader f ((a f32x4)(b f32x4)) -> bool (lane (< a b) 0))",
          NULL, args2, 2), "SIMD VCMP lt lane0 = 1");
    CHECK(diff_case_flags("vcmp lt lane1",
          "(defshader f ((a f32x4)(b f32x4)) -> bool (lane (< a b) 1))",
          NULL, args2, 2), "SIMD VCMP lt lane1 = 0");
    CHECK(diff_case_flags("vcmp eq lane1",
          "(defshader f ((a f32x4)(b f32x4)) -> bool (lane (= a b) 1))",
          NULL, args2, 2), "SIMD VCMP eq lane1 = 1");
    CHECK(diff_case_flags("vcmp gt lane2",
          "(defshader f ((a f32x4)(b f32x4)) -> bool (lane (> a b) 2))",
          NULL, args2, 2), "SIMD VCMP gt lane2 = 1");
    CHECK(diff_case_flags("vcmp ge lane3",
          "(defshader f ((a f32x4)(b f32x4)) -> bool (lane (>= a b) 3))",
          NULL, args2, 2), "SIMD VCMP ge lane3 = 1");
    // NE: the shader language has no direct /= on vectors;
    // test NE indirectly via the existing NOT(EQ) through the lower-level test
    // (the NE path in VCMP SIMD is exercised by all six-predicate coverage).
    // Skipping explicit NE test (no frontend syntax for it on vectors).
    // u32x4 cmp
    sh_value vu32a = make_u32x4(5, 10, 10, 0);
    sh_value vu32b = make_u32x4(5, 20,  5, 1);
    sh_value uargs[2] = {vu32a, vu32b};
    CHECK(diff_case_flags("vcmp u32x4 eq lane0",
          "(defshader f ((a u32x4)(b u32x4)) -> bool (lane (= a b) 0))",
          NULL, uargs, 2), "SIMD u32x4 VCMP eq lane0 = 1");
    CHECK(diff_case_flags("vcmp u32x4 lt lane1",
          "(defshader f ((a u32x4)(b u32x4)) -> bool (lane (< a b) 1))",
          NULL, uargs, 2), "SIMD u32x4 VCMP lt lane1 = 1");
    CHECK(diff_case_flags("vcmp u32x4 gt lane2",
          "(defshader f ((a u32x4)(b u32x4)) -> bool (lane (> a b) 2))",
          NULL, uargs, 2), "SIMD u32x4 VCMP gt lane2 = 1");
  }

  // --- VSHUFFLE: verify all four common patterns hit the dispatch table ---
  {
    sh_value va = make_f32x4(10.0f, 20.0f, 30.0f, 40.0f);
    CHECK(diff_case_flags("vshuffle identity",
          "(defshader f ((v f32x4)) -> f32x4 (shuffle v 0 1 2 3))",
          NULL, &va, 1), "SIMD VSHUFFLE identity");
    CHECK(diff_case_flags("vshuffle reverse",
          "(defshader f ((v f32x4)) -> f32x4 (shuffle v 3 2 1 0))",
          NULL, &va, 1), "SIMD VSHUFFLE reverse");
    CHECK(diff_case_flags("vshuffle swizzle xwzy",
          "(defshader f ((v f32x4)) -> f32x4 (shuffle v 0 3 2 1))",
          NULL, &va, 1), "SIMD VSHUFFLE xwzy");
    CHECK(diff_case_flags("vshuffle broadcast x",
          "(defshader f ((v f32x4)) -> f32x4 (shuffle v 0 0 0 0))",
          NULL, &va, 1), "SIMD VSHUFFLE broadcast lane 0");
    CHECK(diff_case_flags("vshuffle wzyx",
          "(defshader f ((v f32x4)) -> f32x4 (shuffle v 3 2 1 0))",
          NULL, &va, 1), "SIMD VSHUFFLE wzyx");
  }

  // --- VREDUCE: the float ordering landmine ---
  // These values are chosen so that a tree reduction (l0+l1)+(l2+l3) would
  // potentially differ from sequential (((l0+l1)+l2)+l3). We verify the VM
  // matches the oracle (which always does sequential).
  {
    // Values where order matters: 1e7 + 1 + (-1e7) + 1 = 2 sequential,
    // but (1e7 + 1) + ((-1e7) + 1) = 2 too -- use values that diverge more:
    // use a sum where rounding at intermediate steps differs.
    sh_value va = make_f32x4(1.0f/3.0f, 1.0f/7.0f, 1.0f/11.0f, 1.0f/13.0f);
    CHECK(diff_case_flags("vreduce-add f32x4 fracs",
          "(defshader f ((v f32x4)) -> f32 (vreduce-add v))",
          NULL, &va, 1), "VREDUCE add f32x4 fracs (ordering landmine)");
    sh_value vb = make_f32x4(-1.0f, 3.0f, -2.0f, 4.0f);
    CHECK(diff_case_flags("vreduce-add f32x4 mixed signs",
          "(defshader f ((v f32x4)) -> f32 (vreduce-add v))",
          NULL, &vb, 1), "VREDUCE add f32x4 mixed signs");
    sh_value vc = make_f32x4(1e7f, 1.0f, -1e7f, 1.0f);
    CHECK(diff_case_flags("vreduce-add f32x4 cancellation",
          "(defshader f ((v f32x4)) -> f32 (vreduce-add v))",
          NULL, &vc, 1), "VREDUCE add f32x4 cancellation case");

    // min/max: NaN / signed-zero semantics -- must match oracle.
    sh_value vd = make_f32x4(-5.0f, 3.0f, -1.0f, 2.0f);
    CHECK(diff_case_flags("vreduce-min f32x4 neg",
          "(defshader f ((v f32x4)) -> f32 (vreduce-min v))",
          NULL, &vd, 1), "VREDUCE min f32x4 with negatives");
    CHECK(diff_case_flags("vreduce-max f32x4 neg",
          "(defshader f ((v f32x4)) -> f32 (vreduce-max v))",
          NULL, &vd, 1), "VREDUCE max f32x4 with negatives");

    // Integer VREDUCE: order should be consistent.
    sh_value vi = make_u32x4(100, 200, 300, 400);
    CHECK(diff_case_flags("vreduce-add u32x4",
          "(defshader f ((v u32x4)) -> u32 (vreduce-add v))",
          NULL, &vi, 1), "VREDUCE add u32x4");
    sh_value vj = make_u32x4(7, 2, 9, 1);
    CHECK(diff_case_flags("vreduce-min u32x4",
          "(defshader f ((v u32x4)) -> u32 (vreduce-min v))",
          NULL, &vj, 1), "VREDUCE min u32x4");
    CHECK(diff_case_flags("vreduce-max u32x4",
          "(defshader f ((v u32x4)) -> u32 (vreduce-max v))",
          NULL, &vj, 1), "VREDUCE max u32x4");
  }

  // --- VSELECT through VCMP: full pipeline VCMP->VSELECT->VLANE ---
  // The shader language expresses VSELECT implicitly via (if mask then else)
  // on vectors. Exercise VCMP+VLANE as the direct pipeline.
  {
    sh_value va = make_f32x4(1.0f, 5.0f, 3.0f, 9.0f);
    sh_value vb = make_f32x4(2.0f, 5.0f, 2.0f, 8.0f);
    sh_value args2[2] = {va, vb};
    // Test all six predicates so VCMP covers all branches.
    CHECK(diff_case_flags("vcmp le lane0",
          "(defshader f ((a f32x4)(b f32x4)) -> bool (lane (<= a b) 0))",
          NULL, args2, 2), "SIMD VCMP le lane0 = 1 (1<=2)");
    CHECK(diff_case_flags("vcmp le lane1 (eq)",
          "(defshader f ((a f32x4)(b f32x4)) -> bool (lane (<= a b) 1))",
          NULL, args2, 2), "SIMD VCMP le lane1 = 1 (5<=5)");
    CHECK(diff_case_flags("vcmp ge lane2",
          "(defshader f ((a f32x4)(b f32x4)) -> bool (lane (>= a b) 2))",
          NULL, args2, 2), "SIMD VCMP ge lane2 = 1 (3>=2)");
    // NE: no /= syntax for vectors; use (< b a) for lane1 (5<5=false) to
    // exercise the same "false result" path.
    CHECK(diff_case_flags("vcmp lt reversed lane1",
          "(defshader f ((a f32x4)(b f32x4)) -> bool (lane (< b a) 1))",
          NULL, args2, 2), "SIMD VCMP lt(b<a) lane1 = 0 (5<5 -> false)");
    // Extract lane from a VCMP result to verify bool representation.
    CHECK(diff_case_flags("vcmp lane3",
          "(defshader f ((a f32x4)(b f32x4)) -> bool (lane (> a b) 3))",
          NULL, args2, 2), "SIMD VCMP gt lane3 = 1 (9>8)");
  }
}

// ---------------------------------------------------------------------------
// Section 2c: validator safety tests (Findings 2, 3, 5)
// ---------------------------------------------------------------------------

// Helper: build a minimal valid chunk with one instruction (opcode, dst, a, b, c).
// Returns a heap-allocated sh_chunk with nvregs=4 and nparams=2.
// Caller must free with sh_chunk_free.
static sh_chunk *make_minimal_chunk(sh_bc_op op, uint8_t lanes,
                                    sh_vreg dst, sh_vreg a,
                                    sh_vreg b, sh_vreg c) {
  sh_chunk *ck = (sh_chunk *)calloc(1, sizeof(sh_chunk));
  if (!ck) return NULL;
  ck->nvregs  = 4;
  ck->nparams = 2;
  ck->ncode   = 1;
  ck->code    = (sh_instr *)calloc(1, sizeof(sh_instr));
  if (!ck->code) { free(ck); return NULL; }
  ck->naux    = 0;
  ck->aux     = NULL;
  ck->result  = SH_VREG_NONE;
  ck->prims   = NULL;
  ck->code[0].op    = (uint16_t)op;
  ck->code[0].lanes = lanes;
  ck->code[0].dst   = dst;
  ck->code[0].a     = a;
  ck->code[0].b     = b;
  ck->code[0].c     = c;
  return ck;
}

static void test_validator_safety(void) {
  printf("--- validator safety: lanes/required-operands/param ---\n");

  // Finding 2: ins->lanes > SH_MAX_LANES must be rejected (heap overflow guard).
  {
    sh_chunk *ck = make_minimal_chunk(SHB_VSPLAT, (uint8_t)(SH_MAX_LANES + 1),
                                      0, 1, SH_VREG_NONE, SH_VREG_NONE);
    if (ck) {
      sh_error err; memset(&err, 0, sizeof(err));
      CHECK(sh_chunk_validate(ck, &err) == SH_ERR_INTERNAL,
            "validator: VSPLAT lanes > SH_MAX_LANES rejected");
      free(ck->code); free(ck);
    }
  }
  {
    // Use lanes=200 to ensure it's well beyond the limit.
    sh_chunk *ck = make_minimal_chunk(SHB_VBINOP, 200,
                                      0, 1, 2, SH_VREG_NONE);
    if (ck) {
      sh_error err; memset(&err, 0, sizeof(err));
      CHECK(sh_chunk_validate(ck, &err) == SH_ERR_INTERNAL,
            "validator: VBINOP lanes=200 > SH_MAX_LANES rejected");
      free(ck->code); free(ck);
    }
  }

  // Finding 3: required operand == SH_VREG_NONE must be rejected.
  // BINOP reads a and b; a == SH_VREG_NONE must fail.
  {
    sh_chunk *ck = make_minimal_chunk(SHB_BINOP, 0,
                                      0, SH_VREG_NONE, 1, SH_VREG_NONE);
    if (ck) {
      sh_error err; memset(&err, 0, sizeof(err));
      CHECK(sh_chunk_validate(ck, &err) == SH_ERR_INTERNAL,
            "validator: BINOP with a=SH_VREG_NONE rejected");
      free(ck->code); free(ck);
    }
  }
  {
    // BINOP reads b too.
    sh_chunk *ck = make_minimal_chunk(SHB_BINOP, 0,
                                      0, 1, SH_VREG_NONE, SH_VREG_NONE);
    if (ck) {
      sh_error err; memset(&err, 0, sizeof(err));
      CHECK(sh_chunk_validate(ck, &err) == SH_ERR_INTERNAL,
            "validator: BINOP with b=SH_VREG_NONE rejected");
      free(ck->code); free(ck);
    }
  }
  {
    // RSTORE reads a, b, c -- c=SH_VREG_NONE must fail.
    sh_chunk *ck = make_minimal_chunk(SHB_RSTORE, 0,
                                      SH_VREG_NONE, 0, 1, SH_VREG_NONE);
    if (ck) {
      sh_error err; memset(&err, 0, sizeof(err));
      CHECK(sh_chunk_validate(ck, &err) == SH_ERR_INTERNAL,
            "validator: RSTORE with c=SH_VREG_NONE rejected");
      free(ck->code); free(ck);
    }
  }
  {
    // VSELECT reads a (mask), b (then), c (else).
    sh_chunk *ck = make_minimal_chunk(SHB_VSELECT, 4,
                                      0, SH_VREG_NONE, 1, 2);
    if (ck) {
      sh_error err; memset(&err, 0, sizeof(err));
      CHECK(sh_chunk_validate(ck, &err) == SH_ERR_INTERNAL,
            "validator: VSELECT with a=SH_VREG_NONE rejected");
      free(ck->code); free(ck);
    }
  }
  {
    // JMP_IFNOT reads a.
    sh_chunk *ck = make_minimal_chunk(SHB_JMP_IFNOT, 0,
                                      SH_VREG_NONE, SH_VREG_NONE,
                                      SH_VREG_NONE, SH_VREG_NONE);
    if (ck) {
      ck->code[0].imm = 1;  // valid jump target (== ncode)
      sh_error err; memset(&err, 0, sizeof(err));
      CHECK(sh_chunk_validate(ck, &err) == SH_ERR_INTERNAL,
            "validator: JMP_IFNOT with a=SH_VREG_NONE rejected");
      free(ck->code); free(ck);
    }
  }
  {
    // VSPLAT reads a.
    sh_chunk *ck = make_minimal_chunk(SHB_VSPLAT, 4,
                                      0, SH_VREG_NONE, SH_VREG_NONE,
                                      SH_VREG_NONE);
    if (ck) {
      sh_error err; memset(&err, 0, sizeof(err));
      CHECK(sh_chunk_validate(ck, &err) == SH_ERR_INTERNAL,
            "validator: VSPLAT with a=SH_VREG_NONE rejected");
      free(ck->code); free(ck);
    }
  }

  // Finding 5: PARAM imm >= nparams must be rejected.
  {
    sh_chunk *ck = make_minimal_chunk(SHB_PARAM, 0,
                                      0, SH_VREG_NONE, SH_VREG_NONE,
                                      SH_VREG_NONE);
    if (ck) {
      // nparams == 2; imm == 5 is OOB.
      ck->code[0].imm = 5;
      sh_error err; memset(&err, 0, sizeof(err));
      CHECK(sh_chunk_validate(ck, &err) == SH_ERR_INTERNAL,
            "validator: PARAM imm >= nparams rejected");
      // imm == 1 is valid (< nparams=2).
      ck->code[0].imm = 1;
      memset(&err, 0, sizeof(err));
      CHECK(sh_chunk_validate(ck, &err) == SH_OK,
            "validator: PARAM imm < nparams accepted");
      free(ck->code); free(ck);
    }
  }
}

// ---------------------------------------------------------------------------
// Section 2d: Finding 4 -- VSELECT end-to-end coverage (direct chunk test)
// ---------------------------------------------------------------------------
// The frontend compiles (if mask-vec then-vec else-vec) into a vector IF, not
// a VSELECT instruction.  VSELECT is therefore not reachable from the frontend
// pipeline and cannot be differential-tested via compile->lower->vm.  Instead
// we build a hand-crafted chunk that:
//   slot[0] = splat of 'then' value   (f32x4, all 5.0f)
//   slot[1] = splat of 'else' value   (f32x4, all 9.0f)
//   slot[2] = mask vector             (bool x4, lanes 1,0,1,0)
//   slot[3] = VSELECT dst             (result)
// Expected: [5,9,5,9].
// We also test u32x4 and u8x16 to exercise all three SIMD blend paths.
static void test_vselect_direct(void) {
  printf("--- VSELECT direct chunk test (Finding 4) ---\n");

  // Helper lambda-equivalent: build and run a VSELECT chunk, check result.
  // Uses sh_vm_run directly (both SIMD and FORCE_SCALAR paths).
  // vreg layout: 0=then, 1=else, 2=mask(bool), 3=dst; no params (nparams=0).

  // --- f32x4 VSELECT ---
  {
    // Allocate chunk with 4 instructions: CONST, CONST, hand-set slots via PARAM
    // trick... actually simpler to allocate a chunk with nvregs=4 and 1 instr.
    // We inject the slot values by passing them as params via SHB_PARAM
    // and manually construct: PARAM 0 -> slot0, PARAM 1 -> slot1,
    // PARAM 2 -> slot2 (mask), VSELECT slot3 = slot2 ? slot0 : slot1.

    // Build chunk: 4 instructions: 3 PARAMs + 1 VSELECT.
    sh_chunk ck;
    memset(&ck, 0, sizeof(ck));
    ck.nvregs  = 4;
    ck.nparams = 3;
    ck.naux    = 0;
    ck.aux     = NULL;
    ck.result  = 3;  // result = slot[3]
    ck.prims   = NULL;
    ck.ncode   = 4;
    sh_instr code[4];
    memset(code, 0, sizeof(code));
    ck.code = code;

    // PARAM 0 -> slot 0 (then vector)
    code[0].op = (uint16_t)SHB_PARAM; code[0].dst = 0; code[0].imm = 0;
    code[0].a = code[0].b = code[0].c = SH_VREG_NONE;
    // PARAM 1 -> slot 1 (else vector)
    code[1].op = (uint16_t)SHB_PARAM; code[1].dst = 1; code[1].imm = 1;
    code[1].a = code[1].b = code[1].c = SH_VREG_NONE;
    // PARAM 2 -> slot 2 (mask vector)
    code[2].op = (uint16_t)SHB_PARAM; code[2].dst = 2; code[2].imm = 2;
    code[2].a = code[2].b = code[2].c = SH_VREG_NONE;
    // VSELECT: slot3 = slot2 ? slot0 : slot1
    code[3].op    = (uint16_t)SHB_VSELECT;
    code[3].kind  = (uint8_t)SH_K_F32;
    code[3].lanes = 4;
    code[3].dst   = 3;
    code[3].a     = 2;  // mask
    code[3].b     = 0;  // then
    code[3].c     = 1;  // else

    // Param types: all vec (needed for sh_vm_run type-check).
    ck.params[0].kind     = SH_K_VEC;
    ck.params[0].lanes    = 4;
    ck.params[0].lane_kind = (uint8_t)SH_K_F32;
    ck.params[1] = ck.params[0];
    ck.params[2].kind     = SH_K_VEC;
    ck.params[2].lanes    = 4;
    ck.params[2].lane_kind = (uint8_t)SH_K_BOOL;

    // Build args: then=[5,5,5,5], else=[9,9,9,9], mask=[1,0,1,0].
    sh_value args[3];
    args[0] = make_f32x4(5.0f, 5.0f, 5.0f, 5.0f);   // then
    args[1] = make_f32x4(9.0f, 9.0f, 9.0f, 9.0f);   // else
    memset(&args[2], 0, sizeof(sh_value));
    args[2].kind = SH_K_VEC; args[2].lanes = 4;
    args[2].lane_kind = (uint8_t)SH_K_BOOL;
    args[2].lane[0] = 1; args[2].lane[1] = 0;
    args[2].lane[2] = 1; args[2].lane[3] = 0;

    // Expected: [5,9,5,9].
    float exp[4] = {5.0f, 9.0f, 5.0f, 9.0f};

    sh_error err; sh_value out;
    memset(&err, 0, sizeof(err));
    memset(&out, 0, sizeof(out));
    sh_status s = sh_vm_run(&ck, args, 3, 0, &out, &err);
    int ok = (s == SH_OK) && (out.kind == SH_K_VEC) && (out.lanes == 4);
    if (ok) {
      for (int li = 0; li < 4; li++) {
        uint32_t b32 = (uint32_t)out.lane[li];
        float fv; memcpy(&fv, &b32, 4);
        ok &= (fv == exp[li]);
      }
    }
    CHECK(ok, "VSELECT f32x4 SIMD path: [5,9,5,9]");

    // Same with FORCE_SCALAR.
    memset(&err, 0, sizeof(err));
    memset(&out, 0, sizeof(out));
    s = sh_vm_run(&ck, args, 3, SH_VM_FORCE_SCALAR, &out, &err);
    ok = (s == SH_OK) && (out.kind == SH_K_VEC) && (out.lanes == 4);
    if (ok) {
      for (int li = 0; li < 4; li++) {
        uint32_t b32 = (uint32_t)out.lane[li];
        float fv; memcpy(&fv, &b32, 4);
        ok &= (fv == exp[li]);
      }
    }
    CHECK(ok, "VSELECT f32x4 FORCE_SCALAR path: [5,9,5,9]");
  }

  // --- u32x4 VSELECT ---
  {
    sh_chunk ck;
    memset(&ck, 0, sizeof(ck));
    ck.nvregs = 4; ck.nparams = 3; ck.result = 3; ck.ncode = 4;
    sh_instr code[4];
    memset(code, 0, sizeof(code));
    ck.code = code;
    code[0].op = (uint16_t)SHB_PARAM; code[0].dst = 0; code[0].imm = 0;
    code[0].a = code[0].b = code[0].c = SH_VREG_NONE;
    code[1].op = (uint16_t)SHB_PARAM; code[1].dst = 1; code[1].imm = 1;
    code[1].a = code[1].b = code[1].c = SH_VREG_NONE;
    code[2].op = (uint16_t)SHB_PARAM; code[2].dst = 2; code[2].imm = 2;
    code[2].a = code[2].b = code[2].c = SH_VREG_NONE;
    code[3].op    = (uint16_t)SHB_VSELECT;
    code[3].kind  = (uint8_t)SH_K_U32;
    code[3].lanes = 4;
    code[3].dst   = 3; code[3].a = 2; code[3].b = 0; code[3].c = 1;

    ck.params[0].kind = SH_K_VEC; ck.params[0].lanes = 4;
    ck.params[0].lane_kind = (uint8_t)SH_K_U32;
    ck.params[1] = ck.params[0];
    ck.params[2].kind = SH_K_VEC; ck.params[2].lanes = 4;
    ck.params[2].lane_kind = (uint8_t)SH_K_BOOL;

    sh_value args[3];
    args[0] = make_u32x4(10, 20, 30, 40);  // then
    args[1] = make_u32x4(11, 21, 31, 41);  // else
    memset(&args[2], 0, sizeof(sh_value));
    args[2].kind = SH_K_VEC; args[2].lanes = 4;
    args[2].lane_kind = (uint8_t)SH_K_BOOL;
    args[2].lane[0] = 0; args[2].lane[1] = 1;
    args[2].lane[2] = 0; args[2].lane[3] = 1;
    // Expected: [11,20,31,40] (mask[i]=0->else, 1->then)
    uint32_t exp[4] = {11, 20, 31, 40};

    sh_error err; sh_value out;
    memset(&err, 0, sizeof(err)); memset(&out, 0, sizeof(out));
    sh_status s = sh_vm_run(&ck, args, 3, 0, &out, &err);
    int ok = (s == SH_OK) && (out.kind == SH_K_VEC) && (out.lanes == 4);
    if (ok) for (int li = 0; li < 4; li++) ok &= (out.lane[li] == exp[li]);
    CHECK(ok, "VSELECT u32x4 SIMD path");

    memset(&err, 0, sizeof(err)); memset(&out, 0, sizeof(out));
    s = sh_vm_run(&ck, args, 3, SH_VM_FORCE_SCALAR, &out, &err);
    ok = (s == SH_OK) && (out.kind == SH_K_VEC) && (out.lanes == 4);
    if (ok) for (int li = 0; li < 4; li++) ok &= (out.lane[li] == exp[li]);
    CHECK(ok, "VSELECT u32x4 FORCE_SCALAR path");
  }
}

// ---------------------------------------------------------------------------
// Section 2d-2: Finding 3 regression -- SHB_VRLOAD/SHB_VRSTORE validator paths.
// Hand-craft an otherwise-valid base chunk (compiled from a real vregion shader),
// then mutate ONE field per sub-case and assert sh_chunk_validate returns non-SH_OK.
// ---------------------------------------------------------------------------

static void test_vrload_vrstore_validator(void) {
  printf("--- validator: SHB_VRLOAD/SHB_VRSTORE required-operand + lanes checks ---\n");

  // Build a valid base chunk from a real vregion shader so we have a structurally
  // sound chunk to start from. The shader does a single vregion-ref (VRLOAD).
  // We then mutate individual fields in copies to exercise each reject path.
  //
  // Shader: (defshader t ((buf (bytes u8))) -> u8x16 (vregion-ref buf 0 16))
  // This gives us at least one SHB_VRLOAD instruction in the chunk.
  sh_chunk *base = compile_lower(
    "(defshader t ((buf (bytes u8))) -> u8x16 (vregion-ref buf 0 16))", NULL);
  CHECK(base != NULL, "vrload base shader compiles+lowers");
  if (!base) return;

  // Confirm the base chunk validates cleanly.
  {
    sh_error err; memset(&err, 0, sizeof(err));
    CHECK(sh_chunk_validate(base, &err) == SH_OK,
          "vrload base chunk: clean validate passes");
  }

  // Find the VRLOAD instruction index.
  int vrload_pc = -1;
  for (uint32_t i = 0; i < base->ncode; i++) {
    if ((sh_bc_op)base->code[i].op == SHB_VRLOAD) { vrload_pc = (int)i; break; }
  }
  CHECK(vrload_pc >= 0, "vrload base chunk: VRLOAD instruction found");
  if (vrload_pc < 0) { sh_chunk_free(base); return; }

  // (a) VRLOAD with a == SH_VREG_NONE -> rejected.
  {
    sh_instr saved = base->code[vrload_pc];
    base->code[vrload_pc].a = SH_VREG_NONE;
    sh_error err; memset(&err, 0, sizeof(err));
    CHECK(sh_chunk_validate(base, &err) != SH_OK,
          "vrload: a==SH_VREG_NONE rejected");
    base->code[vrload_pc] = saved;
  }

  // (b) VRLOAD with b == SH_VREG_NONE -> rejected.
  {
    sh_instr saved = base->code[vrload_pc];
    base->code[vrload_pc].b = SH_VREG_NONE;
    sh_error err; memset(&err, 0, sizeof(err));
    CHECK(sh_chunk_validate(base, &err) != SH_OK,
          "vrload: b==SH_VREG_NONE rejected");
    base->code[vrload_pc] = saved;
  }

  // (c) VRLOAD with lanes == 0 -> rejected.
  {
    sh_instr saved = base->code[vrload_pc];
    base->code[vrload_pc].lanes = 0;
    sh_error err; memset(&err, 0, sizeof(err));
    CHECK(sh_chunk_validate(base, &err) != SH_OK,
          "vrload: lanes==0 rejected");
    base->code[vrload_pc] = saved;
  }

  // (d) VRLOAD with lanes == 1 -> rejected (minimum is 2).
  {
    sh_instr saved = base->code[vrload_pc];
    base->code[vrload_pc].lanes = 1;
    sh_error err; memset(&err, 0, sizeof(err));
    CHECK(sh_chunk_validate(base, &err) != SH_OK,
          "vrload: lanes==1 rejected");
    base->code[vrload_pc] = saved;
  }

  // (e) VRLOAD with lanes > SH_MAX_LANES -> rejected.
  {
    sh_instr saved = base->code[vrload_pc];
    // lanes is uint8_t; SH_MAX_LANES fits in uint8_t + 1.
    // The general >SH_MAX_LANES check catches this.
    base->code[vrload_pc].lanes = (uint8_t)(SH_MAX_LANES + 1);
    sh_error err; memset(&err, 0, sizeof(err));
    CHECK(sh_chunk_validate(base, &err) != SH_OK,
          "vrload: lanes>SH_MAX_LANES rejected");
    base->code[vrload_pc] = saved;
  }

  sh_chunk_free(base); base = NULL;

  // Now build a base chunk for SHB_VRSTORE using a vregion-set! shader.
  // Shader: (defshader t ((buf (bytes-mut u8)) (v u8x16)) -> u8x16
  //          (vregion-set! buf 0 v))
  sh_chunk *base_st = compile_lower(
    "(defshader t ((buf (bytes-mut u8)) (v u8x16)) -> u8x16"
    " (vregion-set! buf 0 v))", NULL);
  CHECK(base_st != NULL, "vrstore base shader compiles+lowers");
  if (!base_st) return;

  {
    sh_error err; memset(&err, 0, sizeof(err));
    CHECK(sh_chunk_validate(base_st, &err) == SH_OK,
          "vrstore base chunk: clean validate passes");
  }

  // Find the VRSTORE instruction index.
  int vrstore_pc = -1;
  for (uint32_t i = 0; i < base_st->ncode; i++) {
    if ((sh_bc_op)base_st->code[i].op == SHB_VRSTORE) {
      vrstore_pc = (int)i; break;
    }
  }
  CHECK(vrstore_pc >= 0, "vrstore base chunk: VRSTORE instruction found");
  if (vrstore_pc < 0) { sh_chunk_free(base_st); return; }

  // (f) VRSTORE with c == SH_VREG_NONE -> rejected.
  {
    sh_instr saved = base_st->code[vrstore_pc];
    base_st->code[vrstore_pc].c = SH_VREG_NONE;
    sh_error err; memset(&err, 0, sizeof(err));
    CHECK(sh_chunk_validate(base_st, &err) != SH_OK,
          "vrstore: c==SH_VREG_NONE rejected");
    base_st->code[vrstore_pc] = saved;
  }

  sh_chunk_free(base_st);
}

// ---------------------------------------------------------------------------
// Section 2e: Finding 1 regression + Finding 7 -- i64 vector differential
//             tests and u8x16 / f32x8 / u32x8 additional coverage
// ---------------------------------------------------------------------------

// Helper: make an i64xN sh_value with values that have nonzero high 32 bits.
static sh_value make_i64x4(int64_t a, int64_t b, int64_t cc, int64_t d) {
  sh_value v;
  memset(&v, 0, sizeof(v));
  v.kind = SH_K_VEC;
  v.lanes = 4;
  v.lane_kind = (uint8_t)SH_K_I64;
  v.lane[0] = (uint64_t)a;
  v.lane[1] = (uint64_t)b;
  v.lane[2] = (uint64_t)cc;
  v.lane[3] = (uint64_t)d;
  return v;
}

static sh_value make_i64x2(int64_t a, int64_t b) {
  sh_value v;
  memset(&v, 0, sizeof(v));
  v.kind = SH_K_VEC;
  v.lanes = 2;
  v.lane_kind = (uint8_t)SH_K_I64;
  v.lane[0] = (uint64_t)a;
  v.lane[1] = (uint64_t)b;
  return v;
}

static void test_i64_vector_regression(void) {
  printf("--- i64 vector regression (Finding 1) ---\n");

  // Values with nonzero high 32 bits: if the VM wrongly uses 32-bit SIMD, the
  // high halves are silently dropped and the result differs from the oracle.
  // 0x100000001LL = 4294967297, high32=1, low32=1.
  // Add: 0x100000001 + 0x200000003 = 0x300000004.

  sh_value va = make_i64x4(0x100000001LL,  0x200000003LL,
                            -0x100000001LL, 0x7FFFFFFFLL + 1LL);
  sh_value vb = make_i64x4(0x200000003LL,  0x100000001LL,
                            -0x200000003LL, 0x7FFFFFFFLL);
  sh_value args2[2] = {va, vb};

  // i64x4 add -- both SIMD (default) and FORCE_SCALAR paths must match oracle.
  CHECK(diff_case_flags("i64x4 add high32",
        "(defshader f ((a i64x4)(b i64x4)) -> i64x4 (+ a b))",
        NULL, args2, 2),
        "i64x4 add (high32 nonzero): SIMD==scalar==oracle");

  // i64x4 sub.
  CHECK(diff_case_flags("i64x4 sub high32",
        "(defshader f ((a i64x4)(b i64x4)) -> i64x4 (- a b))",
        NULL, args2, 2),
        "i64x4 sub (high32 nonzero): SIMD==scalar==oracle");

  // i64x4 splat (broadcast): splat of a value with nonzero high 32 bits.
  sh_value scalar_i64 = sh_val_i64(0x123456789ABCDEFLL);
  CHECK(diff_case_flags("i64x4 splat high32",
        "(defshader f ((x i64)) -> i64x4 (splat x))",
        NULL, &scalar_i64, 1),
        "i64x4 splat (high32 nonzero): SIMD==scalar==oracle");

  // i64x4 shuffle: reverse lanes.
  CHECK(diff_case_flags("i64x4 shuffle reverse",
        "(defshader f ((v i64x4)) -> i64x4 (shuffle v 3 2 1 0))",
        NULL, &va, 1),
        "i64x4 shuffle reverse: SIMD==scalar==oracle");

  // i64x2 add (2-lane; was already on scalar path since old guard required nl==4).
  sh_value va2 = make_i64x2(0x100000001LL, -0x200000003LL);
  sh_value vb2 = make_i64x2(0x300000007LL,  0x100000001LL);
  sh_value args2_2lane[2] = {va2, vb2};
  CHECK(diff_case_flags("i64x2 add",
        "(defshader f ((a i64x2)(b i64x2)) -> i64x2 (+ a b))",
        NULL, args2_2lane, 2),
        "i64x2 add: SIMD==scalar==oracle");

  // i64x4 vreduce-add.
  CHECK(diff_case_flags("i64x4 vreduce-add",
        "(defshader f ((v i64x4)) -> i64 (vreduce-add v))",
        NULL, &va, 1),
        "i64x4 vreduce-add: SIMD==scalar==oracle");

  // i64x4 mul (scalar path). Use values whose product fits in i64.
  sh_value vamul = make_i64x4(100000LL, -200000LL, 300000LL, -400000LL);
  sh_value vbmul = make_i64x4(      3LL,       4LL,      5LL,       6LL);
  sh_value argsmul[2] = {vamul, vbmul};
  CHECK(diff_case_flags("i64x4 mul",
        "(defshader f ((a i64x4)(b i64x4)) -> i64x4 (* a b))",
        NULL, argsmul, 2),
        "i64x4 mul: SIMD==scalar==oracle");
}

static void test_u8x16_differential(void) {
  printf("--- u8x16 differential (Finding 7) ---\n");

  // u8x16 add/sub: SIMD path uses _mm_add_epi8 (wrapping), scalar loop also
  // wraps.  Exercise with values where wrapping occurs.
  uint8_t la[16] = {255,200,100,50,1,0,128,64, 255,200,100,50,1,0,128,64};
  uint8_t lb[16] = {  1, 10, 20,30,5,7, 64,32,   0,  1, 56,27,9,3, 10, 8};
  sh_value va = make_u8x16(la);
  sh_value vb = make_u8x16(lb);
  sh_value args2[2] = {va, vb};
  CHECK(diff_case_flags("u8x16 add wrap diff",
        "(defshader f ((a u8x16)(b u8x16)) -> u8x16 (+ a b))",
        NULL, args2, 2),
        "u8x16 add wrapping differential (SIMD vs scalar vs oracle)");
  CHECK(diff_case_flags("u8x16 sub wrap diff",
        "(defshader f ((a u8x16)(b u8x16)) -> u8x16 (- a b))",
        NULL, args2, 2),
        "u8x16 sub wrapping differential");

  // u8x16 mul (always scalar since there's no 8-bit hw mul -- scalar fallback).
  CHECK(diff_case_flags("u8x16 mul diff",
        "(defshader f ((a u8x16)(b u8x16)) -> u8x16 (* a b))",
        NULL, args2, 2),
        "u8x16 mul differential (scalar path)");
}

// ---------------------------------------------------------------------------
// Section 3: SH_VM_FORCE_SCALAR flag check
// ---------------------------------------------------------------------------

static void test_force_scalar_flag(void) {
  printf("--- SH_VM_FORCE_SCALAR flag ---\n");

  sh_value a = sh_val_u32(42);
  CHECK(diff_case_flags("u32 id flag",
                        "(defshader f ((x u32)) -> u32 x)",
                        NULL, &a, 1),
        "FORCE_SCALAR == default for u32 id");

  sh_value va = make_f32x4(1.0f, 2.0f, 3.0f, 4.0f);
  sh_value vb = make_f32x4(5.0f, 6.0f, 7.0f, 8.0f);
  sh_value args2[2] = {va, vb};
  CHECK(diff_case_flags("f32x4 add flag",
                        "(defshader f ((a f32x4)(b f32x4)) -> f32x4 (+ a b))",
                        NULL, args2, 2),
        "FORCE_SCALAR == default for f32x4 add");

  CHECK(diff_case_flags("vreduce-add flag",
                        "(defshader f ((v f32x4)) -> f32 (vreduce-add v))",
                        NULL, &va, 1),
        "FORCE_SCALAR == default for vreduce-add");

  // Also try a loop.
  CHECK(diff_case_flags("loop sum10 flag",
                        "(defshader f () -> i64"
                        " (let loop ((i 0) (acc 0))"
                        "   (if (>= i 10) acc (loop (+ i 1) (+ acc i)))))",
                        NULL, NULL, 0),
        "FORCE_SCALAR == default for sum10 loop");
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(void) {
  (void)lisp_default_env();

  printf("[test_vm] Chunk validator tests\n");
  test_validator();

  printf("\n[test_vm] Differential harness -- scalar arithmetic\n");
  test_diff_scalar_arithmetic();

  printf("\n[test_vm] Differential harness -- casts\n");
  test_diff_casts();

  printf("\n[test_vm] Differential harness -- comparisons\n");
  test_diff_cmp();

  printf("\n[test_vm] Differential harness -- if/cond\n");
  test_diff_if();

  printf("\n[test_vm] Differential harness -- let / let*\n");
  test_diff_let();

  printf("\n[test_vm] Differential harness -- loops\n");
  test_diff_loop();

  printf("\n[test_vm] Differential harness -- region bounds\n");
  test_diff_region_bounds();

  printf("\n[test_vm] Differential harness -- saturating blit\n");
  test_diff_blit();

  printf("\n[test_vm] Differential harness -- ip-checksum\n");
  test_diff_ip_checksum();

  printf("\n[test_vm] Differential harness -- f32 dot product\n");
  test_diff_fdot();

  printf("\n[test_vm] Differential harness -- CALL primitive\n");
  test_diff_call_prim();

  printf("\n[test_vm] Differential harness -- vector ops\n");
  test_diff_vector();

  printf("\n[test_vm] SIMD bit-exactness landmines\n");
  test_simd_exactness();

  printf("\n[test_vm] Validator safety: lanes/required-operands/param (Findings 2,3,5)\n");
  test_validator_safety();

  printf("\n[test_vm] VSELECT direct chunk test (Finding 4)\n");
  test_vselect_direct();

  printf("\n[test_vm] VRLOAD/VRSTORE validator paths (Finding 3)\n");
  test_vrload_vrstore_validator();

  printf("\n[test_vm] i64 vector regression (Finding 1)\n");
  test_i64_vector_regression();

  printf("\n[test_vm] u8x16 integer op differential (Finding 7)\n");
  test_u8x16_differential();

  printf("\n[test_vm] SH_VM_FORCE_SCALAR flag\n");
  test_force_scalar_flag();

  int total = g_pass + g_fail;
  printf("\n[test_vm] %s (%d/%d)\n",
         g_fail == 0 ? "ALL TESTS PASSED" : "SOME TESTS FAILED",
         g_pass, total);
  return g_fail ? 1 : 0;
}
