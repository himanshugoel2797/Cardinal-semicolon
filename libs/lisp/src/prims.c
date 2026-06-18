// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

// Built-in procedures: numeric arithmetic/comparison (exact fixnums + inexact
// flonums with the usual contagion), list/pair operations, vectors, predicates,
// and higher-order map/apply. Integer overflow past the fixnum range still wraps
// (bignums are a later phase).

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
    // Higher-order
    def(env, "apply", prim_apply);
    def(env, "map", prim_map);
    def(env, "for-each", prim_for_each);
}
