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

#include "internal.h"  // lisp_apply_reuse for the higher-order primitives
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
    int64_t acc = 0;  // single fixnum pass; bail to the flonum path on the first non-fixnum
    for (int i = 0; i < argc; i++) {
        if (!lisp_is_fixnum(args[i]))
            goto inexact;
        acc += lisp_fixnum_val(args[i]);
    }
    return lisp_fixnum(acc);
inexact:
    if (!all_number(args, argc))
        return prim_err(err, "+ expects numbers");
    double dacc = 0.0;
    for (int i = 0; i < argc; i++)
        dacc += lisp_number_to_double(args[i]);
    return lisp_make_flonum(dacc);
}

static lisp_value prim_mul(lisp_value *args, int argc, const char **err) {
    int64_t acc = 1;
    for (int i = 0; i < argc; i++) {
        if (!lisp_is_fixnum(args[i]))
            goto inexact;
        acc *= lisp_fixnum_val(args[i]);
    }
    return lisp_fixnum(acc);
inexact:
    if (!all_number(args, argc))
        return prim_err(err, "* expects numbers");
    double dacc = 1.0;
    for (int i = 0; i < argc; i++)
        dacc *= lisp_number_to_double(args[i]);
    return lisp_make_flonum(dacc);
}

static lisp_value prim_sub(lisp_value *args, int argc, const char **err) {
    if (argc < 1)
        return prim_err(err, "- expects at least one argument");
    if (lisp_is_fixnum(args[0])) {
        int64_t acc = lisp_fixnum_val(args[0]);
        int i = 1;
        for (; i < argc; i++) {
            if (!lisp_is_fixnum(args[i]))
                goto inexact;
            acc -= lisp_fixnum_val(args[i]);
        }
        return lisp_fixnum(argc == 1 ? -acc : acc);
    }
inexact:
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
        if (lisp_is_bytes(a) && lisp_is_bytes(b)) {
            size_t n = lisp_bytes_len(a);
            return n == lisp_bytes_len(b) &&
                   memcmp(lisp_bytes_data(a), lisp_bytes_data(b), n) == 0;
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
        // Capability/handle types (contexts, grants) have no structural equality:
        // identical handles already matched the eq? short-circuit above, and two
        // DISTINCT handles are intentionally not equal? -- they are identities, not
        // data. They land here and correctly return false.
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
    return bool_val(lisp_is_procedure(a[0]));
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

static lisp_value prim_vector_set(lisp_value *args, int argc, const char **err) {
    if (argc != 3 || !lisp_is_vector(args[0]) || !lisp_is_fixnum(args[1]))
        return prim_err(err, "vector-set! expects (vector index value)");
    int64_t i = lisp_fixnum_val(args[1]);
    if (i < 0 || (size_t)i >= lisp_vector_length(args[0]))
        return prim_err(err, "vector-set!: index out of range");
    lisp_vector_set(args[0], (size_t)i, args[2]);
    return LISP_UNDEF;
}

static lisp_value prim_vector_fill(lisp_value *args, int argc, const char **err) {
    if (argc != 2 || !lisp_is_vector(args[0]))
        return prim_err(err, "vector-fill! expects (vector value)");
    size_t n = lisp_vector_length(args[0]);
    for (size_t i = 0; i < n; i++)
        lisp_vector_set(args[0], i, args[1]);
    return LISP_UNDEF;
}

// (vector-copy! dst doff src soff len) -- copy len elements; ranges checked. Like
// bytes-copy! this is the bulk mover; overlap within one vector is handled by
// choosing copy direction so a self-shift does not clobber unread source slots.
static lisp_value prim_vector_copy(lisp_value *args, int argc, const char **err) {
    if (argc != 5 || !lisp_is_vector(args[0]) || !lisp_is_fixnum(args[1]) ||
        !lisp_is_vector(args[2]) || !lisp_is_fixnum(args[3]) || !lisp_is_fixnum(args[4]))
        return prim_err(err, "vector-copy! expects (dst doff src soff len)");
    int64_t doff = lisp_fixnum_val(args[1]), soff = lisp_fixnum_val(args[3]);
    int64_t len = lisp_fixnum_val(args[4]);
    if (doff < 0 || soff < 0 || len < 0 ||
        (size_t)(doff + len) > lisp_vector_length(args[0]) ||
        (size_t)(soff + len) > lisp_vector_length(args[2]))
        return prim_err(err, "vector-copy!: range out of bounds");
    if (doff > soff)  // overlapping forward shift: copy back-to-front
        for (int64_t k = len - 1; k >= 0; k--)
            lisp_vector_set(args[0], (size_t)(doff + k), lisp_vector_ref(args[2], (size_t)(soff + k)));
    else
        for (int64_t k = 0; k < len; k++)
            lisp_vector_set(args[0], (size_t)(doff + k), lisp_vector_ref(args[2], (size_t)(soff + k)));
    return LISP_UNDEF;
}

// --- Hash tables (equal?-keyed, chained buckets) ----------------------------
//
// Keys are compared with deep_equal (the equal? predicate) and hashed by content
// with a matching structural hash, so lists/strings/bytes/numbers all work as
// keys. The table grows (doubles its bucket count) when the load factor reaches
// 1.0, keeping average chains short. All mutation is in place; the table is
// deep-copied on send (sched.c) so nothing mutable crosses a context boundary.

#define FNV64_OFFSET 1469598103934665603ull
#define FNV64_PRIME 1099511628211ull
#define EHASH_MAX_DEPTH 6   // recursion cap: equal values still hash equally
#define EHASH_MAX_SPAN 64   // per-list/-vector element cap (same justification)

static uint64_t hash_mem(const uint8_t *p, size_t n, uint64_t h) {
    for (size_t i = 0; i < n; i++)
        h = (h ^ p[i]) * FNV64_PRIME;
    return h;
}

// A content hash consistent with deep_equal: equal values hash identically. The
// depth/span caps bound the work on deep or long structures; because they apply
// identically to structurally-equal values, equal keys still collide into the
// same bucket (the only correctness requirement -- extra collisions just lengthen
// a chain).
static uint64_t equal_hash(lisp_value v, int depth) {
    if (lisp_is_fixnum(v)) {
        int64_t x = lisp_fixnum_val(v);
        return hash_mem((const uint8_t *)&x, sizeof x, FNV64_OFFSET);
    }
    if (!lisp_is_ptr(v)) {  // char / bool / () / eof / undef: hash the tagged word
        uint64_t x = (uint64_t)v;
        return hash_mem((const uint8_t *)&x, sizeof x, FNV64_OFFSET);
    }
    switch (LISP_HDR_TYPE(lisp_obj(v))) {
        case LISP_OBJ_SYMBOL:
        case LISP_OBJ_KEYWORD: {
            // Interned: equal? is identity, and the stored 32-bit hash is the name
            // hash -- a stable content key either way.
            uint32_t nh = ((lisp_named *)lisp_obj(v))->hash;
            uint64_t x = nh;
            return hash_mem((const uint8_t *)&x, sizeof x, FNV64_OFFSET ^ 0x53u);
        }
        case LISP_OBJ_STRING:
            return hash_mem((const uint8_t *)lisp_string_data(v), lisp_string_len(v),
                            FNV64_OFFSET ^ 0x05u);
        case LISP_OBJ_BYTES:
            return hash_mem((const uint8_t *)lisp_bytes_data(v), lisp_bytes_len(v),
                            FNV64_OFFSET ^ 0x07u);
        case LISP_OBJ_FLONUM: {
            double d = lisp_flonum_val(v);
            return hash_mem((const uint8_t *)&d, sizeof d, FNV64_OFFSET ^ 0x09u);
        }
        case LISP_OBJ_PAIR: {
            uint64_t h = FNV64_OFFSET ^ 0x11u;
            int n = 0;
            while (lisp_is_pair(v) && n < EHASH_MAX_SPAN) {
                uint64_t ch = depth >= EHASH_MAX_DEPTH ? 0xA5u : equal_hash(lisp_car(v), depth + 1);
                h = (h ^ ch) * FNV64_PRIME;
                v = lisp_cdr(v);
                n++;
            }
            if (lisp_is_ptr(v)) {  // dotted tail or an over-long spine remnant
                uint64_t ch = depth >= EHASH_MAX_DEPTH ? 0x5Au : equal_hash(v, depth + 1);
                h = (h ^ ch) * FNV64_PRIME;
            }
            return h;
        }
        case LISP_OBJ_VECTOR: {
            uint64_t h = FNV64_OFFSET ^ 0x13u;
            size_t n = lisp_vector_length(v);
            if (n > EHASH_MAX_SPAN)
                n = EHASH_MAX_SPAN;
            for (size_t i = 0; i < n; i++) {
                uint64_t ch = depth >= EHASH_MAX_DEPTH ? 0x3Cu : equal_hash(lisp_vector_ref(v, i), depth + 1);
                h = (h ^ ch) * FNV64_PRIME;
            }
            return h;
        }
        default: {  // procedures/contexts: by identity (rarely used as keys)
            uint64_t x = (uint64_t)v;
            return hash_mem((const uint8_t *)&x, sizeof x, FNV64_OFFSET);
        }
    }
}

// Address of the bucket-list slot for `key` inside the table's buckets vector.
static lisp_value *ht_slot(lisp_value ht, lisp_value key) {
    lisp_value bk = lisp_hashtable_buckets(ht);
    size_t nb = lisp_vector_length(bk);
    size_t i = (size_t)(equal_hash(key, 0) % (uint64_t)nb);
    return &((lisp_vector *)lisp_obj(bk))->items[i];
}

// The (key . value) pair for `key`, or LISP_FALSE if absent. A real entry is a
// pair (a heap pointer), never == LISP_FALSE, so the sentinel is unambiguous.
static lisp_value ht_find(lisp_value ht, lisp_value key) {
    for (lisp_value b = *ht_slot(ht, key); lisp_is_pair(b); b = lisp_cdr(b)) {
        lisp_value pr = lisp_car(b);
        if (deep_equal(lisp_car(pr), key))
            return pr;
    }
    return LISP_FALSE;
}

// Double the bucket count and rehash. Best-effort: on OOM it leaves the table
// untouched (the old buckets stay installed) and returns false; the caller then
// just inserts into the existing, denser table. No entry is ever lost.
static bool ht_grow(lisp_value ht) {
    lisp_value oldbk = lisp_hashtable_buckets(ht);
    size_t oldn = lisp_vector_length(oldbk);
    size_t newn = oldn * 2;
    lisp_value newbk = lisp_make_vector(newn, LISP_EMPTY);
    if (newbk == LISP_UNDEF)
        return false;
    for (size_t i = 0; i < oldn; i++) {
        for (lisp_value b = lisp_vector_ref(oldbk, i); lisp_is_pair(b); b = lisp_cdr(b)) {
            lisp_value pr = lisp_car(b);  // reuse the existing entry pair
            size_t j = (size_t)(equal_hash(lisp_car(pr), 0) % (uint64_t)newn);
            lisp_value cell = lisp_cons(pr, lisp_vector_ref(newbk, j));
            if (cell == LISP_UNDEF)
                return false;  // oldbk still installed and whole; abandon newbk
            lisp_vector_set(newbk, j, cell);
        }
    }
    lisp_hashtable_set_buckets(ht, newbk);
    return true;
}

static lisp_value prim_make_hashtable(lisp_value *args, int argc, const char **err) {
    (void)args;
    if (argc != 0)
        return prim_err(err, "make-hash-table takes no arguments");
    lisp_value ht = lisp_make_hashtable(8);
    if (ht == LISP_UNDEF)
        return prim_err(err, "out of memory");
    return ht;
}

static lisp_value prim_hashtablep(lisp_value *args, int argc, const char **err) {
    if (argc != 1)
        return prim_err(err, "hash-table? expects one argument");
    return bool_val(lisp_is_hashtable(args[0]));
}

static lisp_value prim_hash_set(lisp_value *args, int argc, const char **err) {
    if (argc != 3 || !lisp_is_hashtable(args[0]))
        return prim_err(err, "hash-set! expects (hash-table key value)");
    lisp_value ht = args[0], key = args[1], val = args[2];
    lisp_value pr = ht_find(ht, key);
    if (pr != LISP_FALSE) {  // present: overwrite in place
        set_cdr(pr, val);
        return LISP_UNDEF;
    }
    if (lisp_hashtable_count(ht) >= lisp_vector_length(lisp_hashtable_buckets(ht)))
        ht_grow(ht);  // best-effort; ht_slot below reads whatever buckets remain
    lisp_value entry = lisp_cons(key, val);
    if (entry == LISP_UNDEF)
        return prim_err(err, "out of memory");
    lisp_value *slot = ht_slot(ht, key);
    lisp_value cell = lisp_cons(entry, *slot);
    if (cell == LISP_UNDEF)
        return prim_err(err, "out of memory");
    *slot = cell;
    lisp_hashtable_set_count(ht, lisp_hashtable_count(ht) + 1);
    return LISP_UNDEF;
}

static lisp_value prim_hash_ref(lisp_value *args, int argc, const char **err) {
    if (argc < 2 || argc > 3 || !lisp_is_hashtable(args[0]))
        return prim_err(err, "hash-ref expects (hash-table key [default])");
    lisp_value pr = ht_find(args[0], args[1]);
    if (pr != LISP_FALSE)
        return lisp_cdr(pr);
    if (argc == 3)
        return args[2];
    return prim_err(err, "hash-ref: key not found");
}

static lisp_value prim_hash_has_key(lisp_value *args, int argc, const char **err) {
    if (argc != 2 || !lisp_is_hashtable(args[0]))
        return prim_err(err, "hash-has-key? expects (hash-table key)");
    return bool_val(ht_find(args[0], args[1]) != LISP_FALSE);
}

static lisp_value prim_hash_remove(lisp_value *args, int argc, const char **err) {
    if (argc != 2 || !lisp_is_hashtable(args[0]))
        return prim_err(err, "hash-remove! expects (hash-table key)");
    lisp_value ht = args[0], key = args[1];
    lisp_value *slot = ht_slot(ht, key);
    lisp_value prev = LISP_FALSE;
    for (lisp_value b = *slot; lisp_is_pair(b); prev = b, b = lisp_cdr(b)) {
        if (deep_equal(lisp_car(lisp_car(b)), key)) {
            if (prev == LISP_FALSE)
                *slot = lisp_cdr(b);  // unlink head
            else
                set_cdr(prev, lisp_cdr(b));
            lisp_hashtable_set_count(ht, lisp_hashtable_count(ht) - 1);
            return LISP_TRUE;
        }
    }
    return LISP_FALSE;
}

static lisp_value prim_hash_count(lisp_value *args, int argc, const char **err) {
    if (argc != 1 || !lisp_is_hashtable(args[0]))
        return prim_err(err, "hash-count expects a hash-table");
    return lisp_fixnum((int64_t)lisp_hashtable_count(args[0]));
}

// Build a list by walking every bucket; `pick` selects key, value, or the (key .
// value) entry pair itself. Order is unspecified (bucket/chain order).
static lisp_value ht_collect(lisp_value ht, int pick, const char **err) {
    lisp_value bk = lisp_hashtable_buckets(ht);
    size_t nb = lisp_vector_length(bk);
    lisp_value head = LISP_EMPTY, last = LISP_EMPTY;
    for (size_t i = 0; i < nb; i++) {
        for (lisp_value b = lisp_vector_ref(bk, i); lisp_is_pair(b); b = lisp_cdr(b)) {
            lisp_value pr = lisp_car(b);
            lisp_value item = pick == 0 ? lisp_car(pr) : pick == 1 ? lisp_cdr(pr) : pr;
            lisp_value cell = lisp_cons(item, LISP_EMPTY);
            if (cell == LISP_UNDEF)
                return prim_err(err, "out of memory");
            if (head == LISP_EMPTY)
                head = cell;
            else
                set_cdr(last, cell);
            last = cell;
        }
    }
    return head;
}

static lisp_value prim_hash_keys(lisp_value *args, int argc, const char **err) {
    if (argc != 1 || !lisp_is_hashtable(args[0]))
        return prim_err(err, "hash-keys expects a hash-table");
    return ht_collect(args[0], 0, err);
}

static lisp_value prim_hash_values(lisp_value *args, int argc, const char **err) {
    if (argc != 1 || !lisp_is_hashtable(args[0]))
        return prim_err(err, "hash-values expects a hash-table");
    return ht_collect(args[0], 1, err);
}

static lisp_value prim_hash_to_list(lisp_value *args, int argc, const char **err) {
    if (argc != 1 || !lisp_is_hashtable(args[0]))
        return prim_err(err, "hash->list expects a hash-table");
    return ht_collect(args[0], 2, err);
}

// (hash-for-each ht proc) -- apply (proc key value) to every entry. Iterate over
// a SNAPSHOT list of the entry pairs (ht_collect) so a proc that mutates the
// table mid-walk cannot corrupt the chain being traversed.
static lisp_value prim_hash_for_each(lisp_value *args, int argc, const char **err) {
    if (argc != 2 || !lisp_is_hashtable(args[0]))
        return prim_err(err, "hash-for-each expects (hash-table procedure)");
    lisp_value entries = ht_collect(args[0], 2, err);
    if (entries == LISP_UNDEF && err != NULL && *err != NULL)
        return LISP_UNDEF;
    lisp_value ctxv = lisp_ctx_make(LISP_UNDEF, LISP_EMPTY);
    if (ctxv == LISP_UNDEF)
        return prim_err(err, "out of memory");
    for (lisp_value e = entries; lisp_is_pair(e); e = lisp_cdr(e)) {
        lisp_value pr = lisp_car(e);
        lisp_value kv[2] = {lisp_car(pr), lisp_cdr(pr)};
        lisp_value r = lisp_apply_reuse(ctxv, args[1], kv, 2, err);
        if (r == LISP_UNDEF && err != NULL && *err != NULL)
            return LISP_UNDEF;
    }
    return LISP_UNDEF;
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
    // stack on modest lists. One reused context applies the procedure to every
    // element (lisp_apply_reuse), instead of allocating a context per element.
    lisp_value ctxv = lisp_ctx_make(LISP_UNDEF, LISP_EMPTY);
    if (ctxv == LISP_UNDEF)
        return prim_err(err, "out of memory");
    lisp_value head = LISP_EMPTY;
    lisp_value last = LISP_EMPTY;
    lisp_value lst = args[1];
    while (lisp_is_pair(lst)) {
        lisp_value arg = lisp_car(lst);
        lisp_value r = lisp_apply_reuse(ctxv, args[0], &arg, 1, err);
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
    lisp_value ctxv = lisp_ctx_make(LISP_UNDEF, LISP_EMPTY);
    if (ctxv == LISP_UNDEF)
        return prim_err(err, "out of memory");
    lisp_value lst = args[1];
    while (lisp_is_pair(lst)) {
        lisp_value arg = lisp_car(lst);
        lisp_value r = lisp_apply_reuse(ctxv, args[0], &arg, 1, err);
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
// condition/guard system this simply aborts evaluation with the message.
//
// *err is a `const char *` that the explicit-stack machine stores in the failing
// context (lisp_ctx_t::err) and that a scheduler may read AFTER a garbage
// collection -- so it must not point into the GC heap. The message string IS a
// heap object, so copy it into a stable buffer. One error is in flight at a time
// under the cooperative (one-context-at-a-time) scheduler, so a static buffer is
// sufficient; it is consumed before the next context runs.
static lisp_value prim_error(lisp_value *a, int n, const char **e) {
    if (n < 1)
        return prim_err(e, "error: a message argument is required");
    if (lisp_is_string(a[0])) {
        static char msgbuf[256];
        size_t len = lisp_string_len(a[0]);
        if (len >= sizeof(msgbuf))
            len = sizeof(msgbuf) - 1;
        memcpy(msgbuf, lisp_string_data(a[0]), len);
        msgbuf[len] = '\0';
        return prim_err(e, msgbuf);
    }
    return prim_err(e, "error");
}

// --- Bitwise / bitfield (driver substrate) ----------------------------------
//
// Operate on fixnums, which carry 62 signed bits -- enough for any u32 register
// value and for physical addresses up to 2^62. (A full u64 with the top bits set
// does not fit; the driver register width in practice is u32.) Results are taken
// modulo the fixnum range.

static bool all_fixnums(lisp_value *a, int n) {
    for (int i = 0; i < n; i++)
        if (!lisp_is_fixnum(a[i]))
            return false;
    return true;
}

static lisp_value prim_bitand(lisp_value *a, int n, const char **e) {
    if (!all_fixnums(a, n))
        return prim_err(e, "bitwise-and: expects integers");
    int64_t acc = -1;  // identity: all ones
    for (int i = 0; i < n; i++)
        acc &= lisp_fixnum_val(a[i]);
    return lisp_fixnum(acc);
}

static lisp_value prim_bitor(lisp_value *a, int n, const char **e) {
    if (!all_fixnums(a, n))
        return prim_err(e, "bitwise-or: expects integers");
    int64_t acc = 0;
    for (int i = 0; i < n; i++)
        acc |= lisp_fixnum_val(a[i]);
    return lisp_fixnum(acc);
}

static lisp_value prim_bitxor(lisp_value *a, int n, const char **e) {
    if (!all_fixnums(a, n))
        return prim_err(e, "bitwise-xor: expects integers");
    int64_t acc = 0;
    for (int i = 0; i < n; i++)
        acc ^= lisp_fixnum_val(a[i]);
    return lisp_fixnum(acc);
}

static lisp_value prim_bitnot(lisp_value *a, int n, const char **e) {
    if (n != 1 || !lisp_is_fixnum(a[0]))
        return prim_err(e, "bitwise-not: expects one integer");
    return lisp_fixnum(~lisp_fixnum_val(a[0]));
}

// (arithmetic-shift value count): count > 0 shifts left, < 0 shifts right
// (arithmetic, sign-preserving). Shifts of >= 64 saturate (0, or -1 for a
// negative value shifted right).
static lisp_value prim_ashift(lisp_value *a, int n, const char **e) {
    if (n != 2 || !lisp_is_fixnum(a[0]) || !lisp_is_fixnum(a[1]))
        return prim_err(e, "arithmetic-shift: expects (value count)");
    int64_t v = lisp_fixnum_val(a[0]);
    int64_t s = lisp_fixnum_val(a[1]);
    if (s >= 0)  // shift in unsigned to avoid signed-left-shift UB; two's-complement result
        return lisp_fixnum(s >= 64 ? 0 : (int64_t)((uint64_t)v << s));
    int64_t sh = -s;
    return lisp_fixnum(sh >= 63 ? (v < 0 ? -1 : 0) : (v >> sh));
}

// (bit-extract value lo width): the `width` bits of `value` starting at bit `lo`,
// right-justified. lo+width must be <= 62.
static lisp_value prim_bit_extract(lisp_value *a, int n, const char **e) {
    if (n != 3 || !all_fixnums(a, n))
        return prim_err(e, "bit-extract: expects (value lo width)");
    int64_t v = lisp_fixnum_val(a[0]);
    int64_t lo = lisp_fixnum_val(a[1]), w = lisp_fixnum_val(a[2]);
    if (lo < 0 || w < 0 || lo + w > 62)
        return prim_err(e, "bit-extract: lo/width out of range");
    uint64_t mask = (w == 0) ? 0 : ((1ull << w) - 1);
    return lisp_fixnum((int64_t)(((uint64_t)v >> lo) & mask));
}

// (bit-insert value lo width field): `value` with its `width` bits at `lo`
// replaced by the low `width` bits of `field`.
static lisp_value prim_bit_insert(lisp_value *a, int n, const char **e) {
    if (n != 4 || !all_fixnums(a, n))
        return prim_err(e, "bit-insert: expects (value lo width field)");
    int64_t v = lisp_fixnum_val(a[0]);
    int64_t lo = lisp_fixnum_val(a[1]), w = lisp_fixnum_val(a[2]), f = lisp_fixnum_val(a[3]);
    if (lo < 0 || w < 0 || lo + w > 62)
        return prim_err(e, "bit-insert: lo/width out of range");
    uint64_t mask = (w == 0) ? 0 : ((1ull << w) - 1);
    uint64_t res = ((uint64_t)v & ~(mask << lo)) | (((uint64_t)f & mask) << lo);
    return lisp_fixnum((int64_t)res);
}

// --- Mutable byte buffers (driver substrate + bulk IPC) ---------------------
// (lisp_make_bytes / lisp_make_bytes_foreign live in value.c with the other
// constructors, since they allocate via lisp_gc_alloc.)

static lisp_bytes *as_bytes(lisp_value v) { return (lisp_bytes *)lisp_obj(v); }
size_t lisp_bytes_len(lisp_value v) { return as_bytes(v)->len; }
void *lisp_bytes_data(lisp_value v) { return as_bytes(v)->data; }
uint64_t lisp_bytes_phys(lisp_value v) { return as_bytes(v)->phys; }

// (make-bytes n) -> a fresh zeroed mutable buffer of n bytes.
static lisp_value prim_make_bytes(lisp_value *a, int n, const char **e) {
    if (n != 1 || !lisp_is_fixnum(a[0]) || lisp_fixnum_val(a[0]) < 0)
        return prim_err(e, "make-bytes expects a non-negative length");
    lisp_value b = lisp_make_bytes((size_t)lisp_fixnum_val(a[0]));
    if (b == LISP_UNDEF)
        return prim_err(e, "make-bytes: out of memory");
    return b;
}

static lisp_value prim_bytes_length(lisp_value *a, int n, const char **e) {
    if (n != 1 || !lisp_is_bytes(a[0]))
        return prim_err(e, "bytes-length expects a byte buffer");
    return lisp_fixnum((int64_t)as_bytes(a[0])->len);
}

// (bytes-phys b) -> the physical address of a DMA region (0 for a plain buffer).
// NB: the result is a fixnum (62 signed bits), so a physical address >= 2^61 is
// truncated -- fine for current targets (phys addrs are well under that).
static lisp_value prim_bytes_phys(lisp_value *a, int n, const char **e) {
    if (n != 1 || !lisp_is_bytes(a[0]))
        return prim_err(e, "bytes-phys expects a byte buffer");
    return lisp_fixnum((int64_t)as_bytes(a[0])->phys);
}

// Width-checked, bounds-checked, VOLATILE little-endian access. x86 is LE, so a
// single natural-width volatile load/store gives little-endian semantics AND the
// single bus access MMIO requires; the offset should be width-aligned for MMIO.
static lisp_value bytes_ref(lisp_value *a, int n, const char **e, int w) {
    if (n != 2 || !lisp_is_bytes(a[0]) || !lisp_is_fixnum(a[1]))
        return prim_err(e, "bytes-ref expects (bytes index)");
    lisp_bytes *b = as_bytes(a[0]);
    int64_t i = lisp_fixnum_val(a[1]);
    if (i < 0 || (size_t)i + (size_t)w > b->len)
        return prim_err(e, "bytes-ref: index out of range");
    volatile uint8_t *p = b->data + i;
    uint64_t v = 0;
    switch (w) {
        case 1: v = *(volatile uint8_t *)p; break;
        case 2: v = *(volatile uint16_t *)p; break;
        case 4: v = *(volatile uint32_t *)p; break;
        default: v = *(volatile uint64_t *)p; break;
    }
    return lisp_fixnum((int64_t)v);
}

static lisp_value bytes_set(lisp_value *a, int n, const char **e, int w) {
    if (n != 3 || !lisp_is_bytes(a[0]) || !lisp_is_fixnum(a[1]) || !lisp_is_fixnum(a[2]))
        return prim_err(e, "bytes-set! expects (bytes index value)");
    lisp_bytes *b = as_bytes(a[0]);
    int64_t i = lisp_fixnum_val(a[1]);
    if (i < 0 || (size_t)i + (size_t)w > b->len)
        return prim_err(e, "bytes-set!: index out of range");
    uint64_t v = (uint64_t)lisp_fixnum_val(a[2]);
    volatile uint8_t *p = b->data + i;
    switch (w) {
        case 1: *(volatile uint8_t *)p = (uint8_t)v; break;
        case 2: *(volatile uint16_t *)p = (uint16_t)v; break;
        case 4: *(volatile uint32_t *)p = (uint32_t)v; break;
        default: *(volatile uint64_t *)p = v; break;
    }
    return LISP_UNDEF;
}

// Bulk operations. A per-element Lisp loop over bytes-u32-set!/bytes-u8-ref pays
// the full VM dispatch + a primitive call PER element; for a framebuffer fill or
// a buffer blit that is ~hundreds of x slower than C (measured: ~200x vs a scalar
// C loop, ~650x vs memcpy). These do the whole region in one primitive call whose
// inner loop the C compiler vectorizes. They are NON-volatile and so are for bulk
// DATA -- framebuffers, DMA/packet/IO buffers -- not MMIO registers (use the
// width-specific volatile bytes-*-set!/ref for a single register access). The
// non-volatile loop is deliberate: it is what lets the compiler vectorize and
// coalesce stores (the win), which is correct for a framebuffer -- it is pixel
// DATA, mmio-mapped or not, and nothing observes individual write granularity or
// ordering -- but is exactly wrong for a control register. The store pointer comes
// from an escaped argument, so the writes are emitted, never elided.

// (bytes-fill32! b byte-offset count color) -> fill `count` 32-bit words at
// `byte-offset` with `color`. The natural framebuffer clear/paint.
static lisp_value prim_bytes_fill32(lisp_value *a, int n, const char **e) {
    if (n != 4 || !lisp_is_bytes(a[0]) || !lisp_is_fixnum(a[1]) ||
        !lisp_is_fixnum(a[2]) || !lisp_is_fixnum(a[3]))
        return prim_err(e, "bytes-fill32! expects (bytes byte-offset count color)");
    lisp_bytes *b = as_bytes(a[0]);
    int64_t off = lisp_fixnum_val(a[1]), count = lisp_fixnum_val(a[2]);
    // The 32-bit store needs a 4-aligned address, so check the actual data pointer,
    // not just `off` (a foreign bytes object can wrap a non-4-aligned base).
    if (off < 0 || count < 0 ||
        (size_t)off + (size_t)count * 4 > b->len ||
        (((uintptr_t)b->data + (size_t)off) & 3) != 0)
        return prim_err(e, "bytes-fill32!: range out of bounds or not 4-aligned");
    uint32_t color = (uint32_t)lisp_fixnum_val(a[3]);
    uint32_t *p = (uint32_t *)(void *)(b->data + off);
    for (int64_t i = 0; i < count; i++)
        p[i] = color;
    return LISP_UNDEF;
}

// (bytes-copy! dst doff src soff len) -> copy `len` bytes from src[soff..] to
// dst[doff..]. memmove semantics, so an in-place overlapping move (e.g. a console
// scroll) is safe. A true blit.
static lisp_value prim_bytes_copy(lisp_value *a, int n, const char **e) {
    if (n != 5 || !lisp_is_bytes(a[0]) || !lisp_is_fixnum(a[1]) ||
        !lisp_is_bytes(a[2]) || !lisp_is_fixnum(a[3]) || !lisp_is_fixnum(a[4]))
        return prim_err(e, "bytes-copy! expects (dst doff src soff len)");
    lisp_bytes *d = as_bytes(a[0]), *s = as_bytes(a[2]);
    int64_t doff = lisp_fixnum_val(a[1]), soff = lisp_fixnum_val(a[3]),
            len = lisp_fixnum_val(a[4]);
    if (doff < 0 || soff < 0 || len < 0 ||
        (size_t)doff + (size_t)len > d->len ||
        (size_t)soff + (size_t)len > s->len)
        return prim_err(e, "bytes-copy!: range out of bounds");
    memmove(d->data + doff, s->data + soff, (size_t)len);
    return LISP_UNDEF;
}

// (sfence) -> retire the CPU's store / write-combining buffers so every prior
// store is globally visible before any later one. Required after streaming a
// frame to a WRITE-COMBINING framebuffer (mmio-map-wc): WC stores accumulate in
// the write-combine buffer and drain lazily (when it fills, on a fence, or on a
// serializing instruction), so without a fence the tail of a flushed frame can
// still be buffered when the scanout reads VRAM -> tearing at the bottom edge.
static lisp_value prim_sfence(lisp_value *a, int n, const char **e) {
    (void)a;
    if (n != 0)
        return prim_err(e, "sfence: expects no arguments");
    __asm__ volatile("sfence" ::: "memory");
    return LISP_UNDEF;
}

// --- 2D graphics blitters ----------------------------------------------------
// The per-pixel inner loops a UI needs that the interpreted layer can't run fast
// (a per-element Lisp loop is the ~200-650x trap bytes-fill32!/bytes-copy! exist to
// avoid). Each is the SOFTWARE fallback the graphics library's backend dispatch
// (lisp/lib/graphics.clp) uses; a HW-2D driver can override an op with its own.
//
// All operate on a 32-bit-per-pixel destination `bytes` (XRGB/ARGB) with a byte
// `stride` (pitch). `dw`/`dh` are the dst's pixel bounds; every op CLIPS its target
// rectangle to (0,0,dw,dh), so off-screen and partially-off-screen draws are safe.
// They are non-volatile bulk DATA writers (framebuffer pixels), like the bytes-*
// bulk ops above -- not for MMIO control registers.

// Clip a w*h rect at (x,y) to (0,0,dw,dh); writes the clipped box into *cx..*ch and
// the source-origin shift into *sx,*sy. Returns 0 if fully clipped away (empty).
static int gfx_clip(int64_t x, int64_t y, int64_t w, int64_t h, int64_t dw, int64_t dh,
                    int64_t *cx, int64_t *cy, int64_t *cw, int64_t *ch, int64_t *sx, int64_t *sy) {
    int64_t x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int64_t x1 = x + w, y1 = y + h;
    if (x1 > dw) x1 = dw;
    if (y1 > dh) y1 = dh;
    if (x0 >= x1 || y0 >= y1) return 0;
    *cx = x0; *cy = y0; *cw = x1 - x0; *ch = y1 - y0;
    *sx = x0 - x; *sy = y0 - y;
    return 1;
}

// Does the clipped box [cx,cx+cw) x [cy,cy+ch), at 4 bytes/pixel over `stride`-byte
// rows, fit within a `len`-byte buffer? Overflow-SAFE: a malicious caller can pass
// huge fixnum stride/dims, so the naive `(cy+ch-1)*stride + (cx+cw)*4 <= len` check
// can wrap int64 and pass falsely -- here every product is bounded by a division
// against `len`/((size_t)-1) first. Inputs are post-clip (cw,ch >= 1; cx,cy >= 0).
static int gfx_fits(int64_t cx, int64_t cy, int64_t cw, int64_t ch, int64_t stride, size_t len) {
    if (stride <= 0 || cx < 0 || cy < 0 || cw <= 0 || ch <= 0) return 0;
    size_t srow = (size_t)stride;
    size_t last_row = (size_t)cy + (size_t)ch - 1;          // index of the last row written
    if (last_row != 0 && srow > ((size_t)-1) / last_row) return 0;   // last_row*srow overflows
    size_t row_off = last_row * srow;
    if (row_off > len) return 0;
    if ((size_t)cx + (size_t)cw > ((size_t)-1) / 4) return 0;   // (cx+cw)*4 overflows
    size_t col_end = ((size_t)cx + (size_t)cw) * 4;
    return col_end <= len - row_off;                        // row_off+col_end <= len, no overflow
}

// (gfx-fill-rect! dst stride dw dh x y w h color) -> fill the clipped rect.
static lisp_value prim_gfx_fill_rect(lisp_value *a, int n, const char **e) {
    if (n != 9 || !lisp_is_bytes(a[0]))
        return prim_err(e, "gfx-fill-rect! expects (dst stride dw dh x y w h color)");
    for (int i = 1; i < 9; i++)
        if (!lisp_is_fixnum(a[i])) return prim_err(e, "gfx-fill-rect!: non-fixnum arg");
    lisp_bytes *d = as_bytes(a[0]);
    int64_t stride = lisp_fixnum_val(a[1]), dw = lisp_fixnum_val(a[2]), dh = lisp_fixnum_val(a[3]);
    uint32_t color = (uint32_t)lisp_fixnum_val(a[8]);
    int64_t cx, cy, cw, ch, sx, sy;
    if (stride <= 0 || dw < 0 || dh < 0) return prim_err(e, "gfx-fill-rect!: bad geometry");
    if (((uintptr_t)d->data & 3) != 0 || (stride & 3) != 0)   // 32-bit stores need 4-alignment
        return prim_err(e, "gfx-fill-rect!: data/stride not 4-aligned");
    if (!gfx_clip(lisp_fixnum_val(a[4]), lisp_fixnum_val(a[5]), lisp_fixnum_val(a[6]),
                  lisp_fixnum_val(a[7]), dw, dh, &cx, &cy, &cw, &ch, &sx, &sy))
        return LISP_UNDEF;
    if (!gfx_fits(cx, cy, cw, ch, stride, d->len))
        return prim_err(e, "gfx-fill-rect!: out of bounds");
    for (int64_t r = 0; r < ch; r++) {
        uint32_t *row = (uint32_t *)(void *)(d->data + (cy + r) * stride + cx * 4);
        for (int64_t i = 0; i < cw; i++) row[i] = color;
    }
    return LISP_UNDEF;
}

// (gfx-blit! dst dstride dw dh dx dy src sstride sw sh) -> opaque copy of the src
// (sw*sh) image to (dx,dy), clipped. src has its own byte stride.
static lisp_value prim_gfx_blit(lisp_value *a, int n, const char **e) {
    if (n != 10 || !lisp_is_bytes(a[0]) || !lisp_is_bytes(a[6]))
        return prim_err(e, "gfx-blit! expects (dst dstride dw dh dx dy src sstride sw sh)");
    if (!lisp_is_fixnum(a[1]) || !lisp_is_fixnum(a[2]) || !lisp_is_fixnum(a[3]) ||
        !lisp_is_fixnum(a[4]) || !lisp_is_fixnum(a[5]) || !lisp_is_fixnum(a[7]) ||
        !lisp_is_fixnum(a[8]) || !lisp_is_fixnum(a[9]))
        return prim_err(e, "gfx-blit!: non-fixnum arg");
    lisp_bytes *d = as_bytes(a[0]), *s = as_bytes(a[6]);
    int64_t dstride = lisp_fixnum_val(a[1]), dw = lisp_fixnum_val(a[2]), dh = lisp_fixnum_val(a[3]);
    int64_t sstride = lisp_fixnum_val(a[7]), sw = lisp_fixnum_val(a[8]), sh = lisp_fixnum_val(a[9]);
    int64_t cx, cy, cw, ch, sx, sy;
    if (dstride <= 0 || sstride <= 0 || dw < 0 || dh < 0) return prim_err(e, "gfx-blit!: bad geometry");
    if (!gfx_clip(lisp_fixnum_val(a[4]), lisp_fixnum_val(a[5]), sw, sh, dw, dh,
                  &cx, &cy, &cw, &ch, &sx, &sy))
        return LISP_UNDEF;
    if (!gfx_fits(cx, cy, cw, ch, dstride, d->len) ||
        !gfx_fits(sx, sy, cw, ch, sstride, s->len))
        return prim_err(e, "gfx-blit!: out of bounds");
    for (int64_t r = 0; r < ch; r++)
        memcpy(d->data + (cy + r) * dstride + cx * 4,
               s->data + (sy + r) * sstride + sx * 4, (size_t)cw * 4);
    return LISP_UNDEF;
}

// (gfx-blend! dst dstride dw dh dx dy src sstride sw sh) -> alpha-composite the src
// ARGB image (alpha in the top byte) "over" the dst, clipped. The low 3 bytes are
// blended per-byte by alpha, so this is correct for any RGB-in-low-24-bits layout;
// the dst's top byte is left intact.
static lisp_value prim_gfx_blend(lisp_value *a, int n, const char **e) {
    if (n != 10 || !lisp_is_bytes(a[0]) || !lisp_is_bytes(a[6]))
        return prim_err(e, "gfx-blend! expects (dst dstride dw dh dx dy src sstride sw sh)");
    if (!lisp_is_fixnum(a[1]) || !lisp_is_fixnum(a[2]) || !lisp_is_fixnum(a[3]) ||
        !lisp_is_fixnum(a[4]) || !lisp_is_fixnum(a[5]) || !lisp_is_fixnum(a[7]) ||
        !lisp_is_fixnum(a[8]) || !lisp_is_fixnum(a[9]))
        return prim_err(e, "gfx-blend!: non-fixnum arg");
    lisp_bytes *d = as_bytes(a[0]), *s = as_bytes(a[6]);
    int64_t dstride = lisp_fixnum_val(a[1]), dw = lisp_fixnum_val(a[2]), dh = lisp_fixnum_val(a[3]);
    int64_t sstride = lisp_fixnum_val(a[7]), sw = lisp_fixnum_val(a[8]), sh = lisp_fixnum_val(a[9]);
    int64_t cx, cy, cw, ch, sx, sy;
    if (dstride <= 0 || sstride <= 0 || dw < 0 || dh < 0) return prim_err(e, "gfx-blend!: bad geometry");
    if (!gfx_clip(lisp_fixnum_val(a[4]), lisp_fixnum_val(a[5]), sw, sh, dw, dh,
                  &cx, &cy, &cw, &ch, &sx, &sy))
        return LISP_UNDEF;
    if (!gfx_fits(cx, cy, cw, ch, dstride, d->len) ||
        !gfx_fits(sx, sy, cw, ch, sstride, s->len))
        return prim_err(e, "gfx-blend!: out of bounds");
    for (int64_t r = 0; r < ch; r++) {
        uint8_t *dp = d->data + (cy + r) * dstride + cx * 4;
        const uint8_t *sp = s->data + (sy + r) * sstride + sx * 4;
        for (int64_t i = 0; i < cw; i++, dp += 4, sp += 4) {
            uint32_t al = sp[3];
            if (al == 0) continue;
            if (al == 255) { dp[0] = sp[0]; dp[1] = sp[1]; dp[2] = sp[2]; continue; }
            uint32_t ia = 255 - al;
            dp[0] = (uint8_t)((sp[0] * al + dp[0] * ia + 127) / 255);
            dp[1] = (uint8_t)((sp[1] * al + dp[1] * ia + 127) / 255);
            dp[2] = (uint8_t)((sp[2] * al + dp[2] * ia + 127) / 255);
        }
    }
    return LISP_UNDEF;
}

// (gfx-glyph! dst dstride dw dh dx dy bitmap boff gw gh fg bg draw-bg scale) -> blit
// a 1-bpp glyph (MSB = leftmost; row stride = ceil(gw/8) bytes) at (dx,dy), each
// glyph pixel expanded to a scale*scale block. A set bit paints `fg`; a clear bit
// paints `bg` only when draw-bg is non-zero (else it is transparent). The fast
// bitmap-font path -- one C call per glyph.
static lisp_value prim_gfx_glyph(lisp_value *a, int n, const char **e) {
    if (n != 14 || !lisp_is_bytes(a[0]) || !lisp_is_bytes(a[6]))
        return prim_err(e, "gfx-glyph! expects (dst dstride dw dh dx dy bitmap boff gw gh fg bg draw-bg scale)");
    for (int i = 1; i < 14; i++)
        if (i != 6 && !lisp_is_fixnum(a[i])) return prim_err(e, "gfx-glyph!: non-fixnum arg");
    lisp_bytes *d = as_bytes(a[0]), *bm = as_bytes(a[6]);
    int64_t dstride = lisp_fixnum_val(a[1]), dw = lisp_fixnum_val(a[2]), dh = lisp_fixnum_val(a[3]);
    int64_t dx = lisp_fixnum_val(a[4]), dy = lisp_fixnum_val(a[5]);
    int64_t boff = lisp_fixnum_val(a[7]), gw = lisp_fixnum_val(a[8]), gh = lisp_fixnum_val(a[9]);
    uint32_t fg = (uint32_t)lisp_fixnum_val(a[10]), bg = (uint32_t)lisp_fixnum_val(a[11]);
    int64_t draw_bg = lisp_fixnum_val(a[12]), scale = lisp_fixnum_val(a[13]);
    // Bound the dimensions well below any real glyph so gw*scale / gh*rowbytes can't
    // overflow int64 (an overflowed product could wrap a bounds check -- see gfx_fits).
    if (dstride <= 0 || dw < 0 || dh < 0 || gw < 0 || gh < 0 || boff < 0 ||
        scale < 1 || scale > 0x10000 || gw > 0x10000 || gh > 0x10000)
        return prim_err(e, "gfx-glyph!: bad geometry");
    if (((uintptr_t)d->data & 3) != 0 || (dstride & 3) != 0)   // 32-bit stores need 4-alignment
        return prim_err(e, "gfx-glyph!: data/stride not 4-aligned");
    int64_t rowbytes = (gw + 7) / 8;
    // boff + gh*rowbytes <= bm->len, checked overflow-safe (gh,rowbytes bounded above).
    if ((size_t)boff > bm->len ||
        (rowbytes > 0 && (size_t)gh > (bm->len - (size_t)boff) / (size_t)rowbytes))
        return prim_err(e, "gfx-glyph!: bitmap out of bounds");
    int64_t cx, cy, cw, ch, sx, sy;
    if (!gfx_clip(dx, dy, gw * scale, gh * scale, dw, dh, &cx, &cy, &cw, &ch, &sx, &sy))
        return LISP_UNDEF;
    if (!gfx_fits(cx, cy, cw, ch, dstride, d->len))
        return prim_err(e, "gfx-glyph!: out of bounds");
    for (int64_t r = 0; r < ch; r++) {
        int64_t gyrow = (sy + r) / scale;                  // glyph source row
        const uint8_t *grow = bm->data + boff + gyrow * rowbytes;
        uint32_t *dp = (uint32_t *)(void *)(d->data + (cy + r) * dstride + cx * 4);
        for (int64_t i = 0; i < cw; i++) {
            int64_t gxcol = (sx + i) / scale;              // glyph source column
            int set = grow[gxcol >> 3] & (0x80 >> (gxcol & 7));
            if (set) dp[i] = fg;
            else if (draw_bg) dp[i] = bg;
        }
    }
    return LISP_UNDEF;
}

// (gfx-cover! dst dstride dw dh dx dy cover cstride cw ch fg) -> composite a solid
// colour `fg` over the dst through an 8-bit COVERAGE mask (`cover`, 1 byte/pixel,
// 0..255 alpha), clipped. The antialiased-text path: stb_truetype rasterizes a glyph
// to a coverage bitmap (libs/ttf) and this paints it as `fg` in one C call. Blends
// the dst's low 3 bytes (layout-agnostic, like gfx-blend!); the top byte is left.
static lisp_value prim_gfx_cover(lisp_value *a, int n, const char **e) {
    if (n != 11 || !lisp_is_bytes(a[0]) || !lisp_is_bytes(a[6]))
        return prim_err(e, "gfx-cover! expects (dst dstride dw dh dx dy cover cstride cw ch fg)");
    for (int i = 1; i < 11; i++)
        if (i != 6 && !lisp_is_fixnum(a[i])) return prim_err(e, "gfx-cover!: non-fixnum arg");
    lisp_bytes *d = as_bytes(a[0]), *c = as_bytes(a[6]);
    int64_t dstride = lisp_fixnum_val(a[1]), dw = lisp_fixnum_val(a[2]), dh = lisp_fixnum_val(a[3]);
    int64_t cstride = lisp_fixnum_val(a[7]), cw = lisp_fixnum_val(a[8]), ch0 = lisp_fixnum_val(a[9]);
    uint32_t fg = (uint32_t)lisp_fixnum_val(a[10]);
    uint32_t f0 = fg & 0xFF, f1 = (fg >> 8) & 0xFF, f2 = (fg >> 16) & 0xFF;   // dst byte layout
    int64_t cx, cy, ccw, cch, sx, sy;
    if (dstride <= 0 || cstride <= 0 || dw < 0 || dh < 0) return prim_err(e, "gfx-cover!: bad geometry");
    if (!gfx_clip(lisp_fixnum_val(a[4]), lisp_fixnum_val(a[5]), cw, ch0, dw, dh, &cx, &cy, &ccw, &cch, &sx, &sy))
        return LISP_UNDEF;
    if (!gfx_fits(cx, cy, ccw, cch, dstride, d->len))
        return prim_err(e, "gfx-cover!: out of bounds");
    // coverage source bound (1 byte/pixel), overflow-safe.
    { size_t srow = (size_t)cstride, last = (size_t)sy + (size_t)cch - 1;
      if (last != 0 && srow > ((size_t)-1) / last) return prim_err(e, "gfx-cover!: coverage out of bounds");
      size_t roff = last * srow;
      if (roff > c->len || (size_t)sx + (size_t)ccw > c->len - roff)
          return prim_err(e, "gfx-cover!: coverage out of bounds"); }
    for (int64_t r = 0; r < cch; r++) {
        uint8_t *dp = d->data + (cy + r) * dstride + cx * 4;
        const uint8_t *cp = c->data + (sy + r) * cstride + sx;
        for (int64_t i = 0; i < ccw; i++, dp += 4) {
            uint32_t al = cp[i];
            if (al == 0) continue;
            if (al == 255) { dp[0] = (uint8_t)f0; dp[1] = (uint8_t)f1; dp[2] = (uint8_t)f2; continue; }
            uint32_t ia = 255 - al;
            dp[0] = (uint8_t)((f0 * al + dp[0] * ia + 127) / 255);
            dp[1] = (uint8_t)((f1 * al + dp[1] * ia + 127) / 255);
            dp[2] = (uint8_t)((f2 * al + dp[2] * ia + 127) / 255);
        }
    }
    return LISP_UNDEF;
}

static lisp_value prim_b_u8_ref(lisp_value *a, int n, const char **e) { return bytes_ref(a, n, e, 1); }
static lisp_value prim_b_u16_ref(lisp_value *a, int n, const char **e) { return bytes_ref(a, n, e, 2); }
static lisp_value prim_b_u32_ref(lisp_value *a, int n, const char **e) { return bytes_ref(a, n, e, 4); }
static lisp_value prim_b_u64_ref(lisp_value *a, int n, const char **e) { return bytes_ref(a, n, e, 8); }
static lisp_value prim_b_u8_set(lisp_value *a, int n, const char **e) { return bytes_set(a, n, e, 1); }
static lisp_value prim_b_u16_set(lisp_value *a, int n, const char **e) { return bytes_set(a, n, e, 2); }
static lisp_value prim_b_u32_set(lisp_value *a, int n, const char **e) { return bytes_set(a, n, e, 4); }
static lisp_value prim_b_u64_set(lisp_value *a, int n, const char **e) { return bytes_set(a, n, e, 8); }

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
    def(env, "bitwise-and", prim_bitand);
    def(env, "bitwise-or", prim_bitor);
    def(env, "bitwise-xor", prim_bitxor);
    def(env, "bitwise-not", prim_bitnot);
    def(env, "arithmetic-shift", prim_ashift);
    def(env, "bit-extract", prim_bit_extract);
    def(env, "bit-insert", prim_bit_insert);
    def(env, "make-bytes", prim_make_bytes);
    def(env, "bytes-length", prim_bytes_length);
    def(env, "bytes-phys", prim_bytes_phys);
    def(env, "bytes-u8-ref", prim_b_u8_ref);
    def(env, "bytes-u16-ref", prim_b_u16_ref);
    def(env, "bytes-u32-ref", prim_b_u32_ref);
    def(env, "bytes-u64-ref", prim_b_u64_ref);
    def(env, "bytes-u8-set!", prim_b_u8_set);
    def(env, "bytes-u16-set!", prim_b_u16_set);
    def(env, "bytes-u32-set!", prim_b_u32_set);
    def(env, "bytes-u64-set!", prim_b_u64_set);
    def(env, "bytes-fill32!", prim_bytes_fill32);
    def(env, "bytes-copy!", prim_bytes_copy);
    def(env, "sfence", prim_sfence);
    def(env, "gfx-fill-rect!", prim_gfx_fill_rect);
    def(env, "gfx-blit!", prim_gfx_blit);
    def(env, "gfx-blend!", prim_gfx_blend);
    def(env, "gfx-glyph!", prim_gfx_glyph);
    def(env, "gfx-cover!", prim_gfx_cover);
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
    def(env, "vector-set!", prim_vector_set);
    def(env, "vector-fill!", prim_vector_fill);
    def(env, "vector-copy!", prim_vector_copy);
    def(env, "vector-length", prim_vector_length);
    def(env, "vector->list", prim_vector_to_list);
    def(env, "list->vector", prim_list_to_vector);
    // Hash tables (equal?-keyed)
    def(env, "make-hash-table", prim_make_hashtable);
    def(env, "hash-table?", prim_hashtablep);
    def(env, "hash-set!", prim_hash_set);
    def(env, "hash-ref", prim_hash_ref);
    def(env, "hash-has-key?", prim_hash_has_key);
    def(env, "hash-remove!", prim_hash_remove);
    def(env, "hash-count", prim_hash_count);
    def(env, "hash-keys", prim_hash_keys);
    def(env, "hash-values", prim_hash_values);
    def(env, "hash->list", prim_hash_to_list);
    def(env, "hash-for-each", prim_hash_for_each);
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
