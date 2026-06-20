// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

// UNIT A -- the frontend: a (defshader ...) reader-datum into a structured,
// untyped AST (the verifier fills the types). Also owns the arena builders, the
// public sh_compile pipeline glue, sh_free, and the introspection getters (the
// derived, drift-proof contract).
//
// See notes/scratch/shader-proposal-minimalist.md sections 0, 6, 9.
//
// Body-form spellings chosen:
//   arithmetic:   + - * / mod
//   bitwise:      bit-and bit-or bit-xor shl shr
//   unary:        - (negate) not
//   comparisons:  = < <= > >=
//   casts:        (u8 x) (u16 x) (u32 x) (u64 x) (i64 x) (f32 x) (f64 x) (bool x)
//   region ops:   (region-ref buf i) (region-set! buf i v) (region-len buf)
//   vector ops:   (splat x) (shuffle v i0 i1 ...) (dot a b)
//                 (vreduce-add v) (vreduce-min v) (vreduce-max v)
//                 (lane v i)
//   control:      if cond when unless and or begin let let*
//   named-let:    (let NAME ((var init) ...) body)  -> LOOP+RECUR

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>

#include "sh_internal.h"

// Error-and-return helper for functions returning sh_nref:
// sets the error, then returns SH_NREF_NONE (NOT the status code).
// sh_set_error returns the status; we discard it and return SH_NREF_NONE.
#define SHF_FAIL(err, status, line, col, ...) \
  (sh_set_error((err), (status), (line), (col), __VA_ARGS__), SH_NREF_NONE)

// --- arena builders ---------------------------------------------------------

static bool grow(void **buf, uint32_t *cap, uint32_t need, size_t elem) {
  if (need <= *cap) return true;
  uint32_t ncap = *cap ? *cap * 2 : 8;
  while (ncap < need) ncap *= 2;
  void *nb = realloc(*buf, (size_t)ncap * elem);
  if (!nb) return false;
  *buf = nb;
  *cap = ncap;
  return true;
}

sh_nref sh_node_alloc(sh_program *p, sh_op op) {
  if (!grow((void **)&p->nodes, &p->cap_nodes, p->nnodes + 1, sizeof(sh_node)))
    return SH_NREF_NONE;
  sh_nref id = p->nnodes++;
  sh_node *n = &p->nodes[id];
  memset(n, 0, sizeof(*n));
  n->op = (uint16_t)op;
  n->a = n->b = n->c = SH_NREF_NONE;
  return id;
}

bool sh_aux_reserve(sh_program *p, uint32_t n, uint32_t *off) {
  if (!grow((void **)&p->aux, &p->cap_aux, p->naux + n, sizeof(uint32_t)))
    return false;
  *off = p->naux;
  for (uint32_t i = 0; i < n; i++) p->aux[p->naux++] = SH_NREF_NONE;
  return true;
}

sh_nref sh_loop_alloc(sh_program *p, uint32_t *out_index) {
  if (!grow((void **)&p->loops, &p->cap_loops, p->nloops + 1, sizeof(sh_loop)))
    return SH_NREF_NONE;
  uint32_t idx = p->nloops++;
  memset(&p->loops[idx], 0, sizeof(sh_loop));
  *out_index = idx;
  return idx;
}

// --- parse context -----------------------------------------------------------

// Stack entry tracking a named-let (loop) binding scope
#define MAX_LOOP_DEPTH 16

typedef struct {
  const char *name;       // loop label name
  uint32_t loop_idx;      // index into p->loops
  uint32_t var_slot0;     // first induction-var slot
  uint32_t nvars;         // number of induction vars
} loop_frame;

// A local binding (let or loop induction var)
typedef struct {
  const char *name;
  uint32_t slot;
} local_binding;

#define MAX_LOCALS 256

typedef struct {
  sh_program *p;
  sh_error *err;
  const sh_prim_set *prims;

  // parameter names (parallel to p->params / p->nparams)
  const char *param_names[SH_MAX_PARAMS];

  // flat local slot table (let + loop induction vars, assigned in order)
  local_binding locals[MAX_LOCALS];
  uint32_t nlocals_alloc;  // count used so far

  // loop stack
  loop_frame loop_stack[MAX_LOOP_DEPTH];
  int loop_depth;
} parse_ctx;

// --- helper: symbol name comparison -----------------------------------------

static bool sym_eq(lisp_value v, const char *name) {
  return lisp_is_symbol(v) && strcmp(lisp_named_name(v), name) == 0;
}

// --- helper: count list length -----------------------------------------------

static int list_len(lisp_value v) {
  int n = 0;
  while (lisp_is_pair(v)) { n++; v = lisp_cdr(v); }
  return n;
}

// --- helper: nth element of a list -------------------------------------------

static lisp_value list_ref(lisp_value v, int i) {
  while (i-- > 0 && lisp_is_pair(v)) v = lisp_cdr(v);
  return lisp_is_pair(v) ? lisp_car(v) : LISP_EMPTY;
}

// --- type parsing ------------------------------------------------------------
// Returns true on success, fills *out. sym must be a symbol value.

static bool parse_type_sym(lisp_value sym, sh_type *out) {
  if (!lisp_is_symbol(sym)) return false;
  const char *name = lisp_named_name(sym);
  if      (strcmp(name, "u8")   == 0) { *out = sh_type_scalar(SH_K_U8);  return true; }
  else if (strcmp(name, "u16")  == 0) { *out = sh_type_scalar(SH_K_U16); return true; }
  else if (strcmp(name, "u32")  == 0) { *out = sh_type_scalar(SH_K_U32); return true; }
  else if (strcmp(name, "u64")  == 0) { *out = sh_type_scalar(SH_K_U64); return true; }
  else if (strcmp(name, "i64")  == 0) { *out = sh_type_scalar(SH_K_I64); return true; }
  else if (strcmp(name, "f32")  == 0) { *out = sh_type_scalar(SH_K_F32); return true; }
  else if (strcmp(name, "f64")  == 0) { *out = sh_type_scalar(SH_K_F64); return true; }
  else if (strcmp(name, "bool") == 0) { *out = sh_type_scalar(SH_K_BOOL); return true; }
  else if (strcmp(name, "vec2") == 0) { *out = sh_type_vec(SH_K_F32, 2); return true; }
  else if (strcmp(name, "vec3") == 0) { *out = sh_type_vec(SH_K_F32, 3); return true; }
  else if (strcmp(name, "vec4") == 0) { *out = sh_type_vec(SH_K_F32, 4); return true; }
  else {
    // <kind>x<lanes>: e.g. f32x4, u8x16, u32x8
    // parse: scan for 'x' separator
    size_t len = strlen(name);
    for (size_t i = 1; i < len; i++) {
      if (name[i] == 'x' && i > 0 && i + 1 < len) {
        // try parsing lanes after 'x'
        char *endp;
        long lanes = strtol(name + i + 1, &endp, 10);
        if (endp != name + i + 1 && *endp == '\0' &&
            lanes >= 2 && lanes <= SH_MAX_LANES) {
          // parse the kind prefix
          char kbuf[16];
          if (i >= sizeof(kbuf)) break;
          memcpy(kbuf, name, i);
          kbuf[i] = '\0';
          sh_kind kk;
          if      (strcmp(kbuf, "u8")  == 0) kk = SH_K_U8;
          else if (strcmp(kbuf, "u16") == 0) kk = SH_K_U16;
          else if (strcmp(kbuf, "u32") == 0) kk = SH_K_U32;
          else if (strcmp(kbuf, "u64") == 0) kk = SH_K_U64;
          else if (strcmp(kbuf, "i64") == 0) kk = SH_K_I64;
          else if (strcmp(kbuf, "f32") == 0) kk = SH_K_F32;
          else if (strcmp(kbuf, "f64") == 0) kk = SH_K_F64;
          else break;
          *out = sh_type_vec(kk, (uint8_t)lanes);
          return true;
        }
        break;
      }
    }
  }
  return false;
}

// Parse a type form: either a symbol (scalar/vec sugar) or a list (bytes T) / (bytes-mut T)
static bool parse_type(lisp_value v, sh_type *out, sh_error *err) {
  if (lisp_is_symbol(v)) {
    if (parse_type_sym(v, out)) return true;
    sh_set_error(err, SH_ERR_PARSE, -1, -1, "unknown type '%s'", lisp_named_name(v));
    return false;
  }
  if (lisp_is_pair(v)) {
    lisp_value head = lisp_car(v);
    lisp_value rest = lisp_cdr(v);
    if (lisp_is_symbol(head)) {
      const char *hname = lisp_named_name(head);
      bool is_mut = false;
      if (strcmp(hname, "bytes-mut") == 0) is_mut = true;
      else if (strcmp(hname, "bytes") != 0) {
        sh_set_error(err, SH_ERR_PARSE, -1, -1, "unknown type form '%s'", hname);
        return false;
      }
      // (bytes T) or (bytes-mut T)
      if (!lisp_is_pair(rest) || !lisp_is_empty(lisp_cdr(rest))) {
        sh_set_error(err, SH_ERR_PARSE, -1, -1, "bytes/bytes-mut requires exactly one element kind");
        return false;
      }
      lisp_value elem_v = lisp_car(rest);
      sh_type elem;
      if (!parse_type_sym(elem_v, &elem)) {
        sh_set_error(err, SH_ERR_PARSE, -1, -1, "invalid element type in bytes");
        return false;
      }
      if (elem.kind != (uint8_t)SH_K_U8  && elem.kind != (uint8_t)SH_K_U16 &&
          elem.kind != (uint8_t)SH_K_U32 && elem.kind != (uint8_t)SH_K_U64 &&
          elem.kind != (uint8_t)SH_K_I64 &&
          elem.kind != (uint8_t)SH_K_F32 && elem.kind != (uint8_t)SH_K_F64 &&
          elem.kind != (uint8_t)SH_K_BOOL) {
        sh_set_error(err, SH_ERR_PARSE, -1, -1, "region element must be a scalar kind");
        return false;
      }
      *out = sh_type_region((sh_kind)elem.kind, is_mut);
      return true;
    }
  }
  sh_set_error(err, SH_ERR_PARSE, -1, -1, "malformed type expression");
  return false;
}

// --- name lookup helpers -----------------------------------------------------

// Returns slot index (0..nlocals-1) or -1 if not found (searches innermost first)
static int lookup_local(parse_ctx *ctx, const char *name) {
  // Search backwards (innermost scope first)
  for (int i = (int)ctx->nlocals_alloc - 1; i >= 0; i--) {
    if (strcmp(ctx->locals[i].name, name) == 0)
      return (int)ctx->locals[i].slot;
  }
  return -1;
}

static int lookup_param(parse_ctx *ctx, const char *name) {
  for (uint32_t i = 0; i < ctx->p->nparams; i++) {
    if (strcmp(ctx->param_names[i], name) == 0)
      return (int)i;
  }
  return -1;
}

// Find loop by label name; returns -1 if not found
static int lookup_loop(parse_ctx *ctx, const char *name) {
  for (int i = ctx->loop_depth - 1; i >= 0; i--) {
    if (strcmp(ctx->loop_stack[i].name, name) == 0)
      return i;
  }
  return -1;
}

// Allocate a new local slot and register it
static int alloc_local(parse_ctx *ctx, const char *name) {
  if (ctx->nlocals_alloc >= MAX_LOCALS) return -1;
  uint32_t slot = ctx->p->nlocals++;
  ctx->locals[ctx->nlocals_alloc].name = name;
  ctx->locals[ctx->nlocals_alloc].slot = slot;
  ctx->nlocals_alloc++;
  return (int)slot;
}

// --- banned form names -------------------------------------------------------

static bool is_banned_name(const char *name) {
  // banned special forms and Lisp builtins that are forbidden in shader bodies
  static const char *banned[] = {
    "lambda", "define", "set!", "cons", "car", "cdr", "list",
    "make-bytes", "import", "define-module",
    "spawn", "send", "recv", "yield",
    "eval", "quote", "quasiquote",
    NULL
  };
  for (int i = 0; banned[i]; i++) {
    if (strcmp(name, banned[i]) == 0) return true;
  }
  return false;
}

// Check if a symbol name is a cast target type
static bool is_cast_type(const char *name, sh_kind *out_kind) {
  if (strcmp(name, "u8")   == 0) { *out_kind = SH_K_U8;   return true; }
  if (strcmp(name, "u16")  == 0) { *out_kind = SH_K_U16;  return true; }
  if (strcmp(name, "u32")  == 0) { *out_kind = SH_K_U32;  return true; }
  if (strcmp(name, "u64")  == 0) { *out_kind = SH_K_U64;  return true; }
  if (strcmp(name, "i64")  == 0) { *out_kind = SH_K_I64;  return true; }
  if (strcmp(name, "f32")  == 0) { *out_kind = SH_K_F32;  return true; }
  if (strcmp(name, "f64")  == 0) { *out_kind = SH_K_F64;  return true; }
  if (strcmp(name, "bool") == 0) { *out_kind = SH_K_BOOL; return true; }
  return false;
}

// Forward declaration
static sh_nref parse_expr(parse_ctx *ctx, lisp_value v);

// --- expression parsing: application forms -----------------------------------

static sh_nref parse_call_to_prim(parse_ctx *ctx, uint32_t prim_idx, lisp_value args_list) {
  sh_program *p = ctx->p;
  // count args
  uint32_t nargs = (uint32_t)list_len(args_list);
  sh_nref ref = sh_node_alloc(p, SH_OP_CALL);
  if (ref == SH_NREF_NONE)
    return SHF_FAIL(ctx->err, SH_ERR_OOM, -1, -1, "OOM allocating CALL node");

  // allocate aux for args
  uint32_t aux_off = 0;
  if (nargs > 0 && !sh_aux_reserve(p, nargs, &aux_off)) {
    sh_set_error(ctx->err, SH_ERR_OOM, -1, -1, "OOM in CALL aux");
    return SH_NREF_NONE;
  }

  p->nodes[ref].a = prim_idx;
  p->nodes[ref].aux_off = aux_off;
  p->nodes[ref].aux_len = nargs;

  lisp_value cur = args_list;
  for (uint32_t i = 0; i < nargs; i++) {
    sh_nref arg = parse_expr(ctx, lisp_car(cur));
    if (arg == SH_NREF_NONE) return SH_NREF_NONE;
    p->aux[aux_off + i] = arg;
    cur = lisp_cdr(cur);
  }
  return ref;
}

// Parse a (let ...) or (let* ...) or (let NAME ...) form
// form = the full (let ...) list value with 'let' already consumed (rest is the cdr)
// is_star = true for let*
static sh_nref parse_let(parse_ctx *ctx, lisp_value rest, bool is_star) {
  sh_program *p = ctx->p;
  sh_error *err = ctx->err;

  if (!lisp_is_pair(rest))
    return SHF_FAIL(err, SH_ERR_PARSE, -1, -1, "malformed let: missing bindings");

  // Check if this is a named-let: (let NAME ((var init)...) body)
  lisp_value bindings_v = lisp_car(rest);
  bool is_named = lisp_is_symbol(bindings_v);
  const char *loop_name = NULL;

  if (is_named) {
    if (is_star) {
      return SHF_FAIL(err, SH_ERR_BAD_FORM, -1, -1, "let* cannot be named");
    }
    loop_name = lisp_named_name(bindings_v);
    rest = lisp_cdr(rest);
    if (!lisp_is_pair(rest))
      return SHF_FAIL(err, SH_ERR_PARSE, -1, -1, "malformed named-let: missing bindings");
    bindings_v = lisp_car(rest);
  }

  lisp_value body_list = lisp_cdr(rest);
  if (!lisp_is_pair(body_list))
    return SHF_FAIL(err, SH_ERR_PARSE, -1, -1, "malformed let: missing body");

  // Count bindings
  uint32_t nbinds = (uint32_t)list_len(bindings_v);

  if (is_named) {
    // --- NAMED-LET: lower to SH_OP_LOOP + SH_OP_RECUR ---
    if (ctx->loop_depth >= MAX_LOOP_DEPTH)
      return SHF_FAIL(err, SH_ERR_PARSE, -1, -1, "loop nesting too deep");

    // Allocate loop record
    uint32_t loop_idx = 0;
    sh_loop_alloc(p, &loop_idx);

    // Allocate aux for init exprs
    uint32_t init_aux_off = 0;
    if (nbinds > 0 && !sh_aux_reserve(p, nbinds, &init_aux_off)) {
      return SHF_FAIL(err, SH_ERR_OOM, -1, -1, "OOM in loop init aux");
    }

    // Assign local slots for induction vars & parse inits (in outer scope)
    uint32_t var_slot0 = p->nlocals;
    uint32_t saved_nlocals_alloc = ctx->nlocals_alloc;

    lisp_value bv = bindings_v;
    for (uint32_t i = 0; i < nbinds; i++) {
      lisp_value binding = lisp_car(bv);
      if (!lisp_is_pair(binding) || !lisp_is_symbol(lisp_car(binding))) {
        return SHF_FAIL(err, SH_ERR_PARSE, -1, -1,
                                     "named-let binding must be (var init)");
      }
      const char *vname = lisp_named_name(lisp_car(binding));
      lisp_value init_v = lisp_car(lisp_cdr(binding));

      // Parse init in the outer scope (before registering the var)
      sh_nref init_ref = parse_expr(ctx, init_v);
      if (init_ref == SH_NREF_NONE) return SH_NREF_NONE;
      p->aux[init_aux_off + i] = init_ref;

      // Register the local in inner scope for use in body
      int slot = alloc_local(ctx, vname);
      if (slot < 0)
        return SHF_FAIL(err, SH_ERR_PARSE, -1, -1, "too many locals");
      bv = lisp_cdr(bv);
    }

    // Fill in loop record
    p->loops[loop_idx].nvars    = nbinds;
    p->loops[loop_idx].var_slot0 = var_slot0;
    p->loops[loop_idx].init_off  = init_aux_off;

    // Push loop frame
    loop_frame *fr = &ctx->loop_stack[ctx->loop_depth++];
    fr->name      = loop_name;
    fr->loop_idx  = loop_idx;
    fr->var_slot0 = var_slot0;
    fr->nvars     = nbinds;

    // Parse body (last form is the tail)
    // Support multi-form body via implicit begin
    sh_nref body_ref = SH_NREF_NONE;
    lisp_value bl = body_list;
    while (lisp_is_pair(bl)) {
      body_ref = parse_expr(ctx, lisp_car(bl));
      if (body_ref == SH_NREF_NONE) { ctx->loop_depth--; return SH_NREF_NONE; }
      bl = lisp_cdr(bl);
    }
    p->loops[loop_idx].body = body_ref;

    // Pop loop frame
    ctx->loop_depth--;
    // Pop local bindings for induction vars
    ctx->nlocals_alloc = saved_nlocals_alloc;

    // Emit LOOP node
    sh_nref loop_node = sh_node_alloc(p, SH_OP_LOOP);
    if (loop_node == SH_NREF_NONE)
      return SHF_FAIL(err, SH_ERR_OOM, -1, -1, "OOM loop node");
    p->nodes[loop_node].a = loop_idx;
    return loop_node;

  } else {
    // --- Regular LET / LET* ---
    // Allocate aux for binding init exprs
    uint32_t aux_off = 0;
    if (nbinds > 0 && !sh_aux_reserve(p, nbinds, &aux_off)) {
      return SHF_FAIL(err, SH_ERR_OOM, -1, -1, "OOM in let aux");
    }

    uint32_t first_slot = p->nlocals;
    uint32_t saved_nlocals_alloc = ctx->nlocals_alloc;

    lisp_value bv = bindings_v;
    for (uint32_t i = 0; i < nbinds; i++) {
      lisp_value binding = lisp_car(bv);
      if (!lisp_is_pair(binding) || !lisp_is_symbol(lisp_car(binding))) {
        return SHF_FAIL(err, SH_ERR_PARSE, -1, -1,
                                     "let binding must be (var expr)");
      }
      const char *vname = lisp_named_name(lisp_car(binding));
      lisp_value init_v = lisp_car(lisp_cdr(binding));

      // For let*: each binding is visible to subsequent ones
      // For let: all bindings see the outer scope only
      sh_nref init_ref = parse_expr(ctx, init_v);
      if (init_ref == SH_NREF_NONE) {
        ctx->nlocals_alloc = saved_nlocals_alloc;
        return SH_NREF_NONE;
      }
      p->aux[aux_off + i] = init_ref;

      int slot = alloc_local(ctx, vname);
      if (slot < 0) {
        ctx->nlocals_alloc = saved_nlocals_alloc;
        return SHF_FAIL(err, SH_ERR_PARSE, -1, -1, "too many locals");
      }
      if (!is_star && i == 0) first_slot = (uint32_t)slot;
      bv = lisp_cdr(bv);
    }

    // Parse body (last form)
    sh_nref body_ref = SH_NREF_NONE;
    lisp_value bl = body_list;
    while (lisp_is_pair(bl)) {
      body_ref = parse_expr(ctx, lisp_car(bl));
      if (body_ref == SH_NREF_NONE) {
        ctx->nlocals_alloc = saved_nlocals_alloc;
        return SH_NREF_NONE;
      }
      bl = lisp_cdr(bl);
    }

    // Emit LET node
    sh_nref let_node = sh_node_alloc(p, SH_OP_LET);
    if (let_node == SH_NREF_NONE) {
      ctx->nlocals_alloc = saved_nlocals_alloc;
      return SHF_FAIL(err, SH_ERR_OOM, -1, -1, "OOM let node");
    }
    p->nodes[let_node].a = first_slot;
    p->nodes[let_node].b = body_ref;
    p->nodes[let_node].aux_off = aux_off;
    p->nodes[let_node].aux_len = nbinds;

    // Pop local bindings
    ctx->nlocals_alloc = saved_nlocals_alloc;
    return let_node;
  }
}

// Parse (cond clause ...) -> nested ifs
static sh_nref parse_cond(parse_ctx *ctx, lisp_value clauses) {
  sh_error *err = ctx->err;
  sh_program *p = ctx->p;

  if (!lisp_is_pair(clauses) && lisp_is_empty(clauses)) {
    // (cond) with no clauses -> emit a const 0 / void as fallthrough
    sh_nref n = sh_node_alloc(p, SH_OP_CONST);
    if (n == SH_NREF_NONE) {
      sh_set_error(err, SH_ERR_OOM, -1, -1, "OOM");
      return SH_NREF_NONE;
    }
    p->nodes[n].imm = 0;
    return n;
  }

  lisp_value clause = lisp_car(clauses);
  lisp_value rest   = lisp_cdr(clauses);

  if (!lisp_is_pair(clause))
    return SHF_FAIL(err, SH_ERR_PARSE, -1, -1, "cond: malformed clause");

  lisp_value test = lisp_car(clause);
  lisp_value body = lisp_cdr(clause);

  // (else ...) -> last clause
  bool is_else = sym_eq(test, "else");

  if (is_else) {
    // else clause: emit body as begin
    sh_nref result = SH_NREF_NONE;
    while (lisp_is_pair(body)) {
      result = parse_expr(ctx, lisp_car(body));
      if (result == SH_NREF_NONE) return SH_NREF_NONE;
      body = lisp_cdr(body);
    }
    return result;
  }

  sh_nref cond_ref = parse_expr(ctx, test);
  if (cond_ref == SH_NREF_NONE) return SH_NREF_NONE;

  // then: body forms (implicit begin)
  sh_nref then_ref = SH_NREF_NONE;
  if (lisp_is_pair(body)) {
    lisp_value bl = body;
    while (lisp_is_pair(bl)) {
      then_ref = parse_expr(ctx, lisp_car(bl));
      if (then_ref == SH_NREF_NONE) return SH_NREF_NONE;
      bl = lisp_cdr(bl);
    }
  } else {
    then_ref = cond_ref;  // (cond (expr)) -> expr itself
  }

  // else: recursively parse remaining clauses
  sh_nref else_ref;
  if (lisp_is_empty(rest) || !lisp_is_pair(rest)) {
    // No else clause -- emit a 0 const (type will be checked by verifier)
    else_ref = sh_node_alloc(p, SH_OP_CONST);
    if (else_ref == SH_NREF_NONE) {
      sh_set_error(err, SH_ERR_OOM, -1, -1, "OOM");
      return SH_NREF_NONE;
    }
    p->nodes[else_ref].imm = 0;
  } else {
    else_ref = parse_cond(ctx, rest);
    if (else_ref == SH_NREF_NONE) return SH_NREF_NONE;
  }

  sh_nref if_node = sh_node_alloc(p, SH_OP_IF);
  if (if_node == SH_NREF_NONE)
    return SHF_FAIL(err, SH_ERR_OOM, -1, -1, "OOM if node");
  p->nodes[if_node].a = cond_ref;
  p->nodes[if_node].b = then_ref;
  p->nodes[if_node].c = else_ref;
  return if_node;
}

// Parse (and a b ...) -> nested ifs (short-circuit)
static sh_nref parse_and(parse_ctx *ctx, lisp_value args) {
  sh_program *p = ctx->p;
  if (lisp_is_empty(args)) {
    // (and) -> #t
    sh_nref n = sh_node_alloc(p, SH_OP_CONST);
    if (n == SH_NREF_NONE) { sh_set_error(ctx->err, SH_ERR_OOM, -1, -1, "OOM"); return SH_NREF_NONE; }
    p->nodes[n].sub = 2;  // bool
    p->nodes[n].imm = 1;  // #t
    return n;
  }
  if (!lisp_is_pair(args)) {
    sh_set_error(ctx->err, SH_ERR_PARSE, -1, -1, "malformed and");
    return SH_NREF_NONE;
  }
  lisp_value head = lisp_car(args);
  lisp_value tail = lisp_cdr(args);
  sh_nref first = parse_expr(ctx, head);
  if (first == SH_NREF_NONE) return SH_NREF_NONE;
  if (lisp_is_empty(tail)) return first;  // single arg

  sh_nref rest = parse_and(ctx, tail);
  if (rest == SH_NREF_NONE) return SH_NREF_NONE;

  // Build: (if first rest #f)
  sh_nref false_node = sh_node_alloc(p, SH_OP_CONST);
  if (false_node == SH_NREF_NONE) { sh_set_error(ctx->err, SH_ERR_OOM, -1, -1, "OOM"); return SH_NREF_NONE; }
  p->nodes[false_node].sub = 2;  // bool
  p->nodes[false_node].imm = 0;  // #f

  sh_nref if_node = sh_node_alloc(p, SH_OP_IF);
  if (if_node == SH_NREF_NONE) { sh_set_error(ctx->err, SH_ERR_OOM, -1, -1, "OOM"); return SH_NREF_NONE; }
  p->nodes[if_node].a = first;
  p->nodes[if_node].b = rest;
  p->nodes[if_node].c = false_node;
  return if_node;
}

// Parse (or a b ...) -> nested ifs (short-circuit)
static sh_nref parse_or(parse_ctx *ctx, lisp_value args) {
  sh_program *p = ctx->p;
  if (lisp_is_empty(args)) {
    // (or) -> #f
    sh_nref n = sh_node_alloc(p, SH_OP_CONST);
    if (n == SH_NREF_NONE) { sh_set_error(ctx->err, SH_ERR_OOM, -1, -1, "OOM"); return SH_NREF_NONE; }
    p->nodes[n].sub = 2;  // bool
    p->nodes[n].imm = 0;  // #f
    return n;
  }
  if (!lisp_is_pair(args)) {
    sh_set_error(ctx->err, SH_ERR_PARSE, -1, -1, "malformed or");
    return SH_NREF_NONE;
  }
  lisp_value head = lisp_car(args);
  lisp_value tail = lisp_cdr(args);
  sh_nref first = parse_expr(ctx, head);
  if (first == SH_NREF_NONE) return SH_NREF_NONE;
  if (lisp_is_empty(tail)) return first;

  sh_nref rest = parse_or(ctx, tail);
  if (rest == SH_NREF_NONE) return SH_NREF_NONE;

  // Build: (if first #t rest)  -- note: we actually want first itself as the
  // result when truthy; but since shader types are not "any", and the verifier
  // will type-check both arms, we just use: (if first first rest).
  // For boolean ops this is fine (the result is the first truthy value or the last).
  // Actually for typed shaders, just: (if first first rest)
  sh_nref if_node = sh_node_alloc(p, SH_OP_IF);
  if (if_node == SH_NREF_NONE) { sh_set_error(ctx->err, SH_ERR_OOM, -1, -1, "OOM"); return SH_NREF_NONE; }
  p->nodes[if_node].a = first;
  p->nodes[if_node].b = first;
  p->nodes[if_node].c = rest;
  return if_node;
}

// Parse (begin e1 e2 ...) -> last expr
static sh_nref parse_begin(parse_ctx *ctx, lisp_value body) {
  if (!lisp_is_pair(body))
    return SHF_FAIL(ctx->err, SH_ERR_PARSE, -1, -1, "empty begin");
  sh_nref result = SH_NREF_NONE;
  while (lisp_is_pair(body)) {
    result = parse_expr(ctx, lisp_car(body));
    if (result == SH_NREF_NONE) return SH_NREF_NONE;
    body = lisp_cdr(body);
  }
  return result;
}

// Parse (when test body...) -> (if test (begin body...) 0)
static sh_nref parse_when(parse_ctx *ctx, lisp_value rest, bool unless) {
  sh_program *p = ctx->p;
  sh_error *err = ctx->err;
  if (!lisp_is_pair(rest) || !lisp_is_pair(lisp_cdr(rest)))
    return SHF_FAIL(err, SH_ERR_PARSE, -1, -1, "malformed when/unless");
  lisp_value test = lisp_car(rest);
  lisp_value body = lisp_cdr(rest);

  sh_nref cond_ref = parse_expr(ctx, test);
  if (cond_ref == SH_NREF_NONE) return SH_NREF_NONE;

  sh_nref body_ref = parse_begin(ctx, body);
  if (body_ref == SH_NREF_NONE) return SH_NREF_NONE;

  sh_nref zero = sh_node_alloc(p, SH_OP_CONST);
  if (zero == SH_NREF_NONE) return SHF_FAIL(err, SH_ERR_OOM, -1, -1, "OOM");
  p->nodes[zero].imm = 0;

  sh_nref if_node = sh_node_alloc(p, SH_OP_IF);
  if (if_node == SH_NREF_NONE) return SHF_FAIL(err, SH_ERR_OOM, -1, -1, "OOM");
  p->nodes[if_node].a = cond_ref;
  if (unless) {
    p->nodes[if_node].b = zero;
    p->nodes[if_node].c = body_ref;
  } else {
    p->nodes[if_node].b = body_ref;
    p->nodes[if_node].c = zero;
  }
  return if_node;
}

// Parse a RECUR form: (NAME new-args...)
// Called when we see a symbol that matches a loop label in tail position.
static sh_nref parse_recur(parse_ctx *ctx, int loop_frame_idx, lisp_value args_list) {
  sh_program *p = ctx->p;
  sh_error *err = ctx->err;
  loop_frame *fr = &ctx->loop_stack[loop_frame_idx];
  uint32_t nargs = (uint32_t)list_len(args_list);

  if (nargs != fr->nvars)
    return SHF_FAIL(err, SH_ERR_ARITY, -1, -1,
                                  "recur: expected %u induction args, got %u",
                                  fr->nvars, nargs);

  uint32_t aux_off = 0;
  if (nargs > 0 && !sh_aux_reserve(p, nargs, &aux_off)) {
    return SHF_FAIL(err, SH_ERR_OOM, -1, -1, "OOM in RECUR aux");
  }

  lisp_value cur = args_list;
  for (uint32_t i = 0; i < nargs; i++) {
    sh_nref arg = parse_expr(ctx, lisp_car(cur));
    if (arg == SH_NREF_NONE) return SH_NREF_NONE;
    p->aux[aux_off + i] = arg;
    cur = lisp_cdr(cur);
  }

  sh_nref recur_node = sh_node_alloc(p, SH_OP_RECUR);
  if (recur_node == SH_NREF_NONE)
    return SHF_FAIL(err, SH_ERR_OOM, -1, -1, "OOM RECUR node");
  p->nodes[recur_node].a = fr->loop_idx;  // enclosing loop index
  p->nodes[recur_node].aux_off = aux_off;
  p->nodes[recur_node].aux_len = nargs;
  return recur_node;
}

// --- main expression parser --------------------------------------------------

static sh_nref parse_expr(parse_ctx *ctx, lisp_value v) {
  sh_program *p = ctx->p;
  sh_error *err = ctx->err;

  // ---- literals ----
  if (lisp_is_fixnum(v)) {
    sh_nref n = sh_node_alloc(p, SH_OP_CONST);
    if (n == SH_NREF_NONE) { sh_set_error(err, SH_ERR_OOM, -1, -1, "OOM"); return SH_NREF_NONE; }
    p->nodes[n].sub = 0;  // integer
    p->nodes[n].imm = lisp_fixnum_val(v);
    return n;
  }
  if (lisp_is_flonum(v)) {
    sh_nref n = sh_node_alloc(p, SH_OP_CONST);
    if (n == SH_NREF_NONE) { sh_set_error(err, SH_ERR_OOM, -1, -1, "OOM"); return SH_NREF_NONE; }
    p->nodes[n].sub = 1;  // float
    double dv = lisp_flonum_val(v);
    uint64_t bits;
    memcpy(&bits, &dv, sizeof(bits));
    p->nodes[n].imm = (int64_t)bits;
    return n;
  }
  if (v == LISP_TRUE || v == LISP_FALSE) {
    sh_nref n = sh_node_alloc(p, SH_OP_CONST);
    if (n == SH_NREF_NONE) { sh_set_error(err, SH_ERR_OOM, -1, -1, "OOM"); return SH_NREF_NONE; }
    p->nodes[n].sub = 2;  // bool
    p->nodes[n].imm = (v == LISP_TRUE) ? 1 : 0;
    return n;
  }

  // ---- string/char/vector: banned ----
  if (lisp_is_string(v) || lisp_is_char(v) || lisp_is_vector(v) || lisp_is_bytes(v)) {
    sh_set_error(err, SH_ERR_BAD_FORM, -1, -1,
                 "strings, chars, byte literals, and vectors are not allowed in shader bodies");
    return SH_NREF_NONE;
  }

  // ---- symbol: param ref, local ref, or error ----
  if (lisp_is_symbol(v)) {
    const char *name = lisp_named_name(v);
    if (is_banned_name(name)) {
      sh_set_error(err, SH_ERR_BAD_FORM, -1, -1, "banned form: %s", name);
      return SH_NREF_NONE;
    }
    // check local
    int slot = lookup_local(ctx, name);
    if (slot >= 0) {
      sh_nref n = sh_node_alloc(p, SH_OP_LOCAL);
      if (n == SH_NREF_NONE) { sh_set_error(err, SH_ERR_OOM, -1, -1, "OOM"); return SH_NREF_NONE; }
      p->nodes[n].a = (uint32_t)slot;
      return n;
    }
    // check param
    int pidx = lookup_param(ctx, name);
    if (pidx >= 0) {
      sh_nref n = sh_node_alloc(p, SH_OP_PARAM);
      if (n == SH_NREF_NONE) { sh_set_error(err, SH_ERR_OOM, -1, -1, "OOM"); return SH_NREF_NONE; }
      p->nodes[n].a = (uint32_t)pidx;
      return n;
    }
    // free identifier
    sh_set_error(err, SH_ERR_UNKNOWN_NAME, -1, -1, "unknown name: %s", name);
    return SH_NREF_NONE;
  }

  // ---- application / special forms ----
  if (!lisp_is_pair(v)) {
    sh_set_error(err, SH_ERR_PARSE, -1, -1, "unexpected expression form");
    return SH_NREF_NONE;
  }

  lisp_value head = lisp_car(v);
  lisp_value rest = lisp_cdr(v);

  // Must start with a symbol
  if (!lisp_is_symbol(head)) {
    sh_set_error(err, SH_ERR_BAD_FORM, -1, -1,
                 "application head must be a symbol in shader bodies");
    return SH_NREF_NONE;
  }

  const char *hname = lisp_named_name(head);

  // --- BANNED FORM CHECK ---
  if (is_banned_name(hname)) {
    sh_set_error(err, SH_ERR_BAD_FORM, -1, -1, "banned form: %s", hname);
    return SH_NREF_NONE;
  }

  // --- SPECIAL FORMS ---

  // (if cond then else)
  if (strcmp(hname, "if") == 0) {
    if (list_len(rest) != 3)
      return SHF_FAIL(err, SH_ERR_PARSE, -1, -1, "if requires 3 subforms");
    sh_nref cond_ref = parse_expr(ctx, list_ref(rest, 0));
    if (cond_ref == SH_NREF_NONE) return SH_NREF_NONE;
    sh_nref then_ref = parse_expr(ctx, list_ref(rest, 1));
    if (then_ref == SH_NREF_NONE) return SH_NREF_NONE;
    sh_nref else_ref = parse_expr(ctx, list_ref(rest, 2));
    if (else_ref == SH_NREF_NONE) return SH_NREF_NONE;
    sh_nref if_node = sh_node_alloc(p, SH_OP_IF);
    if (if_node == SH_NREF_NONE) return SHF_FAIL(err, SH_ERR_OOM, -1, -1, "OOM");
    p->nodes[if_node].a = cond_ref;
    p->nodes[if_node].b = then_ref;
    p->nodes[if_node].c = else_ref;
    return if_node;
  }

  // (cond clause...)
  if (strcmp(hname, "cond") == 0) {
    return parse_cond(ctx, rest);
  }

  // (when test body...)
  if (strcmp(hname, "when") == 0) {
    return parse_when(ctx, rest, false);
  }

  // (unless test body...)
  if (strcmp(hname, "unless") == 0) {
    return parse_when(ctx, rest, true);
  }

  // (and ...)
  if (strcmp(hname, "and") == 0) {
    return parse_and(ctx, rest);
  }

  // (or ...)
  if (strcmp(hname, "or") == 0) {
    return parse_or(ctx, rest);
  }

  // (begin e1 e2 ...)
  if (strcmp(hname, "begin") == 0) {
    return parse_begin(ctx, rest);
  }

  // (let ...)
  if (strcmp(hname, "let") == 0) {
    return parse_let(ctx, rest, false);
  }

  // (let* ...)
  if (strcmp(hname, "let*") == 0) {
    return parse_let(ctx, rest, true);
  }

  // --- ARITHMETIC BINARY OPS ---
  // (+ a b) (- a b) (* a b) (/ a b) (mod a b)
  {
    sh_binop binop;
    bool is_binop = true;
    bool is_unary_minus = false;
    if      (strcmp(hname, "+")       == 0) binop = SH_BIN_ADD;
    else if (strcmp(hname, "-")       == 0) { binop = SH_BIN_SUB; is_unary_minus = true; }
    else if (strcmp(hname, "*")       == 0) binop = SH_BIN_MUL;
    else if (strcmp(hname, "/")       == 0) binop = SH_BIN_DIV;
    else if (strcmp(hname, "mod")     == 0) binop = SH_BIN_MOD;
    else if (strcmp(hname, "bit-and") == 0) binop = SH_BIN_AND;
    else if (strcmp(hname, "bit-or")  == 0) binop = SH_BIN_OR;
    else if (strcmp(hname, "bit-xor") == 0) binop = SH_BIN_XOR;
    else if (strcmp(hname, "shl")     == 0) binop = SH_BIN_SHL;
    else if (strcmp(hname, "shr")     == 0) binop = SH_BIN_SHR;
    else if (strcmp(hname, "sat+")    == 0) binop = SH_BIN_SADD;
    else if (strcmp(hname, "sat-")    == 0) binop = SH_BIN_SSUB;
    else is_binop = false;

    if (is_binop) {
      int nargs = list_len(rest);
      // unary minus: (- x) -> negate
      if (is_unary_minus && nargs == 1) {
        sh_nref operand = parse_expr(ctx, lisp_car(rest));
        if (operand == SH_NREF_NONE) return SH_NREF_NONE;
        sh_nref unop = sh_node_alloc(p, SH_OP_UNOP);
        if (unop == SH_NREF_NONE) return SHF_FAIL(err, SH_ERR_OOM, -1, -1, "OOM");
        p->nodes[unop].sub = (uint16_t)SH_UN_NEG;
        p->nodes[unop].a   = operand;
        return unop;
      }
      if (nargs != 2)
        return SHF_FAIL(err, SH_ERR_ARITY, -1, -1,
                                      "%s requires 2 operands", hname);
      sh_nref lhs = parse_expr(ctx, list_ref(rest, 0));
      if (lhs == SH_NREF_NONE) return SH_NREF_NONE;
      sh_nref rhs = parse_expr(ctx, list_ref(rest, 1));
      if (rhs == SH_NREF_NONE) return SH_NREF_NONE;
      sh_nref n = sh_node_alloc(p, SH_OP_BINOP);
      if (n == SH_NREF_NONE) return SHF_FAIL(err, SH_ERR_OOM, -1, -1, "OOM");
      p->nodes[n].sub = (uint16_t)binop;
      p->nodes[n].a   = lhs;
      p->nodes[n].b   = rhs;
      return n;
    }
  }

  // --- COMPARISONS ---
  {
    sh_cmp cmpop;
    bool is_cmp = true;
    if      (strcmp(hname, "=")  == 0) cmpop = SH_CMP_EQ;
    else if (strcmp(hname, "<")  == 0) cmpop = SH_CMP_LT;
    else if (strcmp(hname, "<=") == 0) cmpop = SH_CMP_LE;
    else if (strcmp(hname, ">")  == 0) cmpop = SH_CMP_GT;
    else if (strcmp(hname, ">=") == 0) cmpop = SH_CMP_GE;
    else is_cmp = false;

    if (is_cmp) {
      if (list_len(rest) != 2)
        return SHF_FAIL(err, SH_ERR_ARITY, -1, -1,
                                      "%s requires 2 operands", hname);
      sh_nref lhs = parse_expr(ctx, list_ref(rest, 0));
      if (lhs == SH_NREF_NONE) return SH_NREF_NONE;
      sh_nref rhs = parse_expr(ctx, list_ref(rest, 1));
      if (rhs == SH_NREF_NONE) return SH_NREF_NONE;
      sh_nref n = sh_node_alloc(p, SH_OP_CMP);
      if (n == SH_NREF_NONE) return SHF_FAIL(err, SH_ERR_OOM, -1, -1, "OOM");
      p->nodes[n].sub = (uint16_t)cmpop;
      p->nodes[n].a   = lhs;
      p->nodes[n].b   = rhs;
      return n;
    }
  }

  // --- UNARY: not ---
  if (strcmp(hname, "not") == 0) {
    if (list_len(rest) != 1)
      return SHF_FAIL(err, SH_ERR_ARITY, -1, -1, "not requires 1 operand");
    sh_nref operand = parse_expr(ctx, lisp_car(rest));
    if (operand == SH_NREF_NONE) return SH_NREF_NONE;
    sh_nref n = sh_node_alloc(p, SH_OP_UNOP);
    if (n == SH_NREF_NONE) return SHF_FAIL(err, SH_ERR_OOM, -1, -1, "OOM");
    p->nodes[n].sub = (uint16_t)SH_UN_NOT;
    p->nodes[n].a   = operand;
    return n;
  }

  // --- REGION OPS ---
  // (region-ref buf i)
  if (strcmp(hname, "region-ref") == 0) {
    if (list_len(rest) != 2)
      return SHF_FAIL(err, SH_ERR_ARITY, -1, -1, "region-ref requires 2 args");
    sh_nref buf = parse_expr(ctx, list_ref(rest, 0));
    if (buf == SH_NREF_NONE) return SH_NREF_NONE;
    sh_nref idx = parse_expr(ctx, list_ref(rest, 1));
    if (idx == SH_NREF_NONE) return SH_NREF_NONE;
    sh_nref n = sh_node_alloc(p, SH_OP_REGION_LOAD);
    if (n == SH_NREF_NONE) return SHF_FAIL(err, SH_ERR_OOM, -1, -1, "OOM");
    p->nodes[n].a = buf;
    p->nodes[n].b = idx;
    return n;
  }
  // (region-set! buf i val)
  if (strcmp(hname, "region-set!") == 0) {
    if (list_len(rest) != 3)
      return SHF_FAIL(err, SH_ERR_ARITY, -1, -1, "region-set! requires 3 args");
    sh_nref buf = parse_expr(ctx, list_ref(rest, 0));
    if (buf == SH_NREF_NONE) return SH_NREF_NONE;
    sh_nref idx = parse_expr(ctx, list_ref(rest, 1));
    if (idx == SH_NREF_NONE) return SH_NREF_NONE;
    sh_nref val = parse_expr(ctx, list_ref(rest, 2));
    if (val == SH_NREF_NONE) return SH_NREF_NONE;
    sh_nref n = sh_node_alloc(p, SH_OP_REGION_STORE);
    if (n == SH_NREF_NONE) return SHF_FAIL(err, SH_ERR_OOM, -1, -1, "OOM");
    p->nodes[n].a = buf;
    p->nodes[n].b = idx;
    p->nodes[n].c = val;
    return n;
  }
  // (region-len buf)
  if (strcmp(hname, "region-len") == 0) {
    if (list_len(rest) != 1)
      return SHF_FAIL(err, SH_ERR_ARITY, -1, -1, "region-len requires 1 arg");
    sh_nref buf = parse_expr(ctx, lisp_car(rest));
    if (buf == SH_NREF_NONE) return SH_NREF_NONE;
    sh_nref n = sh_node_alloc(p, SH_OP_REGION_LEN);
    if (n == SH_NREF_NONE) return SHF_FAIL(err, SH_ERR_OOM, -1, -1, "OOM");
    p->nodes[n].a = buf;
    return n;
  }

  // --- VECTOR OPS ---
  // (splat x)  -> VSPLAT
  if (strcmp(hname, "splat") == 0) {
    if (list_len(rest) != 1)
      return SHF_FAIL(err, SH_ERR_ARITY, -1, -1, "splat requires 1 arg");
    sh_nref operand = parse_expr(ctx, lisp_car(rest));
    if (operand == SH_NREF_NONE) return SH_NREF_NONE;
    sh_nref n = sh_node_alloc(p, SH_OP_VSPLAT);
    if (n == SH_NREF_NONE) return SHF_FAIL(err, SH_ERR_OOM, -1, -1, "OOM");
    p->nodes[n].a = operand;
    return n;
  }
  // (shuffle v i0 i1 ...) -> VSHUFFLE; indices are constant integers stored in aux as uint32
  if (strcmp(hname, "shuffle") == 0) {
    if (list_len(rest) < 2)
      return SHF_FAIL(err, SH_ERR_ARITY, -1, -1,
                                    "shuffle requires source vector + at least 1 index");
    lisp_value src_v = lisp_car(rest);
    lisp_value idx_list = lisp_cdr(rest);
    sh_nref src = parse_expr(ctx, src_v);
    if (src == SH_NREF_NONE) return SH_NREF_NONE;

    int nidx = list_len(idx_list);
    uint32_t aux_off = 0;
    if (!sh_aux_reserve(p, (uint32_t)nidx, &aux_off))
      return SHF_FAIL(err, SH_ERR_OOM, -1, -1, "OOM in shuffle aux");
    lisp_value cur = idx_list;
    for (int i = 0; i < nidx; i++) {
      lisp_value idx_v = lisp_car(cur);
      if (!lisp_is_fixnum(idx_v))
        return SHF_FAIL(err, SH_ERR_PARSE, -1, -1,
                                      "shuffle indices must be constant integers");
      int64_t ival = lisp_fixnum_val(idx_v);
      // Lane indices are 0-based; a max-width vector has lanes [0, SH_MAX_LANES).
      // (The verifier re-checks each index against the actual source lane count.)
      if (ival < 0 || ival >= SH_MAX_LANES)
        return SHF_FAIL(err, SH_ERR_PARSE, -1, -1,
                                      "shuffle index out of range");
      p->aux[aux_off + i] = (uint32_t)ival;
      cur = lisp_cdr(cur);
    }

    sh_nref n = sh_node_alloc(p, SH_OP_VSHUFFLE);
    if (n == SH_NREF_NONE) return SHF_FAIL(err, SH_ERR_OOM, -1, -1, "OOM");
    p->nodes[n].a       = src;
    p->nodes[n].aux_off = aux_off;
    p->nodes[n].aux_len = (uint32_t)nidx;
    return n;
  }
  // (dot a b) -> VREDUCE/SH_RED_DOT
  if (strcmp(hname, "dot") == 0) {
    if (list_len(rest) != 2)
      return SHF_FAIL(err, SH_ERR_ARITY, -1, -1, "dot requires 2 args");
    sh_nref lhs = parse_expr(ctx, list_ref(rest, 0));
    if (lhs == SH_NREF_NONE) return SH_NREF_NONE;
    sh_nref rhs = parse_expr(ctx, list_ref(rest, 1));
    if (rhs == SH_NREF_NONE) return SH_NREF_NONE;
    sh_nref n = sh_node_alloc(p, SH_OP_VREDUCE);
    if (n == SH_NREF_NONE) return SHF_FAIL(err, SH_ERR_OOM, -1, -1, "OOM");
    p->nodes[n].sub = (uint16_t)SH_RED_DOT;
    p->nodes[n].a   = lhs;
    p->nodes[n].b   = rhs;
    return n;
  }
  // (vreduce-add v) (vreduce-min v) (vreduce-max v)
  if (strcmp(hname, "vreduce-add") == 0 || strcmp(hname, "vreduce-min") == 0 ||
      strcmp(hname, "vreduce-max") == 0) {
    if (list_len(rest) != 1)
      return SHF_FAIL(err, SH_ERR_ARITY, -1, -1,
                                    "%s requires 1 arg", hname);
    sh_reduce rop;
    if      (strcmp(hname, "vreduce-add") == 0) rop = SH_RED_ADD;
    else if (strcmp(hname, "vreduce-min") == 0) rop = SH_RED_MIN;
    else                                         rop = SH_RED_MAX;
    sh_nref operand = parse_expr(ctx, lisp_car(rest));
    if (operand == SH_NREF_NONE) return SH_NREF_NONE;
    sh_nref n = sh_node_alloc(p, SH_OP_VREDUCE);
    if (n == SH_NREF_NONE) return SHF_FAIL(err, SH_ERR_OOM, -1, -1, "OOM");
    p->nodes[n].sub = (uint16_t)rop;
    p->nodes[n].a   = operand;
    return n;
  }
  // (lane v i) -> VLANE; i must be a constant integer
  if (strcmp(hname, "lane") == 0) {
    if (list_len(rest) != 2)
      return SHF_FAIL(err, SH_ERR_ARITY, -1, -1, "lane requires 2 args");
    sh_nref vec = parse_expr(ctx, list_ref(rest, 0));
    if (vec == SH_NREF_NONE) return SH_NREF_NONE;
    lisp_value idx_v = list_ref(rest, 1);
    if (!lisp_is_fixnum(idx_v))
      return SHF_FAIL(err, SH_ERR_PARSE, -1, -1,
                                    "lane index must be a constant integer");
    int64_t lane_idx = lisp_fixnum_val(idx_v);
    if (lane_idx < 0 || lane_idx >= SH_MAX_LANES)
      return SHF_FAIL(err, SH_ERR_PARSE, -1, -1, "lane index out of range");
    sh_nref n = sh_node_alloc(p, SH_OP_VLANE);
    if (n == SH_NREF_NONE) return SHF_FAIL(err, SH_ERR_OOM, -1, -1, "OOM");
    p->nodes[n].a   = vec;
    p->nodes[n].imm = lane_idx;
    return n;
  }

  // --- CASTS: (TYPE x) where TYPE is a scalar kind name ---
  {
    sh_kind cast_kind;
    if (is_cast_type(hname, &cast_kind)) {
      if (list_len(rest) != 1)
        return SHF_FAIL(err, SH_ERR_ARITY, -1, -1,
                                      "cast (%s x) requires exactly 1 argument", hname);
      sh_nref operand = parse_expr(ctx, lisp_car(rest));
      if (operand == SH_NREF_NONE) return SH_NREF_NONE;
      sh_nref n = sh_node_alloc(p, SH_OP_UNOP);
      if (n == SH_NREF_NONE) return SHF_FAIL(err, SH_ERR_OOM, -1, -1, "OOM");
      p->nodes[n].sub  = (uint16_t)SH_UN_CVT;
      p->nodes[n].a    = operand;
      // Frontend sets type for explicit casts (the one exception to "types zeroed")
      p->nodes[n].type = sh_type_scalar(cast_kind);
      return n;
    }
  }

  // --- LOOP RECUR: (LOOPLABEL new-args...) ---
  {
    int li = lookup_loop(ctx, hname);
    if (li >= 0) {
      return parse_recur(ctx, li, rest);
    }
  }

  // --- PRIM CALL: check whitelist ---
  if (ctx->prims) {
    for (uint32_t i = 0; i < ctx->prims->count; i++) {
      if (strcmp(ctx->prims->prims[i].name, hname) == 0) {
        return parse_call_to_prim(ctx, i, rest);
      }
    }
  }

  // If the name looks like it might be a call but is not in the whitelist
  // and is not any known form: report NOT_WHITELISTED for call-like names,
  // UNKNOWN_NAME for bare identifier uses.
  sh_set_error(err, SH_ERR_NOT_WHITELISTED, -1, -1,
               "unknown or non-whitelisted call: %s", hname);
  return SH_NREF_NONE;
}

// --- main frontend entry point -----------------------------------------------

sh_status shf_parse(lisp_value form, sh_program *p, sh_error *err) {
  // Expect: (defshader NAME ((param TYPE) ...) -> RET-TYPE BODY)
  if (!lisp_is_pair(form))
    return sh_set_error(err, SH_ERR_PARSE, -1, -1,
                        "shader: expected a list form");

  lisp_value kw = lisp_car(form);
  if (!sym_eq(kw, "defshader"))
    return sh_set_error(err, SH_ERR_PARSE, -1, -1,
                        "shader: expected (defshader ...)");

  lisp_value rest = lisp_cdr(form);
  if (!lisp_is_pair(rest))
    return sh_set_error(err, SH_ERR_PARSE, -1, -1,
                        "defshader: missing name");

  // NAME
  lisp_value name_v = lisp_car(rest);
  if (!lisp_is_symbol(name_v))
    return sh_set_error(err, SH_ERR_PARSE, -1, -1,
                        "defshader: name must be a symbol");
  const char *sname = lisp_named_name(name_v);
  size_t nlen = strlen(sname);
  if (nlen >= sizeof(p->name))
    return sh_set_error(err, SH_ERR_PARSE, -1, -1, "defshader: name too long");
  memcpy(p->name, sname, nlen + 1);

  rest = lisp_cdr(rest);
  if (!lisp_is_pair(rest))
    return sh_set_error(err, SH_ERR_PARSE, -1, -1,
                        "defshader: missing parameter list");

  // PARAM LIST: ((param TYPE) ...)
  lisp_value params_v = lisp_car(rest);
  rest = lisp_cdr(rest);

  // Build parse context
  parse_ctx ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.p    = p;
  ctx.err  = err;
  ctx.prims = p->prims;

  // Parse parameters
  uint32_t nparams = 0;
  lisp_value pv = params_v;
  while (lisp_is_pair(pv)) {
    lisp_value binding = lisp_car(pv);
    if (nparams >= SH_MAX_PARAMS)
      return sh_set_error(err, SH_ERR_PARSE, -1, -1,
                          "defshader: too many parameters (max %d)", SH_MAX_PARAMS);
    if (!lisp_is_pair(binding))
      return sh_set_error(err, SH_ERR_PARSE, -1, -1,
                          "defshader: parameter must be (name type)");
    lisp_value pname_v = lisp_car(binding);
    lisp_value ptype_v = lisp_car(lisp_cdr(binding));
    if (!lisp_is_symbol(pname_v))
      return sh_set_error(err, SH_ERR_PARSE, -1, -1,
                          "defshader: parameter name must be a symbol");
    sh_type pt;
    if (!parse_type(ptype_v, &pt, err)) return err->status;
    ctx.param_names[nparams] = lisp_named_name(pname_v);
    p->params[nparams] = pt;
    nparams++;
    pv = lisp_cdr(pv);
  }
  p->nparams = nparams;

  // Expect: -> RET-TYPE
  if (!lisp_is_pair(rest))
    return sh_set_error(err, SH_ERR_PARSE, -1, -1,
                        "defshader: missing -> return type");
  lisp_value arrow = lisp_car(rest);
  if (!sym_eq(arrow, "->"))
    return sh_set_error(err, SH_ERR_PARSE, -1, -1,
                        "defshader: expected -> after params, got '%s'",
                        lisp_is_symbol(arrow) ? lisp_named_name(arrow) : "?");
  rest = lisp_cdr(rest);

  if (!lisp_is_pair(rest))
    return sh_set_error(err, SH_ERR_PARSE, -1, -1,
                        "defshader: missing return type");
  lisp_value ret_type_v = lisp_car(rest);
  rest = lisp_cdr(rest);

  sh_type ret;
  if (!parse_type(ret_type_v, &ret, err)) return err->status;
  p->ret = ret;

  // BODY: must have at least one form
  if (!lisp_is_pair(rest))
    return sh_set_error(err, SH_ERR_PARSE, -1, -1, "defshader: missing body");

  // Parse body forms; last result is the shader's value
  sh_nref body_ref = SH_NREF_NONE;
  while (lisp_is_pair(rest)) {
    body_ref = parse_expr(&ctx, lisp_car(rest));
    if (body_ref == SH_NREF_NONE) return err->status;
    rest = lisp_cdr(rest);
  }

  p->root = body_ref;
  // nlocals was updated by alloc_local; ensure it's consistent
  // (nlocals is updated in-place via p->nlocals++ in alloc_local)

  return SH_OK;
}

// --- public compile pipeline ------------------------------------------------

sh_status sh_compile(lisp_value form, const sh_prim_set *prims, uint32_t flags,
                     sh_program **out_prog, sh_error *err) {
  if (!out_prog) return sh_set_error(err, SH_ERR_INTERNAL, -1, -1, "null out_prog");
  *out_prog = NULL;

  sh_program *p = calloc(1, sizeof(*p));
  if (!p) return sh_set_error(err, SH_ERR_OOM, -1, -1, "out of memory");
  p->root = SH_NREF_NONE;
  p->prims = prims;

  sh_status s = shf_parse(form, p, err);
  if (s != SH_OK) { sh_free(p); return s; }

  s = shv_verify(p, prims, flags, err);
  if (s != SH_OK) { sh_free(p); return s; }

  p->verified = true;
  *out_prog = p;
  return SH_OK;
}

sh_status sh_compile_string(const char *src, const sh_prim_set *prims, uint32_t flags,
                            sh_program **out_prog, sh_error *err) {
  if (out_prog) *out_prog = NULL;
  const char *cur = src;
  const char *end = src + strlen(src);
  const char *rerr = NULL;
  lisp_value form = lisp_read(&cur, end, &rerr);
  if (rerr) {
    int line = -1, col = -1;
    lisp_source_location(src, cur, &line, &col);
    return sh_set_error(err, SH_ERR_PARSE, line, col, "reader: %s", rerr);
  }
  return sh_compile(form, prims, flags, out_prog, err);
}

void sh_free(sh_program *p) {
  if (!p) return;
  free(p->nodes);
  free(p->aux);
  free(p->loops);
  free(p);
}

// --- introspection: the derived contract ------------------------------------

const char *sh_name(const sh_program *p) { return p ? p->name : ""; }
uint32_t sh_param_count(const sh_program *p) { return p ? p->nparams : 0; }
sh_type sh_param_type(const sh_program *p, uint32_t i) {
  if (p && i < p->nparams) return p->params[i];
  return sh_type_scalar(SH_K_VOID);
}
sh_type sh_return_type(const sh_program *p) { return p ? p->ret : sh_type_scalar(SH_K_VOID); }
uint64_t sh_static_cost(const sh_program *p) { return p ? p->cost.const_cost : 0; }
bool sh_cost_is_const(const sh_program *p) { return p ? p->cost.is_const : false; }
uint64_t sh_cost_for_args(const sh_program *p, const sh_value *args, uint32_t argc) {
  return shi_cost_for_args(p, args, argc);
}
