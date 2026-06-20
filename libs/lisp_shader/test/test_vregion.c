// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

// S3.5 SLICE 2 -- vector region load/store test suite.
//
// Tests:
//   1. PARSE -- accept vregion-ref/vregion-set!; reject bad N and bad arity.
//   2. VERIFY -- accept valid forms; reject immutable vregion-set!, element-kind
//      mismatch, bad N (0, 1, 17), vregion-ref on non-region.
//   3. INTERP (oracle) -- load a strip, check lanes; store a vector, check bytes.
//   4. DIFFERENTIAL -- compile->lower->vm (SIMD) AND FORCE_SCALAR AND sh_invoke
//      all produce bit-identical results across elem kinds and lane counts.
//      Includes the saturating-blit kernel.
//   5. BOUNDS TRAPS -- idx+N > len, partial last strip, negative i64 index,
//      in-bounds strips never trap.

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

// Compile a shader string and return the program. NULL on failure.
static sh_program *compile_only(const char *src) {
  sh_program *p = NULL;
  sh_error err;
  memset(&err, 0, sizeof(err));
  sh_status s = sh_compile_string(src, NULL, 0, &p, &err);
  if (s != SH_OK) {
    printf("  [compile_only] FAILED: %s\n", err.msg);
    return NULL;
  }
  return p;
}

// Compile + lower. Returns chunk or NULL.
static sh_chunk *compile_lower(const char *src) {
  sh_program *p = NULL;
  sh_error err;
  memset(&err, 0, sizeof(err));
  sh_status s = sh_compile_string(src, NULL, 0, &p, &err);
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

// Compare two sh_values for bit-exact equality (vectors compare packed lanes).
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
// SECTION 1: Parse tests
// ---------------------------------------------------------------------------

static void test_parse(void) {
  printf("\n[parse]\n");

  // vregion-ref parses ok with literal N
  {
    sh_program *p = compile_only(
      "(defshader t ((buf (bytes u8))) -> u8x4 (vregion-ref buf 0 4))");
    CHECK(p != NULL, "vregion-ref: compiles ok");
    if (p) {
      sh_node *root = &p->nodes[p->root];
      CHECK(root->op == (uint16_t)SH_OP_VREGION_LOAD, "vregion-ref -> VREGION_LOAD");
      CHECK(root->imm == 4, "vregion-ref: imm=4");
      sh_free(p);
    }
  }

  // vregion-set! parses ok
  {
    sh_program *p = compile_only(
      "(defshader t ((buf (bytes-mut u8))(v u8x4)) -> u8x4 (vregion-set! buf 0 v))");
    CHECK(p != NULL, "vregion-set!: compiles ok");
    if (p) {
      sh_node *root = &p->nodes[p->root];
      CHECK(root->op == (uint16_t)SH_OP_VREGION_STORE, "vregion-set! -> VREGION_STORE");
      sh_free(p);
    }
  }

  // Bad N=0: rejected at parse time
  {
    sh_program *p = NULL;
    sh_error err;
    memset(&err, 0, sizeof(err));
    sh_status s = sh_compile_string(
      "(defshader t ((buf (bytes u8))) -> u8x4 (vregion-ref buf 0 0))",
      NULL, 0, &p, &err);
    CHECK(s != SH_OK, "vregion-ref N=0: rejected");
    if (p) sh_free(p);
  }

  // Bad N=1: rejected at parse time
  {
    sh_program *p = NULL;
    sh_error err;
    memset(&err, 0, sizeof(err));
    sh_status s = sh_compile_string(
      "(defshader t ((buf (bytes u8))) -> u8 (vregion-ref buf 0 1))",
      NULL, 0, &p, &err);
    CHECK(s != SH_OK, "vregion-ref N=1: rejected");
    if (p) sh_free(p);
  }

  // Bad N=17: rejected at parse time (SH_MAX_LANES=16)
  {
    sh_program *p = NULL;
    sh_error err;
    memset(&err, 0, sizeof(err));
    sh_status s = sh_compile_string(
      "(defshader t ((buf (bytes u8))) -> u8x16 (vregion-ref buf 0 17))",
      NULL, 0, &p, &err);
    CHECK(s != SH_OK, "vregion-ref N=17: rejected");
    if (p) sh_free(p);
  }

  // Non-literal N: rejected at parse time
  {
    sh_program *p = NULL;
    sh_error err;
    memset(&err, 0, sizeof(err));
    sh_status s = sh_compile_string(
      "(defshader t ((buf (bytes u8))(n u32)) -> u8x4 (vregion-ref buf 0 n))",
      NULL, 0, &p, &err);
    CHECK(s != SH_OK, "vregion-ref non-literal N: rejected");
    if (p) sh_free(p);
  }

  // Wrong arity for vregion-ref
  {
    sh_program *p = NULL;
    sh_error err;
    memset(&err, 0, sizeof(err));
    sh_status s = sh_compile_string(
      "(defshader t ((buf (bytes u8))) -> u8x4 (vregion-ref buf 4))",
      NULL, 0, &p, &err);
    CHECK(s != SH_OK, "vregion-ref arity=2: rejected");
    if (p) sh_free(p);
  }
}

// ---------------------------------------------------------------------------
// SECTION 2: Verify tests
// ---------------------------------------------------------------------------

static void test_verify(void) {
  printf("\n[verify]\n");

  // vregion-ref on non-region: SH_ERR_TYPE
  {
    sh_program *p = NULL;
    sh_error err;
    memset(&err, 0, sizeof(err));
    sh_status s = sh_compile_string(
      "(defshader t ((x u32)) -> u32x4 (vregion-ref x 0 4))",
      NULL, 0, &p, &err);
    CHECK(s == SH_ERR_TYPE, "vregion-ref on non-region: SH_ERR_TYPE");
    if (p) sh_free(p);
  }

  // vregion-set! on immutable region: SH_ERR_TYPE
  {
    sh_program *p = NULL;
    sh_error err;
    memset(&err, 0, sizeof(err));
    sh_status s = sh_compile_string(
      "(defshader t ((buf (bytes u8))(v u8x4)) -> u8x4 (vregion-set! buf 0 v))",
      NULL, 0, &p, &err);
    CHECK(s == SH_ERR_TYPE, "vregion-set! on immutable region: SH_ERR_TYPE");
    if (p) sh_free(p);
  }

  // vregion-set! element-kind mismatch: store u16x8 into (bytes-mut u8)
  {
    sh_program *p = NULL;
    sh_error err;
    memset(&err, 0, sizeof(err));
    sh_status s = sh_compile_string(
      "(defshader t ((buf (bytes-mut u8))(v u16x8)) -> u16x8 (vregion-set! buf 0 v))",
      NULL, 0, &p, &err);
    CHECK(s == SH_ERR_TYPE, "vregion-set! kind mismatch u16x8 into u8 region: SH_ERR_TYPE");
    if (p) sh_free(p);
  }

  // vregion-ref valid u8x16: should succeed
  {
    sh_program *p = compile_only(
      "(defshader t ((buf (bytes u8))) -> u8x16 (vregion-ref buf 0 16))");
    CHECK(p != NULL, "vregion-ref u8x16: compiles ok");
    if (p) {
      sh_node *root = &p->nodes[p->root];
      CHECK(root->type.kind == (uint8_t)SH_K_VEC, "result is VEC");
      CHECK(root->type.lane_kind == (uint8_t)SH_K_U8, "result lane_kind=u8");
      CHECK(root->type.lanes == 16, "result lanes=16");
      sh_free(p);
    }
  }

  // vregion-ref valid u32x4: should succeed
  {
    sh_program *p = compile_only(
      "(defshader t ((buf (bytes u32))) -> u32x4 (vregion-ref buf 0 4))");
    CHECK(p != NULL, "vregion-ref u32x4: compiles ok");
    if (p) sh_free(p);
  }

  // vregion-ref valid u16x8: should succeed
  {
    sh_program *p = compile_only(
      "(defshader t ((buf (bytes u16))) -> u16x8 (vregion-ref buf 0 8))");
    CHECK(p != NULL, "vregion-ref u16x8: compiles ok");
    if (p) sh_free(p);
  }

  // vregion-set! valid u8x16: should succeed
  {
    sh_program *p = compile_only(
      "(defshader t ((buf (bytes-mut u8))(v u8x16)) -> u8x16 (vregion-set! buf 0 v))");
    CHECK(p != NULL, "vregion-set! u8x16: compiles ok");
    if (p) sh_free(p);
  }
}

// ---------------------------------------------------------------------------
// SECTION 3: Interp oracle tests
// ---------------------------------------------------------------------------

static void test_interp(void) {
  printf("\n[interp]\n");

  // Load 4 u32 elements and check lanes
  {
    sh_program *p = compile_only(
      "(defshader t ((buf (bytes u32))) -> u32x4 (vregion-ref buf 2 4))");
    CHECK(p != NULL, "vregion-ref u32x4 at idx=2: compile ok");
    if (p) {
      uint32_t data[8] = {10, 20, 30, 40, 50, 60, 70, 80};
      sh_value buf_v = sh_val_region_raw(data, 8, SH_K_U32, false);
      sh_value args[1] = {buf_v};
      sh_value out;
      memset(&out, 0, sizeof(out));
      sh_error err;
      memset(&err, 0, sizeof(err));
      sh_status s = sh_invoke(p, args, 1, &out, &err);
      CHECK(s == SH_OK, "vregion-ref u32x4 idx=2: invoked ok");
      if (s == SH_OK) {
        CHECK(out.kind == SH_K_VEC && out.lanes == 4 &&
              out.lane_kind == (uint8_t)SH_K_U32,
              "result is u32x4");
        CHECK(out.lane[0] == 30 && out.lane[1] == 40 &&
              out.lane[2] == 50 && out.lane[3] == 60,
              "lanes match data[2..5]");
      }
      sh_free(p);
    }
  }

  // Store 4 u8 lanes and check bytes
  {
    sh_program *p = compile_only(
      "(defshader t ((buf (bytes-mut u8))(v u8x4)) -> u8x4 (vregion-set! buf 2 v))");
    CHECK(p != NULL, "vregion-set! u8x4: compile ok");
    if (p) {
      uint8_t data[8];
      memset(data, 0, sizeof(data));
      sh_value buf_v = sh_val_region_raw(data, 8, SH_K_U8, true);
      sh_value vec_v;
      memset(&vec_v, 0, sizeof(vec_v));
      vec_v.kind = SH_K_VEC;
      vec_v.lanes = 4;
      vec_v.lane_kind = (uint8_t)SH_K_U8;
      vec_v.lane[0] = 11; vec_v.lane[1] = 22;
      vec_v.lane[2] = 33; vec_v.lane[3] = 44;
      sh_value args[2] = {buf_v, vec_v};
      sh_value out;
      memset(&out, 0, sizeof(out));
      sh_error err;
      memset(&err, 0, sizeof(err));
      sh_status s = sh_invoke(p, args, 2, &out, &err);
      CHECK(s == SH_OK, "vregion-set! u8x4: invoked ok");
      if (s == SH_OK) {
        CHECK(data[2] == 11 && data[3] == 22 &&
              data[4] == 33 && data[5] == 44,
              "bytes written correctly at offset 2");
        CHECK(data[0] == 0 && data[1] == 0, "before offset untouched");
        CHECK(data[6] == 0 && data[7] == 0, "after strip untouched");
      }
      sh_free(p);
    }
  }

  // Load u8x16 (full strip)
  {
    sh_program *p = compile_only(
      "(defshader t ((buf (bytes u8))) -> u8x16 (vregion-ref buf 0 16))");
    CHECK(p != NULL, "vregion-ref u8x16: compile ok");
    if (p) {
      uint8_t data[16];
      for (int i = 0; i < 16; i++) data[i] = (uint8_t)(i * 7 + 3);
      sh_value buf_v = sh_val_region_raw(data, 16, SH_K_U8, false);
      sh_value args[1] = {buf_v};
      sh_value out;
      memset(&out, 0, sizeof(out));
      sh_error err;
      memset(&err, 0, sizeof(err));
      sh_status s = sh_invoke(p, args, 1, &out, &err);
      CHECK(s == SH_OK, "vregion-ref u8x16 invoked ok");
      if (s == SH_OK) {
        int ok = (out.kind == SH_K_VEC && out.lanes == 16);
        for (int i = 0; i < 16 && ok; i++) ok = (out.lane[i] == data[i]);
        CHECK(ok, "u8x16 lanes match data");
      }
      sh_free(p);
    }
  }
}

// ---------------------------------------------------------------------------
// SECTION 4: Differential tests
// ---------------------------------------------------------------------------

// Differential: load a strip
static void diff_vrload(const char *src, uint8_t *mem, uint32_t len,
                        sh_kind elem, uint32_t idx, int is_ok, const char *label) {
  sh_program *p = NULL;
  sh_error err;
  memset(&err, 0, sizeof(err));
  sh_status s = sh_compile_string(src, NULL, 0, &p, &err);
  if (s != SH_OK) {
    if (is_ok) { g_fail++; printf("  FAIL compile [%s]: %s\n", label, err.msg); }
    else { g_pass++; printf("  ok   compile-fail (expected) [%s]\n", label); }
    return;
  }

  sh_chunk *c = NULL;
  memset(&err, 0, sizeof(err));
  s = sh_lower(p, &c, &err);
  if (s != SH_OK) {
    sh_free(p);
    if (is_ok) { g_fail++; printf("  FAIL lower [%s]: %s\n", label, err.msg); }
    else { g_pass++; printf("  ok   lower-fail (expected) [%s]\n", label); }
    return;
  }

  sh_value buf_v = sh_val_region_raw(mem, len, elem, false);
  sh_value idx_v = sh_val_u32(idx);
  sh_value args[2] = {buf_v, idx_v};

  sh_value oracle_out, vm_out, scalar_out;
  memset(&oracle_out, 0, sizeof(oracle_out));
  memset(&vm_out, 0, sizeof(vm_out));
  memset(&scalar_out, 0, sizeof(scalar_out));

  memset(&err, 0, sizeof(err));
  sh_status so = sh_invoke(p, args, 2, &oracle_out, &err);
  memset(&err, 0, sizeof(err));
  sh_status sv = sh_vm_run(c, args, 2, 0, &vm_out, &err);
  memset(&err, 0, sizeof(err));
  sh_status ss = sh_vm_run(c, args, 2, SH_VM_FORCE_SCALAR, &scalar_out, &err);

  sh_free(p);
  sh_chunk_free(c);

  if (!is_ok) {
    int any_ok = (so == SH_OK || sv == SH_OK || ss == SH_OK);
    CHECK(!any_ok, label);
    return;
  }

  if (so != SH_OK || sv != SH_OK || ss != SH_OK) {
    g_fail++;
    printf("  FAIL [%s] so=%d sv=%d ss=%d\n", label, so, sv, ss);
    return;
  }
  int match = sh_val_eq(oracle_out, vm_out) && sh_val_eq(oracle_out, scalar_out);
  CHECK(match, label);
}

static void test_differential(void) {
  printf("\n[differential]\n");

  // u8x16 load at various indices
  {
    uint8_t data[32];
    for (int i = 0; i < 32; i++) data[i] = (uint8_t)(i * 11 + 7);
    diff_vrload(
      "(defshader t ((buf (bytes u8))(i u32)) -> u8x16 (vregion-ref buf i 16))",
      data, 32, SH_K_U8, 0, 1, "u8x16 load idx=0");
    diff_vrload(
      "(defshader t ((buf (bytes u8))(i u32)) -> u8x16 (vregion-ref buf i 16))",
      data, 32, SH_K_U8, 16, 1, "u8x16 load idx=16");
  }

  // u16x8 load
  {
    uint16_t data[16];
    for (int i = 0; i < 16; i++) data[i] = (uint16_t)(i * 300 + 100);
    diff_vrload(
      "(defshader t ((buf (bytes u16))(i u32)) -> u16x8 (vregion-ref buf i 8))",
      (uint8_t *)data, 16, SH_K_U16, 0, 1, "u16x8 load idx=0");
    diff_vrload(
      "(defshader t ((buf (bytes u16))(i u32)) -> u16x8 (vregion-ref buf i 8))",
      (uint8_t *)data, 16, SH_K_U16, 8, 1, "u16x8 load idx=8");
  }

  // u32x4 load
  {
    uint32_t data[8];
    for (int i = 0; i < 8; i++) data[i] = (uint32_t)(i * 1000000 + 42);
    diff_vrload(
      "(defshader t ((buf (bytes u32))(i u32)) -> u32x4 (vregion-ref buf i 4))",
      (uint8_t *)data, 8, SH_K_U32, 0, 1, "u32x4 load idx=0");
    diff_vrload(
      "(defshader t ((buf (bytes u32))(i u32)) -> u32x4 (vregion-ref buf i 4))",
      (uint8_t *)data, 8, SH_K_U32, 4, 1, "u32x4 load idx=4");
  }

  // Full round-trip: load + store (vregion-set! returns the vector)
  {
    uint8_t src_data[16];
    for (int i = 0; i < 16; i++) src_data[i] = (uint8_t)(i * 13 + 5);

    const char *roundtrip_src =
      "(defshader t ((src (bytes u8))(dst (bytes-mut u8))) -> u8x16"
      "  (vregion-set! dst 0 (vregion-ref src 0 16)))";
    sh_program *p = NULL;
    sh_error err;
    memset(&err, 0, sizeof(err));
    sh_status s = sh_compile_string(roundtrip_src, NULL, 0, &p, &err);
    CHECK(s == SH_OK, "roundtrip shader compiles ok");
    if (p) {
      sh_chunk *c = NULL;
      memset(&err, 0, sizeof(err));
      s = sh_lower(p, &c, &err);
      CHECK(s == SH_OK, "roundtrip shader lowers ok");
      if (c) {
        uint8_t dst_oracle[16], dst_vm[16], dst_scalar[16];
        memset(dst_oracle, 0, 16);
        memset(dst_vm, 0, 16);
        memset(dst_scalar, 0, 16);

        sh_value out;
        sh_value args[2];

        memset(&out, 0, sizeof(out));
        args[0] = sh_val_region_raw(src_data, 16, SH_K_U8, false);
        args[1] = sh_val_region_raw(dst_oracle, 16, SH_K_U8, true);
        memset(&err, 0, sizeof(err));
        sh_status so = sh_invoke(p, args, 2, &out, &err);

        memset(&out, 0, sizeof(out));
        args[0] = sh_val_region_raw(src_data, 16, SH_K_U8, false);
        args[1] = sh_val_region_raw(dst_vm, 16, SH_K_U8, true);
        memset(&err, 0, sizeof(err));
        sh_status sv = sh_vm_run(c, args, 2, 0, &out, &err);

        memset(&out, 0, sizeof(out));
        args[0] = sh_val_region_raw(src_data, 16, SH_K_U8, false);
        args[1] = sh_val_region_raw(dst_scalar, 16, SH_K_U8, true);
        memset(&err, 0, sizeof(err));
        sh_status ss = sh_vm_run(c, args, 2, SH_VM_FORCE_SCALAR, &out, &err);

        CHECK(so == SH_OK && sv == SH_OK && ss == SH_OK, "roundtrip: all succeed");
        CHECK(memcmp(dst_oracle, src_data, 16) == 0, "oracle: dst matches src");
        CHECK(memcmp(dst_vm, src_data, 16) == 0, "vm: dst matches src");
        CHECK(memcmp(dst_scalar, src_data, 16) == 0, "scalar-vm: dst matches src");

        sh_chunk_free(c);
      }
      sh_free(p);
    }
  }

  // Saturating blit differential: sat+ each element
  // (vregion-set! returns vector; thread lane[0] through accumulator so
  //  the effect node is reachable from the tree root -- verifier constraint)
  {
    const char *blit_src =
      "(defshader blit ((buf (bytes-mut u8))(delta u8x16)) -> u32"
      "  (let loop ((i 0)(acc (u32 0)))"
      "    (if (>= i (region-len buf))"
      "        acc"
      "        (loop (+ i 16)"
      "              (+ acc (u32 (lane (vregion-set! buf i (sat+ (vregion-ref buf i 16) delta)) 0)))))))";

    sh_program *p = NULL;
    sh_error err;
    memset(&err, 0, sizeof(err));
    sh_status s = sh_compile_string(blit_src, NULL, 0, &p, &err);
    CHECK(s == SH_OK, "sat-blit: compiles ok");

    if (p) {
      sh_chunk *c = NULL;
      memset(&err, 0, sizeof(err));
      s = sh_lower(p, &c, &err);
      CHECK(s == SH_OK, "sat-blit: lowers ok");

      if (c) {
        // Use a 32-byte buffer (2 strips of 16)
        uint8_t oracle_buf[32], vm_buf[32], scalar_buf[32];
        for (int i = 0; i < 32; i++) {
          oracle_buf[i] = (uint8_t)(200 + (i & 7));  // values near 255 to test saturation
          vm_buf[i] = oracle_buf[i];
          scalar_buf[i] = oracle_buf[i];
        }

        sh_value delta;
        memset(&delta, 0, sizeof(delta));
        delta.kind = SH_K_VEC;
        delta.lanes = 16;
        delta.lane_kind = (uint8_t)SH_K_U8;
        for (int i = 0; i < 16; i++) delta.lane[i] = 100;  // will saturate

        sh_value out;
        sh_value args[2];

        memset(&out, 0, sizeof(out));
        args[0] = sh_val_region_raw(oracle_buf, 32, SH_K_U8, true);
        args[1] = delta;
        memset(&err, 0, sizeof(err));
        sh_status so = sh_invoke(p, args, 2, &out, &err);

        memset(&out, 0, sizeof(out));
        args[0] = sh_val_region_raw(vm_buf, 32, SH_K_U8, true);
        args[1] = delta;
        memset(&err, 0, sizeof(err));
        sh_status sv = sh_vm_run(c, args, 2, 0, &out, &err);

        memset(&out, 0, sizeof(out));
        args[0] = sh_val_region_raw(scalar_buf, 32, SH_K_U8, true);
        args[1] = delta;
        memset(&err, 0, sizeof(err));
        sh_status ss = sh_vm_run(c, args, 2, SH_VM_FORCE_SCALAR, &out, &err);

        CHECK(so == SH_OK && sv == SH_OK && ss == SH_OK, "sat-blit: all succeed");
        CHECK(memcmp(oracle_buf, vm_buf, 32) == 0, "sat-blit: oracle==vm");
        CHECK(memcmp(oracle_buf, scalar_buf, 32) == 0, "sat-blit: oracle==scalar");

        // Verify saturation: all values should be 255 (200+100 > 255, 207+100>255)
        int all_255 = 1;
        for (int i = 0; i < 32; i++) if (oracle_buf[i] != 255) all_255 = 0;
        CHECK(all_255, "sat-blit: all bytes saturated to 255");

        sh_chunk_free(c);
      }
      sh_free(p);
    }
  }
}

// ---------------------------------------------------------------------------
// SECTION 5: Bounds traps
// ---------------------------------------------------------------------------

static void test_bounds_traps(void) {
  printf("\n[bounds_traps]\n");

  // idx + N > len: must trap in both oracle and vm
  {
    sh_program *p = compile_only(
      "(defshader t ((buf (bytes u8))(i u32)) -> u8x16 (vregion-ref buf i 16))");
    CHECK(p != NULL, "bounds trap shader compiles");
    if (p) {
      sh_chunk *c = NULL;
      sh_error err;
      memset(&err, 0, sizeof(err));
      sh_lower(p, &c, &err);

      uint8_t data[20];
      memset(data, 0, sizeof(data));
      // Buffer has 20 bytes = 20 u8 elements. idx=10, N=16 -> 10+16=26 > 20: OOB
      sh_value buf_v = sh_val_region_raw(data, 20, SH_K_U8, false);
      sh_value idx_v = sh_val_u32(10);
      sh_value args[2] = {buf_v, idx_v};
      sh_value out;
      memset(&out, 0, sizeof(out));
      memset(&err, 0, sizeof(err));
      sh_status so = sh_invoke(p, args, 2, &out, &err);
      CHECK(so == SH_ERR_BOUNDS, "oracle: idx+N>len -> SH_ERR_BOUNDS");
      if (c) {
        memset(&err, 0, sizeof(err));
        sh_status sv = sh_vm_run(c, args, 2, 0, &out, &err);
        CHECK(sv == SH_ERR_BOUNDS, "vm: idx+N>len -> SH_ERR_BOUNDS");
        memset(&err, 0, sizeof(err));
        sh_status ss = sh_vm_run(c, args, 2, SH_VM_FORCE_SCALAR, &out, &err);
        CHECK(ss == SH_ERR_BOUNDS, "scalar-vm: idx+N>len -> SH_ERR_BOUNDS");
        sh_chunk_free(c);
      }
      sh_free(p);
    }
  }

  // Length not a multiple of N (last strip would overrun): idx=5, N=16, len=20
  {
    sh_program *p = compile_only(
      "(defshader t ((buf (bytes u8))(i u32)) -> u8x16 (vregion-ref buf i 16))");
    if (p) {
      sh_chunk *c = NULL;
      sh_error err;
      memset(&err, 0, sizeof(err));
      sh_lower(p, &c, &err);

      uint8_t data[20];
      memset(data, 0, sizeof(data));
      sh_value buf_v = sh_val_region_raw(data, 20, SH_K_U8, false);
      sh_value idx_v = sh_val_u32(5);
      sh_value args[2] = {buf_v, idx_v};
      sh_value out;
      memset(&out, 0, sizeof(out));
      memset(&err, 0, sizeof(err));
      sh_status so = sh_invoke(p, args, 2, &out, &err);
      CHECK(so == SH_ERR_BOUNDS, "oracle: partial strip overrun -> SH_ERR_BOUNDS");
      if (c) {
        memset(&err, 0, sizeof(err));
        sh_status sv = sh_vm_run(c, args, 2, 0, &out, &err);
        CHECK(sv == SH_ERR_BOUNDS, "vm: partial strip overrun -> SH_ERR_BOUNDS");
        sh_chunk_free(c);
      }
      sh_free(p);
    }
  }

  // Negative i64 index: should trap (becomes huge uint64)
  {
    sh_program *p = compile_only(
      "(defshader t ((buf (bytes u8))(i i64)) -> u8x16 (vregion-ref buf i 16))");
    if (p) {
      sh_chunk *c = NULL;
      sh_error err;
      memset(&err, 0, sizeof(err));
      sh_lower(p, &c, &err);

      uint8_t data[32];
      memset(data, 0, sizeof(data));
      sh_value buf_v = sh_val_region_raw(data, 32, SH_K_U8, false);
      sh_value idx_v = sh_val_i64(-1);
      sh_value args[2] = {buf_v, idx_v};
      sh_value out;
      memset(&out, 0, sizeof(out));
      memset(&err, 0, sizeof(err));
      sh_status so = sh_invoke(p, args, 2, &out, &err);
      CHECK(so == SH_ERR_BOUNDS, "oracle: negative i64 index -> SH_ERR_BOUNDS");
      if (c) {
        memset(&err, 0, sizeof(err));
        sh_status sv = sh_vm_run(c, args, 2, 0, &out, &err);
        CHECK(sv == SH_ERR_BOUNDS, "vm: negative i64 index -> SH_ERR_BOUNDS");
        sh_chunk_free(c);
      }
      sh_free(p);
    }
  }

  // In-bounds strip: must NOT trap
  {
    sh_program *p = compile_only(
      "(defshader t ((buf (bytes u8))(i u32)) -> u8x16 (vregion-ref buf i 16))");
    if (p) {
      sh_chunk *c = NULL;
      sh_error err;
      memset(&err, 0, sizeof(err));
      sh_lower(p, &c, &err);

      uint8_t data[32];
      for (int i = 0; i < 32; i++) data[i] = (uint8_t)i;
      sh_value buf_v = sh_val_region_raw(data, 32, SH_K_U8, false);
      sh_value idx_v = sh_val_u32(0);
      sh_value args[2] = {buf_v, idx_v};
      sh_value out;
      memset(&out, 0, sizeof(out));
      memset(&err, 0, sizeof(err));
      sh_status so = sh_invoke(p, args, 2, &out, &err);
      CHECK(so == SH_OK, "oracle: in-bounds strip never traps");
      if (c) {
        memset(&err, 0, sizeof(err));
        sh_status sv = sh_vm_run(c, args, 2, 0, &out, &err);
        CHECK(sv == SH_OK, "vm: in-bounds strip never traps");
        sh_chunk_free(c);
      }
      sh_free(p);
    }
  }

  // vregion-set! bounds trap
  {
    sh_program *p = compile_only(
      "(defshader t ((buf (bytes-mut u8))(i u32)(v u8x16)) -> u8x16"
      "  (vregion-set! buf i v))");
    if (p) {
      sh_chunk *c = NULL;
      sh_error err;
      memset(&err, 0, sizeof(err));
      sh_lower(p, &c, &err);

      uint8_t data[20];
      memset(data, 0, sizeof(data));
      sh_value vec_v;
      memset(&vec_v, 0, sizeof(vec_v));
      vec_v.kind = SH_K_VEC; vec_v.lanes = 16; vec_v.lane_kind = (uint8_t)SH_K_U8;
      sh_value buf_v = sh_val_region_raw(data, 20, SH_K_U8, true);
      sh_value idx_v = sh_val_u32(10);  // 10+16=26 > 20
      sh_value args[3] = {buf_v, idx_v, vec_v};
      sh_value out;
      memset(&out, 0, sizeof(out));
      memset(&err, 0, sizeof(err));
      sh_status so = sh_invoke(p, args, 3, &out, &err);
      CHECK(so == SH_ERR_BOUNDS, "oracle: vregion-set! OOB -> SH_ERR_BOUNDS");
      if (c) {
        memset(&err, 0, sizeof(err));
        sh_status sv = sh_vm_run(c, args, 3, 0, &out, &err);
        CHECK(sv == SH_ERR_BOUNDS, "vm: vregion-set! OOB -> SH_ERR_BOUNDS");
        sh_chunk_free(c);
      }
      sh_free(p);
    }
  }
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main(int argc, char **argv) {
  (void)argc; (void)argv;
  (void)compile_lower;  // suppress unused warning -- used in diff_vrload indirectly
  printf("=== test_vregion ===\n");

  test_parse();
  test_verify();
  test_interp();
  test_differential();
  test_bounds_traps();

  printf("\nResults: %d passed, %d failed\n", g_pass, g_fail);
  return g_fail > 0 ? 1 : 0;
}
