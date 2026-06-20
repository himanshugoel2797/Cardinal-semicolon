// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

// UNIT B test suite: type inference, loop bound verification, cost, caps.
// Uses sh_compile_string (frontend + verifier) so tests exercise the full
// compile pipeline up to (but not including) the interpreter.
//
// Covers:
//   - Scalar type inference on arithmetic / comparisons / let bindings
//   - Type mismatch rejection
//   - IF arm mismatch rejection
//   - Region load/store typing + immutable-store rejection + bad index type
//   - CALL type checking against a small prim_set: arity and type ok + rejections
//   - Vector promotion: (+ a b) on f32x4 params -> VBINOP
//   - Vector op typing: VSPLAT / VSHUFFLE / VREDUCE / VLANE / VSELECT
//   - Valid bounded loop with const bound: inferred bound kind + cost
//   - Param-bounded loop accepted by default, rejected with SH_REQUIRE_CONST_COST
//   - Unbounded loop rejected (no exit test, no const/param limit)
//   - SH_REQUIRE_CONST_COST: const-only program accepted, param-bound rejected

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "lisp.h"
#include "sh_internal.h"

static int failures = 0;
static int checks   = 0;

#define CHECK(cond, msg)                                        \
  do {                                                          \
    checks++;                                                   \
    if (!(cond)) {                                              \
      printf("  FAIL [%d] %s\n", __LINE__, (msg));             \
      failures++;                                               \
    } else {                                                    \
      printf("  ok   [%d] %s\n", __LINE__, (msg));             \
    }                                                           \
  } while (0)

// Like CHECK but also returns from the current function (abort on prerequisite failure)
#define REQUIRE(cond, msg)                                      \
  do {                                                          \
    checks++;                                                   \
    if (!(cond)) {                                              \
      printf("  FAIL [%d] %s  (aborting test)\n",              \
             __LINE__, (msg));                                   \
      failures++;                                               \
      return;                                                   \
    } else {                                                    \
      printf("  ok   [%d] %s\n", __LINE__, (msg));             \
    }                                                           \
  } while (0)

// Compile successfully and return the program (caller must sh_free).
// Returns NULL on failure; caller should REQUIRE(p != NULL, ...) before use.
static sh_program *compile_ok(const char *src, const sh_prim_set *prims,
                               uint32_t flags) {
  sh_program *prog = NULL;
  sh_error err = {0};
  sh_status s = sh_compile_string(src, prims, flags, &prog, &err);
  if (s != SH_OK) {
    printf("  [compile_ok failed: %s]\n", err.msg);
    return NULL;
  }
  return prog;
}

// Expect a specific failure status.
static void expect_fail(const char *src, const sh_prim_set *prims,
                        uint32_t flags, sh_status want, const char *label) {
  checks++;
  sh_program *prog = NULL;
  sh_error err = {0};
  sh_status s = sh_compile_string(src, prims, flags, &prog, &err);
  sh_free(prog);
  if (s != want) {
    printf("  FAIL [reject] %-50s got=%d want=%d (%s)\n",
           label, (int)s, (int)want, err.msg);
    failures++;
  } else {
    printf("  ok   [reject] %-50s %s\n", label, err.msg);
  }
}

// Find first node with a given op reachable from root (simple DFS)
#define MAX_NODES 4096
static uint8_t _vis[MAX_NODES];

static sh_nref find_op_inner(sh_program *p, sh_nref ref, sh_op op) {
  if (ref == SH_NREF_NONE || ref >= p->nnodes) return SH_NREF_NONE;
  if (ref < MAX_NODES && _vis[ref]) return SH_NREF_NONE;
  if (ref < MAX_NODES) _vis[ref] = 1;
  if ((sh_op)p->nodes[ref].op == op) return ref;
  sh_op nop = (sh_op)p->nodes[ref].op;
  bool is_leaf = (nop == SH_OP_PARAM || nop == SH_OP_LOCAL || nop == SH_OP_CONST);
  if (is_leaf) return SH_NREF_NONE;
  sh_nref found;
  if (p->nodes[ref].a < p->nnodes) {
    found = find_op_inner(p, p->nodes[ref].a, op);
    if (found != SH_NREF_NONE) return found;
  }
  if (p->nodes[ref].b < p->nnodes) {
    found = find_op_inner(p, p->nodes[ref].b, op);
    if (found != SH_NREF_NONE) return found;
  }
  if (p->nodes[ref].c < p->nnodes) {
    found = find_op_inner(p, p->nodes[ref].c, op);
    if (found != SH_NREF_NONE) return found;
  }
  return SH_NREF_NONE;
}

// Find the first node of the given op type reachable from root.
// Used by tests that need to locate specific nodes in the AST.
static sh_nref find_op(sh_program *p, sh_nref root, sh_op op)
  __attribute__((unused));
static sh_nref find_op(sh_program *p, sh_nref root, sh_op op) {
  memset(_vis, 0, sizeof(_vis));
  return find_op_inner(p, root, op);
}

// =============================================================================
// 1. Scalar type inference
// =============================================================================

static void test_scalar_type_inference(void) {
  printf("--- 1. scalar type inference ---\n");

  sh_program *p;
  sh_nref root;

  // Identity: param type propagates
  p = compile_ok("(defshader id ((x u32)) -> u32 x)", NULL, 0);
  REQUIRE(p != NULL, "identity compiles");
  CHECK(p->nodes[p->root].type.kind == (uint8_t)SH_K_U32, "root type = u32");
  CHECK(sh_type_eq(sh_return_type(p), sh_type_scalar(SH_K_U32)), "return type u32");
  sh_free(p);

  // Arithmetic: (+ a b) on i64 -> i64
  p = compile_ok("(defshader add2 ((a i64) (b i64)) -> i64 (+ a b))", NULL, 0);
  REQUIRE(p != NULL, "add2 compiles");
  root = p->root;
  CHECK(p->nodes[root].type.kind == (uint8_t)SH_K_I64, "add2 result type = i64");
  sh_free(p);

  // Integer literal adopts consumer type
  p = compile_ok("(defshader plus1 ((x u32)) -> u32 (+ x 1))", NULL, 0);
  REQUIRE(p != NULL, "integer literal type inference");
  root = p->root;
  {
    sh_nref rb = p->nodes[root].b;
    CHECK(p->nodes[rb].type.kind == (uint8_t)SH_K_U32,
          "integer literal '1' inferred as u32");
  }
  sh_free(p);

  // Float literal -> f64 by default
  p = compile_ok("(defshader flit ((x f64)) -> f64 (+ x 1.5))", NULL, 0);
  REQUIRE(p != NULL, "float literal inferred");
  {
    sh_nref rb = p->nodes[p->root].b;
    CHECK(p->nodes[rb].type.kind == (uint8_t)SH_K_F64, "float literal inferred f64");
  }
  sh_free(p);

  // Comparison: (< x 0) -> bool
  p = compile_ok("(defshader lt_test ((x i64)) -> bool (< x 0))", NULL, 0);
  REQUIRE(p != NULL, "comparison infers bool");
  CHECK(p->nodes[p->root].type.kind == (uint8_t)SH_K_BOOL, "CMP result type = bool");
  sh_free(p);

  // Cast: (u32 x) where x is i64
  p = compile_ok("(defshader cast_test ((x i64)) -> u32 (u32 x))", NULL, 0);
  REQUIRE(p != NULL, "cast compiles");
  CHECK(p->nodes[p->root].type.kind == (uint8_t)SH_K_U32, "cast result = u32");
  sh_free(p);

  // Let binding type propagation
  p = compile_ok("(defshader sq ((x i64)) -> i64 (let ((y x)) (* y y)))", NULL, 0);
  REQUIRE(p != NULL, "let binding");
  CHECK(p->nodes[p->root].type.kind == (uint8_t)SH_K_I64, "let body type = i64");
  sh_free(p);
}

// =============================================================================
// 2. Type mismatch rejection
// =============================================================================

static void test_type_mismatch(void) {
  printf("--- 2. type mismatch rejection ---\n");

  // Binary op with mismatched types: u32 + i64
  expect_fail(
    "(defshader bad ((a u32) (b i64)) -> u32 (+ a b))",
    NULL, 0, SH_ERR_TYPE, "u32 + i64 mismatch");

  // NEG on bool
  expect_fail(
    "(defshader bad ((x bool)) -> bool (- x))",
    NULL, 0, SH_ERR_TYPE, "NEG on bool");

  // NOT on u32
  expect_fail(
    "(defshader bad ((x u32)) -> bool (not x))",
    NULL, 0, SH_ERR_TYPE, "NOT on u32");

  // CVT of region type
  expect_fail(
    "(defshader bad ((buf (bytes u32)) (i u32)) -> u32 (u32 buf))",
    NULL, 0, SH_ERR_TYPE, "CVT of region");
}

// =============================================================================
// 3. IF arm mismatch
// =============================================================================

static void test_if_arm_mismatch(void) {
  printf("--- 3. IF arm type mismatch ---\n");

  // Then = u32, else = f32 (different kinds)
  expect_fail(
    "(defshader bad ((x bool) (a u32) (b f32)) -> u32 (if x a b))",
    NULL, 0, SH_ERR_TYPE, "if arms: u32 vs f32");

  // Then = bool, else = i64
  expect_fail(
    "(defshader bad ((cond bool) (a bool) (b i64)) -> bool (if cond a b))",
    NULL, 0, SH_ERR_TYPE, "if arms: bool vs i64");

  // Matching arms should compile
  sh_program *p = compile_ok(
    "(defshader ok ((cond bool) (a u32) (b u32)) -> u32 (if cond a b))",
    NULL, 0);
  REQUIRE(p != NULL, "if with matching u32 arms");
  CHECK(p->nodes[p->root].type.kind == (uint8_t)SH_K_U32, "if result = u32");
  sh_free(p);
}

// =============================================================================
// 4. Region load/store typing
// =============================================================================

static void test_region_typing(void) {
  printf("--- 4. region load/store typing ---\n");

  sh_program *p;

  // region-ref on (bytes u8) returns u8
  p = compile_ok(
    "(defshader getbyte ((buf (bytes u8)) (i u32)) -> u8 (region-ref buf i))",
    NULL, 0);
  REQUIRE(p != NULL, "region-ref compiles");
  CHECK(p->nodes[p->root].type.kind == (uint8_t)SH_K_U8, "region-ref result = u8");
  sh_free(p);

  // region-set! on (bytes-mut u32) with u32 value
  p = compile_ok(
    "(defshader setword ((buf (bytes-mut u32)) (i u32) (v u32)) -> u32"
    "  (region-set! buf i v))",
    NULL, 0);
  REQUIRE(p != NULL, "region-set! compiles");
  CHECK(p->nodes[p->root].type.kind == (uint8_t)SH_K_U32,
        "region-set! result type = u32");
  sh_free(p);

  // Immutable store: reject
  expect_fail(
    "(defshader bad ((buf (bytes u8)) (i u32) (v u8)) -> u8 (region-set! buf i v))",
    NULL, 0, SH_ERR_TYPE, "store to immutable region");

  // Wrong index type: bool index
  expect_fail(
    "(defshader bad ((buf (bytes u8)) (cond bool)) -> u8 (region-ref buf cond))",
    NULL, 0, SH_ERR_TYPE, "region-ref with bool index");

  // Wrong value type for store: try to store u32 into (bytes-mut u8)
  expect_fail(
    "(defshader bad ((buf (bytes-mut u8)) (i u32) (v u32)) -> u8"
    "  (region-set! buf i v))",
    NULL, 0, SH_ERR_TYPE, "store value type mismatch (u32 into bytes-mut u8)");

  // region-len returns u32
  p = compile_ok(
    "(defshader rlen ((buf (bytes u32))) -> u32 (region-len buf))",
    NULL, 0);
  REQUIRE(p != NULL, "region-len compiles");
  CHECK(p->nodes[p->root].type.kind == (uint8_t)SH_K_U32, "region-len result = u32");
  sh_free(p);
}

// =============================================================================
// 5. CALL against prim set: arity + type
// =============================================================================

static void test_call_prim(void) {
  printf("--- 5. CALL prim type checking ---\n");

  static sh_prim prims_arr[2] = {
    {
      .name    = "fold-carry",
      .ret     = {SH_K_U32, 0, 0, 0},
      .nparams = 2,
      .params  = {{SH_K_U32, 0, 0, 0}, {SH_K_U32, 0, 0, 0}},
      .fn      = NULL,
    },
    {
      .name    = "f32-sin",
      .ret     = {SH_K_F32, 0, 0, 0},
      .nparams = 1,
      .params  = {{SH_K_F32, 0, 0, 0}},
      .fn      = NULL,
    },
  };
  static sh_prim_set ps = { prims_arr, 2 };

  sh_program *p;

  // OK: correct arity and types
  p = compile_ok(
    "(defshader callprim ((a u32) (b u32)) -> u32 (fold-carry a b))",
    &ps, 0);
  REQUIRE(p != NULL, "call fold-carry ok");
  CHECK(p->nodes[p->root].type.kind == (uint8_t)SH_K_U32, "fold-carry result = u32");
  sh_free(p);

  // OK: single-arg float prim
  p = compile_ok("(defshader sin_test ((x f32)) -> f32 (f32-sin x))", &ps, 0);
  REQUIRE(p != NULL, "call f32-sin ok");
  CHECK(p->nodes[p->root].type.kind == (uint8_t)SH_K_F32, "f32-sin result = f32");
  sh_free(p);

  // Wrong arity: fold-carry with 3 args
  expect_fail(
    "(defshader bad ((a u32) (b u32) (c u32)) -> u32 (fold-carry a b c))",
    &ps, 0, SH_ERR_ARITY, "fold-carry wrong arity");

  // Wrong arg type: fold-carry with i64 arg
  expect_fail(
    "(defshader bad ((a u32) (b i64)) -> u32 (fold-carry a b))",
    &ps, 0, SH_ERR_TYPE, "fold-carry wrong arg type");

  // Not whitelisted: unknown prim with no prims at all
  expect_fail(
    "(defshader bad ((a u32)) -> u32 (unknown-prim a))",
    NULL, 0, SH_ERR_NOT_WHITELISTED, "call not whitelisted (no prims)");

  // Not whitelisted: name not in prim set
  expect_fail(
    "(defshader bad ((a u32)) -> u32 (other-prim a))",
    &ps, 0, SH_ERR_NOT_WHITELISTED, "call not in prim set");
}

// =============================================================================
// 6. Vector promotion: BINOP on vectors -> VBINOP
// =============================================================================

static void test_vector_promotion(void) {
  printf("--- 6. vector promotion BINOP -> VBINOP ---\n");

  sh_program *p;

  // (+ a b) where a,b are f32x4 -> should become VBINOP
  p = compile_ok("(defshader vadd ((a f32x4) (b f32x4)) -> f32x4 (+ a b))", NULL, 0);
  REQUIRE(p != NULL, "vector + compiles");
  {
    sh_nref root = p->root;
    CHECK(p->nodes[root].op == (uint16_t)SH_OP_VBINOP,
          "(+ f32x4 f32x4) promoted to VBINOP");
    CHECK(p->nodes[root].type.kind == (uint8_t)SH_K_VEC, "VBINOP result kind = VEC");
    CHECK(p->nodes[root].type.lane_kind == (uint8_t)SH_K_F32,
          "VBINOP result lane_kind = f32");
    CHECK(p->nodes[root].type.lanes == 4, "VBINOP result lanes = 4");
  }
  sh_free(p);

  // (* a b) on u8x16 -> VBINOP
  p = compile_ok("(defshader vmul ((a u8x16) (b u8x16)) -> u8x16 (* a b))", NULL, 0);
  REQUIRE(p != NULL, "u8x16 * compiles");
  CHECK(p->nodes[p->root].op == (uint16_t)SH_OP_VBINOP, "u8x16 * -> VBINOP");
  sh_free(p);

  // Vector CMP on scalars derived from vectors
  p = compile_ok(
    "(defshader vcmp_test ((a f32x4) (b f32x4)) -> f32x4"
    "  (if (< (vreduce-add a) (vreduce-add b)) a b))",
    NULL, 0);
  REQUIRE(p != NULL, "vector reduce then scalar cmp compiles");
  sh_free(p);

  // Mismatched vector types -> error
  expect_fail(
    "(defshader bad ((a f32x4) (b u8x16)) -> f32x4 (+ a b))",
    NULL, 0, SH_ERR_TYPE, "vector type mismatch");
}

// =============================================================================
// 7. Vector op typing
// =============================================================================

static void test_vector_ops(void) {
  printf("--- 7. vector op typing ---\n");

  sh_program *p;

  // VSPLAT: f32 -> vec4
  p = compile_ok("(defshader sp ((x f32)) -> vec4 (splat x))", NULL, 0);
  REQUIRE(p != NULL, "splat compiles");
  {
    sh_nref root = p->root;
    CHECK(p->nodes[root].op == (uint16_t)SH_OP_VSPLAT, "root is VSPLAT");
    CHECK(p->nodes[root].type.kind == (uint8_t)SH_K_VEC, "VSPLAT result = VEC");
    CHECK(p->nodes[root].type.lane_kind == (uint8_t)SH_K_F32, "VSPLAT lane = f32");
    CHECK(p->nodes[root].type.lanes == 4, "VSPLAT lanes = 4");
  }
  sh_free(p);

  // VSHUFFLE: f32x4 -> f32x4 with (shuffle v 3 2 1 0)
  p = compile_ok("(defshader shuf ((v f32x4)) -> f32x4 (shuffle v 3 2 1 0))", NULL, 0);
  REQUIRE(p != NULL, "shuffle compiles");
  {
    sh_nref root = p->root;
    CHECK(p->nodes[root].op == (uint16_t)SH_OP_VSHUFFLE, "root is VSHUFFLE");
    CHECK(p->nodes[root].type.kind == (uint8_t)SH_K_VEC, "VSHUFFLE result = VEC");
    CHECK(p->nodes[root].type.lanes == 4, "VSHUFFLE result lanes = 4");
  }
  sh_free(p);

  // VSHUFFLE index out of bounds
  expect_fail(
    "(defshader bad ((v f32x4)) -> f32x4 (shuffle v 0 1 2 5))",
    NULL, 0, SH_ERR_TYPE, "shuffle index out of source lane count");

  // VREDUCE: f32x4 -> f32
  p = compile_ok("(defshader vsum ((v f32x4)) -> f32 (vreduce-add v))", NULL, 0);
  REQUIRE(p != NULL, "vreduce-add compiles");
  {
    sh_nref root = p->root;
    CHECK(p->nodes[root].op == (uint16_t)SH_OP_VREDUCE, "root is VREDUCE");
    CHECK(p->nodes[root].type.kind == (uint8_t)SH_K_F32, "VREDUCE result = f32");
  }
  sh_free(p);

  // DOT: f32x4 x f32x4 -> f32
  p = compile_ok("(defshader dp ((a f32x4) (b f32x4)) -> f32 (dot a b))", NULL, 0);
  REQUIRE(p != NULL, "dot product compiles");
  CHECK(p->nodes[p->root].type.kind == (uint8_t)SH_K_F32, "dot result = f32");
  sh_free(p);

  // VLANE: f32x4 lane 2 -> f32
  p = compile_ok("(defshader lx ((v f32x4)) -> f32 (lane v 2))", NULL, 0);
  REQUIRE(p != NULL, "vlane compiles");
  {
    sh_nref root = p->root;
    CHECK(p->nodes[root].op == (uint16_t)SH_OP_VLANE, "root is VLANE");
    CHECK(p->nodes[root].type.kind == (uint8_t)SH_K_F32, "VLANE result = f32");
  }
  sh_free(p);

  // VLANE index out of bounds (lane 4 in a 4-lane vector is out of range)
  expect_fail(
    "(defshader bad ((v f32x4)) -> f32 (lane v 4))",
    NULL, 0, SH_ERR_TYPE, "vlane index >= lanes");
}

// =============================================================================
// 8. Bounded loop verification (const bound)
// =============================================================================

static void test_bounded_loop_const(void) {
  printf("--- 8. bounded loop (const bound) ---\n");

  // Sum 0..9: loop from i=0, exit when i>=10, step +1
  const char *src =
    "(defshader sum10 () -> i64"
    "  (let loop ((i 0) (acc 0))"
    "    (if (>= i 10) acc (loop (+ i 1) (+ acc i)))))";

  sh_program *p = compile_ok(src, NULL, 0);
  REQUIRE(p != NULL, "const-bound loop compiles");
  CHECK(p->nloops == 1, "1 loop record");
  CHECK(p->loops[0].bound.kind == SH_BOUND_CONST, "bound is CONST");
  CHECK(p->loops[0].bound.konst == 10, "trip count ceiling = 10");
  CHECK(p->loops[0].bound.per_iter_cost > 0, "per_iter_cost > 0");
  CHECK(sh_cost_is_const(p), "program cost is const");
  CHECK(sh_static_cost(p) > 0, "static cost > 0");
  sh_free(p);

  // Accepted with SH_REQUIRE_CONST_COST
  p = compile_ok(src, NULL, SH_REQUIRE_CONST_COST);
  REQUIRE(p != NULL, "const-bound loop + REQUIRE_CONST_COST");
  CHECK(sh_cost_is_const(p), "cost is const");
  sh_free(p);
}

// =============================================================================
// 9. Bounded loop with param bound
// =============================================================================

static void test_bounded_loop_param(void) {
  printf("--- 9. bounded loop (param bound) ---\n");

  const char *src =
    "(defshader sumN ((n u32)) -> i64"
    "  (let loop ((i 0) (acc 0))"
    "    (if (>= i n) acc (loop (+ i 1) (+ acc i)))))";

  // Without REQUIRE_CONST_COST: ok
  sh_program *p = compile_ok(src, NULL, 0);
  REQUIRE(p != NULL, "param-bound loop compiles");
  CHECK(p->nloops == 1, "1 loop record");
  CHECK(p->loops[0].bound.kind == SH_BOUND_PARAM, "bound is PARAM");
  CHECK(!sh_cost_is_const(p), "program cost is NOT const");
  sh_free(p);

  // With REQUIRE_CONST_COST: reject
  expect_fail(src, NULL, SH_REQUIRE_CONST_COST, SH_ERR_NONCONST_COST,
              "param-bound loop + REQUIRE_CONST_COST");
}

// =============================================================================
// 10. Unbounded loop rejection
// =============================================================================

static void test_unbounded_loop(void) {
  printf("--- 10. unbounded loop rejection ---\n");

  // Loop with no exit condition (body is just RECUR without an if)
  expect_fail(
    "(defshader badloop ((n i64)) -> i64"
    "  (let loop ((i 0)) (loop (+ i 1))))",
    NULL, 0, SH_ERR_UNBOUNDED_LOOP, "loop with no exit IF");

  // Loop where exit test doesn't involve an induction var
  expect_fail(
    "(defshader badloop2 ((flag bool)) -> i64"
    "  (let loop ((i 0))"
    "    (if flag 0 (loop (+ i 1)))))",
    NULL, 0, SH_ERR_UNBOUNDED_LOOP, "exit test not on induction var");

  // Loop where step is a param (non-const positive)
  expect_fail(
    "(defshader badstep ((n i64) (step i64)) -> i64"
    "  (let loop ((i 0))"
    "    (if (>= i n) 0 (loop (+ i step)))))",
    NULL, 0, SH_ERR_UNBOUNDED_LOOP, "loop step is a param (not const)");
}

// =============================================================================
// 11. Bounded loop over region (bounds annotation)
// =============================================================================

static void test_loop_over_region(void) {
  printf("--- 11. loop over region with bounds annotation ---\n");

  // Classic pattern: (let loop ((i 0)) (if (>= i (region-len buf)) ...
  const char *src =
    "(defshader sum_buf ((buf (bytes u32))) -> u64"
    "  (let loop ((i 0) (acc 0))"
    "    (if (>= i (region-len buf))"
    "        acc"
    "        (loop (+ i 1) (+ acc (u64 (region-ref buf i)))))))";

  sh_program *p = compile_ok(src, NULL, 0);
  REQUIRE(p != NULL, "loop over region compiles");
  CHECK(p->nloops == 1, "1 loop");
  // Bound is PARAM (since region-len is runtime)
  CHECK(p->loops[0].bound.kind == SH_BOUND_PARAM, "region-len bound is PARAM");
  // Check that at least one REGION_LOAD has SH_NF_BOUNDS_PROVEN
  {
    bool found_proven = false;
    for (uint32_t j = 0; j < p->nnodes; j++) {
      if (p->nodes[j].op == (uint16_t)SH_OP_REGION_LOAD &&
          (p->nodes[j].vflags & SH_NF_BOUNDS_PROVEN)) {
        found_proven = true;
        break;
      }
    }
    CHECK(found_proven, "REGION_LOAD has SH_NF_BOUNDS_PROVEN set");
  }
  sh_free(p);

  // Reject loop over region with REQUIRE_CONST_COST (runtime length)
  expect_fail(src, NULL, SH_REQUIRE_CONST_COST, SH_ERR_NONCONST_COST,
              "region-len bounded loop + REQUIRE_CONST_COST");
}

// =============================================================================
// 12. Return type mismatch
// =============================================================================

static void test_return_type_mismatch(void) {
  printf("--- 12. return type mismatch ---\n");

  // Declared return u32 but actual is i64
  expect_fail(
    "(defshader bad ((x i64)) -> u32 x)",
    NULL, 0, SH_ERR_TYPE, "declared u32 but body is i64");

  // Declared return f32 but body is f64
  expect_fail(
    "(defshader bad ((x f64)) -> f32 x)",
    NULL, 0, SH_ERR_TYPE, "declared f32 but body is f64");
}

// =============================================================================
// 13. SH_REQUIRE_CONST_COST: const program accepted
// =============================================================================

static void test_require_const_cost_ok(void) {
  printf("--- 13. REQUIRE_CONST_COST: const program accepted ---\n");

  sh_program *p;

  // Simple arithmetic: no loops -> const cost
  p = compile_ok(
    "(defshader add2 ((a u32) (b u32)) -> u32 (+ a b))",
    NULL, SH_REQUIRE_CONST_COST);
  REQUIRE(p != NULL, "simple arithmetic with REQUIRE_CONST_COST");
  CHECK(sh_cost_is_const(p), "cost is const");
  sh_free(p);

  // Const-bound loop: accepted
  p = compile_ok(
    "(defshader sum10 () -> i64"
    "  (let loop ((i 0) (acc 0))"
    "    (if (>= i 10) acc (loop (+ i 1) (+ acc i)))))",
    NULL, SH_REQUIRE_CONST_COST);
  REQUIRE(p != NULL, "const-bound loop with REQUIRE_CONST_COST");
  CHECK(sh_cost_is_const(p), "cost is const");
  sh_free(p);
}

// =============================================================================
// 14. Multiple bindings + nested let
// =============================================================================

static void test_nested_let(void) {
  printf("--- 14. nested let type inference ---\n");

  sh_program *p = compile_ok(
    "(defshader chain ((x i64)) -> i64"
    "  (let* ((a (+ x 1)) (b (+ a 2)) (c (+ b 3))) c))",
    NULL, 0);
  REQUIRE(p != NULL, "let* chain");
  CHECK(p->nodes[p->root].type.kind == (uint8_t)SH_K_I64,
        "let* chain result = i64");
  sh_free(p);
}

// =============================================================================
// main
// =============================================================================

int main(void) {
  // The reader interns symbols; bring up the runtime.
  (void)lisp_default_env();

  printf("[lisp_shader verifier tests]\n\n");

  test_scalar_type_inference();
  test_type_mismatch();
  test_if_arm_mismatch();
  test_region_typing();
  test_call_prim();
  test_vector_promotion();
  test_vector_ops();
  test_bounded_loop_const();
  test_bounded_loop_param();
  test_unbounded_loop();
  test_loop_over_region();
  test_return_type_mismatch();
  test_require_const_cost_ok();
  test_nested_let();

  printf("\n[lisp_shader verifier] %d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
