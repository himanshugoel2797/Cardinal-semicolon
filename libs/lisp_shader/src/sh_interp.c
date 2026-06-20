// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

// UNIT C -- the reference interpreter (the semantic oracle). A tree-walker over
// the VERIFIED typed AST. Every vector op is defined by a lane loop here (the
// mandatory scalar lowering -- SIMD is a perf property of a future backend,
// never correctness). Every region access is bounds-checked at runtime.
//
// The public sh_invoke (arg validation + dispatch) lives here; shi_invoke is the
// internal entry. See notes/scratch/shader-proposal-minimalist.md sections 1 and 8.

#include <stdlib.h>
#include <string.h>

#include "sh_internal.h"

// ---------------------------------------------------------------------------
// Lane bit-pattern helpers
// ---------------------------------------------------------------------------

// Extract a scalar sh_value from a lane bit pattern + kind.
static sh_value lane_to_scalar(sh_kind k, uint64_t bits) {
  sh_value v;
  memset(&v, 0, sizeof(v));
  v.kind = k;
  v.lanes = 1;
  switch (k) {
    case SH_K_BOOL:
    case SH_K_U8:
    case SH_K_U16:
    case SH_K_U32:
    case SH_K_U64:
      v.u = bits;
      break;
    case SH_K_I64:
      v.i = (int64_t)bits;
      break;
    case SH_K_F32: {
      uint32_t b32 = (uint32_t)bits;
      float fv;
      memcpy(&fv, &b32, sizeof(fv));
      v.f = (double)(float)fv;
      break;
    }
    case SH_K_F64:
      memcpy(&v.f, &bits, sizeof(v.f));
      break;
    default:
      break;
  }
  return v;
}

// Pack a scalar sh_value into a lane bit pattern.
static uint64_t scalar_to_lane(sh_kind k, sh_value v) {
  switch (k) {
    case SH_K_F32: {
      float fv = (float)v.f;
      uint32_t b32;
      memcpy(&b32, &fv, sizeof(b32));
      return (uint64_t)b32;
    }
    case SH_K_F64: {
      uint64_t bits;
      memcpy(&bits, &v.f, sizeof(bits));
      return bits;
    }
    case SH_K_I64:
      return (uint64_t)v.i;
    default:
      return v.u;
  }
}

// ---------------------------------------------------------------------------
// Scalar arithmetic on bit patterns
// ---------------------------------------------------------------------------

// Perform a scalar binop on two lane bit patterns of the given kind.
// Returns the result lane bit pattern.
static uint64_t scalar_binop_bits(sh_binop op, sh_kind k, uint64_t a, uint64_t b) {
  if (k == SH_K_F32 || k == SH_K_F64) {
    double fa, fb, fr;
    if (k == SH_K_F32) {
      uint32_t a32 = (uint32_t)a, b32 = (uint32_t)b;
      float fav, fbv;
      memcpy(&fav, &a32, 4);
      memcpy(&fbv, &b32, 4);
      fa = (double)fav;
      fb = (double)fbv;
    } else {
      memcpy(&fa, &a, 8);
      memcpy(&fb, &b, 8);
    }
    switch (op) {
      case SH_BIN_ADD: fr = fa + fb; break;
      case SH_BIN_SUB: fr = fa - fb; break;
      case SH_BIN_MUL: fr = fa * fb; break;
      case SH_BIN_DIV: fr = (fb != 0.0) ? fa / fb : 0.0; break;
      default: fr = 0.0; break;
    }
    if (k == SH_K_F32) {
      float fres = (float)fr;
      uint32_t r32;
      memcpy(&r32, &fres, 4);
      return (uint64_t)r32;
    } else {
      uint64_t rbits;
      memcpy(&rbits, &fr, 8);
      return rbits;
    }
  }
  switch (k) {
    case SH_K_U8: {
      uint8_t ra = (uint8_t)a, rb = (uint8_t)b, rr = 0;
      switch (op) {
        case SH_BIN_ADD:  rr = (uint8_t)(ra + rb); break;
        case SH_BIN_SUB:  rr = (uint8_t)(ra - rb); break;
        case SH_BIN_MUL:  rr = (uint8_t)(ra * rb); break;
        case SH_BIN_DIV:  rr = rb ? (uint8_t)(ra / rb) : 0; break;
        case SH_BIN_MOD:  rr = rb ? (uint8_t)(ra % rb) : 0; break;
        case SH_BIN_AND:  rr = ra & rb; break;
        case SH_BIN_OR:   rr = ra | rb; break;
        case SH_BIN_XOR:  rr = ra ^ rb; break;
        case SH_BIN_SHL:  rr = (uint8_t)(ra << (rb & 7u)); break;
        case SH_BIN_SHR:  rr = (uint8_t)(ra >> (rb & 7u)); break;
        // sat+: min(a+b, UINT8_MAX); overflow detection: sum > UINT8_MAX iff rb > UINT8_MAX-ra
        case SH_BIN_SADD: rr = (rb > (uint8_t)(0xFFu - ra)) ? 0xFFu : (uint8_t)(ra + rb); break;
        // sat-: max(a-b, 0); underflow iff rb > ra
        case SH_BIN_SSUB: rr = (rb > ra) ? 0u : (uint8_t)(ra - rb); break;
        default: rr = 0; break;
      }
      return (uint64_t)rr;
    }
    case SH_K_U16: {
      uint16_t ra = (uint16_t)a, rb = (uint16_t)b, rr = 0;
      switch (op) {
        case SH_BIN_ADD:  rr = (uint16_t)(ra + rb); break;
        case SH_BIN_SUB:  rr = (uint16_t)(ra - rb); break;
        case SH_BIN_MUL:  rr = (uint16_t)(ra * rb); break;
        case SH_BIN_DIV:  rr = rb ? (uint16_t)(ra / rb) : 0; break;
        case SH_BIN_MOD:  rr = rb ? (uint16_t)(ra % rb) : 0; break;
        case SH_BIN_AND:  rr = ra & rb; break;
        case SH_BIN_OR:   rr = ra | rb; break;
        case SH_BIN_XOR:  rr = ra ^ rb; break;
        case SH_BIN_SHL:  rr = (uint16_t)(ra << (rb & 15u)); break;
        case SH_BIN_SHR:  rr = (uint16_t)(ra >> (rb & 15u)); break;
        case SH_BIN_SADD: rr = (rb > (uint16_t)(0xFFFFu - ra)) ? 0xFFFFu : (uint16_t)(ra + rb); break;
        case SH_BIN_SSUB: rr = (rb > ra) ? 0u : (uint16_t)(ra - rb); break;
        default: rr = 0; break;
      }
      return (uint64_t)rr;
    }
    case SH_K_BOOL:
    case SH_K_U32: {
      uint32_t ra = (uint32_t)a, rb = (uint32_t)b, rr = 0;
      switch (op) {
        case SH_BIN_ADD:  rr = ra + rb; break;
        case SH_BIN_SUB:  rr = ra - rb; break;
        case SH_BIN_MUL:  rr = ra * rb; break;
        case SH_BIN_DIV:  rr = rb ? ra / rb : 0; break;
        case SH_BIN_MOD:  rr = rb ? ra % rb : 0; break;
        case SH_BIN_AND:  rr = ra & rb; break;
        case SH_BIN_OR:   rr = ra | rb; break;
        case SH_BIN_XOR:  rr = ra ^ rb; break;
        case SH_BIN_SHL:  rr = ra << (rb & 31u); break;
        case SH_BIN_SHR:  rr = ra >> (rb & 31u); break;
        case SH_BIN_SADD: rr = (rb > (0xFFFFFFFFu - ra)) ? 0xFFFFFFFFu : (ra + rb); break;
        case SH_BIN_SSUB: rr = (rb > ra) ? 0u : (ra - rb); break;
        default: rr = 0; break;
      }
      return (uint64_t)rr;
    }
    case SH_K_U64: {
      uint64_t ra = a, rb = b;
      switch (op) {
        case SH_BIN_ADD:  return ra + rb;
        case SH_BIN_SUB:  return ra - rb;
        case SH_BIN_MUL:  return ra * rb;
        case SH_BIN_DIV:  return rb ? ra / rb : 0;
        case SH_BIN_MOD:  return rb ? ra % rb : 0;
        case SH_BIN_AND:  return ra & rb;
        case SH_BIN_OR:   return ra | rb;
        case SH_BIN_XOR:  return ra ^ rb;
        case SH_BIN_SHL:  return ra << (rb & 63u);
        case SH_BIN_SHR:  return ra >> (rb & 63u);
        // sat+: overflow iff rb > UINT64_MAX - ra
        case SH_BIN_SADD: return (rb > (UINT64_MAX - ra)) ? UINT64_MAX : (ra + rb);
        // sat-: underflow iff rb > ra
        case SH_BIN_SSUB: return (rb > ra) ? 0u : (ra - rb);
        default: return 0;
      }
    }
    case SH_K_I64: {
      int64_t ra = (int64_t)a, rb = (int64_t)b;
      switch (op) {
        case SH_BIN_ADD: return (uint64_t)(ra + rb);
        case SH_BIN_SUB: return (uint64_t)(ra - rb);
        case SH_BIN_MUL: return (uint64_t)(ra * rb);
        case SH_BIN_DIV: return rb ? (uint64_t)(ra / rb) : 0;
        case SH_BIN_MOD: return rb ? (uint64_t)(ra % rb) : 0;
        case SH_BIN_AND: return (uint64_t)(ra & rb);
        case SH_BIN_OR:  return (uint64_t)(ra | rb);
        case SH_BIN_XOR: return (uint64_t)(ra ^ rb);
        case SH_BIN_SHL: return (uint64_t)(ra << ((uint64_t)rb & 63u));
        case SH_BIN_SHR: return (uint64_t)(ra >> ((uint64_t)rb & 63u));
        // sat+ i64: detect overflow without UB.
        // overflow on add: pos+pos->neg or neg+neg->pos.
        case SH_BIN_SADD: {
          // Use unsigned arithmetic to avoid signed overflow UB.
          uint64_t ua = (uint64_t)ra, ub = (uint64_t)rb;
          uint64_t ur = ua + ub;
          int64_t  r  = (int64_t)ur;
          // Overflow iff same sign inputs but different sign output.
          if (!((ra ^ rb) < 0) && ((ra ^ r) < 0)) {
            // Both same sign; saturate to the direction of that sign.
            return (ra < 0) ? (uint64_t)INT64_MIN : (uint64_t)INT64_MAX;
          }
          return ur;
        }
        // sat- i64: detect underflow without UB (a - b = a + (-b); but -INT64_MIN overflows).
        case SH_BIN_SSUB: {
          uint64_t ua = (uint64_t)ra, ub = (uint64_t)rb;
          uint64_t ur = ua - ub;
          int64_t  r  = (int64_t)ur;
          // Underflow iff signs differ (ra positive, rb negative or vice versa) and
          // the result sign differs from ra's sign.  Equivalently: overflow on a-b
          // when (ra ^ rb) < 0 and (ra ^ r) < 0.
          if (((ra ^ rb) < 0) && ((ra ^ r) < 0)) {
            return (ra < 0) ? (uint64_t)INT64_MIN : (uint64_t)INT64_MAX;
          }
          return ur;
        }
        default: return 0;
      }
    }
    default:
      return 0;
  }
}

// Scalar compare: returns 0 or 1.
static uint64_t scalar_cmp_bits(sh_cmp op, sh_kind k, uint64_t a, uint64_t b) {
  if (k == SH_K_F32 || k == SH_K_F64) {
    double fa, fb;
    if (k == SH_K_F32) {
      uint32_t a32 = (uint32_t)a, b32 = (uint32_t)b;
      float fav, fbv;
      memcpy(&fav, &a32, 4);
      memcpy(&fbv, &b32, 4);
      fa = (double)fav;
      fb = (double)fbv;
    } else {
      memcpy(&fa, &a, 8);
      memcpy(&fb, &b, 8);
    }
    switch (op) {
      case SH_CMP_LT: return fa < fb ? 1 : 0;
      case SH_CMP_LE: return fa <= fb ? 1 : 0;
      case SH_CMP_EQ: return fa == fb ? 1 : 0;
      case SH_CMP_NE: return fa != fb ? 1 : 0;
      case SH_CMP_GT: return fa > fb ? 1 : 0;
      case SH_CMP_GE: return fa >= fb ? 1 : 0;
      default: return 0;
    }
  }
  if (k == SH_K_I64) {
    int64_t ia = (int64_t)a, ib = (int64_t)b;
    switch (op) {
      case SH_CMP_LT: return ia < ib ? 1 : 0;
      case SH_CMP_LE: return ia <= ib ? 1 : 0;
      case SH_CMP_EQ: return ia == ib ? 1 : 0;
      case SH_CMP_NE: return ia != ib ? 1 : 0;
      case SH_CMP_GT: return ia > ib ? 1 : 0;
      case SH_CMP_GE: return ia >= ib ? 1 : 0;
      default: return 0;
    }
  }
  // Unsigned comparison for all other kinds + bool
  switch (op) {
    case SH_CMP_LT: return a < b ? 1 : 0;
    case SH_CMP_LE: return a <= b ? 1 : 0;
    case SH_CMP_EQ: return a == b ? 1 : 0;
    case SH_CMP_NE: return a != b ? 1 : 0;
    case SH_CMP_GT: return a > b ? 1 : 0;
    case SH_CMP_GE: return a >= b ? 1 : 0;
    default: return 0;
  }
}

// ---------------------------------------------------------------------------
// Value construction helpers
// ---------------------------------------------------------------------------

// Build a scalar sh_value for kind k from an int64 literal (CONST sub=0).
static sh_value make_int_value(sh_kind k, int64_t imm) {
  sh_value v;
  memset(&v, 0, sizeof(v));
  v.kind = k;
  v.lanes = 1;
  switch (k) {
    case SH_K_U8:   v.u = (uint64_t)(uint8_t)(int8_t)imm; break;
    case SH_K_U16:  v.u = (uint64_t)(uint16_t)(int16_t)imm; break;
    case SH_K_U32:  v.u = (uint64_t)(uint32_t)(int32_t)imm; break;
    case SH_K_U64:  v.u = (uint64_t)imm; break;
    case SH_K_I64:  v.i = imm; break;
    case SH_K_BOOL: v.u = imm ? 1 : 0; break;
    default:        v.u = (uint64_t)imm; break;
  }
  return v;
}

// Build a float sh_value from a double bit pattern (CONST sub=1).
static sh_value make_float_value(sh_kind k, int64_t imm) {
  sh_value v;
  memset(&v, 0, sizeof(v));
  v.kind = k;
  v.lanes = 1;
  double d;
  uint64_t bits = (uint64_t)imm;
  memcpy(&d, &bits, sizeof(d));
  if (k == SH_K_F32) {
    v.f = (double)(float)d;
  } else {
    v.f = d;
  }
  return v;
}

// Scalar CVT: convert value to target kind.
static sh_value do_cvt(sh_value src, sh_kind dst) {
  sh_value v;
  memset(&v, 0, sizeof(v));
  v.kind = dst;
  v.lanes = 1;

  sh_kind sk = src.kind;
  if (sh_kind_is_float(sk)) {
    double df = src.f;
    switch (dst) {
      case SH_K_U8:   v.u = (uint64_t)(uint8_t)df; break;
      case SH_K_U16:  v.u = (uint64_t)(uint16_t)df; break;
      case SH_K_U32:  v.u = (uint64_t)(uint32_t)df; break;
      case SH_K_U64:  v.u = (uint64_t)df; break;
      case SH_K_I64:  v.i = (int64_t)df; break;
      case SH_K_BOOL: v.u = df != 0.0 ? 1 : 0; break;
      case SH_K_F32:  v.f = (double)(float)df; break;
      case SH_K_F64:  v.f = df; break;
      default: break;
    }
  } else if (sh_kind_is_int(sk) || sk == SH_K_BOOL) {
    // For signed i64, use the signed value; for all others treat as unsigned.
    int64_t si = (sk == SH_K_I64) ? src.i : (int64_t)src.u;
    uint64_t ui = (sk == SH_K_I64) ? (uint64_t)src.i : src.u;
    switch (dst) {
      case SH_K_U8:   v.u = (uint64_t)(uint8_t)ui; break;
      case SH_K_U16:  v.u = (uint64_t)(uint16_t)ui; break;
      case SH_K_U32:  v.u = (uint64_t)(uint32_t)ui; break;
      case SH_K_U64:  v.u = ui; break;
      case SH_K_I64:  v.i = si; break;
      case SH_K_BOOL: v.u = ui ? 1 : 0; break;
      case SH_K_F32:
        // For unsigned sources, cast via unsigned path; for i64 via signed.
        if (sk == SH_K_I64) {
          v.f = (double)(float)(double)si;
        } else {
          v.f = (double)(float)(double)ui;
        }
        break;
      case SH_K_F64:
        if (sk == SH_K_I64) {
          v.f = (double)si;
        } else {
          v.f = (double)ui;
        }
        break;
      default: break;
    }
  }
  return v;
}

// Apply NEG to a scalar sh_value.
static sh_value do_neg(sh_value src) {
  sh_value v = src;
  sh_kind k = src.kind;
  if (sh_kind_is_float(k)) {
    v.f = -src.f;
    if (k == SH_K_F32) {
      v.f = (double)(float)v.f;
    }
  } else if (k == SH_K_I64) {
    v.i = -src.i;
  } else {
    uint64_t mask;
    switch (k) {
      case SH_K_U8:  mask = 0xFFu; break;
      case SH_K_U16: mask = 0xFFFFu; break;
      case SH_K_U32: mask = 0xFFFFFFFFu; break;
      default:       mask = UINT64_MAX; break;
    }
    v.u = (-src.u) & mask;
  }
  return v;
}

// ---------------------------------------------------------------------------
// Interpreter context
// ---------------------------------------------------------------------------

#define MAX_RECUR_VARS 32

typedef struct {
  const sh_program *p;
  const sh_value   *args;
  sh_value         *slots;
  sh_error         *err;
  bool     pending_recur;
  uint32_t pending_loop_idx;
  sh_value pending_recur_vals[MAX_RECUR_VARS];
} interp_ctx;

static sh_status eval(interp_ctx *ctx, sh_nref ref, sh_value *out);

// ---------------------------------------------------------------------------
// Scalar binop / cmp on sh_values
// ---------------------------------------------------------------------------

static sh_value do_binop(sh_binop op, sh_kind k, sh_value va, sh_value vb) {
  sh_value res;
  memset(&res, 0, sizeof(res));
  res.kind = k;
  res.lanes = 1;
  uint64_t abits = scalar_to_lane(k, va);
  uint64_t bbits = scalar_to_lane(k, vb);
  uint64_t rbits = scalar_binop_bits(op, k, abits, bbits);
  if (sh_kind_is_float(k)) {
    sh_value tmp = lane_to_scalar(k, rbits);
    res.f = tmp.f;
  } else if (k == SH_K_I64) {
    res.i = (int64_t)rbits;
  } else {
    res.u = rbits;
  }
  return res;
}

static sh_value do_cmp(sh_cmp op, sh_kind k, sh_value va, sh_value vb) {
  sh_value res;
  memset(&res, 0, sizeof(res));
  res.kind = SH_K_BOOL;
  res.lanes = 1;
  uint64_t abits = scalar_to_lane(k, va);
  uint64_t bbits = scalar_to_lane(k, vb);
  res.u = scalar_cmp_bits(op, k, abits, bbits);
  return res;
}

// ---------------------------------------------------------------------------
// eval: main tree-walker
// ---------------------------------------------------------------------------

static sh_status eval(interp_ctx *ctx, sh_nref ref, sh_value *out) {
  const sh_program *p = ctx->p;
  if (ref == SH_NREF_NONE || ref >= p->nnodes)
    return sh_set_error(ctx->err, SH_ERR_INTERNAL, -1, -1,
                        "eval: invalid node ref %u", ref);

  const sh_node *n = &p->nodes[ref];
  sh_op op = (sh_op)n->op;
  sh_status s;
  sh_value va, vb, vc_val;
  memset(&va, 0, sizeof(va));
  memset(&vb, 0, sizeof(vb));
  memset(&vc_val, 0, sizeof(vc_val));

  switch (op) {

    // --- CONST --------------------------------------------------------------
    case SH_OP_CONST: {
      sh_kind k = (sh_kind)n->type.kind;
      if (n->sub == 2) {
        *out = sh_val_bool(n->imm != 0);
      } else if (n->sub == 1) {
        *out = make_float_value(k, n->imm);
      } else {
        *out = make_int_value(k, n->imm);
      }
      return SH_OK;
    }

    // --- PARAM --------------------------------------------------------------
    case SH_OP_PARAM:
      *out = ctx->args[n->a];
      return SH_OK;

    // --- LOCAL --------------------------------------------------------------
    case SH_OP_LOCAL:
      *out = ctx->slots[n->a];
      return SH_OK;

    // --- UNOP ---------------------------------------------------------------
    case SH_OP_UNOP: {
      s = eval(ctx, n->a, &va);
      if (s != SH_OK) return s;
      sh_unop unop = (sh_unop)n->sub;
      switch (unop) {
        case SH_UN_NEG:
          *out = do_neg(va);
          break;
        case SH_UN_NOT:
          *out = sh_val_bool(va.u == 0);
          break;
        case SH_UN_CVT:
          *out = do_cvt(va, (sh_kind)n->type.kind);
          break;
        default:
          return sh_set_error(ctx->err, SH_ERR_INTERNAL, -1, -1,
                              "eval: unknown UNOP sub %u", n->sub);
      }
      return SH_OK;
    }

    // --- BINOP --------------------------------------------------------------
    case SH_OP_BINOP: {
      s = eval(ctx, n->a, &va);
      if (s != SH_OK) return s;
      s = eval(ctx, n->b, &vb);
      if (s != SH_OK) return s;
      sh_kind k = (sh_kind)n->type.kind;
      *out = do_binop((sh_binop)n->sub, k, va, vb);
      return SH_OK;
    }

    // --- CMP ----------------------------------------------------------------
    case SH_OP_CMP: {
      s = eval(ctx, n->a, &va);
      if (s != SH_OK) return s;
      s = eval(ctx, n->b, &vb);
      if (s != SH_OK) return s;
      // Operand kind from n->a's type (result type is bool)
      sh_kind k = (sh_kind)p->nodes[n->a].type.kind;
      *out = do_cmp((sh_cmp)n->sub, k, va, vb);
      return SH_OK;
    }

    // --- IF -----------------------------------------------------------------
    case SH_OP_IF: {
      s = eval(ctx, n->a, &va);
      if (s != SH_OK) return s;
      if (va.u) {
        return eval(ctx, n->b, out);
      } else {
        return eval(ctx, n->c, out);
      }
    }

    // --- LET ----------------------------------------------------------------
    case SH_OP_LET: {
      uint32_t first_slot = n->a;
      for (uint32_t i = 0; i < n->aux_len; i++) {
        sh_nref init_ref = p->aux[n->aux_off + i];
        sh_value init_val;
        s = eval(ctx, init_ref, &init_val);
        if (s != SH_OK) return s;
        ctx->slots[first_slot + i] = init_val;
      }
      return eval(ctx, n->b, out);
    }

    // --- LOOP ---------------------------------------------------------------
    case SH_OP_LOOP: {
      uint32_t loop_idx = n->a;
      const sh_loop *lp = &p->loops[loop_idx];

      // Evaluate init exprs and store into induction var slots
      for (uint32_t vi = 0; vi < lp->nvars; vi++) {
        sh_nref init_ref = p->aux[lp->init_off + vi];
        sh_value init_val;
        s = eval(ctx, init_ref, &init_val);
        if (s != SH_OK) return s;
        ctx->slots[lp->var_slot0 + vi] = init_val;
      }

      // Iterate until the body does not signal a RECUR for this loop
      while (1) {
        sh_value body_val;
        memset(&body_val, 0, sizeof(body_val));
        s = eval(ctx, lp->body, &body_val);
        if (s != SH_OK) return s;

        if (ctx->pending_recur && ctx->pending_loop_idx == loop_idx) {
          // Update induction vars and continue
          for (uint32_t vi = 0; vi < lp->nvars; vi++) {
            ctx->slots[lp->var_slot0 + vi] = ctx->pending_recur_vals[vi];
          }
          ctx->pending_recur = false;
        } else {
          // Normal exit
          *out = body_val;
          return SH_OK;
        }
      }
    }

    // --- RECUR --------------------------------------------------------------
    case SH_OP_RECUR: {
      uint32_t loop_idx = n->a;
      uint32_t nargs = n->aux_len;
      if (nargs > MAX_RECUR_VARS)
        return sh_set_error(ctx->err, SH_ERR_INTERNAL, -1, -1,
                            "RECUR: too many induction vars (%u)", nargs);
      // Evaluate all new induction args before writing anything
      for (uint32_t i = 0; i < nargs; i++) {
        sh_nref arg_ref = p->aux[n->aux_off + i];
        s = eval(ctx, arg_ref, &ctx->pending_recur_vals[i]);
        if (s != SH_OK) return s;
      }
      ctx->pending_recur = true;
      ctx->pending_loop_idx = loop_idx;
      memset(out, 0, sizeof(*out));
      return SH_OK;
    }

    // --- CALL ---------------------------------------------------------------
    case SH_OP_CALL: {
      uint32_t prim_idx = n->a;
      uint32_t nargs = n->aux_len;
      if (!p->prims || prim_idx >= p->prims->count)
        return sh_set_error(ctx->err, SH_ERR_INTERNAL, -1, -1,
                            "CALL: invalid prim index %u", prim_idx);
      const sh_prim *prim = &p->prims->prims[prim_idx];
      sh_value call_args[SH_MAX_PRIM_PARAMS];
      uint32_t limit = nargs < SH_MAX_PRIM_PARAMS ? nargs : SH_MAX_PRIM_PARAMS;
      for (uint32_t i = 0; i < limit; i++) {
        sh_nref arg_ref = p->aux[n->aux_off + i];
        s = eval(ctx, arg_ref, &call_args[i]);
        if (s != SH_OK) return s;
      }
      if (!prim->fn)
        return sh_set_error(ctx->err, SH_ERR_INTERNAL, -1, -1,
                            "CALL: prim '%s' has null fn", prim->name);
      // Pass limit (not nargs) so the callee sees only initialised entries.
      // The verifier enforces nargs <= SH_MAX_PRIM_PARAMS, so limit == nargs
      // in all valid programs; this guards against a future verifier bug.
      *out = prim->fn(call_args, limit);
      return SH_OK;
    }

    // --- REGION_LOAD --------------------------------------------------------
    case SH_OP_REGION_LOAD: {
      s = eval(ctx, n->a, &va);
      if (s != SH_OK) return s;
      s = eval(ctx, n->b, &vb);
      if (s != SH_OK) return s;
      uint64_t idx = (vb.kind == SH_K_I64) ? (uint64_t)vb.i : vb.u;
      if (idx >= (uint64_t)va.region.len)
        return sh_set_error(ctx->err, SH_ERR_BOUNDS, -1, -1,
                            "region load: index %llu >= length %u",
                            (unsigned long long)idx, va.region.len);
      sh_kind elem = va.region.elem;
      uint32_t esz = sh_kind_size(elem);
      const uint8_t *ptr = va.region.base + idx * esz;
      memset(out, 0, sizeof(*out));
      out->kind = (sh_kind)n->type.kind;
      out->lanes = 1;
      switch (elem) {
        case SH_K_BOOL:
        case SH_K_U8:  out->u = *ptr; break;
        case SH_K_U16: { uint16_t v16; memcpy(&v16, ptr, 2); out->u = v16; break; }
        case SH_K_U32: { uint32_t v32; memcpy(&v32, ptr, 4); out->u = v32; break; }
        case SH_K_U64: { uint64_t v64; memcpy(&v64, ptr, 8); out->u = v64; break; }
        case SH_K_I64: { int64_t  v64; memcpy(&v64, ptr, 8); out->i = v64; break; }
        case SH_K_F32: {
          float fv;
          memcpy(&fv, ptr, 4);
          out->f = (double)(float)fv;
          break;
        }
        case SH_K_F64: { double dv; memcpy(&dv, ptr, 8); out->f = dv; break; }
        default: break;
      }
      return SH_OK;
    }

    // --- REGION_STORE -------------------------------------------------------
    case SH_OP_REGION_STORE: {
      s = eval(ctx, n->a, &va);
      if (s != SH_OK) return s;
      s = eval(ctx, n->b, &vb);
      if (s != SH_OK) return s;
      s = eval(ctx, n->c, &vc_val);
      if (s != SH_OK) return s;
      uint64_t idx = (vb.kind == SH_K_I64) ? (uint64_t)vb.i : vb.u;
      if (idx >= (uint64_t)va.region.len)
        return sh_set_error(ctx->err, SH_ERR_BOUNDS, -1, -1,
                            "region store: index %llu >= length %u",
                            (unsigned long long)idx, va.region.len);
      sh_kind elem = va.region.elem;
      uint32_t esz = sh_kind_size(elem);
      uint8_t *ptr = va.region.base + idx * esz;
      switch (elem) {
        case SH_K_BOOL:
        case SH_K_U8: { uint8_t v8 = (uint8_t)vc_val.u; memcpy(ptr, &v8, 1); break; }
        case SH_K_U16: { uint16_t v16 = (uint16_t)vc_val.u; memcpy(ptr, &v16, 2); break; }
        case SH_K_U32: { uint32_t v32 = (uint32_t)vc_val.u; memcpy(ptr, &v32, 4); break; }
        case SH_K_U64: { memcpy(ptr, &vc_val.u, 8); break; }
        case SH_K_I64: { memcpy(ptr, &vc_val.i, 8); break; }
        case SH_K_F32: {
          float fv = (float)vc_val.f;
          memcpy(ptr, &fv, 4);
          break;
        }
        case SH_K_F64: { memcpy(ptr, &vc_val.f, 8); break; }
        default: break;
      }
      // Result is the stored value (element type, as set by verifier)
      *out = vc_val;
      out->kind = (sh_kind)n->type.kind;
      return SH_OK;
    }

    // --- REGION_LEN ---------------------------------------------------------
    case SH_OP_REGION_LEN: {
      s = eval(ctx, n->a, &va);
      if (s != SH_OK) return s;
      *out = sh_val_u32(va.region.len);
      return SH_OK;
    }

    // --- VSPLAT -------------------------------------------------------------
    case SH_OP_VSPLAT: {
      s = eval(ctx, n->a, &va);
      if (s != SH_OK) return s;
      sh_kind lk = (sh_kind)n->type.lane_kind;
      uint8_t nlanes = n->type.lanes;
      uint64_t bits = scalar_to_lane(lk, va);
      memset(out, 0, sizeof(*out));
      out->kind = SH_K_VEC;
      out->lanes = nlanes;
      out->lane_kind = (uint8_t)lk;
      for (uint8_t li = 0; li < nlanes; li++) out->lane[li] = bits;
      return SH_OK;
    }

    // --- VBINOP -------------------------------------------------------------
    case SH_OP_VBINOP: {
      s = eval(ctx, n->a, &va);
      if (s != SH_OK) return s;
      s = eval(ctx, n->b, &vb);
      if (s != SH_OK) return s;
      sh_kind lk = (sh_kind)n->type.lane_kind;
      uint8_t nlanes = n->type.lanes;
      sh_binop bop = (sh_binop)n->sub;
      memset(out, 0, sizeof(*out));
      out->kind = SH_K_VEC;
      out->lanes = nlanes;
      out->lane_kind = (uint8_t)lk;
      for (uint8_t li = 0; li < nlanes; li++)
        out->lane[li] = scalar_binop_bits(bop, lk, va.lane[li], vb.lane[li]);
      return SH_OK;
    }

    // --- VCMP ---------------------------------------------------------------
    case SH_OP_VCMP: {
      s = eval(ctx, n->a, &va);
      if (s != SH_OK) return s;
      s = eval(ctx, n->b, &vb);
      if (s != SH_OK) return s;
      // Operand lane kind from n->a's type
      sh_kind lk = (sh_kind)p->nodes[n->a].type.lane_kind;
      uint8_t nlanes = n->type.lanes;
      sh_cmp cmp = (sh_cmp)n->sub;
      memset(out, 0, sizeof(*out));
      out->kind = SH_K_VEC;
      out->lanes = nlanes;
      out->lane_kind = (uint8_t)SH_K_BOOL;
      for (uint8_t li = 0; li < nlanes; li++)
        out->lane[li] = scalar_cmp_bits(cmp, lk, va.lane[li], vb.lane[li]);
      return SH_OK;
    }

    // --- VSELECT ------------------------------------------------------------
    case SH_OP_VSELECT: {
      s = eval(ctx, n->a, &va);
      if (s != SH_OK) return s;
      s = eval(ctx, n->b, &vb);
      if (s != SH_OK) return s;
      s = eval(ctx, n->c, &vc_val);
      if (s != SH_OK) return s;
      sh_kind lk = (sh_kind)n->type.lane_kind;
      uint8_t nlanes = n->type.lanes;
      memset(out, 0, sizeof(*out));
      out->kind = SH_K_VEC;
      out->lanes = nlanes;
      out->lane_kind = (uint8_t)lk;
      for (uint8_t li = 0; li < nlanes; li++)
        out->lane[li] = va.lane[li] ? vb.lane[li] : vc_val.lane[li];
      return SH_OK;
    }

    // --- VSHUFFLE -----------------------------------------------------------
    case SH_OP_VSHUFFLE: {
      s = eval(ctx, n->a, &va);
      if (s != SH_OK) return s;
      sh_kind lk = (sh_kind)n->type.lane_kind;
      uint8_t nlanes = n->type.lanes;
      memset(out, 0, sizeof(*out));
      out->kind = SH_K_VEC;
      out->lanes = nlanes;
      out->lane_kind = (uint8_t)lk;
      for (uint8_t li = 0; li < nlanes; li++) {
        uint32_t src_idx = p->aux[n->aux_off + li];
        if (src_idx >= va.lanes)
          return sh_set_error(ctx->err, SH_ERR_INTERNAL, -1, -1,
                              "VSHUFFLE: lane index %u >= nlanes %u",
                              src_idx, (uint32_t)va.lanes);
        out->lane[li] = va.lane[src_idx];
      }
      return SH_OK;
    }

    // --- VREDUCE ------------------------------------------------------------
    case SH_OP_VREDUCE: {
      s = eval(ctx, n->a, &va);
      if (s != SH_OK) return s;
      sh_reduce rop = (sh_reduce)n->sub;
      sh_kind lk = (sh_kind)p->nodes[n->a].type.lane_kind;
      uint8_t nlanes = p->nodes[n->a].type.lanes;

      if (rop == SH_RED_DOT) {
        s = eval(ctx, n->b, &vb);
        if (s != SH_OK) return s;
        uint64_t acc = 0;
        for (uint8_t li = 0; li < nlanes; li++) {
          uint64_t prod = scalar_binop_bits(SH_BIN_MUL, lk, va.lane[li], vb.lane[li]);
          if (li == 0) {
            acc = prod;
          } else {
            acc = scalar_binop_bits(SH_BIN_ADD, lk, acc, prod);
          }
        }
        *out = lane_to_scalar(lk, acc);
      } else {
        if (nlanes == 0) {
          memset(out, 0, sizeof(*out));
          out->kind = (sh_kind)n->type.kind;
          out->lanes = 1;
          return SH_OK;
        }
        uint64_t acc = va.lane[0];
        for (uint8_t li = 1; li < nlanes; li++) {
          uint64_t cur = va.lane[li];
          switch (rop) {
            case SH_RED_ADD:
              acc = scalar_binop_bits(SH_BIN_ADD, lk, acc, cur);
              break;
            case SH_RED_MIN: {
              uint64_t lt = scalar_cmp_bits(SH_CMP_LT, lk, cur, acc);
              acc = lt ? cur : acc;
              break;
            }
            case SH_RED_MAX: {
              uint64_t gt = scalar_cmp_bits(SH_CMP_GT, lk, cur, acc);
              acc = gt ? cur : acc;
              break;
            }
            default: break;
          }
        }
        *out = lane_to_scalar(lk, acc);
      }
      out->kind = (sh_kind)n->type.kind;
      return SH_OK;
    }

    // --- VLANE --------------------------------------------------------------
    case SH_OP_VLANE: {
      s = eval(ctx, n->a, &va);
      if (s != SH_OK) return s;
      sh_kind lk = (sh_kind)p->nodes[n->a].type.lane_kind;
      uint32_t lane_idx = (uint32_t)n->imm;
      if (lane_idx >= va.lanes)
        return sh_set_error(ctx->err, SH_ERR_INTERNAL, -1, -1,
                            "VLANE: lane index %u >= nlanes %u",
                            lane_idx, (uint32_t)va.lanes);
      *out = lane_to_scalar(lk, va.lane[lane_idx]);
      out->kind = (sh_kind)n->type.kind;
      return SH_OK;
    }

    // --- VREGION_LOAD -------------------------------------------------------
    case SH_OP_VREGION_LOAD: {
      s = eval(ctx, n->a, &va);
      if (s != SH_OK) return s;
      s = eval(ctx, n->b, &vb);
      if (s != SH_OK) return s;
      uint64_t idx = (vb.kind == SH_K_I64) ? (uint64_t)vb.i : vb.u;
      uint64_t len = (uint64_t)va.region.len;
      uint64_t N   = (uint64_t)n->imm;
      // Overflow-free bounds check: idx > len || (len - idx) < N
      if (idx > len || (uint64_t)(len - idx) < N)
        return sh_set_error(ctx->err, SH_ERR_BOUNDS, -1, -1,
                            "vregion-ref: bounds check failed (idx=%llu len=%llu N=%llu)",
                            (unsigned long long)idx,
                            (unsigned long long)len,
                            (unsigned long long)N);
      sh_kind elem = va.region.elem;
      uint32_t esz = sh_kind_size(elem);
      const uint8_t *base = va.region.base + idx * esz;
      memset(out, 0, sizeof(*out));
      out->kind      = SH_K_VEC;
      out->lanes     = (uint8_t)N;
      out->lane_kind = (uint8_t)elem;
      for (uint64_t k = 0; k < N; k++) {
        const uint8_t *ptr = base + k * esz;
        uint64_t bits = 0;
        switch (elem) {
          case SH_K_BOOL:
          case SH_K_U8:  bits = *ptr; break;
          case SH_K_U16: { uint16_t v16; memcpy(&v16, ptr, 2); bits = v16; break; }
          case SH_K_U32: { uint32_t v32; memcpy(&v32, ptr, 4); bits = v32; break; }
          case SH_K_U64: { uint64_t v64; memcpy(&v64, ptr, 8); bits = v64; break; }
          case SH_K_I64: { int64_t  v64; memcpy(&v64, ptr, 8); bits = (uint64_t)v64; break; }
          case SH_K_F32: {
            float fv;
            memcpy(&fv, ptr, 4);
            uint32_t b32;
            memcpy(&b32, &fv, 4);
            bits = (uint64_t)b32;
            break;
          }
          case SH_K_F64: { double dv; memcpy(&dv, ptr, 8); memcpy(&bits, &dv, 8); break; }
          default: break;
        }
        out->lane[k] = bits;
      }
      return SH_OK;
    }

    // --- VREGION_STORE ------------------------------------------------------
    case SH_OP_VREGION_STORE: {
      s = eval(ctx, n->a, &va);
      if (s != SH_OK) return s;
      s = eval(ctx, n->b, &vb);
      if (s != SH_OK) return s;
      s = eval(ctx, n->c, &vc_val);
      if (s != SH_OK) return s;
      uint64_t idx = (vb.kind == SH_K_I64) ? (uint64_t)vb.i : vb.u;
      uint64_t len = (uint64_t)va.region.len;
      uint64_t N   = (uint64_t)vc_val.lanes;
      // Overflow-free bounds check
      if (idx > len || (uint64_t)(len - idx) < N)
        return sh_set_error(ctx->err, SH_ERR_BOUNDS, -1, -1,
                            "vregion-set!: bounds check failed (idx=%llu len=%llu N=%llu)",
                            (unsigned long long)idx,
                            (unsigned long long)len,
                            (unsigned long long)N);
      sh_kind elem = va.region.elem;
      uint32_t esz = sh_kind_size(elem);
      uint8_t *base = va.region.base + idx * esz;
      for (uint64_t k = 0; k < N; k++) {
        uint8_t *ptr = base + k * esz;
        uint64_t bits = vc_val.lane[k];
        switch (elem) {
          case SH_K_BOOL:
          case SH_K_U8:  { uint8_t  v8  = (uint8_t)bits;  memcpy(ptr, &v8,  1); break; }
          case SH_K_U16: { uint16_t v16 = (uint16_t)bits; memcpy(ptr, &v16, 2); break; }
          case SH_K_U32: { uint32_t v32 = (uint32_t)bits; memcpy(ptr, &v32, 4); break; }
          case SH_K_U64: { memcpy(ptr, &bits, 8); break; }
          case SH_K_I64: { memcpy(ptr, &bits, 8); break; }
          case SH_K_F32: { uint32_t b32 = (uint32_t)bits; memcpy(ptr, &b32, 4); break; }
          case SH_K_F64: { memcpy(ptr, &bits, 8); break; }
          default: break;
        }
      }
      *out = vc_val;  // return the stored vector
      return SH_OK;
    }

    default:
      return sh_set_error(ctx->err, SH_ERR_INTERNAL, -1, -1,
                          "eval: unknown op %u at node %u", n->op, ref);
  }
}

// ---------------------------------------------------------------------------
// shi_invoke: internal entry (program must already be verified)
// ---------------------------------------------------------------------------

sh_status shi_invoke(const sh_program *p, const sh_value *args, uint32_t argc,
                     sh_value *out, sh_error *err) {
  sh_value *slots = NULL;
  if (p->nlocals > 0) {
    slots = (sh_value *)calloc(p->nlocals, sizeof(sh_value));
    if (!slots)
      return sh_set_error(err, SH_ERR_OOM, -1, -1,
                          "shi_invoke: out of memory for slot array");
  }

  interp_ctx ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.p = p;
  ctx.args = args;
  ctx.slots = slots;
  ctx.err = err;

  // Validate arg types match declared param types
  for (uint32_t i = 0; i < argc; i++) {
    sh_kind ak = args[i].kind;
    sh_kind pk = (sh_kind)p->params[i].kind;
    if (pk != ak) {
      free(slots);
      return sh_set_error(err, SH_ERR_TYPE, -1, -1,
                          "arg %u: expected kind %u, got kind %u", i, pk, ak);
    }
  }

  sh_status s = eval(&ctx, p->root, out);
  free(slots);
  return s;
}

// ---------------------------------------------------------------------------
// shi_cost_for_args: compute actual worst-case cost with concrete args
// ---------------------------------------------------------------------------

uint64_t shi_cost_for_args(const sh_program *p, const sh_value *args,
                           uint32_t argc) {
  if (!p) return 0;
  if (p->cost.is_const) return p->cost.const_cost;

  uint64_t total = 0;
  for (uint32_t i = 0; i < p->nloops; i++) {
    const sh_loop *lp = &p->loops[i];
    if (lp->bound.kind == SH_BOUND_CONST) {
      total += lp->bound.konst * lp->bound.per_iter_cost;
    } else if (lp->bound.kind == SH_BOUND_PARAM) {
      uint32_t pidx = lp->bound.param_idx;
      uint64_t trip = 0;
      if (pidx < argc) {
        trip = args[pidx].u;
      }
      total += trip * lp->bound.per_iter_cost;
    }
  }
  return total;
}

// ---------------------------------------------------------------------------
// Public entry: validate then dispatch to shi_invoke
// ---------------------------------------------------------------------------

sh_status sh_invoke(const sh_program *p, const sh_value *args, uint32_t argc,
                    sh_value *out, sh_error *err) {
  if (!p || !p->verified)
    return sh_set_error(err, SH_ERR_INTERNAL, -1, -1,
                        "invoke of unverified program");
  if (argc != p->nparams)
    return sh_set_error(err, SH_ERR_ARITY, -1, -1,
                        "shader expects %u args, got %u", p->nparams, argc);
  return shi_invoke(p, args, argc, out, err);
}
