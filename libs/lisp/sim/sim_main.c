// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

// The Cardinal Lisp simulator: runs the OS's Lisp servers and a user app on the
// host with fake, host-backed drivers, so user-facing apps can be developed and
// tested without booting a VM. See sim/README.md.
//
// It wires the public Lisp embedding API (GC + default env + scheduler + module
// loader, all already used by libs/lisp/test/) to a host presentation backend
// (X11 window, or offscreen PPM + scripted input) and a small set of host prims
// (host_prims.c). The control flow:
//
//   init GC / env / scheduler / module loader / host prims
//   eval "(import boot)(sim-start)"   -- spawns servers + fake drivers + app
//   loop: pump backend input -> queue -> step the scheduler -> (app presents)
//
// Presentation happens inside the scheduler when the app sends a frame to the
// fake display driver, which calls host-present!.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "backend.h"
#include "lisp.h"

// Provided by host_prims.c.
void sim_host_init(sim_backend *be, int w, int h);
void sim_host_enqueue(const sim_event *evs, int n);
void sim_host_install_prims(lisp_value env);

#ifndef SIM_SRC_DIR
#define SIM_SRC_DIR "."
#endif
#ifndef LISP_ROOT_DIR
#define LISP_ROOT_DIR "lisp"
#endif

static char g_simdir[1024];
static char g_lispdir[1024];

// Module loader: the sim dir (fake drivers + demo app + boot) first, then the
// real lisp/ tree (servers, lib, drivers) -- so the harness loads real servers
// unchanged while keeping host-only Lisp out of the target initrd.
static bool sim_loader(const char *name, const char **src, size_t *len,
                       void *ctx) {
    (void)ctx;
    const char *bases[] = {g_simdir, NULL, NULL, NULL};
    char servers[1100], lib[1100], drivers[1100];
    snprintf(servers, sizeof servers, "%s/servers", g_lispdir);
    snprintf(lib, sizeof lib, "%s/lib", g_lispdir);
    snprintf(drivers, sizeof drivers, "%s/drivers", g_lispdir);
    bases[1] = servers;
    bases[2] = lib;
    bases[3] = drivers;
    for (size_t b = 0; b < sizeof bases / sizeof bases[0]; b++) {
        char path[1200];
        snprintf(path, sizeof path, "%s/%s.clp", bases[b], name);
        FILE *f = fopen(path, "rb");
        if (f == NULL)
            continue;
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (sz < 0) {  // ftell failed (e.g. not a regular file)
            fclose(f);
            return false;
        }
        char *buf = (char *)malloc((size_t)sz + 1);
        if (buf == NULL || fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
            free(buf);
            fclose(f);
            return false;
        }
        buf[sz] = '\0';
        fclose(f);
        *src = buf;
        *len = (size_t)sz;
        return true;
    }
    return false;
}

static void out_fn(const char *s, size_t len, void *ctx) {
    (void)ctx;
    fwrite(s, 1, len, stdout);
}

static int env_int(const char *name, int dflt) {
    const char *v = getenv(name);
    return v != NULL ? atoi(v) : dflt;
}

// Pick the backend: SIM_BACKEND=x11|offscreen, else auto (X11 if $DISPLAY and
// it opens, otherwise offscreen). Returns an OPENED backend or NULL.
static sim_backend *pick_backend(int w, int h) {
    const char *sel = getenv("SIM_BACKEND");
    sim_backend *be = NULL;
    if (sel != NULL && strcmp(sel, "offscreen") == 0) {
        be = sim_backend_offscreen();
    } else if (sel != NULL && strcmp(sel, "x11") == 0) {
        be = sim_backend_x11();
    } else {  // auto
        if (getenv("DISPLAY") != NULL)
            be = sim_backend_x11();
    }
    if (be != NULL && be->open(be, w, h, "Cardinal; simulator")) {
        printf("[sim] backend: %s %dx%d\n", be->name, w, h);
        return be;
    }
    // Fall back to offscreen (e.g. X11 requested/auto but no display).
    be = sim_backend_offscreen();
    if (be->open(be, w, h, "Cardinal; simulator")) {
        printf("[sim] backend: %s %dx%d\n", be->name, w, h);
        return be;
    }
    return NULL;
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    const char *sd = getenv("SIM_DIR");
    const char *ld = getenv("SIM_LISP_DIR");
    snprintf(g_simdir, sizeof g_simdir, "%s", sd ? sd : SIM_SRC_DIR);
    snprintf(g_lispdir, sizeof g_lispdir, "%s", ld ? ld : LISP_ROOT_DIR);

    int w = env_int("SIM_W", 640);
    int h = env_int("SIM_H", 480);
    int passes = env_int("SIM_PASSES", 32);
    int frame_us = env_int("SIM_FRAME_US", 16000);
    long max_iters = env_int("SIM_MAX_ITERS", 200000);

    sim_backend *be = pick_backend(w, h);
    if (be == NULL) {
        fprintf(stderr, "[sim] no usable backend\n");
        return 1;
    }

    uintptr_t stack_marker;
    lisp_gc_init(&stack_marker);
    lisp_value env = lisp_default_env();
    lisp_install_sched(env);
    lisp_set_output(out_fn, NULL);
    lisp_set_module_loader(sim_loader, NULL);
    sim_host_install_prims(env);
    sim_host_init(be, w, h);

    lisp_sched_t s;
    lisp_sched_init(&s, 4000);

    const char *err = NULL;
    lisp_eval_string("(import boot)(sim-start)", env, &err);
    if (err != NULL) {
        fprintf(stderr, "[sim] bring-up error: %s\n", err);
        be->close(be);
        return 1;
    }
    printf("[sim] running (close the window or exhaust the script to stop)\n");

    long iters = 0;
    for (;;) {
        sim_event in[128];
        int n = be->poll(be, in, 128);
        sim_event keep[128];
        int k = 0, quit = 0;
        for (int i = 0; i < n; i++) {
            if (in[i].type == SIM_EV_QUIT)
                quit = 1;
            else
                keep[k++] = in[i];
        }
        if (k > 0)
            sim_host_enqueue(keep, k);
        if (quit)
            break;

        lisp_sched_run(&s, passes);
        fflush(stdout);

        if (++iters >= max_iters) {
            printf("[sim] hit SIM_MAX_ITERS (%ld); stopping\n", max_iters);
            break;
        }
        usleep((useconds_t)frame_us);
    }

    printf("[sim] stopped after %ld iterations\n", iters);
    be->close(be);
    return 0;
}
