// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

// Host test for the reader's error reporting (reader.c): a parse failure must
// name what went wrong AND leave the cursor at the offending byte, which
// lisp_source_location turns into a 1-based line:column. Unterminated lists and
// strings point back at the '(' / '"' that opened them, not at end-of-input.

#include <stdio.h>
#include <string.h>

#include "lisp.h"

static int checks = 0;
static int failures = 0;

// A malformed input: expect LISP_UNDEF, an error message containing `needle`,
// the error cursor at byte offset `off`, and that offset reported as `line:col`.
static void rerr(const char *src, const char *needle, int off, int line, int col) {
    checks++;
    const char *cur = src;
    const char *end = src + strlen(src);
    const char *err = NULL;
    lisp_value v = lisp_read(&cur, end, &err);
    int aoff = (int)(cur - src);
    int l = 0, c = 0;
    lisp_source_location(src, cur, &l, &c);
    if (v != LISP_UNDEF || err == NULL || strstr(err, needle) == NULL ||
        aoff != off || l != line || c != col) {
        printf("  FAIL \"%s\"\n        -> v=%s err='%s' off=%d(want %d) %d:%d(want %d:%d)\n",
               src, v == LISP_UNDEF ? "UNDEF" : "(value)", err ? err : "(null)",
               aoff, off, l, c, line, col);
        failures++;
    } else {
        printf("  ok   %-26s -> %d:%d  %s\n", src, l, c, err);
    }
}

// A well-formed input: expect a value (not UNDEF/EOF) and the cursor advanced to
// `endoff` (so the next read resumes correctly).
static void rok(const char *src, int endoff) {
    checks++;
    const char *cur = src;
    const char *end = src + strlen(src);
    const char *err = NULL;
    lisp_value v = lisp_read(&cur, end, &err);
    int aoff = (int)(cur - src);
    if (v == LISP_UNDEF || v == LISP_EOF || err != NULL || aoff != endoff) {
        printf("  FAIL ok \"%s\" -> err='%s' off=%d(want %d)\n", src,
               err ? err : "(null)", aoff, endoff);
        failures++;
    } else {
        printf("  ok   %-26s -> parsed, cursor@%d\n", src, aoff);
    }
}

int main(void) {
    uintptr_t stack_marker;
    lisp_gc_init(&stack_marker);

    printf("[lisp reader] error position + message\n");

    // Unterminated list points at its opening '(' (offset 0, line 1 col 1).
    rerr("(a b c", "unterminated list", 0, 1, 1);
    // A nested unterminated list points at the INNER open paren, on its own line.
    rerr("(foo\n  (bar", "unterminated list", 7, 2, 3);
    // Stray close paren, at the paren.
    rerr(")", "unexpected ')'", 0, 1, 1);
    // Unterminated string points at its opening quote.
    rerr("\"abc", "unterminated string", 0, 1, 1);
    rerr("(x \"oops)", "unterminated string", 3, 1, 4);
    // A backslash escape with no following char: point at the opening quote too.
    rerr("\"abc\\", "unterminated escape", 0, 1, 1);
    // Dotted-tail errors.
    rerr("(a .", "missing element after '.'", 0, 1, 1);
    rerr("(. 1)", "nothing before '.'", 1, 1, 2);
    // Quote family with nothing following (real end-of-input).
    rerr("'", "nothing to quote", 1, 1, 2);
    rerr(",@", "nothing to unquote-splice", 2, 1, 3);
    // Hash syntax.
    rerr("#", "dangling '#'", 0, 1, 1);
    rerr("#z", "unsupported '#' syntax", 0, 1, 1);
    rerr("#x1g", "bad digit in #x", 3, 1, 4);
    rerr("#b102", "bad digit in #b", 4, 1, 5);
    // Brackets are not supported.
    rerr("[1 2]", "not supported", 0, 1, 1);

    printf("[lisp reader] well-formed inputs still parse cleanly\n");
    rok("(+ 1 2)", 7);
    rok("  42  ", 4);   // stops right after the token
    rok("(1 . 2)", 7);
    rok("'(a b)", 6);
    rok("#(1 2 3)", 8);
    rok("#xFF", 4);
    rok("\"hi\"", 4);

    // Multi-datum: the cursor from one read feeds the next (as eval_string does).
    {
        checks++;
        const char *src = "1 2 3";
        const char *cur = src, *end = src + strlen(src), *err = NULL;
        int n = 0;
        for (;;) {
            lisp_value v = lisp_read(&cur, end, &err);
            if (v == LISP_EOF) break;
            if (v == LISP_UNDEF) { err = err ? err : "UNDEF"; break; }
            n++;
        }
        if (n == 3 && err == NULL) {
            printf("  ok   multi-datum stream            -> read %d data\n", n);
        } else {
            printf("  FAIL multi-datum stream -> n=%d err=%s\n", n, err ? err : "(null)");
            failures++;
        }
    }

    printf("\n[lisp reader] %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
