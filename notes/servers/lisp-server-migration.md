# Migrating the Core* servers to Lisp (async actor model)

The kernel-resident Lisp runtime is now the OS scheduler (see
`notes/core/lisp-substrate.md`, phases K1–K5d). The next arc moves the `Core*`
servers off native C — where a driver registers a struct of function pointers and
the server calls them **synchronously** — and onto Lisp **contexts** that signal
drivers **asynchronously** by message passing.

This is forced, not stylistic: a Lisp server context and a Lisp driver context are
**shared-nothing** (separate heaps, no shared mutable state), so one cannot
synchronously call into the other. Every callback in the old ABI becomes a message.

## The contract (worked example: the input service)

The first server migrated is **CoreInput** (slices 1a/1b, commits on
`claude/kernel-lisp`). It is the template:

- The **server** is a long-lived context: a registration table + a `(recv)` loop
  that dispatches on the message tag.
- A **driver** context sends it messages. For input (a one-way event stream) two
  tags suffice:
  - `(register <name>)` — the driver announces itself; the server records it.
  - `(event <payload>)` — one input event.
- The server never calls back into the driver synchronously, so the
  network-style *"rx handler re-enters the same driver's tx and self-deadlocks"*
  gotcha (all over `CLAUDE.md`) **cannot arise** — a reply is just another message.

For **command-style** servers (storage `read`/`write`, display `flush`) the same
shape gains a request/response pair: the server sends `(request <id> <op> <args>)`
to the driver and the driver answers `(complete <id> <result>)`. The synchronous
`int (*read)(state, lba, n, buf)` becomes submit-then-complete.

### Driver side (the ISR → wake → poll → send loop)

A migrated driver keeps its device-poking C (port/MMIO/decode) but loses its
native poll task and synchronous registration. Instead:

1. Its IRQ handler is a minimal **native ISR** that queues the hardware event and
   calls a hook that does `lisp_ctx_wake(<the driver context>)` — ISR-safe, just a
   word write. (ps2: `ps2_set_irq_hook` + `ps2_wake_hook` in SysLisp.)
2. A small **FFI primitive** dequeues one decoded event as a Lisp value
   (`%ps2-poll` → `(key <scancode> <pressed?>)` or `#f`).
3. The **driver context** is pure Lisp: `register`, then pump — drain the FFI,
   `send` each event to the server, and park on a wait primitive (`%ps2-wait`,
   which `cli`-guards the check-then-park window against the same-core IRQ) when
   the queue is empty. The IRQ wakes it. This is the universal completion path
   (the same shape serves DMA/disk/NIC completions and timers).

The IOAPIC RTE must be programmed for a legacy line (`interrupt_mapinterrupt`),
not just the handler registered + the line unmasked — otherwise the device raises
the IRQ but it is never delivered (this bit the ps2 migration).

## Constraints (current first cut)

- **Co-located on the BSP.** K5d left contexts as per-core islands (no cross-core
  `send`; a context handle never crosses cores). So a server and its drivers must
  share one core; long-lived services run on the BSP. Cross-core service placement
  waits on cross-core messaging.
- **Bulk data — now available.** The mutable byte-buffer type (`LISP_OBJ_BYTES`)
  is both the driver MMIO/DMA region and the bulk-data message: `send` deep-copies
  it (a snapshot into the receiver's heap), so packets / disk blocks can cross a
  context boundary. (Zero-copy shared buffers across contexts are still future;
  copy-on-send is the shared-nothing default.)
- **Heavy protocol logic: decide per server.** The Lisp server can be a thin async
  *coordinator* over the existing C protocol logic (FFI), or a genuine rewrite.
  The async message contract is fixed now; the coordinate-vs-rewrite call is made
  when each heavy server (network, storage) is actually tackled.

## Driver substrate (available now)

The Lisp toolkit for writing drivers (commits 1812bdc / e5378ac / ca0f3c1):

- **Bitwise / bitfield** (base prims): `bitwise-and/or/xor/not`,
  `arithmetic-shift`, `bit-extract`/`bit-insert` (register fields). Fixnums carry
  62 bits — covers any u32 register.
- **Mutable byte buffers** (`LISP_OBJ_BYTES`): `make-bytes`, `bytes-length`,
  `bytes-phys`, and volatile little-endian `bytes-u{8,16,32,64}-{ref,set!}`.
- **MMIO / DMA / port I/O** (SysLisp, kernel): `(mmio-map phys size)` and
  `(dma-alloc size)` return byte buffers over a device BAR / a contiguous DMA
  region (`bytes-phys` gives the address to program into the device);
  `in-u{8,16,32}` / `out-u{8,16,32}` for legacy ports. Unrestricted today —
  capability-gated later.
- **Register/field DSL** (prelude, closures, no macros): `(register region off
  size)` → a read/write accessor; `(field reg lo width)` → a bit-range accessor
  (read = extract, write = read-modify-write). Constants bake into the closures.

Example: `(define ctrl (register (mmio-map bar #x1000) #x40 4))` then
`(define speed (field ctrl 4 3))`, `(speed 5)` / `(speed)`.

## Status / order

- **CoreInput — done (Lisp).** ps2 keyboard migrated; events flow
  IRQ→wake→poll→send→coreinput. The native `servers/CoreInput` C module is
  **superseded** and no longer loaded (it left the boot path when `servicescript`
  was retired in K5b). Mouse is still queued by ps2 but not yet pumped to Lisp.
- **Driver substrate — done** (bitwise + byte buffers + MMIO/DMA/port-IO +
  register/field DSL; see above). The bulk-data message type is in place.
- **Next:** migrate a real device driver in Lisp using the substrate (a NIC is the
  natural first target — it pairs with the passive CoreNetwork), then
  **CoreStorage** (request/response) and **CoreNetwork** (where the no-re-entrancy
  win pays off most). Wire each migrated driver into `loadscript` before SysLisp
  so its FFI symbols resolve.

## Testing

The pipeline is provable host-side (stub the driver FFI, run the scheduler — see
the `test_*.c` harnesses) and in QEMU. For input, `scripts/run-qemu.sh SENDKEY=a,b`
injects keystrokes via the qemu monitor (headless, over a VNC console so the input
subsystem has somewhere to route) and the coreinput context prints the events.
