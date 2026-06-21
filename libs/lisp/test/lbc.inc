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

typedef struct bcchunk {
    instr *code;
    int ncode, capcode;
    lisp_value *consts;
    int nconsts, capconsts;
    int nlocals;       // size of the runtime locals array
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
    } locals[LBC_MAX_LOCALS];
    int nlocals;          // active lexical bindings (a scope stack)
    int next_slot;        // monotonic slot high-water (== chunk->nlocals)
    lisp_value self_name; // this function's own name for self-recursion, or UNDEF
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
    fn->nlocals++;
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
            *kindout = R_UPVAL;
            return add_upval(C, fn, true, p->locals[i].slot);
        }
    if (name == p->self_name) {
        // The parent's own self-closure becomes an upvalue. Capturing a
        // mutually/forward recursive sibling by value is unsound; self capture
        // of an already-existing closure is fine.
        lbc_decline(C, "capture of enclosing self not supported");
    }
    int pk;
    int up = resolve_upval(C, p, name, &pk);
    if (up < 0)
        return -1;
    *kindout = R_UPVAL;
    return add_upval(C, fn, false, up);
}

// Resolve a name in fn: local slot, self, an upvalue index, or global.
static int resolve(compiler *C, fnstate *fn, lisp_value name, int *kind) {
    for (int i = fn->nlocals - 1; i >= 0; i--)
        if (fn->locals[i].name == name) {
            *kind = R_LOCAL;
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
    (void)resolve(C, fn, head, &kind);
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

// --- list helpers -----------------------------------------------------------

static int list_len(lisp_value l) {
    int n = 0;
    while (lisp_is_pair(l)) {
        n++;
        l = lisp_cdr(l);
    }
    return n;
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
    // Leading internal defines.
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
        int kind, idx = resolve(C, fn, e, &kind);
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
            int kind, idx = resolve(C, fn, target, &kind);
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
