// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

// Host runner for the curated R7RS conformance subset (conformance.scm). Loads
// the script, evaluates it with output wired to stdout, and checks the global
// `fail` counter the in-Scheme harness maintains. argv[1] is the directory the
// script lives in (passed by build-and-run.sh).

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lisp.h"

static void host_out(const char *s, size_t len, void *ctx) {
    (void)ctx;
    fwrite(s, 1, len, stdout);
}

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : ".";
    char path[1024];
    snprintf(path, sizeof(path), "%s/conformance.scm", dir);

    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        printf("[conformance] cannot open %s\n", path);
        return 1;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *src = (char *)malloc((size_t)sz + 1);
    if (src == NULL || fread(src, 1, (size_t)sz, f) != (size_t)sz) {
        printf("[conformance] read error\n");
        return 1;
    }
    src[sz] = '\0';
    fclose(f);

    printf("[lisp conformance] running %s\n", path);
    lisp_set_output(host_out, NULL);
    lisp_value env = lisp_default_env();
    const char *err = NULL;
    lisp_value r = lisp_eval_string(src, env, &err);
    free(src);
    if (r == LISP_UNDEF && err != NULL) {
        printf("[conformance] eval error: %s\n", err);
        return 1;
    }

    // Read the harness's pass/fail counters back out of the environment.
    lisp_value failv, passv;
    int failed = 1, passed = 0;
    if (lisp_env_lookup(env, lisp_make_symbol("fail", 4), &failv) && lisp_is_fixnum(failv))
        failed = (int)lisp_fixnum_val(failv);
    if (lisp_env_lookup(env, lisp_make_symbol("pass", 4), &passv) && lisp_is_fixnum(passv))
        passed = (int)lisp_fixnum_val(passv);

    printf("[lisp conformance] %d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
