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
    struct gc_obj *next;  // intrusive all-objects list (live) / free list (dead)
    uint32_t mark;        // 0 / 1
    uint32_t cls;         // size class, or GC_CLS_LARGE for an individual malloc
} gc_obj;
// The 16-byte header keeps the payload at least 8-aligned (malloc/slabs return >=
// 8-aligned), so payload pointers have their low 3 bits clear -- the conservative
// filter and the address hash rely on that.

// --- pooled allocation ------------------------------------------------------
//
// Objects are not malloc'd one at a time (that dominated the profile -- one
// malloc per cons, one free per dead cell). Instead each heap bump-allocates
// fixed size-class slots out of large slabs and recycles dead slots onto a
// per-class free list, so steady churn (cons/kont) reuses memory with no libc
// alloc traffic. Slabs are released only when the whole heap is torn down. This
// stays NON-MOVING, so it is compatible with the system heap's conservative
// roots (which forbid relocation). Objects larger than the biggest class fall
// back to an individual malloc/free (rare: big vectors/strings/bytes).
#define GC_CLASS_STEP 16u                                  // slot granularity
#define GC_MAX_POOLED 512u                                 // largest pooled slot
#define GC_NUM_CLASSES (GC_MAX_POOLED / GC_CLASS_STEP)     // 32 classes
#define GC_CLS_LARGE 0xffffffffu                           // individually malloc'd
#define GC_SLAB_HDR 16u                                    // keeps carves 16-aligned
#define GC_SLAB_BYTES (64u * 1024u)                        // carve area per slab

typedef struct gc_slab {
    struct gc_slab *next;
} gc_slab;

// A heap: an intrusive object list plus its counters, free lists, and slabs.
struct lisp_heap {
    gc_obj *all;
    size_t live;
    size_t bytes_since;
    size_t collections;
    size_t threshold;
    int enabled;
    int want_gc;        // a context heap defers collection to the next safe point
    lisp_value owner;   // owning context value, or LISP_EMPTY for the system heap
    gc_obj *freelist[GC_NUM_CLASSES];  // recycled dead slots, per size class
    gc_slab *slabs;     // backing slabs (freed only at heap teardown)
    char *slab_cur;     // bump cursor into the current slab
    size_t slab_left;   // bytes remaining in the current slab
};

#define GC_THRESHOLD (256 * 1024)

// The shared system heap. Everything else (counters, free lists, slabs) is
// zero/NULL -> an empty pool, as required.
static struct lisp_heap g_system = {.threshold = GC_THRESHOLD, .owner = LISP_EMPTY};
static void *g_stack_base = NULL;  // for the system heap's conservative scan

// Size class for a total allocation (header + payload), or >= GC_NUM_CLASSES if
// it exceeds the largest pooled slot. Class i holds slots of (i + 1) * 16 bytes.
static inline uint32_t size_to_class(size_t total) {
    return (uint32_t)((total + GC_CLASS_STEP - 1) / GC_CLASS_STEP - 1);
}
static inline size_t class_slot(uint32_t cls) {
    return (size_t)(cls + 1) * GC_CLASS_STEP;
}

// The heap each core currently allocates into. PER-CORE: two cores run two
// contexts (each in its own heap) at once, so a single global target would have
// them stomp each other's choice. A NULL slot means "the system heap" (so the
// array needs no run-time initialisation). Indexed by lisp_rt_core().
static struct lisp_heap *g_alloc_heap[LISP_MAX_CORES] = {0};

// --- concurrency hooks ------------------------------------------------------
//
// Installed by the embedder via lisp_set_concurrency; no-ops / core 0 until then
// (single-threaded host tests and single-core boot). lisp_rt_lock guards the
// system heap list+counters, the intern table, and the GC mark scratch -- the
// only state shared between cores. g_multicore freezes the system heap (see
// lisp_gc_set_multicore): its conservative collector cannot see other cores'
// stacks, so it must not run once a second core is live.
static void (*g_lock_fn)(void) = NULL;
static void (*g_unlock_fn)(void) = NULL;
static int (*g_core_fn)(void) = NULL;
static int g_multicore = 0;

void lisp_rt_lock(void) {
    if (g_lock_fn != NULL)
        g_lock_fn();
}
void lisp_rt_unlock(void) {
    if (g_unlock_fn != NULL)
        g_unlock_fn();
}
// This core's small index, clamped to a valid per-core array slot. The kernel
// passes interrupt_get_cpu_idx (the APIC id), which is 0..N-1 dense on the x2APIC
// /xAPIC parts we target but is NOT guaranteed dense or < 256 on all hardware; a
// stray id must never index the per-core arrays out of bounds (it would only cost
// correct per-core isolation for that exotic core, not memory safety).
int lisp_rt_core(void) {
    int id = g_core_fn != NULL ? g_core_fn() : 0;
    return (id >= 0 && id < LISP_MAX_CORES) ? id : 0;
}

void lisp_set_concurrency(void (*lock)(void), void (*unlock)(void), int (*core_id)(void)) {
    g_lock_fn = lock;
    g_unlock_fn = unlock;
    g_core_fn = core_id;
}

void lisp_gc_set_multicore(int grow_only_system_heap) { g_multicore = grow_only_system_heap; }

// This core's current allocation-target heap (the system heap if its slot is the
// default NULL).
static struct lisp_heap *cur_alloc_heap(void) {
    struct lisp_heap *h = g_alloc_heap[lisp_rt_core()];
    return h != NULL ? h : &g_system;
}

static gc_obj *hdr(lisp_value v) { return (gc_obj *)(uintptr_t)v - 1; }

// --- object-address set (membership = "belongs to the heap being collected") -

// A PERSISTENT open-addressed set, rebuilt each collection from the heap's live
// objects. Rebuilding used to free+malloc+zero the whole table on every
// collection -- needless churn of a scratch buffer (and on the kernel's O(n)
// best-fit allocator, a big alloc/free + fragmentation each cycle). Instead the
// table is kept allocated and each slot carries the generation it was written
// in: bumping a global generation makes every prior entry logically empty with
// NO zeroing. The table is zeroed exactly once per growth (rare), never per
// collection.
typedef struct {
    uintptr_t addr;
    uint32_t gen;
} gc_set_slot;

static gc_set_slot *g_set = NULL;
static size_t g_set_cap = 0;
static uint32_t g_set_gen = 0;  // current generation; slots with gen != this are empty

static int set_contains(uintptr_t a) {
    if (g_set_cap == 0)
        return 0;
    size_t mask = g_set_cap - 1;
    size_t i = (a >> 4) & mask;  // payloads are 16-aligned (gc_obj is 16 bytes)
    while (g_set[i].gen == g_set_gen) {
        if (g_set[i].addr == a)
            return 1;
        i = (i + 1) & mask;
    }
    return 0;
}

static int build_set(struct lisp_heap *h) {
    size_t cap = 1024;
    while (cap < h->live * 2)
        cap <<= 1;
    if (cap > g_set_cap) {  // grow-only: the only place the table is zeroed
        gc_set_slot *ns = (gc_set_slot *)realloc(g_set, cap * sizeof(gc_set_slot));
        if (ns == NULL)
            return 0;  // keep the old table; caller skips this collection
        g_set = ns;
        for (size_t i = 0; i < cap; i++)
            g_set[i].gen = 0;
        g_set_cap = cap;
        g_set_gen = 0;  // the bump below makes the current generation 1
    }
    if (++g_set_gen == 0) {  // generation wrapped (after 4B collections): re-zero once
        for (size_t i = 0; i < g_set_cap; i++)
            g_set[i].gen = 0;
        g_set_gen = 1;
    }
    size_t mask = g_set_cap - 1;
    for (gc_obj *o = h->all; o != NULL; o = o->next) {
        uintptr_t a = (uintptr_t)(o + 1);
        size_t i = (a >> 4) & mask;
        while (g_set[i].gen == g_set_gen)
            i = (i + 1) & mask;
        g_set[i].addr = a;
        g_set[i].gen = g_set_gen;
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
            mark_push(e->table);  // hash buckets of a top-level frame (LISP_EMPTY otherwise)
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
//
// PRECONDITION: the caller holds lisp_rt_lock. The shared mark scratch (g_set /
// g_ms) and -- for the system heap -- the intern table are read/written here, so
// concurrent collections on two cores would corrupt them. The lock serialises
// all collection; the public entry points (lisp_gc_collect / lisp_heap_collect)
// and the allocators take it before calling in.
static void collect_heap_locked(struct lisp_heap *h) {
    if (h->owner == LISP_EMPTY && g_stack_base == NULL)
        return;  // system heap before init: no roots to scan -> keep everything
    if (h->owner == LISP_EMPTY && g_multicore)
        return;  // multi-core: the system heap is frozen (grow-only), never swept

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
            if (o->cls == GC_CLS_LARGE) {
                free(o);  // individually malloc'd
            } else {
                o->next = h->freelist[o->cls];  // recycle the slot, no libc free
                h->freelist[o->cls] = o;
            }
            h->live--;
        }
        o = next;
    }

    // g_set is kept allocated across collections (generation-tagged); not freed here.
    h->bytes_since = 0;
    h->want_gc = 0;
    h->collections++;
}

// --- allocation -------------------------------------------------------------

// Carve `slot` bytes off the heap's current slab, grabbing a fresh slab first if
// the current one can't satisfy it. Returns NULL only on slab malloc failure.
// When a fresh slab is taken the old slab's tail (< slot bytes, so < GC_MAX_POOLED)
// is stranded until heap teardown -- worst case ~0.8% of a slab, an accepted trade.
static gc_obj *slab_carve(struct lisp_heap *h, size_t slot) {
    if (h->slab_left < slot) {
        char *block = (char *)malloc(GC_SLAB_HDR + GC_SLAB_BYTES);
        if (block == NULL)
            return NULL;
        gc_slab *s = (gc_slab *)block;
        s->next = h->slabs;
        h->slabs = s;
        h->slab_cur = block + GC_SLAB_HDR;
        h->slab_left = GC_SLAB_BYTES;
    }
    gc_obj *o = (gc_obj *)h->slab_cur;
    h->slab_cur += slot;
    h->slab_left -= slot;
    return o;
}

// Obtain an uninitialised gc_obj big enough for `size` bytes of payload: pop the
// size class's free list, else carve a slot from a slab; oversized requests fall
// back to an individual malloc (tagged GC_CLS_LARGE). Sets o->cls; the caller
// links it via heap_link. Returns NULL on OOM.
static gc_obj *obj_alloc(struct lisp_heap *h, size_t size) {
    // Callers always allocate at least a lisp_header, so total >= 24 and
    // size_to_class never underflows; size 0 would wrap to the large path, which
    // is harmless but never happens.
    size_t total = sizeof(gc_obj) + size;
    uint32_t cls = size_to_class(total);
    if (cls >= GC_NUM_CLASSES) {  // larger than any class: individual malloc
        gc_obj *o = (gc_obj *)malloc(total);
        if (o != NULL)
            o->cls = GC_CLS_LARGE;
        return o;
    }
    gc_obj *o = h->freelist[cls];
    if (o != NULL) {
        h->freelist[cls] = o->next;  // recycle a dead slot
    } else {
        o = slab_carve(h, class_slot(cls));
        if (o == NULL)
            return NULL;
    }
    o->cls = cls;
    return o;
}

// Push a freshly-allocated object onto a heap's intrusive list + counters.
static void *heap_link(struct lisp_heap *h, gc_obj *o, size_t size) {
    o->next = h->all;
    o->mark = 0;
    h->all = o;
    h->live++;
    h->bytes_since += size;
    return (void *)(o + 1);
}

// Allocate into the system heap. PRECONDITION: caller holds lisp_rt_lock (the
// system heap's list/counters + the conservative-collect scratch are shared). A
// collection here roots conservatively from the calling core's C stack, which is
// only sound single-threaded -- collect_heap_locked refuses once g_multicore.
static void *sys_alloc_locked(size_t size) {
    struct lisp_heap *h = &g_system;
    if (h->enabled && h->bytes_since >= h->threshold)
        collect_heap_locked(h);  // no-op once the heap is frozen (multi-core)
    gc_obj *o = obj_alloc(h, size);
    if (o == NULL) {  // last-ditch: collect what we safely can, then retry
        collect_heap_locked(h);
        o = obj_alloc(h, size);
        if (o == NULL)
            return NULL;
    }
    return heap_link(h, o, size);
}

void *lisp_gc_alloc(size_t size) {
    struct lisp_heap *h = cur_alloc_heap();
    if (h->owner == LISP_EMPTY) {  // the shared system heap: serialise
        lisp_rt_lock();
        void *p = sys_alloc_locked(size);
        lisp_rt_unlock();
        return p;
    }

    // A per-context heap is touched only by this core, so the fast path is
    // lock-free. Collecting mid-step would miss values held only in C locals
    // (e.g. a do_call args[] array), so defer to the next safe point; only the
    // last-ditch OOM collect runs inline, and it takes the lock for the scratch.
    if (h->enabled && h->bytes_since >= h->threshold)
        h->want_gc = 1;
    gc_obj *o = obj_alloc(h, size);
    if (o == NULL) {
        lisp_rt_lock();
        collect_heap_locked(h);
        lisp_rt_unlock();
        o = obj_alloc(h, size);
        if (o == NULL)
            return NULL;
    }
    return heap_link(h, o, size);
}

// Allocate into the shared system heap regardless of the current target. Interned
// symbols use this: they are the shared-immutable region and must never live in a
// per-context heap (other contexts reference them).
void *lisp_gc_alloc_shared(size_t size) {
    lisp_rt_lock();
    void *p = sys_alloc_locked(size);
    lisp_rt_unlock();
    return p;
}

// Same, but the caller (intern) already holds the lock.
void *lisp_gc_alloc_shared_nolock(size_t size) { return sys_alloc_locked(size); }

// --- per-context heap API ---------------------------------------------------

struct lisp_heap *lisp_gc_set_alloc_heap(struct lisp_heap *h) {
    int c = lisp_rt_core();
    struct lisp_heap *prev = g_alloc_heap[c] != NULL ? g_alloc_heap[c] : &g_system;
    g_alloc_heap[c] = h;
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
    for (size_t i = 0; i < GC_NUM_CLASSES; i++)
        h->freelist[i] = NULL;
    h->slabs = NULL;
    h->slab_cur = NULL;
    h->slab_left = 0;
    return h;
}

void lisp_heap_collect(struct lisp_heap *h) {
    lisp_rt_lock();
    collect_heap_locked(h);
    lisp_rt_unlock();
}

int lisp_heap_wants_gc(struct lisp_heap *h) { return h->want_gc; }

size_t lisp_heap_live(struct lisp_heap *h) { return h->live; }

size_t lisp_heap_collections(struct lisp_heap *h) { return h->collections; }

void lisp_heap_free(struct lisp_heap *h) {
    if (h == NULL)
        return;
    // Never leave a core's allocator targeting a freed heap. A context heap is
    // only ever the target of the core that runs that context, and that same
    // core frees it (at a safe point), so resetting this core's slot suffices.
    if (g_alloc_heap[lisp_rt_core()] == h)
        g_alloc_heap[lisp_rt_core()] = NULL;
    // Pooled objects (live or free-listed) are backed by the slabs, so freeing
    // the slabs reclaims them all at once; only the large (individually malloc'd)
    // objects still on the all-list need an individual free.
    for (gc_obj *o = h->all; o != NULL;) {
        gc_obj *next = o->next;
        if (o->cls == GC_CLS_LARGE)
            free(o);
        o = next;
    }
    for (gc_slab *s = h->slabs; s != NULL;) {
        gc_slab *next = s->next;
        free(s);
        s = next;
    }
    free(h);
}

// --- system heap (back-compatible global API) -------------------------------

void lisp_gc_collect(void) {
    lisp_rt_lock();
    collect_heap_locked(&g_system);
    lisp_rt_unlock();
}

void lisp_gc_init(void *stack_base) {
    g_stack_base = stack_base;
    g_system.enabled = 1;
}

size_t lisp_gc_live_count(void) { return g_system.live; }
size_t lisp_gc_collections(void) { return g_system.collections; }
