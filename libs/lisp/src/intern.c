// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

// Symbol/keyword interning: equal names map to one identical object, so eq? is a
// pointer compare and special-form dispatch is cheap. Open-addressed hash table
// with linear probing, grown at a 0.7 load factor.
//
// The table is process-global mutable state shared by every core. intern()
// takes the runtime lock (lisp_rt_lock) across the whole probe/insert/grow so
// concurrent interning on two cores is safe, and the system-heap collector --
// which reads the table as a root under the same lock -- never races a grow.
// Single-threaded embedders install no lock, so this is free there.

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "internal.h"
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
    // Interned symbols/keywords are the shared-immutable region: they must live in
    // the system heap even when a per-context heap is the current target, since
    // other contexts (and the intern table) reference them. The caller (intern)
    // already holds the runtime lock, so use the no-lock system allocator -- the
    // lock is not recursive.
    lisp_named *s = (lisp_named *)lisp_gc_alloc_shared_nolock(sizeof(lisp_named) + len + 1);
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
    uint32_t hash = name_hash(name, len);
    // The whole probe/insert/grow runs under the runtime lock: the table is
    // shared by every core, and alloc_named below uses the no-lock allocator
    // since we already hold it (the lock is not recursive).
    lisp_rt_lock();
    lisp_value result = LISP_UNDEF;
    bool ok = true;
    if (g_count * 10 >= g_cap * 7)  // also covers the initial g_cap == 0 case
        ok = intern_grow();
    if (ok) {
        size_t mask = g_cap - 1;
        size_t i = hash & mask;
        while (g_table[i] != 0) {
            if (entry_matches(g_table[i], type, name, len, hash)) {
                result = g_table[i];  // already interned
                break;
            }
            i = (i + 1) & mask;
        }
        if (result == LISP_UNDEF && g_table[i] == 0) {  // empty slot: insert
            lisp_value sym = alloc_named(type, name, len, hash);
            if (sym != LISP_UNDEF) {
                g_table[i] = sym;
                g_count++;
                result = sym;
            }
        }
    }
    lisp_rt_unlock();
    return result;
}

// Expose the table so the GC can mark interned symbols as roots (they are not
// otherwise reachable, and are intended to persist).
lisp_value *lisp_intern_table(size_t *cap_out) {
    *cap_out = g_cap;
    return g_table;
}

lisp_value lisp_make_symbol(const char *name, size_t len) {
    return intern(LISP_OBJ_SYMBOL, name, len);
}

lisp_value lisp_make_keyword(const char *name, size_t len) {
    return intern(LISP_OBJ_KEYWORD, name, len);
}
