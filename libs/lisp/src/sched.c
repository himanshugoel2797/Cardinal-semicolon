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

// The active scheduler and the context currently being resumed -- PER-CORE, so
// two cores each running their own scheduler loop don't stomp each other's
// "current". Indexed by lisp_rt_core() (slot 0 single-threaded). g_sched defaults
// to NULL ("no scheduler"); g_current defaults to 0 ("none"), which the accessor
// maps to LISP_EMPTY (a real context is a non-zero pointer, LISP_EMPTY is not 0,
// so 0 is unambiguously the never-set state).
static lisp_sched_t *g_sched[LISP_MAX_CORES] = {0};
static lisp_value g_current[LISP_MAX_CORES] = {0};

static lisp_sched_t *cur_sched(void) { return g_sched[lisp_rt_core()]; }
static void set_sched(lisp_sched_t *s) { g_sched[lisp_rt_core()] = s; }
static void set_current(lisp_value v) { g_current[lisp_rt_core()] = v; }

static lisp_ctx_t *as_ctx(lisp_value v) { return (lisp_ctx_t *)lisp_obj(v); }

// The context currently being resumed (LISP_EMPTY when none). A primitive uses
// this to refer to "self" -- e.g. to register the running context as the waiter
// for a hardware event before parking it.
lisp_value lisp_current_ctx(void) {
    lisp_value v = g_current[lisp_rt_core()];
    return v == 0 ? LISP_EMPTY : v;
}

// This core's scheduler run queue (the list of live context values), or
// LISP_EMPTY if no scheduler is current. Read-only -- sys-debug's (ctx-list)
// exposes it so a debugger can enumerate and inspect the live contexts.
lisp_value lisp_sched_queue(void) {
    lisp_sched_t *s = cur_sched();
    return s != NULL ? s->queue : LISP_EMPTY;
}

// Swap this core's current-context slot, returning the previous value. sys-debug's
// ctx-step brackets a nested run with this so the stepped context is "self" for
// any scheduler primitives it invokes (yield/capabilities/spawn/recv), instead of
// the debugger that is driving it. (LISP_EMPTY stored as 0, matching set_current.)
lisp_value lisp_sched_swap_current(lisp_value v) {
    int core = lisp_rt_core();
    lisp_value prev = g_current[core];
    g_current[core] = (v == LISP_EMPTY) ? 0 : v;
    return prev == 0 ? LISP_EMPTY : prev;
}

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
        case LISP_OBJ_CTX:
            // A context handle is a shared REFERENCE -- an actor identity, like an
            // Erlang PID -- not data, so it is passed by IDENTITY (never copied),
            // like an interned symbol. This is what lets a client hand a server its
            // own handle for callbacks: the power/storage/network fan-out reply
            // path sends to the registered context. Copying it would hand the
            // server a dead duplicate that no scheduler ever runs (the bug this
            // case fixes). GC-safe because ctx_alloc allocates the context in the
            // system heap, which is EXTERNAL to every per-context collector: a
            // per-context mark/sweep only ever frees objects in its own heap's
            // object set (gc.c mark_push gates on set_contains), so a system-heap
            // context a mailbox points at is never swept out from under the handle
            // -- regardless of whether the system heap is frozen (multicore) yet.
            return v;
        case LISP_OBJ_STRING:
            return lisp_make_string(lisp_string_data(v), lisp_string_len(v));
        case LISP_OBJ_FLONUM:
            return lisp_make_flonum(lisp_flonum_val(v));
        case LISP_OBJ_BYTES: {
            // Snapshot the bytes into a fresh OWNED buffer in the receiver's heap
            // (shared-nothing). A foreign/MMIO region thus sends its current
            // contents, not the mapping -- device handles are not shared.
            size_t bn = lisp_bytes_len(v);
            lisp_value out = lisp_make_bytes(bn);
            if (out == LISP_UNDEF)
                return prim_err(err, "send: out of memory");
            memcpy(lisp_bytes_data(out), lisp_bytes_data(v), bn);
            return out;
        }
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
    lisp_sched_t *s = cur_sched();
    if (s == NULL)
        return prim_err(e, "spawn: no scheduler is running");
    if (n != 1)
        return prim_err(e, "spawn expects one argument (a thunk)");
    lisp_value proc = a[0];
    if (!lisp_is_procedure(proc))
        return prim_err(e, "spawn: argument must be a procedure");
    // Build the new context's initial expression (proc) in the SYSTEM heap: it
    // becomes cx->control of a system-heap context object, so it must not live in
    // the spawner's own heap (which the spawner's GC would later reclaim under it).
    lisp_heap_t *prev = lisp_gc_set_alloc_heap(lisp_gc_system_heap());
    lisp_value expr = lisp_cons(proc, LISP_EMPTY);  // the application (proc)
    lisp_gc_set_alloc_heap(prev);
    if (expr == LISP_UNDEF)
        return prim_err(e, "spawn: out of memory");
    // The closure carries its own defining env; a fresh empty env suffices for
    // evaluating the (already-value) operator position. The ctx object itself is
    // allocated in the system heap by lisp_ctx_make.
    lisp_value ctx = lisp_ctx_make(expr, LISP_EMPTY);
    if (ctx == LISP_UNDEF)
        return prim_err(e, "spawn: out of memory");
    // Propagate the spawner's restriction: a child cannot exceed its parent's
    // authority. A root (unrestricted) spawner yields a root child (caps==UNDEF,
    // as ctx_alloc set); a restricted spawner's child INHERITS the same grant.
    // Without this, a sandbox could escalate by spawning an unrestricted worker.
    // The parent's caps list already lives in the system heap (immutable), so the
    // child safely shares the pointer -- no copy. Use spawn-restricted to NARROW.
    lisp_value self = lisp_current_ctx();
    if (self != LISP_EMPTY)
        as_ctx(ctx)->caps = as_ctx(self)->caps;
    // Attach the own heap BEFORE enqueueing, so a failure never leaves a context
    // running in the wrong heap or an un-enqueued handle in the caller's hands.
    if (s->per_context_heaps && lisp_ctx_attach_heap(ctx) != 0)
        return prim_err(e, "spawn: out of memory");
    if (!lisp_sched_add(s, ctx))  // never return an un-enqueued handle
        return prim_err(e, "spawn: out of memory");
    return ctx;
}

// Is `sym` present in capability list `caps`? Capability symbols are interned
// (read or copied from interned), so identity compare suffices.
static bool cap_in_list(lisp_value sym, lisp_value caps) {
    for (lisp_value l = caps; lisp_is_pair(l); l = lisp_cdr(l))
        if (lisp_car(l) == sym)
            return true;
    return false;
}

// (spawn-restricted caps thunk) -- like spawn, but the new context is RESTRICTED:
// it may (import ...) only the modules named in `caps` (a list of symbols), only
// ones already loaded, and may not define-module. This is the W7 grant: authority
// travels with the spawned computation, and you cannot grant what you lack --
// every requested capability must be in the spawner's own set (an unrestricted /
// root spawner may grant anything). The grant is the capability: a context's
// reach is exactly the modules it was handed.
static lisp_value prim_spawn_restricted(lisp_value *a, int n, const char **e) {
    lisp_sched_t *s = cur_sched();
    if (s == NULL)
        return prim_err(e, "spawn-restricted: no scheduler is running");
    if (n != 2)
        return prim_err(e, "spawn-restricted expects (caps thunk)");
    lisp_value caps = a[0];
    lisp_value proc = a[1];
    if (caps != LISP_EMPTY && !lisp_is_pair(caps))
        return prim_err(e, "spawn-restricted: caps must be a list of module names");
    if (!lisp_is_procedure(proc))
        return prim_err(e, "spawn-restricted: second argument must be a procedure");
    for (lisp_value l = caps; lisp_is_pair(l); l = lisp_cdr(l))
        if (!lisp_is_symbol(lisp_car(l)))
            return prim_err(e, "spawn-restricted: caps must be module-name symbols");
    // No escalation: if the spawner is itself restricted, every requested cap
    // must be in its set. An unrestricted (caps==UNDEF) spawner may grant any.
    lisp_value self = lisp_current_ctx();
    if (self != LISP_EMPTY) {
        lisp_value mine = as_ctx(self)->caps;
        if (mine != LISP_UNDEF)
            for (lisp_value l = caps; lisp_is_pair(l); l = lisp_cdr(l))
                if (!cap_in_list(lisp_car(l), mine))
                    return prim_err(e, "spawn-restricted: cannot grant a capability you lack");
    }
    // The context object lives in the system heap, so both its initial expression
    // and its caps spine must too (never the spawner's own heap -- its GC would
    // reclaim them under the child). Symbols are interned, so only the spine is
    // copied. Build both under the system heap, then restore.
    lisp_heap_t *prev = lisp_gc_set_alloc_heap(lisp_gc_system_heap());
    lisp_value expr = lisp_cons(proc, LISP_EMPTY);
    lisp_value capcopy = (expr == LISP_UNDEF) ? LISP_UNDEF : lisp_caps_copy_sys(caps);
    lisp_gc_set_alloc_heap(prev);
    if (expr == LISP_UNDEF || capcopy == LISP_UNDEF)
        return prim_err(e, "spawn-restricted: out of memory");
    lisp_value ctx = lisp_ctx_make(expr, LISP_EMPTY);
    if (ctx == LISP_UNDEF)
        return prim_err(e, "spawn-restricted: out of memory");
    as_ctx(ctx)->caps = capcopy;
    if (s->per_context_heaps && lisp_ctx_attach_heap(ctx) != 0)
        return prim_err(e, "spawn-restricted: out of memory");
    if (!lisp_sched_add(s, ctx))
        return prim_err(e, "spawn-restricted: out of memory");
    return ctx;
}

// (capabilities) -- the running context's grant: #t if unrestricted (root),
// else the list of module-name symbols it may import. Lets code (a REPL, a test)
// see its own authority.
static lisp_value prim_capabilities(lisp_value *a, int n, const char **e) {
    (void)a;
    (void)n;
    (void)e;
    lisp_value self = lisp_current_ctx();
    if (self == LISP_EMPTY)
        return LISP_TRUE;  // boot / no scheduler == root
    lisp_value c = as_ctx(self)->caps;
    return c == LISP_UNDEF ? LISP_TRUE : c;
}

// (yield) -- give up the rest of this slice; resume right after the call.
static lisp_value prim_yield(lisp_value *a, int n, const char **e) {
    (void)a;
    (void)n;
    lisp_value self = lisp_current_ctx();
    if (self == LISP_EMPTY)
        return prim_err(e, "yield: not running under a scheduler");
    as_ctx(self)->budget = 0;  // suspend at the next safe point
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
    lisp_ctx_t *tcx = as_ctx(target);
    // Copy the message and enqueue it INTO THE RECEIVER's heap, so the receiver
    // owns its messages and no SENDER-heap pointer survives across the boundary
    // (shared-nothing). A receiver with no own heap uses the system heap -- never
    // leave the copy in the sender's heap, or the sender's GC would free it under
    // the receiver. Always switch (and always restore, including the error path).
    lisp_heap_t *dst = (tcx->heap != NULL) ? tcx->heap : lisp_gc_system_heap();
    lisp_heap_t *prev = lisp_gc_set_alloc_heap(dst);
    lisp_value copy = deep_copy(a[1], 0, e);
    bool ok = !(copy == LISP_UNDEF && *e != NULL) && mailbox_push(tcx, copy, e);
    lisp_gc_set_alloc_heap(prev);
    if (!ok)
        return LISP_UNDEF;  // deep_copy or mailbox_push set *e
    tcx->blocked = 0;  // a waiting receiver becomes runnable
    return LISP_UNDEF;
}

// (%mailbox-empty?) -- #t if the current context has no pending messages.
static lisp_value prim_mailbox_empty(lisp_value *a, int n, const char **e) {
    (void)a;
    (void)n;
    lisp_value self = lisp_current_ctx();
    if (self == LISP_EMPTY)
        return prim_err(e, "recv: not running under a scheduler");
    return lisp_is_pair(as_ctx(self)->mailbox) ? LISP_FALSE : LISP_TRUE;
}

// (%mailbox-pop) -- remove and return the oldest message of the current context.
static lisp_value prim_mailbox_pop(lisp_value *a, int n, const char **e) {
    (void)a;
    (void)n;
    lisp_value self = lisp_current_ctx();
    if (self == LISP_EMPTY)
        return prim_err(e, "recv: not running under a scheduler");
    lisp_ctx_t *cx = as_ctx(self);
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
    lisp_value self = lisp_current_ctx();
    if (self == LISP_EMPTY)
        return prim_err(e, "recv: not running under a scheduler");
    lisp_ctx_t *cx = as_ctx(self);
    cx->blocked = 1;
    cx->budget = 0;
    return LISP_UNDEF;
}

// (self) -- the running context's own handle, or #f outside the scheduler. The
// actor model's self(): lets a context hand its identity to another -- a client
// passing its reply target into a request, or a service passing itself so a
// callee can call back (the storage probe/claim and read-completion paths). The
// handle is a shared system-heap reference, so it survives `send`'s copy intact.
static lisp_value prim_self(lisp_value *a, int n, const char **e) {
    (void)a;
    (void)n;
    (void)e;
    lisp_value self = lisp_current_ctx();
    return self == LISP_EMPTY ? LISP_FALSE : self;
}

// --- Installation -----------------------------------------------------------

static void def(lisp_value env, const char *name, lisp_primitive_fn fn) {
    lisp_value sym = lisp_make_symbol(name, strlen(name));
    lisp_value prim = lisp_make_primitive(fn, name);
    lisp_env_define(env, sym, prim);
}

void lisp_install_sched(lisp_value env) {
    def(env, "spawn", prim_spawn);
    def(env, "spawn-restricted", prim_spawn_restricted);
    def(env, "capabilities", prim_capabilities);
    def(env, "yield", prim_yield);
    def(env, "send", prim_send);
    def(env, "self", prim_self);
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
    s->per_context_heaps = 0;
    set_sched(s);
}

bool lisp_sched_add(lisp_sched_t *s, lisp_value ctx) {
    // The run queue is a system-level structure: its spine must live in the shared
    // heap even when spawn is called from within a per-context-heap context (a
    // context's precise GC would otherwise free a cons it cannot see as a root).
    lisp_heap_t *prev = lisp_gc_set_alloc_heap(lisp_gc_system_heap());
    lisp_value cell = lisp_cons(ctx, LISP_EMPTY);  // ctx stays rooted (caller's C local)
    lisp_gc_set_alloc_heap(prev);
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
    set_sched(s);
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
                    set_current(cxv);
                    lisp_ctx_resume(cxv, s->slice);  // runs until budget/yield/done/error
                    set_current(LISP_EMPTY);
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
    // `s` is typically a caller stack local; do not leave this core's g_sched
    // dangling past the run. A later spawn/send without a fresh lisp_sched_init is
    // then a clean "no scheduler" error rather than a use-after-stack-free.
    set_sched(NULL);
    return passes;
}
