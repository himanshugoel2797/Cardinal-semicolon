# nvme

> NVMe (NVM Express) PCIe block-storage driver: admin + I/O queue pair with phase-bit completion polling, registering a namespace as a block device with [corestorage](../servers/corestorage.md).

| | |
|---|---|
| **Source** | `lisp/drivers/nvme.clp` |
| **Kind** | driver |
| **Bound by** | `lisp/init.clp` — `(for-each (lambda (ecam) (nvme-init storage ecam)) (pci-find-class-all #x01 #x08))` (PCI class 01h, subclass 08h — any NVMe controller) |
| **Registers with** | [corestorage](../servers/corestorage.md) via `(send storage (list 'register-blockdev 'nvme0 <bsize> <nsze> <driver-ctx>))` |
| **Capabilities** | `sys-mmio` (`mmio-map`, `dma-alloc-32`, `bytes-phys`), `sys-pci` (`pci-find-class`, `pci-assign-bars`, `pci-setup-msi`), `driver-util` (`wait-until`, `make-cell`, `bar-base`, `pci-enable-mem-bus-master!`) |

## Overview

The NVMe driver brings up a single NVMe controller over its MMIO register
window (BAR0) and exposes its first namespace as a block device.  It is bound by
**PCI class** rather than vendor/device ID, so any NVMe controller (class 01h /
subclass 08h) is picked up — under QEMU that is `-device nvme` (`1b36:0010`).

Completion is **poll-driven**: the driver tracks the completion-queue **phase
bit** and busy/yield-polls for it rather than relying on an MSI interrupt.  This
keeps the controller logic simple and robust — there is exactly one command in
flight per queue pair, and [corestorage](../servers/corestorage.md) serializes
requests through the driver context's mailbox, so depth-4 rings are ample.

The driver is pure block transport: it owns the admin and I/O queue pairs and a
per-request DMA buffer, and answers `read`/`write` requests from corestorage.
All filesystem logic lives elsewhere ([cardfs](../servers/cardfs.md) probes each
registered device).

Ring geometry: **admin queue depth 4**, **I/O queue depth 4**.  Submission-queue
entries are 64 bytes, completion-queue entries 16 bytes.  A single request is
capped at **two PRP pages** (8 × 512-byte blocks, or 1 × 4096-byte block).

## Initialization

`init.clp` calls `nvme-init` once per enumerated NVMe controller:

```scheme
(nvme-init storage ecam)   ; → 'nvme-spawned, or #f on no-BAR / no-device
```

- `storage` — the corestorage service handle (from `(start-storage-service)`).
- `ecam` — the device's ECAM config-space base from `pci-find-class-all`.

`nvme-init` maps the config space, enables memory-space decode + bus-mastering,
maps BAR0 (calling `pci-assign-bars` first if firmware left it unassigned), then
**spawns** the blocking bring-up in a restricted context and returns immediately
(fire-and-forget, like ahci) — boot's `system-init` is never blocked.

### Bring-up sequence (spawned context)

1. **Allocate + zero the queues.** Admin SQ/CQ and I/O SQ/CQ via `dma-alloc-32`.
   Each is **explicitly zeroed** — the completion phase-bit protocol requires
   every CQ slot to start at phase 0, or the first poll mistakes stale DMA
   garbage for a completion.
2. **Disable the controller** (`CC.EN = 0`) and wait for `CSTS.RDY = 0`.
3. **Program the admin queues**: `AQA` (admin SQ/CQ sizes), `ASQ`/`ACQ` (their
   physical base addresses).
4. **Enable the controller** (`CC`): `IOSQES = 6` (bits 19:16 → 64-byte SQ
   entries), `IOCQES = 4` (bits 23:20 → 16-byte CQ entries), `EN = 1`; wait for
   `CSTS.RDY = 1`.
5. **IDENTIFY NAMESPACE** (admin opcode `0x06`, CNS=0, nsid=1) into a 4 KiB DMA
   buffer → `NSZE` (block count) and the active LBA format's `LBADS` exponent →
   block size (`1 << LBADS`, typically 512 or 4096).
6. **Create the I/O completion queue** (admin opcode `0x05`), then the I/O
   **submission queue** (opcode `0x01`) pointed at it.
7. **Register** the namespace with corestorage and spawn the request loop.

A smoke read of block 0 runs before registration (logs only on failure).
Returns `'up` on success; logs and returns `'fail` on any timeout or command
error (`disable timeout`, `enable (RDY) timeout`, `IDENTIFY NS failed`,
`create IO queues failed`).

## Message protocol

The driver context registered with corestorage answers the standard block-device
protocol (see [corestorage](../servers/corestorage.md)).  corestorage
bounds-checks `lba + count ≤ nsze` before forwarding and serializes per mailbox,
so the driver handles one request at a time.

### `read`

- **Request:** `(read <lba> <count> <reply-ctx>)` — read `count` blocks from LBA
  `lba`.
- **Reply:** `(send reply-ctx (list 'complete <status> <bytes>))` — `status` 0 on
  success with `count*bsize` bytes, or `-1` with `#f` on failure (including a
  request larger than the 2-PRP cap).

### `write`

- **Request:** `(write <lba> <count> <data> <reply-ctx>)` — write `data`
  (`count*bsize` bytes) at LBA `lba`.
- **Reply:** `(send reply-ctx (list 'complete <status>))` — 0 / -1.

Both go through the shared `submit!`/`io!` path: build a 64-byte SQE at the ring
tail, ring the SQ doorbell, poll the CQ slot for the expected phase, read the
15-bit status, advance + ring the CQ head doorbell.

## Exported functions

The pure builders/parsers are exported so a host unit test
(`libs/lisp/test/test_nvme.c`) can drive them over mock byte buffers without
hardware:

### `(nvme-cmd-build! sqe opc cid nsid prp1 prp2 cdws)`

Zeroes and fills a 64-byte submission-queue entry: CDW0 (`opc` | `cid`<<16),
NSID, PRP1/PRP2 (each split low/high u32), and CDW10..15 from the `cdws` list.

### `(cq-status cqbuf off)` / `(cq-phase cqbuf off)` / `(cq-cid cqbuf off)`

Parse a 16-byte completion entry at byte offset `off`: the 15-bit status field
(0 = success), the phase bit, and the command id.

### `(id-ns-nsze idns)` / `(id-ns-lbads idns)` / `(id-ns-bsize idns)`

Parse an IDENTIFY-NAMESPACE structure: namespace size in blocks (`NSZE`, low 48
bits), the selected LBA format's `LBADS` exponent, and block size in bytes
(`1 << LBADS`, guarded to 512 for a garbage exponent).

## Notes / gotchas

- **Poll, don't interrupt.** Completion is detected by the CQ phase bit, not an
  MSI — `pci-setup-msi` is wired for completeness but never required.  This
  sidesteps controller-specific interrupt configuration entirely.

- **Queues must be zeroed.** `alloc-q` zeroes every queue buffer.  The phase
  protocol expects all CQ slots at phase 0 initially; `dma-alloc` does not
  guarantee zeroed memory, and stale garbage with the phase bit set causes a
  premature false completion and a bogus status.

- **`CC.IOSQES`/`IOCQES` bit positions.** Per the NVMe spec `IOSQES` is bits
  19:16 (value 6 → 64-byte SQ entries) and `IOCQES` is bits 23:20 (value 4 →
  16-byte CQ entries).  Swapping them makes the controller reject Create-I/O-CQ
  with "invalid queue size"/"invalid entry size".

- **Spawned bring-up errors are silent.** The bring-up runs in a
  `spawn-restricted` context; a Lisp runtime error (e.g. a prim called with the
  wrong arity — `make-cell` requires an initial value) kills that context with
  no output.  When debugging, add a log past each step, or use QEMU's
  `-trace 'pci_nvme_err*'` to see the controller's own rejection reason.

- **One command in flight per queue.** Depth-4 rings, one slot used at a time;
  corestorage serializes requests through the driver mailbox, so this is
  sufficient and avoids any in-flight tracking.

- **Request size cap.** A single request transfers at most two PRP pages (no PRP
  list page), so oversize multi-block requests are refused with `-1` rather than
  chunked.  Fine for filesystem block I/O; a PRP-list page would lift it.

- **First namespace only.** The driver identifies and registers nsid 1 as
  `nvme0`; multi-namespace controllers are not enumerated.
