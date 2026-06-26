# coreinput

> Asynchronous input aggregator: collects raw key events from every registered
> input driver into a single message-loop context.

| | |
|---|---|
| **Source** | `lisp/servers/coreinput.clp` |
| **Kind** | server |
| **Bound by** | `lisp/init.clp` — always (unconditional; not gated on `pci-find`) |
| **Registers with** | n/a — coreinput *is* the registration point; input drivers register with it |
| **Capabilities** | none — the service context is spawned via `spawn-restricted '()` and holds no import authority |

## Overview

`coreinput` is the canonical Cardinal Lisp server: a long-lived restricted
context that owns a device list and a message loop. Input drivers call
`(send input '(register <name>))` when they come up and
`(send input (list 'event <payload>))` for each raw event. The service threads
the registered-device list as its loop state; there is no shared mutable
structure, so no locking is needed.

The server holds **no capabilities** (`serve` internally uses
`spawn-restricted '()`). A compromised or wedged service context cannot acquire
hardware authority. The drivers that feed it — ps2 and usb-hid today — also run
as restricted contexts; they captured their hardware primitives lexically at load
time.

Because all communication is via `send` (one-way, non-blocking), the
RX-handler-re-enters-TX self-deadlock that the old C `CoreInput` had to work
around cannot arise by construction.

Event dispatch to consumer contexts (e.g. a focused window in the compositor)
is not yet implemented: `event` currently logs to COM1 only. This is an
acknowledged stub; [corecompositor](corecompositor.md) phase 6 is the planned
home for the routing logic.

## Initialization

`init.clp` calls `start-input-service` inside `setup-input`, then wires the
PS/2 and USB HID drivers to the returned handle:

```scheme
;; In lisp/init.clp:
(define (setup-input)
  (let ((input (start-input-service)))   ; spawn the service; get its handle
    (ps2-init)                           ; i8042 bring-up (needs port-I/O authority)
    (spawn-restricted '()                ; keyboard pump needs no import authority
      (lambda () (ps2-keyboard-driver input)))
    input))

;; The handle is later passed to USB HID during USB bring-up:
(usb-hid-init usb input)
```

`start-input-service` itself takes no arguments:

```scheme
(start-input-service)   ; → context handle (drivers send to this)
```

It spawns a restricted message loop with an initially empty device list (`'()`)
and returns the context handle. The initial state and the message dispatch
function are both defined inline; no external configuration is required.

## Message protocol

All messages are sent one-way with `send`; coreinput never replies. The message
format is a list whose `car` is the tag symbol.

### `register`

Sent by an input driver at start-up to announce itself to the service.

- **Request:** `(register <name>)` — `<name>` is a symbol or string identifying
  the device (e.g. `'ps2-keyboard`, `"USB Keyboard"`).
- **Reply:** none.
- **Effect:** `<name>` is prepended to the internal registered-device list and
  logged to COM1 as `[coreinput] device registered: <name>`.
- **Errors:** an unknown tag is silently ignored (the `else` branch returns the
  state unchanged).

```scheme
;; PS/2 keyboard driver:
(send input (list 'register 'ps2-keyboard))

;; USB HID keyboard:
(send input (list 'register "USB Keyboard"))
```

### `event`

Sent by a driver each time a raw input event is ready.

- **Request:** `(event <payload>)` — `<payload>` is a driver-defined list
  describing the event. See [Event payload formats](#event-payload-formats) below.
- **Reply:** none.
- **Effect (stub):** logs `[coreinput] event <payload>` to COM1 and returns the
  state unchanged. **No routing to consumer contexts is performed yet.**
- **Errors:** none (any payload shape is accepted; the handler does not inspect
  the payload beyond logging it).

```scheme
;; Sending a key-down event:
(send input (list 'event (list 'key #x1e 1)))
```

## Event payload formats

The `event` payload is defined by the sending driver, not by coreinput itself.
Current in-tree drivers use the following layouts:

### PS/2 keyboard (`lisp/drivers/ps2.clp`)

```scheme
(key <scancode> <pressed?>)
```

| Field | Type | Description |
|---|---|---|
| `<scancode>` | fixnum | PS/2 Set-1 scan code byte (e.g. `#x1e` = A). |
| `<pressed?>` | fixnum | `1` = key down, `0` = key up. |

Multi-byte PS/2 sequences (e.g. extended `E0 xx` codes) are not split across
separate messages; the driver accumulates the full sequence before sending.

### USB HID keyboard (`lisp/drivers/usb-hid.clp`)

Key down and key up are separate tags inside the payload list:

```scheme
(kbd-down <usage-id>)   ; key pressed
(kbd-up   <usage-id>)   ; key released
```

| Field | Type | Description |
|---|---|---|
| `<usage-id>` | fixnum | HID Boot Protocol keycode (0–255), from the interrupt-IN report's keycode slots (bytes 2–7). |

The driver diffs successive 8-byte boot-protocol reports to produce per-key
down/up events; modifier bytes (report byte 0) are not forwarded individually
at this time.

## Exported functions

### `(start-input-service)`

```scheme
(start-input-service)   ; → context
```

Spawns the input service message loop with an empty device list. Returns the
context handle that drivers `send` to. Called once at boot by `init.clp`'s
`setup-input`.

No arguments. No side-effects beyond spawning the context.

## Notes / gotchas

**Event handler is a stub.** The `event` message handler logs to COM1 but does
not forward events to any consumer. A focused-window routing layer is planned for
[corecompositor](corecompositor.md) phase 6. Code that needs key events today
must read from the log or add routing directly.

**Wiring is policy in `init.clp`, not here.** `coreinput` knows nothing about
PS/2 or USB; it is pure mechanism. Which drivers feed it and how they are started
is entirely in `lisp/init.clp`. This is intentional: adding a new input source
(e.g. a Bluetooth HID driver) only requires wiring it in `init.clp`.

**`send` is non-blocking and one-way.** A driver calling `send input (list 'event ...)` never blocks or re-enters its own TX path, making the
self-deadlock that required careful discipline in the old C `CoreInput` impossible
to trigger.

**No capabilities in the service context.** `serve` uses `spawn-restricted '()`
(see `lisp/lib/driver-util.clp`). The service loop inherits the driver primitives
its defining module imported at load time but cannot acquire new `sys-*` authority
at runtime. A compromised service cannot reach hardware.

**Unknown message tags are silently dropped.** The dispatch `cond` falls through
to `else devs` for any tag other than `register` or `event`, returning the state
unchanged. There is no error log for unrecognized messages.
