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

> **NOTE (USB→Lisp cleanup):** the C `intel_gfx` driver was **removed** (orphaned
> — never bound by any boot path; no Lisp port exists yet). The findings below
> are historical; `CoreDisplay` remains. Lisp display drivers today are
> `lisp/drivers/virtio-gpu` and `lfb`.

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

### 2D graphics API (added)
`lisp/lib/graphics.clp` + `lisp/lib/font.clp` are a UI-oriented 2D drawing library
over a framebuffer `surface`: solid/outline rects, h/v/Bresenham lines, midpoint +
filled circles, opaque + alpha image blits, and bitmap-font text (default 8×16 font
`lisp/data/font8x16.bin`, from `scripts/gen-font.py`). The per-pixel-heavy ops are
four ambient C primitives (`gfx-fill-rect!`/`gfx-blit!`/`gfx-blend!`/`gfx-glyph!`,
in `prims.c`, bounds-checked overflow-safe) — the interpreted layer is the usual
200-650× per-pixel trap. A surface carries a **backend** dispatch alist so a HW-2D
driver can override fill/blit/etc., falling back to software per-op (the
override seam the user asked for). Validated by 61 host pixel-assertions
(`test_graphics.c`) and a `cardinal.gfxdemo`-gated in-OS demo (the `gfxdemo-image`
ISO; `run-qemu.sh SCREENSHOT=`).

Two gotchas a real UI must handle (the demo works around both):
- **A framebuffer can't be passed by message** — `send` copies `bytes` (copy-on-
  send), so a `get-framebuffer` reply hands the consumer a COPY whose writes never
  reach the scanout. The drawing context must `mmio-map` the framebuffer itself (the
  demo maps `HW/BOOTINFO/FRAMEBUFFER` directly). lfb gained a `paint?` flag to skip
  its bring-up gradient when a UI is taking over.
- **SysDebug shares the LFB.** `sysdebug_install_lfb` mirrors the COM1 debug log to
  the same framebuffer (`render_char`), so any `(display ...)` overwrites UI pixels.
  There is no hook yet to hand the framebuffer to a UI / silence the LFB console —
  a real compositor will want one. The demo repaints a few times to cover straggler
  debug text. Also: a 24bpp boot mode mis-strides (lib + lfb assume 4-byte pixels);
  the demo forces `gfxmode=...x32`.

### TrueType text (stb_truetype) (added)
`libs/ttf/` wraps **stb_truetype** (`libs/stb/stb_truetype.h`, public domain) as a
runtime glyph rasterizer; `lisp/lib/ttf.clp` adds antialiased text over graphics.clp
with a hash-table glyph cache, and `lisp/data/DejaVuSans-subset.ttf` (DejaVu Sans,
Latin subset, ~26 KiB, regen `scripts/gen-ttf.py`) is the default font. New prims:
`ttf-rasterize`/`ttf-vmetrics` (`sys-ttf`, into libs/ttf) and the ambient `gfx-cover!`
(8-bit coverage → solid colour, alpha-composited). Validated by a C host test
(`libs/ttf/test_ttf.c`), the `gfx-cover!` host pixel-asserts, and the in-OS demo
(antialiased text at 14–40 px, over the alpha panel).

Hard-won notes for FP-in-a-kernel-module (the rasterizer is float-based; SSE is on,
per SysFP):
- `libs/ttf` re-enables SSE like libs/lisp (`-msse2 -mstackrealign -fno-vectorize`)
  and **must stay scalar** until the loader aligns section data — see the
  `[INCOMPLETE]` loader-alignment item below. Only integer/pointer values cross the
  ttf ABI, so the `-mno-sse` callers that link it are unaffected.
- stb's many small per-glyph `malloc`/`free` calls **hang the kernel heap** when
  driven from the Lisp-eval context; ttf gives stb a per-call **bump arena** instead
  (1 MiB static; the prim serialises it).
- `sqrt` is inline `sqrtsd` asm, NOT `__builtin_sqrt`: at `-O0` the builtin lowers to
  a *call* to `sqrt`, and the freestanding `sqrt` we define would then recurse.

### [FIXED] Relocatable-module loader now aligns section data (vectorization enabled)
The loader lands a module at a **64-aligned base** (`load_script.c`, `MOD_ALIGN`) and
now allocates **NOBITS (`.bss`) at `sh_addralign`** (`elf.c`); PROGBITS run in place,
and the linker already aligns each section's file offset to `sh_addralign`, so a
64-aligned base makes `hdr + sh_offset` aligned (asserted in `elf.c` so a toolchain
change fails loudly, not as a stray `#GP`). With that, `libs/lisp` (the flonum
runtime + whole VM) and `libs/ttf` (the rasterizer) build **with auto-vectorization**
(the `-fno-vectorize` workaround is gone) — the aligned `.rodata.cst16` packed
constant loads (`movapd`) are now safe; vectorized loops over runtime heap pointers
already used unaligned moves. Kept at **SSE2 width, not AVX**: AVX *state* is enabled
(SysFP `xsetbv` sets XCR0 to the CPU's XSAVE components; `xsave`/`xrstor` preserve
YMM), but some targets (Cherry Trail / Airmont, e.g. the AtomicPi) have no AVX, so
`-mavx` would `#UD` there — it can be an opt-in for AVX-only deployments. Validated
Debug + Release (in-OS 45/0; the TrueType demo renders).

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

### [FIXED] pagealloc re-insert failures are unchecked (page leak vector)
`modules/SysPhysicalMemory/src/page_allocator.c` — the scan's re-inserts no
longer silently drop entries: `insert_queue`/`insert_queue_front` now
`compact_queue()` and retry on a full queue, and `PANIC` if the entry still does
not fit. So a re-insert either succeeds or halts loudly; it cannot leak pages or
shrink the free list. (The original [LIKELY] finding described the pre-fix state.)

### [LIKELY] Unsynchronised bump allocator on SMP
`modules/SysVirtualMemory/src/platform/x86_64/pc/vmem.c:76` — `vmem_vmalloc`
advances `kernel_vmalloc` with no lock; the code's own TODO notes it is not
atomic to preemption. Race on concurrent callers. *(Fixed: a dedicated
`kernel_vmalloc_lock` (`local_spinlock`) now guards both `vmem_vmalloc` and
`vmem_vfree`, so the bump pointer is updated atomically across cores. `vmem_vfree`
keeps its LIFO-only release behaviour — only the most recent allocation can be
returned — which is inherent to a bump allocator, not a bug.)*

### [FIXED] Unsynchronised physical page allocator on SMP
`modules/SysPhysicalMemory/src/page_allocator.c` — `physmem_alloc`/`physmem_free`
mutated the free list (`btm_level`, a plain non-atomic ring buffer) with **no
lock**, yet are called concurrently from every core (vmem page-table pages,
`SysMemory` heap growth, `dma-alloc` device buffers). Two cores racing on the queue
can corrupt it and hand out **overlapping physical pages**, after which one
allocation's writes stomp another's live data. *(Fixed: a leaf `physmem_lock`
(`local_spinlock`, taken after `cli()` like the bootstrap/SysMemory allocators)
serialises both entry points; it nests inside vmem's `kmem.lock` and SysMemory's
`alloc_lock` and takes no other lock, so there is no ordering hazard. Single-core
behaviour unchanged.)*

### [FIXED] AP/BSP kernel stack too small for the resident Lisp runtime — *the* intermittent SMP boot fault
`kernel/src/platform/x86_64/pc/main.c` `alloc_ap_stack` gave each application
processor a **16 KiB** stack (`malloc(4096*4)`); `boot.S` reserves the BSP the same
16 KiB ("enough until threading is setup"). That sufficed for early single-threaded
bring-up, but each core now runs the **whole resident Lisp runtime** on that stack —
the compiler, the bytecode VM, the GC, and nested interrupt handlers. A deep compile
interrupted by the periodic APIC timer tick overflows 16 KiB, and because the AP
stack is `malloc`'d in the shared kernel heap arena the overflow **silently corrupts
whatever module data sits just below it** — e.g. SysTimer's `apic_state` (a NOBITS
`.lbss` global `elf_load` places in that arena), overwritten with a stack-resident
heap pointer. The APIC timer handler then reads `apic_state` as a per-core TLS offset
via `%gs` and faults on the garbage value. Different overflow victims → different
faulting rips, so it read as a mysterious **~10%-of-boots `#GP`/`#PF`** during
multi-core bring-up; `SMP=1` never hit it (one stack, and its overflow lands in
kernel `.bss` slack, not `malloc`'d module data).

> NB: this is exactly the "stack overflow" the older SysTaskMgr-era AP-fault
> investigation below **ruled out** for *that* fault — 16 KiB was fine for the native
> task scheduler, but not for the Lisp evaluator that replaced it.

*(Fixed: both stacks enlarged **16 KiB → 256 KiB** — `AP_STACK_SIZE` in
`alloc_ap_stack`; the `.bootstrap_stack` `.skip` in `boot.S` (`@nobits`, so no image
cost). 80+ consecutive `SMP=2` boots clean (was ~1 in 10 faulting); `SMP=1` clean.
Root-caused with a new panic-time **stack backtrace + active/kernel GS-base dump**
added to `dump_trap_frame` in `modules/SysInterrupts/src/platform/x86_64/pc/idt.c` —
worth keeping for future SMP crash triage.)*

### [FIXED] SMP application-processor (AP) bring-up — race resolved, APs active by default
> **SUPERSEDED BY DESIGN CHANGE (per-core run queues).** The whole class of
> cross-core scheduler races below was eliminated structurally rather than patched:
> the scheduler is now **shared-nothing**. Each core owns a private run queue
> (`run_queues[]`/`rq_locks[]`, indexed by a sequential `core_idx`); a task is
> created on, scheduled by, and **freed only by** its single owning core. The
> cross-core use-after-free that drove this entire investigation is therefore
> *impossible by construction* — no other core can free a task's stack/reg_state
> while its owner is mid-`iret` on it, because no other core can reach that task.
>
> What that removed: the global `processes` list + `process_lock` (the hot path now
> takes only this core's `rq_lock`, ≈uncontended); the separate `task_cleanup` task
> (each core frees its own departed task at the top of its next scheduler pass via
> `core_desc_t.prev_dead`); the `task_state_reapable` state and the `last_dead`
> deferred-reap *marking* dance; and the cross-core cleanup that the deferred reap
> existed to make safe. The one remaining subtlety — not freeing a task on the same
> pass we switch away from it (we're still on its stack until `iret`) — is now a
> trivially-correct *same-core* one-pass deferral (`prev_dead`).
>
> Distribution: tasks created after the APs come online round-robin across cores
> (`pick_target_core`); boot-time tasks (servicescript, per-core idle) stay on their
> creating core. Cross-core task migration is not yet implemented (a follow-up: an
> explicit "move descriptor between queues" message — the microkernel way).
> Validated: 6/6 clean TCG SMP boots, APs active, servicescript exit/reap (the old
> repro) faultless. The historical narrative below documents the original diagnosis
> and the timing-based fix this design replaced; it is kept for context.
>
> **Prior fix (now replaced).** The timing-sensitive SMP race was first fixed with a
> deferred one-quantum reap (see "ROOT CAUSE FOUND & FIXED" below). `CALL:task_release_aps`
> is in `servicescript.txt`, so the default boot is multi-core. The historical
> narrative below is kept because it documents the diagnosis and the supporting
> infrastructure (IST exception dumps) the work built on.
>
> The two **diagnostic** aids used to localise the bug were removed once it was fixed:
> the per-task `reg_state` **page-guard** (it never recycled its vmalloc virtual range,
> leaking address space per task — `reg_state` is back on the heap, which the
> deferred reap makes safe) and the `owner_core` **ownership tripwire** (its hypothesis
> was disproven; it added a panic in the scheduler hot path). Residual caveat: under a
> *synthetic* sustained-preemption stress (a kernel task spinning ~5e7 iterations without
> yielding while APs churn) a single #GP with a garbage selector was seen once — i.e. a
> rare reg_state corruption may still exist outside the exit/reap window. It did not recur
> in normal boots; chasing it wants an uncontended host (TCG-only repro) and likely the
> page-guard temporarily re-enabled to name the writer. Tracked, not yet root-caused.

APs are brought up (TLS/vmem/interrupts/timer per `apscript.txt`) and the machinery to
release them into the scheduler is fully in place. The mechanism is *single-threaded
boot, then release*: the kernel module loader (`elf_load`/`elf_resolvefunction`/symbol
DB/`bootstrap_alloc`) is single-threaded-only, so the APs stay parked in
`mp_signalready()` (`SysMP/.../mp.c`) for the entire load; a `CALL:task_release_aps`
line in `servicescript.txt` (just before `CALL:end_task_syscall`) then calls
`mp_set_ap_entry(task_ap_entry)`, and each AP runs `task_ap_entry`: per-core setup
(`task_core_setup`/`task_core_arm`, interrupt stack + a per-core idle task) and joins
scheduling. The scheduler entry is split into per-core and one-time global steps.

**Historically parked by default** (pre-redesign state described by this narrative):
the `CALL:task_release_aps` line had been *removed* from `servicescript.txt`, making
that boot single-core. It is now present in `servicescript.txt` (see the superseding
note above), so the shipped boot is **multi-core** by default. Under **KVM** both
cores schedule independently (verified: two distinct per-core GS bases take timer
interrupts, no faults across many runs).

**The blocker to trusting it on by default is a timing-sensitive SMP race** that, under
**TCG only**, intermittently corrupts a task structure on the heap. Two distinct
manifestations were captured (both legible only thanks to the IST work below; before
it they triple-faulted silently):
- AP returns from its timer interrupt into a task whose saved `reg_state.rip` is a
  constant `0xb8`;
- the scheduler dereferences a `cur_task`/`next` task pointer whose **high 32 bits are
  clobbered** (`0x00000110` instead of `0xffffffff`; the low 32 are intact) — a
  classic allocator-metadata-over-live-pointer signature. The constant small values
  (`0xb8` = 184, `0x110` = 272) are consistent with a freed chunk's `len` field landing
  on a still-referenced task field, i.e. a use-after-free / double-use of a task buffer.

One real UAF here was found and fixed but is *not* the whole story:
- **task_cleanup could free an exited task while a core still held it as `cur_task`**
  (`SysTaskMgr/src/task.c`). On exit (`end_task_kernel`) a task is marked
  `task_state_exited` and releases `process_lock`; in the gap before it re-yields,
  `task_cleanup` on another core could free it, leaving the owning core's `cur_task`
  dangling and corrupting reused memory. Fix: a **two-phase reap** — a new
  `task_state_reapable` is set by the *owning* core's scheduler once it has switched
  away from an exited task, and `task_cleanup` only frees `reapable` tasks. This is
  correct and necessary and removed the common case, but the corruption still
  reproduces, so at least one more freed-while-referenced path remains.

**This race is a heisenbug.** A stress "churn" task (spawn+exit tasks continuously)
reproduces it ~1/8 under TCG. But *any* validation probe added to the scheduler hot
path (e.g. checking each task pointer / saved RIP before use) shifts timing enough to
hide it entirely (0/10) — so the usual "add a check to catch it" approach is a dead
end; detection must be passive (fault-on-access), not active polling.

Ruled out so far:
- **Stack overflow** — a canary at the bottom of the AP's 16 KiB bootstrap stack
  (`alloc_ap_stack`) stayed intact across runs where the `#PF` still fired, so the AP
  is not overflowing its bootstrap stack into adjacent heap. (Both bootstrap stacks are
  16 KiB; not the cause here.) **[Later footnote]** correct *for this native-task
  fault*, but the 16 KiB stack DID later prove too small once the resident **Lisp**
  evaluator (compiler/VM/GC + interrupt nesting) ran on it — a separate intermittent
  SMP boot fault, now fixed by enlarging both stacks to 256 KiB (see "[FIXED] AP/BSP
  kernel stack too small for the resident Lisp runtime" above).

Refined diagnosis (from the legible IST dumps): the restored task's saved registers
are **foreign context**, not the task's own — in one build `rax` = the APIC physmap
vaddr, `rdi` = `0xB0` (the `APIC_EOI` register offset), `rip` = `0xb8` (exactly the
values live inside `interrupt_sendeoi`); in another, `rsp`/`rbp` point into the AP
interrupt stack and `rip`/`rcx`/`rdx` hold heap/code pointers. So a task's `reg_state`
buffer is being **overwritten with interrupt/stack context** — its corrupt content
just tracks the build's allocation layout (the constant `0xb8` was a coincidence of
one layout, not a fixed writer).

A poison test pinned the buffer involved: in `task_cleanup`, *poisoning and leaking*
`reg_state` (instead of `free`-ing it, so it can't be reused) **changed** the fault —
which means **servicescript's freed-and-reused `reg_state` is implicated**. The
working theory: servicescript's `reg_state` is freed, the about-to-schedule AP reuses
that buffer for a new task's `reg_state`, and something still writes servicescript's
state into the old buffer — corrupting the new task. The two-phase reap closed the
`cur_task`-dangling path but a second freed-while-referenced `reg_state` path remains
(likely tied to the AP's *first* schedule racing servicescript's exit/reap).

**Page-guard landed — the heisenbug is now deterministic.** Each task's `reg_state`
is now allocated as its own dedicated kernel page from the `vmem_vmalloc` region
(page-allocator + `vmem_map`, zero-filled) and `vmem_unmap`'d on free, with the
virtual range deliberately *never* recycled (`regstate_guard_alloc`/`_free` in
`SysTaskMgr/src/task.c`). Surfacing this required fixing a latent bug it depended on:
**`vmem_unmap` was a complete no-op** — it began with `size = 0;`, so its
`while (size > 0)` loop never ran and no mapping was ever torn down (the user-stack and
descriptor unmaps in `task_cleanup` silently leaked). Removed that line; `vmem_destroy`
only frees the `vmem_t` struct (never walks page tables) so honoring the size cannot
double-free. A second prerequisite: each AP snapshots the kernel half of the page table
*once* in `vmem_mp_init` and never refreshes it, so a kernel mapping that creates a
**new** PML4 entry after that snapshot is invisible to a running AP. `vmem_init` now
pre-creates the vmalloc region's PML4 entry before any AP boots, so guard mappings stay
coherent across cores (verified: APs schedule on guarded `reg_state` with no
false-positive faults).

**What it caught (first post-release run, deterministically):** an instruction-fetch
`#PF` (`err=0x10`, `rip = cr2 = 0xa0`) on the **AP** (`apicid 0x1`), immediately after
`releasing APs`, in the same instant the BSP finished `end_task_syscall` and exited
`servicescript`. The restored task's saved context was foreign interrupt state:
`rax = 0xffff8080fee00000` (the UC APIC physmap base), `rdi = 0xb0` (`APIC_EOI`),
`rcx = 0x2c` (vector 44) — i.e. mid-`interrupt_sendeoi` register state — and crucially
`rbp = 0xffff810000004000`, **an address inside the `reg_state` guard region itself**.
So a task was scheduled with a `reg_state` whose contents had been overwritten by EOI
interrupt context (and a guard-region/`reg_state` pointer leaked into its saved `rbp`),
then `iret`'d into garbage. This **confirms `reg_state` is the corrupted object** and
ties the corruption precisely to *the AP's first schedule racing the BSP's exit/reap of
`servicescript`*.

Note the guard did **not** fault on the corrupting *write*: the overwritten
`reg_state` was still **live/mapped** (no fault reading it during restore), so the wild
write went into a *valid* buffer — this is a write into a live `reg_state`, not a
use-after-free of a freed one. To name the exact writer, the next passive technique is
to keep each `reg_state` page **read-only except during the explicit save window**
(flip RW around `mp_platform_getstate`), so any other write faults at the offending
instruction. Caveat: without cross-core TLB shootdown (`vmem_flush` only does local
`invlpg`; shootdown is an acknowledged TODO) a write from a core with the page cached
RW in its TLB may not trap — so this may need a shootdown first, or correlating the
faulting RIP from a disassembly of the scheduler save/restore + `idt_mainhandler` EOI
path. Heap red-zones in `SysMemory` would generalise the live-buffer case.

**ROOT CAUSE FOUND & FIXED — the scheduler ran on a stack it then let be freed.**
The corruptor was never a *write to the wrong reg_state*; it was the owning core
still **executing on an exited task's kernel stack** while another core freed that
stack out from under it. The periodic timer is vector ≥32, so its IDT entry uses
`ist=0` (`SysInterrupts/.../idt.c`) — i.e. `idt_mainhandler` and `task_switch_handler`
run **on the interrupted task's own kernel stack**, and the trap frame the final
`iret` consumes lives on that stack too. When the current task had exited, the
scheduler marked it `task_state_reapable` and dropped `process_lock` *before*
returning through the interrupt epilogue (`interrupt_sendeoi`) and `iret` — all of
which still execute on that task's stack. The instant `process_lock` was released,
`task_cleanup` on the other core was free to `free()` the stack / `regstate_guard_free`
the reg_state / `free` the `process_desc_t`. The reused memory was then scribbled by
whatever allocation grabbed it, which is exactly both captured signatures: the freed
stack's `iret` frame overwritten → `iret` into garbage; and a freed `reg_state`/stack
page reallocated to a *new* task and stomped while two cores briefly touched it (the
"EOI context in a live reg_state, `rbp` inside the guard region" capture — the guard
didn't fault because the page was still mapped, just reallocated). `task_yield`'s
restore path has the same hazard: it builds its `iret` frame on the outgoing task's
stack after `task_yield_stage2` returns. The two-phase reap closed the
`cur_task`-dangling sub-case but not this one, because "switched away" in the
scheduler is not complete until the `iret` retires.

**Fix (timing-independent — `SysTaskMgr/src/task.c`, `task_priv.h`):** *deferred,
one-quantum reap.* Added `core_desc_t.last_dead` (per core). When the scheduler
switches away from an exited task it now **stashes** it in `last_dead` (leaving it
`task_state_exited`, which is neither selectable nor reapable) instead of marking it
reapable immediately. `task_reap_deferred()`, called at the top of both scheduler
entry points (`task_switch_handler`, `task_yield_stage2`) under `process_lock`,
promotes the *previous* pass's `last_dead` to `task_state_reapable`. By the next
scheduler pass on that core the `iret` has retired and the core is demonstrably
running on a **different** task's stack, so `task_cleanup` can free the dead task's
stack/reg_state/struct with no live reference remaining. No hot-path validation probe
(which the AUDIT notes perturbs the timing and hides the race) — the change is purely
structural. Every core always idles + takes the periodic tick, so the one-quantum
hold drains promptly. The two-phase reap and the `vmem_unmap`/PML4 prerequisites above
remain and compose with this; the `reg_state` page-guard was a diagnostic and has since
been reverted (see the banner at the top of this section).

*Reproduction & validation:* the pre-fix `#PF` reproduces under **TCG** with the full
`servicescript` (`cr2 = 0xa0`, a freed-chunk metadata value, dereferenced in the
scheduler's `vmem_setactive(ntask->mem)` path, on the BSP the instant `servicescript`
exits right after `releasing APs`). The race is sensitive to the *full* boot timing —
a stripped servicescript that exits immediately does **not** reproduce it (17/17 clean
both ways), so it must be tested with the real boot. Post-fix, every boot that reached
the AP-release window survived with no fault. A clean large-N statistical A/B on this
dev box is throttled by intermittent host CPU starvation (the guest is TCG-bound; see
the build-env notes) and should be re-run once the host is uncontended.

Two pieces of hardening landed to make such faults debuggable (previously a fault
during AP bring-up cascaded silently into a triple-fault reboot):
- **Full CPU-exception dumps** (`SysInterrupts/.../idt.c`) — any vector <32 now prints
  the named exception, error code, CR2 (for `#PF`), the whole register frame, and the
  faulting core's APIC id (read via CPUID so it works even when per-core TLS is bad),
  then panics. `intr.c`'s dedicated `pagefault_handler` was removed so `#PF` flows
  through this unified path. NULL guards on `idt`/`reg_state`/`reg_ref` in the dispatch
  and register-state accessors turn an uninitialised-per-core-state fault into a
  legible panic instead of a NULL deref.
- **IST fault stacks** (`SysInterrupts/.../gdt.c` + `idt.c`) — the per-core TSS now
  carries two dedicated fault stacks; the IDT routes `#DF` to IST2 and every other
  architectural exception (0..31) to IST1, so a fault whose own stack is corrupt or
  exhausted still runs its handler on a known-good stack and can print, rather than
  triple-faulting. Device IRQs (>=32) keep the running task's kernel stack.

Fixed along the way (all valid on single-core too):
- **AHCI `module_init` hung the boot forever via `task_sleep` under `cli()`**
  (`drivers/ahci/src/ahci.c` `ahci_obtainownership`) — it called `task_sleep` for the
  BIOS/OS handoff delay while the driver held `cli()` across init. `task_sleep`
  (`SysTaskMgr/src/task.c`) only marks the task `task_state_sleep` and relies on the
  preemption timer to wake it, which can never fire with interrupts off, so the
  servicescript task was flagged sleeping and never resumed. Because AHCI is the last
  service loaded, the boot *looked* complete (it stalled right at AHCI's last print) —
  in fact `coredisplay_postinit`/`end_task_syscall` had never run. Replaced all three
  `task_sleep` calls in AHCI init (`ahci_obtainownership` ×2, `ahci_initializeport`
  ×1) with bounded polled spins (appropriate for an interrupts-off init); also wrote
  back the cleared `PxCMD` bits in `ahci_initializeport`, which the original code
  computed but never stored. NB: `task_sleep` itself is still broken for the general
  case (see below).
- **`malloc`/`free` were not SMP-safe** (`SysMemory/src/allocator.c`) — used only
  `cli()` (local-core) to guard a shared free list. Now also serialised by an
  `alloc_lock` spinlock taken after `cli()`.
- **`kmem.lock`/`vm->lock` were acquired with interrupts enabled**
  (`SysVirtualMemory/.../vmem.c`) — the preemption-timer ISR also takes these via
  `vmem_setactive`, so a task preempted while holding one self-deadlocked (latent
  even single-core; reliable hang on SMP). All runtime acquirers now `cli()` across
  the critical section.
- **Per-core TLS was not zeroed** (`SysMP/.../mp.c` `mp_tls_setup`) — `static TLS`
  pointers (`apic_state`, `lcl`, `core_descs`, `idt`) are written assuming
  zero-init and gate per-core setup on a NULL check; an AP saw garbage and skipped
  its own init. Now `memset(0)` on allocation.
- **Local APIC timer registration was per-core and exclusive**
  (`SysTimer/.../apic.c`, `SysTimer/src/main.c`) — registered once per core into a
  fixed-size global table and marked globally `in_use`, so a second core could not
  request "the local timer". Now registered once, with per-core TLS state, and
  `timer_request` treats `timer_features_local` timers as non-exclusive.
- **`task_cleanup` busy-spun `process_lock`** (`SysTaskMgr/src/task.c`) — an
  infinite non-yielding loop that relied on preemption; on SMP it starved other
  cores of the scheduler. Now `task_yield()`s each pass.

### [FIXED] task_sleep does not actually deschedule
`modules/SysTaskMgr/src/task.c` `task_sleep` — the control flow was inverted: the
found-task branch set `state = task_state_sleep` and `sleep_end` then **returned
without yielding** (so the "sleeping" task kept running, merely mislabelled), while
the *not-found* branch called `task_yield()` (descheduling whatever unrelated task
happened to be running). The AHCI hang above was a symptom. Fixed: the not-found
branch now just returns; the found branch yields **only when it put this core's own
current task to sleep** (`iter == core_descs->cur_task`, captured under `cli()`),
so a self-sleep actually switches away while sleeping another task only marks it.
Wake path verified sound — `task_runnable` returns true once
`timer_timestamp_ns() >= sleep_end`, and `task_yield`/`task_switch_handler` only
reset `running → pending` so the `sleep` state survives the switch. Validated with a
temporary self-test (`task_sleep(self, 100ms)` measured 100ms elapsed under
`-smp 1/2`; previously ~0). Callers still holding `cli()` across init must NOT use it
(it yields, which is illegal under `cli()`); those paths (AHCI init bounded polled
spins, **UHCI init** `uhci_delay_ns` wall-clock busy-wait) remain on busy-waits by
design.

### [INCOMPLETE] Stubs / TODOs (tracked, not bugs)
- ~~`kernel/src/bootstrap_alloc.c` `realloc` → `PANIC("unimplemented")`.~~
  *(Fixed: bootstrap `realloc` implemented via the size-prefix header
  (alloc/copy/free, no-op on shrink). Added a `realloc_hndl` that
  `kernel_updatememhandlers` resolves from the loaded heap allocator, mirroring
  `malloc_hndl`/`free_hndl`; if the real heap is installed but exports no
  `realloc`, the bootstrap one refuses rather than reading foreign metadata.
  SysMemory now exports `realloc` (`SysMemory/src/allocator.c`): `malloc`/`free`
  were factored into lock-free `malloc_unlocked`/`free_unlocked` inners, and
  `realloc` takes the `cli()`+`alloc_lock` pair once before calling them
  (NULL→malloc, 0→free, in-place when the node's `len` already covers the rounded
  request, else alloc/copy/free with the old block preserved on OOM). So
  `kernel_updatememhandlers` now resolves `realloc_hndl` to a real heap realloc.
  In-place shrink does not return the slack, consistent with the allocator's
  whole-node model. No in-tree callers yet.)*
- ~~`common/src/time.c` `gmtime` partial, `strftime` is a no-op.~~ *(Fixed:
  full `gmtime` (epoch→broken-down UTC, leap-correct, post-1970) and a real
  `strftime` subset (`%Y %y %m %d %e %H %M %S %j %p %a %b %%`, bounds-checked,
  unknown specifiers emitted verbatim). Host unit tests in `tests/test_time.c`.
  No in-tree callers yet.)*
- SMP timer/IPI TODOs (`SysTimer/src/main.c:39`) — load-bearing once APs schedule.

### [FIXED] Unbounded hardware busy-waits (no timeout)
The AHCI/RTL spins flagged below were bounded on branch
`claude/driver-busywait-timeouts`: `ahci_resethba` (GHC reset) and the AHCI
command-completion `PxCI` spin (also dropped its per-iteration `DEBUG_PRINT`
flood, now returns -1 on timeout), and the `rtl8139`/`rtl8169` tx
descriptor-ownership waits (bounded, drop the packet on timeout; rtl8169 unlocks
before returning). All use ~2e8 iteration caps matching the existing
`ahci_obtainownership` pattern.

### [VERIFIED] crypto: short signing key reads out of bounds (documented, not changed)
`libs/crypto/hmac.c` `hmac_init` unconditionally `sha256_update(..., key, 32)`,
i.e. it always reads 32 bytes of the key. If a signing key file
(`KMOD_HMAC_Key.txt`/`SERV_HMAC_Key.txt`) decodes to fewer than 32 bytes,
`sign_exec` and the kernel verifier read past the buffer. *Not changed* (it is
the signing boundary and changing it would invalidate every signed module), but
callers must guarantee >=32-byte keys, and this HMAC is a **non-standard**
construction (hashes a fixed 32 bytes, 32-byte pad not block-size) so it does
not match RFC 2104/4231 — see `tests/test_hmac.c`. A future re-key should move to
standard HMAC and a length-checked key load.

### [DONE] Shared kernel PML4
The kernel half of the address space is no longer copied per-core. There is one **master
kernel PML4** (`kmem.pml4` in `SysVirtualMemory/.../vmem.c`) whose upper 256 entries are
the kernel address space; every process's PML4 (its own hardware page, allocated in
`vmem_create`) copies those 256 entries *once* at creation. Because the entries point at
**shared** lower-level tables, a runtime kernel-map change is instantly visible in every
address space with no resync — provided no *new* top-level kernel PML4 entry is created
after boot, which is why `vmem_init` pre-creates every kernel PML4 entry (physmap,
kernel-top, vmalloc). This deleted the per-core `lcl->ktable`, the three 256-entry
`memcpy`s per context switch, and `vmem_savestate` entirely: `vmem_setactive` is now just
a `cr3` load (cr3 points straight at the task's PML4), and a core with no task runs on
`kmem.pml4` directly. Verified: 6/6 SMP boots fault-free, every context switch a bare cr3
load against per-task shared-kernel PML4s.

A cross-core TLB-shootdown primitive (`vmem_flush` + an IPI vector) was prototyped while
this was per-core, then **removed** as dead code, and has now been **reintroduced**
adapted to the shared PML4 (see "[FIXED] cross-core TLB shootdown" below). The shared PML4
still makes kernel-map *creation* visible everywhere without any IPI; the shootdown exists
for the invalidation cases creation does not cover (runtime kernel *unmap* / permission
downgrade, and cross-core user unmap).

### [FIXED] new top-level kernel PML4 entries at runtime would not propagate
*(Previously latent: a kernel map needing a brand-new top-level PML4 entry at runtime
would not reach already-created address spaces, since each process PML4 copies the
kernel half once at `vmem_create`. Nothing creates one today — the only runtime-growing
kernel region is `vmem_vmalloc`, which has 512 GiB under its single pre-created entry
(259, after the WB/UC/WC physmap windows at 256/257/258) and currently has zero callers. Closed up front anyway: `vmem_init` now reserves
**every** kernel-half PML4 entry (256..511) by installing a zeroed PDPT for each absent
one before any AP boots or any task is created, so all 256 are present and shared and any
future kernel region can grow across a 512 GiB PML4 boundary with no per-address-space
resync. Cost: at most 256 PDPT pages (1 MiB) pinned for the life of the system; the empty
PDPTs map nothing, so accesses under them still fault until explicitly mapped. This is the
"reserve the full kernel PML4 entry range up front" option; the alternative (propagate new
entries to every live address space) was rejected — there is no registry of live `vmem_t`,
and it would race context switches and `vmem_create`. Verified: full KVM boot to
`servicescript` exit with APs scheduling, fault-free. Orthogonal to TLB shootdown (next
section).)*

### [FIXED] cross-core TLB shootdown + missing local TLB invalidation
*(Two gaps, one in `vmem_unmap` itself, one cross-core.*

**Local:** `vmem_unmap` edited the page tables but never issued any `invlpg`, so even
single-core the stale translation survived until the next context switch — a permission
downgrade (`task_updatemap` does unmap+remap) was not enforced, and a freed-then-reused
frame stayed writable through the unmapping task's own stale entry. `vmem_unmap` now calls
`vmem_local_flush` (per-page `invlpg`, or a `cr3` reload above 1 GiB — kernel pages are not
global, so a `cr3` reload flushes them too) over the range before returning.

**Cross-core:** a user mapping mutated via `task_unmap`/`task_updatemap` (both take an
arbitrary target task id, so one core can unmap/downgrade an address space *live on another
core*) left the other core's TLB stale → it could write a freed/reused frame (UAF) or keep
the old, more-permissive entry. `task_unmap` even `physmem_free`s the frame immediately.
Reintroduced a synchronous IPI shootdown (`vmem_shootdown`, `vmem_smp_init`,
`CALL:vmem_smp_init` after `CALL:mem_init` in `loadscript.txt`), adapted to the shared PML4
(the old per-core kernel-half refresh is gone; the handler is just local-flush + ack):
- **Targeting.** Each `vmem_t` records the single `active_apic` it is active on (set/cleared
  in `vmem_setactive`; one task per AS and no migration ⇒ ≤1 core). Kernel ranges
  (`virt < 0`) broadcast to all other cores; user ranges shoot down only `active_apic`.
- **Lifetime/deadlock-safety.** The page-table edit and the cheap local flush run under the
  task/`vmem` lock; the active-core APIC id is *snapshotted by value* there
  (`vmem_active_apic`); then the lock is dropped and `vmem_shootdown` runs with interrupts
  **on** and no lock held (it busy-waits for the target to ack from interrupt context — a
  target spinning with interrupts off on a lock the initiator holds would deadlock, hence
  the strict contract). The `vmem_t` is never dereferenced during the wait, so the target
  task exiting/freeing it mid-shootdown is harmless. The shootdown completes **before**
  `physmem_free`, so no core can touch the frame through a stale entry once it is reusable.
- A core that *enters* an AS does a `cr3` load (full flush), so any core that becomes active
  after the edit needs no shootdown; a stale snapshot only ever causes a harmless extra IPI.

No-op on a single core / before `vmem_smp_init`. Verified: full KVM SMP boot to
`servicescript` exit, APs scheduling, fault-free.)*

---

## Compositor — shared-memory grants (phase 1)

The grant substrate (`libs/lisp/src/grant.c`, `sys-shm`/`sys-shm-mint` in
`modules/SysLisp/src/main.c`) backs the compositor's zero-copy surfaces; design
in `notes/servers/CoreCompositor.md`. Two known limitations are tracked here:

### Read-only grants are enforced in software, not by the page table
`modules/SysLisp/src/main.c` `prim_map_grant`; `libs/lisp/src/prims.c` (bytes mutators)

The page table cannot enforce read-only here: `vmem_phystovirt`
(`modules/SysVirtualMemory/.../vmem.c:748`) selects a prebuilt physmap window
purely by cache type and ignores write-permission bits, so every physmap window is
RW. Read-only is therefore enforced **in software**: `map-grant` marks a `'ro`
grant's `bytes` view read-only (`lisp_bytes_mark_readonly`), and every bytes
mutator (`bytes-*-set!`/`bytes-fill32!`/`bytes-copy!`, `gfx-*`) refuses to write a
read-only destination. This is **airtight in the Lisp sandbox** — a grantee can
reach the region only through those prims (no pointer arithmetic; without
`sys-mmio` it cannot re-map the phys writable), which is the correct enforcement
layer for this system (the VM/capability boundary, not the page table). `'ro` is
the default; a writable grant must be minted `'rw`.
**Caveat for non-Lisp paths:** the flag is a VM-accessor check, so a *C-level* or
*device-DMA* writer with the raw phys would bypass it. No such path can be reached
by a `sys-shm`-only grantee today; if one is added, it must honor the flag (or a
real read-only physmap window is then warranted).

### [DONE] Use-after-revoke neutralized in software (zero-page semantics)
`grant-revoke` flips the table slot so future `map-grant` returns `#f`, AND a
grantee's existing mapped view is now neutralized: `map-grant` stamps the grant's
`(index, generation)` onto the `bytes` view (`lisp_bytes_set_grant`), and every
bytes accessor re-validates it against the grant table (`lisp_grant_is_live`) — once
revoked, the view **reads as a zero page and refuses writes**, so a late
use-after-revoke can neither read the (reused) RAM nor corrupt it. The page table
can't enforce this here (`map-grant` hands back the shared physmap window from
`vmem_phystovirt`, not a private mapping), so like read-only it is enforced **in
software at the bytes layer** — airtight in the sandbox, where a grantee reaches the
region only through these prims. Coverage is EVERY path that touches a granted
view's backing, not just the obvious mutators: writes via one guard
(`BYTES_WR_GUARD` — `bytes-*-set!/fill32!/copy!` + every `gfx-*`); and reads, which
must not leak the reused RAM either — `bytes-ref` returns 0, a revoked
`gfx-*`/`bytes-copy!` SOURCE draws/copies nothing, **`send` (`deep_copy`) snapshots
zeros not the backing**, **`equal?` (`deep_equal`) returns not-equal without a
memcmp oracle**, a hash-table key (`equal_hash`) hashes a constant, `bytes-phys`
returns 0 (no address disclosure), and `ttf-rasterize`/`ttf-vmetrics` (a client can
pass a view as a "font") skip it. Tested: `test_grant.c` (live read/write → revoke →
read-0 / write-refused / dead-source no-op / send-zeroed / equal?-#f / phys-0) + the
in-OS `cardinal.compositortest` (real `map-grant`: after `destroy-surface` revokes,
the client's still-held view reads 0 — "revoke OK use-after-revoke reads zero").
`grant.c`/`prims.c`/`value.c`/`sched.c`/`modules/SysLisp/src/main.c`.

### [INCOMPLETE] Phase-4 present blocks the root serve loop on the flush ack
`lisp/init.clp` (`compositor-gpu-target` present closure); `lisp/servers/corecompositor.clp` (`present!`)

The injected virtio-gpu `present` does `(send gpu (list 'flush-rects rects (self)))`
then `(recv)` to block on the driver's `'flushed` ack — deliberate **backpressure**
so a fast client can't pile unacked frames in the driver's mailbox. But `present!`
runs **inside the root `serve` step** (per commit/configure/raise/destroy), so if the
GPU driver context has exited (device hang, future driver error — a `serve` loop dies
on any unguarded prim error, and `send` to a dead ctx is silently dropped), the
`(recv)` never returns and the **root compositor wedges permanently**. v1 contract:
the GPU driver does not exit (QEMU never faults it); a production seam needs a
deadline-bounded flush wait or driver-liveness detection that degrades to off-screen
compositing. Same shape as the documented `start-gpu-demo` get-framebuffer block
(`init.clp`), promoted from demo to infrastructure.

### [DONE] Per-verb arity + type validation (the compositor)
`lisp/servers/corecompositor.clp` (`len>=`, the per-op guards)

The compositor is the first `serve` service to fully validate message **shape**, not
just the reply-handle type: every primary-loop and handler-op verb guards with a
safe `len>=` length walk (never `cdddr`, whose `(cdr '())` would abort the loop on a
truncated message) and `integer?`-checks every id/x/y/buf before it can reach `=` /
`gfx-blit!`. So no malformed message from a semi-trusted client — short, non-integer
id, garbage `buf` — can kill the root or a handler. This is the concrete instance of
the "broader arity sweep" the `ctx?`/`reply-to` note below leaves open for the other
servers.

### Reply-handle validation in request/reply servers (`ctx?` / `reply-to`)
`lisp/lib/driver-util.clp` (`reply-to`); `libs/lisp/src/sched.c` (`ctx?`)

A `serve` loop has no try/catch, so a `send` to a non-context **aborts the service
permanently**. Any request/reply server that `send`s to a reply handle taken from
an (untrusted) message was therefore one forged handle away from a DoS. The fix is
the `ctx?` predicate + the `reply-to` helper (deliver only to a context): a server
either `reply-to`s a direct reply, or guards `(ctx? handle)` before forwarding a
reply handle into another message (so the eventual sender is protected). Adopted in
`coreaudio`, `cardfs`, `corestorage` (read/write dispatch), `corenetwork`
(service + txworker). **Still open:** (a) trusted-registration handles
(`corepower`/`coreinput` device ctx, `corenetwork/tcp` connection owner) are
validated only by virtue of trusted callers, not at registration — harden if those
become client-reachable; (b) this guards the *type* of the reply handle, not message
*arity* — a too-short message still aborts the loop on `cadr`/`nth`; full per-verb
arity validation is a broader, separate sweep (the compositor does it, most servers
don't yet).

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

### [KNOWN LIMITATION] Lisp xHCI control IN ignores short-packet residual
`lisp/drivers/xhci/driver.clp` `do-control` returns the *requested* byte count
on an IN transfer even when the completion is `SHORT_PACKET`: the DATA TRB has no
ISP bit, so only the STATUS TRB raises a Transfer Event and the data-stage
residual is unrecoverable from it. Compliant devices return standard descriptors
at full length, so enumeration is unaffected; a device that short-packets a
control IN would have the tail of the bounce buffer read as stale data. Inherited
verbatim from the old C `xhci` driver. Fix = set ISP on the DATA TRB and wait for
the DATA event first to read the residual. (The sibling C-driver bug where Address
Device BSR=0 re-pointed EP0 at stale TRBs is *fixed* in the Lisp port — the TRDP
is set to the ring's current enqueue, not its base.)

### [OBSOLETE — code removed] USB enumeration / type field uninitialised
*(The entire C USB stack — `CoreUsb`, `uhci`/`xhci`/`ehci`, the `usb_*` class
drivers — was removed when USB moved to Lisp (`lisp/servers/coreusb` +
`lisp/drivers/{uhci,xhci,usb-hid,usb-hub,usb-storage}`). The note below is
historical.)*
`servers/CoreUsb/src/main.c` `usb_register_device` — `def->idx =
devIDs[def->type]++` indexed `devIDs` with an uninitialised `def->type` (an OOB
read/write on garbage). *(Fixed: `def->type` is now pinned to
`usb_device_type_unknown` before the index. Root cause is a design gap —
`usb_device_t` carries no device-class/type field, unlike `usb_hci_desc_t`; a
real type/class should be added there so devices get distinct id namespaces.
Recent EHCI/UHCI/CoreUsb work is mid-flight, so this is the minimal safe fix.)*

### [LIKELY] Unbounded hardware busy-waits (no timeout)
- `drivers/ahci/src/ahci.c:236` spins on `PxCI` while spamming `DEBUG_PRINT`
  (the read path; `ahci_resethba`'s `while (GHC & 1)` and the port-init waits are
  likewise unbounded — bound these like `ahci_obtainownership` now is).
- `drivers/rtl8169/src/driver.c:86`, `drivers/rtl8139/src/driver.c:56` spin on
  descriptor ownership with no timeout (8169 also drops/retakes the lock around
  the spin, racing the DMA engine).

### [INCOMPLETE] Stubs
`CoreAudio`, `tarfs` module_init are empty. `CoreStorage` now has a working
block-device + fs-provider registry (`usb_storage` registers a block device;
`drivers/cardfs` registers as an fs provider and is probed per device). cardfs's
probe is read-only by default — the destructive format/roundtrip exploration is
behind `CARDFS_SELFTEST` (off). A userspace file-I/O path remains TODO. `intel_wifi`
`module_init` early-returns before any init. `CoreNetwork` ARP/ICMP/IPv4-rx and a
minimal tx path are implemented (echo/ARP reply work), and now also a **UDP layer**
(port bind/unbind + `udp_send_to`) and a general **reliable delivery transport
(RDT)** over UDP — responder-driven, no device-side timers. The **live K5 stack is
the Lisp `corenetwork` port**, which additionally carries a **DHCP client** (the
default; `cardinal.ip=A.B.C.D` forces a static address), **outbound ARP
resolution** (`arp-resolve`), and **ARP cache aging** — the functionality the C
PRs #63/#64 prototyped, ported into Lisp (see `notes/servers/CoreNetwork.md`,
"Lisp port status"). Verified live: a no-`cardinal.ip` virtio-net/slirp boot
auto-acquires `10.0.2.15` from DHCP. The new
`CoreNetDebug` server (cmdline-gated by `cardinal.netdbg`) consumes these to offer
a UDP echo + reliable named-blob upload for fast, swap-free network debug, plus a
**TCP echo on port 7**. **TCP is now implemented** (`lisp/servers/corenetwork/tcp.clp`):
active + passive open, in-order data with cumulative ACKs, out-of-order reassembly,
timeout retransmission (Go-Back-N) driven by a ticker context, FIN teardown with
TIME-WAIT, the pseudo-header checksum, and peer-window flow control; congestion
control / SACK / window-scaling are intentionally omitted (LAN/loopback target).
The socket surface is the in-OS message API (`tcp-listen`/`tcp-connect`/`tcp-send`/
`tcp-close` + `tcp-accept`/`tcp-connected`/`tcp-rx`/`tcp-closed` events, plus a
synchronous `tcp-connect-blocking` helper) — the connection-oriented analogue of
the UDP `udp-bind` pattern. Verified live: a host `nc`/socket drives the guest TCP
echo over slirp `hostfwd`, including multi-segment transfer and clean teardown.
The retransmission + out-of-order reassembly paths are validated under a test-only
loss injector (`cardinal.tcploss` drops 1 in 4 received segments; an 8 KB echo
still completes byte-perfect). The socket API is the message protocol itself, so it
is already usable from sandboxed (capability-gated) Lisp contexts — the userspace
model in this OS; no ring-3 surface is required. What remains is transport-level
(routing over the gateway, DNS, async tx/packet-hold). See
`notes/servers/CoreNetwork.md`. Matches the README status notes.

### [FIXED] virtio-net header is 10 bytes but the modern device needs 12
`drivers/virtio/net/inc/net.h` `virtio_net_cmd_hdr_t` was the 10-byte legacy
header. `virtio_common` negotiates `VIRTIO_F_VERSION_1` for the modern device
(1af4:1041, QEMU's `virtio-net-pci`), which mandates the 12-byte header with a
trailing `num_buffers`. With the 10-byte struct the device consumed the first 2
bytes of every **tx** frame as header (and the driver mis-located the **rx**
frame by 2 bytes). *(Fixed: added `uint16_t num_buffers`. Confirmed by packet
capture — tx frames previously arrived shifted left by 2 bytes, e.g. broadcast
`ff:ff:ff:ff:ff:ff` seen as `ff:ff:ff:ff:52:54`; after the fix ARP + ICMP echo
complete a full exchange with the slirp peer.)* Caveat: a pure-legacy device
without `VERSION_1`/`MRG_RXBUF` uses a 10-byte header — if such a device is ever
targeted, the header size must become feature-dependent.

### [FIXED] boot hangs under KVM the instant servicescript is scheduled
*(Fixed: the root cause was an APIC-timer/scheduler-entry ordering bug — the first
preemption tick was never delivered under KVM, so the scheduler never ran the first
non-idle task. With the ordering fix the full boot now completes under `-accel kvm`
(fast) as well as `-accel tcg`; all servers load and `servicescript` runs to
`end_task_syscall`. KVM is now the default for runtime smoke tests.)*

Historical symptom: under `qemu -accel kvm` (q35), the boot reached `[SysTaskMgr]
Process Started: servicescript` / `[SysTimer] Allocated timer: apic_local` and then
hung — **no servers loaded** (no `[Kernel] Load module:./Core*` lines) — while the
same image booted fully under `-accel tcg`. Reproduced on **master** with `-smp 1`
and `-smp 2`.

---

## USB stack — bugs found during USB hardening (2026-06)

These surfaced while extending the USB stack (enumeration foundation, robustness,
mass-storage). All three turned out to be **one root cause** — a VM capture-
analysis bug, fixed in `libs/lisp/src/lbc.c`. With that fix, UHCI + xHCI both
enumerate and UHCI mass-storage works end-to-end.

### [FIXED] VM #PF / silent state corruption: `(define (f) ..)` capture missed
The register-bytecode VM's capture analysis (`collect_captures`) recognized
`lambda` and named-`let` heads but **not** the `(define (f . params) body)`
internal-define shorthand. A binding referenced ONLY inside an internal-define
body was never boxed (`ROP_MKCELL`) yet was read/written as a cell
(`ROP_CELLGET`/`CELLSET` → `lisp_car` on a raw value): a `#PF` (`cr2` small, e.g.
`0x9`) on a captured-and-mutated binding, and **silent value corruption** on one
that was only read. Fixed by handling the define shorthand like a lambda in
`collect_captures`. `cardfs` survived only by luck — its one mutable binding was
also referenced in a body-level named-let, which the analysis did catch.

The two symptoms below were the SAME bug downstream, both fixed by the above:

### [FIXED] UHCI: transfers from a class-driver context never completed
Was: the `usb-storage` block server's first bulk stalled forever. Cause: the
captured params `dev`/`in-ep`/`out-ep` (used only inside internal defines) were
corrupted, so the bulk's `(usb-dev-hci dev)` handle was garbage and the send went
nowhere. With the capture fix, UHCI mass-storage runs INQUIRY / READ CAPACITY /
register cleanly. (The make-cell "workaround" tried earlier was insufficient — it
celled the four `let` bindings but not the captured params — proof a workaround is
not a fix; `usb-storage` is back to plain `set!`.)

### [FIXED] xHCI: root-port poll never detected a connected device
Was: `poll-ports!` never reported a connect (no `[xhci] port up`). Cause: the
port-poll path captured a binding only inside internal defines, corrupted by the
same VM bug, so connect detection silently failed. With the fix, `USB=xhci-kbd`
enumerates the keyboard and `xhci-storage` the disk.

### Isochronous transfers (added)
Both controllers now serve an `(isoch addr speed ep maxp data len dir-in? reply)`
transfer message (proto.clp), the streaming primitive USB audio needs. UHCI
schedules one iso TD per packet directly into consecutive frame-list slots (each
TD links onward to the control QH), waits for the frames to pass, then restores
the slots. xHCI configures an isochronous endpoint (CErr=0, Interval=3) and pushes
one Isoch TRB per packet, all SIA (Start Isoch ASAP), IOC on the last.

Known simplifications (sufficient for tone playback; revisit for gapless/UAC2):
- **Synchronous chunked feed.** A submission is one bounded buffer (UHCI ≤4 KiB /
  64 packets; xHCI ≤2 KiB), clocked out whole before the completion replies. The
  class driver streams chunk-by-chunk, so there is a small inter-chunk gap (a
  benign under-run — invisible on QEMU's sink device).
- **xHCI interval hardcoded to 1 ms** (Interval=3): the transfer protocol does not
  carry bInterval, and 1 ms is the universal audio service interval. A non-1 ms iso
  endpoint would be mis-scheduled.
- **OUT-focused.** `usb-isoch-in` exists and the controllers handle dir-in?, but
  per-packet IN residuals are not tracked (the audio scope is playback-only).

### USB Audio class driver (added)
`lisp/drivers/usb-audio.clp` is the class handler for bInterfaceClass==1 (Audio).
On `(probe dev)` it finds an AudioStreaming interface alt with an iso OUT endpoint,
activates it (`SET_INTERFACE`), best-effort sets the rate (UAC1 SET_CUR
SAMPLING_FREQ), registers a card (`usbaudioN`) with coreaudio, and answers the same
`tone`/`play`/`endpoints`/`get-volume`/`set-volume`/`mute` protocol hdaudio does —
synthesizing 48 kHz stereo-16 square-wave PCM (mirroring hdaudio's `fill-tone!`) and
streaming it over the iso OUT endpoint chunk-by-chunk. Validated end-to-end on both
UHCI and xHCI against QEMU's `usb-audio` device (`USB=uhci-audio|xhci-audio`): the
captured WAV is 100% non-zero (real audio through the iso path). Like usb-hid, its
completion wait is **stop-aware** — a `(stop)` on hot-remove aborts an in-flight tone
and exits the context instead of leaking it.

Not yet wired: volume/mute (a UAC Feature Unit — accepted + ignored), capture (mic),
non-48 kHz formats, and UAC2 clock-source rate setting.

### EHCI host controller (added)
`lisp/drivers/ehci/` is a third host-controller driver (USB 2.0 high-speed),
registering with coreusb like uhci/xhci. It uses the minimal **async-only,
single-reusable-QH** model (mirroring uhci's single control QH): one Queue Head is
the async reclamation head; per transfer it builds a qTD chain, rewrites the QH's
endpoint fields, and arms by writing the overlay Next-qTD pointer **last** (the
atomic arm, so the always-on async schedule never executes a half-updated QH).
Control/bulk/interrupt-in all ride the async schedule — a one-shot async IN serves
an interrupt poll exactly as it serves bulk. Completion is polled from the qTD
token in DMA (no MSI), as uhci does. Validated with QEMU `usb-ehci` + a HS
`usb-storage` (`USB=ehci-storage`): enumerates, INQUIRY / READ CAPACITY / CBW /
CSW, registers `usb0`.

Not implemented (noted for follow-up):
- **Periodic schedule** (hardware-paced interrupt/iso intervals) — interrupt-in is
  a one-shot async IN with a NAK budget (fine for HID polling cadence), and **iso
  replies an error** (no iTD/siTD).
- **Split transactions** (a FS/LS device behind a HS hub via a TT). EHCI root ports
  are HS-only here; a FS/LS device on a root port is **released to a companion**
  controller (PortOwner). A bare `usb-ehci` with no companion can't bind FS/LS.
- One reusable QH ⇒ one outstanding transfer at a time (the single-context model
  already serializes), and per-transfer QH-field rewrites are done while the QH is
  idle (safe under the always-on async schedule).

A port-ack bug found in review and fixed before merge: the PORTSC W1C
acknowledge preserved PED, which is write-1-to-disable — writing back a read PED=1
would have disabled the just-enumerated port (PED dropped from the preserve mask).

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
