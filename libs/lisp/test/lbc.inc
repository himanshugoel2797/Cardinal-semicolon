// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

// lbc -- a prototype bytecode compiler + threaded VM for the base Lisp, built
// per notes/core/lisp-bytecode.md. This is a HOST-ONLY prototype #included by a
// single test (test_bytecode.c); it is deliberately NOT in libs/lisp/src (so it
// never enters the kernel build or other tests) until the design is reviewed and
// wired into the live evaluator as a real S-stage.
//
// It demonstrates the architecture the note proposes:
//   - cons-AST -> flat bytecode `chunk` with LEXICAL ADDRESSING (locals are
//     slots, resolved at compile time; no runtime frame_find scan),
//   - FLAT, BY-VALUE upvalue capture (immutable bindings => no escape analysis,
//     no shared mutable env cells),
//   - FROZEN-OP fast paths (+, -, *, comparisons, cons/car/cdr/null?/pair?/not,
//     eq?) inlined as opcodes, with an exact fallback to the real primitive for
//     the non-fixnum cases so results match the tree-walker bit-for-bit,
//   - a THREADED VM with an explicit frame stack and PROPER TAIL CALLS, so loops
//     (named-let / tail self-calls) run in O(1) frames,
//   - DECLINE-TO-ORACLE: anything outside coverage makes the compiler decline,
//     and the harness runs the form on the tree-walker -- so adding the backend
//     can never regress correctness (the shader-JIT discipline).
//
// Values flowing through the VM are ordinary `lisp_value`s (dynamic typing is
// preserved). Compiled closures are C-heap `bcclosure` structs referenced inside
// the VM with the otherwise-unused tag 0b11 (LISP_TAG reserved); they never reach
// the GC or the oracle. The prototype runs with the GC uninitialized (grow-only),
// so there are no rooting concerns for a bounded test corpus.

#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "lisp.h"
#include "../src/internal.h"  // lisp_prim_t (for the thin VM->primitive call path)

// When set, a VM call to a built-in primitive invokes its C function pointer
// directly with the operand-stack slice (zero-copy args, one err branch) instead
// of routing through lisp_apply (which drives the full CEK machine). This is the
// "break the C ABI for the hot path" experiment; toggled by the bench.
static int g_thin_prim = 1;

// When set, a reference to a global resolves to its binding CELL at compile time
// and loads via one cdr deref at runtime (the env stores each binding as a
// mutable (sym . val) cons that define/set! mutate in place, so the cell is a
// stable, repatchable slot) -- instead of a per-execution hash lookup. Toggled
// by the bench.
static int g_global_slots = 1;

// Call a Lisp procedure value with args (already contiguous, e.g. on the operand
// stack). Primitives go through the thin path when enabled; closures (and the
// non-thin mode) go through lisp_apply.
static lisp_value lbc_call_proc(lisp_value callee, lisp_value *args, int n,
                                const char **err) {
    if (g_thin_prim && lisp_is_objtype(callee, LISP_OBJ_PRIMITIVE))
        return ((lisp_prim_t *)lisp_obj(callee))->fn(args, n, err);
    return lisp_apply(callee, args, n, err);
}

// A compiled-closure pointer is carried inside a lisp_value using tag 0b11, which
// the core value representation reserves and never produces. The VM is the only
// thing that inspects these; real lisp values never collide.
#define LBC_TAG_CLO 0x3u
#define LBC_IS_CLO(v) (((v) & LISP_TAG_MASK) == LBC_TAG_CLO)
#define LBC_MK_CLO(p) ((lisp_value)((uintptr_t)(p) | LBC_TAG_CLO))
#define LBC_CLO(v) ((bcclosure *)(uintptr_t)((v) & ~(uintptr_t)LISP_TAG_MASK))

#define LBC_MAX_LOCALS 64
#define LBC_MAX_STACK 256
#define LBC_MAX_FRAMES 4096

// --- Bytecode ---------------------------------------------------------------

typedef enum {
    OP_CONST,        // push consts[a]
    OP_LOADLOCAL,    // push locals[a]
    OP_LOADUPVAL,    // push upvals[a]
    OP_LOADGLOBAL,   // push (lookup symbol consts[a] in genv); error if unbound
    OP_LOADGLOBAL_SLOT,  // push (cdr consts[a]) -- consts[a] is the binding cell
    OP_LOADSELF,     // push the running closure (for self-recursion)
    OP_STORELOCAL,   // locals[a] = pop          (let init / internal define)
    OP_STOREGLOBAL,  // define/set! consts[a] = top (leaves value on stack)
    OP_SETGLOBAL,    // set! consts[a] = pop; error if unbound; push undef
    OP_POP,          // drop top
    OP_CLOSURE,      // push a closure of child chunk a, capturing per its updesc
    OP_JMP,          // pc = a
    OP_JMPF,         // v=pop; if !truthy pc=a
    OP_JMPF_KEEP,    // if !truthy(top) pc=a (keep top); else pop      (for and)
    OP_JMPT_KEEP,    // if  truthy(top) pc=a (keep top); else pop      (for or)
    OP_CALL,         // call top-(a) ... callee then a args; result pushed
    OP_TAILCALL,     // tail call (reuse frame); terminates the frame
    OP_RET,          // return top
    // frozen ops (binary unless noted). a selects the variant; for ARITH/CMP, b
    // is the const index of the real primitive for the non-fixnum fallback.
    OP_ARITH2,       // a: 0=+ 1=- 2=*
    OP_CMP2,         // a: 0=< 1=<= 2=> 3=>= 4== (numeric)
    OP_EQ,           // eq? (identity)
    OP_CONS,
    OP_CAR,
    OP_CDR,
    OP_NULLP,
    OP_PAIRP,
    OP_NOT,
} bcop;

typedef struct {
    uint8_t op;
    int32_t a;
    int32_t b;
} instr;

// Register-form instruction (3 register operands + an aux const index). The
// register VM (rvm_run) reads these; a=dst, b/c=src registers, d=aux const.
typedef enum {
    ROP_LOADK,      // r[a] = consts[b]
    ROP_MOVE,       // r[a] = r[b]
    ROP_LOADUP,     // r[a] = upvals[b]
    ROP_LOADG,      // r[a] = lookup(symbol consts[b]); error if unbound
    ROP_LOADGS,     // r[a] = cdr consts[b]   (global slot)
    ROP_LOADSELF,   // r[a] = running closure
    ROP_SETG,       // set! symbol consts[b] = r[a]; error if unbound
    ROP_CLOSURE,    // r[a] = closure of child chunk b
    ROP_JMP,        // pc = a
    ROP_JMPF,       // if !truthy(r[a]) pc = b
    ROP_JMPT,       // if  truthy(r[a]) pc = b
    ROP_CALL,       // call r[a] with b args at r[a+1..a+b]; result -> r[a]
    ROP_TAILCALL,   // tail call (reuse frame); terminates
    ROP_RET,        // return r[a]
    ROP_OPCALL,     // r[a] = inline-cached op ics[d] on r[b] (and r[c] if binary)
    ROP_MKCELL,     // r[a] = box(r[a])      -- wrap a captured binding in a cell
    ROP_CELLGET,    // r[a] = cell-ref(r[b]) -- read through a captured binding
    ROP_CELLSET,    // cell-set!(r[a], r[b]) -- write through a captured binding
} rop;

typedef struct {
    uint8_t op;
    int32_t a, b, c, d;
} rinstr;

// Inline-cache fast-op selectors. These name the CANONICAL semantics of a core
// operator; the IC's `expected` pins the builtin primitive that implements them
// (Decision 2: core ops stay redefinable -- the inlined path runs ONLY while the
// operator still resolves to that exact builtin; otherwise we call whatever it
// is now). FK_*_BIN take r[b],r[c]; FK_*_UN take r[b].
typedef enum {
    FK_ADD, FK_SUB, FK_MUL,                  // binary fixnum arithmetic
    FK_LT, FK_LE, FK_GT, FK_GE, FK_NUMEQ,    // binary numeric comparison
    FK_EQ, FK_CONS,                          // binary: identity, cons
    FK_CAR, FK_CDR, FK_NULLP, FK_PAIRP, FK_NOT,  // unary
} fastkind;

static bool fastkind_unary(fastkind fk) { return fk >= FK_CAR; }

// One inline cache: the operator's global binding CELL (a stable (sym . val)
// cons that define/set! mutate in place), the builtin primitive `expected` that
// was canonical when this site compiled, and the fast-op `kind`. At runtime the
// VM reads cur = cdr(cell); if cur == expected the inlined path is valid,
// otherwise it calls cur (handles redefinition + off-type operands uniformly).
typedef struct {
    lisp_value cell;
    lisp_value expected;
    uint8_t kind;
} bcic;

typedef struct bcchunk {
    instr *code;
    int ncode, capcode;
    rinstr *rcode;     // register-form code (NULL if stack-compiled)
    int nrcode, caprcode;
    int nregs;         // register-file size for the register VM
    lisp_value *consts;
    int nconsts, capconsts;
    bcic *ics;         // inline caches for ROP_OPCALL sites
    int nics, capics;
    int nlocals;       // size of the runtime locals array (stack VM)
    int nparams;
    bool has_rest;     // last param collects the rest as a list
    struct bcchunk **children;
    int nchildren, capchildren;
    struct {           // how to build a closure of THIS chunk from its parent
        bool from_local;
        int index;
    } updesc[LBC_MAX_LOCALS];
    int nupdesc;
} bcchunk;

typedef struct bcclosure {
    bcchunk *chunk;
    lisp_value upvals[];  // nupdesc entries, captured by value
} bcclosure;

// --- Compiler ---------------------------------------------------------------

typedef struct fnstate {
    struct fnstate *parent;
    bcchunk *chunk;
    struct {
        lisp_value name;  // interned symbol
        int slot;
        bool boxed;       // captured by an inner lambda -> stored as a heap cell
    } locals[LBC_MAX_LOCALS];
    int nlocals;          // active lexical bindings (a scope stack)
    int next_slot;        // monotonic slot high-water (== chunk->nlocals)
    lisp_value self_name; // this function's own name for self-recursion, or UNDEF
    int freereg;          // register VM: next free register (locals + temps share)
    int maxreg;           // register VM: high-water (== chunk->nregs)
} fnstate;

typedef struct {
    lisp_value genv;       // global env, for resolving globals + frozen prims
    jmp_buf decline;       // longjmp target when a form is unsupported
    const char *why;       // decline reason
} compiler;

static void lbc_decline(compiler *C, const char *why) {
    C->why = why;
    longjmp(C->decline, 1);
}

static void *lbc_xalloc(compiler *C, size_t n) {
    void *p = calloc(1, n);
    if (p == NULL)
        lbc_decline(C, "out of memory");
    return p;
}

static bcchunk *chunk_new(compiler *C) {
    return (bcchunk *)lbc_xalloc(C, sizeof(bcchunk));
}

static int emit(compiler *C, bcchunk *k, bcop op, int32_t a, int32_t b) {
    if (k->ncode == k->capcode) {
        int nc = k->capcode ? k->capcode * 2 : 32;
        instr *ni = (instr *)realloc(k->code, (size_t)nc * sizeof(instr));
        if (ni == NULL)
            lbc_decline(C, "out of memory");
        k->code = ni;
        k->capcode = nc;
    }
    int at = k->ncode;
    k->code[at].op = (uint8_t)op;
    k->code[at].a = a;
    k->code[at].b = b;
    k->ncode++;
    return at;
}

static int add_const(compiler *C, bcchunk *k, lisp_value v) {
    for (int i = 0; i < k->nconsts; i++)
        if (k->consts[i] == v)
            return i;
    if (k->nconsts == k->capconsts) {
        int nc = k->capconsts ? k->capconsts * 2 : 8;
        lisp_value *nv = (lisp_value *)realloc(k->consts, (size_t)nc * sizeof(lisp_value));
        if (nv == NULL)
            lbc_decline(C, "out of memory");
        k->consts = nv;
        k->capconsts = nc;
    }
    k->consts[k->nconsts] = v;
    return k->nconsts++;
}

// Allocate a fresh local slot in the current function (no reuse; bounded by
// source size). Returns the slot index.
static int alloc_slot(compiler *C, fnstate *fn) {
    if (fn->next_slot >= LBC_MAX_LOCALS)
        lbc_decline(C, "too many locals");
    int s = fn->next_slot++;
    if (fn->next_slot > fn->chunk->nlocals)
        fn->chunk->nlocals = fn->next_slot;
    return s;
}

static void bind_local(compiler *C, fnstate *fn, lisp_value name, int slot) {
    if (fn->nlocals >= LBC_MAX_LOCALS)
        lbc_decline(C, "too many locals");
    fn->locals[fn->nlocals].name = name;
    fn->locals[fn->nlocals].slot = slot;
    fn->locals[fn->nlocals].boxed = false;
    fn->nlocals++;
}

// Mark the most recently bound local as captured (stored in a heap cell).
static void mark_boxed(fnstate *fn) {
    if (fn->nlocals > 0)
        fn->locals[fn->nlocals - 1].boxed = true;
}

// Resolution result kinds.
enum { R_LOCAL, R_UPVAL, R_SELF, R_GLOBAL };

static int resolve_upval(compiler *C, fnstate *fn, lisp_value name, int *kindout);

// Add an upvalue descriptor to fn capturing (from_local, index); dedup.
static int add_upval(compiler *C, fnstate *fn, bool from_local, int index) {
    bcchunk *k = fn->chunk;
    for (int i = 0; i < k->nupdesc; i++)
        if (k->updesc[i].from_local == from_local && k->updesc[i].index == index)
            return i;
    if (k->nupdesc >= LBC_MAX_LOCALS)
        lbc_decline(C, "too many upvalues");
    k->updesc[k->nupdesc].from_local = from_local;
    k->updesc[k->nupdesc].index = index;
    return k->nupdesc++;
}

// Resolve `name` as seen from fn's PARENT chain: a parent local becomes an
// upvalue captured from the parent frame; a parent upvalue is captured
// transitively. Returns the upvalue index in fn, or -1 if it is a global.
static int resolve_upval(compiler *C, fnstate *fn, lisp_value name, int *kindout) {
    fnstate *p = fn->parent;
    if (p == NULL)
        return -1;
    for (int i = p->nlocals - 1; i >= 0; i--)
        if (p->locals[i].name == name) {
            // A captured parent local is always boxed (the capture analysis ran
            // on the parent), so the upvalue refers to its heap cell by
            // reference -- shared mutable state, which is what makes set! on a
            // captured binding and mutual/forward recursion work.
            *kindout = R_UPVAL;
            return add_upval(C, fn, true, p->locals[i].slot);
        }
    // p->self_name is only a fast-path alias usable inside p's OWN frame
    // (R_SELF); from a child, the name resolves to its real boxed binding in an
    // ancestor frame, so we keep climbing. A name bound as a sibling/ancestor
    // cell resolves fine (mutual/forward recursion). The one case we cannot do
    // is capturing a frame's OWN self-closure that has no cell -- a named-let
    // loop name or simple internal define referenced from a nested lambda; that
    // name is bound nowhere as a local, so the climb returns -1 and we decline.
    int pk;
    int up = resolve_upval(C, p, name, &pk);
    if (up < 0) {
        if (name == p->self_name)
            lbc_decline(C, "capture of enclosing self not supported");
        return -1;
    }
    *kindout = R_UPVAL;
    return add_upval(C, fn, false, up);
}

// Resolve a name in fn: local slot, self, an upvalue index, or global. If
// `boxed` is non-NULL it reports whether the binding is a heap cell (every
// R_UPVAL is, since captured locals are always boxed; an R_LOCAL is iff it was
// captured by an inner lambda).
static int resolve(compiler *C, fnstate *fn, lisp_value name, int *kind,
                   bool *boxed) {
    if (boxed != NULL)
        *boxed = false;
    for (int i = fn->nlocals - 1; i >= 0; i--)
        if (fn->locals[i].name == name) {
            *kind = R_LOCAL;
            if (boxed != NULL)
                *boxed = fn->locals[i].boxed;
            return fn->locals[i].slot;
        }
    if (name == fn->self_name) {
        *kind = R_SELF;
        return 0;
    }
    int k2;
    int up = resolve_upval(C, fn, name, &k2);
    if (up >= 0) {
        *kind = R_UPVAL;
        if (boxed != NULL)
            *boxed = true;
        return up;
    }
    *kind = R_GLOBAL;
    return 0;
}

// --- form / frozen-op recognition (by name; the compiler is not hot) --------

static bool sym_is(lisp_value s, const char *name) {
    if (!lisp_is_symbol(s))
        return false;
    size_t n = lisp_named_len(s);
    return n == strlen(name) && memcmp(lisp_named_name(s), name, n) == 0;
}

// Mutate a pair's cdr -- used only on cons cells this compiler just built
// (param/init list spines), never on user data.
static void set_cdr_lbc(lisp_value pair, lisp_value v) {
    ((lisp_pair *)lisp_obj(pair))->cdr = v;
}

// A captured (boxed) binding is a one-slot mutable heap cell, represented as a
// pair with the value in its car (a real GC object, so the value stays rooted
// and shared by every closure that captured the binding). set_car_lbc mutates a
// cell built by MKCELL; cell-ref reads it.
static void set_car_lbc(lisp_value cell, lisp_value v) {
    ((lisp_pair *)lisp_obj(cell))->car = v;
}

// Find the (sym . val) binding cell for `sym` walking `env`'s chain, or
// LISP_EMPTY. Mirrors the evaluator's frame_find/lisp_env_lookup. The cell is a
// stable slot: define/set! mutate its cdr in place, never replace it, so caching
// it at compile time stays valid across redefinition (the jump-table model).
static lisp_value find_global_cell(lisp_value env, lisp_value sym) {
    while (lisp_is_objtype(env, LISP_OBJ_ENV)) {
        lisp_env_t *e = (lisp_env_t *)lisp_obj(env);
        lisp_value chain;
        if (e->table != LISP_EMPTY) {
            size_t nb = lisp_vector_length(e->table);
            size_t b = (size_t)((lisp_named *)lisp_obj(sym))->hash & (nb - 1);
            chain = lisp_vector_ref(e->table, b);
        } else {
            chain = e->bindings;
        }
        while (lisp_is_pair(chain)) {
            lisp_value cell = lisp_car(chain);
            if (lisp_is_pair(cell) && lisp_car(cell) == sym)
                return cell;
            chain = lisp_cdr(chain);
        }
        env = e->parent;
    }
    return LISP_EMPTY;
}

// Is `head` the name of any special form (handled or declined)? Used in tail
// position to tell an application from a special form.
static bool is_special_head(lisp_value head) {
    static const char *forms[] = {
        "quote", "quasiquote", "if", "define", "lambda", "set!", "begin", "let",
        "let*", "letrec", "and", "or", "cond", "when", "unless", "while", "case",
        "define-module", "import", "include", NULL};
    for (int i = 0; forms[i]; i++)
        if (sym_is(head, forms[i]))
            return true;
    return false;
}

// Is `name` a frozen op we have a fast opcode for, at this arity, and NOT
// shadowed by a local/upvalue? Fills *op/*variant. Returns false otherwise
// (caller emits a general call instead).
static bool frozen_op(compiler *C, fnstate *fn, lisp_value head, int argc, bcop *op,
                      int *variant) {
    if (!lisp_is_symbol(head))
        return false;
    int kind;
    (void)resolve(C, fn, head, &kind, NULL);
    if (kind != R_GLOBAL)
        return false;  // shadowed -> not the frozen primitive
    if (argc == 2) {
        if (sym_is(head, "+")) { *op = OP_ARITH2; *variant = 0; return true; }
        if (sym_is(head, "-")) { *op = OP_ARITH2; *variant = 1; return true; }
        if (sym_is(head, "*")) { *op = OP_ARITH2; *variant = 2; return true; }
        if (sym_is(head, "<")) { *op = OP_CMP2; *variant = 0; return true; }
        if (sym_is(head, "<=")) { *op = OP_CMP2; *variant = 1; return true; }
        if (sym_is(head, ">")) { *op = OP_CMP2; *variant = 2; return true; }
        if (sym_is(head, ">=")) { *op = OP_CMP2; *variant = 3; return true; }
        if (sym_is(head, "=")) { *op = OP_CMP2; *variant = 4; return true; }
        if (sym_is(head, "eq?")) { *op = OP_EQ; *variant = 0; return true; }
        if (sym_is(head, "cons")) { *op = OP_CONS; *variant = 0; return true; }
    } else if (argc == 1) {
        if (sym_is(head, "car")) { *op = OP_CAR; *variant = 0; return true; }
        if (sym_is(head, "cdr")) { *op = OP_CDR; *variant = 0; return true; }
        if (sym_is(head, "null?")) { *op = OP_NULLP; *variant = 0; return true; }
        if (sym_is(head, "pair?")) { *op = OP_PAIRP; *variant = 0; return true; }
        if (sym_is(head, "not")) { *op = OP_NOT; *variant = 0; return true; }
    }
    return false;
}

// Resolve a frozen primitive (for the non-fixnum fallback) to its const slot.
static int frozen_prim_const(compiler *C, bcchunk *k, const char *name) {
    lisp_value v;
    if (!lisp_env_lookup(C->genv, lisp_make_symbol(name, strlen(name)), &v))
        lbc_decline(C, "frozen primitive unbound");
    return add_const(C, k, v);
}

// --- inline-cache (register form, no frozen ops) ----------------------------

// Canonical name for each fast-op selector (NULL-terminated lookups by index).
static const char *fastkind_name(fastkind fk) {
    switch (fk) {
        case FK_ADD: return "+";   case FK_SUB: return "-";   case FK_MUL: return "*";
        case FK_LT: return "<";    case FK_LE: return "<=";   case FK_GT: return ">";
        case FK_GE: return ">=";   case FK_NUMEQ: return "=";
        case FK_EQ: return "eq?";  case FK_CONS: return "cons";
        case FK_CAR: return "car"; case FK_CDR: return "cdr";
        case FK_NULLP: return "null?"; case FK_PAIRP: return "pair?";
        case FK_NOT: return "not";
    }
    return "";
}

// The canonical builtin primitive for each fast op, captured once (in a real OS,
// at builtin-install time; here, lazily on first compile -- before any test
// redefines an op). An IC inlines ONLY for this exact identity, so the inlined
// fixnum/pair semantics are guaranteed to match the operator's real meaning.
#define NFAST (FK_NOT + 1)
static lisp_value g_canon[NFAST];
static int g_canon_ready = 0;
static void ensure_canon(lisp_value genv) {
    if (g_canon_ready)
        return;
    for (int fk = 0; fk < NFAST; fk++) {
        const char *nm = fastkind_name((fastkind)fk);
        lisp_value v;
        g_canon[fk] = lisp_env_lookup(genv, lisp_make_symbol(nm, strlen(nm)), &v)
                          ? v : LISP_UNDEF;
    }
    g_canon_ready = 1;
}

static int add_ic(compiler *C, bcchunk *k, lisp_value cell, lisp_value expected,
                  fastkind kind) {
    if (k->nics == k->capics) {
        int nc = k->capics ? k->capics * 2 : 8;
        bcic *ni = (bcic *)realloc(k->ics, (size_t)nc * sizeof(bcic));
        if (ni == NULL)
            lbc_decline(C, "out of memory");
        k->ics = ni;
        k->capics = nc;
    }
    k->ics[k->nics].cell = cell;
    k->ics[k->nics].expected = expected;
    k->ics[k->nics].kind = (uint8_t)kind;
    return k->nics++;
}

// Recognize `head` as a fast op at this arity that is (a) not shadowed by a
// local/upvalue and (b) CURRENTLY bound to its canonical builtin. On success
// fills *kind/*cell/*expected (but does NOT add an IC -- so it is safe to call
// as a pure predicate). Otherwise returns false and the caller emits a general
// call, which is also the correct lowering when the op has been redefined.
static bool reg_fastop(compiler *C, fnstate *fn, lisp_value head, int argc,
                       fastkind *kind, lisp_value *cell_out, lisp_value *exp_out) {
    if (!lisp_is_symbol(head))
        return false;
    int rk;
    (void)resolve(C, fn, head, &rk, NULL);
    if (rk != R_GLOBAL)
        return false;  // shadowed -> not the builtin
    fastkind fk;
    bool found = false;
    if (argc == 2) {
        if (sym_is(head, "+")) { fk = FK_ADD; found = true; }
        else if (sym_is(head, "-")) { fk = FK_SUB; found = true; }
        else if (sym_is(head, "*")) { fk = FK_MUL; found = true; }
        else if (sym_is(head, "<")) { fk = FK_LT; found = true; }
        else if (sym_is(head, "<=")) { fk = FK_LE; found = true; }
        else if (sym_is(head, ">")) { fk = FK_GT; found = true; }
        else if (sym_is(head, ">=")) { fk = FK_GE; found = true; }
        else if (sym_is(head, "=")) { fk = FK_NUMEQ; found = true; }
        else if (sym_is(head, "eq?")) { fk = FK_EQ; found = true; }
        else if (sym_is(head, "cons")) { fk = FK_CONS; found = true; }
    } else if (argc == 1) {
        if (sym_is(head, "car")) { fk = FK_CAR; found = true; }
        else if (sym_is(head, "cdr")) { fk = FK_CDR; found = true; }
        else if (sym_is(head, "null?")) { fk = FK_NULLP; found = true; }
        else if (sym_is(head, "pair?")) { fk = FK_PAIRP; found = true; }
        else if (sym_is(head, "not")) { fk = FK_NOT; found = true; }
    }
    if (!found)
        return false;
    ensure_canon(C->genv);
    lisp_value cell = find_global_cell(C->genv, head);
    if (cell == LISP_EMPTY)
        return false;  // unbound at compile -> general path (will error like oracle)
    lisp_value cur = lisp_cdr(cell);
    if (cur != g_canon[fk] || g_canon[fk] == LISP_UNDEF)
        return false;  // already redefined / non-canonical -> call it normally
    *kind = fk;
    *cell_out = cell;
    *exp_out = g_canon[fk];
    return true;
}

// --- list helpers -----------------------------------------------------------

static int list_len(lisp_value l) {
    int n = 0;
    while (lisp_is_pair(l)) {
        n++;
        l = lisp_cdr(l);
    }
    return n;
}

// --- capture analysis (which bindings must be boxed) ------------------------
//
// A frame's binding must be a heap cell iff it is referenced from inside a
// lexically-nested lambda (capture by reference). We compute this with a free-
// variable walk that is deliberately a SAFE OVER-APPROXIMATION: it may report a
// few extra captures (boxing a binding that didn't need it -- a tiny perf cost)
// but never misses a real one (which would be a correctness bug). The only
// invariant that matters is: never remove a name from `bound` unless that name
// is genuinely bound at the use site.

#define SS_CAP 128
typedef struct {
    lisp_value items[SS_CAP];
    int n;
} symset;

static bool ss_has(const symset *s, lisp_value v) {
    for (int i = 0; i < s->n; i++)
        if (s->items[i] == v)
            return true;
    return false;
}

static void ss_add(compiler *C, symset *s, lisp_value v) {
    if (ss_has(s, v))
        return;
    if (s->n >= SS_CAP)
        lbc_decline(C, "capture analysis: too many names");
    s->items[s->n++] = v;
}

// Add a parameter list's names (incl. a dotted/rest tail) to a set.
static void ss_add_params(compiler *C, symset *s, lisp_value params) {
    lisp_value p = params;
    while (lisp_is_pair(p)) {
        if (lisp_is_symbol(lisp_car(p)))
            ss_add(C, s, lisp_car(p));
        p = lisp_cdr(p);
    }
    if (lisp_is_symbol(p))
        ss_add(C, s, p);
}

static void fv(compiler *C, lisp_value e, const symset *bound, symset *out);

static void fv_seq(compiler *C, lisp_value body, const symset *bound, symset *out) {
    for (lisp_value b = body; lisp_is_pair(b); b = lisp_cdr(b))
        fv(C, lisp_car(b), bound, out);
}

// Free variables of `e` not in `bound`, accumulated into `out`.
static void fv(compiler *C, lisp_value e, const symset *bound, symset *out) {
    if (lisp_is_symbol(e)) {
        if (!ss_has(bound, e))
            ss_add(C, out, e);
        return;
    }
    if (!lisp_is_pair(e))
        return;
    lisp_value h = lisp_car(e), rest = lisp_cdr(e);
    if (lisp_is_symbol(h)) {
        if (sym_is(h, "quote"))
            return;
        if (sym_is(h, "lambda") && lisp_is_pair(rest)) {
            symset nb = *bound;
            ss_add_params(C, &nb, lisp_car(rest));
            fv_seq(C, lisp_cdr(rest), &nb, out);
            return;
        }
        if (sym_is(h, "let") && lisp_is_pair(rest) && lisp_is_symbol(lisp_car(rest))) {
            // named let: inits in the outer scope; body sees loopname + params.
            lisp_value binds = lisp_car(lisp_cdr(rest));
            for (lisp_value b = binds; lisp_is_pair(b); b = lisp_cdr(b))
                if (lisp_is_pair(lisp_cdr(lisp_car(b))))
                    fv(C, lisp_car(lisp_cdr(lisp_car(b))), bound, out);
            symset nb = *bound;
            ss_add(C, &nb, lisp_car(rest));
            for (lisp_value b = binds; lisp_is_pair(b); b = lisp_cdr(b))
                ss_add(C, &nb, lisp_car(lisp_car(b)));
            fv_seq(C, lisp_cdr(lisp_cdr(rest)), &nb, out);
            return;
        }
        if ((sym_is(h, "let") || sym_is(h, "let*") || sym_is(h, "letrec")) &&
            lisp_is_pair(rest)) {
            // inits scanned in the outer `bound` (over-approx for let*/letrec is
            // safe); body sees the binding names.
            lisp_value binds = lisp_car(rest);
            for (lisp_value b = binds; lisp_is_pair(b); b = lisp_cdr(b))
                if (lisp_is_pair(lisp_car(b)) && lisp_is_pair(lisp_cdr(lisp_car(b))))
                    fv(C, lisp_car(lisp_cdr(lisp_car(b))), bound, out);
            symset nb = *bound;
            for (lisp_value b = binds; lisp_is_pair(b); b = lisp_cdr(b))
                if (lisp_is_pair(lisp_car(b)))
                    ss_add(C, &nb, lisp_car(lisp_car(b)));
            fv_seq(C, lisp_cdr(rest), &nb, out);
            return;
        }
        if (sym_is(h, "define") && lisp_is_pair(rest)) {
            lisp_value tgt = lisp_car(rest);
            if (lisp_is_pair(tgt)) {  // (define (f . a) body...)
                symset nb = *bound;
                ss_add_params(C, &nb, lisp_cdr(tgt));
                fv_seq(C, lisp_cdr(rest), &nb, out);
            } else {  // (define x val)
                fv_seq(C, lisp_cdr(rest), bound, out);
            }
            return;
        }
        // if/begin/and/or/cond/when/unless/while/case/set!/quasiquote/apply:
        // scan every subform (over-approx-safe).
    }
    for (lisp_value p = e; lisp_is_pair(p); p = lisp_cdr(p))
        fv(C, lisp_car(p), bound, out);
}

// Find variables in `frame` that are captured by a lambda nested in `e`.
static void collect_captures(compiler *C, lisp_value e, const symset *frame,
                             symset *out) {
    if (!lisp_is_pair(e))
        return;
    lisp_value h = lisp_car(e), rest = lisp_cdr(e);
    if (lisp_is_symbol(h)) {
        if (sym_is(h, "quote"))
            return;
        if (sym_is(h, "lambda") && lisp_is_pair(rest)) {
            symset params = {.n = 0}, lf = {.n = 0};
            ss_add_params(C, &params, lisp_car(rest));
            fv_seq(C, lisp_cdr(rest), &params, &lf);  // covers deeper lambdas too
            for (int i = 0; i < lf.n; i++)
                if (ss_has(frame, lf.items[i]))
                    ss_add(C, out, lf.items[i]);
            return;
        }
        if (sym_is(h, "let") && lisp_is_pair(rest) && lisp_is_symbol(lisp_car(rest))) {
            // named-let body is a lambda over loopname + params.
            lisp_value binds = lisp_car(lisp_cdr(rest));
            symset params = {.n = 0}, lf = {.n = 0};
            ss_add(C, &params, lisp_car(rest));
            for (lisp_value b = binds; lisp_is_pair(b); b = lisp_cdr(b))
                if (lisp_is_pair(lisp_car(b)))
                    ss_add(C, &params, lisp_car(lisp_car(b)));
            fv_seq(C, lisp_cdr(lisp_cdr(rest)), &params, &lf);
            for (int i = 0; i < lf.n; i++)
                if (ss_has(frame, lf.items[i]))
                    ss_add(C, out, lf.items[i]);
            for (lisp_value b = binds; lisp_is_pair(b); b = lisp_cdr(b))  // inits
                if (lisp_is_pair(lisp_car(b)) && lisp_is_pair(lisp_cdr(lisp_car(b))))
                    collect_captures(C, lisp_car(lisp_cdr(lisp_car(b))), frame, out);
            return;
        }
    }
    for (lisp_value p = e; lisp_is_pair(p); p = lisp_cdr(p))
        collect_captures(C, lisp_car(p), frame, out);
}

// Captures of a whole body sequence (list of forms) for a frame.
static void captures_of_body(compiler *C, lisp_value body, const symset *frame,
                             symset *out) {
    for (lisp_value b = body; lisp_is_pair(b); b = lisp_cdr(b))
        collect_captures(C, lisp_car(b), frame, out);
}

// --- forward decls ----------------------------------------------------------

static void compile_value(compiler *C, fnstate *fn, lisp_value e);
static void compile_tail(compiler *C, fnstate *fn, lisp_value e);
static bcchunk *compile_lambda(compiler *C, fnstate *parent, lisp_value params,
                               lisp_value body, lisp_value self_name);

// Compile a body (list of forms): all but the last for effect, the last in the
// given position (tail or value). Internal defines at the head are supported as
// local bindings (no forward/mutual local recursion -- that declines).
static void compile_body(compiler *C, fnstate *fn, lisp_value body, bool tail) {
    if (!lisp_is_pair(body)) {
        // empty body -> unspecified
        emit(C, fn->chunk, OP_CONST, add_const(C, fn->chunk, LISP_UNDEF), 0);
        if (tail)
            emit(C, fn->chunk, OP_RET, 0, 0);
        return;
    }
    // Leading internal defines. The legacy stack form captures upvalues by value
    // and binds defines sequentially, so it cannot express mutual/forward local
    // recursion; decline a group of 2+ (the register form handles them as a
    // letrec* group) rather than mis-compiling a forward reference as a global.
    {
        int ndef = 0;
        for (lisp_value b = body; lisp_is_pair(b); b = lisp_cdr(b)) {
            if (!(lisp_is_pair(lisp_car(b)) && sym_is(lisp_car(lisp_car(b)), "define")))
                break;
            ndef++;
        }
        if (ndef > 1)
            lbc_decline(C, "stack form: mutual internal defines unsupported");
    }
    while (lisp_is_pair(body)) {
        lisp_value form = lisp_car(body);
        if (!(lisp_is_pair(form) && sym_is(lisp_car(form), "define")))
            break;
        lisp_value rest = lisp_cdr(form);
        if (!lisp_is_pair(rest))
            lbc_decline(C, "malformed define");
        lisp_value target = lisp_car(rest);
        if (lisp_is_symbol(target)) {
            if (!lisp_is_pair(lisp_cdr(rest)))
                lbc_decline(C, "malformed define");
            int slot = alloc_slot(C, fn);
            compile_value(C, fn, lisp_car(lisp_cdr(rest)));
            emit(C, fn->chunk, OP_STORELOCAL, slot, 0);
            bind_local(C, fn, target, slot);  // in scope AFTER its init
        } else if (lisp_is_pair(target)) {  // (define (f . args) body...)
            lisp_value name = lisp_car(target);
            if (!lisp_is_symbol(name))
                lbc_decline(C, "define: name must be a symbol");
            int slot = alloc_slot(C, fn);
            bind_local(C, fn, name, slot);  // visible for self-recursion
            bcchunk *child =
                compile_lambda(C, fn, lisp_cdr(target), lisp_cdr(rest), name);
            int ci = fn->chunk->nchildren;
            if (fn->chunk->nchildren == fn->chunk->capchildren) {
                int nc = fn->chunk->capchildren ? fn->chunk->capchildren * 2 : 4;
                bcchunk **nch = (bcchunk **)realloc(fn->chunk->children,
                                                    (size_t)nc * sizeof(bcchunk *));
                if (nch == NULL)
                    lbc_decline(C, "out of memory");
                fn->chunk->children = nch;
                fn->chunk->capchildren = nc;
            }
            fn->chunk->children[ci] = child;
            fn->chunk->nchildren++;
            emit(C, fn->chunk, OP_CLOSURE, ci, 0);
            emit(C, fn->chunk, OP_STORELOCAL, slot, 0);
        } else {
            lbc_decline(C, "malformed define");
        }
        body = lisp_cdr(body);
    }
    if (!lisp_is_pair(body)) {  // body was only defines
        emit(C, fn->chunk, OP_CONST, add_const(C, fn->chunk, LISP_UNDEF), 0);
        if (tail)
            emit(C, fn->chunk, OP_RET, 0, 0);
        return;
    }
    while (lisp_is_pair(lisp_cdr(body))) {
        compile_value(C, fn, lisp_car(body));
        emit(C, fn->chunk, OP_POP, 0, 0);
        body = lisp_cdr(body);
    }
    if (tail)
        compile_tail(C, fn, lisp_car(body));
    else
        compile_value(C, fn, lisp_car(body));
}

// Add a child chunk to fn and emit OP_CLOSURE for it.
static void emit_closure(compiler *C, fnstate *fn, bcchunk *child) {
    int ci = fn->chunk->nchildren;
    if (fn->chunk->nchildren == fn->chunk->capchildren) {
        int nc = fn->chunk->capchildren ? fn->chunk->capchildren * 2 : 4;
        bcchunk **nch = (bcchunk **)realloc(fn->chunk->children,
                                            (size_t)nc * sizeof(bcchunk *));
        if (nch == NULL)
            lbc_decline(C, "out of memory");
        fn->chunk->children = nch;
        fn->chunk->capchildren = nc;
    }
    fn->chunk->children[ci] = child;
    fn->chunk->nchildren++;
    emit(C, fn->chunk, OP_CLOSURE, ci, 0);
}

static bcchunk *compile_lambda(compiler *C, fnstate *parent, lisp_value params,
                               lisp_value body, lisp_value self_name) {
    fnstate fn;
    memset(&fn, 0, sizeof(fn));
    fn.parent = parent;
    fn.chunk = chunk_new(C);
    fn.self_name = self_name == 0 ? LISP_UNDEF : self_name;
    if (fn.self_name == 0)
        fn.self_name = LISP_UNDEF;
    // Parameters: each gets slot 0..n-1.
    int np = 0;
    lisp_value p = params;
    while (lisp_is_pair(p)) {
        lisp_value name = lisp_car(p);
        if (!lisp_is_symbol(name))
            lbc_decline(C, "bad parameter");
        int slot = alloc_slot(C, &fn);
        bind_local(C, &fn, name, slot);
        np++;
        p = lisp_cdr(p);
    }
    if (lisp_is_symbol(p)) {  // (lambda (a b . rest) ...) or (lambda args ...)
        int slot = alloc_slot(C, &fn);
        bind_local(C, &fn, p, slot);
        fn.chunk->has_rest = true;
    } else if (!lisp_is_empty(p)) {
        lbc_decline(C, "malformed parameter list");
    }
    fn.chunk->nparams = np;
    compile_body(C, &fn, body, true);
    return fn.chunk;
}

// Compile an application (general path): operator then args, then CALL/TAILCALL.
static void compile_call(compiler *C, fnstate *fn, lisp_value e, bool tail) {
    lisp_value head = lisp_car(e);
    lisp_value args = lisp_cdr(e);
    int argc = list_len(args);
    lisp_value tailp = args;  // reject an improper argument list
    while (lisp_is_pair(tailp))
        tailp = lisp_cdr(tailp);
    if (!lisp_is_empty(tailp))
        lbc_decline(C, "improper argument list");
    bcop fop;
    int variant;
    if (frozen_op(C, fn, head, argc, &fop, &variant)) {
        for (lisp_value a = args; lisp_is_pair(a); a = lisp_cdr(a))
            compile_value(C, fn, lisp_car(a));
        if (fop == OP_ARITH2 || fop == OP_CMP2) {
            const char *nm = NULL;
            switch (variant + (fop == OP_CMP2 ? 100 : 0)) {
                case 0: nm = "+"; break;
                case 1: nm = "-"; break;
                case 2: nm = "*"; break;
                case 100: nm = "<"; break;
                case 101: nm = "<="; break;
                case 102: nm = ">"; break;
                case 103: nm = ">="; break;
                case 104: nm = "="; break;
            }
            int pc = frozen_prim_const(C, fn->chunk, nm);
            emit(C, fn->chunk, fop, variant, pc);
        } else {
            emit(C, fn->chunk, fop, variant, 0);
        }
        if (tail)
            emit(C, fn->chunk, OP_RET, 0, 0);
        return;
    }
    compile_value(C, fn, head);  // operator
    for (lisp_value a = args; lisp_is_pair(a); a = lisp_cdr(a))
        compile_value(C, fn, lisp_car(a));
    emit(C, fn->chunk, tail ? OP_TAILCALL : OP_CALL, argc, 0);
}

// Compile `e` to leave exactly one value on the stack (non-tail position).
static void compile_value(compiler *C, fnstate *fn, lisp_value e) {
    bcchunk *k = fn->chunk;
    // self-evaluating atoms
    if (lisp_is_fixnum(e) || lisp_is_flonum(e) || lisp_is_string(e) ||
        lisp_is_char(e) || lisp_is_keyword(e) || e == LISP_TRUE ||
        e == LISP_FALSE || e == LISP_UNDEF) {
        emit(C, k, OP_CONST, add_const(C, k, e), 0);
        return;
    }
    if (lisp_is_symbol(e)) {
        int kind, idx = resolve(C, fn, e, &kind, NULL);
        switch (kind) {
            case R_LOCAL: emit(C, k, OP_LOADLOCAL, idx, 0); return;
            case R_UPVAL: emit(C, k, OP_LOADUPVAL, idx, 0); return;
            case R_SELF: emit(C, k, OP_LOADSELF, 0, 0); return;
            case R_GLOBAL: {
                if (g_global_slots) {
                    lisp_value cell = find_global_cell(C->genv, e);
                    if (cell != LISP_EMPTY) {
                        emit(C, k, OP_LOADGLOBAL_SLOT, add_const(C, k, cell), 0);
                        return;
                    }
                }
                emit(C, k, OP_LOADGLOBAL, add_const(C, k, e), 0);
                return;
            }
        }
        return;
    }
    if (!lisp_is_pair(e)) {
        if (lisp_is_empty(e))
            lbc_decline(C, "cannot evaluate ()");
        emit(C, k, OP_CONST, add_const(C, k, e), 0);
        return;
    }
    lisp_value head = lisp_car(e), rest = lisp_cdr(e);
    if (lisp_is_symbol(head)) {
        if (sym_is(head, "quote")) {
            emit(C, k, OP_CONST, add_const(C, k, lisp_car(rest)), 0);
            return;
        }
        if (sym_is(head, "if")) {
            compile_value(C, fn, lisp_car(rest));
            int jf = emit(C, k, OP_JMPF, -1, 0);
            compile_value(C, fn, lisp_car(lisp_cdr(rest)));
            int je = emit(C, k, OP_JMP, -1, 0);
            k->code[jf].a = k->ncode;
            lisp_value elsep = lisp_cdr(lisp_cdr(rest));
            if (lisp_is_pair(elsep))
                compile_value(C, fn, lisp_car(elsep));
            else
                emit(C, k, OP_CONST, add_const(C, k, LISP_UNDEF), 0);
            k->code[je].a = k->ncode;
            return;
        }
        if (sym_is(head, "begin")) {
            if (!lisp_is_pair(rest)) {
                emit(C, k, OP_CONST, add_const(C, k, LISP_UNDEF), 0);
                return;
            }
            compile_body(C, fn, rest, false);
            return;
        }
        if (sym_is(head, "lambda")) {
            bcchunk *child = compile_lambda(C, fn, lisp_car(rest), lisp_cdr(rest), 0);
            emit_closure(C, fn, child);
            return;
        }
        if (sym_is(head, "and")) {
            if (!lisp_is_pair(rest)) {
                emit(C, k, OP_CONST, add_const(C, k, LISP_TRUE), 0);
                return;
            }
            int jmps[64];
            int n = 0;
            lisp_value a = rest;
            while (lisp_is_pair(lisp_cdr(a))) {
                compile_value(C, fn, lisp_car(a));
                if (n >= 64)
                    lbc_decline(C, "and too long");
                // JMPF_KEEP pops the value on fall-through (truthy) itself.
                jmps[n++] = emit(C, k, OP_JMPF_KEEP, -1, 0);
                a = lisp_cdr(a);
            }
            compile_value(C, fn, lisp_car(a));  // last value is the result
            for (int i = 0; i < n; i++)
                k->code[jmps[i]].a = k->ncode;
            return;
        }
        if (sym_is(head, "or")) {
            if (!lisp_is_pair(rest)) {
                emit(C, k, OP_CONST, add_const(C, k, LISP_FALSE), 0);
                return;
            }
            int jmps[64];
            int n = 0;
            lisp_value a = rest;
            while (lisp_is_pair(lisp_cdr(a))) {
                compile_value(C, fn, lisp_car(a));
                if (n >= 64)
                    lbc_decline(C, "or too long");
                // JMPT_KEEP pops the value on fall-through (falsey) itself.
                jmps[n++] = emit(C, k, OP_JMPT_KEEP, -1, 0);
                a = lisp_cdr(a);
            }
            compile_value(C, fn, lisp_car(a));
            for (int i = 0; i < n; i++)
                k->code[jmps[i]].a = k->ncode;
            return;
        }
        if (sym_is(head, "when") || sym_is(head, "unless")) {
            bool want = sym_is(head, "when");
            compile_value(C, fn, lisp_car(rest));
            if (!want)
                emit(C, k, OP_NOT, 0, 0);
            int jf = emit(C, k, OP_JMPF, -1, 0);
            compile_body(C, fn, lisp_cdr(rest), false);
            int je = emit(C, k, OP_JMP, -1, 0);
            k->code[jf].a = k->ncode;
            emit(C, k, OP_CONST, add_const(C, k, LISP_UNDEF), 0);
            k->code[je].a = k->ncode;
            return;
        }
        if (sym_is(head, "cond")) {
            // (cond (test body...) ... (else body...))
            int endjmps[64];
            int ne = 0;
            lisp_value cl = rest;
            bool had_else = false;
            while (lisp_is_pair(cl)) {
                lisp_value clause = lisp_car(cl);
                if (!lisp_is_pair(clause))
                    lbc_decline(C, "malformed cond clause");
                lisp_value test = lisp_car(clause);
                lisp_value cbody = lisp_cdr(clause);
                if (sym_is(test, "else")) {
                    compile_body(C, fn, cbody, false);
                    had_else = true;
                    break;
                }
                if (!lisp_is_pair(cbody))
                    lbc_decline(C, "cond clause without body unsupported");
                compile_value(C, fn, test);
                int jf = emit(C, k, OP_JMPF, -1, 0);
                compile_body(C, fn, cbody, false);
                if (ne >= 64)
                    lbc_decline(C, "cond too long");
                endjmps[ne++] = emit(C, k, OP_JMP, -1, 0);
                k->code[jf].a = k->ncode;
                cl = lisp_cdr(cl);
            }
            if (!had_else)
                emit(C, k, OP_CONST, add_const(C, k, LISP_UNDEF), 0);
            for (int i = 0; i < ne; i++)
                k->code[endjmps[i]].a = k->ncode;
            return;
        }
        if (sym_is(head, "let")) {
            if (lisp_is_pair(rest) && lisp_is_symbol(lisp_car(rest))) {
                // named let -> a self-recursive lambda applied to the inits.
                lisp_value name = lisp_car(rest);
                lisp_value binds = lisp_car(lisp_cdr(rest));
                lisp_value lbody = lisp_cdr(lisp_cdr(rest));
                lisp_value params = LISP_EMPTY, ptail = LISP_EMPTY;
                int saved = fn->nlocals;
                // compile inits in the OUTER scope, pushing them as call args
                // AFTER we emit the closure. Build param list + collect inits.
                // Emit: closure; inits...; CALL.
                lisp_value pl = LISP_EMPTY, pt = LISP_EMPTY;
                for (lisp_value b = binds; lisp_is_pair(b); b = lisp_cdr(b)) {
                    lisp_value bd = lisp_car(b);
                    lisp_value pc = lisp_cons(lisp_car(bd), LISP_EMPTY);
                    if (pl == LISP_EMPTY) pl = pc; else set_cdr_lbc(pt, pc);
                    pt = pc;
                }
                (void)params; (void)ptail;
                bcchunk *child = compile_lambda(C, fn, pl, lbody, name);
                emit_closure(C, fn, child);
                for (lisp_value b = binds; lisp_is_pair(b); b = lisp_cdr(b))
                    compile_value(C, fn, lisp_car(lisp_cdr(lisp_car(b))));
                emit(C, k, OP_CALL, list_len(binds), 0);
                fn->nlocals = saved;
                return;
            }
            lisp_value binds = lisp_car(rest);
            lisp_value lbody = lisp_cdr(rest);
            int saved = fn->nlocals;
            // let: all inits evaluate in the OUTER scope, then bind.
            int slots[LBC_MAX_LOCALS];
            int nb = 0;
            for (lisp_value b = binds; lisp_is_pair(b); b = lisp_cdr(b)) {
                lisp_value bd = lisp_car(b);
                compile_value(C, fn, lisp_car(lisp_cdr(bd)));
                slots[nb++] = alloc_slot(C, fn);
            }
            for (int i = nb - 1; i >= 0; i--)  // stack top is the last init
                emit(C, k, OP_STORELOCAL, slots[i], 0);
            int bi = 0;
            for (lisp_value b = binds; lisp_is_pair(b); b = lisp_cdr(b), bi++)
                bind_local(C, fn, lisp_car(lisp_car(b)), slots[bi]);
            compile_body(C, fn, lbody, false);
            fn->nlocals = saved;
            return;
        }
        if (sym_is(head, "let*")) {
            lisp_value binds = lisp_car(rest);
            lisp_value lbody = lisp_cdr(rest);
            int saved = fn->nlocals;
            for (lisp_value b = binds; lisp_is_pair(b); b = lisp_cdr(b)) {
                lisp_value bd = lisp_car(b);
                compile_value(C, fn, lisp_car(lisp_cdr(bd)));
                int slot = alloc_slot(C, fn);
                emit(C, k, OP_STORELOCAL, slot, 0);
                bind_local(C, fn, lisp_car(bd), slot);  // visible to later inits
            }
            compile_body(C, fn, lbody, false);
            fn->nlocals = saved;
            return;
        }
        if (sym_is(head, "set!")) {
            lisp_value target = lisp_car(rest);
            int kind, idx = resolve(C, fn, target, &kind, NULL);
            if (kind == R_UPVAL || kind == R_SELF)
                lbc_decline(C, "set! on captured/self binding unsupported");
            compile_value(C, fn, lisp_car(lisp_cdr(rest)));
            if (kind == R_LOCAL) {
                emit(C, k, OP_STORELOCAL, idx, 0);
                emit(C, k, OP_CONST, add_const(C, k, LISP_UNDEF), 0);
            } else {
                emit(C, k, OP_SETGLOBAL, add_const(C, k, target), 0);
            }
            return;
        }
        if (sym_is(head, "define")) {
            lbc_decline(C, "define only supported at body head");
        }
        if (sym_is(head, "letrec") || sym_is(head, "case") || sym_is(head, "while") ||
            sym_is(head, "quasiquote") || sym_is(head, "define-module") ||
            sym_is(head, "import") || sym_is(head, "include")) {
            lbc_decline(C, "form not supported by prototype");
        }
    }
    compile_call(C, fn, e, false);
}

// Compile `e` in TAIL position (terminates the frame with TAILCALL or RET).
static void compile_tail(compiler *C, fnstate *fn, lisp_value e) {
    bcchunk *k = fn->chunk;
    if (lisp_is_pair(e) && lisp_is_symbol(lisp_car(e))) {
        lisp_value head = lisp_car(e), rest = lisp_cdr(e);
        if (sym_is(head, "if")) {
            compile_value(C, fn, lisp_car(rest));
            int jf = emit(C, k, OP_JMPF, -1, 0);
            compile_tail(C, fn, lisp_car(lisp_cdr(rest)));
            k->code[jf].a = k->ncode;
            lisp_value elsep = lisp_cdr(lisp_cdr(rest));
            if (lisp_is_pair(elsep)) {
                compile_tail(C, fn, lisp_car(elsep));
            } else {
                emit(C, k, OP_CONST, add_const(C, k, LISP_UNDEF), 0);
                emit(C, k, OP_RET, 0, 0);
            }
            return;
        }
        if (sym_is(head, "begin")) {
            if (lisp_is_pair(rest)) {
                compile_body(C, fn, rest, true);
                return;
            }
        }
        if (sym_is(head, "when") || sym_is(head, "unless")) {
            bool want = sym_is(head, "when");
            compile_value(C, fn, lisp_car(rest));
            if (!want)
                emit(C, k, OP_NOT, 0, 0);
            int jf = emit(C, k, OP_JMPF, -1, 0);
            compile_body(C, fn, lisp_cdr(rest), true);
            k->code[jf].a = k->ncode;
            emit(C, k, OP_CONST, add_const(C, k, LISP_UNDEF), 0);
            emit(C, k, OP_RET, 0, 0);
            return;
        }
        if (sym_is(head, "cond")) {
            lisp_value cl = rest;
            bool had_else = false;
            while (lisp_is_pair(cl)) {
                lisp_value clause = lisp_car(cl);
                if (!lisp_is_pair(clause))
                    lbc_decline(C, "malformed cond clause");
                lisp_value test = lisp_car(clause);
                lisp_value cbody = lisp_cdr(clause);
                if (sym_is(test, "else")) {
                    compile_body(C, fn, cbody, true);
                    had_else = true;
                    break;
                }
                if (!lisp_is_pair(cbody))
                    lbc_decline(C, "cond clause without body unsupported");
                compile_value(C, fn, test);
                int jf = emit(C, k, OP_JMPF, -1, 0);
                compile_body(C, fn, cbody, true);
                k->code[jf].a = k->ncode;
                cl = lisp_cdr(cl);
            }
            if (!had_else) {
                emit(C, k, OP_CONST, add_const(C, k, LISP_UNDEF), 0);
                emit(C, k, OP_RET, 0, 0);
            }
            return;
        }
        if (sym_is(head, "let") && !(lisp_is_pair(rest) && lisp_is_symbol(lisp_car(rest)))) {
            lisp_value binds = lisp_car(rest);
            lisp_value lbody = lisp_cdr(rest);
            int saved = fn->nlocals;
            int slots[LBC_MAX_LOCALS];
            int nb = 0;
            for (lisp_value b = binds; lisp_is_pair(b); b = lisp_cdr(b)) {
                lisp_value bd = lisp_car(b);
                compile_value(C, fn, lisp_car(lisp_cdr(bd)));
                slots[nb++] = alloc_slot(C, fn);
            }
            for (int i = nb - 1; i >= 0; i--)
                emit(C, k, OP_STORELOCAL, slots[i], 0);
            int i = 0;
            for (lisp_value b = binds; lisp_is_pair(b); b = lisp_cdr(b), i++)
                bind_local(C, fn, lisp_car(lisp_car(b)), slots[i]);
            compile_body(C, fn, lbody, true);
            fn->nlocals = saved;
            return;
        }
        if (sym_is(head, "let*")) {
            lisp_value binds = lisp_car(rest);
            lisp_value lbody = lisp_cdr(rest);
            int saved = fn->nlocals;
            for (lisp_value b = binds; lisp_is_pair(b); b = lisp_cdr(b)) {
                lisp_value bd = lisp_car(b);
                compile_value(C, fn, lisp_car(lisp_cdr(bd)));
                int slot = alloc_slot(C, fn);
                emit(C, k, OP_STORELOCAL, slot, 0);
                bind_local(C, fn, lisp_car(bd), slot);
            }
            compile_body(C, fn, lbody, true);
            fn->nlocals = saved;
            return;
        }
        // Other symbol-headed forms fall through to unified handling below.
    }
    if (lisp_is_pair(e)) {
        lisp_value head = lisp_car(e);
        bcop fop;
        int variant;
        bool is_app = !(lisp_is_symbol(head) && is_special_head(head));
        bool is_frozen =
            is_app && frozen_op(C, fn, head, list_len(lisp_cdr(e)), &fop, &variant);
        if (is_app && !is_frozen) {
            compile_call(C, fn, e, true);  // tail call (incl. compound operator)
            return;
        }
    }
    // default: produce a value then RET (frozen ops, special forms, atoms)
    compile_value(C, fn, e);
    emit(C, k, OP_RET, 0, 0);
}

// --- Top-level compile entry ------------------------------------------------

static bool lbc_compile(lisp_value genv, lisp_value expr, bcchunk **out,
                        const char **why) {
    compiler C;
    memset(&C, 0, sizeof(C));
    C.genv = genv;
    if (setjmp(C.decline)) {
        *why = C.why;
        return false;
    }
    fnstate fn;
    memset(&fn, 0, sizeof(fn));
    fn.parent = NULL;
    fn.chunk = chunk_new(&C);
    fn.self_name = LISP_UNDEF;
    lisp_value body = lisp_cons(expr, LISP_EMPTY);  // a one-form body, in tail pos
    compile_body(&C, &fn, body, true);
    *out = fn.chunk;
    return true;
}

// --- Threaded VM ------------------------------------------------------------

typedef struct {
    bcchunk *chunk;
    bcclosure *clo;
    lisp_value locals[LBC_MAX_LOCALS];
    lisp_value stk[LBC_MAX_STACK];
    int sp;
    int pc;
} vframe;

static bool setup_frame(vframe *f, bcclosure *clo, lisp_value *args, int argc,
                        const char **err) {
    bcchunk *k = clo->chunk;
    f->clo = clo;
    f->chunk = k;
    f->sp = 0;
    f->pc = 0;
    for (int i = 0; i < k->nlocals; i++)
        f->locals[i] = LISP_UNDEF;
    if (k->has_rest) {
        if (argc < k->nparams) {
            *err = "too few arguments";
            return false;
        }
        for (int i = 0; i < k->nparams; i++)
            f->locals[i] = args[i];
        lisp_value rest = LISP_EMPTY;
        for (int j = argc - 1; j >= k->nparams; j--) {
            lisp_value c = lisp_cons(args[j], rest);
            if (c == LISP_UNDEF) {
                *err = "out of memory";
                return false;
            }
            rest = c;
        }
        f->locals[k->nparams] = rest;
    } else {
        if (argc != k->nparams) {
            *err = "argument count mismatch";
            return false;
        }
        for (int i = 0; i < argc; i++)
            f->locals[i] = args[i];
    }
    return true;
}

static bool vm_run(bcclosure *top, lisp_value genv, lisp_value *out,
                   const char **errout) {
    vframe *frames = (vframe *)malloc(sizeof(vframe) * LBC_MAX_FRAMES);
    if (frames == NULL) {
        *errout = "out of memory";
        return false;
    }
    const char *err = NULL;
    int depth = 0;
    if (!setup_frame(&frames[0], top, NULL, 0, &err))
        goto fail;
    depth = 1;

    for (;;) {
        vframe *f = &frames[depth - 1];
        instr ins = f->chunk->code[f->pc++];
        switch (ins.op) {
            case OP_CONST: f->stk[f->sp++] = f->chunk->consts[ins.a]; break;
            case OP_LOADLOCAL: f->stk[f->sp++] = f->locals[ins.a]; break;
            case OP_LOADUPVAL: f->stk[f->sp++] = f->clo->upvals[ins.a]; break;
            case OP_LOADSELF: f->stk[f->sp++] = LBC_MK_CLO(f->clo); break;
            case OP_LOADGLOBAL: {
                lisp_value v;
                if (!lisp_env_lookup(genv, f->chunk->consts[ins.a], &v)) {
                    err = "unbound variable";
                    goto fail;
                }
                f->stk[f->sp++] = v;
                break;
            }
            case OP_LOADGLOBAL_SLOT:
                f->stk[f->sp++] = lisp_cdr(f->chunk->consts[ins.a]);
                break;
            case OP_STORELOCAL: f->locals[ins.a] = f->stk[--f->sp]; break;
            case OP_STOREGLOBAL:
                lisp_env_define(genv, f->chunk->consts[ins.a], f->stk[f->sp - 1]);
                break;
            case OP_SETGLOBAL: {
                lisp_value v = f->stk[--f->sp];
                if (!lisp_env_set(genv, f->chunk->consts[ins.a], v)) {
                    err = "set! on unbound variable";
                    goto fail;
                }
                f->stk[f->sp++] = LISP_UNDEF;
                break;
            }
            case OP_POP: f->sp--; break;
            case OP_CLOSURE: {
                bcchunk *child = f->chunk->children[ins.a];
                bcclosure *c = (bcclosure *)malloc(
                    sizeof(bcclosure) + sizeof(lisp_value) * (size_t)child->nupdesc);
                if (c == NULL) {
                    err = "out of memory";
                    goto fail;
                }
                c->chunk = child;
                for (int i = 0; i < child->nupdesc; i++)
                    c->upvals[i] = child->updesc[i].from_local
                                       ? f->locals[child->updesc[i].index]
                                       : f->clo->upvals[child->updesc[i].index];
                f->stk[f->sp++] = LBC_MK_CLO(c);
                break;
            }
            case OP_JMP: f->pc = ins.a; break;
            case OP_JMPF:
                if (!lisp_truthy(f->stk[--f->sp]))
                    f->pc = ins.a;
                break;
            case OP_JMPF_KEEP:
                if (!lisp_truthy(f->stk[f->sp - 1]))
                    f->pc = ins.a;
                else
                    f->sp--;
                break;
            case OP_JMPT_KEEP:
                if (lisp_truthy(f->stk[f->sp - 1]))
                    f->pc = ins.a;
                else
                    f->sp--;
                break;
            case OP_ARITH2: {
                lisp_value b = f->stk[--f->sp], a = f->stk[--f->sp];
                if (lisp_is_fixnum(a) && lisp_is_fixnum(b)) {
                    int64_t x = lisp_fixnum_val(a), y = lisp_fixnum_val(b), r;
                    switch (ins.a) {
                        case 0: r = x + y; break;
                        case 1: r = x - y; break;
                        default: r = x * y; break;
                    }
                    f->stk[f->sp++] = lisp_fixnum(r);
                } else {
                    lisp_value as[2] = {a, b};
                    lisp_value r = lbc_call_proc(f->chunk->consts[ins.b], as, 2, &err);
                    if (r == LISP_UNDEF && err != NULL)
                        goto fail;
                    f->stk[f->sp++] = r;
                }
                break;
            }
            case OP_CMP2: {
                lisp_value b = f->stk[--f->sp], a = f->stk[--f->sp];
                if (lisp_is_fixnum(a) && lisp_is_fixnum(b)) {
                    int64_t x = lisp_fixnum_val(a), y = lisp_fixnum_val(b);
                    bool ok;
                    switch (ins.a) {
                        case 0: ok = x < y; break;
                        case 1: ok = x <= y; break;
                        case 2: ok = x > y; break;
                        case 3: ok = x >= y; break;
                        default: ok = x == y; break;
                    }
                    f->stk[f->sp++] = ok ? LISP_TRUE : LISP_FALSE;
                } else {
                    lisp_value as[2] = {a, b};
                    lisp_value r = lbc_call_proc(f->chunk->consts[ins.b], as, 2, &err);
                    if (r == LISP_UNDEF && err != NULL)
                        goto fail;
                    f->stk[f->sp++] = r;
                }
                break;
            }
            case OP_EQ: {
                lisp_value b = f->stk[--f->sp], a = f->stk[--f->sp];
                f->stk[f->sp++] = (a == b) ? LISP_TRUE : LISP_FALSE;
                break;
            }
            case OP_CONS: {
                lisp_value b = f->stk[--f->sp], a = f->stk[--f->sp];
                lisp_value c = lisp_cons(a, b);
                if (c == LISP_UNDEF) {
                    err = "out of memory";
                    goto fail;
                }
                f->stk[f->sp++] = c;
                break;
            }
            case OP_CAR: {
                lisp_value a = f->stk[--f->sp];
                if (!lisp_is_pair(a)) {
                    err = "car: not a pair";
                    goto fail;
                }
                f->stk[f->sp++] = lisp_car(a);
                break;
            }
            case OP_CDR: {
                lisp_value a = f->stk[--f->sp];
                if (!lisp_is_pair(a)) {
                    err = "cdr: not a pair";
                    goto fail;
                }
                f->stk[f->sp++] = lisp_cdr(a);
                break;
            }
            case OP_NULLP: {
                lisp_value a = f->stk[--f->sp];
                f->stk[f->sp++] = lisp_is_empty(a) ? LISP_TRUE : LISP_FALSE;
                break;
            }
            case OP_PAIRP: {
                lisp_value a = f->stk[--f->sp];
                f->stk[f->sp++] = lisp_is_pair(a) ? LISP_TRUE : LISP_FALSE;
                break;
            }
            case OP_NOT: {
                lisp_value a = f->stk[--f->sp];
                f->stk[f->sp++] = lisp_truthy(a) ? LISP_FALSE : LISP_TRUE;
                break;
            }
            case OP_CALL: {
                int n = ins.a;
                lisp_value callee = f->stk[f->sp - n - 1];
                lisp_value *args = &f->stk[f->sp - n];
                if (LBC_IS_CLO(callee)) {
                    if (depth >= LBC_MAX_FRAMES) {
                        err = "call depth exceeded";
                        goto fail;
                    }
                    if (!setup_frame(&frames[depth], LBC_CLO(callee), args, n, &err))
                        goto fail;
                    f->sp -= n + 1;
                    depth++;
                } else {
                    lisp_value r = lbc_call_proc(callee, args, n, &err);
                    if (r == LISP_UNDEF && err != NULL)
                        goto fail;
                    f->sp -= n + 1;
                    f->stk[f->sp++] = r;
                }
                break;
            }
            case OP_TAILCALL: {
                int n = ins.a;
                lisp_value callee = f->stk[f->sp - n - 1];
                lisp_value *args = &f->stk[f->sp - n];
                if (LBC_IS_CLO(callee)) {
                    vframe tmp;
                    if (!setup_frame(&tmp, LBC_CLO(callee), args, n, &err))
                        goto fail;
                    *f = tmp;  // reuse this frame slot -> O(1) loop depth
                } else {
                    lisp_value r = lbc_call_proc(callee, args, n, &err);
                    if (r == LISP_UNDEF && err != NULL)
                        goto fail;
                    depth--;
                    if (depth == 0) {
                        *out = r;
                        free(frames);
                        return true;
                    }
                    frames[depth - 1].stk[frames[depth - 1].sp++] = r;
                }
                break;
            }
            case OP_RET: {
                lisp_value r = f->stk[--f->sp];
                depth--;
                if (depth == 0) {
                    *out = r;
                    free(frames);
                    return true;
                }
                frames[depth - 1].stk[frames[depth - 1].sp++] = r;
                break;
            }
            default:
                err = "bad opcode";
                goto fail;
        }
    }
fail:
    free(frames);
    *errout = err != NULL ? err : "error";
    return false;
}

// --- Public-ish prototype API -----------------------------------------------

typedef enum { LBC_OK, LBC_DECLINED, LBC_ERR } lbc_status;

static lbc_status lbc_eval(lisp_value genv, lisp_value expr, lisp_value *out,
                           const char **msg) {
    bcchunk *k = NULL;
    const char *why = NULL;
    if (!lbc_compile(genv, expr, &k, &why)) {
        *msg = why;
        return LBC_DECLINED;
    }
    bcclosure *top = (bcclosure *)calloc(1, sizeof(bcclosure));
    if (top == NULL) {
        *msg = "out of memory";
        return LBC_ERR;
    }
    top->chunk = k;
    const char *err = NULL;
    if (!vm_run(top, genv, out, &err)) {
        *msg = err;
        return LBC_ERR;
    }
    return LBC_OK;
}

// ===========================================================================
// Register-bytecode form (the dispatch-count lever). A parallel compiler + VM
// reusing the shared scope/resolve/const/frozen-op/global-slot/thin-call
// machinery above. Instructions name operand + destination registers explicitly,
// so a local read is free (the op addresses the local's register directly) and
// `(+ x y)` is ONE dispatch vs the stack form's LOAD, LOAD, ADD. Differential-
// tested against BOTH lisp_eval and the stack VM.
// ===========================================================================

static int remit(compiler *C, bcchunk *k, rop op, int a, int b, int c, int d) {
    if (k->nrcode == k->caprcode) {
        int nc = k->caprcode ? k->caprcode * 2 : 32;
        rinstr *ni = (rinstr *)realloc(k->rcode, (size_t)nc * sizeof(rinstr));
        if (ni == NULL)
            lbc_decline(C, "out of memory");
        k->rcode = ni;
        k->caprcode = nc;
    }
    int at = k->nrcode;
    k->rcode[at].op = (uint8_t)op;
    k->rcode[at].a = a;
    k->rcode[at].b = b;
    k->rcode[at].c = c;
    k->rcode[at].d = d;
    k->nrcode++;
    return at;
}

static int ralloc(compiler *C, fnstate *fn) {
    if (fn->freereg >= LBC_MAX_STACK)
        lbc_decline(C, "register pressure");
    int r = fn->freereg++;
    if (fn->freereg > fn->maxreg)
        fn->maxreg = fn->freereg;
    return r;
}

// Bind a name to a freshly allocated register (param / let / internal define).
static int rbind(compiler *C, fnstate *fn, lisp_value name) {
    int slot = ralloc(C, fn);
    bind_local(C, fn, name, slot);
    return slot;
}

static int radd_child(compiler *C, bcchunk *parent, bcchunk *child) {
    if (parent->nchildren == parent->capchildren) {
        int nc = parent->capchildren ? parent->capchildren * 2 : 4;
        bcchunk **nch =
            (bcchunk **)realloc(parent->children, (size_t)nc * sizeof(bcchunk *));
        if (nch == NULL)
            lbc_decline(C, "out of memory");
        parent->children = nch;
        parent->capchildren = nc;
    }
    parent->children[parent->nchildren] = child;
    return parent->nchildren++;
}

static void rcompile_into(compiler *C, fnstate *fn, lisp_value e, int dst);
static void rcompile_tail(compiler *C, fnstate *fn, lisp_value e);
static bcchunk *rcompile_lambda(compiler *C, fnstate *parent, lisp_value params,
                                lisp_value body, lisp_value self_name);

// Produce e's value into some register and return it. A local resolves to its
// own register with NO code emitted -- the dispatch-saving core of the register
// form.
static int rcompile_rvalue(compiler *C, fnstate *fn, lisp_value e) {
    if (lisp_is_symbol(e)) {
        int kind;
        bool boxed;
        int idx = resolve(C, fn, e, &kind, &boxed);
        if (kind == R_LOCAL && !boxed)
            return idx;  // an unboxed local IS its register -- zero code
    }
    int r = ralloc(C, fn);
    rcompile_into(C, fn, e, r);
    return r;
}

// Compile a body into dst (tail==false) or in tail position (tail==true, dst
// ignored). Leading internal defines bind register-locals.
static void rcompile_body(compiler *C, fnstate *fn, lisp_value body, int dst,
                          bool tail) {
    // Leading internal defines form a mutually-recursive group (letrec*): bind
    // every name to an undef cell up front so the inits can reference each other
    // (forward + mutual recursion), then compile each init in that scope. A
    // (define (f ...) ...) init is a lambda compiled with self_name f, so a
    // self-call still uses the fast LOADSELF; a sibling call goes through f's
    // cell.
    bcchunk *k = fn->chunk;
    int dslot[LBC_MAX_LOCALS], nd = 0;
    lisp_value dname[LBC_MAX_LOCALS], dform[LBC_MAX_LOCALS];
    bool dlam[LBC_MAX_LOCALS];
    while (lisp_is_pair(body)) {
        lisp_value form = lisp_car(body);
        if (!(lisp_is_pair(form) && sym_is(lisp_car(form), "define")))
            break;
        lisp_value rest = lisp_cdr(form);
        if (!lisp_is_pair(rest))
            lbc_decline(C, "malformed define");
        lisp_value target = lisp_car(rest);
        lisp_value nm;
        bool islam;
        if (lisp_is_symbol(target)) {
            if (!lisp_is_pair(lisp_cdr(rest)))
                lbc_decline(C, "malformed define");
            nm = target;
            islam = false;
        } else if (lisp_is_pair(target)) {
            nm = lisp_car(target);
            if (!lisp_is_symbol(nm))
                lbc_decline(C, "define: name must be a symbol");
            islam = true;
        } else {
            lbc_decline(C, "malformed define");
            return;  // unreachable; silences -Wmaybe-uninitialized
        }
        if (nd >= LBC_MAX_LOCALS)
            lbc_decline(C, "too many internal defines");
        int slot = ralloc(C, fn);
        remit(C, k, ROP_LOADK, slot, add_const(C, k, LISP_UNDEF), 0, 0);
        remit(C, k, ROP_MKCELL, slot, 0, 0, 0);
        bind_local(C, fn, nm, slot);
        mark_boxed(fn);
        dname[nd] = nm;
        dslot[nd] = slot;
        dform[nd] = form;
        dlam[nd] = islam;
        nd++;
        body = lisp_cdr(body);
    }
    for (int i = 0; i < nd; i++) {
        int save2 = fn->freereg, v;
        lisp_value rest = lisp_cdr(dform[i]);
        if (dlam[i]) {  // (define (f . a) body...)
            lisp_value target = lisp_car(rest);
            bcchunk *child = rcompile_lambda(C, fn, lisp_cdr(target),
                                             lisp_cdr(rest), dname[i]);
            int ci = radd_child(C, k, child);
            v = ralloc(C, fn);
            remit(C, k, ROP_CLOSURE, v, ci, 0, 0);
        } else {  // (define x val)
            v = rcompile_rvalue(C, fn, lisp_car(lisp_cdr(rest)));
        }
        remit(C, k, ROP_CELLSET, dslot[i], v, 0, 0);
        fn->freereg = save2;
    }
    if (!lisp_is_pair(body)) {
        int r = tail ? ralloc(C, fn) : dst;
        remit(C, fn->chunk, ROP_LOADK, r, add_const(C, fn->chunk, LISP_UNDEF), 0, 0);
        if (tail)
            remit(C, fn->chunk, ROP_RET, r, 0, 0, 0);
        return;
    }
    while (lisp_is_pair(lisp_cdr(body))) {
        int save = fn->freereg;
        int t = ralloc(C, fn);
        rcompile_into(C, fn, lisp_car(body), t);
        fn->freereg = save;  // discard the for-effect value
        body = lisp_cdr(body);
    }
    if (tail)
        rcompile_tail(C, fn, lisp_car(body));
    else
        rcompile_into(C, fn, lisp_car(body), dst);
}

// let / let* / letrec with capture analysis -> boxed cells. kind: 0=let (inits in
// the outer scope), 1=let* (sequential), 2=letrec (all names pre-bound to undef
// cells so the inits can mutually reference each other). Used in both value
// (tail==false, value into dst) and tail position.
static void rcompile_let(compiler *C, fnstate *fn, int kind, lisp_value binds,
                         lisp_value lbody, int dst, bool tail) {
    bcchunk *k = fn->chunk;
    int save_fr = fn->freereg, save_nl = fn->nlocals;
    symset frame = {.n = 0}, cap = {.n = 0};
    for (lisp_value b = binds; lisp_is_pair(b); b = lisp_cdr(b))
        if (lisp_is_pair(lisp_car(b)))
            ss_add(C, &frame, lisp_car(lisp_car(b)));
    if (kind == 2) {  // letrec: box every binding (forward-cell semantics)
        for (int i = 0; i < frame.n; i++)
            ss_add(C, &cap, frame.items[i]);
    } else {
        if (kind == 1)  // let*: a later init may capture an earlier binding
            for (lisp_value b = binds; lisp_is_pair(b); b = lisp_cdr(b))
                if (lisp_is_pair(lisp_car(b)) && lisp_is_pair(lisp_cdr(lisp_car(b))))
                    collect_captures(C, lisp_car(lisp_cdr(lisp_car(b))), &frame, &cap);
        captures_of_body(C, lbody, &frame, &cap);
    }

    if (kind == 2) {
        int slots[LBC_MAX_LOCALS], nb = 0;
        for (lisp_value b = binds; lisp_is_pair(b); b = lisp_cdr(b)) {
            int slot = ralloc(C, fn);
            remit(C, k, ROP_LOADK, slot, add_const(C, k, LISP_UNDEF), 0, 0);
            remit(C, k, ROP_MKCELL, slot, 0, 0, 0);
            bind_local(C, fn, lisp_car(lisp_car(b)), slot);
            mark_boxed(fn);
            slots[nb++] = slot;
        }
        int i = 0;
        for (lisp_value b = binds; lisp_is_pair(b); b = lisp_cdr(b), i++) {
            lisp_value nm = lisp_car(lisp_car(b));
            lisp_value ie = lisp_car(lisp_cdr(lisp_car(b)));
            int save2 = fn->freereg, v;
            if (lisp_is_pair(ie) && sym_is(lisp_car(ie), "lambda")) {
                bcchunk *child = rcompile_lambda(C, fn, lisp_car(lisp_cdr(ie)),
                                                 lisp_cdr(lisp_cdr(ie)), nm);
                int ci = radd_child(C, k, child);
                v = ralloc(C, fn);
                remit(C, k, ROP_CLOSURE, v, ci, 0, 0);
            } else {
                v = rcompile_rvalue(C, fn, ie);
            }
            remit(C, k, ROP_CELLSET, slots[i], v, 0, 0);
            fn->freereg = save2;
        }
    } else if (kind == 0) {  // let: all inits in outer scope, then bind + box
        int slots[LBC_MAX_LOCALS], nb = 0;
        for (lisp_value b = binds; lisp_is_pair(b); b = lisp_cdr(b)) {
            int slot = ralloc(C, fn);
            rcompile_into(C, fn, lisp_car(lisp_cdr(lisp_car(b))), slot);
            slots[nb++] = slot;
        }
        int i = 0;
        for (lisp_value b = binds; lisp_is_pair(b); b = lisp_cdr(b), i++) {
            lisp_value nm = lisp_car(lisp_car(b));
            bind_local(C, fn, nm, slots[i]);
            if (ss_has(&cap, nm)) {
                mark_boxed(fn);
                remit(C, k, ROP_MKCELL, slots[i], 0, 0, 0);
            }
        }
    } else {  // let*: sequential bind + box
        for (lisp_value b = binds; lisp_is_pair(b); b = lisp_cdr(b)) {
            lisp_value bd = lisp_car(b);
            int slot = ralloc(C, fn);
            rcompile_into(C, fn, lisp_car(lisp_cdr(bd)), slot);
            lisp_value nm = lisp_car(bd);
            bind_local(C, fn, nm, slot);
            if (ss_has(&cap, nm)) {
                mark_boxed(fn);
                remit(C, k, ROP_MKCELL, slot, 0, 0, 0);
            }
        }
    }
    rcompile_body(C, fn, lbody, tail ? -1 : dst, tail);
    fn->freereg = save_fr;
    fn->nlocals = save_nl;
}

static bcchunk *rcompile_lambda(compiler *C, fnstate *parent, lisp_value params,
                                lisp_value body, lisp_value self_name) {
    fnstate fn;
    memset(&fn, 0, sizeof(fn));
    fn.parent = parent;
    fn.chunk = chunk_new(C);
    fn.self_name = (self_name == 0) ? LISP_UNDEF : self_name;
    // Which parameters are captured by a nested lambda -> box them at entry.
    symset frame = {.n = 0}, cap = {.n = 0};
    ss_add_params(C, &frame, params);
    captures_of_body(C, body, &frame, &cap);
    int np = 0;
    lisp_value p = params;
    while (lisp_is_pair(p)) {
        if (!lisp_is_symbol(lisp_car(p)))
            lbc_decline(C, "bad parameter");
        int slot = rbind(C, &fn, lisp_car(p));
        if (ss_has(&cap, lisp_car(p))) {
            mark_boxed(&fn);
            remit(C, fn.chunk, ROP_MKCELL, slot, 0, 0, 0);  // wrap the arg value
        }
        np++;
        p = lisp_cdr(p);
    }
    if (lisp_is_symbol(p)) {
        int slot = rbind(C, &fn, p);
        if (ss_has(&cap, p)) {
            mark_boxed(&fn);
            remit(C, fn.chunk, ROP_MKCELL, slot, 0, 0, 0);
        }
        fn.chunk->has_rest = true;
    } else if (!lisp_is_empty(p)) {
        lbc_decline(C, "malformed parameter list");
    }
    fn.chunk->nparams = np;
    rcompile_body(C, &fn, body, -1, true);
    fn.chunk->nregs = fn.maxreg;
    return fn.chunk;
}

// Emit a call: operator into base, args into base+1.., then CALL/TAILCALL.
static int rcompile_callseq(compiler *C, fnstate *fn, lisp_value e) {
    lisp_value head = lisp_car(e), args = lisp_cdr(e);
    lisp_value t = args;
    while (lisp_is_pair(t))
        t = lisp_cdr(t);
    if (!lisp_is_empty(t))
        lbc_decline(C, "improper argument list");
    int base = ralloc(C, fn);
    rcompile_into(C, fn, head, base);
    int n = 0;
    for (lisp_value a = args; lisp_is_pair(a); a = lisp_cdr(a)) {
        int r = ralloc(C, fn);
        rcompile_into(C, fn, lisp_car(a), r);
        (void)r;
        n++;
    }
    return (base << 8) | n;  // pack base + nargs (n < 256, base < 256 in practice)
}

static void rcompile_into(compiler *C, fnstate *fn, lisp_value e, int dst) {
    bcchunk *k = fn->chunk;
    if (lisp_is_fixnum(e) || lisp_is_flonum(e) || lisp_is_string(e) ||
        lisp_is_char(e) || lisp_is_keyword(e) || e == LISP_TRUE ||
        e == LISP_FALSE || e == LISP_UNDEF) {
        remit(C, k, ROP_LOADK, dst, add_const(C, k, e), 0, 0);
        return;
    }
    if (lisp_is_symbol(e)) {
        int kind;
        bool boxed;
        int idx = resolve(C, fn, e, &kind, &boxed);
        switch (kind) {
            case R_LOCAL:
                if (boxed)
                    remit(C, k, ROP_CELLGET, dst, idx, 0, 0);  // read through cell
                else if (idx != dst)
                    remit(C, k, ROP_MOVE, dst, idx, 0, 0);
                return;
            case R_UPVAL:  // upvalue is always a cell: load it, then read its car
                remit(C, k, ROP_LOADUP, dst, idx, 0, 0);
                remit(C, k, ROP_CELLGET, dst, dst, 0, 0);
                return;
            case R_SELF: remit(C, k, ROP_LOADSELF, dst, 0, 0, 0); return;
            case R_GLOBAL: {
                if (g_global_slots) {
                    lisp_value cell = find_global_cell(C->genv, e);
                    if (cell != LISP_EMPTY) {
                        remit(C, k, ROP_LOADGS, dst, add_const(C, k, cell), 0, 0);
                        return;
                    }
                }
                remit(C, k, ROP_LOADG, dst, add_const(C, k, e), 0, 0);
                return;
            }
        }
        return;
    }
    if (!lisp_is_pair(e)) {
        if (lisp_is_empty(e))
            lbc_decline(C, "cannot evaluate ()");
        remit(C, k, ROP_LOADK, dst, add_const(C, k, e), 0, 0);
        return;
    }
    lisp_value head = lisp_car(e), rest = lisp_cdr(e);
    if (lisp_is_symbol(head)) {
        if (sym_is(head, "quote")) {
            remit(C, k, ROP_LOADK, dst, add_const(C, k, lisp_car(rest)), 0, 0);
            return;
        }
        if (sym_is(head, "if")) {
            int tc = rcompile_rvalue(C, fn, lisp_car(rest));
            int jf = remit(C, k, ROP_JMPF, tc, -1, 0, 0);
            rcompile_into(C, fn, lisp_car(lisp_cdr(rest)), dst);
            int je = remit(C, k, ROP_JMP, -1, 0, 0, 0);
            k->rcode[jf].b = k->nrcode;
            lisp_value elsep = lisp_cdr(lisp_cdr(rest));
            if (lisp_is_pair(elsep))
                rcompile_into(C, fn, lisp_car(elsep), dst);
            else
                remit(C, k, ROP_LOADK, dst, add_const(C, k, LISP_UNDEF), 0, 0);
            k->rcode[je].a = k->nrcode;
            return;
        }
        if (sym_is(head, "begin")) {
            if (!lisp_is_pair(rest)) {
                remit(C, k, ROP_LOADK, dst, add_const(C, k, LISP_UNDEF), 0, 0);
                return;
            }
            rcompile_body(C, fn, rest, dst, false);
            return;
        }
        if (sym_is(head, "lambda")) {
            bcchunk *child = rcompile_lambda(C, fn, lisp_car(rest), lisp_cdr(rest), 0);
            int ci = radd_child(C, k, child);
            remit(C, k, ROP_CLOSURE, dst, ci, 0, 0);
            return;
        }
        if (sym_is(head, "and") || sym_is(head, "or")) {
            bool is_and = sym_is(head, "and");
            if (!lisp_is_pair(rest)) {
                remit(C, k, ROP_LOADK, dst, add_const(C, k, is_and ? LISP_TRUE : LISP_FALSE), 0, 0);
                return;
            }
            int jmps[64], n = 0;
            lisp_value a = rest;
            while (lisp_is_pair(lisp_cdr(a))) {
                rcompile_into(C, fn, lisp_car(a), dst);
                if (n >= 64)
                    lbc_decline(C, "and/or too long");
                jmps[n++] = remit(C, k, is_and ? ROP_JMPF : ROP_JMPT, dst, -1, 0, 0);
                a = lisp_cdr(a);
            }
            rcompile_into(C, fn, lisp_car(a), dst);
            for (int i = 0; i < n; i++)
                k->rcode[jmps[i]].b = k->nrcode;
            return;
        }
        if (sym_is(head, "when") || sym_is(head, "unless")) {
            bool want = sym_is(head, "when");
            int tc = rcompile_rvalue(C, fn, lisp_car(rest));
            int jf = remit(C, k, want ? ROP_JMPF : ROP_JMPT, tc, -1, 0, 0);
            rcompile_body(C, fn, lisp_cdr(rest), dst, false);
            int je = remit(C, k, ROP_JMP, -1, 0, 0, 0);
            k->rcode[jf].b = k->nrcode;
            remit(C, k, ROP_LOADK, dst, add_const(C, k, LISP_UNDEF), 0, 0);
            k->rcode[je].a = k->nrcode;
            return;
        }
        if (sym_is(head, "cond")) {
            int endj[64], ne = 0;
            lisp_value cl = rest;
            bool had_else = false;
            while (lisp_is_pair(cl)) {
                lisp_value clause = lisp_car(cl);
                if (!lisp_is_pair(clause))
                    lbc_decline(C, "malformed cond clause");
                lisp_value test = lisp_car(clause), cbody = lisp_cdr(clause);
                if (sym_is(test, "else")) {
                    rcompile_body(C, fn, cbody, dst, false);
                    had_else = true;
                    break;
                }
                if (!lisp_is_pair(cbody))
                    lbc_decline(C, "cond clause without body unsupported");
                int tc = rcompile_rvalue(C, fn, test);
                int jf = remit(C, k, ROP_JMPF, tc, -1, 0, 0);
                rcompile_body(C, fn, cbody, dst, false);
                if (ne >= 64)
                    lbc_decline(C, "cond too long");
                endj[ne++] = remit(C, k, ROP_JMP, -1, 0, 0, 0);
                k->rcode[jf].b = k->nrcode;
                cl = lisp_cdr(cl);
            }
            if (!had_else)
                remit(C, k, ROP_LOADK, dst, add_const(C, k, LISP_UNDEF), 0, 0);
            for (int i = 0; i < ne; i++)
                k->rcode[endj[i]].a = k->nrcode;
            return;
        }
        if (sym_is(head, "let") && lisp_is_pair(rest) && lisp_is_symbol(lisp_car(rest))) {
            // named let -> self-recursive lambda applied to inits.
            lisp_value name = lisp_car(rest);
            lisp_value binds = lisp_car(lisp_cdr(rest));
            lisp_value lbody = lisp_cdr(lisp_cdr(rest));
            lisp_value pl = LISP_EMPTY, pt = LISP_EMPTY;
            for (lisp_value b = binds; lisp_is_pair(b); b = lisp_cdr(b)) {
                lisp_value pc = lisp_cons(lisp_car(lisp_car(b)), LISP_EMPTY);
                if (pl == LISP_EMPTY) pl = pc; else set_cdr_lbc(pt, pc);
                pt = pc;
            }
            bcchunk *child = rcompile_lambda(C, fn, pl, lbody, name);
            int ci = radd_child(C, k, child);
            int base = ralloc(C, fn);
            remit(C, k, ROP_CLOSURE, base, ci, 0, 0);
            int n = 0;
            for (lisp_value b = binds; lisp_is_pair(b); b = lisp_cdr(b)) {
                int r = ralloc(C, fn);
                rcompile_into(C, fn, lisp_car(lisp_cdr(lisp_car(b))), r);
                (void)r;
                n++;
            }
            remit(C, k, ROP_CALL, base, n, 0, 0);
            if (dst != base)
                remit(C, k, ROP_MOVE, dst, base, 0, 0);
            fn->freereg = base;
            return;
        }
        if (sym_is(head, "let")) {
            rcompile_let(C, fn, 0, lisp_car(rest), lisp_cdr(rest), dst, false);
            return;
        }
        if (sym_is(head, "let*")) {
            rcompile_let(C, fn, 1, lisp_car(rest), lisp_cdr(rest), dst, false);
            return;
        }
        if (sym_is(head, "letrec")) {
            rcompile_let(C, fn, 2, lisp_car(rest), lisp_cdr(rest), dst, false);
            return;
        }
        if (sym_is(head, "set!")) {
            lisp_value target = lisp_car(rest);
            int kind;
            bool boxed;
            int idx = resolve(C, fn, target, &kind, &boxed);
            if (kind == R_SELF)
                lbc_decline(C, "set! on a self binding unsupported");
            lisp_value valexpr = lisp_car(lisp_cdr(rest));
            int save = fn->freereg;
            if (kind == R_LOCAL && !boxed) {
                rcompile_into(C, fn, valexpr, idx);  // direct register write
            } else if (kind == R_LOCAL) {            // boxed local: write its cell
                int v = rcompile_rvalue(C, fn, valexpr);
                remit(C, k, ROP_CELLSET, idx, v, 0, 0);
            } else if (kind == R_UPVAL) {            // captured: cell via upvalue
                int v = rcompile_rvalue(C, fn, valexpr);
                int cellr = ralloc(C, fn);
                remit(C, k, ROP_LOADUP, cellr, idx, 0, 0);
                remit(C, k, ROP_CELLSET, cellr, v, 0, 0);
            } else {                                 // global slot
                int v = rcompile_rvalue(C, fn, valexpr);
                remit(C, k, ROP_SETG, v, add_const(C, k, target), 0, 0);
            }
            fn->freereg = save;
            remit(C, k, ROP_LOADK, dst, add_const(C, k, LISP_UNDEF), 0, 0);
            return;
        }
        if (sym_is(head, "define"))
            lbc_decline(C, "define only supported at body head");
        if (is_special_head(head))
            lbc_decline(C, "form not supported by prototype");

        // inline-cached fast op? (no frozen ops -- the op stays redefinable; the
        // IC guards on its binding cell at runtime, deopting to a real call.)
        fastkind fk;
        lisp_value cell, expected;
        int argc = list_len(rest);
        if (reg_fastop(C, fn, head, argc, &fk, &cell, &expected)) {
            int save = fn->freereg;
            int ra = rcompile_rvalue(C, fn, lisp_car(rest));
            int rb = fastkind_unary(fk)
                         ? 0
                         : rcompile_rvalue(C, fn, lisp_car(lisp_cdr(rest)));
            int ic = add_ic(C, k, cell, expected, fk);
            remit(C, k, ROP_OPCALL, dst, ra, rb, ic);
            fn->freereg = save;
            return;
        }
    }
    // general application
    int save = fn->freereg;
    int packed = rcompile_callseq(C, fn, e);
    int base = packed >> 8, n = packed & 0xff;
    remit(C, k, ROP_CALL, base, n, 0, 0);
    if (dst != base)
        remit(C, k, ROP_MOVE, dst, base, 0, 0);
    fn->freereg = save;
}

static void rcompile_tail(compiler *C, fnstate *fn, lisp_value e) {
    bcchunk *k = fn->chunk;
    if (lisp_is_pair(e) && lisp_is_symbol(lisp_car(e))) {
        lisp_value head = lisp_car(e), rest = lisp_cdr(e);
        if (sym_is(head, "if")) {
            int tc = rcompile_rvalue(C, fn, lisp_car(rest));
            int jf = remit(C, k, ROP_JMPF, tc, -1, 0, 0);
            rcompile_tail(C, fn, lisp_car(lisp_cdr(rest)));
            k->rcode[jf].b = k->nrcode;
            lisp_value elsep = lisp_cdr(lisp_cdr(rest));
            if (lisp_is_pair(elsep)) {
                rcompile_tail(C, fn, lisp_car(elsep));
            } else {
                int r = ralloc(C, fn);
                remit(C, k, ROP_LOADK, r, add_const(C, k, LISP_UNDEF), 0, 0);
                remit(C, k, ROP_RET, r, 0, 0, 0);
            }
            return;
        }
        if (sym_is(head, "begin") && lisp_is_pair(rest)) {
            rcompile_body(C, fn, rest, -1, true);
            return;
        }
        if (sym_is(head, "when") || sym_is(head, "unless")) {
            bool want = sym_is(head, "when");
            int tc = rcompile_rvalue(C, fn, lisp_car(rest));
            int jf = remit(C, k, want ? ROP_JMPF : ROP_JMPT, tc, -1, 0, 0);
            rcompile_body(C, fn, lisp_cdr(rest), -1, true);
            k->rcode[jf].b = k->nrcode;
            int r = ralloc(C, fn);
            remit(C, k, ROP_LOADK, r, add_const(C, k, LISP_UNDEF), 0, 0);
            remit(C, k, ROP_RET, r, 0, 0, 0);
            return;
        }
        if (sym_is(head, "cond")) {
            lisp_value cl = rest;
            bool had_else = false;
            while (lisp_is_pair(cl)) {
                lisp_value clause = lisp_car(cl);
                if (!lisp_is_pair(clause))
                    lbc_decline(C, "malformed cond clause");
                lisp_value test = lisp_car(clause), cbody = lisp_cdr(clause);
                if (sym_is(test, "else")) {
                    rcompile_body(C, fn, cbody, -1, true);
                    had_else = true;
                    break;
                }
                if (!lisp_is_pair(cbody))
                    lbc_decline(C, "cond clause without body unsupported");
                int tc = rcompile_rvalue(C, fn, test);
                int jf = remit(C, k, ROP_JMPF, tc, -1, 0, 0);
                rcompile_body(C, fn, cbody, -1, true);
                k->rcode[jf].b = k->nrcode;
                cl = lisp_cdr(cl);
            }
            if (!had_else) {
                int r = ralloc(C, fn);
                remit(C, k, ROP_LOADK, r, add_const(C, k, LISP_UNDEF), 0, 0);
                remit(C, k, ROP_RET, r, 0, 0, 0);
            }
            return;
        }
        if (sym_is(head, "let") && lisp_is_pair(rest) && lisp_is_symbol(lisp_car(rest))) {
            // named let in tail position -> tail call of the loop closure.
            lisp_value name = lisp_car(rest);
            lisp_value binds = lisp_car(lisp_cdr(rest));
            lisp_value lbody = lisp_cdr(lisp_cdr(rest));
            lisp_value pl = LISP_EMPTY, pt = LISP_EMPTY;
            for (lisp_value b = binds; lisp_is_pair(b); b = lisp_cdr(b)) {
                lisp_value pc = lisp_cons(lisp_car(lisp_car(b)), LISP_EMPTY);
                if (pl == LISP_EMPTY) pl = pc; else set_cdr_lbc(pt, pc);
                pt = pc;
            }
            bcchunk *child = rcompile_lambda(C, fn, pl, lbody, name);
            int ci = radd_child(C, k, child);
            int base = ralloc(C, fn);
            remit(C, k, ROP_CLOSURE, base, ci, 0, 0);
            int n = 0;
            for (lisp_value b = binds; lisp_is_pair(b); b = lisp_cdr(b)) {
                int r = ralloc(C, fn);
                rcompile_into(C, fn, lisp_car(lisp_cdr(lisp_car(b))), r);
                (void)r;
                n++;
            }
            remit(C, k, ROP_TAILCALL, base, n, 0, 0);
            return;
        }
        if (sym_is(head, "let") || sym_is(head, "let*") || sym_is(head, "letrec")) {
            int kind = sym_is(head, "letrec") ? 2 : sym_is(head, "let*") ? 1 : 0;
            rcompile_let(C, fn, kind, lisp_car(rest), lisp_cdr(rest), -1, true);
            return;
        }
        // fast op or special form: produce a value then RET (below). A plain
        // application becomes a tail call.
        fastkind fk;
        lisp_value cell, expected;
        if (!is_special_head(head) &&
            !reg_fastop(C, fn, head, list_len(rest), &fk, &cell, &expected)) {
            int packed = rcompile_callseq(C, fn, e);
            remit(C, k, ROP_TAILCALL, packed >> 8, packed & 0xff, 0, 0);
            return;
        }
    } else if (lisp_is_pair(e)) {
        int packed = rcompile_callseq(C, fn, e);
        remit(C, k, ROP_TAILCALL, packed >> 8, packed & 0xff, 0, 0);
        return;
    }
    int r = rcompile_rvalue(C, fn, e);
    remit(C, k, ROP_RET, r, 0, 0, 0);
}

static bool rlbc_compile(lisp_value genv, lisp_value expr, bcchunk **out,
                         const char **why) {
    compiler C;
    memset(&C, 0, sizeof(C));
    C.genv = genv;
    if (setjmp(C.decline)) {
        *why = C.why;
        return false;
    }
    fnstate fn;
    memset(&fn, 0, sizeof(fn));
    fn.chunk = chunk_new(&C);
    fn.self_name = LISP_UNDEF;
    rcompile_body(&C, &fn, lisp_cons(expr, LISP_EMPTY), -1, true);
    fn.chunk->nregs = fn.maxreg;
    *out = fn.chunk;
    return true;
}

// --- Register VM (contiguous register stack with register windows) ---------
//
// One preallocated value array R is the register stack; each active frame owns a
// window R[base .. base+nregs). A non-tail call places the callee's window right
// after the operator slot, so the args (already laid out at base+callbase+1..)
// ARE the callee's parameter registers -- zero copy. A tail call moves the args
// down to the current window's base and reuses the frame -- no allocation, ever.

#define LBC_REGSTACK 65536

typedef struct {
    bcchunk *chunk;
    bcclosure *clo;
    int base;        // this frame's window start in R
    int pc;
    int result_abs;  // absolute R index to receive this frame's return; -1 = top
} rframe;

// Set up a callee's window at R[abase..]: args already sit at R[abase..abase+nargs);
// check arity and collect any rest list. Registers above the params are NOT
// cleared: the compiler assigns every register before it is read (allocated then
// written by the producing op; locals bound before use), so leftover values in
// the window are never observed -- and the fuzzer guards that invariant.
static bool init_callee(lisp_value *R, int abase, bcchunk *k, int nargs,
                        const char **err) {
    if (k->has_rest) {
        if (nargs < k->nparams) { *err = "too few arguments"; return false; }
        lisp_value rest = LISP_EMPTY;
        for (int j = nargs - 1; j >= k->nparams; j--) {
            lisp_value c = lisp_cons(R[abase + j], rest);
            if (c == LISP_UNDEF) { *err = "out of memory"; return false; }
            rest = c;
        }
        R[abase + k->nparams] = rest;
    } else if (nargs != k->nparams) {
        *err = "argument count mismatch";
        return false;
    }
    return true;
}

static bool rvm_run(bcclosure *top, lisp_value genv, lisp_value *out,
                    const char **errout) {
    lisp_value *R = (lisp_value *)malloc(sizeof(lisp_value) * LBC_REGSTACK);
    rframe *frames = (rframe *)malloc(sizeof(rframe) * LBC_MAX_FRAMES);
    if (R == NULL || frames == NULL) {
        free(R); free(frames); *errout = "out of memory"; return false;
    }
    const char *err = NULL;
    int depth = 0;
    if (top->chunk->nregs > LBC_REGSTACK) { err = "register stack overflow"; goto fail; }
    for (int i = 0; i < top->chunk->nregs; i++)
        R[i] = LISP_UNDEF;
    frames[0] = (rframe){top->chunk, top, 0, 0, -1};
    depth = 1;

    for (;;) {
        rframe *f = &frames[depth - 1];
        lisp_value *Rf = R + f->base;  // frame-relative register window
        rinstr ins = f->chunk->rcode[f->pc++];
        switch (ins.op) {
            case ROP_LOADK: Rf[ins.a] = f->chunk->consts[ins.b]; break;
            case ROP_MOVE: Rf[ins.a] = Rf[ins.b]; break;
            case ROP_LOADUP: Rf[ins.a] = f->clo->upvals[ins.b]; break;
            case ROP_LOADG: {
                lisp_value v;
                if (!lisp_env_lookup(genv, f->chunk->consts[ins.b], &v)) {
                    err = "unbound variable"; goto fail;
                }
                Rf[ins.a] = v;
                break;
            }
            case ROP_LOADGS: Rf[ins.a] = lisp_cdr(f->chunk->consts[ins.b]); break;
            case ROP_LOADSELF: Rf[ins.a] = LBC_MK_CLO(f->clo); break;
            case ROP_SETG:
                if (!lisp_env_set(genv, f->chunk->consts[ins.b], Rf[ins.a])) {
                    err = "set! on unbound variable"; goto fail;
                }
                break;
            case ROP_CLOSURE: {
                bcchunk *child = f->chunk->children[ins.b];
                bcclosure *c = (bcclosure *)malloc(
                    sizeof(bcclosure) + sizeof(lisp_value) * (size_t)child->nupdesc);
                if (c == NULL) { err = "out of memory"; goto fail; }
                c->chunk = child;
                for (int i = 0; i < child->nupdesc; i++)
                    c->upvals[i] = child->updesc[i].from_local
                                       ? Rf[child->updesc[i].index]
                                       : f->clo->upvals[child->updesc[i].index];
                Rf[ins.a] = LBC_MK_CLO(c);
                break;
            }
            case ROP_JMP: f->pc = ins.a; break;
            case ROP_JMPF: if (!lisp_truthy(Rf[ins.a])) f->pc = ins.b; break;
            case ROP_JMPT: if (lisp_truthy(Rf[ins.a])) f->pc = ins.b; break;
            case ROP_OPCALL: {
                // No frozen ops: the operator is still an ordinary global. The IC
                // pins the builtin that was canonical when this site compiled;
                // the inlined path runs ONLY while cur == expected and the
                // operands are the fast type, else we call cur (which handles
                // redefinition and off-type/flonum operands with oracle parity).
                bcic *ic = &f->chunk->ics[ins.d];
                lisp_value cur = lisp_cdr(ic->cell);
                lisp_value a = Rf[ins.b];
                bool slow = false;
                if (cur == ic->expected) {
                    switch (ic->kind) {
                        case FK_ADD: case FK_SUB: case FK_MUL: {
                            lisp_value b = Rf[ins.c];
                            if (lisp_is_fixnum(a) && lisp_is_fixnum(b)) {
                                int64_t x = lisp_fixnum_val(a), y = lisp_fixnum_val(b);
                                int64_t r = ic->kind == FK_ADD ? x + y
                                            : ic->kind == FK_SUB ? x - y : x * y;
                                Rf[ins.a] = lisp_fixnum(r);
                            } else {
                                slow = true;
                            }
                            break;
                        }
                        case FK_LT: case FK_LE: case FK_GT: case FK_GE: case FK_NUMEQ: {
                            lisp_value b = Rf[ins.c];
                            if (lisp_is_fixnum(a) && lisp_is_fixnum(b)) {
                                int64_t x = lisp_fixnum_val(a), y = lisp_fixnum_val(b);
                                bool ok = ic->kind == FK_LT ? x < y
                                          : ic->kind == FK_LE ? x <= y
                                          : ic->kind == FK_GT ? x > y
                                          : ic->kind == FK_GE ? x >= y : x == y;
                                Rf[ins.a] = ok ? LISP_TRUE : LISP_FALSE;
                            } else {
                                slow = true;
                            }
                            break;
                        }
                        case FK_EQ:
                            Rf[ins.a] = (a == Rf[ins.c]) ? LISP_TRUE : LISP_FALSE;
                            break;
                        case FK_CONS: {
                            lisp_value c = lisp_cons(a, Rf[ins.c]);
                            if (c == LISP_UNDEF) { err = "out of memory"; goto fail; }
                            Rf[ins.a] = c;
                            break;
                        }
                        case FK_CAR:
                            if (lisp_is_pair(a)) Rf[ins.a] = lisp_car(a);
                            else slow = true;  // call builtin car -> oracle's error
                            break;
                        case FK_CDR:
                            if (lisp_is_pair(a)) Rf[ins.a] = lisp_cdr(a);
                            else slow = true;
                            break;
                        case FK_NULLP:
                            Rf[ins.a] = lisp_is_empty(a) ? LISP_TRUE : LISP_FALSE;
                            break;
                        case FK_PAIRP:
                            Rf[ins.a] = lisp_is_pair(a) ? LISP_TRUE : LISP_FALSE;
                            break;
                        case FK_NOT:
                            Rf[ins.a] = lisp_truthy(a) ? LISP_FALSE : LISP_TRUE;
                            break;
                        default: slow = true; break;
                    }
                } else {
                    slow = true;  // operator was redefined -> call the new binding
                }
                if (slow) {
                    lisp_value as[2];
                    int n = fastkind_unary((fastkind)ic->kind) ? 1 : 2;
                    as[0] = a;
                    if (n == 2) as[1] = Rf[ins.c];
                    lisp_value r = lbc_call_proc(cur, as, n, &err);
                    if (r == LISP_UNDEF && err != NULL) goto fail;
                    Rf[ins.a] = r;
                }
                break;
            }
            case ROP_MKCELL: {
                lisp_value c = lisp_cons(Rf[ins.a], LISP_EMPTY);
                if (c == LISP_UNDEF) { err = "out of memory"; goto fail; }
                Rf[ins.a] = c;
                break;
            }
            case ROP_CELLGET: Rf[ins.a] = lisp_car(Rf[ins.b]); break;
            case ROP_CELLSET: set_car_lbc(Rf[ins.a], Rf[ins.b]); break;
            case ROP_CALL: {
                int callbase = ins.a, n = ins.b;
                lisp_value callee = Rf[callbase];
                if (LBC_IS_CLO(callee)) {
                    bcchunk *ck = LBC_CLO(callee)->chunk;
                    int cbase = f->base + callbase + 1;  // args already here -> zero copy
                    if (cbase + ck->nregs > LBC_REGSTACK) { err = "register stack overflow"; goto fail; }
                    if (depth >= LBC_MAX_FRAMES) { err = "call depth exceeded"; goto fail; }
                    if (!init_callee(R, cbase, ck, n, &err)) goto fail;
                    frames[depth] = (rframe){ck, LBC_CLO(callee), cbase, 0, f->base + callbase};
                    depth++;
                } else {
                    lisp_value r = lbc_call_proc(callee, &Rf[callbase + 1], n, &err);
                    if (r == LISP_UNDEF && err != NULL) goto fail;
                    Rf[callbase] = r;
                }
                break;
            }
            case ROP_TAILCALL: {
                int callbase = ins.a, n = ins.b;
                lisp_value callee = Rf[callbase];
                if (LBC_IS_CLO(callee)) {
                    bcchunk *ck = LBC_CLO(callee)->chunk;
                    for (int i = 0; i < n; i++)        // move args down to the window base
                        Rf[i] = Rf[callbase + 1 + i];
                    if (f->base + ck->nregs > LBC_REGSTACK) { err = "register stack overflow"; goto fail; }
                    if (!init_callee(R, f->base, ck, n, &err)) goto fail;
                    f->chunk = ck;
                    f->clo = LBC_CLO(callee);
                    f->pc = 0;  // base + result_abs unchanged -> returns to our caller
                } else {
                    lisp_value r = lbc_call_proc(callee, &Rf[callbase + 1], n, &err);
                    if (r == LISP_UNDEF && err != NULL) goto fail;
                    int ra = f->result_abs;
                    depth--;
                    if (depth == 0) { *out = r; free(R); free(frames); return true; }
                    R[ra] = r;
                }
                break;
            }
            case ROP_RET: {
                lisp_value r = Rf[ins.a];
                int ra = f->result_abs;
                depth--;
                if (depth == 0) { *out = r; free(R); free(frames); return true; }
                R[ra] = r;
                break;
            }
            default: err = "bad register opcode"; goto fail;
        }
    }
fail:
    free(R); free(frames);
    *errout = err != NULL ? err : "error";
    return false;
}

static lbc_status rlbc_eval(lisp_value genv, lisp_value expr, lisp_value *out,
                            const char **msg) {
    bcchunk *k = NULL;
    const char *why = NULL;
    if (!rlbc_compile(genv, expr, &k, &why)) {
        *msg = why;
        return LBC_DECLINED;
    }
    bcclosure *top = (bcclosure *)calloc(1, sizeof(bcclosure));
    if (top == NULL) { *msg = "out of memory"; return LBC_ERR; }
    top->chunk = k;
    const char *err = NULL;
    if (!rvm_run(top, genv, out, &err)) {
        *msg = err;
        return LBC_ERR;
    }
    return LBC_OK;
}
