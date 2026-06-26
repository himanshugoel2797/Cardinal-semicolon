# corepower

> Fan-out power-management service: registers devices by class and delivers global or per-device power-state transitions to each matching context via `send`.

| | |
|---|---|
| **Source** | `lisp/servers/corepower.clp` |
| **Kind** | server |
| **Bound by** | `lisp/init.clp` — always (unconditionally at boot in `system-init`) |
| **Registers with** | n/a |
| **Capabilities** | none — no `sys-*` modules imported; uses `driver-util` only |

## Overview

`corepower` owns a list of registered power-managed devices; each entry is a triple `(name class-bits ctx)`. When a caller sends a power-transition event the service fans the event out — via `send`, never a re-entrant callback — to every device whose class bitmask intersects the event's class bitmask, then returns to its `recv` loop.

The `send`-based fan-out is deliberately non-blocking: a device context that processes a power event (and might, for example, call back into another service) can never deadlock the power server, because the power server does not wait for any reply before moving on.

The server idles parked on `recv` until a caller sends it a message. As of the current tree no driver feeds it and **the handle returned by `start-power-service` is not captured in `init.clp`** (see [Notes](#notes-gotchas)), so the service effectively idles for the lifetime of the boot.

## Initialization

`init.clp` calls `start-power-service` unconditionally from `system-init`, after the audio service and before the storage service:

```scheme
(start-power-service)   ; → service context handle (currently dropped)
```

`start-power-service` takes no arguments. Internally it calls `serve` from `driver-util`, which calls `spawn-restricted` to create an isolated context running a tail-recursive `recv` loop, and returns the handle to that context. The caller is expected to store this handle and use it for all subsequent `send` calls.

## Message protocol

All messages are sent to the handle returned by `start-power-service`. There is no reply to any message; all three request types are fire-and-forget from the sender's perspective.

### `register`

Adds a device to the service's list of registered power-managed devices.

- **Request:** `(register <name> <class-bits> <ctx>)`
  - `name` — a symbol identifying the device (printed on registration; no uniqueness check).
  - `class-bits` — an integer bitmask built from the `pwr-*` constants (see [Exported constants](#exported-constants)); indicates which power classes affect this device.
  - `ctx` — the context handle to which power-event messages will be delivered.
- **Reply:** none.
- **Side effect:** logs `[corepower] device registered: <name>` to the debug console (COM1).

```scheme
;; Example: a display driver registers itself for display power events
(send power-handle (list 'register 'my-display pwr-display (self)))
```

### `event-g`

Signals a global power-state transition and fans a `pwr-g` message to every device whose class intersects `class-bits`.

- **Request:** `(event-g <class-bits> <gstate> <pstate>)`
  - `class-bits` — bitmask of affected device classes; only devices with an overlapping class receive the event.
  - `gstate` — global power state (opaque; no enumeration is enforced).
  - `pstate` — performance state (opaque).
- **Reply:** none (to the sender).
- **Fan-out message delivered to each matching device:** `(pwr-g <gstate> <pstate>)`

```scheme
;; Notify all display devices of a global power change
(send power-handle (list 'event-g pwr-display 'suspend 0))
;; Each registered display context receives: (pwr-g suspend 0)
```

### `event-d`

Signals a device-local power-state transition and fans a `pwr-d` message to every device whose class intersects `class-bits`.

- **Request:** `(event-d <class-bits> <dstate>)`
  - `class-bits` — bitmask of affected device classes.
  - `dstate` — device power state (opaque; D0–D3 by convention; not enforced).
- **Reply:** none (to the sender).
- **Fan-out message delivered to each matching device:** `(pwr-d <dstate>)`

```scheme
;; Put all audio-output devices into D3 (off)
(send power-handle (list 'event-d pwr-audio-out 3))
;; Each registered audio-out context receives: (pwr-d 3)
```

### Unknown tags

Any message whose `car` is not `register`, `event-g`, or `event-d` is silently ignored; the device list is left unchanged and no reply is sent.

## Exported constants

These integer bitmasks are the device power-class flags. Combine them with `bitwise-or` to register a device for multiple classes.

| Constant | Value | Meaning |
|---|---|---|
| `pwr-generic` | `1` | Generic / uncategorised device |
| `pwr-display` | `2` | Display / framebuffer |
| `pwr-audio-out` | `4` | Audio output (speaker, headphone) |
| `pwr-audio-in` | `8` | Audio input (microphone, line-in) |
| `pwr-hid` | `16` | Human-interface device (keyboard, mouse) |
| `pwr-camera` | `32` | Camera / image capture |
| `pwr-processor` | `64` | Processor / CPU core |

## Exported functions

### `(start-power-service)`

Spawns the power-management service context and returns its handle.

- **Arguments:** none.
- **Returns:** a context handle (pass to `send` to submit registrations and events).
- **Side effects:** creates a `spawn-restricted` context; no hardware access.

## Notes / gotchas

**Handle is dropped at boot (stub condition).** In the current `lisp/init.clp`, `start-power-service` is called but the returned handle is not bound to any variable:

```scheme
(start-power-service)   ; return value discarded — nothing can send to it
```

This means no driver or service can currently reach the power server. The comment in `init.clp` acknowledges this: *"power [has] no drivers feeding [it] yet — [it] idle[s] parked on recv."* Before writing a driver that registers with corepower, `init.clp` must be updated to capture and publish the handle.

**No reply to any message.** The protocol is fully asynchronous (fire-and-forget in both directions). A device that needs to confirm a power transition must implement its own acknowledgement protocol on top.

**No uniqueness check on `register`.** The same name or context may be registered multiple times; each registration adds another entry to the list and the device will receive duplicate event messages.

**Fan-out is sequential.** `pwr-fanout` iterates the device list with `for-each`, calling `send` for each matching entry in order. A context that has exited (dead handle) will cause the `send` to abort the service loop — there is no dead-handle guard. Devices should not be registered and then allowed to exit without a corresponding deregistration mechanism (which does not currently exist).

**No deregister message.** There is no way to remove a device from the list once it has registered. A driver that restarts or crashes will leave a stale entry.
