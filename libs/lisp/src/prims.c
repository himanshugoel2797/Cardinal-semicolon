// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

// Built-in procedures: numeric arithmetic/comparison (exact fixnums + inexact
// flonums with the usual contagion), list/pair operations, vectors, predicates,
// and higher-order map/apply. Integer overflow past the fixnum range still wraps
// (bignums are a later phase).

#include <stdint.h>
#include <stdlib.h>
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

static bool all_number(lisp_value *args, int argc) {
    for (int i = 0; i < argc; i++)
        if (!lisp_is_number(args[i]))
            return false;
    return true;
}

// Append a freshly-built cell to a list being constructed in place. Used only on
// cells this code just allocated, so mutation is safe (not user-visible data).
static void set_cdr(lisp_value pair, lisp_value v) {
    ((lisp_pair *)lisp_obj(pair))->cdr = v;
}

// --- Arithmetic -------------------------------------------------------------

// Arithmetic: exact (fixnum) when all operands are fixnums, else inexact
// (flonum) -- the usual numeric contagion.
static lisp_value prim_add(lisp_value *args, int argc, const char **err) {
    if (all_fixnum(args, argc)) {
        int64_t acc = 0;
        for (int i = 0; i < argc; i++)
            acc += lisp_fixnum_val(args[i]);
        return lisp_fixnum(acc);
    }
    if (!all_number(args, argc))
        return prim_err(err, "+ expects numbers");
    double acc = 0.0;
    for (int i = 0; i < argc; i++)
        acc += lisp_number_to_double(args[i]);
    return lisp_make_flonum(acc);
}

static lisp_value prim_mul(lisp_value *args, int argc, const char **err) {
    if (all_fixnum(args, argc)) {
        int64_t acc = 1;
        for (int i = 0; i < argc; i++)
            acc *= lisp_fixnum_val(args[i]);
        return lisp_fixnum(acc);
    }
    if (!all_number(args, argc))
        return prim_err(err, "* expects numbers");
    double acc = 1.0;
    for (int i = 0; i < argc; i++)
        acc *= lisp_number_to_double(args[i]);
    return lisp_make_flonum(acc);
}

static lisp_value prim_sub(lisp_value *args, int argc, const char **err) {
    if (argc < 1)
        return prim_err(err, "- expects at least one argument");
    if (all_fixnum(args, argc)) {
        int64_t acc = lisp_fixnum_val(args[0]);
        if (argc == 1)
            return lisp_fixnum(-acc);
        for (int i = 1; i < argc; i++)
            acc -= lisp_fixnum_val(args[i]);
        return lisp_fixnum(acc);
    }
    if (!all_number(args, argc))
        return prim_err(err, "- expects numbers");
    double acc = lisp_number_to_double(args[0]);
    if (argc == 1)
        return lisp_make_flonum(-acc);
    for (int i = 1; i < argc; i++)
        acc -= lisp_number_to_double(args[i]);
    return lisp_make_flonum(acc);
}

// Division: exact only when all operands are fixnums and every step divides
// evenly; otherwise inexact float division. Division by zero is an error.
static lisp_value prim_div(lisp_value *args, int argc, const char **err) {
    if (argc < 1)
        return prim_err(err, "/ expects at least one argument");
    if (!all_number(args, argc))
        return prim_err(err, "/ expects numbers");
    if (all_fixnum(args, argc)) {
        int64_t acc = lisp_fixnum_val(args[0]);
        bool exact = true;
        if (argc == 1) {
            if (acc == 0)
                return prim_err(err, "division by zero");
            exact = (acc == 1 || acc == -1);
            if (exact)
                acc = 1 / acc;
        } else {
            for (int i = 1; i < argc; i++) {
                int64_t d = lisp_fixnum_val(args[i]);
                if (d == 0)
                    return prim_err(err, "division by zero");
                if (acc % d != 0) {
                    exact = false;
                    break;
                }
                acc /= d;
            }
        }
        if (exact)
            return lisp_fixnum(acc);
    }
    double acc = lisp_number_to_double(args[0]);
    if (argc == 1) {
        if (acc == 0.0)
            return prim_err(err, "division by zero");
        return lisp_make_flonum(1.0 / acc);
    }
    for (int i = 1; i < argc; i++) {
        double d = lisp_number_to_double(args[i]);
        if (d == 0.0)
            return prim_err(err, "division by zero");
        acc /= d;
    }
    return lisp_make_flonum(acc);
}

// modulo: result takes the sign of the divisor (R7RS), unlike C's %.
static lisp_value prim_mod(lisp_value *args, int argc, const char **err) {
    if (argc != 2 || !all_fixnum(args, argc))
        return prim_err(err, "modulo expects two integers");
    int64_t n = lisp_fixnum_val(args[0]), d = lisp_fixnum_val(args[1]);
    if (d == 0)
        return prim_err(err, "division by zero");
    int64_t r = n % d;
    if (r != 0 && ((r < 0) != (d < 0)))
        r += d;
    return lisp_fixnum(r);
}

// remainder: takes the sign of the dividend (C's % semantics).
static lisp_value prim_remainder(lisp_value *args, int argc, const char **err) {
    if (argc != 2 || !all_fixnum(args, argc))
        return prim_err(err, "remainder expects two integers");
    int64_t d = lisp_fixnum_val(args[1]);
    if (d == 0)
        return prim_err(err, "division by zero");
    return lisp_fixnum(lisp_fixnum_val(args[0]) % d);
}

// quotient: truncating integer division.
static lisp_value prim_quotient(lisp_value *args, int argc, const char **err) {
    if (argc != 2 || !all_fixnum(args, argc))
        return prim_err(err, "quotient expects two integers");
    int64_t d = lisp_fixnum_val(args[1]);
    if (d == 0)
        return prim_err(err, "division by zero");
    return lisp_fixnum(lisp_fixnum_val(args[0]) / d);
}

// --- Comparison (chained: (< 1 2 3) => #t) ----------------------------------

typedef enum { CMP_EQ, CMP_LT, CMP_GT, CMP_LE, CMP_GE } cmp_op;

static lisp_value compare(lisp_value *args, int argc, const char **err, cmp_op op,
                          const char *name) {
    if (argc < 1 || !all_number(args, argc))
        return prim_err(err, name);
    bool fix = all_fixnum(args, argc);  // exact compare when no flonum involved
    for (int i = 0; i + 1 < argc; i++) {  // (< x) is vacuously #t
        bool ok;
        if (fix) {
            int64_t a = lisp_fixnum_val(args[i]), b = lisp_fixnum_val(args[i + 1]);
            switch (op) {
                case CMP_EQ: ok = a == b; break;
                case CMP_LT: ok = a < b; break;
                case CMP_GT: ok = a > b; break;
                case CMP_LE: ok = a <= b; break;
                case CMP_GE: ok = a >= b; break;
            }
        } else {
            // Mixed exact/inexact: compare as doubles (R7RS converts the exact
            // operand to inexact). A fixnum magnitude > 2^53 loses low bits here;
            // that imprecision is spec-permitted, not a bug.
            double a = lisp_number_to_double(args[i]);
            double b = lisp_number_to_double(args[i + 1]);
            switch (op) {
                case CMP_EQ: ok = a == b; break;
                case CMP_LT: ok = a < b; break;
                case CMP_GT: ok = a > b; break;
                case CMP_LE: ok = a <= b; break;
                case CMP_GE: ok = a >= b; break;
            }
        }
        if (!ok)
            return LISP_FALSE;
    }
    return LISP_TRUE;
}

static lisp_value prim_numeq(lisp_value *a, int n, const char **e) {
    return compare(a, n, e, CMP_EQ, "= expects numbers");
}
static lisp_value prim_lt(lisp_value *a, int n, const char **e) {
    return compare(a, n, e, CMP_LT, "< expects numbers");
}
static lisp_value prim_gt(lisp_value *a, int n, const char **e) {
    return compare(a, n, e, CMP_GT, "> expects numbers");
}
static lisp_value prim_le(lisp_value *a, int n, const char **e) {
    return compare(a, n, e, CMP_LE, "<= expects numbers");
}
static lisp_value prim_ge(lisp_value *a, int n, const char **e) {
    return compare(a, n, e, CMP_GE, ">= expects numbers");
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
    if (argc != 1 || !lisp_is_number(args[0]))
        return prim_err(err, "zero? expects a number");
    return bool_val(lisp_number_to_double(args[0]) == 0.0);
}

// eq?: identity. Symbols/keywords are interned and immediates/fixnums are
// unboxed, so a single word compare is correct for all of them. Deep structural
// equality is equal?, below.
static lisp_value prim_eqp(lisp_value *args, int argc, const char **err) {
    if (argc != 2)
        return prim_err(err, "eq? expects two arguments");
    return bool_val(args[0] == args[1]);
}

// equal?: deep structural equality over pairs, vectors, and strings; eq? for
// everything else (interned symbols, immediates, fixnums, procedures).
static bool deep_equal(lisp_value a, lisp_value b) {
    // Iterate the cdr spine so a long flat list does not recurse per element;
    // car/elements still recurse, bounded by nesting depth rather than length.
    for (;;) {
        if (a == b)
            return true;
        if (lisp_is_pair(a) && lisp_is_pair(b)) {
            if (!deep_equal(lisp_car(a), lisp_car(b)))
                return false;
            a = lisp_cdr(a);
            b = lisp_cdr(b);
            continue;
        }
        if (lisp_is_flonum(a) && lisp_is_flonum(b))
            return lisp_flonum_val(a) == lisp_flonum_val(b);
        if (lisp_is_string(a) && lisp_is_string(b)) {
            size_t n = lisp_string_len(a);
            return n == lisp_string_len(b) &&
                   memcmp(lisp_string_data(a), lisp_string_data(b), n) == 0;
        }
        if (lisp_is_vector(a) && lisp_is_vector(b)) {
            size_t n = lisp_vector_length(a);
            if (n != lisp_vector_length(b))
                return false;
            for (size_t i = 0; i < n; i++)
                if (!deep_equal(lisp_vector_ref(a, i), lisp_vector_ref(b, i)))
                    return false;
            return true;
        }
        return false;
    }
}

static lisp_value prim_equalp(lisp_value *args, int argc, const char **err) {
    if (argc != 2)
        return prim_err(err, "equal? expects two arguments");
    return bool_val(deep_equal(args[0], args[1]));
}

// --- Type predicates --------------------------------------------------------

static lisp_value prim_symbolp(lisp_value *a, int n, const char **e) {
    if (n != 1) return prim_err(e, "symbol? expects one argument");
    return bool_val(lisp_is_symbol(a[0]));
}
static lisp_value prim_integerp(lisp_value *a, int n, const char **e) {
    if (n != 1) return prim_err(e, "integer? expects one argument");
    // Exact integers, plus integral flonums (R7RS: (integer? 2.0) => #t).
    if (lisp_is_fixnum(a[0]))
        return LISP_TRUE;
    if (lisp_is_flonum(a[0])) {
        double v = lisp_flonum_val(a[0]);
        if (v != v || v - v != 0.0)
            return LISP_FALSE;  // nan / inf
        // |v| >= 2^53 doubles have no fractional bits, so they are integral; for
        // smaller magnitudes the int64 cast is in range and exact.
        if (v >= 9.007199254740992e15 || v <= -9.007199254740992e15)
            return LISP_TRUE;
        return bool_val((double)(int64_t)v == v);
    }
    return LISP_FALSE;
}
static lisp_value prim_numberp(lisp_value *a, int n, const char **e) {
    if (n != 1) return prim_err(e, "number? expects one argument");
    return bool_val(lisp_is_number(a[0]));
}
static lisp_value prim_exactp(lisp_value *a, int n, const char **e) {
    if (n != 1 || !lisp_is_number(a[0])) return prim_err(e, "exact? expects a number");
    return bool_val(lisp_is_fixnum(a[0]));
}
static lisp_value prim_inexactp(lisp_value *a, int n, const char **e) {
    if (n != 1 || !lisp_is_number(a[0])) return prim_err(e, "inexact? expects a number");
    return bool_val(lisp_is_flonum(a[0]));
}
static lisp_value prim_exact_to_inexact(lisp_value *a, int n, const char **e) {
    if (n != 1 || !lisp_is_number(a[0])) return prim_err(e, "inexact expects a number");
    return lisp_is_flonum(a[0]) ? a[0] : lisp_make_flonum(lisp_number_to_double(a[0]));
}
static lisp_value prim_inexact_to_exact(lisp_value *a, int n, const char **e) {
    if (n != 1 || !lisp_is_number(a[0])) return prim_err(e, "exact expects a number");
    if (lisp_is_fixnum(a[0]))
        return a[0];
    double v = lisp_flonum_val(a[0]);
    if (v != v || v - v != 0.0)
        return prim_err(e, "exact: not a finite number");
    // Range-check before the cast (UB otherwise) and before fixnum() (which would
    // overflow the 62-bit tag); bignums for the rest are a later phase.
    if (v > (double)LISP_FIXNUM_MAX || v < (double)LISP_FIXNUM_MIN)
        return prim_err(e, "exact: out of fixnum range (bignums not yet implemented)");
    if ((double)(int64_t)v != v)
        return prim_err(e, "exact: not an integer-valued number");
    return lisp_fixnum((int64_t)v);
}
static lisp_value prim_booleanp(lisp_value *a, int n, const char **e) {
    if (n != 1) return prim_err(e, "boolean? expects one argument");
    return bool_val(a[0] == LISP_TRUE || a[0] == LISP_FALSE);
}
static lisp_value prim_stringp(lisp_value *a, int n, const char **e) {
    if (n != 1) return prim_err(e, "string? expects one argument");
    return bool_val(lisp_is_string(a[0]));
}
static lisp_value prim_charp(lisp_value *a, int n, const char **e) {
    if (n != 1) return prim_err(e, "char? expects one argument");
    return bool_val(lisp_is_char(a[0]));
}
static lisp_value prim_vectorp(lisp_value *a, int n, const char **e) {
    if (n != 1) return prim_err(e, "vector? expects one argument");
    return bool_val(lisp_is_vector(a[0]));
}
static lisp_value prim_procp(lisp_value *a, int n, const char **e) {
    if (n != 1) return prim_err(e, "procedure? expects one argument");
    return bool_val(lisp_is_objtype(a[0], LISP_OBJ_PRIMITIVE) ||
                    lisp_is_objtype(a[0], LISP_OBJ_CLOSURE));
}

// --- More list operations ---------------------------------------------------

static lisp_value prim_length(lisp_value *args, int argc, const char **err) {
    if (argc != 1)
        return prim_err(err, "length expects one argument");
    int64_t n = 0;
    lisp_value p = args[0];
    while (lisp_is_pair(p)) {
        n++;
        p = lisp_cdr(p);
    }
    if (!lisp_is_empty(p))
        return prim_err(err, "length: improper list");
    return lisp_fixnum(n);
}

static lisp_value prim_reverse(lisp_value *args, int argc, const char **err) {
    if (argc != 1)
        return prim_err(err, "reverse expects one argument");
    lisp_value out = LISP_EMPTY;
    lisp_value p = args[0];
    while (lisp_is_pair(p)) {
        lisp_value cell = lisp_cons(lisp_car(p), out);
        if (cell == LISP_UNDEF)
            return prim_err(err, "out of memory");
        out = cell;
        p = lisp_cdr(p);
    }
    if (!lisp_is_empty(p))
        return prim_err(err, "reverse: improper list");
    return out;
}

// Copy list `a`, with `tail` as the final cdr (used by append). Iterative so a
// long list does not recurse per element.
static lisp_value copy_with_tail(lisp_value a, lisp_value tail, const char **err) {
    lisp_value head = LISP_EMPTY;
    lisp_value last = LISP_EMPTY;
    while (lisp_is_pair(a)) {
        lisp_value cell = lisp_cons(lisp_car(a), LISP_EMPTY);
        if (cell == LISP_UNDEF)
            return prim_err(err, "out of memory");
        if (head == LISP_EMPTY)
            head = cell;
        else
            set_cdr(last, cell);
        last = cell;
        a = lisp_cdr(a);
    }
    if (!lisp_is_empty(a))
        return prim_err(err, "append: improper list");
    if (head == LISP_EMPTY)
        return tail;
    set_cdr(last, tail);
    return head;
}

static lisp_value prim_append(lisp_value *args, int argc, const char **err) {
    if (argc == 0)
        return LISP_EMPTY;
    lisp_value acc = args[argc - 1];  // last arg used as-is (may be improper)
    for (int i = argc - 2; i >= 0; i--) {
        acc = copy_with_tail(args[i], acc, err);
        if (acc == LISP_UNDEF && err != NULL && *err != NULL)
            return LISP_UNDEF;
    }
    return acc;
}

static lisp_value prim_list_ref(lisp_value *args, int argc, const char **err) {
    if (argc != 2 || !lisp_is_fixnum(args[1]))
        return prim_err(err, "list-ref expects a list and an index");
    int64_t k = lisp_fixnum_val(args[1]);
    if (k < 0)
        return prim_err(err, "list-ref: negative index");
    lisp_value p = args[0];
    while (k > 0 && lisp_is_pair(p)) {
        p = lisp_cdr(p);
        k--;
    }
    if (!lisp_is_pair(p))
        return prim_err(err, "list-ref: index out of range");
    return lisp_car(p);
}

// --- Vectors ----------------------------------------------------------------

static lisp_value prim_vector(lisp_value *args, int argc, const char **err) {
    lisp_value v = lisp_make_vector((size_t)argc, LISP_UNDEF);
    if (v == LISP_UNDEF)
        return prim_err(err, "out of memory");
    for (int i = 0; i < argc; i++)
        lisp_vector_set_init(v, (size_t)i, args[i]);
    return v;
}

static lisp_value prim_make_vector(lisp_value *args, int argc, const char **err) {
    if (argc < 1 || argc > 2 || !lisp_is_fixnum(args[0]))
        return prim_err(err, "make-vector expects a length and optional fill");
    int64_t n = lisp_fixnum_val(args[0]);
    if (n < 0)
        return prim_err(err, "make-vector: negative length");
    lisp_value fill = argc == 2 ? args[1] : LISP_FALSE;
    lisp_value v = lisp_make_vector((size_t)n, fill);
    if (v == LISP_UNDEF)
        return prim_err(err, "out of memory");
    return v;
}

static lisp_value prim_vector_ref(lisp_value *args, int argc, const char **err) {
    if (argc != 2 || !lisp_is_vector(args[0]) || !lisp_is_fixnum(args[1]))
        return prim_err(err, "vector-ref expects a vector and an index");
    int64_t i = lisp_fixnum_val(args[1]);
    if (i < 0 || (size_t)i >= lisp_vector_length(args[0]))
        return prim_err(err, "vector-ref: index out of range");
    return lisp_vector_ref(args[0], (size_t)i);
}

static lisp_value prim_vector_length(lisp_value *args, int argc, const char **err) {
    if (argc != 1 || !lisp_is_vector(args[0]))
        return prim_err(err, "vector-length expects a vector");
    return lisp_fixnum((int64_t)lisp_vector_length(args[0]));
}

static lisp_value prim_vector_to_list(lisp_value *args, int argc, const char **err) {
    if (argc != 1 || !lisp_is_vector(args[0]))
        return prim_err(err, "vector->list expects a vector");
    size_t n = lisp_vector_length(args[0]);
    lisp_value out = LISP_EMPTY;
    for (size_t i = n; i > 0; i--) {
        lisp_value cell = lisp_cons(lisp_vector_ref(args[0], i - 1), out);
        if (cell == LISP_UNDEF)
            return prim_err(err, "out of memory");
        out = cell;
    }
    return out;
}

static lisp_value prim_list_to_vector(lisp_value *args, int argc, const char **err) {
    if (argc != 1)
        return prim_err(err, "list->vector expects one argument");
    int64_t n = 0;
    for (lisp_value p = args[0]; lisp_is_pair(p); p = lisp_cdr(p))
        n++;
    lisp_value v = lisp_make_vector((size_t)n, LISP_UNDEF);
    if (v == LISP_UNDEF)
        return prim_err(err, "out of memory");
    size_t i = 0;
    for (lisp_value p = args[0]; lisp_is_pair(p); p = lisp_cdr(p))
        lisp_vector_set_init(v, i++, lisp_car(p));
    return v;
}

// --- Strings ----------------------------------------------------------------

static lisp_value prim_string_length(lisp_value *a, int n, const char **e) {
    if (n != 1 || !lisp_is_string(a[0]))
        return prim_err(e, "string-length expects a string");
    return lisp_fixnum((int64_t)lisp_string_len(a[0]));
}

static lisp_value prim_string_ref(lisp_value *a, int n, const char **e) {
    if (n != 2 || !lisp_is_string(a[0]) || !lisp_is_fixnum(a[1]))
        return prim_err(e, "string-ref expects a string and an index");
    int64_t i = lisp_fixnum_val(a[1]);
    if (i < 0 || (size_t)i >= lisp_string_len(a[0]))
        return prim_err(e, "string-ref: index out of range");
    return lisp_char((uint8_t)lisp_string_data(a[0])[i]);
}

static lisp_value prim_substring(lisp_value *a, int n, const char **e) {
    if (n != 3 || !lisp_is_string(a[0]) || !lisp_is_fixnum(a[1]) || !lisp_is_fixnum(a[2]))
        return prim_err(e, "substring expects a string and two indices");
    int64_t start = lisp_fixnum_val(a[1]), end = lisp_fixnum_val(a[2]);
    size_t len = lisp_string_len(a[0]);
    if (start < 0 || end < start || (size_t)end > len)
        return prim_err(e, "substring: indices out of range");
    return lisp_make_string(lisp_string_data(a[0]) + start, (size_t)(end - start));
}

static lisp_value prim_string_append(lisp_value *args, int argc, const char **err) {
    size_t total = 0;
    for (int i = 0; i < argc; i++) {
        if (!lisp_is_string(args[i]))
            return prim_err(err, "string-append expects strings");
        total += lisp_string_len(args[i]);
    }
    char *tmp = (char *)malloc(total ? total : 1);
    if (tmp == NULL)
        return prim_err(err, "out of memory");
    size_t off = 0;
    for (int i = 0; i < argc; i++) {
        size_t l = lisp_string_len(args[i]);
        memcpy(tmp + off, lisp_string_data(args[i]), l);
        off += l;
    }
    lisp_value r = lisp_make_string(tmp, total);
    free(tmp);
    return r == LISP_UNDEF ? prim_err(err, "out of memory") : r;
}

// Lexicographic compare of two strings: <0, 0, >0.
static int string_cmp(lisp_value x, lisp_value y) {
    size_t lx = lisp_string_len(x), ly = lisp_string_len(y);
    size_t m = lx < ly ? lx : ly;
    int c = memcmp(lisp_string_data(x), lisp_string_data(y), m);
    if (c != 0)
        return c;
    return lx < ly ? -1 : (lx > ly ? 1 : 0);
}

static lisp_value prim_string_eq(lisp_value *a, int n, const char **e) {
    for (int i = 0; i < n; i++)
        if (!lisp_is_string(a[i]))
            return prim_err(e, "string=? expects strings");
    for (int i = 0; i + 1 < n; i++)
        if (string_cmp(a[i], a[i + 1]) != 0)
            return LISP_FALSE;
    return LISP_TRUE;
}

static lisp_value prim_string_lt(lisp_value *a, int n, const char **e) {
    for (int i = 0; i < n; i++)
        if (!lisp_is_string(a[i]))
            return prim_err(e, "string<? expects strings");
    for (int i = 0; i + 1 < n; i++)
        if (string_cmp(a[i], a[i + 1]) >= 0)
            return LISP_FALSE;
    return LISP_TRUE;
}

static lisp_value prim_string_to_list(lisp_value *a, int n, const char **e) {
    if (n != 1 || !lisp_is_string(a[0]))
        return prim_err(e, "string->list expects a string");
    const char *d = lisp_string_data(a[0]);
    size_t len = lisp_string_len(a[0]);
    lisp_value out = LISP_EMPTY;
    for (size_t i = len; i > 0; i--) {
        lisp_value cell = lisp_cons(lisp_char((uint8_t)d[i - 1]), out);
        if (cell == LISP_UNDEF)
            return prim_err(e, "out of memory");
        out = cell;
    }
    return out;
}

// Collect a list/array of char values into a freshly-allocated string. `list`
// (when not LISP_UNDEF) must be a proper list of characters; a non-list or
// improper/non-char element is an error (not silently truncated).
static lisp_value chars_to_string(lisp_value *items, int count, lisp_value list,
                                  const char **err) {
    size_t n = (size_t)count;
    if (list != LISP_UNDEF) {
        lisp_value p = list;
        for (; lisp_is_pair(p); p = lisp_cdr(p))
            n++;
        if (!lisp_is_empty(p))
            return prim_err(err, "list->string: not a proper list of characters");
    }
    char *tmp = (char *)malloc(n ? n : 1);
    if (tmp == NULL)
        return prim_err(err, "out of memory");
    size_t idx = 0;
    for (int i = 0; i < count; i++) {
        if (!lisp_is_char(items[i])) {
            free(tmp);
            return prim_err(err, "expected characters");
        }
        tmp[idx++] = (char)lisp_char_val(items[i]);
    }
    if (list != LISP_UNDEF)
        for (lisp_value p = list; lisp_is_pair(p); p = lisp_cdr(p)) {
            if (!lisp_is_char(lisp_car(p))) {
                free(tmp);
                return prim_err(err, "list->string: expected characters");
            }
            tmp[idx++] = (char)lisp_char_val(lisp_car(p));
        }
    lisp_value r = lisp_make_string(tmp, n);
    free(tmp);
    return r == LISP_UNDEF ? prim_err(err, "out of memory") : r;
}

static lisp_value prim_list_to_string(lisp_value *a, int n, const char **e) {
    if (n != 1)
        return prim_err(e, "list->string expects one argument");
    return chars_to_string(NULL, 0, a[0], e);
}

static lisp_value prim_string(lisp_value *a, int n, const char **e) {
    return chars_to_string(a, n, LISP_UNDEF, e);
}

static lisp_value prim_make_string(lisp_value *a, int n, const char **e) {
    if (n < 1 || n > 2 || !lisp_is_fixnum(a[0]))
        return prim_err(e, "make-string expects a length and optional char");
    int64_t len = lisp_fixnum_val(a[0]);
    if (len < 0)
        return prim_err(e, "make-string: negative length");
    char fill = ' ';
    if (n == 2) {
        if (!lisp_is_char(a[1]))
            return prim_err(e, "make-string: fill must be a character");
        fill = (char)lisp_char_val(a[1]);
    }
    char *tmp = (char *)malloc(len ? (size_t)len : 1);
    if (tmp == NULL)
        return prim_err(e, "out of memory");
    for (int64_t i = 0; i < len; i++)
        tmp[i] = fill;
    lisp_value r = lisp_make_string(tmp, (size_t)len);
    free(tmp);
    return r == LISP_UNDEF ? prim_err(e, "out of memory") : r;
}

static lisp_value prim_symbol_to_string(lisp_value *a, int n, const char **e) {
    if (n != 1 || !lisp_is_symbol(a[0]))
        return prim_err(e, "symbol->string expects a symbol");
    return lisp_make_string(lisp_named_name(a[0]), lisp_named_len(a[0]));
}

static lisp_value prim_string_to_symbol(lisp_value *a, int n, const char **e) {
    if (n != 1 || !lisp_is_string(a[0]))
        return prim_err(e, "string->symbol expects a string");
    return lisp_make_symbol(lisp_string_data(a[0]), lisp_string_len(a[0]));
}

static lisp_value prim_number_to_string(lisp_value *a, int n, const char **e) {
    if (n != 1 || !lisp_is_number(a[0]))
        return prim_err(e, "number->string expects a number");
    char buf[512];
    size_t len = lisp_print(a[0], buf, sizeof(buf));
    if (len >= sizeof(buf))
        len = sizeof(buf) - 1;
    return lisp_make_string(buf, len);
}

static lisp_value prim_string_to_number(lisp_value *a, int n, const char **e) {
    if (n != 1 || !lisp_is_string(a[0]))
        return prim_err(e, "string->number expects a string");
    const char *cur = lisp_string_data(a[0]);
    const char *end = cur + lisp_string_len(a[0]);
    const char *rerr = NULL;
    lisp_value v = lisp_read(&cur, end, &rerr);
    if (v == LISP_UNDEF || v == LISP_EOF || !lisp_is_number(v))
        return LISP_FALSE;  // not a number -> #f (per R7RS), not an error
    while (cur < end && (*cur == ' ' || *cur == '\t' || *cur == '\n' || *cur == '\r'))
        cur++;
    return cur == end ? v : LISP_FALSE;  // the whole string must be the number
}

// --- Characters -------------------------------------------------------------

static lisp_value prim_char_to_integer(lisp_value *a, int n, const char **e) {
    if (n != 1 || !lisp_is_char(a[0]))
        return prim_err(e, "char->integer expects a character");
    return lisp_fixnum((int64_t)lisp_char_val(a[0]));
}

static lisp_value prim_integer_to_char(lisp_value *a, int n, const char **e) {
    if (n != 1 || !lisp_is_fixnum(a[0]))
        return prim_err(e, "integer->char expects an integer");
    int64_t cp = lisp_fixnum_val(a[0]);
    if (cp < 0 || cp > 0x10FFFF)
        return prim_err(e, "integer->char: out of range");
    return lisp_char((uint32_t)cp);
}

typedef enum { CC_EQ, CC_LT, CC_GT, CC_LE, CC_GE } char_cmp;

static lisp_value char_compare(lisp_value *a, int n, const char **e, char_cmp op,
                               const char *name) {
    for (int i = 0; i < n; i++)
        if (!lisp_is_char(a[i]))
            return prim_err(e, name);
    for (int i = 0; i + 1 < n; i++) {
        uint32_t x = lisp_char_val(a[i]), y = lisp_char_val(a[i + 1]);
        bool ok;
        switch (op) {
            case CC_EQ: ok = x == y; break;
            case CC_LT: ok = x < y; break;
            case CC_GT: ok = x > y; break;
            case CC_LE: ok = x <= y; break;
            case CC_GE: ok = x >= y; break;
        }
        if (!ok)
            return LISP_FALSE;
    }
    return LISP_TRUE;
}

static lisp_value prim_char_eq(lisp_value *a, int n, const char **e) {
    return char_compare(a, n, e, CC_EQ, "char=? expects characters");
}
static lisp_value prim_char_lt(lisp_value *a, int n, const char **e) {
    return char_compare(a, n, e, CC_LT, "char<? expects characters");
}
static lisp_value prim_char_gt(lisp_value *a, int n, const char **e) {
    return char_compare(a, n, e, CC_GT, "char>? expects characters");
}
static lisp_value prim_char_le(lisp_value *a, int n, const char **e) {
    return char_compare(a, n, e, CC_LE, "char<=? expects characters");
}
static lisp_value prim_char_ge(lisp_value *a, int n, const char **e) {
    return char_compare(a, n, e, CC_GE, "char>=? expects characters");
}

static lisp_value prim_char_upcase(lisp_value *a, int n, const char **e) {
    if (n != 1 || !lisp_is_char(a[0]))
        return prim_err(e, "char-upcase expects a character");
    uint32_t c = lisp_char_val(a[0]);
    if (c >= 'a' && c <= 'z')
        c -= 32;
    return lisp_char(c);
}
static lisp_value prim_char_downcase(lisp_value *a, int n, const char **e) {
    if (n != 1 || !lisp_is_char(a[0]))
        return prim_err(e, "char-downcase expects a character");
    uint32_t c = lisp_char_val(a[0]);
    if (c >= 'A' && c <= 'Z')
        c += 32;
    return lisp_char(c);
}

static lisp_value prim_char_alphabeticp(lisp_value *a, int n, const char **e) {
    if (n != 1 || !lisp_is_char(a[0]))
        return prim_err(e, "char-alphabetic? expects a character");
    uint32_t c = lisp_char_val(a[0]);
    return bool_val((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'));
}
static lisp_value prim_char_numericp(lisp_value *a, int n, const char **e) {
    if (n != 1 || !lisp_is_char(a[0]))
        return prim_err(e, "char-numeric? expects a character");
    uint32_t c = lisp_char_val(a[0]);
    return bool_val(c >= '0' && c <= '9');
}
static lisp_value prim_char_whitespacep(lisp_value *a, int n, const char **e) {
    if (n != 1 || !lisp_is_char(a[0]))
        return prim_err(e, "char-whitespace? expects a character");
    uint32_t c = lisp_char_val(a[0]);
    return bool_val(c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f');
}

// --- Higher-order -----------------------------------------------------------

#define PRIM_MAX_ARGS 64

static lisp_value prim_apply(lisp_value *args, int argc, const char **err) {
    if (argc < 2)
        return prim_err(err, "apply expects a procedure and an argument list");
    lisp_value last = args[argc - 1];
    int extra = 0;
    lisp_value p = last;
    for (; lisp_is_pair(p); p = lisp_cdr(p))
        extra++;
    if (!lisp_is_empty(p))
        return prim_err(err, "apply: last argument must be a proper list");
    int total = (argc - 2) + extra;
    if (total > PRIM_MAX_ARGS)
        return prim_err(err, "apply: too many arguments");
    lisp_value call[PRIM_MAX_ARGS];
    int idx = 0;
    for (int i = 1; i < argc - 1; i++)
        call[idx++] = args[i];
    for (lisp_value p = last; lisp_is_pair(p); p = lisp_cdr(p))
        call[idx++] = lisp_car(p);
    return lisp_apply(args[0], call, total, err);
}

static lisp_value prim_map(lisp_value *args, int argc, const char **err) {
    if (argc != 2)
        return prim_err(err, "map expects a procedure and one list");
    // Build the result forward (head/tail) rather than recursing per element --
    // each step also nests a full lisp_eval, so recursion would blow the kernel
    // stack on modest lists.
    lisp_value head = LISP_EMPTY;
    lisp_value last = LISP_EMPTY;
    lisp_value lst = args[1];
    while (lisp_is_pair(lst)) {
        lisp_value arg = lisp_car(lst);
        lisp_value r = lisp_apply(args[0], &arg, 1, err);
        if (r == LISP_UNDEF && err != NULL && *err != NULL)
            return LISP_UNDEF;
        lisp_value cell = lisp_cons(r, LISP_EMPTY);
        if (cell == LISP_UNDEF)
            return prim_err(err, "out of memory");
        if (head == LISP_EMPTY)
            head = cell;
        else
            set_cdr(last, cell);
        last = cell;
        lst = lisp_cdr(lst);
    }
    if (!lisp_is_empty(lst))
        return prim_err(err, "map: improper list");
    return head;
}

static lisp_value prim_for_each(lisp_value *args, int argc, const char **err) {
    if (argc != 2)
        return prim_err(err, "for-each expects a procedure and one list");
    lisp_value lst = args[1];
    while (lisp_is_pair(lst)) {
        lisp_value arg = lisp_car(lst);
        lisp_value r = lisp_apply(args[0], &arg, 1, err);
        if (r == LISP_UNDEF && err != NULL && *err != NULL)
            return LISP_UNDEF;
        lst = lisp_cdr(lst);
    }
    if (!lisp_is_empty(lst))
        return prim_err(err, "for-each: improper list");
    return LISP_UNDEF;
}

// --- Output (display / write / newline) -------------------------------------

static lisp_output_fn g_out = NULL;
static void *g_out_ctx = NULL;

void lisp_set_output(lisp_output_fn fn, void *ctx) {
    g_out = fn;
    g_out_ctx = ctx;
}

static void out(const char *s, size_t len) {
    if (g_out != NULL)
        g_out(s, len, g_out_ctx);
}

// Render `v` (write or display form) and push it to the output sink, growing past
// the stack buffer if needed.
static lisp_value emit_value(lisp_value v, bool readable, const char **err) {
    char buf[256];
    size_t n = readable ? lisp_print(v, buf, sizeof(buf)) : lisp_display(v, buf, sizeof(buf));
    if (n < sizeof(buf)) {
        out(buf, n);
    } else {
        char *big = (char *)malloc(n + 1);
        if (big == NULL)
            return prim_err(err, "out of memory");
        if (readable)
            lisp_print(v, big, n + 1);
        else
            lisp_display(v, big, n + 1);
        out(big, n);
        free(big);
    }
    return LISP_UNDEF;  // unspecified
}

static lisp_value prim_display(lisp_value *a, int n, const char **e) {
    if (n != 1)
        return prim_err(e, "display expects one argument");
    return emit_value(a[0], false, e);
}
static lisp_value prim_write(lisp_value *a, int n, const char **e) {
    if (n != 1)
        return prim_err(e, "write expects one argument");
    return emit_value(a[0], true, e);
}
static lisp_value prim_newline(lisp_value *a, int n, const char **e) {
    (void)a;
    if (n != 0)
        return prim_err(e, "newline expects no arguments");
    out("\n", 1);
    return LISP_UNDEF;
}

// (error message irritants...) signals an error carrying the message. Without a
// condition/guard system this simply aborts evaluation with the message (the
// data is NUL-terminated; the caller reads *err before any further allocation).
static lisp_value prim_error(lisp_value *a, int n, const char **e) {
    if (n < 1)
        return prim_err(e, "error: a message argument is required");
    if (lisp_is_string(a[0]))
        return prim_err(e, lisp_string_data(a[0]));
    return prim_err(e, "error");
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
    def(env, "remainder", prim_remainder);
    def(env, "quotient", prim_quotient);
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
    def(env, "equal?", prim_equalp);
    // Type predicates
    def(env, "symbol?", prim_symbolp);
    def(env, "integer?", prim_integerp);
    def(env, "number?", prim_numberp);
    def(env, "real?", prim_numberp);  // all our numbers are real
    def(env, "exact?", prim_exactp);
    def(env, "inexact?", prim_inexactp);
    def(env, "exact->inexact", prim_exact_to_inexact);
    def(env, "inexact", prim_exact_to_inexact);
    def(env, "inexact->exact", prim_inexact_to_exact);
    def(env, "exact", prim_inexact_to_exact);
    def(env, "boolean?", prim_booleanp);
    def(env, "string?", prim_stringp);
    def(env, "char?", prim_charp);
    def(env, "vector?", prim_vectorp);
    def(env, "procedure?", prim_procp);
    // List library
    def(env, "length", prim_length);
    def(env, "reverse", prim_reverse);
    def(env, "append", prim_append);
    def(env, "list-ref", prim_list_ref);
    // Vectors
    def(env, "vector", prim_vector);
    def(env, "make-vector", prim_make_vector);
    def(env, "vector-ref", prim_vector_ref);
    def(env, "vector-length", prim_vector_length);
    def(env, "vector->list", prim_vector_to_list);
    def(env, "list->vector", prim_list_to_vector);
    // Strings
    def(env, "string-length", prim_string_length);
    def(env, "string-ref", prim_string_ref);
    def(env, "substring", prim_substring);
    def(env, "string-append", prim_string_append);
    def(env, "string=?", prim_string_eq);
    def(env, "string<?", prim_string_lt);
    def(env, "string->list", prim_string_to_list);
    def(env, "list->string", prim_list_to_string);
    def(env, "string", prim_string);
    def(env, "make-string", prim_make_string);
    def(env, "symbol->string", prim_symbol_to_string);
    def(env, "string->symbol", prim_string_to_symbol);
    def(env, "number->string", prim_number_to_string);
    def(env, "string->number", prim_string_to_number);
    // Characters
    def(env, "char->integer", prim_char_to_integer);
    def(env, "integer->char", prim_integer_to_char);
    def(env, "char=?", prim_char_eq);
    def(env, "char<?", prim_char_lt);
    def(env, "char>?", prim_char_gt);
    def(env, "char<=?", prim_char_le);
    def(env, "char>=?", prim_char_ge);
    def(env, "char-upcase", prim_char_upcase);
    def(env, "char-downcase", prim_char_downcase);
    def(env, "char-alphabetic?", prim_char_alphabeticp);
    def(env, "char-numeric?", prim_char_numericp);
    def(env, "char-whitespace?", prim_char_whitespacep);
    // Higher-order
    def(env, "apply", prim_apply);
    def(env, "map", prim_map);
    def(env, "for-each", prim_for_each);
    // Output
    def(env, "display", prim_display);
    def(env, "write", prim_write);
    def(env, "newline", prim_newline);
    def(env, "error", prim_error);
}
