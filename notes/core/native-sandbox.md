<!---
 Copyright (c) 2026 Himanshu Goel

 This software is released under the MIT License.
 https://opensource.org/licenses/MIT
-->

# Native sandboxes hosted by Lisp contexts

> **SUPERSEDED by [`wasm-guests.md`](wasm-guests.md).** This ring-3 / hardware-
> isolation approach was rejected for dragging native-process machinery (rings,
> MMU/CR3, syscall trampolines, fault-vs-panic unwinding) into an OS whose whole
> point is that *the interpreter is the security boundary*. Kept for the analysis
> of why hardware isolation doesn't fit and the survey of the dormant native
> machinery. The chosen direction runs foreign code as **WebAssembly in a
> Lisp-hosted interpreter** — see `wasm-guests.md`.

Status: **design proposal, NOT pursued** (kept for reference). A plan for running non-Lisp
(native, ring-3) code — a ported C program such as Doom, a language runtime, an
untrusted blob — inside Cardinal; without teaching the scheduler about user mode.

## Goal

Run a native guest such that:

1. The **hosting Lisp context defines the guest's syscall surface** — the guest
   can only do what its host's capabilities allow.
2. Control **periodically returns from the native code** to the Lisp world, so a
   guest cannot starve the cooperative Lisp scheduler.
3. **The scheduler never learns ring-3 exists.** From the scheduler's view there
   is only a Lisp context spending its slice; whether that slice ran bytecode or
   bounced through ring-3 is invisible to it.

This is a revival of the founding model in [`Syscalls.md`](Syscalls.md)
("*watchdog used instead of a regular timeslice... yield/tick call performs the
watchdog reset followed by a task switch*"), re-expressed as a coroutine owned by
a Lisp context rather than as a competing native scheduler.

## Key insight: the machinery already exists, dormant

Everything ring-3 needs was built for the pre-K5 native scheduler and still
compiles; K5 ("interpreter-as-scheduler") just stopped *using* it:

| Mechanism | Where | State today |
|---|---|---|
| Ring-3 task descriptor (own `vmem_t`/CR3, kernel+user stacks, saved regs, FPU buffer, `syscall_data`) | `modules/SysTaskMgr/inc/task_priv.h` `process_desc_t` | built, used pre-K5 |
| Per-task address-space isolation (CR3 per task) | `SysVirtualMemory` `vmem_setactive()` | works |
| `syscall`/`sysret` transitions + MSR setup (LSTAR/STAR/FMASK) | `modules/SysUser/.../syscall.c` | works |
| **Syscall *sets*: per-task allowlist** (`syscall_set_table[128]`, two-level dispatch) | `modules/SysUser/.../syscall.c` | works — *this is exactly the per-host syscall control we want* |
| Preemption while in ring-3 (timer → TSS RSP0 → IDT → iret) | `SysInterrupts` IDT + per-core TSS | works |
| User-ELF load + ring-3 entry | `task_startnew_user`, `mana` built `-fpie` | exists (mana is a stub) |

What is **dormant**: the native run queues (`run_queues[]`), the 50 µs
preemption timer (`task_core_arm`), and `task_start_kernel`'s enqueue path. Under
K5 each core runs one ring-0 thread in `lisp_core_loop`
(`modules/SysLisp/src/main.c`) that cooperatively multiplexes all Lisp contexts
by reduction budget (`lisp_ctx_t.budget`, checked at every VM safe point in
`libs/lisp/src/lbc.c`). No native task is ever created from Lisp today.

The plan **does not re-arm the native scheduler.** It reuses the descriptor +
ring-transition as a *primitive* the Lisp host drives directly.

## The model: a guest is a coroutine owned by a host context

```
   Lisp scheduler (per core, cooperative)
        │  schedules host contexts only — knows nothing below this line
        ▼
   ┌─────────────── host Lisp context ───────────────┐
   │  caps = (sys-display sys-input ...)              │
   │  loop:                                           │
   │    status = (native-resume guest budget-ns)  ───┼──► ring-3 guest runs
   │    dispatch status (upcall / timeout / exit) ◄──┼──   until it upcalls,
   │    ... talk to Core* servers in Lisp ...         │     exits, or the
   │    (yield)  ; give the slice back when idle      │     watchdog fires
   └──────────────────────────────────────────────────┘
```

The guest descriptor is **created but never put on a run queue**. Only
`native-resume`, called from the host context, ever runs it. So:

- The **Lisp scheduler** schedules the host context exactly as any other context.
- The **host context** is the guest's scheduler: it decides when to resume it,
  for how long, and what each upcall means.
- The **native scheduler** stays dormant. Requirement (3) holds by construction.

## Components

### 1. Guest descriptor — reuse `process_desc_t`, unscheduled

Add a `task_create_native()` that allocates a `process_desc_t` with
`task_permissions_none` (its own `vmem_t`/CR3, ring-3 user stack at the existing
`0x1_0000_0000`, kernel stack, `syscall_data`, `fpu_state`) **but does not call
`task_start_kernel`** (which would enqueue it on `run_queues[]`). The descriptor
exists solely to be driven by `native-resume`. This keeps the run-queue/native
scheduler untouched.

### 2. `native_enter(desc, budget_ns) → status` — the core primitive

A new SysTaskMgr/SysUser export. Conceptually a synchronous coroutine switch:

```
native_enter(desc, budget_ns):
    save host kernel continuation (setjmp-style buffer on host's stack)
    save host (Lisp core) FPU state; load desc->fpu_state
    install desc->syscall_set as the active set (per-core syscall_state)
    vmem_setactive(desc->mem)          ; CR3 -> guest address space
    arm watchdog one-shot timer (budget_ns) -> native_watchdog_isr
    sysret/iret into ring-3 at desc saved RIP   ; (reuses user_transition)
    ; ---- control leaves; returns here via the return-to-host trampoline ----
    disarm watchdog
    vmem_setactive(host kernel space)
    save desc->fpu_state; restore host FPU
    return status                     ; UPCALL | TIMESLICE | EXITED | FAULT
```

#### The return-to-host trampoline (the one load-bearing new mechanism)

Today the syscall handler always `sysret`s back to ring-3. We add a path where
control instead unwinds to the *host's* kernel continuation, so `native_enter`
returns to its caller. It is a constrained `longjmp` and works the same way for
all three exit sources.

**The continuation.** `native_enter` captures, into a buffer that lives on the
host's kernel stack, the callee-saved set needed to resume itself:
`{rbx, rbp, r12–r15, host_rsp, host_cr3, return_rip}`. A per-core pointer
`current_native_return` points at the active buffer. Nesting (a host servicing an
upcall by resuming a *different* guest) is handled by save/restore of that
pointer around the inner `native_enter` — so the design supports one host
managing many guests without a fixed depth.

**Three exit sources, one convergence.** Each saves the guest's full state into
`desc` first, then calls `native_return(status, payload)`:

| source | how it fires | guest state saved by | status |
|---|---|---|---|
| `sys_upcall` | guest executes `syscallq` | the syscall save path (`syscall_getfullstate(desc->syscall_data)`) | `UPCALL` (+ tag/args from saved GPRs) |
| watchdog | one-shot timer IRQ while in ring-3 | the IDT save path → `desc->reg_state` | `TIMESLICE` |
| CPU exception | #PF/#GP/#UD/… in ring-3 | the IDT save path → `desc->reg_state` | `FAULT` (+ vector, error code, CR2) |

`native_return` switches `rsp` from the guest kernel stack (TSS RSP0) back to
`current_native_return->host_rsp`, restores the callee-saved set and host CR3,
and `ret`s to `return_rip` — i.e. back inside `native_enter`, which then disarms
the watchdog, restores FPU, and returns `status` to the host Lisp prim.

**Resume with a result.** `(native-resume guest result)` writes `result` into the
guest's saved `RAX` (`desc->syscall_data`) before the next `sysret`, so the
guest's `sys_upcall` call returns that value. Multi-word results go through the
shared buffer (§6).

**Distinguishing yield from ordinary interrupts.** While the guest runs with
IF=1, unrelated device IRQs and the Lisp scheduler's own timer can fire. Those
are handled normally and `iret` **back to the guest** — they are *not* yield
points. Only the watchdog vector, a fault, or `sys_upcall` reach
`native_return`. (The Lisp interrupt bridge / MSI handlers only set a flag and
clear a target ctx's `blocked`; they never context-switch, so a device IRQ
mid-guest just wakes a Lisp driver ctx that runs later, when the host yields its
slice. This is already how those handlers behave.)

### 3. Yielding: cooperative upcalls + a watchdog safety net  *(decided)*

Two ways control returns to the host, mirroring the Lisp budget model. **Both**
are in scope: cooperative as the fast path, watchdog as the safety net.

- **Cooperative (common path).** The guest issues an upcall (see §4) and blocks
  for the result. The upcall *is* the yield. For ported event-loop code this is
  natural: doomgeneric upcalls once per frame to present + read input — that is a
  perfect, frequent yield point.
- **Watchdog (safety net).** `native_enter` arms a one-shot timer for
  `budget_ns`. If the guest runs that long without upcalling, the watchdog ISR
  is the trampoline: it saves guest state and returns `TIMESLICE` to the host.
  The host can simply resume it next slice — i.e. a non-cooperating guest is
  *preemptively time-sliced*, still interleaved with other Lisp contexts. This
  is the [`Syscalls.md`](Syscalls.md) "watchdog instead of timeslice" idea, and
  it is the native analog of the Lisp reduction budget.

The watchdog is armed **only** around `native_enter` and disarmed on return, so
pure-Lisp cores are unaffected. It is a *distinct* `timer_request` handler — it
must never call the dormant `task_switch_handler`.

### 4. The ABI — hybrid: generic upcall + a few fast syscalls  *(decided)*

Keep the kernel-level ABI tiny and push policy into the host Lisp context, but
service genuinely hot operations in C so they don't pay a Lisp round-trip. Two
tiers, both gated by the host's caps:

**Tier 1 — generic upcall (policy, IO).** One syscall:

```
sys_upcall(tag, a0, a1, a2) -> u64     ; r12 = SYS_UPCALL, r13 = guest's set
```

`sys_upcall` saves guest state and returns to the host with a *request*
`(tag a0 a1 a2)` (the trampoline's `UPCALL` status). The host Lisp context
interprets the tag using its own capabilities and message-passing to the `Core*`
servers, then `(native-resume guest result)`s. So the guest's entire IO "kernel"
is whatever vocabulary the host defines in Lisp — requirement (1), literally.

**Tier 2 — fast syscalls (hot paths, no upcall).** A small fixed set serviced
entirely in ring-0 C against resources pre-granted at spawn, never leaving the
kernel. The host chooses *which* are installed in the guest's `syscall_set`
(built from its caps), so the capability story is unchanged — a guest simply
lacks the slots it wasn't granted:

| fast syscall | services against | why it can't be an upcall |
|---|---|---|
| `sbrk(delta)` | the guest's pre-granted heap arena | called constantly by an allocator |
| `get_ticks()` | `timer_timestamp_ns` | cheap, frequent (frame pacing, profiling) |
| `yield()` | the trampoline directly (`TIMESLICE`) | the bare reschedule, no payload |

Rule of thumb: a tag is Tier 2 only if it needs **no Lisp policy and no Core\***
**server** — pure arithmetic against a resource already handed to the guest.
Everything else (display, input, files, networking, exit) is Tier 1. Example
Tier-1 tags:

| tag | meaning | host services it by |
|---|---|---|
| `present` | frame ready in shared buffer | message `coredisplay` (needs `sys-display` cap) |
| `poll-input` | get next key event | message `coreinput` |
| `read-file` | WAD chunk by offset/len | `sys-initrd` read into shared buffer |
| `log` | debug line | `(log ...)` |
| `exit` | terminate | tear the guest down |

### 5. Capability mapping: host caps → guest syscall set

When the host calls `(native-spawn elf caps)`, the runtime builds the guest's
`syscall_set_table` entry from `caps` (a subset of the host context's own
`lisp_ctx_t.caps`, enforced by the existing no-escalation check in
`spawn-restricted`). A guest whose host lacks `sys-display` simply has no
`present` tag wired and no display fast-syscall installed. The Lisp W7
capability model and the native syscall-set model become **two faces of one
grant**, joined at `native-spawn`.

### 6. Shared memory for bulk data (no per-pixel syscalls)

The framebuffer and input must not flow through copies. Reuse the existing grant
/ SHM substrate (`sys-shm`, `grant-mint`, `dma-alloc`): allocate one physically
contiguous, kernel-mapped buffer; map it into the guest's low half with U=1; the
host reads it through its kernel-mapped virtual address. The guest's
`DG_ScreenBuffer` lives there; `present` is just "the bytes are ready, blit
them." Crucially, the guest only ever names this buffer **by offset**, never by
raw pointer — see §8.

### 7. FPU / SSE

Today Lisp cores never save FPU because they are never preempted
(`libs/lisp/inc/lisp.h`). A native guest *can* use SSE and the watchdog *can*
fire mid-guest, so `native_enter` must save the host FPU and give the guest its
own `fpu_state` (the field already exists; `fp_platform_get/setstate` already
exist). This is done at the resume boundary, not in any scheduler. (Doom itself
is pure fixed-point and needs none of this, but the mechanism must be correct
for general guests.)

### 8. Guest faults must unwind, not panic

The dormant ring-3 path has not executed since K5, and the IDT today assumes any
fault is a kernel bug → `PANIC`. A ported guest *will* fault (segfault, bad
opcode, divide-by-zero). Those must become a `FAULT` return to the host, not a
kernel panic.

Mechanism: `native_enter` sets a per-core `in_guest` flag (cleared on return).
In `idt_mainhandler`, for a CPU exception (vectors < 32) **with `CPL==3` and
`in_guest` set**, route to `native_return(FAULT, {vector, err, cr2})` instead of
the panic path. The host gets a structured fault and can log it, dump the guest,
and tear it down — exactly like a Lisp ctx that errors out. A ring-3 exception
with `in_guest` *unset* is impossible (no other ring-3 exists) and may keep
panicking. Faults that occur in ring-0 (in a syscall handler) stay panics — they
*are* kernel bugs.

This is the single riskiest piece to get right and is why Phase 1 is "re-validate
the dormant ring-3 path" before anything is layered on top.

### 9. Security properties

- **Containment.** The guest is ring-3 with U=0 on every kernel page; it cannot
  read the Lisp heap or touch other address spaces. Its only kernel entry is
  `syscallq` into a host-chosen set. A guest fault is contained (§8); worst case
  it wedges its own slice, caught by the watchdog.
- **Sidesteps the known pointer-safety gap.** Syscall args from ring-3 are
  *unvalidated* today (no copy-in/out; flagged in the SysUser survey). The
  offset-based shared-buffer ABI avoids the problem: the guest passes scalars and
  offsets, never kernel pointers, and the host validates each offset against the
  buffer length. We never dereference a guest-supplied pointer in the kernel.
- **TOCTOU on the shared buffer.** The host reads the buffer *after* the guest is
  parked (control is back in the host; the guest is not running), so there is no
  concurrent mutation — the guest cannot race the host's validation. (A guest
  pinned to one core, never on a run queue, cannot run while its host runs.)
- **No escalation.** `native-spawn`'s caps must be a subset of the host
  context's caps (reuse the `spawn-restricted` check).
- **GC must not move the shared buffer.** Back it with a non-moving, non-GC-owned
  allocation (`lisp_make_bytes_foreign` / `dma-alloc`), since its physical pages
  are mapped into the guest's address space. The guest *descriptor* wrapped as a
  Lisp value must be GC-traced (so it survives while the host holds it) with a
  finalizer or explicit `(native-destroy guest)` that frees the `vmem_t`, stacks,
  and arena.
- **FMASK already clears IF on entry** (`syscall_plat_init`), so a `sys_upcall`
  enters with interrupts off; the watchdog/IRQs only fire mid-guest with the
  guest's own RFLAGS.IF. No change needed there.

## Control flow — one host slice

1. Lisp scheduler gives the host context its reduction budget.
2. Host calls `(native-resume guest 4ms)`.
3. `native_enter`: save host kernel cont + FPU, install guest syscall set, switch
   CR3, arm watchdog, `sysret` into the guest.
4. Guest renders a frame, issues `sys_upcall(present, off, len, 0)`.
5. Syscall handler saves guest state, runs the **return-to-host trampoline** →
   `native_enter` returns `(UPCALL present off len)`.
6. Host (in Lisp) validates `off/len`, messages `coredisplay` to blit, polls
   `coreinput`, then `(native-resume guest 4ms)` again with the input result.
7. When the guest has nothing pending (or the host wants fairness), the host
   `(yield)`s — handing the *slice* back to the Lisp scheduler, which moves to
   the next context, none the wiser about ring-3.

## Worked example: Doom

- doomgeneric compiled `-fpie`, no floating point (fits the kernel's `-mno-sse`),
  its own zone allocator over a granted arena.
- Platform shim: `DG_DrawFrame → sys_upcall(present)`; `DG_GetKey →
  sys_upcall(poll-input)` (or a fast syscall draining a shared ring);
  `DG_GetTicksMs → get-ticks` fast syscall; WAD `fread → read-file` upcall over
  the initrd.
- Host Lisp app (caps `(sys-display sys-input sys-initrd)`): opens a
  `coredisplay` surface, loops resume → present → blit → input, paces with
  `sleep`. A Doom crash cannot panic the kernel (contrast: a privileged C
  `Sys*` module would). See [Doom porting notes] in chat history / a future
  `notes/` entry.

## What to build (phases)

1. **Re-validate the dormant ring-3 path + `native_enter` + trampoline + fault
   unwind** (SysUser/SysTaskMgr/SysInterrupts). Prove a hand-written ring-3 blob
   can `sys_upcall` and resume, *and* that a deliberate fault (#GP/#PF) returns
   `FAULT` to the caller instead of panicking (§8). No Lisp yet — drive it from a
   C harness. This phase carries the real risk.
2. **Watchdog timer handler** + `budget_ns` enforcement; prove a spinning guest
   is forcibly returned with `TIMESLICE`.
3. **`task_create_native()`** (unscheduled descriptor) + user-ELF load from
   initrd (extend `task_startnew_user`).
4. **Lisp value type + prims**: `native-spawn`, `native-resume`, `native-state`,
   `native-destroy`; cap → syscall-set wiring; FPU save/restore at the boundary.
5. **Shared-buffer plumbing** (reuse `sys-shm`/grants, non-moving) + the Tier-1
   `present`/`poll-input`/`read-file` vocabulary and the Tier-2 fast syscalls.
6. **Doom** as the first real guest.

## Decided

- **ABI: hybrid** — generic `sys_upcall` for policy/IO + a few C-serviced fast
  syscalls (`sbrk`, `get_ticks`, `yield`) for hot paths; both gated by host caps.
- **Slice bound: cooperative + watchdog** — upcalls are the fast-path yield, a
  per-resume one-shot watchdog forcibly returns a non-yielding guest.

## Still open (settled defaults, revisit if needed)

- **Reuse `process_desc_t`** for the guest descriptor (free CR3/stacks/FPU/
  `syscall_data`) but never enqueue it on a run queue. *(default: yes)*
- **Per-guest core affinity** — pin a guest to its host's core; no cross-core
  guest migration in v1. *(default: pinned)*
- **Multiple guests per host** — the per-core continuation pointer already
  supports nesting; whether v1 exercises it is a scope choice. *(default: one
  guest per host in v1)*
```
