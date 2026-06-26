// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

// Shared-memory grants: the substrate for the compositor's zero-copy surfaces and
// layer buffers (notes/servers/CoreCompositor.md). A grant is an unforgeable,
// revocable capability to map a specific physical region into another context.
//
// The minter (an owner: the compositor) holds a DMA/MMIO buffer and mints a grant
// over ITS physical region; the grant is sent BY IDENTITY (deep_copy returns it
// unchanged, like a context handle) to a single grantee, which maps exactly that
// region with the kernel-side map-grant. The grantee can map ONLY what it was
// handed -- it never names a physical address -- which is the whole point versus
// handing a client raw sys-mmio (that would map any RAM). grant-revoke flips the
// slot so future map-grant fails.
//
// This file is the PORTABLE half: the grant table + mint/revoke/resolve, with no
// vmem/physmem dependency, so it is exercised directly by the host test suite. The
// actual phys->virt mapping lives in the kernel (modules/SysLisp prim_map_grant),
// which calls lisp_grant_resolve here and maps the region it returns.
//
// Why a table indexed by (index, generation) rather than embedding phys/len in the
// grant object: revocation. The grantee holds its own reference to the grant
// object, so revoke cannot reach into its heap to null it out. The table is the
// single authority for "is this grant live"; revoke sets the slot dead, and a
// later mint that reuses the slot bumps its generation, so a stale handle to the
// reused slot fails the generation check rather than mapping the new occupant.

#include <stdint.h>
#include <string.h>

#include "internal.h"  // lisp_gc_alloc_shared, lisp_rt_lock/unlock
#include "lisp.h"

// A fixed table is sufficient: a grant is per surface/layer buffer (a few per
// client), not per frame, so even a busy desktop uses a few dozen. Exhaustion is
// a hard error at mint, never a silent overwrite.
#define LISP_GRANT_MAX 1024

typedef struct {
    uint64_t phys;
    size_t len;
    uint32_t perms;       // 0 = read-only, 1 = read-write
    uint32_t generation;  // bumped on each (re)use; a handle must match to resolve
    uint8_t live;         // 1 = active grant, 0 = free/revoked
} grant_slot;

static grant_slot g_grants[LISP_GRANT_MAX];

static lisp_grant *as_grant(lisp_value v) { return (lisp_grant *)lisp_obj(v); }

// Allocate the grant handle object in the SYSTEM heap (like a context): it is
// referenced across contexts once sent, and a per-context collector only frees
// objects in its own heap, so a system-heap grant a mailbox points at is never
// swept from under the holder. A GC leaf -- no Lisp children to trace.
static lisp_value make_grant_obj(uint32_t index, uint32_t generation) {
    lisp_grant *g = (lisp_grant *)lisp_gc_alloc_shared(sizeof(lisp_grant));
    if (g == NULL)
        return LISP_UNDEF;
    g->h.header = LISP_MK_HEADER(LISP_OBJ_GRANT, 0);
    g->index = index;
    g->generation = generation;
    return lisp_from_obj(g);
}

lisp_value lisp_grant_mint(uint64_t phys, size_t len, uint32_t perms) {
    // Reserve a free slot under the lock; record its index+generation; then drop
    // the lock before allocating the handle object (lisp_gc_alloc_shared takes the
    // runtime lock itself, so holding it here would deadlock). If the allocation
    // fails, free the slot back so an OOM does not leak table capacity.
    lisp_rt_lock();
    int idx = -1;
    for (int i = 0; i < LISP_GRANT_MAX; i++) {
        if (!g_grants[i].live) {
            idx = i;
            break;
        }
    }
    if (idx < 0) {
        lisp_rt_unlock();
        return LISP_UNDEF;  // table full
    }
    grant_slot *s = &g_grants[idx];
    s->phys = phys;
    s->len = len;
    s->perms = perms;
    // Generations start at 1 and increase on every reuse, so a zeroed handle
    // (index 0, generation 0) never matches a live slot -- a forged/blank grant
    // cannot resolve.
    s->generation++;
    if (s->generation == 0)
        s->generation = 1;
    s->live = 1;
    uint32_t gen = s->generation;
    lisp_rt_unlock();

    lisp_value g = make_grant_obj((uint32_t)idx, gen);
    if (g == LISP_UNDEF) {
        lisp_rt_lock();
        s->live = 0;  // hand the slot back
        lisp_rt_unlock();
        return LISP_UNDEF;
    }
    return g;
}

int lisp_grant_resolve(lisp_value g, uint64_t *phys, size_t *len, uint32_t *perms) {
    if (!lisp_is_grant(g))
        return -1;
    lisp_grant *h = as_grant(g);
    if (h->index >= LISP_GRANT_MAX)
        return -1;
    lisp_rt_lock();
    grant_slot *s = &g_grants[h->index];
    int rc = -1;
    if (s->live && s->generation == h->generation) {
        if (phys)
            *phys = s->phys;
        if (len)
            *len = s->len;
        if (perms)
            *perms = s->perms;
        rc = 0;
    }
    lisp_rt_unlock();
    return rc;
}

int lisp_grant_revoke(lisp_value g) {
    if (!lisp_is_grant(g))
        return -1;
    lisp_grant *h = as_grant(g);
    if (h->index >= LISP_GRANT_MAX)
        return -1;
    lisp_rt_lock();
    grant_slot *s = &g_grants[h->index];
    int rc = -1;
    if (s->live && s->generation == h->generation) {
        s->live = 0;  // future resolve fails; a later mint reuses + bumps generation
        rc = 0;
    }
    lisp_rt_unlock();
    return rc;
}
