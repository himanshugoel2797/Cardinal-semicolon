// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

// S3 UNIT 2+3 -- the bytecode VM: chunk validator + scalar executor +
//   SSE/AVX fast paths for vector ops.
//
// sh_chunk_validate:  reject any chunk with OOB vreg refs, jump targets,
//   aux ranges, prim indices, or a bad result vreg. Defense-in-depth so a
//   buggy lowerer cannot drive sh_vm_run out of bounds.
//
// sh_vm_run: execute the chunk on typed args; runtime-bounds-check every
//   region access; semantics match sh_interp.c bit-for-bit. With SSE2
//   available, vector ops use SIMD intrinsic paths for common widths; the
//   scalar lane-loop fallback is retained for odd widths, ops without clean
//   intrinsics, and SH_VM_FORCE_SCALAR.
//
// SIMD paths implemented (S3-3):
//   SHB_VSPLAT  -- f32x4 (_mm_set1_ps), u32x4/i32x4 (_mm_set1_epi32),
//                  u8x16 (_mm_set1_epi8), u16x8 (_mm_set1_epi16);
//                  f32x8/u32x8 via AVX2 when available.
//                  i64 vectors (any width) always fall through to the scalar
//                  lane loop: each i64 lane is 8 bytes; _mm_set1_epi32 is
//                  4-byte and would silently truncate each i64 lane.
//   SHB_VBINOP  -- f32x4 add/sub/mul/div; u32x4 add/sub;
//                  u8x16/u16x8 add/sub; u32x4 mul (SSE4.1 mullo);
//                  all 256-bit variants via AVX2.  Integer div/mod always scalar.
//                  i64 vectors always use the scalar lane loop (see VSPLAT note).
//   SHB_VCMP    -- f32x4 all six predicates; u32x4 eq/lt/gt; u8x16 eq.
//                  Result: one BOOL lane per 1-byte slot = 0 or 1 (matches oracle).
//   SHB_VSELECT -- f32x4/u32x4/u8x16: blend from 0/1 bool-lane mask.
//   SHB_VSHUFFLE-- 4-lane shuffles where all indices fit _mm_shuffle (imm8);
//                  else scalar.
//   SHB_VREDUCE -- ALWAYS scalar (float reductions MUST be left-to-right;
//                  a tree reduction reassociates and diverges -- this is the
//                  #1 bit-exactness trap; integer reductions also scalar for
//                  simplicity and correctness with the oracle's ordering).
//   SHB_VLANE   -- always scalar (single-element extract; no SIMD payoff).
//
// Value representation (internal):
//   typedef vm_value (see below). Scalars: kind + scalar u64 field.
//   Vectors: kind=SH_K_VEC, lanes/lane_kind set, bytes packed at native
//   element width in a 16-byte-aligned vec[SH_MAX_LANES * 8] buffer so that
//   _mm_load_* works directly without a reformat step.
//   Regions: kind=SH_K_REGION, base/len/elem/mut carried directly.
//
// Dispatch: a switch() over sh_bc_op in the main pc loop.
// All scalar math is delegated to static helpers (scalar_binop_bits,
// scalar_cmp_bits) that mirror sh_interp.c exactly.

#include <stdlib.h>
#include <string.h>

// SSE/AVX intrinsics -- guarded so the file compiles without them.
#if defined(__SSE2__)
#include <immintrin.h>
#endif

#include "sh_bytecode.h"

// ---------------------------------------------------------------------------
// Internal value representation
// ---------------------------------------------------------------------------

// vm_value: the slot-file cell. Vectors are stored PACKED at native element
// width, 16-byte aligned, so S3-3 can _mm_load_* them directly.
// Scalars live in the union's `scalar` field.
// Regions carry the pointer + length + elem-kind.
typedef struct {
  sh_kind  kind;
  uint8_t  lanes;      // 0 for scalars / regions
  uint8_t  lane_kind;  // SH_K_VEC: the element's scalar kind
  uint8_t  flags;      // bit0 = region mutable
  union {
    uint64_t scalar;   // scalar bit pattern (u8/u16/u32/u64/i64/bool/f32/f64)
    struct {
      uint8_t  *base;
      uint32_t  len;   // element count
      sh_kind   elem;
    } region;
    // Packed vector lanes at native element width. 16-byte aligned.
    _Alignas(16) uint8_t vec[SH_MAX_LANES * 8];
  };
} vm_value;

// ---------------------------------------------------------------------------
// Public/internal value conversion helpers
// ---------------------------------------------------------------------------

// Convert public sh_value -> vm_value (used at arg load boundary).
static vm_value from_sh_value(sh_value sv) {
  vm_value vv;
  memset(&vv, 0, sizeof(vv));
  vv.kind = sv.kind;

  if (sv.kind == SH_K_VEC) {
    vv.lanes     = sv.lanes;
    vv.lane_kind = sv.lane_kind;
    sh_kind lk   = (sh_kind)sv.lane_kind;
    uint32_t esz = sh_kind_size(lk);
    if (esz == 0) esz = 1;
    for (uint8_t li = 0; li < sv.lanes; li++) {
      uint64_t bits = sv.lane[li];
      // Narrow to native element width.
      uint8_t *dst = &vv.vec[li * esz];
      memcpy(dst, &bits, esz);
    }
  } else if (sv.kind == SH_K_REGION) {
    vv.region.base = sv.region.base;
    vv.region.len  = sv.region.len;
    vv.region.elem = sv.region.elem;
    vv.flags       = sv.region.mutable_ ? 1u : 0u;
  } else {
    // Scalar: store bit pattern.
    switch (sv.kind) {
      case SH_K_BOOL:
      case SH_K_U8:
      case SH_K_U16:
      case SH_K_U32:
      case SH_K_U64:
        vv.scalar = sv.u; break;
      case SH_K_I64:
        vv.scalar = (uint64_t)sv.i; break;
      case SH_K_F32: {
        // Store as the f32 bit pattern (as interp: f holds the narrowed double).
        float fv = (float)sv.f;
        uint32_t b32;
        memcpy(&b32, &fv, 4);
        vv.scalar = (uint64_t)b32;
        break;
      }
      case SH_K_F64:
        memcpy(&vv.scalar, &sv.f, 8); break;
      default:
        vv.scalar = sv.u; break;
    }
  }
  return vv;
}

// Convert vm_value -> public sh_value (used at result boundary).
static sh_value to_sh_value(vm_value vv) {
  sh_value sv;
  memset(&sv, 0, sizeof(sv));
  sv.kind = vv.kind;

  if (vv.kind == SH_K_VEC) {
    sv.lanes     = vv.lanes;
    sv.lane_kind = vv.lane_kind;
    sh_kind lk   = (sh_kind)vv.lane_kind;
    uint32_t esz = sh_kind_size(lk);
    if (esz == 0) esz = 1;
    for (uint8_t li = 0; li < vv.lanes; li++) {
      uint64_t bits = 0;
      memcpy(&bits, &vv.vec[li * esz], esz);
      sv.lane[li] = bits;
    }
  } else if (vv.kind == SH_K_REGION) {
    sv.region.base   = vv.region.base;
    sv.region.len    = vv.region.len;
    sv.region.elem   = vv.region.elem;
    sv.region.mutable_ = (vv.flags & 1u) ? 1 : 0;
  } else {
    switch (vv.kind) {
      case SH_K_BOOL:
      case SH_K_U8:
      case SH_K_U16:
      case SH_K_U32:
      case SH_K_U64:
        sv.u = vv.scalar; sv.lanes = 1; break;
      case SH_K_I64:
        sv.i = (int64_t)vv.scalar; sv.lanes = 1; break;
      case SH_K_F32: {
        uint32_t b32 = (uint32_t)vv.scalar;
        float fv;
        memcpy(&fv, &b32, 4);
        sv.f = (double)(float)fv;  // match interp: narrowed double
        sv.lanes = 1;
        break;
      }
      case SH_K_F64:
        memcpy(&sv.f, &vv.scalar, 8); sv.lanes = 1; break;
      default:
        sv.u = vv.scalar; sv.lanes = 1; break;
    }
  }
  return sv;
}

// ---------------------------------------------------------------------------
// Scalar bit-pattern helpers (mirror sh_interp.c exactly)
// ---------------------------------------------------------------------------

// Extract a 64-bit bit pattern from a vm_value scalar slot.
static uint64_t vm_scalar_bits(const vm_value *v) {
  return v->scalar;
}

// Store a 64-bit bit pattern into a scalar vm_value for the given kind.
static void vm_set_scalar(vm_value *v, sh_kind k, uint64_t bits) {
  v->kind   = k;
  v->lanes  = 0;
  v->scalar = bits;
}

// Scalar binop on two bit patterns; result is a bit pattern. Mirrors
// scalar_binop_bits in sh_interp.c exactly.
static uint64_t scalar_binop_bits(sh_binop op, sh_kind k,
                                   uint64_t a, uint64_t b) {
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
        case SH_BIN_SADD: rr = (rb > (uint8_t)(0xFFu - ra)) ? 0xFFu : (uint8_t)(ra + rb); break;
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
        case SH_BIN_SADD: return (rb > (UINT64_MAX - ra)) ? UINT64_MAX : (ra + rb);
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
        // sat+ i64: detect overflow without signed-overflow UB.
        case SH_BIN_SADD: {
          uint64_t ua = (uint64_t)ra, ub = (uint64_t)rb;
          uint64_t ur = ua + ub;
          int64_t  r  = (int64_t)ur;
          if (!((ra ^ rb) < 0) && ((ra ^ r) < 0))
            return (ra < 0) ? (uint64_t)INT64_MIN : (uint64_t)INT64_MAX;
          return ur;
        }
        // sat- i64: detect underflow without signed-overflow UB.
        case SH_BIN_SSUB: {
          uint64_t ua = (uint64_t)ra, ub = (uint64_t)rb;
          uint64_t ur = ua - ub;
          int64_t  r  = (int64_t)ur;
          if (((ra ^ rb) < 0) && ((ra ^ r) < 0))
            return (ra < 0) ? (uint64_t)INT64_MIN : (uint64_t)INT64_MAX;
          return ur;
        }
        default: return 0;
      }
    }
    default:
      return 0;
  }
}

// Scalar comparison on two bit patterns; returns 0 or 1. Mirrors sh_interp.c.
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
  // Unsigned comparison for all other kinds + bool.
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

// Read one lane bit-pattern from a packed vec[] at the given element size.
static uint64_t vec_lane_get(const vm_value *v, uint8_t li) {
  sh_kind lk   = (sh_kind)v->lane_kind;
  uint32_t esz = sh_kind_size(lk);
  if (esz == 0) esz = 1;
  uint64_t bits = 0;
  memcpy(&bits, &v->vec[li * esz], esz);
  return bits;
}

// Write one lane bit-pattern into a packed vec[] at the given element size.
static void vec_lane_set(vm_value *v, uint8_t li, uint64_t bits) {
  sh_kind lk   = (sh_kind)v->lane_kind;
  uint32_t esz = sh_kind_size(lk);
  if (esz == 0) esz = 1;
  memcpy(&v->vec[li * esz], &bits, esz);
}

// ---------------------------------------------------------------------------
// sh_chunk_validate
// ---------------------------------------------------------------------------

sh_status sh_chunk_validate(const sh_chunk *c, sh_error *err) {
  if (!c)
    return sh_set_error(err, SH_ERR_INTERNAL, -1, -1,
                        "validate: NULL chunk");

  // chunk->result must be a valid vreg (or SH_VREG_NONE if a RET is used).
  if (c->result != SH_VREG_NONE && c->result >= c->nvregs)
    return sh_set_error(err, SH_ERR_INTERNAL, -1, -1,
                        "validate: result vreg %u >= nvregs %u",
                        c->result, c->nvregs);

  for (uint32_t i = 0; i < c->ncode; i++) {
    const sh_instr *ins = &c->code[i];
    sh_bc_op op = (sh_bc_op)ins->op;

    // Check dst/a/b/c when not SH_VREG_NONE (bounds only; per-opcode required
    // checks follow below, using CHK_REQ_VREG for operands that must be valid).
#define CHK_VREG(field)                                                       \
    if (ins->field != SH_VREG_NONE && ins->field >= c->nvregs)               \
      return sh_set_error(err, SH_ERR_INTERNAL, -1, -1,                      \
                          "validate: pc=%u op=%u: " #field                    \
                          "=%u >= nvregs=%u",                                 \
                          i, ins->op, ins->field, c->nvregs)
    CHK_VREG(dst);
    CHK_VREG(a);
    CHK_VREG(b);
    CHK_VREG(c);
#undef CHK_VREG

    // Per-opcode: required operand vregs must not be SH_VREG_NONE.
    // The VM dereferences these unconditionally; SH_VREG_NONE (0xFFFFFFFF) would
    // produce a wild OOB access even though CHK_VREG above passed (it skips NONE).
#define CHK_REQ_VREG(field)                                                   \
    if (ins->field == SH_VREG_NONE)                                           \
      return sh_set_error(err, SH_ERR_INTERNAL, -1, -1,                      \
                          "validate: pc=%u op=%u: required operand " #field   \
                          " is SH_VREG_NONE",                                 \
                          i, ins->op)
    switch (op) {
      // Scalar arithmetic / compare: read a and b.
      case SHB_BINOP: CHK_REQ_VREG(a); CHK_REQ_VREG(b); break;
      case SHB_CMP:   CHK_REQ_VREG(a); CHK_REQ_VREG(b); break;
      // Unary: read a.
      case SHB_UNOP:  CHK_REQ_VREG(a); break;
      // MOV: read a.
      case SHB_MOV:   CHK_REQ_VREG(a); break;
      // Region ops: RLOAD reads a (region) and b (index).
      case SHB_RLOAD: CHK_REQ_VREG(a); CHK_REQ_VREG(b); break;
      // RSTORE: reads a (region), b (index), c (value).
      case SHB_RSTORE: CHK_REQ_VREG(a); CHK_REQ_VREG(b); CHK_REQ_VREG(c); break;
      // RLEN: reads a (region).
      case SHB_RLEN: CHK_REQ_VREG(a); break;
      // JMP_IFNOT: reads a (condition).
      case SHB_JMP_IFNOT: CHK_REQ_VREG(a); break;
      // RET: reads a (return value) when present; a == SH_VREG_NONE is allowed
      // (bare ret with no value, result comes from chunk->result).
      case SHB_RET: break;
      // Vector ops: VSPLAT reads a (scalar to broadcast).
      case SHB_VSPLAT: CHK_REQ_VREG(a); break;
      // VBINOP: reads a and b.
      case SHB_VBINOP: CHK_REQ_VREG(a); CHK_REQ_VREG(b); break;
      // VCMP: reads a and b.
      case SHB_VCMP: CHK_REQ_VREG(a); CHK_REQ_VREG(b); break;
      // VSELECT: reads a (mask), b (then), c (else).
      case SHB_VSELECT: CHK_REQ_VREG(a); CHK_REQ_VREG(b); CHK_REQ_VREG(c); break;
      // VSHUFFLE: reads a (source vector).
      case SHB_VSHUFFLE: CHK_REQ_VREG(a); break;
      // VREDUCE: reads a; DOT also reads b.
      case SHB_VREDUCE:
        CHK_REQ_VREG(a);
        if ((sh_reduce)ins->sub == SH_RED_DOT) CHK_REQ_VREG(b);
        break;
      // VLANE: reads a (source vector).
      case SHB_VLANE: CHK_REQ_VREG(a); break;
      // VRLOAD/VRSTORE: vector region ops.
      case SHB_VRLOAD:  CHK_REQ_VREG(a); CHK_REQ_VREG(b); break;
      case SHB_VRSTORE: CHK_REQ_VREG(a); CHK_REQ_VREG(b); CHK_REQ_VREG(c); break;
      // CONST / PARAM / JMP / CALL: no required a/b/c vreg reads in the vm loop.
      default: break;
    }
#undef CHK_REQ_VREG

    // Finding 2: reject out-of-range lane counts to prevent scalar-loop OOB.
    // ins->lanes is uint8_t (max 255); SH_MAX_LANES is the safe upper bound.
    if (ins->lanes > SH_MAX_LANES)
      return sh_set_error(err, SH_ERR_INTERNAL, -1, -1,
                          "validate: pc=%u op=%u: lanes=%u > SH_MAX_LANES=%u",
                          i, ins->op, (unsigned)ins->lanes,
                          (unsigned)SH_MAX_LANES);
    if ((op == SHB_VRLOAD || op == SHB_VRSTORE) && ins->lanes < 2)
      return sh_set_error(err, SH_ERR_INTERNAL, -1, -1,
                          "validate: pc=%u op=%u: lanes=%u < 2",
                          i, ins->op, (unsigned)ins->lanes);

    // Finding 5: PARAM.imm must be a valid parameter index.
    if (op == SHB_PARAM && ins->imm >= c->nparams)
      return sh_set_error(err, SH_ERR_INTERNAL, -1, -1,
                          "validate: pc=%u PARAM: imm=%u >= nparams=%u",
                          i, ins->imm, c->nparams);

    // Jump targets: SHB_JMP/JMP_IFNOT imm is a pc index.
    // Targets equal to ncode are "past the end" and are valid (the VM exits).
    if (op == SHB_JMP || op == SHB_JMP_IFNOT) {
      if (ins->imm > c->ncode)
        return sh_set_error(err, SH_ERR_INTERNAL, -1, -1,
                            "validate: pc=%u: jump target %u > ncode %u",
                            i, ins->imm, c->ncode);
    }

    // Aux ranges.
    if (ins->aux_len > 0) {
      uint64_t end = (uint64_t)ins->aux_off + (uint64_t)ins->aux_len;
      if (end > (uint64_t)c->naux)
        return sh_set_error(err, SH_ERR_INTERNAL, -1, -1,
                            "validate: pc=%u: aux[%u..+%u) exceeds naux=%u",
                            i, ins->aux_off, ins->aux_len, c->naux);
    }

    // CALL prim index.
    if (op == SHB_CALL) {
      if (!c->prims || ins->imm >= c->prims->count)
        return sh_set_error(err, SH_ERR_INTERNAL, -1, -1,
                            "validate: pc=%u CALL: prim index %u out of range"
                            " (count=%u)",
                            i, ins->imm,
                            c->prims ? c->prims->count : 0u);
    }

    // VSHUFFLE / VLANE: lane indices must be within the vector's lane count.
    // For VSHUFFLE: aux entries are constant indices into the SOURCE vector.
    // instr.lanes = result lane count; we need the SOURCE lane count.
    // The source is vreg a; we can only check that shuffle indices are < lanes
    // of the source at validate time if we know them. Since the lowerer always
    // emits the source with exactly instr.lanes lanes (or more), and the verifier
    // already checked this, we just confirm aux indices are < SH_MAX_LANES as
    // a safe upper bound. The VM runtime loop checks per-access.
    if (op == SHB_VSHUFFLE) {
      for (uint32_t ai = ins->aux_off; ai < ins->aux_off + ins->aux_len; ai++) {
        if (c->aux[ai] >= SH_MAX_LANES)
          return sh_set_error(err, SH_ERR_INTERNAL, -1, -1,
                              "validate: pc=%u VSHUFFLE: lane index %u >= "
                              "SH_MAX_LANES",
                              i, c->aux[ai]);
      }
    }
    if (op == SHB_VLANE) {
      if (ins->imm >= SH_MAX_LANES)
        return sh_set_error(err, SH_ERR_INTERNAL, -1, -1,
                            "validate: pc=%u VLANE: imm=%u >= SH_MAX_LANES",
                            i, ins->imm);
    }

    // CALL aux: entries are vreg indices; validate them.
    if (op == SHB_CALL && ins->aux_len > 0) {
      for (uint32_t ai = ins->aux_off; ai < ins->aux_off + ins->aux_len; ai++) {
        if (c->aux[ai] >= c->nvregs)
          return sh_set_error(err, SH_ERR_INTERNAL, -1, -1,
                              "validate: pc=%u CALL aux[%u]=%u >= nvregs=%u",
                              i, ai, c->aux[ai], c->nvregs);
      }
    }
  }

  return SH_OK;
}

// ---------------------------------------------------------------------------
// sh_vm_run  (scalar execution; SH_VM_FORCE_SCALAR wired but no-op for now)
// ---------------------------------------------------------------------------

sh_status sh_vm_run(const sh_chunk *c, const sh_value *args, uint32_t argc,
                    uint32_t flags, sh_value *out, sh_error *err) {
  const int force_scalar = (flags & SH_VM_FORCE_SCALAR) ? 1 : 0;
  (void)force_scalar;  // suppressed when no SIMD compiled in

  if (!c || !out)
    return sh_set_error(err, SH_ERR_INTERNAL, -1, -1,
                        "vm_run: NULL chunk or out pointer");

  // Validate argc.
  if (argc != c->nparams)
    return sh_set_error(err, SH_ERR_ARITY, -1, -1,
                        "vm_run: shader expects %u args, got %u",
                        c->nparams, argc);

  // Validate arg kinds match declared param kinds.
  for (uint32_t i = 0; i < argc; i++) {
    sh_kind ak = args[i].kind;
    sh_kind pk = (sh_kind)c->params[i].kind;
    if (pk != ak)
      return sh_set_error(err, SH_ERR_TYPE, -1, -1,
                          "vm_run: arg %u: expected kind %u, got kind %u",
                          i, (unsigned)pk, (unsigned)ak);
  }

  // Re-validate the chunk (defense in depth; cheap, total).
  {
    sh_status vs = sh_chunk_validate(c, err);
    if (vs != SH_OK) return vs;
  }

  // Allocate the slot file, 16-byte aligned. vm_value declares _Alignas(16) so the
  // vector ops can use aligned SSE moves (movdqa/movaps) on `.vec`; but a freestanding
  // allocator only guarantees 8-byte alignment (the kernel's malloc rounds to 8), and
  // an aligned move on an 8-aligned slot #GPs. So over-allocate and align the base by
  // hand, keeping the raw pointer to free. sizeof(vm_value) is a multiple of 16, so
  // every slot stays aligned. (Host malloc is already 16-aligned; harmless there.)
  void *slots_raw = NULL;
  vm_value *slots = NULL;
  if (c->nvregs > 0) {
    slots_raw = calloc((size_t)c->nvregs * sizeof(vm_value) + 15u, 1);
    if (!slots_raw)
      return sh_set_error(err, SH_ERR_OOM, -1, -1,
                          "vm_run: OOM allocating slot file (%u vregs)",
                          c->nvregs);
    slots = (vm_value *)(((uintptr_t)slots_raw + 15u) & ~(uintptr_t)15u);
  }

  sh_status status = SH_OK;
  bool did_ret = false;
  vm_value ret_val;
  memset(&ret_val, 0, sizeof(ret_val));

  // Main execution loop.
  uint32_t pc = 0;
  while (pc < c->ncode) {
    const sh_instr *ins = &c->code[pc];
    sh_bc_op op = (sh_bc_op)ins->op;
    pc++;  // advance past this instruction (jumps overwrite pc below)

    switch (op) {

      // -----------------------------------------------------------------------
      // SHB_CONST: dst = literal, decoded via sub flag.
      // sub=0 int, sub=1 float bits, sub=2 bool. Mirrors make_int/float_value.
      // -----------------------------------------------------------------------
      case SHB_CONST: {
        sh_kind k = (sh_kind)ins->kind;
        vm_value v;
        memset(&v, 0, sizeof(v));
        v.kind = k;
        if (ins->sub == 2) {
          // Bool literal.
          v.scalar = ins->imm64 ? 1u : 0u;
        } else if (ins->sub == 1) {
          // Float literal: imm64 is the double bit pattern.
          uint64_t bits = (uint64_t)ins->imm64;
          double d;
          memcpy(&d, &bits, 8);
          if (k == SH_K_F32) {
            float fv = (float)d;
            uint32_t b32;
            memcpy(&b32, &fv, 4);
            v.scalar = (uint64_t)b32;
          } else {
            memcpy(&v.scalar, &d, 8);
          }
        } else {
          // Integer literal: interpret per kind.
          int64_t imm = ins->imm64;
          switch (k) {
            case SH_K_U8:   v.scalar = (uint64_t)(uint8_t)(int8_t)imm; break;
            case SH_K_U16:  v.scalar = (uint64_t)(uint16_t)(int16_t)imm; break;
            case SH_K_U32:  v.scalar = (uint64_t)(uint32_t)(int32_t)imm; break;
            case SH_K_U64:  v.scalar = (uint64_t)imm; break;
            case SH_K_I64:  v.scalar = (uint64_t)imm; break;
            case SH_K_BOOL: v.scalar = imm ? 1u : 0u; break;
            default:        v.scalar = (uint64_t)imm; break;
          }
        }
        if (ins->dst != SH_VREG_NONE) slots[ins->dst] = v;
        break;
      }

      // -----------------------------------------------------------------------
      // SHB_MOV: dst = a (copy).
      // -----------------------------------------------------------------------
      case SHB_MOV: {
        if (ins->dst != SH_VREG_NONE && ins->a != SH_VREG_NONE)
          slots[ins->dst] = slots[ins->a];
        break;
      }

      // -----------------------------------------------------------------------
      // SHB_PARAM: dst = args[imm].
      // -----------------------------------------------------------------------
      case SHB_PARAM: {
        if (ins->dst != SH_VREG_NONE && ins->imm < argc)
          slots[ins->dst] = from_sh_value(args[ins->imm]);
        break;
      }

      // -----------------------------------------------------------------------
      // SHB_UNOP: dst = unop(a). sub = sh_unop; kind = result/target kind.
      // -----------------------------------------------------------------------
      case SHB_UNOP: {
        if (ins->dst == SH_VREG_NONE || ins->a == SH_VREG_NONE) break;
        vm_value va = slots[ins->a];
        vm_value dst;
        memset(&dst, 0, sizeof(dst));
        dst.kind = (sh_kind)ins->kind;
        sh_unop unop = (sh_unop)ins->sub;

        if (unop == SH_UN_NOT) {
          dst.scalar = (va.scalar == 0) ? 1u : 0u;
          dst.kind   = SH_K_BOOL;
        } else if (unop == SH_UN_NEG) {
          sh_kind k = va.kind;
          if (k == SH_K_F32 || k == SH_K_F64) {
            if (k == SH_K_F32) {
              uint32_t b32 = (uint32_t)va.scalar;
              float fv;
              memcpy(&fv, &b32, 4);
              fv = -fv;
              memcpy(&b32, &fv, 4);
              dst.scalar = (uint64_t)b32;
            } else {
              double d;
              memcpy(&d, &va.scalar, 8);
              d = -d;
              memcpy(&dst.scalar, &d, 8);
            }
          } else if (k == SH_K_I64) {
            int64_t iv = (int64_t)va.scalar;
            dst.scalar = (uint64_t)(-iv);
          } else {
            // Unsigned types: -(uint)x, masked to width.
            uint64_t mask;
            switch (k) {
              case SH_K_U8:  mask = 0xFFu; break;
              case SH_K_U16: mask = 0xFFFFu; break;
              case SH_K_U32: mask = 0xFFFFFFFFu; break;
              default:       mask = UINT64_MAX; break;
            }
            dst.scalar = (-(va.scalar)) & mask;
          }
          dst.kind = k;
        } else if (unop == SH_UN_CVT) {
          // CVT: convert va to target kind (ins->kind).
          sh_kind sk  = va.kind;
          sh_kind dk  = (sh_kind)ins->kind;
          uint64_t vbits = va.scalar;

          if (sh_kind_is_float(sk)) {
            double df;
            if (sk == SH_K_F32) {
              uint32_t b32 = (uint32_t)vbits;
              float fv;
              memcpy(&fv, &b32, 4);
              df = (double)fv;
            } else {
              memcpy(&df, &vbits, 8);
            }
            switch (dk) {
              case SH_K_U8:   dst.scalar = (uint64_t)(uint8_t)df; break;
              case SH_K_U16:  dst.scalar = (uint64_t)(uint16_t)df; break;
              case SH_K_U32:  dst.scalar = (uint64_t)(uint32_t)df; break;
              case SH_K_U64:  dst.scalar = (uint64_t)df; break;
              case SH_K_I64:  dst.scalar = (uint64_t)(int64_t)df; break;
              case SH_K_BOOL: dst.scalar = df != 0.0 ? 1u : 0u; break;
              case SH_K_F32: {
                float fres = (float)df;
                uint32_t r32;
                memcpy(&r32, &fres, 4);
                dst.scalar = (uint64_t)r32;
                break;
              }
              case SH_K_F64:
                memcpy(&dst.scalar, &df, 8); break;
              default: break;
            }
          } else {
            // Integer/bool source.
            int64_t si = (sk == SH_K_I64) ? (int64_t)vbits : (int64_t)vbits;
            uint64_t ui = vbits;
            // Signed interpretation for i64, unsigned otherwise.
            if (sk != SH_K_I64) si = (int64_t)ui;
            switch (dk) {
              case SH_K_U8:   dst.scalar = (uint64_t)(uint8_t)ui; break;
              case SH_K_U16:  dst.scalar = (uint64_t)(uint16_t)ui; break;
              case SH_K_U32:  dst.scalar = (uint64_t)(uint32_t)ui; break;
              case SH_K_U64:  dst.scalar = ui; break;
              case SH_K_I64:  dst.scalar = (uint64_t)si; break;
              case SH_K_BOOL: dst.scalar = ui ? 1u : 0u; break;
              case SH_K_F32: {
                double df;
                if (sk == SH_K_I64) {
                  df = (double)(float)(double)si;
                } else {
                  df = (double)(float)(double)ui;
                }
                float fres = (float)df;
                uint32_t r32;
                memcpy(&r32, &fres, 4);
                dst.scalar = (uint64_t)r32;
                break;
              }
              case SH_K_F64:
                if (sk == SH_K_I64) {
                  double df = (double)si;
                  memcpy(&dst.scalar, &df, 8);
                } else {
                  double df = (double)ui;
                  memcpy(&dst.scalar, &df, 8);
                }
                break;
              default: break;
            }
          }
          dst.kind = dk;
        }
        slots[ins->dst] = dst;
        break;
      }

      // -----------------------------------------------------------------------
      // SHB_BINOP: dst = binop(a, b). kind = operand/result scalar kind.
      // -----------------------------------------------------------------------
      case SHB_BINOP: {
        if (ins->dst == SH_VREG_NONE) break;
        sh_kind k    = (sh_kind)ins->kind;
        uint64_t ab  = vm_scalar_bits(&slots[ins->a]);
        uint64_t bb  = vm_scalar_bits(&slots[ins->b]);
        uint64_t res = scalar_binop_bits((sh_binop)ins->sub, k, ab, bb);
        vm_set_scalar(&slots[ins->dst], k, res);
        break;
      }

      // -----------------------------------------------------------------------
      // SHB_CMP: dst = cmp(a, b) -> bool. kind = OPERAND kind.
      // -----------------------------------------------------------------------
      case SHB_CMP: {
        if (ins->dst == SH_VREG_NONE) break;
        sh_kind k   = (sh_kind)ins->kind;
        uint64_t ab = vm_scalar_bits(&slots[ins->a]);
        uint64_t bb = vm_scalar_bits(&slots[ins->b]);
        uint64_t r  = scalar_cmp_bits((sh_cmp)ins->sub, k, ab, bb);
        vm_set_scalar(&slots[ins->dst], SH_K_BOOL, r);
        break;
      }

      // -----------------------------------------------------------------------
      // SHB_RLOAD: dst = region[b] of region-vreg a. kind = elem kind.
      // -----------------------------------------------------------------------
      case SHB_RLOAD: {
        if (ins->dst == SH_VREG_NONE) break;
        vm_value *va  = &slots[ins->a];
        vm_value *vb  = &slots[ins->b];
        sh_kind   ek  = va->region.elem;
        uint64_t  idx = vb->scalar;
        if (vb->kind == SH_K_I64) idx = (uint64_t)(int64_t)vb->scalar;

        if (idx >= (uint64_t)va->region.len) {
          status = sh_set_error(err, SH_ERR_BOUNDS, -1, -1,
                                "vm: region load: index %llu >= len %u",
                                (unsigned long long)idx, va->region.len);
          goto done;
        }
        uint32_t esz = sh_kind_size(ek);
        const uint8_t *ptr = va->region.base + idx * esz;
        vm_value dst;
        memset(&dst, 0, sizeof(dst));
        dst.kind = (sh_kind)ins->kind;
        switch (ek) {
          case SH_K_BOOL:
          case SH_K_U8:  dst.scalar = *ptr; break;
          case SH_K_U16: { uint16_t v16; memcpy(&v16, ptr, 2); dst.scalar = v16; break; }
          case SH_K_U32: { uint32_t v32; memcpy(&v32, ptr, 4); dst.scalar = v32; break; }
          case SH_K_U64: { uint64_t v64; memcpy(&v64, ptr, 8); dst.scalar = v64; break; }
          case SH_K_I64: { int64_t  v64; memcpy(&v64, ptr, 8); dst.scalar = (uint64_t)v64; break; }
          case SH_K_F32: {
            float fv;
            memcpy(&fv, ptr, 4);
            // Match interp: store as f32 bit pattern (f holds narrowed double there,
            // but internally we store the f32 bits).
            uint32_t b32;
            memcpy(&b32, &fv, 4);
            dst.scalar = (uint64_t)b32;
            break;
          }
          case SH_K_F64: { double dv; memcpy(&dv, ptr, 8);
                           memcpy(&dst.scalar, &dv, 8); break; }
          default: break;
        }
        slots[ins->dst] = dst;
        break;
      }

      // -----------------------------------------------------------------------
      // SHB_RSTORE: region[b] = c. kind = elem kind. dst = c (stored value).
      // -----------------------------------------------------------------------
      case SHB_RSTORE: {
        vm_value *va  = &slots[ins->a];
        vm_value *vb  = &slots[ins->b];
        vm_value *vc  = &slots[ins->c];
        sh_kind   ek  = va->region.elem;
        uint64_t  idx = vb->scalar;
        if (vb->kind == SH_K_I64) idx = (uint64_t)(int64_t)vb->scalar;

        if (idx >= (uint64_t)va->region.len) {
          status = sh_set_error(err, SH_ERR_BOUNDS, -1, -1,
                                "vm: region store: index %llu >= len %u",
                                (unsigned long long)idx, va->region.len);
          goto done;
        }
        uint32_t esz  = sh_kind_size(ek);
        uint8_t *ptr  = va->region.base + idx * esz;
        uint64_t bits = vc->scalar;
        switch (ek) {
          case SH_K_BOOL:
          case SH_K_U8:  { uint8_t  v8  = (uint8_t)bits;  memcpy(ptr, &v8,  1); break; }
          case SH_K_U16: { uint16_t v16 = (uint16_t)bits; memcpy(ptr, &v16, 2); break; }
          case SH_K_U32: { uint32_t v32 = (uint32_t)bits; memcpy(ptr, &v32, 4); break; }
          case SH_K_U64: { memcpy(ptr, &bits, 8); break; }
          case SH_K_I64: { memcpy(ptr, &bits, 8); break; }
          case SH_K_F32: {
            // vc->scalar holds f32 bit pattern.
            uint32_t b32 = (uint32_t)bits;
            memcpy(ptr, &b32, 4);
            break;
          }
          case SH_K_F64: { memcpy(ptr, &bits, 8); break; }
          default: break;
        }
        // Result = stored value (elem-typed). Mirror interp: dst gets vc's kind
        // set to ins->kind. We copy the vm_value and adjust kind.
        if (ins->dst != SH_VREG_NONE) {
          vm_value res = *vc;
          res.kind = (sh_kind)ins->kind;
          slots[ins->dst] = res;
        }
        break;
      }

      // -----------------------------------------------------------------------
      // SHB_RLEN: dst = len(region a) -> u32.
      // -----------------------------------------------------------------------
      case SHB_RLEN: {
        if (ins->dst == SH_VREG_NONE) break;
        uint32_t len = slots[ins->a].region.len;
        vm_set_scalar(&slots[ins->dst], SH_K_U32, (uint64_t)len);
        break;
      }

      // -----------------------------------------------------------------------
      // SHB_CALL: dst = prims[imm](args from aux vregs).
      // -----------------------------------------------------------------------
      case SHB_CALL: {
        const sh_prim *prim = &c->prims->prims[ins->imm];
        uint32_t nargs = ins->aux_len;
        sh_value call_args[SH_MAX_PRIM_PARAMS];
        uint32_t limit = nargs < SH_MAX_PRIM_PARAMS ? nargs : SH_MAX_PRIM_PARAMS;
        for (uint32_t ai = 0; ai < limit; ai++) {
          uint32_t vr = c->aux[ins->aux_off + ai];
          call_args[ai] = to_sh_value(slots[vr]);
        }
        if (!prim->fn) {
          status = sh_set_error(err, SH_ERR_INTERNAL, -1, -1,
                                "vm: CALL: prim '%s' has null fn", prim->name);
          goto done;
        }
        sh_value result = prim->fn(call_args, limit);
        if (ins->dst != SH_VREG_NONE)
          slots[ins->dst] = from_sh_value(result);
        break;
      }

      // -----------------------------------------------------------------------
      // SHB_JMP: pc = imm (unconditional; the loop back-edge).
      // -----------------------------------------------------------------------
      case SHB_JMP: {
        pc = ins->imm;
        break;
      }

      // -----------------------------------------------------------------------
      // SHB_JMP_IFNOT: if a is false: pc = imm.
      // -----------------------------------------------------------------------
      case SHB_JMP_IFNOT: {
        if (slots[ins->a].scalar == 0)
          pc = ins->imm;
        break;
      }

      // -----------------------------------------------------------------------
      // SHB_RET: return a.
      // -----------------------------------------------------------------------
      case SHB_RET: {
        if (ins->a != SH_VREG_NONE) {
          ret_val  = slots[ins->a];
          did_ret  = true;
        }
        pc = c->ncode;  // force exit
        break;
      }

      // -----------------------------------------------------------------------
      // SHB_VSPLAT: dst = broadcast scalar a over `lanes` of `kind`.
      // SSE/AVX path: use _mm_set1_* for the common fixed widths.
      // -----------------------------------------------------------------------
      case SHB_VSPLAT: {
        if (ins->dst == SH_VREG_NONE) break;
        sh_kind lk    = (sh_kind)ins->kind;
        uint8_t nlanes = ins->lanes;
        uint64_t bits  = vm_scalar_bits(&slots[ins->a]);
        vm_value dst;
        memset(&dst, 0, sizeof(dst));
        dst.kind      = SH_K_VEC;
        dst.lanes     = nlanes;
        dst.lane_kind = (uint8_t)lk;

#if defined(__SSE2__)
        if (!force_scalar) {
          if (lk == SH_K_F32 && nlanes == 4) {
            uint32_t b32 = (uint32_t)bits;
            float fv;
            memcpy(&fv, &b32, 4);
            __m128 r = _mm_set1_ps(fv);
            _mm_store_ps((float *)dst.vec, r);
            slots[ins->dst] = dst;
            break;
          }
          if (lk == SH_K_U32 && nlanes == 4) {
            // Store 4 x 32-bit lanes.
            __m128i r = _mm_set1_epi32((int)(uint32_t)bits);
            _mm_store_si128((__m128i *)dst.vec, r);
            slots[ins->dst] = dst;
            break;
          }
          // NOTE: i64 vectors (any width) fall through to the scalar lane loop
          // below.  _mm_set1_epi32 is 4-byte and would silently truncate the
          // high 32 bits of each i64 lane; _mm_set1_epi64x exists but mixing
          // the 4-lane SIMD path for 8-byte lanes is layout-incompatible with
          // the packed vec[] stride.  Scalar is correct and cheap enough.
          if (lk == SH_K_U8 && nlanes == 16) {
            __m128i r = _mm_set1_epi8((char)(uint8_t)bits);
            _mm_store_si128((__m128i *)dst.vec, r);
            slots[ins->dst] = dst;
            break;
          }
          if (lk == SH_K_U16 && nlanes == 8) {
            __m128i r = _mm_set1_epi16((short)(uint16_t)bits);
            _mm_store_si128((__m128i *)dst.vec, r);
            slots[ins->dst] = dst;
            break;
          }
#if defined(__AVX2__)
          if (lk == SH_K_F32 && nlanes == 8) {
            uint32_t b32 = (uint32_t)bits;
            float fv;
            memcpy(&fv, &b32, 4);
            __m256 r = _mm256_set1_ps(fv);
            _mm256_store_ps((float *)dst.vec, r);
            slots[ins->dst] = dst;
            break;
          }
          if (lk == SH_K_U32 && nlanes == 8) {
            __m256i r = _mm256_set1_epi32((int)(uint32_t)bits);
            _mm256_store_si256((__m256i *)dst.vec, r);
            slots[ins->dst] = dst;
            break;
          }
#endif  // __AVX2__
        }
#endif  // __SSE2__

        // Scalar fallback (always-correct path; also used for force_scalar).
        for (uint8_t li = 0; li < nlanes; li++)
          vec_lane_set(&dst, li, bits);
        slots[ins->dst] = dst;
        break;
      }

      // -----------------------------------------------------------------------
      // SHB_VBINOP: dst = lane-wise binop(a, b). sub = sh_binop; kind = lane kind.
      // SSE/AVX path: f32x4 add/sub/mul/div; integer add/sub for 8/16/32-bit
      // lanes; 32-bit mul (SSE4.1).  Integer div/mod always scalar (no hw div).
      // -----------------------------------------------------------------------
      case SHB_VBINOP: {
        if (ins->dst == SH_VREG_NONE) break;
        sh_kind lk    = (sh_kind)ins->kind;
        uint8_t nl    = ins->lanes;
        sh_binop bop  = (sh_binop)ins->sub;
        vm_value *va  = &slots[ins->a];
        vm_value *vb  = &slots[ins->b];
        vm_value dst;
        memset(&dst, 0, sizeof(dst));
        dst.kind      = SH_K_VEC;
        dst.lanes     = nl;
        dst.lane_kind = (uint8_t)lk;

#if defined(__SSE2__)
        if (!force_scalar) {
          // --- f32x4 add/sub/mul ---
          // NOTE: SH_BIN_DIV is EXCLUDED from the SIMD path. The oracle returns
          // 0.0 for division-by-zero (it checks fb != 0.0), but _mm_div_ps
          // produces NaN for 0/0 and ±Inf for nonzero/0 -- not bit-equal.
          // Div stays on the scalar fallback (same result, correct by contract).
          if (lk == SH_K_F32 && nl == 4 &&
              (bop == SH_BIN_ADD || bop == SH_BIN_SUB || bop == SH_BIN_MUL)) {
            __m128 ra = _mm_load_ps((const float *)va->vec);
            __m128 rb = _mm_load_ps((const float *)vb->vec);
            __m128 rr;
            switch (bop) {
              case SH_BIN_ADD: rr = _mm_add_ps(ra, rb); break;
              case SH_BIN_SUB: rr = _mm_sub_ps(ra, rb); break;
              case SH_BIN_MUL: rr = _mm_mul_ps(ra, rb); break;
              default: rr = _mm_setzero_ps(); break;
            }
            _mm_store_ps((float *)dst.vec, rr);
            slots[ins->dst] = dst;
            break;
          }
          // --- u32x4 add/sub ---
          // NOTE: i64 is intentionally excluded. _mm_add_epi32/_mm_sub_epi32
          // operate on 4 x 32-bit lanes; i64 lanes are 8 bytes each and would
          // be silently truncated.  i64 vectors fall through to the scalar loop.
          if (lk == SH_K_U32 && nl == 4 &&
              (bop == SH_BIN_ADD || bop == SH_BIN_SUB)) {
            __m128i ra = _mm_load_si128((const __m128i *)va->vec);
            __m128i rb = _mm_load_si128((const __m128i *)vb->vec);
            __m128i rr = (bop == SH_BIN_ADD)
                         ? _mm_add_epi32(ra, rb)
                         : _mm_sub_epi32(ra, rb);
            _mm_store_si128((__m128i *)dst.vec, rr);
            slots[ins->dst] = dst;
            break;
          }
          // --- u32x4 mul (SSE4.1 mullo_epi32) ---
#if defined(__SSE4_1__)
          if (lk == SH_K_U32 && nl == 4 && bop == SH_BIN_MUL) {
            __m128i ra = _mm_load_si128((const __m128i *)va->vec);
            __m128i rb = _mm_load_si128((const __m128i *)vb->vec);
            __m128i rr = _mm_mullo_epi32(ra, rb);
            _mm_store_si128((__m128i *)dst.vec, rr);
            slots[ins->dst] = dst;
            break;
          }
#endif
          // --- u8x16 add/sub (wrapping, not saturating -- matches scalar) ---
          if (lk == SH_K_U8 && nl == 16 &&
              (bop == SH_BIN_ADD || bop == SH_BIN_SUB)) {
            __m128i ra = _mm_load_si128((const __m128i *)va->vec);
            __m128i rb = _mm_load_si128((const __m128i *)vb->vec);
            // _mm_add_epi8 wraps (modular), matching scalar u8 add.
            __m128i rr = (bop == SH_BIN_ADD)
                         ? _mm_add_epi8(ra, rb)
                         : _mm_sub_epi8(ra, rb);
            _mm_store_si128((__m128i *)dst.vec, rr);
            slots[ins->dst] = dst;
            break;
          }
          // --- u8x16 saturating add/sub: _mm_adds_epu8 / _mm_subs_epu8 ---
          if (lk == SH_K_U8 && nl == 16 &&
              (bop == SH_BIN_SADD || bop == SH_BIN_SSUB)) {
            __m128i ra = _mm_load_si128((const __m128i *)va->vec);
            __m128i rb = _mm_load_si128((const __m128i *)vb->vec);
            __m128i rr = (bop == SH_BIN_SADD)
                         ? _mm_adds_epu8(ra, rb)
                         : _mm_subs_epu8(ra, rb);
            _mm_store_si128((__m128i *)dst.vec, rr);
            slots[ins->dst] = dst;
            break;
          }
          // --- u16x8 add/sub ---
          if (lk == SH_K_U16 && nl == 8 &&
              (bop == SH_BIN_ADD || bop == SH_BIN_SUB)) {
            __m128i ra = _mm_load_si128((const __m128i *)va->vec);
            __m128i rb = _mm_load_si128((const __m128i *)vb->vec);
            __m128i rr = (bop == SH_BIN_ADD)
                         ? _mm_add_epi16(ra, rb)
                         : _mm_sub_epi16(ra, rb);
            _mm_store_si128((__m128i *)dst.vec, rr);
            slots[ins->dst] = dst;
            break;
          }
          // --- u16x8 saturating add/sub: _mm_adds_epu16 / _mm_subs_epu16 ---
          if (lk == SH_K_U16 && nl == 8 &&
              (bop == SH_BIN_SADD || bop == SH_BIN_SSUB)) {
            __m128i ra = _mm_load_si128((const __m128i *)va->vec);
            __m128i rb = _mm_load_si128((const __m128i *)vb->vec);
            __m128i rr = (bop == SH_BIN_SADD)
                         ? _mm_adds_epu16(ra, rb)
                         : _mm_subs_epu16(ra, rb);
            _mm_store_si128((__m128i *)dst.vec, rr);
            slots[ins->dst] = dst;
            break;
          }
#if defined(__AVX2__)
          // --- f32x8 add/sub/mul (div excluded: see f32x4 comment above) ---
          if (lk == SH_K_F32 && nl == 8 &&
              (bop == SH_BIN_ADD || bop == SH_BIN_SUB || bop == SH_BIN_MUL)) {
            __m256 ra = _mm256_load_ps((const float *)va->vec);
            __m256 rb = _mm256_load_ps((const float *)vb->vec);
            __m256 rr;
            switch (bop) {
              case SH_BIN_ADD: rr = _mm256_add_ps(ra, rb); break;
              case SH_BIN_SUB: rr = _mm256_sub_ps(ra, rb); break;
              case SH_BIN_MUL: rr = _mm256_mul_ps(ra, rb); break;
              default: rr = _mm256_setzero_ps(); break;
            }
            _mm256_store_ps((float *)dst.vec, rr);
            slots[ins->dst] = dst;
            break;
          }
          // --- u32x8 add/sub ---
          if (lk == SH_K_U32 && nl == 8 &&
              (bop == SH_BIN_ADD || bop == SH_BIN_SUB)) {
            __m256i ra = _mm256_load_si256((const __m256i *)va->vec);
            __m256i rb = _mm256_load_si256((const __m256i *)vb->vec);
            __m256i rr = (bop == SH_BIN_ADD)
                         ? _mm256_add_epi32(ra, rb)
                         : _mm256_sub_epi32(ra, rb);
            _mm256_store_si256((__m256i *)dst.vec, rr);
            slots[ins->dst] = dst;
            break;
          }
          // --- u32x8 mul ---
          if (lk == SH_K_U32 && nl == 8 && bop == SH_BIN_MUL) {
            __m256i ra = _mm256_load_si256((const __m256i *)va->vec);
            __m256i rb = _mm256_load_si256((const __m256i *)vb->vec);
            __m256i rr = _mm256_mullo_epi32(ra, rb);
            _mm256_store_si256((__m256i *)dst.vec, rr);
            slots[ins->dst] = dst;
            break;
          }
#endif  // __AVX2__
        }
#endif  // __SSE2__

        // Scalar fallback (always-correct; also for force_scalar, div/mod, odd widths).
        for (uint8_t li = 0; li < nl; li++) {
          uint64_t ab  = vec_lane_get(va, li);
          uint64_t bb  = vec_lane_get(vb, li);
          uint64_t res = scalar_binop_bits(bop, lk, ab, bb);
          vec_lane_set(&dst, li, res);
        }
        slots[ins->dst] = dst;
        break;
      }

      // -----------------------------------------------------------------------
      // SHB_VCMP: dst = lane-wise cmp(a, b) -> bool-mask. kind = operand lane kind.
      // SSE/AVX path: f32x4 all six predicates; u32x4 eq/lt/gt; u8x16 eq.
      // Result MUST be 0 or 1 per lane (bool lane_kind), same as oracle.
      // We expand the all-ones/-zeros mask to 0/1 bytes in the bool vec[].
      // -----------------------------------------------------------------------
      case SHB_VCMP: {
        if (ins->dst == SH_VREG_NONE) break;
        sh_kind lk    = (sh_kind)ins->kind;
        uint8_t  nl   = ins->lanes;
        sh_cmp   cmp  = (sh_cmp)ins->sub;
        vm_value *va  = &slots[ins->a];
        vm_value *vb  = &slots[ins->b];
        vm_value dst;
        memset(&dst, 0, sizeof(dst));
        dst.kind      = SH_K_VEC;
        dst.lanes     = nl;
        dst.lane_kind = (uint8_t)SH_K_BOOL;

#if defined(__SSE2__)
        if (!force_scalar) {
          // --- f32x4: use _mm_cmp*_ps, then expand mask to 0/1 uint8 lanes ---
          if (lk == SH_K_F32 && nl == 4) {
            __m128 ra = _mm_load_ps((const float *)va->vec);
            __m128 rb = _mm_load_ps((const float *)vb->vec);
            __m128 rm;
            switch (cmp) {
              case SH_CMP_LT: rm = _mm_cmplt_ps(ra, rb);  break;
              case SH_CMP_LE: rm = _mm_cmple_ps(ra, rb);  break;
              case SH_CMP_EQ: rm = _mm_cmpeq_ps(ra, rb);  break;
              case SH_CMP_NE: rm = _mm_cmpneq_ps(ra, rb); break;
              case SH_CMP_GT: rm = _mm_cmpgt_ps(ra, rb);  break;
              case SH_CMP_GE: rm = _mm_cmpge_ps(ra, rb);  break;
              default: rm = _mm_setzero_ps(); break;
            }
            // movemask gives a 4-bit integer; expand to 4 x uint8 lanes (0/1).
            int msk = _mm_movemask_ps(rm);
            for (int li = 0; li < 4; li++)
              dst.vec[li] = (uint8_t)((msk >> li) & 1);
            slots[ins->dst] = dst;
            break;
          }
          // --- u32x4 eq/lt/gt (SSE2 has no unsigned lt; we handle all via helper) ---
          if (lk == SH_K_U32 && nl == 4 &&
              (cmp == SH_CMP_EQ || cmp == SH_CMP_LT || cmp == SH_CMP_GT ||
               cmp == SH_CMP_LE || cmp == SH_CMP_GE || cmp == SH_CMP_NE)) {
            // SSE2 only has signed 32-bit compare (cmpeq_epi32, cmpgt_epi32).
            // For unsigned comparisons, bias both operands by 0x80000000 so
            // that unsigned order maps to signed order, then use signed cmpgt.
            __m128i ra = _mm_load_si128((const __m128i *)va->vec);
            __m128i rb = _mm_load_si128((const __m128i *)vb->vec);
            __m128i bias = _mm_set1_epi32((int)0x80000000u);
            __m128i ras = _mm_xor_si128(ra, bias);
            __m128i rbs = _mm_xor_si128(rb, bias);
            __m128i rm;
            switch (cmp) {
              case SH_CMP_EQ: rm = _mm_cmpeq_epi32(ra, rb); break;
              case SH_CMP_NE: rm = _mm_andnot_si128(_mm_cmpeq_epi32(ra, rb),
                                                     _mm_set1_epi32(-1)); break;
              case SH_CMP_LT: rm = _mm_cmpgt_epi32(rbs, ras); break;
              case SH_CMP_GT: rm = _mm_cmpgt_epi32(ras, rbs); break;
              case SH_CMP_LE: rm = _mm_andnot_si128(_mm_cmpgt_epi32(ras, rbs),
                                                     _mm_set1_epi32(-1)); break;
              case SH_CMP_GE: rm = _mm_andnot_si128(_mm_cmpgt_epi32(rbs, ras),
                                                     _mm_set1_epi32(-1)); break;
              default: rm = _mm_setzero_si128(); break;
            }
            // movemask on epi8 gives 16 bits; grab the high bit of each 32-bit lane.
            // _mm_movemask_epi8 bit [4*i+3] corresponds to lane i's sign/all-ones.
            int msk = _mm_movemask_epi8(rm);
            for (int li = 0; li < 4; li++)
              dst.vec[li] = (uint8_t)((msk >> (li * 4 + 3)) & 1);
            slots[ins->dst] = dst;
            break;
          }
          // --- u8x16 eq ---
          if (lk == SH_K_U8 && nl == 16 && cmp == SH_CMP_EQ) {
            __m128i ra = _mm_load_si128((const __m128i *)va->vec);
            __m128i rb = _mm_load_si128((const __m128i *)vb->vec);
            __m128i rm = _mm_cmpeq_epi8(ra, rb);
            int msk = _mm_movemask_epi8(rm);
            for (int li = 0; li < 16; li++)
              dst.vec[li] = (uint8_t)((msk >> li) & 1);
            slots[ins->dst] = dst;
            break;
          }
        }
#endif  // __SSE2__

        // Scalar fallback.
        for (uint8_t li = 0; li < nl; li++) {
          uint64_t ab  = vec_lane_get(va, li);
          uint64_t bb  = vec_lane_get(vb, li);
          uint64_t res = scalar_cmp_bits(cmp, lk, ab, bb);
          vec_lane_set(&dst, li, res);
        }
        slots[ins->dst] = dst;
        break;
      }

      // -----------------------------------------------------------------------
      // SHB_VSELECT: dst = lane-wise (mask a) ? b : c. kind = lane kind.
      // SSE/AVX path: convert 0/1 bool lanes to all-zeros/all-ones mask,
      // then blend. Result is the data (lk-typed) lanes.
      // -----------------------------------------------------------------------
      case SHB_VSELECT: {
        if (ins->dst == SH_VREG_NONE) break;
        sh_kind lk    = (sh_kind)ins->kind;
        uint8_t  nl   = ins->lanes;
        vm_value *va  = &slots[ins->a];  // bool mask
        vm_value *vb  = &slots[ins->b];  // then
        vm_value *vc  = &slots[ins->c];  // else
        vm_value dst;
        memset(&dst, 0, sizeof(dst));
        dst.kind      = SH_K_VEC;
        dst.lanes     = nl;
        dst.lane_kind = (uint8_t)lk;

#if defined(__SSE2__)
        if (!force_scalar) {
          // --- f32x4: build movemask from bool vec[], then blend ---
          if (lk == SH_K_F32 && nl == 4) {
            // Build a 4-bit mask from the bool-lane bytes (lane i -> bit i).
            int msk = 0;
            for (int li = 0; li < 4; li++)
              msk |= (va->vec[li] ? 1 : 0) << li;
            __m128 rb = _mm_load_ps((const float *)vb->vec);
            __m128 rc = _mm_load_ps((const float *)vc->vec);
#if defined(__SSE4_1__)
            __m128 rr = _mm_blendv_ps(rc, rb,
                          _mm_castsi128_ps(_mm_set_epi32(
                            (msk >> 3 & 1) ? -1 : 0,
                            (msk >> 2 & 1) ? -1 : 0,
                            (msk >> 1 & 1) ? -1 : 0,
                            (msk >> 0 & 1) ? -1 : 0)));
#else
            // SSE2-only: manual per-lane AND/ANDNOT/OR blend.
            __m128i imsk = _mm_set_epi32(
              (msk >> 3 & 1) ? -1 : 0,
              (msk >> 2 & 1) ? -1 : 0,
              (msk >> 1 & 1) ? -1 : 0,
              (msk >> 0 & 1) ? -1 : 0);
            __m128 fmsk  = _mm_castsi128_ps(imsk);
            __m128 rr    = _mm_or_ps(_mm_and_ps(fmsk, rb),
                                     _mm_andnot_ps(fmsk, rc));
#endif
            _mm_store_ps((float *)dst.vec, rr);
            slots[ins->dst] = dst;
            break;
          }
          // --- u32x4 blend ---
          if (lk == SH_K_U32 && nl == 4) {
            int msk = 0;
            for (int li = 0; li < 4; li++)
              msk |= (va->vec[li] ? 1 : 0) << li;
            __m128i rb = _mm_load_si128((const __m128i *)vb->vec);
            __m128i rc = _mm_load_si128((const __m128i *)vc->vec);
            __m128i imsk = _mm_set_epi32(
              (msk >> 3 & 1) ? -1 : 0,
              (msk >> 2 & 1) ? -1 : 0,
              (msk >> 1 & 1) ? -1 : 0,
              (msk >> 0 & 1) ? -1 : 0);
            __m128i rr = _mm_or_si128(_mm_and_si128(imsk, rb),
                                      _mm_andnot_si128(imsk, rc));
            _mm_store_si128((__m128i *)dst.vec, rr);
            slots[ins->dst] = dst;
            break;
          }
          // --- u8x16 blend ---
          if (lk == SH_K_U8 && nl == 16) {
            __m128i rb = _mm_load_si128((const __m128i *)vb->vec);
            __m128i rc = _mm_load_si128((const __m128i *)vc->vec);
            // Build a byte mask from bool lanes.
            __m128i imsk = _mm_set_epi8(
              (char)(va->vec[15] ? 0xFF : 0), (char)(va->vec[14] ? 0xFF : 0),
              (char)(va->vec[13] ? 0xFF : 0), (char)(va->vec[12] ? 0xFF : 0),
              (char)(va->vec[11] ? 0xFF : 0), (char)(va->vec[10] ? 0xFF : 0),
              (char)(va->vec[ 9] ? 0xFF : 0), (char)(va->vec[ 8] ? 0xFF : 0),
              (char)(va->vec[ 7] ? 0xFF : 0), (char)(va->vec[ 6] ? 0xFF : 0),
              (char)(va->vec[ 5] ? 0xFF : 0), (char)(va->vec[ 4] ? 0xFF : 0),
              (char)(va->vec[ 3] ? 0xFF : 0), (char)(va->vec[ 2] ? 0xFF : 0),
              (char)(va->vec[ 1] ? 0xFF : 0), (char)(va->vec[ 0] ? 0xFF : 0));
            __m128i rr = _mm_or_si128(_mm_and_si128(imsk, rb),
                                      _mm_andnot_si128(imsk, rc));
            _mm_store_si128((__m128i *)dst.vec, rr);
            slots[ins->dst] = dst;
            break;
          }
        }
#endif  // __SSE2__

        // Scalar fallback.
        for (uint8_t li = 0; li < nl; li++) {
          uint64_t mask_bit = vec_lane_get(va, li);
          uint64_t res      = mask_bit ? vec_lane_get(vb, li)
                                       : vec_lane_get(vc, li);
          vec_lane_set(&dst, li, res);
        }
        slots[ins->dst] = dst;
        break;
      }

      // -----------------------------------------------------------------------
      // SHB_VSHUFFLE: dst = { a.lane[aux[k]] }. Constant indices in aux.
      // SSE/AVX path: for f32x4 with 4 result lanes, use _mm_shuffle_ps when
      // the pattern fits (all sources from the same 4-lane vector).
      // For u32x4 use _mm_shuffle_epi32.  All other cases: scalar fallback.
      // -----------------------------------------------------------------------
      case SHB_VSHUFFLE: {
        if (ins->dst == SH_VREG_NONE) break;
        sh_kind lk    = (sh_kind)ins->kind;
        uint8_t  nl   = ins->lanes;  // result lane count == aux_len
        vm_value *va  = &slots[ins->a];
        vm_value dst;
        memset(&dst, 0, sizeof(dst));
        dst.kind      = SH_K_VEC;
        dst.lanes     = nl;
        dst.lane_kind = (uint8_t)lk;

        // VSHUFFLE SIMD path: _mm_shuffle_ps/_mm_shuffle_epi32 require a
        // compile-time constant imm8. Use a macro-generated dispatch table for
        // the 256 possible 4-lane shuffle patterns (indices 0..3 in each slot).
        // We only do this for the common 4-lane cases (f32x4, u32x4).
#if defined(__SSE2__)
        {
        int simd_took = 0;  // set to 1 if we take the SIMD path below
        if (!force_scalar && nl == 4 && ins->aux_len == 4 && va->lanes == 4) {
          uint32_t idx0 = c->aux[ins->aux_off + 0];
          uint32_t idx1 = c->aux[ins->aux_off + 1];
          uint32_t idx2 = c->aux[ins->aux_off + 2];
          uint32_t idx3 = c->aux[ins->aux_off + 3];
          if (idx0 < 4 && idx1 < 4 && idx2 < 4 && idx3 < 4) {
            int ctrl = (int)((idx3 << 6) | (idx2 << 4) | (idx1 << 2) | idx0);
            // _mm_shuffle_ps requires a compile-time const, so enumerate all 256.
#define SH_SHUF_CASE_F32(imm)                                               \
            case imm: {                                                       \
              __m128 ra = _mm_load_ps((const float *)va->vec);               \
              __m128 rr = _mm_shuffle_ps(ra, ra, imm);                       \
              _mm_store_ps((float *)dst.vec, rr);                            \
              slots[ins->dst] = dst;                                         \
              simd_took = 1; goto vshuffle_done;                             \
            }
#define SH_SHUF_CASE_I32(imm)                                               \
            case imm: {                                                       \
              __m128i ra = _mm_load_si128((const __m128i *)va->vec);         \
              __m128i rr = _mm_shuffle_epi32(ra, imm);                       \
              _mm_store_si128((__m128i *)dst.vec, rr);                       \
              slots[ins->dst] = dst;                                         \
              simd_took = 1; goto vshuffle_done;                             \
            }
            if (lk == SH_K_F32) {
              switch (ctrl) {
                SH_SHUF_CASE_F32(0x00) SH_SHUF_CASE_F32(0x01) SH_SHUF_CASE_F32(0x02) SH_SHUF_CASE_F32(0x03)
                SH_SHUF_CASE_F32(0x04) SH_SHUF_CASE_F32(0x05) SH_SHUF_CASE_F32(0x06) SH_SHUF_CASE_F32(0x07)
                SH_SHUF_CASE_F32(0x08) SH_SHUF_CASE_F32(0x09) SH_SHUF_CASE_F32(0x0A) SH_SHUF_CASE_F32(0x0B)
                SH_SHUF_CASE_F32(0x0C) SH_SHUF_CASE_F32(0x0D) SH_SHUF_CASE_F32(0x0E) SH_SHUF_CASE_F32(0x0F)
                SH_SHUF_CASE_F32(0x10) SH_SHUF_CASE_F32(0x11) SH_SHUF_CASE_F32(0x12) SH_SHUF_CASE_F32(0x13)
                SH_SHUF_CASE_F32(0x14) SH_SHUF_CASE_F32(0x15) SH_SHUF_CASE_F32(0x16) SH_SHUF_CASE_F32(0x17)
                SH_SHUF_CASE_F32(0x18) SH_SHUF_CASE_F32(0x19) SH_SHUF_CASE_F32(0x1A) SH_SHUF_CASE_F32(0x1B)
                SH_SHUF_CASE_F32(0x1C) SH_SHUF_CASE_F32(0x1D) SH_SHUF_CASE_F32(0x1E) SH_SHUF_CASE_F32(0x1F)
                SH_SHUF_CASE_F32(0x20) SH_SHUF_CASE_F32(0x21) SH_SHUF_CASE_F32(0x22) SH_SHUF_CASE_F32(0x23)
                SH_SHUF_CASE_F32(0x24) SH_SHUF_CASE_F32(0x25) SH_SHUF_CASE_F32(0x26) SH_SHUF_CASE_F32(0x27)
                SH_SHUF_CASE_F32(0x28) SH_SHUF_CASE_F32(0x29) SH_SHUF_CASE_F32(0x2A) SH_SHUF_CASE_F32(0x2B)
                SH_SHUF_CASE_F32(0x2C) SH_SHUF_CASE_F32(0x2D) SH_SHUF_CASE_F32(0x2E) SH_SHUF_CASE_F32(0x2F)
                SH_SHUF_CASE_F32(0x30) SH_SHUF_CASE_F32(0x31) SH_SHUF_CASE_F32(0x32) SH_SHUF_CASE_F32(0x33)
                SH_SHUF_CASE_F32(0x34) SH_SHUF_CASE_F32(0x35) SH_SHUF_CASE_F32(0x36) SH_SHUF_CASE_F32(0x37)
                SH_SHUF_CASE_F32(0x38) SH_SHUF_CASE_F32(0x39) SH_SHUF_CASE_F32(0x3A) SH_SHUF_CASE_F32(0x3B)
                SH_SHUF_CASE_F32(0x3C) SH_SHUF_CASE_F32(0x3D) SH_SHUF_CASE_F32(0x3E) SH_SHUF_CASE_F32(0x3F)
                SH_SHUF_CASE_F32(0x40) SH_SHUF_CASE_F32(0x41) SH_SHUF_CASE_F32(0x42) SH_SHUF_CASE_F32(0x43)
                SH_SHUF_CASE_F32(0x44) SH_SHUF_CASE_F32(0x45) SH_SHUF_CASE_F32(0x46) SH_SHUF_CASE_F32(0x47)
                SH_SHUF_CASE_F32(0x48) SH_SHUF_CASE_F32(0x49) SH_SHUF_CASE_F32(0x4A) SH_SHUF_CASE_F32(0x4B)
                SH_SHUF_CASE_F32(0x4C) SH_SHUF_CASE_F32(0x4D) SH_SHUF_CASE_F32(0x4E) SH_SHUF_CASE_F32(0x4F)
                SH_SHUF_CASE_F32(0x50) SH_SHUF_CASE_F32(0x51) SH_SHUF_CASE_F32(0x52) SH_SHUF_CASE_F32(0x53)
                SH_SHUF_CASE_F32(0x54) SH_SHUF_CASE_F32(0x55) SH_SHUF_CASE_F32(0x56) SH_SHUF_CASE_F32(0x57)
                SH_SHUF_CASE_F32(0x58) SH_SHUF_CASE_F32(0x59) SH_SHUF_CASE_F32(0x5A) SH_SHUF_CASE_F32(0x5B)
                SH_SHUF_CASE_F32(0x5C) SH_SHUF_CASE_F32(0x5D) SH_SHUF_CASE_F32(0x5E) SH_SHUF_CASE_F32(0x5F)
                SH_SHUF_CASE_F32(0x60) SH_SHUF_CASE_F32(0x61) SH_SHUF_CASE_F32(0x62) SH_SHUF_CASE_F32(0x63)
                SH_SHUF_CASE_F32(0x64) SH_SHUF_CASE_F32(0x65) SH_SHUF_CASE_F32(0x66) SH_SHUF_CASE_F32(0x67)
                SH_SHUF_CASE_F32(0x68) SH_SHUF_CASE_F32(0x69) SH_SHUF_CASE_F32(0x6A) SH_SHUF_CASE_F32(0x6B)
                SH_SHUF_CASE_F32(0x6C) SH_SHUF_CASE_F32(0x6D) SH_SHUF_CASE_F32(0x6E) SH_SHUF_CASE_F32(0x6F)
                SH_SHUF_CASE_F32(0x70) SH_SHUF_CASE_F32(0x71) SH_SHUF_CASE_F32(0x72) SH_SHUF_CASE_F32(0x73)
                SH_SHUF_CASE_F32(0x74) SH_SHUF_CASE_F32(0x75) SH_SHUF_CASE_F32(0x76) SH_SHUF_CASE_F32(0x77)
                SH_SHUF_CASE_F32(0x78) SH_SHUF_CASE_F32(0x79) SH_SHUF_CASE_F32(0x7A) SH_SHUF_CASE_F32(0x7B)
                SH_SHUF_CASE_F32(0x7C) SH_SHUF_CASE_F32(0x7D) SH_SHUF_CASE_F32(0x7E) SH_SHUF_CASE_F32(0x7F)
                SH_SHUF_CASE_F32(0x80) SH_SHUF_CASE_F32(0x81) SH_SHUF_CASE_F32(0x82) SH_SHUF_CASE_F32(0x83)
                SH_SHUF_CASE_F32(0x84) SH_SHUF_CASE_F32(0x85) SH_SHUF_CASE_F32(0x86) SH_SHUF_CASE_F32(0x87)
                SH_SHUF_CASE_F32(0x88) SH_SHUF_CASE_F32(0x89) SH_SHUF_CASE_F32(0x8A) SH_SHUF_CASE_F32(0x8B)
                SH_SHUF_CASE_F32(0x8C) SH_SHUF_CASE_F32(0x8D) SH_SHUF_CASE_F32(0x8E) SH_SHUF_CASE_F32(0x8F)
                SH_SHUF_CASE_F32(0x90) SH_SHUF_CASE_F32(0x91) SH_SHUF_CASE_F32(0x92) SH_SHUF_CASE_F32(0x93)
                SH_SHUF_CASE_F32(0x94) SH_SHUF_CASE_F32(0x95) SH_SHUF_CASE_F32(0x96) SH_SHUF_CASE_F32(0x97)
                SH_SHUF_CASE_F32(0x98) SH_SHUF_CASE_F32(0x99) SH_SHUF_CASE_F32(0x9A) SH_SHUF_CASE_F32(0x9B)
                SH_SHUF_CASE_F32(0x9C) SH_SHUF_CASE_F32(0x9D) SH_SHUF_CASE_F32(0x9E) SH_SHUF_CASE_F32(0x9F)
                SH_SHUF_CASE_F32(0xA0) SH_SHUF_CASE_F32(0xA1) SH_SHUF_CASE_F32(0xA2) SH_SHUF_CASE_F32(0xA3)
                SH_SHUF_CASE_F32(0xA4) SH_SHUF_CASE_F32(0xA5) SH_SHUF_CASE_F32(0xA6) SH_SHUF_CASE_F32(0xA7)
                SH_SHUF_CASE_F32(0xA8) SH_SHUF_CASE_F32(0xA9) SH_SHUF_CASE_F32(0xAA) SH_SHUF_CASE_F32(0xAB)
                SH_SHUF_CASE_F32(0xAC) SH_SHUF_CASE_F32(0xAD) SH_SHUF_CASE_F32(0xAE) SH_SHUF_CASE_F32(0xAF)
                SH_SHUF_CASE_F32(0xB0) SH_SHUF_CASE_F32(0xB1) SH_SHUF_CASE_F32(0xB2) SH_SHUF_CASE_F32(0xB3)
                SH_SHUF_CASE_F32(0xB4) SH_SHUF_CASE_F32(0xB5) SH_SHUF_CASE_F32(0xB6) SH_SHUF_CASE_F32(0xB7)
                SH_SHUF_CASE_F32(0xB8) SH_SHUF_CASE_F32(0xB9) SH_SHUF_CASE_F32(0xBA) SH_SHUF_CASE_F32(0xBB)
                SH_SHUF_CASE_F32(0xBC) SH_SHUF_CASE_F32(0xBD) SH_SHUF_CASE_F32(0xBE) SH_SHUF_CASE_F32(0xBF)
                SH_SHUF_CASE_F32(0xC0) SH_SHUF_CASE_F32(0xC1) SH_SHUF_CASE_F32(0xC2) SH_SHUF_CASE_F32(0xC3)
                SH_SHUF_CASE_F32(0xC4) SH_SHUF_CASE_F32(0xC5) SH_SHUF_CASE_F32(0xC6) SH_SHUF_CASE_F32(0xC7)
                SH_SHUF_CASE_F32(0xC8) SH_SHUF_CASE_F32(0xC9) SH_SHUF_CASE_F32(0xCA) SH_SHUF_CASE_F32(0xCB)
                SH_SHUF_CASE_F32(0xCC) SH_SHUF_CASE_F32(0xCD) SH_SHUF_CASE_F32(0xCE) SH_SHUF_CASE_F32(0xCF)
                SH_SHUF_CASE_F32(0xD0) SH_SHUF_CASE_F32(0xD1) SH_SHUF_CASE_F32(0xD2) SH_SHUF_CASE_F32(0xD3)
                SH_SHUF_CASE_F32(0xD4) SH_SHUF_CASE_F32(0xD5) SH_SHUF_CASE_F32(0xD6) SH_SHUF_CASE_F32(0xD7)
                SH_SHUF_CASE_F32(0xD8) SH_SHUF_CASE_F32(0xD9) SH_SHUF_CASE_F32(0xDA) SH_SHUF_CASE_F32(0xDB)
                SH_SHUF_CASE_F32(0xDC) SH_SHUF_CASE_F32(0xDD) SH_SHUF_CASE_F32(0xDE) SH_SHUF_CASE_F32(0xDF)
                SH_SHUF_CASE_F32(0xE0) SH_SHUF_CASE_F32(0xE1) SH_SHUF_CASE_F32(0xE2) SH_SHUF_CASE_F32(0xE3)
                SH_SHUF_CASE_F32(0xE4) SH_SHUF_CASE_F32(0xE5) SH_SHUF_CASE_F32(0xE6) SH_SHUF_CASE_F32(0xE7)
                SH_SHUF_CASE_F32(0xE8) SH_SHUF_CASE_F32(0xE9) SH_SHUF_CASE_F32(0xEA) SH_SHUF_CASE_F32(0xEB)
                SH_SHUF_CASE_F32(0xEC) SH_SHUF_CASE_F32(0xED) SH_SHUF_CASE_F32(0xEE) SH_SHUF_CASE_F32(0xEF)
                SH_SHUF_CASE_F32(0xF0) SH_SHUF_CASE_F32(0xF1) SH_SHUF_CASE_F32(0xF2) SH_SHUF_CASE_F32(0xF3)
                SH_SHUF_CASE_F32(0xF4) SH_SHUF_CASE_F32(0xF5) SH_SHUF_CASE_F32(0xF6) SH_SHUF_CASE_F32(0xF7)
                SH_SHUF_CASE_F32(0xF8) SH_SHUF_CASE_F32(0xF9) SH_SHUF_CASE_F32(0xFA) SH_SHUF_CASE_F32(0xFB)
                SH_SHUF_CASE_F32(0xFC) SH_SHUF_CASE_F32(0xFD) SH_SHUF_CASE_F32(0xFE) SH_SHUF_CASE_F32(0xFF)
                default: break;
              }
            // NOTE: i64 is excluded -- _mm_shuffle_epi32 treats the register
            // as 4 x 32-bit lanes, which corrupts 8-byte i64 lanes.
            } else if (lk == SH_K_U32) {
              switch (ctrl) {
                SH_SHUF_CASE_I32(0x00) SH_SHUF_CASE_I32(0x01) SH_SHUF_CASE_I32(0x02) SH_SHUF_CASE_I32(0x03)
                SH_SHUF_CASE_I32(0x04) SH_SHUF_CASE_I32(0x05) SH_SHUF_CASE_I32(0x06) SH_SHUF_CASE_I32(0x07)
                SH_SHUF_CASE_I32(0x08) SH_SHUF_CASE_I32(0x09) SH_SHUF_CASE_I32(0x0A) SH_SHUF_CASE_I32(0x0B)
                SH_SHUF_CASE_I32(0x0C) SH_SHUF_CASE_I32(0x0D) SH_SHUF_CASE_I32(0x0E) SH_SHUF_CASE_I32(0x0F)
                SH_SHUF_CASE_I32(0x10) SH_SHUF_CASE_I32(0x11) SH_SHUF_CASE_I32(0x12) SH_SHUF_CASE_I32(0x13)
                SH_SHUF_CASE_I32(0x14) SH_SHUF_CASE_I32(0x15) SH_SHUF_CASE_I32(0x16) SH_SHUF_CASE_I32(0x17)
                SH_SHUF_CASE_I32(0x18) SH_SHUF_CASE_I32(0x19) SH_SHUF_CASE_I32(0x1A) SH_SHUF_CASE_I32(0x1B)
                SH_SHUF_CASE_I32(0x1C) SH_SHUF_CASE_I32(0x1D) SH_SHUF_CASE_I32(0x1E) SH_SHUF_CASE_I32(0x1F)
                SH_SHUF_CASE_I32(0x20) SH_SHUF_CASE_I32(0x21) SH_SHUF_CASE_I32(0x22) SH_SHUF_CASE_I32(0x23)
                SH_SHUF_CASE_I32(0x24) SH_SHUF_CASE_I32(0x25) SH_SHUF_CASE_I32(0x26) SH_SHUF_CASE_I32(0x27)
                SH_SHUF_CASE_I32(0x28) SH_SHUF_CASE_I32(0x29) SH_SHUF_CASE_I32(0x2A) SH_SHUF_CASE_I32(0x2B)
                SH_SHUF_CASE_I32(0x2C) SH_SHUF_CASE_I32(0x2D) SH_SHUF_CASE_I32(0x2E) SH_SHUF_CASE_I32(0x2F)
                SH_SHUF_CASE_I32(0x30) SH_SHUF_CASE_I32(0x31) SH_SHUF_CASE_I32(0x32) SH_SHUF_CASE_I32(0x33)
                SH_SHUF_CASE_I32(0x34) SH_SHUF_CASE_I32(0x35) SH_SHUF_CASE_I32(0x36) SH_SHUF_CASE_I32(0x37)
                SH_SHUF_CASE_I32(0x38) SH_SHUF_CASE_I32(0x39) SH_SHUF_CASE_I32(0x3A) SH_SHUF_CASE_I32(0x3B)
                SH_SHUF_CASE_I32(0x3C) SH_SHUF_CASE_I32(0x3D) SH_SHUF_CASE_I32(0x3E) SH_SHUF_CASE_I32(0x3F)
                SH_SHUF_CASE_I32(0x40) SH_SHUF_CASE_I32(0x41) SH_SHUF_CASE_I32(0x42) SH_SHUF_CASE_I32(0x43)
                SH_SHUF_CASE_I32(0x44) SH_SHUF_CASE_I32(0x45) SH_SHUF_CASE_I32(0x46) SH_SHUF_CASE_I32(0x47)
                SH_SHUF_CASE_I32(0x48) SH_SHUF_CASE_I32(0x49) SH_SHUF_CASE_I32(0x4A) SH_SHUF_CASE_I32(0x4B)
                SH_SHUF_CASE_I32(0x4C) SH_SHUF_CASE_I32(0x4D) SH_SHUF_CASE_I32(0x4E) SH_SHUF_CASE_I32(0x4F)
                SH_SHUF_CASE_I32(0x50) SH_SHUF_CASE_I32(0x51) SH_SHUF_CASE_I32(0x52) SH_SHUF_CASE_I32(0x53)
                SH_SHUF_CASE_I32(0x54) SH_SHUF_CASE_I32(0x55) SH_SHUF_CASE_I32(0x56) SH_SHUF_CASE_I32(0x57)
                SH_SHUF_CASE_I32(0x58) SH_SHUF_CASE_I32(0x59) SH_SHUF_CASE_I32(0x5A) SH_SHUF_CASE_I32(0x5B)
                SH_SHUF_CASE_I32(0x5C) SH_SHUF_CASE_I32(0x5D) SH_SHUF_CASE_I32(0x5E) SH_SHUF_CASE_I32(0x5F)
                SH_SHUF_CASE_I32(0x60) SH_SHUF_CASE_I32(0x61) SH_SHUF_CASE_I32(0x62) SH_SHUF_CASE_I32(0x63)
                SH_SHUF_CASE_I32(0x64) SH_SHUF_CASE_I32(0x65) SH_SHUF_CASE_I32(0x66) SH_SHUF_CASE_I32(0x67)
                SH_SHUF_CASE_I32(0x68) SH_SHUF_CASE_I32(0x69) SH_SHUF_CASE_I32(0x6A) SH_SHUF_CASE_I32(0x6B)
                SH_SHUF_CASE_I32(0x6C) SH_SHUF_CASE_I32(0x6D) SH_SHUF_CASE_I32(0x6E) SH_SHUF_CASE_I32(0x6F)
                SH_SHUF_CASE_I32(0x70) SH_SHUF_CASE_I32(0x71) SH_SHUF_CASE_I32(0x72) SH_SHUF_CASE_I32(0x73)
                SH_SHUF_CASE_I32(0x74) SH_SHUF_CASE_I32(0x75) SH_SHUF_CASE_I32(0x76) SH_SHUF_CASE_I32(0x77)
                SH_SHUF_CASE_I32(0x78) SH_SHUF_CASE_I32(0x79) SH_SHUF_CASE_I32(0x7A) SH_SHUF_CASE_I32(0x7B)
                SH_SHUF_CASE_I32(0x7C) SH_SHUF_CASE_I32(0x7D) SH_SHUF_CASE_I32(0x7E) SH_SHUF_CASE_I32(0x7F)
                SH_SHUF_CASE_I32(0x80) SH_SHUF_CASE_I32(0x81) SH_SHUF_CASE_I32(0x82) SH_SHUF_CASE_I32(0x83)
                SH_SHUF_CASE_I32(0x84) SH_SHUF_CASE_I32(0x85) SH_SHUF_CASE_I32(0x86) SH_SHUF_CASE_I32(0x87)
                SH_SHUF_CASE_I32(0x88) SH_SHUF_CASE_I32(0x89) SH_SHUF_CASE_I32(0x8A) SH_SHUF_CASE_I32(0x8B)
                SH_SHUF_CASE_I32(0x8C) SH_SHUF_CASE_I32(0x8D) SH_SHUF_CASE_I32(0x8E) SH_SHUF_CASE_I32(0x8F)
                SH_SHUF_CASE_I32(0x90) SH_SHUF_CASE_I32(0x91) SH_SHUF_CASE_I32(0x92) SH_SHUF_CASE_I32(0x93)
                SH_SHUF_CASE_I32(0x94) SH_SHUF_CASE_I32(0x95) SH_SHUF_CASE_I32(0x96) SH_SHUF_CASE_I32(0x97)
                SH_SHUF_CASE_I32(0x98) SH_SHUF_CASE_I32(0x99) SH_SHUF_CASE_I32(0x9A) SH_SHUF_CASE_I32(0x9B)
                SH_SHUF_CASE_I32(0x9C) SH_SHUF_CASE_I32(0x9D) SH_SHUF_CASE_I32(0x9E) SH_SHUF_CASE_I32(0x9F)
                SH_SHUF_CASE_I32(0xA0) SH_SHUF_CASE_I32(0xA1) SH_SHUF_CASE_I32(0xA2) SH_SHUF_CASE_I32(0xA3)
                SH_SHUF_CASE_I32(0xA4) SH_SHUF_CASE_I32(0xA5) SH_SHUF_CASE_I32(0xA6) SH_SHUF_CASE_I32(0xA7)
                SH_SHUF_CASE_I32(0xA8) SH_SHUF_CASE_I32(0xA9) SH_SHUF_CASE_I32(0xAA) SH_SHUF_CASE_I32(0xAB)
                SH_SHUF_CASE_I32(0xAC) SH_SHUF_CASE_I32(0xAD) SH_SHUF_CASE_I32(0xAE) SH_SHUF_CASE_I32(0xAF)
                SH_SHUF_CASE_I32(0xB0) SH_SHUF_CASE_I32(0xB1) SH_SHUF_CASE_I32(0xB2) SH_SHUF_CASE_I32(0xB3)
                SH_SHUF_CASE_I32(0xB4) SH_SHUF_CASE_I32(0xB5) SH_SHUF_CASE_I32(0xB6) SH_SHUF_CASE_I32(0xB7)
                SH_SHUF_CASE_I32(0xB8) SH_SHUF_CASE_I32(0xB9) SH_SHUF_CASE_I32(0xBA) SH_SHUF_CASE_I32(0xBB)
                SH_SHUF_CASE_I32(0xBC) SH_SHUF_CASE_I32(0xBD) SH_SHUF_CASE_I32(0xBE) SH_SHUF_CASE_I32(0xBF)
                SH_SHUF_CASE_I32(0xC0) SH_SHUF_CASE_I32(0xC1) SH_SHUF_CASE_I32(0xC2) SH_SHUF_CASE_I32(0xC3)
                SH_SHUF_CASE_I32(0xC4) SH_SHUF_CASE_I32(0xC5) SH_SHUF_CASE_I32(0xC6) SH_SHUF_CASE_I32(0xC7)
                SH_SHUF_CASE_I32(0xC8) SH_SHUF_CASE_I32(0xC9) SH_SHUF_CASE_I32(0xCA) SH_SHUF_CASE_I32(0xCB)
                SH_SHUF_CASE_I32(0xCC) SH_SHUF_CASE_I32(0xCD) SH_SHUF_CASE_I32(0xCE) SH_SHUF_CASE_I32(0xCF)
                SH_SHUF_CASE_I32(0xD0) SH_SHUF_CASE_I32(0xD1) SH_SHUF_CASE_I32(0xD2) SH_SHUF_CASE_I32(0xD3)
                SH_SHUF_CASE_I32(0xD4) SH_SHUF_CASE_I32(0xD5) SH_SHUF_CASE_I32(0xD6) SH_SHUF_CASE_I32(0xD7)
                SH_SHUF_CASE_I32(0xD8) SH_SHUF_CASE_I32(0xD9) SH_SHUF_CASE_I32(0xDA) SH_SHUF_CASE_I32(0xDB)
                SH_SHUF_CASE_I32(0xDC) SH_SHUF_CASE_I32(0xDD) SH_SHUF_CASE_I32(0xDE) SH_SHUF_CASE_I32(0xDF)
                SH_SHUF_CASE_I32(0xE0) SH_SHUF_CASE_I32(0xE1) SH_SHUF_CASE_I32(0xE2) SH_SHUF_CASE_I32(0xE3)
                SH_SHUF_CASE_I32(0xE4) SH_SHUF_CASE_I32(0xE5) SH_SHUF_CASE_I32(0xE6) SH_SHUF_CASE_I32(0xE7)
                SH_SHUF_CASE_I32(0xE8) SH_SHUF_CASE_I32(0xE9) SH_SHUF_CASE_I32(0xEA) SH_SHUF_CASE_I32(0xEB)
                SH_SHUF_CASE_I32(0xEC) SH_SHUF_CASE_I32(0xED) SH_SHUF_CASE_I32(0xEE) SH_SHUF_CASE_I32(0xEF)
                SH_SHUF_CASE_I32(0xF0) SH_SHUF_CASE_I32(0xF1) SH_SHUF_CASE_I32(0xF2) SH_SHUF_CASE_I32(0xF3)
                SH_SHUF_CASE_I32(0xF4) SH_SHUF_CASE_I32(0xF5) SH_SHUF_CASE_I32(0xF6) SH_SHUF_CASE_I32(0xF7)
                SH_SHUF_CASE_I32(0xF8) SH_SHUF_CASE_I32(0xF9) SH_SHUF_CASE_I32(0xFA) SH_SHUF_CASE_I32(0xFB)
                SH_SHUF_CASE_I32(0xFC) SH_SHUF_CASE_I32(0xFD) SH_SHUF_CASE_I32(0xFE) SH_SHUF_CASE_I32(0xFF)
                default: break;
              }
            }
#undef SH_SHUF_CASE_F32
#undef SH_SHUF_CASE_I32
          }
          vshuffle_done:;
          (void)0;  // label needs a statement
        }
        // If SIMD path stored the result, we're done; else fall to scalar.
        if (simd_took) break;
        }  // end of SIMD block
#endif  // __SSE2__

        // Scalar fallback.
        for (uint8_t li = 0; li < nl; li++) {
          uint32_t src_idx = c->aux[ins->aux_off + li];
          if (src_idx >= va->lanes) {
            status = sh_set_error(err, SH_ERR_INTERNAL, -1, -1,
                                  "vm: VSHUFFLE: lane index %u >= nlanes %u",
                                  src_idx, (uint32_t)va->lanes);
            goto done;
          }
          vec_lane_set(&dst, li, vec_lane_get(va, (uint8_t)src_idx));
        }
        slots[ins->dst] = dst;
        break;
      }

      // -----------------------------------------------------------------------
      // SHB_VREDUCE: dst = reduce(a) -> scalar. sub = sh_reduce.
      //   kind = lane kind of input; lanes = input lane count.
      //   DOT uses b as second vector.
      // Reduction is ALWAYS SCALAR (no SIMD path). The spec mandates a
      // sequential left-to-right fold; a SIMD horizontal or tree reduction
      // reassociates floats and produces a different bit-pattern -- that is the
      // primary float bit-exactness trap. Integer reductions stay scalar too for
      // simplicity (the lane-wise ops are where the SIMD win is anyway).
      // -----------------------------------------------------------------------
      case SHB_VREDUCE: {
        if (ins->dst == SH_VREG_NONE) break;
        sh_kind  lk  = (sh_kind)ins->kind;
        uint8_t  nl  = ins->lanes;
        vm_value *va = &slots[ins->a];
        sh_reduce rop = (sh_reduce)ins->sub;
        vm_value dst;
        memset(&dst, 0, sizeof(dst));

        if (rop == SH_RED_DOT) {
          vm_value *vb = &slots[ins->b];
          uint64_t acc = 0;
          for (uint8_t li = 0; li < nl; li++) {
            uint64_t prod = scalar_binop_bits(SH_BIN_MUL, lk,
                                              vec_lane_get(va, li),
                                              vec_lane_get(vb, li));
            acc = (li == 0) ? prod
                            : scalar_binop_bits(SH_BIN_ADD, lk, acc, prod);
          }
          vm_set_scalar(&dst, lk, acc);
        } else {
          if (nl == 0) {
            dst.kind = lk;
            dst.scalar = 0;
          } else {
            uint64_t acc = vec_lane_get(va, 0);
            for (uint8_t li = 1; li < nl; li++) {
              uint64_t cur = vec_lane_get(va, li);
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
            vm_set_scalar(&dst, lk, acc);
          }
        }
        // Mirror interp: set the result kind to the declared result kind.
        // (The bytecode carries the result scalar kind in the instruction.)
        // For scalars from a reduce, result is a scalar of lane kind.
        // The interp sets out->kind = n->type.kind; the lowerer doesn't
        // separately carry the result kind in the instruction -- it just sets
        // kind = lane kind. Use lk for the result kind.
        dst.kind = lk;
        slots[ins->dst] = dst;
        break;
      }

      // -----------------------------------------------------------------------
      // SHB_VLANE: dst = a.lane[imm] -> scalar. kind = lane kind.
      // -----------------------------------------------------------------------
      case SHB_VLANE: {
        if (ins->dst == SH_VREG_NONE) break;
        sh_kind  lk  = (sh_kind)ins->kind;
        uint32_t li  = ins->imm;
        vm_value *va = &slots[ins->a];
        if (li >= va->lanes) {
          status = sh_set_error(err, SH_ERR_INTERNAL, -1, -1,
                                "vm: VLANE: lane %u >= nlanes %u",
                                li, (uint32_t)va->lanes);
          goto done;
        }
        uint64_t bits = vec_lane_get(va, (uint8_t)li);
        vm_set_scalar(&slots[ins->dst], lk, bits);
        break;
      }

      // -----------------------------------------------------------------------
      // SHB_VRLOAD: dst = vec of N consecutive elements from region a at index b.
      // SSE path: _mm_loadu_si128 for 128-bit strips (N*esz == 16 bytes).
      // Scalar fallback for other widths.
      // -----------------------------------------------------------------------
      case SHB_VRLOAD: {
        if (ins->dst == SH_VREG_NONE) break;
        vm_value *va = &slots[ins->a];
        vm_value *vb = &slots[ins->b];
        uint64_t idx = vb->scalar;
        if (vb->kind == SH_K_I64) idx = (uint64_t)(int64_t)vb->scalar;
        uint64_t len = (uint64_t)va->region.len;
        uint64_t N   = (uint64_t)ins->lanes;
        sh_kind  ek  = va->region.elem;
        // Overflow-free bounds check (same formula as interp oracle)
        if (idx > len || (uint64_t)(len - idx) < N) {
          status = sh_set_error(err, SH_ERR_BOUNDS, -1, -1,
                                "vm: vregion-ref: bounds check failed"
                                " (idx=%llu len=%llu N=%llu)",
                                (unsigned long long)idx,
                                (unsigned long long)len,
                                (unsigned long long)N);
          goto done;
        }
        uint32_t esz = sh_kind_size(ek);
        const uint8_t *src = va->region.base + idx * esz;
        vm_value dst;
        memset(&dst, 0, sizeof(dst));
        dst.kind      = SH_K_VEC;
        dst.lanes     = (uint8_t)N;
        dst.lane_kind = (uint8_t)ek;

#if defined(__SSE2__)
        if (!force_scalar && N * esz == 16) {
          // 128-bit strip: use _mm_loadu_si128 (unaligned load from region).
          __m128i r = _mm_loadu_si128((const __m128i *)src);
          _mm_store_si128((__m128i *)dst.vec, r);
          slots[ins->dst] = dst;
          break;
        }
#endif
        // Scalar fallback: memcpy N*esz bytes into the packed vec[].
        memcpy(dst.vec, src, N * esz);
        slots[ins->dst] = dst;
        break;
      }

      // -----------------------------------------------------------------------
      // SHB_VRSTORE: write N lanes of vector c to region a at index b.
      // SSE path: _mm_storeu_si128 for 128-bit strips.
      // Scalar fallback for other widths.
      // -----------------------------------------------------------------------
      case SHB_VRSTORE: {
        vm_value *va = &slots[ins->a];
        vm_value *vb = &slots[ins->b];
        vm_value *vc = &slots[ins->c];
        uint64_t idx = vb->scalar;
        if (vb->kind == SH_K_I64) idx = (uint64_t)(int64_t)vb->scalar;
        uint64_t len = (uint64_t)va->region.len;
        // Finding 4: use the runtime vector slot's lane count (vc->lanes) so
        // the data written and the count agree and match the oracle. For
        // well-formed chunks ins->lanes == vc->lanes; using vc->lanes ensures
        // the SSE path (N*esz bytes read from vc->vec) and the bounds check
        // are always consistent with the actual data copied.
        uint64_t N   = (uint64_t)vc->lanes;
        sh_kind  ek  = va->region.elem;
        // Overflow-free bounds check (same formula as interp oracle)
        if (idx > len || (uint64_t)(len - idx) < N) {
          status = sh_set_error(err, SH_ERR_BOUNDS, -1, -1,
                                "vm: vregion-set!: bounds check failed"
                                " (idx=%llu len=%llu N=%llu)",
                                (unsigned long long)idx,
                                (unsigned long long)len,
                                (unsigned long long)N);
          goto done;
        }
        uint32_t esz  = sh_kind_size(ek);
        uint8_t *dest = va->region.base + idx * esz;

#if defined(__SSE2__)
        if (!force_scalar && N * esz == 16) {
          // 128-bit strip: use _mm_load_si128 from (aligned) vec, then storeu.
          __m128i r = _mm_load_si128((const __m128i *)vc->vec);
          _mm_storeu_si128((__m128i *)dest, r);
          if (ins->dst != SH_VREG_NONE) slots[ins->dst] = *vc;
          break;
        }
#endif
        // Scalar fallback: memcpy N*esz bytes from packed vec[].
        memcpy(dest, vc->vec, N * esz);
        if (ins->dst != SH_VREG_NONE) slots[ins->dst] = *vc;
        break;
      }

      default:
        status = sh_set_error(err, SH_ERR_INTERNAL, -1, -1,
                              "vm: unknown opcode %u at pc=%u",
                              (unsigned)op, pc - 1);
        goto done;
    }
  }

done:
  if (status == SH_OK) {
    vm_value result;
    if (did_ret) {
      result = ret_val;
    } else if (c->result != SH_VREG_NONE) {
      result = slots[c->result];
    } else {
      memset(&result, 0, sizeof(result));
    }
    *out = to_sh_value(result);
  }

  free(slots_raw);  // the raw (pre-alignment) pointer
  return status;
}
