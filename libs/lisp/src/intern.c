// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

// Symbol/keyword interning: equal names map to one identical object, so eq? is a
// pointer compare and special-form dispatch is cheap. Open-addressed hash table
// with linear probing, grown at a 0.7 load factor.
//
// NOTE: the table is process-global mutable state and is NOT yet thread-safe; a
// lock arrives with kernelization (Phase 6 / the concurrency phase). Single
// runtime thread for now.

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "lisp.h"

// FNV-1a over the name bytes.
static uint32_t name_hash(const char *s, size_t len) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; i++) {
        h ^= (uint8_t)s[i];
        h *= 16777619u;
    }
    return h;
}

static lisp_value alloc_named(lisp_objtype type, const char *name, size_t len, uint32_t hash) {
    lisp_named *s = (lisp_named *)malloc(sizeof(lisp_named) + len + 1);
    if (s == NULL)
        return LISP_UNDEF;
    s->h.header = LISP_MK_HEADER(type, len);
    s->hash = hash;
    memcpy(s->name, name, len);
    s->name[len] = '\0';
    return lisp_from_obj(s);
}

// Open-addressing table. 0 is the empty sentinel (never a valid tagged object).
static lisp_value *g_table = NULL;
static size_t g_cap = 0;    // power of two
static size_t g_count = 0;

static bool entry_matches(lisp_value e, lisp_objtype type, const char *name, size_t len,
                          uint32_t hash) {
    lisp_named *s = (lisp_named *)lisp_obj(e);
    return LISP_HDR_TYPE(&s->h) == type && s->hash == hash &&
           lisp_named_len(e) == len && memcmp(s->name, name, len) == 0;
}

static bool intern_grow(void) {
    size_t newcap = g_cap == 0 ? 256 : g_cap * 2;
    lisp_value *nt = (lisp_value *)malloc(newcap * sizeof(lisp_value));
    if (nt == NULL)
        return false;
    for (size_t i = 0; i < newcap; i++)
        nt[i] = 0;
    // Rehash existing entries into the new table.
    for (size_t i = 0; i < g_cap; i++) {
        lisp_value e = g_table[i];
        if (e == 0)
            continue;
        uint32_t h = ((lisp_named *)lisp_obj(e))->hash;
        size_t j = h & (newcap - 1);
        while (nt[j] != 0)
            j = (j + 1) & (newcap - 1);
        nt[j] = e;
    }
    free(g_table);
    g_table = nt;
    g_cap = newcap;
    return true;
}

static lisp_value intern(lisp_objtype type, const char *name, size_t len) {
    if (g_count * 10 >= g_cap * 7) {  // also covers the initial g_cap == 0 case
        if (!intern_grow())
            return LISP_UNDEF;
    }
    uint32_t hash = name_hash(name, len);
    size_t mask = g_cap - 1;
    size_t i = hash & mask;
    while (g_table[i] != 0) {
        if (entry_matches(g_table[i], type, name, len, hash))
            return g_table[i];  // already interned
        i = (i + 1) & mask;
    }
    lisp_value sym = alloc_named(type, name, len, hash);
    if (sym == LISP_UNDEF)
        return LISP_UNDEF;
    g_table[i] = sym;
    g_count++;
    return sym;
}

lisp_value lisp_make_symbol(const char *name, size_t len) {
    return intern(LISP_OBJ_SYMBOL, name, len);
}

lisp_value lisp_make_keyword(const char *name, size_t len) {
    return intern(LISP_OBJ_KEYWORD, name, len);
}
