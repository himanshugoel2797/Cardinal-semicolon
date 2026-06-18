// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

// Conservative-or-precise, non-moving mark-sweep garbage collector with
// PER-CONTEXT heaps (K3 of the process model -- see notes/core/lisp-substrate.md).
//
// There is one shared SYSTEM heap and zero or more per-context heaps:
//
//   - The system heap holds shared-immutable data (interned symbols), the global
//     environment / prelude, the scheduler structures, and the context objects
//     themselves -- everything that may be referenced across contexts. It is
//     collected CONSERVATIVELY (callee-saved registers + the C stack + the intern
//     table as roots), because main-line C code holds live values in locals.
//   - Each green-thread context may own its own heap holding only its transient
//     working data. It is collected PRECISELY from the context's explicit CEK
//     registers (control/env/accum/kont/mailbox) -- possible because that state is
//     heap-resident -- and only at a SAFE POINT (between reductions, driven by the
//     interpreter loop), where no C temporary holds an un-rooted value.
//
// Soundness rests on the cross-heap discipline: a context heap's objects may point
// INTO the system heap (read-only shared data) but the system heap never points
// into a context heap, and contexts never share heap objects with each other
// (messages are deep-copied into the receiver's heap -- see sched.c). A collection
// therefore only ever sweeps its OWN heap; a pointer into another heap is an
// external root: marked-and-stopped, never traced or freed. The single mechanism
// that makes this work is that mark_push only marks objects belonging to the heap
// being collected (validated against an address set built from that heap).
//
// x86_64-only (the register capture is inline asm); the kernel has no setjmp.

#include <stdint.h>
#include <stdlib.h>

#include "internal.h"

typedef struct gc_obj {
    struct gc_obj *next;  // intrusive per-heap all-objects list
    uintptr_t mark;       // 0 / 1
} gc_obj;
// The 16-byte header keeps the payload at least 8-aligned (malloc returns >=
// 8-aligned), so payload pointers have their low 3 bits clear -- the conservative
// filter and the address hash rely on that.

// A heap: an intrusive object list plus its counters and collection policy.
struct lisp_heap {
    gc_obj *all;
    size_t live;
    size_t bytes_since;
    size_t collections;
    size_t threshold;
    int enabled;
    int want_gc;        // a context heap defers collection to the next safe point
    lisp_value owner;   // owning context value, or LISP_EMPTY for the system heap
};

#define GC_THRESHOLD (256 * 1024)

// The shared system heap, and the heap that lisp_gc_alloc currently targets.
static struct lisp_heap g_system = {NULL, 0, 0, 0, GC_THRESHOLD, 0, 0, LISP_EMPTY};
static struct lisp_heap *g_alloc_heap = &g_system;
static void *g_stack_base = NULL;  // for the system heap's conservative scan

static gc_obj *hdr(lisp_value v) { return (gc_obj *)(uintptr_t)v - 1; }

// --- object-address set (membership = "belongs to the heap being collected") -

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

static int build_set(struct lisp_heap *h) {
    size_t cap = 1024;
    while (cap < h->live * 2)
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
    for (gc_obj *o = h->all; o != NULL; o = o->next) {
        uintptr_t a = (uintptr_t)(o + 1);
        size_t i = (a >> 3) & mask;
        while (g_set[i])
            i = (i + 1) & mask;
        g_set[i] = a;
    }
    return 1;
}

// --- mark worklist (shared scratch; one collection runs at a time) -----------

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

// Mark a value and enqueue it for tracing -- but ONLY if it belongs to the heap
// being collected (its payload address is in g_set). A pointer to another heap
// (shared system data, or another context) is an external root: it is neither
// marked nor traced nor swept here. This is the whole cross-heap firewall.
static void mark_push(lisp_value v) {
    if (!lisp_is_ptr(v))
        return;
    if (!set_contains((uintptr_t)v))
        return;  // external object: mark-and-stop
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
            // The CEK registers are the precise roots of a (possibly suspended)
            // context; err is a static C string and `heap` is a side table, not
            // traced. NB: when a context owns its own heap, its working data lives
            // there, so the ctx object itself (in the system heap) mark-stops at
            // those children during a SYSTEM collection.
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

// --- conservative roots (system heap only) ----------------------------------

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

// Collect a single heap. System heaps (owner == LISP_EMPTY) root conservatively
// from the C stack/registers + the intern table; context heaps root precisely
// from the owning context's CEK registers. Either way only this heap's objects
// are marked (mark_push gate) and only this heap's list is swept.
static void collect_heap(struct lisp_heap *h) {
    if (h->owner == LISP_EMPTY && g_stack_base == NULL)
        return;  // system heap before init: no roots to scan -> keep everything

    g_mark_oom = 0;
    g_ms_len = 0;
    if (!build_set(h))
        return;  // can't validate roots -> skip this cycle (keep everything)

    if (h->owner == LISP_EMPTY) {
        // Precise root: the interned-symbol table (shared-immutable region).
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
    } else {
        // Precise roots: the owning context's explicit execution state. Symbols
        // and the shared environment it references live in the system heap and are
        // external (mark-stopped) -- they are not roots of this collection.
        lisp_ctx_t *o = (lisp_ctx_t *)lisp_obj(h->owner);
        mark_push(o->control);
        mark_push(o->env);
        mark_push(o->accum);
        mark_push(o->kont);
        mark_push(o->mailbox);
    }

    while (g_ms_len > 0)
        trace(g_ms[--g_ms_len]);

    gc_obj **link = &h->all;
    gc_obj *o = h->all;
    while (o != NULL) {
        gc_obj *next = o->next;
        if (o->mark || g_mark_oom) {
            o->mark = 0;
            link = &o->next;
        } else {
            *link = next;
            free(o);
            h->live--;
        }
        o = next;
    }

    free(g_set);
    g_set = NULL;
    g_set_cap = 0;
    h->bytes_since = 0;
    h->want_gc = 0;
    h->collections++;
}

// --- allocation -------------------------------------------------------------

void *lisp_gc_alloc(size_t size) {
    struct lisp_heap *h = g_alloc_heap;
    if (h->enabled && h->bytes_since >= h->threshold) {
        // The system heap collects in place (its conservative scan covers C
        // temporaries). A context heap defers: collecting mid-step would miss
        // values held only in C locals (e.g. a do_call args[] array), so it is
        // unsafe until the interpreter loop reaches a safe point.
        if (h->owner == LISP_EMPTY)
            collect_heap(h);
        else
            h->want_gc = 1;
    }
    gc_obj *o = (gc_obj *)malloc(sizeof(gc_obj) + size);
    if (o == NULL) {  // last-ditch: collect what we safely can, then retry
        if (h->owner == LISP_EMPTY)
            collect_heap(h);
        o = (gc_obj *)malloc(sizeof(gc_obj) + size);
        if (o == NULL)
            return NULL;
    }
    o->next = h->all;
    o->mark = 0;
    h->all = o;
    h->live++;
    h->bytes_since += size;
    return (void *)(o + 1);
}

// Allocate into the shared system heap regardless of the current target. Interned
// symbols use this: they are the shared-immutable region and must never live in a
// per-context heap (other contexts reference them).
void *lisp_gc_alloc_shared(size_t size) {
    struct lisp_heap *prev = g_alloc_heap;
    g_alloc_heap = &g_system;
    void *p = lisp_gc_alloc(size);
    g_alloc_heap = prev;
    return p;
}

// --- per-context heap API ---------------------------------------------------

struct lisp_heap *lisp_gc_set_alloc_heap(struct lisp_heap *h) {
    struct lisp_heap *prev = g_alloc_heap;
    g_alloc_heap = h;
    return prev;
}

struct lisp_heap *lisp_gc_system_heap(void) { return &g_system; }

struct lisp_heap *lisp_heap_new(lisp_value owner) {
    struct lisp_heap *h = (struct lisp_heap *)malloc(sizeof(struct lisp_heap));
    if (h == NULL)
        return NULL;
    h->all = NULL;
    h->live = 0;
    h->bytes_since = 0;
    h->collections = 0;
    h->threshold = GC_THRESHOLD;
    h->enabled = 1;
    h->want_gc = 0;
    h->owner = owner;
    return h;
}

void lisp_heap_collect(struct lisp_heap *h) { collect_heap(h); }

int lisp_heap_wants_gc(struct lisp_heap *h) { return h->want_gc; }

size_t lisp_heap_live(struct lisp_heap *h) { return h->live; }

size_t lisp_heap_collections(struct lisp_heap *h) { return h->collections; }

void lisp_heap_free(struct lisp_heap *h) {
    if (h == NULL)
        return;
    if (g_alloc_heap == h)  // never leave the allocator targeting a freed heap
        g_alloc_heap = &g_system;
    gc_obj *o = h->all;
    while (o != NULL) {
        gc_obj *next = o->next;
        free(o);
        o = next;
    }
    free(h);
}

// --- system heap (back-compatible global API) -------------------------------

void lisp_gc_collect(void) { collect_heap(&g_system); }

void lisp_gc_init(void *stack_base) {
    g_stack_base = stack_base;
    g_system.enabled = 1;
}

size_t lisp_gc_live_count(void) { return g_system.live; }
size_t lisp_gc_collections(void) { return g_system.collections; }
