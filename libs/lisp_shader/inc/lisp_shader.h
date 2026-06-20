// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#ifndef CARDINAL_LISP_SHADER_H
#define CARDINAL_LISP_SHADER_H

// Cardinal; shader tier -- the PUBLIC surface (see notes/core/lisp-shaders.md).
//
// A "shader" is a typed, compiled, restricted Lisp kernel: monomorphic, unboxed,
// allocation-free, with a closed typed signature that is its capability list
// (W7). This header is the painful-to-retrofit boundary -- the type encoding, the
// value ABI, the capability model, and the compile/invoke API -- and is frozen
// deliberately. The IR itself (src/sh_internal.h) is private and may change.
//
// Scope of the first deliverable: S0 (frontend+types+verifier), S1 (typed AST +
// reference interpreter), S2-scalar (fixed-width vectors present, scalar-LOWERED
// in the interpreter -- no SIMD, no x86 backend, no in-OS integration yet).

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lisp.h"  // the existing reader / lisp_value / lisp_bytes the frontend reuses

// --- limits -----------------------------------------------------------------
#define SH_MAX_LANES 16        // u8x16, f32x4, u32x8, ...; a wider type is a verifier error
#define SH_MAX_PARAMS 16       // shader parameters
#define SH_MAX_PRIM_PARAMS 8   // params of a whitelisted primitive

// --- scalar / aggregate kind ------------------------------------------------
typedef enum {
    SH_K_VOID = 0,
    SH_K_BOOL,
    SH_K_U8, SH_K_U16, SH_K_U32, SH_K_U64, SH_K_I64,
    SH_K_F32, SH_K_F64,
    SH_K_VEC,     // a fixed-width vector; lane kind + count carried in sh_type
    SH_K_REGION,  // a typed bytes view; element kind carried in sh_type
} sh_kind;

// --- a shader type: one 4-byte comparable POD, no allocation -----------------
// Scalar:        {kind, 0, 0, 0}
// Vector <N x T>:{SH_K_VEC, T, N, 0}            (vecN sugar maps here -- unified)
// Region (bytes T):{SH_K_REGION, T, 0, mut?1:0}
// Length is NOT in the type -- it is provenance the verifier tracks (see note).
typedef struct {
    uint8_t kind;       // sh_kind
    uint8_t lane_kind;  // SH_K_VEC/REGION: the element's (scalar) sh_kind
    uint8_t lanes;      // SH_K_VEC: lane count (2,3,4,8,16); else 0
    uint8_t flags;      // bit0 = region is mutable (store allowed)
} sh_type;

#define SH_TYPE_FLAG_MUTABLE 0x1u

static inline sh_type sh_type_scalar(sh_kind k) {
    sh_type t = {(uint8_t)k, 0, 0, 0};
    return t;
}
static inline sh_type sh_type_vec(sh_kind lane, uint8_t n) {
    sh_type t = {(uint8_t)SH_K_VEC, (uint8_t)lane, n, 0};
    return t;
}
static inline sh_type sh_type_region(sh_kind elem, bool mut) {
    sh_type t = {(uint8_t)SH_K_REGION, (uint8_t)elem, 0,
                 (uint8_t)(mut ? SH_TYPE_FLAG_MUTABLE : 0)};
    return t;
}
static inline bool sh_type_eq(sh_type a, sh_type b) {
    return a.kind == b.kind && a.lane_kind == b.lane_kind &&
           a.lanes == b.lanes && a.flags == b.flags;
}
static inline uint32_t sh_kind_size(sh_kind k) {
    switch (k) {
        case SH_K_BOOL: case SH_K_U8: return 1;
        case SH_K_U16: return 2;
        case SH_K_U32: case SH_K_F32: return 4;
        case SH_K_U64: case SH_K_I64: case SH_K_F64: return 8;
        default: return 0;
    }
}
static inline bool sh_kind_is_int(sh_kind k) {
    return k == SH_K_U8 || k == SH_K_U16 || k == SH_K_U32 ||
           k == SH_K_U64 || k == SH_K_I64;
}
static inline bool sh_kind_is_float(sh_kind k) { return k == SH_K_F32 || k == SH_K_F64; }

// --- a runtime value: a small self-checking tagged union (the oracle is cold) -
// f32 is held as a double and narrowed (double)(float) at every store/const/op so
// a future SIMD backend's f32x4 matches bit-for-bit (the note's differential test).
typedef struct {
    sh_kind kind;   // SH_K_*; for a vector value, SH_K_VEC
    uint8_t lanes;  // 1 for scalars; N for a vector value
    uint8_t lane_kind;  // for a vector value: the lane scalar kind
    union {
        uint64_t u;  // u8/u16/u32/u64 zero-extended; bool in {0,1}
        int64_t  i;  // i64
        double   f;  // f32 (narrowed) or f64
        struct {
            uint8_t *base;
            uint32_t len;     // element count
            sh_kind  elem;
            uint8_t  mutable_;
        } region;
        uint64_t lane[SH_MAX_LANES];  // vector lanes, each a scalar bit pattern
    };
} sh_value;

// --- whitelisted primitive: the closed typed signature IS the capability ------
typedef sh_value (*sh_prim_fn)(const sh_value *args, uint32_t argc);
typedef struct {
    const char *name;  // the symbol a shader writes to call it
    sh_type     ret;
    uint8_t     nparams;
    sh_type     params[SH_MAX_PRIM_PARAMS];
    sh_prim_fn  fn;  // host C impl (scalar, pure, allocation-free); NULL = stub
} sh_prim;
typedef struct {
    const sh_prim *prims;
    uint32_t       count;
} sh_prim_set;

// --- status / structured error ----------------------------------------------
typedef enum {
    SH_OK = 0,
    SH_ERR_PARSE,           // not a well-formed (defshader ...) datum
    SH_ERR_BAD_FORM,        // a banned/unknown form in the body
    SH_ERR_TYPE,            // a type mismatch
    SH_ERR_UNKNOWN_NAME,    // a free identifier (not param/local/prim)
    SH_ERR_NOT_WHITELISTED, // call to a name not in the prim set
    SH_ERR_UNBOUNDED_LOOP,  // a loop the bounded-loop template did not recognize
    SH_ERR_NONCONST_COST,   // SH_REQUIRE_CONST_COST set but cost is arg-dependent
    SH_ERR_ARITY,           // wrong arg count at a call or at invoke
    SH_ERR_BOUNDS,          // runtime: region access out of range (a trap)
    SH_ERR_OOM,
    SH_ERR_INTERNAL,
} sh_status;

typedef struct {
    sh_status status;
    char      msg[160];
    int       line, col;  // source location when known (-1 otherwise)
} sh_error;

// --- compile flags ----------------------------------------------------------
enum { SH_REQUIRE_CONST_COST = 1u << 0 };  // reject arg-dependent cost (ISR/cli use)

// --- the compiled handle (opaque; one owned allocation) ---------------------
typedef struct sh_program sh_program;

// --- compile (frontend + verifier) ------------------------------------------
// Compile ONE shader. `form` is a (defshader ...) datum from the existing reader.
// `prims` is the closed capability set (may be NULL for none). On SH_OK returns
// *out_prog (free with sh_free); otherwise fills *err. Total: every input either
// compiles or yields a structured error -- never UB.
sh_status sh_compile(lisp_value form, const sh_prim_set *prims, uint32_t flags,
                     sh_program **out_prog, sh_error *err);
// Read the first datum from `src` then compile it.
sh_status sh_compile_string(const char *src, const sh_prim_set *prims, uint32_t flags,
                            sh_program **out_prog, sh_error *err);
void sh_free(sh_program *p);

// --- introspection: the DERIVED contract (free, drift-proof) ----------------
const char *sh_name(const sh_program *p);
uint32_t sh_param_count(const sh_program *p);
sh_type  sh_param_type(const sh_program *p, uint32_t i);
sh_type  sh_return_type(const sh_program *p);
// Worst-case cost. If arg-independent (all bounds constant) the value is exact;
// otherwise use sh_cost_for_args with concrete args. sh_cost_is_const reports which.
uint64_t sh_static_cost(const sh_program *p);
bool     sh_cost_is_const(const sh_program *p);
uint64_t sh_cost_for_args(const sh_program *p, const sh_value *args, uint32_t argc);

// --- invoke (the reference interpreter) -------------------------------------
// Run a VERIFIED program on typed args. Validates argc + each arg type against the
// declared params. A runtime trap (bounds) returns an error; the program can never
// corrupt memory (every access is checked). On SH_OK *out holds the typed result.
sh_status sh_invoke(const sh_program *p, const sh_value *args, uint32_t argc,
                    sh_value *out, sh_error *err);

// --- value helpers (host builds args / reads results) -----------------------
static inline sh_value sh_val_bool(bool b) {
    sh_value v = {0}; v.kind = SH_K_BOOL; v.lanes = 1; v.u = b ? 1 : 0; return v;
}
static inline sh_value sh_val_u8(uint8_t x) {
    sh_value v = {0}; v.kind = SH_K_U8; v.lanes = 1; v.u = x; return v;
}
static inline sh_value sh_val_u16(uint16_t x) {
    sh_value v = {0}; v.kind = SH_K_U16; v.lanes = 1; v.u = x; return v;
}
static inline sh_value sh_val_u32(uint32_t x) {
    sh_value v = {0}; v.kind = SH_K_U32; v.lanes = 1; v.u = x; return v;
}
static inline sh_value sh_val_u64(uint64_t x) {
    sh_value v = {0}; v.kind = SH_K_U64; v.lanes = 1; v.u = x; return v;
}
static inline sh_value sh_val_i64(int64_t x) {
    sh_value v = {0}; v.kind = SH_K_I64; v.lanes = 1; v.i = x; return v;
}
static inline sh_value sh_val_f32(float x) {
    sh_value v = {0}; v.kind = SH_K_F32; v.lanes = 1; v.f = (double)x; return v;
}
static inline sh_value sh_val_f64(double x) {
    sh_value v = {0}; v.kind = SH_K_F64; v.lanes = 1; v.f = x; return v;
}
// Wrap a lisp_bytes (or raw storage) as a typed region argument.
sh_value sh_val_region(lisp_value bytes, sh_kind elem, bool mutable_);
sh_value sh_val_region_raw(void *base, uint32_t len, sh_kind elem, bool mutable_);

#endif  // CARDINAL_LISP_SHADER_H
