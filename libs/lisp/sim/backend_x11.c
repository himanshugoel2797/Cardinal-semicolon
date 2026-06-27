// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

// Interactive X11 backend: a real window that shows the Lisp framebuffer and
// pipes the host keyboard/mouse into the OS input path. Built only when the X11
// headers are present (-DHAVE_X11, set by build.sh); without them this file
// compiles to a stub factory that returns NULL so the simulator falls back to
// the offscreen backend.

#include <stddef.h>

#include "backend.h"

#ifndef HAVE_X11

sim_backend *sim_backend_x11(void) { return NULL; }

#else

#include <stdlib.h>
#include <string.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>

typedef struct {
    Display *dpy;
    Window win;
    GC gc;
    XImage *img;
    Atom wm_delete;
    int w, h;
    uint32_t *pixels;  // img-owned 32bpp buffer, BGRA in memory
} x11_state;

static bool x11_open(sim_backend *be, int w, int h, const char *title) {
    Display *dpy = XOpenDisplay(NULL);
    if (dpy == NULL)
        return false;  // no DISPLAY / no X server -> caller falls back

    int screen = DefaultScreen(dpy);
    Window root = RootWindow(dpy, screen);
    Visual *visual = DefaultVisual(dpy, screen);
    int depth = DefaultDepth(dpy, screen);

    Window win = XCreateSimpleWindow(dpy, root, 0, 0, (unsigned)w, (unsigned)h, 0,
                                     BlackPixel(dpy, screen), BlackPixel(dpy, screen));
    XStoreName(dpy, win, title);
    // Lock the size: the framebuffer is fixed, so disallow resize.
    XSizeHints *hints = XAllocSizeHints();
    hints->flags = PMinSize | PMaxSize;
    hints->min_width = hints->max_width = w;
    hints->min_height = hints->max_height = h;
    XSetWMNormalHints(dpy, win, hints);
    XFree(hints);

    XSelectInput(dpy, win,
                 KeyPressMask | KeyReleaseMask | ButtonPressMask |
                     ButtonReleaseMask | PointerMotionMask | ExposureMask |
                     StructureNotifyMask);
    Atom wm_delete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy, win, &wm_delete, 1);
    XMapWindow(dpy, win);

    uint32_t *pixels = (uint32_t *)malloc((size_t)w * (size_t)h * 4);
    if (pixels == NULL) {
        XDestroyWindow(dpy, win);
        XCloseDisplay(dpy);
        return false;
    }
    memset(pixels, 0, (size_t)w * (size_t)h * 4);
    XImage *img = XCreateImage(dpy, visual, (unsigned)depth, ZPixmap, 0,
                               (char *)pixels, (unsigned)w, (unsigned)h, 32, 0);
    if (img == NULL) {  // unsupported depth / bad visual on an exotic server
        free(pixels);
        XDestroyWindow(dpy, win);
        XCloseDisplay(dpy);
        return false;
    }
    // Our pixels are 0x00RRGGBB host-endian; force the interpretation regardless
    // of server byte order / visual masks so colours are correct everywhere.
    img->byte_order = LSBFirst;
    img->red_mask = 0x00FF0000;
    img->green_mask = 0x0000FF00;
    img->blue_mask = 0x000000FF;

    x11_state *st = (x11_state *)calloc(1, sizeof(x11_state));
    if (st == NULL) {
        XDestroyImage(img);  // frees pixels too
        XDestroyWindow(dpy, win);
        XCloseDisplay(dpy);
        return false;
    }
    st->dpy = dpy;
    st->win = win;
    st->gc = XCreateGC(dpy, win, 0, NULL);
    st->img = img;
    st->wm_delete = wm_delete;
    st->w = w;
    st->h = h;
    st->pixels = pixels;
    be->impl = st;
    return true;
}

static void x11_present(sim_backend *be, const uint8_t *px, int w, int h,
                        int stride) {
    x11_state *st = (x11_state *)be->impl;
    if (st == NULL)
        return;
    int rows = h < st->h ? h : st->h;
    int cols = w < st->w ? w : st->w;
    // Copy row by row: the source stride may differ from w*4, and the XImage is
    // tightly packed at st->w*4.
    for (int y = 0; y < rows; y++) {
        const uint8_t *src = px + (size_t)y * (size_t)stride;
        uint32_t *dst = st->pixels + (size_t)y * (size_t)st->w;
        memcpy(dst, src, (size_t)cols * 4);
    }
    XPutImage(st->dpy, st->win, st->gc, st->img, 0, 0, 0, 0, (unsigned)st->w,
              (unsigned)st->h);
    XFlush(st->dpy);
}

static int x11_poll(sim_backend *be, sim_event *out, int max) {
    x11_state *st = (x11_state *)be->impl;
    if (st == NULL)
        return 0;
    int n = 0;
    while (n < max && XPending(st->dpy) > 0) {
        XEvent ev;
        XNextEvent(st->dpy, &ev);
        switch (ev.type) {
            case KeyPress:
            case KeyRelease: {
                KeySym ks = XLookupKeysym(&ev.xkey, 0);
                int sc = sim_keysym_to_scancode((unsigned long)ks);
                if (sc >= 0) {
                    out[n].type = SIM_EV_KEY;
                    out[n].a = sc;
                    out[n].b = (ev.type == KeyPress) ? 1 : 0;
                    n++;
                }
                break;
            }
            case ButtonPress:
            case ButtonRelease:
                if (ev.xbutton.button == Button1) {
                    out[n].type = SIM_EV_POINTER;
                    out[n].a = ev.xbutton.x;
                    out[n].b = ev.xbutton.y;
                    out[n].c = (ev.type == ButtonPress) ? 1 : 0;
                    n++;
                }
                break;
            case MotionNotify:
                out[n].type = SIM_EV_POINTER;
                out[n].a = ev.xmotion.x;
                out[n].b = ev.xmotion.y;
                // Button1 held during the drag -> down, else a hover (up).
                out[n].c = (ev.xmotion.state & Button1Mask) ? 1 : 0;
                n++;
                break;
            case ClientMessage:
                if ((Atom)ev.xclient.data.l[0] == st->wm_delete) {
                    out[n].type = SIM_EV_QUIT;
                    n++;
                }
                break;
            default:
                break;
        }
    }
    return n;
}

static void x11_close(sim_backend *be) {
    x11_state *st = (x11_state *)be->impl;
    if (st == NULL)
        return;
    // XDestroyImage frees st->pixels (the data we handed XCreateImage).
    XDestroyImage(st->img);
    XFreeGC(st->dpy, st->gc);
    XDestroyWindow(st->dpy, st->win);
    XCloseDisplay(st->dpy);
    free(st);
    be->impl = NULL;
}

sim_backend *sim_backend_x11(void) {
    static sim_backend be;
    be.name = "x11";
    be.open = x11_open;
    be.present = x11_present;
    be.poll = x11_poll;
    be.close = x11_close;
    be.impl = NULL;
    return &be;
}

#endif  // HAVE_X11
