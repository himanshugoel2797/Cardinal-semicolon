# Sys runtime modules

The kernel-privileged runtime layer: interrupt routing and dispatch
(`SysInterrupts`), symmetric-multiprocessing bring-up and per-core thread-local
storage (`SysMP`), the timer/clock abstraction (`SysTimer`), and FPU/SSE state
management (`SysFP`). All four are `x86_64`/`pc` platform implementations behind
the architecture-neutral headers in `modules/inc/`.

---

## `interrupt_sendeoi`

- **kind:** function
- **lang:** c
- **source:** `modules/SysInterrupts/src/platform/x86_64/pc/apic.c`
- **hash:** 266df2477c83a2b1

Sends an end-of-interrupt to the local APIC for `irq`, but only if that vector's
in-service bit is actually set.

**Parameters**

- `irq` — the interrupt vector being acknowledged.

The in-service-register check guards against issuing a spurious EOI: it reads the
APIC ISR for the vector's bank/bit and writes `APIC_EOI` only when the bit is
set. Call this at the end of an interrupt handler once the device condition has
been cleared.

---

## `interrupt_register_handler`

- **kind:** function
- **lang:** c
- **source:** `modules/SysInterrupts/src/platform/x86_64/pc/idt.c`
- **hash:** b91b07dbc1079e9e

Adds `handler` to the list of functions invoked when vector `irq` fires.

**Parameters**

- `irq` — vector to attach to.
- `handler` — `void (*)(int)` callback; receives the vector number.

Multiple handlers can share a vector (up to `IDT_HANDLER_CNT` slots); the handler
is placed in the first free slot. Registration runs under `cli()` plus
`interrupt_alloc_lock`, and the dispatch path reads the (volatile) slot array
lock-free, so a partially written handler pointer is never observed. Oversubscribing
a vector beyond the slot count `PANIC`s.

---

## `interrupt_unregister_handler`

- **kind:** function
- **lang:** c
- **source:** `modules/SysInterrupts/src/platform/x86_64/pc/idt.c`
- **hash:** 64003b264559c525

Removes every slot whose handler equals `handler` from vector `irq`'s dispatch
list.

**Parameters**

- `irq` — vector to detach from.
- `handler` — the previously registered callback to remove.

Like registration this runs under `cli()` + `interrupt_alloc_lock`. It clears all
matching slots (so registering the same handler twice and unregistering once
removes both).

---

## `interrupt_allocate`

- **kind:** function
- **lang:** c
- **source:** `modules/SysInterrupts/src/platform/x86_64/pc/idt.c`
- **hash:** afb1e41506c3bf63

Reserves a contiguous run of `cnt` interrupt vectors, either at a caller-fixed
base or wherever a free run is found.

**Parameters**

- `cnt` — number of consecutive vectors to allocate.
- `flags` — `interrupt_flags_t` bitmask: `interrupt_flags_fixed` requires the run
  to start exactly at `*base`; `interrupt_flags_exclusive` marks the run as
  blocked so it cannot be allocated again.
- `base` — in/out: with `_fixed` it is the requested base; otherwise it is a hint
  (nonzero is tried first as fixed) and receives the chosen base on success.

**Returns** `CS_OK` on success, `CS_UNKN` if no satisfying run exists.

Non-fixed allocation first tries the hinted `*base` as a fixed request, then scans
from vector 32 (device IRQs start there on x86; 0–31 are CPU exceptions) for the
first free run. Only `interrupt_flags_exclusive` allocations are recorded as
blocked; a non-exclusive allocation does not reserve the vectors against future
callers.

---

## `interrupt_mapinterrupt`

- **kind:** function
- **lang:** c
- **source:** `modules/SysInterrupts/src/platform/x86_64/pc/ioapic.c`
- **hash:** 2ad01d4ed937ce50

Programs an I/O APIC redirection entry so global interrupt `line` is delivered as
vector `irq`.

**Parameters**

- `line` — the global system interrupt (GSI) line to route.
- `irq` — the destination IDT vector.
- `active_low` — polarity (true = active-low).
- `level_trig` — trigger mode (true = level, false = edge).

The correct I/O APIC is chosen by finding the one whose `global_intr_base` is the
closest below `line`, then the redirection entry is written via `ioapic_map`.

---

## `interrupt_get_cpu_idx`

- **kind:** function
- **lang:** c
- **source:** `modules/SysInterrupts/src/platform/x86_64/pc/apic.c`
- **hash:** 5d23e0dc817d6099

Returns the local APIC ID of the calling core, used throughout the kernel as the
per-CPU index.

Reads the current core's `apic->id`. Because the APIC pointer is per-core TLS,
this returns a different value on each core.

---

## `interrupt_setmask`

- **kind:** function
- **lang:** c
- **source:** `modules/SysInterrupts/src/platform/x86_64/pc/ioapic.c`
- **hash:** 4f6a08cd6e82fe6a

Masks or unmasks an I/O APIC interrupt `line`.

**Parameters**

- `line` — the global interrupt line to (un)mask.
- `mask` — true to mask (suppress) the line, false to enable it.

Selects the owning I/O APIC the same way as `interrupt_mapinterrupt` and toggles
the entry's mask bit via `ioapic_setmask`.

---

## `interrupt_sendipi`

- **kind:** function
- **lang:** c
- **source:** `modules/SysInterrupts/src/platform/x86_64/pc/apic.c`
- **hash:** 46c03ddfcafdee11

Sends an inter-processor interrupt to a target core through the local APIC ICR.

**Parameters**

- `cpu` — destination APIC ID.
- `vector` — interrupt vector to deliver (ignored for INIT/STARTUP modes by the
  hardware, but still encoded — STARTUP uses it as the page number).
- `delivery_mode` — `ipi_delivery_mode_t`: `fixed`, `init`, or `startup`.

Encodes vector + delivery mode + the assert bit, then writes the ICR. The code
handles both x2APIC mode (single 64-bit ICR write with the destination in the
high dword) and legacy xAPIC mode (separate high/low ICR writes with the
destination in bits 56–63). INIT/STARTUP modes are the AP bring-up sequence used
by `SysMP`.

---

## `interrupt_setstack`

- **kind:** function
- **lang:** c
- **source:** `modules/SysInterrupts/src/platform/x86_64/pc/gdt.c`
- **hash:** c9dab13495aa1970

Sets the kernel stack (`TSS.rsp0`) the CPU switches to on a privilege-level
transition into ring 0.

**Parameters**

- `stack` — top-of-stack pointer to install as `rsp0`.

Writes the calling core's TSS, so each core must call it with its own stack.

---

## `interrupt_set_register_state`

- **kind:** function
- **lang:** c
- **source:** `modules/SysInterrupts/src/platform/x86_64/pc/idt.c`
- **hash:** b52e577de80b3d24

Overwrites the saved register frame of the currently-interrupted context, so the
interrupt returns into a different state (the mechanism a scheduler uses to
context-switch).

**Parameters**

- `state` — full `interrupt_register_state_t` to install; a NULL pointer is a
  no-op.

Must be called from within an interrupt handler: it writes through
`idt->reg_ref`, the per-core pointer to the on-stack saved frame, and `PANIC`s if
that pointer is NULL (i.e. called outside interrupt context). Note `state->rsp` is
written to the frame's `useresp` slot.

---

## `interrupt_get_register_state`

- **kind:** function
- **lang:** c
- **source:** `modules/SysInterrupts/src/platform/x86_64/pc/idt.c`
- **hash:** 949a5927a372199a

Copies the saved register frame of the currently-interrupted context out into
`state`.

**Parameters**

- `state` — destination `interrupt_register_state_t`; NULL is a no-op.

The read counterpart of `interrupt_set_register_state`; same interrupt-context
requirement and `PANIC` on a NULL `idt->reg_ref`. Together they let `SysMP`'s
`mp_platform_getstate`/`setstate` snapshot and restore a thread.

---

## `interrupt_msi_register_addr`

- **kind:** function
- **lang:** c
- **source:** `modules/SysInterrupts/src/platform/x86_64/pc/intr.c`
- **hash:** f8acd5607e744bc1

Computes the MSI message *address* value steering a message-signalled interrupt
to a given core.

**Parameters**

- `cpu_idx` — destination APIC ID (placed in the destination field).

**Returns** the 32-bit MSI address (`0xFEE00000` base, destination in bits
12–19, fixed/physical destination mode). Pair with `interrupt_msi_register_data`
when programming a device's MSI/MSI-X capability.

---

## `interrupt_msi_register_data`

- **kind:** function
- **lang:** c
- **source:** `modules/SysInterrupts/src/platform/x86_64/pc/intr.c`
- **hash:** 39c50a19cd7ca3f5

Computes the MSI message *data* value carrying the delivered vector.

**Parameters**

- `vec` — the interrupt vector the device should raise.

**Returns** the MSI data word (the low byte holds the vector). The companion of
`interrupt_msi_register_addr`. When wiring MSI for a device, register the handler
and these address/data values first and enable the device's own MSI mask last, so
an early edge-triggered message cannot fire and be lost before a handler exists.

---

## `interrupt_register_state`

- **kind:** struct
- **lang:** c
- **source:** `modules/inc/SysInterrupts/interrupts.h`
- **hash:** 50fbcf8908aefef9

The full general-purpose + control register snapshot of an interrupted context,
as laid out on the interrupt stack frame (`x86_64`).

Holds the GPRs (`r15`…`rax`), `rflags`, `rip`, `cs`, `ss`, `rbp`, and `rsp`. It
is the unit traded by `interrupt_get_register_state` /
`interrupt_set_register_state` and, one level up, by `SysMP`'s platform-state
calls — i.e. the saved thread context the scheduler manipulates. Defined only
under `__x86_64__`.

---

## `interrupt_flags_t`

- **kind:** enum
- **lang:** c
- **source:** `modules/inc/SysInterrupts/interrupts.h`
- **hash:** 4eb44716f1a1eca9

Allocation flags for `interrupt_allocate`.

`interrupt_flags_none` (0), `interrupt_flags_exclusive` (reserve the run so it
can't be allocated again), `interrupt_flags_fixed` (require the run to start at
the caller-supplied base).

---

## `ipi_delivery_mode_t`

- **kind:** enum
- **lang:** c
- **source:** `modules/inc/SysInterrupts/interrupts.h`
- **hash:** 835082f7fc84ed49

Inter-processor-interrupt delivery modes for `interrupt_sendipi`.

`ipi_delivery_mode_fixed` (0, ordinary vector delivery), `ipi_delivery_mode_init`
(5, INIT assert), `ipi_delivery_mode_startup` (6, SIPI). The values match the
local APIC ICR delivery-mode encoding; INIT followed by STARTUP is the standard
AP wake sequence.

---

## `InterruptHandler`

- **kind:** typedef
- **lang:** c
- **source:** `modules/inc/SysInterrupts/interrupts.h`
- **hash:** 4f3e7b9495181e4c

Type of an interrupt handler callback: `void (*)(int)`.

The single `int` argument is the vector number the handler was invoked for.
Registered via `interrupt_register_handler`.

---

## `mp_tls_setup`

- **kind:** function
- **lang:** c
- **source:** `modules/SysMP/src/platform/x86_64/pc/mp.c`
- **hash:** f13d66b76493b4da

Allocates and installs this core's thread-local-storage block (a 16 KiB region
pointed to by `GS_BASE`).

**Returns** `CS_OK` (0) on success, `-1` if the allocation fails.

Each core calls this for itself during bring-up. The block is zeroed before use —
static `TLS` variables across the kernel assume zero-initialisation and gate their
per-core setup on NULL/zero checks, so an un-zeroed block would let an AP skip its
own initialisation. After this returns, TLS offsets allocated by `mp_tls_alloc`
are valid on the calling core.

---

## `mp_tls_alloc`

- **kind:** function
- **lang:** c
- **source:** `modules/SysMP/src/platform/x86_64/pc/mp.c`
- **hash:** 1f3d019c89c8edcb

Reserves `bytes` of per-core TLS and returns the byte offset within every core's
TLS block.

**Parameters**

- `bytes` — size to reserve.

**Returns** the offset of the new slot (pass to `mp_tls_get`).

The bump allocator is shared across cores (under a spinlock) and hands out the
*same* offset to every core, which each resolve against their own TLS base.
Allocate slots **once on the BSP before APs come up** so the offset is part of
every core's block. Overflowing the 16 KiB block `PANIC`s.

---

## `mp_tls_get`

- **kind:** function
- **lang:** c
- **source:** `modules/SysMP/src/platform/x86_64/pc/mp.c`
- **hash:** 0357e5f8337889c8

Resolves a TLS offset to a `TLS`-qualified pointer into the calling core's block.

**Parameters**

- `off` — an offset previously returned by `mp_tls_alloc`.

**Returns** a `TLS void*` (GS-relative) for `off`, or NULL if `off` is past the
block size. Dereferencing the result reads/writes this core's copy of the slot.

---

## `mp_corecount`

- **kind:** function
- **lang:** c
- **source:** `modules/SysMP/src/platform/x86_64/pc/mp.c`
- **hash:** cad21359edf07018

Returns the number of cores that have come online so far.

The counter starts at 1 (the BSP) and is incremented by each AP in
`mp_signalready`, so the value is only final once SMP bring-up has completed.

---

## `mp_set_ap_entry`

- **kind:** function
- **lang:** c
- **source:** `modules/SysMP/src/platform/x86_64/pc/mp.c`
- **hash:** c987dcaad7546556

Publishes the entry point that parked application processors will jump into once
the scheduler is online.

**Parameters**

- `entry` — `void (*)(void)` the APs should run; not expected to return.

APs that have signalled ready spin in `mp_signalready` waiting for this pointer to
become non-NULL, then call it (typically to join the scheduler loop). Setting it
once, after the scheduler is up, releases all waiting APs.

---

## `mp_platform_getstatesize`

- **kind:** function
- **lang:** c
- **source:** `modules/SysMP/src/platform/x86_64/pc/mp.c`
- **hash:** 0707f3bba9b8310c

Returns the size in bytes of an architecture thread-context buffer.

**Returns** `sizeof(interrupt_register_state_t)`. Callers allocate a buffer of
this size for `mp_platform_getstate`/`setstate`/`getdefaultstate`.

---

## `mp_platform_getstate`

- **kind:** function
- **lang:** c
- **source:** `modules/SysMP/src/platform/x86_64/pc/mp.c`
- **hash:** 2426bf6876ce46b4

Snapshots the currently-interrupted thread's register state into `buf`.

**Parameters**

- `buf` — buffer of at least `mp_platform_getstatesize()` bytes; NULL `PANIC`s.

A thin platform wrapper over `interrupt_get_register_state`, so the same
interrupt-context requirement applies. This is the scheduler's "save the
outgoing thread" primitive.

---

## `mp_platform_setstate`

- **kind:** function
- **lang:** c
- **source:** `modules/SysMP/src/platform/x86_64/pc/mp.c`
- **hash:** 7fa7473ac1e4f2fa

Installs `buf` as the register state the current interrupt will return into.

**Parameters**

- `buf` — a context buffer (`interrupt_register_state_t` layout); NULL `PANIC`s.

Wraps `interrupt_set_register_state`; the scheduler's "resume the incoming
thread" primitive. Must run inside an interrupt handler.

---

## `mp_platform_getdefaultstate`

- **kind:** function
- **lang:** c
- **source:** `modules/SysMP/src/platform/x86_64/pc/mp.c`
- **hash:** b33e4a9e7dbbc97e

Builds a fresh thread context that will start executing at a given instruction
pointer with a given stack and two argument registers.

**Parameters**

- `buf` — destination context buffer (zeroed first).
- `stackpointer` — initial `rsp`/`rbp`.
- `instr_ptr` — initial `rip`.
- `args0` — value placed in `rdi` (first SysV argument).
- `args1` — value placed in `rsi` (second SysV argument).

Sets ring-0 selectors (`cs=0x8`, `ss=0x10`) and `rflags=0x200` (interrupts
enabled). Used to spawn a new kernel thread: hand the result to
`mp_platform_setstate` from inside the scheduler's interrupt to launch it.

---

## `timer_wait`

- **kind:** function
- **lang:** c
- **source:** `modules/SysTimer/src/main.c`
- **hash:** 20acd28dfd79967a

Blocks the calling core for approximately `ns` nanoseconds using the best
available timer.

**Parameters**

- `ns` — delay in nanoseconds.

Prefers a readable counter (e.g. the calibrated TSC) and polls it — that path is
SMP-safe, any number of cores may call concurrently. If no readable counter
exists it falls back to a per-core (`local`) periodic timer, arming it and
`halt()`ing until the per-core TLS wait state signals completion. `PANIC`s if no
suitable timer is registered. This is a busy/halt wait, not a scheduler
deschedule; for cooperative sleeping that yields the CPU use the scheduler's
`task_sleep` instead (but `task_sleep` is unsafe when already holding `cli()` —
that is exactly when `timer_wait`/`timer_busywait_ns` are appropriate).

---

## `timer_request`

- **kind:** function
- **lang:** c
- **source:** `modules/SysTimer/src/main.c`
- **hash:** 7dde7f5f9bf3193b

Allocates a hardware timer matching the requested feature set and arms it to
invoke `handler`.

**Parameters**

- `features` — `timer_features_t` mask the chosen timer must satisfy (e.g.
  oneshot/periodic, local, write, …).
- `ns` — period/deadline in nanoseconds (used when `timer_features_write` is set).
- `handler` — `void (*)(int)` callback invoked on each fire.

**Returns** 0 on success, `-1` if no registered timer satisfies `features`.

Non-`local` timers are first-come exclusive (marked `in_use`); `local` timers are
per-core hardware behind one registration, so every core may request the same
entry and configure its own state. The nanosecond value is converted to ticks via
the timer's rate before being written.

---

## `timer_timestamp`

- **kind:** function
- **lang:** c
- **source:** `modules/SysTimer/src/main.c`
- **hash:** c88c6f859a2cb41e

Returns the current raw tick count of the persistent counter (TSC/HPET).

**Returns** the counter value, or `TIMER_NO_COUNTER` (`(uint64_t)-1`) if no
readable, persistent counter timer is registered. Use `timer_counter_rate` to
convert ticks to time, or `timer_timestamp_ns` to get nanoseconds directly.

---

## `timer_counter_rate`

- **kind:** function
- **lang:** c
- **source:** `modules/SysTimer/src/main.c`
- **hash:** e4375f16f3f67311

Returns the tick rate (ticks per second) of the persistent counter.

**Returns** the counter frequency in Hz, or 0 if no readable persistent counter
is calibrated. This is the divisor for turning `timer_timestamp` deltas into
seconds; `timer_timeout_start` uses it to decide between a real timed deadline and
the iteration-count fallback.

---

## `timer_timestamp_ns`

- **kind:** function
- **lang:** c
- **source:** `modules/SysTimer/src/main.c`
- **hash:** 3e468e1488c1b652

Returns the current time in nanoseconds from the persistent counter.

**Returns** nanoseconds since the counter's epoch, or `TIMER_NO_COUNTER` if no
readable counter exists or its rate is 0. The conversion is done in integer math
split into whole/fractional ticks (kernel modules build `-mno-sse`, so no float),
and structured to avoid the overflow that `ticks * 1e9` would hit a few seconds
after boot.

---

## `timer_timeout_start`

- **kind:** function
- **lang:** c
- **source:** `modules/SysTimer/src/main.c`
- **hash:** 9397c115df267957

Initialises a bounded busy-wait timeout for `ns` nanoseconds.

**Parameters**

- `t` — `timer_timeout_t` to initialise.
- `ns` — timeout duration in nanoseconds.

If a persistent counter is calibrated, it sets a real wall-clock deadline
(`timed = 1`); the fractional-second tick count is rounded **up** so a sub-tick
timeout never truncates to 0 (which would report expired immediately). With no
calibrated time source it falls back to a bounded iteration cap (`timed = 0`),
which is CPU-speed-dependent but always terminates. Poll with
`timer_timeout_expired`. Pure polling — safe with interrupts disabled, e.g. from a
driver's `module_init`, unlike `task_sleep`.

---

## `timer_timeout_expired`

- **kind:** function
- **lang:** c
- **source:** `modules/SysTimer/src/main.c`
- **hash:** bca7febd0ae82865

Tests (and, in fallback mode, advances) a timeout started by
`timer_timeout_start`.

**Parameters**

- `t` — the timeout being polled.

**Returns** nonzero once the timeout has elapsed. In timed mode it compares the
counter against the deadline; in iteration-fallback mode each call increments the
spin counter and reports expiry once the cap is reached — so it must be called in
a loop for the fallback to make progress.

---

## `timer_busywait_ns`

- **kind:** function
- **lang:** c
- **source:** `modules/SysTimer/src/main.c`
- **hash:** 572b0e412db59070

Busy-waits roughly `ns` nanoseconds for one-off hardware settle/recovery delays.

**Parameters**

- `ns` — delay in nanoseconds.

A convenience wrapper that starts a `timer_timeout_t` and spins on
`timer_timeout_expired`. Counter-paced when a persistent counter is calibrated,
otherwise a bounded spin. Interrupt-disable safe (no scheduler dependency).

---

## `timer_features_t`

- **kind:** enum
- **lang:** c
- **source:** `modules/inc/SysTimer/timer.h`
- **hash:** 6b1651cd7ad7972e

Capability bitmask describing/requesting a timer's features.

Flags include `oneshot`, `periodic`, `read`, `persistent`, `absolute`, `64bit`,
`write`, `local` (per-core hardware), `pcie_msg_intr`, `fixed_intr`, and
`counter`. Passed to `timer_request` to select a timer; the same flags annotate
each registered timer so the selection logic (and `timer_wait`) can find a
readable counter or a per-core periodic source.

---

## `timer_timeout_t`

- **kind:** struct
- **lang:** c
- **source:** `modules/inc/SysTimer/timer.h`
- **hash:** a6a2d58a08028e82

State for a bounded busy-wait timeout.

Holds `deadline` (counter ticks at which a timed timeout fires), `spin` /
`spin_cap` (the iteration counter and cap used by the no-counter fallback), and
`timed` (1 = using the persistent counter, 0 = fallback). Initialised by
`timer_timeout_start` and consumed by `timer_timeout_expired`.

---

## `TIMER_NO_COUNTER`

- **kind:** macro
- **lang:** c
- **source:** `modules/inc/SysTimer/timer.h`
- **hash:** 440d36f5598e37d8

Sentinel `((uint64_t)-1)` returned by `timer_timestamp` / `timer_timestamp_ns`
when no readable counter timer is registered.

Callers that must tolerate a counter-less platform should compare against this
before treating the return value as a real timestamp.

---

## `fp_mp_init`

- **kind:** function
- **lang:** c
- **source:** `modules/SysFP/src/main.c`
- **hash:** 174a50a09bda2d96

Per-core FPU/SSE initialisation, run on each application processor during AP
bring-up.

**Returns** 0 on success.

Delegates to the same `fp_platform_init` the BSP runs in `module_init`: it clears
`CR0.EM`, sets `CR0.MP`, enables `CR4.OSFXSR`/`OSXMMEXCPT` (and `OSXSAVE` plus the
XSAVE feature mask when XSAVE is available), and `fninit`s the FPU. **Every AP
must call this** (it is wired into `apscript.txt`); skipping it leaves SSE/XSAVE
unconfigured on that core, so the first `xrstor`/SSE use there faults (`#UD`).

---

## `fp_platform_getstatesize`

- **kind:** function
- **lang:** c
- **source:** `modules/SysFP/src/platform/x86_64/plat.c`
- **hash:** 7ea05ed9ad11d6b1

Returns the byte size of an FPU state save area.

**Returns** `xsave_sz + 64` when XSAVE is supported (sized from the registry's
reported XSAVE area), otherwise 512 (the `fxsave` area). Allocate a buffer of this
size — and align it per `fp_platform_getalign` — before calling
`fp_platform_getstate`.

---

## `fp_platform_getalign`

- **kind:** function
- **lang:** c
- **source:** `modules/SysFP/src/platform/x86_64/plat.c`
- **hash:** f950812da0c60591

Returns the required alignment of an FPU state buffer.

**Returns** 64 bytes when XSAVE is in use, otherwise 16 (the `fxsave` alignment).
The save/restore instructions fault on a misaligned buffer, so honour this when
allocating.

---

## `fp_platform_getstate`

- **kind:** function
- **lang:** c
- **source:** `modules/SysFP/src/platform/x86_64/plat.c`
- **hash:** fde7642d6150471b

Saves the calling core's full FPU/SSE state into `buf`.

**Parameters**

- `buf` — buffer of `fp_platform_getstatesize()` bytes, aligned to
  `fp_platform_getalign()`.

Emits `xsaveq` (all components) when XSAVE is available, else `fxsaveq`. The
scheduler's "save FP context" half; pair with `fp_platform_setstate`.

---

## `fp_platform_setstate`

- **kind:** function
- **lang:** c
- **source:** `modules/SysFP/src/platform/x86_64/plat.c`
- **hash:** 0fb7ef6653cad164

Restores FPU/SSE state from `buf` into the calling core.

**Parameters**

- `buf` — a state buffer previously filled by `fp_platform_getstate` or
  `fp_platform_getdefaultstate`.

Emits `xrstorq` (XSAVE) or `fxrstorq`. Restoring on a core whose FPU was never
initialised by `fp_mp_init` faults — initialise every core first.

---

## `fp_platform_getdefaultstate`

- **kind:** function
- **lang:** c
- **source:** `modules/SysFP/src/platform/x86_64/plat.c`
- **hash:** 7d768524f5eb143d

Fills `buf` with a clean default FPU state for a new thread.

**Parameters**

- `buf` — buffer of `fp_platform_getstatesize()` bytes (zeroed, then stamped).

Zeroes the area and sets the x87 control word to `0x33F` and `MXCSR` to `0x1F80`
(the standard reset values), so a freshly spawned thread starts with a sane FPU
configuration. Apply it via `fp_platform_setstate`.
