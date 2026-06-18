// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

// Host-built Phase 3c test: the string and character library.

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
        printf("  FAIL %-44s -> error: %s\n", src, err);
        failures++;
        return;
    }
    char buf[256];
    lisp_print(v, buf, sizeof(buf));
    if (strcmp(buf, expect) != 0) {
        printf("  FAIL %-44s -> got '%s' want '%s'\n", src, buf, expect);
        failures++;
    } else {
        printf("  ok   %-44s -> %s\n", src, buf);
    }
}

static void evalerr(const char *src) {
    checks++;
    lisp_value env = lisp_default_env();
    const char *err = NULL;
    lisp_value v = lisp_eval_string(src, env, &err);
    if (v == LISP_UNDEF && err != NULL) {
        printf("  ok   %-44s -> (error: %s)\n", src, err);
    } else {
        printf("  FAIL %-44s -> expected error\n", src);
        failures++;
    }
}

int main(void) {
    printf("[lisp Phase 3c] strings + chars\n");

    // strings
    evals("(string-length \"hello\")", "5");
    evals("(string-length \"\")", "0");
    evals("(string-ref \"abc\" 0)", "#\\a");
    evals("(string-ref \"abc\" 2)", "#\\c");
    evals("(substring \"hello world\" 0 5)", "\"hello\"");
    evals("(substring \"hello\" 1 4)", "\"ell\"");
    evals("(string-append \"foo\" \"bar\" \"baz\")", "\"foobarbaz\"");
    evals("(string-append)", "\"\"");
    evals("(string=? \"abc\" \"abc\")", "#t");
    evals("(string=? \"abc\" \"abd\")", "#f");
    evals("(string<? \"abc\" \"abd\")", "#t");
    evals("(string<? \"abc\" \"ab\")", "#f");
    evals("(string->list \"abc\")", "(#\\a #\\b #\\c)");
    evals("(list->string (list #\\h #\\i))", "\"hi\"");
    evals("(string #\\x #\\y #\\z)", "\"xyz\"");
    evals("(make-string 3 #\\a)", "\"aaa\"");
    evals("(string-length (make-string 4))", "4");

    // symbol <-> string
    evals("(symbol->string 'hello)", "\"hello\"");
    evals("(string->symbol \"world\")", "world");
    evals("(eq? (string->symbol \"foo\") 'foo)", "#t");  // interned

    // number <-> string
    evals("(number->string 42)", "\"42\"");
    evals("(number->string -7)", "\"-7\"");
    evals("(number->string 3.5)", "\"3.5\"");
    evals("(string->number \"42\")", "42");
    evals("(string->number \"3.14\")", "3.14");
    evals("(string->number \"-5\")", "-5");
    evals("(string->number \"abc\")", "#f");      // not a number -> #f
    evals("(string->number \"12abc\")", "#f");    // trailing junk -> #f
    evals("(string->number \"\")", "#f");

    // chars
    evals("(char->integer #\\A)", "65");
    evals("(integer->char 97)", "#\\a");
    evals("(char=? #\\a #\\a)", "#t");
    evals("(char<? #\\a #\\b)", "#t");
    evals("(char>? #\\b #\\a)", "#t");
    evals("(char-upcase #\\a)", "#\\A");
    evals("(char-downcase #\\Z)", "#\\z");
    evals("(char-upcase #\\5)", "#\\5");          // non-letter unchanged
    evals("(char-alphabetic? #\\q)", "#t");
    evals("(char-alphabetic? #\\5)", "#f");
    evals("(char-numeric? #\\7)", "#t");
    evals("(char-whitespace? #\\a)", "#f");
    evals("(char<=? #\\a #\\a #\\b)", "#t");
    evals("(char>=? #\\c #\\b #\\a)", "#t");
    evals("(list->string '())", "\"\"");

    // a small program combining pieces
    evals("(list->string (map char-upcase (string->list \"hello\")))", "\"HELLO\"");

    // errors
    evalerr("(string-ref \"abc\" 5)");
    evalerr("(substring \"abc\" 2 1)");
    evalerr("(integer->char -1)");
    evalerr("(string-length 42)");
    evalerr("(list->string 42)");           // non-list -> error, not ""
    evalerr("(list->string (cons #\\a 5))"); // improper list -> error
    evalerr("(list->string '(#\\a 5))");     // non-char element -> error

    printf("\n[lisp Phase 3c] %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
