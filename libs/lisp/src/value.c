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

// Mutable byte buffers (driver substrate + bulk IPC). lisp_make_bytes owns its
// inline storage; lisp_make_bytes_foreign wraps external (MMIO/DMA) memory the GC
// must not free. See lisp_bytes in lisp.h.
lisp_value lisp_make_bytes(size_t len) {
    lisp_bytes *b = (lisp_bytes *)lisp_gc_alloc(sizeof(lisp_bytes) + len);
    if (b == NULL)
        return LISP_UNDEF;
    b->h.header = LISP_MK_HEADER(LISP_OBJ_BYTES, 0);
    b->data = (uint8_t *)(b + 1);  // inline storage trails the header
    b->len = len;
    b->phys = 0;
    b->owned = 1;
    memset(b->data, 0, len);
    return lisp_from_obj(b);
}

lisp_value lisp_make_bytes_foreign(void *ptr, size_t len, uint64_t phys) {
    lisp_bytes *b = (lisp_bytes *)lisp_gc_alloc(sizeof(lisp_bytes));
    if (b == NULL)
        return LISP_UNDEF;
    b->h.header = LISP_MK_HEADER(LISP_OBJ_BYTES, 0);
    b->data = (uint8_t *)ptr;  // foreign (MMIO/DMA): never freed by the GC
    b->len = len;
    b->phys = phys;
    b->owned = 0;
    return lisp_from_obj(b);
}

// Opaque foreign handles (lisp_make_handle in lisp.h): an external resource the GC
// finalizes. The wrapped pointer is foreign storage libs/lisp never dereferences;
// gc.c calls `fin` when the handle is swept (a GC leaf -- see the trace `default`).
lisp_value lisp_make_handle(void *ptr, void (*finalize)(void *ptr), uint32_t tag) {
    lisp_handle_t *o = (lisp_handle_t *)lisp_gc_alloc(sizeof(lisp_handle_t));
    if (o == NULL)
        return LISP_UNDEF;
    o->h.header = LISP_MK_HEADER(LISP_OBJ_HANDLE, 0);
    o->ptr = ptr;
    o->fin = finalize;
    o->tag = tag;
    return lisp_from_obj(o);
}

bool lisp_is_handle(lisp_value v) {
    return lisp_is_ptr(v) && LISP_HDR_TYPE(lisp_obj(v)) == LISP_OBJ_HANDLE;
}
void *lisp_handle_ptr(lisp_value v) {
    return lisp_is_handle(v) ? ((lisp_handle_t *)lisp_obj(v))->ptr : NULL;
}
uint32_t lisp_handle_tag(lisp_value v) {
    return lisp_is_handle(v) ? ((lisp_handle_t *)lisp_obj(v))->tag : 0;
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
