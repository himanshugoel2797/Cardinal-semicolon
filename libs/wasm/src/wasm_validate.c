// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

// Wasm MVP module validation: the hardening pass that rejects malformed or
// ill-typed modules up front, so an externally-supplied guest never reaches the
// interpreter unless it type-checks. Runtime memory safety does not depend on
// this (every access in wasm_exec.c is bounds-checked), but validation lets us
// cleanly reject a bad module rather than trapping mid-run.
//
// Implements the standard abstract-stack type-checking algorithm from the spec
// (https://webassembly.github.io/spec/core/valid/), MVP scope: index-range and
// limits checks over the module's sections, plus a per-function body type check
// with an operand-type stack and a control-frame stack handling structured
// control flow and stack-polymorphism after unreachable/br/return.
//
// Decode (wasm_module.c) has already split the module into typed sections and
// validated the const-expr type of each global init; we re-check the defensive
// pieces here and own all type checking of function bodies.

#include "wasm_internal.h"
#include <stdlib.h>
#include <string.h>

// ---- Abstract value types ----------------------------------------------
//
// The operand-type stack holds value types, plus a sentinel "unknown" used by
// the stack-polymorphism rule: after an unconditional branch / unreachable, the
// rest of a block is unreachable and may pop any type (which yields "unknown")
// and push freely. We model "unknown" as a distinct tag.

typedef int16_t vtype_t;       // a wasm_valtype_t byte, or VT_UNKNOWN
#define VT_UNKNOWN  ((vtype_t)-1)

static bool is_valtype(uint8_t b) {
    return b == WASM_I32 || b == WASM_I64 || b == WASM_F32 ||
           b == WASM_F64 || b == WASM_FUNCREF || b == WASM_EXTERNREF;
}

// ---- Validation context -------------------------------------------------

typedef struct {
    uint8_t kind;          // 0=block 1=loop 2=if (matches wasm_label_t.kind)
    bool unreachable;      // the rest of this block is statically unreachable
    bool saw_else;         // for an if frame: an else clause was seen
    uint32_t height;       // operand-stack height at frame entry
    // input/result types of the block (copied out of the module's types)
    vtype_t *in;  uint32_t n_in;
    vtype_t *out; uint32_t n_out;
} ctrl_frame_t;

typedef struct {
    wasm_module_t *m;
    const wasm_func_t *f;
    const uint8_t *code;
    uint32_t code_len;
    uint32_t pc;

    vtype_t *opnds;   uint32_t n_opnds, cap_opnds;     // operand-type stack
    ctrl_frame_t *ctrl; uint32_t n_ctrl, cap_ctrl;     // control-frame stack

    bool failed;
} vctx;

// ---- Small growable-array helpers --------------------------------------

static bool push_opnd(vctx *c, vtype_t t) {
    if (c->n_opnds == c->cap_opnds) {
        uint32_t nc = c->cap_opnds ? c->cap_opnds * 2 : 32;
        vtype_t *p = realloc(c->opnds, nc * sizeof(vtype_t));
        if (!p) { c->failed = true; return false; }
        c->opnds = p;
        c->cap_opnds = nc;
    }
    c->opnds[c->n_opnds++] = t;
    return true;
}

// Pop one operand, checking it matches `expect` (VT_UNKNOWN = accept any). The
// stack-polymorphism rule: if we are at the current frame's base height and the
// frame is unreachable, popping yields VT_UNKNOWN without underflowing. Returns
// the popped type (or VT_UNKNOWN) in *out; sets failed and returns false on a
// genuine type mismatch / underflow in reachable code.
static bool pop_opnd_expect(vctx *c, vtype_t expect, vtype_t *out) {
    ctrl_frame_t *cf = &c->ctrl[c->n_ctrl - 1];
    if (c->n_opnds == cf->height) {
        if (cf->unreachable) {
            if (out) *out = VT_UNKNOWN;
            return true;             // polymorphic: synthesize an unknown
        }
        c->failed = true;            // underflow past the block in reachable code
        return false;
    }
    vtype_t got = c->opnds[--c->n_opnds];
    if (expect != VT_UNKNOWN && got != VT_UNKNOWN && got != expect) {
        c->failed = true;
        return false;
    }
    if (out) *out = (got == VT_UNKNOWN) ? expect : got;
    return true;
}

static bool pop_type(vctx *c, vtype_t expect) {
    return pop_opnd_expect(c, expect, NULL);
}

// ---- Control frames -----------------------------------------------------

static bool push_ctrl(vctx *c, uint8_t kind, vtype_t *in, uint32_t n_in,
                      vtype_t *out, uint32_t n_out) {
    if (c->n_ctrl == c->cap_ctrl) {
        uint32_t nc = c->cap_ctrl ? c->cap_ctrl * 2 : 16;
        ctrl_frame_t *p = realloc(c->ctrl, nc * sizeof(ctrl_frame_t));
        if (!p) { c->failed = true; return false; }
        c->ctrl = p;
        c->cap_ctrl = nc;
    }
    ctrl_frame_t *cf = &c->ctrl[c->n_ctrl++];
    cf->kind = kind;
    cf->unreachable = false;
    cf->saw_else = false;
    cf->height = c->n_opnds;
    cf->in = in;   cf->n_in = n_in;
    cf->out = out; cf->n_out = n_out;
    // The block's inputs are already on the operand stack (popped before the
    // frame is pushed); re-push them inside the new frame's scope.
    for (uint32_t i = 0; i < n_in; i++)
        if (!push_opnd(c, in[i])) return false;
    cf->height = c->n_opnds - n_in;   // base height excludes the re-pushed inputs
    return true;
}

// The label types of a branch to control frame `cf`: loop branches to its
// inputs, block/if to their results.
static vtype_t *label_types(ctrl_frame_t *cf, uint32_t *n) {
    if (cf->kind == 1) { *n = cf->n_in; return cf->in; }
    *n = cf->n_out; return cf->out;
}

// Set the current frame unreachable: discard operands down to its base height
// (the spec's "unreachable" makes the rest of the block polymorphic).
static void mark_unreachable(vctx *c) {
    ctrl_frame_t *cf = &c->ctrl[c->n_ctrl - 1];
    c->n_opnds = cf->height;
    cf->unreachable = true;
}

// ---- Blocktype decode ---------------------------------------------------
//
// Decode the blocktype at c->pc into input/result type arrays (caller frees, or
// they point into a static one-element scratch we own). 0x40 -> void; a single
// valtype -> 0 in / 1 out; an s33 type index -> that functype's params/results.

static bool read_blocktype(vctx *c, vtype_t **in, uint32_t *n_in,
                           vtype_t **out, uint32_t *n_out,
                           vtype_t *scratch /* >= 1 slot */) {
    if (c->pc >= c->code_len) { c->failed = true; return false; }
    uint8_t b = c->code[c->pc];
    if (b == 0x40) {
        c->pc++;
        *in = NULL; *n_in = 0; *out = NULL; *n_out = 0;
        return true;
    }
    if (is_valtype(b)) {
        c->pc++;
        scratch[0] = (vtype_t)b;
        *in = NULL; *n_in = 0;
        *out = scratch; *n_out = 1;
        return true;
    }
    int64_t ti;
    if (!wasm_read_s64(c->code, c->code_len, &c->pc, &ti)) { c->failed = true; return false; }
    if (ti < 0 || (uint64_t)ti >= c->m->n_types) { c->failed = true; return false; }
    wasm_functype_t *ft = &c->m->types[(uint32_t)ti];
    // A wasm_valtype_t is a small positive byte (fits a vtype_t and never equals
    // VT_UNKNOWN), but the element type differs, so copy into per-call heap
    // arrays the caller frees.
    vtype_t *bi = ft->n_params ? malloc(ft->n_params * sizeof(vtype_t)) : NULL;
    vtype_t *bo = ft->n_results ? malloc(ft->n_results * sizeof(vtype_t)) : NULL;
    if ((ft->n_params && !bi) || (ft->n_results && !bo)) {
        free(bi); free(bo); c->failed = true; return false;
    }
    for (uint32_t i = 0; i < ft->n_params; i++)  bi[i] = (vtype_t)ft->params[i];
    for (uint32_t i = 0; i < ft->n_results; i++) bo[i] = (vtype_t)ft->results[i];
    *in = bi; *n_in = ft->n_params;
    *out = bo; *n_out = ft->n_results;
    return true;
}

// A blocktype's type arrays are heap-allocated only for the s33 (multi-value)
// case; the void/single forms point at the caller's scratch or NULL. Track
// whether to free with a flag carried alongside each frame's in/out.
//
// To keep the control-frame struct frozen-simple, we instead always copy the
// block's in/out into fresh heap arrays owned by the frame, freed at end/cleanup.

static bool dup_types(vtype_t *src, uint32_t n, vtype_t **dst) {
    if (n == 0) { *dst = NULL; return true; }
    vtype_t *p = malloc(n * sizeof(vtype_t));
    if (!p) return false;
    memcpy(p, src, n * sizeof(vtype_t));
    *dst = p;
    return true;
}

// ---- Function-index / signature helpers --------------------------------

// Resolve a function index (import space) to its functype, or NULL if the index
// is out of range. Mirrors func_signature in wasm_instance.c.
static const wasm_functype_t *func_sig(wasm_module_t *m, uint32_t func_index) {
    uint32_t total = m->n_imported_funcs + m->n_funcs;
    if (func_index >= total) return NULL;
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

// ---- Per-instruction type checking -------------------------------------

// Pop a single value type (no polymorphism beyond pop_type's own handling).
#define POP(t)        do { if (!pop_type(c, (t))) return false; } while (0)
#define PUSH(t)       do { if (!push_opnd(c, (t))) return false; } while (0)
#define FAIL()        do { c->failed = true; return false; } while (0)
#define RD_U32(out)   do { if (!wasm_read_u32(c->code, c->code_len, &c->pc, &(out))) FAIL(); } while (0)
#define RD_S32(out)   do { if (!wasm_read_s32(c->code, c->code_len, &c->pc, &(out))) FAIL(); } while (0)
#define RD_S64(out)   do { if (!wasm_read_s64(c->code, c->code_len, &c->pc, &(out))) FAIL(); } while (0)

// memarg: align (log2) + offset. align must be <= log2(natural access size).
static bool check_memarg(vctx *c, uint32_t natural_log2) {
    if (!c->m->has_mem) FAIL();
    uint32_t align, offset;
    RD_U32(align);
    RD_U32(offset);
    (void)offset;
    if (align > natural_log2) FAIL();
    return true;
}

// load: pop i32 address, push the result type.
static bool ck_load(vctx *c, uint32_t natural_log2, vtype_t result) {
    if (!check_memarg(c, natural_log2)) return false;
    POP(WASM_I32);
    PUSH(result);
    return true;
}

// store: pop the value type, then the i32 address.
static bool ck_store(vctx *c, uint32_t natural_log2, vtype_t value) {
    if (!check_memarg(c, natural_log2)) return false;
    POP(value);
    POP(WASM_I32);
    return true;
}

static bool unop(vctx *c, vtype_t in, vtype_t out)  { POP(in); PUSH(out); return true; }
static bool binop(vctx *c, vtype_t in, vtype_t out) { POP(in); POP(in); PUSH(out); return true; }
static bool relop(vctx *c, vtype_t in)              { POP(in); POP(in); PUSH(WASM_I32); return true; }
static bool testop(vctx *c, vtype_t in)             { POP(in); PUSH(WASM_I32); return true; }

// Type-check one function body. Returns false (and sets c->failed) on any
// violation or truncation.
static bool check_body(vctx *c) {
    wasm_module_t *m = c->m;
    const wasm_func_t *f = c->f;
    const wasm_functype_t *sig = &m->types[f->type_index];

    // Outermost implicit frame: block with no inputs and the function's results.
    {
        vtype_t *out = NULL;
        if (sig->n_results) {
            out = malloc(sig->n_results * sizeof(vtype_t));
            if (!out) FAIL();
            for (uint32_t i = 0; i < sig->n_results; i++) out[i] = (vtype_t)sig->results[i];
        }
        if (!push_ctrl(c, 0 /*block*/, NULL, 0, out, sig->n_results)) {
            free(out);
            return false;
        }
    }

    for (;;) {
        if (c->n_ctrl == 0) break;   // outermost frame ended -> function done

        if (c->pc >= c->code_len) {
            // Body ran out without a final END for every open frame: only OK if
            // exactly the outer frame remains and we treat EOF as its END. The
            // decoder guarantees a trailing END, but be defensive: if frames are
            // still open, that's malformed.
            FAIL();
        }

        uint8_t op = c->code[c->pc++];

        switch (op) {
        case OP_UNREACHABLE:
            mark_unreachable(c);
            break;
        case OP_NOP:
            break;

        case OP_BLOCK:
        case OP_LOOP:
        case OP_IF: {
            vtype_t scratch[1];
            vtype_t *bin, *bout;
            uint32_t n_in, n_out;
            if (!read_blocktype(c, &bin, &n_in, &bout, &n_out, scratch)) return false;
            uint8_t kind = (op == OP_BLOCK) ? 0 : (op == OP_LOOP) ? 1 : 2;
            // if pops the i32 condition first.
            if (op == OP_IF) POP(WASM_I32);
            // pop the block inputs (they become the frame's in-scope operands).
            for (uint32_t i = n_in; i > 0; i--) {
                if (!pop_type(c, bin[i - 1])) {
                    if (bin != scratch) free(bin);
                    if (bout != scratch) free(bout);
                    return false;
                }
            }
            // Own copies for the frame (read_blocktype's heap arrays or scratch).
            vtype_t *fin = NULL, *fout = NULL;
            bool ok = dup_types(bin, n_in, &fin) && dup_types(bout, n_out, &fout);
            if (bin != scratch) free(bin);
            if (bout != scratch) free(bout);
            if (!ok) { free(fin); free(fout); FAIL(); }
            if (!push_ctrl(c, kind, fin, n_in, fout, n_out)) {
                free(fin); free(fout);
                return false;
            }
            break;
        }

        case OP_ELSE: {
            ctrl_frame_t *cf = &c->ctrl[c->n_ctrl - 1];
            if (cf->kind != 2) FAIL();        // else without if
            // End of the then-arm: results must match, stack back to inputs.
            for (uint32_t i = cf->n_out; i > 0; i--)
                POP(cf->out[i - 1]);
            if (c->n_opnds != cf->height) FAIL();   // extra/leftover operands
            if (cf->saw_else) FAIL();               // two else clauses
            // Re-enter for the else-arm: same frame, reset reachability + inputs.
            cf->saw_else = true;
            cf->unreachable = false;
            for (uint32_t i = 0; i < cf->n_in; i++)
                PUSH(cf->in[i]);
            break;
        }

        case OP_END: {
            ctrl_frame_t *cf = &c->ctrl[c->n_ctrl - 1];
            // An if with no else is equivalent to an empty else: that empty arm
            // must type-check, which requires the if's inputs equal its results
            // (same arity AND same types).
            if (cf->kind == 2 && !cf->saw_else) {
                if (cf->n_in != cf->n_out) FAIL();
                for (uint32_t i = 0; i < cf->n_in; i++)
                    if (cf->in[i] != cf->out[i]) FAIL();
            }
            // Results must be on top, exactly.
            for (uint32_t i = cf->n_out; i > 0; i--)
                POP(cf->out[i - 1]);
            if (c->n_opnds != cf->height) FAIL();
            // Push the results into the enclosing frame.
            uint32_t n_out = cf->n_out;
            vtype_t outbuf[64];
            vtype_t *outp = cf->out;
            bool spill = false;
            if (n_out > 64) {
                outp = malloc(n_out * sizeof(vtype_t));
                if (!outp) FAIL();
                spill = true;
            } else if (n_out) {
                memcpy(outbuf, cf->out, n_out * sizeof(vtype_t));
                outp = outbuf;
            }
            // free the frame's owned arrays and pop it.
            free(cf->in);
            free(cf->out);
            c->n_ctrl--;
            if (c->n_ctrl == 0) {
                // Function-level end: outer results already validated above.
                if (spill) free(outp);
                // There must be no trailing code after the outermost end.
                // (Decoder allows trailing; loop continues and EOF ends it.)
                // Stop here once the implicit frame closed.
                goto done;
            }
            for (uint32_t i = 0; i < n_out; i++)
                if (!push_opnd(c, outp[i])) { if (spill) free(outp); return false; }
            if (spill) free(outp);
            break;
        }

        case OP_BR: {
            uint32_t depth;
            RD_U32(depth);
            if (depth >= c->n_ctrl) FAIL();
            ctrl_frame_t *tgt = &c->ctrl[c->n_ctrl - 1 - depth];
            uint32_t n; vtype_t *lt = label_types(tgt, &n);
            for (uint32_t i = n; i > 0; i--) POP(lt[i - 1]);
            mark_unreachable(c);
            break;
        }

        case OP_BR_IF: {
            uint32_t depth;
            RD_U32(depth);
            POP(WASM_I32);
            if (depth >= c->n_ctrl) FAIL();
            ctrl_frame_t *tgt = &c->ctrl[c->n_ctrl - 1 - depth];
            uint32_t n; vtype_t *lt = label_types(tgt, &n);
            // Pop and re-push the label types (br_if is conditional: stack stays).
            for (uint32_t i = n; i > 0; i--) POP(lt[i - 1]);
            for (uint32_t i = 0; i < n; i++) PUSH(lt[i]);
            break;
        }

        case OP_BR_TABLE: {
            uint32_t count;
            RD_U32(count);
            POP(WASM_I32);
            // Read all targets; every target must share the default's arity, and
            // its label types must be poppable from the stack.
            uint32_t default_n = 0;
            bool have_arity = false;
            // We need the default first to know the shared arity, but it comes
            // last in the encoding. Two-pass: record positions by re-reading.
            uint32_t save_pc = c->pc;
            // Skip targets to read the default.
            for (uint32_t i = 0; i < count; i++) {
                uint32_t d; RD_U32(d);
            }
            uint32_t def; RD_U32(def);
            if (def >= c->n_ctrl) FAIL();
            { uint32_t n; (void)label_types(&c->ctrl[c->n_ctrl - 1 - def], &n);
              default_n = n; have_arity = true; }
            (void)have_arity;
            // Second pass: validate each case target shares the arity and types.
            c->pc = save_pc;
            for (uint32_t i = 0; i < count; i++) {
                uint32_t d; RD_U32(d);
                if (d >= c->n_ctrl) FAIL();
                uint32_t n; vtype_t *lt = label_types(&c->ctrl[c->n_ctrl - 1 - d], &n);
                if (n != default_n) FAIL();
                // Each target's types must be present on the stack (peek-check by
                // pop+repush against the polymorphic stack).
                for (uint32_t k = n; k > 0; k--) {
                    vtype_t got;
                    if (!pop_opnd_expect(c, lt[k - 1], &got)) return false;
                }
                for (uint32_t k = 0; k < n; k++) PUSH(lt[k]);
            }
            { uint32_t dd; RD_U32(dd); (void)dd; }   // consume default again
            // Finally pop the default's types and go unreachable.
            { uint32_t n; vtype_t *lt = label_types(&c->ctrl[c->n_ctrl - 1 - def], &n);
              for (uint32_t i = n; i > 0; i--) POP(lt[i - 1]); }
            mark_unreachable(c);
            break;
        }

        case OP_RETURN: {
            // Branch to the outermost frame: pop the function results.
            ctrl_frame_t *outer = &c->ctrl[0];
            for (uint32_t i = outer->n_out; i > 0; i--) POP(outer->out[i - 1]);
            mark_unreachable(c);
            break;
        }

        case OP_CALL: {
            uint32_t fi;
            RD_U32(fi);
            const wasm_functype_t *s = func_sig(m, fi);
            if (!s) FAIL();
            for (uint32_t i = s->n_params; i > 0; i--) POP((vtype_t)s->params[i - 1]);
            for (uint32_t i = 0; i < s->n_results; i++) PUSH((vtype_t)s->results[i]);
            break;
        }

        case OP_CALL_INDIRECT: {
            uint32_t typeidx, tableidx;
            RD_U32(typeidx);
            RD_U32(tableidx);
            if (!m->has_table) FAIL();
            if (tableidx != 0) FAIL();          // MVP: single table
            if (typeidx >= m->n_types) FAIL();
            const wasm_functype_t *s = &m->types[typeidx];
            POP(WASM_I32);                      // the table index operand
            for (uint32_t i = s->n_params; i > 0; i--) POP((vtype_t)s->params[i - 1]);
            for (uint32_t i = 0; i < s->n_results; i++) PUSH((vtype_t)s->results[i]);
            break;
        }

        case OP_DROP: {
            vtype_t got;
            if (!pop_opnd_expect(c, VT_UNKNOWN, &got)) return false;
            break;
        }

        case OP_SELECT: {
            POP(WASM_I32);                      // the condition
            vtype_t a, b;
            if (!pop_opnd_expect(c, VT_UNKNOWN, &b)) return false;
            if (!pop_opnd_expect(c, VT_UNKNOWN, &a)) return false;
            // The two values must have the same (numeric) type.
            vtype_t r = VT_UNKNOWN;
            if (a != VT_UNKNOWN && b != VT_UNKNOWN) {
                if (a != b) FAIL();
                // MVP select operands must be numeric (not reference types).
                if (a == WASM_FUNCREF || a == WASM_EXTERNREF) FAIL();
                r = a;
            } else if (a != VT_UNKNOWN) {
                r = a;
            } else {
                r = b;
            }
            PUSH(r);
            break;
        }

        case OP_LOCAL_GET: {
            uint32_t idx; RD_U32(idx);
            if (idx >= f->n_locals) FAIL();
            PUSH((vtype_t)f->local_types[idx]);
            break;
        }
        case OP_LOCAL_SET: {
            uint32_t idx; RD_U32(idx);
            if (idx >= f->n_locals) FAIL();
            POP((vtype_t)f->local_types[idx]);
            break;
        }
        case OP_LOCAL_TEE: {
            uint32_t idx; RD_U32(idx);
            if (idx >= f->n_locals) FAIL();
            vtype_t t = (vtype_t)f->local_types[idx];
            POP(t);
            PUSH(t);
            break;
        }
        case OP_GLOBAL_GET: {
            uint32_t idx; RD_U32(idx);
            if (idx >= m->n_globals) FAIL();
            PUSH((vtype_t)m->globals[idx].type);
            break;
        }
        case OP_GLOBAL_SET: {
            uint32_t idx; RD_U32(idx);
            if (idx >= m->n_globals) FAIL();
            if (!m->globals[idx].mutable_) FAIL();   // set on immutable global
            POP((vtype_t)m->globals[idx].type);
            break;
        }

        // ---- Memory loads/stores (natural-alignment log2 in the call) ----
        case OP_I32_LOAD:     if (!ck_load(c, 2, WASM_I32)) return false; break;
        case OP_I64_LOAD:     if (!ck_load(c, 3, WASM_I64)) return false; break;
        case OP_F32_LOAD:     if (!ck_load(c, 2, WASM_F32)) return false; break;
        case OP_F64_LOAD:     if (!ck_load(c, 3, WASM_F64)) return false; break;
        case OP_I32_LOAD8_S:  case OP_I32_LOAD8_U:  if (!ck_load(c, 0, WASM_I32)) return false; break;
        case OP_I32_LOAD16_S: case OP_I32_LOAD16_U: if (!ck_load(c, 1, WASM_I32)) return false; break;
        case OP_I64_LOAD8_S:  case OP_I64_LOAD8_U:  if (!ck_load(c, 0, WASM_I64)) return false; break;
        case OP_I64_LOAD16_S: case OP_I64_LOAD16_U: if (!ck_load(c, 1, WASM_I64)) return false; break;
        case OP_I64_LOAD32_S: case OP_I64_LOAD32_U: if (!ck_load(c, 2, WASM_I64)) return false; break;

        case OP_I32_STORE:    if (!ck_store(c, 2, WASM_I32)) return false; break;
        case OP_I64_STORE:    if (!ck_store(c, 3, WASM_I64)) return false; break;
        case OP_F32_STORE:    if (!ck_store(c, 2, WASM_F32)) return false; break;
        case OP_F64_STORE:    if (!ck_store(c, 3, WASM_F64)) return false; break;
        case OP_I32_STORE8:   if (!ck_store(c, 0, WASM_I32)) return false; break;
        case OP_I32_STORE16:  if (!ck_store(c, 1, WASM_I32)) return false; break;
        case OP_I64_STORE8:   if (!ck_store(c, 0, WASM_I64)) return false; break;
        case OP_I64_STORE16:  if (!ck_store(c, 1, WASM_I64)) return false; break;
        case OP_I64_STORE32:  if (!ck_store(c, 2, WASM_I64)) return false; break;

        case OP_MEMORY_SIZE: {
            uint32_t mi; RD_U32(mi);
            if (!m->has_mem || mi != 0) FAIL();
            PUSH(WASM_I32);
            break;
        }
        case OP_MEMORY_GROW: {
            uint32_t mi; RD_U32(mi);
            if (!m->has_mem || mi != 0) FAIL();
            POP(WASM_I32);
            PUSH(WASM_I32);
            break;
        }

        // ---- Constants ----
        case OP_I32_CONST: { int32_t v; RD_S32(v); PUSH(WASM_I32); break; }
        case OP_I64_CONST: { int64_t v; RD_S64(v); PUSH(WASM_I64); break; }
        case OP_F32_CONST: {
            if ((uint64_t)c->pc + 4 > c->code_len) FAIL();
            c->pc += 4; PUSH(WASM_F32); break;
        }
        case OP_F64_CONST: {
            if ((uint64_t)c->pc + 8 > c->code_len) FAIL();
            c->pc += 8; PUSH(WASM_F64); break;
        }

        // ---- i32 comparisons / test ----
        case OP_I32_EQZ: if (!testop(c, WASM_I32)) return false; break;
        case OP_I32_EQ: case OP_I32_NE:
        case OP_I32_LT_S: case OP_I32_LT_U: case OP_I32_GT_S: case OP_I32_GT_U:
        case OP_I32_LE_S: case OP_I32_LE_U: case OP_I32_GE_S: case OP_I32_GE_U:
            if (!relop(c, WASM_I32)) return false; break;

        case OP_I64_EQZ: if (!testop(c, WASM_I64)) return false; break;
        case OP_I64_EQ: case OP_I64_NE:
        case OP_I64_LT_S: case OP_I64_LT_U: case OP_I64_GT_S: case OP_I64_GT_U:
        case OP_I64_LE_S: case OP_I64_LE_U: case OP_I64_GE_S: case OP_I64_GE_U:
            if (!relop(c, WASM_I64)) return false; break;

        case OP_F32_EQ: case OP_F32_NE: case OP_F32_LT:
        case OP_F32_GT: case OP_F32_LE: case OP_F32_GE:
            if (!relop(c, WASM_F32)) return false; break;
        case OP_F64_EQ: case OP_F64_NE: case OP_F64_LT:
        case OP_F64_GT: case OP_F64_LE: case OP_F64_GE:
            if (!relop(c, WASM_F64)) return false; break;

        // ---- i32 arithmetic / bitwise ----
        case OP_I32_CLZ: case OP_I32_CTZ: case OP_I32_POPCNT:
            if (!unop(c, WASM_I32, WASM_I32)) return false; break;
        case OP_I32_ADD: case OP_I32_SUB: case OP_I32_MUL:
        case OP_I32_DIV_S: case OP_I32_DIV_U: case OP_I32_REM_S: case OP_I32_REM_U:
        case OP_I32_AND: case OP_I32_OR: case OP_I32_XOR:
        case OP_I32_SHL: case OP_I32_SHR_S: case OP_I32_SHR_U:
        case OP_I32_ROTL: case OP_I32_ROTR:
            if (!binop(c, WASM_I32, WASM_I32)) return false; break;

        // ---- i64 arithmetic / bitwise ----
        case OP_I64_CLZ: case OP_I64_CTZ: case OP_I64_POPCNT:
            if (!unop(c, WASM_I64, WASM_I64)) return false; break;
        case OP_I64_ADD: case OP_I64_SUB: case OP_I64_MUL:
        case OP_I64_DIV_S: case OP_I64_DIV_U: case OP_I64_REM_S: case OP_I64_REM_U:
        case OP_I64_AND: case OP_I64_OR: case OP_I64_XOR:
        case OP_I64_SHL: case OP_I64_SHR_S: case OP_I64_SHR_U:
        case OP_I64_ROTL: case OP_I64_ROTR:
            if (!binop(c, WASM_I64, WASM_I64)) return false; break;

        // ---- f32 ----
        case OP_F32_ABS: case OP_F32_NEG: case OP_F32_CEIL: case OP_F32_FLOOR:
        case OP_F32_TRUNC: case OP_F32_NEAREST: case OP_F32_SQRT:
            if (!unop(c, WASM_F32, WASM_F32)) return false; break;
        case OP_F32_ADD: case OP_F32_SUB: case OP_F32_MUL: case OP_F32_DIV:
        case OP_F32_MIN: case OP_F32_MAX: case OP_F32_COPYSIGN:
            if (!binop(c, WASM_F32, WASM_F32)) return false; break;

        // ---- f64 ----
        case OP_F64_ABS: case OP_F64_NEG: case OP_F64_CEIL: case OP_F64_FLOOR:
        case OP_F64_TRUNC: case OP_F64_NEAREST: case OP_F64_SQRT:
            if (!unop(c, WASM_F64, WASM_F64)) return false; break;
        case OP_F64_ADD: case OP_F64_SUB: case OP_F64_MUL: case OP_F64_DIV:
        case OP_F64_MIN: case OP_F64_MAX: case OP_F64_COPYSIGN:
            if (!binop(c, WASM_F64, WASM_F64)) return false; break;

        // ---- Conversions ----
        case OP_I32_WRAP_I64:       if (!unop(c, WASM_I64, WASM_I32)) return false; break;
        case OP_I32_TRUNC_F32_S: case OP_I32_TRUNC_F32_U:
            if (!unop(c, WASM_F32, WASM_I32)) return false; break;
        case OP_I32_TRUNC_F64_S: case OP_I32_TRUNC_F64_U:
            if (!unop(c, WASM_F64, WASM_I32)) return false; break;
        case OP_I64_EXTEND_I32_S: case OP_I64_EXTEND_I32_U:
            if (!unop(c, WASM_I32, WASM_I64)) return false; break;
        case OP_I64_TRUNC_F32_S: case OP_I64_TRUNC_F32_U:
            if (!unop(c, WASM_F32, WASM_I64)) return false; break;
        case OP_I64_TRUNC_F64_S: case OP_I64_TRUNC_F64_U:
            if (!unop(c, WASM_F64, WASM_I64)) return false; break;
        case OP_F32_CONVERT_I32_S: case OP_F32_CONVERT_I32_U:
            if (!unop(c, WASM_I32, WASM_F32)) return false; break;
        case OP_F32_CONVERT_I64_S: case OP_F32_CONVERT_I64_U:
            if (!unop(c, WASM_I64, WASM_F32)) return false; break;
        case OP_F32_DEMOTE_F64:     if (!unop(c, WASM_F64, WASM_F32)) return false; break;
        case OP_F64_CONVERT_I32_S: case OP_F64_CONVERT_I32_U:
            if (!unop(c, WASM_I32, WASM_F64)) return false; break;
        case OP_F64_CONVERT_I64_S: case OP_F64_CONVERT_I64_U:
            if (!unop(c, WASM_I64, WASM_F64)) return false; break;
        case OP_F64_PROMOTE_F32:    if (!unop(c, WASM_F32, WASM_F64)) return false; break;
        case OP_I32_REINTERPRET_F32: if (!unop(c, WASM_F32, WASM_I32)) return false; break;
        case OP_I64_REINTERPRET_F64: if (!unop(c, WASM_F64, WASM_I64)) return false; break;
        case OP_F32_REINTERPRET_I32: if (!unop(c, WASM_I32, WASM_F32)) return false; break;
        case OP_F64_REINTERPRET_I64: if (!unop(c, WASM_I64, WASM_F64)) return false; break;

        // ---- sign-extension ----
        case OP_I32_EXTEND8_S: case OP_I32_EXTEND16_S:
            if (!unop(c, WASM_I32, WASM_I32)) return false; break;
        case OP_I64_EXTEND8_S: case OP_I64_EXTEND16_S: case OP_I64_EXTEND32_S:
            if (!unop(c, WASM_I64, WASM_I64)) return false; break;

        default:
            FAIL();     // unknown opcode -> invalid
        }
    }

done:
    return true;
}

// Free any control frames left after a failed body check.
static void vctx_cleanup(vctx *c) {
    for (uint32_t i = 0; i < c->n_ctrl; i++) {
        free(c->ctrl[i].in);
        free(c->ctrl[i].out);
    }
    free(c->ctrl);
    free(c->opnds);
    c->ctrl = NULL;
    c->opnds = NULL;
    c->n_ctrl = c->n_opnds = 0;
}

// ---- Module-level structural / limits checks ---------------------------

static bool validate_module_structure(wasm_module_t *m) {
    // Type indices on every function (imported + defined) must be in range.
    for (uint32_t i = 0; i < m->n_imports; i++) {
        if (m->imports[i].kind == WASM_EXTERN_FUNC &&
            m->imports[i].type_index >= m->n_types)
            return false;
    }
    for (uint32_t i = 0; i < m->n_funcs; i++)
        if (m->funcs[i].type_index >= m->n_types)
            return false;

    // Memory limits.
    if (m->has_mem) {
        if (m->mem_max_pages != 0 && m->mem_max_pages < m->mem_min_pages)
            return false;
        if (m->mem_min_pages > 65536) return false;
        if (m->mem_max_pages > 65536) return false;
    }

    // Table limits.
    if (m->has_table) {
        if (m->table_max != 0 && m->table_max < m->table_min)
            return false;
    }

    uint32_t total_funcs = m->n_imported_funcs + m->n_funcs;

    // Globals: init type matches declared type (decode already evaluated it).
    for (uint32_t i = 0; i < m->n_globals; i++) {
        // The init value's type is implied by how decode stored it; decode
        // already rejected a mismatch, but re-assert the declared valtype is a
        // real value type.
        wasm_valtype_t t = m->globals[i].type;
        if (!is_valtype((uint8_t)t)) return false;
    }

    // Element segments: each entry is a valid function index.
    if (m->n_elem && !m->has_table) return false;
    for (uint32_t s = 0; s < m->n_elem; s++) {
        for (uint32_t k = 0; k < m->elem[s].n; k++)
            if (m->elem[s].func_indices[k] >= total_funcs)
                return false;
    }

    // Data segments: require a memory.
    if (m->n_data && !m->has_mem) return false;

    // Exports: in range for their kind + unique names.
    for (uint32_t i = 0; i < m->n_exports; i++) {
        wasm_export_t *e = &m->exports[i];
        switch (e->kind) {
        case WASM_EXTERN_FUNC:
            if (e->index >= total_funcs) return false;
            break;
        case WASM_EXTERN_TABLE:
            if (!m->has_table || e->index != 0) return false;
            break;
        case WASM_EXTERN_MEM:
            if (!m->has_mem || e->index != 0) return false;
            break;
        case WASM_EXTERN_GLOBAL:
            if (e->index >= m->n_globals) return false;
            break;
        default:
            return false;
        }
        for (uint32_t j = 0; j < i; j++)
            if (strcmp(m->exports[j].name, e->name) == 0)
                return false;   // duplicate export name
    }

    // Start function: in range and type [] -> [].
    if (m->has_start) {
        if (m->start_func >= total_funcs) return false;
        const wasm_functype_t *s = func_sig(m, m->start_func);
        if (!s || s->n_params != 0 || s->n_results != 0) return false;
    }

    return true;
}

// ---- Entry point --------------------------------------------------------

wasm_result_t wasm_validate_impl(wasm_module_t *m) {
    if (!m) return WASM_ERR_VALIDATE;

    if (!validate_module_structure(m))
        return WASM_ERR_VALIDATE;

    // Type-check every defined function body.
    for (uint32_t i = 0; i < m->n_funcs; i++) {
        wasm_func_t *f = &m->funcs[i];
        if (f->type_index >= m->n_types) return WASM_ERR_VALIDATE;
        // local_types must cover all locals (decode allocates it; be defensive).
        if (f->n_locals != 0 && !f->local_types) return WASM_ERR_VALIDATE;
        for (uint32_t k = 0; k < f->n_locals; k++)
            if (!is_valtype((uint8_t)f->local_types[k])) return WASM_ERR_VALIDATE;

        vctx c;
        memset(&c, 0, sizeof(c));
        c.m = m;
        c.f = f;
        c.code = f->code;
        c.code_len = f->code_len;
        c.pc = 0;

        bool ok = check_body(&c);
        if (!ok || c.failed) {
            vctx_cleanup(&c);
            return WASM_ERR_VALIDATE;
        }
        // After a successful check, the outermost frame was popped on its END.
        // Any operands or open frames left over mean a malformed body.
        if (c.n_ctrl != 0 || c.n_opnds != 0) {
            vctx_cleanup(&c);
            return WASM_ERR_VALIDATE;
        }
        vctx_cleanup(&c);
    }

    return WASM_OK;
}
