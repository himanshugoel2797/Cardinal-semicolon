// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

// Scheme s-expression reader. Phase 0 scope: integers (fixnum-range only),
// symbols, booleans (#t/#f), the empty list (), strings, single-character
// literals (#\x; named chars like #\newline are deferred), lists, and the quote
// shorthand '. Commas are whitespace and ; runs to end-of-line. There is no nil.
// Vectors #(...) arrive with the persistent data structures in Phase 2; for now
// they are reported as errors rather than silently mis-parsed.

#include <stdint.h>
#include <string.h>

#include "lisp.h"

static bool is_ws(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

// A delimiter ends an atom token. In Scheme ` and , are reader macros
// (quasiquote/unquote), so they delimit too.
static bool is_delim(char c) {
    return is_ws(c) || c == '(' || c == ')' || c == '[' || c == ']' || c == '{' ||
           c == '}' || c == '"' || c == ';' || c == '\'' || c == '`' || c == ',';
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

// Parse a flonum literal: [+-]? digits? ('.' digits?)? ([eE][+-]?digits)? with
// at least one digit and at least one of '.'/'e' (so plain integers fall through
// to parse_int and over-range integers stay symbols, not silently inexact). The
// conversion is a simple mantissa * 10^exp -- not perfectly rounded, adequate for
// literals; a correctly-rounded reader is a later refinement.
static bool parse_float(const char *tok, size_t len, lisp_value *out) {
    size_t i = 0;
    bool neg = false;
    if (i < len && (tok[i] == '+' || tok[i] == '-')) {
        neg = (tok[i] == '-');
        i++;
    }
    double mant = 0.0;
    int fracdigits = 0;
    bool has_digit = false, has_dot = false, has_exp = false;
    while (i < len && tok[i] >= '0' && tok[i] <= '9') {
        mant = mant * 10.0 + (tok[i] - '0');
        has_digit = true;
        i++;
    }
    if (i < len && tok[i] == '.') {
        has_dot = true;
        i++;
        while (i < len && tok[i] >= '0' && tok[i] <= '9') {
            mant = mant * 10.0 + (tok[i] - '0');
            fracdigits++;
            has_digit = true;
            i++;
        }
    }
    int exp = 0;
    bool expneg = false;
    if (i < len && (tok[i] == 'e' || tok[i] == 'E')) {
        has_exp = true;
        i++;
        if (i < len && (tok[i] == '+' || tok[i] == '-')) {
            expneg = (tok[i] == '-');
            i++;
        }
        if (i >= len || tok[i] < '0' || tok[i] > '9')
            return false;  // exponent marker with no digits
        while (i < len && tok[i] >= '0' && tok[i] <= '9') {
            if (exp < 100000)  // cap: avoids int overflow; beyond double range anyway
                exp = exp * 10 + (tok[i] - '0');
            i++;
        }
    }
    if (i != len || !has_digit || (!has_dot && !has_exp))
        return false;  // trailing junk, no digits, or a plain integer

    int e = (expneg ? -exp : exp) - fracdigits;
    int mag = e < 0 ? -e : e;
    if (mag > 400)
        mag = 400;  // 10^309 already overflows double to inf; cap the loop
    double scale = 1.0;
    for (int k = 0; k < mag; k++)
        scale *= 10.0;
    double value = e < 0 ? mant / scale : mant * scale;
    if (neg)
        value = -value;
    *out = lisp_make_flonum(value);
    return *out != LISP_UNDEF;
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

    // Booleans, the empty list, and chars are #-syntax (handled in read_hash);
    // there is no nil. A leading ':' is just a symbol constituent in Scheme, so
    // :foo reads as an ordinary symbol (#:foo keyword syntax is deferred).
    lisp_value num;
    if (parse_int(start, len, &num))
        return num;
    if (parse_float(start, len, &num))
        return num;

    return lisp_make_symbol(start, len);
}

static lisp_value read_list(const char **cur, const char *end, const char **err);

// Convert a proper list to an immutable vector (for #(...) literals). A dotted
// tail (#(1 . 2)) is a read error, not a silent truncation.
static lisp_value list_to_vector(lisp_value lst, const char **err) {
    size_t n = 0;
    lisp_value p = lst;
    for (; lisp_is_pair(p); p = lisp_cdr(p))
        n++;
    if (!lisp_is_empty(p))
        return fail(err, "vector literal must be a proper list");
    lisp_value v = lisp_make_vector(n, LISP_UNDEF);
    if (v == LISP_UNDEF)
        return fail(err, "out of memory");
    size_t i = 0;
    for (lisp_value p = lst; lisp_is_pair(p); p = lisp_cdr(p))
        lisp_vector_set_init(v, i++, lisp_car(p));
    return v;
}

// Parse #-prefixed syntax: #t/#true, #f/#false, #\<char> literals, and #(...)
// vectors.
static lisp_value read_hash(const char **cur, const char *end, const char **err) {
    const char *c = *cur + 1;  // skip '#'
    if (c >= end)
        return fail(err, "dangling # syntax");
    if (*c == '\\') {
        // Character literal. Phase 0: a single character after #\ (named chars
        // like #\newline are a later phase). One must be present.
        if (c + 1 >= end)
            return fail(err, "dangling character literal");
        uint32_t cp = (uint8_t)c[1];
        *cur = c + 2;
        return lisp_char(cp);
    }
    if (*c == '(') {  // #(...) vector literal
        lisp_value lst = read_list(&c, end, err);
        if (lst == LISP_UNDEF)
            return LISP_UNDEF;
        *cur = c;
        return list_to_vector(lst, err);
    }
    // Boolean token: #t / #true / #f / #false.
    const char *start = c;
    while (c < end && !is_delim(*c))
        c++;
    size_t len = (size_t)(c - start);
    if ((len == 1 && start[0] == 't') || (len == 4 && memcmp(start, "true", 4) == 0)) {
        *cur = c;
        return LISP_TRUE;
    }
    if ((len == 1 && start[0] == 'f') || (len == 5 && memcmp(start, "false", 5) == 0)) {
        *cur = c;
        return LISP_FALSE;
    }
    return fail(err, "unsupported # syntax");
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
        // Dotted tail: a lone '.' (followed by a delimiter) sets the final cdr.
        if (*c == '.' && (c + 1 >= end || is_delim(c[1]))) {
            if (head == LISP_EMPTY)
                return fail(err, "nothing before . in list");
            c++;  // skip the dot
            const char *sub = c;
            lisp_value tailv = lisp_read(&sub, end, err);
            if (tailv == LISP_UNDEF)
                return LISP_UNDEF;
            if (tailv == LISP_EOF)
                return fail(err, "missing element after . in list");
            c = sub;
            ((lisp_pair *)lisp_obj(tail))->cdr = tailv;
            skip_ws(&c, end);
            if (c >= end || *c != ')')
                return fail(err, "expected ) after dotted tail");
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

// Read the next datum and wrap it as (name datum). Shared by the quote-family
// reader macros: ' ` , ,@
static lisp_value read_prefixed(const char *name, size_t nlen, const char **cur,
                                const char *end, const char **err, const char *empty_msg) {
    lisp_value datum = lisp_read(cur, end, err);
    if (datum == LISP_UNDEF)
        return LISP_UNDEF;
    if (datum == LISP_EOF)
        return fail(err, empty_msg);
    lisp_value sym = lisp_make_symbol(name, nlen);
    lisp_value rest = lisp_cons(datum, LISP_EMPTY);
    if (sym == LISP_UNDEF || rest == LISP_UNDEF)
        return fail(err, "out of memory");
    lisp_value form = lisp_cons(sym, rest);
    if (form == LISP_UNDEF)
        return fail(err, "out of memory");
    return form;
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
            c++;  // skip '
            lisp_value v = read_prefixed("quote", 5, &c, end, err, "nothing to quote");
            if (v == LISP_UNDEF)
                return LISP_UNDEF;
            *cursor = c;
            return v;
        }
        case '`': {
            c++;  // skip `
            lisp_value v =
                read_prefixed("quasiquote", 10, &c, end, err, "nothing to quasiquote");
            if (v == LISP_UNDEF)
                return LISP_UNDEF;
            *cursor = c;
            return v;
        }
        case ',': {
            c++;  // skip ,
            const char *name = "unquote";
            size_t nlen = 7;
            const char *empty = "nothing to unquote";
            if (c < end && *c == '@') {  // ,@
                c++;
                name = "unquote-splicing";
                nlen = 16;
                empty = "nothing to unquote-splice";
            }
            lisp_value v = read_prefixed(name, nlen, &c, end, err, empty);
            if (v == LISP_UNDEF)
                return LISP_UNDEF;
            *cursor = c;
            return v;
        }
        case '#': {
            lisp_value v = read_hash(&c, end, err);
            *cursor = c;
            return v;
        }
        case '[':
        case ']':
        case '{':
        case '}':
            return fail(err, "[] / {} not supported");
        default: {
            lisp_value v = read_atom(&c, end, err);
            *cursor = c;
            return v;
        }
    }
}
