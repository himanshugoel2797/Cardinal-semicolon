// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

// The host-only Lisp primitives the simulator adds on top of the normal Lisp
// core. They are the seam between the OS-side fake drivers (pure Lisp) and the
// host backend:
//
//   (host-screen-size)              -> (width height)
//   (host-present! fb w h stride)   -> push a frame to the backend; #t
//   (host-input-poll)               -> a list of pending input events, each
//                                      (key  <scancode> <pressed 1|0>) or
//                                      (pointer <x> <y> <down? #t|#f>)
//
// These mirror exactly what a real display/input driver would do over MMIO and
// IRQs -- but backed by an X11 window or a PPM/script file. The run loop in
// sim_main.c pumps the backend into a small queue here; (host-input-poll)
// drains it. QUIT is handled in sim_main, not surfaced to Lisp.

#include "backend.h"
#include "lisp.h"

#include <string.h>

// --- shared state set up by sim_main ---------------------------------------

static sim_backend *g_backend;
static int g_screen_w, g_screen_h;

// A small queue of input events waiting for the next (host-input-poll). Filled
// by sim_host_enqueue (from the run loop), drained by prim_input_poll.
#define HOST_Q_CAP 512
static sim_event g_queue[HOST_Q_CAP];
static int g_qn;

void sim_host_init(sim_backend *be, int w, int h) {
    g_backend = be;
    g_screen_w = w;
    g_screen_h = h;
    g_qn = 0;
}

// Append backend events to the queue (dropping on overflow -- input is sampled
// fresh each frame, so a momentary flood losing a stale event is harmless).
void sim_host_enqueue(const sim_event *evs, int n) {
    for (int i = 0; i < n; i++) {
        if (g_qn < HOST_Q_CAP)
            g_queue[g_qn++] = evs[i];
    }
}

// --- primitives -------------------------------------------------------------

static lisp_value prim_err(const char **err, const char *msg) {
    if (err != NULL)
        *err = msg;
    return LISP_UNDEF;
}

// (host-screen-size) -> (w h)
static lisp_value prim_screen_size(lisp_value *args, int argc,
                                   const char **err) {
    (void)args;
    if (argc != 0)
        return prim_err(err, "host-screen-size takes no arguments");
    return lisp_cons(lisp_fixnum(g_screen_w),
                     lisp_cons(lisp_fixnum(g_screen_h), LISP_EMPTY));
}

// (host-present! fb w h stride) -> #t. fb is a bytes framebuffer in 0x00RRGGBB.
static lisp_value prim_present(lisp_value *args, int argc, const char **err) {
    if (argc != 4 || !lisp_is_bytes(args[0]) || !lisp_is_fixnum(args[1]) ||
        !lisp_is_fixnum(args[2]) || !lisp_is_fixnum(args[3]))
        return prim_err(err, "host-present! expects (bytes w h stride)");
    int w = (int)lisp_fixnum_val(args[1]);
    int h = (int)lisp_fixnum_val(args[2]);
    int stride = (int)lisp_fixnum_val(args[3]);
    if (w <= 0 || h <= 0 || stride < w * 4)
        return prim_err(err, "host-present!: bad dimensions");
    if (lisp_bytes_len(args[0]) < (size_t)stride * (size_t)h)
        return prim_err(err, "host-present!: framebuffer too small");
    if (g_backend != NULL && g_backend->present != NULL)
        g_backend->present(g_backend, (const uint8_t *)lisp_bytes_data(args[0]),
                           w, h, stride);
    return LISP_TRUE;
}

// Build one event tuple. Locals are stack roots (conservative GC), so the
// intermediate conses are safe across allocation.
static lisp_value make_key_event(int sc, int pressed) {
    lisp_value tail = lisp_cons(lisp_fixnum(pressed), LISP_EMPTY);
    tail = lisp_cons(lisp_fixnum(sc), tail);
    return lisp_cons(lisp_make_symbol("key", 3), tail);
}

static lisp_value make_pointer_event(int x, int y, int down) {
    lisp_value tail = lisp_cons(down ? LISP_TRUE : LISP_FALSE, LISP_EMPTY);
    tail = lisp_cons(lisp_fixnum(y), tail);
    tail = lisp_cons(lisp_fixnum(x), tail);
    return lisp_cons(lisp_make_symbol("pointer", 7), tail);
}

// (host-input-poll) -> list of event tuples queued since the last call.
static lisp_value prim_input_poll(lisp_value *args, int argc,
                                  const char **err) {
    (void)args;
    if (argc != 0)
        return prim_err(err, "host-input-poll takes no arguments");
    // Build the list newest-first, then it is naturally oldest-first if we walk
    // the queue from the end; instead build from the end so the result is FIFO.
    lisp_value out = LISP_EMPTY;
    for (int i = g_qn - 1; i >= 0; i--) {
        sim_event *e = &g_queue[i];
        lisp_value ev = LISP_EMPTY;
        if (e->type == SIM_EV_KEY)
            ev = make_key_event(e->a, e->b);
        else if (e->type == SIM_EV_POINTER)
            ev = make_pointer_event(e->a, e->b, e->c);
        else
            continue;  // QUIT never reaches Lisp
        out = lisp_cons(ev, out);
    }
    g_qn = 0;
    return out;
}

static void def(lisp_value env, const char *name, lisp_primitive_fn fn) {
    lisp_value sym = lisp_make_symbol(name, strlen(name));
    lisp_value prim = lisp_make_primitive(fn, name);
    lisp_env_define(env, sym, prim);
}

void sim_host_install_prims(lisp_value env) {
    def(env, "host-screen-size", prim_screen_size);
    def(env, "host-present!", prim_present);
    def(env, "host-input-poll", prim_input_poll);
}
