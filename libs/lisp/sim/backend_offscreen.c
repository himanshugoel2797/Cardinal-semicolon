// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

// Headless backend: present writes the framebuffer to a PPM file and input
// comes from a scripted text file. This is the CI / no-display path -- it runs
// on a server over SSH and produces a deterministic image to assert against. It
// drives the EXACT same fake drivers and servers as the X11 backend.
//
// Env knobs:
//   SIM_PPM       output image path (default "sim-frame.ppm"); overwritten each
//                 present, so it holds the LATEST frame at exit.
//   SIM_PPM_ALL=1 also write numbered frames sim-NNNN.ppm (every present).
//   SIM_SCRIPT    input script path; one event per line:
//                   key <scancode> <1|0>     scancode = decimal, 0xHEX, or 'c
//                   char <c> <1|0>           a character -> its scancode
//                   pointer <x> <y> <1|0>
//                   wait                     deliver nothing this poll (a beat)
//                   quit                     stop the run
//                   # ...                    comment / blank lines ignored
//                 With no SIM_SCRIPT the sim renders the initial frame and quits.

#include "backend.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    sim_event *evs;
    int nev, pos;
    int w, h;
    int frame;
    int drain_after;  // idle polls left after the script, before we emit QUIT
    bool quit_sent;
} off_state;

// Parse a scancode token: decimal, 0xHEX, or 'c (a quoted char -> scancode).
static int parse_scancode(const char *tok) {
    if (tok[0] == '\'' && tok[1] != '\0')
        return sim_keysym_to_scancode((unsigned long)(unsigned char)tok[1]);
    return (int)strtol(tok, NULL, 0);
}

static sim_event *load_script(const char *path, int *count) {
    *count = 0;
    FILE *f = fopen(path, "r");
    if (f == NULL)
        return NULL;
    int cap = 64, n = 0;
    sim_event *evs = (sim_event *)malloc((size_t)cap * sizeof(sim_event));
    char line[256];
    while (fgets(line, sizeof line, f) != NULL) {
        char op[32], t1[32], t2[32], t3[32];
        int got = sscanf(line, "%31s %31s %31s %31s", op, t1, t2, t3);
        if (got <= 0 || op[0] == '#')
            continue;
        if (n == cap) {
            cap *= 2;
            sim_event *grown =
                (sim_event *)realloc(evs, (size_t)cap * sizeof(sim_event));
            if (grown == NULL) {  // keep what we have rather than leak/crash
                fclose(f);
                *count = n;
                return evs;
            }
            evs = grown;
        }
        sim_event *e = &evs[n];
        memset(e, 0, sizeof *e);
        if (strcmp(op, "key") == 0 && got >= 3) {
            e->type = SIM_EV_KEY;
            e->a = parse_scancode(t1);
            e->b = atoi(t2);
            n++;
        } else if (strcmp(op, "char") == 0 && got >= 3) {
            e->type = SIM_EV_KEY;
            e->a = sim_keysym_to_scancode((unsigned long)(unsigned char)t1[0]);
            e->b = atoi(t2);
            n++;
        } else if (strcmp(op, "pointer") == 0 && got >= 4) {
            e->type = SIM_EV_POINTER;
            e->a = atoi(t1);
            e->b = atoi(t2);
            e->c = atoi(t3);
            n++;
        } else if (strcmp(op, "quit") == 0) {
            e->type = SIM_EV_QUIT;
            n++;
        } else if (strcmp(op, "wait") == 0) {
            // A no-op beat: a POINTER with sentinel so poll() emits nothing but
            // still consumes a slot (gives the app a poll to react + present).
            e->type = SIM_EV_POINTER;
            e->a = e->b = e->c = -1;
            n++;
        }
        // unknown ops are silently skipped
    }
    fclose(f);
    *count = n;
    return evs;
}

static bool off_open(sim_backend *be, int w, int h, const char *title) {
    (void)title;
    off_state *st = (off_state *)calloc(1, sizeof(off_state));
    st->w = w;
    st->h = h;
    const char *script = getenv("SIM_SCRIPT");
    if (script != NULL)
        st->evs = load_script(script, &st->nev);
    st->drain_after = 4;  // a few polls so the final frame renders before QUIT
    be->impl = st;
    return true;
}

static void write_ppm(const char *path, const uint8_t *px, int w, int h,
                      int stride) {
    FILE *f = fopen(path, "wb");
    if (f == NULL)
        return;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int y = 0; y < h; y++) {
        const uint8_t *row = px + (size_t)y * (size_t)stride;
        for (int x = 0; x < w; x++) {
            // pixel = 0x00RRGGBB little-endian -> bytes B,G,R,X. Emit R,G,B.
            uint8_t b = row[x * 4 + 0], g = row[x * 4 + 1], r = row[x * 4 + 2];
            fputc(r, f);
            fputc(g, f);
            fputc(b, f);
        }
    }
    fclose(f);
}

static void off_present(sim_backend *be, const uint8_t *px, int w, int h,
                        int stride) {
    off_state *st = (off_state *)be->impl;
    const char *out = getenv("SIM_PPM");
    write_ppm(out ? out : "sim-frame.ppm", px, w, h, stride);
    if (getenv("SIM_PPM_ALL") != NULL) {
        char path[64];
        snprintf(path, sizeof path, "sim-%04d.ppm", st->frame);
        write_ppm(path, px, w, h, stride);
    }
    st->frame++;
}

static int off_poll(sim_backend *be, sim_event *out, int max) {
    off_state *st = (off_state *)be->impl;
    if (max < 1)
        return 0;
    // Deliver one scripted event per poll so the app gets a present in between.
    if (st->pos < st->nev) {
        sim_event e = st->evs[st->pos++];
        if (e.type == SIM_EV_POINTER && e.a == -1)
            return 0;  // a "wait" beat: nothing this poll
        out[0] = e;
        return 1;
    }
    // Script exhausted: a few idle polls, then a single QUIT.
    if (st->quit_sent)
        return 0;
    if (st->drain_after-- > 0)
        return 0;
    out[0].type = SIM_EV_QUIT;
    st->quit_sent = true;
    return 1;
}

static void off_close(sim_backend *be) {
    off_state *st = (off_state *)be->impl;
    if (st == NULL)
        return;
    free(st->evs);
    free(st);
    be->impl = NULL;
}

sim_backend *sim_backend_offscreen(void) {
    static sim_backend be;
    be.name = "offscreen";
    be.open = off_open;
    be.present = off_present;
    be.poll = off_poll;
    be.close = off_close;
    be.impl = NULL;
    return &be;
}
