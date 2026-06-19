// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

// Host-built test for the module system (module.c): define-module / import,
// private bindings, prefix / only clauses, transitive imports, idempotent
// loading, and the two error paths (unknown module, circular dependency).
//
// Module source is served from an in-memory table by a test loader, exercising
// the lisp_set_module_loader hook exactly as the kernel's initrd loader does --
// the byte range it returns need not be NUL-terminated (the reader is bounded),
// so the table stores plain string literals and reports strlen.

#include <stdio.h>
#include <string.h>

#include "lisp.h"

// --- in-memory module table + loader ----------------------------------------

struct modsrc {
    const char *name;
    const char *src;
};

static const struct modsrc MODULES[] = {
    // Exports two procedures; `helper` is private (never exported).
    {"math-utils",
     "(define-module math-utils (export square cube)"
     "  (define (square x) (* x x))"
     "  (define helper 'private)"
     "  (define (cube x) (* x (square x))))"},

    // Imports math-utils for its own use (square), but re-exports only `area`,
    // so an importer of `shapes` does NOT thereby get `square`.
    {"shapes",
     "(define-module shapes (export area)"
     "  (import math-utils)"
     "  (define (area r) (* 3 (square r))))"},

    // Two modules that both export the same name `tag`; prefixes disambiguate.
    {"alpha", "(define-module alpha (export tag) (define tag 'alpha-tag))"},
    {"beta", "(define-module beta (export tag) (define tag 'beta-tag))"},

    // Exports a freshly-consed object: importing twice must yield the SAME
    // object (eq?) iff the body ran only once -- i.e. loading is idempotent.
    {"stamp", "(define-module stamp (export token) (define token (list 1)))"},

    // A module with no exports at all (valid; binds nothing on import).
    {"empty", "(define-module empty (export))"},

    // A mutual import cycle: loading either must report a circular dependency.
    {"ouro", "(define-module ouro (export a) (import boro) (define a 1))"},
    {"boro", "(define-module boro (export b) (import ouro) (define b 2))"},

    // Fails while loading (body references an unbound name): the failed load
    // must not leave the module stuck as "loading" for a later retry.
    {"broken", "(define-module broken (export x) (define x (nope)))"},
};

static bool test_loader(const char *name, const char **src, size_t *len, void *ctx) {
    (void)ctx;
    for (size_t i = 0; i < sizeof(MODULES) / sizeof(MODULES[0]); i++) {
        if (strcmp(MODULES[i].name, name) == 0) {
            *src = MODULES[i].src;
            *len = strlen(MODULES[i].src);
            return true;
        }
    }
    return false;
}

// --- harness ----------------------------------------------------------------

static int failures = 0;
static int checks = 0;

// Evaluate `src` in a FRESH default env (clean module namespace each time) and
// check the printed result equals `expect`.
static void evals(const char *src, const char *expect) {
    checks++;
    lisp_value env = lisp_default_env();
    const char *err = NULL;
    lisp_value v = lisp_eval_string(src, env, &err);
    if (v == LISP_UNDEF && err != NULL) {
        printf("  FAIL %-58s -> error: %s\n", src, err);
        failures++;
        return;
    }
    char buf[512];
    lisp_print(v, buf, sizeof(buf));
    if (strcmp(buf, expect) != 0) {
        printf("  FAIL %-58s -> got '%s' want '%s'\n", src, buf, expect);
        failures++;
    } else {
        printf("  ok   %-58s -> %s\n", src, buf);
    }
}

// Evaluate `src` expecting an error whose message contains `needle`.
static void errs(const char *src, const char *needle) {
    checks++;
    lisp_value env = lisp_default_env();
    const char *err = NULL;
    lisp_value v = lisp_eval_string(src, env, &err);
    if (v != LISP_UNDEF || err == NULL) {
        printf("  FAIL %-58s -> expected error containing '%s'\n", src, needle);
        failures++;
        return;
    }
    if (strstr(err, needle) == NULL) {
        printf("  FAIL %-58s -> error '%s' lacks '%s'\n", src, err, needle);
        failures++;
    } else {
        printf("  ok   %-58s -> error: %s\n", src, err);
    }
}

int main(void) {
    uintptr_t stack_marker;
    lisp_gc_init(&stack_marker);  // run under the collector, as the real runtime does
    lisp_set_module_loader(test_loader, NULL);

    printf("[lisp modules] define-module / import\n");

    // Basic export + use.
    evals("(import math-utils) (square 5)", "25");
    evals("(import math-utils) (cube 3)", "27");

    // A private (unexported) binding is not visible to the importer.
    errs("(import math-utils) helper", "unbound variable");

    // Transitive import: shapes uses math-utils internally...
    evals("(import shapes) (area 2)", "12");
    // ...but does NOT re-export square (only what shapes lists in its own export).
    errs("(import shapes) (square 2)", "unbound variable");

    // prefix clause: bindings arrive renamed; the bare name is not introduced.
    evals("(import (math-utils (prefix m:))) (m:square 4)", "16");
    errs("(import (math-utils (prefix m:))) (square 4)", "unbound variable");

    // only clause: just the listed exports are bound.
    evals("(import (math-utils (only cube))) (cube 2)", "8");
    errs("(import (math-utils (only cube))) (square 2)", "unbound variable");

    // Same-named exports from two modules coexist under distinct prefixes.
    evals("(import (alpha (prefix a:)) (beta (prefix b:))) (list a:tag b:tag)",
          "(alpha-tag beta-tag)");

    // Idempotency: importing one module twice loads it once, so both prefixed
    // copies of its freshly-consed export are the very same object.
    evals("(import (stamp (prefix x:)) (stamp (prefix y:))) (eq? x:token y:token)",
          "#t");

    // A module with no exports imports cleanly and binds nothing.
    evals("(import empty) 'ok", "ok");

    // Multiple specs in one import form.
    evals("(import math-utils (alpha (prefix a:))) (list (square 3) a:tag)",
          "(9 alpha-tag)");

    // Error paths.
    errs("(import does-not-exist)", "module not found");
    errs("(import ouro)", "circular module dependency");

    // define-module returns its name; the form is usable directly (no loader).
    evals("(define-module direct (export answer) (define answer 42))", "direct");
    evals("(define-module direct (export answer) (define answer 42))"
          " (import direct) answer",
          "42");

    // Malformed forms are reported, not crashed.
    errs("(define-module 5 (export))", "must be a symbol");
    errs("(define-module m (export missing))", "exported name is not defined");

    // Retry after a failed load (SAME env): the first import errors while
    // evaluating the module body; the second must report the SAME real error,
    // not misdiagnose a leftover %loading marker as a circular dependency.
    {
        checks++;
        lisp_value env = lisp_default_env();
        const char *e1 = NULL, *e2 = NULL;
        lisp_eval_string("(import broken)", env, &e1);
        lisp_eval_string("(import broken)", env, &e2);
        if (e1 == NULL || e2 == NULL || strstr(e2, "circular") != NULL ||
            strcmp(e1, e2) != 0) {
            printf("  FAIL retry-after-failed-load -> first '%s' second '%s'\n",
                   e1 ? e1 : "(none)", e2 ? e2 : "(none)");
            failures++;
        } else {
            printf("  ok   retry-after-failed-load -> both: %s\n", e2);
        }
    }

    printf("\n[lisp modules] %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
