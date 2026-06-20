// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

// Shared structured-error helper. Used by ALL units. (Owned by Unit A but kept
// in its own tiny TU so the verifier/interpreter can call it without pulling in
// the frontend.)

#include <stdarg.h>
#include <stdio.h>

#include "sh_internal.h"

sh_status sh_set_error(sh_error *err, sh_status status, int line, int col,
                       const char *fmt, ...) {
    if (err) {
        err->status = status;
        err->line = line;
        err->col = col;
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(err->msg, sizeof(err->msg), fmt, ap);
        va_end(ap);
    }
    return status;
}
