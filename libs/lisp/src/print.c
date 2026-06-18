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

static void emit_string_literal(sink *s, lisp_value v) {
    emit_ch(s, '"');
    const char *d = lisp_string_data(v);
    size_t len = lisp_string_len(v);
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

static void print_val(sink *s, lisp_value v);

static void print_list(sink *s, lisp_value v) {
    emit_ch(s, '(');
    bool first = true;
    while (lisp_is_pair(v)) {
        if (!first)
            emit_ch(s, ' ');
        first = false;
        print_val(s, lisp_car(v));
        v = lisp_cdr(v);
    }
    // Improper list (dotted tail): print " . tail" for anything not the empty list.
    if (!lisp_is_empty(v)) {
        emit_cstr(s, " . ");
        print_val(s, v);
    }
    emit_ch(s, ')');
}

static void print_val(sink *s, lisp_value v) {
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
                // Reader-faithful #\c for printable ASCII; an unambiguous
                // non-readable form otherwise (named chars / UTF-8 are later).
                uint32_t cp = lisp_char_val(v);
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
        case LISP_OBJ_PAIR: print_list(s, v); return;
        case LISP_OBJ_SYMBOL: emit(s, lisp_named_name(v), lisp_named_len(v)); return;
        case LISP_OBJ_KEYWORD:
            emit_ch(s, ':');
            emit(s, lisp_named_name(v), lisp_named_len(v));
            return;
        case LISP_OBJ_STRING: emit_string_literal(s, v); return;
        default: emit_cstr(s, "#<obj>"); return;
    }
}

size_t lisp_print(lisp_value v, char *buf, size_t cap) {
    sink s = {buf, cap, 0};
    print_val(&s, v);
    if (cap > 0) {
        size_t term = s.pos < cap ? s.pos : cap - 1;
        buf[term] = '\0';
    }
    return s.pos;
}
