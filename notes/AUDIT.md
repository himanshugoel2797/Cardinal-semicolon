# Cardinal; code audit (revival pass)

A survey of bugs, smells, and incomplete code captured while reviving the build
on a modern toolchain (clang/lld 20, CMake 4.x). It is a roadmap for follow-up
foundation work, not an exhaustive review. Findings are tagged:

- **[VERIFIED]** — read the code, confident it is a real bug.
- **[LIKELY]** — strong smell, worth a closer look / runtime check.
- **[INCOMPLETE]** — intentional stub / unfinished feature, listed so it is tracked.

Line numbers are as of the revival branch; they drift as fixes land.

---

## Display stack (intel_gfx + CoreDisplay) — primary focus

### [VERIFIED] EDID monitor-name parse copies one byte 13×
`servers/CoreDisplay/src/edid.c:103`
```c
for (int n_i = 0; n_i < 13; n_i++)
    result->display_name[n_i] = raw[54 + i + 5];   // missing + n_i
```
Every slot of `display_name` gets the same source byte. Should be
`raw[54 + i + 5 + n_i]`. Also: `display_name` is not guaranteed NUL-terminated
and the descriptor name field is space/`0x0A`-padded per the EDID spec — worth
trimming. *(Fixed in the correctness-fixes PR.)*

### [LIKELY] GMBUS wait has no timeout (can hang forever)
`drivers/intel_gfx/src/gmbus.c:38`
```c
void igfx_gmbus_wait(igfx_dev_state_t *driver) {
    while (!(igfx_read32(driver, ... IGFX_GMBUS2) & (1 << 11))) ;
}
```
A dead/absent panel or a GMBUS error wedges the driver. Needs an iteration/time
bound and an error-bit (GMBUS2 bit 10) check. Same pattern in the EDID read path.
*(Fixed: `igfx_gmbus_wait` now bounds the spin and checks the error bit,
returning failure; `igfx_gmbus_read` aborts the transaction on timeout/error.)*

### [INCOMPLETE] Haswell support is essentially absent
- `drivers/intel_gfx/inc/hsw-regs.h` is empty (header guards only).
- `drivers/intel_gfx/src/main.c:73` sets `display_mmio_base`/`gtt_base` for
  Cherrytrail only; Haswell leaves them zeroed.
- `igfx_display_init` / `_isconnected` / `_enable_port` / `_pipe_setup`
  (`drivers/intel_gfx/src/display.c`) are all Cherrytrail-only; Haswell uses the
  DDI/transcoder model and has no code path.
- GTT functions (`drivers/intel_gfx/inc/gtt.h`) are declared but unimplemented.
- The driver never calls `display_register()` (cf. the working
  `drivers/virtio/gpu/src/main.c:432`), so even Cherrytrail never reaches
  CoreDisplay.
- Plane setup in `display.c` (DSPCADDR/DSPCSURF/DSPCSTRIDE) is commented out.

Suggested first milestone for Haswell bring-up: populate `hsw-regs.h`, set the
Haswell MMIO/GTT bases, implement GTT alloc + a `display_desc_t` registration so
the LFB-equivalent path works, *then* DDI/DPLL/pipe mode-set.

### [INCOMPLETE] CoreDisplay server
`servers/CoreDisplay/src/main.c` — registration/deregistration and LFB fallback
work; `set_resolution`/`set_brightness`/`set_state` mode-switching is not
implemented (drivers expose the callbacks but the server never drives them).

---

## Kernel core + system modules

### [VERIFIED] registry_readkey_ptr uses the wrong KVS getter
`modules/SysReg/src/main.c:375` — `registry_readkey_ptr(..., uintptr_t *val)`
calls `kvs_get_uint(key_kvs, val)`. `kvs_get_uint` asserts the entry type is
`kvs_val_uint`, but this path has already confirmed the entry is `kvs_val_ptr`,
so the call always fails and the function never returns a pointer value. Should
call `kvs_get_ptr`. *(Fixed in the correctness-fixes PR.)*

### [LIKELY] Bitwise `|` used as logical `||` in branch conditions
`kernel/src/elf.c:175,272,289,341`, `kernel/src/initrd.c:47`, e.g.
```c
if ((shdr[i].sh_type == SHT_SYMTAB) | (shdr[i].sh_type == SHT_STRTAB))
```
Operands are side-effect-free comparisons so the result is currently correct,
but it defeats short-circuiting and reads as a typo. Low risk; cleanup.
*(Fixed: switched to `||` at all listed sites.)*

### [LIKELY] pagealloc OOM sentinel is `-1` cast to `uintptr_t`
`modules/SysPhysicalMemory/src/page_allocator.c:224` returns `-1`; callers such
as `modules/SysVirtualMemory/.../vmem.c:127` use the result without checking,
so an allocation failure becomes a write to `0xFFFF…FFFF`. Define an explicit
error sentinel and check it. *(Fixed: `phys_mem.h` now defines
`PHYSMEM_NO_ALLOC`; `pagealloc_alloc` returns it consistently on all failure
paths (the mid-scan `PANIC` is gone). Callers handle it in tiers — critical
infra (vmem page tables, SysMemory heap, SysTaskMgr stack/image) PANICs with a
clear message; device drivers log and fail init/probe gracefully. Note: vmem's
old check tested `== 0`, which both missed the real sentinel and would have
fired on a legitimate page at address 0 — now fixed to `== PHYSMEM_NO_ALLOC`.
The debug-only `pmem_initial_test` is left as-is since it only prints the
result.)*

On the failure path itself there is nothing to unwind: the scan dequeues one
free-list entry at a time and always returns it or re-inserts it before the next
iteration, so the free list is whole at every iteration boundary. The bail
(`page_allocator.c:182`) triggers on a *failed* `queue_trydequeue` (empty queue,
nothing removed), so `btm_level` and `free_mem` are left exactly as they were —
no partial allocation, no leaked pages.

### [LIKELY] pagealloc re-insert failures are unchecked (page leak vector)
`modules/SysPhysicalMemory/src/page_allocator.c:195,204,219` — when the scan
puts a dequeued block back (wrong zone, too small, or the leftover after a
split), it ignores the `insert_queue*` return value. If the queue is full the
entry is silently dropped, leaking those pages and shrinking the free list (a
later `queue_trydequeue` then fails). Independent of the OOM-sentinel work; the
allocation logic itself otherwise conserves pages. Should check the return and
size the queue so re-inserts cannot fail.

### [LIKELY] Unsynchronised bump allocator on SMP
`modules/SysVirtualMemory/src/platform/x86_64/pc/vmem.c:76` — `vmem_vmalloc`
advances `kernel_vmalloc` with no lock; the code's own TODO notes it is not
atomic to preemption. Race on concurrent callers. *(Fixed: a dedicated
`kernel_vmalloc_lock` (`local_spinlock`) now guards both `vmem_vmalloc` and
`vmem_vfree`, so the bump pointer is updated atomically across cores. `vmem_vfree`
keeps its LIFO-only release behaviour — only the most recent allocation can be
returned — which is inherent to a bump allocator, not a bug.)*

### [INCOMPLETE] Stubs / TODOs (tracked, not bugs)
- `kernel/src/bootstrap_alloc.c:101` `realloc` → `PANIC("unimplemented")`.
- `common/src/time.c` `gmtime` partial, `strftime` is a no-op.
- TLB shootdown missing (`vmem.c:260`), SMP timer/IPI TODOs
  (`SysTimer/src/main.c:39`, `SysInterrupts/.../apic.c:125`).

---

## Servers + other drivers

### [VERIFIED] CorePower passes a NULL output pointer to the queue
`servers/CorePower/src/main.c:36` and `:52`
```c
pwr_device_t *device = NULL;
if (queue_trydequeue(&devices, (uint64_t*)device)) { ... }
```
`queue_trydequeue` writes the dequeued value to `*val`; passing the NULL value
of `device` dereferences NULL. Must be `(uint64_t*)&device`. *(Fixed in the
correctness-fixes PR.)*

### [LIKELY] USB enumeration / type field uninitialised
`servers/CoreUsb/src/main.c:105` — `def->idx = devIDs[def->type]++` with
`def->type` never set from the incoming descriptor; indexes `devIDs` with
garbage. Recent EHCI/UHCI/CoreUsb work is mid-flight here.

### [LIKELY] Unbounded hardware busy-waits (no timeout)
- `drivers/ahci/src/ahci.c:236` spins on `PxCI` while spamming `DEBUG_PRINT`.
- `drivers/rtl8169/src/driver.c:86`, `drivers/rtl8139/src/driver.c:56` spin on
  descriptor ownership with no timeout (8169 also drops/retakes the lock around
  the spin, racing the DMA engine).

### [INCOMPLETE] Stubs
`CoreAudio`, `CoreStorage`, `tarfs` module_init are empty; `intel_wifi`
`module_init` early-returns before any init; `CoreNetwork` ARP/IP/TCP paths are
TODO. Matches the README status notes.

---

## Toolchain / build notes (addressed in the revival PRs)

- `cmake_minimum_required(VERSION 2.8)` is a hard error on CMake 4.x — bumped to 3.20.
- clang rejects C bodies in `naked` functions and cannot lower `memcpy` through
  `__seg_gs` (address space 256) pointers — both worked around in
  `modules/SysUser/src/platform/x86_64/syscall.c`.
- clang drives lld to a PIE for `x86_64-elf` and ignores `-no-pie`; `-static`
  is required so the non-PIC absolute relocations link.
- Many drivers access MMIO via `volatile` structs with bitfields; clang's
  `-Wvolatile`/codegen around bitfield volatiles is stricter than GCC's — worth
  migrating hot register paths to explicit `read32`/`write32` helpers over time.
