// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

// syntax-rules macros: pattern matching with ellipsis (...) and template
// expansion. Supports single and nested ellipsis via per-binding depth tracking.
//
// Limitations (documented intentional exceptions): macros are NOT hygienic --
// template-introduced identifiers are inserted as-is, so a macro can capture or
// be captured in adversarial cases; ellipsis escaping (... ...) is unsupported.
// Hygiene-sensitive conformance tests are expected to fail.
//
// All intermediate values are ordinary lisp objects held in C locals, so a GC
// triggered mid-expansion finds them via the conservative stack scan.

#include <stdint.h>
#include <string.h>

#include "internal.h"
#include "lisp.h"

// --- small list helpers ------------------------------------------------------

static lisp_value cadr(lisp_value v) { return lisp_car(lisp_cdr(v)); }

static int list_len(lisp_value v) {  // count leading pairs (ignores improper tail)
    int n = 0;
    while (lisp_is_pair(v)) {
        n++;
        v = lisp_cdr(v);
    }
    return n;
}

static lisp_value reverse(lisp_value lst) {
    lisp_value out = LISP_EMPTY;
    while (lisp_is_pair(lst)) {
        out = lisp_cons(lisp_car(lst), out);
        lst = lisp_cdr(lst);
    }
    return out;
}

// Error during expansion: a plain error, so clear any stale nonlocal-exit kind
// (otherwise an enclosing guard could misclassify it -- see control.c).
static lisp_value mfail(const char **err, const char *msg) {
    if (err != NULL)
        *err = msg;
    lisp_ctl_clear();
    return LISP_UNDEF;
}

static bool sym_named(lisp_value v, const char *name) {
    return lisp_is_symbol(v) && lisp_named_len(v) == strlen(name) &&
           memcmp(lisp_named_name(v), name, strlen(name)) == 0;
}

static bool is_ellipsis(lisp_value v) { return sym_named(v, "..."); }

static bool in_list(lisp_value x, lisp_value lst) {  // eq? membership (interned)
    while (lisp_is_pair(lst)) {
        if (lisp_car(lst) == x)
            return true;
        lst = lisp_cdr(lst);
    }
    return false;
}

// --- bindings: alist of (var depth . value) ---------------------------------

static lisp_value b_add(lisp_value binds, lisp_value var, int depth, lisp_value val) {
    lisp_value entry = lisp_cons(var, lisp_cons(lisp_fixnum(depth), val));
    return lisp_cons(entry, binds);
}
static lisp_value b_find(lisp_value binds, lisp_value var) {  // entry or LISP_UNDEF
    while (lisp_is_pair(binds)) {
        lisp_value e = lisp_car(binds);
        if (lisp_car(e) == var)
            return e;
        binds = lisp_cdr(binds);
    }
    return LISP_UNDEF;
}
static int b_depth(lisp_value entry) { return (int)lisp_fixnum_val(lisp_car(lisp_cdr(entry))); }
static lisp_value b_val(lisp_value entry) { return lisp_cdr(lisp_cdr(entry)); }

// Collect the pattern variables of `pat` (symbols that are not literals, not _,
// not ...) into `acc`.
static lisp_value pattern_vars(lisp_value pat, lisp_value lits, lisp_value acc) {
    if (lisp_is_symbol(pat)) {
        if (sym_named(pat, "_") || is_ellipsis(pat) || in_list(pat, lits))
            return acc;
        return lisp_cons(pat, acc);
    }
    if (lisp_is_pair(pat)) {
        acc = pattern_vars(lisp_car(pat), lits, acc);
        return pattern_vars(lisp_cdr(pat), lits, acc);
    }
    return acc;
}

// --- matching ---------------------------------------------------------------

static bool match(lisp_value pat, lisp_value form, lisp_value lits, lisp_value *binds,
                  int depth);

// (sub ... . tail): sub matches the leading elements; tail matches the rest.
static bool match_ellipsis(lisp_value pat, lisp_value form, lisp_value lits,
                           lisp_value *binds, int depth) {
    lisp_value sub = lisp_car(pat);
    lisp_value tail = lisp_cdr(lisp_cdr(pat));  // skip sub and ...
    int nform = list_len(form);
    int ntail = list_len(tail);
    int nmatch = nform - ntail;
    if (nmatch < 0)
        return false;

    // Match each of the first nmatch elements against `sub`, keeping a frame of
    // bindings per element.
    lisp_value frames = LISP_EMPTY;  // reversed list of per-element binding sets
    lisp_value f = form;
    for (int i = 0; i < nmatch; i++) {
        lisp_value frame = LISP_EMPTY;
        if (!match(sub, lisp_car(f), lits, &frame, depth + 1))
            return false;
        frames = lisp_cons(frame, frames);
        f = lisp_cdr(f);
    }
    frames = reverse(frames);

    // For each variable in `sub`, bind it (at depth+1) to the sequence of its
    // per-element values, in order.
    lisp_value vars = pattern_vars(sub, lits, LISP_EMPTY);
    for (lisp_value vp = vars; lisp_is_pair(vp); vp = lisp_cdr(vp)) {
        lisp_value var = lisp_car(vp);
        lisp_value seq = LISP_EMPTY;  // built reversed then reversed
        for (lisp_value fr = frames; lisp_is_pair(fr); fr = lisp_cdr(fr)) {
            lisp_value e = b_find(lisp_car(fr), var);
            seq = lisp_cons(e == LISP_UNDEF ? LISP_UNDEF : b_val(e), seq);
        }
        *binds = b_add(*binds, var, depth + 1, reverse(seq));
    }
    return match(tail, f, lits, binds, depth);
}

static bool match(lisp_value pat, lisp_value form, lisp_value lits, lisp_value *binds,
                  int depth) {
    if (lisp_is_symbol(pat)) {
        if (sym_named(pat, "_"))
            return true;
        if (in_list(pat, lits))
            return lisp_is_symbol(form) && form == pat;  // literal must match itself
        *binds = b_add(*binds, pat, depth, form);
        return true;
    }
    if (lisp_is_pair(pat)) {
        if (lisp_is_pair(lisp_cdr(pat)) && is_ellipsis(cadr(pat)))
            return match_ellipsis(pat, form, lits, binds, depth);
        if (!lisp_is_pair(form))
            return false;
        return match(lisp_car(pat), lisp_car(form), lits, binds, depth) &&
               match(lisp_cdr(pat), lisp_cdr(form), lits, binds, depth);
    }
    if (lisp_is_empty(pat))
        return lisp_is_empty(form);
    return pat == form;  // datum (fixnum/bool/char); strings compared by identity for now
}

// --- template instantiation -------------------------------------------------

static lisp_value instantiate(lisp_value tmpl, lisp_value binds, lisp_value lits,
                              const char **err);

// Expand `(sub ...)`: iterate the ellipsis variables of `sub` (those bound at
// depth >= 1) in lockstep, instantiating `sub` for each, then splice before the
// instantiation of the rest of the list.
static lisp_value expand_ellipsis(lisp_value tmpl, lisp_value binds, lisp_value lits,
                                  const char **err) {
    lisp_value sub = lisp_car(tmpl);
    lisp_value rest = lisp_cdr(lisp_cdr(tmpl));  // after the ...

    // Iteration variables: template vars of `sub` bound with depth >= 1. All of
    // their sequences must share one length (R7RS: mismatched counts are an error).
    lisp_value ivars = LISP_EMPTY;
    int n = -1;
    for (lisp_value vp = pattern_vars(sub, lits, LISP_EMPTY); lisp_is_pair(vp);
         vp = lisp_cdr(vp)) {
        lisp_value e = b_find(binds, lisp_car(vp));
        if (e != LISP_UNDEF && b_depth(e) >= 1) {
            ivars = lisp_cons(lisp_car(vp), ivars);
            int len = list_len(b_val(e));
            if (n < 0)
                n = len;
            else if (len != n)
                return mfail(err, "macro: mismatched ellipsis lengths");
        }
    }
    if (n < 0)
        n = 0;  // no iteration variables -> zero expansions

    lisp_value results = LISP_EMPTY;  // built reversed
    for (int i = 0; i < n; i++) {
        lisp_value binds2 = binds;  // shadow each ivar with its i-th element, depth-1
        for (lisp_value vp = ivars; lisp_is_pair(vp); vp = lisp_cdr(vp)) {
            lisp_value var = lisp_car(vp);
            lisp_value e = b_find(binds, var);
            lisp_value seq = b_val(e);
            for (int k = 0; k < i && lisp_is_pair(seq); k++)
                seq = lisp_cdr(seq);
            lisp_value elem = lisp_is_pair(seq) ? lisp_car(seq) : LISP_UNDEF;
            binds2 = b_add(binds2, var, b_depth(e) - 1, elem);
        }
        lisp_value item = instantiate(sub, binds2, lits, err);
        if (item == LISP_UNDEF && err != NULL && *err != NULL)
            return LISP_UNDEF;
        results = lisp_cons(item, results);
    }

    // tail ++ : cons the in-order items (results is reversed) onto the rest.
    lisp_value out = instantiate(rest, binds, lits, err);
    if (out == LISP_UNDEF && err != NULL && *err != NULL)
        return LISP_UNDEF;
    for (lisp_value p = results; lisp_is_pair(p); p = lisp_cdr(p)) {
        out = lisp_cons(lisp_car(p), out);
        if (out == LISP_UNDEF)
            return mfail(err, "out of memory in macro expansion");
    }
    return out;
}

static lisp_value instantiate(lisp_value tmpl, lisp_value binds, lisp_value lits,
                              const char **err) {
    if (lisp_is_symbol(tmpl)) {
        lisp_value e = b_find(binds, tmpl);
        if (e == LISP_UNDEF)
            return tmpl;  // free identifier inserted as-is (unhygienic)
        if (b_depth(e) != 0)  // ellipsis var used without enough ellipses
            return mfail(err, "macro: ellipsis variable used at the wrong depth");
        return b_val(e);
    }
    if (lisp_is_pair(tmpl)) {
        if (lisp_is_pair(lisp_cdr(tmpl)) && is_ellipsis(cadr(tmpl)))
            return expand_ellipsis(tmpl, binds, lits, err);
        lisp_value a = instantiate(lisp_car(tmpl), binds, lits, err);
        if (a == LISP_UNDEF && err != NULL && *err != NULL)
            return LISP_UNDEF;
        lisp_value d = instantiate(lisp_cdr(tmpl), binds, lits, err);
        if (d == LISP_UNDEF && err != NULL && *err != NULL)
            return LISP_UNDEF;
        lisp_value c = lisp_cons(a, d);
        if (c == LISP_UNDEF)
            return mfail(err, "out of memory in macro expansion");
        return c;
    }
    return tmpl;
}

// --- constructor + entry point ----------------------------------------------

lisp_value lisp_make_macro(lisp_value literals, lisp_value rules, lisp_value def_env) {
    lisp_macro_t *m = (lisp_macro_t *)lisp_gc_alloc(sizeof(lisp_macro_t));
    if (m == NULL)
        return LISP_UNDEF;
    m->h.header = LISP_MK_HEADER(LISP_OBJ_MACRO, 0);
    // Initialize all object fields before any further allocation could GC and
    // trace this (already-listed) macro (mirrors lisp_make_primitive).
    m->literals = LISP_EMPTY;
    m->rules = LISP_EMPTY;
    m->def_env = LISP_EMPTY;
    m->literals = literals;
    m->rules = rules;
    m->def_env = def_env;
    return lisp_from_obj(m);
}

lisp_value lisp_macro_expand(lisp_value macro, lisp_value form, const char **err) {
    lisp_macro_t *m = (lisp_macro_t *)lisp_obj(macro);
    for (lisp_value r = m->rules; lisp_is_pair(r); r = lisp_cdr(r)) {
        lisp_value rule = lisp_car(r);
        if (!lisp_is_pair(rule) || !lisp_is_pair(lisp_cdr(rule)))
            continue;  // malformed rule, skip
        lisp_value pat = lisp_car(rule);
        lisp_value tmpl = cadr(rule);
        lisp_value binds = LISP_EMPTY;
        // The pattern's first element is the macro keyword; match the rest.
        if (lisp_is_pair(pat) &&
            match(lisp_cdr(pat), lisp_cdr(form), m->literals, &binds, 0))
            return instantiate(tmpl, binds, m->literals, err);
    }
    return mfail(err, "no matching syntax-rules pattern");
}
