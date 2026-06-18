// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

// A cooperative round-robin scheduler over execution contexts (the CEK machines
// in eval.c). This is the language-level process model: a "process" is a context
// (an independent root environment + its execution state), scheduled as a green
// thread. Each context is resumed for a reduction slice and suspends at a safe
// point (budget exhaustion or an explicit (yield)); the scheduler then moves on.
// An infinite loop therefore cannot wedge the scheduler -- it is preempted at the
// next safe point.
//
// Isolation is shared-nothing: contexts communicate only by message passing, and
// (send target msg) DEEP-COPIES the message (copy-on-send) so no mutable
// structure is shared. The copy is cheap because values are immutable; interned
// symbols are the shared-immutable region and are not copied. (Today there is one
// global heap, so the copy is a semantic contract that the per-context-GC phase
// will come to rely on; establishing it now keeps the model honest.)
//
// Host-side there is one global scheduler and a "current context" pointer used by
// the scheduler primitives. In the kernel these become per-core state.

#include <stdint.h>
#include <string.h>

#include "internal.h"  // lisp_ctx_t internals (status/blocked/mailbox)
#include "lisp.h"

// The active scheduler and the context currently being resumed. One of each
// host-side (a single scheduler thread); per-core in the kernel later.
static lisp_sched_t *g_sched = NULL;
static lisp_value g_current = LISP_EMPTY;  // a context value, or LISP_EMPTY

static lisp_ctx_t *as_ctx(lisp_value v) { return (lisp_ctx_t *)lisp_obj(v); }

// Mutate a pair's cdr (the run queue and mailboxes are evaluator-owned plumbing,
// not user-visible data; the immutable-pair rule does not apply to them).
static void set_cdr(lisp_value pair, lisp_value v) {
    ((lisp_pair *)lisp_obj(pair))->cdr = v;
}

static lisp_value prim_err(const char **err, const char *msg) {
    if (err != NULL)
        *err = msg;
    return LISP_UNDEF;
}

// --- Copy-on-send -----------------------------------------------------------

// Bound on nesting depth: deep_copy recurses into each car / vector element, and
// the kernel task stack is small. A list SPINE is copied iteratively (so length
// is unbounded), but element nesting beyond this is rejected rather than risking
// a stack overflow -- the primitives' bounds checks stand in for the MMU here.
#define COPY_MAX_DEPTH 128

// Deep-copy an immutable value (the message contract for shared-nothing IPC).
// Atoms and immediates are values; interned symbols/keywords are the shared
// region (not copied); strings/flonums/pairs/vectors are rebuilt. Procedures,
// environments, and other runtime objects are NOT data and may not be sent.
// Immutable values are acyclic, so the bounded recursion terminates.
static lisp_value deep_copy(lisp_value v, int depth, const char **err) {
    if (depth > COPY_MAX_DEPTH)
        return prim_err(err, "send: message nested too deeply");
    if (!lisp_is_ptr(v))
        return v;  // fixnums, booleans, chars, () -- pure values
    switch (LISP_HDR_TYPE(lisp_obj(v))) {
        case LISP_OBJ_SYMBOL:
        case LISP_OBJ_KEYWORD:
            return v;  // interned: shared-immutable region, not copied
        case LISP_OBJ_STRING:
            return lisp_make_string(lisp_string_data(v), lisp_string_len(v));
        case LISP_OBJ_FLONUM:
            return lisp_make_flonum(lisp_flonum_val(v));
        case LISP_OBJ_PAIR: {
            // Copy the cdr SPINE iteratively (a long list does not grow the C
            // stack); recurse only into each car. head/tail keep the partial copy
            // rooted across every allocation.
            lisp_value head = LISP_EMPTY, tail = LISP_EMPTY;
            while (lisp_is_pair(v)) {
                lisp_value ca = deep_copy(lisp_car(v), depth + 1, err);
                if (ca == LISP_UNDEF && *err != NULL)
                    return LISP_UNDEF;
                lisp_value cell = lisp_cons(ca, LISP_EMPTY);
                if (cell == LISP_UNDEF)
                    return prim_err(err, "send: out of memory");
                if (head == LISP_EMPTY)
                    head = cell;
                else
                    set_cdr(tail, cell);
                tail = cell;
                v = lisp_cdr(v);
            }
            if (!lisp_is_empty(v)) {  // dotted tail
                lisp_value t = deep_copy(v, depth + 1, err);
                if (t == LISP_UNDEF && *err != NULL)
                    return LISP_UNDEF;
                set_cdr(tail, t);
            }
            return head;
        }
        case LISP_OBJ_VECTOR: {
            size_t n = lisp_vector_length(v);
            lisp_value out = lisp_make_vector(n, LISP_UNDEF);
            if (out == LISP_UNDEF)
                return prim_err(err, "send: out of memory");
            for (size_t i = 0; i < n; i++) {
                // `out` (with elements < i filled) and `v` stay rooted as C locals.
                lisp_value e = deep_copy(lisp_vector_ref(v, i), depth + 1, err);
                if (e == LISP_UNDEF && *err != NULL)
                    return LISP_UNDEF;
                lisp_vector_set_init(out, i, e);
            }
            return out;
        }
        default:
            return prim_err(err, "send: message must be data (not a procedure/handle)");
    }
}

// --- Mailbox ----------------------------------------------------------------

// Append `msg` to the end of a context's FIFO mailbox (in-place plumbing).
static bool mailbox_push(lisp_ctx_t *tcx, lisp_value msg, const char **err) {
    lisp_value cell = lisp_cons(msg, LISP_EMPTY);  // msg stays rooted (caller's C local)
    if (cell == LISP_UNDEF)
        return prim_err(err, "send: out of memory"), false;
    if (!lisp_is_pair(tcx->mailbox)) {
        tcx->mailbox = cell;
        return true;
    }
    lisp_value p = tcx->mailbox;
    while (lisp_is_pair(lisp_cdr(p)))
        p = lisp_cdr(p);
    set_cdr(p, cell);
    return true;
}

// --- Scheduler primitives ---------------------------------------------------

// (spawn thunk) -- create a context that evaluates (thunk), enqueue it, and
// return its handle (a context value, usable as a send target).
static lisp_value prim_spawn(lisp_value *a, int n, const char **e) {
    if (g_sched == NULL)
        return prim_err(e, "spawn: no scheduler is running");
    if (n != 1)
        return prim_err(e, "spawn expects one argument (a thunk)");
    lisp_value proc = a[0];
    if (!lisp_is_objtype(proc, LISP_OBJ_CLOSURE) && !lisp_is_objtype(proc, LISP_OBJ_PRIMITIVE))
        return prim_err(e, "spawn: argument must be a procedure");
    lisp_value expr = lisp_cons(proc, LISP_EMPTY);  // the application (proc)
    if (expr == LISP_UNDEF)
        return prim_err(e, "spawn: out of memory");
    // The closure carries its own defining env; a fresh empty env suffices for
    // evaluating the (already-value) operator position.
    lisp_value ctx = lisp_ctx_make(expr, LISP_EMPTY);
    if (ctx == LISP_UNDEF)
        return prim_err(e, "spawn: out of memory");
    if (!lisp_sched_add(g_sched, ctx))  // never return an un-enqueued handle
        return prim_err(e, "spawn: out of memory");
    return ctx;
}

// (yield) -- give up the rest of this slice; resume right after the call.
static lisp_value prim_yield(lisp_value *a, int n, const char **e) {
    (void)a;
    (void)n;
    if (g_current == LISP_EMPTY)
        return prim_err(e, "yield: not running under a scheduler");
    as_ctx(g_current)->budget = 0;  // suspend at the next safe point
    return LISP_UNDEF;
}

// (send target message) -- deep-copy the message into target's mailbox and wake
// it. Returns the (unspecified) void value.
static lisp_value prim_send(lisp_value *a, int n, const char **e) {
    if (n != 2)
        return prim_err(e, "send expects (target message)");
    lisp_value target = a[0];
    if (!lisp_is_objtype(target, LISP_OBJ_CTX))
        return prim_err(e, "send: target is not a context");
    lisp_value copy = deep_copy(a[1], 0, e);
    if (copy == LISP_UNDEF && *e != NULL)
        return LISP_UNDEF;
    lisp_ctx_t *tcx = as_ctx(target);
    if (!mailbox_push(tcx, copy, e))
        return LISP_UNDEF;
    tcx->blocked = 0;  // a waiting receiver becomes runnable
    return LISP_UNDEF;
}

// (%mailbox-empty?) -- #t if the current context has no pending messages.
static lisp_value prim_mailbox_empty(lisp_value *a, int n, const char **e) {
    (void)a;
    (void)n;
    if (g_current == LISP_EMPTY)
        return prim_err(e, "recv: not running under a scheduler");
    return lisp_is_pair(as_ctx(g_current)->mailbox) ? LISP_FALSE : LISP_TRUE;
}

// (%mailbox-pop) -- remove and return the oldest message of the current context.
static lisp_value prim_mailbox_pop(lisp_value *a, int n, const char **e) {
    (void)a;
    (void)n;
    if (g_current == LISP_EMPTY)
        return prim_err(e, "recv: not running under a scheduler");
    lisp_ctx_t *cx = as_ctx(g_current);
    if (!lisp_is_pair(cx->mailbox))
        return prim_err(e, "recv: mailbox is empty");
    lisp_value msg = lisp_car(cx->mailbox);
    cx->mailbox = lisp_cdr(cx->mailbox);
    return msg;
}

// (%block) -- park the current context until a message arrives (a send clears the
// blocked flag and wakes it). Suspends at the next safe point.
static lisp_value prim_block(lisp_value *a, int n, const char **e) {
    (void)a;
    (void)n;
    if (g_current == LISP_EMPTY)
        return prim_err(e, "recv: not running under a scheduler");
    lisp_ctx_t *cx = as_ctx(g_current);
    cx->blocked = 1;
    cx->budget = 0;
    return LISP_UNDEF;
}

// --- Installation -----------------------------------------------------------

static void def(lisp_value env, const char *name, lisp_primitive_fn fn) {
    lisp_value sym = lisp_make_symbol(name, strlen(name));
    lisp_value prim = lisp_make_primitive(fn, name);
    lisp_env_define(env, sym, prim);
}

void lisp_install_sched(lisp_value env) {
    def(env, "spawn", prim_spawn);
    def(env, "yield", prim_yield);
    def(env, "send", prim_send);
    def(env, "%mailbox-empty?", prim_mailbox_empty);
    def(env, "%mailbox-pop", prim_mailbox_pop);
    def(env, "%block", prim_block);
    // recv is blocking, which an atomic C primitive cannot express (it would have
    // to re-run on wake): define it in Lisp as a loop over the non-blocking
    // mailbox primitives so the re-check happens via ordinary tail recursion.
    const char *err = NULL;
    lisp_eval_string(
        "(define (recv) (if (%mailbox-empty?) (begin (%block) (recv)) (%mailbox-pop)))",
        env, &err);
}

// --- Scheduler driver -------------------------------------------------------

void lisp_sched_init(lisp_sched_t *s, int64_t slice) {
    s->queue = LISP_EMPTY;
    s->slice = slice > 0 ? slice : 256;
    g_sched = s;
}

bool lisp_sched_add(lisp_sched_t *s, lisp_value ctx) {
    lisp_value cell = lisp_cons(ctx, LISP_EMPTY);  // ctx stays rooted (caller's C local)
    if (cell == LISP_UNDEF)
        return false;  // OOM: the caller must not treat the context as scheduled
    if (!lisp_is_pair(s->queue)) {
        s->queue = cell;
        return true;
    }
    lisp_value p = s->queue;
    while (lisp_is_pair(lisp_cdr(p)))
        p = lisp_cdr(p);
    set_cdr(p, cell);
    return true;
}

static bool ctx_finished(lisp_ctx_t *cx) {
    return cx->status == LISP_CTX_DONE || cx->status == LISP_CTX_ERROR;
}

int lisp_sched_run(lisp_sched_t *s, int max_passes) {
    g_sched = s;
    int passes = 0;
    for (;;) {
        if (max_passes > 0 && passes >= max_passes)
            break;
        bool any_alive = false, any_runnable = false;
        // A context spawned mid-pass appends to the tail of the very list we are
        // walking, so bound the pass at the tail that exists NOW; contexts spawned
        // during the pass are then picked up on the next pass (and a spawn-loop
        // cannot starve max_passes by extending the current pass indefinitely).
        lisp_value pass_end = s->queue;
        while (lisp_is_pair(pass_end) && lisp_is_pair(lisp_cdr(pass_end)))
            pass_end = lisp_cdr(pass_end);
        for (lisp_value p = s->queue; lisp_is_pair(p); p = lisp_cdr(p)) {
            lisp_value cxv = lisp_car(p);
            lisp_ctx_t *cx = as_ctx(cxv);
            if (!ctx_finished(cx)) {
                any_alive = true;
                if (cx->blocked) {
                    /* parked waiting for a message */
                } else {
                    any_runnable = true;
                    g_current = cxv;
                    lisp_ctx_resume(cxv, s->slice);  // runs until budget/yield/done/error
                    g_current = LISP_EMPTY;
                }
            }
            if (p == pass_end)
                break;  // stop before contexts appended during this pass
        }
        passes++;
        if (!any_alive)
            break;  // every context finished
        if (!any_runnable)
            break;  // all remaining contexts are blocked -> deadlock
    }
    return passes;
}
