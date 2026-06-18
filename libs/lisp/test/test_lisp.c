// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

// Host-built Phase 0 test: read->print round-trip + representation invariants.
// Built and run on the host (see build-and-run.sh) so the runtime can be
// iterated where a crash is not a kernel panic -- the de-risking strategy from
// notes/core/lisp-substrate.md. Uses only the portable libc subset shared by
// the host and common/.

#include <stdio.h>
#include <string.h>

#include "lisp.h"

static int failures = 0;
static int checks = 0;

// Read one datum from `src`, print it, and assert the output equals `expect`.
static void roundtrip(const char *src, const char *expect) {
    checks++;
    const char *cur = src;
    const char *end = src + strlen(src);
    const char *err = NULL;
    lisp_value v = lisp_read(&cur, end, &err);
    char buf[512];
    if (v == LISP_UNDEF) {
        printf("  FAIL read %-28s -> error: %s\n", src, err ? err : "?");
        failures++;
        return;
    }
    lisp_print(v, buf, sizeof(buf));
    if (strcmp(buf, expect) != 0) {
        printf("  FAIL %-28s -> got '%s' want '%s'\n", src, buf, expect);
        failures++;
    } else {
        printf("  ok   %-28s -> %s\n", src, buf);
    }
}

static void expect_true(const char *label, int cond) {
    checks++;
    if (!cond) {
        printf("  FAIL invariant: %s\n", label);
        failures++;
    } else {
        printf("  ok   invariant: %s\n", label);
    }
}

int main(void) {
    printf("[lisp Phase 0] representation invariants\n");
    expect_true("fixnum roundtrip 42", lisp_fixnum_val(lisp_fixnum(42)) == 42);
    expect_true("fixnum roundtrip -7", lisp_fixnum_val(lisp_fixnum(-7)) == -7);
    expect_true("fixnum is_fixnum", lisp_is_fixnum(lisp_fixnum(0)));
    expect_true("fixnum max", lisp_fixnum_val(lisp_fixnum(LISP_FIXNUM_MAX)) == LISP_FIXNUM_MAX);
    expect_true("fixnum min", lisp_fixnum_val(lisp_fixnum(LISP_FIXNUM_MIN)) == LISP_FIXNUM_MIN);
    expect_true("#t != #f", LISP_TRUE != LISP_FALSE);
    expect_true("#f != empty", LISP_FALSE != LISP_EMPTY);
    expect_true("only #f not truthy", !lisp_truthy(LISP_FALSE));
    expect_true("#t is truthy", lisp_truthy(LISP_TRUE));
    expect_true("0 is truthy", lisp_truthy(lisp_fixnum(0)));
    expect_true("empty is truthy (Scheme)", lisp_truthy(LISP_EMPTY));
    expect_true("char roundtrip", lisp_char_val(lisp_char('Q')) == 'Q');
    expect_true("fixnum not ptr", !lisp_is_ptr(lisp_fixnum(123)));
    expect_true("#f not ptr", !lisp_is_ptr(LISP_FALSE));

    printf("\n[lisp Phase 0] read -> print round-trips\n");
    roundtrip("42", "42");
    roundtrip("-7", "-7");
    roundtrip("+0", "0");
    roundtrip("#t", "#t");
    roundtrip("#f", "#f");
    roundtrip("#true", "#t");
    roundtrip("#false", "#f");
    roundtrip("nil", "nil");  // no nil in Scheme: just an ordinary symbol
    roundtrip("foo", "foo");
    roundtrip("foo-bar?", "foo-bar?");
    roundtrip("set!", "set!");
    roundtrip("+", "+");
    roundtrip("-", "-");
    roundtrip(":keyword", ":keyword");  // leading colon -> ordinary symbol
    roundtrip("->string", "->string");
    roundtrip("()", "()");
    roundtrip("(1 2 3)", "(1 2 3)");
    roundtrip("(1 (2 3) 4)", "(1 (2 3) 4)");
    roundtrip("  (  a   b  c )  ", "(a b c)");
    roundtrip("(foo :bar 1 nil)", "(foo :bar 1 nil)");
    roundtrip("'x", "(quote x)");
    roundtrip("'(1 2)", "(quote (1 2))");
    roundtrip("\"hello\"", "\"hello\"");
    roundtrip("\"a\\nb\"", "\"a\\nb\"");
    roundtrip("; comment line\n99", "99");
    roundtrip("(a, b, c)", "(a b c)");  // commas are whitespace
    roundtrip("#\\a", "#\\a");          // char literal round-trips
    roundtrip("#\\)", "#\\)");          // delimiter as a char
    roundtrip("(#\\x #\\y)", "(#\\x #\\y)");
    roundtrip("(#t #f 1)", "(#t #f 1)");
    // Over-fixnum-range integers are read as symbols (bignums are a later phase),
    // not silently wrapped.
    roundtrip("99999999999999999999999", "99999999999999999999999");
    roundtrip("9223372036854775808", "9223372036854775808");
    // Largest / smallest in-range fixnum literals decode correctly.
    roundtrip("2305843009213693951", "2305843009213693951");    // LISP_FIXNUM_MAX
    roundtrip("-2305843009213693952", "-2305843009213693952");  // LISP_FIXNUM_MIN

    printf("\n[lisp Phase 0] %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
