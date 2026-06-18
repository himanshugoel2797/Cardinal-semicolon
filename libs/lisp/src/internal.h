// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

// Cross-translation-unit internals shared between the runtime .c files but not
// part of the public API: the runtime-plumbing object layouts (env/closure/
// primitive), the GC allocator, and the intern-table root accessor.

#ifndef CARDINAL_LISP_INTERNAL_H
#define CARDINAL_LISP_INTERNAL_H

#include <stddef.h>

#include "lisp.h"

// Lexical environment frame (see eval.c). Deliberately mutable runtime plumbing.
typedef struct {
    lisp_header h;
    lisp_value parent;    // enclosing env, or LISP_EMPTY at the top level
    lisp_value bindings;  // assoc list ((sym . val) ...), mutated in place
} lisp_env_t;

// A lambda: parameter list + body + captured definition environment.
typedef struct {
    lisp_header h;
    lisp_value params;
    lisp_value body;
    lisp_value env;
} lisp_closure_t;

// A built-in procedure backed by a C function.
typedef struct {
    lisp_header h;
    lisp_primitive_fn fn;
    lisp_value name;
} lisp_prim_t;

// A continuation frame for the explicit-stack (CEK) evaluator. The frames form a
// heap-linked chain (`next`); a frame's *kind* lives in the header aux byte and
// selects how `a`/`b`/`c` are interpreted (see the K_* enum in eval.c). `env` is
// the environment to restore when this frame resumes. Frames are evaluator-owned
// plumbing (never user-visible) and some are mutated in place while iterating.
typedef struct {
    lisp_header h;
    lisp_value next;  // enclosing frame, or LISP_EMPTY at the bottom
    lisp_value env;
    lisp_value a, b, c;
} lisp_kont_t;

// An execution context: the CEK machine state made explicit and heap-resident so
// it can be suspended/resumed and precisely traced. `status` is a lisp_ctx_status
// (EVAL/APPLY/DONE/ERROR); `err` is a static string when status==ERROR; `budget`
// is the reductions remaining in the current slice. The four lisp_value fields
// are the machine registers and are the precise GC roots of a suspended context.
typedef struct {
    lisp_header h;
    lisp_value control;  // expression under evaluation (when status==EVAL)
    lisp_value env;
    lisp_value accum;    // value being returned (when status==APPLY)
    lisp_value kont;     // top continuation frame, or LISP_EMPTY
    lisp_value mailbox;  // FIFO list of received messages (K2 scheduler / IPC)
    uint32_t status;
    uint32_t blocked;    // 1 = parked waiting for a message; the scheduler skips it
    const char *err;
    int64_t budget;
    lisp_heap_t *heap;   // own per-context heap (K3), or NULL to use the system heap
} lisp_ctx_t;

// GC-tracked allocation (gc.c). All heap Lisp objects are allocated through this
// so the collector can find and reclaim them; it targets the current heap (the
// system heap by default, or a context's own heap while it runs). Returns NULL on
// OOM. lisp_gc_alloc_shared forces the system heap (for interned symbols, which
// are the shared-immutable region and must not live in a per-context heap).
void *lisp_gc_alloc(size_t size);
void *lisp_gc_alloc_shared(size_t size);

// Make `h` the current allocation target, returning the previous one (so the
// interpreter loop can switch into a context's heap while it runs and restore
// afterwards). lisp_heap_wants_gc reports a deferred per-context collection that
// the loop should run at its next safe point.
lisp_heap_t *lisp_gc_set_alloc_heap(lisp_heap_t *h);
lisp_heap_t *lisp_gc_system_heap(void);
int lisp_heap_wants_gc(lisp_heap_t *h);

// The intern table, exposed so the GC can treat interned symbols as roots
// (intern.c). Returns the slot array; *cap_out is its length. Slots are 0
// (empty) or an interned symbol/keyword value.
lisp_value *lisp_intern_table(size_t *cap_out);

#endif
