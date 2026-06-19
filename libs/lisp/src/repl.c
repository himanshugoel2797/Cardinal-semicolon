// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

// The read-eval-print core, factored out so it can be host-tested without any
// serial hardware. lisp_repl_eval reads every datum in a chunk of input, evaluates
// each in a persistent environment, and writes a human-readable transcript (values
// or errors) into a caller buffer. A reader error is reported WITH its source
// location (line:column, via the reader's error cursor + lisp_source_location) --
// the whole reason the reader was taught to track position. The serial shell
// (modules/SysLisp) is then just this engine wired to a CSMUX channel.

#include <stddef.h>

#include "internal.h"  // lisp_gc_set_alloc_heap / lisp_gc_system_heap
#include "lisp.h"

// Bounded append: copy the NUL-terminated `s` into out[*o .. cap), advancing *o.
// Never overflows; silently truncates if the buffer fills (the transcript is a
// debug convenience, not a protocol). Always leaves room to NUL-terminate later.
static void app(char *out, size_t cap, size_t *o, const char *s) {
    while (*s != '\0' && *o + 1 < cap)
        out[(*o)++] = *s++;
}

static void app_int(char *out, size_t cap, size_t *o, int v) {
    char tmp[12];
    int n = 0;
    if (v < 0) {
        app(out, cap, o, "-");
        v = -v;
    }
    do {
        tmp[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v != 0 && n < (int)sizeof(tmp));
    while (n > 0)
        if (*o + 1 < cap)
            out[(*o)++] = tmp[--n];
        else
            break;
}

int lisp_repl_eval(const char *in, size_t len, lisp_value env, char *out, size_t cap) {
    const char *cur = in;
    const char *end = in + len;
    size_t o = 0;
    int forms = 0;
    for (;;) {
        const char *rerr = NULL;
        lisp_value form = lisp_read(&cur, end, &rerr);
        if (form == LISP_EOF)
            break;
        if (form == LISP_UNDEF) {
            // Reader error: report the message AND where it is. `cur` was left at
            // the offending byte by lisp_read; turn it into 1-based line:column.
            int line = 1, col = 1;
            lisp_source_location(in, cur, &line, &col);
            app(out, cap, &o, "error: ");
            app(out, cap, &o, rerr != NULL ? rerr : "read error");
            app(out, cap, &o, " (line ");
            app_int(out, cap, &o, line);
            app(out, cap, &o, ", column ");
            app_int(out, cap, &o, col);
            app(out, cap, &o, ")\n");
            if (cap > 0)
                out[o < cap ? o : cap - 1] = '\0';
            return -1;  // a malformed datum invalidates the rest of the chunk
        }
        const char *eerr = NULL;
        lisp_value v = lisp_eval(form, env, &eerr);
        if (v == LISP_UNDEF && eerr != NULL) {
            app(out, cap, &o, "error: ");
            app(out, cap, &o, eerr);
            app(out, cap, &o, "\n");
        } else {
            char vb[256];
            lisp_print(v, vb, sizeof vb);
            app(out, cap, &o, vb);
            app(out, cap, &o, "\n");
        }
        forms++;
    }
    if (cap > 0)
        out[o < cap ? o : cap - 1] = '\0';
    return forms;
}

int lisp_repl_serve(const char *in, size_t len, lisp_value env, char *out, size_t cap) {
    // Evaluate with all allocation directed at the SYSTEM heap. A persistent REPL
    // env (its `define`s) and the user's computation must NOT land in the calling
    // context's per-context heap: that heap roots only its owner's registers, so
    // anything reachable solely through `env` (which lives in the system heap)
    // would be swept out from under the REPL on its next collection. Keeping it in
    // the system heap makes it consistently rooted (grow-only once multicore --
    // a long REPL session's bindings accumulate, the accepted cost of a live
    // shell). The transcript is copied out by the caller after the heap restores.
    lisp_heap_t *prev = lisp_gc_set_alloc_heap(lisp_gc_system_heap());
    int r = lisp_repl_eval(in, len, env, out, cap);
    lisp_gc_set_alloc_heap(prev);
    return r;
}
