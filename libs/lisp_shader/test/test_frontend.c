// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

// UNIT A test suite: parse / desugar / banned-form rejection.
// Tests shf_parse directly (the verifier is still a stub, so we don't go
// through sh_compile end-to-end for success cases -- we inspect the produced
// p->nodes/params/root/nlocals directly).
//
// Run with: bash libs/lisp_shader/test/build-and-run.sh

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

// ----- helpers ----------------------------------------------------------------

// Read + parse a defshader form. Returns SH_OK on success and fills *p_out
// (caller must sh_free). On failure fills *err_out and returns the error code.
// The program is zeroed+prims-wired before shf_parse is called.
static sh_status parse_shader(const char *src, const sh_prim_set *prims,
                               sh_program **p_out, sh_error *err_out) {
  *p_out = NULL;
  *err_out = (sh_error){0};

  const char *cur = src;
  const char *end = src + strlen(src);
  const char *rerr = NULL;
  lisp_value form = lisp_read(&cur, end, &rerr);
  if (rerr) {
    err_out->status = SH_ERR_PARSE;
    snprintf(err_out->msg, sizeof(err_out->msg), "reader: %s", rerr);
    return SH_ERR_PARSE;
  }

  sh_program *p = calloc(1, sizeof(*p));
  if (!p) {
    err_out->status = SH_ERR_OOM;
    return SH_ERR_OOM;
  }
  p->root  = SH_NREF_NONE;
  p->prims = prims;

  sh_status s = shf_parse(form, p, err_out);
  if (s != SH_OK) {
    sh_free(p);
    return s;
  }
  *p_out = p;
  return SH_OK;
}

// Ops whose a/b/c fields are NOT child node references (they hold raw values):
// PARAM (a=param idx), LOCAL (a=slot), CONST (imm), VLANE (a=vec nref, imm=lane)
// For these we only recurse if the field is clearly a valid nref (< nnodes)
// and we use a visited bitmap to prevent infinite loops on DAG-shared nodes.

#define MAX_NODES 4096
static uint8_t _visited[MAX_NODES];

static bool _is_leaf(sh_op op) {
  return op == SH_OP_PARAM || op == SH_OP_LOCAL || op == SH_OP_CONST;
}

// Return true if nref looks like a valid child reference (not a raw immediate)
static bool _is_child_ref(sh_program *p, sh_nref ref) {
  return ref != SH_NREF_NONE && ref < p->nnodes;
}

static sh_nref _find_op_inner(sh_program *p, sh_nref root, sh_op op) {
  if (root == SH_NREF_NONE || root >= p->nnodes) return SH_NREF_NONE;
  if (root < MAX_NODES && _visited[root]) return SH_NREF_NONE;
  if (root < MAX_NODES) _visited[root] = 1;

  sh_op node_op = (sh_op)p->nodes[root].op;
  if (node_op == op) return root;
  if (_is_leaf(node_op)) return SH_NREF_NONE;  // no valid child refs

  sh_nref found;
  if (_is_child_ref(p, p->nodes[root].a)) {
    found = _find_op_inner(p, p->nodes[root].a, op);
    if (found != SH_NREF_NONE) return found;
  }
  if (_is_child_ref(p, p->nodes[root].b)) {
    found = _find_op_inner(p, p->nodes[root].b, op);
    if (found != SH_NREF_NONE) return found;
  }
  if (_is_child_ref(p, p->nodes[root].c)) {
    found = _find_op_inner(p, p->nodes[root].c, op);
    if (found != SH_NREF_NONE) return found;
  }
  return SH_NREF_NONE;
}

// Find the first node with the given op reachable from root.
static sh_nref find_op(sh_program *p, sh_nref root, sh_op op) {
  memset(_visited, 0, sizeof(_visited));
  return _find_op_inner(p, root, op);
}

// ----- REJECTION helper -------------------------------------------------------

static void expect_fail(const char *src, sh_status want,
                        const char *label, const sh_prim_set *prims) {
  checks++;
  sh_program *p = NULL;
  sh_error err = {0};
  sh_status s = parse_shader(src, prims, &p, &err);
  sh_free(p);
  if (s != want) {
    printf("  FAIL [reject] %-45s: got %d want %d (%s)\n", label, (int)s, (int)want, err.msg);
    failures++;
  } else {
    printf("  ok   [reject] %-45s: %s\n", label, err.msg);
  }
}

// =============================================================================
// TEST CASES
// =============================================================================

// --- 1. Simple identity shader -----------------------------------------------

static void test_simple_identity(void) {
  printf("--- 1. simple identity shader ---\n");

  sh_program *p = NULL;
  sh_error err = {0};
  sh_status s = parse_shader(
    "(defshader id ((x u32)) -> u32 x)",
    NULL, &p, &err);

  CHECK(s == SH_OK, "parse succeeds");
  if (s != SH_OK) { printf("   msg: %s\n", err.msg); return; }

  CHECK(strcmp(sh_name(p), "id") == 0, "shader name = id");
  CHECK(sh_param_count(p) == 1, "1 param");
  CHECK(sh_param_type(p, 0).kind == SH_K_U32, "param 0 is u32");
  CHECK(sh_type_eq(sh_return_type(p), sh_type_scalar(SH_K_U32)), "return type u32");

  // Root should be SH_OP_PARAM with a=0
  CHECK(p->root != SH_NREF_NONE, "root is set");
  CHECK(p->nodes[p->root].op == (uint16_t)SH_OP_PARAM, "root is PARAM");
  CHECK(p->nodes[p->root].a  == 0, "param index 0");

  sh_free(p);
}

// --- 2. Arithmetic expressions -----------------------------------------------

static void test_arithmetic(void) {
  printf("--- 2. arithmetic expressions ---\n");

  sh_program *p = NULL;
  sh_error err = {0};
  sh_status s = parse_shader(
    "(defshader add2 ((a i64) (b i64)) -> i64 (+ a b))",
    NULL, &p, &err);

  CHECK(s == SH_OK, "parse add2 succeeds");
  if (s != SH_OK) { printf("   msg: %s\n", err.msg); sh_free(p); return; }

  CHECK(sh_param_count(p) == 2, "2 params");
  CHECK(sh_param_type(p, 0).kind == SH_K_I64, "param 0 i64");
  CHECK(sh_param_type(p, 1).kind == SH_K_I64, "param 1 i64");

  sh_nref root = p->root;
  CHECK(p->nodes[root].op  == (uint16_t)SH_OP_BINOP, "root is BINOP");
  CHECK(p->nodes[root].sub == (uint16_t)SH_BIN_ADD,  "BINOP sub = ADD");
  // children are PARAM nodes
  sh_nref la = p->nodes[root].a;
  sh_nref lb = p->nodes[root].b;
  CHECK(la != SH_NREF_NONE && p->nodes[la].op == (uint16_t)SH_OP_PARAM, "left = PARAM");
  CHECK(lb != SH_NREF_NONE && p->nodes[lb].op == (uint16_t)SH_OP_PARAM, "right = PARAM");
  CHECK(p->nodes[la].a == 0, "left param idx = 0");
  CHECK(p->nodes[lb].a == 1, "right param idx = 1");

  sh_free(p);

  // unary minus
  s = parse_shader("(defshader neg ((x i64)) -> i64 (- x))", NULL, &p, &err);
  CHECK(s == SH_OK, "parse unary minus");
  if (s == SH_OK) {
    CHECK(p->nodes[p->root].op  == (uint16_t)SH_OP_UNOP, "root is UNOP");
    CHECK(p->nodes[p->root].sub == (uint16_t)SH_UN_NEG,  "UNOP sub = NEG");
    sh_free(p);
  }

  // bitwise ops
  s = parse_shader("(defshader bitops ((a u32) (b u32)) -> u32 (bit-and a b))",
                   NULL, &p, &err);
  CHECK(s == SH_OK, "parse bit-and");
  if (s == SH_OK) {
    CHECK(p->nodes[p->root].sub == (uint16_t)SH_BIN_AND, "BIN_AND");
    sh_free(p);
  }

  s = parse_shader("(defshader s ((a u32) (b u32)) -> u32 (shl a b))", NULL, &p, &err);
  CHECK(s == SH_OK, "parse shl");
  if (s == SH_OK) {
    CHECK(p->nodes[p->root].sub == (uint16_t)SH_BIN_SHL, "BIN_SHL");
    sh_free(p);
  }
}

// --- 3. Comparisons + if + bool literals -------------------------------------

static void test_if_bool(void) {
  printf("--- 3. if + bool + comparisons ---\n");

  sh_program *p = NULL;
  sh_error err = {0};
  sh_status s = parse_shader(
    "(defshader clamp-pos ((x i64)) -> i64 (if (< x 0) 0 x))",
    NULL, &p, &err);

  CHECK(s == SH_OK, "parse clamp-pos");
  if (s != SH_OK) { printf("   msg: %s\n", err.msg); sh_free(p); return; }

  sh_nref root = p->root;
  CHECK(p->nodes[root].op == (uint16_t)SH_OP_IF, "root is IF");
  sh_nref cond = p->nodes[root].a;
  CHECK(p->nodes[cond].op  == (uint16_t)SH_OP_CMP, "cond is CMP");
  CHECK(p->nodes[cond].sub == (uint16_t)SH_CMP_LT, "CMP_LT");

  // then = CONST 0
  sh_nref then_ = p->nodes[root].b;
  CHECK(p->nodes[then_].op  == (uint16_t)SH_OP_CONST, "then is CONST");
  CHECK(p->nodes[then_].imm == 0, "const 0");

  // else = PARAM
  sh_nref else_ = p->nodes[root].c;
  CHECK(p->nodes[else_].op == (uint16_t)SH_OP_PARAM, "else is PARAM");

  sh_free(p);
}

// --- 4. Let bindings ---------------------------------------------------------

static void test_let(void) {
  printf("--- 4. let bindings ---\n");

  sh_program *p = NULL;
  sh_error err = {0};
  sh_status s = parse_shader(
    "(defshader sq ((x i64)) -> i64 (let ((y x)) (* y y)))",
    NULL, &p, &err);

  CHECK(s == SH_OK, "parse let-sq");
  if (s != SH_OK) { printf("   msg: %s\n", err.msg); sh_free(p); return; }

  // Root should be LET node
  sh_nref root = p->root;
  CHECK(p->nodes[root].op == (uint16_t)SH_OP_LET, "root is LET");
  CHECK(p->nodes[root].aux_len == 1, "1 binding");

  // The init expr is in aux[aux_off]: should be PARAM 0
  uint32_t aoff = p->nodes[root].aux_off;
  sh_nref init_ref = p->aux[aoff];
  CHECK(p->nodes[init_ref].op == (uint16_t)SH_OP_PARAM, "init is PARAM");

  // Body: should be BINOP(MUL, LOCAL 0, LOCAL 0)
  sh_nref body = p->nodes[root].b;
  CHECK(p->nodes[body].op  == (uint16_t)SH_OP_BINOP, "body is BINOP");
  CHECK(p->nodes[body].sub == (uint16_t)SH_BIN_MUL,  "MUL");
  sh_nref la = p->nodes[body].a;
  sh_nref lb = p->nodes[body].b;
  CHECK(p->nodes[la].op == (uint16_t)SH_OP_LOCAL, "left = LOCAL");
  CHECK(p->nodes[lb].op == (uint16_t)SH_OP_LOCAL, "right = LOCAL");
  CHECK(p->nodes[la].a == 0, "local slot 0");
  CHECK(p->nodes[lb].a == 0, "local slot 0 (same var)");

  // nlocals must be at least 1
  CHECK(p->nlocals >= 1, "nlocals >= 1");

  sh_free(p);
}

// --- 5. Named-let (bounded loop) --------------------------------------------

static void test_named_let_loop(void) {
  printf("--- 5. named-let (LOOP + RECUR) ---\n");

  sh_program *p = NULL;
  sh_error err = {0};
  // Sum from 0 to n: (let loop ((i 0) (acc 0)) (if (>= i n) acc (loop (+ i 1) (+ acc i))))
  const char *src =
    "(defshader sumN ((n i64)) -> i64"
    "  (let loop ((i 0) (acc 0))"
    "    (if (>= i n)"
    "        acc"
    "        (loop (+ i 1) (+ acc i)))))";

  sh_status s = parse_shader(src, NULL, &p, &err);
  CHECK(s == SH_OK, "parse named-let loop");
  if (s != SH_OK) { printf("   msg: %s\n", err.msg); sh_free(p); return; }

  // Root must be SH_OP_LOOP
  sh_nref root = p->root;
  CHECK(p->nodes[root].op == (uint16_t)SH_OP_LOOP, "root is LOOP");

  // Loop table must have exactly 1 entry
  CHECK(p->nloops == 1, "1 loop record");
  sh_loop *lp = &p->loops[0];
  CHECK(lp->nvars == 2, "2 induction vars (i, acc)");

  // induction var slots should start at 0 (they are the first locals)
  CHECK(lp->var_slot0 == 0, "var_slot0 = 0");

  // init exprs: should be 2 CONSTs (0, 0)
  CHECK(lp->init_off != SH_NREF_NONE, "init_off is set");
  sh_nref i0 = p->aux[lp->init_off + 0];
  sh_nref i1 = p->aux[lp->init_off + 1];
  CHECK(i0 != SH_NREF_NONE && p->nodes[i0].op == (uint16_t)SH_OP_CONST, "init 0 is CONST");
  CHECK(i1 != SH_NREF_NONE && p->nodes[i1].op == (uint16_t)SH_OP_CONST, "init 1 is CONST");
  CHECK(p->nodes[i0].imm == 0, "init i = 0");
  CHECK(p->nodes[i1].imm == 0, "init acc = 0");

  // Body must exist
  CHECK(lp->body != SH_NREF_NONE, "loop body is set");

  // Body is IF
  sh_nref body = lp->body;
  CHECK(p->nodes[body].op == (uint16_t)SH_OP_IF, "body is IF");

  // There must be a RECUR somewhere in the body
  sh_nref recur = find_op(p, body, SH_OP_RECUR);
  CHECK(recur != SH_NREF_NONE, "RECUR found in body");
  if (recur != SH_NREF_NONE) {
    // RECUR.a = loop index (0)
    CHECK(p->nodes[recur].a == 0, "RECUR.a = loop index 0");
    // RECUR must carry 2 new induction args
    CHECK(p->nodes[recur].aux_len == 2, "RECUR has 2 args");
  }

  // nlocals must include the 2 loop vars
  CHECK(p->nlocals >= 2, "nlocals >= 2");

  sh_free(p);
}

// --- 6. Casts (explicit) -----------------------------------------------------

static void test_casts(void) {
  printf("--- 6. explicit casts ---\n");

  sh_program *p = NULL;
  sh_error err = {0};
  sh_status s = parse_shader(
    "(defshader cnv ((x i64)) -> u32 (u32 x))",
    NULL, &p, &err);

  CHECK(s == SH_OK, "parse cast i64->u32");
  if (s != SH_OK) { printf("   msg: %s\n", err.msg); sh_free(p); return; }

  sh_nref root = p->root;
  CHECK(p->nodes[root].op  == (uint16_t)SH_OP_UNOP, "root is UNOP");
  CHECK(p->nodes[root].sub == (uint16_t)SH_UN_CVT,  "UNOP sub = CVT");
  // Frontend sets type for explicit casts
  CHECK(p->nodes[root].type.kind == (uint8_t)SH_K_U32, "cast target type = u32");

  sh_free(p);
}

// --- 7. Region ops -----------------------------------------------------------

static void test_region_ops(void) {
  printf("--- 7. region ops ---\n");

  sh_program *p = NULL;
  sh_error err = {0};
  // (region-ref buf i)
  sh_status s = parse_shader(
    "(defshader getbyte ((buf (bytes u8)) (i u32)) -> u8 (region-ref buf i))",
    NULL, &p, &err);

  CHECK(s == SH_OK, "parse region-ref");
  if (s != SH_OK) { printf("   msg: %s\n", err.msg); sh_free(p); return; }

  CHECK(sh_param_count(p) == 2, "2 params");
  CHECK(sh_param_type(p, 0).kind == (uint8_t)SH_K_REGION, "param 0 is region");
  CHECK(sh_param_type(p, 0).lane_kind == (uint8_t)SH_K_U8, "region elem = u8");
  CHECK((sh_param_type(p, 0).flags & SH_TYPE_FLAG_MUTABLE) == 0, "region is immutable");
  CHECK(sh_param_type(p, 1).kind == (uint8_t)SH_K_U32, "param 1 is u32");

  sh_nref root = p->root;
  CHECK(p->nodes[root].op == (uint16_t)SH_OP_REGION_LOAD, "root is REGION_LOAD");
  sh_nref buf_ref = p->nodes[root].a;
  sh_nref idx_ref = p->nodes[root].b;
  CHECK(p->nodes[buf_ref].op == (uint16_t)SH_OP_PARAM && p->nodes[buf_ref].a == 0,
        "buf = PARAM 0");
  CHECK(p->nodes[idx_ref].op == (uint16_t)SH_OP_PARAM && p->nodes[idx_ref].a == 1,
        "idx = PARAM 1");

  sh_free(p);

  // (region-set! mutable buf i v) -- use as the body expression so root is REGION_STORE
  s = parse_shader(
    "(defshader setbyte ((buf (bytes-mut u8)) (i u32) (v u8)) -> u8"
    "  (region-set! buf i v))",
    NULL, &p, &err);
  CHECK(s == SH_OK, "parse region-set!");
  if (s == SH_OK) {
    CHECK(sh_param_type(p, 0).kind == (uint8_t)SH_K_REGION, "param 0 region");
    CHECK((sh_param_type(p, 0).flags & SH_TYPE_FLAG_MUTABLE) != 0, "region is mutable");
    // Root should be REGION_STORE directly
    CHECK(p->nodes[p->root].op == (uint16_t)SH_OP_REGION_STORE, "root is REGION_STORE");
    sh_free(p);
  }

  // (region-len buf)
  s = parse_shader(
    "(defshader len ((buf (bytes u32))) -> u32 (region-len buf))",
    NULL, &p, &err);
  CHECK(s == SH_OK, "parse region-len");
  if (s == SH_OK) {
    CHECK(p->nodes[p->root].op == (uint16_t)SH_OP_REGION_LEN, "root is REGION_LEN");
    sh_free(p);
  }
}

// --- 8. Vector forms ---------------------------------------------------------

static void test_vector_forms(void) {
  printf("--- 8. vector forms ---\n");

  sh_program *p = NULL;
  sh_error err = {0};

  // splat: (defshader s ((x f32)) -> vec4 (splat x))
  sh_status s = parse_shader(
    "(defshader sp ((x f32)) -> vec4 (splat x))",
    NULL, &p, &err);
  CHECK(s == SH_OK, "parse splat");
  if (s != SH_OK) { printf("   msg: %s\n", err.msg); }
  else {
    sh_nref root = p->root;
    CHECK(p->nodes[root].op == (uint16_t)SH_OP_VSPLAT, "root is VSPLAT");
    CHECK(sh_return_type(p).kind  == (uint8_t)SH_K_VEC, "return type is VEC");
    CHECK(sh_return_type(p).lanes == 4, "return type has 4 lanes");
    sh_free(p);
  }

  // shuffle: (shuffle v 0 2 1 3)
  s = parse_shader(
    "(defshader shuf ((v f32x4)) -> f32x4 (shuffle v 0 2 1 3))",
    NULL, &p, &err);
  CHECK(s == SH_OK, "parse shuffle");
  if (s != SH_OK) { printf("   msg: %s\n", err.msg); }
  else {
    sh_nref root = p->root;
    CHECK(p->nodes[root].op == (uint16_t)SH_OP_VSHUFFLE, "root is VSHUFFLE");
    CHECK(p->nodes[root].aux_len == 4, "shuffle has 4 indices");
    uint32_t aoff = p->nodes[root].aux_off;
    CHECK(p->aux[aoff + 0] == 0, "idx 0=0");
    CHECK(p->aux[aoff + 1] == 2, "idx 1=2");
    CHECK(p->aux[aoff + 2] == 1, "idx 2=1");
    CHECK(p->aux[aoff + 3] == 3, "idx 3=3");
    sh_free(p);
  }

  // dot: (dot a b)
  s = parse_shader(
    "(defshader dp ((a f32x4) (b f32x4)) -> f32 (dot a b))",
    NULL, &p, &err);
  CHECK(s == SH_OK, "parse dot");
  if (s != SH_OK) { printf("   msg: %s\n", err.msg); }
  else {
    sh_nref root = p->root;
    CHECK(p->nodes[root].op  == (uint16_t)SH_OP_VREDUCE,   "root is VREDUCE");
    CHECK(p->nodes[root].sub == (uint16_t)SH_RED_DOT, "VREDUCE sub = DOT");
    sh_free(p);
  }

  // vreduce-add
  s = parse_shader(
    "(defshader vsum ((v f32x4)) -> f32 (vreduce-add v))",
    NULL, &p, &err);
  CHECK(s == SH_OK, "parse vreduce-add");
  if (s != SH_OK) { printf("   msg: %s\n", err.msg); }
  else {
    CHECK(p->nodes[p->root].sub == (uint16_t)SH_RED_ADD, "VREDUCE sub = ADD");
    sh_free(p);
  }

  // lane extraction
  s = parse_shader(
    "(defshader lx ((v f32x4)) -> f32 (lane v 2))",
    NULL, &p, &err);
  CHECK(s == SH_OK, "parse lane");
  if (s != SH_OK) { printf("   msg: %s\n", err.msg); }
  else {
    sh_nref root = p->root;
    CHECK(p->nodes[root].op  == (uint16_t)SH_OP_VLANE, "root is VLANE");
    CHECK(p->nodes[root].imm == 2, "lane index = 2");
    sh_free(p);
  }
}

// --- 9. cond / when / unless / and / or / begin / let* ----------------------

static void test_control_forms(void) {
  printf("--- 9. control forms ---\n");

  sh_program *p = NULL;
  sh_error err = {0};

  // (cond ((< x 0) 0) (else x))
  sh_status s = parse_shader(
    "(defshader c ((x i64)) -> i64 (cond ((< x 0) 0) (else x)))",
    NULL, &p, &err);
  CHECK(s == SH_OK, "parse cond");
  if (s == SH_OK) {
    // Should produce an IF node
    CHECK(find_op(p, p->root, SH_OP_IF) != SH_NREF_NONE, "cond -> IF node");
    sh_free(p);
  }

  // (when (< x 0) 0) -> IF
  s = parse_shader(
    "(defshader w ((x i64)) -> i64 (when (< x 0) 0))",
    NULL, &p, &err);
  CHECK(s == SH_OK, "parse when");
  if (s == SH_OK) { sh_free(p); }

  // (begin x x)
  s = parse_shader(
    "(defshader b ((x i64)) -> i64 (begin x x))",
    NULL, &p, &err);
  CHECK(s == SH_OK, "parse begin");
  if (s == SH_OK) {
    CHECK(p->nodes[p->root].op == (uint16_t)SH_OP_PARAM, "begin returns last expr");
    sh_free(p);
  }

  // (and (< x 0) (> x -10)) -> nested IFs
  s = parse_shader(
    "(defshader a ((x i64)) -> bool (and (< x 0) (> x -10)))",
    NULL, &p, &err);
  CHECK(s == SH_OK, "parse and");
  if (s == SH_OK) {
    CHECK(find_op(p, p->root, SH_OP_IF) != SH_NREF_NONE, "and -> IF");
    sh_free(p);
  }

  // let*
  s = parse_shader(
    "(defshader lstar ((x i64)) -> i64 (let* ((a x) (b (* a 2))) b))",
    NULL, &p, &err);
  CHECK(s == SH_OK, "parse let*");
  if (s == SH_OK) {
    CHECK(p->nlocals == 2, "let* has 2 locals");
    sh_free(p);
  }
}

// --- 10. Type parsing ---------------------------------------------------------

static void test_type_parsing(void) {
  printf("--- 10. type parsing ---\n");

  sh_program *p = NULL;
  sh_error err = {0};

  // vec sugar
  sh_status s = parse_shader(
    "(defshader vt ((a vec3) (b vec4)) -> vec2 (splat 0.0))",
    NULL, &p, &err);
  CHECK(s == SH_OK, "parse vec2/vec3/vec4 sugar");
  if (s == SH_OK) {
    CHECK(sh_param_type(p, 0).kind  == (uint8_t)SH_K_VEC,   "vec3 kind");
    CHECK(sh_param_type(p, 0).lanes == 3, "vec3 lanes = 3");
    CHECK(sh_param_type(p, 0).lane_kind == (uint8_t)SH_K_F32, "vec3 lane_kind = f32");
    CHECK(sh_param_type(p, 1).lanes == 4, "vec4 lanes = 4");
    CHECK(sh_return_type(p).lanes == 2, "vec2 return lanes = 2");
    sh_free(p);
  }

  // u8x16 vector type
  s = parse_shader(
    "(defshader vt2 ((v u8x16)) -> u8 (lane v 0))",
    NULL, &p, &err);
  CHECK(s == SH_OK, "parse u8x16");
  if (s == SH_OK) {
    CHECK(sh_param_type(p, 0).kind  == (uint8_t)SH_K_VEC, "u8x16 kind");
    CHECK(sh_param_type(p, 0).lanes == 16, "u8x16 lanes = 16");
    CHECK(sh_param_type(p, 0).lane_kind == (uint8_t)SH_K_U8, "u8x16 lane_kind = u8");
    sh_free(p);
  }

  // f64 region
  s = parse_shader(
    "(defshader vt3 ((buf (bytes-mut f64)) (i u32) (v f64)) -> f64"
    "  (region-set! buf i v)"
    "  (region-ref buf i))",
    NULL, &p, &err);
  CHECK(s == SH_OK, "parse bytes-mut f64 region");
  if (s == SH_OK) {
    CHECK(sh_param_type(p, 0).kind  == (uint8_t)SH_K_REGION, "region kind");
    CHECK(sh_param_type(p, 0).lane_kind == (uint8_t)SH_K_F64, "region elem = f64");
    CHECK((sh_param_type(p, 0).flags & SH_TYPE_FLAG_MUTABLE) != 0, "region mutable");
    sh_free(p);
  }
}

// --- 11. Prim-set calls -------------------------------------------------------

static void test_prim_call(void) {
  printf("--- 11. prim-set calls ---\n");

  // Build a tiny prim set
  static sh_prim prims_arr[1] = {{
    .name    = "fold-carry",
    .ret     = {SH_K_U32, 0, 0, 0},
    .nparams = 2,
    .params  = {{SH_K_U32, 0, 0, 0}, {SH_K_U32, 0, 0, 0}},
    .fn      = NULL,
  }};
  static sh_prim_set ps = { prims_arr, 1 };

  sh_program *p = NULL;
  sh_error err = {0};
  sh_status s = parse_shader(
    "(defshader callprim ((a u32) (b u32)) -> u32 (fold-carry a b))",
    &ps, &p, &err);

  CHECK(s == SH_OK, "parse prim call");
  if (s != SH_OK) { printf("   msg: %s\n", err.msg); sh_free(p); return; }

  sh_nref root = p->root;
  CHECK(p->nodes[root].op       == (uint16_t)SH_OP_CALL, "root is CALL");
  CHECK(p->nodes[root].a        == 0, "prim index = 0");
  CHECK(p->nodes[root].aux_len  == 2, "2 call args");

  // Args should be PARAM 0 and PARAM 1
  uint32_t aoff = p->nodes[root].aux_off;
  sh_nref arg0 = p->aux[aoff + 0];
  sh_nref arg1 = p->aux[aoff + 1];
  CHECK(p->nodes[arg0].op == (uint16_t)SH_OP_PARAM && p->nodes[arg0].a == 0, "arg0 = PARAM 0");
  CHECK(p->nodes[arg1].op == (uint16_t)SH_OP_PARAM && p->nodes[arg1].a == 1, "arg1 = PARAM 1");

  sh_free(p);
}

// --- 12. Rejection cases -----------------------------------------------------

static void test_rejections(void) {
  printf("--- 12. rejection cases ---\n");

  // Missing defshader keyword
  expect_fail("(foo id ((x u32)) -> u32 x)", SH_ERR_PARSE,
              "wrong keyword", NULL);

  // Missing ->
  expect_fail("(defshader id ((x u32)) u32 x)", SH_ERR_PARSE,
              "missing ->", NULL);

  // Unknown type
  expect_fail("(defshader id ((x foobar)) -> u32 x)", SH_ERR_PARSE,
              "unknown param type", NULL);

  // lambda is banned
  expect_fail("(defshader bad ((x u32)) -> u32 (lambda (y) y))", SH_ERR_BAD_FORM,
              "lambda banned", NULL);

  // define is banned
  expect_fail("(defshader bad ((x u32)) -> u32 (define y 1))", SH_ERR_BAD_FORM,
              "define banned", NULL);

  // set! is banned
  expect_fail("(defshader bad ((x u32)) -> u32 (set! x 1))", SH_ERR_BAD_FORM,
              "set! banned", NULL);

  // quote is banned
  expect_fail("(defshader bad ((x u32)) -> u32 (quote foo))", SH_ERR_BAD_FORM,
              "quote banned", NULL);

  // cons is banned
  expect_fail("(defshader bad ((x u32)) -> u32 (cons x x))", SH_ERR_BAD_FORM,
              "cons banned", NULL);

  // free identifier (not param, not local)
  expect_fail("(defshader bad ((x u32)) -> u32 y)", SH_ERR_UNKNOWN_NAME,
              "free identifier", NULL);

  // unknown call (no prim set)
  expect_fail("(defshader bad ((x u32)) -> u32 (unknown-fn x))", SH_ERR_NOT_WHITELISTED,
              "unknown call without prim set", NULL);

  // string literal banned
  expect_fail("(defshader bad ((x u32)) -> u32 \"hello\")", SH_ERR_BAD_FORM,
              "string literal banned", NULL);

  // wrong arity for if
  expect_fail("(defshader bad ((x bool)) -> u32 (if x 1))", SH_ERR_PARSE,
              "if requires 3 forms", NULL);

  // wrong arity for binary op
  expect_fail("(defshader bad ((x u32)) -> u32 (+ x x x))", SH_ERR_ARITY,
              "binary + requires exactly 2 args", NULL);

  // non-constant shuffle index
  expect_fail("(defshader bad ((v f32x4) (i u32)) -> f32x4 (shuffle v i 1 2 3))",
              SH_ERR_PARSE, "non-constant shuffle index", NULL);

  // unknown name used as bare symbol (not call)
  expect_fail("(defshader bad ((x u32)) -> u32 foo)", SH_ERR_UNKNOWN_NAME,
              "unknown bare symbol", NULL);

  // spawn is banned
  expect_fail("(defshader bad ((x u32)) -> u32 (spawn x))", SH_ERR_BAD_FORM,
              "spawn banned", NULL);

  // eval is banned
  expect_fail("(defshader bad ((x u32)) -> u32 (eval x))", SH_ERR_BAD_FORM,
              "eval banned", NULL);
}

// --- 13. Multiple let* locals and their slot ordering -------------------------

static void test_let_star_slots(void) {
  printf("--- 13. let* slot ordering ---\n");

  sh_program *p = NULL;
  sh_error err = {0};
  sh_status s = parse_shader(
    "(defshader chain ((x i64)) -> i64"
    "  (let* ((a (+ x 1))"
    "         (b (+ a 1))"
    "         (c (+ b 1)))"
    "    c))",
    NULL, &p, &err);

  CHECK(s == SH_OK, "parse let* chain");
  if (s != SH_OK) { printf("   msg: %s\n", err.msg); sh_free(p); return; }

  CHECK(p->nlocals == 3, "3 locals");

  // Root should be a LET node (let* may be compiled as nested lets or flat)
  // The body (c) should be a LOCAL node
  sh_nref lroot = p->root;
  // Walk to the leaf body: should end at LOCAL
  // Find a LOCAL node somewhere in the tree
  sh_nref local_ref = find_op(p, lroot, SH_OP_LOCAL);
  CHECK(local_ref != SH_NREF_NONE, "LOCAL node found in let* body");

  sh_free(p);
}

// --- 14. pipeline: frontend + verifier real; compile succeeds end-to-end -----

static void test_pipeline_compiles(void) {
  printf("--- 14. pipeline compiles end-to-end ---\n");

  sh_program *prog = NULL;
  sh_error err = {0};
  sh_status s = sh_compile_string(
    "(defshader id ((x u32)) -> u32 x)",
    NULL, 0, &prog, &err);

  // sh_compile runs parse + verify (not invoke), so the identity shader compiles.
  CHECK(s == SH_OK, "identity shader compiles (SH_OK)");
  CHECK(prog != NULL, "out_prog non-NULL on success");
  if (prog) {
    CHECK(strcmp(sh_name(prog), "id") == 0, "shader name is 'id'");
    CHECK(sh_param_count(prog) == 1, "one parameter");
    CHECK(sh_type_eq(sh_param_type(prog, 0), sh_type_scalar(SH_K_U32)), "param type u32");
    CHECK(sh_type_eq(sh_return_type(prog), sh_type_scalar(SH_K_U32)), "return type u32");
    sh_free(prog);
  }
}

// --- 15. float literal -------------------------------------------------------

static void test_float_literal(void) {
  printf("--- 15. float literal ---\n");

  sh_program *p = NULL;
  sh_error err = {0};
  // The Lisp reader produces a flonum for 1.5
  sh_status s = parse_shader(
    "(defshader f ((x f64)) -> f64 1.5)",
    NULL, &p, &err);
  CHECK(s == SH_OK, "parse float literal");
  if (s == SH_OK) {
    sh_nref root = p->root;
    CHECK(p->nodes[root].op  == (uint16_t)SH_OP_CONST, "root is CONST");
    CHECK(p->nodes[root].sub == 1, "CONST sub=1 (float)");
    // Verify the bit pattern matches 1.5
    uint64_t bits = (uint64_t)p->nodes[root].imm;
    double dv;
    memcpy(&dv, &bits, sizeof(dv));
    CHECK(dv == 1.5, "float value = 1.5");
    sh_free(p);
  }
}

// --- 16. nested loop (named-let inside a let) --------------------------------

static void test_nested_let_loop(void) {
  printf("--- 16. nested let inside named-let ---\n");

  sh_program *p = NULL;
  sh_error err = {0};
  // A loop that uses a let inside the body
  const char *src =
    "(defshader nested ((n i64)) -> i64"
    "  (let loop ((i 0) (acc 0))"
    "    (if (>= i n)"
    "        acc"
    "        (let ((step (+ i 1)))"
    "          (loop step (+ acc step))))))";

  sh_status s = parse_shader(src, NULL, &p, &err);
  CHECK(s == SH_OK, "parse nested let-inside-loop");
  if (s != SH_OK) { printf("   msg: %s\n", err.msg); sh_free(p); return; }

  CHECK(p->nloops == 1, "1 loop");
  CHECK(p->nlocals >= 3, "nlocals >= 3 (2 loop vars + 1 let var)");

  // LOOP + RECUR should both be present
  CHECK(p->nodes[p->root].op == (uint16_t)SH_OP_LOOP, "root is LOOP");
  sh_nref body = p->loops[0].body;
  sh_nref recur = find_op(p, body, SH_OP_RECUR);
  CHECK(recur != SH_NREF_NONE, "RECUR found in nested body");

  sh_free(p);
}

// =============================================================================
// main
// =============================================================================

int main(void) {
  // The reader interns symbols; bring up the runtime.
  (void)lisp_default_env();

  printf("[lisp_shader frontend tests]\n\n");

  test_simple_identity();
  test_arithmetic();
  test_if_bool();
  test_let();
  test_named_let_loop();
  test_casts();
  test_region_ops();
  test_vector_forms();
  test_control_forms();
  test_type_parsing();
  test_prim_call();
  test_rejections();
  test_let_star_slots();
  test_pipeline_compiles();
  test_float_literal();
  test_nested_let_loop();

  printf("\n[lisp_shader frontend] %d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
