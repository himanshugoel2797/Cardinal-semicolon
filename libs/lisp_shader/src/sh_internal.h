// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#ifndef CARDINAL_LISP_SHADER_INTERNAL_H
#define CARDINAL_LISP_SHADER_INTERNAL_H

// Private IR + the three cross-unit seams. This is the ONE file all three
// implementation units include; it is frozen before fan-out. If you must change
// it (a new op, a new field), coordinate -- it is the shared surface.
//
// The IR is a typed AST in a flat arena of fixed-size nodes addressed by 32-bit
// index (sh_nref), not a pointer-linked tree: one allocation per program, cheap
// to free, trivially serializable later. The verifier ANNOTATES nodes in place
// (filling sh_node.type, loop bounds, the cost). The interpreter walks the
// verified tree. See notes/scratch/shader-proposal-minimalist.md for the rationale.

#include "lisp_shader.h"

typedef uint32_t sh_nref;  // index into sh_program.nodes
#define SH_NREF_NONE 0xFFFFFFFFu

// --- operations -------------------------------------------------------------
typedef enum {
    SH_OP_CONST,         // literal: payload in node.imm, type in node.type
    SH_OP_PARAM,         // read parameter #a
    SH_OP_LOCAL,         // read let/loop-bound local: a = slot index
    SH_OP_UNOP,          // sub = sh_unop;  a = operand
    SH_OP_BINOP,         // sub = sh_binop; a,b = operands
    SH_OP_CMP,           // sub = sh_cmp;   a,b = operands -> bool
    SH_OP_IF,            // a = cond(bool), b = then, c = else (arms same type)
    SH_OP_LET,           // bind slots; bindings + body in aux[] (see frontend convention)
    SH_OP_REGION_LOAD,   // a = region, b = index; elem type = node.type
    SH_OP_REGION_STORE,  // a = region, b = index, c = value (region must be mutable)
    SH_OP_REGION_LEN,    // a = region -> u32 length
    SH_OP_LOOP,          // named-let loop header; a = index into sh_program.loops
    SH_OP_RECUR,         // tail re-entry of enclosing loop; new induction args in aux[]
    SH_OP_CALL,          // a = prim index in prim_set; args in aux[]
    // --- S2 vectors (scalar-lowered in the interpreter; SIMD is perf-only) ---
    SH_OP_VSPLAT,        // a = scalar -> <N x T> broadcast (N,T from node.type)
    SH_OP_VBINOP,        // sub = sh_binop; a,b = <N x T>, lane-wise
    SH_OP_VCMP,          // sub = sh_cmp;   a,b = <N x T> -> mask<N>
    SH_OP_VSELECT,       // a = mask<N>, b = then<N x T>, c = else<N x T>
    SH_OP_VSHUFFLE,      // a = src<N x T>; constant lane indices in aux[]
    SH_OP_VREDUCE,       // sub = sh_reduce; a = <N x T> -> scalar T (dot uses b)
    SH_OP_VLANE,         // a = <N x T>, lane const in imm -> scalar T (extract)
} sh_op;

typedef enum { SH_UN_NEG, SH_UN_NOT, SH_UN_CVT } sh_unop;  // CVT: result kind = node.type
typedef enum {
    SH_BIN_ADD, SH_BIN_SUB, SH_BIN_MUL, SH_BIN_DIV, SH_BIN_MOD,
    SH_BIN_AND, SH_BIN_OR, SH_BIN_XOR, SH_BIN_SHL, SH_BIN_SHR,
} sh_binop;
typedef enum { SH_CMP_LT, SH_CMP_LE, SH_CMP_EQ, SH_CMP_NE, SH_CMP_GT, SH_CMP_GE } sh_cmp;
typedef enum { SH_RED_ADD, SH_RED_MIN, SH_RED_MAX, SH_RED_DOT } sh_reduce;

// Fixed-size node. Variadic operands (LET bindings, CALL/RECUR args, SHUFFLE
// indices) live in the shared aux[] addressed by (aux_off, aux_len).
typedef struct {
    uint16_t op;        // sh_op
    uint16_t sub;       // sh_unop/sh_binop/sh_cmp/sh_reduce, else 0
    sh_type  type;      // result type -- FILLED BY THE VERIFIER (zeroed at parse)
    uint8_t  vflags;    // verifier flags (e.g. SH_NF_BOUNDS_PROVEN); 0 at parse
    uint8_t  pad[3];
    uint32_t a, b, c;   // child sh_nref or small immediate; SH_NREF_NONE if unused
    uint32_t aux_off;   // start in sh_program.aux[]
    uint32_t aux_len;   // count there
    int64_t  imm;       // CONST payload / lane index; floats via the f64 bit pattern
} sh_node;

#define SH_NF_BOUNDS_PROVEN 0x1u  // verifier discharged this region access statically

// --- bounded loop (named-let) -----------------------------------------------
typedef enum { SH_BOUND_NONE = 0, SH_BOUND_CONST, SH_BOUND_PARAM } sh_bound_kind;
typedef struct {
    sh_bound_kind kind;
    uint64_t      konst;          // CONST: the trip-count ceiling
    uint32_t      param_idx;      // PARAM: trip count <= value of this u32 param
    uint64_t      per_iter_cost;  // worst-case cost of one body iteration
} sh_bound;

typedef struct {
    uint32_t nvars;     // induction variables
    uint32_t var_slot0; // first local slot they occupy
    sh_nref  init_off;  // aux[] start of nvars init exprs (sh_nref each)
    sh_nref  body;      // loop body (contains SH_OP_RECUR in tail position)
    sh_bound bound;     // filled by the verifier
} sh_loop;

// --- whole-program worst-case cost ------------------------------------------
typedef struct {
    bool     is_const;    // true => const_cost exact; false => use sh_cost_for_args
    uint64_t const_cost;  // valid iff is_const
} sh_cost;

// --- the compiled program: one owned allocation -----------------------------
struct sh_program {
    char     name[64];
    uint32_t nparams;
    sh_type  params[SH_MAX_PARAMS];
    sh_type  ret;

    sh_node *nodes;  uint32_t nnodes, cap_nodes;   // IR arena
    uint32_t *aux;   uint32_t naux,   cap_aux;      // variadic operand side-array
    sh_loop *loops;  uint32_t nloops, cap_loops;    // SH_OP_LOOP side records
    sh_nref  root;   // entry expression

    uint32_t nlocals;  // number of local slots (let/loop bindings) for the interp
    sh_cost  cost;     // verifier result

    const sh_prim_set *prims;  // capability set captured at compile (interp needs fns)
    bool verified;             // set true by shv_verify on SH_OK
};

// --- arena builders (owned by the frontend; declared for clarity) -----------
// Append a node, returns its nref (or SH_NREF_NONE on OOM). Grows nodes[].
sh_nref sh_node_alloc(sh_program *p, sh_op op);
// Reserve `n` aux slots, returns the offset (sets *off); grows aux[].
bool    sh_aux_reserve(sh_program *p, uint32_t n, uint32_t *off);
sh_nref sh_loop_alloc(sh_program *p, uint32_t *out_index);

// --- shared error helper (sh_error.c; used by ALL units) --------------------
// Set *err (if non-NULL) and return `status`. Format like printf.
sh_status sh_set_error(sh_error *err, sh_status status, int line, int col,
                       const char *fmt, ...) __attribute__((format(printf, 5, 6)));

// =====================  THE THREE CROSS-UNIT SEAMS  =========================
// UNIT A (frontend): (defshader ...) datum -> structured untyped AST in `p`.
// On return SH_OK, p has nodes/aux/loops/params/ret/name/root filled, types
// zeroed. p is caller-allocated+zeroed (sh_compile owns the arena lifetime).
sh_status shf_parse(lisp_value form, sh_program *p, sh_error *err);

// UNIT B (verifier, the moat): type-check the parsed program IN PLACE, fill
// sh_node.type, sh_loop.bound, p->cost, p->nlocals, validate caps/bounds. After
// SH_OK the program is verified and safe to interpret. No allocation, no I/O.
sh_status shv_verify(sh_program *p, const sh_prim_set *prims, uint32_t flags,
                     sh_error *err);

// UNIT C (interpreter): run a VERIFIED program.
sh_status shi_invoke(const sh_program *p, const sh_value *args, uint32_t argc,
                     sh_value *out, sh_error *err);
uint64_t  shi_cost_for_args(const sh_program *p, const sh_value *args, uint32_t argc);

#endif  // CARDINAL_LISP_SHADER_INTERNAL_H
