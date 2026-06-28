// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

// Stackless, fuel-metered Wasm interpreter: the control-flow prepass and the
// dispatch loop. Implemented against the frozen wasm_internal.h.
//
// The interpreter keeps no native call/operand stack of its own: the operand
// stack (inst->vstack), call frames (inst->frames), and control labels
// (inst->ctrl) all live in heap arrays on the instance. The dispatch loop is a
// plain switch driven by frame->pc; on fuel exhaustion or an imported-call
// suspend it simply returns to wasm_resume() with inst->status set, leaving the
// frame/pc/stacks exactly where they were so a later wasm_resume() continues.
//
// See notes/core/wasm-guests.md.

#include "wasm_internal.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

// ---- Control-flow side table -------------------------------------------
//
// The prepass scans a function body once, matching block/loop/if/else/end. For
// every control-flow opcode that needs a precomputed target we record one
// `cf_entry`, indexed by the opcode's byte offset (pc of the opcode byte). The
// table is a flat array of length code_len; only the byte offsets that begin a
// control opcode are populated (the rest stay {.valid=0}). This trades a little
// memory for O(1), allocation-free lookup in the hot dispatch loop.
//
//   BLOCK / IF : br to this label targets the pc just AFTER the matching END.
//                We store end_pc (one past END), the arity, and (for IF) the
//                else_pc (pc just after ELSE, or == end target if no else).
//   LOOP       : br targets the loop header (pc just after the loop blocktype),
//                which is stored in target_pc; end_pc is one past the END.
//   END        : we record arity so a function-level / block END can adjust the
//                value stack. (Block ENDs simply pop a label; we read arity off
//                the label, but recording here keeps the structure uniform.)
//   BR_TABLE   : no per-pc precompute needed (targets are resolved through the
//                live control stack at run time), so it is not stored here.

typedef struct {
    uint8_t valid;     // 1 if this byte offset begins a recorded control opcode
    uint8_t kind;      // 0=block 1=loop 2=if (matches wasm_label_t.kind)
    uint32_t target_pc; // br target: loop=>header, block/if=>after END
    uint32_t else_pc;   // IF: pc just after ELSE (or == target_pc if no else)
    uint32_t end_pc;    // pc just after the matching END
    uint32_t arity;     // result-value count of the block
} cf_entry;

typedef struct {
    cf_entry *entries;  // length == code_len
    uint32_t len;
} cf_table;

// Decode a blocktype byte/s33 at *pc; return arity (result count) in *arity.
// Handles 0x40 (void), single valtype (arity 1), and an s33 type index
// (multi-value: arity = that functype's result count). Returns false on a
// malformed blocktype.
static bool decode_blocktype(const uint8_t *code, uint32_t code_len, uint32_t *pc,
                             wasm_module_t *m, uint32_t *arity) {
    if (*pc >= code_len) return false;
    uint8_t b = code[*pc];
    if (b == 0x40) {                 // void
        (*pc)++;
        *arity = 0;
        return true;
    }
    if (b == 0x7F || b == 0x7E || b == 0x7D || b == 0x7C ||
        b == 0x70 || b == 0x6F) {    // single valtype
        (*pc)++;
        *arity = 1;
        return true;
    }
    // Otherwise an s33 type index (signed LEB).
    int64_t ti;
    if (!wasm_read_s64(code, code_len, pc, &ti)) return false;
    if (ti < 0 || (uint64_t)ti >= m->n_types) return false;
    *arity = m->types[(uint32_t)ti].n_results;
    return true;
}

// Skip a blocktype without recording arity (used when re-scanning is cheaper).
static bool skip_blocktype(const uint8_t *code, uint32_t code_len, uint32_t *pc,
                           wasm_module_t *m) {
    uint32_t a;
    return decode_blocktype(code, code_len, pc, m, &a);
}

// Per-opcode immediate skipper for the prepass: advance *pc past the operands of
// the opcode whose byte is at code[*pc-1] (already consumed). Control opcodes
// (block/loop/if) consume their blocktype; everything else its LEB/imm bytes.
// Returns false on truncation.
static bool skip_immediates(const uint8_t *code, uint32_t code_len, uint32_t *pc,
                            wasm_module_t *m, uint8_t op) {
    uint32_t u;
    int32_t s32;
    int64_t s64;
    switch (op) {
    case OP_BLOCK:
    case OP_LOOP:
    case OP_IF:
        return skip_blocktype(code, code_len, pc, m);

    case OP_BR:
    case OP_BR_IF:
    case OP_CALL:
    case OP_LOCAL_GET:
    case OP_LOCAL_SET:
    case OP_LOCAL_TEE:
    case OP_GLOBAL_GET:
    case OP_GLOBAL_SET:
        return wasm_read_u32(code, code_len, pc, &u);

    case OP_BR_TABLE: {
        uint32_t n;
        if (!wasm_read_u32(code, code_len, pc, &n)) return false;
        for (uint32_t i = 0; i < n; i++)
            if (!wasm_read_u32(code, code_len, pc, &u)) return false;
        return wasm_read_u32(code, code_len, pc, &u);   // default label
    }

    case OP_CALL_INDIRECT:
        if (!wasm_read_u32(code, code_len, pc, &u)) return false; // typeidx
        return wasm_read_u32(code, code_len, pc, &u);             // tableidx

    case OP_I32_CONST:
        return wasm_read_s32(code, code_len, pc, &s32);
    case OP_I64_CONST:
        return wasm_read_s64(code, code_len, pc, &s64);
    case OP_F32_CONST:
        if (*pc + 4 > code_len) return false;
        *pc += 4;
        return true;
    case OP_F64_CONST:
        if (*pc + 8 > code_len) return false;
        *pc += 8;
        return true;

    // All memory load/store: memarg = align u32 + offset u32.
    case OP_I32_LOAD: case OP_I64_LOAD: case OP_F32_LOAD: case OP_F64_LOAD:
    case OP_I32_LOAD8_S: case OP_I32_LOAD8_U: case OP_I32_LOAD16_S: case OP_I32_LOAD16_U:
    case OP_I64_LOAD8_S: case OP_I64_LOAD8_U: case OP_I64_LOAD16_S: case OP_I64_LOAD16_U:
    case OP_I64_LOAD32_S: case OP_I64_LOAD32_U:
    case OP_I32_STORE: case OP_I64_STORE: case OP_F32_STORE: case OP_F64_STORE:
    case OP_I32_STORE8: case OP_I32_STORE16:
    case OP_I64_STORE8: case OP_I64_STORE16: case OP_I64_STORE32:
        if (!wasm_read_u32(code, code_len, pc, &u)) return false; // align
        return wasm_read_u32(code, code_len, pc, &u);             // offset

    case OP_MEMORY_SIZE:
    case OP_MEMORY_GROW:
        return wasm_read_u32(code, code_len, pc, &u);   // mem index (0x00)

    default:
        // No immediates (arithmetic, comparisons, conversions, nop, end, else,
        // return, drop, select, unreachable, ...).
        return true;
    }
}

bool wasm_exec_prepare_func(wasm_module_t *m, wasm_func_t *f, wasm_result_t *err) {
    // `m` resolves s33 multi-value blocktypes (which index module->types). The
    // module is passed in directly (not via a global) so the prepass is reentrant
    // and SMP-safe with NO thread-local storage -- the kernel module loader has
    // no ELF-TLS runtime, so __thread is unavailable here. Two cores can prepare
    // functions of two instances concurrently; cf is written once then read-only.

    const uint8_t *code = f->code;
    uint32_t code_len = f->code_len;

    cf_table *tbl = calloc(1, sizeof(cf_table));
    if (!tbl) { if (err) *err = WASM_ERR_OOM; return false; }
    tbl->len = code_len;
    tbl->entries = code_len ? calloc(code_len, sizeof(cf_entry)) : NULL;
    if (code_len && !tbl->entries) {
        free(tbl);
        if (err) *err = WASM_ERR_OOM;
        return false;
    }

    // Open-control stack of pcs (opcode byte offsets) awaiting their END.
    // Implicit function body block is NOT recorded here (its END is handled as a
    // function-level return by the dispatch loop).
    uint32_t *open = code_len ? malloc(sizeof(uint32_t) * (code_len + 1))
                              : malloc(sizeof(uint32_t));
    if (!open) {
        free(tbl->entries);
        free(tbl);
        if (err) *err = WASM_ERR_OOM;
        return false;
    }
    uint32_t depth = 0;

    uint32_t pc = 0;
    bool ok = true;
    while (pc < code_len) {
        uint32_t op_pc = pc;
        uint8_t op = code[pc++];

        if (op == OP_BLOCK || op == OP_LOOP || op == OP_IF) {
            uint32_t arity = 0;
            if (!decode_blocktype(code, code_len, &pc, m, &arity)) { ok = false; break; }
            cf_entry *e = &tbl->entries[op_pc];
            e->valid = 1;
            e->arity = arity;
            if (op == OP_BLOCK) e->kind = 0;
            else if (op == OP_LOOP) e->kind = 1;
            else e->kind = 2;
            // LOOP header = pc immediately after the blocktype.
            if (op == OP_LOOP) e->target_pc = pc;
            // else_pc default: no else -> patched to end target at END.
            e->else_pc = 0;
            e->end_pc = 0;            // patched at matching END
            if (depth > code_len) { ok = false; break; }
            open[depth++] = op_pc;
        } else if (op == OP_ELSE) {
            if (depth == 0) { ok = false; break; }
            uint32_t owner = open[depth - 1];
            cf_entry *oe = &tbl->entries[owner];
            if (oe->kind != 2) { ok = false; break; }   // ELSE without IF
            // else target = pc just after the ELSE byte.
            oe->else_pc = pc;
        } else if (op == OP_END) {
            if (depth == 0) {
                // Function-level END: must be the final byte. Allow trailing.
                continue;
            }
            uint32_t owner = open[--depth];
            cf_entry *oe = &tbl->entries[owner];
            oe->end_pc = pc;          // pc is just after the END byte
            if (oe->kind == 1) {
                // LOOP: br target already set to header; end_pc recorded above.
            } else {
                // BLOCK / IF: br target = pc after END.
                oe->target_pc = pc;
                if (oe->kind == 2 && oe->else_pc == 0) {
                    // IF with no ELSE: a false condition jumps past the END.
                    oe->else_pc = pc;
                }
            }
        } else {
            if (!skip_immediates(code, code_len, &pc, m, op)) { ok = false; break; }
        }
    }

    if (ok && depth != 0) ok = false;   // unbalanced block/end

    free(open);
    if (!ok) {
        free(tbl->entries);
        free(tbl);
        if (err) *err = WASM_ERR_VALIDATE;
        return false;
    }

    f->cf = tbl;
    if (err) *err = WASM_OK;
    return true;
}

// ---- Freestanding IEEE-754 math -----------------------------------------
//
// The kernel build is -ffreestanding with no libm, so we implement every
// float primitive the interpreter needs here from bit ops and the SSE2 sqrt
// instruction. The SAME code is used in the host build (no #ifdef split) so
// the host suite exercises it. The semantics below are exactly what Wasm
// requires (round-toward-zero trunc, round-to-{-inf,+inf} floor/ceil,
// round-to-nearest-ties-to-even for `nearest`, sign-bit fabs/copysign).
//
// Pattern mirrors libs/ttf/ttf.c's minimal libm.

// sqrt via the bare SSE instruction (NOT __builtin_sqrt: at -O0 the builtin
// lowers to a CALL to `sqrt`, a missing symbol freestanding). Requires -msse2.
static inline double wm_sqrt(double x) {
    double r;
    __asm__("sqrtsd %1, %0" : "=x"(r) : "x"(x));
    return r;
}
static inline float wm_sqrtf(float x) {
    float r;
    __asm__("sqrtss %1, %0" : "=x"(r) : "x"(x));
    return r;
}

// Bit punning helpers (exact, no aliasing UB: copy through memcpy).
static inline uint64_t wm_d2b(double x) { uint64_t b; memcpy(&b, &x, 8); return b; }
static inline double   wm_b2d(uint64_t b) { double x; memcpy(&x, &b, 8); return x; }
static inline uint32_t wm_f2b(float x)  { uint32_t b; memcpy(&b, &x, 4); return b; }
static inline float    wm_b2f(uint32_t b) { float x; memcpy(&x, &b, 4); return x; }

// NaN constants built from bits (no NAN macro under -ffreestanding).
static inline double wm_nan(void)  { return wm_b2d(0x7FF8000000000000ull); }
static inline float  wm_nanf(void) { return wm_b2f(0x7FC00000u); }

static inline int wm_isnan(double x)  { return x != x; }
static inline int wm_isnanf(float x)  { return x != x; }

// signbit: top bit of the bit pattern (correct for -0.0 and NaN too).
static inline int wm_signbit(double x)  { return (int)(wm_d2b(x) >> 63); }
static inline int wm_signbitf(float x)  { return (int)(wm_f2b(x) >> 31); }

// fabs / copysign via the sign bit.
static inline double wm_fabs(double x)  { return wm_b2d(wm_d2b(x) & 0x7FFFFFFFFFFFFFFFull); }
static inline float  wm_fabsf(float x)  { return wm_b2f(wm_f2b(x) & 0x7FFFFFFFu); }
static inline double wm_copysign(double a, double b) {
    return wm_b2d((wm_d2b(a) & 0x7FFFFFFFFFFFFFFFull) | (wm_d2b(b) & 0x8000000000000000ull));
}
static inline float wm_copysignf(float a, float b) {
    return wm_b2f((wm_f2b(a) & 0x7FFFFFFFu) | (wm_f2b(b) & 0x80000000u));
}

// |x| >= 2^52 (f64) / 2^23 (f32) means x is already an exact integer; this
// magnitude test also catches +/-inf (whose magnitude exceeds the threshold)
// and lets NaN fall through to the explicit isnan check. For everything else
// we round-trip through int64 (toward zero) and adjust per rounding mode,
// re-applying x's sign with copysign so -0.0 and small negatives are exact.

static double wm_trunc(double x) {
    if (wm_isnan(x) || wm_fabs(x) >= 4503599627370496.0) return x; // 2^52 / inf / nan
    double t = (double)(int64_t)x;                                  // toward zero
    return wm_copysign(t, x);                                       // keep -0.0
}
static float wm_truncf(float x) {
    if (wm_isnanf(x) || wm_fabsf(x) >= 8388608.0f) return x;        // 2^23 / inf / nan
    float t = (float)(int32_t)x;
    return wm_copysignf(t, x);
}

static double wm_floor(double x) {
    if (wm_isnan(x) || wm_fabs(x) >= 4503599627370496.0) return x;
    double t = wm_trunc(x);
    if (t > x) t -= 1.0;
    return wm_copysign(t, x);   // floor(-0.0) = -0.0
}
static float wm_floorf(float x) {
    if (wm_isnanf(x) || wm_fabsf(x) >= 8388608.0f) return x;
    float t = wm_truncf(x);
    if (t > x) t -= 1.0f;
    return wm_copysignf(t, x);
}

static double wm_ceil(double x) {
    if (wm_isnan(x) || wm_fabs(x) >= 4503599627370496.0) return x;
    double t = wm_trunc(x);
    if (t < x) t += 1.0;
    return wm_copysign(t, x);   // ceil(-0.0) = -0.0
}
static float wm_ceilf(float x) {
    if (wm_isnanf(x) || wm_fabsf(x) >= 8388608.0f) return x;
    float t = wm_truncf(x);
    if (t < x) t += 1.0f;
    return wm_copysignf(t, x);
}

// Round to nearest, ties to even (IEEE roundTiesToEven == Wasm `nearest`).
static double wm_nearbyint(double x) {
    if (wm_isnan(x) || wm_fabs(x) >= 4503599627370496.0) return x;
    double t = wm_trunc(x);          // integer part toward zero
    double d = wm_fabs(x - t);       // fractional magnitude in [0,1)
    if (d < 0.5) return wm_copysign(t, x);
    if (d > 0.5) {
        double a = wm_fabs(t) + 1.0;
        return wm_copysign(a, x);
    }
    // exactly halfway: pick the even neighbour
    int64_t ti = (int64_t)t;
    if (ti & 1) {                    // t is odd -> step away from zero to even
        double a = wm_fabs(t) + 1.0;
        return wm_copysign(a, x);
    }
    return wm_copysign(t, x);        // t already even
}
static float wm_nearbyintf(float x) {
    if (wm_isnanf(x) || wm_fabsf(x) >= 8388608.0f) return x;
    float t = wm_truncf(x);
    float d = wm_fabsf(x - t);
    if (d < 0.5f) return wm_copysignf(t, x);
    if (d > 0.5f) {
        float a = wm_fabsf(t) + 1.0f;
        return wm_copysignf(a, x);
    }
    int32_t ti = (int32_t)t;
    if (ti & 1) {
        float a = wm_fabsf(t) + 1.0f;
        return wm_copysignf(a, x);
    }
    return wm_copysignf(t, x);
}

// ---- Float helpers ------------------------------------------------------

static float f32_nearest(float x) {
    // round-half-to-even (Wasm "nearest")
    float r = wm_nearbyintf(x);
    return r == 0.0f ? wm_copysignf(0.0f, x) : r;
}
static double f64_nearest(double x) {
    double r = wm_nearbyint(x);
    return r == 0.0 ? wm_copysign(0.0, x) : r;
}
static float f32_trunc(float x) { return wm_truncf(x); }
static double f64_trunc(double x) { return wm_trunc(x); }

static float wasm_f32_min(float a, float b) {
    if (wm_isnanf(a) || wm_isnanf(b)) return wm_nanf();
    if (a == 0.0f && b == 0.0f) return wm_signbitf(a) ? a : b; // -0 < +0
    return a < b ? a : b;
}
static float wasm_f32_max(float a, float b) {
    if (wm_isnanf(a) || wm_isnanf(b)) return wm_nanf();
    if (a == 0.0f && b == 0.0f) return wm_signbitf(a) ? b : a;
    return a > b ? a : b;
}
static double wasm_f64_min(double a, double b) {
    if (wm_isnan(a) || wm_isnan(b)) return wm_nan();
    if (a == 0.0 && b == 0.0) return wm_signbit(a) ? a : b;
    return a < b ? a : b;
}
static double wasm_f64_max(double a, double b) {
    if (wm_isnan(a) || wm_isnan(b)) return wm_nan();
    if (a == 0.0 && b == 0.0) return wm_signbit(a) ? b : a;
    return a > b ? a : b;
}

// ---- Frame / signature helpers -----------------------------------------

// Resolve a function index to its functype. Mirrors func_signature in
// wasm_instance.c (which is static there).
static const wasm_functype_t *sig_of(wasm_module_t *m, uint32_t func_index) {
    uint32_t ti;
    if (func_index < m->n_imported_funcs) {
        uint32_t seen = 0;
        for (uint32_t i = 0; i < m->n_imports; i++) {
            if (m->imports[i].kind != WASM_EXTERN_FUNC) continue;
            if (seen == func_index) { ti = m->imports[i].type_index; goto got; }
            seen++;
        }
        return NULL;
    }
    ti = m->funcs[func_index - m->n_imported_funcs].type_index;
got:
    if (ti >= m->n_types) return NULL;
    return &m->types[ti];
}

// Map a function-index-space func import to its resolved-import slot. The
// resolved-import array (inst->imports) is parallel to module->imports filtered
// to func imports; v1 only has func imports, so the i-th func import is
// inst->imports[that import's array slot]. We find the slot for the n-th func
// import.
static wasm_resolved_import_t *resolved_func_import(wasm_instance_t *inst,
                                                    uint32_t func_index) {
    wasm_module_t *m = inst->module;
    uint32_t seen = 0;
    for (uint32_t i = 0; i < m->n_imports; i++) {
        if (m->imports[i].kind != WASM_EXTERN_FUNC) continue;
        if (seen == func_index) return &inst->imports[i];
        seen++;
    }
    return NULL;
}

#define TRAP(t) do { inst->trap = (t); inst->status = WASM_RUN_TRAPPED; return; } while (0)

// Push a new defined-function frame. Returns false (after setting a trap) on
// overflow. On success, *pframe is updated to point at the new top frame.
static bool enter_defined(wasm_instance_t *inst, uint32_t func_index,
                          wasm_frame_t **pframe) {
    wasm_module_t *m = inst->module;
    if (inst->fsp >= WASM_FRAME_CAP) {
        inst->trap = WASM_TRAP_CALL_STACK_EXHAUSTED;
        inst->status = WASM_RUN_TRAPPED;
        return false;
    }
    // Bounds-check the function index before indexing m->funcs[] (item 4). A
    // crafted call / call_indirect can name an index past the defined functions.
    if (func_index < m->n_imported_funcs ||
        func_index >= m->n_imported_funcs + m->n_funcs) {
        inst->trap = WASM_TRAP_UNREACHABLE;
        inst->status = WASM_RUN_TRAPPED;
        return false;
    }
    wasm_func_t *f = &m->funcs[func_index - m->n_imported_funcs];
    if (!f->cf) {
        wasm_result_t e = WASM_OK;
        if (!wasm_exec_prepare_func(m, f, &e)) {
            inst->trap = WASM_TRAP_UNREACHABLE;   // malformed body -> trap
            inst->status = WASM_RUN_TRAPPED;
            return false;
        }
    }
    const wasm_functype_t *sig = sig_of(m, func_index);
    uint32_t n_params = f->n_params;
    uint32_t n_locals = f->n_locals;
    // Args are the top n_params operands; locals_base sits beneath them.
    uint32_t locals_base = inst->vsp - n_params;
    // Zero the declared (non-param) locals.
    if (locals_base + n_locals > WASM_VSTACK_CAP) {
        inst->trap = WASM_TRAP_STACK_OVERFLOW;
        inst->status = WASM_RUN_TRAPPED;
        return false;
    }
    for (uint32_t i = n_params; i < n_locals; i++) {
        wasm_value_t z = {0};
        inst->vstack[locals_base + i] = z;
    }
    inst->vsp = locals_base + n_locals;

    wasm_frame_t *nf = &inst->frames[inst->fsp++];
    nf->func_index = func_index;
    nf->code = f->code;
    nf->code_len = f->code_len;
    nf->pc = 0;
    nf->locals_base = locals_base;
    nf->operand_base = locals_base + n_locals;
    nf->ctrl_base = inst->csp;
    nf->result_arity = sig ? sig->n_results : 0;
    *pframe = nf;
    return true;
}

// Handle a call to an imported function (func_index < n_imported_funcs). Pops
// the n_params arguments. For a synchronous (fn != NULL) import, invokes it and
// pushes the results, continuing execution; a non-NONE trap from the handler
// stops the VM. For a host-serviced (fn == NULL) import, stashes args into
// inst->pending_args, fills inst->pending, sets WASM_RUN_SUSPENDED and returns
// (pc is already past the call). Returns false when the caller must return to
// wasm_resume() immediately (suspend, trap, or an internal stack fault); true
// when execution continued synchronously.
static bool do_call_import(wasm_instance_t *inst, uint32_t func_index) {
    wasm_module_t *m = inst->module;
    const wasm_functype_t *sig = sig_of(m, func_index);
    if (!sig) { inst->trap = WASM_TRAP_HOST; inst->status = WASM_RUN_TRAPPED; return false; }
    uint32_t n_params = sig->n_params;
    uint32_t n_results = sig->n_results;
    if (n_params > 16 || n_results > 16) {
        inst->trap = WASM_TRAP_HOST;
        inst->status = WASM_RUN_TRAPPED;
        return false;
    }
    if (inst->vsp < n_params) {
        inst->trap = WASM_TRAP_STACK_OVERFLOW;
        inst->status = WASM_RUN_TRAPPED;
        return false;
    }

    wasm_resolved_import_t *ri = resolved_func_import(inst, func_index);
    if (!ri) { inst->trap = WASM_TRAP_HOST; inst->status = WASM_RUN_TRAPPED; return false; }

    // Pop args into a scratch buffer (in source order: args[0] is deepest).
    wasm_value_t args[16];
    for (uint32_t i = 0; i < n_params; i++)
        args[i] = inst->vstack[inst->vsp - n_params + i];
    inst->vsp -= n_params;

    if (ri->fn) {
        // Synchronous host handler.
        wasm_value_t results[16];
        for (uint32_t i = 0; i < n_results; i++) {
            wasm_value_t z = {0};
            results[i] = z;
        }
        wasm_trap_t t = ri->fn(inst, ri->user, args, results);
        if (t != WASM_TRAP_NONE) {
            inst->trap = t;
            inst->status = WASM_RUN_TRAPPED;
            return false;
        }
        for (uint32_t i = 0; i < n_results; i++) {
            wasm_push(inst, results[i]);
            if (inst->status == WASM_RUN_TRAPPED) return false;
        }
        return true;
    }

    // Host-serviced: suspend. pc is already advanced past the call.
    for (uint32_t i = 0; i < n_params; i++) inst->pending_args[i] = args[i];
    inst->pending_func_index = func_index;  // so wasm_pending_*_types can recover the signature
    inst->pending.host_id = ri->host_id;
    inst->pending.module_name = ri->module_name;
    inst->pending.field_name = ri->field_name;
    inst->pending.args = inst->pending_args;
    inst->pending.n_args = n_params;
    inst->pending.n_results = n_results;
    inst->status = WASM_RUN_SUSPENDED;
    return false;
}

void wasm_exec_free_func(wasm_func_t *f) {
    if (!f || !f->cf) return;
    cf_table *tbl = (cf_table *)f->cf;
    free(tbl->entries);
    free(tbl);
    f->cf = NULL;
}

void wasm_exec_run(wasm_instance_t *inst, int64_t fuel) {
    (void)fuel;   // already copied into inst->fuel by wasm_resume
    wasm_module_t *m = inst->module;
    wasm_label_t *ctrl = (wasm_label_t *)inst->ctrl;

    // ---- ENTRY: build frame 0 from the staged entry call. ----
    if (inst->entry_pending) {
        // Bounds-check the staged entry index before indexing m->funcs[] (item 4,
        // defense in depth -- wasm_call already validates, but never trust it).
        if (inst->entry_func < m->n_imported_funcs ||
            inst->entry_func >= m->n_imported_funcs + m->n_funcs) {
            inst->trap = WASM_TRAP_UNREACHABLE;
            inst->status = WASM_RUN_TRAPPED;
            return;
        }
        wasm_func_t *f = &m->funcs[inst->entry_func - m->n_imported_funcs];
        if (!f->cf) {
            wasm_result_t e = WASM_OK;
            if (!wasm_exec_prepare_func(m, f, &e)) {
                inst->trap = WASM_TRAP_UNREACHABLE;
                inst->status = WASM_RUN_TRAPPED;
                return;
            }
        }
        const wasm_functype_t *sig = sig_of(m, inst->entry_func);
        uint32_t n_params = f->n_params;
        uint32_t n_locals = f->n_locals;
        // Cap the local frame to the value-stack capacity (item 5). enter_defined
        // checks this; the entry path must too, or a huge n_locals overruns vstack.
        if (n_locals > WASM_VSTACK_CAP) {
            inst->trap = WASM_TRAP_STACK_OVERFLOW;
            inst->status = WASM_RUN_TRAPPED;
            return;
        }
        // entry args already at vstack[0..entry_nargs); entry_nargs == n_params.
        for (uint32_t i = n_params; i < n_locals; i++) {
            wasm_value_t z = {0};
            inst->vstack[i] = z;
        }
        inst->vsp = n_locals;

        wasm_frame_t *fr = &inst->frames[0];
        fr->func_index = inst->entry_func;
        fr->code = f->code;
        fr->code_len = f->code_len;
        fr->pc = 0;
        fr->locals_base = 0;
        fr->operand_base = n_locals;
        fr->ctrl_base = 0;          // csp is 0
        fr->result_arity = sig ? sig->n_results : 0;
        inst->fsp = 1;
        inst->csp = 0;
        inst->entry_pending = false;
    }

    // ---- RESUME from a suspended import: push the provided results. ----
    if (inst->has_provided) {
        for (uint32_t i = 0; i < inst->n_provided; i++)
            wasm_push(inst, inst->provided[i]);
        inst->has_provided = false;
        inst->n_provided = 0;
        if (inst->status == WASM_RUN_TRAPPED) return;   // push overflow
    }

    inst->status = WASM_RUN_FUEL;   // running

    wasm_frame_t *frame = &inst->frames[inst->fsp - 1];
    uint32_t br_depth = 0;          // shared by the BR/BR_IF/BR_TABLE -> do_branch path

    for (;;) {
        if (inst->fuel <= 0) {
            inst->status = WASM_RUN_FUEL;
            return;
        }
        if (frame->pc >= frame->code_len) {
            // Reaching the end of the body without an explicit END acts as a
            // function-level return (the implicit body END).
            goto func_return;
        }
        uint8_t op = frame->code[frame->pc++];

        switch (op) {

        case OP_UNREACHABLE:
            TRAP(WASM_TRAP_UNREACHABLE);
        case OP_NOP:
            break;

        // ---- Structured control ----
        case OP_BLOCK:
        case OP_LOOP:
        case OP_IF: {
            uint32_t op_pc = frame->pc - 1;
            cf_table *tbl = (cf_table *)m->funcs[frame->func_index -
                                                 m->n_imported_funcs].cf;
            cf_entry *e = &tbl->entries[op_pc];
            // Advance pc past the blocktype (entries don't store its length, so
            // re-skip; cheap and avoids a second field).
            if (!skip_blocktype(frame->code, frame->code_len, &frame->pc, m))
                TRAP(WASM_TRAP_UNREACHABLE);

            if (op == OP_IF) {
                wasm_value_t c = wasm_pop(inst);
                if (inst->status == WASM_RUN_TRAPPED) return;
                // Push the label regardless (br within either arm targets END).
                if (inst->csp >= WASM_CTRL_CAP) TRAP(WASM_TRAP_STACK_OVERFLOW);
                wasm_label_t *lbl = &ctrl[inst->csp++];
                lbl->kind = 2;
                lbl->target_pc = e->target_pc;
                lbl->else_pc = e->else_pc;
                lbl->end_pc = e->end_pc;
                lbl->arity = e->arity;
                lbl->height = inst->vsp;
                if (c.i32 == 0) {
                    // jump to the else body (or past END if no else)
                    frame->pc = e->else_pc;
                }
            } else {
                if (inst->csp >= WASM_CTRL_CAP) TRAP(WASM_TRAP_STACK_OVERFLOW);
                wasm_label_t *lbl = &ctrl[inst->csp++];
                lbl->kind = e->kind;        // 0 block / 1 loop
                lbl->target_pc = e->target_pc;  // block=>after END, loop=>header
                lbl->else_pc = 0;
                lbl->end_pc = e->end_pc;
                lbl->arity = e->arity;
                lbl->height = inst->vsp;
            }
            break;
        }

        case OP_ELSE: {
            // Reached only by falling out of the THEN arm: skip to END.
            if (inst->csp <= frame->ctrl_base) TRAP(WASM_TRAP_UNREACHABLE);
            wasm_label_t *lbl = &ctrl[inst->csp - 1];
            frame->pc = lbl->end_pc;        // jump past the matching END
            // The label remains; the END below will pop it.
            inst->csp--;                    // we leap over END, so pop here
            break;
        }

        case OP_END: {
            if (inst->csp <= frame->ctrl_base) {
                // Function-level END.
                goto func_return;
            }
            // Block/loop/if structural END: just pop the label. Operand-stack
            // results are already on top in source order.
            inst->csp--;
            break;
        }

        case OP_BR: {
            if (!wasm_read_u32(frame->code, frame->code_len, &frame->pc, &br_depth))
                TRAP(WASM_TRAP_UNREACHABLE);
            goto do_branch;
        }

        case OP_BR_IF: {
            if (!wasm_read_u32(frame->code, frame->code_len, &frame->pc, &br_depth))
                TRAP(WASM_TRAP_UNREACHABLE);
            wasm_value_t c = wasm_pop(inst);
            if (inst->status == WASM_RUN_TRAPPED) return;
            if (c.i32 != 0) goto do_branch;
            break;
        }

        case OP_BR_TABLE: {
            uint32_t n;
            if (!wasm_read_u32(frame->code, frame->code_len, &frame->pc, &n))
                TRAP(WASM_TRAP_UNREACHABLE);
            wasm_value_t iv = wasm_pop(inst);
            if (inst->status == WASM_RUN_TRAPPED) return;
            uint32_t sel = iv.u32;
            uint32_t chosen = 0;
            bool have = false;
            for (uint32_t i = 0; i < n; i++) {
                uint32_t d;
                if (!wasm_read_u32(frame->code, frame->code_len, &frame->pc, &d))
                    TRAP(WASM_TRAP_UNREACHABLE);
                if (i == sel) { chosen = d; have = true; }
            }
            uint32_t def;
            if (!wasm_read_u32(frame->code, frame->code_len, &frame->pc, &def))
                TRAP(WASM_TRAP_UNREACHABLE);
            br_depth = have ? chosen : def;
            // Charge fuel proportional to the table width scanned (item 13) so a
            // pathologically wide br_table cannot run unbounded without fuel cost.
            inst->fuel -= (int64_t)n;
            goto do_branch;
        }

        do_branch: {
            // Shared branch target: br_depth is the label depth (0 = innermost).
            if (inst->csp < frame->ctrl_base + br_depth + 1) {
                // Branch past the outermost label == function return.
                goto func_return;
            }
            uint32_t target = inst->csp - 1 - br_depth;
            wasm_label_t *lbl = &ctrl[target];
            // br to a LOOP targets the header and keeps the loop's parameter
            // count (0 in MVP); br to a block/if keeps that block's result
            // arity. So keep 0 for loops, lbl->arity otherwise.
            bool is_loop = (lbl->kind == 1);
            uint32_t keep = is_loop ? 0 : lbl->arity;

            if (keep > inst->vsp - lbl->height) keep = inst->vsp - lbl->height;
            for (uint32_t i = 0; i < keep; i++)
                inst->vstack[lbl->height + i] = inst->vstack[inst->vsp - keep + i];
            inst->vsp = lbl->height + keep;

            if (is_loop) {
                inst->csp = target + 1;     // keep the loop label; re-enter
                inst->fuel--;               // back-edge consumes fuel
            } else {
                inst->csp = target;         // pop through the target label
            }
            frame->pc = lbl->target_pc;
            break;
        }

        case OP_RETURN:
            goto func_return;

        // ---- Calls ----
        case OP_CALL: {
            uint32_t funcidx;
            if (!wasm_read_u32(frame->code, frame->code_len, &frame->pc, &funcidx))
                TRAP(WASM_TRAP_UNREACHABLE);
            inst->fuel--;
            if (funcidx < m->n_imported_funcs) {
                if (!do_call_import(inst, funcidx)) return;
                if (inst->status != WASM_RUN_FUEL) return;  // suspended/trapped/done
                // frame unchanged (synchronous import returned)
            } else {
                if (!enter_defined(inst, funcidx, &frame)) return;
            }
            break;
        }

        case OP_CALL_INDIRECT: {
            uint32_t typeidx, tableidx;
            if (!wasm_read_u32(frame->code, frame->code_len, &frame->pc, &typeidx))
                TRAP(WASM_TRAP_UNREACHABLE);
            if (!wasm_read_u32(frame->code, frame->code_len, &frame->pc, &tableidx))
                TRAP(WASM_TRAP_UNREACHABLE);
            (void)tableidx;   // v1: single table 0
            wasm_value_t ev = wasm_pop(inst);
            if (inst->status == WASM_RUN_TRAPPED) return;
            uint32_t elem = ev.u32;
            if (elem >= inst->table_len) TRAP(WASM_TRAP_OOB_TABLE);
            uint32_t funcidx = inst->table[elem];
            if (funcidx == 0xFFFFFFFFu) TRAP(WASM_TRAP_UNINIT_ELEMENT);
            // Type check.
            if (typeidx >= m->n_types) TRAP(WASM_TRAP_INDIRECT_TYPE_MISMATCH);
            const wasm_functype_t *want = &m->types[typeidx];
            const wasm_functype_t *got = sig_of(m, funcidx);
            if (!got || got->n_params != want->n_params ||
                got->n_results != want->n_results) {
                TRAP(WASM_TRAP_INDIRECT_TYPE_MISMATCH);
            }
            for (uint32_t i = 0; i < want->n_params; i++)
                if (got->params[i] != want->params[i])
                    TRAP(WASM_TRAP_INDIRECT_TYPE_MISMATCH);
            for (uint32_t i = 0; i < want->n_results; i++)
                if (got->results[i] != want->results[i])
                    TRAP(WASM_TRAP_INDIRECT_TYPE_MISMATCH);
            inst->fuel--;
            if (funcidx < m->n_imported_funcs) {
                if (!do_call_import(inst, funcidx)) return;
                if (inst->status != WASM_RUN_FUEL) return;
            } else {
                if (!enter_defined(inst, funcidx, &frame)) return;
            }
            break;
        }

        // ---- Parametric ----
        case OP_DROP:
            (void)wasm_pop(inst);
            if (inst->status == WASM_RUN_TRAPPED) return;
            break;
        case OP_SELECT: {
            wasm_value_t c = wasm_pop(inst);
            wasm_value_t b = wasm_pop(inst);
            wasm_value_t a = wasm_pop(inst);
            if (inst->status == WASM_RUN_TRAPPED) return;
            wasm_push(inst, c.i32 != 0 ? a : b);
            break;
        }

        // ---- Locals / globals ----
        case OP_LOCAL_GET: {
            uint32_t idx;
            if (!wasm_read_u32(frame->code, frame->code_len, &frame->pc, &idx))
                TRAP(WASM_TRAP_UNREACHABLE);
            // n_locals for the running frame == operand_base - locals_base
            // (item 1: bounds-check before indexing vstack).
            if (idx >= frame->operand_base - frame->locals_base)
                TRAP(WASM_TRAP_UNREACHABLE);
            wasm_push(inst, inst->vstack[frame->locals_base + idx]);
            break;
        }
        case OP_LOCAL_SET: {
            uint32_t idx;
            if (!wasm_read_u32(frame->code, frame->code_len, &frame->pc, &idx))
                TRAP(WASM_TRAP_UNREACHABLE);
            if (idx >= frame->operand_base - frame->locals_base)   // item 1
                TRAP(WASM_TRAP_UNREACHABLE);
            wasm_value_t v = wasm_pop(inst);
            if (inst->status == WASM_RUN_TRAPPED) return;
            inst->vstack[frame->locals_base + idx] = v;
            break;
        }
        case OP_LOCAL_TEE: {
            uint32_t idx;
            if (!wasm_read_u32(frame->code, frame->code_len, &frame->pc, &idx))
                TRAP(WASM_TRAP_UNREACHABLE);
            if (idx >= frame->operand_base - frame->locals_base)   // item 1
                TRAP(WASM_TRAP_UNREACHABLE);
            if (inst->vsp == 0) TRAP(WASM_TRAP_STACK_UNDERFLOW);   // item 2: peek guard
            wasm_value_t v = inst->vstack[inst->vsp - 1];   // peek
            inst->vstack[frame->locals_base + idx] = v;
            break;
        }
        case OP_GLOBAL_GET: {
            uint32_t idx;
            if (!wasm_read_u32(frame->code, frame->code_len, &frame->pc, &idx))
                TRAP(WASM_TRAP_UNREACHABLE);
            if (idx >= inst->n_globals) TRAP(WASM_TRAP_UNREACHABLE);   // item 3
            wasm_push(inst, inst->globals[idx]);
            break;
        }
        case OP_GLOBAL_SET: {
            uint32_t idx;
            if (!wasm_read_u32(frame->code, frame->code_len, &frame->pc, &idx))
                TRAP(WASM_TRAP_UNREACHABLE);
            if (idx >= inst->n_globals) TRAP(WASM_TRAP_UNREACHABLE);   // item 3
            wasm_value_t v = wasm_pop(inst);
            if (inst->status == WASM_RUN_TRAPPED) return;
            inst->globals[idx] = v;
            break;
        }

        // ---- Memory loads/stores ----
        // memarg = align u32 + offset u32. effective addr = base(i32) + offset.
#define READ_MEMARG(offv)                                                       \
        do {                                                                  \
            uint32_t _align;                                                 \
            if (!wasm_read_u32(frame->code, frame->code_len, &frame->pc, &_align)) \
                TRAP(WASM_TRAP_UNREACHABLE);                                 \
            if (!wasm_read_u32(frame->code, frame->code_len, &frame->pc, &(offv))) \
                TRAP(WASM_TRAP_UNREACHABLE);                                 \
        } while (0)

#define LOAD(TY, FIELD, NBYTES, EXTEND)                                         \
        do {                                                                  \
            uint32_t _off;                                                   \
            READ_MEMARG(_off);                                               \
            wasm_value_t _b = wasm_pop(inst);                               \
            if (inst->status == WASM_RUN_TRAPPED) return;                    \
            uint64_t _ea = (uint64_t)_b.u32 + _off;                         \
            if (_ea > 0xFFFFFFFFu) TRAP(WASM_TRAP_OOB_MEMORY);             \
            uint8_t *_p = wasm_mem_ea(inst, (uint32_t)_ea, NBYTES);        \
            if (!_p) return;                                                 \
            TY _raw;                                                         \
            memcpy(&_raw, _p, NBYTES);                                       \
            wasm_value_t _r;                                                 \
            _r.FIELD = EXTEND;                                              \
            wasm_push(inst, _r);                                            \
        } while (0)

#define STORE(TY, FIELD, NBYTES)                                                \
        do {                                                                  \
            uint32_t _off;                                                   \
            READ_MEMARG(_off);                                               \
            wasm_value_t _v = wasm_pop(inst);                               \
            wasm_value_t _b = wasm_pop(inst);                               \
            if (inst->status == WASM_RUN_TRAPPED) return;                    \
            uint64_t _ea = (uint64_t)_b.u32 + _off;                         \
            if (_ea > 0xFFFFFFFFu) TRAP(WASM_TRAP_OOB_MEMORY);             \
            uint8_t *_p = wasm_mem_ea(inst, (uint32_t)_ea, NBYTES);        \
            if (!_p) return;                                                 \
            TY _raw = (TY)_v.FIELD;                                         \
            memcpy(_p, &_raw, NBYTES);                                       \
        } while (0)

        case OP_I32_LOAD:    LOAD(uint32_t, u32, 4, _raw); break;
        case OP_I64_LOAD:    LOAD(uint64_t, u64, 8, _raw); break;
        case OP_F32_LOAD:    LOAD(uint32_t, u32, 4, _raw); break;
        case OP_F64_LOAD:    LOAD(uint64_t, u64, 8, _raw); break;
        case OP_I32_LOAD8_S: LOAD(int8_t,  i32, 1, (int32_t)_raw); break;
        case OP_I32_LOAD8_U: LOAD(uint8_t, u32, 1, (uint32_t)_raw); break;
        case OP_I32_LOAD16_S:LOAD(int16_t, i32, 2, (int32_t)_raw); break;
        case OP_I32_LOAD16_U:LOAD(uint16_t,u32, 2, (uint32_t)_raw); break;
        case OP_I64_LOAD8_S: LOAD(int8_t,  i64, 1, (int64_t)_raw); break;
        case OP_I64_LOAD8_U: LOAD(uint8_t, u64, 1, (uint64_t)_raw); break;
        case OP_I64_LOAD16_S:LOAD(int16_t, i64, 2, (int64_t)_raw); break;
        case OP_I64_LOAD16_U:LOAD(uint16_t,u64, 2, (uint64_t)_raw); break;
        case OP_I64_LOAD32_S:LOAD(int32_t, i64, 4, (int64_t)_raw); break;
        case OP_I64_LOAD32_U:LOAD(uint32_t,u64, 4, (uint64_t)_raw); break;

        case OP_I32_STORE:   STORE(uint32_t, u32, 4); break;
        case OP_I64_STORE:   STORE(uint64_t, u64, 8); break;
        case OP_F32_STORE:   STORE(uint32_t, u32, 4); break;
        case OP_F64_STORE:   STORE(uint64_t, u64, 8); break;
        case OP_I32_STORE8:  STORE(uint8_t,  u32, 1); break;
        case OP_I32_STORE16: STORE(uint16_t, u32, 2); break;
        case OP_I64_STORE8:  STORE(uint8_t,  u64, 1); break;
        case OP_I64_STORE16: STORE(uint16_t, u64, 2); break;
        case OP_I64_STORE32: STORE(uint32_t, u64, 4); break;

        case OP_MEMORY_SIZE: {
            uint32_t mi;
            if (!wasm_read_u32(frame->code, frame->code_len, &frame->pc, &mi))
                TRAP(WASM_TRAP_UNREACHABLE);
            (void)mi;
            wasm_value_t r;
            r.u32 = inst->cur_pages;
            wasm_push(inst, r);
            break;
        }
        case OP_MEMORY_GROW: {
            uint32_t mi;
            if (!wasm_read_u32(frame->code, frame->code_len, &frame->pc, &mi))
                TRAP(WASM_TRAP_UNREACHABLE);
            (void)mi;
            wasm_value_t d = wasm_pop(inst);
            if (inst->status == WASM_RUN_TRAPPED) return;
            wasm_value_t r;
            r.u32 = wasm_mem_grow(inst, d.u32);
            wasm_push(inst, r);
            break;
        }

        // ---- Constants ----
        case OP_I32_CONST: {
            int32_t v;
            if (!wasm_read_s32(frame->code, frame->code_len, &frame->pc, &v))
                TRAP(WASM_TRAP_UNREACHABLE);
            wasm_value_t r; r.i32 = v; wasm_push(inst, r);
            break;
        }
        case OP_I64_CONST: {
            int64_t v;
            if (!wasm_read_s64(frame->code, frame->code_len, &frame->pc, &v))
                TRAP(WASM_TRAP_UNREACHABLE);
            wasm_value_t r; r.i64 = v; wasm_push(inst, r);
            break;
        }
        case OP_F32_CONST: {
            if (frame->pc + 4 > frame->code_len) TRAP(WASM_TRAP_UNREACHABLE);
            uint32_t bits;
            memcpy(&bits, frame->code + frame->pc, 4);
            frame->pc += 4;
            wasm_value_t r; r.u32 = bits; wasm_push(inst, r);
            break;
        }
        case OP_F64_CONST: {
            if (frame->pc + 8 > frame->code_len) TRAP(WASM_TRAP_UNREACHABLE);
            uint64_t bits;
            memcpy(&bits, frame->code + frame->pc, 8);
            frame->pc += 8;
            wasm_value_t r; r.u64 = bits; wasm_push(inst, r);
            break;
        }

        // ---- i32 comparisons ----
#define I32_CMP(EXPR)                                                           \
        do {                                                                  \
            wasm_value_t b = wasm_pop(inst), a = wasm_pop(inst);            \
            if (inst->status == WASM_RUN_TRAPPED) return;                    \
            wasm_value_t r; r.i32 = (EXPR) ? 1 : 0; wasm_push(inst, r);    \
        } while (0)

        case OP_I32_EQZ: {
            wasm_value_t a = wasm_pop(inst);
            if (inst->status == WASM_RUN_TRAPPED) return;
            wasm_value_t r; r.i32 = (a.i32 == 0) ? 1 : 0; wasm_push(inst, r);
            break;
        }
        case OP_I32_EQ:   I32_CMP(a.i32 == b.i32); break;
        case OP_I32_NE:   I32_CMP(a.i32 != b.i32); break;
        case OP_I32_LT_S: I32_CMP(a.i32 <  b.i32); break;
        case OP_I32_LT_U: I32_CMP(a.u32 <  b.u32); break;
        case OP_I32_GT_S: I32_CMP(a.i32 >  b.i32); break;
        case OP_I32_GT_U: I32_CMP(a.u32 >  b.u32); break;
        case OP_I32_LE_S: I32_CMP(a.i32 <= b.i32); break;
        case OP_I32_LE_U: I32_CMP(a.u32 <= b.u32); break;
        case OP_I32_GE_S: I32_CMP(a.i32 >= b.i32); break;
        case OP_I32_GE_U: I32_CMP(a.u32 >= b.u32); break;

        case OP_I64_EQZ: {
            wasm_value_t a = wasm_pop(inst);
            if (inst->status == WASM_RUN_TRAPPED) return;
            wasm_value_t r; r.i32 = (a.i64 == 0) ? 1 : 0; wasm_push(inst, r);
            break;
        }
        case OP_I64_EQ:   I32_CMP(a.i64 == b.i64); break;
        case OP_I64_NE:   I32_CMP(a.i64 != b.i64); break;
        case OP_I64_LT_S: I32_CMP(a.i64 <  b.i64); break;
        case OP_I64_LT_U: I32_CMP(a.u64 <  b.u64); break;
        case OP_I64_GT_S: I32_CMP(a.i64 >  b.i64); break;
        case OP_I64_GT_U: I32_CMP(a.u64 >  b.u64); break;
        case OP_I64_LE_S: I32_CMP(a.i64 <= b.i64); break;
        case OP_I64_LE_U: I32_CMP(a.u64 <= b.u64); break;
        case OP_I64_GE_S: I32_CMP(a.i64 >= b.i64); break;
        case OP_I64_GE_U: I32_CMP(a.u64 >= b.u64); break;

        case OP_F32_EQ:   I32_CMP(a.f32 == b.f32); break;
        case OP_F32_NE:   I32_CMP(a.f32 != b.f32); break;
        case OP_F32_LT:   I32_CMP(a.f32 <  b.f32); break;
        case OP_F32_GT:   I32_CMP(a.f32 >  b.f32); break;
        case OP_F32_LE:   I32_CMP(a.f32 <= b.f32); break;
        case OP_F32_GE:   I32_CMP(a.f32 >= b.f32); break;
        case OP_F64_EQ:   I32_CMP(a.f64 == b.f64); break;
        case OP_F64_NE:   I32_CMP(a.f64 != b.f64); break;
        case OP_F64_LT:   I32_CMP(a.f64 <  b.f64); break;
        case OP_F64_GT:   I32_CMP(a.f64 >  b.f64); break;
        case OP_F64_LE:   I32_CMP(a.f64 <= b.f64); break;
        case OP_F64_GE:   I32_CMP(a.f64 >= b.f64); break;

        // ---- i32 arithmetic / bitwise ----
#define I32_BIN(FIELD, EXPR)                                                    \
        do {                                                                  \
            wasm_value_t b = wasm_pop(inst), a = wasm_pop(inst);            \
            if (inst->status == WASM_RUN_TRAPPED) return;                    \
            wasm_value_t r; r.FIELD = (EXPR); wasm_push(inst, r);          \
        } while (0)

        case OP_I32_CLZ: {
            wasm_value_t a = wasm_pop(inst);
            if (inst->status == WASM_RUN_TRAPPED) return;
            wasm_value_t r;
            r.u32 = a.u32 ? (uint32_t)__builtin_clz(a.u32) : 32;
            wasm_push(inst, r);
            break;
        }
        case OP_I32_CTZ: {
            wasm_value_t a = wasm_pop(inst);
            if (inst->status == WASM_RUN_TRAPPED) return;
            wasm_value_t r;
            r.u32 = a.u32 ? (uint32_t)__builtin_ctz(a.u32) : 32;
            wasm_push(inst, r);
            break;
        }
        case OP_I32_POPCNT: {
            wasm_value_t a = wasm_pop(inst);
            if (inst->status == WASM_RUN_TRAPPED) return;
            wasm_value_t r; r.u32 = (uint32_t)__builtin_popcount(a.u32);
            wasm_push(inst, r);
            break;
        }
        case OP_I32_ADD: I32_BIN(i32, a.i32 + b.i32); break;
        case OP_I32_SUB: I32_BIN(i32, a.i32 - b.i32); break;
        case OP_I32_MUL: I32_BIN(i32, (int32_t)((uint32_t)a.i32 * (uint32_t)b.i32)); break;
        case OP_I32_DIV_S: {
            wasm_value_t b = wasm_pop(inst), a = wasm_pop(inst);
            if (inst->status == WASM_RUN_TRAPPED) return;
            if (b.i32 == 0) TRAP(WASM_TRAP_DIV_BY_ZERO);
            if (a.i32 == INT32_MIN && b.i32 == -1) TRAP(WASM_TRAP_INT_OVERFLOW);
            wasm_value_t r; r.i32 = a.i32 / b.i32; wasm_push(inst, r);
            break;
        }
        case OP_I32_DIV_U: {
            wasm_value_t b = wasm_pop(inst), a = wasm_pop(inst);
            if (inst->status == WASM_RUN_TRAPPED) return;
            if (b.u32 == 0) TRAP(WASM_TRAP_DIV_BY_ZERO);
            wasm_value_t r; r.u32 = a.u32 / b.u32; wasm_push(inst, r);
            break;
        }
        case OP_I32_REM_S: {
            wasm_value_t b = wasm_pop(inst), a = wasm_pop(inst);
            if (inst->status == WASM_RUN_TRAPPED) return;
            if (b.i32 == 0) TRAP(WASM_TRAP_DIV_BY_ZERO);
            wasm_value_t r;
            if (a.i32 == INT32_MIN && b.i32 == -1) r.i32 = 0;  // no overflow
            else r.i32 = a.i32 % b.i32;
            wasm_push(inst, r);
            break;
        }
        case OP_I32_REM_U: {
            wasm_value_t b = wasm_pop(inst), a = wasm_pop(inst);
            if (inst->status == WASM_RUN_TRAPPED) return;
            if (b.u32 == 0) TRAP(WASM_TRAP_DIV_BY_ZERO);
            wasm_value_t r; r.u32 = a.u32 % b.u32; wasm_push(inst, r);
            break;
        }
        case OP_I32_AND: I32_BIN(u32, a.u32 & b.u32); break;
        case OP_I32_OR:  I32_BIN(u32, a.u32 | b.u32); break;
        case OP_I32_XOR: I32_BIN(u32, a.u32 ^ b.u32); break;
        case OP_I32_SHL: I32_BIN(u32, a.u32 << (b.u32 & 31)); break;
        case OP_I32_SHR_S: I32_BIN(i32, a.i32 >> (b.u32 & 31)); break;
        case OP_I32_SHR_U: I32_BIN(u32, a.u32 >> (b.u32 & 31)); break;
        case OP_I32_ROTL: {
            wasm_value_t b = wasm_pop(inst), a = wasm_pop(inst);
            if (inst->status == WASM_RUN_TRAPPED) return;
            uint32_t s = b.u32 & 31;
            wasm_value_t r; r.u32 = s ? ((a.u32 << s) | (a.u32 >> (32 - s))) : a.u32;
            wasm_push(inst, r);
            break;
        }
        case OP_I32_ROTR: {
            wasm_value_t b = wasm_pop(inst), a = wasm_pop(inst);
            if (inst->status == WASM_RUN_TRAPPED) return;
            uint32_t s = b.u32 & 31;
            wasm_value_t r; r.u32 = s ? ((a.u32 >> s) | (a.u32 << (32 - s))) : a.u32;
            wasm_push(inst, r);
            break;
        }

        // ---- i64 arithmetic / bitwise ----
        case OP_I64_CLZ: {
            wasm_value_t a = wasm_pop(inst);
            if (inst->status == WASM_RUN_TRAPPED) return;
            wasm_value_t r; r.u64 = a.u64 ? (uint64_t)__builtin_clzll(a.u64) : 64;
            wasm_push(inst, r);
            break;
        }
        case OP_I64_CTZ: {
            wasm_value_t a = wasm_pop(inst);
            if (inst->status == WASM_RUN_TRAPPED) return;
            wasm_value_t r; r.u64 = a.u64 ? (uint64_t)__builtin_ctzll(a.u64) : 64;
            wasm_push(inst, r);
            break;
        }
        case OP_I64_POPCNT: {
            wasm_value_t a = wasm_pop(inst);
            if (inst->status == WASM_RUN_TRAPPED) return;
            wasm_value_t r; r.u64 = (uint64_t)__builtin_popcountll(a.u64);
            wasm_push(inst, r);
            break;
        }
        case OP_I64_ADD: I32_BIN(i64, a.i64 + b.i64); break;
        case OP_I64_SUB: I32_BIN(i64, a.i64 - b.i64); break;
        case OP_I64_MUL: I32_BIN(i64, (int64_t)((uint64_t)a.i64 * (uint64_t)b.i64)); break;
        case OP_I64_DIV_S: {
            wasm_value_t b = wasm_pop(inst), a = wasm_pop(inst);
            if (inst->status == WASM_RUN_TRAPPED) return;
            if (b.i64 == 0) TRAP(WASM_TRAP_DIV_BY_ZERO);
            if (a.i64 == INT64_MIN && b.i64 == -1) TRAP(WASM_TRAP_INT_OVERFLOW);
            wasm_value_t r; r.i64 = a.i64 / b.i64; wasm_push(inst, r);
            break;
        }
        case OP_I64_DIV_U: {
            wasm_value_t b = wasm_pop(inst), a = wasm_pop(inst);
            if (inst->status == WASM_RUN_TRAPPED) return;
            if (b.u64 == 0) TRAP(WASM_TRAP_DIV_BY_ZERO);
            wasm_value_t r; r.u64 = a.u64 / b.u64; wasm_push(inst, r);
            break;
        }
        case OP_I64_REM_S: {
            wasm_value_t b = wasm_pop(inst), a = wasm_pop(inst);
            if (inst->status == WASM_RUN_TRAPPED) return;
            if (b.i64 == 0) TRAP(WASM_TRAP_DIV_BY_ZERO);
            wasm_value_t r;
            if (a.i64 == INT64_MIN && b.i64 == -1) r.i64 = 0;
            else r.i64 = a.i64 % b.i64;
            wasm_push(inst, r);
            break;
        }
        case OP_I64_REM_U: {
            wasm_value_t b = wasm_pop(inst), a = wasm_pop(inst);
            if (inst->status == WASM_RUN_TRAPPED) return;
            if (b.u64 == 0) TRAP(WASM_TRAP_DIV_BY_ZERO);
            wasm_value_t r; r.u64 = a.u64 % b.u64; wasm_push(inst, r);
            break;
        }
        case OP_I64_AND: I32_BIN(u64, a.u64 & b.u64); break;
        case OP_I64_OR:  I32_BIN(u64, a.u64 | b.u64); break;
        case OP_I64_XOR: I32_BIN(u64, a.u64 ^ b.u64); break;
        case OP_I64_SHL: I32_BIN(u64, a.u64 << (b.u64 & 63)); break;
        case OP_I64_SHR_S: I32_BIN(i64, a.i64 >> (b.u64 & 63)); break;
        case OP_I64_SHR_U: I32_BIN(u64, a.u64 >> (b.u64 & 63)); break;
        case OP_I64_ROTL: {
            wasm_value_t b = wasm_pop(inst), a = wasm_pop(inst);
            if (inst->status == WASM_RUN_TRAPPED) return;
            uint64_t s = b.u64 & 63;
            wasm_value_t r; r.u64 = s ? ((a.u64 << s) | (a.u64 >> (64 - s))) : a.u64;
            wasm_push(inst, r);
            break;
        }
        case OP_I64_ROTR: {
            wasm_value_t b = wasm_pop(inst), a = wasm_pop(inst);
            if (inst->status == WASM_RUN_TRAPPED) return;
            uint64_t s = b.u64 & 63;
            wasm_value_t r; r.u64 = s ? ((a.u64 >> s) | (a.u64 << (64 - s))) : a.u64;
            wasm_push(inst, r);
            break;
        }

        // ---- f32 ----
#define F_UN(FIELD, EXPR)                                                       \
        do {                                                                  \
            wasm_value_t a = wasm_pop(inst);                                \
            if (inst->status == WASM_RUN_TRAPPED) return;                    \
            wasm_value_t r; r.FIELD = (EXPR); wasm_push(inst, r);          \
        } while (0)
#define F_BIN(FIELD, EXPR)                                                      \
        do {                                                                  \
            wasm_value_t b = wasm_pop(inst), a = wasm_pop(inst);            \
            if (inst->status == WASM_RUN_TRAPPED) return;                    \
            wasm_value_t r; r.FIELD = (EXPR); wasm_push(inst, r);          \
        } while (0)

        case OP_F32_ABS:   F_UN(f32, wm_fabsf(a.f32)); break;
        case OP_F32_NEG:   F_UN(f32, -a.f32); break;
        case OP_F32_CEIL:  F_UN(f32, wm_ceilf(a.f32)); break;
        case OP_F32_FLOOR: F_UN(f32, wm_floorf(a.f32)); break;
        case OP_F32_TRUNC: F_UN(f32, f32_trunc(a.f32)); break;
        case OP_F32_NEAREST: F_UN(f32, f32_nearest(a.f32)); break;
        case OP_F32_SQRT:  F_UN(f32, wm_sqrtf(a.f32)); break;
        case OP_F32_ADD:   F_BIN(f32, a.f32 + b.f32); break;
        case OP_F32_SUB:   F_BIN(f32, a.f32 - b.f32); break;
        case OP_F32_MUL:   F_BIN(f32, a.f32 * b.f32); break;
        case OP_F32_DIV:   F_BIN(f32, a.f32 / b.f32); break;
        case OP_F32_MIN:   F_BIN(f32, wasm_f32_min(a.f32, b.f32)); break;
        case OP_F32_MAX:   F_BIN(f32, wasm_f32_max(a.f32, b.f32)); break;
        case OP_F32_COPYSIGN: F_BIN(f32, wm_copysignf(a.f32, b.f32)); break;

        case OP_F64_ABS:   F_UN(f64, wm_fabs(a.f64)); break;
        case OP_F64_NEG:   F_UN(f64, -a.f64); break;
        case OP_F64_CEIL:  F_UN(f64, wm_ceil(a.f64)); break;
        case OP_F64_FLOOR: F_UN(f64, wm_floor(a.f64)); break;
        case OP_F64_TRUNC: F_UN(f64, f64_trunc(a.f64)); break;
        case OP_F64_NEAREST: F_UN(f64, f64_nearest(a.f64)); break;
        case OP_F64_SQRT:  F_UN(f64, wm_sqrt(a.f64)); break;
        case OP_F64_ADD:   F_BIN(f64, a.f64 + b.f64); break;
        case OP_F64_SUB:   F_BIN(f64, a.f64 - b.f64); break;
        case OP_F64_MUL:   F_BIN(f64, a.f64 * b.f64); break;
        case OP_F64_DIV:   F_BIN(f64, a.f64 / b.f64); break;
        case OP_F64_MIN:   F_BIN(f64, wasm_f64_min(a.f64, b.f64)); break;
        case OP_F64_MAX:   F_BIN(f64, wasm_f64_max(a.f64, b.f64)); break;
        case OP_F64_COPYSIGN: F_BIN(f64, wm_copysign(a.f64, b.f64)); break;

        // ---- Conversions ----
        case OP_I32_WRAP_I64: F_UN(i32, (int32_t)(a.u64 & 0xFFFFFFFFu)); break;

        // float -> int truncations (trap on NaN / out of range)
        case OP_I32_TRUNC_F32_S: {
            wasm_value_t a = wasm_pop(inst);
            if (inst->status == WASM_RUN_TRAPPED) return;
            float x = a.f32;
            if (x != x) TRAP(WASM_TRAP_INVALID_CONVERSION);  // NaN
            if (!(x >= -2147483648.0f && x < 2147483648.0f)) TRAP(WASM_TRAP_INVALID_CONVERSION);
            wasm_value_t r; r.i32 = (int32_t)x; wasm_push(inst, r);
            break;
        }
        case OP_I32_TRUNC_F32_U: {
            wasm_value_t a = wasm_pop(inst);
            if (inst->status == WASM_RUN_TRAPPED) return;
            float x = a.f32;
            if (x != x) TRAP(WASM_TRAP_INVALID_CONVERSION);  // NaN
            if (!(x > -1.0f && x < 4294967296.0f)) TRAP(WASM_TRAP_INVALID_CONVERSION);
            wasm_value_t r; r.u32 = (uint32_t)x; wasm_push(inst, r);
            break;
        }
        case OP_I32_TRUNC_F64_S: {
            wasm_value_t a = wasm_pop(inst);
            if (inst->status == WASM_RUN_TRAPPED) return;
            double x = a.f64;
            if (x != x) TRAP(WASM_TRAP_INVALID_CONVERSION);  // NaN
            if (!(x >= -2147483648.0 && x < 2147483648.0)) TRAP(WASM_TRAP_INVALID_CONVERSION);
            wasm_value_t r; r.i32 = (int32_t)x; wasm_push(inst, r);
            break;
        }
        case OP_I32_TRUNC_F64_U: {
            wasm_value_t a = wasm_pop(inst);
            if (inst->status == WASM_RUN_TRAPPED) return;
            double x = a.f64;
            if (x != x) TRAP(WASM_TRAP_INVALID_CONVERSION);  // NaN
            if (!(x > -1.0 && x < 4294967296.0)) TRAP(WASM_TRAP_INVALID_CONVERSION);
            wasm_value_t r; r.u32 = (uint32_t)x; wasm_push(inst, r);
            break;
        }
        case OP_I64_EXTEND_I32_S: F_UN(i64, (int64_t)a.i32); break;
        case OP_I64_EXTEND_I32_U: F_UN(u64, (uint64_t)a.u32); break;
        case OP_I64_TRUNC_F32_S: {
            wasm_value_t a = wasm_pop(inst);
            if (inst->status == WASM_RUN_TRAPPED) return;
            float x = a.f32;
            if (x != x) TRAP(WASM_TRAP_INVALID_CONVERSION);  // NaN
            if (!(x >= -9223372036854775808.0f && x < 9223372036854775808.0f)) TRAP(WASM_TRAP_INVALID_CONVERSION);
            wasm_value_t r; r.i64 = (int64_t)x; wasm_push(inst, r);
            break;
        }
        case OP_I64_TRUNC_F32_U: {
            wasm_value_t a = wasm_pop(inst);
            if (inst->status == WASM_RUN_TRAPPED) return;
            float x = a.f32;
            if (x != x) TRAP(WASM_TRAP_INVALID_CONVERSION);  // NaN
            if (!(x > -1.0f && x < 18446744073709551616.0f)) TRAP(WASM_TRAP_INVALID_CONVERSION);
            wasm_value_t r; r.u64 = (uint64_t)x; wasm_push(inst, r);
            break;
        }
        case OP_I64_TRUNC_F64_S: {
            wasm_value_t a = wasm_pop(inst);
            if (inst->status == WASM_RUN_TRAPPED) return;
            double x = a.f64;
            if (x != x) TRAP(WASM_TRAP_INVALID_CONVERSION);  // NaN
            if (!(x >= -9223372036854775808.0 && x < 9223372036854775808.0)) TRAP(WASM_TRAP_INVALID_CONVERSION);
            wasm_value_t r; r.i64 = (int64_t)x; wasm_push(inst, r);
            break;
        }
        case OP_I64_TRUNC_F64_U: {
            wasm_value_t a = wasm_pop(inst);
            if (inst->status == WASM_RUN_TRAPPED) return;
            double x = a.f64;
            if (x != x) TRAP(WASM_TRAP_INVALID_CONVERSION);  // NaN
            if (!(x > -1.0 && x < 18446744073709551616.0)) TRAP(WASM_TRAP_INVALID_CONVERSION);
            wasm_value_t r; r.u64 = (uint64_t)x; wasm_push(inst, r);
            break;
        }
        case OP_F32_CONVERT_I32_S: F_UN(f32, (float)a.i32); break;
        case OP_F32_CONVERT_I32_U: F_UN(f32, (float)a.u32); break;
        case OP_F32_CONVERT_I64_S: F_UN(f32, (float)a.i64); break;
        case OP_F32_CONVERT_I64_U: F_UN(f32, (float)a.u64); break;
        case OP_F32_DEMOTE_F64:    F_UN(f32, (float)a.f64); break;
        case OP_F64_CONVERT_I32_S: F_UN(f64, (double)a.i32); break;
        case OP_F64_CONVERT_I32_U: F_UN(f64, (double)a.u32); break;
        case OP_F64_CONVERT_I64_S: F_UN(f64, (double)a.i64); break;
        case OP_F64_CONVERT_I64_U: F_UN(f64, (double)a.u64); break;
        case OP_F64_PROMOTE_F32:   F_UN(f64, (double)a.f32); break;

        // reinterpret: bit-preserving copy (values are stored bitwise in the
        // union already, so this is structurally a no-op, but we keep it for
        // clarity / field correctness).
        case OP_I32_REINTERPRET_F32: F_UN(u32, a.u32); break;
        case OP_I64_REINTERPRET_F64: F_UN(u64, a.u64); break;
        case OP_F32_REINTERPRET_I32: F_UN(u32, a.u32); break;
        case OP_F64_REINTERPRET_I64: F_UN(u64, a.u64); break;

        // sign-extension ops
        case OP_I32_EXTEND8_S:  F_UN(i32, (int32_t)(int8_t)(a.u32 & 0xFF)); break;
        case OP_I32_EXTEND16_S: F_UN(i32, (int32_t)(int16_t)(a.u32 & 0xFFFF)); break;
        case OP_I64_EXTEND8_S:  F_UN(i64, (int64_t)(int8_t)(a.u64 & 0xFF)); break;
        case OP_I64_EXTEND16_S: F_UN(i64, (int64_t)(int16_t)(a.u64 & 0xFFFF)); break;
        case OP_I64_EXTEND32_S: F_UN(i64, (int64_t)(int32_t)(a.u64 & 0xFFFFFFFF)); break;

        default:
            // Unknown / unimplemented opcode.
            TRAP(WASM_TRAP_UNREACHABLE);
        }
        continue;

    func_return:
        {
            uint32_t arity = frame->result_arity;
            uint32_t base = frame->locals_base;
            // Move the top `arity` operands down to the frame base.
            if (arity > inst->vsp - base) arity = inst->vsp - base;
            for (uint32_t i = 0; i < arity; i++)
                inst->vstack[base + i] = inst->vstack[inst->vsp - arity + i];
            inst->vsp = base + arity;
            inst->csp = frame->ctrl_base;
            inst->fsp--;
            if (inst->fsp == 0) {
                // Top-level return: copy out results.
                uint32_t n = arity < 16 ? arity : 16;
                for (uint32_t i = 0; i < n; i++)
                    inst->results[i] = inst->vstack[base + i];
                inst->n_results = n;
                inst->status = WASM_RUN_DONE;
                return;
            }
            frame = &inst->frames[inst->fsp - 1];
        }
        continue;
    }
}

#undef I32_CMP
#undef I32_BIN
#undef F_UN
#undef F_BIN
#undef LOAD
#undef STORE
#undef READ_MEMARG
#undef TRAP
