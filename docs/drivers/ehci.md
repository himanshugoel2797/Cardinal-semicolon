# ehci

> EHCI (USB 2.0 high-speed) host-controller driver; enumerates HS root ports
> and serves control/bulk/interrupt transfers to [coreusb](../servers/coreusb.md)
> over a single reusable async-schedule Queue Head with polled qTD completion.

| | |
|---|---|
| **Source** | `lisp/drivers/ehci.clp` (+ `lisp/drivers/ehci/regs.clp`, `lisp/drivers/ehci/driver.clp`) |
| **Kind** | driver |
| **Bound by** | `lisp/init.clp` — unconditional call to `(ehci-init usb)` after uhci/xhci |
| **Registers with** | [coreusb](../servers/coreusb.md) via `port-connected` / `port-disconnected` notifications |
| **Capabilities** | `sys-mmio` (MMIO map + 32-bit DMA alloc), `sys-pci` (device find + bus-master + BAR assign), `driver-util` (`wait-until`/USB constants), `coreusb` (transfer-protocol constants) |

## Overview

The `ehci` driver implements a USB 2.0 host controller using EHCI's async
schedule in a minimal single-QH model, mirroring how the `uhci` driver works.
One Queue Head (QH) serves as the async reclamation head; each transfer rewrites
the QH's endpoint descriptor fields and arms it by writing the overlay Next-qTD
pointer last — the atomic arm that lets the always-running async schedule never
execute a half-updated QH.

All three supported transfer types — control (EP0 3-stage), bulk, and
interrupt-in — ride the async schedule.  Interrupt-in is serviced as a one-shot
async IN with a short NAK budget (5 ms), which is sufficient for HID polling
cadence without the periodic schedule hardware.  **Completion is polled from the
qTD Active bit in DMA; no MSI is used.**

EHCI root ports carry **high-speed (480 Mbit/s) devices only**.  Full- and
low-speed devices detected on a root port are released to a companion controller
(UHCI/OHCI) by setting the PORTSC `PortOwner` bit; they will not be enumerated
by this driver (see [Gotchas](#notes-gotchas)).

The driver spawns a long-lived `spawn-restricted '()` context (closing over the
captured `sys-mmio`/`sys-pci` primitives) that both serves transfer messages
from coreusb and polls root ports at a 250 ms cadence.

## Initialization

`init.clp` calls `ehci-init` once after `uhci-init` and `xhci-init`:

```scheme
(ehci-init usb)   ; usb — the coreusb context handle
```

`ehci-init` performs the following steps:

1. **PCI discovery** — probes the four known EHCI PCI IDs in order
   (`8086:24cd`, `8086:293a`, `8086:293c`, `1033:00e0`).  Returns `#f` and
   logs `[ehci] no controller present` if none is found.
2. **Enable** — calls `pci-enable-mem-bus-master!` on the config-space ECAM.
3. **BAR resolution** — reads BAR 0; if it is zero, calls `pci-assign-bars` to
   let the kernel assign it, then re-reads.  Returns `#f` and logs
   `[ehci] no BAR` if the BAR is still absent.
4. **Spawn** — calls `(spawn-restricted '() (lambda () (ehci-bringup bar ecam usb)))`.

`ehci-bringup` (internal, runs in the spawned context) performs the hardware
bring-up:

```scheme
(ehci-bringup bar ecam usb)
; bar  — physical base address of the MMIO region (BAR 0)
; ecam — MMIO-mapped PCI config space (4 KiB)
; usb  — coreusb context handle
```

1. Maps `bar` as a 4 KiB MMIO region (`mmio-map`).
2. Reads `CAPLENGTH` to compute the operational-register offset `op`.
3. Reads `N_PORTS` from `HCSPARAMS[3:0]`.
4. Allocates two 32-bit DMA buffers: a 4 KiB **schedule page** (QH at offset
   `0x000`, qTDs at `0x080`, SETUP staging at `0x300`) and an 8 KiB **data
   page** for transfer payloads.  Also allocates a 2 KiB `ep-toggle` byte
   array (128 devices × 16 endpoints).
5. Initialises the QH as the async reclamation head (`qh-init-head!`).
6. Resets and configures the controller (`ehci-reset`): stops → HCRESET → sets
   `CTRLDSSEG=0` (32-bit addressing), `ASYNCBASE=qh-phys`, `USBINTR=0`,
   `USBCMD = RS | ASE | ITC-1`, `CONFIGFLAG=1`, then waits for `ASS=1`.
7. Powers all root ports (writes `PORTSC_PP` for each).
8. Sleeps 50 ms, then enters `ehci-loop`.

## Message protocol

The spawned EHCI context is itself a **host-controller context** as defined by
coreusb's transfer protocol (`lisp/servers/coreusb/proto.clp`).  coreusb
dispatches transfer requests by sending messages to this context; the context
always replies with `(complete <n> <data-or-#f>)`.

### `:control`

```scheme
; Request
(control addr speed mps setup data len reply)
; addr   — USB device address (0–127)
; speed  — USB-SPEED-LOW=0 / USB-SPEED-FULL=1 / USB-SPEED-HIGH=2
; mps    — control EP0 max-packet size (clamped to 8 if ≤ 0)
; setup  — 8-byte bytevector (the SETUP packet)
; data   — OUT payload bytevector (ignored for IN; may be #f)
; len    — wLength; clamped to [0, 4096]
; reply  — context handle to receive the completion

; Reply (sent to reply)
(complete n data)
; n    — bytes transferred in the DATA stage (0 for OUT or no-data); -1 on error
; data — bytevector (IN transfers) or #f (OUT / no-data / error)
```

Builds a 3-stage qTD chain: SETUP (DATA0) → DATA (DATA1, if `len>0`) → STATUS
(DATA1, IOC).  The direction of DATA and STATUS are derived from `setup[0]`
bit 7 (`USB-REQ-DIR-IN`).  Times out after 200 ms.

### `:interrupt-in`

```scheme
; Request
(interrupt-in addr speed ep maxp len reply)
; addr  — USB device address
; speed — USB speed
; ep    — endpoint number (direction bit ignored; always IN)
; maxp  — max-packet size
; len   — max bytes to read (clamped to 4096)
; reply — completion recipient

; Reply
(complete n data)
; n    — bytes received (0 = endpoint NAK'd within budget, not an error); -1 on error
; data — bytevector of `n` bytes, or empty bytes on timeout, or #f on error
```

Issued as a **single-qTD async IN** with a 5 ms NAK budget.  A timeout (the
endpoint only NAK'd) returns `(complete 0 <empty-bytes>)`, not an error.

**Note:** the periodic schedule is not used.  For low-rate HID polling (10 ms
or slower) the 5 ms NAK window is sufficient.  For audio/video isochronous
endpoints, use `:isoch` (unsupported; see below).

### `:bulk`

```scheme
; Request
(bulk addr ep maxp data len dir-in? reply)
; addr   — USB device address
; ep     — endpoint number (direction bit included in the endpoint field)
; maxp   — max-packet size
; data   — OUT payload bytevector (or #f for IN)
; len    — byte count (clamped to 4096)
; dir-in? — #t = IN, #f = OUT
; reply  — completion recipient

; Reply
(complete n data)
; n    — bytes transferred; -1 on error/timeout
; data — bytevector (IN) or #f (OUT/error)
```

Single qTD, 200 ms timeout.  Bulk is **high-speed only** on EHCI — the speed
field is hard-coded to `USB-SPEED-HIGH` regardless of what is passed.  The
data toggle is maintained in the `ep-toggle` array and advanced by the number
of packets actually transferred (derived from `qtd-remaining`).

### `:isoch`

```scheme
; Request  (not supported)
(isoch addr speed ep maxp data len dir-in? reply)

; Reply
(complete -1 #f)
```

Isochronous transfers require the periodic schedule (iTD/siTD), which is not
implemented.  The handler immediately replies with an error.

### `:prepare-downstream`

```scheme
; Request
(prepare-downstream parent-addr port speed reply)

; Reply
(complete 0 #f)
```

Split-transaction TT configuration.  EHCI can issue split transactions but this
driver does not implement them.  Returns no-op success so coreusb's hub driver
can still call it without checking for an EHCI-specific capability.

### `:mark-hub`

```scheme
; Request
(mark-hub addr nports reply)

; Reply
(complete 0 #f)
```

No-op success.  xHCI uses this to program a root-port context; EHCI ignores it.

### `:disconnect-dev`

```scheme
; Request (fire-and-forget, no reply)
(disconnect-dev addr)
; addr — USB device address being torn down
```

Clears the 16-entry toggle row for `addr` in the `ep-toggle` array, so a
newly re-addressed device at the same slot starts with DATA0.

## Port events sent to coreusb

The HC context sends unsolicited messages to the `usb` (coreusb) handle when
root-port state changes.  coreusb listens for these in its enumerator loop.

```scheme
; Device attached at root port i (high-speed only)
(port-connected <hci-ctx> i USB-SPEED-HIGH)
; hci-ctx — (self), the EHCI context handle coreusb should use for transfers
; i       — root port index (0-based)

; Device detached from root port i
(port-disconnected <hci-ctx> i)
```

Full- and low-speed devices detected at a root port are **not** reported to
coreusb; instead the driver sets the port's `PortOwner` bit to release the
port to a companion UHCI/OHCI controller.  If no companion is present, those
devices are silently inaccessible.

## Exported functions

The module exports exactly one public function:

### `(ehci-init usb)`

```scheme
(ehci-init usb)   ; usb — coreusb context handle
                  ; → 'ehci-spawned | #f
```

Entry point called by `init.clp`.  Discovers the EHCI controller, enables bus
mastering, resolves BAR 0, and spawns the HC context.  Returns the symbol
`'ehci-spawned` on success, or `#f` if no controller was found or BAR
resolution failed.  All remaining functions (`ehci-bringup`, `ehci-reset`,
`ehci-loop`, `ehci-handle`, `ehci-poll-ports!`, `ehci-control`, `ehci-data`,
`ehci-clear-toggle`, `ehci-enable-port!`, plus all `regs.clp` helpers) are
module-internal.

## Register map summary

All registers are 32-bit MMIO (`bytes-u32-ref` / `bytes-u32-set!`).

| Constant | Offset | Description |
|----------|--------|-------------|
| `ECAP-CAPLENGTH` | `base+0x00` (u8) | Capability-register length → operational base offset |
| `ECAP-HCSPARAMS` | `base+0x04` | Structural parameters; `[3:0]` = N\_PORTS |
| `ECAP-HCCPARAMS` | `base+0x08` | Capability parameters (64-bit addr flag, ext-caps ptr) |
| `EOP-USBCMD`     | `op+0x00` | Run/Stop, HCRESET, ASE, ITC |
| `EOP-USBSTS`     | `op+0x04` | HCHALTED, ASS |
| `EOP-USBINTR`    | `op+0x08` | Interrupt enable (set to 0 — polled mode) |
| `EOP-FRINDEX`    | `op+0x0C` | Microframe index (not used) |
| `EOP-CTRLDSSEG`  | `op+0x10` | 64-bit segment (set to 0 for 32-bit) |
| `EOP-PERIODICBASE` | `op+0x14` | Periodic frame-list base (not programmed) |
| `EOP-ASYNCBASE`  | `op+0x18` | Async list base (set to QH physical address) |
| `EOP-CONFIGFLAG` | `op+0x40` | Route all ports to EHCI (set to 1) |
| `(EOP-PORTSC n)` | `op+0x44+n*4` | Per-port status/control |

## DMA schedule layout

One 4 KiB `dma-alloc-32` page, physical address recorded as `sched-phys`:

| Offset | Content |
|--------|---------|
| `0x000` | Queue Head (48 bytes, 32-byte aligned) |
| `0x080` | qTD 0 (32 bytes) — SETUP or sole DATA qTD |
| `0x0A0` | qTD 1 (32 bytes) — DATA stage (control) |
| `0x0C0` | qTD 2 (32 bytes) — STATUS stage (control) |
| `0x300` | 8-byte SETUP staging buffer |

Bulk/control data uses a separate 8 KiB `dma-alloc-32` page (`data` /
`data-phys`).  Maximum per-transfer payload is **4096 bytes** (a single qTD
can span one page boundary via its two buffer-pointer slots, but the driver
caps transfers at one page).

## Notes / gotchas

**High-speed only — FS/LS devices are released to companion.**
EHCI root ports enumerate only high-speed devices.  When a full- or low-speed
device is detected (K-state line or `PED=0` after reset), the driver sets
`PortOwner=1`.  With a companion UHCI/OHCI present that device will be bound
there; with a bare `usb-ehci` (e.g. QEMU `usb-ehci` alone) the device is
simply inaccessible.

**No MSI — polled completion.**
The controller's interrupt register (`USBINTR`) is set to 0 at reset.
Completion is determined by polling the qTD's Active bit (`bit 7` of the token
dword) and the QH overlay Halted bit.  The poll loop yields with `(sleep 100000)`
(100 µs) between iterations.

**Periodic schedule not programmed.**
`PERIODICBASE` is never written and the `PSE` bit is never set.  Isochronous
transfers (`isoch`) reply with `(complete -1 #f)`.  Interrupt-in transfers
use a one-shot async IN with a 5 ms NAK budget instead of the periodic schedule.

**Split transactions (TT) not implemented.**
`prepare-downstream` is a no-op.  FS/LS devices behind a HS hub via a
Transaction Translator cannot be reached.

**One outstanding transfer at a time.**
The single-context model (one message dequeued → transfer polled → reply sent,
then next message) serializes all transfers naturally.  Per-transfer QH-field
rewrites (`qh-config!`) are done while the QH is idle (overlay Next = Terminate),
so the always-running async schedule never executes a stale QH.

**PORTSC W1C preserve mask excludes PED.**
The change-bit acknowledge write uses `PORTSC-PRESERVE = PP | PO` only.  `PED`
(Port Enable/Disable) is intentionally excluded: it is write-1-to-disable, not
a value bit.  Writing back a read `PED=1` would disable the just-enumerated
port.  See `notes/AUDIT.md` ("A port-ack bug found in review and fixed before
merge").

**Toggle tracking.**
The `ep-toggle` byte array (128 × 16 bytes, indexed `addr*16 + ep`) shadows
the DATA0/1 toggle for each (device, endpoint) pair.  The driver advances the
toggle by `ceil(transferred / mps)` packets after each bulk or interrupt-in
transfer.  `disconnect-dev` zeros the 16-entry row for a departing device.
Class drivers that issue `CLEAR_FEATURE(ENDPOINT_HALT)` (stall recovery) must
ensure coreusb resets the corresponding toggle entry via `disconnect-dev` +
re-enumeration, or explicitly via a future dedicated message.

**Validated hardware (QEMU):**
`usb-ehci` (PCI `8086:24cd`), `ich9-usb-ehci1` (`8086:293a`),
`ich9-usb-ehci2` (`8086:293c`).  Tested with a high-speed `usb-storage`
device: enumerates, INQUIRY / READ CAPACITY / CBW / CSW, registers `usb0`.
See `notes/AUDIT.md` § "EHCI host controller (added)" for the full validation
report.
