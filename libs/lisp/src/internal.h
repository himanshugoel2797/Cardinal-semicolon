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

// A syntax-rules macro transformer.
typedef struct {
    lisp_header h;
    lisp_value literals;  // list of literal identifiers
    lisp_value rules;     // list of (pattern template)
    lisp_value def_env;   // definition environment (for future hygiene)
} lisp_macro_t;

lisp_value lisp_make_macro(lisp_value literals, lisp_value rules, lisp_value def_env);

// Expand one macro use: `form` is the whole (keyword . args) form. Returns the
// expansion, or LISP_UNDEF + *err if no rule matches.
lisp_value lisp_macro_expand(lisp_value macro, lisp_value form, const char **err);

// An escape continuation (call/cc). Escape-only: valid only while its call/cc is
// still on the stack (re-entrant continuations are a documented exception).
typedef struct {
    lisp_header h;
    uint64_t id;
} lisp_cont_t;

// --- Nonlocal-exit control state (control.c) --------------------------------
//
// Every nonlocal exit travels up the C stack via the existing `*err != NULL`
// propagation. The control state disambiguates the kind so catchers
// (call/cc, guard, with-exception-handler) can decide whether to catch.
#define LISP_CTL_NONE 0
#define LISP_CTL_CONT 1   // a call/cc escape is unwinding
#define LISP_CTL_RAISE 2  // an exception is unwinding

int lisp_ctl_kind(void);
lisp_value lisp_ctl_value(void);
uint64_t lisp_ctl_cont_id(void);
void lisp_ctl_clear(void);             // back to NONE (called by plain-error paths)
void lisp_ctl_set_raise(lisp_value condition);
void lisp_ctl_set_cont(uint64_t id, lisp_value value);

// Invoke an escape continuation: sets the control state + *err, returns UNDEF.
lisp_value lisp_cont_invoke(lisp_value cont, lisp_value value, const char **err);

// R7RS error objects (used by `error` and to wrap caught plain errors).
lisp_value lisp_make_error_object(lisp_value message, lisp_value irritants);
bool lisp_is_error_object(lisp_value v);

// Install call/cc, raise, error, error-object?/-message/-irritants,
// with-exception-handler, values, call-with-values.
void lisp_install_control(lisp_value env);

// GC-tracked allocation (gc.c). All heap Lisp objects are allocated through this
// so the collector can find and reclaim them. Returns NULL on OOM.
void *lisp_gc_alloc(size_t size);

// The intern table, exposed so the GC can treat interned symbols as roots
// (intern.c). Returns the slot array; *cap_out is its length. Slots are 0
// (empty) or an interned symbol/keyword value.
lisp_value *lisp_intern_table(size_t *cap_out);

#endif
