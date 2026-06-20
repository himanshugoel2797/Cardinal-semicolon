// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

// S3.5 SLICE 1 -- saturating integer add/sub (sat+, sat-) test suite.
//
// Tests:
//   1. PARSE -- (sat+ a b)/(sat- a b) produce SH_OP_BINOP with SH_BIN_SADD/SSUB.
//   2. VERIFY -- accept integer sat+/sat-; reject float operands (SH_ERR_TYPE);
//      accept vector promotion (sat+ on u8x16 -> VBINOP).
//   3. INTERP (oracle) -- boundary correctness across u8/u16/u32/u64/i64.
//   4. DIFFERENTIAL -- compile->lower->vm_run (SIMD) AND vm_run
//      (FORCE_SCALAR) AND sh_invoke (oracle) all agree bit-for-bit.
//      Covers scalar u8/u16/u32/u64/i64 and vector u8x16/u16x8/u32x4.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lisp.h"
#include "sh_bytecode.h"
#include "sh_internal.h"

// ---------------------------------------------------------------------------
// Harness
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
// SECTION 1: Parse tests
// ---------------------------------------------------------------------------

static void test_parse(void) {
  printf("\n[parse]\n");
  sh_error err;
  memset(&err, 0, sizeof(err));

  // sat+ should produce SH_OP_BINOP with sub=SH_BIN_SADD
  {
    sh_program *p = NULL;
    sh_status s = sh_compile_string(
      "(defshader sat-add-parse ((a u8)(b u8)) -> u8 (sat+ a b))",
      NULL, 0, &p, &err);
    CHECK(s == SH_OK, "sat+ compiles ok");
    if (p) {
      // Root node should be BINOP with sub=SH_BIN_SADD
      sh_node *root = &p->nodes[p->root];
      CHECK(root->op == (uint16_t)SH_OP_BINOP, "sat+ -> SH_OP_BINOP");
      CHECK(root->sub == (uint16_t)SH_BIN_SADD, "sat+ -> SH_BIN_SADD");
      sh_free(p);
    }
  }

  // sat- should produce SH_OP_BINOP with sub=SH_BIN_SSUB
  {
    sh_program *p = NULL;
    sh_status s = sh_compile_string(
      "(defshader sat-sub-parse ((a u8)(b u8)) -> u8 (sat- a b))",
      NULL, 0, &p, &err);
    CHECK(s == SH_OK, "sat- compiles ok");
    if (p) {
      sh_node *root = &p->nodes[p->root];
      CHECK(root->op == (uint16_t)SH_OP_BINOP, "sat- -> SH_OP_BINOP");
      CHECK(root->sub == (uint16_t)SH_BIN_SSUB, "sat- -> SH_BIN_SSUB");
      sh_free(p);
    }
  }

  // sat+ with vector operands: verifier should promote to VBINOP
  {
    sh_program *p = NULL;
    sh_status s = sh_compile_string(
      "(defshader sat-add-vec ((a u8x16)(b u8x16)) -> u8x16 (sat+ a b))",
      NULL, 0, &p, &err);
    CHECK(s == SH_OK, "sat+ on u8x16 compiles ok");
    if (p) {
      sh_node *root = &p->nodes[p->root];
      // Verifier promotes BINOP->VBINOP when both operands are vectors
      CHECK(root->op == (uint16_t)SH_OP_VBINOP, "sat+ u8x16 -> SH_OP_VBINOP");
      CHECK(root->sub == (uint16_t)SH_BIN_SADD, "sat+ u8x16 -> SH_BIN_SADD");
      sh_free(p);
    }
  }
}

// ---------------------------------------------------------------------------
// SECTION 2: Verify tests
// ---------------------------------------------------------------------------

static void test_verify(void) {
  printf("\n[verify]\n");
  sh_error err;
  memset(&err, 0, sizeof(err));

  // Float operands must be rejected with SH_ERR_TYPE
  {
    sh_program *p = NULL;
    sh_status s = sh_compile_string(
      "(defshader sat-float-reject ((a f32)(b f32)) -> f32 (sat+ a b))",
      NULL, 0, &p, &err);
    CHECK(s == SH_ERR_TYPE, "sat+ on f32 -> SH_ERR_TYPE");
    if (p) sh_free(p);
  }

  // f64 likewise
  {
    sh_program *p = NULL;
    sh_status s = sh_compile_string(
      "(defshader sat-f64-reject ((a f64)(b f64)) -> f64 (sat- a b))",
      NULL, 0, &p, &err);
    CHECK(s == SH_ERR_TYPE, "sat- on f64 -> SH_ERR_TYPE");
    if (p) sh_free(p);
  }

  // Integer kinds accepted
  {
    static const char *int_kinds[] = {
      "(defshader s ((a u8)(b u8)) -> u8 (sat+ a b))",
      "(defshader s ((a u16)(b u16)) -> u16 (sat+ a b))",
      "(defshader s ((a u32)(b u32)) -> u32 (sat- a b))",
      "(defshader s ((a u64)(b u64)) -> u64 (sat- a b))",
      "(defshader s ((a i64)(b i64)) -> i64 (sat+ a b))",
      NULL
    };
    for (int i = 0; int_kinds[i]; i++) {
      sh_program *p = NULL;
      sh_status s = sh_compile_string(int_kinds[i], NULL, 0, &p, &err);
      CHECK(s == SH_OK, "integer kind accepted");
      if (p) sh_free(p);
    }
  }

  // Vector integer kinds accepted and promoted to VBINOP
  {
    sh_program *p = NULL;
    sh_status s = sh_compile_string(
      "(defshader s ((a u16x8)(b u16x8)) -> u16x8 (sat- a b))",
      NULL, 0, &p, &err);
    CHECK(s == SH_OK, "sat- on u16x8 ok");
    if (p) {
      sh_node *root = &p->nodes[p->root];
      CHECK(root->op == (uint16_t)SH_OP_VBINOP, "u16x8 sat- -> VBINOP");
      sh_free(p);
    }
  }
}

// ---------------------------------------------------------------------------
// SECTION 3: Interpreter oracle boundary tests
// ---------------------------------------------------------------------------

// Run a two-arg scalar shader and return the u64 result bits.
// Returns UINT64_MAX on failure.
static uint64_t run_scalar(const char *src, sh_value a, sh_value b) {
  sh_program *p = NULL;
  sh_error err;
  memset(&err, 0, sizeof(err));
  sh_status s = sh_compile_string(src, NULL, 0, &p, &err);
  if (s != SH_OK) { printf("  compile fail: %s\n", err.msg); return UINT64_MAX; }
  sh_value args[2] = {a, b};
  sh_value out;
  memset(&out, 0, sizeof(out));
  s = sh_invoke(p, args, 2, &out, &err);
  sh_free(p);
  if (s != SH_OK) { printf("  invoke fail: %s\n", err.msg); return UINT64_MAX; }
  return out.u;  // u/i share the same bytes; caller interprets
}

static void test_interp_oracle(void) {
  printf("\n[interp oracle boundaries]\n");

  // --- u8 ---
  // 255 sat+ 1 = 255 (clamp)
  CHECK(run_scalar("(defshader s ((a u8)(b u8)) -> u8 (sat+ a b))",
                   sh_val_u8(255), sh_val_u8(1)) == 255,
        "u8: 255 sat+ 1 = 255");
  // 0 sat- 1 = 0 (clamp)
  CHECK(run_scalar("(defshader s ((a u8)(b u8)) -> u8 (sat- a b))",
                   sh_val_u8(0), sh_val_u8(1)) == 0,
        "u8: 0 sat- 1 = 0");
  // 200 sat+ 100 = 255
  CHECK(run_scalar("(defshader s ((a u8)(b u8)) -> u8 (sat+ a b))",
                   sh_val_u8(200), sh_val_u8(100)) == 255,
        "u8: 200 sat+ 100 = 255");
  // 100 sat+ 50 = 150 (no clamp)
  CHECK(run_scalar("(defshader s ((a u8)(b u8)) -> u8 (sat+ a b))",
                   sh_val_u8(100), sh_val_u8(50)) == 150,
        "u8: 100 sat+ 50 = 150");
  // 100 sat- 50 = 50 (no clamp)
  CHECK(run_scalar("(defshader s ((a u8)(b u8)) -> u8 (sat- a b))",
                   sh_val_u8(100), sh_val_u8(50)) == 50,
        "u8: 100 sat- 50 = 50");
  // 5 sat- 10 = 0
  CHECK(run_scalar("(defshader s ((a u8)(b u8)) -> u8 (sat- a b))",
                   sh_val_u8(5), sh_val_u8(10)) == 0,
        "u8: 5 sat- 10 = 0");
  // 128 sat+ 128 = 255
  CHECK(run_scalar("(defshader s ((a u8)(b u8)) -> u8 (sat+ a b))",
                   sh_val_u8(128), sh_val_u8(128)) == 255,
        "u8: 128 sat+ 128 = 255");

  // --- u16 ---
  CHECK(run_scalar("(defshader s ((a u16)(b u16)) -> u16 (sat+ a b))",
                   sh_val_u16(65535), sh_val_u16(1)) == 65535,
        "u16: 65535 sat+ 1 = 65535");
  CHECK(run_scalar("(defshader s ((a u16)(b u16)) -> u16 (sat- a b))",
                   sh_val_u16(0), sh_val_u16(1)) == 0,
        "u16: 0 sat- 1 = 0");
  CHECK(run_scalar("(defshader s ((a u16)(b u16)) -> u16 (sat+ a b))",
                   sh_val_u16(60000), sh_val_u16(6000)) == 65535,
        "u16: 60000 sat+ 6000 = 65535");
  CHECK(run_scalar("(defshader s ((a u16)(b u16)) -> u16 (sat+ a b))",
                   sh_val_u16(1000), sh_val_u16(2000)) == 3000,
        "u16: 1000 sat+ 2000 = 3000");

  // --- u32 ---
  CHECK(run_scalar("(defshader s ((a u32)(b u32)) -> u32 (sat+ a b))",
                   sh_val_u32(0xFFFFFFFFu), sh_val_u32(1)) == 0xFFFFFFFFu,
        "u32: MAX sat+ 1 = MAX");
  CHECK(run_scalar("(defshader s ((a u32)(b u32)) -> u32 (sat- a b))",
                   sh_val_u32(0), sh_val_u32(1)) == 0,
        "u32: 0 sat- 1 = 0");
  CHECK(run_scalar("(defshader s ((a u32)(b u32)) -> u32 (sat+ a b))",
                   sh_val_u32(100), sh_val_u32(200)) == 300,
        "u32: 100 sat+ 200 = 300");
  CHECK(run_scalar("(defshader s ((a u32)(b u32)) -> u32 (sat+ a b))",
                   sh_val_u32(0xFFFFFFF0u), sh_val_u32(0x20u)) == 0xFFFFFFFFu,
        "u32: near-max sat+ = MAX");

  // --- u64 ---
  {
    uint64_t max64 = UINT64_MAX;
    sh_value a, b;
    memset(&a, 0, sizeof(a)); a.kind = SH_K_U64; a.lanes = 1; a.u = max64;
    memset(&b, 0, sizeof(b)); b.kind = SH_K_U64; b.lanes = 1; b.u = 1;
    CHECK(run_scalar("(defshader s ((a u64)(b u64)) -> u64 (sat+ a b))", a, b)
          == UINT64_MAX, "u64: MAX sat+ 1 = MAX");

    memset(&a, 0, sizeof(a)); a.kind = SH_K_U64; a.lanes = 1; a.u = 0;
    memset(&b, 0, sizeof(b)); b.kind = SH_K_U64; b.lanes = 1; b.u = 1;
    CHECK(run_scalar("(defshader s ((a u64)(b u64)) -> u64 (sat- a b))", a, b)
          == 0, "u64: 0 sat- 1 = 0");
  }

  // --- i64 ---
  {
    sh_value a, b;
    // INT64_MAX sat+ 1 = INT64_MAX
    memset(&a, 0, sizeof(a)); a.kind = SH_K_I64; a.lanes = 1; a.i = INT64_MAX;
    memset(&b, 0, sizeof(b)); b.kind = SH_K_I64; b.lanes = 1; b.i = 1;
    uint64_t r = run_scalar("(defshader s ((a i64)(b i64)) -> i64 (sat+ a b))", a, b);
    CHECK((int64_t)r == INT64_MAX, "i64: INT64_MAX sat+ 1 = INT64_MAX");

    // INT64_MIN sat- 1 = INT64_MIN
    memset(&a, 0, sizeof(a)); a.kind = SH_K_I64; a.lanes = 1; a.i = INT64_MIN;
    memset(&b, 0, sizeof(b)); b.kind = SH_K_I64; b.lanes = 1; b.i = 1;
    r = run_scalar("(defshader s ((a i64)(b i64)) -> i64 (sat- a b))", a, b);
    CHECK((int64_t)r == INT64_MIN, "i64: INT64_MIN sat- 1 = INT64_MIN");

    // Normal (non-saturating) i64 add: 10 sat+ 20 = 30
    memset(&a, 0, sizeof(a)); a.kind = SH_K_I64; a.lanes = 1; a.i = 10;
    memset(&b, 0, sizeof(b)); b.kind = SH_K_I64; b.lanes = 1; b.i = 20;
    r = run_scalar("(defshader s ((a i64)(b i64)) -> i64 (sat+ a b))", a, b);
    CHECK((int64_t)r == 30, "i64: 10 sat+ 20 = 30");

    // Normal sub: 10 sat- 5 = 5
    memset(&a, 0, sizeof(a)); a.kind = SH_K_I64; a.lanes = 1; a.i = 10;
    memset(&b, 0, sizeof(b)); b.kind = SH_K_I64; b.lanes = 1; b.i = 5;
    r = run_scalar("(defshader s ((a i64)(b i64)) -> i64 (sat- a b))", a, b);
    CHECK((int64_t)r == 5, "i64: 10 sat- 5 = 5");

    // Negative sat+: -5 sat+ -3 = -8 (no clamp, both same sign but result still in range)
    memset(&a, 0, sizeof(a)); a.kind = SH_K_I64; a.lanes = 1; a.i = -5;
    memset(&b, 0, sizeof(b)); b.kind = SH_K_I64; b.lanes = 1; b.i = -3;
    r = run_scalar("(defshader s ((a i64)(b i64)) -> i64 (sat+ a b))", a, b);
    CHECK((int64_t)r == -8, "i64: -5 sat+ -3 = -8");

    // INT64_MIN sat+ INT64_MIN = INT64_MIN (two negatives overflow to min)
    memset(&a, 0, sizeof(a)); a.kind = SH_K_I64; a.lanes = 1; a.i = INT64_MIN;
    memset(&b, 0, sizeof(b)); b.kind = SH_K_I64; b.lanes = 1; b.i = INT64_MIN;
    r = run_scalar("(defshader s ((a i64)(b i64)) -> i64 (sat+ a b))", a, b);
    CHECK((int64_t)r == INT64_MIN, "i64: INT64_MIN sat+ INT64_MIN = INT64_MIN");

    // INT64_MAX sat- INT64_MIN: positive - very_negative = would overflow -> INT64_MAX
    memset(&a, 0, sizeof(a)); a.kind = SH_K_I64; a.lanes = 1; a.i = INT64_MAX;
    memset(&b, 0, sizeof(b)); b.kind = SH_K_I64; b.lanes = 1; b.i = INT64_MIN;
    r = run_scalar("(defshader s ((a i64)(b i64)) -> i64 (sat- a b))", a, b);
    CHECK((int64_t)r == INT64_MAX, "i64: INT64_MAX sat- INT64_MIN = INT64_MAX");
  }
}

// ---------------------------------------------------------------------------
// SECTION 4: Differential -- SIMD VM == FORCE_SCALAR VM == oracle
// ---------------------------------------------------------------------------

// Test result comparison: two sh_values bit-for-bit equal
static int values_equal(sh_value a, sh_value b) {
  if (a.kind != b.kind) return 0;
  if (a.lanes != b.lanes) return 0;
  if (a.kind == SH_K_VEC) {
    if (a.lane_kind != b.lane_kind) return 0;
    sh_kind lk = (sh_kind)a.lane_kind;
    uint32_t esz = sh_kind_size(lk);
    if (esz == 0) esz = 1;
    for (int li = 0; li < a.lanes; li++) {
      if (a.lane[li] != b.lane[li]) return 0;
    }
    return 1;
  }
  // Scalar: compare the raw 64-bit pattern
  if (a.kind == SH_K_I64) return a.i == b.i;
  if (a.kind == SH_K_F32 || a.kind == SH_K_F64) {
    // Use bit comparison for floats too (NaN-aware identity)
    uint64_t fa, fb;
    memcpy(&fa, &a.f, 8);
    memcpy(&fb, &b.f, 8);
    return fa == fb;
  }
  return a.u == b.u;
}

// Run a differential test: compile src, lower to chunk, run with SIMD,
// run with FORCE_SCALAR, run oracle (sh_invoke). Check all three equal.
// Returns 1 on success, 0 on any mismatch or error.
static int diff_test(const char *label, const char *src,
                     sh_value *args, uint32_t argc) {
  sh_error err;
  memset(&err, 0, sizeof(err));

  // Compile + verify
  sh_program *prog = NULL;
  sh_status s = sh_compile_string(src, NULL, 0, &prog, &err);
  if (s != SH_OK) {
    printf("  [diff:%s] compile fail: %s\n", label, err.msg);
    return 0;
  }

  // Oracle
  sh_value oracle_out;
  memset(&oracle_out, 0, sizeof(oracle_out));
  s = sh_invoke(prog, args, argc, &oracle_out, &err);
  if (s != SH_OK) {
    printf("  [diff:%s] oracle fail: %s\n", label, err.msg);
    sh_free(prog);
    return 0;
  }

  // Lower
  sh_chunk *c = NULL;
  s = sh_lower(prog, &c, &err);
  sh_free(prog);
  if (s != SH_OK) {
    printf("  [diff:%s] lower fail: %s\n", label, err.msg);
    return 0;
  }

  // VM SIMD
  sh_value vm_simd_out;
  memset(&vm_simd_out, 0, sizeof(vm_simd_out));
  s = sh_vm_run(c, args, argc, 0, &vm_simd_out, &err);
  if (s != SH_OK) {
    printf("  [diff:%s] vm(SIMD) fail: %s\n", label, err.msg);
    sh_chunk_free(c);
    return 0;
  }

  // VM FORCE_SCALAR
  sh_value vm_scalar_out;
  memset(&vm_scalar_out, 0, sizeof(vm_scalar_out));
  s = sh_vm_run(c, args, argc, SH_VM_FORCE_SCALAR, &vm_scalar_out, &err);
  sh_chunk_free(c);
  if (s != SH_OK) {
    printf("  [diff:%s] vm(SCALAR) fail: %s\n", label, err.msg);
    return 0;
  }

  int ok_simd   = values_equal(oracle_out, vm_simd_out);
  int ok_scalar = values_equal(oracle_out, vm_scalar_out);

  if (!ok_simd) {
    printf("  [diff:%s] SIMD != oracle\n", label);
    if (oracle_out.kind == SH_K_VEC) {
      for (int li = 0; li < oracle_out.lanes; li++) {
        printf("    lane[%d]: oracle=%llu simd=%llu\n", li,
               (unsigned long long)oracle_out.lane[li],
               (unsigned long long)vm_simd_out.lane[li]);
      }
    } else {
      printf("    oracle.u=%llu simd.u=%llu\n",
             (unsigned long long)oracle_out.u,
             (unsigned long long)vm_simd_out.u);
    }
  }
  if (!ok_scalar) {
    printf("  [diff:%s] FORCE_SCALAR != oracle\n", label);
  }

  return ok_simd && ok_scalar;
}

// Build a vector sh_value with all lanes set to v, lane_kind=lk, lanes=nl.
static sh_value make_vec_splat(sh_kind lk, uint8_t nl, uint64_t v) {
  sh_value sv;
  memset(&sv, 0, sizeof(sv));
  sv.kind      = SH_K_VEC;
  sv.lanes     = nl;
  sv.lane_kind = (uint8_t)lk;
  for (uint8_t li = 0; li < nl; li++) sv.lane[li] = v;
  return sv;
}

// Build a vector with each lane = base + li*step (truncated to lane width).
static sh_value make_vec_ramp(sh_kind lk, uint8_t nl,
                               uint64_t base, uint64_t step) {
  sh_value sv;
  memset(&sv, 0, sizeof(sv));
  sv.kind      = SH_K_VEC;
  sv.lanes     = nl;
  sv.lane_kind = (uint8_t)lk;
  for (uint8_t li = 0; li < nl; li++) {
    uint64_t v = base + (uint64_t)li * step;
    // Mask to lane width
    switch (lk) {
      case SH_K_U8:  v &= 0xFFu; break;
      case SH_K_U16: v &= 0xFFFFu; break;
      case SH_K_U32: v &= 0xFFFFFFFFu; break;
      default: break;
    }
    sv.lane[li] = v;
  }
  return sv;
}

static void test_differential(void) {
  printf("\n[differential: SIMD == SCALAR == oracle]\n");
  int ok;

  // --- Scalar u8 sat+ non-saturating ---
  {
    sh_value args[2] = {sh_val_u8(10), sh_val_u8(20)};
    ok = diff_test("u8 sat+ no-clamp",
                   "(defshader s ((a u8)(b u8)) -> u8 (sat+ a b))", args, 2);
    CHECK(ok, "diff u8: 10 sat+ 20 = 30");
  }
  // --- Scalar u8 sat+ saturating ---
  {
    sh_value args[2] = {sh_val_u8(200), sh_val_u8(100)};
    ok = diff_test("u8 sat+ clamp",
                   "(defshader s ((a u8)(b u8)) -> u8 (sat+ a b))", args, 2);
    CHECK(ok, "diff u8: 200 sat+ 100 = 255");
  }
  // --- Scalar u8 sat- non-saturating ---
  {
    sh_value args[2] = {sh_val_u8(100), sh_val_u8(50)};
    ok = diff_test("u8 sat- no-clamp",
                   "(defshader s ((a u8)(b u8)) -> u8 (sat- a b))", args, 2);
    CHECK(ok, "diff u8: 100 sat- 50 = 50");
  }
  // --- Scalar u8 sat- saturating ---
  {
    sh_value args[2] = {sh_val_u8(5), sh_val_u8(10)};
    ok = diff_test("u8 sat- clamp",
                   "(defshader s ((a u8)(b u8)) -> u8 (sat- a b))", args, 2);
    CHECK(ok, "diff u8: 5 sat- 10 = 0");
  }
  // --- Scalar u16 boundaries ---
  {
    sh_value args[2] = {sh_val_u16(65000), sh_val_u16(600)};
    ok = diff_test("u16 sat+ clamp",
                   "(defshader s ((a u16)(b u16)) -> u16 (sat+ a b))", args, 2);
    CHECK(ok, "diff u16: 65000 sat+ 600 = 65535");
  }
  {
    sh_value args[2] = {sh_val_u16(100), sh_val_u16(200)};
    ok = diff_test("u16 sat- clamp",
                   "(defshader s ((a u16)(b u16)) -> u16 (sat- a b))", args, 2);
    CHECK(ok, "diff u16: 100 sat- 200 = 0");
  }
  // --- Scalar u32 ---
  {
    sh_value args[2] = {sh_val_u32(0xFFFFFFF0u), sh_val_u32(0x100u)};
    ok = diff_test("u32 sat+ clamp",
                   "(defshader s ((a u32)(b u32)) -> u32 (sat+ a b))", args, 2);
    CHECK(ok, "diff u32: near-max sat+ = MAX");
  }
  {
    sh_value args[2] = {sh_val_u32(1000), sh_val_u32(500)};
    ok = diff_test("u32 sat- no-clamp",
                   "(defshader s ((a u32)(b u32)) -> u32 (sat- a b))", args, 2);
    CHECK(ok, "diff u32: 1000 sat- 500 = 500");
  }
  // --- Scalar i64 ---
  {
    sh_value a, b;
    memset(&a, 0, sizeof(a)); a.kind = SH_K_I64; a.lanes = 1; a.i = INT64_MAX;
    memset(&b, 0, sizeof(b)); b.kind = SH_K_I64; b.lanes = 1; b.i = 1;
    sh_value args[2] = {a, b};
    ok = diff_test("i64 sat+ MAX",
                   "(defshader s ((a i64)(b i64)) -> i64 (sat+ a b))", args, 2);
    CHECK(ok, "diff i64: INT64_MAX sat+ 1 = INT64_MAX");
  }
  {
    sh_value a, b;
    memset(&a, 0, sizeof(a)); a.kind = SH_K_I64; a.lanes = 1; a.i = INT64_MIN;
    memset(&b, 0, sizeof(b)); b.kind = SH_K_I64; b.lanes = 1; b.i = 1;
    sh_value args[2] = {a, b};
    ok = diff_test("i64 sat- MIN",
                   "(defshader s ((a i64)(b i64)) -> i64 (sat- a b))", args, 2);
    CHECK(ok, "diff i64: INT64_MIN sat- 1 = INT64_MIN");
  }
  {
    sh_value a, b;
    memset(&a, 0, sizeof(a)); a.kind = SH_K_I64; a.lanes = 1; a.i = 100;
    memset(&b, 0, sizeof(b)); b.kind = SH_K_I64; b.lanes = 1; b.i = 200;
    sh_value args[2] = {a, b};
    ok = diff_test("i64 sat+ no-clamp",
                   "(defshader s ((a i64)(b i64)) -> i64 (sat+ a b))", args, 2);
    CHECK(ok, "diff i64: 100 sat+ 200 = 300");
  }

  // --- Vector u8x16 sat+ (uses SSE _mm_adds_epu8 path) ---
  {
    // All lanes = 200; sat+ 100 -> all 255
    sh_value a = make_vec_splat(SH_K_U8, 16, 200);
    sh_value b = make_vec_splat(SH_K_U8, 16, 100);
    sh_value args[2] = {a, b};
    ok = diff_test("u8x16 sat+ clamp all-lanes",
                   "(defshader s ((a u8x16)(b u8x16)) -> u8x16 (sat+ a b))",
                   args, 2);
    CHECK(ok, "diff u8x16: 200 sat+ 100 = 255 all lanes");
  }
  {
    // Mixed lanes: ramp 240+, and add 10 -> some clamp some not
    sh_value a = make_vec_ramp(SH_K_U8, 16, 240, 1);  // 240,241,...,255
    sh_value b = make_vec_splat(SH_K_U8, 16, 10);
    sh_value args[2] = {a, b};
    ok = diff_test("u8x16 sat+ mixed",
                   "(defshader s ((a u8x16)(b u8x16)) -> u8x16 (sat+ a b))",
                   args, 2);
    CHECK(ok, "diff u8x16: ramp sat+ 10 mixed lanes");
  }
  {
    // sat-: all lanes 5, subtract 10 -> all 0
    sh_value a = make_vec_splat(SH_K_U8, 16, 5);
    sh_value b = make_vec_splat(SH_K_U8, 16, 10);
    sh_value args[2] = {a, b};
    ok = diff_test("u8x16 sat- clamp",
                   "(defshader s ((a u8x16)(b u8x16)) -> u8x16 (sat- a b))",
                   args, 2);
    CHECK(ok, "diff u8x16: 5 sat- 10 = 0 all lanes");
  }
  {
    // sat-: ramp 0..15, subtract 8 -> clamp on first 8 lanes
    sh_value a = make_vec_ramp(SH_K_U8, 16, 0, 1);  // 0,1,...,15
    sh_value b = make_vec_splat(SH_K_U8, 16, 8);
    sh_value args[2] = {a, b};
    ok = diff_test("u8x16 sat- mixed",
                   "(defshader s ((a u8x16)(b u8x16)) -> u8x16 (sat- a b))",
                   args, 2);
    CHECK(ok, "diff u8x16: ramp sat- 8 mixed lanes");
  }

  // --- Vector u16x8 sat+ (uses SSE _mm_adds_epu16 path) ---
  {
    sh_value a = make_vec_splat(SH_K_U16, 8, 65000);
    sh_value b = make_vec_splat(SH_K_U16, 8, 600);
    sh_value args[2] = {a, b};
    ok = diff_test("u16x8 sat+ clamp",
                   "(defshader s ((a u16x8)(b u16x8)) -> u16x8 (sat+ a b))",
                   args, 2);
    CHECK(ok, "diff u16x8: 65000 sat+ 600 = 65535 all lanes");
  }
  {
    sh_value a = make_vec_ramp(SH_K_U16, 8, 65500, 1);  // 65500..65507
    sh_value b = make_vec_splat(SH_K_U16, 8, 100);
    sh_value args[2] = {a, b};
    ok = diff_test("u16x8 sat+ mixed",
                   "(defshader s ((a u16x8)(b u16x8)) -> u16x8 (sat+ a b))",
                   args, 2);
    CHECK(ok, "diff u16x8: ramp sat+ 100 mixed lanes");
  }
  {
    sh_value a = make_vec_splat(SH_K_U16, 8, 50);
    sh_value b = make_vec_splat(SH_K_U16, 8, 100);
    sh_value args[2] = {a, b};
    ok = diff_test("u16x8 sat- clamp",
                   "(defshader s ((a u16x8)(b u16x8)) -> u16x8 (sat- a b))",
                   args, 2);
    CHECK(ok, "diff u16x8: 50 sat- 100 = 0 all lanes");
  }

  // --- Vector u32x4 sat+ (scalar fallback: no SSE saturating 32-bit) ---
  {
    sh_value a = make_vec_splat(SH_K_U32, 4, 0xFFFFFFF0u);
    sh_value b = make_vec_splat(SH_K_U32, 4, 0x20u);
    sh_value args[2] = {a, b};
    ok = diff_test("u32x4 sat+ clamp (scalar fb)",
                   "(defshader s ((a u32x4)(b u32x4)) -> u32x4 (sat+ a b))",
                   args, 2);
    CHECK(ok, "diff u32x4: near-max sat+ = MAX all lanes");
  }
  {
    sh_value a = make_vec_ramp(SH_K_U32, 4, 100, 100);  // 100,200,300,400
    sh_value b = make_vec_splat(SH_K_U32, 4, 50);
    sh_value args[2] = {a, b};
    ok = diff_test("u32x4 sat+ no-clamp (scalar fb)",
                   "(defshader s ((a u32x4)(b u32x4)) -> u32x4 (sat+ a b))",
                   args, 2);
    CHECK(ok, "diff u32x4: ramp sat+ 50 no-clamp");
  }

  // --- Normal (non-saturating) values equal plain add/sub ---
  {
    sh_value args[2] = {sh_val_u8(10), sh_val_u8(20)};
    ok = diff_test("sat+ equals plain + when no clamp",
                   "(defshader s ((a u8)(b u8)) -> u8 (sat+ a b))", args, 2);
    CHECK(ok, "diff u8: sat+ equals plain + for no-overflow values");
  }
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(void) {
  // Initialize the Lisp runtime (needed for the reader used by sh_compile_string).
  (void)lisp_default_env();

  printf("=== test_saturate: S3.5 slice 1: sat+/sat- ===\n");

  test_parse();
  test_verify();
  test_interp_oracle();
  test_differential();

  printf("\n=== SUMMARY: %d passed, %d failed ===\n", g_pass, g_fail);
  return g_fail > 0 ? 1 : 0;
}
