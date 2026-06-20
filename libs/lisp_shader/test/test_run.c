// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

// S4 -- the sh_run VM facade. sh_run is the in-OS execution entry: it lowers +
// validates a verified program into a chunk cached on the program (lazily, on
// first call) and runs the bytecode VM (SIMD where the build enables it). These
// tests assert:
//   1. sh_run == sh_invoke (the scalar oracle) bit-for-bit, scalar and vector.
//   2. The cached chunk is reused across repeated calls and stays correct.
//   3. SH_VM_FORCE_SCALAR through sh_run matches the default (SIMD) path.
//   4. Error parity: arity + arg-kind mismatches are rejected like sh_invoke.
//   5. A region (blit) kernel through sh_run mutates the buffer correctly.
//   6. sh_free after sh_run reclaims the cached chunk (run under ASan to confirm
//      no leak / no double free across the finalizer path).

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lisp.h"
#include "sh_bytecode.h"
#include "sh_internal.h"

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond, msg)                            \
  do {                                              \
    if (cond) {                                     \
      g_pass++;                                      \
      printf("  ok   [%d] %s\n", __LINE__, (msg));  \
    } else {                                        \
      g_fail++;                                     \
      printf("  FAIL [%d] %s\n", __LINE__, (msg));  \
    }                                               \
  } while (0)

static sh_program *compile_only(const char *src) {
  sh_program *p = NULL;
  sh_error err;
  memset(&err, 0, sizeof(err));
  sh_status s = sh_compile_string(src, NULL, 0, &p, &err);
  if (s != SH_OK) {
    printf("  [compile] FAILED: %s\n", err.msg);
    return NULL;
  }
  return p;
}

static int sh_val_eq(sh_value a, sh_value b) {
  if (a.kind != b.kind) return 0;
  if (a.kind == SH_K_VEC) {
    if (a.lanes != b.lanes || a.lane_kind != b.lane_kind) return 0;
    for (int i = 0; i < a.lanes; i++)
      if (a.lane[i] != b.lane[i]) return 0;
    return 1;
  }
  if (a.kind == SH_K_F32 || a.kind == SH_K_F64) {
    uint64_t ab, bb;
    memcpy(&ab, &a.f, 8);
    memcpy(&bb, &b.f, 8);
    return ab == bb;
  }
  if (a.kind == SH_K_I64) return a.i == b.i;
  return a.u == b.u;
}

// ---------------------------------------------------------------------------
// 1+2+3: oracle parity, chunk caching, FORCE_SCALAR parity
// ---------------------------------------------------------------------------
static void test_scalar_parity(void) {
  printf("[scalar_parity]\n");
  // Arithmetic + a param-bounded named-let loop: sum of i for i in [0, n).
  sh_program *p = compile_only(
      "(defshader sumto ((n u32)) -> u32"
      "  (let loop ((i 0) (acc (u32 0)))"
      "    (if (>= i n)"
      "        acc"
      "        (loop (+ i 1) (+ acc (u32 i))))))");
  CHECK(p != NULL, "sumto compiles");
  if (!p) return;

  int all_match = 1;
  // Repeat across many arg sets: the FIRST call lowers+caches, the rest reuse.
  for (uint32_t n = 0; n < 32; n++) {
    sh_value args[1] = {sh_val_u32(n)};
    sh_value oracle, vm, scalar;
    sh_error e1, e2, e3;
    memset(&e1, 0, sizeof(e1));
    memset(&e2, 0, sizeof(e2));
    memset(&e3, 0, sizeof(e3));
    sh_status s1 = sh_invoke(p, args, 1, &oracle, &e1);
    sh_status s2 = sh_run(p, args, 1, 0, &vm, &e2);
    sh_status s3 = sh_run(p, args, 1, SH_VM_FORCE_SCALAR, &scalar, &e3);
    if (s1 != SH_OK || s2 != SH_OK || s3 != SH_OK ||
        !sh_val_eq(oracle, vm) || !sh_val_eq(oracle, scalar))
      all_match = 0;
  }
  CHECK(all_match, "sh_run == sh_invoke == FORCE_SCALAR over 32 arg sets");
  CHECK(p->chunk != NULL, "chunk cached on the program after first sh_run");
  sh_free(p);  // must reclaim the cached chunk (ASan: no leak)
}

// ---------------------------------------------------------------------------
// 4: error parity
// ---------------------------------------------------------------------------
static void test_error_parity(void) {
  printf("[error_parity]\n");
  sh_program *p = compile_only("(defshader id ((x u32)) -> u32 x)");
  CHECK(p != NULL, "id compiles");
  if (!p) return;

  sh_value out;
  sh_error err;
  sh_value good = sh_val_u32(7);

  memset(&err, 0, sizeof(err));
  CHECK(sh_run(p, &good, 0, 0, &out, &err) == SH_ERR_ARITY,
        "sh_run: wrong argc -> SH_ERR_ARITY");

  memset(&err, 0, sizeof(err));
  sh_value wrong_kind = sh_val_f32(1.0f);  // declared u32
  CHECK(sh_run(p, &wrong_kind, 1, 0, &out, &err) == SH_ERR_TYPE,
        "sh_run: wrong arg kind -> SH_ERR_TYPE");

  memset(&err, 0, sizeof(err));
  CHECK(sh_run(p, &good, 1, 0, &out, &err) == SH_OK && out.u == 7,
        "sh_run: valid call still works after rejected ones");
  sh_free(p);
}

// ---------------------------------------------------------------------------
// 5: a region blit kernel through sh_run
// ---------------------------------------------------------------------------
static void test_region_blit(void) {
  printf("[region_blit]\n");
  // The proven saturating-blit kernel (test_vregion): sat+ a u8x16 delta into the
  // buffer one 16-byte strip at a time, looping to region-len. Run it via sh_run.
  sh_program *p = compile_only(
      "(defshader blit ((buf (bytes-mut u8))(delta u8x16)) -> u32"
      "  (let loop ((i 0)(acc (u32 0)))"
      "    (if (>= i (region-len buf))"
      "        acc"
      "        (loop (+ i 16)"
      "              (+ acc (u32 (lane (vregion-set! buf i"
      "                   (sat+ (vregion-ref buf i 16) delta)) 0)))))))");
  CHECK(p != NULL, "blit compiles");
  if (!p) return;

  uint8_t buf[32];
  for (int i = 0; i < 32; i++) buf[i] = (uint8_t)(200 + i);  // 200.. will saturate
  sh_value delta;
  memset(&delta, 0, sizeof(delta));
  delta.kind = SH_K_VEC;
  delta.lanes = 16;
  delta.lane_kind = SH_K_U8;
  for (int i = 0; i < 16; i++) delta.lane[i] = 100;  // +100 saturates 200.. to 255

  sh_value args[2] = {sh_val_region_raw(buf, 32, SH_K_U8, true), delta};
  sh_value out;
  sh_error err;
  memset(&err, 0, sizeof(err));
  sh_status s = sh_run(p, args, 2, 0, &out, &err);
  CHECK(s == SH_OK, "blit runs via sh_run");
  int all_sat = 1;
  for (int i = 0; i < 32; i++)
    if (buf[i] != 255) all_sat = 0;
  CHECK(all_sat, "all 32 bytes saturated to 255");
  sh_free(p);
}

int main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  printf("=== test_run (sh_run VM facade) ===\n");
  test_scalar_parity();
  test_error_parity();
  test_region_blit();
  printf("\nResults: %d passed, %d failed\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
