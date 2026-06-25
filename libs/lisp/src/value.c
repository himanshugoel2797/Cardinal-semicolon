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

const char *lisp_named_name(lisp_value v) { return ((lisp_named *)lisp_obj(v))->name; }
size_t lisp_named_len(lisp_value v) { return (size_t)LISP_HDR_AUX(lisp_obj(v)); }
const char *lisp_string_data(lisp_value v) { return ((lisp_string *)lisp_obj(v))->data; }
size_t lisp_string_len(lisp_value v) { return (size_t)LISP_HDR_AUX(lisp_obj(v)); }

// Vectors are mutable (vector-set!), per-context, deep-copied on send -- the same
// shared-nothing contract as bytes (see header). The storage is a flat inline
// array trailing the header; the GC traces every element and frees the whole
// object in one piece.
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
void lisp_vector_set(lisp_value v, size_t i, lisp_value x) {
    ((lisp_vector *)lisp_obj(v))->items[i] = x;
}
void lisp_vector_set_init(lisp_value v, size_t i, lisp_value x) {
    ((lisp_vector *)lisp_obj(v))->items[i] = x;  // identical store; constructor intent
}

// A hash table is a struct + a Lisp VECTOR of bucket lists, so the GC reaches all
// keys/values through the single `buckets` child and frees the table with the
// sweep (no finalizer). The caller seeds it with `nbuckets` empty buckets.
lisp_value lisp_make_hashtable(size_t nbuckets) {
    if (nbuckets < 1)
        nbuckets = 1;
    // Allocate the buckets vector first; if it fails, no half-built table escapes.
    lisp_value buckets = lisp_make_vector(nbuckets, LISP_EMPTY);
    if (buckets == LISP_UNDEF)
        return LISP_UNDEF;
    lisp_hashtable *ht = (lisp_hashtable *)lisp_gc_alloc(sizeof(lisp_hashtable));
    if (ht == NULL)
        return LISP_UNDEF;
    ht->h.header = LISP_MK_HEADER(LISP_OBJ_HASHTABLE, 0);
    ht->buckets = buckets;
    ht->count = 0;
    return lisp_from_obj(ht);
}

lisp_value lisp_hashtable_buckets(lisp_value ht) {
    return ((lisp_hashtable *)lisp_obj(ht))->buckets;
}
size_t lisp_hashtable_count(lisp_value ht) {
    return ((lisp_hashtable *)lisp_obj(ht))->count;
}
void lisp_hashtable_set_buckets(lisp_value ht, lisp_value buckets) {
    ((lisp_hashtable *)lisp_obj(ht))->buckets = buckets;
}
void lisp_hashtable_set_count(lisp_value ht, size_t count) {
    ((lisp_hashtable *)lisp_obj(ht))->count = count;
}

lisp_value lisp_make_flonum(double x) {
    lisp_flonum *f = (lisp_flonum *)lisp_gc_alloc(sizeof(lisp_flonum));
    if (f == NULL)
        return LISP_UNDEF;
    f->h.header = LISP_MK_HEADER(LISP_OBJ_FLONUM, 0);
    f->val = x;
    return lisp_from_obj(f);
}
