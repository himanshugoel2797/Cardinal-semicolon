// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

// A Scheme-inspired Lisp evaluator built as an explicit-stack CEK abstract
// machine: the execution state (control expr, environment, accumulator, and a
// heap-linked chain of continuation frames) lives in a lisp_ctx_t heap object
// rather than on the C stack. This lets a computation be suspended at a safe
// point (when a per-slice reduction budget runs out), resumed later, and traced
// precisely by the GC -- the substrate the process model needs (a "process" is a
// context). lisp_eval / lisp_apply are thin wrappers that drive a context to
// completion, preserving their original synchronous contract.
//
// Tail calls cost O(1) continuation frames (the analogue of the old `goto tail`):
// a call in tail position pushes no frame. Deep NON-tail recursion grows the heap
// chain, not the C stack. Special forms: quote, quasiquote, if, define, lambda,
// let/let*/letrec/named-let, begin, set!, cond, and, or, when, unless, while,
// case. The common derived forms are kept as interpreter special cases (cheap,
// and a future JIT can recognize them directly) rather than as a macro engine.
// Objects are GC-allocated (gc.c).
//
// Deliberately NOT included (cut to keep the substrate small -- see
// notes/core/lisp-substrate.md): syntax-rules macros and call/cc / the
// exception system. They can return if a concrete need (e.g. a driver DSL)
// arises.

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "internal.h"  // env/closure/primitive/kont/ctx layouts + lisp_gc_alloc
#include "lisp.h"

#define MAX_ARGS 64  // cap on call arity (and rest-arg spread); raised with the VM.

// An effectively-unbounded reduction budget for the synchronous wrappers (which
// run to completion and never suspend). Not INT64_MAX: the freestanding stdint.h
// in common/ does not define the limit macros.
#define CTX_BUDGET_UNBOUNDED ((int64_t)0x7fffffffffffffffLL)

// --- Small helpers ----------------------------------------------------------

static lisp_value fail(const char **err, const char *msg) {
    if (err != NULL)
        *err = msg;
    return LISP_UNDEF;
}

// Mutate a pair's car/cdr. Used only on evaluator-owned structures (env
// bindings), never on user data -- the language model keeps pairs immutable.
static void set_cdr(lisp_value pair, lisp_value v) { ((lisp_pair *)lisp_obj(pair))->cdr = v; }

// --- Constructors -----------------------------------------------------------

lisp_value lisp_make_primitive(lisp_primitive_fn fn, const char *name) {
    lisp_prim_t *p = (lisp_prim_t *)lisp_gc_alloc(sizeof(lisp_prim_t));
    if (p == NULL)
        return LISP_UNDEF;
    p->h.header = LISP_MK_HEADER(LISP_OBJ_PRIMITIVE, 0);
    p->fn = fn;
    // Initialize name to a non-pointer BEFORE the nested allocation below: that
    // alloc can trigger a GC which would trace this (already-listed) object and
    // read p->name. LISP_UNDEF is an immediate, so trace() ignores it.
    p->name = LISP_UNDEF;
    p->name = lisp_make_symbol(name, strlen(name));
    if (p->name == LISP_UNDEF)
        return LISP_UNDEF;  // p is GC-tracked; the collector reclaims it
    return lisp_from_obj(p);
}

// --- Environments -----------------------------------------------------------

// Buckets in the top-level frame's hash table (power of two). The global frame
// holds ~150 bindings; 256 buckets keep chains near length 1.
#define GLOBAL_ENV_BUCKETS 256

// The hash bucket for `sym` in a `table`-backed frame. Symbols are interned and
// carry their name hash, so this is a field read + mask.
static inline size_t env_bucket(lisp_value table, lisp_value sym) {
    return (size_t)((lisp_named *)lisp_obj(sym))->hash & (lisp_vector_length(table) - 1);
}

lisp_value lisp_make_env(lisp_value parent) {
    lisp_env_t *e = (lisp_env_t *)lisp_gc_alloc(sizeof(lisp_env_t));
    if (e == NULL)
        return LISP_UNDEF;
    e->h.header = LISP_MK_HEADER(LISP_OBJ_ENV, 0);
    e->parent = parent;
    e->bindings = LISP_EMPTY;
    e->table = LISP_EMPTY;  // set before the nested alloc below can trigger a GC
    // The top-level frame (no parent) gathers every primitive/prelude/global
    // define -- hundreds of bindings looked up on every global reference -- so
    // back it with a hash table for O(1) lookup. Per-call frames are tiny and
    // stay assoc lists (no per-call hash allocation).
    if (parent == LISP_EMPTY) {
        lisp_value t = lisp_make_vector(GLOBAL_ENV_BUCKETS, LISP_EMPTY);
        if (t != LISP_UNDEF)
            e->table = t;  // non-moving GC: e stays valid across the alloc; OOM -> assoc list
    }
    return lisp_from_obj(e);
}

// Find the (sym . val) binding cell for `sym` in this single frame, or LISP_EMPTY.
// Both the binding key and `sym` are interned symbols (every symbol-construction
// path goes through intern()), so equality is pointer identity -- no need to
// compare names. This is the interpreter's hottest loop (profiling: variable
// lookup dominates), so a general name compare's length+memcmp per non-matching
// binding is pure waste and is avoided. A table-backed (top-level) frame scans
// only one hash bucket; a plain frame scans its (short) assoc list.
static lisp_value frame_find(lisp_value env, lisp_value sym) {
    lisp_env_t *e = (lisp_env_t *)lisp_obj(env);
    lisp_value chain = e->table != LISP_EMPTY
                           ? lisp_vector_ref(e->table, env_bucket(e->table, sym))
                           : e->bindings;
    while (lisp_is_pair(chain)) {
        lisp_value cell = lisp_car(chain);
        if (lisp_is_pair(cell) && lisp_car(cell) == sym)
            return cell;
        chain = lisp_cdr(chain);
    }
    return LISP_EMPTY;
}

void lisp_env_define(lisp_value env, lisp_value sym, lisp_value val) {
    lisp_value cell = frame_find(env, sym);
    if (lisp_is_pair(cell)) {
        set_cdr(cell, val);  // redefining in the same frame overwrites
        return;
    }
    lisp_env_t *e = (lisp_env_t *)lisp_obj(env);
    lisp_value entry = lisp_cons(sym, val);  // (sym . val); held live across the next cons
    if (e->table != LISP_EMPTY) {
        size_t b = env_bucket(e->table, sym);
        // Prepend the entry to bucket b. The table is evaluator-owned plumbing, so
        // its slot is mutated in place (the language never sees this vector).
        ((lisp_vector *)lisp_obj(e->table))->items[b] =
            lisp_cons(entry, lisp_vector_ref(e->table, b));
    } else {
        e->bindings = lisp_cons(entry, e->bindings);
    }
}

bool lisp_env_lookup(lisp_value env, lisp_value sym, lisp_value *out) {
    while (lisp_is_objtype(env, LISP_OBJ_ENV)) {
        lisp_value cell = frame_find(env, sym);
        if (lisp_is_pair(cell)) {
            if (out != NULL)
                *out = lisp_cdr(cell);
            return true;
        }
        env = ((lisp_env_t *)lisp_obj(env))->parent;
    }
    return false;
}

bool lisp_env_set(lisp_value env, lisp_value sym, lisp_value val) {
    while (lisp_is_objtype(env, LISP_OBJ_ENV)) {
        lisp_value cell = frame_find(env, sym);
        if (lisp_is_pair(cell)) {
            set_cdr(cell, val);
            return true;
        }
        env = ((lisp_env_t *)lisp_obj(env))->parent;
    }
    return false;
}

// --- Keyword matching by name -----------------------------------------------

// --- Context construction + public API --------------------------------------

lisp_value lisp_caps_copy_sys(lisp_value l) {
    if (!lisp_is_pair(l))
        return l;  // empty tail
    lisp_value rest = lisp_caps_copy_sys(lisp_cdr(l));
    if (rest == LISP_UNDEF)
        return LISP_UNDEF;  // OOM propagates
    return lisp_cons(lisp_car(l), rest);
}

static lisp_value ctx_alloc(lisp_value expr, lisp_value env) {
    // The context object lives in the shared system heap: it is referenced across
    // contexts (a scheduler queue, a send target handle) and outlives its own
    // working heap, so it must not live in any per-context heap.
    lisp_ctx_t *cx = (lisp_ctx_t *)lisp_gc_alloc_shared(sizeof(lisp_ctx_t));
    if (cx == NULL)
        return LISP_UNDEF;
    cx->h.header = LISP_MK_HEADER(LISP_OBJ_CTX, 0);
    cx->control = expr;
    cx->env = env;
    cx->accum = LISP_UNDEF;
    cx->kont = LISP_EMPTY;
    cx->mailbox = LISP_EMPTY;
    cx->caps = LISP_UNDEF;  // unrestricted by default; spawn-restricted narrows it
    cx->status = LISP_CTX_EVAL;
    cx->blocked = 0;
    cx->err = NULL;
    cx->budget = 0;
    cx->heap = NULL;  // uses the system heap unless given its own (K3)
    cx->vm = NULL;    // bytecode VM state, lazily prepared on first resume
    return lisp_from_obj(cx);
}

lisp_value lisp_ctx_make(lisp_value expr, lisp_value env) { return ctx_alloc(expr, env); }

lisp_ctx_status lisp_ctx_state(lisp_value ctxv) {
    return (lisp_ctx_status)((lisp_ctx_t *)lisp_obj(ctxv))->status;
}

// DIAGNOSTIC: 1 if the context is parked waiting for a message, else 0.
int lisp_ctx_is_blocked(lisp_value ctxv) {
    return (int)((lisp_ctx_t *)lisp_obj(ctxv))->blocked;
}

// Mark a parked context runnable again. Deliberately a SINGLE word write (no
// allocation, no lock) so a native interrupt handler can call it to wake a
// context parked on a hardware event -- the ISR -> wake-context bridge.
void lisp_ctx_wake(lisp_value ctxv) { ((lisp_ctx_t *)lisp_obj(ctxv))->blocked = 0; }

// Park a context: skip it in the scheduler and yield at the next safe point.
void lisp_ctx_block(lisp_value ctxv) {
    lisp_ctx_t *cx = (lisp_ctx_t *)lisp_obj(ctxv);
    cx->blocked = 1;
    cx->budget = 0;  // suspend at the next safe point
}

int lisp_ctx_attach_heap(lisp_value ctxv) {
    lisp_ctx_t *cx = (lisp_ctx_t *)lisp_obj(ctxv);
    if (cx->heap != NULL)
        return 0;  // already has one
    cx->heap = lisp_heap_new(ctxv);
    return cx->heap != NULL ? 0 : -1;
}

size_t lisp_ctx_heap_live(lisp_value ctxv) {
    lisp_ctx_t *cx = (lisp_ctx_t *)lisp_obj(ctxv);
    return cx->heap != NULL ? lisp_heap_live(cx->heap) : 0;
}

size_t lisp_ctx_heap_collections(lisp_value ctxv) {
    lisp_ctx_t *cx = (lisp_ctx_t *)lisp_obj(ctxv);
    return cx->heap != NULL ? lisp_heap_collections(cx->heap) : 0;
}

lisp_ctx_status lisp_ctx_resume(lisp_value ctxv, int64_t budget) {
    lisp_ctx_t *cx = (lisp_ctx_t *)lisp_obj(ctxv);
    cx->budget = budget;
    if (cx->status == LISP_CTX_DONE || cx->status == LISP_CTX_ERROR)
        return (lisp_ctx_status)cx->status;
    // The bytecode VM is the evaluator: compile the context's expr on first resume
    // (a form it cannot lower made lbc_ctx_prepare set the context's error), then
    // run it for `budget` reductions.
    if (lbc_ctx_prepare(cx) == 1)
        return lbc_ctx_run(cx);
    return LISP_CTX_ERROR;
}

lisp_value lisp_ctx_value(lisp_value ctxv) { return ((lisp_ctx_t *)lisp_obj(ctxv))->accum; }

// The capability set of a context: LISP_UNDEF (unrestricted) or a list of the
// module-name symbols it may import. Read-only; set via lisp_ctx_set_caps.
lisp_value lisp_ctx_caps(lisp_value ctxv) { return ((lisp_ctx_t *)lisp_obj(ctxv))->caps; }

// The control register: the expression a context (in the EVAL state) is about to
// evaluate. The sys-debug reflective capability surfaces this so a Lisp debugger
// can show what a single-stepped context is doing next.
lisp_value lisp_ctx_control(lisp_value ctxv) {
    return ((lisp_ctx_t *)lisp_obj(ctxv))->control;
}

// Set a context's capability set. `caps` is LISP_UNDEF (unrestricted), LISP_EMPTY
// (no import authority), or a list of interned module-name symbols. The list
// spine is rebuilt in the system heap so it can never dangle when a per-context
// heap is collected (symbols are interned there already). Returns 0, or -1 on
// OOM (leaving the prior caps in place). The embedder uses this to launch a
// sandboxed context (e.g. a non-root serial REPL) from C.
int lisp_ctx_set_caps(lisp_value ctxv, lisp_value caps) {
    lisp_ctx_t *cx = (lisp_ctx_t *)lisp_obj(ctxv);
    if (!lisp_is_pair(caps)) {  // LISP_UNDEF or LISP_EMPTY: store as-is
        cx->caps = caps;
        return 0;
    }
    lisp_heap_t *prev = lisp_gc_set_alloc_heap(lisp_gc_system_heap());
    lisp_value copy = lisp_caps_copy_sys(caps);
    lisp_gc_set_alloc_heap(prev);
    if (copy == LISP_UNDEF) {
        // Fail CLOSED: grant no authority rather than leaving the context root
        // (its caps may still be the UNDEF default). A caller that ignores the -1
        // then gets a context that can import nothing, never an escalation.
        cx->caps = LISP_EMPTY;
        return -1;
    }
    cx->caps = copy;
    return 0;
}

const char *lisp_ctx_error(lisp_value ctxv) { return ((lisp_ctx_t *)lisp_obj(ctxv))->err; }

lisp_value lisp_eval(lisp_value expr, lisp_value env, const char **err) {
    if (err != NULL)
        *err = NULL;
    lisp_value ctxv = ctx_alloc(expr, env);
    if (ctxv == LISP_UNDEF)
        return fail(err, "out of memory");
    lisp_ctx_t *cx = (lisp_ctx_t *)lisp_obj(ctxv);
    if (lbc_ctx_prepare(cx) != 1)
        return fail(err, cx->err);  // a form the compiler could not lower
    lisp_ctx_status st;
    do {
        cx->budget = CTX_BUDGET_UNBOUNDED;
        st = lbc_ctx_run(cx);
    } while (st == LISP_CTX_SUSPENDED);
    lisp_value r = (st == LISP_CTX_ERROR) ? fail(err, cx->err) : cx->accum;
    lbc_ctx_free(cx);  // transient context: reclaim its VM
    return r;
}

lisp_value lisp_apply(lisp_value proc, lisp_value *args, int argc, const char **err) {
    if (err != NULL)
        *err = NULL;
    const char *e = NULL;
    lisp_value r = lbc_apply(proc, args, argc, &e);
    return (e != NULL) ? fail(err, e) : r;
}

// Like lisp_apply, but runs in a caller-owned context reused across calls. A
// higher-order primitive that applies a procedure to every element of a list
// (map/for-each) allocates ONE context (lisp_ctx_make(LISP_UNDEF, LISP_EMPTY))
// and reuses it here, rather than allocating a fresh context per element. The
// caller must keep `ctxv` reachable (a GC root) for the whole loop.
lisp_value lisp_apply_reuse(lisp_value ctxv, lisp_value proc, lisp_value *args,
                            int argc, const char **err) {
    if (err != NULL)
        *err = NULL;
    const char *e = NULL;
    lisp_value r = lbc_apply_reuse(ctxv, proc, args, argc, &e);
    return (e != NULL) ? fail(err, e) : r;
}

lisp_value lisp_default_env(void) {
    lisp_value env = lisp_make_env(LISP_EMPTY);
    if (env != LISP_UNDEF) {
        lisp_install_primitives(env);
        lisp_load_prelude(env);     // standard library, defined in Scheme
    }
    return env;
}

lisp_value lisp_eval_string(const char *src, lisp_value env, const char **err) {
    const char *cur = src;
    const char *end = src + strlen(src);
    lisp_value result = LISP_UNDEF;
    for (;;) {
        lisp_value form = lisp_read(&cur, end, err);
        if (form == LISP_EOF)
            return result;
        if (form == LISP_UNDEF)
            return LISP_UNDEF;  // reader error, *err already set
        result = lisp_eval(form, env, err);
        if (result == LISP_UNDEF && err != NULL && *err != NULL)
            return LISP_UNDEF;
    }
}
