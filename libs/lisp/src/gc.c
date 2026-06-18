// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

// Conservative, non-moving mark-sweep garbage collector.
//
// Every heap object is allocated with a small gc_obj header prepended and linked
// onto a global all-objects list. Collection:
//   1. Build a hash set of live object addresses (to validate conservative roots).
//   2. Mark precise roots: the interned-symbol table.
//   3. Mark conservative roots: callee-saved registers + the C stack between the
//      current SP and the recorded stack base. Any word that is 16-aligned,
//      non-zero, and a known object address is treated as a root.
//   4. Trace reachable objects PRECISELY (by type tag) via an explicit mark
//      worklist -- no C recursion, so deep/long structures can't overflow.
//   5. Sweep: free unmarked objects; clear marks on survivors.
//
// Conservative roots over-approximate (may retain a little garbage) but never
// free something reachable, so correct programs are never broken. It is
// non-moving, which is why conservative roots are sound; a future *moving*
// collector would need precise roots (the bytecode VM's value stack).
//
// x86_64-only (the register capture is inline asm); the kernel has no setjmp.

#include <stdint.h>
#include <stdlib.h>

#include "internal.h"

typedef struct gc_obj {
    struct gc_obj *next;  // intrusive all-objects list
    uintptr_t mark;       // 0 / 1
} gc_obj;
// The 16-byte header keeps the payload at least 8-aligned (malloc returns >=
// 8-aligned), so payload pointers have their low 3 bits clear -- the conservative
// filter and the address hash rely on that.

static gc_obj *g_all = NULL;
static size_t g_live = 0;
static size_t g_bytes_since = 0;
static size_t g_collections = 0;
static void *g_stack_base = NULL;
static int g_enabled = 0;

#define GC_THRESHOLD (256 * 1024)

static gc_obj *hdr(lisp_value v) { return (gc_obj *)(uintptr_t)v - 1; }

// --- mark worklist ----------------------------------------------------------

static lisp_value *g_ms = NULL;
static size_t g_ms_len = 0, g_ms_cap = 0;
static int g_mark_oom = 0;  // set if the worklist couldn't grow -> keep everything

static void ms_push(lisp_value v) {
    if (g_ms_len == g_ms_cap) {
        size_t nc = g_ms_cap ? g_ms_cap * 2 : 1024;
        lisp_value *n = (lisp_value *)realloc(g_ms, nc * sizeof(lisp_value));
        if (n == NULL) {
            g_mark_oom = 1;  // safe fallback: sweep will keep all objects this cycle
            return;
        }
        g_ms = n;
        g_ms_cap = nc;
    }
    g_ms[g_ms_len++] = v;
}

// Mark a value if it is an unmarked heap object, and enqueue it for tracing.
static void mark_push(lisp_value v) {
    if (!lisp_is_ptr(v))
        return;
    gc_obj *o = hdr(v);
    if (o->mark)
        return;
    o->mark = 1;
    ms_push(v);
}

// Enqueue the (precise) children of an already-marked object.
static void trace(lisp_value v) {
    switch (LISP_HDR_TYPE(lisp_obj(v))) {
        case LISP_OBJ_PAIR:
            mark_push(lisp_car(v));
            mark_push(lisp_cdr(v));
            break;
        case LISP_OBJ_VECTOR: {
            size_t n = lisp_vector_length(v);
            for (size_t i = 0; i < n; i++)
                mark_push(lisp_vector_ref(v, i));
            break;
        }
        case LISP_OBJ_ENV: {
            lisp_env_t *e = (lisp_env_t *)lisp_obj(v);
            mark_push(e->parent);
            mark_push(e->bindings);
            break;
        }
        case LISP_OBJ_CLOSURE: {
            lisp_closure_t *c = (lisp_closure_t *)lisp_obj(v);
            mark_push(c->params);
            mark_push(c->body);
            mark_push(c->env);
            break;
        }
        case LISP_OBJ_PRIMITIVE:
            mark_push(((lisp_prim_t *)lisp_obj(v))->name);
            break;
        case LISP_OBJ_KONT: {
            lisp_kont_t *k = (lisp_kont_t *)lisp_obj(v);
            mark_push(k->next);
            mark_push(k->env);
            mark_push(k->a);
            mark_push(k->b);
            mark_push(k->c);
            break;
        }
        case LISP_OBJ_CTX: {
            // The four CEK registers are the precise roots of a (possibly
            // suspended) context; err is a static C string, not traced.
            lisp_ctx_t *c = (lisp_ctx_t *)lisp_obj(v);
            mark_push(c->control);
            mark_push(c->env);
            mark_push(c->accum);
            mark_push(c->kont);
            mark_push(c->mailbox);
            break;
        }
        default:
            break;  // symbol / keyword / string / flonum are leaves
    }
}

// --- object-address set (for validating conservative roots) -----------------

static uintptr_t *g_set = NULL;
static size_t g_set_cap = 0;

static int set_contains(uintptr_t a) {
    if (g_set_cap == 0)
        return 0;
    size_t mask = g_set_cap - 1;
    size_t i = (a >> 3) & mask;  // payloads are 8-aligned
    while (g_set[i]) {
        if (g_set[i] == a)
            return 1;
        i = (i + 1) & mask;
    }
    return 0;
}

static int build_set(void) {
    size_t cap = 1024;
    while (cap < g_live * 2)
        cap <<= 1;
    free(g_set);
    g_set = (uintptr_t *)malloc(cap * sizeof(uintptr_t));
    if (g_set == NULL) {
        g_set_cap = 0;
        return 0;
    }
    for (size_t i = 0; i < cap; i++)
        g_set[i] = 0;
    g_set_cap = cap;
    size_t mask = cap - 1;
    for (gc_obj *o = g_all; o != NULL; o = o->next) {
        uintptr_t a = (uintptr_t)(o + 1);
        size_t i = (a >> 3) & mask;
        while (g_set[i])
            i = (i + 1) & mask;
        g_set[i] = a;
    }
    return 1;
}

static void consider(uintptr_t w) {
    if (w != 0 && (w & 0x7) == 0 && set_contains(w))  // payloads are 8-aligned
        mark_push((lisp_value)w);
}

static void scan_range(uintptr_t *lo, uintptr_t *hi) {
    for (uintptr_t *p = lo; p < hi; p++)
        consider(*p);
}

// Capture the callee-saved registers so a live value held only in one of them
// (never spilled to the stack) is still seen as a root.
static void capture_registers(uintptr_t regs[6]) {
    __asm__ __volatile__(
        "movq %%rbx,  0(%0)\n\t"
        "movq %%rbp,  8(%0)\n\t"
        "movq %%r12, 16(%0)\n\t"
        "movq %%r13, 24(%0)\n\t"
        "movq %%r14, 32(%0)\n\t"
        "movq %%r15, 40(%0)\n\t"
        :
        : "r"(regs)
        : "memory");
}

// --- collection -------------------------------------------------------------

void lisp_gc_collect(void) {
    if (g_stack_base == NULL)  // not initialized -> scanning would be unsafe
        return;
    g_mark_oom = 0;
    g_ms_len = 0;
    if (!build_set())
        return;  // can't validate roots -> skip this cycle (keep everything)

    // Precise roots: the interned-symbol table.
    size_t icap;
    lisp_value *itab = lisp_intern_table(&icap);
    for (size_t i = 0; i < icap; i++)
        if (itab[i] != 0)
            mark_push(itab[i]);

    // Conservative roots: callee-saved registers, then the C stack.
    uintptr_t regs[6];
    capture_registers(regs);
    for (int i = 0; i < 6; i++)
        consider(regs[i]);

    uintptr_t marker;
    uintptr_t *lo = &marker;
    uintptr_t *hi = (uintptr_t *)g_stack_base;
    if (lo > hi) {
        uintptr_t *t = lo;
        lo = hi;
        hi = t;
    }
    scan_range(lo, hi);

    // Trace.
    while (g_ms_len > 0)
        trace(g_ms[--g_ms_len]);

    // Sweep.
    gc_obj **link = &g_all;
    gc_obj *o = g_all;
    while (o != NULL) {
        gc_obj *next = o->next;
        if (o->mark || g_mark_oom) {
            o->mark = 0;
            link = &o->next;
        } else {
            *link = next;
            free(o);
            g_live--;
        }
        o = next;
    }

    free(g_set);
    g_set = NULL;
    g_set_cap = 0;
    g_bytes_since = 0;
    g_collections++;
}

// --- allocation -------------------------------------------------------------

void *lisp_gc_alloc(size_t size) {
    if (g_enabled && g_bytes_since >= GC_THRESHOLD)
        lisp_gc_collect();
    gc_obj *o = (gc_obj *)malloc(sizeof(gc_obj) + size);
    if (o == NULL) {  // last-ditch: collect and retry
        lisp_gc_collect();
        o = (gc_obj *)malloc(sizeof(gc_obj) + size);
        if (o == NULL)
            return NULL;
    }
    o->next = g_all;
    o->mark = 0;
    g_all = o;
    g_live++;
    g_bytes_since += size;
    return (void *)(o + 1);
}

void lisp_gc_init(void *stack_base) {
    g_stack_base = stack_base;
    g_enabled = 1;
}

size_t lisp_gc_live_count(void) { return g_live; }
size_t lisp_gc_collections(void) { return g_collections; }
