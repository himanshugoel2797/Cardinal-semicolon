# Lisp servers & shared libraries API

The OS services (`Core*`) and the substrate they share are **Lisp** modules
under `lisp/`. This file documents the *exported* surface of the shared driver
libraries (`lisp/lib/`), the `Core*` service modules (`lisp/servers/`), and the
boot-policy entry points in `lisp/init.clp` — i.e. the symbols other Lisp
modules `(import …)` or that the kernel calls.

Only names in each module's `(export …)` list are documented here; module-private
helpers (e.g. the per-protocol internals spliced into `corenetwork` via
`(include …)`) are intentionally omitted. Every server is built on the `serve`
mold from `driver-util`: a long-lived restricted context owning some state and a
message loop, addressed by `send` rather than called synchronously.

---

## `nth`

- **kind:** lisp-fn
- **lang:** lisp
- **source:** `lisp/lib/driver-util.clp`
- **hash:** f80737b859103ad4

Returns the `k`-th element (0-based) of list `lst`.

**Parameters**
- `lst` — a proper list.
- `k` — a non-negative index.

**Returns:** `(car (cdr^k lst))`. Recurses `cdr` `k` times then takes `car`; an
out-of-range index errors when it `car`s the empty list. The list-indexing
primitive every Lisp driver and the queue-record accessors in `virtio` build on.

---

## `make-cell`

- **kind:** lisp-fn
- **lang:** lisp
- **source:** `lisp/lib/driver-util.clp`
- **hash:** 2a627d82e67a2a58

Allocates a mutable 1-word cell initialized to `v`.

The language's pairs and vectors are immutable, so a mutable scalar store is
built on the byte-buffer primitive: this allocates an 8-byte buffer with
`make-bytes` and writes `v` as a u64 at offset 0. Returns the buffer, used as an
opaque cell handle by `cell-ref` / `cell-set!`.

---

## `cell-ref`

- **kind:** lisp-fn
- **lang:** lisp
- **source:** `lisp/lib/driver-util.clp`
- **hash:** d1e8931dfdb2adfa

Reads the current value out of a cell created by `make-cell`.

**Returns:** the u64 stored at offset 0 of cell `c`.

---

## `cell-set!`

- **kind:** lisp-fn
- **lang:** lisp
- **source:** `lisp/lib/driver-util.clp`
- **hash:** ad8caeb257d7ed1c

Stores value `v` into cell `c` (in place).

Writes `v` as a u64 at offset 0 of the cell's backing buffer. Returns the result
of the underlying `bytes-u64-set!`.

---

## `put-be16!`

- **kind:** lisp-fn
- **lang:** lisp
- **source:** `lisp/lib/driver-util.clp`
- **hash:** 01f44fc91e8b0605

Writes the 16-bit value `v` into buffer `b` at byte offset `off` in big-endian
(network) order.

The volatile byte accessors are little-endian-native, so protocol headers (which
are big-endian on the wire) are laid down a byte at a time: the high byte goes to
`off`, the low byte to `off+1`.

**Parameters**
- `b` — destination byte buffer.
- `off` — byte offset.
- `v` — 16-bit value.

---

## `get-be16`

- **kind:** lisp-fn
- **lang:** lisp
- **source:** `lisp/lib/driver-util.clp`
- **hash:** b19586239262d3c7

Reads a big-endian 16-bit value from buffer `b` at byte offset `off`.

**Returns:** `(b[off] << 8) | b[off+1]`. The read counterpart of `put-be16!` for
big-endian protocol fields.

---

## `put-be32!`

- **kind:** lisp-fn
- **lang:** lisp
- **source:** `lisp/lib/driver-util.clp`
- **hash:** 604f83131980ace9

Writes the 32-bit value `v` into buffer `b` at byte offset `off` in big-endian
order.

Composed from two `put-be16!` writes (high half-word at `off`, low at `off+2`).

---

## `get-be32`

- **kind:** lisp-fn
- **lang:** lisp
- **source:** `lisp/lib/driver-util.clp`
- **hash:** 450c1510052048ab

Reads a big-endian 32-bit value from buffer `b` at byte offset `off`.

**Returns:** `(get-be16 b off) << 16 | get-be16 b (off+2)`. The read counterpart
of `put-be32!`.

---

## `copy-bytes`

- **kind:** lisp-fn
- **lang:** lisp
- **source:** `lisp/lib/driver-util.clp`
- **hash:** e6a141c67c4646ee

Copies `len` bytes out of `src` starting at offset `off` into a freshly allocated
owned buffer, which it returns.

The NIC RX path needs this: a device receive buffer is recycled, so a frame
handed to the network stack must be snapshotted into memory the driver owns
first. Allocates `len` bytes with `make-bytes` and copies byte-by-byte.

**Parameters**
- `src` — source byte buffer.
- `off` — starting offset in `src`.
- `len` — number of bytes to copy.

**Returns:** a new `len`-byte buffer.

---

## `bytes-copy-into!`

- **kind:** lisp-fn
- **lang:** lisp
- **source:** `lisp/lib/driver-util.clp`
- **hash:** 101f267b6f8bc10e

Copies `len` bytes from the start of `src` into `dst` at offset `off`, in place.

**Parameters**
- `dst` — destination buffer (mutated).
- `off` — offset in `dst` to write at.
- `src` — source buffer (read from index 0).
- `len` — number of bytes.

**Returns:** `dst`.

---

## `put-list!`

- **kind:** lisp-fn
- **lang:** lisp
- **source:** `lisp/lib/driver-util.clp`
- **hash:** 2762034363759e2c

Writes a list of byte values into buffer `b` starting at offset `off`.

Iterates `lst`, writing each element as a u8 at consecutive offsets from `off`.
Handy for laying out small fixed byte sequences (e.g. a hardware command).

**Returns:** `b`.

---

## `wait-until`

- **kind:** lisp-fn
- **lang:** lisp
- **source:** `lisp/lib/driver-util.clp`
- **hash:** 2efb36d4e4ed372f

Polls predicate `pred` until it returns true or `timeout-ns` nanoseconds elapse,
yielding the CPU between polls.

The device-bring-up analogue of the C drivers' `timer_timeout` reset/link-settle
loops. It computes a deadline from `(uptime-ns)`, then loops: if `(pred)` is
true returns `#t`; if past the deadline returns `#f`; otherwise `(sleep 200000)`
(~200 µs) and loops. Under the scheduler `sleep` deschedules so other contexts
run while it waits; at boot-time direct eval (no other context) it falls back to
a counter wait. The ~200 µs poll interval is fine for millisecond-scale waits.

**Parameters**
- `pred` — a thunk tested each iteration.
- `timeout-ns` — maximum wait in nanoseconds.

**Returns:** `#t` if `pred` became true within the timeout, `#f` on timeout.

---

## `serve`

- **kind:** lisp-fn
- **lang:** lisp
- **source:** `lisp/lib/driver-util.clp`
- **hash:** da7119f62e57a53c

Spawns a long-lived restricted service context running a state-threading message
loop, and returns its context handle for callers to `send` to.

This is the canonical Cardinal Lisp **server mold**. OS services are long-lived
contexts that own some state and a message loop; callers `send` to them and never
call them synchronously, so the rx-handler-re-enters-tx self-deadlock that
plagued the C servers cannot arise. `serve` captures that shape once: it calls
`(spawn-restricted '() …)` — the **empty capability grant**, so a wedged or
compromised service cannot `import` new authority — running a loop seeded with
`init` that, for each received message `m`, computes `(step state m)` as the next
state. The `step` handler closes over whatever capabilities its defining module
imported lexically.

**Parameters**
- `init` — the initial state value threaded through the loop.
- `step` — a 2-arg function `(step state msg) -> next-state`.

**Returns:** the context handle (`send`-target) of the spawned service.

---

## `PCI-COMMAND`

- **kind:** lisp-const
- **lang:** lisp
- **source:** `lisp/lib/driver-util.clp`
- **hash:** 43c7b79fd752ed6e

PCI config-space offset of the 16-bit COMMAND register (`#x04`).

Bit 1 enables memory-space decoding; bit 2 enables bus mastering. Shared by every
PCI driver and re-exported by the `virtio` module.

---

## `bar-base`

- **kind:** lisp-fn
- **lang:** lisp
- **source:** `lisp/lib/driver-util.clp`
- **hash:** c8985ac271ebeca6

Resolves the base physical address of BAR `bar-idx` from a mapped PCI ECAM config
space, handling 64-bit memory BARs.

Reads the 32-bit BAR at `#x10 + bar-idx*4`. If its type field (bits 2:1) is `2`
(64-bit memory BAR) it combines the masked low dword with the next dword shifted
left 32; otherwise it returns the low dword masked to `#xFFFFFFF0`.

**Parameters**
- `cfg` — the mapped ECAM config-space byte region.
- `bar-idx` — BAR index 0..5.

**Returns:** the BAR's base physical address. (Also re-exported by `virtio`.)

---

## `pci-enable-mem-bus-master!`

- **kind:** lisp-fn
- **lang:** lisp
- **source:** `lisp/lib/driver-util.clp`
- **hash:** d4c5d813a291acde

Enables memory-space decoding and bus mastering on a PCI device by setting COMMAND
bits 1 and 2.

ORs `#x6` into the 16-bit COMMAND register of the mapped config space `cfg` — the
two bits every memory-mapped, DMA-capable PCI device needs before use.

**Parameters**
- `cfg` — the mapped ECAM config-space byte region.

---

## `find-virtio-cap`

- **kind:** lisp-fn
- **lang:** lisp
- **source:** `lisp/lib/virtio.clp`
- **hash:** 2409d276b3926da1

Walks the PCI capability list of config space `cfg` and returns the config offset
of the virtio vendor capability of the requested config type, or `#f`.

Starts from the capabilities pointer at `#x34` and follows the `next` link of each
capability, matching entries whose capability id is `PCI-CAP-VNDR` (`#x09`,
vendor-specific) and whose `cfg_type` byte (at `cap+3`) equals `cfg-type`.

**Parameters**
- `cfg` — mapped ECAM config space.
- `cfg-type` — one of `VIRTIO-CFG-COMMON` / `VIRTIO-CFG-NOTIFY` /
  `VIRTIO-CFG-DEVICE`.

**Returns:** the capability's config-space offset, or `#f` if not present.

---

## `bar-base`  (virtio re-export)

Re-exported from `driver-util` — see the `bar-base` entry above. The `virtio`
module lists it in its `(export …)` so existing `(import virtio)` users still see
it; the definition (and the hash) lives in `lisp/lib/driver-util.clp`.

---

## `map-cap`

- **kind:** lisp-fn
- **lang:** lisp
- **source:** `lisp/lib/virtio.clp`
- **hash:** 0d7a449304d9c570

Maps the BAR window that a virtio capability points at and returns an MMIO byte
region over it.

Reads the capability's `bar` index (at `cap-off+4`), `offset` (u32 at `cap+8`),
and `length` (u32 at `cap+12`), then `mmio-map`s `bar-base(cfg,bar)+offset` for
`length` bytes.

**Parameters**
- `cfg` — mapped ECAM config space.
- `cap-off` — config offset of a virtio capability (from `find-virtio-cap`).

**Returns:** a mapped byte region over the capability's MMIO window.

---

## `VIRTIO-CFG-COMMON`

- **kind:** lisp-const
- **lang:** lisp
- **source:** `lisp/lib/virtio.clp`
- **hash:** 6b662f49379bcd9b

The virtio PCI capability `cfg_type` for the **common configuration** structure
(`1`). Passed to `find-virtio-cap`.

---

## `VIRTIO-CFG-NOTIFY`

- **kind:** lisp-const
- **lang:** lisp
- **source:** `lisp/lib/virtio.clp`
- **hash:** f06ee5f421d70802

The virtio PCI capability `cfg_type` for the **notification** structure (`2`).
Passed to `find-virtio-cap`; the dword at `cap+16` is the notify-off multiplier.

---

## `VIRTIO-CFG-DEVICE`

- **kind:** lisp-const
- **lang:** lisp
- **source:** `lisp/lib/virtio.clp`
- **hash:** fe4f33f6b8497264

The virtio PCI capability `cfg_type` for the **device-specific configuration**
structure (`4`). Passed to `find-virtio-cap`.

---

## `virtio-status-set!`

- **kind:** lisp-fn
- **lang:** lisp
- **source:** `lisp/lib/virtio.clp`
- **hash:** 203b652f074df118

Sets (ORs in) status `bits` in the virtio DEVICE_STATUS register of the common
config region.

Reads the current u8 DEVICE_STATUS (offset 20 in the common region), ORs `bits`,
and writes it back — the additive step of the virtio status handshake.

**Parameters**
- `common` — mapped virtio common-config region.
- `bits` — status bits to set (the `VIRTIO-STATUS-*` constants).

---

## `virtio-features-offered`

- **kind:** lisp-fn
- **lang:** lisp
- **source:** `lisp/lib/virtio.clp`
- **hash:** f2887255a9672d88

Reads the 32-bit feature word the device offers for the given feature `select`
window.

Writes `select` to DEVICE_FEATURE_SELECT (0 = feature bits 0..31, 1 = 32..63)
then reads the DEVICE_FEATURE register.

**Parameters**
- `common` — mapped virtio common-config region.
- `select` — feature-word selector (0 or 1).

**Returns:** the 32-bit offered-features word for that window.

---

## `virtio-features-accept!`

- **kind:** lisp-fn
- **lang:** lisp
- **source:** `lisp/lib/virtio.clp`
- **hash:** 11cb97f35acaf493

Writes the driver's accepted features for the given `select` window.

Writes `select` to DRIVER_FEATURE_SELECT then `val` to DRIVER_FEATURE — the
driver's half of feature negotiation (callers pass `offered & want`).

**Parameters**
- `common` — mapped virtio common-config region.
- `select` — feature-word selector (0 or 1).
- `val` — the 32-bit accepted-features word.

---

## `VIRTIO-STATUS-ACK`

- **kind:** lisp-const
- **lang:** lisp
- **source:** `lisp/lib/virtio.clp`
- **hash:** 0bbfc4222aa74f35

Device-status bit `ACKNOWLEDGE` (`1`): the guest has noticed the device.

---

## `VIRTIO-STATUS-DRIVER`

- **kind:** lisp-const
- **lang:** lisp
- **source:** `lisp/lib/virtio.clp`
- **hash:** f8c43ba13c6969a8

Device-status bit `DRIVER` (`2`): the guest knows how to drive the device.

---

## `VIRTIO-STATUS-DRIVER-OK`

- **kind:** lisp-const
- **lang:** lisp
- **source:** `lisp/lib/virtio.clp`
- **hash:** d38847ab02408187

Device-status bit `DRIVER_OK` (`4`): the driver is set up and the device may run.
Set last by a driver after its queues exist.

---

## `VIRTIO-STATUS-FEATURES-OK`

- **kind:** lisp-const
- **lang:** lisp
- **source:** `lisp/lib/virtio.clp`
- **hash:** 9e4d9c3b8f9863d4

Device-status bit `FEATURES_OK` (`8`): feature negotiation is complete. After
setting it the driver re-reads DEVICE_STATUS; if the device cleared the bit it
rejected the feature set.

---

## `VIRTIO-F-VERSION-1-BIT`

- **kind:** lisp-const
- **lang:** lisp
- **source:** `lisp/lib/virtio.clp`
- **hash:** 0b714e8a8f7154bf

The feature **bit index** (`0`) of `VIRTIO_F_VERSION_1` within the low feature
word — the modern (1.0) virtio device flag every driver must negotiate.

---

## `VIRTQ-DESC-F-NEXT`

- **kind:** lisp-const
- **lang:** lisp
- **source:** `lisp/lib/virtio.clp`
- **hash:** 1426ad9453efa19a

Split-virtqueue descriptor flag `NEXT` (`1`): this descriptor chains to the one
named in its `next` field.

---

## `VIRTQ-DESC-F-WRITE`

- **kind:** lisp-const
- **lang:** lisp
- **source:** `lisp/lib/virtio.clp`
- **hash:** 854764765ebb91c4

Split-virtqueue descriptor flag `WRITE` (`2`): the buffer is device-writable
(the device writes into it, e.g. a response buffer).

---

## `desc-set!`

- **kind:** lisp-fn
- **lang:** lisp
- **source:** `lisp/lib/virtio.clp`
- **hash:** 9f07a79872885f5c

Writes descriptor `i` of a split virtqueue descriptor table.

Each descriptor is 16 bytes (`addr` u64, `len` u32, `flags` u16, `next` u16); this
writes all four fields at base `i*16`.

**Parameters**
- `desc` — the descriptor-table buffer.
- `i` — descriptor index.
- `addr` — buffer physical address.
- `len` — buffer length.
- `flags` — descriptor flags (`VIRTQ-DESC-F-*`).
- `next` — index of the next descriptor in the chain.

---

## `avail-push!`

- **kind:** lisp-fn
- **lang:** lisp
- **source:** `lisp/lib/virtio.clp`
- **hash:** d8acee9b227e5c97

Publishes descriptor index `d` into the available ring and bumps the free-running
16-bit `idx`.

Writes `d` into the avail ring slot at `4 + 2*(idx mod qsize)`, then stores
`(idx+1) & 0xFFFF` back as the new avail `idx` (offset 2). The standard split-ring
publish step.

**Parameters**
- `avail` — the avail-ring buffer.
- `qsize` — queue size (ring length).
- `d` — head descriptor index to make available.

---

## `q-size`

- **kind:** lisp-fn
- **lang:** lisp
- **source:** `lisp/lib/virtio.clp`
- **hash:** 1f100129613809b6

Accessor: the queue size from a queue record `(size desc avail used notify-off)`
(element 0).

---

## `q-desc`

- **kind:** lisp-fn
- **lang:** lisp
- **source:** `lisp/lib/virtio.clp`
- **hash:** e07bad15c9508722

Accessor: the descriptor-table buffer from a queue record (element 1).

---

## `q-avail`

- **kind:** lisp-fn
- **lang:** lisp
- **source:** `lisp/lib/virtio.clp`
- **hash:** e7b761c988f9c8ed

Accessor: the avail-ring buffer from a queue record (element 2).

---

## `q-used`

- **kind:** lisp-fn
- **lang:** lisp
- **source:** `lisp/lib/virtio.clp`
- **hash:** 6fdefff2a511ed8b

Accessor: the used-ring buffer from a queue record (element 3).

---

## `q-noff`

- **kind:** lisp-fn
- **lang:** lisp
- **source:** `lisp/lib/virtio.clp`
- **hash:** b12c97d9358ad973

Accessor: the queue's notify-offset from a queue record (element 4). Combined with
the notify-off multiplier to find the queue's notification register.

---

## `virtio-setup-queue`

- **kind:** lisp-fn
- **lang:** lisp
- **source:** `lisp/lib/virtio.clp`
- **hash:** c4e408a9530b1374

Selects queue `qidx`, allocates its split-ring buffers, hands the device their
physical addresses, points its MSI-X vector at table entry 0, and enables it.

Writes `qidx` to QUEUE_SELECT and reads QUEUE_SIZE; returns `#f` if the size is 0
(queue absent). Otherwise it `dma-alloc`s the descriptor table (`qsize*16`), avail
ring (`6 + 2*qsize`), and used ring (`6 + 8*qsize`), writes their physical
addresses to QUEUE_DESC/DRIVER/DEVICE, sets QUEUE_MSIX to 0, and sets QUEUE_ENABLE.

**Parameters**
- `common` — mapped virtio common-config region.
- `qidx` — queue index.

**Returns:** the queue record `(qsize desc avail used noff)`, or `#f` if the queue
size is 0.

---

## `notify-queue!`

- **kind:** lisp-fn
- **lang:** lisp
- **source:** `lisp/lib/virtio.clp`
- **hash:** 20a463279807acfe

Notifies the device that a queue has new available entries.

Writes a (device-ignored) u16 0 into the notify region at `q-noff(q) * mult` — the
queue's notification address.

**Parameters**
- `notify` — the mapped NOTIFY region.
- `mult` — the notify-off multiplier (from the device record).
- `q` — the queue record.

---

## `virtio-bringup`

- **kind:** lisp-fn
- **lang:** lisp
- **source:** `lisp/lib/virtio.clp`
- **hash:** b44cdd5dbf0f3abd

The device-agnostic virtio 1.0 discover-and-negotiate dance: maps the three config
regions, drives the status handshake, and negotiates features.

Maps `#x1000` of ECAM, enables memory + bus-master, then `find-virtio-cap` +
`map-cap`s the COMMON, DEVICE, and NOTIFY regions (reading the notify-off
multiplier from `ncap+16`). It clears DEVICE_STATUS, sets ACK then DRIVER, reads
both offered feature words and accepts `offered & want` per word (so device-
specific feature knowledge stays in the caller), sets FEATURES_OK, and re-reads
it.

**Parameters**
- `ecam` — the device's ECAM physical base.
- `lo-want` — desired feature mask for bits 0..31.
- `hi-want` — desired feature mask for bits 32..63.

**Returns:** the device record `(common devcfg notify mult ecam)`, or `#f` if the
device cleared FEATURES_OK (rejected the negotiated feature set).

---

## `ctrlq-cmd!`

- **kind:** lisp-fn
- **lang:** lisp
- **source:** `lisp/lib/virtio.clp`
- **hash:** d4d7205a4dd61a22

Runs the 2-descriptor command/response idiom on a control-style virtqueue and
returns the device's response buffer.

Descriptor 0 is the device-readable command buffer (flagged `NEXT` → descriptor
1); descriptor 1 is the device-writable response buffer. The control queue is
strictly serial (one command in flight), so the fixed pair (0,1) needs no
allocator. It snapshots `used.idx`, posts the pair on the avail ring, kicks the
device with `notify-queue!`, then `wait-until` `used.idx` advances.

**Parameters**
- `q` — the control queue record.
- `notify`, `mult` — the NOTIFY region and notify-off multiplier.
- `cmd`, `cmd-len` — command buffer and its length.
- `resp`, `resp-len` — response buffer and its length.
- `timeout-ns` — wait budget in nanoseconds.

**Returns:** `resp` (the response buffer) on completion, or `#f` on timeout.

---

## `used-advanced?`

- **kind:** lisp-fn
- **lang:** lisp
- **source:** `lisp/lib/virtio.clp`
- **hash:** 50fc5a15e879cf84

Tests whether a queue's used ring has advanced past a snapshot index `last`.

**Returns:** `#t` iff the current used `idx` (u16 at offset 2 of the used ring)
differs from `last`. Used as the completion predicate inside `ctrlq-cmd!` and by
drivers polling for queue completions.

---

## `VIRTIO-DEVICE-STATUS`

- **kind:** lisp-const
- **lang:** lisp
- **source:** `lisp/lib/virtio.clp`
- **hash:** 80bac16604592f44

Offset of the u8 DEVICE_STATUS register within the common-config region (`20`).
Re-exported so drivers can poke device-status directly when they need explicit
DRIVER_OK timing control.

---

## `VIRTIO-MSIX-CONFIG`

- **kind:** lisp-const
- **lang:** lisp
- **source:** `lisp/lib/virtio.clp`
- **hash:** d7b7795d7d90e026

Offset of the u16 MSIX config-vector register within the common-config region
(`16`). Re-exported for drivers managing the device's MSI-X config vector.

---

## `start-audio-service`

- **kind:** lisp-fn
- **lang:** lisp
- **source:** `lisp/servers/coreaudio.clp`
- **hash:** b2f8c4e9c58f87d6

Spawns the audio service and returns the handle audio drivers `send` to.

A placeholder server mirroring the C `CoreAudio` stub: a `serve` loop holding a
list of registered cards. It handles `(register <name>)` by logging the card and
prepending its name to the list; all other messages are ignored. The real
mixing/stream plumbing is TODO — the endpoint exists so a future driver
(e.g. hdaudio) has something to attach to.

**Returns:** the service context handle.

---

## `start-display-service`

- **kind:** lisp-fn
- **lang:** lisp
- **source:** `lisp/servers/coredisplay.clp`
- **hash:** 03c09b5c0b12c892

Spawns the display registry service and returns its handle.

A `serve` loop owning a list of registered displays. It handles
`(register <name> <connection> <ctx>)` by logging and storing `(name connection
ctx)`; other messages are ignored. Boot policy (in `init`) loads the `lfb`
fallback only when no real display driver is present.

**Returns:** the service context handle.

---

## `parse-edid`

- **kind:** lisp-fn
- **lang:** lisp
- **source:** `lisp/servers/coredisplay.clp`
- **hash:** 24d8a66edc8d0d28

Pure parser turning a 128-byte EDID blob into resolutions and a monitor name; a
direct Lisp port of `coredisplay_parse_edid` (`servers/.../edid.c`).

Validates the 8-byte EDID header magic, decodes the colour bit-depth (byte 20),
established-timings bitmap (bytes 35–37), the eight standard-timing slots
(bytes 38–53), and the four 18-byte detailed descriptors (byte 54), extracting a
`0xFC` display name and each digital detailed mode's full timing geometry.

**Parameters**
- `b` — a 128-byte EDID byte buffer.

**Returns:** an alist
`((bit-depth . N) (gamma . N) (established . N) (standard-timings . (…))
(display-name . "…") (detailed-modes . (<mode-alist> …)))`, or `#f` if the header
is bad, the bit-depth code is reserved, or any detailed mode is analog
(unsupported) — matching the C's `false` returns. Each detailed mode is itself an
alist of `pixel-clock`, `hactive`/`vactive`, blanking, sync porch/pulse, physical
size, and borders.

---

## `start-input-service`

- **kind:** lisp-fn
- **lang:** lisp
- **source:** `lisp/servers/coreinput.clp`
- **hash:** 89c6cc5823f78010

Spawns the asynchronous input service and returns the handle input drivers `send`
to.

The canonical Cardinal Lisp server: a `serve` loop owning a registered-device
list. It handles `(register <name>)` (logs, prepends the name) and `(event
<payload>)` (logs the event); other messages are ignored. There is no synchronous
callback ABI, so the rx-re-enters-tx deadlock cannot arise. Pure mechanism —
*which* driver feeds it (ps2 today, USB HID later) is policy decided in `init`.

**Returns:** the service context handle.

---

## `start-power-service`

- **kind:** lisp-fn
- **lang:** lisp
- **source:** `lisp/servers/corepower.clp`
- **hash:** ff8c7275167110d1

Spawns the power-management service and returns the handle to `send`
registrations and power events to.

A `serve` loop owning a list of `(name class ctx)` device entries. Protocol:
`(register <name> <class-bits> <ctx>)` joins a device; `(event-g <class-bits>
<gstate> <pstate>)` and `(event-d <class-bits> <dstate>)` fan an event out — via
`send`, never a re-entrant callback — to every registered device whose class
bitmask intersects the event class, delivering `(pwr-g <gstate> <pstate>)` or
`(pwr-d <dstate>)` respectively.

**Returns:** the service context handle.

---

## `pwr-generic`

- **kind:** lisp-const
- **lang:** lisp
- **source:** `lisp/servers/corepower.clp`
- **hash:** 695783a6ff511b4a

Power device-class bit `generic` (`1`). Mirrors `device_pwr_class` in
`servers/inc/CorePower/power.h`.

---

## `pwr-display`

- **kind:** lisp-const
- **lang:** lisp
- **source:** `lisp/servers/corepower.clp`
- **hash:** 512e08b1342a8d74

Power device-class bit `display` (`2`).

---

## `pwr-audio-out`

- **kind:** lisp-const
- **lang:** lisp
- **source:** `lisp/servers/corepower.clp`
- **hash:** eb2b86b38864f096

Power device-class bit `audio-out` (`4`).

---

## `pwr-audio-in`

- **kind:** lisp-const
- **lang:** lisp
- **source:** `lisp/servers/corepower.clp`
- **hash:** f11c0e9404a3518f

Power device-class bit `audio-in` (`8`).

---

## `pwr-hid`

- **kind:** lisp-const
- **lang:** lisp
- **source:** `lisp/servers/corepower.clp`
- **hash:** 00913273b1248d2f

Power device-class bit `hid` (`16`).

---

## `pwr-camera`

- **kind:** lisp-const
- **lang:** lisp
- **source:** `lisp/servers/corepower.clp`
- **hash:** 7558071db5768718

Power device-class bit `camera` (`32`).

---

## `pwr-processor`

- **kind:** lisp-const
- **lang:** lisp
- **source:** `lisp/servers/corepower.clp`
- **hash:** daafd66a4c2aba8d

Power device-class bit `processor` (`64`).

---

## `start-storage-service`

- **kind:** lisp-fn
- **lang:** lisp
- **source:** `lisp/servers/corestorage.clp`
- **hash:** ebed8f993428bd02

Spawns the block-device + filesystem-provider registry service and returns its
handle; a Lisp port of `CoreStorage`.

A `serve` loop whose state is `(devs provs)`. It mediates all block I/O so the
device list stays the I/O authority. Protocol: `(register-blockdev <name> <bsize>
<bcount> <driver-ctx>)`, `(register-fsprovider <name> <provider-ctx>)`, `(claim
<name>)` (a provider mounts a device), `(read <name> <lba> <count> <reply>)`, and
`(write <name> <lba> <count> <data> <reply>)`. On registration it offers each
unclaimed device to each provider via a `(probe …)` message; read/write requests
are bounds-checked against the device's `bcount` and forwarded to the owning
driver context, or answered with `(complete -1 …)` on a bad name / out-of-range
range. No re-entrant calls, so a provider doing block I/O while probing cannot
deadlock the service.

**Returns:** the service context handle.

---

## `start-network-service`

- **kind:** lisp-fn
- **lang:** lisp
- **source:** `lisp/servers/corenetwork/service.clp`
- **hash:** 6d8ff27a615e2e3a

Spawns the IPv4 network stack service and returns its handle; the single export
of the `corenetwork` module (whose source is split across
`lisp/servers/corenetwork/*.clp` via `(include …)`).

A `serve` loop whose state is `(ip mac nic-tx arp-cache udp-binds)`, seeded with
`our-ip`. It owns the interface address, ARP cache, and UDP port table, demuxing
received frames (ethernet → ARP / IPv4 → ICMP / UDP) and building replies it
hands back to the NIC. Protocol: `(register-nic <mac-list> <tx-ctx>)`, `(rx
<frame-bytes> <len>)`, `(arp-request <ip-list>)`, `(arp-lookup <ip-list>
<reply-ctx>)`, `(udp-bind <port> <handler-ctx>)`, `(udp-send <dst-ip> <dst-mac>
<sport> <dport> <payload-bytes>)`, and `(ping <dst-ip> <dst-mac> <id> <seq>)`. A
bound UDP handler receives `(udp-rx <src-ip> <src-mac> <src-port> <payload>)`;
`arp-lookup` replies the cached MAC or `#f`. Every step is a message — no
re-entrancy, no locks.

**Parameters**
- `our-ip` — this interface's IPv4 address as a 4-element list.

**Returns:** the service context handle.

---

## `system-init`

- **kind:** lisp-fn
- **lang:** lisp
- **source:** `lisp/init.clp`
- **hash:** e1c299439e69865a

The system entry point: called once on the BSP after the scheduler is live to
bring up the `Core*` services and bind drivers to hardware — the single place boot
policy lives.

Brings up input (start `coreinput`, run `ps2-init` in the root context, spawn a
restricted keyboard pump), audio, and power; starts the storage registry and
`ahci-init`; starts the display registry, runs `virtio-gpu-init`, and falls back
to `lfb-init` only when no virtio-gpu device is on the bus; starts the network
stack for the slirp guest address `10.0.2.15`, brings up `virtio-net` (preferred)
or `rtl8139`, and primes the ARP cache with a who-has for the gateway. Each driver
bring-up is gated on `pci-find`, so a headless / device-less boot just logs and
continues. Services are spawned as restricted (`'()`-grant) contexts; `init`
itself holds root authority so its driver `import`s succeed.

**Returns:** the symbol `'system-up`.

---

## `start-repl`

- **kind:** lisp-fn
- **lang:** lisp
- **source:** `lisp/init.clp`
- **hash:** b913c3bbd2f6a179

Spawns the interactive serial REPL on COM1 (started only under the
`cardinal.repl` boot gate).

Spawns a **root** context (a debug shell wants full authority): it `import`s
`sys-console` + `sys-irq`, registers COM1 receive (ISA IRQ 4) with
`irq-register`, arms the UART RX interrupt with `console-arm-rx`, prints and
flushes a banner, then parks on the IRQ. On an arriving byte it drains
`console-poll`, evaluates the input with `repl-eval`, writes and flushes the
transcript, and parks again — no busy-polling. The IRQ count is snapshotted
before each drain so a byte landing mid-drain is never lost.

**Returns:** the spawned REPL context handle (the spawn result).

---

## `start-usb-service`

- **kind:** lisp-fn
- **lang:** lisp
- **source:** `lisp/servers/coreusb/enum.clp`
- **hash:** 8d395edf5af76543

Runs the `coreusb` USB enumeration + class-dispatch service as a `serve` registry context (never returns).

The single public entry point of the service. It calls `serve` with the initial
state `('() '() '())` — `(class-table used-addrs records)`, where `class-table`
is an alist of `(class-byte . class-ctx)`, `used-addrs` is the list of allocated
USB bus addresses, and `records` is the list of enumerated-device records
`(address hci port parent class)`. The returned context handle is the one other
USB drivers send messages to. The service holds **no** hardware capability — it
only routes messages and parses descriptor bytes.

**Message protocol** (send a list to the service handle):
- `(register-class <class-byte> <class-ctx>)` — a class driver registers itself
  to receive `(probe dev)` / `(remove addr)` for devices of `<class-byte>`.
- `(port-connected <hci-ctx> <port> <speed>)` — a host controller reports a
  root-port attach; spawns an enumerator with `parent = 0`.
- `(port-disconnected <hci-ctx> <port>)` — root-port detach; tears down the
  matching records.
- `(enumerate-downstream <hci-ctx> <parent-addr> <port> <speed>)` — a hub driver
  asks for enumeration of a device behind it.
- `(disconnect-downstream <hci-ctx> <parent-addr> <port>)` — a hub driver
  reports a downstream detach.
- `(enum-done <addr> <hci> <port> <parent> <class>)` and `(enum-failed <addr>)`
  are **internal** reports from a spawned enumerator context.

On attach it allocates the lowest free bus address (1..127), reserves it
immediately so a concurrent connect cannot reuse it, and spawns a restricted
enumerator context to run the USB 2.0 ch.9 sequence. On disconnect it notifies
the owning class driver (`(remove addr)`), tells the controller
(`(disconnect-dev addr)`), and frees the address.

---

## `usb-control-in`

- **kind:** lisp-fn
- **lang:** lisp
- **source:** `lisp/servers/coreusb/proto.clp`
- **hash:** 65fb791e66dacdb5

Issues an IN-direction control transfer to a USB device and returns the received data bytevector, or `#f` on error.

**Parameters:** `dev` (an enumerated-device value from a `probe`), `bmReq`
(bmRequestType bits other than direction), `bReq` (bRequest), `wValue`, `wIndex`,
`len` (wLength / max bytes to read).

Builds an 8-byte setup packet with `USB-REQ-DIR-IN` OR-ed into `bmReq`, sends a
`control` request to the device's host-controller context, and waits for the
`(complete n data)` reply. **Returns** the data bytevector (length ≤ `len`), or
`#f` if the completion byte count `n` is negative (error/timeout/STALL). Runs in
the class driver's own context.

---

## `usb-control-out`

- **kind:** lisp-fn
- **lang:** lisp
- **source:** `lisp/servers/coreusb/proto.clp`
- **hash:** 8d017c90a1df5f5f

Issues an OUT-direction control transfer to a USB device and returns the byte count transferred (`-1` on error).

**Parameters:** `dev`, `bmReq`, `bReq`, `wValue`, `wIndex`, `data` (OUT payload
bytevector, or `#f` for no data), `len` (wLength).

Builds an 8-byte setup packet with `USB-REQ-DIR-OUT` OR-ed into `bmReq`, sends a
`control` request carrying `data` to the device's host-controller context, and
**returns** the completion byte count (`>= 0` on success, negative on error).

---

## `usb-interrupt-in`

- **kind:** lisp-fn
- **lang:** lisp
- **source:** `lisp/servers/coreusb/proto.clp`
- **hash:** 948121772afe492b

Issues an interrupt-IN transfer on a given endpoint and returns the received bytevector, or `#f` on error.

**Parameters:** `dev`, `endpoint` (endpoint address), `max-packet` (max packet
size), `len` (bytes to read).

Sends an `interrupt-in` request to the device's host-controller context and waits
for completion. **Returns** the data bytevector, or `#f` if the completion byte
count is negative. Used by the HID and hub poll loops to read interrupt endpoints.

---

## `usb-bulk-in`

- **kind:** lisp-fn
- **lang:** lisp
- **source:** `lisp/servers/coreusb/proto.clp`
- **hash:** a247beed0f97501d

Issues a bulk-IN transfer on a given endpoint and returns the received bytevector, or `#f` on error.

**Parameters:** `dev`, `endpoint`, `max-packet`, `len` (bytes to read).

Sends a `bulk` request with `dir-in? = #t` and no OUT payload to the device's
host-controller context. **Returns** the data bytevector, or `#f` if the
completion byte count is negative.

---

## `usb-bulk-out`

- **kind:** lisp-fn
- **lang:** lisp
- **source:** `lisp/servers/coreusb/proto.clp`
- **hash:** 25770b65b128b3d6

Issues a bulk-OUT transfer on a given endpoint and returns the byte count transferred (negative on error).

**Parameters:** `dev`, `endpoint`, `max-packet`, `data` (OUT payload bytevector),
`len`.

Sends a `bulk` request with `dir-in? = #f` carrying `data` to the device's
host-controller context. **Returns** the completion byte count.

---

## `usb-find-endpoint`

- **kind:** lisp-fn
- **lang:** lisp
- **source:** `lisp/servers/coreusb/proto.clp`
- **hash:** 4b20cc079e1bce88

Finds the first endpoint of a given transfer type and direction in a device's configuration descriptor.

**Parameters:** `dev`, `type` (transfer type from bmAttributes bits 1:0 —
`USB-XFER-BULK` = 2, `USB-XFER-INTERRUPT` = 3), `dir-in?` (`#t` for an IN
endpoint).

Walks the configuration-descriptor chain (`usb-dev-config`) by `bLength`, looking
for an endpoint descriptor whose attributes match `type` and whose
direction bit (bmEndpointAddress bit 7) matches `dir-in?`. **Returns**
`(list ep-address max-packet)` for the first match (max-packet read as a u16 from
wMaxPacketSize), or `#f` if none is found.

---

## `usb-iface-class`

- **kind:** lisp-fn
- **lang:** lisp
- **source:** `lisp/servers/coreusb/proto.clp`
- **hash:** 8f7cc6994e070851

Returns the first interface descriptor's bInterfaceClass, or `-1` if there is no interface descriptor.

**Parameters:** `dev`. Walks the configuration descriptor for the first
`USB-DESC-INTERFACE` descriptor and reads byte offset 5 (bInterfaceClass).
**Returns** the class byte, or `-1`. (Thin wrapper over the internal
`usb-iface-field` helper.)

---

## `usb-iface-protocol`

- **kind:** lisp-fn
- **lang:** lisp
- **source:** `lisp/servers/coreusb/proto.clp`
- **hash:** 652e2c70f97d4835

Returns the first interface descriptor's bInterfaceProtocol, or `-1` if there is no interface descriptor.

**Parameters:** `dev`. Reads byte offset 7 (bInterfaceProtocol) of the first
interface descriptor in the configuration. **Returns** the protocol byte, or `-1`.

---

## `usb-iface-number`

- **kind:** lisp-fn
- **lang:** lisp
- **source:** `lisp/servers/coreusb/proto.clp`
- **hash:** 72fc9a89ec2d83a1

Returns the first interface descriptor's bInterfaceNumber, or `-1` if there is no interface descriptor.

**Parameters:** `dev`. Reads byte offset 2 (bInterfaceNumber) of the first
interface descriptor in the configuration. **Returns** the interface number, or
`-1`.

---

## `usb-mark-hub`

- **kind:** lisp-fn
- **lang:** lisp
- **source:** `lisp/servers/coreusb/proto.clp`
- **hash:** c95a7dcfb2b29b6a

Tells the host controller that a device is a USB hub so it routes transfers to devices behind it.

**Parameters:** `dev`, `nports` (number of downstream ports).

Sends a `mark-hub` request to the device's host-controller context and
**returns** the completion status byte. xHCI uses this to update the device's
slot context; UHCI replies with a no-op `(complete 0 #f)`.

---

## `usb-enumerate-downstream`

- **kind:** lisp-fn
- **lang:** lisp
- **source:** `lisp/servers/coreusb/proto.clp`
- **hash:** 7c8347ffd907aaea

Asks the coreusb service to enumerate a device newly attached to a downstream hub port.

**Parameters:** `usb` (the coreusb service handle), `dev` (the hub device),
`port` (downstream port number), `speed`.

Fire-and-forget `send` to the coreusb service of
`(enumerate-downstream <hub-hci> <hub-addr> port speed)`, which spawns an
enumerator for that port with the hub as the parent. Called by the hub class
driver.

---

## `usb-disconnect-downstream`

- **kind:** lisp-fn
- **lang:** lisp
- **source:** `lisp/servers/coreusb/proto.clp`
- **hash:** 9e56cf22ffbd8ea8

Tells the coreusb service that a device on a downstream hub port has detached.

**Parameters:** `usb` (the coreusb service handle), `dev` (the hub device),
`port`.

Fire-and-forget `send` to the coreusb service of
`(disconnect-downstream <hub-hci> <hub-addr> port)`, which tears down the
matching device record (notifies its class driver and controller, frees its
address). Called by the hub class driver.

---

## `usb-dev-hci`

- **kind:** lisp-fn
- **lang:** lisp
- **source:** `lisp/servers/coreusb/proto.clp`
- **hash:** ea89f98b0e7f45c6

Returns the host-controller context handle of an enumerated-device value.

**Parameters:** `d` (an enumerated-device value, the 6-element list
`(hci address speed max-packet0 config-bytes config-len)`). **Returns** the first
element — the controller context a class driver sends transfer messages to.

---

## `usb-dev-address`

- **kind:** lisp-fn
- **lang:** lisp
- **source:** `lisp/servers/coreusb/proto.clp`
- **hash:** 44e31195b1d61fed

Returns the assigned USB bus address of an enumerated-device value.

**Parameters:** `d`. **Returns** the device's bus address (1..127) assigned
during enumeration.

---

## `usb-dev-speed`

- **kind:** lisp-fn
- **lang:** lisp
- **source:** `lisp/servers/coreusb/proto.clp`
- **hash:** b50f1e774eacd358

Returns the link speed of an enumerated-device value.

**Parameters:** `d`. **Returns** the device speed, one of `USB-SPEED-LOW`,
`USB-SPEED-FULL`, `USB-SPEED-HIGH`, `USB-SPEED-SUPER`.

---

## `usb-dev-config`

- **kind:** lisp-fn
- **lang:** lisp
- **source:** `lisp/servers/coreusb/proto.clp`
- **hash:** 977b8b3bbcf7a687

Returns the raw configuration-descriptor bytevector of an enumerated-device value.

**Parameters:** `d`. **Returns** the full configuration descriptor (header +
interface + endpoint descriptors) as a bytevector, the buffer walked by
`usb-find-endpoint` and the `usb-iface-*` accessors.

---

## `usb-dev-config-len`

- **kind:** lisp-fn
- **lang:** lisp
- **source:** `lisp/servers/coreusb/proto.clp`
- **hash:** 771653fb705ab94b

Returns the byte length of an enumerated-device value's configuration descriptor.

**Parameters:** `d`. **Returns** the length (in bytes) of the configuration
descriptor returned by `usb-dev-config` (capped at 512 during enumeration).

---

## `complete-n`

- **kind:** lisp-fn
- **lang:** lisp
- **source:** `lisp/servers/coreusb/proto.clp`
- **hash:** e109aeb6a81d3ea7

Extracts the byte-count field from a host-controller `(complete n data)` reply.

**Parameters:** `c` (a completion message `(complete n data)`). **Returns** `n`,
the number of bytes transferred — negative on error, timeout, or STALL. Used by
drivers that issue raw `hci-*` round-trips and inspect the result directly.

---

## `complete-data`

- **kind:** lisp-fn
- **lang:** lisp
- **source:** `lisp/servers/coreusb/proto.clp`
- **hash:** 86ac81785ca428e1

Extracts the data bytevector from a host-controller `(complete n data)` reply.

**Parameters:** `c` (a completion message). **Returns** the data field — a fresh
bytevector for IN transfers (the caller owns its own copy), or `#f` for
OUT/no-data transfers.

---

## `USB-REQ-DIR-IN`

- **kind:** lisp-const
- **lang:** lisp
- **source:** `lisp/servers/coreusb/proto.clp`
- **hash:** ac5e6e7f285df877

bmRequestType direction bit for device-to-host (IN) control transfers — `#x80`.

OR-ed into `bmRequestType`; `usb-control-in` sets it automatically.

---

## `USB-REQ-DIR-OUT`

- **kind:** lisp-const
- **lang:** lisp
- **source:** `lisp/servers/coreusb/proto.clp`
- **hash:** efd4626739fa4a68

bmRequestType direction bit for host-to-device (OUT) control transfers — `#x00`.

OR-ed into `bmRequestType`; `usb-control-out` sets it automatically.

---

## `USB-REQ-TYPE-CLASS`

- **kind:** lisp-const
- **lang:** lisp
- **source:** `lisp/servers/coreusb/proto.clp`
- **hash:** 7e666bf2be568132

bmRequestType type field selecting a class-specific request — `#x20`.

OR-ed into `bmRequestType` for class requests (e.g. HID SET_PROTOCOL, hub
port-status requests).

---

## `USB-REQ-RECIP-INTERFACE`

- **kind:** lisp-const
- **lang:** lisp
- **source:** `lisp/servers/coreusb/proto.clp`
- **hash:** 96444eb32fcf7595

bmRequestType recipient field selecting an interface — `#x01`.

OR-ed into `bmRequestType` to target an interface (e.g. HID class requests).

---

## `USB-REQ-RECIP-OTHER`

- **kind:** lisp-const
- **lang:** lisp
- **source:** `lisp/servers/coreusb/proto.clp`
- **hash:** d0a543df029310b4

bmRequestType recipient field selecting "other" (e.g. a hub port) — `#x03`.

OR-ed into `bmRequestType` to target a hub port for port-feature requests.

---

## `USB-REQ-GET-STATUS`

- **kind:** lisp-const
- **lang:** lisp
- **source:** `lisp/servers/coreusb/proto.clp`
- **hash:** 844673c299a3323d

Standard `GET_STATUS` bRequest code — `0`.

Passed as `bReq` to the control-transfer API.

---

## `USB-REQ-CLEAR-FEATURE`

- **kind:** lisp-const
- **lang:** lisp
- **source:** `lisp/servers/coreusb/proto.clp`
- **hash:** 74d950365827da17

Standard `CLEAR_FEATURE` bRequest code — `1`.

Passed as `bReq` to the control-transfer API (e.g. clearing a hub port feature).

---

## `USB-REQ-SET-FEATURE`

- **kind:** lisp-const
- **lang:** lisp
- **source:** `lisp/servers/coreusb/proto.clp`
- **hash:** 2b2e22b1d077fc56

Standard `SET_FEATURE` bRequest code — `3`.

Passed as `bReq` to the control-transfer API (e.g. setting a hub port feature
such as port reset/power).

---

## `USB-REQ-GET-DESCRIPTOR`

- **kind:** lisp-const
- **lang:** lisp
- **source:** `lisp/servers/coreusb/proto.clp`
- **hash:** ed294e5355d33f42

Standard `GET_DESCRIPTOR` bRequest code — `6`.

Passed as `bReq` with `wValue = (descriptor-type << 8 | index)`; the enumerator
uses it to read device and configuration descriptors.

---

## `USB-REQ-SET-INTERFACE`

- **kind:** lisp-const
- **lang:** lisp
- **source:** `lisp/servers/coreusb/proto.clp`
- **hash:** 5ac0e4297edc26c8

Standard `SET_INTERFACE` bRequest code — `11`.

Passed as `bReq` to the control-transfer API to select an alternate interface
setting.

---

## `USB-REQ-SET-ADDRESS`

- **kind:** lisp-const
- **lang:** lisp
- **source:** `lisp/servers/coreusb/proto.clp`
- **hash:** 2ec0d6356310f041

Standard `SET_ADDRESS` bRequest code — `5`.

Used by the enumerator (while the device is still at the default address 0) to
assign the device its allocated bus address.

---

## `USB-REQ-SET-CONFIGURATION`

- **kind:** lisp-const
- **lang:** lisp
- **source:** `lisp/servers/coreusb/proto.clp`
- **hash:** d64725f7b95fb46d

Standard `SET_CONFIGURATION` bRequest code — `9`.

Used by the enumerator to select the device's configuration (by bConfigurationValue)
before dispatching to the class driver.

---

## `USB-DESC-DEVICE`

- **kind:** lisp-const
- **lang:** lisp
- **source:** `lisp/servers/coreusb/proto.clp`
- **hash:** 2a498c0dc33134d5

Device-descriptor type code — `1`.

Shifted into `wValue` for `GET_DESCRIPTOR`; the enumerator reads the device
descriptor first (8 bytes, then the full 18).

---

## `USB-DESC-CONFIG`

- **kind:** lisp-const
- **lang:** lisp
- **source:** `lisp/servers/coreusb/proto.clp`
- **hash:** 6219461982214f17

Configuration-descriptor type code — `2`.

Shifted into `wValue` for `GET_DESCRIPTOR`; the enumerator reads the 9-byte
header for wTotalLength, then the full configuration.

---

## `USB-DESC-INTERFACE`

- **kind:** lisp-const
- **lang:** lisp
- **source:** `lisp/servers/coreusb/proto.clp`
- **hash:** c4fc8d2d0d92e603

Interface-descriptor type code — `4`.

The descriptor type matched by the `usb-iface-*` accessors when walking the
configuration descriptor.

---

## `USB-DESC-ENDPOINT`

- **kind:** lisp-const
- **lang:** lisp
- **source:** `lisp/servers/coreusb/proto.clp`
- **hash:** 369bc5f3d7465750

Endpoint-descriptor type code — `5`.

The descriptor type matched by `usb-find-endpoint` when walking the configuration
descriptor.

---

## `USB-CLASS-HID`

- **kind:** lisp-const
- **lang:** lisp
- **source:** `lisp/servers/coreusb/proto.clp`
- **hash:** aa8ffdf81ee8d9e2

USB HID device-class code — `#x03`.

The class byte a HID driver passes to `(register-class …)`; the enumerator
dispatches matching devices to it.

---

## `USB-CLASS-MASS-STORAGE`

- **kind:** lisp-const
- **lang:** lisp
- **source:** `lisp/servers/coreusb/proto.clp`
- **hash:** d328c21b81ced458

USB mass-storage device-class code — `#x08`.

The class byte the usb-storage driver registers for.

---

## `USB-CLASS-HUB`

- **kind:** lisp-const
- **lang:** lisp
- **source:** `lisp/servers/coreusb/proto.clp`
- **hash:** f9fe8d178789429f

USB hub device-class code — `#x09`.

The class byte the usb-hub driver registers for.

---

## `USB-XFER-BULK`

- **kind:** lisp-const
- **lang:** lisp
- **source:** `lisp/servers/coreusb/proto.clp`
- **hash:** b4e23d80dc6bd86a

Bulk transfer type (bmAttributes bits 1:0) — `2`.

Passed as the `type` argument to `usb-find-endpoint` to locate a bulk endpoint.

---

## `USB-XFER-INTERRUPT`

- **kind:** lisp-const
- **lang:** lisp
- **source:** `lisp/servers/coreusb/proto.clp`
- **hash:** 941d27bca23543e8

Interrupt transfer type (bmAttributes bits 1:0) — `3`.

Passed as the `type` argument to `usb-find-endpoint` to locate an interrupt
endpoint.

---

## `USB-SPEED-LOW`

- **kind:** lisp-const
- **lang:** lisp
- **source:** `lisp/servers/coreusb/proto.clp`
- **hash:** 95401618e76c4486

Low-speed link rate (`usb_speed_t`) — `0`.

One of the values reported by `usb-dev-speed` and carried in port-connect /
transfer messages.

---

## `USB-SPEED-FULL`

- **kind:** lisp-const
- **lang:** lisp
- **source:** `lisp/servers/coreusb/proto.clp`
- **hash:** 100fc6d3e15cfcf0

Full-speed link rate (`usb_speed_t`) — `1`.

One of the values reported by `usb-dev-speed`.

---

## `USB-SPEED-HIGH`

- **kind:** lisp-const
- **lang:** lisp
- **source:** `lisp/servers/coreusb/proto.clp`
- **hash:** 6d4779a67ec6a4a5

High-speed link rate (`usb_speed_t`) — `2`.

One of the values reported by `usb-dev-speed`.

---

## `USB-SPEED-SUPER`

- **kind:** lisp-const
- **lang:** lisp
- **source:** `lisp/servers/coreusb/proto.clp`
- **hash:** 9b4faa748d21742a

SuperSpeed link rate (`usb_speed_t`) — `3`.

One of the values reported by `usb-dev-speed`.
