// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

// sys-debug: a reflective debugging capability, written against the CEK machine's
// own suspend/resume. Because a context IS its heap-resident execution state, a
// debugger needs almost nothing new from the interpreter -- just a handle to a
// paused context and a way to advance it one reduction at a time and read its
// registers. The debugger LOGIC (breakpoints, stepping, stack display) is then
// ordinary Lisp on top of these primitives.
//
//   (ctx-make thunk)   -> a PAUSED context that will apply `thunk` (a 0-arg
//                         procedure). The thunk is a closure, so it carries its
//                         own definition environment -- that is how the stepped
//                         code sees the surrounding lexical scope (the same trick
//                         spawn uses). The context is NOT on any scheduler queue;
//                         only ctx-step drives it.
//   (ctx-step c [n])   -> run up to n reductions (default 1), returning the new
//                         status symbol (eval/apply/done/error/suspended).
//   (ctx-status c)     -> the context's stored status symbol.
//   (ctx-control c)    -> the expression it is about to evaluate (EVAL state).
//   (ctx-value c)      -> its result once done.
//   (ctx-error c)      -> its error message string, or #f.
//
// These are POWERFUL -- driving or reading another context is exactly the
// authority a sandbox must not have over the system -- so they are exposed as a
// gated module: only a context granted `sys-debug` can (import sys-debug). Step
// only contexts you made with ctx-make; stepping a scheduler-owned context would
// race the scheduler.

#include <string.h>

#include "internal.h"  // lisp_ctx_t, lisp_caps_copy_sys is unrelated; ctx layout
#include "lisp.h"

static lisp_value derr(const char **err, const char *msg) {
    if (err != NULL)
        *err = msg;
    return LISP_UNDEF;
}

// Map a context status to its reader-faithful symbol.
static lisp_value status_symbol(lisp_ctx_status s) {
    const char *name;
    switch (s) {
        case LISP_CTX_EVAL: name = "eval"; break;
        case LISP_CTX_APPLY: name = "apply"; break;
        case LISP_CTX_DONE: name = "done"; break;
        case LISP_CTX_ERROR: name = "error"; break;
        case LISP_CTX_SUSPENDED: name = "suspended"; break;
        default: name = "unknown"; break;
    }
    return lisp_make_symbol(name, strlen(name));
}

// (ctx-make thunk) -> a paused context that will apply the 0-arg procedure.
static lisp_value prim_ctx_make(lisp_value *a, int n, const char **e) {
    if (n != 1 ||
        (!lisp_is_objtype(a[0], LISP_OBJ_CLOSURE) &&
         !lisp_is_objtype(a[0], LISP_OBJ_PRIMITIVE)))
        return derr(e, "ctx-make expects one argument (a procedure)");
    // Build the application (thunk) in the SYSTEM heap: it becomes the control of
    // a system-heap context object and must not live in the caller's own heap
    // (which the caller's GC would later reclaim under it). Mirrors prim_spawn.
    lisp_heap_t *prev = lisp_gc_set_alloc_heap(lisp_gc_system_heap());
    lisp_value expr = lisp_cons(a[0], LISP_EMPTY);
    lisp_gc_set_alloc_heap(prev);
    if (expr == LISP_UNDEF)
        return derr(e, "ctx-make: out of memory");
    // A fresh empty env suffices: the operator position is already a value (the
    // closure), and the closure carries its own defining environment.
    lisp_value ctx = lisp_ctx_make(expr, LISP_EMPTY);
    if (ctx == LISP_UNDEF)
        return derr(e, "ctx-make: out of memory");
    // Give the context its OWN heap. Without it the context (heap==NULL) would
    // allocate its stepped working data into whatever heap is current at
    // ctx-step time -- i.e. the DEBUGGER's per-context heap -- where it is not a
    // GC root (a heap roots only its owner's registers), so the debugger's next
    // collection would reclaim the stepped context's live cons/kont cells under
    // it: a cross-heap use-after-free. Its own heap is precisely rooted from its
    // own registers. Mirrors prim_spawn.
    if (lisp_ctx_attach_heap(ctx) != 0)
        return derr(e, "ctx-make: out of memory");
    // Inherit the maker's capability grant: a stepped context must not wield more
    // authority than whoever made it (the same no-escalation rule as spawn). The
    // grant list already lives in the system heap, so share the pointer.
    lisp_value self = lisp_current_ctx();
    if (self != LISP_EMPTY)
        ((lisp_ctx_t *)lisp_obj(ctx))->caps = ((lisp_ctx_t *)lisp_obj(self))->caps;
    return ctx;
}

// (ctx-step c [n]) -> run up to n (default 1) reductions; return the RESUME
// result symbol: done / error, or `suspended` when the budget ran out mid-
// computation (the context is still runnable -- step again). Note this differs
// from (ctx-status c), which reports the stored register state (eval/apply/done/
// error) and so never says `suspended`.
static lisp_value prim_ctx_step(lisp_value *a, int n, const char **e) {
    if (n < 1 || n > 2 || !lisp_is_objtype(a[0], LISP_OBJ_CTX))
        return derr(e, "ctx-step expects (context [count])");
    int64_t steps = 1;
    if (n == 2) {
        if (!lisp_is_fixnum(a[1]) || lisp_fixnum_val(a[1]) < 1)
            return derr(e, "ctx-step: count must be a positive integer");
        steps = lisp_fixnum_val(a[1]);
    }
    // Run the target AS the current context so any scheduler primitive it invokes
    // (yield/capabilities/spawn/recv) sees the target as self, not this debugger.
    lisp_value prev = lisp_sched_swap_current(a[0]);
    lisp_ctx_status r = lisp_ctx_resume(a[0], steps);
    lisp_sched_swap_current(prev);
    return status_symbol(r);
}

static lisp_value prim_ctx_status(lisp_value *a, int n, const char **e) {
    if (n != 1 || !lisp_is_objtype(a[0], LISP_OBJ_CTX))
        return derr(e, "ctx-status expects a context");
    return status_symbol(lisp_ctx_state(a[0]));
}

static lisp_value prim_ctx_control(lisp_value *a, int n, const char **e) {
    if (n != 1 || !lisp_is_objtype(a[0], LISP_OBJ_CTX))
        return derr(e, "ctx-control expects a context");
    return lisp_ctx_control(a[0]);
}

static lisp_value prim_ctx_value(lisp_value *a, int n, const char **e) {
    if (n != 1 || !lisp_is_objtype(a[0], LISP_OBJ_CTX))
        return derr(e, "ctx-value expects a context");
    return lisp_ctx_value(a[0]);
}

static lisp_value prim_ctx_error(lisp_value *a, int n, const char **e) {
    if (n != 1 || !lisp_is_objtype(a[0], LISP_OBJ_CTX))
        return derr(e, "ctx-error expects a context");
    const char *m = lisp_ctx_error(a[0]);
    return m != NULL ? lisp_make_string(m, strlen(m)) : LISP_FALSE;
}

// Register the sys-debug module so a context granted it can (import sys-debug).
void lisp_register_debug_module(lisp_value env) {
    static const lisp_builtin_export exports[] = {
        {"ctx-make", prim_ctx_make},     {"ctx-step", prim_ctx_step},
        {"ctx-status", prim_ctx_status}, {"ctx-control", prim_ctx_control},
        {"ctx-value", prim_ctx_value},   {"ctx-error", prim_ctx_error},
    };
    lisp_register_builtin_module(env, "sys-debug", exports,
                                 sizeof(exports) / sizeof(exports[0]));
}
