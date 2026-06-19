// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

// Canonical, reader-faithful printer. Output is the self-documenting textual
// form of a value (homoiconicity: the printed form reads back to an equal
// value). Returns an snprintf-style length so truncation is detectable.

#include <stdint.h>
#include <string.h>

#include "lisp.h"

// A tiny append-with-bound helper. `pos` tracks the notional length (which may
// exceed cap); bytes are only written while they fit, and a NUL is kept at the
// end of the buffer.
typedef struct {
    char *buf;
    size_t cap;
    size_t pos;
} sink;

static void emit_ch(sink *s, char c) {
    if (s->cap > 0 && s->pos < s->cap - 1)
        s->buf[s->pos] = c;
    s->pos++;
}

static void emit(sink *s, const char *str, size_t len) {
    for (size_t i = 0; i < len; i++)
        emit_ch(s, str[i]);
}

static void emit_cstr(sink *s, const char *str) { emit(s, str, strlen(str)); }

static void emit_int(sink *s, int64_t v) {
    char tmp[24];
    size_t n = 0;
    uint64_t u;
    bool neg = v < 0;
    u = neg ? (uint64_t)(-(v + 1)) + 1u : (uint64_t)v;  // avoid INT64_MIN overflow
    do {
        tmp[n++] = (char)('0' + (u % 10));
        u /= 10;
    } while (u > 0);
    if (neg)
        emit_ch(s, '-');
    while (n > 0)
        emit_ch(s, tmp[--n]);
}

// A simple decimal flonum formatter: sign, integer part, '.', up to 10 rounded
// fractional digits (trailing zeros trimmed, at least one kept). Not a shortest-
// round-trip (Ryu/Grisu) printer -- adequate for display; a correctly-rounded
// version is a later refinement. Non-finite values use the R7RS names.
static void emit_flonum(sink *s, double v) {
    if (v != v) {
        emit_cstr(s, "+nan.0");
        return;
    }
    bool neg = v < 0.0;
    if (neg)
        v = -v;
    if (v - v != 0.0) {  // infinity (v is now +inf)
        emit_cstr(s, neg ? "-inf.0" : "+inf.0");
        return;
    }
    if (neg)
        emit_ch(s, '-');
    // Normalize very large magnitudes with a decimal exponent so the integer-part
    // cast below stays within int64 range (that conversion would otherwise be UB).
    // ip is kept SIGNED (int64) on purpose: the (double)ip back-conversion below
    // is then a single cvtsi2sd. An unsigned (uint64)->double would instead lower
    // to the SSE2 magic-constant sequence (movapd of a 16-byte .rodata constant +
    // subpd), whose ALIGNED load #GPs because the kernel module loader does not
    // 16-align section data. The exponent is appended as a trailing eN.
    int e10 = 0;
    while (v >= 9.223372036854775808e18) {  // 2^63: keep ip in int64 range
        v /= 10.0;
        e10++;
    }
    const int FRAC = 10;
    double rounding = 0.5;
    for (int k = 0; k < FRAC; k++)
        rounding /= 10.0;
    v += rounding;  // round at the FRAC-th fractional digit (may carry into ip)
    int64_t ip = (int64_t)v;
    emit_int(s, ip);
    emit_ch(s, '.');
    double frac = v - (double)ip;
    char digits[16];
    int nd = 0;
    for (int k = 0; k < FRAC; k++) {
        frac *= 10.0;
        int d = (int)frac;
        if (d < 0)
            d = 0;
        if (d > 9)
            d = 9;
        digits[nd++] = (char)('0' + d);
        frac -= d;
    }
    while (nd > 1 && digits[nd - 1] == '0')
        nd--;
    for (int k = 0; k < nd; k++)
        emit_ch(s, digits[k]);
    if (e10 != 0) {
        emit_ch(s, 'e');
        emit_int(s, (int64_t)e10);
    }
}

// `readable`: true for write (quoted strings, #\c chars -- reads back); false for
// display (raw string bytes, bare chars -- human output).
static void emit_string_literal(sink *s, lisp_value v, bool readable) {
    const char *d = lisp_string_data(v);
    size_t len = lisp_string_len(v);
    if (!readable) {
        emit(s, d, len);  // display: raw, unquoted
        return;
    }
    emit_ch(s, '"');
    for (size_t i = 0; i < len; i++) {
        char c = d[i];
        switch (c) {
            case '"': emit_cstr(s, "\\\""); break;
            case '\\': emit_cstr(s, "\\\\"); break;
            case '\n': emit_cstr(s, "\\n"); break;
            case '\t': emit_cstr(s, "\\t"); break;
            case '\r': emit_cstr(s, "\\r"); break;
            default: emit_ch(s, c); break;
        }
    }
    emit_ch(s, '"');
}

static void print_val(sink *s, lisp_value v, bool readable);

static void print_list(sink *s, lisp_value v, bool readable) {
    emit_ch(s, '(');
    bool first = true;
    while (lisp_is_pair(v)) {
        if (!first)
            emit_ch(s, ' ');
        first = false;
        print_val(s, lisp_car(v), readable);
        v = lisp_cdr(v);
    }
    // Improper list (dotted tail): print " . tail" for anything not the empty list.
    if (!lisp_is_empty(v)) {
        emit_cstr(s, " . ");
        print_val(s, v, readable);
    }
    emit_ch(s, ')');
}

static void print_val(sink *s, lisp_value v, bool readable) {
    if (lisp_is_fixnum(v)) {
        emit_int(s, lisp_fixnum_val(v));
        return;
    }
    if (lisp_is_imm(v)) {
        switch (lisp_imm_subtype(v)) {
            case LISP_IMM_FALSE: emit_cstr(s, "#f"); return;
            case LISP_IMM_TRUE: emit_cstr(s, "#t"); return;
            case LISP_IMM_EMPTY: emit_cstr(s, "()"); return;
            case LISP_IMM_EOF: emit_cstr(s, "#<eof>"); return;
            case LISP_IMM_CHAR: {
                uint32_t cp = lisp_char_val(v);
                if (!readable) {  // display: the bare character
                    emit_ch(s, (char)cp);
                    return;
                }
                // write: reader-faithful #\c for printable ASCII; an unambiguous
                // non-readable form otherwise (named chars / UTF-8 are later).
                if (cp >= 0x21 && cp <= 0x7e) {
                    emit_cstr(s, "#\\");
                    emit_ch(s, (char)cp);
                } else {
                    emit_cstr(s, "#<char ");
                    emit_int(s, (int64_t)cp);
                    emit_ch(s, '>');
                }
                return;
            }
            default: emit_cstr(s, "#<undef>"); return;
        }
    }
    if (!lisp_is_ptr(v)) {
        emit_cstr(s, "#<invalid>");
        return;
    }
    switch (LISP_HDR_TYPE(lisp_obj(v))) {
        case LISP_OBJ_PAIR: print_list(s, v, readable); return;
        case LISP_OBJ_SYMBOL: emit(s, lisp_named_name(v), lisp_named_len(v)); return;
        case LISP_OBJ_KEYWORD:
            emit_ch(s, ':');
            emit(s, lisp_named_name(v), lisp_named_len(v));
            return;
        case LISP_OBJ_STRING: emit_string_literal(s, v, readable); return;
        case LISP_OBJ_VECTOR: {
            emit_cstr(s, "#(");
            size_t n = lisp_vector_length(v);
            for (size_t i = 0; i < n; i++) {
                if (i > 0)
                    emit_ch(s, ' ');
                print_val(s, lisp_vector_ref(v, i), readable);
            }
            emit_ch(s, ')');
            return;
        }
        case LISP_OBJ_FLONUM: emit_flonum(s, lisp_flonum_val(v)); return;
        case LISP_OBJ_CLOSURE: emit_cstr(s, "#<procedure>"); return;
        case LISP_OBJ_PRIMITIVE: emit_cstr(s, "#<primitive>"); return;
        case LISP_OBJ_BYTES:
            emit_cstr(s, "#<bytes ");
            emit_int(s, (int64_t)lisp_bytes_len(v));
            emit_ch(s, '>');
            return;
        default: emit_cstr(s, "#<obj>"); return;
    }
}

static size_t print_into(lisp_value v, char *buf, size_t cap, bool readable) {
    sink s = {buf, cap, 0};
    print_val(&s, v, readable);
    if (cap > 0) {
        size_t term = s.pos < cap ? s.pos : cap - 1;
        buf[term] = '\0';
    }
    return s.pos;
}

size_t lisp_print(lisp_value v, char *buf, size_t cap) {
    return print_into(v, buf, cap, true);  // write form (reads back)
}

size_t lisp_display(lisp_value v, char *buf, size_t cap) {
    return print_into(v, buf, cap, false);  // human form
}
