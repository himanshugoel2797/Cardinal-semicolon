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
#include "boot_information.h"  // GetBootInfo()->Cmdline (the cardinal.repl flag)
#include "pci/pci.h"
#include "pci/pci_irq.h"
#include "pci/pci_alloc.h"  // pci_assign_bars (BARs for firmware-unconfigured devices)
#include <stdlib.h>  // itoa

// Kernel services resolved at module-load time (this module is already verified).
// SysLisp no longer depends on the native task API (task_create/yield/monitor):
// it IS the per-core scheduler loop now, entered via CALL from the boot script.
int print_str(const char *s);
uint64_t timer_timestamp_ns(void);
bool Initrd_GetFile(const char *file, void **loc, size_t *size);  // kernel: initrd reader
void serial_enable_rx_irq(void);  // SysDebug: arm COM1 RX (IRQ 4) for the REPL

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

// The timeout queue (defined with the timer tick, below). A wait primitive with
// a deadline arg registers a sleeper too, so the tick wakes it if the signal
// does not arrive in time -- the timer and the signal share one wake.
static int sleeper_claim(uint64_t deadline, lisp_value ctx);
static void sleeper_release(int slot);
static int sleeper_pending(int slot);  // 1 while armed; 0 once the tick fired it

// (irq-wait handle seen [timeout-ns]) -> park the running context until the
// line's counter passes `seen`; returns immediately (#f) if it already has. With
// a timeout, also wake after `timeout-ns` (the caller re-checks irq-count to tell
// signal from timeout). cli() closes the check-then-park race against the
// same-core IRQ (mirrors msi-wait). The line is routed to the calling/BSP core,
// so cli() here masks it; a line re-routed to another core could lose the wake in
// this window (the cross-core-messaging caveat).
static lisp_value prim_irq_wait(lisp_value *a, int n, const char **e) {
    if ((n != 2 && n != 3) || !lisp_is_fixnum(a[0]) || !lisp_is_fixnum(a[1]) ||
        (n == 3 && (!lisp_is_fixnum(a[2]) || lisp_fixnum_val(a[2]) < 0)))
        return (*e = "irq-wait: expects (handle seen [timeout-ns])"), LISP_UNDEF;
    int slot = (int)lisp_fixnum_val(a[0]);
    if (slot < 0 || slot >= LISP_MAX_IRQ_LINES || g_irq_lines[slot].vector == 0)
        return (*e = "irq-wait: bad handle"), LISP_UNDEF;
    uint32_t seen = (uint32_t)lisp_fixnum_val(a[1]);
    lisp_value self = lisp_current_ctx();
    if (self == LISP_EMPTY)
        return (*e = "irq-wait: not under the scheduler"), LISP_UNDEF;
    g_irq_lines[slot].waiter = self;
    int sl = -1;
    if (n == 3) {
        sl = sleeper_claim(timer_timestamp_ns() + (uint64_t)lisp_fixnum_val(a[2]), self);
        if (sl < 0)
            return (*e = "irq-wait: too many sleepers"), LISP_UNDEF;
    }
    int cli_state = cli();
    if (g_irq_lines[slot].count != seen) {  // an interrupt already advanced it
        sti(cli_state);
        sleeper_release(sl);
        return LISP_FALSE;
    }
    if (n == 3 && !sleeper_pending(sl)) {  // the timeout already elapsed
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
static lisp_value dma_alloc_impl(lisp_value *a, int n, const char **e,
                                 physmem_alloc_flags_t extra, const char *who) {
    if (n != 1 || !lisp_is_fixnum(a[0]) || lisp_fixnum_val(a[0]) <= 0)
        return (*e = "dma-alloc: expects a positive size"), LISP_UNDEF;
    size_t size = (size_t)lisp_fixnum_val(a[0]);
    uintptr_t phys =
        physmem_alloc(0, 0, physmem_alloc_flags_data | physmem_alloc_flags_zero | extra, size);
    if (phys == PHYSMEM_NO_ALLOC)
        return (*e = who), LISP_UNDEF;
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

static lisp_value prim_dma_alloc(lisp_value *a, int n, const char **e) {
    return dma_alloc_impl(a, n, e, 0, "dma-alloc: out of physical memory");
}

// (dma-alloc-32 size) -> a DMA buffer guaranteed to have a physical address below
// 4 GiB, for a device that DMAs 32-bit addresses (rtl8139/8169, the USB host
// controllers). Same as dma-alloc otherwise; (bytes-phys b) fits in 32 bits.
static lisp_value prim_dma_alloc_32(lisp_value *a, int n, const char **e) {
    return dma_alloc_impl(a, n, e, physmem_alloc_flags_32bit,
                          "dma-alloc-32: out of 32-bit physical memory");
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

// --- the driver MSI(-X) -> ISR -> wake-context bridge -------------------------
//
// Each MSI source gets its own slot in a table PARALLEL to g_irq_lines (the ISA
// lines), so several MSI devices coexist: pci-setup-msi claims a slot and returns
// a handle, and (msi-count h)/(msi-wait h seen) name exactly that device's
// interrupt -- the same count-based, never-lost wake the ISA path uses. The ISR
// (interrupt context: no alloc) dispatches by vector, bumps that slot's counter,
// and wakes its parked context. The MSI targets CPU 0 (interrupt_msi_register_addr
// (0)), where the driver contexts run, so msi-wait's cli() closes the
// check-then-park window. (A single global counter sufficed when virtio-net was
// the only MSI device; a second MSI driver -- ahci, rtl8169 -- needs its own.)
#define LISP_MAX_MSI_LINES 8
static struct lisp_msi_line {
    volatile int vector;         // allocated vector; 0 = free slot (publish last)
    volatile uint32_t count;     // bumped on each MSI (count-based wake, never lost)
    volatile lisp_value waiter;  // parked context to wake (0 = none)
} g_msi_lines[LISP_MAX_MSI_LINES];

static void msi_isr(int vec) {  // interrupt context
    for (int i = 0; i < LISP_MAX_MSI_LINES; i++) {
        if (g_msi_lines[i].vector == vec) {
            g_msi_lines[i].count++;
            lisp_value w = g_msi_lines[i].waiter;
            if (w != 0)
                lisp_ctx_wake(w);  // ISR-safe: a single word write
            return;
        }
    }
}

// (pci-setup-msi ecam-phys) -> an opaque handle (a small fixnum slot), or #f.
// Sets up the device's MSI/MSI-X (whichever it offers) with the shared wake ISR
// and claims a waiter slot; the caller then points the device's vectors at table
// entry 0 and waits on the handle. _exclusive reserves the allocated vector so
// nothing else (an ISA line, another MSI device) can land on it.
static lisp_value prim_pci_setup_msi(lisp_value *a, int n, const char **e) {
    if (n != 1 || !lisp_is_fixnum(a[0]))
        return (*e = "pci-setup-msi: expects (ecam-phys)"), LISP_UNDEF;
    int slot = -1;
    for (int i = 0; i < LISP_MAX_MSI_LINES; i++)
        if (g_msi_lines[i].vector == 0) {
            slot = i;
            break;
        }
    if (slot < 0)
        return (*e = "pci-setup-msi: no free MSI slots"), LISP_UNDEF;
    pci_config_t *dev = (pci_config_t *)vmem_phystovirt(
        (intptr_t)lisp_fixnum_val(a[0]), 0x1000,
        vmem_flags_uncached | vmem_flags_kernel | vmem_flags_rw);
    int vec = pci_setup_msi_handler(dev, interrupt_flags_exclusive, msi_isr);
    if (vec < 0)
        return LISP_FALSE;
    g_msi_lines[slot].count = 0;
    g_msi_lines[slot].waiter = 0;
    g_msi_lines[slot].vector = vec;  // publish last: ISR ignores the slot until set
    return lisp_fixnum(slot);
}

// (msi-count handle) -> this device's MSI counter (advances on every MSI).
static lisp_value prim_msi_count(lisp_value *a, int n, const char **e) {
    if (n != 1 || !lisp_is_fixnum(a[0]))
        return (*e = "msi-count: expects (handle)"), LISP_UNDEF;
    int slot = (int)lisp_fixnum_val(a[0]);
    if (slot < 0 || slot >= LISP_MAX_MSI_LINES || g_msi_lines[slot].vector == 0)
        return (*e = "msi-count: bad handle"), LISP_UNDEF;
    return lisp_fixnum((int64_t)g_msi_lines[slot].count);
}

// (msi-wait handle seen [timeout-ns]) -> park the running context until this
// device's MSI counter passes `seen`, unless it already has (#f). With a timeout,
// also wake after `timeout-ns` -- the caller re-checks msi-count to tell signal
// (count advanced) from timeout (it did not). cli() closes the check-then-park
// race against the same-core MSI (mirrors irq-wait).
static lisp_value prim_msi_wait(lisp_value *a, int n, const char **e) {
    if ((n != 2 && n != 3) || !lisp_is_fixnum(a[0]) || !lisp_is_fixnum(a[1]) ||
        (n == 3 && (!lisp_is_fixnum(a[2]) || lisp_fixnum_val(a[2]) < 0)))
        return (*e = "msi-wait: expects (handle seen [timeout-ns])"), LISP_UNDEF;
    int slot = (int)lisp_fixnum_val(a[0]);
    if (slot < 0 || slot >= LISP_MAX_MSI_LINES || g_msi_lines[slot].vector == 0)
        return (*e = "msi-wait: bad handle"), LISP_UNDEF;
    uint32_t seen = (uint32_t)lisp_fixnum_val(a[1]);
    lisp_value self = lisp_current_ctx();
    if (self == LISP_EMPTY)
        return (*e = "msi-wait: not under the scheduler"), LISP_UNDEF;
    g_msi_lines[slot].waiter = self;
    int sl = -1;
    if (n == 3) {
        sl = sleeper_claim(timer_timestamp_ns() + (uint64_t)lisp_fixnum_val(a[2]), self);
        if (sl < 0)
            return (*e = "msi-wait: too many sleepers"), LISP_UNDEF;
    }
    int cli_state = cli();
    if (g_msi_lines[slot].count != seen) {  // an interrupt already advanced it
        sti(cli_state);
        sleeper_release(sl);
        return LISP_FALSE;
    }
    if (n == 3 && !sleeper_pending(sl)) {  // the timeout already elapsed
        sti(cli_state);
        return LISP_FALSE;
    }
    lisp_ctx_block(self);
    sti(cli_state);
    return LISP_UNDEF;
}

// (pci-assign-bars ecam-phys) -> the device's first assigned BAR base, or #f.
// For a device firmware never configured (e.g. an onboard NIC behind a closed
// PCIe root port): places its BARs above the existing windows and opens every
// bridge window up to the root bus. SIDE-EFFECTFUL (mutates bridge config), not a
// query -- call once, before mapping the BAR.
static lisp_value prim_pci_assign_bars(lisp_value *a, int n, const char **e) {
    if (n != 1 || !lisp_is_fixnum(a[0]))
        return (*e = "pci-assign-bars: expects (ecam-phys)"), LISP_UNDEF;
    uint64_t ecam = (uint64_t)lisp_fixnum_val(a[0]);
    pci_config_t *dev = (pci_config_t *)vmem_phystovirt(
        (intptr_t)ecam, 0x1000,
        vmem_flags_uncached | vmem_flags_kernel | vmem_flags_rw);
    uint64_t base = pci_assign_bars(dev, ecam);
    return base == 0 ? LISP_FALSE : lisp_fixnum((int64_t)base);
}

// --- timed wakeups: a software timeout queue over one periodic tick -----------
//
// A context yields for a duration the same way it waits for a device: it parks
// (lisp_ctx_block) and is woken (lisp_ctx_wake -- one word, ISR-safe) by an
// interrupt. The wake source for a timeout is a periodic local-APIC tick (the
// exact timer the old preemptive scheduler used, idle under K5) that scans this
// small table and wakes every context whose deadline has passed. `deadline == 0`
// marks a free slot (timer_timestamp_ns is always > 0 a few ns into boot). This
// lets a timeout COMPOSE with a device signal: a context registers a sleeper AND
// a signal waiter, parks, and whichever ISR fires first wakes it -- the basis for
// "wait for the IRQ, but give up after N ms".
#define LISP_MAX_SLEEPERS 32
static struct lisp_sleeper {
    volatile uint64_t deadline;  // ns (timer_timestamp_ns clock); 0 = free
    volatile lisp_value ctx;     // context to wake at the deadline
} g_sleepers[LISP_MAX_SLEEPERS];

// The periodic tick (interrupt context: no alloc). Wakes every past-deadline
// sleeper and frees its slot. O(table) per tick; the table is tiny.
static void lisp_timer_tick(int irq) {
    (void)irq;
    uint64_t now = timer_timestamp_ns();
    for (int i = 0; i < LISP_MAX_SLEEPERS; i++) {
        uint64_t d = g_sleepers[i].deadline;
        if (d != 0 && now >= d) {
            lisp_value c = g_sleepers[i].ctx;
            g_sleepers[i].deadline = 0;  // free + never double-wake
            if (c != 0)
                lisp_ctx_wake(c);
        }
    }
}

// Claim a sleeper slot for ctx at `deadline`. cli-guarded against the same-core
// tick. Returns the slot, or -1 if the table is full.
static int sleeper_claim(uint64_t deadline, lisp_value ctx) {
    int cli_state = cli();
    int slot = -1;
    for (int i = 0; i < LISP_MAX_SLEEPERS; i++)
        if (g_sleepers[i].deadline == 0) {
            slot = i;
            break;
        }
    if (slot >= 0) {
        g_sleepers[slot].ctx = ctx;
        g_sleepers[slot].deadline = deadline;  // publish last
    }
    sti(cli_state);
    return slot;
}

static void sleeper_release(int slot) {
    if (slot >= 0 && slot < LISP_MAX_SLEEPERS)
        g_sleepers[slot].deadline = 0;
}

// 1 while the slot is still armed; 0 once the tick has fired (and freed) it. A
// timeout-capable wait checks this under cli() to avoid parking after its
// deadline already elapsed (a lost-timeout race, the mirror of the count check).
static int sleeper_pending(int slot) {
    return slot >= 0 && slot < LISP_MAX_SLEEPERS && g_sleepers[slot].deadline != 0;
}

// Arm the periodic tick on the CURRENT core (local timer -> per-core). The same
// 50us tick the native preemptive scheduler ran; harmless and idle otherwise.
static void lisp_arm_timer_tick(void) {
    timer_request(timer_features_periodic | timer_features_local, 50000, lisp_timer_tick);
}

// (sleep ns) -> deschedule the running context for about `ns` nanoseconds, then
// resume. Yields the core (it runs other contexts / idles) -- not a busy-wait.
// Resolution is the tick (~50us), so sub-tick sleeps round up to one tick.
static lisp_value prim_sleep(lisp_value *a, int n, const char **e) {
    if (n != 1 || !lisp_is_fixnum(a[0]) || lisp_fixnum_val(a[0]) < 0)
        return (*e = "sleep: expects (nanoseconds)"), LISP_UNDEF;
    uint64_t deadline = timer_timestamp_ns() + (uint64_t)lisp_fixnum_val(a[0]);
    lisp_value self = lisp_current_ctx();
    if (self == LISP_EMPTY) {
        // No scheduler context to yield TO (a boot-time direct eval -- driver
        // bring-up before the per-core loop, or a self-test): nothing else is
        // runnable on this core, so there is nothing to yield to. Wait the
        // duration out on the counter. A driver that runs its bring-up inside a
        // spawned context instead gets the real yield below.
        while (timer_timestamp_ns() < deadline)
            __asm__ volatile("pause");
        return LISP_UNDEF;
    }
    int slot = sleeper_claim(deadline, self);
    if (slot < 0)
        return (*e = "sleep: too many sleepers"), LISP_UNDEF;
    int cli_state = cli();
    // If the tick already fired and freed our slot (a sub-tick deadline), don't
    // park -- the deadline has passed. Otherwise block; a later tick wakes us.
    if (g_sleepers[slot].deadline != 0)
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

// --- sys-cmdline: read the kernel command line (boot policy in Lisp) -----------
//
// The kernel command line (GetBootInfo()->Cmdline) carries boot-time policy flags
// (cardinal.repl, cardinal.test, cardinal.netdbg, ...). Exposing it as a
// capability lets the privileged `init` module gate features in Lisp rather than
// hardcoding strstr() calls in C -- e.g. a Lisp network-debug server brings itself
// up only under cardinal.netdbg. Read-only: a context can observe the cmdline but
// never change it.

// Copy a Lisp string argument into a NUL-terminated C buffer; returns -1 (and
// sets *e) if it is not a string or does not fit. strstr needs a terminated
// needle, and Lisp strings are length-counted, not NUL-terminated.
static int cmdline_arg(lisp_value v, char *buf, size_t cap, const char **e) {
    if (!lisp_is_string(v))
        return (*e = "expects a string"), -1;
    size_t ln = lisp_string_len(v);
    if (ln >= cap)
        return (*e = "string too long"), -1;
    memcpy(buf, lisp_string_data(v), ln);
    buf[ln] = '\0';
    return (int)ln;
}

// (cmdline-has? "substr") -> #t if substr occurs in the kernel command line.
static lisp_value prim_cmdline_has(lisp_value *a, int n, const char **e) {
    char needle[128];
    if (n != 1 || cmdline_arg(a[0], needle, sizeof needle, e) < 0)
        return (*e = *e ? *e : "cmdline-has?: expects (string)"), LISP_UNDEF;
    CardinalBootInfo *bi = GetBootInfo();
    if (bi == NULL)
        return LISP_FALSE;
    return strstr(bi->Cmdline, needle) != NULL ? LISP_TRUE : LISP_FALSE;
}

// (cmdline-get "key=") -> the token following the first occurrence of "key=", up
// to the next whitespace or end, as a string; #f if the key is absent. Lets a
// Lisp server read a parameter (e.g. (cmdline-get "cardinal.ip=") -> "10.0.2.15").
static lisp_value prim_cmdline_get(lisp_value *a, int n, const char **e) {
    char key[128];
    int kl = (n == 1) ? cmdline_arg(a[0], key, sizeof key, e) : -1;
    if (kl < 0)
        return (*e = *e ? *e : "cmdline-get: expects (string)"), LISP_UNDEF;
    CardinalBootInfo *bi = GetBootInfo();
    if (bi == NULL)
        return LISP_FALSE;
    const char *p = strstr(bi->Cmdline, key);
    if (p == NULL)
        return LISP_FALSE;
    p += kl;
    const char *q = p;
    while (*q != '\0' && *q != ' ' && *q != '\t')
        q++;
    return lisp_make_string(p, (size_t)(q - p));
}

// --- sys-reg: read the hardware/config registry --------------------------------
//
// The kernel registry (SysReg) is a hierarchical key/value store the bus/boot
// enumeration populates -- e.g. HW/BOOTINFO/FRAMEBUFFER/{ADDR,WIDTH,...} or
// HW/PCI/<i>/<field>. pci-find already walks it internally; this exposes a direct
// unsigned read so a Lisp driver (lfb) can find its non-PCI device. Read-only.

// (reg-read-uint "path" "key") -> the unsigned value at path/key, or #f if absent.
static lisp_value prim_reg_read_uint(lisp_value *a, int n, const char **e) {
    char path[160], key[64];
    if (n != 2 || cmdline_arg(a[0], path, sizeof path, e) < 0 ||
        cmdline_arg(a[1], key, sizeof key, e) < 0)
        return (*e = *e ? *e : "reg-read-uint: expects (path key) strings"), LISP_UNDEF;
    uint64_t val = 0;
    if (registry_readkey_uint(path, key, &val) != CS_OK)
        return LISP_FALSE;
    return lisp_fixnum((int64_t)val);
}

// --- sys-initrd: read a file from the boot initrd ------------------------------
//
// The initrd (a tar in the boot image) carries the Lisp source and could carry
// device firmware blobs. This hands a Lisp driver a file's bytes (copied into an
// owned buffer, so the driver may keep it past the initrd mapping). Read-only.

// (initrd-file "name") -> a bytes buffer with the file's contents, or #f if not
// present. The name is the tar path, e.g. "./iwifi_fw/foo.ucode".
static lisp_value prim_initrd_file(lisp_value *a, int n, const char **e) {
    char name[192];
    if (n != 1 || cmdline_arg(a[0], name, sizeof name, e) < 0)
        return (*e = *e ? *e : "initrd-file: expects (name) string"), LISP_UNDEF;
    void *loc = NULL;
    size_t sz = 0;
    if (!Initrd_GetFile(name, &loc, &sz))
        return LISP_FALSE;
    lisp_value b = lisp_make_bytes(sz);
    if (b == LISP_UNDEF)
        return (*e = "initrd-file: out of memory"), LISP_UNDEF;
    memcpy(lisp_bytes_data(b), loc, sz);
    return b;
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
    {"dma-alloc-32", prim_dma_alloc_32},
};
static const lisp_builtin_export sys_pci_exports[] = {
    {"pci-find", prim_pci_find},   {"pci-setup-msi", prim_pci_setup_msi},
    {"msi-count", prim_msi_count}, {"msi-wait", prim_msi_wait},
    {"pci-assign-bars", prim_pci_assign_bars},
};
static const lisp_builtin_export sys_irq_exports[] = {
    {"irq-register", prim_irq_register}, {"irq-count", prim_irq_count},
    {"irq-wait", prim_irq_wait},
};
static const lisp_builtin_export sys_cmdline_exports[] = {
    {"cmdline-has?", prim_cmdline_has}, {"cmdline-get", prim_cmdline_get},
};
static const lisp_builtin_export sys_reg_exports[] = {
    {"reg-read-uint", prim_reg_read_uint},
};
static const lisp_builtin_export sys_initrd_exports[] = {
    {"initrd-file", prim_initrd_file},
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
        {"sys-cmdline", sys_cmdline_exports, ARRAY_LEN(sys_cmdline_exports)},
        {"sys-reg", sys_reg_exports, ARRAY_LEN(sys_reg_exports)},
        {"sys-initrd", sys_initrd_exports, ARRAY_LEN(sys_initrd_exports)},
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

// True iff "cardinal.repl" is in the kernel command line. When set, the BSP brings
// up the interactive serial REPL: activate CSMUX (so the log and the REPL get
// their own framed channels over the one link) and spawn the REPL context. Off by
// default so a normal boot -- and the CI smoke test, which reads raw COM1 -- is
// unchanged (no framing, no REPL).
static int g_repl_enabled = 0;

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

// (console-arm-rx) -> enable COM1's receive interrupt (IRQ 4). Call AFTER claiming
// the line with (irq-register 4), so the REPL parks on irq-wait instead of busy-
// polling: an arriving byte raises IRQ 4, the generic ISA-IRQ bridge wakes the
// parked REPL context, and it drains the input.
static lisp_value prim_console_arm_rx(lisp_value *a, int n, const char **e) {
    (void)a;
    if (n != 0)
        return (*e = "console-arm-rx: expects no arguments"), LISP_UNDEF;
    serial_enable_rx_irq();
    return LISP_UNDEF;
}

// (console-flush) -> push out the coalesced debug-log buffer now. The log
// (display/print_str -> CH_LOG) is batched for throughput; an interactive REPL
// must flush it so output appears promptly and in order with the REPL transcript
// (CH_REPL, which csmux_send delivers immediately).
static lisp_value prim_console_flush(lisp_value *a, int n, const char **e) {
    (void)a;
    if (n != 0)
        return (*e = "console-flush: expects no arguments"), LISP_UNDEF;
    csmux_log_flush();
    return LISP_UNDEF;
}

static const lisp_builtin_export sys_console_exports[] = {
    {"console-poll", prim_console_poll},
    {"console-write", prim_console_write},
    {"console-arm-rx", prim_console_arm_rx},
    {"console-flush", prim_console_flush},
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
    static const char suf[] = ".clp";
    // A module's source may live directly under ./lisp/ (init) or in one of the
    // organizational subdirectories (libraries, the Core* servers, the device
    // drivers). The name stays a plain identifier -- the folder is NOT part of it
    // -- so imports and (define-module ...) are unchanged by the layout; the
    // loader searches this fixed, loader-controlled path list and the first hit
    // wins. Module names cannot contain '/' (a name could otherwise escape the
    // search dirs, e.g. "../evil"); the subdirs here are the only allowed dirs.
    static const char *const dirs[] = {
        "./lisp/", "./lisp/lib/", "./lisp/servers/", "./lisp/drivers/",
    };
    // An internal '/' is allowed so `include` can request "<module>/<part>" (a
    // module's parts live in a same-named subdir). But a leading '/' or any ".."
    // could escape the search dirs, so reject those -- the source tier is trusted,
    // but the guard is free.
    if (name[0] == '/' || strstr(name, "..") != NULL)
        return false;
    size_t nlen = strlen(name);
    for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
        char path[192];
        size_t dlen = strlen(dirs[i]);
        if (dlen + nlen + (sizeof(suf) - 1) >= sizeof path)
            continue;  // this prefix would overflow; try the next
        size_t o = 0;
        memcpy(path + o, dirs[i], dlen);
        o += dlen;
        memcpy(path + o, name, nlen);
        o += nlen;
        memcpy(path + o, suf, sizeof(suf) - 1);
        o += sizeof(suf) - 1;
        path[o] = '\0';

        void *loc = NULL;
        size_t sz = 0;
        if (Initrd_GetFile(path, &loc, &sz)) {
            *src = (const char *)loc;
            *len = sz;
            return true;
        }
    }
    return false;
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

// CorePower-in-Lisp under the real scheduler: register a fake audio device with
// the power service, fire a class-matching global power event, and confirm the
// device received it. The device context's return value becomes the p-state it
// was told to enter, so reading it back proves the (register -> event -> matched
// fan-out -> per-device send) path -- the message-passing replacement for the C
// server's synchronous event_g callback. (Class filtering itself is covered by
// the Lisp dispatch; here a matching class must deliver.)
static void check_power(lisp_value env) {
    lisp_sched_t s;
    lisp_sched_init(&s, 100000);
    s.per_context_heaps = 1;
    const char *err = NULL;
    lisp_eval_string(
        "(import corepower)"
        // A fake power device: parks until the service fans an event out to it,
        // then returns the p-state from that (pwr-g gstate pstate) message.
        "(define pdev (spawn (lambda () (let ((m (recv))) (caddr m)))))"
        "(define psrv (start-power-service))"
        "(send psrv (list 'register 'fake pwr-audio-out pdev))"
        "(send psrv (list 'event-g pwr-audio-out 1 7))",
        env, &err);
    lisp_value pdev = lisp_eval_string("pdev", env, &err);
    lisp_sched_run(&s, 0);
    char buf[64];
    lisp_print(lisp_ctx_value(pdev), buf, sizeof buf);
    if (err == NULL && lisp_ctx_state(pdev) == LISP_CTX_DONE && strcmp(buf, "7") == 0) {
        print_str("[SysLisp]  ok  power-event fan-out (class-matched delivery)  -> ");
        print_str(buf);
        print_str("\r\n");
        g_pass++;
    } else {
        print_str("[SysLisp] FAIL power-event fan-out  -> ");
        print_str(buf);
        print_str("\r\n");
        g_fail++;
    }
}

// CoreStorage-in-Lisp under the real scheduler. Exercises the two patterns the
// port introduces: (1) a block-read REQUEST/RESPONSE round-trip -- a reader sends
// storage a read with its own (self) as the reply target, storage bounds-checks
// and forwards to the fake driver, the driver answers the reader directly with a
// bytes buffer -- proving context handles survive being relayed through two
// hops of send; and (2) the fs-PROBE offer -- a provider registered after a
// device is offered that device. The reader's value becomes the byte the driver
// returned (= the requested lba); the provider's value becomes the device name it
// was probed with.
static void check_storage(lisp_value env) {
    lisp_sched_t s;
    lisp_sched_init(&s, 1000000);
    s.per_context_heaps = 1;
    const char *err = NULL;
    lisp_eval_string(
        "(import corestorage)"
        // Fake block driver: answers a read with a 1-byte buffer holding the lba.
        "(define disk (spawn (lambda () (let loop () (let ((m (recv)))"
        "   (if (eq? (car m) 'read)"
        "       (let ((b (make-bytes 1))) (bytes-u8-set! b 0 (cadr m))"
        "         (send (cadddr m) (list 'complete 0 b))))"
        "   (loop))))))"
        "(define stor (start-storage-service))"
        "(send stor (list 'register-blockdev 'disk0 512 100 disk))"
        // Reader: ask storage to read lba 5, return the byte that comes back.
        "(define rdr (spawn (lambda () (send stor (list 'read 'disk0 5 1 (self)))"
        "   (let ((m (recv))) (bytes-u8-ref (caddr m) 0)))))"
        // Provider registered after disk0 -> storage offers disk0 to it.
        "(define prov (spawn (lambda () (let ((m (recv))) (cadr m)))))"
        "(send stor (list 'register-fsprovider 'fakefs prov))",
        env, &err);
    lisp_value rdr = lisp_eval_string("rdr", env, &err);
    lisp_value prov = lisp_eval_string("prov", env, &err);
    lisp_sched_run(&s, 0);
    char rb[32], pb[32];
    lisp_print(lisp_ctx_value(rdr), rb, sizeof rb);
    lisp_print(lisp_ctx_value(prov), pb, sizeof pb);
    if (err == NULL && lisp_ctx_state(rdr) == LISP_CTX_DONE &&
        strcmp(rb, "5") == 0 && strcmp(pb, "disk0") == 0) {
        print_str("[SysLisp]  ok  storage block-read round-trip + fs-probe offer\r\n");
        g_pass++;
    } else {
        print_str("[SysLisp] FAIL storage  read-> ");
        print_str(rb);
        print_str("  probe-> ");
        print_str(pb);
        print_str("\r\n");
        g_fail++;
    }
}

// CoreNetwork-in-Lisp under the real scheduler, driven entirely by Lisp contexts
// (no NIC). Two scenarios prove the stack end to end:
//   (1) ARP: a peer ARPs for our IP; a fake NIC captures the frame the service
//       emits and we assert it is an ARP REPLY (oper 2) advertising our IP.
//   (2) UDP round-trip: netC builds + sends a UDP datagram; a "wire" context
//       relays the emitted frame back in as RX to netB, whose bound port-9999
//       handler receives the payload. This exercises ipv4/udp BUILD and the RX
//       parse + IP-header and UDP (pseudo-header) checksum VERIFY in one shot --
//       the handler only fires if every checksum the build path wrote validates.
static void check_network(lisp_value env) {
    lisp_sched_t s;
    lisp_sched_init(&s, 2000000);
    s.per_context_heaps = 1;
    const char *err = NULL;
    lisp_eval_string(
        "(import corenetwork driver-util)"
        // (1) ARP request -> reply, captured by a fake NIC.
        "(define arp-info (spawn (lambda () (let ((m (recv))) (let ((f (cadr m)))"
        "  (list (get-be16 f 20) (bytes-u8-ref f 28) (bytes-u8-ref f 29)"
        "        (bytes-u8-ref f 30) (bytes-u8-ref f 31)))))))"
        "(define netA (start-network-service (list 10 0 2 15)))"
        "(send netA (list 'register-nic (list 2 0 0 0 0 1) arp-info))"
        "(define areq (make-bytes 42))"
        "(put-list! areq 0 (list 255 255 255 255 255 255))"
        "(put-list! areq 6 (list 2 0 0 0 0 2))"
        "(put-be16! areq 12 2054) (put-be16! areq 14 1) (put-be16! areq 16 2048)"
        "(bytes-u8-set! areq 18 6) (bytes-u8-set! areq 19 4) (put-be16! areq 20 1)"
        "(put-list! areq 22 (list 2 0 0 0 0 2)) (put-list! areq 28 (list 10 0 2 2))"
        "(put-list! areq 38 (list 10 0 2 15))"
        "(send netA (list 'rx areq 42))"
        // (2) UDP round-trip netC -> wire -> netB:9999.
        "(define netB (start-network-service (list 10 0 2 20)))"
        "(define udp-got (spawn (lambda () (let ((m (recv))) (bytes-u8-ref (nth m 4) 0)))))"
        "(send netB (list 'udp-bind 9999 udp-got))"
        "(define wire (spawn (lambda () (let loop () (let ((m (recv)))"
        "  (if (eq? (car m) 'tx) (send netB (list 'rx (cadr m) (caddr m)))) (loop))))))"
        "(define netC (start-network-service (list 10 0 2 15)))"
        "(send netC (list 'register-nic (list 2 0 0 0 0 1) wire))"
        "(define pl (make-bytes 1)) (bytes-u8-set! pl 0 222)"
        "(send netC (list 'udp-send (list 10 0 2 20) (list 2 0 0 0 0 2) 1111 9999 pl))"
        // (3) Robustness: a crafted IPv4 frame with a lying ihl (=15 -> 60-byte
        // header) that runs off a short frame must NOT kill the service -- the
        // checksum read past the buffer would error and terminate the context if
        // unbounded. Feed netD the bad frame, then a valid ARP who-has; netD must
        // survive and still emit the ARP reply.
        "(define dinfo (spawn (lambda () (let ((m (recv))) (get-be16 (cadr m) 20)))))"
        "(define netD (start-network-service (list 10 0 2 15)))"
        "(send netD (list 'register-nic (list 2 0 0 0 0 1) dinfo))"
        "(define bad (make-bytes 40))"                 // shorter than ihl=15 claims
        "(put-be16! bad 12 2048) (bytes-u8-set! bad 14 79)"  // IPv4 ethertype; byte14=0x4F (v4,ihl15)
        "(send netD (list 'rx bad 40))"                // must be ignored, not fatal
        "(send netD (list 'rx areq 42))",              // then a valid ARP -> reply
        env, &err);
    lisp_value arp = lisp_eval_string("arp-info", env, &err);
    lisp_value udp = lisp_eval_string("udp-got", env, &err);
    lisp_value robust = lisp_eval_string("dinfo", env, &err);
    lisp_sched_run(&s, 0);
    char ab[64], ub[32], rb[32];
    lisp_print(lisp_ctx_value(arp), ab, sizeof ab);
    lisp_print(lisp_ctx_value(udp), ub, sizeof ub);
    lisp_print(lisp_ctx_value(robust), rb, sizeof rb);
    if (err == NULL && strcmp(ab, "(2 10 0 2 15)") == 0 && strcmp(ub, "222") == 0 &&
        strcmp(rb, "2") == 0) {  // ARP reply emitted after the malformed frame
        print_str("[SysLisp]  ok  network ARP + UDP round-trip + survives malformed IP\r\n");
        g_pass++;
    } else {
        print_str("[SysLisp] FAIL network  arp-> ");
        print_str(ab);
        print_str("  udp-> ");
        print_str(ub);
        print_str("  robust-> ");
        print_str(rb);
        print_str("\r\n");
        g_fail++;
    }
}

// VirtioGpu-in-Lisp, hardware-free. Three layers, all under the real scheduler:
//   (1) command-struct LAYOUT: build each control-queue command and assert the
//       exact little-endian bytes + total length. This is the offset regression
//       net -- if a field offset drifts from virtio_gpu.h, a byte mismatches.
//   (2) the ctrlq-cmd! transport: a FAKE virtqueue (make-bytes desc/avail/used
//       rings + a fake notify) plus a fake "device" context that bumps used.idx.
//       The test context posts a command via ctrlq-cmd!, which parks on wait-until
//       until used.idx advances; we assert the descriptor pair (desc0 = readable
//       command, NEXT->1; desc1 = writable response) is built correctly and that
//       ctrlq-cmd! returns the response buffer (pre-stamped OK_NODATA).
//   (3) REGISTRATION: a fake coredisplay context captures the register message and
//       returns its name -> must be "Virtio GPU Display".
// Layers (1)/(3) need no timer; (2) parks on wait-until's sleep, so its harness
// mirrors check_sleep -- run the scheduler, take a tick, until the prober is done.
static void check_gpu(lisp_value env) {
    lisp_sched_t s;
    lisp_sched_init(&s, 200000);
    s.per_context_heaps = 1;
    const char *err = NULL;

    // (1) Layout: exercise the exported builders and assert byte offsets/lengths.
    // virtio-gpu exports the builders + response accessors; virtio exports the
    // ctrlq-cmd! transport helper exercised in layer (2).
    lisp_eval_string(
        "(import virtio-gpu virtio)"
        "(define layout-ok (spawn (lambda ()"
        "  (let ((c2 (make-create-2d 1 4 1024 768))"
        "        (ab (make-attach-backing 7 305419896 3145728))"
        "        (ss (make-set-scanout 2 7 0 0 1024 768))"
        "        (tr (make-transfer-2d 7 0 0 0 1024 768))"
        "        (fl (make-flush 7 0 0 1024 768))"
        "        (di (make-display-info-cmd)))"
        "    (and"
        "      (= (bytes-length c2) 40) (= (bytes-u32-ref c2 0) 257)"   // 0x0101
        "      (= (bytes-u32-ref c2 24) 1) (= (bytes-u32-ref c2 28) 4)"
        "      (= (bytes-u32-ref c2 32) 1024) (= (bytes-u32-ref c2 36) 768)"
        "      (= (bytes-length ab) 48) (= (bytes-u32-ref ab 0) 262)"   // 0x0106
        "      (= (bytes-u32-ref ab 24) 7) (= (bytes-u32-ref ab 28) 1)"
        "      (= (bytes-u64-ref ab 32) 305419896) (= (bytes-u32-ref ab 40) 3145728)"
        "      (= (bytes-length ss) 48) (= (bytes-u32-ref ss 0) 259)"   // 0x0103
        "      (= (bytes-u32-ref ss 32) 1024) (= (bytes-u32-ref ss 36) 768)"
        "      (= (bytes-u32-ref ss 40) 2) (= (bytes-u32-ref ss 44) 7)"
        "      (= (bytes-length tr) 56) (= (bytes-u32-ref tr 0) 261)"   // 0x0105
        "      (= (bytes-u64-ref tr 40) 0) (= (bytes-u32-ref tr 48) 7)"
        "      (= (bytes-length fl) 48) (= (bytes-u32-ref fl 0) 260)"   // 0x0104
        "      (= (bytes-u32-ref fl 40) 7)"
        "      (= (bytes-length di) 24) (= (bytes-u32-ref di 0) 256))))))"  // 0x0100
        // (3) Registration: a fake coredisplay captures the register message name.
        "(define reg-name (spawn (lambda () (let ((m (recv))) (cadr m)))))"
        "(define gpu-reg (spawn (lambda ()"
        "  (send reg-name (list 'register \"Virtio GPU Display\" 'unknown (self)"
        "                       (list (list 0 1 1024 768 (make-bytes 16))))) 'sent)))"
        // (2) ctrlq-cmd! over a fake queue. The rings must be SHARED across the
        // prober and the fake-device context, so they are dma-alloc buffers
        // (foreign physical memory -- not copied by the per-context GC, unlike
        // make-bytes). The fake device bumps used.idx once the prober posts; the
        // prober's ctrlq-cmd! parks on wait-until until then. import sys-mmio is
        // legal here because the self-test root env is unrestricted.
        "(import sys-mmio)"
        "(define qsz 4)"
        "(define dsc (dma-alloc (* qsz 16)))"
        "(define avl (dma-alloc (+ 6 (* 2 qsz))))"
        "(define usd (dma-alloc (+ 6 (* 8 qsz))))"
        "(define nfy (dma-alloc 8))"
        "(define fq (list qsz dsc avl usd 0))"
        "(define cmd (dma-alloc 40)) (bytes-u32-set! cmd 0 257)"   // a CREATE_2D-typed cmd
        "(define rsp (dma-alloc 408))"
        "(bytes-u32-set! rsp 0 4352)"                  // pre-stamp OK_NODATA (0x1100)
        // fake device: once avail.idx advances (the prober posted), bump used.idx.
        "(define fdev (spawn (lambda ()"
        "  (let loop () (if (= (bytes-u16-ref avl 2) 0) (begin (sleep 100000) (loop))"
        "                   (bytes-u16-set! usd 2 1))))))"
        "(define ctrl-ok (spawn (lambda ()"
        "  (let ((r (ctrlq-cmd! fq nfy 1 cmd 40 rsp 408 1000000000)))"
        "    (and r"
        "         (= (bytes-u16-ref dsc 12) 1)"           // desc0 flags = NEXT
        "         (= (bytes-u16-ref dsc 14) 1)"           // desc0 next  = 1
        "         (= (bytes-u32-ref dsc 8) 40)"           // desc0 len   = cmd-len
        "         (= (bytes-phys cmd) (bytes-u64-ref dsc 0))"  // desc0 addr = cmd phys
        "         (= (bytes-u16-ref dsc 28) 2)"           // desc1 flags = WRITE
        "         (= (bytes-phys rsp) (bytes-u64-ref dsc 16))"  // desc1 addr = resp phys
        "         (= (gpu-resp-type r) 4352))))))",       // returned resp = OK_NODATA
        env, &err);

    lisp_value layout = lisp_eval_string("layout-ok", env, &err);
    lisp_value regn   = lisp_eval_string("reg-name", env, &err);
    lisp_value ctrl   = lisp_eval_string("ctrl-ok", env, &err);

    // Tick-driven harness (mirrors check_sleep): run the scheduler, take a timer
    // tick, repeat, until the ctrlq prober finishes or a 2s wall bound elapses.
    uint64_t deadline = timer_timestamp_ns() + 2000000000ull;
    while (lisp_ctx_state(ctrl) != LISP_CTX_DONE &&
           timer_timestamp_ns() < deadline) {
        lisp_sched_run(&s, 0);
        if (lisp_ctx_state(ctrl) == LISP_CTX_DONE)
            break;
        __asm__ volatile("sti; hlt");
    }
    __asm__ volatile("cli");

    char lb[16], rb[48], cb[16];
    lisp_print(lisp_ctx_value(layout), lb, sizeof lb);
    lisp_print(lisp_ctx_value(regn), rb, sizeof rb);
    lisp_print(lisp_ctx_value(ctrl), cb, sizeof cb);
    if (err == NULL &&
        lisp_ctx_state(layout) == LISP_CTX_DONE && strcmp(lb, "#t") == 0 &&
        lisp_ctx_state(regn) == LISP_CTX_DONE &&
        strcmp(rb, "\"Virtio GPU Display\"") == 0 &&
        lisp_ctx_state(ctrl) == LISP_CTX_DONE && strcmp(cb, "#t") == 0) {
        print_str("[SysLisp]  ok  virtio-gpu command layout + ctrlq round-trip + registration\r\n");
        g_pass++;
    } else {
        print_str("[SysLisp] FAIL virtio-gpu  layout-> ");
        print_str(lb);
        print_str("  reg-> ");
        print_str(rb);
        print_str("  ctrl-> ");
        print_str(cb);
        print_str("\r\n");
        g_fail++;
    }
}

// AHCI-in-Lisp, hardware-free: exercise the EXPORTED pure FIS/PRDT builders + the
// IDENTIFY parser on make-bytes buffers (no MMIO, no device -- the bring-up that
// touches the HBA is verified live under DISK=). Four layers in one spawned ctx:
//   (1) FIS builder: an IDENTIFY FIS (byte0=0x27 type, C bit, command=0xEC) and a
//       READ DMA EXT FIS for a known LBA48 + count -- assert the FIS type, the
//       command, the device byte (0x40 = LBA), the LBA bytes at the right wire
//       offsets (4/5/6 low, 8/9/10 high), and the split count bytes (12/13).
//   (2) PRDT setter: a fake phys + len -> DBA(lo)/DBAU(hi)/DBC(=len-1, bit31=I) at
//       command-table offset 0x80.
//   (3) IDENTIFY parse: a crafted 512B IDENTIFY (word83 bit10 = LBA48, words
//       100-103 = a known 48-bit sector count, words 27-46 = a byte-swapped model)
//       -> the parser recovers the count and decodes the model string.
//   (4) Registration round-trip (check_storage shape): a fake corestorage captures
//       (register-blockdev name bsize bcount driver), and a fake driver answers a
//       read -> assert the registration carries bsize 512 + the sector count + a
//       ctx, and the read round-trips the lba.
static void check_ahci(lisp_value env) {
    lisp_sched_t s;
    lisp_sched_init(&s, 200000);
    s.per_context_heaps = 1;
    const char *err = NULL;
    lisp_eval_string(
        "(import ahci corestorage driver-util)"
        // (1)+(2)+(3): pure builders/parser, all byte math -> one boolean.
        "(define pure-ok (spawn (lambda ()"
        "  (let ((idf (make-bytes 32)) (rdf (make-bytes 32)) (tbl (make-bytes 512))"
        "        (idb (make-bytes 512)))"
        // IDENTIFY FIS: command 0xEC, count 0, device 0.
        "    (fis-build! idf ATA-IDENTIFY 0 0 0)"
        // READ FIS: lba = 0x123456789A, count = 8, device = 0x40 (LBA).
        "    (fis-build! rdf ATA-READ-EXT 78187493530 8 #x40)"
        // PRDT: phys 0x100000, len 4096 -> DBC = 4095, bit31 set.
        "    (prdt-set! tbl 1048576 4096 #f)"
        // IDENTIFY data: word83 bit10 (LBA48), words100-103 = 0x000012345678,
        // model words 27-28 = 'TE' 'ST' (byte-swapped: hi byte first on the wire).
        "    (bytes-u16-set! idb (* 83 2) #x0400)"
        "    (bytes-u16-set! idb (* 100 2) #x5678)"
        "    (bytes-u16-set! idb (* 101 2) #x1234)"
        "    (bytes-u16-set! idb (* 102 2) 0) (bytes-u16-set! idb (* 103 2) 0)"
        // Model words are ATA byte-swapped (2nd char in the low byte): 0x5445
        // decodes to 'T'(0x54)'E'(0x45), 0x5354 to 'S'(0x53)'T'(0x54) -> "TEST".
        "    (bytes-u16-set! idb (* 27 2) #x5445)"
        "    (bytes-u16-set! idb (* 28 2) #x5354)"
        "    (and"
        // FIS (1): IDENTIFY
        "      (= (bytes-u8-ref idf 0) #x27) (= (bytes-u8-ref idf 1) #x80)"
        "      (= (bytes-u8-ref idf 2) #xEC)"
        // FIS (1): READ -- command, device, LBA bytes, count
        "      (= (bytes-u8-ref rdf 2) #x25) (= (bytes-u8-ref rdf 7) #x40)"
        "      (= (bytes-u8-ref rdf 4) #x9A) (= (bytes-u8-ref rdf 5) #x78)"
        "      (= (bytes-u8-ref rdf 6) #x56) (= (bytes-u8-ref rdf 8) #x34)"
        "      (= (bytes-u8-ref rdf 9) #x12) (= (bytes-u8-ref rdf 10) 0)"
        "      (= (bytes-u8-ref rdf 12) 8) (= (bytes-u8-ref rdf 13) 0)"
        // PRDT (2): DBA lo/hi at 0x80/0x84, DBC = len-1 with bit31 at 0x8C
        "      (= (bytes-u32-ref tbl #x80) 1048576) (= (bytes-u32-ref tbl #x84) 0)"
        "      (= (bytes-u32-ref tbl #x8C) (bitwise-or 4095 #x80000000))"
        // IDENTIFY parse (3): sector count + model
        "      (= (id-sector-count idb) 305419896)"     // 0x12345678
        "      (string=? (id-model idb) \"TEST\"))))))"
        // (4) Registration round-trip via corestorage (check_storage shape).
        "(define disk (spawn (lambda () (let loop () (let ((m (recv)))"
        "   (if (eq? (car m) 'read)"
        "       (let ((b (make-bytes 1))) (bytes-u8-set! b 0 (cadr m))"
        "         (send (cadddr m) (list 'complete 0 b))))"
        "   (loop))))))"
        "(define reg (spawn (lambda () (let ((m (recv)))"
        "  (list (caddr m) (cadddr m) (if (nth m 4) 'has-ctx 'no-ctx))))))"
        "(define stor (start-storage-service))"
        // emulate ahci-init's registration: bsize 512, a sector count, the driver.
        "(send reg (list 'register-blockdev 'ahci0 512 305419896 disk))"
        "(send stor (list 'register-blockdev 'ahci0 512 305419896 disk))"
        "(define rdr (spawn (lambda () (send stor (list 'read 'ahci0 7 1 (self)))"
        "   (let ((m (recv))) (bytes-u8-ref (caddr m) 0)))))",
        env, &err);
    lisp_value pure = lisp_eval_string("pure-ok", env, &err);
    lisp_value reg  = lisp_eval_string("reg", env, &err);
    lisp_value rdr  = lisp_eval_string("rdr", env, &err);
    lisp_sched_run(&s, 0);
    char pb[16], gb[48], rb[16];
    lisp_print(lisp_ctx_value(pure), pb, sizeof pb);
    lisp_print(lisp_ctx_value(reg), gb, sizeof gb);
    lisp_print(lisp_ctx_value(rdr), rb, sizeof rb);
    if (err == NULL &&
        lisp_ctx_state(pure) == LISP_CTX_DONE && strcmp(pb, "#t") == 0 &&
        lisp_ctx_state(reg) == LISP_CTX_DONE &&
        strcmp(gb, "(512 305419896 has-ctx)") == 0 &&
        lisp_ctx_state(rdr) == LISP_CTX_DONE && strcmp(rb, "7") == 0) {
        print_str("[SysLisp]  ok  ahci FIS/PRDT builders + IDENTIFY parse + registration\r\n");
        g_pass++;
    } else {
        print_str("[SysLisp] FAIL ahci  pure-> ");
        print_str(pb);
        print_str("  reg-> ");
        print_str(gb);
        print_str("  read-> ");
        print_str(rb);
        print_str("\r\n");
        g_fail++;
    }
}

// lfb-in-Lisp (the linear framebuffer driver), hardware-free. Three layers, no
// device map -- lfb-init itself maps real MMIO, so it is NOT called here:
//   (1) REGISTRY: the boot framebuffer's WIDTH key is a number at boot (SysReg's
//       bootinfo wrote it), and a missing key reads back #f. This is the sys-reg
//       capability path the driver's fb-params rides.
//   (2) pack-rgb (pure): the exported packer assembles R8G8B8 into the pixel word
//       for the standard 16/8/0 layout -- assert two known colours.
//   (3) REGISTRATION: a fake coredisplay context captures the register message;
//       drive the exported lfb-register-msg with fake geometry/ctx (NOT a real
//       fb map) and assert the captured name is "Linear Framebuffer".
static void check_lfb(lisp_value env) {
    lisp_sched_t s;
    lisp_sched_init(&s, 200000);
    s.per_context_heaps = 1;
    const char *err = NULL;

    lisp_eval_string(
        "(import lfb sys-reg)"
        // (1) registry + (2) pack-rgb: pure/synchronous, run in one spawned probe.
        "(define lfb-pure (spawn (lambda ()"
        "  (and (number? (reg-read-uint \"HW/BOOTINFO/FRAMEBUFFER\" \"WIDTH\"))"
        "       (eq? (reg-read-uint \"HW/BOOTINFO/FRAMEBUFFER\" \"NOPE\") #f)"
        "       (= (pack-rgb 255 0 0 16 8 0) 16711680)"      // 0xFF0000
        "       (= (pack-rgb 18 52 86 16 8 0) 1193046)))))"  // 0x123456
        // (3) registration: a fake coredisplay captures the register name; a driver
        // probe drives the exported lfb-register-msg against fake geometry + ctx.
        "(define lfb-reg-name (spawn (lambda () (let ((m (recv))) (cadr m)))))"
        "(define lfb-reg (spawn (lambda ()"
        "  (lfb-register-msg lfb-reg-name 1024 768 4096 (self)) 'sent)))",
        env, &err);

    lisp_value pure = lisp_eval_string("lfb-pure", env, &err);
    lisp_value regn = lisp_eval_string("lfb-reg-name", env, &err);
    lisp_sched_run(&s, 0);

    char pb[16], rb[48];
    lisp_print(lisp_ctx_value(pure), pb, sizeof pb);
    lisp_print(lisp_ctx_value(regn), rb, sizeof rb);
    if (err == NULL &&
        lisp_ctx_state(pure) == LISP_CTX_DONE && strcmp(pb, "#t") == 0 &&
        lisp_ctx_state(regn) == LISP_CTX_DONE &&
        strcmp(rb, "\"Linear Framebuffer\"") == 0) {
        print_str("[SysLisp]  ok  lfb registry read + pack-rgb + registration\r\n");
        g_pass++;
    } else {
        print_str("[SysLisp] FAIL lfb  pure-> ");
        print_str(pb);
        print_str("  reg-> ");
        print_str(rb);
        print_str("\r\n");
        g_fail++;
    }
}

// rtl8139-in-Lisp, hardware-free. The driver's pure cores (rx-parse-one, tx-fill!)
// take their buffers as arguments, so they run against FAKE make-bytes buffers --
// rtl8139-init itself needs a real device, so it is NOT called here. Three layers,
// all under the real scheduler:
//   (1) RX PARSE: hand-write two back-to-back 8139 rx-headers (status ROK, length
//       = frame+4 for the CRC) into a fake ring and assert rx-parse-one recovers
//       each frame's offset (header+4), CRC-stripped length, and the dword-rounded
//       next-header offset; a third parse near the 64K end checks the modulo wrap.
//   (2) TX FILL: fake regs + four fake tx slots + a free cell; tx-fill! must copy
//       the frame to offset 0 of the current slot, write (len & 0xFFF) into that
//       slot's TX-STS register, and advance the free cell mod 4.
//   (3) REGISTRATION: a fake corenetwork context captures the (register-nic mac
//       tx-ctx) message the driver sends and returns the mac -- proving the
//       register handshake's shape (the rest of bring-up is the live boot test).
static void check_rtl8139(lisp_value env) {
    lisp_sched_t s;
    lisp_sched_init(&s, 200000);
    s.per_context_heaps = 1;
    const char *err = NULL;
    lisp_eval_string(
        "(import rtl8139 driver-util)"
        // (1) RX parse over a fake ring with two frames + a wrap probe.
        "(define rx-ok (spawn (lambda ()"
        // big enough that the wrap-probe header at 65530 is read in-bounds.
        "  (let ((rb (make-bytes 65540)))"
        // frame A at off 0: status=ROK(1), length=64 (60-byte frame + 4 CRC).
        "    (bytes-u16-set! rb 0 1) (bytes-u16-set! rb 2 64)"
        // frame B at off 68 (= (0+64+4) dword-rounded): status=ROK, length=50.
        "    (bytes-u16-set! rb 68 1) (bytes-u16-set! rb 70 50)"
        // wrap probe near the ring end: a header at 65530 with length 12.
        "    (bytes-u16-set! rb 65530 1) (bytes-u16-set! rb 65532 12)"
        "    (let ((a (rx-parse-one rb 0))"
        "          (b (rx-parse-one rb 68))"
        // wrap: next-off math runs off the 64K ring -> mod 65536.
        "          (w (rx-parse-one rb 65530)))"
        "      (and (= (nth a 0) 4) (= (nth a 1) 60) (= (nth a 2) 68)"
        "           (= (nth b 0) 72) (= (nth b 1) 46) (= (nth b 2) 124)"
        // (65530 + 12 + 4 + 3) & ~3 = 65548 & ~3 = 65548 -> mod 65536 = 12.
        "           (= (nth w 2) 12)))))))"
        // (2) TX fill: slot 0 gets the frame at offset 0, TX-STS(0)=len, free->1.
        "(define tx-ok (spawn (lambda ()"
        "  (let ((regs (make-bytes 256))"
        "        (t0 (make-bytes 2048)) (t1 (make-bytes 2048))"
        "        (t2 (make-bytes 2048)) (t3 (make-bytes 2048))"
        "        (free (make-cell 0))"
        "        (frame (make-bytes 4)))"
        "    (bytes-u8-set! frame 0 222) (bytes-u8-set! frame 1 173)"
        "    (let ((r (tx-fill! regs (list t0 t1 t2 t3) free frame 4)))"
        "      (and (eq? r 'sent)"
        "           (= (bytes-u8-ref t0 0) 222) (= (bytes-u8-ref t0 1) 173)"
        "           (= (bytes-u32-ref regs 16) 4)"   // TX-STS(0) = #x10, len & 0xFFF
        "           (= (cell-ref free) 1)))))))"
        // (3) Registration: a fake corenetwork captures (register-nic mac tx-ctx).
        "(define reg-mac (spawn (lambda () (let ((m (recv)))"
        "  (if (eq? (car m) 'register-nic) (cadr m) 'wrong)))))"
        "(define reg-send (spawn (lambda ()"
        "  (send reg-mac (list 'register-nic (list 82 84 0 18 52 86) (self))) 'sent)))",
        env, &err);
    lisp_value rx  = lisp_eval_string("rx-ok", env, &err);
    lisp_value tx  = lisp_eval_string("tx-ok", env, &err);
    lisp_value reg = lisp_eval_string("reg-mac", env, &err);

    // tx-fill! parks on wait-until's sleep (the slot-idle poll), so drive the
    // scheduler with timer ticks until tx finishes or a 2s wall bound elapses.
    uint64_t deadline = timer_timestamp_ns() + 2000000000ull;
    while (lisp_ctx_state(tx) != LISP_CTX_DONE &&
           timer_timestamp_ns() < deadline) {
        lisp_sched_run(&s, 0);
        if (lisp_ctx_state(tx) == LISP_CTX_DONE)
            break;
        __asm__ volatile("sti; hlt");
    }
    __asm__ volatile("cli");

    char xb[16], tb[16], rb[48];
    lisp_print(lisp_ctx_value(rx), xb, sizeof xb);
    lisp_print(lisp_ctx_value(tx), tb, sizeof tb);
    lisp_print(lisp_ctx_value(reg), rb, sizeof rb);
    if (err == NULL &&
        lisp_ctx_state(rx) == LISP_CTX_DONE && strcmp(xb, "#t") == 0 &&
        lisp_ctx_state(tx) == LISP_CTX_DONE && strcmp(tb, "#t") == 0 &&
        lisp_ctx_state(reg) == LISP_CTX_DONE && strcmp(rb, "(82 84 0 18 52 86)") == 0) {
        print_str("[SysLisp]  ok  rtl8139 RX parse + TX fill + registration\r\n");
        g_pass++;
    } else {
        print_str("[SysLisp] FAIL rtl8139  rx-> ");
        print_str(xb);
        print_str("  tx-> ");
        print_str(tb);
        print_str("  reg-> ");
        print_str(rb);
        print_str("\r\n");
        g_fail++;
    }
}

// (sleep ns) under the real wake path: a context records uptime, sleeps ~20ms,
// records uptime again, and returns the elapsed time. The context PARKS on the
// timeout queue -- it is woken by the periodic tick, not a busy-wait -- so this
// also verifies the tick is actually delivered (if it were not, the context would
// never wake and the deadline-bounded harness below would report a hang). The
// harness mirrors run_isr_demo: run the scheduler, then sti+hlt to take the tick,
// until the sleeper finishes or a 2s wall-clock bound elapses.
static void check_sleep(lisp_value env) {
    lisp_sched_t s;
    lisp_sched_init(&s, 2000);
    s.per_context_heaps = 1;
    const char *err = NULL;
    lisp_eval_string(
        "(define slept (spawn (lambda () (let ((a (uptime-ns)))"
        "                                  (sleep 20000000) (- (uptime-ns) a)))))",
        env, &err);
    lisp_value slept = lisp_eval_string("slept", env, &err);
    uint64_t deadline = timer_timestamp_ns() + 2000000000ull;  // 2s safety bound
    while (lisp_ctx_state(slept) != LISP_CTX_DONE &&
           timer_timestamp_ns() < deadline) {
        lisp_sched_run(&s, 0);                 // run until the context parks (or done)
        if (lisp_ctx_state(slept) == LISP_CTX_DONE)
            break;
        __asm__ volatile("sti; hlt");          // wait for the next tick, then retry
    }
    __asm__ volatile("cli");                   // restore the phase's interrupts-off
    lisp_value v = lisp_ctx_value(slept);
    // The 20ms sleep should elapse roughly on time -- allow a wide band for tick
    // granularity and a slow emulator, but it must be in the ballpark, not 0.
    if (err == NULL && lisp_ctx_state(slept) == LISP_CTX_DONE && lisp_is_fixnum(v) &&
        lisp_fixnum_val(v) >= 15000000 && lisp_fixnum_val(v) <= 500000000) {
        char nb[24];
        lisp_print(v, nb, sizeof nb);
        print_str("[SysLisp]  ok  (sleep) parks and a timer tick wakes it -> ");
        print_str(nb);
        print_str(" ns\r\n");
        g_pass++;
    } else {
        print_str("[SysLisp] FAIL (sleep) -- the periodic tick did not wake it\r\n");
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
    // sys-cmdline through the capability path: the mechanism, asserted without
    // depending on any boot-specific flag so it holds on every boot (cardinal.test
    // or not). The empty substring is always "present" (strstr returns the string);
    // a guaranteed-absent flag is not; and an absent key reads back #f.
    check(env, "(begin"
               "  (define-module cmdline-probe (export run)"
               "    (import sys-cmdline)"
               "    (define (run)"
               "      (and (cmdline-has? \"\")"
               "           (not (cmdline-has? \"zzqq.absent.flag\"))"
               "           (eq? (cmdline-get \"zzqq.absent.key=\") #f))))"
               "  (import (cmdline-probe (prefix cl:)))"
               "  (cl:run))",
          "#t");
    // driver-util byte-order helpers: a big-endian u32 round-trips, and the bytes
    // land MSB-first (the network order the IP/UDP/ARP layers depend on).
    check(env, "(begin"
               "  (define-module du-probe (export run)"
               "    (import driver-util)"
               "    (define (run)"
               "      (let ((b (make-bytes 4)))"
               "        (put-be32! b 0 305419896)"            // 0x12345678
               "        (and (= (get-be32 b 0) 305419896)"
               "             (= (bytes-u8-ref b 0) 18)"       // 0x12 = MSB first
               "             (= (get-be16 b 0) 4660)))))"     // 0x1234
               "  (import (du-probe (prefix du:)))"
               "  (du:run))",
          "#t");
    // The driver substrate added for the device-driver port: a sub-4GB DMA buffer
    // (phys in the 32-bit range), a registry read (a present key -> a number, an
    // absent one -> #f), an initrd read (init.clp is present and non-empty, a
    // bogus path -> #f), and the wait-until busy-wait helper (true predicate ->
    // #t, never-true within the budget -> #f). MSI multiplexing is exercised live
    // by virtio-net, not here (it needs a device).
    check(env, "(begin"
               "  (define-module subst-probe (export run)"
               "    (import sys-mmio sys-reg sys-initrd driver-util)"
               "    (define (run)"
               "      (and (let ((d (dma-alloc-32 64)))"
               "             (and (> (bytes-phys d) 0) (< (bytes-phys d) 4294967296)))"
               "           (number? (reg-read-uint \"HW/PCI\" \"COUNT\"))"
               "           (eq? (reg-read-uint \"HW/NOPE\" \"NOPE\") #f)"
               "           (> (bytes-length (initrd-file \"./lisp/init.clp\")) 0)"
               "           (eq? (initrd-file \"./lisp/nope.clp\") #f)"
               "           (wait-until (lambda () #t) 1000)"
               "           (not (wait-until (lambda () #f) 1000)))))"
               "  (import (subst-probe (prefix sp:)))"
               "  (sp:run))",
          "#t");
    // CoreDisplay's EDID parser (pure): build a synthetic 128-byte EDID with a
    // known digital 8bpc input, one 1920x1080@60 16:9 standard timing, a 1920x1080
    // detailed mode, and the monitor name "TESTDISPLAY", then assert parse-edid
    // recovers them. No scheduler needed -- it is byte math -- so it runs inline.
    check(env, "(begin"
               "  (define-module edid-probe (export run)"
               "    (import coredisplay)"
               "    (define (run)"
               "      (let ((b (make-bytes 128)))"
               "        (bytes-u8-set! b 1 255) (bytes-u8-set! b 2 255) (bytes-u8-set! b 3 255)"
               "        (bytes-u8-set! b 4 255) (bytes-u8-set! b 5 255) (bytes-u8-set! b 6 255)"
               "        (bytes-u8-set! b 20 160) (bytes-u8-set! b 23 120) (bytes-u8-set! b 35 32)"
               "        (bytes-u8-set! b 38 209) (bytes-u8-set! b 39 192)"
               "        (let loop ((i 40)) (if (< i 54) (begin (bytes-u8-set! b i 1) (loop (+ i 1))) 0))"
               "        (bytes-u8-set! b 54 2) (bytes-u8-set! b 55 58)"
               "        (bytes-u8-set! b 56 128) (bytes-u8-set! b 58 113)"
               "        (bytes-u8-set! b 59 56) (bytes-u8-set! b 61 64)"
               "        (bytes-u8-set! b 71 30)"
               "        (bytes-u8-set! b 75 252)"
               "        (bytes-u8-set! b 77 84) (bytes-u8-set! b 78 69) (bytes-u8-set! b 79 83)"
               "        (bytes-u8-set! b 80 84) (bytes-u8-set! b 81 68) (bytes-u8-set! b 82 73)"
               "        (bytes-u8-set! b 83 83) (bytes-u8-set! b 84 80) (bytes-u8-set! b 85 76)"
               "        (bytes-u8-set! b 86 65) (bytes-u8-set! b 87 89) (bytes-u8-set! b 88 10)"
               "        (let* ((e (parse-edid b))"
               "               (st (car (cdr (assq 'standard-timings e))))"
               "               (dm (car (cdr (assq 'detailed-modes e)))))"
               "          (list (cdr (assq 'bit-depth e)) (car st) (cadr st)"
               "                (string=? (cdr (assq 'display-name e)) \"TESTDISPLAY\")"
               "                (cdr (assq 'hactive dm)) (cdr (assq 'vactive dm)))))))"
               "  (import (edid-probe (prefix ed:)))"
               "  (ed:run))",
          "(8 1920 1080 #t 1920 1080)");
    check_scheduler(env);
    check_capabilities(env);
    check_repl(env);
    check_power(env);
    check_storage(env);
    check_ahci(env);
    check_network(env);
    check_gpu(env);
    check_lfb(env);
    check_rtl8139(env);
    check_sleep(env);
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

    // Enter the LIVE phase interrupts-ON. The single-core boot phase ran
    // interrupts-off by design (deterministic, no preemption while the BSP built
    // the shared heap), and the self-test harnesses cli() back to that default
    // (see run_self_test). That convention must be reversed exactly here -- the
    // heap is frozen and this is the resident scheduler -- so device interrupts
    // (a driver's MSI/MSI-X, an ISA IRQ) are delivered PROMPTLY while a context
    // evaluates, not just in the narrow `sti; hlt` idle window (where the
    // higher-priority scheduler tick always wins, starving lower-vector device
    // interrupts -- the bug that made every MSI fall back to polling). This is
    // the model the wait primitives already assume: they cli() around their
    // check-then-park precisely because IF=1 is meant to be ambient once live.
    __asm__ volatile("sti");

    // The timeout-queue tick is per-core (timer_features_local). The BSP armed its
    // own in lisp_scheduler_enter; each AP arms its here so a context sleeping /
    // waiting-with-timeout on this core is woken. (Re-arming on the BSP would be
    // harmless, but it is already done.)
    if (id != 0)
        lisp_arm_timer_tick();

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
        // Interactive serial REPL (cardinal.repl): announce it on the still-raw
        // log, switch the link into framed CSMUX (log -> CH_LOG, REPL <-> CH_REPL),
        // then spawn the REPL context. It claims COM1 RX (IRQ 4) and parks on it --
        // arm-rx is done by the context AFTER irq-register so an early byte can't
        // hit an unrouted line -- so the BSP idles (hlt) until a keystroke arrives.
        if (g_repl_enabled) {
            print_str("[SysLisp] cardinal.repl: starting serial REPL on CSMUX ch2\r\n");
            csmux_activate();
            const char *rerr = NULL;
            lisp_eval_string("(start-repl)", g_env, &rerr);
            if (rerr != NULL) {
                print_str("[SysLisp] start-repl error: ");
                print_str(rerr);
                print_str("\r\n");
            }
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
    // Run the scheduler until the PROOF context finishes -- not until every
    // context blocks. `sched` also carries the long-lived service contexts that
    // (system-init) spawned, and with interrupts live the periodic tick keeps
    // waking sleepers, so an "all contexts blocked" condition may never hold;
    // gating on it (the old lisp_sched_run(&sched, 0)) would spin here forever
    // and never reach the resident loop. Bounded passes let the finite proof
    // complete, then we hand off to the resident loop, which idles at sti;hlt.
    if (err == NULL)
        for (int guard = 0; guard < 100000 &&
                            lisp_ctx_state(proof) != LISP_CTX_DONE &&
                            lisp_ctx_state(proof) != LISP_CTX_ERROR;
             guard++)
            lisp_sched_run(&sched, 64);

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

    // Opt in to the interactive serial REPL via the kernel command line. Off by
    // default: a normal boot stays raw-COM1 (the CI smoke test reads it directly).
    CardinalBootInfo *bi = GetBootInfo();
    g_repl_enabled = (bi != NULL) && (strstr(bi->Cmdline, "cardinal.repl") != NULL);
    // "cardinal.test": the in-OS test gate. SysLisp's single-core self-tests are
    // the meaningful in-OS suite under interpreter-as-scheduler (SysTest's native-
    // task framework can't run -- there is no native scheduler), so in test mode
    // we run them and exit the machine rather than booting on into the scheduler.
    int test_mode = (bi != NULL) && (strstr(bi->Cmdline, "cardinal.test") != NULL);

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
    // (sleep ns): yield the running context for a duration. Ambient like uptime-ns
    // -- any context may deschedule itself; it bears no hardware authority. Backed
    // by the timeout queue + the periodic tick armed below.
    lisp_env_define(g_env, lisp_make_symbol("sleep", 5),
                    lisp_make_primitive(prim_sleep, "sleep"));
    // Arm the periodic tick on the BSP now, so a (sleep)/timeout wait works during
    // the single-core self-test phase too (each AP arms its own in lisp_core_loop).
    lisp_arm_timer_tick();
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

    // Test gate: in "cardinal.test" mode, exit the machine on the self-test
    // verdict via QEMU's isa-debug-exit (0xf4: pass=0x10 -> exit 33, fail=0x11 ->
    // exit 35, matching SysTest's encoding), instead of booting on into the
    // scheduler. run-tests-qemu.sh greps "[SysLisp] ALL TESTS PASSED".
    if (test_mode) {
        outb(0xf4, g_fail == 0 ? 0x10 : 0x11);
        for (;;)
            __asm__ volatile("cli; hlt");  // if isa-debug-exit is absent, just stop
    }

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
