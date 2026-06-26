# uhci

> UHCI (USB 1.1) host-controller driver: discovers a PIIX3/PIIX4/ICH9 UHCI
> function, resets it, and runs a long-lived host-controller context that
> serves control, bulk, interrupt, and isochronous transfer requests from
> [coreusb](../servers/coreusb.md) while polling both root ports for hotplug.

| | |
|---|---|
| **Source** | `lisp/drivers/uhci.clp` (+ `lisp/drivers/uhci/regs.clp`, `driver.clp`, `xfer.clp`) |
| **Kind** | driver |
| **Bound by** | `lisp/init.clp` — `(uhci-init usb)` always called; gated internally on `pci-find` |
| **Registers with** | [coreusb](../servers/coreusb.md) via `port-connected` / `port-disconnected` messages (not a `register-class` call) |
| **Capabilities** | `sys-io` (port-I/O register access), `sys-mmio` (config-space map + 32-bit DMA), `sys-pci` (device discovery + bus-master enable) |

## Overview

UHCI is a port-I/O controller: its twelve registers live at an I/O BAR
(BAR4) and are read/written with `in-u16`/`out-u16`/`out-u32` instructions
gated by the `sys-io` capability.  Its schedule is entirely DMA memory the
hardware walks autonomously: a 1024-entry frame list (one 32-bit physical
pointer per 1 ms frame), each entry normally pointing at a persistent control
queue head (QH).  Transfers are performed by building a chain of transfer
descriptors (TDs) in a 32-bit DMA scratch page, swinging the QH's element
pointer at the chain, and polling the TD status bytes until the hardware
retires them.

The driver captures its three capability primitives in the `init.clp`
context, then spawns a single restricted `'()` context (`uhci-bringup`) that
closes over them.  That context is the host-controller handle coreusb routes
all transfers to.  Because the context services one mailbox message at a time,
no lock is needed for the shared DMA buffers — the context itself is the
serialization the original C `ctrl_lock` provided.

There is no interrupt path: the PIIX/ICH9 UHCI function exposes no MSI
capability and the kernel lacks ACPI `_PRT` parsing for INTx# GSI routing.
Transfer completion is detected by polling DMA-written TD status bytes;
between polls the context yields via `sleep` so the scheduler can run other
contexts.

## Initialization

`lisp/init.clp` brings up coreusb first, then calls:

```scheme
(uhci-init usb)   ; usb = the coreusb context handle
```

`uhci-init` is the sole exported symbol.  It:

1. Calls `(find-uhci UHCI-IDS)` — iterates a fixed list of five VENDOR/DEVICE
   pairs and returns the ECAM pointer of the first PCI function that matches, or
   `#f`.  Supported IDs:

   | ID | Controller |
   |---|---|
   | `8086:7020` | Intel PIIX3 USB |
   | `8086:7112` | Intel PIIX4 USB |
   | `8086:2934` | Intel ICH9 UHCI #1 |
   | `8086:2935` | Intel ICH9 UHCI #2 |
   | `8086:2936` | Intel ICH9 UHCI #3 |

   Note: discovery is by VENDOR/DEVICE only — no PCI class-code search yet
   (same caveat as the `ahci` driver).

2. Maps the config space with `(mmio-map ecam 4096)` and calls
   `(pci-enable-mem-bus-master! cfg)` to enable bus mastering.

3. Reads BAR4 with `(bar-base cfg 4)` to get the I/O BAR base address
   (`iobar`).

4. Spawns the HC context:
   ```scheme
   (spawn-restricted '() (lambda () (uhci-bringup iobar usb)))
   ```
   Returns the symbol `'uhci-spawned` on success or `#f` if no controller
   was found.

### `uhci-bringup`

Runs inside the spawned HC context.  Allocates all DMA buffers, initialises
the frame list, resets the controller, then enters `uhci-loop`.

```scheme
(uhci-bringup iobar usb)
```

DMA allocations (all via `dma-alloc-32`, which returns sub-32-bit physical
pages):

| Buffer | Size | Purpose |
|---|---|---|
| `fl` | 4 KiB | 1024-entry frame list (4 bytes per entry) |
| `dma` | 4 KiB | Control QH + TD array + SETUP scratch + IN/OUT data |
| `itd` | 4 KiB | Isochronous TD array (up to 64 iso TDs × 16 bytes) |
| `idata` | 4 KiB (`ISO-DATA-MAX`) | Isochronous payload buffer |
| `ep-toggle` | 2048 bytes (128×16) | Per-(address,endpoint) DATA0/1 toggle tracking |

Every frame-list slot is initialised to point at the idle control QH
(`dma-phys | 2`, Q-bit set).  Then `uhci-reset` is called and the serve
loop begins.

### `uhci-reset`

```scheme
(uhci-reset iobar framelist-phys)
```

Sequence: assert `GRESET` for 10 ms, de-assert, assert `HCRESET` and wait
for it to self-clear (bounded by `wait-until`, 100 ms wall-clock), write
`FRBASEADDR`, set `Run` (`USBCMD-RS`).

## DMA scratch layout (`dma` buffer)

All offsets are relative to the start of the 4 KiB `dma` page.

| Offset | Size | Contents |
|---|---|---|
| `0x000` (`UHCI-QH-OFF`) | 8 bytes | Persistent control QH: `hlp` (head link, Terminate) + `elp` (element link, armed or Terminate) |
| `0x100` (`UHCI-TD-OFF`) | 64 × 16 bytes | TD array — control and data transfers use up to `UHCI-TD-COUNT` (64) TDs |
| `0x600` (`UHCI-SETUP-OFF`) | 8 bytes | Copy of the 8-byte SETUP packet for a control transfer |
| `0x800` (`UHCI-DATA-OFF`) | 2048 bytes (`UHCI-DATA-MAX`) | IN or OUT payload for control/data transfers |

## Register map

All registers are at `iobar + offset`, accessed by port I/O.

| Name | Offset | Width | Notes |
|---|---|---|---|
| `USBCMD` | `0x00` | u16 | `RS`=bit0, `HCRESET`=bit1, `GRESET`=bit2 |
| `USBSTS` | `0x02` | u16 | `USBINT`=bit0 (not used; no interrupt path) |
| `USBINTR` | `0x04` | u16 | Interrupt-enable (written 0; interrupts disabled) |
| `FRNUM` | `0x06` | u16 | Current frame index, bits\[10:0\] |
| `FRBASEADDR` | `0x08` | u32 | Physical address of the 1024-entry frame list |
| `SOFMOD` | `0x0C` | u8 | SOF timing (not written; reset default used) |
| `PORTSC(n)` | `0x10 + n*2` | u16 | Root-port status/control; n ∈ {0, 1} |

Relevant `PORTSC` bits:

| Bit | Constant | Meaning |
|---|---|---|
| 0 | `PORTSC-CURCONNECT` | Device currently connected |
| 1 | `PORTSC-CONNECTCHG` | Connect-change latch (write 1 to ack) |
| 2 | `PORTSC-PORTEN` | Port enable |
| 3 | `PORTSC-PORTENCHG` | Port-enable change latch |
| 8 | `PORTSC-LOWSPEED` | Device is low-speed |
| 9 | `PORTSC-PORTRESET` | Assert port reset |

## Port management

`poll-ports!` runs every `POLL-INTERVAL` (250 ms) inside the HC context loop.
For each of the two root ports it reads PORTSC, acknowledges any
connect-change latch, and:

- **New connect** — calls `enable-port!` (asserts reset for 15 ms, de-asserts
  + 20 ms recovery, enables the port + another 20 ms) then sends coreusb:
  ```scheme
  (send usb (list 'port-connected (self) port speed))
  ; speed = USB-SPEED-LOW (0) or USB-SPEED-FULL (1)
  ```
  `(self)` is the HC context handle coreusb will use to route transfers.

- **New disconnect** — sends coreusb:
  ```scheme
  (send usb (list 'port-disconnected (self) port))
  ```

## HC context loop

```scheme
(uhci-loop iobar b usb port-enum last-poll)
```

`b` is the DMA bundle list `(fl dma dma-phys itd itd-phys idata idata-phys ep-toggle)`.

Priority order each iteration:

1. If the mailbox is non-empty: drain and handle one transfer message
   (`uhci-handle`), then recurse immediately (drains the mailbox before
   yielding).
2. If ≥ 250 ms have elapsed since `last-poll`: call `poll-ports!` and update
   `last-poll`.
3. Otherwise: `(sleep IDLE-NS)` (2 ms nap) and recurse.

## Transfer protocol

The HC context handles the standard [coreusb transfer protocol](../servers/coreusb.md).
Every request carries the sender's context as its last field (`reply`); the
HC replies `(complete n data)` where `n` is the byte count (negative on error)
and `data` is a fresh bytevector for IN transfers or `#f`.

### `:control`

```scheme
(control addr speed mps setup data len reply)
→ (complete n data|#f)
```

Implemented by `uhci-control`.

- `setup` — 8-byte bytevector; direction derived from `setup[0]` bit 7
  (`USB-REQ-DIR-IN`).
- `data` — OUT payload bytevector or `#f`; ignored for IN transfers.
- `len` — `wLength`; clamped to `UHCI-DATA-MAX` (2048).
- `mps` — max-packet size for EP0; defaults to 8 if ≤ 0.

Builds three stages in the TD array:

1. **SETUP** (TD 0): PID=`SETUP` (`0x2D`), 8 bytes, DATA0.
2. **DATA** (TDs 1…n-1): PID=`IN` or `OUT`, max-packet chunks, toggle
   alternating from DATA1; up to `UHCI-TD-COUNT − 2` TDs.
3. **STATUS** (last TD): opposite direction, zero-length, DATA1.

After `link-chain!` wires the TDs depth-first and `qh-arm!` points the
persistent QH at the chain, `poll-until` yields in 100 µs sleeps until the
STATUS TD's Active bit clears or a fatal error bit is set or the 100 ms
deadline expires.  `qh-idle!` detaches the chain.

Actual length is the sum of `actlen` fields from the DATA-stage TDs (each
`actlen` value of `0x7FF` = zero bytes).

On success for IN: returns `(list total data-bytes)` where `data-bytes` is a
fresh copy of `dma[UHCI-DATA-OFF…]`.  On error or timeout: `(list -1 #f)`.

### `:interrupt-in`

```scheme
(interrupt-in addr speed ep maxp len reply)
→ (complete n data)
```

Implemented by `uhci-data` with a 5 ms timeout.  Tracks DATA0/1 toggle in
`ep-toggle`.  An IN poll that times out having received only NAKs returns
`(complete 0 <empty-bytes>)` (no data available, not an error).

### `:bulk`

```scheme
(bulk addr ep maxp data len dir-in? reply)
→ (complete n data|#f)
```

Implemented by `uhci-data` with a 200 ms timeout.  Same toggle-tracking
as interrupt.  `addr` is always full-speed for bulk (`ls=0`).

### `:isoch`

```scheme
(isoch addr speed ep maxp data len dir-in? reply)
→ (complete n data|#f)
```

Implemented by `uhci-isoch`.  Isochronous transfers bypass the QH entirely:
each packet (up to `ISO-TD-COUNT` = 64, total ≤ `ISO-DATA-MAX` = 4096 bytes)
gets its own TD in the `itd` buffer, injected directly into consecutive
frame-list slots beginning two frames ahead of the current `FRNUM`.  Each iso
TD links forward to the persistent control QH (Q-bit set in the link pointer)
so that control/bulk traffic still runs in the same frame after the iso TD.

The driver waits for the last scheduled frame to pass (`n + 8` ms deadline),
then restores all modified frame-list slots to point back at the control QH.
There is no retry or handshake — a dropped packet is simply lost (the iso
contract).

- `data` — OUT payload or `#f` for IN.
- `toggle` is always DATA0 (iso TDs carry no DATA1 toggle).

### `:prepare-downstream`

```scheme
(prepare-downstream parent port speed reply)
→ (complete 0 #f)
```

No-op.  UHCI has no transaction translator (TT) and does not need split-
transaction setup; the reply is issued immediately.

### `:mark-hub`

```scheme
(mark-hub addr nports reply)
→ (complete 0 #f)
```

No-op.  UHCI does not need to know which devices are hubs at the controller
level.

### `:disconnect-dev`

```scheme
(disconnect-dev addr)
```

Fire-and-forget (no reply).  Calls `uhci-clear-toggle`, zeroing all 16
endpoint toggle slots for `addr` in `ep-toggle` (`addr * 16 + ep`, for ep
0..15).

## TD and QH internals

### TD layout (16 bytes, little-endian dwords)

| Dword | Field | Notes |
|---|---|---|
| 0 | Link pointer | `T` bit = terminate; bit 2 = depth-first (TD→TD); bit 1 = Q (points to QH) |
| 1 | Status | Active=bit23, err_count=bits\[28:27\]=3, LS=bit26, ISO=bit25; fatal bits\[22:17\] = STALL/DataBuffer/Babble/CRC/bitstuff |
| 2 | Token | pid\[7:0\], devaddr\[14:8\], endpoint\[18:15\], toggle\[19\], maxlen\[31:21\] (N−1, 0x7FF=zero) |
| 3 | Buffer pointer | Physical address of the data buffer |

`td-fatal?` checks bits\[22:17\] of dword 1 (STALL/DataBuffer/Babble/CRC-timeout/bitstuff),
excluding NAK (bit 3).

### QH layout (8 bytes)

| Dword | Field |
|---|---|
| 0 | Head link pointer (`hlp`) — always Terminate (single QH per controller) |
| 1 | Element link pointer (`elp`) — `qh-arm!` points this at the first TD; `qh-idle!` sets it to Terminate |

### Data-toggle tracking

`ep-toggle` is a `(make-bytes (* 128 16))` byte array indexed by
`(bitwise-and addr 0x7F) * 16 + (bitwise-and endpoint 0xF)`.  Each byte
stores the next expected DATA toggle (0 or 1).  `uhci-data` reads the toggle
before building TDs and writes the next one back after a successful transfer.
`uhci-clear-toggle` zeros all 16 entries for an address on disconnect.

### Isochronous TD status dword

```scheme
(define (td-iso-status-dw)
  (bitwise-or #x00800000 (arithmetic-shift 1 25)))
```

ISO bit (bit 25) is set so the HC never retries the packet; `err_count`
remains 0; `ls` remains 0 (UHCI isochronous is full-speed only).

## Exported functions

Only one function is exported by the `uhci` module:

### `(uhci-init usb)`

Entry point called from `init.clp`.  Discovers the first present UHCI PCI
function, enables bus mastering, reads the I/O BAR, and spawns the HC context.
Returns `'uhci-spawned` on success, `#f` if no UHCI controller is found.

All other definitions (`uhci-reset`, `enable-port!`, `poll-ports!`,
`uhci-loop`, `uhci-bringup`, and the `xfer`/`regs` helpers) are
component-private; they are not exported and are only accessible to the
spawned HC context via closure.

## Notes and gotchas

**No interrupt path.** UHCI PCI functions (PIIX/ICH9) expose no MSI
capability, and the kernel has no ACPI `_PRT` parser for INTx# GSI routing.
All transfer completion detection is by polling DMA-written TD status bytes.
The poll granularity is 100 µs (`sleep 100000`); under heavy load this adds
measurable latency to isochronous streams.

**32-bit DMA only.** All DMA pages are allocated via `dma-alloc-32`, which
returns physically sub-4 GB pages.  UHCI's `FRBASEADDR` register and all TD
buffer pointers are 32-bit — 64-bit physical addresses will corrupt the
schedule silently.

**Transfer serialization.** The HC context serves one transfer request at a
time from its mailbox.  Callers must not assume concurrent execution across two
transfers; coreusb and class drivers should pipeline requests by keeping one
outstanding per device and endpoint.

**Isochronous scheduling window.** `uhci-isoch` schedules starting
`(uhci-frnum + 2) mod 1024`, two frames ahead of the current hardware frame
index.  The two-frame lead is necessary to give the hardware time to observe
the modified frame-list entries; a one-frame lead is occasionally insufficient
under busy system load.

**Timeout constants** (`xfer.clp`):

| Transfer type | Timeout |
|---|---|
| Control | 100 ms (`MS-100`) |
| Interrupt-IN | 5 ms (`MS-5`) — NAK-only timeout returns 0 bytes, not an error |
| Bulk | 200 ms (`MS-200`) |
| Isochronous | `(n + 8)` ms where n = packet count |

**Port-reset timing.** `enable-port!` uses `sleep` for all timing (15 ms
assert, 20 ms de-assert recovery, 20 ms enable-settle), yielding to the
scheduler.  Do not call this from a context already holding `cli()`.

**pci-find by ID only.** The driver tries five hard-coded VENDOR/DEVICE pairs;
there is no class-code scan.  An unusual UHCI function with a non-standard
DEVICE ID will be missed.  Both QEMU's `piix3-usb-uhci` and `ich9-usb-uhci1`
match (IDs `7020` and `2934`).

**Single-controller support.** `find-uhci` returns after the first match.
Systems with multiple UHCI functions (e.g. ICH9's three UHCI PCI functions)
will enumerate devices only on the first function found.
