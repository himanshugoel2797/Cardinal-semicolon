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

// GC-tracked allocation (gc.c). All heap Lisp objects are allocated through this
// so the collector can find and reclaim them. Returns NULL on OOM.
void *lisp_gc_alloc(size_t size);

// The intern table, exposed so the GC can treat interned symbols as roots
// (intern.c). Returns the slot array; *cap_out is its length. Slots are 0
// (empty) or an interned symbol/keyword value.
lisp_value *lisp_intern_table(size_t *cap_out);

#endif
