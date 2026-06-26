# usb-hid

> USB HID boot-protocol class driver — claims keyboard and mouse devices, polls interrupt-IN endpoints for reports, and forwards key events to [coreinput](../servers/coreinput.md).

| | |
|---|---|
| **Source** | `lisp/drivers/usb-hid.clp` |
| **Kind** | driver |
| **Bound by** | `lisp/init.clp` — always; `(usb-hid-init usb input)` is called unconditionally before USB host controllers are started |
| **Registers with** | [coreusb](../servers/coreusb.md) via `register-class` (class byte `0x03` / `USB-CLASS-HID`) |
| **Capabilities** | None — no `sys-*` / hardware grants. Imports `coreusb` (transfer API + USB constants) and `driver-util` (`serve`); `spawn-restricted`, `self`, `send`, `recv` are VM primitives |

## Overview

`usb-hid` is a zero-hardware-capability USB class driver for HID devices. The
class-driver context and every per-device poll context hold no hardware grants —
all transfers are messages to the host controller context that [coreusb](../servers/coreusb.md)
owns. The driver restricts itself to the USB 2.0 boot protocol (no HID report
descriptor parsing) and currently decodes only keyboard reports.

On probe, it claims any device whose `bInterfaceClass` is `0x03` (HID) and that
exposes at least one interrupt-IN endpoint. It forces the device into boot
protocol, then spawns a per-device `spawn-restricted` context that polls the
endpoint in a tight loop. Decoded key-down/key-up events are forwarded as
messages to the coreinput service handle passed at init time. Mouse devices are
claimed and polled but their reports are not yet decoded (see Notes).

## Initialization

`init.clp` calls this entry point once, before starting any USB host controller:

```scheme
(usb-hid-init usb input)
```

- `usb` — context handle of the running [coreusb](../servers/coreusb.md) service.
- `input` — context handle of the [coreinput](../servers/coreinput.md) service.

`usb-hid-init` spawns a long-lived `serve` context (the class-driver context)
that maintains a list of active `(bus-address . poll-ctx)` pairs, then sends:

```scheme
(send usb (list 'register-class USB-CLASS-HID ctx))
```

where `USB-CLASS-HID` = `#x03`. From this point on, coreusb dispatches every
newly enumerated HID device to the class-driver context as a `probe` message.

`usb-hid-init` returns the class-driver context handle (rarely needed by callers
— `init.clp` discards it).

## coreusb Integration

### Class matching

coreusb's enumerator reads `bInterfaceClass` from the first interface descriptor.
If it equals `0x03`, it sends a `probe` message to the registered class context.
There is no filtering on `bInterfaceSubClass` or `bInterfaceProtocol` at the
coreusb level; `usb-hid` reads the protocol field itself via `usb-iface-protocol`
to distinguish keyboards from mice.

### `probe` message

Sent by coreusb when a new HID device has been enumerated and configured:

- **Received:** `(probe dev)` — `dev` is the opaque enumerated-device record
  from `proto.clp` (carries HCI context, bus address, speed, ep0 max-packet, and
  raw configuration descriptor bytes).
- **Effect:** calls `hid-on-probe` (see Probe flow below).
- **No reply expected.**

### `remove` message

Sent by coreusb when a device disconnects:

- **Received:** `(remove addr)` — `addr` is the bus address of the disconnected
  device.
- **Effect:** calls `hid-on-remove`, which locates the matching poll context in
  the device list and sends it `(stop)`.
- **No reply expected.**

## Probe flow

`hid-on-probe dev input devs` executes in the class-driver context:

1. **Locate interrupt-IN endpoint.** Calls `usb-find-endpoint dev USB-XFER-INTERRUPT #t`. If no such endpoint exists, logs `[usb-hid] no interrupt IN endpoint; not claiming` and leaves `devs` unchanged.

2. **Read boot-protocol variant.** Calls `usb-iface-protocol dev`:
   - `1` = keyboard (`HID-PROTO-KEYBOARD`)
   - `2` = mouse (`HID-PROTO-MOUSE`)
   - any other value = generic HID

3. **Force boot protocol.** Sends `SET_PROTOCOL` (request `#x0B`, wValue `0`) via `usb-control-out` to the interface (`HID-IFACE-REQ` = `#x21`). Best-effort; a failure does not abort probe.

4. **Suppress idle reports.** Sends `SET_IDLE` (request `#x0A`, wValue `0`, wIndex = interface number). Best-effort.

5. **Register keyboard with coreinput.** If protocol is keyboard, sends:
   ```scheme
   (send input (list 'register "USB Keyboard"))
   ```

6. **Spawn poll context.**
   ```scheme
   (spawn-restricted '()
     (lambda () (hid-poll dev epaddr mps proto input)))
   ```
   The poll context holds no capabilities (`'()`).

7. **Record device.** Prepends `(cons addr poll-ctx)` to `devs` and returns the updated list.

## Interrupt-IN poll loop

`hid-poll dev ep mps proto input` runs in the spawned-restricted context for the
lifetime of the device. It maintains a `stopped` flag and a `last` snapshot of
the previous 8-byte report.

### Transfer request

Each iteration sends a transfer request directly to the host controller:

```scheme
(send (usb-dev-hci dev)
      (list 'interrupt-in
            (usb-dev-address dev)
            (usb-dev-speed dev)
            ep          ; endpoint address (with direction bit)
            n           ; max bytes requested = min(mps, 8)
            n
            (self)))    ; reply target
```

The transfer max-size `n` is `min(mps, 8)`: HID boot-protocol reports are at
most 8 bytes.

### Completion handling

The loop uses a private `await` helper that drains the mailbox until a
`(complete n data)` arrives. Critically, if a `(stop)` message arrives **while**
an in-flight transfer is outstanding, `await` sets the `stopped` flag and
continues waiting for the controller's `complete` reply rather than dropping it.
This closes a race where a fast disconnect could lose the stop signal.

```scheme
(define (await)
  (let ((m (recv)))
    (cond ((eq? (car m) 'complete) m)
          ((eq? (car m) 'stop) (set! stopped #t) (await))
          (else (await)))))
```

### Report handling

After each `(complete nb rpt)`:

| Condition | Action |
|-----------|--------|
| `stopped` is set | Return `'stopped` and exit |
| `nb >= 0` and `(bytes-length rpt) >= 8` | Decode report (keyboard only), sleep 8 ms, loop with updated `last` and `fails = 0` |
| `nb < 0` and `fails < 2` | Sleep 8 ms, loop with `fails + 1` |
| `nb < 0` and `fails >= 2` | Attempt `CLEAR_FEATURE(ENDPOINT_HALT)`, sleep 8 ms, loop with `fails = 0` |
| Otherwise (NAK or short) | Sleep 8 ms, loop unchanged |

The 8 ms sleep (`8_000_000` nanoseconds) paces the poll and prevents a quiet
keyboard from busy-looping the scheduler.

### Stall recovery (`clear-halt`)

After three consecutive interrupt-IN transfers fail (the failure counter
reaches 2), `usb-hid` sends `CLEAR_FEATURE(ENDPOINT_HALT)` (USB feature selector `0`, recipient =
`USB-REQ-RECIP-ENDPOINT`) via its own `ctl0` helper. `ctl0` routes the control
transfer through the same stop-aware `await` rather than `await-complete`
(the standard `usb-clear-halt` utility uses `await-complete` which drops
non-completion messages, so calling it from the poll context would silently
swallow a concurrent `stop` message).

## Report parsing (keyboard)

`decode-keyboard rpt last input` implements a diff-based decoder for USB HID
boot-protocol keyboard reports.

### Report layout

```
byte[0]  modifier byte (Ctrl/Shift/Alt/GUI bits) — NOT currently decoded
byte[1]  reserved (always 0x00)
bytes[2..7]  up to 6 simultaneously pressed USB HID usage codes (0x00 = empty slot)
```

### Diffing algorithm

- **Key-down:** for each slot in `rpt[2..7]`, if the usage code is non-zero and
  not present in `last[2..7]`, emit a `kbd-down` event.
- **Key-up:** for each slot in `last[2..7]`, if the usage code is non-zero and
  not present in `rpt[2..7]`, emit a `kbd-up` event.

No event is emitted for modifier byte changes (byte[0]).

### Events sent to coreinput

```scheme
(send input (list 'event (list 'kbd-down k)))  ; key pressed
(send input (list 'event (list 'kbd-up   k)))  ; key released
```

`k` is the raw USB HID usage code (an integer, e.g. `0x04` = key A). Translation
to OS keycodes or ASCII is the responsibility of the consumer above coreinput.
Simultaneous keypresses up to the 6-key boot-protocol limit are reported
faithfully.

## Exported functions

### `(usb-hid-init usb input)`

Entry point. Spawns the class-driver `serve` context, registers it with
[coreusb](../servers/coreusb.md) for class `0x03`, and returns the class-driver
context handle.

- `usb` — coreusb service handle.
- `input` — coreinput service handle; stored in the class-driver closure and
  passed to every spawned poll context.
- **Returns:** the class-driver context handle.
- **Side effects:** sends `register-class` to coreusb; subsequent `probe`/`remove`
  messages arrive asynchronously.

## Notes / gotchas

**Mouse reports are received but not decoded.** A mouse device with
`bInterfaceProtocol == 2` is claimed, SET_PROTOCOL'd to boot, and polled.
However, `decode-keyboard` is guarded by `(if (= proto HID-PROTO-KEYBOARD) ...)`,
so mouse reports are received and the poll loop maintains the `last` snapshot and
`fails` counter, but no events are forwarded to coreinput. Mouse support is a
stub.

**Modifier byte is not diffed.** Byte[0] of the boot-keyboard report carries
eight modifier bits (left/right Ctrl, Shift, Alt, GUI). These are never compared
between `rpt` and `last`, so modifier-only key events (e.g. pressing Ctrl alone)
are silently dropped. Only the usage-code slots at bytes 2–7 are diffed.

**HID descriptor not parsed.** The driver does not fetch or parse the HID report
descriptor (`GET_DESCRIPTOR` type `0x22`). It relies entirely on the boot
protocol's fixed 8-byte keyboard report layout. Devices that report
`bInterfaceProtocol == 0` (none) or `== 2` (mouse) are polled but produce no
decoded events.

**No subclass or protocol filter at claim time.** Any device with
`bInterfaceClass == 0x03` and an interrupt-IN endpoint is claimed. A device with
an unusual protocol byte that is neither 1 nor 2 will be polled but produce no
events; it will not be refused to other potential class drivers.

**Transfer cap at 8 bytes.** The poll caps the requested length at
`min(mps, 8)`. Devices that support larger reports (e.g. extended-format
keyboards that exceed the boot-protocol report size) are read only up to 8 bytes.

**Stall recovery vs. `usb-clear-halt`.** `proto.clp` provides a shared
`usb-clear-halt` utility, but `usb-hid` does not use it for stall recovery
because it calls `await-complete`, which drops non-completion messages.
`usb-hid` instead uses a private `ctl0` helper that routes the control transfer
through the stop-aware `await`, ensuring a `(stop)` that arrives during recovery
is captured rather than lost.

**No context holds a hardware capability.** `serve` spawns the class-driver
context with an empty capability grant (`'()`), and each poll context is spawned
with `'()` as well; the handler closures merely capture the module's imports
(`coreusb`, `driver-util`), neither of which grants MMIO or DMA access. All
actual hardware communication is performed by the host-controller context on the
driver's behalf, via message.
