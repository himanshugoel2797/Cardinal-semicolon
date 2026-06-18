// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

// Core value constructors and accessors. Allocation goes through malloc for now;
// Phase 4 replaces this with a TLAB bump allocator + non-moving mark-sweep GC
// (see notes/core/lisp-substrate.md). Nothing here embeds a raw native pointer
// inside a Lisp value, so a future relocating/persistent collector stays open.

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "internal.h"
#include "lisp.h"

lisp_value lisp_cons(lisp_value car, lisp_value cdr) {
    lisp_pair *p = (lisp_pair *)lisp_gc_alloc(sizeof(lisp_pair));
    if (p == NULL)
        return LISP_UNDEF;
    p->h.header = LISP_MK_HEADER(LISP_OBJ_PAIR, 0);
    p->car = car;
    p->cdr = cdr;
    return lisp_from_obj(p);
}

// Symbols/keywords are interned (see intern.c); strings are not.
lisp_value lisp_make_string(const char *data, size_t len) {
    // Over-allocate one byte for a defensive NUL: the length in the header is
    // authoritative (strings may contain embedded NULs), but a sentinel keeps
    // accidental C-string use from running off the end.
    lisp_string *s = (lisp_string *)lisp_gc_alloc(sizeof(lisp_string) + len + 1);
    if (s == NULL)
        return LISP_UNDEF;
    s->h.header = LISP_MK_HEADER(LISP_OBJ_STRING, len);
    memcpy(s->data, data, len);
    s->data[len] = '\0';
    return lisp_from_obj(s);
}

const char *lisp_named_name(lisp_value v) { return ((lisp_named *)lisp_obj(v))->name; }
size_t lisp_named_len(lisp_value v) { return (size_t)LISP_HDR_AUX(lisp_obj(v)); }
const char *lisp_string_data(lisp_value v) { return ((lisp_string *)lisp_obj(v))->data; }
size_t lisp_string_len(lisp_value v) { return (size_t)LISP_HDR_AUX(lisp_obj(v)); }

// Vectors are immutable: there is no vector-set!. An "update" allocates a fresh
// vector (copy-on-write of the flat array). Structural-sharing variants (RRB)
// are a later optimization; flat COW is the small/elegant starting point.
lisp_value lisp_make_vector(size_t len, lisp_value fill) {
    lisp_vector *v = (lisp_vector *)lisp_gc_alloc(sizeof(lisp_vector) + len * sizeof(lisp_value));
    if (v == NULL)
        return LISP_UNDEF;
    v->h.header = LISP_MK_HEADER(LISP_OBJ_VECTOR, len);
    for (size_t i = 0; i < len; i++)
        v->items[i] = fill;
    return lisp_from_obj(v);
}

size_t lisp_vector_length(lisp_value v) { return (size_t)LISP_HDR_AUX(lisp_obj(v)); }
lisp_value lisp_vector_ref(lisp_value v, size_t i) {
    return ((lisp_vector *)lisp_obj(v))->items[i];
}
void lisp_vector_set_init(lisp_value v, size_t i, lisp_value x) {
    ((lisp_vector *)lisp_obj(v))->items[i] = x;  // for constructors only (see header)
}

lisp_value lisp_make_flonum(double x) {
    lisp_flonum *f = (lisp_flonum *)lisp_gc_alloc(sizeof(lisp_flonum));
    if (f == NULL)
        return LISP_UNDEF;
    f->h.header = LISP_MK_HEADER(LISP_OBJ_FLONUM, 0);
    f->val = x;
    return lisp_from_obj(f);
}
