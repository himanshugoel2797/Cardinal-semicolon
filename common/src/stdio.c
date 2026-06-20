// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

// Minimal freestanding vsnprintf/snprintf. See common/inc/stdio.h for the
// supported subset and semantics. Deliberately tiny: no width/precision/flags,
// since no in-kernel call site uses them. Number formatting reuses the same
// digits-in-reverse approach as itoa (common/src/stdlib.c).

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <types.h>

// A bounded output cursor: writes into buf[0..size-1] (reserving the last byte for
// the NUL) while always counting the would-be length, so truncation is invisible
// to the count the caller gets back.
typedef struct {
    char *buf;
    size_t size;
    size_t len;
} snbuf;

static void sb_putc(snbuf *s, char c) {
    if (s->len + 1 < s->size)  // keep one byte for the terminating NUL
        s->buf[s->len] = c;
    s->len++;
}

static void sb_puts(snbuf *s, const char *str) {
    if (str == NULL)
        str = "(null)";
    while (*str != '\0')
        sb_putc(s, *str++);
}

static void sb_putu(snbuf *s, uint64_t v, unsigned base, bool upper) {
    char tmp[24];
    int n = 0;
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    if (v == 0)
        tmp[n++] = '0';
    while (v != 0) {
        tmp[n++] = digits[v % base];
        v /= base;
    }
    while (n > 0)
        sb_putc(s, tmp[--n]);
}

static void sb_puti(snbuf *s, int64_t v) {
    if (v < 0) {
        sb_putc(s, '-');
        // Negate via unsigned to avoid INT64_MIN overflow.
        sb_putu(s, (uint64_t)(-(v + 1)) + 1, 10, false);
    } else {
        sb_putu(s, (uint64_t)v, 10, false);
    }
}

int WEAK vsnprintf(char *buf, size_t size, const char *fmt, va_list ap) {
    snbuf s = {buf, size, 0};
    for (const char *p = fmt; *p != '\0'; p++) {
        if (*p != '%') {
            sb_putc(&s, *p);
            continue;
        }
        p++;
        int lng = 0;       // 0 = int, 1 = long, 2 = long long
        bool zmod = false;  // size_t
        while (*p == 'l') {
            lng++;
            p++;
        }
        if (*p == 'z') {
            zmod = true;
            p++;
        }
        switch (*p) {
            case 'd':
            case 'i': {
                int64_t v = zmod      ? (int64_t)va_arg(ap, size_t)
                            : lng >= 2 ? (int64_t)va_arg(ap, long long)
                            : lng == 1 ? (int64_t)va_arg(ap, long)
                                       : (int64_t)va_arg(ap, int);
                sb_puti(&s, v);
                break;
            }
            case 'u': {
                uint64_t v = zmod      ? (uint64_t)va_arg(ap, size_t)
                             : lng >= 2 ? (uint64_t)va_arg(ap, unsigned long long)
                             : lng == 1 ? (uint64_t)va_arg(ap, unsigned long)
                                        : (uint64_t)va_arg(ap, unsigned int);
                sb_putu(&s, v, 10, false);
                break;
            }
            case 'x':
            case 'X': {
                uint64_t v = zmod      ? (uint64_t)va_arg(ap, size_t)
                             : lng >= 2 ? (uint64_t)va_arg(ap, unsigned long long)
                             : lng == 1 ? (uint64_t)va_arg(ap, unsigned long)
                                        : (uint64_t)va_arg(ap, unsigned int);
                sb_putu(&s, v, 16, *p == 'X');
                break;
            }
            case 'p': {
                uintptr_t v = (uintptr_t)va_arg(ap, void *);
                sb_puts(&s, "0x");
                sb_putu(&s, (uint64_t)v, 16, false);
                break;
            }
            case 'c':
                sb_putc(&s, (char)va_arg(ap, int));
                break;
            case 's':
                sb_puts(&s, va_arg(ap, const char *));
                break;
            case '%':
                sb_putc(&s, '%');
                break;
            case '\0':  // trailing '%': emit it literally and stop
                sb_putc(&s, '%');
                p--;
                break;
            default:  // unknown spec: pass it through verbatim
                sb_putc(&s, '%');
                sb_putc(&s, *p);
                break;
        }
    }
    if (s.size > 0)
        s.buf[s.len < s.size ? s.len : s.size - 1] = '\0';
    return (int)s.len;
}

int WEAK snprintf(char *buf, size_t size, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return r;
}
