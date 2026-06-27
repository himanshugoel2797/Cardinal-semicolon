# ps2

> All-Lisp i8042 PS/2 keyboard + mouse driver that uses a generic ISA-IRQ wake bridge to deliver scancode and pointer events to [coreinput](../servers/coreinput.md).

| | |
|---|---|
| **Source** | `lisp/drivers/ps2.clp` |
| **Kind** | driver |
| **Bound by** | `lisp/init.clp` — always (not PCI-gated); `(ps2-init)` runs in the root init context, then `(ps2-keyboard-driver input)` is spawned as a restricted context |
| **Registers with** | [coreinput](../servers/coreinput.md) via a `(register 'ps2-keyboard)` message |
| **Capabilities** | `sys-io` (legacy port I/O: `in-u8`/`out-u8`), `sys-irq` (ISA-IRQ wake bridge: `irq-register`/`irq-count`/`irq-wait`) |

## Overview

`ps2` is a zero-C driver: every layer from controller bring-up through scancode
decoding is Lisp. The only non-Lisp piece is `lisp_irq_isr` in `SysLisp`, a
shared interrupt-context trampoline that wakes the parked driver context on each
IRQ — alloc and GC are illegal in interrupt context, so this trampoline is generic
and shared by every ISA driver.

The driver owns the i8042 at I/O ports `0x60` (data) and `0x64` (command/status).
Scancode decode is deferred out of interrupt context: the ISR only wakes the driver
context, which then drains the i8042's one-byte output buffer. Because the
Output-Buffer-Full (OBF) bit gates the next IRQ, no byte is ever lost while the
context is scheduled to drain — only throughput drops under load.

**Keyboard and mouse.** `ps2-init` enables the auxiliary (mouse) port and
initializes the mouse; the single pump drains both devices from port `0x60`,
routing each byte to the keyboard or mouse path by the i8042 status mouse bit
(`0x20`). The mouse path reassembles the standard 3-byte movement packet, keeps
an accumulated absolute pointer position, and emits `(pointer x y down?)` events.
If the mouse does not initialize the keyboard still comes up.

The driver sits between the i8042 hardware and [coreinput](../servers/coreinput.md).
No other context interacts with it directly; all keyboard and pointer input
reaches userspace through the coreinput message protocol.

## Initialization

`init.clp` calls both entry points from `setup-input` during `system-init`:

```scheme
(ps2-init)                          ; → #t on success, #f if controller absent
(ps2-keyboard-driver coreinput)     ; → never returns (tail-recursive pump loop)
```

### `(ps2-init)`

Runs in the root init context (which holds `sys-io` authority). Performs the
full i8042 bring-up sequence:

1. Disable both PS/2 ports (`0xAD`, `0xA7`).
2. Flush any stale output bytes from the buffer.
3. Read the controller configuration byte (`0x20`), clear both port-IRQ bits
   (bits 0 and 1), and write the result back (`0x60`) — IRQs are quiet during
   setup.
4. Issue a controller self-test (`0xAA`). Expected reply: `0x55`. If the reply is
   wrong (controller absent or broken), logs `[ps2] controller self-test failed;
   no PS/2 input` and returns `#f`.
5. Re-write the config byte (self-test can reset it on some controllers).
6. Enable port 1 (`0xAE`) and call `(ps2-kbd-init)` to enable scanning.
7. Re-read the config byte and set bit 0 (keyboard IRQ enable), writing it back.
8. Logs `[ps2] keyboard up` and returns `#t`.

Translation (config byte bit 6) is left as the firmware set it. The scancode pump
is set-agnostic (it handles both make-only and `0xF0`-prefixed break codes), so
forcing translation off is not necessary and can change what some i8042s emit.

### `(ps2-kbd-init)`

Called by `ps2-init`. Sends `0xF4` (enable scanning) to the keyboard and consumes
the `0xFA` ACK. Does **not** send `0xFF` (reset) or `0xF0` (set selection):
a BAT reset takes tens of milliseconds and can overrun the bounded waits; the
firmware-enabled default already produces valid scancodes.

### `(ps2-keyboard-driver coreinput)`

Spawned by `init.clp` as a **restricted context with no import authority** — it
inherits `sys-io` and `sys-irq` only through the closed-over module-level
definitions, not through a fresh `import`. The context:

1. Sends a `(register 'ps2-keyboard)` message to `coreinput` (`(send coreinput (list 'register 'ps2-keyboard))`).
2. Claims IRQ 1 with `(irq-register 1)`. Logs and returns `#f` if registration
   fails.
3. Runs `(ps2-irq-selftest irq)` to prove the interrupt pipeline end-to-end.
4. Enters the tail-recursive pump loop: drain → park on IRQ → drain → …

## IRQ handling

The driver uses the generic ISA-IRQ wake bridge rather than a raw interrupt
handler.

```scheme
(define irq (irq-register 1))   ; claim ISA IRQ 1 (keyboard)
(irq-count irq)                 ; → monotonic delivery count (never misses)
(irq-wait  irq seen)            ; park until (irq-count irq) != seen
```

`irq-count` is captured **before** draining, so a key that arrives during the
drain increments the counter and causes the next `irq-wait` to return immediately
(re-drain) instead of parking on a byte already sitting in the buffer. This
pattern is safe against the race: the count-based wake never misses a delivery.

IRQ 12 (mouse) is also registered with `(irq-register 12)` so the aux line is
routed and counted. There is one waiter per IRQ line and no `irq-wait-any`
primitive, so the single pump parks on **IRQ 1** with a short bounded timeout
(~8 ms): a keystroke wakes it immediately, and the timeout makes it re-drain the
shared `0x60` port for mouse activity even when the keyboard is quiet.

## Scancode + mouse pump

`(ps2-drain coreinput state)` drains the i8042 output buffer in a loop, routing
each byte by the i8042 status mouse bit (`0x20`) and forwarding events to
coreinput. `state` is the mutable 3-byte mouse-packet framer.

| Byte read | Action |
|-----------|--------|
| `0xFA` (keyboard) | ACK — ignore (leftover from init commands) |
| `0xF0` (keyboard) | Set-2 break prefix — read the next byte; send a key-release event |
| mouse byte (status bit 5 set) | Feed the 3-byte packet framer (`ps2-mouse-feed`); emit a pointer event on the 3rd byte |
| any other make code | Send a key-press event |

The mouse path reassembles a packet, updates the accumulated `(x y)` and left-
button state, and emits:

```scheme
(send coreinput (list 'event (list 'pointer <x> <y> <left-down?>)))
```

where `left-down?` is `#t`/`#f` (the [compositor](../servers/corecompositor.md)
requires a boolean for the pointer button, unlike the 1/0 key convention).

Events sent to `coreinput`:

```scheme
; key press
(send coreinput (list 'event (list 'key <scancode> 1)))

; key release (set-2 break: 0xF0 <code>; or set-1 make codes with bit 7 set
; are not specially handled — the pump is set-agnostic for make codes only)
(send coreinput (list 'event (list 'key <scancode> 0)))
```

`<scancode>` is the raw byte read from port `0x60`. No translation or keysym
mapping is applied; that is [coreinput](../servers/coreinput.md)'s responsibility.

## Mouse

`ps2-init` brings up the auxiliary device after the keyboard:

1. Enable port 2 (controller command `0xA8`).
2. `(ps2-mouse-init)` — write to the aux device via the `0xD4` prefix:
   `0xFF` (reset; consumes the `0xFA` ACK + `0xAA` self-test-pass + `0x00`
   device id), `0xF6` (set defaults), `0xF4` (enable data reporting); each gated
   on the `0xFA` ACK through the bounded `ps2-read`. A missing/wedged mouse logs
   `[ps2] no mouse (aux init failed); keyboard only` and leaves the aux IRQ
   masked — the keyboard is unaffected.
3. Re-read the config byte and set bit 1 (aux IRQ) **only if the mouse ACK'd**,
   and clear bit 5 (aux-clock-disable) so port 2 is clocked.

The standard 3-byte movement packet is `(flags, dx, dy)`: `flags` carries the
button states (bit 0 left, 1 right, 2 middle), the X/Y sign bits (4/5), the
overflow bits (6/7), and an always-1 bit (3) used for resync. `dx`/`dy` are
reconstructed as 9-bit signed values; **`dy` is negated** because PS/2 reports
positive-up while screen Y is positive-down. The driver maintains an accumulated
absolute position over a fixed **1024×768** space (there is no screen-size source
in the driver), clamped to `[0,1023] × [0,767]`, starting centred.

## Self-test / smoke-test

```scheme
(ps2-irq-selftest irq)
```

Called once at startup, after IRQ 1 is enabled, to prove the interrupt pipeline
without requiring a physical keypress:

1. Capture the current `irq-count`.
2. Send `0xEE` (echo command) to the keyboard via `ps2-write` (waits for IBF
   clear first).
3. Park with `irq-wait` until IRQ 1 fires.
4. Read back up to 5 bytes (4 retries) looking for `0xEE`. Any byte that raced the
   just-unmasked line ahead of the reply is consumed and discarded so it does not
   leak into the pump as a bogus key event.
5. Logs `[ps2] irq self-test ok (echo via IRQ 1)` on success, or
   `[ps2] irq self-test: no echo reply` if the echo never arrived.

Headless smoke tests can assert the `irq self-test ok` log line to confirm the
interrupt path is working without needing physical key input.

## Exported functions

Only two symbols are exported from the `ps2` module:

### `(ps2-init)`

Bring up the i8042 controller and keyboard. Returns `#t` on success, `#f` if the
controller self-test fails (no PS/2 hardware or controller absent). Must be called
from a context that holds `sys-io` authority. See [Initialization](#initialization).

### `(ps2-keyboard-driver coreinput)`

The IRQ-driven keyboard + mouse pump. Takes a handle to the running
[coreinput](../servers/coreinput.md) context. Registers as `'ps2-keyboard`,
claims IRQ 1 and IRQ 12, runs the echo self-test, then loops forever delivering
scancode and pointer events. Never returns under normal operation.

### Pure mouse helpers (host-testable)

Exported so a host unit test (`libs/lisp/test/test_ps2_mouse.c`) can drive them
over mock packet bytes without hardware:

- `(ps2-mouse-decode flags b1 b2)` → `(dx dy left? right? middle?)` — 9-bit
  signed reconstruction with the screen-Y negation and `#t`/`#f` buttons.
- `(ps2-mouse-accum x y dx dy)` → `(new-x new-y)` — accumulate + clamp to
  1024×768.
- `(ps2-mouse-sync? flags)` — the byte-0 "always-1" (bit 3) resync check.
- `(ps2-mouse-new-state)` — a fresh framer vector (pointer centred, button up).

## I/O port map

| Port | Direction | Use |
|------|-----------|-----|
| `0x60` | R/W | Data: read scancodes / write keyboard commands |
| `0x64` | R | Status register (OBF bit 0, IBF bit 1, mouse-byte bit 5) |
| `0x64` | W | Command register (controller commands) |

All reads and writes go through bounded-spin helpers (`ps2-wait-read`,
`ps2-wait-write`) with a cap of 1 000 000 iterations. A wedged or absent
controller (IBF stuck set) cannot hang boot indefinitely.

## Notes / gotchas

**QEMU does not deliver injected keyboard *or mouse* input to the PS/2 device.**
Keys sent via QEMU's `sendkey` HMP command, the QMP `send-key`/`input-send-event`
APIs, or VNC/RFB events are NOT routed through the emulated i8042. So the live
keyboard path is verified by the `0xEE` echo self-test, and the **mouse pointer
path is verified only by the host unit test** (`test_ps2_mouse.c`, over mock
packet bytes) and on real hardware — the i8042 aux *bring-up* does complete under
QEMU (the emulated mouse ACKs), but no synthetic movement reaches it.

**No keyboard reset (`0xFF`) at init.** Sending a BAT reset takes tens of
milliseconds and produces a long ACK stream that can overrun the bounded waits and
desync the init sequence. The firmware-configured power-on default (scanning
enabled) is used directly.

**Set-agnostic pump.** The pump recognises the set-2/3 break prefix `0xF0` but
otherwise forwards raw make codes without interpreting the active scancode set.
Firmware translation (config byte bit 6) is left as-is; the active set does not
affect the driver's correctness.

**Port-I/O capability is not re-imported by the pump context.** `ps2-keyboard-driver`
runs in a restricted `spawn-restricted '()` context (no import authority). It
reaches `in-u8`/`out-u8` and `irq-*` only through the module-level closures
captured at define time — the `sys-io` and `sys-irq` imports belong to the `ps2`
module, not to the spawned context. This is intentional: the pump needs hardware
access but must not be able to `import` new capabilities at runtime.
