// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

// S3 UNIT 2 -- the bytecode VM: chunk validator + scalar executor.
//
// sh_chunk_validate:  reject any chunk with OOB vreg refs, jump targets,
//   aux ranges, prim indices, or a bad result vreg. Defense-in-depth so a
//   buggy lowerer cannot drive sh_vm_run out of bounds.
//
// sh_vm_run: execute the chunk on typed args; runtime-bounds-check every
//   region access; semantics match sh_interp.c bit-for-bit. Vector ops use
//   the scalar lane-loop path (SH_VM_FORCE_SCALAR is a no-op for now).
//
// Value representation (internal, for S3-3 compatibility):
//   typedef vm_value (see below). Scalars: kind + scalar u64 field.
//   Vectors: kind=SH_K_VEC, lanes/lane_kind set, bytes packed at native
//   element width in a 16-byte-aligned vec[SH_MAX_LANES * 8] buffer so that
//   S3-3 can _mm_load_* them directly without a reformat.
//   Regions: kind=SH_K_REGION, base/len/elem/mut carried directly.
//
// Dispatch: a switch() over sh_bc_op in the main pc loop.
// All scalar math is delegated to static helpers (scalar_binop_bits,
// scalar_cmp_bits) that mirror sh_interp.c exactly.

#include <stdlib.h>
#include <string.h>

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
        case SH_BIN_ADD: rr = (uint8_t)(ra + rb); break;
        case SH_BIN_SUB: rr = (uint8_t)(ra - rb); break;
        case SH_BIN_MUL: rr = (uint8_t)(ra * rb); break;
        case SH_BIN_DIV: rr = rb ? (uint8_t)(ra / rb) : 0; break;
        case SH_BIN_MOD: rr = rb ? (uint8_t)(ra % rb) : 0; break;
        case SH_BIN_AND: rr = ra & rb; break;
        case SH_BIN_OR:  rr = ra | rb; break;
        case SH_BIN_XOR: rr = ra ^ rb; break;
        case SH_BIN_SHL: rr = (uint8_t)(ra << (rb & 7u)); break;
        case SH_BIN_SHR: rr = (uint8_t)(ra >> (rb & 7u)); break;
        default: rr = 0; break;
      }
      return (uint64_t)rr;
    }
    case SH_K_U16: {
      uint16_t ra = (uint16_t)a, rb = (uint16_t)b, rr = 0;
      switch (op) {
        case SH_BIN_ADD: rr = (uint16_t)(ra + rb); break;
        case SH_BIN_SUB: rr = (uint16_t)(ra - rb); break;
        case SH_BIN_MUL: rr = (uint16_t)(ra * rb); break;
        case SH_BIN_DIV: rr = rb ? (uint16_t)(ra / rb) : 0; break;
        case SH_BIN_MOD: rr = rb ? (uint16_t)(ra % rb) : 0; break;
        case SH_BIN_AND: rr = ra & rb; break;
        case SH_BIN_OR:  rr = ra | rb; break;
        case SH_BIN_XOR: rr = ra ^ rb; break;
        case SH_BIN_SHL: rr = (uint16_t)(ra << (rb & 15u)); break;
        case SH_BIN_SHR: rr = (uint16_t)(ra >> (rb & 15u)); break;
        default: rr = 0; break;
      }
      return (uint64_t)rr;
    }
    case SH_K_BOOL:
    case SH_K_U32: {
      uint32_t ra = (uint32_t)a, rb = (uint32_t)b, rr = 0;
      switch (op) {
        case SH_BIN_ADD: rr = ra + rb; break;
        case SH_BIN_SUB: rr = ra - rb; break;
        case SH_BIN_MUL: rr = ra * rb; break;
        case SH_BIN_DIV: rr = rb ? ra / rb : 0; break;
        case SH_BIN_MOD: rr = rb ? ra % rb : 0; break;
        case SH_BIN_AND: rr = ra & rb; break;
        case SH_BIN_OR:  rr = ra | rb; break;
        case SH_BIN_XOR: rr = ra ^ rb; break;
        case SH_BIN_SHL: rr = ra << (rb & 31u); break;
        case SH_BIN_SHR: rr = ra >> (rb & 31u); break;
        default: rr = 0; break;
      }
      return (uint64_t)rr;
    }
    case SH_K_U64: {
      uint64_t ra = a, rb = b;
      switch (op) {
        case SH_BIN_ADD: return ra + rb;
        case SH_BIN_SUB: return ra - rb;
        case SH_BIN_MUL: return ra * rb;
        case SH_BIN_DIV: return rb ? ra / rb : 0;
        case SH_BIN_MOD: return rb ? ra % rb : 0;
        case SH_BIN_AND: return ra & rb;
        case SH_BIN_OR:  return ra | rb;
        case SH_BIN_XOR: return ra ^ rb;
        case SH_BIN_SHL: return ra << (rb & 63u);
        case SH_BIN_SHR: return ra >> (rb & 63u);
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

    // Check dst/a/b/c when not SH_VREG_NONE.
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
  (void)flags;  // SH_VM_FORCE_SCALAR: no SIMD yet, so always scalar

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

  // Allocate the slot file.
  vm_value *slots = NULL;
  if (c->nvregs > 0) {
    slots = (vm_value *)calloc(c->nvregs, sizeof(vm_value));
    if (!slots)
      return sh_set_error(err, SH_ERR_OOM, -1, -1,
                          "vm_run: OOM allocating slot file (%u vregs)",
                          c->nvregs);
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
      // -----------------------------------------------------------------------
      case SHB_VSPLAT: {
        if (ins->dst == SH_VREG_NONE) break;
        sh_kind lk    = (sh_kind)ins->kind;
        uint8_t nlanes = ins->lanes;
        uint64_t bits  = vm_scalar_bits(&slots[ins->a]);
        // For f32 scalar the scalar field already holds the f32 bit pattern.
        // Mirror interp's scalar_to_lane: for F32 keep the 32-bit pattern.
        vm_value dst;
        memset(&dst, 0, sizeof(dst));
        dst.kind      = SH_K_VEC;
        dst.lanes     = nlanes;
        dst.lane_kind = (uint8_t)lk;
        for (uint8_t li = 0; li < nlanes; li++)
          vec_lane_set(&dst, li, bits);
        slots[ins->dst] = dst;
        break;
      }

      // -----------------------------------------------------------------------
      // SHB_VBINOP: dst = lane-wise binop(a, b). sub = sh_binop; kind = lane kind.
      // -----------------------------------------------------------------------
      case SHB_VBINOP: {
        if (ins->dst == SH_VREG_NONE) break;
        sh_kind lk   = (sh_kind)ins->kind;
        uint8_t nl   = ins->lanes;
        vm_value *va  = &slots[ins->a];
        vm_value *vb  = &slots[ins->b];
        vm_value dst;
        memset(&dst, 0, sizeof(dst));
        dst.kind      = SH_K_VEC;
        dst.lanes     = nl;
        dst.lane_kind = (uint8_t)lk;
        for (uint8_t li = 0; li < nl; li++) {
          uint64_t ab  = vec_lane_get(va, li);
          uint64_t bb  = vec_lane_get(vb, li);
          uint64_t res = scalar_binop_bits((sh_binop)ins->sub, lk, ab, bb);
          vec_lane_set(&dst, li, res);
        }
        slots[ins->dst] = dst;
        break;
      }

      // -----------------------------------------------------------------------
      // SHB_VCMP: dst = lane-wise cmp(a, b) -> bool-mask. kind = operand lane kind.
      // -----------------------------------------------------------------------
      case SHB_VCMP: {
        if (ins->dst == SH_VREG_NONE) break;
        sh_kind lk    = (sh_kind)ins->kind;
        uint8_t  nl   = ins->lanes;
        vm_value *va  = &slots[ins->a];
        vm_value *vb  = &slots[ins->b];
        vm_value dst;
        memset(&dst, 0, sizeof(dst));
        dst.kind      = SH_K_VEC;
        dst.lanes     = nl;
        dst.lane_kind = (uint8_t)SH_K_BOOL;
        for (uint8_t li = 0; li < nl; li++) {
          uint64_t ab  = vec_lane_get(va, li);
          uint64_t bb  = vec_lane_get(vb, li);
          uint64_t res = scalar_cmp_bits((sh_cmp)ins->sub, lk, ab, bb);
          vec_lane_set(&dst, li, res);
        }
        slots[ins->dst] = dst;
        break;
      }

      // -----------------------------------------------------------------------
      // SHB_VSELECT: dst = lane-wise (mask a) ? b : c. kind = lane kind.
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
      // Reduction is SEQUENTIAL left-to-right (the spec mandates it; a tree
      // reduction reassociates and diverges for floats).
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

  free(slots);
  return status;
}
