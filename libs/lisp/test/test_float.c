// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

// Host-built Phase 3b test: flonums (hardware double, enabled because the
// runtime runs in FP-managed task context). Reader, printer, arithmetic
// contagion, comparisons, predicates, exact/inexact conversions.

#include <stdio.h>
#include <string.h>

#include "lisp.h"

static int failures = 0;
static int checks = 0;

static void evals(const char *src, const char *expect) {
    checks++;
    lisp_value env = lisp_default_env();
    const char *err = NULL;
    lisp_value v = lisp_eval_string(src, env, &err);
    if (v == LISP_UNDEF && err != NULL) {
        printf("  FAIL %-40s -> error: %s\n", src, err);
        failures++;
        return;
    }
    char buf[256];
    lisp_print(v, buf, sizeof(buf));
    if (strcmp(buf, expect) != 0) {
        printf("  FAIL %-40s -> got '%s' want '%s'\n", src, buf, expect);
        failures++;
    } else {
        printf("  ok   %-40s -> %s\n", src, buf);
    }
}

int main(void) {
    printf("[lisp Phase 3b] flonums\n");

    // Reader + printer round-trips
    evals("3.14", "3.14");
    evals("0.5", "0.5");
    evals("-2.5", "-2.5");
    evals(".5", "0.5");
    evals("5.", "5.0");
    evals("2.0", "2.0");
    evals("1e3", "1000.0");
    evals("1.5e2", "150.0");
    evals("-0.25", "-0.25");
    // tokens that are NOT floats stay symbols / ints
    evals("'a.b", "a.b");
    evals("42", "42");

    // Arithmetic contagion: any flonum operand => flonum result
    evals("(+ 1.5 2.5)", "4.0");
    evals("(+ 1 2.0)", "3.0");
    evals("(* 2 3.5)", "7.0");
    evals("(- 5.0 1)", "4.0");
    evals("(- 2.5)", "-2.5");
    evals("(+ 1 2 3)", "6");        // all-int stays exact
    evals("(* 2 3 4)", "24");
    // Division: exact when it divides evenly, else inexact
    evals("(/ 6 2)", "3");
    evals("(/ 7 2)", "3.5");
    evals("(/ 1 4)", "0.25");
    evals("(/ 10.0 4)", "2.5");
    evals("(/ 2)", "0.5");

    // Comparisons across exact/inexact
    evals("(= 2 2.0)", "#t");
    evals("(< 1 2.5 3)", "#t");
    evals("(< 1 2.5 2)", "#f");
    evals("(> 3.0 2)", "#t");
    evals("(zero? 0.0)", "#t");
    evals("(zero? 0)", "#t");

    // Predicates
    evals("(number? 1.5)", "#t");
    evals("(real? 1.5)", "#t");
    evals("(integer? 5)", "#t");
    evals("(integer? 2.0)", "#t");   // integral flonum
    evals("(integer? 2.5)", "#f");
    evals("(exact? 5)", "#t");
    evals("(exact? 5.0)", "#f");
    evals("(inexact? 5.0)", "#t");

    // Conversions
    evals("(exact->inexact 4)", "4.0");
    evals("(inexact 1)", "1.0");
    evals("(inexact->exact 3.0)", "3");
    evals("(exact 7.0)", "7");

    // equal? on flonums is value equality
    evals("(equal? 1.5 1.5)", "#t");
    evals("(equal? 1.5 1.6)", "#f");
    evals("(eq? 1.5 1.5)", "#f");           // distinct boxes

    // Edge cases that previously risked UB / huge loops (now bounded):
    evals("(integer? (* 1e10 1e10))", "#t");        // 1e20 is integral, no int64 UB
    evals("1e400", "+inf.0");                        // over double range -> inf, no hang
    evals("1e-400", "0.0");                          // underflow -> 0
    evals("(> (* 1e10 1e10) 1000000)", "#t");        // huge finite value, no UB
    evals("(* 1e9 1e9)", "1000000000000000000.0");   // 1e18 fits int64 part exactly

    printf("\n[lisp Phase 3b] %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
