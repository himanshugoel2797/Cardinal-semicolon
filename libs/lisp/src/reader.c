// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

// S-expression / EDN reader. Phase 0 scope: integers (fixnum-range only),
// symbols, keywords (:foo), nil/true/false, strings, single-character literals
// (\x; named chars like \newline are deferred), lists, and the quote shorthand
// '. Commas are whitespace and ; runs to end-of-line, both Clojure-style.
// Vectors [] and maps {} arrive with the persistent data structures in Phase 2;
// for now they are reported as errors rather than silently mis-parsed.

#include <stdint.h>
#include <string.h>

#include "lisp.h"

static bool is_ws(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == ',';
}

// A delimiter ends an atom token.
static bool is_delim(char c) {
    return is_ws(c) || c == '(' || c == ')' || c == '[' || c == ']' || c == '{' ||
           c == '}' || c == '"' || c == ';' || c == '\'';
}

static void skip_ws(const char **cur, const char *end) {
    const char *c = *cur;
    while (c < end) {
        if (is_ws(*c)) {
            c++;
        } else if (*c == ';') {
            while (c < end && *c != '\n')
                c++;
        } else {
            break;
        }
    }
    *cur = c;
}

static lisp_value fail(const char **err, const char *msg) {
    if (err != NULL)
        *err = msg;
    return LISP_UNDEF;
}

// Parse an all-[+-]?digits token as a fixnum. Returns true and sets *out if the
// token is a valid integer literal that fits a fixnum; false means "treat as a
// symbol" (non-numeric OR out of fixnum range -- bignums are a later phase).
// Accumulation is unsigned to avoid signed-overflow UB, and bounded so an
// over-range literal never silently wraps.
static bool parse_int(const char *tok, size_t len, lisp_value *out) {
    if (len == 0)
        return false;
    size_t i = 0;
    bool neg = false;
    if (tok[0] == '+' || tok[0] == '-') {
        neg = (tok[0] == '-');
        i = 1;
        if (len == 1)
            return false;  // bare "+" / "-" is a symbol
    }
    // Magnitude limit: |min| for negatives, max for positives.
    const uint64_t limit =
        neg ? (uint64_t)(-(LISP_FIXNUM_MIN + 1)) + 1u : (uint64_t)LISP_FIXNUM_MAX;
    uint64_t u = 0;
    for (; i < len; i++) {
        if (tok[i] < '0' || tok[i] > '9')
            return false;
        uint64_t digit = (uint64_t)(tok[i] - '0');
        if (u > (limit - digit) / 10u)
            return false;  // would exceed fixnum range -> not a fixnum literal
        u = u * 10u + digit;
    }
    *out = lisp_fixnum(neg ? -(int64_t)u : (int64_t)u);
    return true;
}

static lisp_value read_string(const char **cur, const char *end, const char **err) {
    const char *c = *cur + 1;  // skip opening quote
    char buf[1024];
    size_t n = 0;
    while (c < end && *c != '"') {
        char ch = *c++;
        if (ch == '\\') {
            if (c >= end)
                return fail(err, "unterminated escape in string");
            char e = *c++;
            switch (e) {
                case 'n': ch = '\n'; break;
                case 't': ch = '\t'; break;
                case 'r': ch = '\r'; break;
                case '0': ch = '\0'; break;
                case '\\': ch = '\\'; break;
                case '"': ch = '"'; break;
                default: ch = e; break;
            }
        }
        if (n >= sizeof(buf))
            return fail(err, "string literal too long");
        buf[n++] = ch;
    }
    if (c >= end)
        return fail(err, "unterminated string");
    c++;  // skip closing quote
    *cur = c;
    return lisp_make_string(buf, n);
}

static lisp_value read_atom(const char **cur, const char *end, const char **err) {
    const char *start = *cur;
    const char *c = start;
    while (c < end && !is_delim(*c))
        c++;
    size_t len = (size_t)(c - start);
    *cur = c;

    if (len == 0)
        return fail(err, "empty token");

    // Keyword :foo
    if (start[0] == ':') {
        if (len == 1)
            return fail(err, "bare colon is not a keyword");
        return lisp_make_keyword(start + 1, len - 1);
    }

    // Named constants
    if (len == 3 && memcmp(start, "nil", 3) == 0)
        return LISP_NIL;
    if (len == 4 && memcmp(start, "true", 4) == 0)
        return LISP_TRUE;
    if (len == 5 && memcmp(start, "false", 5) == 0)
        return LISP_FALSE;

    lisp_value num;
    if (parse_int(start, len, &num))
        return num;

    return lisp_make_symbol(start, len);
}

// Forward decl for recursion.
lisp_value lisp_read(const char **cursor, const char *end, const char **err);

static lisp_value read_list(const char **cur, const char *end, const char **err) {
    const char *c = *cur + 1;  // skip '('
    // Build the list, then we own a head pointer. Append by tracking the tail.
    lisp_value head = LISP_EMPTY;
    lisp_value tail = LISP_EMPTY;
    for (;;) {
        skip_ws(&c, end);
        if (c >= end)
            return fail(err, "unterminated list");
        if (*c == ')') {
            c++;
            *cur = c;
            return head;
        }
        const char *sub = c;
        lisp_value elem = lisp_read(&sub, end, err);
        if (elem == LISP_UNDEF)
            return LISP_UNDEF;
        c = sub;
        lisp_value cell = lisp_cons(elem, LISP_EMPTY);
        if (cell == LISP_UNDEF)
            return fail(err, "out of memory");
        if (head == LISP_EMPTY)
            head = cell;
        else
            ((lisp_pair *)lisp_obj(tail))->cdr = cell;
        tail = cell;
    }
}

lisp_value lisp_read(const char **cursor, const char *end, const char **err) {
    if (err != NULL)
        *err = NULL;
    const char *c = *cursor;
    skip_ws(&c, end);
    if (c >= end) {
        *cursor = c;
        return LISP_EOF;
    }

    char ch = *c;
    switch (ch) {
        case '(': {
            lisp_value v = read_list(&c, end, err);
            *cursor = c;
            return v;
        }
        case ')':
            return fail(err, "unexpected )");
        case '"': {
            lisp_value v = read_string(&c, end, err);
            *cursor = c;
            return v;
        }
        case '\'': {
            c++;  // skip quote
            lisp_value quoted = lisp_read(&c, end, err);
            if (quoted == LISP_UNDEF)
                return LISP_UNDEF;
            if (quoted == LISP_EOF)
                return fail(err, "nothing to quote");
            *cursor = c;
            lisp_value sym = lisp_make_symbol("quote", 5);
            lisp_value rest = lisp_cons(quoted, LISP_EMPTY);
            if (sym == LISP_UNDEF || rest == LISP_UNDEF)
                return fail(err, "out of memory");
            lisp_value form = lisp_cons(sym, rest);
            if (form == LISP_UNDEF)
                return fail(err, "out of memory");
            return form;
        }
        case '\\': {
            // Character literal. Phase 0: a single character after the backslash
            // (named chars like \newline are a later phase). Must be present.
            if (c + 1 >= end)
                return fail(err, "dangling character literal");
            uint32_t cp = (uint8_t)c[1];
            c += 2;
            *cursor = c;
            return lisp_char(cp);
        }
        case '[':
        case '{':
            return fail(err, "vectors/maps not yet supported (Phase 2)");
        default: {
            lisp_value v = read_atom(&c, end, err);
            *cursor = c;
            return v;
        }
    }
}
