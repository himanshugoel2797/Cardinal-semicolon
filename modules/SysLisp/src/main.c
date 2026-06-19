// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

// SysLisp -- the kernel-resident Scheme runtime, folded into the OS. K4 wrapped
// the host-proven runtime as a signed module that self-tested on boot; K5c made
// it the per-core scheduler loop (the boot thread CALLs lisp_scheduler_enter and
// never returns -- the interpreter IS the scheduler). K5d takes it MULTI-CORE:
// one Lisp scheduler loop per CPU.
//
// Each core runs its own scheduler over its own contexts (in their own precisely-
// collected per-context heaps). The only state shared between cores is the
// interned-symbol table and the system heap, guarded by a single runtime lock
// installed below; once the secondary cores are released the system heap is
// FROZEN (grow-only) because its conservative collector cannot see another core's
// stack roots (see notes/core/lisp-substrate.md, K5d). Cores are otherwise
// independent islands -- cross-core messaging is a later step.
//
// KNOWN FIRST-CUT LIMITATIONS (correctness is solid; these are performance, and
// are deferred -- the plan's "global lock first, revisit if contention shows"):
//   - The single runtime lock also guards the GC mark scratch, so per-context
//     collections SERIALISE across cores even though the heaps are disjoint.
//     Per-core scratch would let them run in parallel.
//   - The kernel allocator (modules/SysMemory) is O(n) best-fit with no free-list
//     coalescing (mem_compact is a TODO), so heavy GC churn from many cores
//     degrades super-linearly. A big proof loop (n>>1e4) at 4 cores can take many
//     seconds; the per-core proof below is deliberately kept modest.

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <cardinal/local_spinlock.h>

#include "lisp.h"

#include "SysTimer/timer.h"
#include "SysMP/mp.h"
#include "SysInterrupts/interrupts.h"
#include "SysVirtualMemory/vmem.h"
#include "SysPhysicalMemory/phys_mem.h"
#include "SysReg/registry.h"
#include "SysDebug/csmux.h"
#include "pci/pci.h"
#include "pci/pci_irq.h"
#include <stdlib.h>  // itoa

// Kernel services resolved at module-load time (this module is already verified).
// SysLisp no longer depends on the native task API (task_create/yield/monitor):
// it IS the per-core scheduler loop now, entered via CALL from the boot script.
int print_str(const char *s);
uint64_t timer_timestamp_ns(void);
bool Initrd_GetFile(const char *file, void **loc, size_t *size);  // kernel: initrd reader

// --- Runtime lock + shared environment ----------------------------------------

// The single lock guarding the runtime's cross-core shared state (system heap +
// intern table + GC scratch). A plain spinlock is sufficient: only the per-core
// scheduler loops (task context) take it, never the event ISR -- which does just
// an ISR-safe word write via lisp_ctx_wake -- so it can't be re-entered from an
// interrupt and needs no cli().
static int g_runtime_lock = 0;
static void lisp_lock(void) { local_spinlock_lock(&g_runtime_lock); }
static void lisp_unlock(void) { local_spinlock_unlock(&g_runtime_lock); }

// The one shared global environment, built once on the BSP and read (never
// mutated) by every core's contexts. Living in the frozen system heap, it is a
// permanent root; per-context heaps mark-stop at it.
static lisp_value g_env = LISP_EMPTY;

// --- Output sink: route (display)/(write)/(newline) to the debug log ----------

// lisp_output_fn hands us a byte run that may not be NUL-terminated and may
// exceed any fixed buffer, so copy in chunks and NUL-terminate for print_str.
// (print_str is a C string, so a Lisp string containing an embedded NUL would be
// truncated at it; acceptable for a debug sink -- such strings are not expected.)
static void lisp_out(const char *s, size_t len, void *ctx) {
    (void)ctx;
    char buf[129];
    while (len > 0) {
        size_t n = len < sizeof(buf) - 1 ? len : sizeof(buf) - 1;
        memcpy(buf, s, n);
        buf[n] = '\0';
        print_str(buf);
        s += n;
        len -= n;
    }
}

// --- Foreign-function examples: kernel services as Lisp primitives ------------

// (uptime-ns) -> nanoseconds since boot, as an exact integer. The fixnum range is
// 62-bit (~146 years of ns), so no truncation in practice; a no-counter timer
// returns the (uint64_t)-1 sentinel, surfaced here as an error.
static lisp_value prim_uptime_ns(lisp_value *a, int n, const char **e) {
    (void)a;
    if (n != 0)
        return (*e = "uptime-ns: expects no arguments"), LISP_UNDEF;
    uint64_t ns = timer_timestamp_ns();
    if (ns == (uint64_t)-1)  // TIMER_NO_COUNTER
        return (*e = "uptime-ns: no readable timer"), LISP_UNDEF;
    return lisp_fixnum((int64_t)ns);
}

// --- The native-ISR -> event -> wake-context bridge ---------------------------
//
// A minimal native ISR is NOT a Lisp evaluator: it runs in interrupt context
// where allocation/GC are illegal. It only POSTS an event -- here, bumps a
// counter and clears the waiting context's blocked flag (two word writes) -- and
// a Lisp context resumes in normal task context. This is the universal completion
// path the whole async-yield model rides on (the same shape serves DMA/disk/NIC
// completions, timers, and inter-context messages).

static volatile uint32_t g_event_count = 0;     // bumped by the ISR; the scheduler monitors it
// The single parked context to wake (0 = none). One slot is fine today: %event-wait
// is used only by run_isr_demo in the single-core phase. TODO(multi-core): when a
// migrated driver parks contexts from multiple cores, replace this with a per-core
// (or per-IRQ) waiter table so two waiters can't race the one slot.
static volatile lisp_value g_event_waiter = 0;

static void lisp_event_isr(int irq) {
    (void)irq;
    g_event_count++;
    lisp_value w = g_event_waiter;
    if (w != 0)
        lisp_ctx_wake(w);  // ISR-safe: a single word write
}

// (%event-count) -> the current hardware-event counter.
static lisp_value prim_event_count(lisp_value *a, int n, const char **e) {
    (void)a;
    (void)n;
    (void)e;
    return lisp_fixnum((int64_t)g_event_count);
}

// (%event-wait) -> register the running context as the event waiter and park it.
// It is woken by the next lisp_event_isr. The Lisp `wait-event` wrapper below
// loops over %event-count so no event is ever missed (count-based, not edge).
static lisp_value prim_event_wait(lisp_value *a, int n, const char **e) {
    (void)a;
    (void)n;
    lisp_value self = lisp_current_ctx();
    if (self == LISP_EMPTY)
        return (*e = "%event-wait: not under the scheduler"), LISP_UNDEF;
    g_event_waiter = self;
    lisp_ctx_block(self);
    return LISP_UNDEF;
}

// --- Generic ISA/IOAPIC IRQ -> wake-context bridge ----------------------------
//
// Generalises the single-slot event/MSI bridges above to a small table of ISA
// interrupt lines, so a Lisp driver can claim a hardware line, park on it, and be
// woken by that line's interrupt -- with ALL device logic (port I/O, decode) in
// Lisp. The native floor is exactly this: an interrupt-context trampoline (alloc
// and GC are illegal there) that bumps a counter and wakes the parked context.
// The ps2 keyboard driver (./lisp/ps2.clp) is the first user; any future ISA
// driver reuses it unchanged.
#define LISP_MAX_IRQ_LINES 8
static struct lisp_irq_line {
    volatile int vector;      // allocated IDT vector; 0 = free slot. volatile so the
                              // ISR's read and the "publish vector last" store in
                              // irq-register are ordered wrt the volatile count/waiter
                              // stores (not merely by x86 TSO) under the C memory model.
    volatile uint32_t count;  // bumped each interrupt (count-based wake, never lost)
    volatile lisp_value waiter;  // parked context to wake (0 = none)
} g_irq_lines[LISP_MAX_IRQ_LINES];

// The shared ISA-IRQ ISR. The dispatcher passes the vector (idt.c: h(int_no)), so
// one handler registered on every claimed vector dispatches by vector. A context
// in the (frozen, never-reaped) system heap can't dangle, so the wake stays valid.
static void lisp_irq_isr(int vec) {  // interrupt context
    for (int i = 0; i < LISP_MAX_IRQ_LINES; i++) {
        if (g_irq_lines[i].vector == vec) {
            g_irq_lines[i].count++;
            lisp_value w = g_irq_lines[i].waiter;
            if (w != 0)
                lisp_ctx_wake(w);  // ISR-safe: a single word write
            return;
        }
    }
}

// (irq-register gsi) -> an opaque small-fixnum handle, or #f. Allocates a vector,
// installs lisp_irq_isr on it, and routes the IOAPIC line `gsi` (edge-triggered,
// active-high -- the ISA convention) to it, destined for the calling core. Must be
// called on the core whose contexts will (irq-wait) on it (BSP today: services are
// BSP-pinned and interrupt_mapinterrupt targets the current core).
static lisp_value prim_irq_register(lisp_value *a, int n, const char **e) {
    if (n != 1 || !lisp_is_fixnum(a[0]) || lisp_fixnum_val(a[0]) < 0)
        return (*e = "irq-register: expects (gsi)"), LISP_UNDEF;
    uint32_t gsi = (uint32_t)lisp_fixnum_val(a[0]);
    int slot = -1;
    for (int i = 0; i < LISP_MAX_IRQ_LINES; i++)
        if (g_irq_lines[i].vector == 0) { slot = i; break; }
    if (slot < 0)
        return (*e = "irq-register: no free IRQ slots"), LISP_UNDEF;
    // vec=0 + no _fixed: interrupt_allocate scans for a free vector (>=32) and
    // writes it back. (_fixed instead means "give me exactly *base" -- passing 0
    // there would route the line to vector 0, a CPU-exception slot.)
    int vec = 0;
    if (interrupt_allocate(1, interrupt_flags_exclusive, &vec) != CS_OK)
        return (*e = "irq-register: vector allocation failed"), LISP_UNDEF;
    g_irq_lines[slot].count = 0;
    g_irq_lines[slot].waiter = 0;
    g_irq_lines[slot].vector = vec;  // publish last: ISR ignores the slot until set
    interrupt_register_handler(vec, lisp_irq_isr);
    // interrupt_mapinterrupt already clears the RTE mask, so no separate unmask.
    interrupt_mapinterrupt(gsi, vec, false, false);
    return lisp_fixnum(slot);
}

// (irq-count handle) -> this line's interrupt counter (advances on every IRQ).
static lisp_value prim_irq_count(lisp_value *a, int n, const char **e) {
    if (n != 1 || !lisp_is_fixnum(a[0]))
        return (*e = "irq-count: expects (handle)"), LISP_UNDEF;
    int slot = (int)lisp_fixnum_val(a[0]);
    if (slot < 0 || slot >= LISP_MAX_IRQ_LINES || g_irq_lines[slot].vector == 0)
        return (*e = "irq-count: bad handle"), LISP_UNDEF;
    return lisp_fixnum((int64_t)g_irq_lines[slot].count);
}

// (irq-wait handle seen) -> park the running context until the line's counter
// passes `seen`; returns immediately (#f) if it already has. cli() closes the
// check-then-park race against the same-core IRQ (mirrors net-wait). The line is
// routed to the calling/BSP core, so cli() here masks it; a line re-routed to
// another core could lose the wake in this window (the cross-core-messaging caveat).
static lisp_value prim_irq_wait(lisp_value *a, int n, const char **e) {
    if (n != 2 || !lisp_is_fixnum(a[0]) || !lisp_is_fixnum(a[1]))
        return (*e = "irq-wait: expects (handle seen)"), LISP_UNDEF;
    int slot = (int)lisp_fixnum_val(a[0]);
    if (slot < 0 || slot >= LISP_MAX_IRQ_LINES || g_irq_lines[slot].vector == 0)
        return (*e = "irq-wait: bad handle"), LISP_UNDEF;
    uint32_t seen = (uint32_t)lisp_fixnum_val(a[1]);
    lisp_value self = lisp_current_ctx();
    if (self == LISP_EMPTY)
        return (*e = "irq-wait: not under the scheduler"), LISP_UNDEF;
    g_irq_lines[slot].waiter = self;
    int cli_state = cli();
    if (g_irq_lines[slot].count != seen) {  // an interrupt already advanced it
        sti(cli_state);
        return LISP_FALSE;
    }
    lisp_ctx_block(self);  // blocked=1, budget=0 -> suspends at the next safe point
    sti(cli_state);
    return LISP_UNDEF;
}

// --- Driver substrate: MMIO / DMA / port I/O (D3) -----------------------------
//
// These mint the FOREIGN byte buffers (lisp_make_bytes_foreign) that the D2
// volatile accessors (bytes-uN-ref/set!) then drive. They are kernel-only (vmem /
// physmem / port instructions) so they live here, not in the portable lib. NB:
// they are unrestricted today (any context may map any physical address) -- the
// capability model that gates them is a later phase.

// (mmio-map phys size) -> a byte buffer over the device's MMIO at physical
// address `phys`, mapped uncached. Accessors bounds-check against `size`.
static lisp_value prim_mmio_map(lisp_value *a, int n, const char **e) {
    if (n != 2 || !lisp_is_fixnum(a[0]) || !lisp_is_fixnum(a[1]) || lisp_fixnum_val(a[1]) <= 0)
        return (*e = "mmio-map: expects (phys-addr size)"), LISP_UNDEF;
    intptr_t phys = (intptr_t)lisp_fixnum_val(a[0]);
    size_t size = (size_t)lisp_fixnum_val(a[1]);
    // vmem_phystovirt PANICs on an out-of-range physical address rather than
    // returning an error; an unrestricted caller mapping a bad address is a fatal
    // bug until the capability model gates these prims.
    intptr_t virt = vmem_phystovirt(phys, size, vmem_flags_uncached | vmem_flags_kernel | vmem_flags_rw);
    lisp_value b = lisp_make_bytes_foreign((void *)virt, size, (uint64_t)phys);
    if (b == LISP_UNDEF)
        return (*e = "mmio-map: out of memory"), LISP_UNDEF;
    return b;
}

// (dma-alloc size) -> a physically-contiguous, zeroed, uncached byte buffer for
// device DMA. (bytes-phys b) gives the physical address to program into the
// device; the accessors read/write the CPU-side view.
static lisp_value prim_dma_alloc(lisp_value *a, int n, const char **e) {
    if (n != 1 || !lisp_is_fixnum(a[0]) || lisp_fixnum_val(a[0]) <= 0)
        return (*e = "dma-alloc: expects a positive size"), LISP_UNDEF;
    size_t size = (size_t)lisp_fixnum_val(a[0]);
    uintptr_t phys = physmem_alloc(0, 0, physmem_alloc_flags_data | physmem_alloc_flags_zero, size);
    if (phys == PHYSMEM_NO_ALLOC)
        return (*e = "dma-alloc: out of physical memory"), LISP_UNDEF;
    intptr_t virt = vmem_phystovirt((intptr_t)phys, size, vmem_flags_uncached | vmem_flags_kernel | vmem_flags_rw);
    // physmem_alloc ignores its flags (incl. _zero), so the pages are NOT zeroed;
    // a DMA descriptor ring needs clean memory, so zero the mapping ourselves.
    memset((void *)virt, 0, size);
    lisp_value b = lisp_make_bytes_foreign((void *)virt, size, (uint64_t)phys);
    if (b == LISP_UNDEF) {  // give the physical pages back rather than leaking them
        physmem_free(phys, size);
        return (*e = "dma-alloc: out of memory"), LISP_UNDEF;
    }
    return b;
}

// (pci-find vendor-id device-id) -> the ECAM physical address of the first
// matching PCI function, or #f. The PCI bus is enumerated into the registry at
// boot (HW/PCI/COUNT + HW/PCI/<hex-idx>/{VENDOR_ID,DEVICE_ID,ECAM_ADDR,...}); a
// Lisp driver finds its device here, then mmio-maps the ECAM to read config space.
static uint64_t pci_find_ecam(uint32_t vid, uint32_t did) {
    uint64_t count = 0;
    if (registry_readkey_uint("HW/PCI", "COUNT", &count) != CS_OK)
        return 0;
    for (uint64_t i = 0; i < count; i++) {
        char key[64] = "HW/PCI/";
        char num[16];
        strncat(key, itoa((int)i, num, 16), sizeof(key) - 8);
        uint64_t v = 0, d = 0, ecam = 0;
        if (registry_readkey_uint(key, "VENDOR_ID", &v) != CS_OK) continue;
        if (registry_readkey_uint(key, "DEVICE_ID", &d) != CS_OK) continue;
        if (v == vid && d == did &&
            registry_readkey_uint(key, "ECAM_ADDR", &ecam) == CS_OK)
            return ecam;
    }
    return 0;
}

static lisp_value prim_pci_find(lisp_value *a, int n, const char **e) {
    if (n != 2 || !lisp_is_fixnum(a[0]) || !lisp_is_fixnum(a[1]))
        return (*e = "pci-find: expects (vendor-id device-id)"), LISP_UNDEF;
    uint64_t ecam = pci_find_ecam((uint32_t)lisp_fixnum_val(a[0]), (uint32_t)lisp_fixnum_val(a[1]));
    return ecam == 0 ? LISP_FALSE : lisp_fixnum((int64_t)ecam);
}

// --- the driver MSI(-X) -> ISR -> wake-context bridge (N3) ---------------------
//
// A device's MSI(-X) is wired through the SAME minimal-ISR -> wake-a-Lisp-context
// path the timer demo and ps2 use -- this is its first exercise by a real PCI
// device. The ISR (interrupt context: no alloc) bumps a counter and wakes the
// parked driver context; the context drains the device (used ring) in normal
// task context. The MSI targets CPU 0 (interrupt_msi_register_addr(0)), where the
// driver context runs, so net-wait's cli() closes the check-then-park window.
static volatile uint32_t g_net_count = 0;
static volatile lisp_value g_net_waiter = 0;

static void net_msi_isr(int vec) {
    (void)vec;
    g_net_count++;
    lisp_value w = g_net_waiter;
    if (w != 0)
        lisp_ctx_wake(w);
}

// (pci-setup-msi ecam-phys) -> the allocated interrupt vector, or #f. Sets up the
// device's MSI/MSI-X (whichever it offers) with the wake ISR; the caller (the
// virtio driver) then points the device's msix vectors at table entry 0.
static lisp_value prim_pci_setup_msi(lisp_value *a, int n, const char **e) {
    if (n != 1 || !lisp_is_fixnum(a[0]))
        return (*e = "pci-setup-msi: expects (ecam-phys)"), LISP_UNDEF;
    pci_config_t *dev = (pci_config_t *)vmem_phystovirt(
        (intptr_t)lisp_fixnum_val(a[0]), 0x1000,
        vmem_flags_uncached | vmem_flags_kernel | vmem_flags_rw);
    // _exclusive reserves the allocated vector so nothing else (e.g. an ISA line
    // claimed via irq-register, which also allocates from this space) can land on
    // the same vector and share the dispatch slot.
    int vec = pci_setup_msi_handler(dev, interrupt_flags_exclusive, net_msi_isr);
    return vec < 0 ? LISP_FALSE : lisp_fixnum(vec);
}

// (net-count) -> the device-interrupt counter (advances on every MSI).
static lisp_value prim_net_count(lisp_value *a, int n, const char **e) {
    (void)a;
    (void)n;
    (void)e;
    return lisp_fixnum((int64_t)g_net_count);
}

// (net-wait seen) -> park the running context until the next device MSI, unless
// the counter already advanced past `seen` (in which case stay runnable). cli()
// closes the check-then-park race against the same-core MSI. LOAD-BEARING: the
// MSI is routed to CPU 0 (interrupt_msi_register_addr(0)) and the driver context
// runs on the BSP, so cli() here masks it. If the MSI were ever re-routed to
// another core, the wake could be lost in this window (same caveat as irq-wait);
// that waits on cross-core messaging.
static lisp_value prim_net_wait(lisp_value *a, int n, const char **e) {
    if (n != 1 || !lisp_is_fixnum(a[0]))
        return (*e = "net-wait: expects (seen-count)"), LISP_UNDEF;
    uint32_t seen = (uint32_t)lisp_fixnum_val(a[0]);
    lisp_value self = lisp_current_ctx();
    if (self == LISP_EMPTY)
        return (*e = "net-wait: not under the scheduler"), LISP_UNDEF;
    g_net_waiter = self;
    int cli_state = cli();
    if (g_net_count != seen) {  // an interrupt already advanced the counter
        sti(cli_state);
        return LISP_FALSE;
    }
    lisp_ctx_block(self);
    sti(cli_state);
    return LISP_UNDEF;
}

// Legacy port I/O. (in-uN port) -> value; (out-uN port value) -> unspecified.
static lisp_value prim_in_u8(lisp_value *a, int n, const char **e) {
    if (n != 1 || !lisp_is_fixnum(a[0]))
        return (*e = "in-u8: expects (port)"), LISP_UNDEF;
    return lisp_fixnum(inb((uint16_t)lisp_fixnum_val(a[0])));
}
static lisp_value prim_in_u16(lisp_value *a, int n, const char **e) {
    if (n != 1 || !lisp_is_fixnum(a[0]))
        return (*e = "in-u16: expects (port)"), LISP_UNDEF;
    return lisp_fixnum(inw((uint16_t)lisp_fixnum_val(a[0])));
}
static lisp_value prim_in_u32(lisp_value *a, int n, const char **e) {
    if (n != 1 || !lisp_is_fixnum(a[0]))
        return (*e = "in-u32: expects (port)"), LISP_UNDEF;
    return lisp_fixnum((int64_t)inl((uint16_t)lisp_fixnum_val(a[0])));
}
static lisp_value prim_out_u8(lisp_value *a, int n, const char **e) {
    if (n != 2 || !lisp_is_fixnum(a[0]) || !lisp_is_fixnum(a[1]))
        return (*e = "out-u8: expects (port value)"), LISP_UNDEF;
    outb((uint16_t)lisp_fixnum_val(a[0]), (uint8_t)lisp_fixnum_val(a[1]));
    return LISP_UNDEF;
}
static lisp_value prim_out_u16(lisp_value *a, int n, const char **e) {
    if (n != 2 || !lisp_is_fixnum(a[0]) || !lisp_is_fixnum(a[1]))
        return (*e = "out-u16: expects (port value)"), LISP_UNDEF;
    outw((uint16_t)lisp_fixnum_val(a[0]), (uint16_t)lisp_fixnum_val(a[1]));
    return LISP_UNDEF;
}
static lisp_value prim_out_u32(lisp_value *a, int n, const char **e) {
    if (n != 2 || !lisp_is_fixnum(a[0]) || !lisp_is_fixnum(a[1]))
        return (*e = "out-u32: expects (port value)"), LISP_UNDEF;
    outl((uint16_t)lisp_fixnum_val(a[0]), (uint32_t)lisp_fixnum_val(a[1]));
    return LISP_UNDEF;
}

// The capability-bearing driver primitives, grouped into named modules instead
// of dumped into the global env. A Lisp driver gains an authority only by
// importing its module -- (import sys-mmio) to map device memory, (import sys-io)
// for legacy port I/O, and so on -- so a context that imports none of them simply
// cannot name mmio-map/out-u8/pci-find/irq-register. This is the first half of
// the lexical (W7-style) capability model: the primitives are no longer ambient.
// Gating WHO may import each module is the next step (see notes).
static const lisp_builtin_export sys_io_exports[] = {
    {"in-u8", prim_in_u8},   {"in-u16", prim_in_u16},   {"in-u32", prim_in_u32},
    {"out-u8", prim_out_u8}, {"out-u16", prim_out_u16}, {"out-u32", prim_out_u32},
};
static const lisp_builtin_export sys_mmio_exports[] = {
    {"mmio-map", prim_mmio_map}, {"dma-alloc", prim_dma_alloc},
};
static const lisp_builtin_export sys_pci_exports[] = {
    {"pci-find", prim_pci_find},   {"pci-setup-msi", prim_pci_setup_msi},
    {"net-count", prim_net_count}, {"net-wait", prim_net_wait},
};
static const lisp_builtin_export sys_irq_exports[] = {
    {"irq-register", prim_irq_register}, {"irq-count", prim_irq_count},
    {"irq-wait", prim_irq_wait},
};

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

static void register_driver_modules(lisp_value env) {
    static const struct {
        const char *name;
        const lisp_builtin_export *exports;
        size_t count;
    } mods[] = {
        {"sys-io", sys_io_exports, ARRAY_LEN(sys_io_exports)},
        {"sys-mmio", sys_mmio_exports, ARRAY_LEN(sys_mmio_exports)},
        {"sys-pci", sys_pci_exports, ARRAY_LEN(sys_pci_exports)},
        {"sys-irq", sys_irq_exports, ARRAY_LEN(sys_irq_exports)},
    };
    for (size_t i = 0; i < ARRAY_LEN(mods); i++)
        // Only OOM can fail this, and only at boot with a fresh heap -- but a
        // silently-absent capability module would surface later as a confusing
        // "unbound variable" inside a driver, so name the culprit here.
        if (lisp_register_builtin_module(env, mods[i].name, mods[i].exports,
                                         mods[i].count) != 0) {
            print_str("[SysLisp] FATAL: failed to register module ");
            print_str(mods[i].name);
            print_str("\r\n");
        }
}

// --- The interactive serial REPL (replaces the GDB stub) ----------------------
//
// The REPL rides CSMUX_CH_REPL (the channel the GDB tunnel used to). console-poll
// /-write move bytes over it; repl-eval runs the read-eval-print engine in a
// PERSISTENT environment (g_repl_env, a child of the shared env, rooted via a
// hidden %repl-env binding so the collector keeps it). Evaluation is directed at
// the system heap (lisp_repl_serve) so the REPL's definitions survive across
// lines without being reclaimed by the REPL context's own heap. These are a gated
// capability -- (import sys-console) yields an eval-anything shell over the wire,
// so only a context init grants it (the REPL context, run root for OS debugging)
// can name them; the REPL context's own grant governs what typed input may import.

static lisp_value g_repl_env = LISP_EMPTY;

// (console-poll) -> a string of the bytes currently waiting on the REPL channel,
// or #f if none. Non-blocking; the REPL loop yields and retries on #f.
static lisp_value prim_console_poll(lisp_value *a, int n, const char **e) {
    (void)a;
    if (n != 0)
        return (*e = "console-poll: expects no arguments"), LISP_UNDEF;
    char buf[256];
    int got = csmux_chan_read(CSMUX_CH_REPL, buf, sizeof buf);
    if (got <= 0)
        return LISP_FALSE;
    return lisp_make_string(buf, (size_t)got);
}

// (console-write str) -> emit the string's bytes on the REPL channel. csmux_send
// rejects a frame larger than CSMUX_MAX_PAYLOAD, so a long transcript is split
// into payload-sized frames rather than silently dropped.
static lisp_value prim_console_write(lisp_value *a, int n, const char **e) {
    if (n != 1 || !lisp_is_string(a[0]))
        return (*e = "console-write: expects (string)"), LISP_UNDEF;
    const char *p = lisp_string_data(a[0]);
    size_t len = lisp_string_len(a[0]);
    do {  // do/while so a zero-length write still emits one (empty) frame
        uint32_t chunk = len > CSMUX_MAX_PAYLOAD ? CSMUX_MAX_PAYLOAD : (uint32_t)len;
        csmux_send(CSMUX_CH_REPL, p, chunk);
        p += chunk;
        len -= chunk;
    } while (len > 0);
    return LISP_UNDEF;
}

// (repl-eval str) -> the transcript (values / located errors) from evaluating the
// input in the persistent REPL env.
static lisp_value prim_repl_eval(lisp_value *a, int n, const char **e) {
    if (n != 1 || !lisp_is_string(a[0]))
        return (*e = "repl-eval: expects (string)"), LISP_UNDEF;
    char out[2048];
    lisp_repl_serve(lisp_string_data(a[0]), lisp_string_len(a[0]), g_repl_env,
                    out, sizeof out);
    return lisp_make_string(out, strlen(out));
}

static const lisp_builtin_export sys_console_exports[] = {
    {"console-poll", prim_console_poll},
    {"console-write", prim_console_write},
    {"repl-eval", prim_repl_eval},
};

// Resolve a Lisp module NAME (as written in `import`/`define-module`) to its
// source bytes in the initrd, by mapping it to ./lisp/<name>.clp. Installed via
// lisp_set_module_loader so `(import foo)` pulls ./lisp/foo.clp. The initrd
// payload is persistent and the reader is length-bounded, so we hand back a
// pointer straight into it -- no copy, no NUL terminator needed.
static bool syslisp_module_loader(const char *name, const char **src, size_t *len,
                                  void *ctx) {
    (void)ctx;
    static const char pre[] = "./lisp/";
    static const char suf[] = ".clp";
    char path[160];
    // Module names are plain identifiers; a '/' would let a name escape ./lisp/
    // (e.g. "../evil"). The source tier is trusted, but the guard is free.
    if (strchr(name, '/') != NULL)
        return false;
    size_t nlen = strlen(name);
    if ((sizeof(pre) - 1) + nlen + (sizeof(suf) - 1) >= sizeof path)
        return false;  // path too long for the buffer
    size_t o = 0;
    memcpy(path + o, pre, sizeof(pre) - 1);
    o += sizeof(pre) - 1;
    memcpy(path + o, name, nlen);
    o += nlen;
    memcpy(path + o, suf, sizeof(suf) - 1);
    o += sizeof(suf) - 1;
    path[o] = '\0';

    void *loc = NULL;
    size_t sz = 0;
    if (!Initrd_GetFile(path, &loc, &sz))
        return false;
    *src = (const char *)loc;
    *len = sz;
    return true;
}

// --- Self-test ----------------------------------------------------------------

static int g_pass = 0;
static int g_fail = 0;

static void check(lisp_value env, const char *src, const char *want) {
    const char *err = NULL;
    lisp_value r = lisp_eval_string(src, env, &err);
    char buf[160];
    if (err != NULL) {
        print_str("[SysLisp] FAIL ");
        print_str(src);
        print_str("  -> error: ");
        print_str(err);
        print_str("\r\n");
        g_fail++;
        return;
    }
    lisp_print(r, buf, sizeof buf);
    if (strcmp(buf, want) == 0) {
        print_str("[SysLisp]  ok  ");
        print_str(src);
        print_str("  -> ");
        print_str(buf);
        print_str("\r\n");
        g_pass++;
    } else {
        print_str("[SysLisp] FAIL ");
        print_str(src);
        print_str("  -> ");
        print_str(buf);
        print_str(" (want ");
        print_str(want);
        print_str(")\r\n");
        g_fail++;
    }
}

static void check_scheduler(lisp_value env) {
    lisp_sched_t s;
    lisp_sched_init(&s, 100);
    s.per_context_heaps = 1;
    const char *err = NULL;
    lisp_eval_string(
        "(define consumer"
        "  (spawn (lambda () (let loop ((acc 0) (k 5))"
        "                      (if (= k 0) acc (loop (+ acc (recv)) (- k 1)))))))"
        "(define producer"
        "  (spawn (lambda () (let loop ((i 1))"
        "                      (if (> i 5) 'done (begin (send consumer i) (loop (+ i 1))))))))",
        env, &err);
    lisp_value consumer = lisp_eval_string("consumer", env, &err);
    lisp_sched_run(&s, 0);
    char buf[64];
    lisp_print(lisp_ctx_value(consumer), buf, sizeof buf);
    if (err == NULL && strcmp(buf, "15") == 0) {
        print_str("[SysLisp]  ok  scheduler producer/consumer  -> ");
        print_str(buf);
        print_str("\r\n");
        g_pass++;
    } else {
        print_str("[SysLisp] FAIL scheduler producer/consumer  -> ");
        print_str(buf);
        print_str("\r\n");
        g_fail++;
    }
}

// Capability gate (W7 step 2) under the real kernel scheduler, against the real
// sys-mmio module: a context GRANTED sys-mmio may import it and allocate DMA; a
// context granted something else is DENIED the import (its context errors). This
// is the knob the serial REPL will ride -- root when debugging the OS, sandboxed
// otherwise.
static void check_capabilities(lisp_value env) {
    lisp_sched_t s;
    lisp_sched_init(&s, 1000);
    s.per_context_heaps = 1;
    const char *err = NULL;
    lisp_eval_string(
        "(define cap-ok"
        "  (spawn-restricted '(sys-mmio)"
        "    (lambda () (import sys-mmio) (> (bytes-phys (dma-alloc 32)) 0))))"
        "(define cap-deny"
        "  (spawn-restricted '(sys-irq)"
        "    (lambda () (import sys-mmio) 'leaked)))",
        env, &err);
    lisp_value ok = lisp_eval_string("cap-ok", env, &err);
    lisp_value deny = lisp_eval_string("cap-deny", env, &err);
    lisp_sched_run(&s, 0);

    char buf[64];
    lisp_print(lisp_ctx_value(ok), buf, sizeof buf);
    bool ok_pass = err == NULL && lisp_ctx_state(ok) == LISP_CTX_DONE &&
                   strcmp(buf, "#t") == 0;
    bool deny_pass = lisp_ctx_state(deny) == LISP_CTX_ERROR;
    if (ok_pass && deny_pass) {
        print_str("[SysLisp]  ok  capability gate (granted imports, ungranted denied)\r\n");
        g_pass++;
    } else {
        print_str("[SysLisp] FAIL capability gate  granted-> ");
        print_str(buf);
        print_str(deny_pass ? "  ungranted=denied" : "  ungranted=LEAKED");
        print_str("\r\n");
        g_fail++;
    }
}

// The serial REPL's eval, under the real scheduler: a context (own per-context
// heap) imports sys-console, defines a value through repl-eval, then churns its
// OWN heap hard enough to force a collection, and reads the value back. It must
// survive -- proving the REPL's persistent env (and lisp_repl_serve's system-heap
// direction) keeps definitions across lines without the context's heap reclaiming
// them. This is the in-OS proof of the REPL engine + capability path; the live
// serial loop over CSMUX_CH_REPL is the interactive bring-up (needs an RX-wake
// bridge so it parks instead of busy-polling).
static void check_repl(lisp_value env) {
    lisp_sched_t s;
    lisp_sched_init(&s, 4000000);
    s.per_context_heaps = 1;
    const char *err = NULL;
    lisp_eval_string(
        "(define rc (spawn (lambda ()"
        "  (import sys-console)"
        "  (repl-eval \"(define rz 4242)\")"
        "  (let loop ((i 0) (acc '()))"
        "    (if (= i 9000) 'churned (loop (+ i 1) (cons i acc))))"
        "  (string=? (repl-eval \"rz\") \"4242\\n\"))))",
        env, &err);
    lisp_value rc = lisp_eval_string("rc", env, &err);
    lisp_sched_run(&s, 0);
    char buf[16];
    lisp_print(lisp_ctx_value(rc), buf, sizeof buf);
    if (err == NULL && lisp_ctx_state(rc) == LISP_CTX_DONE && strcmp(buf, "#t") == 0) {
        print_str("[SysLisp]  ok  serial REPL eval + persistent env survives GC\r\n");
        g_pass++;
    } else {
        print_str("[SysLisp] FAIL serial REPL eval -> ");
        print_str(buf);
        print_str("\r\n");
        g_fail++;
    }
}

static void run_self_test(lisp_value env) {
    check(env, "(+ 1 2 3)", "6");
    check(env, "(define (fact n) (if (= n 0) 1 (* n (fact (- n 1))))) (fact 6)", "720");
    check(env, "(map (lambda (x) (* x x)) '(1 2 3 4))", "(1 4 9 16)");
    check(env, "(let loop ((i 0) (n 1000)) (if (= n 0) i (loop (+ i 1) (- n 1))))", "1000");
    check(env, "(+ 1.5 2.25)", "3.75");
    check(env, "(/ 7 2)", "3.5");
    check(env, "(begin (display \"hello from lisp\") (newline) 'displayed)", "displayed");
    check(env, "(define keep (list 1 2 3 4 5))", "keep");
    lisp_gc_collect();
    lisp_gc_collect();
    check(env, "(apply + keep)", "15");
    check(env, "(> (uptime-ns) 0)", "#t");
    // Driver substrate: bitfields (ambient base language) + a real DMA buffer
    // round-trip reached THROUGH the capability path -- a module that (import
    // sys-mmio)s to obtain dma-alloc. This exercises the whole Q1 mechanism at
    // boot: built-in module -> import -> use a physically-addressed volatile
    // buffer. dma-alloc is no longer ambient, so a bare (dma-alloc ...) in this
    // env would be unbound by design.
    check(env, "(bit-insert (bit-insert 0 0 1 1) 4 3 5)", "81");
    check(env, "(begin"
               "  (define-module dma-probe (export run)"
               "    (import sys-mmio)"
               "    (define (run)"
               "      (let ((d (dma-alloc 64)))"
               "        (bytes-u32-set! d 0 305419896)"
               "        (and (= (bytes-u32-ref d 0) 305419896) (> (bytes-phys d) 0)))))"
               // Prefix the import so this test does not leave a bare `run` bound
               // in the shared env (a common name a later driver might want).
               "  (import (dma-probe (prefix dp:)))"
               "  (dp:run))",
          "#t");
    check_scheduler(env);
    check_capabilities(env);
    check_repl(env);
    // sys-debug through the capability path: a module imports the reflective
    // debugger and single-steps a sub-context to completion (Lisp debugging Lisp).
    check(env, "(begin"
               "  (define-module dbg-probe (export run)"
               "    (import sys-debug)"
               "    (define (run)"
               "      (let ((c (ctx-make (lambda () (* 7 6)))))"
               "        (let loop () (if (eq? (ctx-step c) 'done) (ctx-value c) (loop))))))"
               "  (import (dbg-probe (prefix d:)))"
               "  (d:run))",
          "42");

    char num[24];
    print_str("[SysLisp] ");
    lisp_print(lisp_fixnum(g_pass), num, sizeof num);
    print_str(num);
    print_str(" passed, ");
    lisp_print(lisp_fixnum(g_fail), num, sizeof num);
    print_str(num);
    print_str(" failed\r\n");
    print_str(g_fail == 0 ? "[SysLisp] ALL TESTS PASSED\r\n" : "[SysLisp] SELF-TEST FAILED\r\n");
}

// --- Persistent scheduler task ------------------------------------------------

// Drive Lisp contexts until the ISR-waiting `waiter` finishes. When no context is
// runnable (the waiter is parked on a hardware event), sleep until the next event
// via task_monitor on the event counter; the timer ISR bumps it and the native
// scheduler re-runs us. This is exactly the loop a full per-core Lisp scheduler
// runs; here it hosts one event-driven context + one CPU-bound context.
static void run_isr_demo(lisp_value env) {
    // Arm a one-shot HPET timer (~10ms) as a stand-in hardware event source; its
    // ISR wakes a parked Lisp context. (Periodic timers aren't free here -- the
    // local APIC timer is the OS scheduler's -- so this is a single event, which
    // is all that is needed to prove the bridge: a native ISR resuming a context.)
    int vec = timer_request(timer_features_oneshot, 10000000ull, lisp_event_isr);
    if (vec < 0) {
        print_str("[SysLisp] (no interrupt timer available; skipping ISR demo)\r\n");
        return;
    }

    lisp_sched_t s;
    lisp_sched_init(&s, 200);
    s.per_context_heaps = 1;
    const char *err = NULL;
    lisp_eval_string(
        // Parks until the event counter advances (count-based, so the wake is
        // never lost even if the ISR fires before the context parks):
        "(define waiter"
        "  (spawn (lambda () (let ((start (%event-count)))"
        "                      (let loop () (if (> (%event-count) start) 'woke-by-isr"
        "                                       (begin (%event-wait) (loop))))))))"
        // A concurrent CPU-bound context that runs to completion WHILE the waiter
        // is parked -- cooperative concurrency under the Lisp scheduler:
        "(define worker"
        "  (spawn (lambda () (let loop ((i 0) (k 20000)) (if (= k 0) i (loop (+ i 1) (- k 1)))))))",
        env, &err);
    lisp_value waiter = lisp_eval_string("waiter", env, &err);
    lisp_value worker = lisp_eval_string("worker", env, &err);

    // Bound the wait with the (counter-based, interrupt-free) timestamp so a timer
    // ISR that never gets delivered cannot hang the boot.
    uint64_t deadline = timer_timestamp_ns() + 2000000000ull;  // 2s
    for (;;) {
        uint32_t seen = g_event_count;  // capture BEFORE running, so no wakeup is lost
        lisp_sched_run(&s, 0);          // run all runnable contexts until parked/done
        if (lisp_ctx_state(waiter) == LISP_CTX_DONE) {
            char wb[64], kb[64];
            lisp_print(lisp_ctx_value(waiter), wb, sizeof wb);
            lisp_print(lisp_ctx_value(worker), kb, sizeof kb);
            print_str("[SysLisp]  ok  context woken by a timer ISR   -> ");
            print_str(wb);
            print_str("\r\n[SysLisp]  ok  concurrent worker completed    -> ");
            print_str(kb);
            print_str("\r\n[SysLisp] PERSISTENT SCHEDULER + ISR BRIDGE OK\r\n");
            g_event_waiter = 0;  // the waiter is gone; stop the ISR touching it
            return;
        }
        // The waiter is parked on the event: enable interrupts and wait for the
        // ISR to bump the counter (busy-poll, deadline-bounded so a non-delivered
        // interrupt can't wedge us). A real driver IRQ uses the same wake path.
        __asm__ volatile("sti");
        while (g_event_count == seen && timer_timestamp_ns() < deadline)
            __asm__ volatile("pause");
        if (g_event_count == seen) {  // deadline hit without a tick
            print_str("[SysLisp] (timer ISR not delivered in time; ending ISR demo)\r\n");
            g_event_waiter = 0;
            return;
        }
    }
}

// --- The per-core scheduler loop ----------------------------------------------

// Build "[SysLisp] core <id> online: ... proof -> <result>" in one buffer and
// emit it with a single print_str, so concurrent cores' lines never interleave
// (one print_str call is atomic on COM1) and we avoid nesting the serial lock
// inside the runtime lock. `append` truncates safely on overflow.
static void announce_core(int id, const char *err, lisp_value proof) {
    char line[160];
    char num[24];
    size_t p = 0;
#define APP(s)                                              \
    do {                                                    \
        const char *x_ = (s);                               \
        while (*x_ != '\0' && p + 1 < sizeof line)          \
            line[p++] = *x_++;                              \
    } while (0)
    APP("[SysLisp] core ");
    lisp_print(lisp_fixnum(id), num, sizeof num);
    APP(num);
    APP(" online: lisp scheduler running, proof -> ");
    if (err == NULL && proof != LISP_UNDEF) {
        lisp_print(lisp_ctx_value(proof), num, sizeof num);
        APP(num);
    } else {
        APP("ERROR");
    }
    APP("\r\n");
#undef APP
    line[p] = '\0';
    print_str(line);
}

// --- Servers-as-Lisp ----------------------------------------------------------
//
// The OS services are Lisp contexts, brought up by the privileged `init` module
// (./lisp/init.clp), not C. CoreInput is a long-lived context owning a device
// table: drivers `send` it (register <name>) / (event <payload>) instead of the
// old synchronous callback ABI, so the rx-handler-re-enters-tx self-deadlock
// cannot occur. init spawns those service contexts with explicit grants (today
// the empty grant -- they need no runtime import authority), and the kernel only
// invokes (system-init) on the BSP. See init.clp for the policy and rationale.

// THE PER-CORE SCHEDULER LOOP, run by EVERY core (the BSP at the tail of
// lisp_scheduler_enter, each AP once released). It NEVER returns. This core
// runs its OWN scheduler over its OWN contexts (each in its own precisely-
// collected heap); the shared system heap + intern table are guarded by the
// runtime lock. There is no native task switcher underneath -- one native thread
// per core runs Lisp contexts that context-switch among themselves at safe
// points. FP is safe with no native preemption to clobber SSE state.
static void NORETURN lisp_core_loop(void) {
    int id = interrupt_get_cpu_idx();

    // This core's scheduler. A stack local (its run queue is a GC root reachable
    // from this frame); per_context_heaps gives each spawned context its own heap.
    lisp_sched_t sched;
    lisp_sched_init(&sched, 256);
    sched.per_context_heaps = 1;

    // Long-lived OS services live on the BSP for now (cross-core messaging is a
    // later step, so a service + its drivers must share one core). The privileged
    // init module owns the policy: (system-init) brings up the input service and
    // the NIC, spawning each service context with its grant. It runs as root (a
    // direct eval, not under the scheduler -> no current context), and its spawns
    // enqueue onto THIS scheduler (lisp_sched_init made it current) to run below.
    if (id == 0) {
        const char *ierr = NULL;
        lisp_eval_string("(system-init)", g_env, &ierr);
        if (ierr != NULL) {
            print_str("[SysLisp] system-init error: ");
            print_str(ierr);
            print_str("\r\n");
        }
    }

    // Per-core proof of life: spawn a context that does a real (heap-allocating,
    // per-context-GC-exercising) computation in the SHARED environment, run it to
    // completion on THIS core's scheduler, and report the result with the core id.
    // Kept modest: it's a liveness check, not a benchmark -- per-context GC across
    // all cores currently serialises on the one runtime lock (see the file header).
    const char *err = NULL;
    lisp_value proof = lisp_eval_string(
        "(spawn (lambda () (let loop ((i 0) (n 2000)) (if (= n 0) i (loop (+ i 1) (- n 1))))))",
        g_env, &err);
    lisp_sched_run(&sched, 0);

    // Announce on a single line, built in a local buffer and emitted with ONE
    // print_str. print_str itself serialises a whole call on COM1, so distinct
    // cores' announcements don't interleave -- and crucially we do NOT hold the
    // runtime lock across print_str (which takes its own serial lock under cli());
    // nesting those two locks would impose a fragile g_runtime_lock -> serial-lock
    // ordering.
    announce_core(id, err, proof);

    // Resident scheduler loop: run any runnable contexts, then idle until an
    // interrupt (a migrated driver's IRQ wakes a parked context via the ISR
    // bridge). With drivers off there are no persistent contexts yet, so this
    // simply idles. Never returns.
    for (;;) {
        lisp_sched_run(&sched, 64);
        __asm__ volatile("sti; hlt");
    }
}

// Each application processor, parked in mp_signalready() since boot, is released
// straight into lisp_core_loop and joins as another Lisp scheduler core. It needs
// no extra per-core hardware bring-up: apscript already ran intr_mp_init (which
// allocates this core's GDT/TSS and the exception IST stacks via gdt_init) and
// fp_mp_init, and a ring-0-only Lisp core never uses TSS rsp0 (that is only the
// ring3->ring0 stack). No native run queue / idle task / preemption timer either
// -- that machinery is dormant under the interpreter-as-scheduler model. The
// system heap is already frozen and per-context heaps are precise, so an AP also
// needs no GC stack base.

// THE ENTRY POINT, called once via `CALL:lisp_scheduler_enter` at the end of the
// boot script, on the BSP boot thread -- it NEVER returns. It performs the one-
// time global runtime init (single-core, so the system collector is live for the
// self-test), then goes multi-core: freeze the system heap, release the APs into
// lisp_core_loop, and run this (BSP) core's own scheduler loop.
int lisp_scheduler_enter() {
    // Install the concurrency hooks FIRST, so all subsequent interning/allocation
    // is already lock-guarded (uncontended while single-core) and every core
    // resolves to its own per-core scheduler slot by APIC id.
    lisp_set_concurrency(lisp_lock, lisp_unlock, interrupt_get_cpu_idx);
    lisp_gc_init(__builtin_frame_address(0));
    lisp_set_output(lisp_out, NULL);

    print_str("\r\n[SysLisp] interpreter-as-scheduler, multi-core bring-up\r\n");

    g_env = lisp_default_env();
    if (g_env == LISP_UNDEF) {
        print_str("[SysLisp] FAIL: could not build the default environment\r\n");
        for (;;)
            __asm__ volatile("cli; hlt");
    }
    lisp_install_sched(g_env);
    lisp_env_define(g_env, lisp_make_symbol("uptime-ns", 9),
                    lisp_make_primitive(prim_uptime_ns, "uptime-ns"));
    lisp_env_define(g_env, lisp_make_symbol("%event-count", 12),
                    lisp_make_primitive(prim_event_count, "%event-count"));
    lisp_env_define(g_env, lisp_make_symbol("%event-wait", 11),
                    lisp_make_primitive(prim_event_wait, "%event-wait"));
    // Expose the capability-bearing primitives as importable modules (sys-io /
    // sys-mmio / sys-pci / sys-irq) rather than ambient globals.
    register_driver_modules(g_env);
    // The reflective debugger is a capability too: only a context granted
    // sys-debug can import ctx-make/ctx-step/... and drive another context.
    lisp_register_debug_module(g_env);
    // The interactive serial REPL's I/O + eval, gated as sys-console. Build its
    // persistent environment now (single-core) and root it via a hidden binding
    // so the collector keeps it across the freeze and across REPL lines.
    g_repl_env = lisp_make_env(g_env);
    lisp_env_define(g_env, lisp_make_symbol("%repl-env", 9), g_repl_env);
    lisp_register_builtin_module(g_env, "sys-console", sys_console_exports,
                                 ARRAY_LEN(sys_console_exports));
    // Resolve `import` against the initrd (./lisp/<name>.clp). Must be set before
    // loading any source that imports a library. Module loading shares the same
    // single-core boot window (the registry lives in the shared env, which the
    // system collector must see fully built before the APs go live).
    lisp_set_module_loader(syslisp_module_loader, NULL);
    // Load the privileged system initializer. (import init) loads ./lisp/init.clp
    // -- a define-module that itself (import ps2 virtio-net)s (pulling in each
    // driver and the sys-* capabilities they need) and exports `system-init`,
    // which is bound into the shared env here. ALL boot policy (what services come
    // up, and the grants their contexts get) lives in init.clp now, not in C; the
    // kernel just calls (system-init) on the BSP once the scheduler is live.
    // Single-core, before the APs go live (module loading mutates the shared env).
    const char *imperr = NULL;
    lisp_eval_string("(import init)", g_env, &imperr);
    if (imperr != NULL) {
        // init now holds ALL boot policy; without it the OS would boot headless
        // (no input, no NIC) and silently. Treat a failed load as fatal -- mirrors
        // the g_env-build failure above -- so the cause is unmistakable.
        print_str("[SysLisp] FATAL: could not load the init module: ");
        print_str(imperr);
        print_str("\r\n");
        for (;;)
            __asm__ volatile("cli; hlt");
    }

    // Single-core phase: self-test + the ISR-bridge demo run with the system
    // collector still active (the BSP is the only core building shared state).
    run_self_test(g_env);
    run_isr_demo(g_env);

    // Go multi-core: freeze the (now fully-built) shared system heap -- its
    // conservative collector can't see other cores' stacks -- then release the
    // APs. They were parked in mp_signalready() since boot; each now runs
    // lisp_core_loop and becomes another Lisp scheduler core. (lisp_core_loop is
    // NORETURN; the cast to the plain ap-entry pointer type drops only that
    // attribute.)
    lisp_gc_set_multicore(1);
    print_str("[SysLisp] system heap frozen; releasing APs as Lisp cores\r\n");
    mp_set_ap_entry((void (*)(void))lisp_core_loop);

    // The BSP becomes its own Lisp scheduler core. Never returns.
    lisp_core_loop();
    return 0;  // unreached
}

int module_init() {
    // The runtime is entered as the per-core scheduler loop by the boot script's
    // CALL:lisp_scheduler_enter (which never returns), not from here.
    print_str("[SysLisp] loaded (runtime enters via CALL:lisp_scheduler_enter)\r\n");
    return 0;
}
