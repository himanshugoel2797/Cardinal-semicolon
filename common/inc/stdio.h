// Copyright (c) 2017 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#ifndef CARDINAL_STDIO_H
#define CARDINAL_STDIO_H

#include <stdarg.h>
#include <stddef.h>

// Minimal freestanding string formatting. The kernel has no full printf-family
// (modules format numbers via itoa/ltoa); these give the small subset that
// length-counted in-kernel buffers need -- used by libs/lisp_shader's structured
// errors. C99 truncation semantics: at most size-1 chars + a NUL are written, and
// the length that WOULD have been written is returned. Supports %d/%i %u %x/%X %p
// %c %s %% with optional l/ll/z length modifiers (no width/precision/flags).
int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
int snprintf(char *buf, size_t size, const char *fmt, ...);

#endif
