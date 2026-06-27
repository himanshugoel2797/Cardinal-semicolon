# virtio-blk

> virtio block-device driver: a single request virtqueue over the shared virtio substrate, registering a disk with [corestorage](../servers/corestorage.md).

| | |
|---|---|
| **Source** | `lisp/drivers/virtio-blk.clp` |
| **Kind** | driver |
| **Bound by** | `lisp/init.clp` — `(virtio-blk-init storage ecam)` over `(pci-find-all #x1af4 #x1042)` (modern) and `(pci-find-all #x1af4 #x1001)` (transitional) |
| **Registers with** | [corestorage](../servers/corestorage.md) via `(send storage (list 'register-blockdev 'vblk0 512 <capacity> <driver-ctx>))` |
| **Capabilities** | `sys-mmio`, `sys-pci`, `driver-util`, `virtio` (the shared `lisp/lib/virtio.clp` library) |

## Overview

virtio-blk is the simplest storage path: one **request virtqueue** built on the
shared [virtio library](virtio-gpu.md) (`virtio-bringup`, `virtio-setup-queue`,
the descriptor/avail/used-ring helpers).  It reads the device capacity from the
virtio device-config region and exposes the disk to
[corestorage](../servers/corestorage.md) under the name `vblk0`.

Completion is **polled**: each request posts a 3-descriptor chain and waits for
the used ring to advance (`wait-until-spin`).  There is one outstanding request
at a time — corestorage serializes through the driver mailbox — so no in-flight
tracking is needed.  Block size is fixed at **512 bytes** (the virtio-blk sector
size), and a single request is capped at **8 sectors** (matching the per-request
DMA buffer).

## Initialization

```scheme
(virtio-blk-init storage ecam)   ; → spawns bring-up, returns immediately
```

- `storage` — the corestorage service handle.
- `ecam` — the device's ECAM base from `pci-find-all`.

The spawned bring-up context:

1. `virtio-bringup` negotiating only `VIRTIO-F-VERSION-1` → the common/device/notify
   config regions.
2. Reads the **capacity** (in 512-byte sectors) from device-config: u32 @0 (low) |
   u32 @4 (high).
3. `virtio-setup-queue` for the single request queue (index 0); sets `DRIVER_OK`.
4. Allocates header / data / status DMA buffers.
5. Registers `(register-blockdev 'vblk0 512 <capacity> <driver-ctx>)`.

## Message protocol

The driver context answers the standard [corestorage](../servers/corestorage.md)
block protocol; corestorage bounds-checks and serializes.

### `read` / `write`

- **Request:** `(read lba count reply-ctx)` / `(write lba count data reply-ctx)`.
- **Reply:** `(complete status bytes)` for read (status 0/-1), `(complete status)`
  for write.

Each request builds the virtio-blk 3-descriptor chain: a 16-byte header
(device-readable), the data buffer (device-writable for read / readable for
write), and a 1-byte status (device-writable); avail-pushes the head, notifies
the queue, polls `used.idx`, then reads the status byte and (for reads) copies
the data out.

## Exported functions

Pure helpers for the host unit test (`libs/lisp/test/test_virtio_blk.c`):

### `(blk-build-header! buf type sector)`

Stamps the 16-byte virtio-blk request header into `buf`: `type` u32 @0
(`VIRTIO-BLK-T-IN` = 0 read / `VIRTIO-BLK-T-OUT` = 1 write), reserved u32 @4 (0),
and the 512-byte `sector` LBA as a little-endian u64 @8.

### `(blk-status-ok? b)`

True when the device's 1-byte status reply is `0` (OK), false for `1` (IOERR) /
`2` (UNSUPP).

## Notes / gotchas

- **Both PCI IDs are bound.** QEMU presents `virtio-blk-pci` as modern
  (`1af4:1042`) on a PCIe root or transitional (`1af4:1001`) on a legacy bus;
  binding both covers either.

- **512-byte sectors.** The driver registers `bsize = 512`, so a corestorage
  `lba` maps directly to a virtio-blk sector.

- **Per-request cap.** The data DMA buffer holds 8 sectors; corestorage only
  bounds `lba + count` against capacity, so the 8-sector cap lives in the driver
  context (the same approach [ahci](ahci.md) takes).

- **Polled, single outstanding.** No MSI; correctness depends on corestorage
  serializing requests per mailbox.  An `IO-TIMEOUT-NS` guard fails a stuck
  request rather than hanging the driver context.
