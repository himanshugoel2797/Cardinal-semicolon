# virtio-net

> Virtio 1.0 (modern PCI) network driver: handles RX/TX ring management and NIC registration with the [corenetwork](../servers/corenetwork.md) stack.

| | |
|---|---|
| **Source** | `lisp/drivers/virtio-net.clp` |
| **Kind** | driver |
| **Bound by** | `lisp/init.clp` — gated on `(pci-find-all #x1af4 #x1041)` for each matching device |
| **Registers with** | [corenetwork](../servers/corenetwork.md) via `(send net '(register-nic <mac> <tx-ctx>))` |
| **Capabilities** | `sys-mmio` (`mmio-map`, `dma-alloc`), `sys-pci` (`pci-find-all`, `pci-setup-msi`, `msi-count`, `msi-wait`), `driver-util`, `virtio` |

## Overview

`virtio-net` is the first device driver written in Cardinal Lisp. It handles the
virtio-net-specific half of a virtio 1.0 NIC: feature negotiation for `VIRTIO_NET_F_MAC`,
the 12-byte `virtio_net_hdr` framing, RX/TX split-virtqueue population and draining, and
MSI-X interrupt delivery. All device-agnostic transport work (capability walk, status/feature
handshake, split-virtqueue allocation) is delegated to the shared `virtio` library
(`lisp/lib/virtio.clp`), imported here.

The driver is pure transport. All ethernet/ARP/IP framing lives in the
[corenetwork](../servers/corenetwork.md) service. `virtio-net` receives raw ethernet
frames from the hardware and forwards them to corenetwork; corenetwork hands back raw
frames to the driver's TX context for transmission.

`init.clp` supports multiple NICs: `pci-find-all` enumerates all devices with VID
`#x1af4` / DID `#x1041` and calls `virtio-net-init` once per ECAM, so two virtio-net
devices each get their own independent RX/TX pair and their own registration with the
network stack.

## Initialization

`init.clp` calls `virtio-net-init` for each discovered virtio-net ECAM:

```scheme
(virtio-net-init net dev-ecam)
```

- `net` — handle of the running [corenetwork](../servers/corenetwork.md) service context,
  obtained via `(start-network-service)`.
- `dev-ecam` — the device's ECAM physical base address, as returned by `pci-find-all`.
  Passing `#f` causes the driver to print a diagnostic and return `#f` immediately.

On success returns `'ok`; on failure (ECAM absent, `FEATURES_OK` rejected, or MSI-X
setup failure) returns `#f` after printing a diagnostic on `[virtio-net]`.

### Bring-up sequence

1. `virtio-bringup` walks the PCI capability list, maps the `COMMON`, `DEVICE`, and
   `NOTIFY` config regions, drives the status handshake (`ACK` → `DRIVER` →
   `FEATURES_OK` → `DRIVER_OK`), and negotiates features. The driver requests
   `VIRTIO_NET_F_MAC` (bit 5, low word) and `VIRTIO_F_VERSION_1` (bit 0, high word).
2. RX queue (index 0) and TX queue (index 1) are set up with `virtio-setup-queue`, which
   allocates DMA-coherent descriptor/avail/used rings and programs
   `VIRTIO_QUEUE_MSIX_VECTOR` → entry 0 for each.
3. `VIRTIO_MSIX_CONFIG` is set to 0 (config-change events → MSI-X entry 0).
4. `pci-setup-msi` enables the MSI-X capability and returns an opaque MSI handle.
5. `VIRTIO_STATUS_DRIVER_OK` is written, making the device live.
6. RX buffers are pre-populated (`rx-populate!`).
7. Two restricted contexts are spawned: the TX context and the RX context (see below).
8. `(send net '(register-nic <mac> <tx-ctx>))` announces the NIC to the network stack.

## Ring layout

### RX

```
NRX = 16     ; descriptors pre-populated in the RX virtqueue
RXSLOT = 2048 bytes per slot
  [ 12-byte virtio_net_hdr ][ up to 1514 bytes of ethernet frame ]
```

One contiguous DMA buffer (`NRX × RXSLOT` bytes) is allocated. Descriptor `i` points at
`base + i × RXSLOT`; the virtio-net header occupies the first `VNET-HDR = 12` bytes of
each slot, so the ethernet frame starts at offset `VNET-HDR` within the slot.

### TX

A single `RXSLOT`-byte DMA buffer is allocated. Layout:

```
[ 12 zero bytes (virtio_net_hdr) ][ ethernet frame ]
```

Only one TX buffer exists; `tx-frame!` waits (via `wait-until`) for the device to consume
it before returning, so the TX context serializes frames naturally without a ring allocator.

## Contexts spawned

### TX context

Spawned with an empty capability grant (`spawn-restricted '()`). Receives messages from
the [corenetwork](../servers/corenetwork.md) service:

```scheme
(tx <frame-bytes> <len>)
(tx <frame-bytes> <len> <reply-ctx>)
```

- `<frame-bytes>` — a byte buffer containing the raw ethernet frame (no virtio header).
- `<len>` — length of the frame in bytes.
- `<reply-ctx>` — optional; if present, a `(tx-done)` message is sent to it once the
  device has consumed the descriptor. Corenetwork uses this to throttle in-flight frames.

The TX context copies the frame into `txbuf` at offset `VNET-HDR`, zeroes the header bytes,
posts descriptor 0 on the avail ring, kicks the device via `notify-queue!`, then spins
(via `wait-until`, 200 ms timeout) until `used.idx` advances before looping.

### RX context

Spawned with an empty capability grant (`spawn-restricted '()`). Loops forever:

1. Calls `rx-drain!` to consume all newly completed used-ring entries.
2. For each completed entry, copies the frame payload out of the recycled `rxbuf` slot
   with `copy-bytes` (taking a snapshot before the buffer is recycled), then delivers:

```scheme
(send net (list 'rx <frame-snapshot> <len>))
```

3. After draining, checks `msi-count` against the last seen value; if no new interrupt has
   arrived, calls `msi-wait` to yield until the device fires another MSI-X.

The RX context never holds any lock across the `send` to `net`. Corenetwork processes the
frame synchronously and may produce a reply (ARP response, ICMP echo reply, UDP datagram)
that re-enters the TX path by sending to `tx-ctx` — but `tx-ctx` is a separate context and
the send is non-blocking from corenetwork's perspective, so no deadlock can occur.

## Exported functions

### `(virtio-net-init net dev-ecam)`

The sole exported symbol. Entry point called by `init.clp` once per discovered device.
See [Initialization](#initialization) for full details.

No other functions are exported; the RX and TX contexts are private to the driver closure.

## Internal helpers

These are module-private (not exported) but documented here for cross-reference:

### `(rx-populate! rxq rxbuf notify mult)`

Fills up to `min(q-size, NRX)` RX descriptors flagged `VIRTQ-DESC-F-WRITE`, pushes them
onto the avail ring, and kicks the device. Called once during initialization.

### `(rx-drain! rxq rxbuf last notify mult handler)`

Consumes all entries in the used ring since `(cell-ref last)`, calls `handler` with
`(offset frame-len)` for each (where `offset` is relative to `rxbuf` start, already
adjusted past `VNET-HDR`), recycles each descriptor back into the avail ring, and kicks
the device. Updates `last` via `cell-set!`.

### `(tx-frame! txq txbuf notify mult frame-len)`

Posts descriptor 0 (pointing at `txbuf`, `VNET-HDR + frame-len` bytes) on the avail ring
and kicks the device. Does not wait; the TX context caller waits for `used.idx` separately.

### `(virtio-net-read-mac devcfg)`

Reads bytes 0–5 of the device-config region and returns them as a 6-element list of
integers (the MAC address). The device config region is only valid after `FEATURES_OK`
confirms `VIRTIO_NET_F_MAC` was accepted.

## PCI binding

| Field | Value |
|-------|-------|
| Vendor ID | `#x1af4` (Red Hat / QEMU virtio) |
| Device ID | `#x1041` (virtio 1.0 network device) |
| Feature bits negotiated | `VIRTIO_NET_F_MAC` (bit 5 of word 0), `VIRTIO_F_VERSION_1` (bit 0 of word 1) |
| MSI-X | Required; driver prints an error and returns `#f` if `pci-setup-msi` fails |

The driver uses the `virtio` library's `virtio-bringup` to walk PCI vendor-specific
capabilities (`cap-id 0x09`) looking for `cfg-type` 1 (COMMON), 2 (NOTIFY), and 4
(DEVICE). It calls `pci-enable-mem-bus-master!` on the ECAM before mapping any region.

## Notes / gotchas

### RX-handler locking rule

The RX context delivers frames by calling `(send net ...)`, which is a non-blocking
message enqueue. The network service processes the frame and may immediately reply by
sending to `tx-ctx`. Because `send` is non-blocking and the TX context is a separate
scheduler entity, **no lock is held across the frame delivery** — there is no lock to
deadlock. This is the Lisp-native solution to the C-level "never hold a driver lock
across `network_rx_packet`" rule: the message-passing architecture eliminates shared
mutable state between the RX and TX paths entirely.

### Single TX buffer

Only one outbound frame is in flight at a time. The TX context busy-waits (up to 200 ms)
for `used.idx` to advance before looping. This is adequate for the current control-traffic
rate (ARP, DHCP, ICMP, UDP). A TX ring with multiple descriptors is noted in the source
as a future refinement.

### Frame copy on RX

Each received frame is copied out of the shared `rxbuf` DMA buffer with `copy-bytes`
*before* the descriptor is recycled back into the avail ring. Without this copy, the
frame memory could be overwritten by a subsequent RX before corenetwork finishes reading
it.

### MSI-X ordering

`virtio-setup-queue` programs `VIRTIO_QUEUE_MSIX_VECTOR` for each queue, and
`VIRTIO_MSIX_CONFIG` is set, before `pci-setup-msi` enables the MSI-X PCI capability.
This matches the virtio 1.0 spec requirement: virtio's own vector-register configuration
must precede the PCI capability enable to avoid lost interrupts.

### Interrupt delivery requires `sti`

The Lisp core loop (`lisp_core_loop` in C) must run with interrupts enabled for MSI-X
events to reach the LAPIC and increment `msi-count`. If the eval loop ever runs with
`cli` in effect, `msi-wait` will block forever. This is a known boot-sequence concern
documented in `notes/AUDIT.md`.
