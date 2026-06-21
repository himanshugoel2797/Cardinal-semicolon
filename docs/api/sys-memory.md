The Cardinal; **memory subsystem** is split across three kernel-privileged `Sys*`
modules. `SysPhysicalMemory` owns the physical frame allocator (a coalescing
free-list of `(address, page-count)` runs) and hands out raw physical page
ranges. `SysVirtualMemory` builds the `x86_64` 4-level page tables on top of it:
it owns the shared kernel address space (the upper-half PML4 every process
inherits), per-task address spaces (`vmem_t`), the permanent physical-to-virtual
maps used to touch physical memory without an explicit mapping, a bump
`vmalloc`, and the cross-core TLB-shootdown machinery. `SysMemory` is a thin
higher-level shim (its public surface today is just `stack_alloc`, currently a
stub). All of this runs in kernel space with no floating point and no libc; the
addressing convention `virt < 0` (high-half) means "kernel address" throughout
the virtual layer.

## `physmem_alloc_flags_t`

- **kind:** enum
- **lang:** c
- **source:** `modules/inc/SysPhysicalMemory/phys_mem.h`
- **hash:** d2e19d99a2bc572f

Bit flags describing the kind/placement of a physical allocation requested from `physmem_alloc`.

Defined values:

- `physmem_alloc_flags_reclaimable` (1<<0) — frame may later be reclaimed.
- `physmem_alloc_flags_data` (1<<1) — general data frame.
- `physmem_alloc_flags_instr` (1<<2) — frame will hold instructions.
- `physmem_alloc_flags_pagetable` (1<<3) — frame will be used as a page table.
- `physmem_alloc_flags_zero` (1<<4) — caller wants zeroed memory.
- `physmem_alloc_flags_32bit` (1<<5) — frame must lie below 4 GiB (for 32-bit-only DMA).

**Caveat:** the current `physmem_alloc` implementation zeroes `domain`, `color`,
and `flags` at entry, so most of these hints are advisory and **not** acted
upon — the one exception is `physmem_alloc_flags_32bit`, which is honored at the
call sites that pass it directly even though the parameter copy is reset (see
`physmem_alloc`). Treat zeroing as a request that may be ignored.

---

## `PHYSMEM_NO_ALLOC`

- **kind:** macro
- **lang:** c
- **source:** `modules/inc/SysPhysicalMemory/phys_mem.h`
- **hash:** 2e0366c5d75def01

Sentinel `((uintptr_t)-1)` returned by `physmem_alloc` when a request cannot be satisfied.

Physical address `0` is a legal allocation, so the all-ones value is the failure
sentinel rather than NULL. Callers **must** compare the full `uintptr_t` return
against `PHYSMEM_NO_ALLOC` before using it, and — critically — must do that
comparison *before* truncating to 32 bits when they used
`physmem_alloc_flags_32bit`.

---

## `physmem_alloc`

- **kind:** function
- **lang:** c
- **source:** `modules/SysPhysicalMemory/src/page_allocator.c`
- **hash:** 9899dbb36da61a44

Allocate a physically contiguous range of at least `size` bytes from the frame allocator's free list.

**Parameters**

- `domain` — intended NUMA domain. **Ignored** (reset to 0 internally).
- `color` — intended cache color. **Ignored** (reset to 0 internally).
- `flags` — `physmem_alloc_flags_t`. The local copy is reset to 0, so only
  `physmem_alloc_flags_32bit` (enforced via the `flags & physmem_alloc_flags_32bit`
  check before the reset takes effect at the relevant call paths) constrains
  placement in practice; the others are advisory.
- `size` — requested size in bytes.

**Returns** the physical base address of the allocated run, or `PHYSMEM_NO_ALLOC`
on out-of-memory / excessive fragmentation.

**Semantics.** `size` is rounded up to the allocator's bottom granularity
(`BTM_LEVEL`, the page quantum); the page count is held in 64 bits so an
absurdly large request fails cleanly as OOM rather than wrapping. The allocator
scans the free list (a queue of `(addr, page_count)` entries), and on a run that
is large enough, carves off the requested pages and re-inserts the remainder at
the front. If `physmem_alloc_flags_32bit` is set, runs whose address has any
bit above 32 are skipped. It makes up to two passes, compacting/coalescing the
queue between them, before giving up. Does **not** zero the returned memory.
This call manipulates a shared free list — callers in interrupt-sensitive or
SMP contexts must serialize at a higher layer (the virtual layer always holds
its page-table lock with interrupts disabled around `physmem_alloc`).

---

## `physmem_free`

- **kind:** function
- **lang:** c
- **source:** `modules/SysPhysicalMemory/src/page_allocator.c`
- **hash:** fdbbdf0ae2bd0583

Return a previously allocated physical range to the free list.

**Parameters**

- `addr` — physical base address. **Must** be `BTM_LEVEL`-aligned or the kernel
  panics ("Misaligned address").
- `size` — size in bytes. **Must** be a multiple of `BTM_LEVEL` or the kernel
  panics ("Misaligned size").

**Returns** nothing.

**Semantics.** The range is split into entries of at most `MAX_ENTRIES` pages
and enqueued onto the free list; `free_mem` is credited. There is no
double-free detection — freeing a range that was never allocated, or freeing it
twice, silently corrupts the allocator. As with `physmem_alloc`, the queue is
shared global state and is expected to be called under the caller's serialization
(the virtual layer holds its page-table lock).

---

## `vmem_t`

- **kind:** typedef
- **lang:** c
- **source:** `modules/inc/SysVirtualMemory/vmem.h`
- **hash:** 5543463775ad4234

Opaque handle for one address space — owns a hardware PML4 page plus bookkeeping.

The type is forward-declared in the header (`typedef struct vmem vmem_t;`); the
full definition lives in the platform implementation and holds the PML4's
physical address (`pml4_phys`, what cr3 points at), a physmap pointer to that
page (`pml4`), `flags`, a per-AS spinlock (`lock`), and `active_apic` — the APIC
id of the core this address space is currently active on, or -1. The model is
**one task per address space, no migration**, so an address space is live on at
most one core at a time, which is what makes targeted (rather than broadcast)
user-range TLB shootdowns correct.

---

## `vmem_flags_t`

- **kind:** enum
- **lang:** c
- **source:** `modules/inc/SysVirtualMemory/vmem.h`
- **hash:** 0d42f896f9382dee

Permission and cache/ownership flags for `vmem_map` / `vmem_phystovirt`.

Permission bits: `vmem_flags_read` (0, the default), `vmem_flags_write` (1<<0),
`vmem_flags_exec` (1<<1), and the convenience combo `vmem_flags_rw`
(read|write). Cache mode bits are mutually exclusive in practice and map to the
`x86_64` PAT/PWT/PCD encoding: `vmem_flags_cachewritethrough` (1<<2),
`vmem_flags_cachewriteback` (1<<3), `vmem_flags_cachewritecomplete` (1<<4),
`vmem_flags_uncached` (1<<5). Ownership: `vmem_flags_kernel` (1<<10) sets
nothing in the PTE itself but marks intent; `vmem_flags_user` (1<<11) sets the
hardware USER bit. Absence of `vmem_flags_exec` sets the NX bit. Note that
`vmem_phystovirt` only honors `vmem_flags_cachewriteback` and
`vmem_flags_uncached` (it has no write-through/write-complete physmap window).

---

## `vmem_init`

- **kind:** function
- **lang:** c
- **source:** `modules/SysVirtualMemory/src/platform/x86_64/pc/vmem.c`
- **hash:** 5933dc5171cb4228

Build the master kernel address space and switch the boot core onto it.

**Returns** `0` (panics on any allocation failure).

**Semantics (boot-only, single-core).** Allocates the master kernel PML4 whose
upper 256 entries *are* the shared kernel address space, then installs three
permanent kernel maps: the kernel image window at `KERN_TOP_BASE` (2 GiB,
RWX/WB), and the full physical-to-virtual maps at `KERN_PHYSMAP_BASE` (256 GiB
write-back) and `KERN_PHYSMAP_BASE_UC` (256 GiB uncached) that `vmem_phystovirt`
returns into. It reserves the vmalloc PML4 entry and pre-creates **all** 256
kernel-half PML4 entries (256..511) up front, installing empty PDPTs where
needed: because each process PML4 *copies* the kernel-half entries once at
creation and shares the lower-level tables, every top-level kernel entry must
exist before any address space is cloned, or a later kernel mapping that needed
a new PML4 entry would be invisible to already-created address spaces. Finally
loads cr3. Must run before any other vmem call, before APs boot, and before any
task is created.

---

## `vmem_map`

- **kind:** function
- **lang:** c
- **source:** `modules/SysVirtualMemory/src/platform/x86_64/pc/vmem.c`
- **hash:** 150e33e0a1a0093a

Install a virtual-to-physical mapping of `size` bytes in an address space.

**Parameters**

- `vm` — the target address space. Pass `NULL` for kernel mappings (`virt < 0`);
  it is dereferenced only for user mappings.
- `virt` — virtual base. A negative (high-half) `virt` selects the **shared
  kernel** map; a non-negative `virt` selects the per-task user map in `vm`.
- `phys` — physical base.
- `size` — bytes to map.
- `perms` — `vmem_flags_t` permission + cache bits.
- `flags` — extra `vmem_flags_t`; not currently interpreted by the walker.

**Returns** `CS_OK` (0) on success; `CS_ALREADYMAPPED` if any covered slot is
already present (or collides with a large page); other `cs_error` from the
recursive walk.

**Semantics & locking.** Recursively walks/creates the 4 levels, using large
pages (2 MiB / 1 GiB) when the range is aligned and large enough, otherwise
allocating intermediate page tables via `physmem_alloc`. A kernel map writes
straight into the master kernel PML4 and shared lower tables, so it is
**immediately visible in every address space** with no resync. The call takes
`kmem.lock` (kernel) or `vm->lock` (user) **with interrupts disabled** for the
whole walk — the preemption-timer ISR also takes these locks (via
`vmem_setactive`), so holding them with interrupts on would self-deadlock or
deadlock ABBA across cores. Mapping does not flush remote TLBs (irrelevant for
fresh maps); changing or removing a mapping is `vmem_unmap` + `vmem_shootdown`.

---

## `vmem_unmap`

- **kind:** function
- **lang:** c
- **source:** `modules/SysVirtualMemory/src/platform/x86_64/pc/vmem.c`
- **hash:** ee2f049f8142804f

Remove a mapping of `size` bytes and flush the local core's TLB for the range.

**Parameters** — `vm` (NULL for kernel/`virt < 0`), `virt`, `size`.

**Returns** `CS_OK` on success, `CS_NOMAPPING` if part of the range was not
mapped; panics on the (unimplemented) partial-unmap of a large page.

**Semantics & locking.** Walks the tables clearing PTEs, and frees now-empty
intermediate page tables back to `physmem_alloc`'s pool with `physmem_free`.
Held under `kmem.lock`/`vm->lock` with interrupts disabled, exactly like
`vmem_map`. It flushes **only the calling core's** TLB (`vmem_local_flush`),
which is correct for the common self-unmap and harmless otherwise. **Any other
core that may have cached the translation is NOT invalidated here** — for a
kernel range, or a user range whose address space is active on a different core,
the caller must follow up with `vmem_shootdown` (interrupts on, no lock held)
*before* the freed/reused frame or the tightened permissions are relied upon.
Snapshot the target core via `vmem_active_apic` under this same lock.

---

## `vmem_create`

- **kind:** function
- **lang:** c
- **source:** `modules/SysVirtualMemory/src/platform/x86_64/pc/vmem.c`
- **hash:** 27c7026fd2e8dd76

Allocate a new, empty per-task address space that inherits the shared kernel half.

**Parameters** — `vm` (out): receives the new `vmem_t *`.

**Returns** `0` on success, `-1` on allocation failure (of the struct or its
PML4 frame).

**Semantics.** `malloc`s the `vmem_t`, allocates a fresh PML4 page
(`physmem_alloc_flags_pagetable`), zeroes it, marks it `vmem_flags_user` with
`active_apic = -1`, then copies the master kernel PML4's upper 256 entries
(`kmem.pml4[256..511]`) into it under `kmem.lock` (interrupts disabled). Those
entries point at shared lower-level tables, so later kernel-map changes stay
visible without per-AS resync (this is why `vmem_init` pre-creates every kernel
PML4 entry). The user half starts entirely empty.

---

## `vmem_destroy`

- **kind:** function
- **lang:** c
- **source:** `modules/SysVirtualMemory/src/platform/x86_64/pc/vmem.c`
- **hash:** fae4a865d81052a2

Free an address space's PML4 page and its handle.

**Parameters** — `vm` (NULL is a no-op).

**Returns** nothing.

**Semantics.** Takes `vm->lock` with interrupts disabled, frees **only** the
PML4 page (`physmem_free`) and the `vmem_t`. The kernel-half entries point at
shared tables that must not be freed; the user-half lower-level tables are
expected to have been released by `vmem_unmap` during task teardown — this call
does **not** walk and free them, so unmap the user space first to avoid leaking
intermediate page tables and data frames.

---

## `vmem_setactive`

- **kind:** function
- **lang:** c
- **source:** `modules/SysVirtualMemory/src/platform/x86_64/pc/vmem.c`
- **hash:** 9c99a7c28fcb8f65

Make `vm` the calling core's active address space (load its cr3).

**Returns** `0`.

**Semantics.** With interrupts disabled: updates the per-core `cur_vmem`,
atomically clears the previously-active space's `active_apic` and stamps `vm`'s
`active_apic` with this core's APIC id (when the APIC-id helper is available;
before `vmem_smp_init` nothing schedules, so the early gap is harmless), then
loads cr3 with `vm->pml4_phys`. The cr3 write is a full TLB flush (kernel pages
are not marked global), so a core entering an address space always starts with a
clean TLB and never needs a shootdown for edits made while it was elsewhere.
Called from the scheduler/preemption path, hence the interrupts-off discipline
shared with `vmem_map`/`vmem_unmap`.

---

## `vmem_getactive`

- **kind:** function
- **lang:** c
- **source:** `modules/SysVirtualMemory/src/platform/x86_64/pc/vmem.c`
- **hash:** 108fcb7cc3a1c388

Return the calling core's currently active address space.

**Parameters** — `vm` (out): receives the active `vmem_t *` (may be `NULL` if the
core is on the bare shared kernel PML4, i.e. not running a task).

**Returns** `0`. Reads the per-core TLS `cur_vmem`; no locking.

---

## `vmem_virttophys`

- **kind:** function
- **lang:** c
- **source:** `modules/SysVirtualMemory/src/platform/x86_64/pc/vmem.c`
- **hash:** bd1ea35991f163db

Translate a virtual address to its physical address by walking the page tables.

**Parameters**

- `vm` — address space for user (`virt >= 0`) lookups; ignored (may be NULL) for
  kernel addresses, which walk the shared master kernel PML4.
- `virt` — the virtual address.
- `phys` (out) — receives the physical address (page base plus in-page offset;
  large pages handled).

**Returns** `0` on success; `-1` if unmapped at some level; `-2` if a user
address was requested with `vm == NULL`.

**Semantics.** A pure read-walk (no locking) using the physmap to dereference
each table level. Sign of `virt` selects kernel vs. user space, mirroring
`vmem_map`.

---

## `vmem_phystovirt`

- **kind:** function
- **lang:** c
- **source:** `modules/SysVirtualMemory/src/platform/x86_64/pc/vmem.c`
- **hash:** 29bd8d47a4fe9c86

Map a physical address into the permanent kernel physmap window for direct CPU access.

**Parameters**

- `phys` — physical base.
- `sz` — range size (the whole `[phys, phys+sz)` must fit in one window).
- `flags` — only `vmem_flags_cachewriteback` and `vmem_flags_uncached` are
  honored.

**Returns** a kernel virtual address into the appropriate permanent map:
write-back addresses below 2 GiB resolve into the kernel-top window
(`KERN_TOP_BASE`), other write-back addresses into `KERN_PHYSMAP_BASE`, and
uncached requests into `KERN_PHYSMAP_BASE_UC`.

**Caveat.** This is **not** an allocation — it returns the fixed offset into the
boot-time physmap, so it needs no unmap and is the standard way to touch a
physical frame (e.g. a freshly allocated page table) without `vmem_map`. If the
range falls outside the mapped physmap (`phys_map_sz`, 256 GiB) or an
unsupported cache flag is passed, the kernel **panics** ("Invalid Address
Detected!"). It is used pervasively inside the vmem walker itself.

---

## `vmem_vmalloc`

- **kind:** function
- **lang:** c
- **source:** `modules/SysVirtualMemory/src/platform/x86_64/pc/vmem.c`
- **hash:** c5a81f819d60c368

Reserve `sz` bytes of kernel *virtual* address space from a bump allocator.

**Parameters** — `sz`: size in bytes. **Returns** the reserved virtual base.

**Semantics.** A trivial monotonic bump allocator over the dedicated kernel
vmalloc region (above the uncached physmap), serialized by its own spinlock. It
hands out **virtual addresses only** — no physical backing is mapped; the caller
must `vmem_map` frames into the returned range. The region's PML4 entry is
pre-created in `vmem_init` so allocations stay coherent across address spaces.

---

## `vmem_vfree`

- **kind:** function
- **lang:** c
- **source:** `modules/SysVirtualMemory/src/platform/x86_64/pc/vmem.c`
- **hash:** 6595247be17a4d70

Return a `vmem_vmalloc` reservation, but only if it was the most recent one.

**Parameters** — `virt`, `sz`. **Returns** nothing.

**Semantics.** Because the backing allocator is a pure bump pointer, this only
rewinds the pointer when `virt + sz` equals the current high-water mark
(LIFO/most-recent allocation); any other free is silently dropped (the space
leaks). Serialized by the same vmalloc spinlock.

---

## `vmem_smp_init`

- **kind:** function
- **lang:** c
- **source:** `modules/SysVirtualMemory/src/platform/x86_64/pc/vmem.c`
- **hash:** 1cce7b3ec253762b

Set up the cross-core TLB-shootdown IPI machinery.

**Returns** `CS_OK`; `CS_UNKN` if a required SysInterrupts symbol is missing,
`CS_OUTOFMEM` if no interrupt vector can be allocated.

**Semantics & ordering.** Resolves SysInterrupts entry points at runtime (it
loads after SysVirtualMemory, so they are not link-time symbols), enumerates
every core's APIC id from the registry (`HW/LAPIC/<idx>`, populated from the
ACPI MADT), allocates an exclusive interrupt vector, and registers the
shootdown handler once (the IDT/handler table is global, covering all cores).
**Call exactly once, after `mp_init` (SysInterrupts/SysMP up) but before APs
start scheduling** — i.e. `CALL:vmem_smp_init` after `CALL:mp_init` in
`loadscript.txt`. Until this runs, `vmem_shootdown` is a no-op and only the
local-core flush in `vmem_unmap` is in effect.

---

## `vmem_active_apic`

- **kind:** function
- **lang:** c
- **source:** `modules/SysVirtualMemory/src/platform/x86_64/pc/vmem.c`
- **hash:** e6d53997acda7c95

Return the APIC id of the core a user address space is currently active on, or -1.

**Parameters** — `vm` (NULL returns -1).

**Returns** the atomically-loaded `active_apic`.

**Semantics.** Used to target a user-range TLB shootdown at the single core that
could have the translation cached (the one-task-per-AS / no-migration model
guarantees at most one). Snapshot this **under the same lock that performed the
`vmem_unmap`**, then pass the value to `vmem_shootdown` after dropping locks —
do **not** dereference the `vmem_t` during the IPI wait, because the owning task
may exit and free it.

---

## `vmem_shootdown`

- **kind:** function
- **lang:** c
- **source:** `modules/SysVirtualMemory/src/platform/x86_64/pc/vmem.c`
- **hash:** 82084e00e70715e3

Complete a cross-core TLB invalidation for a range just unmapped/downgraded by `vmem_unmap`.

**Parameters**

- `virt` — the range base (sign selects kernel vs. user, as elsewhere).
- `size` — range size.
- `target_apic` — for a **user** range, the core to invalidate (from
  `vmem_active_apic`); ignored for kernel ranges.

**Returns** nothing.

**Semantics & contract.** No-op on a single core or before `vmem_smp_init`
(`tlb_vec < 0`). For a **kernel** range (`virt < 0`) it broadcasts to every
*other* core; for a **user** range it shoots down only `target_apic`, and only
when that is a different core. Under `tlb_lock` it publishes the range, then
sends the IPI to one target at a time and spins on a per-target ack (which also
avoids overrunning the local APIC ICR). **Must be called with interrupts
enabled and holding no page-table or task lock** — the receiving cores run the
handler in interrupt context and the sender busy-waits for their acks, so
holding a lock here can deadlock. The local core was already flushed by
`vmem_unmap`, so it is never shot down itself.

---

## `stack_alloc`

- **kind:** function
- **lang:** c
- **source:** `modules/SysMemory/src/main.c`
- **hash:** caea5ad4f90574c5

Intended helper to allocate a stack region; currently a stub.

**Parameters** — `size_t` (requested size) and `bool` (kernel vs. user). Both
are presently ignored.

**Returns** `NULL`.

**Semantics.** `SysMemory` is the higher-level memory shim, but `stack_alloc` is
not yet implemented — it discards its arguments and returns `NULL`. Documented
here as the module's declared public surface; do not rely on it until it is
filled in (use `vmem_vmalloc` + `vmem_map` over `physmem_alloc` to build a stack
in the meantime).
