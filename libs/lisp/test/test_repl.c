// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

// Host test for the REPL engine (repl.c): the read-eval-print core that the
// serial shell wires to the raw serial line. Exercises value transcripts, a
// persistent environment across calls, located reader errors, and eval errors --
// no serial hardware involved.

#include <stdio.h>
#include <string.h>

#include "lisp.h"

static int checks = 0;
static int failures = 0;

// Evaluate `in` in the given (persistent) env and check the transcript == want.
static void chk(lisp_value env, const char *in, const char *want) {
    checks++;
    char out[512];
    lisp_repl_eval(in, strlen(in), env, out, sizeof(out));
    if (strcmp(out, want) != 0) {
        printf("  FAIL %s\n        got : '%s'\n        want: '%s'\n", in, out, want);
        failures++;
    } else {
        printf("  ok   %-28s -> %s", in, out);
    }
}

int main(void) {
    uintptr_t stack_marker;
    lisp_gc_init(&stack_marker);

    printf("[lisp repl] evaluate + print\n");

    // A fresh env per case for the standalone checks.
    chk(lisp_default_env(), "(+ 1 2 3)", "6\n");
    chk(lisp_default_env(), "(* 6 7)", "42\n");
    // Several forms in one chunk: one transcript line each.
    chk(lisp_default_env(), "1 2 3", "1\n2\n3\n");
    // Strings print in write form (quoted).
    chk(lisp_default_env(), "\"hi\"", "\"hi\"\n");

    // A persistent env: a define on one line is visible on the next.
    {
        lisp_value env = lisp_default_env();
        chk(env, "(define x 10)", "x\n");
        chk(env, "(* x x)", "100\n");
        chk(env, "(define (sq n) (* n n)) (sq 9)", "sq\n81\n");
    }

    printf("[lisp repl] errors\n");
    // Eval error: unbound variable.
    chk(lisp_default_env(), "nope", "error: unbound variable\n");
    // Reader error carries a source location (the reader-error payoff).
    chk(lisp_default_env(), "(+ 1 2", "error: unterminated list (unclosed '(') (line 1, column 1)\n");
    chk(lisp_default_env(), ")", "error: unexpected ')' (line 1, column 1)\n");
    // A located error on a later line/column.
    chk(lisp_default_env(), "(define a 1)\n(foo ]", "a\nerror: '[' ']' '{' '}' are not supported (use parens) (line 2, column 6)\n");

    printf("\n[lisp repl] %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
