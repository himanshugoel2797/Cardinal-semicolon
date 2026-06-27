# Debug the OS

*A practical guide to the three debug surfaces in Cardinal; — from boot-log
triage to live context inspection.*

Cardinal; provides three complementary debug surfaces. Understanding which one
to reach for first saves time:

| Surface | What it covers | When to use it |
|---------|---------------|----------------|
| **COM1 boot log / `DEBUG_PRINT`** | Kernel + C module output, baked-in traces | First look: boot panics, module load order, early driver state |
| **Interactive Lisp REPL on COM1** | Live Lisp evaluation in the running OS; component log query via `log-*` prims | Prod drivers and services while the OS runs; read per-source logs with `(log-dump "src")` |
| **`sys-debug` context inspector** | Pause, step, and inspect Lisp contexts | Track down a spinning or wedged Lisp server; single-step Lisp code |

All three surfaces share COM1. The boot log streams raw until the REPL starts;
once the REPL takes COM1, component logs go to an in-memory per-source store
and are read back at the REPL with `log-*` primitives.

---

## 1. The COM1 boot log and `DEBUG_PRINT`

Every kernel module and Lisp driver emits traces through `DEBUG_PRINT(...)`,
which goes to COM1. When you boot headlessly via `run-qemu.sh`, COM1 is
mapped to stdio:

```bash
./scripts/run-qemu.sh
# COM1 output streams directly to your terminal.
# Save it: ./scripts/run-qemu.sh 2>&1 | tee boot.log
```

The default `TIMEOUT` is 30 s; set `TIMEOUT=0` to let the guest run
indefinitely (useful when waiting for a slow driver to finish probing):

```bash
TIMEOUT=0 ./scripts/run-qemu.sh
```

### Batched-log flush

The Lisp `display`/`newline` path is **buffered**. If a driver panics or
hangs mid-sequence, the last few log lines may not have been flushed yet.
The pattern used throughout the drivers is an explicit `(console-flush)` at
the end of a critical section — the same call `start-repl` makes after
printing its banner (`lisp/init.clp`, line 784). If you are adding temporary
traces to a driver and the output disappears at the crash point, add a
`(console-flush)` right after the last `(display ...)` you expect to see.

### Useful `run-qemu.sh` knobs for log triage

```bash
# Attach a NIC + capture the network traffic:
NIC=virtio-net NET_PCAP=/tmp/cap.pcap ./scripts/run-qemu.sh

# Attach a disk image (drives the ahci block-device driver):
DISK=/tmp/test.img ./scripts/run-qemu.sh

# Add an HD Audio controller so the hdaudio driver probes:
AUDIO=hda ./scripts/run-qemu.sh

# Run SMP=1 to serialise context scheduling and expose race-like bugs:
SMP=1 ./scripts/run-qemu.sh
```

All knobs are documented at the top of `scripts/run-qemu.sh`.

---

## 2. The interactive Lisp REPL

The REPL is an opt-in feature gated on the kernel command line. When the kernel
boots with `cardinal.repl`, `system-init` in `lisp/init.clp` calls
`(start-repl)`, which `spawn`s a **root-authority context** that:

1. Imports `sys-console` and `sys-irq`.
2. Claims COM1 RX as ISA IRQ 4 via `(irq-register 4)`.
3. Arms the UART receive interrupt with `(console-arm-rx)`.
4. Binds `(play-tone)` and `(set-vol)` into the REPL environment so you can
   call them bare at the prompt.
5. Parks in an IRQ-driven loop — `(console-poll)` for input, `(irq-wait …)`
   between keystrokes — so the BSP idles when nothing is typed (no busy-poll).

### The serial link is raw — no framing

Under `cardinal.repl`, once `start-repl` runs, the REPL takes COM1. There is
no framing protocol: the wire is a plain 115200-8N1 serial line. Before the
REPL starts, the boot log streams raw to COM1 exactly as a normal boot does.
After `start-repl` completes, component logs no longer go to the serial line —
they are written to an **in-memory per-source log store** (`modules/SysDebug/src/logstore.c`)
and read back at the REPL with the `log-*` primitives (see below).

### Launching the REPL with `serial-repl.py`

Build the REPL ISO with the `repl-image` CMake target, then launch the raw
serial terminal (it boots `build/ISO/os-repl.iso` under QEMU by default):

```bash
cmake --build build --target repl-image   # -> build/ISO/os-repl.iso
python3 scripts/serial-repl.py            # launches QEMU, relays COM1 raw
# ... boot log scrolls by ...
# [repl] serial REPL ready on COM1 -- try (play-tone)
(+ 1 2)
3
(play-tone 440 4000 9600)
playing
```

Drive the REPL non-interactively (for tests) with `--exec` (repeatable) or
`--script FILE`, which wait for the `serial REPL ready` banner, send each line,
print the output, and exit:

```bash
python3 scripts/serial-repl.py --exec '(+ 1 2)' --exec '(play-tone)'
```

On real hardware, point the same tool at the adapter instead of QEMU:

```bash
python3 scripts/serial-repl.py --serial-device /dev/ttyUSB0
```

Because the spawned context has full (root) import authority, you can
`(import sys-debug)` at the prompt and immediately use the context-inspection
primitives described in the next section.

!!! warning "REPL authority"
    The REPL context has root authority — it can `(import sys-pci)` and reach
    new hardware. Do not boot production images with `cardinal.repl`.

### Per-source log store

Once the REPL owns COM1, every `(log "src" ...)` call from a driver or server
appends to a ring buffer keyed by source name (`modules/SysDebug/src/logstore.c`).
The REPL provides four global primitives to read them back:

| Primitive | Description |
|-----------|-------------|
| `(log-sources)` | Newline-separated string of all known source names |
| `(log-dump "src")` | Full buffered log for `src`, oldest-first, as a string |
| `(log-tail "src" n)` | Last `n` lines of `src` as a string |
| `(log-clear "src")` | Discard the buffer for `src` |

**Worked example — reading the nvme driver's log:**

```scheme
; 1. Boot with cardinal.repl, wait for the ready banner, then:
(display (log-sources))
; ahci
; nvme
; corenetwork
; ...

; 2. Dump everything logged by the nvme driver since boot:
(display (log-dump "nvme"))
; nvme: BAR0 phys=0xd0000000 size=0x4000
; nvme: CC.EN=0 -> 1 (controller reset)
; nvme: CSTS.RDY within 50ms
; nvme: identify ctrl ok, nn=1
; nvme: ns1: 2097152 sectors (1 GiB)

; 3. Only the last 5 lines:
(display (log-tail "nvme" 5))
```

Each module uses `(log "src" arg...)` directly (args rendered display-style:
strings bare, other values via the printer), or gets a convenience closure via
`make-logger`:

```scheme
(define lg (make-logger 'nvme))
(lg "found " n " queues")
; equivalent to: (log "nvme" "found " n " queues")
```

`log` and `make-logger` are global (available in any context, including
restricted drivers, just like `display`). The `log-*` query primitives are
also global — call them at the REPL without any `import`.

---

## 3. Live Lisp debugging with `sys-debug`

`sys-debug` (`libs/lisp/src/debug.c`) is a capability-gated module that
exposes reflective primitives over the VM's own context objects.  A context
is its execution state — heap, continuation, value registers — so the
debugger needs almost nothing new from the interpreter: just a handle to a
context and a way to advance it one reduction at a time.

### Getting access

```scheme
(import sys-debug)   ; only succeeds if the current context was granted this capability
```

At the REPL (which has root authority) this always succeeds.

### Listing running contexts

```scheme
(ctx-list)
; => (#<ctx:0x...> #<ctx:0x...> ...)
```

`ctx-list` returns the live run queue of **this core's scheduler** — the
contexts the scheduler is currently cycling through.  It does not span all
cores.

### Reading a context's state

```scheme
(ctx-status c)    ; => eval | apply | done | error
(ctx-blocked? c)  ; => #t if parked waiting for a message, #f if runnable
(ctx-control c)   ; => the expression the context is about to evaluate (EVAL state)
(ctx-error c)     ; => error string, or #f
```

`ctx-blocked?` is the key predicate for distinguishing a **parked server**
(normal — it is `recv`-blocked waiting for messages) from a **busy-spinning
context** (pathological — `ctx-blocked?` returns `#f` and it is consuming the
core).

### Pausing and stepping a scheduler-owned context

```scheme
(define c (car (ctx-list)))   ; grab a live context
(ctx-pause c)                 ; mark it blocked — the scheduler skips it
(ctx-step c)                  ; advance one reduction; returns: done | error | suspended
(ctx-step c 10)               ; advance up to 10 reductions
(ctx-control c)               ; see what it is about to do
(ctx-unpause c)               ; return it to the scheduler
```

!!! warning "Only step contexts you own or have paused"
    The comment at the top of `debug.c` is explicit: **stepping a
    scheduler-owned context without first calling `ctx-pause` races the
    scheduler**.  On a cooperative single-core the target is not running when
    you call `ctx-pause` (only one context runs at a time), so the pause is
    race-free.  Under SMP, a context on a different core is genuinely
    concurrent; `ctx-pause` only sets a flag and does not IPI-halt the remote
    core, so multi-core stepping needs care.

### Creating an isolated context for step-debugging

```scheme
(define c (ctx-make (lambda () (my-suspect-function arg1 arg2))))
(ctx-step c 100)      ; => suspended  (budget ran out mid-computation)
(ctx-control c)       ; what reduction is next?
(ctx-step c)          ; one more step
(ctx-value c)         ; once (ctx-step ...) returns 'done, read the result
(ctx-error c)         ; or the error
```

The thunk passed to `ctx-make` is a closure, so it captures the surrounding
lexical scope.  The new context starts paused and is **never on the scheduler
queue** — only `ctx-step` drives it.  The created context inherits the
capability grant of its maker (no escalation).

!!! note "`ctx-status` vs the return of `ctx-step`"
    `ctx-status` reports the stored register state (`eval`/`apply`/`done`/`error`).
    `ctx-step` returns `suspended` when the reduction budget ran out but the
    context is still runnable — `ctx-status` will say `eval` or `apply` in
    that case, not `suspended`.  Use the return value of `ctx-step` to drive
    a step loop; use `ctx-status` to classify a paused context you did not
    step yourself.

---

## 4. Interrupt-delivery / "nothing is firing" debugging

If an interrupt handler never runs or an MSI-triggered callback is silent,
the most common root causes are:

### IF=0 (interrupts disabled)

The Lisp eval loop historically ran with interrupts disabled (`cli`).  A device
MSI would be delivered to the LAPIC, pend, and never fire because `IF=0` meant
the CPU never acknowledged it.  The fix — `sti` in `lisp_core_loop` — is in
place on the current tree, but this is worth checking if you re-introduce C
code that wraps a long section with `cli`.

QEMU diagnostic:

```bash
# In the QEMU monitor (Ctrl-A c in the default stdio mode):
info lapic       # shows IRR/ISR bits — a stuck bit here means the CPU saw the
                 # interrupt but IF=0 prevented dispatch

# QEMU debug log (add to the qemu invocation):
-d int           # prints every interrupt vector delivery to stderr
```

### MSI handler registered after device enabled

The canonical PCI driver bring-up order is:

1. Call `pci_setup_msi_handler()` to register the handler (this arms the
   handler table entry).
2. Enable the device's own interrupt mask **last** (the device can now fire).

Reversing the order means an edge-triggered MSI can fire before anything is
listening and be silently lost, wedging interrupts permanently until the next
MSI edge.  This is documented in `CLAUDE.md` under "PCI drivers".

### RX-handler self-deadlock

A receive handler that runs the network stack synchronously can re-enter the
same driver's TX path when a frame needs a reply (ARP, ICMP, a UDP service).
Holding a driver lock across that call self-deadlocks the moment a
reply-triggering frame arrives — the RX ring then fills and frames stop being
processed. The Lisp driver model avoids this structurally (separate RX and TX
contexts, non-blocking `send`); see
[Message passing & concurrency](../concepts/message-passing.md#driver-rx-handlers-may-re-enter-the-tx-path)
for the full rule and [Add a PCI driver](add-a-pci-driver.md#6-the-rx-handler-locking-rule)
for the driver-side checklist.

---

## 5. KVM vs TCG

Most bugs reproduce under both accelerators.  A few categories are
accelerator-specific:

| Symptom | Likely accelerator to test under |
|---------|----------------------------------|
| APIC-timer / scheduler timing bugs | **KVM** (real APIC timing; TCG is too slow to trigger races) |
| Instruction-emulation bugs (rare opcodes, privilege checks) | **TCG** (full software emulation catches mis-encodings KVM passes through) |
| Bare-metal bring-up failures | Real hardware; KVM is the closest proxy |

`run-qemu.sh` defaults to `ACCEL=auto` (KVM if `/dev/kvm` is accessible,
TCG otherwise).  Force one or the other:

```bash
ACCEL=kvm  ./scripts/run-qemu.sh   # hardware-accelerated
ACCEL=tcg  ./scripts/run-qemu.sh   # software emulation
```

The `apic-timer` ordering fix (see memory note `boot-kvm-hang-use-tcg.md`)
is already merged; branches on or after that commit boot cleanly under KVM in
seconds.

!!! tip "SMP=1 for serializing races"
    `SMP=1` forces a single BSP with no APs.  Scheduler-related races and
    driver-init ordering bugs that are timing-sensitive under SMP often become
    deterministic under `SMP=1`, making them easier to bisect.

---

## Next steps

- [Add a PCI driver](add-a-pci-driver.md) — the MSI bring-up sequence in detail
- [Message passing](../concepts/message-passing.md) — how Lisp contexts communicate; relevant to REPL→server calls
- [VM API reference](../vm/capabilities.md#sys-debug-reflective-debugger-capability) — full list of Lisp primitives including `sys-debug`

## See also

- `libs/lisp/src/debug.c` — `sys-debug` primitive implementations and their contracts
- `modules/SysDebug/src/logstore.c` — the per-source ring-buffer log store
- `lisp/init.clp` — `start-repl` and `system-init` (the boot policy and REPL spawn)
- `scripts/run-qemu.sh` — all QEMU knobs documented inline
- `notes/AUDIT.md` — known stubs and intentionally unfinished areas (check here before assuming something is broken)
