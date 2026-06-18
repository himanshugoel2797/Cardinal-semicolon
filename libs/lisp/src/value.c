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

#include "lisp.h"

// FNV-1a over the name bytes; used later for symbol interning / map hashing.
static uint32_t name_hash(const char *s, size_t len) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; i++) {
        h ^= (uint8_t)s[i];
        h *= 16777619u;
    }
    return h;
}

lisp_value lisp_cons(lisp_value car, lisp_value cdr) {
    lisp_pair *p = (lisp_pair *)malloc(sizeof(lisp_pair));
    if (p == NULL)
        return LISP_UNDEF;
    p->h.header = LISP_MK_HEADER(LISP_OBJ_PAIR, 0);
    p->car = car;
    p->cdr = cdr;
    return lisp_from_obj(p);
}

static lisp_value make_named(lisp_objtype type, const char *name, size_t len) {
    lisp_named *s = (lisp_named *)malloc(sizeof(lisp_named) + len + 1);
    if (s == NULL)
        return LISP_UNDEF;
    s->h.header = LISP_MK_HEADER(type, len);
    s->hash = name_hash(name, len);
    memcpy(s->name, name, len);
    s->name[len] = '\0';
    return lisp_from_obj(s);
}

lisp_value lisp_make_symbol(const char *name, size_t len) {
    return make_named(LISP_OBJ_SYMBOL, name, len);
}

lisp_value lisp_make_keyword(const char *name, size_t len) {
    return make_named(LISP_OBJ_KEYWORD, name, len);
}

lisp_value lisp_make_string(const char *data, size_t len) {
    // Over-allocate one byte for a defensive NUL: the length in the header is
    // authoritative (strings may contain embedded NULs), but a sentinel keeps
    // accidental C-string use from running off the end.
    lisp_string *s = (lisp_string *)malloc(sizeof(lisp_string) + len + 1);
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
