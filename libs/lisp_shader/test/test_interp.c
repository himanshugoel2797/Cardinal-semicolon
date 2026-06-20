// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

// UNIT C test suite: reference interpreter.
// Tests shi_invoke / sh_invoke via the full sh_compile_string -> sh_invoke pipeline.
// Covers: scalar arithmetic across kinds, casts, comparisons, if/cond,
// let/let* nesting, bounded loops over regions, CALL primitives, every vector
// op vs a hand-written lane loop, and differential kernels (ip-checksum,
// saturating-add blit, dot product).
//
// Run with: bash libs/lisp_shader/test/build-and-run.sh

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lisp.h"
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

// Compile-and-invoke helper: zero prims, no flags.
static sh_status invoke0(const char *src, const sh_value *args, uint32_t argc,
                         sh_value *out, sh_error *err) {
  sh_program *p = NULL;
  sh_status s = sh_compile_string(src, NULL, 0, &p, err);
  if (s != SH_OK) return s;
  s = sh_invoke(p, args, argc, out, err);
  sh_free(p);
  return s;
}

// Compile-and-invoke with a prim set.
static sh_status invoke_prims(const char *src, const sh_prim_set *prims,
                              const sh_value *args, uint32_t argc,
                              sh_value *out, sh_error *err) {
  sh_program *p = NULL;
  sh_status s = sh_compile_string(src, prims, 0, &p, err);
  if (s != SH_OK) return s;
  s = sh_invoke(p, args, argc, out, err);
  sh_free(p);
  return s;
}

// ---------------------------------------------------------------------------
// Section 1: Scalar identity and arithmetic
// ---------------------------------------------------------------------------

static void test_scalar_identity(void) {
  printf("--- scalar identity ---\n");
  sh_error err;
  sh_value out;

  sh_value a = sh_val_u32(42);
  CHECK(invoke0("(defshader id ((x u32)) -> u32 x)", &a, 1, &out, &err) == SH_OK
        && out.u == 42, "u32 identity");

  sh_value b = sh_val_i64(-7);
  CHECK(invoke0("(defshader id ((x i64)) -> i64 x)", &b, 1, &out, &err) == SH_OK
        && out.i == -7, "i64 identity");

  sh_value c = sh_val_f32(3.14f);
  CHECK(invoke0("(defshader id ((x f32)) -> f32 x)", &c, 1, &out, &err) == SH_OK
        && (float)out.f == 3.14f, "f32 identity");

  sh_value d = sh_val_bool(true);
  CHECK(invoke0("(defshader id ((x bool)) -> bool x)", &d, 1, &out, &err) == SH_OK
        && out.u == 1, "bool identity true");
}

static void test_scalar_arithmetic(void) {
  printf("--- scalar arithmetic ---\n");
  sh_error err;
  sh_value out;
  sh_value args[2];

  // u32 basic ops
  args[0] = sh_val_u32(10); args[1] = sh_val_u32(3);
  CHECK(invoke0("(defshader add ((a u32)(b u32)) -> u32 (+ a b))",
                args, 2, &out, &err) == SH_OK && out.u == 13, "u32 add");
  CHECK(invoke0("(defshader sub ((a u32)(b u32)) -> u32 (- a b))",
                args, 2, &out, &err) == SH_OK && out.u == 7, "u32 sub");
  CHECK(invoke0("(defshader mul ((a u32)(b u32)) -> u32 (* a b))",
                args, 2, &out, &err) == SH_OK && out.u == 30, "u32 mul");
  CHECK(invoke0("(defshader div ((a u32)(b u32)) -> u32 (/ a b))",
                args, 2, &out, &err) == SH_OK && out.u == 3, "u32 div");
  CHECK(invoke0("(defshader mod ((a u32)(b u32)) -> u32 (mod a b))",
                args, 2, &out, &err) == SH_OK && out.u == 1, "u32 mod");

  // i64 arithmetic
  args[0] = sh_val_i64(-100); args[1] = sh_val_i64(7);
  CHECK(invoke0("(defshader div ((a i64)(b i64)) -> i64 (/ a b))",
                args, 2, &out, &err) == SH_OK && out.i == -14,
        "i64 div truncate toward zero");
  CHECK(invoke0("(defshader mod ((a i64)(b i64)) -> i64 (mod a b))",
                args, 2, &out, &err) == SH_OK && out.i == -2, "i64 mod");

  // u32 wrap (2^32 - 1 + 1 = 0)
  args[0] = sh_val_u32(0xFFFFFFFFu); args[1] = sh_val_u32(1);
  CHECK(invoke0("(defshader add ((a u32)(b u32)) -> u32 (+ a b))",
                args, 2, &out, &err) == SH_OK && out.u == 0, "u32 wraparound");

  // f32 narrowing
  args[0] = sh_val_f32(1.0f / 3.0f); args[1] = sh_val_f32(1.0f);
  CHECK(invoke0("(defshader add ((a f32)(b f32)) -> f32 (+ a b))",
                args, 2, &out, &err) == SH_OK
        && (float)out.f == (float)(1.0f / 3.0f + 1.0f), "f32 add narrowed");

  // Bitwise ops
  args[0] = sh_val_u32(0xF0); args[1] = sh_val_u32(0xFF);
  CHECK(invoke0("(defshader and ((a u32)(b u32)) -> u32 (bit-and a b))",
                args, 2, &out, &err) == SH_OK && out.u == 0xF0, "u32 bit-and");
  CHECK(invoke0("(defshader or ((a u32)(b u32)) -> u32 (bit-or a b))",
                args, 2, &out, &err) == SH_OK && out.u == 0xFF, "u32 bit-or");
  CHECK(invoke0("(defshader xor ((a u32)(b u32)) -> u32 (bit-xor a b))",
                args, 2, &out, &err) == SH_OK && out.u == 0x0F, "u32 bit-xor");

  // Shift ops
  args[0] = sh_val_u32(1); args[1] = sh_val_u32(4);
  CHECK(invoke0("(defshader shl ((a u32)(b u32)) -> u32 (shl a b))",
                args, 2, &out, &err) == SH_OK && out.u == 16, "u32 shl");
  args[0] = sh_val_u32(256); args[1] = sh_val_u32(3);
  CHECK(invoke0("(defshader shr ((a u32)(b u32)) -> u32 (shr a b))",
                args, 2, &out, &err) == SH_OK && out.u == 32, "u32 shr");

  // Unary negation
  sh_value neg_a = sh_val_i64(5);
  CHECK(invoke0("(defshader neg ((a i64)) -> i64 (- a))",
                &neg_a, 1, &out, &err) == SH_OK && out.i == -5, "i64 neg");

  // Division by zero: result 0, no trap
  args[0] = sh_val_u32(10); args[1] = sh_val_u32(0);
  CHECK(invoke0("(defshader divz ((a u32)(b u32)) -> u32 (/ a b))",
                args, 2, &out, &err) == SH_OK && out.u == 0,
        "u32 div by zero -> 0");
}

static void test_casts(void) {
  printf("--- casts ---\n");
  sh_error err;
  sh_value out;
  sh_value a;

  // u32 -> f32
  a = sh_val_u32(7);
  CHECK(invoke0("(defshader cvt ((x u32)) -> f32 (f32 x))", &a, 1, &out, &err) == SH_OK
        && (float)out.f == 7.0f, "u32->f32");

  // f64 -> i64 (truncate toward zero)
  a = sh_val_f64(-3.9);
  CHECK(invoke0("(defshader cvt ((x f64)) -> i64 (i64 x))", &a, 1, &out, &err) == SH_OK
        && out.i == -3, "f64->i64 truncate");

  // f64 -> f32 narrowing
  a = sh_val_f64(1.0 / 3.0);
  CHECK(invoke0("(defshader cvt ((x f64)) -> f32 (f32 x))", &a, 1, &out, &err) == SH_OK
        && (float)out.f == (float)(1.0 / 3.0), "f64->f32 narrow");

  // u32 -> u8 truncate
  a = sh_val_u32(257);
  CHECK(invoke0("(defshader cvt ((x u32)) -> u8 (u8 x))", &a, 1, &out, &err) == SH_OK
        && out.u == 1, "u32->u8 truncate");

  // bool -> u32
  a = sh_val_bool(true);
  CHECK(invoke0("(defshader cvt ((x bool)) -> u32 (u32 x))", &a, 1, &out, &err) == SH_OK
        && out.u == 1, "bool->u32");

  // u64 -> f64 (large unsigned value)
  a = sh_val_u64(1000000000ULL);
  CHECK(invoke0("(defshader cvt ((x u64)) -> f64 (f64 x))", &a, 1, &out, &err) == SH_OK
        && out.f == 1000000000.0, "u64->f64");

  // i64 -> f32
  a = sh_val_i64(-42);
  CHECK(invoke0("(defshader cvt ((x i64)) -> f32 (f32 x))", &a, 1, &out, &err) == SH_OK
        && (float)out.f == -42.0f, "i64->f32");
}

static void test_comparisons(void) {
  printf("--- comparisons ---\n");
  sh_error err;
  sh_value out;
  sh_value args[2];

  args[0] = sh_val_u32(5); args[1] = sh_val_u32(10);
  CHECK(invoke0("(defshader cmp ((a u32)(b u32)) -> bool (< a b))",
                args, 2, &out, &err) == SH_OK && out.u == 1, "u32 < true");
  CHECK(invoke0("(defshader cmp ((a u32)(b u32)) -> bool (> a b))",
                args, 2, &out, &err) == SH_OK && out.u == 0, "u32 > false");
  CHECK(invoke0("(defshader cmp ((a u32)(b u32)) -> bool (= a b))",
                args, 2, &out, &err) == SH_OK && out.u == 0, "u32 = false");

  args[0] = sh_val_u32(5); args[1] = sh_val_u32(5);
  CHECK(invoke0("(defshader cmp ((a u32)(b u32)) -> bool (= a b))",
                args, 2, &out, &err) == SH_OK && out.u == 1, "u32 = equal");
  CHECK(invoke0("(defshader cmp ((a u32)(b u32)) -> bool (<= a b))",
                args, 2, &out, &err) == SH_OK && out.u == 1, "u32 <= equal");
  CHECK(invoke0("(defshader cmp ((a u32)(b u32)) -> bool (>= a b))",
                args, 2, &out, &err) == SH_OK && out.u == 1, "u32 >= equal");

  // i64 signed comparison
  args[0] = sh_val_i64(-1); args[1] = sh_val_i64(1);
  CHECK(invoke0("(defshader cmp ((a i64)(b i64)) -> bool (< a b))",
                args, 2, &out, &err) == SH_OK && out.u == 1, "i64 < true signed");

  // bool not
  sh_value bv = sh_val_bool(false);
  CHECK(invoke0("(defshader nt ((x bool)) -> bool (not x))", &bv, 1, &out, &err) == SH_OK
        && out.u == 1, "not false");
  bv = sh_val_bool(true);
  CHECK(invoke0("(defshader nt ((x bool)) -> bool (not x))", &bv, 1, &out, &err) == SH_OK
        && out.u == 0, "not true");
}

static void test_if_cond(void) {
  printf("--- if / cond ---\n");
  sh_error err;
  sh_value out;
  sh_value args[2];

  // max(a,b)
  args[0] = sh_val_u32(7); args[1] = sh_val_u32(3);
  CHECK(invoke0("(defshader max2 ((a u32)(b u32)) -> u32 (if (> a b) a b))",
                args, 2, &out, &err) == SH_OK && out.u == 7, "if: max(7,3)=7");

  args[0] = sh_val_u32(2); args[1] = sh_val_u32(9);
  CHECK(invoke0("(defshader max2 ((a u32)(b u32)) -> u32 (if (> a b) a b))",
                args, 2, &out, &err) == SH_OK && out.u == 9, "if: max(2,9)=9");

  // Abs (negate if negative)
  sh_value neg_arg = sh_val_i64(-5);
  CHECK(invoke0("(defshader abs1 ((x i64)) -> i64 (if (< x 0) (- x) x))",
                &neg_arg, 1, &out, &err) == SH_OK && out.i == 5, "if: abs(-5)=5");

  sh_value pos_arg = sh_val_i64(3);
  CHECK(invoke0("(defshader abs1 ((x i64)) -> i64 (if (< x 0) (- x) x))",
                &pos_arg, 1, &out, &err) == SH_OK && out.i == 3, "if: abs(3)=3");
}

static void test_let(void) {
  printf("--- let / let* ---\n");
  sh_error err;
  sh_value out;
  sh_value a = sh_val_u32(6);

  CHECK(invoke0("(defshader sq ((x u32)) -> u32 (let ((y (* x x))) y))",
                &a, 1, &out, &err) == SH_OK && out.u == 36, "let: square 6->36");

  CHECK(invoke0("(defshader nested ((x u32)) -> u32"
                " (let* ((y (* x x)) (z (+ y 1))) z))",
                &a, 1, &out, &err) == SH_OK && out.u == 37, "let*: nested 6->37");

  CHECK(invoke0("(defshader deep ((x u32)) -> u32"
                " (let* ((a (+ x 1)) (b (+ a 1)) (c (+ b 1))) c))",
                &a, 1, &out, &err) == SH_OK && out.u == 9, "let*: three levels 6->9");

  // let with multiple parallel bindings
  sh_value two = sh_val_u32(2);
  CHECK(invoke0("(defshader swap ((x u32)) -> u32"
                " (let ((a (+ x 1)) (b (* x 2))) (+ a b)))",
                &two, 1, &out, &err) == SH_OK && out.u == 7, "let: parallel binds 2->(3+4)=7");
}

// ---------------------------------------------------------------------------
// Section 2: Loops over regions
// ---------------------------------------------------------------------------

static void test_loop_sum(void) {
  printf("--- loop sum ---\n");
  sh_error err;
  sh_value out;

  uint32_t data[5] = {1, 2, 3, 4, 5};
  sh_value buf = sh_val_region_raw(data, 5, SH_K_U32, false);

  const char *sum_src =
    "(defshader sum ((buf (bytes u32))) -> u32"
    " (let loop ((i 0) (acc 0))"
    "   (if (>= i (region-len buf))"
    "     acc"
    "     (loop (+ i 1) (+ acc (region-ref buf i))))))";

  CHECK(invoke0(sum_src, &buf, 1, &out, &err) == SH_OK && out.u == 15,
        "loop sum u32 [1..5] = 15");

  // Empty region
  sh_value empty_buf = sh_val_region_raw(data, 0, SH_K_U32, false);
  CHECK(invoke0(sum_src, &empty_buf, 1, &out, &err) == SH_OK && out.u == 0,
        "loop sum u32 empty = 0");

  // u16 sum -- use (u32 0) for accumulator to anchor its type
  uint16_t u16data[4] = {0x1234, 0x5678, 0x9ABC, 0xDEF0};
  sh_value u16buf = sh_val_region_raw(u16data, 4, SH_K_U16, false);
  const char *u16sum_src =
    "(defshader u16sum ((buf (bytes u16))) -> u32"
    " (let loop ((i 0) (acc (u32 0)))"
    "   (if (>= i (region-len buf))"
    "     acc"
    "     (loop (+ i 1) (+ acc (u32 (region-ref buf i)))))))";
  uint32_t expected = (uint32_t)0x1234 + (uint32_t)0x5678 +
                      (uint32_t)0x9ABC + (uint32_t)0xDEF0;
  CHECK(invoke0(u16sum_src, &u16buf, 1, &out, &err) == SH_OK && out.u == expected,
        "loop sum u16 words");

  // Sum 0..9 (const-bound loop)
  const char *sum10_src =
    "(defshader sum10 () -> i64"
    " (let loop ((i 0) (acc 0))"
    "   (if (>= i 10) acc (loop (+ i 1) (+ acc i)))))";
  CHECK(invoke0(sum10_src, NULL, 0, &out, &err) == SH_OK && out.i == 45,
        "const-bound loop: sum 0..9 = 45");
}

static void test_loop_bounds_trap(void) {
  printf("--- region bounds check traps ---\n");
  sh_error err;
  sh_value out;

  uint32_t data[3] = {10, 20, 30};
  sh_value buf = sh_val_region_raw(data, 3, SH_K_U32, false);
  sh_value idx = sh_val_u32(3);  // out of bounds

  const char *oob_src =
    "(defshader oob ((buf (bytes u32)) (i u32)) -> u32"
    " (region-ref buf i))";
  sh_value oob_args[2] = {buf, idx};
  CHECK(invoke0(oob_src, oob_args, 2, &out, &err) == SH_ERR_BOUNDS,
        "OOB load returns SH_ERR_BOUNDS");

  // In-bounds read
  idx = sh_val_u32(2);
  oob_args[1] = idx;
  CHECK(invoke0(oob_src, oob_args, 2, &out, &err) == SH_OK && out.u == 30,
        "in-bounds load succeeds");

  // Mutable store out of bounds
  uint32_t dst[3] = {0, 0, 0};
  sh_value mbuf = sh_val_region_raw(dst, 3, SH_K_U32, true);
  sh_value st_args[2] = {mbuf, sh_val_u32(3)};
  const char *store_src =
    "(defshader st ((buf (bytes-mut u32)) (i u32)) -> u32"
    " (region-set! buf i 99))";
  CHECK(invoke0(store_src, st_args, 2, &out, &err) == SH_ERR_BOUNDS,
        "OOB store returns SH_ERR_BOUNDS");
}

static void test_loop_store(void) {
  printf("--- loop store into region ---\n");
  sh_error err;
  sh_value out;

  uint32_t dst[4] = {0, 0, 0, 0};
  sh_value buf = sh_val_region_raw(dst, 4, SH_K_U32, true);

  // Accumulate region-set! return value (the stored value) into a dummy
  // accumulator so that the RECUR node appears directly in the IF else arm
  // (which is required by the bounded-loop template: the IF's two arms must
  // be exactly exit-value vs RECUR).
  const char *fill_src =
    "(defshader fill ((buf (bytes-mut u32))) -> u32"
    " (let loop ((i 0) (acc (u32 0)))"
    "   (if (>= i (region-len buf))"
    "     (region-len buf)"
    "     (loop (+ i 1)"
    "           (+ acc (region-set! buf i (u32 (* i 2))))))))";
  CHECK(invoke0(fill_src, &buf, 1, &out, &err) == SH_OK && out.u == 4,
        "fill loop returns length 4");
  CHECK(dst[0] == 0 && dst[1] == 2 && dst[2] == 4 && dst[3] == 6,
        "fill loop values: [0,2,4,6]");
}

// ---------------------------------------------------------------------------
// Section 3: CALL primitive
// ---------------------------------------------------------------------------

static sh_value prim_clamp_u32(const sh_value *args, uint32_t argc) {
  (void)argc;
  uint32_t x  = (uint32_t)args[0].u;
  uint32_t lo = (uint32_t)args[1].u;
  uint32_t hi = (uint32_t)args[2].u;
  uint32_t r = x < lo ? lo : (x > hi ? hi : x);
  return sh_val_u32(r);
}

static void test_call_prim(void) {
  printf("--- CALL primitive ---\n");
  sh_error err;
  sh_value out;

  sh_type u32t = sh_type_scalar(SH_K_U32);
  sh_prim clamp_prim = {
    .name = "clamp",
    .ret = u32t,
    .nparams = 3,
    .params = {u32t, u32t, u32t},
    .fn = prim_clamp_u32,
  };
  sh_prim_set prims = {&clamp_prim, 1};

  const char *src =
    "(defshader test ((x u32)) -> u32 (clamp x 10 20))";

  sh_value a = sh_val_u32(5);
  CHECK(invoke_prims(src, &prims, &a, 1, &out, &err) == SH_OK && out.u == 10,
        "clamp(5,10,20) = 10");

  a = sh_val_u32(15);
  CHECK(invoke_prims(src, &prims, &a, 1, &out, &err) == SH_OK && out.u == 15,
        "clamp(15,10,20) = 15");

  a = sh_val_u32(25);
  CHECK(invoke_prims(src, &prims, &a, 1, &out, &err) == SH_OK && out.u == 20,
        "clamp(25,10,20) = 20");
}

// ---------------------------------------------------------------------------
// Section 4: Vector ops
// ---------------------------------------------------------------------------

// Build an f32x4 sh_value from four floats
static sh_value make_f32x4(float a, float b, float c, float d) {
  sh_value v;
  memset(&v, 0, sizeof(v));
  v.kind = SH_K_VEC;
  v.lanes = 4;
  v.lane_kind = (uint8_t)SH_K_F32;
  float vals[4] = {a, b, c, d};
  for (int i = 0; i < 4; i++) {
    uint32_t bits;
    memcpy(&bits, &vals[i], 4);
    v.lane[i] = (uint64_t)bits;
  }
  return v;
}

static float get_f32_lane(const sh_value *v, int k) {
  uint32_t bits = (uint32_t)v->lane[k];
  float f;
  memcpy(&f, &bits, 4);
  return f;
}

static void test_vsplat(void) {
  printf("--- VSPLAT ---\n");
  sh_error err;
  sh_value out;
  sh_value a = sh_val_f32(3.0f);

  const char *src =
    "(defshader splat ((x f32)) -> vec4 (splat x))";
  sh_status s = invoke0(src, &a, 1, &out, &err);
  CHECK(s == SH_OK, "vsplat compiles and runs");
  if (s == SH_OK && out.kind == SH_K_VEC && out.lanes == 4) {
    int ok = (get_f32_lane(&out, 0) == 3.0f && get_f32_lane(&out, 1) == 3.0f &&
              get_f32_lane(&out, 2) == 3.0f && get_f32_lane(&out, 3) == 3.0f);
    CHECK(ok, "vsplat all lanes == 3.0");
  } else {
    g_fail++;
    printf("  FAIL [%d] vsplat wrong type/kind\n", __LINE__);
  }
}

static void test_vbinop(void) {
  printf("--- VBINOP lane-wise ---\n");
  sh_error err;
  sh_value out;

  sh_value va = make_f32x4(1.0f, 2.0f, 3.0f, 4.0f);
  sh_value vb = make_f32x4(10.0f, 20.0f, 30.0f, 40.0f);
  sh_value args[2] = {va, vb};

  const char *add_src =
    "(defshader vadd ((a f32x4)(b f32x4)) -> f32x4 (+ a b))";
  sh_status s = invoke0(add_src, args, 2, &out, &err);
  CHECK(s == SH_OK, "vbinop f32x4 add compiles");
  if (s == SH_OK && out.kind == SH_K_VEC && out.lanes == 4) {
    int ok = 1;
    for (int i = 0; i < 4; i++) {
      float expected = get_f32_lane(&va, i) + get_f32_lane(&vb, i);
      ok &= (get_f32_lane(&out, i) == expected);
    }
    CHECK(ok, "vbinop add == lane-wise reference");
  } else {
    g_fail++;
    printf("  FAIL [%d] vbinop add wrong type\n", __LINE__);
  }

  // u8x4 multiply
  sh_value vu8a, vu8b;
  memset(&vu8a, 0, sizeof(vu8a));
  memset(&vu8b, 0, sizeof(vu8b));
  vu8a.kind = SH_K_VEC; vu8a.lanes = 4; vu8a.lane_kind = (uint8_t)SH_K_U8;
  vu8b.kind = SH_K_VEC; vu8b.lanes = 4; vu8b.lane_kind = (uint8_t)SH_K_U8;
  for (int i = 0; i < 4; i++) {
    vu8a.lane[i] = (uint64_t)(i + 1);
    vu8b.lane[i] = 2;
  }
  sh_value u8args[2] = {vu8a, vu8b};
  const char *mu8_src =
    "(defshader vmul ((a u8x4)(b u8x4)) -> u8x4 (* a b))";
  s = invoke0(mu8_src, u8args, 2, &out, &err);
  CHECK(s == SH_OK, "u8x4 vbinop mul compiles");
  if (s == SH_OK && out.kind == SH_K_VEC && out.lanes == 4) {
    CHECK(out.lane[0] == 2 && out.lane[1] == 4 &&
          out.lane[2] == 6 && out.lane[3] == 8,
          "u8x4 mul: [2,4,6,8]");
  } else {
    g_fail++;
    printf("  FAIL [%d] u8x4 mul wrong type\n", __LINE__);
  }
}

static void test_vcmp(void) {
  printf("--- VCMP (lane compare, result via lane extraction) ---\n");
  sh_error err;
  sh_value out;

  // (> a b) on f32x4 promotes to VCMP -> bool-vec4
  // Extract lane 0 of the mask result using (lane ... 0) -> bool
  sh_value va = make_f32x4(1.0f, 5.0f, 3.0f, 4.0f);
  sh_value vb = make_f32x4(2.0f, 2.0f, 4.0f, 4.0f);
  sh_value args[2] = {va, vb};

  // Lane 0: 1.0 > 2.0 = false (0)
  const char *cmp0 =
    "(defshader vcmp0 ((a f32x4)(b f32x4)) -> bool"
    " (lane (> a b) 0))";
  CHECK(invoke0(cmp0, args, 2, &out, &err) == SH_OK && out.u == 0,
        "vcmp lane0: 1.0>2.0=false");

  // Lane 1: 5.0 > 2.0 = true (1)
  const char *cmp1 =
    "(defshader vcmp1 ((a f32x4)(b f32x4)) -> bool"
    " (lane (> a b) 1))";
  CHECK(invoke0(cmp1, args, 2, &out, &err) == SH_OK && out.u == 1,
        "vcmp lane1: 5.0>2.0=true");

  // Lane 2: 3.0 > 4.0 = false
  const char *cmp2 =
    "(defshader vcmp2 ((a f32x4)(b f32x4)) -> bool"
    " (lane (> a b) 2))";
  CHECK(invoke0(cmp2, args, 2, &out, &err) == SH_OK && out.u == 0,
        "vcmp lane2: 3.0>4.0=false");

  // Lane 3: 4.0 >= 4.0 = true
  const char *cmp3 =
    "(defshader vcmp3 ((a f32x4)(b f32x4)) -> bool"
    " (lane (>= a b) 3))";
  CHECK(invoke0(cmp3, args, 2, &out, &err) == SH_OK && out.u == 1,
        "vcmp lane3: 4.0>=4.0=true");
}

static void test_vector_if_scalar_cond(void) {
  // IF with scalar bool condition and vector arms: valid (IF node, not VSELECT)
  printf("--- IF with vector arms (scalar bool cond) ---\n");
  sh_error err;
  sh_value out;

  sh_value va = make_f32x4(1.0f, 2.0f, 3.0f, 4.0f);
  sh_value vb = make_f32x4(10.0f, 20.0f, 30.0f, 40.0f);

  // select va if lane0(va) < lane0(vb), else vb
  sh_value args[2] = {va, vb};
  const char *sel_src =
    "(defshader ifvec ((a f32x4)(b f32x4)) -> f32x4"
    " (if (< (lane a 0) (lane b 0)) a b))";
  sh_status s = invoke0(sel_src, args, 2, &out, &err);
  // lane0(va)=1.0 < lane0(vb)=10.0 => true => pick va
  CHECK(s == SH_OK, "vector IF with scalar cond compiles");
  if (s == SH_OK && out.kind == SH_K_VEC && out.lanes == 4) {
    CHECK(get_f32_lane(&out, 0) == 1.0f && get_f32_lane(&out, 3) == 4.0f,
          "vector IF picks va when cond true");
  } else {
    g_fail++;
    printf("  FAIL [%d] vector IF wrong result\n", __LINE__);
  }
}

static void test_vshuffle(void) {
  printf("--- VSHUFFLE ---\n");
  sh_error err;
  sh_value out;

  sh_value va = make_f32x4(1.0f, 2.0f, 3.0f, 4.0f);

  // Reverse
  const char *rev_src =
    "(defshader vrev ((v f32x4)) -> f32x4 (shuffle v 3 2 1 0))";
  sh_status s = invoke0(rev_src, &va, 1, &out, &err);
  CHECK(s == SH_OK, "vshuffle reverse compiles");
  if (s == SH_OK && out.kind == SH_K_VEC && out.lanes == 4) {
    CHECK(get_f32_lane(&out, 0) == 4.0f && get_f32_lane(&out, 1) == 3.0f &&
          get_f32_lane(&out, 2) == 2.0f && get_f32_lane(&out, 3) == 1.0f,
          "vshuffle reverse correct");
  } else {
    g_fail++;
    printf("  FAIL [%d] vshuffle reverse wrong\n", __LINE__);
  }

  // xxzz swizzle
  const char *xxzz_src =
    "(defshader xxzz ((v f32x4)) -> f32x4 (shuffle v 0 0 2 2))";
  s = invoke0(xxzz_src, &va, 1, &out, &err);
  CHECK(s == SH_OK, "vshuffle xxzz compiles");
  if (s == SH_OK && out.kind == SH_K_VEC && out.lanes == 4) {
    CHECK(get_f32_lane(&out, 0) == 1.0f && get_f32_lane(&out, 1) == 1.0f &&
          get_f32_lane(&out, 2) == 3.0f && get_f32_lane(&out, 3) == 3.0f,
          "vshuffle xxzz correct");
  } else {
    g_fail++;
    printf("  FAIL [%d] vshuffle xxzz wrong\n", __LINE__);
  }
}

static void test_vreduce(void) {
  printf("--- VREDUCE + dot ---\n");
  sh_error err;
  sh_value out;

  sh_value va = make_f32x4(1.0f, 2.0f, 3.0f, 4.0f);

  // vreduce-add: left-to-right accumulation
  const char *add_src =
    "(defshader vsum ((v f32x4)) -> f32 (vreduce-add v))";
  sh_status s = invoke0(add_src, &va, 1, &out, &err);
  CHECK(s == SH_OK, "vreduce-add compiles");
  if (s == SH_OK) {
    CHECK((float)out.f == 10.0f, "vreduce-add [1,2,3,4] = 10.0");
  }

  // vreduce-min
  const char *min_src =
    "(defshader vmin ((v f32x4)) -> f32 (vreduce-min v))";
  s = invoke0(min_src, &va, 1, &out, &err);
  CHECK(s == SH_OK && (float)out.f == 1.0f, "vreduce-min [1,2,3,4] = 1.0");

  // vreduce-max
  const char *max_src =
    "(defshader vmax ((v f32x4)) -> f32 (vreduce-max v))";
  s = invoke0(max_src, &va, 1, &out, &err);
  CHECK(s == SH_OK && (float)out.f == 4.0f, "vreduce-max [1,2,3,4] = 4.0");

  // dot product with [1,1,1,1]
  sh_value vb = make_f32x4(1.0f, 1.0f, 1.0f, 1.0f);
  sh_value dot_args[2] = {va, vb};
  const char *dot_src =
    "(defshader vdot ((a f32x4)(b f32x4)) -> f32 (dot a b))";
  s = invoke0(dot_src, dot_args, 2, &out, &err);
  CHECK(s == SH_OK, "dot product compiles");
  if (s == SH_OK) {
    CHECK((float)out.f == 10.0f, "dot [1,2,3,4].[1,1,1,1] = 10.0");
  }

  // dot with itself: 1+4+9+16 = 30
  sh_value self_dot_args[2] = {va, va};
  s = invoke0(dot_src, self_dot_args, 2, &out, &err);
  CHECK(s == SH_OK && (float)out.f == 30.0f,
        "dot [1,2,3,4].[1,2,3,4] = 30.0");
}

static void test_vlane(void) {
  printf("--- VLANE ---\n");
  sh_error err;
  sh_value out;
  sh_value va = make_f32x4(1.0f, 2.0f, 3.0f, 4.0f);

  const char *l0 = "(defshader l0 ((v f32x4)) -> f32 (lane v 0))";
  const char *l1 = "(defshader l1 ((v f32x4)) -> f32 (lane v 1))";
  const char *l2 = "(defshader l2 ((v f32x4)) -> f32 (lane v 2))";
  const char *l3 = "(defshader l3 ((v f32x4)) -> f32 (lane v 3))";
  CHECK(invoke0(l0, &va, 1, &out, &err) == SH_OK && (float)out.f == 1.0f,
        "vlane 0 = 1.0");
  CHECK(invoke0(l1, &va, 1, &out, &err) == SH_OK && (float)out.f == 2.0f,
        "vlane 1 = 2.0");
  CHECK(invoke0(l2, &va, 1, &out, &err) == SH_OK && (float)out.f == 3.0f,
        "vlane 2 = 3.0");
  CHECK(invoke0(l3, &va, 1, &out, &err) == SH_OK && (float)out.f == 4.0f,
        "vlane 3 = 4.0");
}

// ---------------------------------------------------------------------------
// Section 5: Differential tests (shader result == independent C reference)
// ---------------------------------------------------------------------------

// IP-checksum: sum of u16 words
static uint32_t ref_sum_u16(const uint16_t *data, uint32_t n) {
  uint32_t acc = 0;
  for (uint32_t i = 0; i < n; i++) acc += data[i];
  return acc;
}

static void test_diff_ip_checksum(void) {
  printf("--- differential: ip-checksum ---\n");
  sh_error err;
  sh_value out;

  uint16_t pkt[8] = {0x4500, 0x003C, 0x1C46, 0x4000, 0x4006, 0x0000, 0xAC10, 0x0A63};
  sh_value buf = sh_val_region_raw(pkt, 8, SH_K_U16, false);

  // Use (u32 0) to anchor acc type (u16 region loads need explicit u32 cast)
  const char *src =
    "(defshader ipsum ((buf (bytes u16))) -> u32"
    " (let loop ((i 0) (acc (u32 0)))"
    "   (if (>= i (region-len buf))"
    "     acc"
    "     (loop (+ i 1) (+ acc (u32 (region-ref buf i)))))))";

  sh_status s = invoke0(src, &buf, 1, &out, &err);
  CHECK(s == SH_OK, "ip-checksum compiles");
  uint32_t ref = ref_sum_u16(pkt, 8);
  CHECK(out.u == ref, "ip-checksum == C reference");
}

// Saturating-add blit: dst[i] = min(src[i] + delta, 255)
static void ref_blit_sat(uint8_t *dst, const uint8_t *src, uint32_t n,
                         uint32_t delta) {
  for (uint32_t i = 0; i < n; i++) {
    uint32_t v = (uint32_t)src[i] + delta;
    dst[i] = v > 255 ? 255 : (uint8_t)v;
  }
}

static void test_diff_blit_sat(void) {
  printf("--- differential: saturating blit ---\n");
  sh_error err;
  sh_value out;

  uint8_t src_data[6] = {200, 100, 50, 10, 255, 0};
  uint8_t dst_data[6] = {0};
  sh_value src_buf = sh_val_region_raw(src_data, 6, SH_K_U8, false);
  sh_value dst_buf = sh_val_region_raw(dst_data, 6, SH_K_U8, true);
  sh_value delta   = sh_val_u32(60);
  sh_value args[3] = {src_buf, dst_buf, delta};

  // The RECUR must be directly in the IF else arm (not wrapped in let/begin).
  // We thread a dummy accumulator through the loop to thread the store result
  // into the RECUR args while keeping RECUR as the direct else child of IF.
  const char *blit_src =
    "(defshader blit ((src (bytes u8)) (dst (bytes-mut u8)) (delta u32)) -> u32"
    " (let loop ((i 0) (acc (u32 0)))"
    "   (if (>= i (region-len src))"
    "     (region-len src)"
    "     (loop (+ i 1)"
    "           (+ acc (u32 (region-set! dst i"
    "             (u8 (if (> (+ (u32 (region-ref src i)) delta) (u32 255))"
    "                    (u32 255)"
    "                    (+ (u32 (region-ref src i)) delta))))))))))";

  sh_status s = invoke0(blit_src, args, 3, &out, &err);
  CHECK(s == SH_OK, "blit-sat compiles and runs");
  if (s == SH_OK) {
    uint8_t ref_dst[6];
    ref_blit_sat(ref_dst, src_data, 6, 60);
    int ok = 1;
    for (int i = 0; i < 6; i++) ok &= (dst_data[i] == ref_dst[i]);
    CHECK(ok, "blit-sat == C reference");
  }
}

// f32 dot product via region loop
static float ref_fdot(const float *a, const float *b, uint32_t n) {
  float acc = 0.0f;
  for (uint32_t i = 0; i < n; i++) acc += a[i] * b[i];
  return acc;
}

static void test_diff_dot(void) {
  printf("--- differential: f32 dot via region ---\n");
  sh_error err;
  sh_value out;

  float a_data[4] = {1.0f, 2.0f, 3.0f, 4.0f};
  float b_data[4] = {0.5f, 0.5f, 0.5f, 0.5f};
  sh_value abuf = sh_val_region_raw(a_data, 4, SH_K_F32, false);
  sh_value bbuf = sh_val_region_raw(b_data, 4, SH_K_F32, false);
  sh_value args[2] = {abuf, bbuf};

  // Use (f32 0.0) to anchor the accumulator type; uncast 0.0 defaults to f64
  const char *src =
    "(defshader fdot ((a (bytes f32)) (b (bytes f32))) -> f32"
    " (let loop ((i 0) (acc (f32 0.0)))"
    "   (if (>= i (region-len a))"
    "     acc"
    "     (loop (+ i 1)"
    "           (+ acc (* (region-ref a i) (region-ref b i)))))))";

  sh_status s = invoke0(src, args, 2, &out, &err);
  CHECK(s == SH_OK, "f32 dot via region compiles");
  if (s == SH_OK) {
    float ref = ref_fdot(a_data, b_data, 4);
    CHECK((float)out.f == ref, "f32 dot == C reference");
  }
}

// ---------------------------------------------------------------------------
// Section 6: Cost model
// ---------------------------------------------------------------------------

static void test_cost(void) {
  printf("--- cost model ---\n");
  sh_error err;
  sh_program *p = NULL;

  // Constant-bound loop: cost is const
  const char *const_src =
    "(defshader cloop ((x u32)) -> u32"
    " (let loop ((i 0) (acc 0))"
    "   (if (>= i 10)"
    "     acc"
    "     (loop (+ i 1) (+ acc x)))))";
  CHECK(sh_compile_string(const_src, NULL, 0, &p, &err) == SH_OK,
        "const-bound loop compiles");
  if (p) {
    CHECK(sh_cost_is_const(p), "const-bound has const cost");
    CHECK(sh_static_cost(p) > 0, "const-bound cost > 0");
    sh_free(p); p = NULL;
  }

  // Param-bound loop: cost depends on args
  const char *param_src =
    "(defshader ploop ((n u32) (x u32)) -> u32"
    " (let loop ((i 0) (acc 0))"
    "   (if (>= i n)"
    "     acc"
    "     (loop (+ i 1) (+ acc x)))))";
  CHECK(sh_compile_string(param_src, NULL, 0, &p, &err) == SH_OK,
        "param-bound loop compiles");
  if (p) {
    CHECK(!sh_cost_is_const(p), "param-bound has non-const cost");
    sh_value args100[2] = {sh_val_u32(100), sh_val_u32(1)};
    uint64_t cost100 = sh_cost_for_args(p, args100, 2);
    CHECK(cost100 > 0, "sh_cost_for_args(n=100) > 0");

    sh_value args200[2] = {sh_val_u32(200), sh_val_u32(1)};
    uint64_t cost200 = sh_cost_for_args(p, args200, 2);
    CHECK(cost200 > cost100, "sh_cost_for_args(n=200) > cost(n=100)");
    sh_free(p); p = NULL;
  }
}

// ---------------------------------------------------------------------------
// Section 7: Edge cases
// ---------------------------------------------------------------------------

static void test_edge_cases(void) {
  printf("--- edge cases ---\n");
  sh_error err;
  sh_value out;

  // Arity mismatch: 2 args instead of 1
  sh_value a = sh_val_u32(1);
  sh_value b = sh_val_u32(2);
  sh_program *p = NULL;
  sh_compile_string("(defshader id ((x u32)) -> u32 x)", NULL, 0, &p, &err);
  if (p) {
    sh_value twoargs[2] = {a, b};
    CHECK(sh_invoke(p, twoargs, 2, &out, &err) == SH_ERR_ARITY,
          "arity mismatch -> SH_ERR_ARITY");
    sh_free(p);
  }

  // Bool literal in conditional -- must use typed arms so the verifier can
  // resolve both integer literals (they need type context from each other or
  // from an explicit cast; bare 1/0 alone are both VOID until resolved).
  const char *bool_src =
    "(defshader btest () -> u32 (if #t (u32 1) (u32 0)))";
  CHECK(invoke0(bool_src, NULL, 0, &out, &err) == SH_OK && out.u == 1,
        "bool literal #t in if -> 1");

  const char *boolF_src =
    "(defshader btest () -> u32 (if #f (u32 1) (u32 0)))";
  CHECK(invoke0(boolF_src, NULL, 0, &out, &err) == SH_OK && out.u == 0,
        "bool literal #f in if -> 0");

  // Region store returns the stored value
  uint32_t dst[2] = {0, 0};
  sh_value dst_buf = sh_val_region_raw(dst, 2, SH_K_U32, true);
  sh_value idx = sh_val_u32(0);
  sh_value rargs[2] = {dst_buf, idx};
  const char *store_ret_src =
    "(defshader st ((buf (bytes-mut u32)) (i u32)) -> u32"
    " (region-set! buf i 42))";
  CHECK(invoke0(store_ret_src, rargs, 2, &out, &err) == SH_OK && out.u == 42,
        "region-set! returns stored value");
  CHECK(dst[0] == 42, "region-set! actually wrote 42");
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(void) {
  // Bring up the Lisp runtime (needed for symbol interning by the reader).
  (void)lisp_default_env();

  printf("[test_interp] Scalar tests\n");
  test_scalar_identity();
  test_scalar_arithmetic();
  test_casts();
  test_comparisons();
  test_if_cond();
  test_let();

  printf("[test_interp] Loop + region tests\n");
  test_loop_sum();
  test_loop_bounds_trap();
  test_loop_store();

  printf("[test_interp] CALL primitive tests\n");
  test_call_prim();

  printf("[test_interp] Vector op tests\n");
  test_vsplat();
  test_vbinop();
  test_vcmp();
  test_vector_if_scalar_cond();
  test_vshuffle();
  test_vreduce();
  test_vlane();

  printf("[test_interp] Differential tests\n");
  test_diff_ip_checksum();
  test_diff_blit_sat();
  test_diff_dot();

  printf("[test_interp] Cost model tests\n");
  test_cost();

  printf("[test_interp] Edge case tests\n");
  test_edge_cases();

  int total = g_pass + g_fail;
  printf("\n[test_interp] %s (%d/%d)\n",
         g_fail == 0 ? "ALL TESTS PASSED" : "SOME TESTS FAILED",
         g_pass, total);
  return g_fail ? 1 : 0;
}
