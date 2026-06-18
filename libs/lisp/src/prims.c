// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

// Phase 1 built-in procedures: integer arithmetic and comparison, list/pair
// operations, and basic predicates. Integer-only (the kernel is no-FP); overflow
// past the fixnum range silently wraps for now -- bignums are a later phase.

#include <stdint.h>
#include <string.h>

#include "lisp.h"

static lisp_value prim_err(const char **err, const char *msg) {
    if (err != NULL)
        *err = msg;
    return LISP_UNDEF;
}

static bool all_fixnum(lisp_value *args, int argc) {
    for (int i = 0; i < argc; i++)
        if (!lisp_is_fixnum(args[i]))
            return false;
    return true;
}

// --- Arithmetic -------------------------------------------------------------

static lisp_value prim_add(lisp_value *args, int argc, const char **err) {
    if (!all_fixnum(args, argc))
        return prim_err(err, "+ expects integers");
    int64_t acc = 0;
    for (int i = 0; i < argc; i++)
        acc += lisp_fixnum_val(args[i]);
    return lisp_fixnum(acc);
}

static lisp_value prim_mul(lisp_value *args, int argc, const char **err) {
    if (!all_fixnum(args, argc))
        return prim_err(err, "* expects integers");
    int64_t acc = 1;
    for (int i = 0; i < argc; i++)
        acc *= lisp_fixnum_val(args[i]);
    return lisp_fixnum(acc);
}

static lisp_value prim_sub(lisp_value *args, int argc, const char **err) {
    if (argc < 1)
        return prim_err(err, "- expects at least one argument");
    if (!all_fixnum(args, argc))
        return prim_err(err, "- expects integers");
    int64_t acc = lisp_fixnum_val(args[0]);
    if (argc == 1)
        return lisp_fixnum(-acc);
    for (int i = 1; i < argc; i++)
        acc -= lisp_fixnum_val(args[i]);
    return lisp_fixnum(acc);
}

static lisp_value prim_div(lisp_value *args, int argc, const char **err) {
    if (argc < 1)
        return prim_err(err, "/ expects at least one argument");
    if (!all_fixnum(args, argc))
        return prim_err(err, "/ expects integers");
    int64_t acc = lisp_fixnum_val(args[0]);
    if (argc == 1) {
        if (acc == 0)
            return prim_err(err, "division by zero");
        return lisp_fixnum(1 / acc);
    }
    for (int i = 1; i < argc; i++) {
        int64_t d = lisp_fixnum_val(args[i]);
        if (d == 0)
            return prim_err(err, "division by zero");
        acc /= d;
    }
    return lisp_fixnum(acc);
}

static lisp_value prim_mod(lisp_value *args, int argc, const char **err) {
    if (argc != 2 || !all_fixnum(args, argc))
        return prim_err(err, "modulo expects two integers");
    int64_t d = lisp_fixnum_val(args[1]);
    if (d == 0)
        return prim_err(err, "division by zero");
    return lisp_fixnum(lisp_fixnum_val(args[0]) % d);
}

// --- Comparison (chained: (< 1 2 3) => #t) ----------------------------------

typedef enum { CMP_EQ, CMP_LT, CMP_GT, CMP_LE, CMP_GE } cmp_op;

static lisp_value compare(lisp_value *args, int argc, const char **err, cmp_op op,
                          const char *name) {
    if (argc < 1)
        return prim_err(err, name);
    if (!all_fixnum(args, argc))
        return prim_err(err, name);
    for (int i = 0; i + 1 < argc; i++) {  // (< x) is vacuously #t
        int64_t a = lisp_fixnum_val(args[i]);
        int64_t b = lisp_fixnum_val(args[i + 1]);
        bool ok;
        switch (op) {
            case CMP_EQ: ok = a == b; break;
            case CMP_LT: ok = a < b; break;
            case CMP_GT: ok = a > b; break;
            case CMP_LE: ok = a <= b; break;
            case CMP_GE: ok = a >= b; break;
        }
        if (!ok)
            return LISP_FALSE;
    }
    return LISP_TRUE;
}

static lisp_value prim_numeq(lisp_value *a, int n, const char **e) {
    return compare(a, n, e, CMP_EQ, "= expects integers");
}
static lisp_value prim_lt(lisp_value *a, int n, const char **e) {
    return compare(a, n, e, CMP_LT, "< expects integers");
}
static lisp_value prim_gt(lisp_value *a, int n, const char **e) {
    return compare(a, n, e, CMP_GT, "> expects integers");
}
static lisp_value prim_le(lisp_value *a, int n, const char **e) {
    return compare(a, n, e, CMP_LE, "<= expects integers");
}
static lisp_value prim_ge(lisp_value *a, int n, const char **e) {
    return compare(a, n, e, CMP_GE, ">= expects integers");
}

// --- Pairs / lists ----------------------------------------------------------

static lisp_value prim_cons(lisp_value *args, int argc, const char **err) {
    if (argc != 2)
        return prim_err(err, "cons expects two arguments");
    return lisp_cons(args[0], args[1]);
}

static lisp_value prim_car(lisp_value *args, int argc, const char **err) {
    if (argc != 1 || !lisp_is_pair(args[0]))
        return prim_err(err, "car expects a pair");
    return lisp_car(args[0]);
}

static lisp_value prim_cdr(lisp_value *args, int argc, const char **err) {
    if (argc != 1 || !lisp_is_pair(args[0]))
        return prim_err(err, "cdr expects a pair");
    return lisp_cdr(args[0]);
}

static lisp_value prim_list(lisp_value *args, int argc, const char **err) {
    lisp_value lst = LISP_EMPTY;
    for (int i = argc - 1; i >= 0; i--) {
        lisp_value cell = lisp_cons(args[i], lst);
        if (cell == LISP_UNDEF)
            return prim_err(err, "out of memory");
        lst = cell;
    }
    return lst;
}

// --- Predicates -------------------------------------------------------------

static lisp_value bool_val(bool b) { return b ? LISP_TRUE : LISP_FALSE; }

static lisp_value prim_nullp(lisp_value *args, int argc, const char **err) {
    if (argc != 1)
        return prim_err(err, "null? expects one argument");
    return bool_val(lisp_is_empty(args[0]));
}

static lisp_value prim_pairp(lisp_value *args, int argc, const char **err) {
    if (argc != 1)
        return prim_err(err, "pair? expects one argument");
    return bool_val(lisp_is_pair(args[0]));
}

static lisp_value prim_not(lisp_value *args, int argc, const char **err) {
    if (argc != 1)
        return prim_err(err, "not expects one argument");
    return bool_val(!lisp_truthy(args[0]));
}

static lisp_value prim_zerop(lisp_value *args, int argc, const char **err) {
    if (argc != 1 || !lisp_is_fixnum(args[0]))
        return prim_err(err, "zero? expects an integer");
    return bool_val(lisp_fixnum_val(args[0]) == 0);
}

// eq?: identity for heap objects, value-equality for immediates/fixnums (both
// reduce to comparing the tagged words). Symbols are compared by name as a
// Phase 1 stand-in -- the language intends them interned, so once symbol
// interning lands (Phase 2) this reduces to the pure word compare. Deep
// structural equality (equal?) is a separate, later addition.
static lisp_value prim_eqp(lisp_value *args, int argc, const char **err) {
    if (argc != 2)
        return prim_err(err, "eq? expects two arguments");
    if (args[0] == args[1])
        return LISP_TRUE;
    if (lisp_is_symbol(args[0]) && lisp_is_symbol(args[1])) {
        size_t len = lisp_named_len(args[0]);
        if (len == lisp_named_len(args[1]) &&
            memcmp(lisp_named_name(args[0]), lisp_named_name(args[1]), len) == 0)
            return LISP_TRUE;
    }
    return LISP_FALSE;
}

// --- Installation -----------------------------------------------------------

static void def(lisp_value env, const char *name, lisp_primitive_fn fn) {
    lisp_value sym = lisp_make_symbol(name, strlen(name));
    lisp_value prim = lisp_make_primitive(fn, name);
    lisp_env_define(env, sym, prim);
}

void lisp_install_primitives(lisp_value env) {
    def(env, "+", prim_add);
    def(env, "-", prim_sub);
    def(env, "*", prim_mul);
    def(env, "/", prim_div);
    def(env, "modulo", prim_mod);
    def(env, "=", prim_numeq);
    def(env, "<", prim_lt);
    def(env, ">", prim_gt);
    def(env, "<=", prim_le);
    def(env, ">=", prim_ge);
    def(env, "cons", prim_cons);
    def(env, "car", prim_car);
    def(env, "cdr", prim_cdr);
    def(env, "list", prim_list);
    def(env, "null?", prim_nullp);
    def(env, "pair?", prim_pairp);
    def(env, "not", prim_not);
    def(env, "zero?", prim_zerop);
    def(env, "eq?", prim_eqp);
}
