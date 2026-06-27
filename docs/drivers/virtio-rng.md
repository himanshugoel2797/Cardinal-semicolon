# virtio-rng

> virtio entropy-source driver: pulls random bytes from a single virtqueue and serves them via a small `(get-random …)` message service.

| | |
|---|---|
| **Source** | `lisp/drivers/virtio-rng.clp` |
| **Kind** | driver |
| **Bound by** | `lisp/init.clp` — `(rng-init ecam)` over `(pci-find-all #x1af4 #x1044)` (modern) and `(pci-find-all #x1af4 #x1005)` (transitional) |
| **Registers with** | n/a — exposes its own entropy service handle (no `Core*` registry) |
| **Capabilities** | `sys-mmio`, `sys-pci`, `driver-util`, `virtio` |

## Overview

The simplest virtio device: one virtqueue, no device-config.  The driver posts
device-writable buffers to the queue; the device fills them with random bytes and
returns them via the used ring (the used `len` is the count written).  `rng-init`
spawns a `serve` loop that buffers entropy in a pool and answers `(get-random n
reply)` requests, refilling from the device as needed.

## Initialization

```scheme
(rng-init ecam)   ; → the entropy service handle, or #f on no device
```

`virtio-bringup` (VERSION_1 only) → `virtio-setup-queue` for the single queue
(index 0) → `DRIVER_OK` → spawn the entropy `serve` loop.  Returns the service
handle (fire-and-forget; nothing in the OS consumes it yet).

## Message protocol

### `get-random`

- **Request:** `(get-random <n> <reply>)` — request `n` random bytes.
- **Reply:** `(send reply (list 'random <bytes>))` — a fresh `bytes` of length
  `n`, drawn from the pool (topped up from the device round-trip when short).

## Exported functions

### `(rng-init ecam)`

Entry point; see [Initialization](#initialization).

### `(rng-take pool want out off)`

Pure helper (host-tested in `libs/lisp/test/test_virtio_rng.c`): slices up to
`want` bytes from the entropy `pool` into `out` at `off`, handling partial /
exhausted / zero-length requests.

## Notes / gotchas

- **Transitional ID required.** QEMU's `-device virtio-rng-pci` enumerates as the
  **transitional** `1af4:1005` by default, not the modern `1af4:1044` — binding
  only the modern ID silently misses the device.  Both are bound.

- **No standard consumer yet.** The service handle is currently fire-and-forget;
  wiring it to a `sys-random`-style facility (or TCP ISN generation) is a
  follow-up.
