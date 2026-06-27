// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

// The host-presentation backend the simulator drives. A backend turns the
// Lisp side's framebuffer + input into something on the host: an X11 window
// with a live keyboard/mouse, or an offscreen PPM dump fed by a scripted input
// file. The fake drivers and servers are identical across backends -- only this
// vtable changes -- so the same `sim` binary can run interactively on a desktop
// and headlessly (CI / this dev box) by selecting the backend at startup.

#ifndef CARDINAL_LISP_SIM_BACKEND_H
#define CARDINAL_LISP_SIM_BACKEND_H

#include <stdbool.h>
#include <stdint.h>

// One host input event, already translated into the OS's own event space so the
// Lisp side needs no host-specific knowledge:
//   KEY     -> a, b = PS/2 set-1 scancode, pressed(1/0)   [matches the ps2 driver]
//   POINTER -> a, b, c = x, y, button-down(1/0)           [screen coords]
//   QUIT    -> the window was closed; the run loop should stop.
typedef enum { SIM_EV_KEY = 0, SIM_EV_POINTER = 1, SIM_EV_QUIT = 2 } sim_ev_type;

typedef struct {
    sim_ev_type type;
    int a, b, c;
} sim_event;

// A presentation backend. All hooks may be NULL-safe only where noted; the
// simulator checks `open`'s return before using the rest.
typedef struct sim_backend {
    const char *name;

    // Create the window / open the output. Returns true on success. `w`/`h` are
    // the framebuffer size in pixels; the backend keeps them.
    bool (*open)(struct sim_backend *be, int w, int h, const char *title);

    // Push a full BGRA/X8R8G8B8 frame (pixel = 0x00RRGGBB, little-endian, so the
    // bytes are B,G,R,X) to the output. `stride` is the row stride in bytes.
    void (*present)(struct sim_backend *be, const uint8_t *px, int w, int h,
                    int stride);

    // Drain pending host input into `out` (capacity `max`); return the count.
    // Non-blocking: returns 0 when nothing is pending.
    int (*poll)(struct sim_backend *be, sim_event *out, int max);

    // Tear down.
    void (*close)(struct sim_backend *be);

    void *impl;  // backend-private state
} sim_backend;

// Factories. Each returns a ready-to-open backend, or NULL if unavailable
// (e.g. X11 not compiled in, or no DISPLAY). The offscreen backend always
// works.
sim_backend *sim_backend_x11(void);
sim_backend *sim_backend_offscreen(void);

// Translate a host KeySym (X11) to a PS/2 set-1 scancode, or -1 if unmapped.
// Shared by the X11 backend; lives in keymap.c so the table is testable.
int sim_keysym_to_scancode(unsigned long keysym);

#endif  // CARDINAL_LISP_SIM_BACKEND_H
