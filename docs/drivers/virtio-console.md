# virtio-console

> virtio serial-console driver (single-port): transmits bytes over a virtqueue to a host character device, with minimal receive.

| | |
|---|---|
| **Source** | `lisp/drivers/virtio-console.clp` |
| **Kind** | driver |
| **Bound by** | `lisp/init.clp` — `(console-init ecam)` over `(pci-find-all #x1af4 #x1043)` (modern) and `(pci-find-all #x1af4 #x1003)` (transitional) |
| **Registers with** | n/a — exposes its own console service handle (no `Core*` registry) |
| **Capabilities** | `sys-mmio`, `sys-pci`, `driver-util`, `virtio` |

## Overview

A single-port virtio-console (the `VIRTIO_CONSOLE_F_MULTIPORT` feature is **not**
negotiated, so the layout is the simple two-queue one: **receiveq** index 0,
**transmitq** index 1).  On bring-up it transmits a one-line banner so a QEMU
chardev smoke test shows output; thereafter `console-init`'s service answers
`(write …)` requests by transmitting their bytes.  Receive is minimal (drain on
demand / optional forward to a subscriber).

## Initialization

```scheme
(console-init ecam)   ; → the console service handle, or #f on no device
```

`virtio-bringup` (VERSION_1 only) → `virtio-setup-queue` for the receive (0) and
transmit (1) queues → `DRIVER_OK` → spawn the console `serve` loop.  The banner is
queued as the service's first `write` (so the blocking used-ring wait runs under
the scheduler, never in init's non-scheduled path).

## Message protocol

### `write`

- **Request:** `(write <string-or-bytes>)` — transmit the payload out the
  console.
- **Reply:** none (fire-and-forget over the transmitq).

### `subscribe`

- **Request:** `(subscribe <ctx>)` — register `ctx` to receive drained RX bytes
  (minimal; RX is best-effort).

## Exported functions

### `(console-init ecam)`

Entry point; see [Initialization](#initialization).

### `(console-pack s)`

Pure helper (host-tested in `libs/lisp/test/test_virtio_console.c`): packs a
string into a `bytes` buffer for transmission, passing existing `bytes` through
unchanged.

## Notes / gotchas

- **Transitional ID required.** QEMU's `-device virtio-serial-pci` enumerates as
  the **transitional** `1af4:1003` by default, not the modern `1af4:1043` —
  binding only the modern ID silently misses the device.  Both are bound.

- **TX-focused.** The receive path is minimal (no interrupt/timer wiring; drain
  on an explicit poll), and the device-config `cols`/`rows` are informational
  only.  Sufficient for a debug-output console; a full interactive port is a
  follow-up.
