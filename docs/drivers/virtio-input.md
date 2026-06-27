# virtio-input

> virtio input driver (tablet / keyboard / mouse): decodes Linux evdev events from the event virtqueue and feeds them to [coreinput](../servers/coreinput.md).

| | |
|---|---|
| **Source** | `lisp/drivers/virtio-input.clp` |
| **Kind** | driver |
| **Bound by** | `lisp/init.clp` — `(virtio-input-init input ecam)` over `(pci-find-all #x1af4 #x1052)` |
| **Registers with** | [coreinput](../servers/coreinput.md) via `(send coreinput (list 'event <payload>))` |
| **Capabilities** | `sys-mmio`, `sys-pci`, `driver-util`, `virtio` |

## Overview

virtio-input is the clean VM input path.  It brings up a virtio-input device
(`-device virtio-tablet-pci` / `virtio-keyboard-pci` / `virtio-mouse-pci`,
`1af4:1052`), drains its **event virtqueue**, and translates the Linux **evdev**
events into the event payloads [coreinput](../servers/coreinput.md) fans out —
the same shapes the [ps2](ps2.md) driver emits:

```scheme
(pointer <x> <y> <down?>)   ; down? is #t / #f
(key <code> <pressed>)      ; pressed 1 / 0 (here: evdev value)
```

The **tablet** (absolute pointer, `EV_ABS`) is the priority path: absolute X/Y
plus the left button map cleanly to `(pointer x y down?)`.  A **mouse** reports
relative deltas (`EV_REL`) which the driver accumulates onto a running position.
A **keyboard** reports `EV_KEY` with evdev keycodes.

Two virtqueues are used: the **event queue** (index 0, device→driver, filled with
device-writable 8-byte buffers) and an idle **status queue** (index 1).  A
device's events are batched and committed on the `EV_SYN` frame boundary.

## Initialization

```scheme
(virtio-input-init coreinput ecam)   ; → spawns bring-up, returns immediately
```

- `coreinput` — the input service handle (from `setup-input` in init.clp).
- `ecam` — the device's ECAM base from `pci-find-all`.

The spawned bring-up: `virtio-bringup` (VERSION_1 only); `virtio-setup-queue` for
the event queue (0) filled with 32 device-writable 8-byte buffers and the status
queue (1); reads device-config (`EV_BITS`/`ABS_INFO`) to classify the device and
seed the absolute-pointer range; sets `DRIVER_OK`; `pci-setup-msi`; registers with
coreinput; spawns the MSI-driven event pump; enables interrupts last.

## Message protocol

The driver emits to coreinput only — it consumes no request protocol.  Each
`EV_SYN` frame produces zero or more `(event …)` sends:

| evdev event | emitted to coreinput |
|---|---|
| `EV_ABS` X/Y + `EV_SYN` | `(pointer x y down?)` (absolute) |
| `EV_REL` X/Y (accumulated) + `EV_SYN` | `(pointer x y down?)` |
| `EV_KEY` keycode | `(key code value)` |
| `EV_KEY` `BTN_LEFT` | updates the pointer `down?` state |

## Exported functions

Pure decode/reduce functions, exported for the host unit test
(`libs/lisp/test/test_virtio_input.c`):

### `(parse-input-event buf off)`

Decodes a `virtio_input_event` at byte `off`: `(type code value)` with `type`/
`code` as little-endian u16 and `value` a sign-extended u32 (so `EV_REL` negative
deltas decode correctly).

### `(reduce-event state ev)` / `(reduce-events state evs)`

Fold an event (or a sequence) into the pointer/key accumulator `state`; on
`EV_SYN` the reducer yields the coreinput payload(s) to emit.  `pstate0` / `mk` /
`ps-out` construct and read the accumulator.  These let a test assert, e.g.,
`EV_ABS X, EV_ABS Y, BTN_LEFT down, EV_SYN → (pointer x y #t)` with no hardware.

## Notes / gotchas

- **Keycode space mismatch.** The keyboard emits **Linux evdev** keycodes under
  the `key` tag.  Consumers (the [compositor](../servers/corecompositor.md))
  currently expect **PS/2 set-1** scancodes (what [ps2](ps2.md) emits), so
  virtio-keyboard keys are mis-mapped until an evdev→set-1 translation is added
  here or the compositor learns evdev — a known follow-up.  The **tablet**
  pointer path has no such issue and is the recommended input device.

- **Absolute values are raw.** Tablet `EV_ABS` values are in the device's logical
  range (from `ABS_INFO`), **not** framebuffer pixels.  The driver passes the raw
  absolute value through; a consumer may need to scale `(pointer x y …)` to the
  screen.  The driver does not touch the compositor.

- **Device classification is best-effort** (via `EV_BITS`); the pump is
  type-driven regardless, so a misread only changes the registration log name.
