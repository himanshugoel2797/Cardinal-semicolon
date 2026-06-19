// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

// Host test for the tagged special-form dispatch (eval.c): the evaluator
// classifies a form's head symbol by a cached integer id (lisp_named.form_id)
// instead of a linear name-compare cascade. Two layers:
//
//   1. Differential classification -- an independent name-table oracle
//      (classic_form_id) must agree with the id the runtime cached on each
//      interned symbol, for every special form AND a batch of decoys (ordinary
//      variables, near-miss names, and the `else`/`unquote` keywords that are
//      deliberately NOT top-level forms). This catches a mistyped table entry or
//      an id/enum-order mismatch directly.
//   2. Behavioural equivalence -- a corpus run through the evaluator, including
//      the cases the change could plausibly break: a form name bound as a
//      variable (special only in head position), quoting, and ordinary
//      application of near-miss names.

#include <stdio.h>
#include <string.h>

#include "lisp.h"

static int failures = 0;
static int checks = 0;

// The "old way": identify a special form by name. Order matches the SF_* enum
// in eval.c (id = index + 1); 0 means ordinary symbol.
static int classic_form_id(const char *n) {
    static const char *forms[] = {
        "quote", "quasiquote", "if",   "define", "lambda", "set!",  "begin",
        "let",   "let*",       "letrec", "and",  "or",     "cond",  "when",
        "unless", "while",     "case",  "define-module", "import",
    };
    for (size_t i = 0; i < sizeof(forms) / sizeof(forms[0]); i++)
        if (strcmp(forms[i], n) == 0)
            return (int)i + 1;
    return 0;
}

// The id the runtime cached on the interned symbol for `name`.
static int runtime_form_id(const char *name) {
    lisp_value s = lisp_make_symbol(name, strlen(name));
    return (int)((lisp_named *)lisp_obj(s))->form_id;
}

static void classify(const char *name) {
    checks++;
    int want = classic_form_id(name);
    int got = runtime_form_id(name);
    if (got != want) {
        printf("  FAIL classify %-16s -> id %d, name-oracle says %d\n", name, got, want);
        failures++;
    } else {
        printf("  ok   classify %-16s -> id %d\n", name, got);
    }
}

static void evals(lisp_value env, const char *src, const char *expect) {
    checks++;
    const char *err = NULL;
    lisp_value v = lisp_eval_string(src, env, &err);
    if (v == LISP_UNDEF && err != NULL) {
        printf("  FAIL %-46s -> error: %s\n", src, err);
        failures++;
        return;
    }
    char buf[256];
    lisp_print(v, buf, sizeof(buf));
    if (strcmp(buf, expect) != 0) {
        printf("  FAIL %-46s -> got '%s' want '%s'\n", src, buf, expect);
        failures++;
    } else {
        printf("  ok   %-46s -> %s\n", src, buf);
    }
}

int main(void) {
    uintptr_t stack_marker;
    lisp_gc_init(&stack_marker);
    lisp_value env = lisp_default_env();  // tags the special-form symbols

    printf("[lisp dispatch] tagged special-form classification\n");

    // Every special form classifies to its id...
    const char *forms[] = {
        "quote", "quasiquote", "if",   "define", "lambda", "set!",  "begin",
        "let",   "let*",       "letrec", "and",  "or",     "cond",  "when",
        "unless", "while",     "case",  "define-module", "import",
    };
    for (size_t i = 0; i < sizeof(forms) / sizeof(forms[0]); i++)
        classify(forms[i]);

    // ...and decoys classify to 0, including the keywords (else/unquote/
    // unquote-splicing) that are handled inside cond/case/quasiquote via is_form
    // and must NOT be tagged as top-level forms.
    const char *decoys[] = {
        "else",   "unquote", "unquote-splicing", "iffy",  "if?",   "i",
        "lambda?", "define!", "lett",  "let**",  "cond1", "x",     "list",
        "+",      "map",     "quote-it", "while1", "begin?", "Case",
    };
    for (size_t i = 0; i < sizeof(decoys) / sizeof(decoys[0]); i++)
        classify(decoys[i]);

    printf("\n[lisp dispatch] behavioural equivalence\n");

    // A representative use of each form still evaluates correctly.
    evals(env, "(quote (a b))", "(a b)");
    evals(env, "`(1 ,(+ 2 3))", "(1 5)");
    evals(env, "(if #t 'yes 'no)", "yes");
    evals(env, "(begin 1 2 3)", "3");
    evals(env, "(let ((a 2) (b 3)) (* a b))", "6");
    evals(env, "(let* ((a 2) (b (+ a 1))) b)", "3");
    evals(env, "(and 1 2 3)", "3");
    evals(env, "(or #f 7)", "7");
    evals(env, "(cond (#f 1) (else 2))", "2");
    evals(env, "(when #t 'w)", "w");
    evals(env, "(unless #f 'u)", "u");
    evals(env, "(case 2 ((2) 'two) (else 'no))", "two");
    evals(env, "(letrec ((f (lambda (n) (if (= n 0) 'done (f (- n 1)))))) (f 3))",
          "done");
    evals(env, "(let ((x 0)) (while (< x 3) (set! x (+ x 1))) x)", "3");
    // define-module / import dispatch (semantics covered in depth by test_modules).
    evals(env, "(define-module dm (export v) (define v 41))", "dm");
    evals(env, "(import dm) v", "41");

    // The change's real risk: a form NAME used as a variable. In head position a
    // form is always special (cannot be shadowed as an operator); elsewhere the
    // same name is an ordinary variable. Both must hold, exactly as before.
    evals(env, "(let ((if 5)) if)", "5");          // non-head: variable lookup
    evals(env, "(let ((if 5)) (if #t 1 2))", "1");  // head: still the special form
    evals(env, "(define lambda 9) lambda", "9");     // global var named like a form
    evals(env, "(car '(if a b))", "if");             // quoted: not dispatched

    // Ordinary application is unaffected, including near-miss names.
    evals(env, "(define (f x) (+ x 1)) (f 10)", "11");
    evals(env, "(define (iff a b) (+ a b)) (iff 3 4)", "7");

    printf("\n[lisp dispatch] %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
