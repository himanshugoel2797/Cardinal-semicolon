# xhci

> An xHCI (USB 3) host-controller driver that discovers a supported PCIe xHCI
> controller, brings it up with a full command/event/transfer-ring model, and
> serves USB transfer messages on behalf of [coreusb](../servers/coreusb.md).

| | |
|---|---|
| **Source** | `lisp/drivers/xhci.clp` (+ `lisp/drivers/xhci/regs.clp`, `lisp/drivers/xhci/driver.clp`) |
| **Kind** | driver |
| **Bound by** | `lisp/init.clp` — gated on `(pci-find ...)` across `XHCI-IDS` (see Supported hardware) |
| **Registers with** | [coreusb](../servers/coreusb.md) — sends `port-connected` / `port-disconnected` events; the xHCI context itself IS the host-controller handle coreusb hands to class drivers |
| **Capabilities** | `sys-mmio` (`mmio-map`, `dma-alloc-32`, `bytes-phys`), `sys-pci` (`pci-find`, `pci-enable-mem-bus-master!`, `pci-assign-bars`, `pci-setup-msi`, `bar-base`), `driver-util`, `coreusb` |

## Overview

`xhci` is a SPLIT module compiled from two component-private include files
(`regs` and `driver`) under `lisp/drivers/xhci/`. It owns the full xHCI
hardware model:

- **Capability / operational / runtime / doorbell register windows** — decoded
  from the MMIO BAR and the capability register chain. Context size (32 or 64
  bytes per entry) is detected from `HCCPARAMS1` bit 2 and applied to every
  device-context and input-context field offset.
- **Command ring + event ring** — a 64-TRB producer ring on the command side
  and a 64-TRB consumer ring backed by a single ERST segment on the event
  side. An MMIO write to the doorbell register at slot 0 kicks a command;
  completions appear in the event ring.
- **Device Context Base Address Array (DCBAA)** — a 4096-byte DMA page; each
  slot entry (8 bytes at `slot * 8`) holds the physical address of that
  device's context buffer.
- **Per-slot / per-endpoint transfer rings** — created lazily on first use
  (one `ring-make` per endpoint). Each ring is 64 TRBs with a permanent
  Link TRB in the last slot carrying a Toggle Cycle flag.
- **Address→slot map** — a 256-byte mutable array keyed on USB bus address
  (0–255), mapping to the xHCI slot ID obtained from Enable Slot.
- **Bounce buffers** — a 4096-byte DMA page for control/bulk/interrupt data
  and a separate 4096-byte page for isochronous data; maximum transfer per
  call is `XHCI-BOUNCE-MAX = 2048` bytes.

All mutable ring cursors, event-ring state, the slot alist, the per-endpoint
ring alist, and the address→slot map live as `set!`-able bindings captured by
the long-lived bring-up thunk. The HC runs as a single context, so mutation is
race-free.

Endpoint rings and input context DMA buffers are allocated and never freed
(no `dma-free` primitive exists). This is a bounded, enumeration-scoped leak,
consistent with the same policy in `ahci`.

## Supported hardware

`xhci-init` scans `XHCI-IDS` in order, returning the first matching ECAM
pointer:

| Vendor:Device | Description |
|---|---|
| `1b36:000d` | QEMU xHCI (primary test target) |
| `1033:0194` | NEC/Renesas USB xHCI |
| `8086:9d2f` | Intel Sunrise Point-LP PCH xHCI |
| `8086:a12f` | Intel Sunrise Point-H PCH xHCI |
| `8086:31a8` | Intel Gemini Lake PCH xHCI |

## Initialization

`init.clp` calls `xhci-init` after all class drivers have been registered with
coreusb:

```scheme
(xhci-init usb)   ; → 'xhci-spawned on success, #f if no controller found
```

`usb` is the coreusb context handle.  `xhci-init` returns before bring-up
completes — it spawns `xhci-bringup` in a restricted `'()` capability context
and returns immediately.

### Bring-up sequence (`xhci-bringup`)

`xhci-bringup` runs entirely inside the spawned context and owns all
hardware state.

1. **Map MMIO** — `mmio-map bar 65536` gives the full register window.
2. **Read capability registers** — `CAPLENGTH`, `HCSPARAMS1` (max-slots,
   max-ports), `HCCPARAMS1` (ctx-size flag), `RTSOFF` (masked to 32-byte
   boundary), `DBOFF` (masked to 4-byte boundary).
3. **Allocate DMA structures** — DCBAA (4096 B), command ring, event ring
   buffer (4096 B), ERST page (4096 B), bounce buffer (4096 B), iso bounce
   buffer (4096 B). Physical addresses are captured via `bytes-phys`.
4. **Reset** — write 0 to `USBCMD` (stop); poll `USBSTS.HCH` up to 100 ms;
   write `HCRST` to `USBCMD`; poll until `HCRST` self-clears AND `CNR` clears
   (up to 1 s).
5. **Configure** — write `max-slots` (capped at 64) to `CONFIG`; write DCBAA
   physical address to `DCBAAP`; write command-ring physical address (with
   producer cycle = 1) to `CRCR`.
6. **Event ring** — write event-ring physical address and size (64) into the
   ERST segment; program `ERSTSZ = 1`, `ERSTBA`, and `ERDP`; all at
   `rt + IR0 + offset`.
7. **MSI** — `pci-setup-msi ecam` installs an MSI handler. The slot is stored
   in `msi` but is not used to await completions (see `xhci-wait` below).
   Interrupt moderation (`IMOD`) is set to 4000; `IMAN.IE` is set to enable
   the interrupter.
8. **Start** — write `RS | INTE` to `USBCMD`.
9. **Power ports** — write `PP` to every root port's `PORTSC` register.
10. **Sleep 100 ms** — allow USB PHY debounce.
11. **Enter the serve/poll loop** — alternates between draining the mailbox
    (transfer messages) and polling ports every 250 ms (`XPOLL-INTERVAL`).

### BAR assignment

BAR0 is expected to be a 64-bit MMIO BAR. If firmware left it at 0 (an
unconfigured device), `pci-assign-bars ecam` is called and BAR0 is re-read:

```scheme
(let ((bar (let ((b (bar-base cfg 0)))
             (if (= b 0) (begin (pci-assign-bars ecam) (bar-base cfg 0)) b))))
  ...)
```

## Register layout

All accesses go through `rd32`/`wr32`/`rd64`/`wr64` helpers indexing absolute
byte offsets in the single 65536-byte MMIO window.

| Symbolic name | Offset from base | Description |
|---|---|---|
| `XHCI-CAP-CAPLENGTH` | `0x00` (u8) | Capability registers length; `op = base + CAPLENGTH` |
| `XHCI-CAP-HCSPARAMS1` | `0x04` | Bits[7:0] max-slots, bits[31:24] max-ports |
| `XHCI-CAP-HCCPARAMS1` | `0x10` | Bit 2: context-size (1=64B, 0=32B) |
| `XHCI-CAP-DBOFF` | `0x14` | Doorbell array offset from base (low 2 bits RO) |
| `XHCI-CAP-RTSOFF` | `0x18` | Runtime register set offset (low 5 bits RO) |
| `XHCI-OP-USBCMD` | `op + 0x00` | Run/Stop (`RS`=1), Host Reset (`HCRST`=2), `INTE`=4 |
| `XHCI-OP-USBSTS` | `op + 0x04` | `HCH`=1 (halted), `CNR`=0x800 (controller not ready) |
| `XHCI-OP-CRCR` | `op + 0x18` (64-bit) | Command Ring Control — physical address + producer cycle |
| `XHCI-OP-DCBAAP` | `op + 0x30` (64-bit) | DCBAA physical address |
| `XHCI-OP-CONFIG` | `op + 0x38` | Max device slots enabled |
| `XHCI-OP-PORTSC p` | `op + 0x400 + (p-1)*0x10` | Port status/control (1-based port index) |
| `XHCI-RT-IR0` | `rt + 0x20` | Interrupter 0 register set |
| `XHCI-IR-IMAN` | `IR0 + 0x00` | Interrupt Management (`IP`=1, `IE`=2) |
| `XHCI-IR-IMOD` | `IR0 + 0x04` | Interrupt moderation interval |
| `XHCI-IR-ERSTSZ` | `IR0 + 0x08` | Event Ring Segment Table size |
| `XHCI-IR-ERSTBA` | `IR0 + 0x10` (64-bit) | ERST base physical address |
| `XHCI-IR-ERDP` | `IR0 + 0x18` (64-bit) | Event Ring Dequeue Pointer (bit 3 = EHB) |

`PORTSC` bit masks used: `CCS` (0x1), `PED` (0x2), `PR` (0x10), `PP` (0x200),
`CSC` (0x20000), `PRC` (0x200000). Speed bits at `[13:10]`: 1=full, 2=low,
3=high, 4=super.

## TRB ring model

A *producer ring* is a 4096-byte DMA page holding 64 16-byte TRBs. Slot 63 is
permanently occupied by a Link TRB pointing back to slot 0 with Toggle Cycle
set; the effective ring depth is 63 usable slots.

```scheme
(ring-make)     ; → (buf phys ctrl)  — allocates and initialises a fresh ring
(ring-buf  r)   ; → DMA buffer (bytes)
(ring-phys r)   ; → physical address (integer)
(ring-enq  r)   ; → current enqueue index (0–62)
(ring-cyc  r)   ; → current producer cycle bit (0 or 1)
```

```scheme
(ring-push r param status control)
; → physical address of the TRB just written
;
; Stamps the producer cycle into the TRB's control word,
; writes the Link TRB with the new cycle, and wraps enqueue
; back to 0 (toggling the cycle bit) when the ring is full.
; `param` is a 64-bit parameter (address or setup packet).
; `status` and `control` are the remaining two 32-bit TRB words
; (caller OR-in type bits via `trb-set-type` and other flags).
```

### TRB type constants

| Constant | Value | Use |
|---|---|---|
| `TRB-NORMAL` | 1 | Bulk / interrupt data TRB |
| `TRB-SETUP` | 2 | Control setup stage TRB |
| `TRB-DATA` | 3 | Control data stage TRB |
| `TRB-STATUS` | 4 | Control status stage TRB |
| `TRB-ISOCH` | 5 | Isochronous TRB |
| `TRB-LINK` | 6 | Ring link (wrap-around) |
| `TRB-ENABLE-SLOT` | 9 | Command: allocate a device slot |
| `TRB-DISABLE-SLOT` | 10 | Command: release a device slot |
| `TRB-ADDRESS-DEVICE` | 11 | Command: address a device and configure EP0 |
| `TRB-CONFIGURE-ENDPOINT` | 12 | Command: configure a non-control endpoint |
| `TRB-EVENT-TRANSFER` | 32 | Event: transfer completion |
| `TRB-EVENT-CMD-COMPLETE` | 33 | Event: command completion |
| `TRB-EVENT-PORT-STATUS` | 34 | Event: port status change |

Completion codes of interest: `XHCI-CC-SUCCESS` (1), `XHCI-CC-SHORT-PACKET`
(13). Both are treated as success by the transfer handlers.

### Endpoint context EP-type constants

| Constant | Value |
|---|---|
| `EP-TYPE-ISOCH-OUT` | 1 |
| `EP-TYPE-BULK-OUT` | 2 |
| `EP-TYPE-INTR-OUT` | 3 |
| `EP-TYPE-CONTROL` | 4 |
| `EP-TYPE-ISOCH-IN` | 5 |
| `EP-TYPE-BULK-IN` | 6 |
| `EP-TYPE-INTR-IN` | 7 |

The DCI (Doorbell Context Index) for a non-control endpoint is
`(ep-number * 2) + (if dir-in? 1 0)`. EP0 is always DCI 1.

## Event ring and completion waiting

### `next-event`

```scheme
(next-event)   ; → (param status ctrl) on a pending event, #f if ring is empty
```

Reads the TRB at `ev-idx` and checks the cycle bit against `ev-cyc`. On a
match: advances `ev-idx` (wrapping at `XHCI-RING-SIZE` and toggling `ev-cyc`),
updates `ERDP` (with bit 3 set as the EHB acknowledgment), and returns the
three TRB words. Returns `#f` when no new event is present.

### `xhci-wait`

```scheme
(xhci-wait type slot timeout-ns)
; → (param status ctrl) for the first matching event, or #f on timeout
```

Polls the event ring for an event of TRB type `type` on slot `slot` (`slot < 0`
matches any slot). Between polls it calls `(sleep 100000)` (100 µs). The
deadline is computed once from `(uptime-ns)` at entry; every iteration checks
the deadline before polling.

**Critical gotcha:** `xhci-wait` polls the event ring buffer directly — the
controller DMAs completion events there regardless of MSI delivery. It does NOT
park on `msi-wait`. A missed or delayed MSI interrupt cannot wedge a transfer
because the poll loop will catch the event unconditionally. Intervening events
of a different type (e.g. port-status-change events arriving during a transfer
wait) are consumed and discarded to prevent livelock.

## Slot and address model

### Slot record

```scheme
(mk-slot id dc dcp speed rp route depth)
; id     — xHCI slot ID (1-based, ≤ max-slots)
; dc     — device context DMA buffer (bytes)
; dcp    — device context physical address
; speed  — xHCI speed code (1=full, 2=low, 3=high, 4=super)
; rp     — root port number (1-based)
; route  — 20-bit route string (4 bits per hub depth level)
; depth  — hub depth (0 for root-port devices)
```

Accessor shorthands: `sl-id`, `sl-devctx`, `sl-speed`, `sl-rootport`,
`sl-route`, `sl-depth`.

Slots are stored in the `slots` alist keyed on slot ID; endpoint rings are
stored in `slot-eps` keyed on `(slot-id * 64 + dci)`. Both are accessed via
`assq-num`.

### Port connection flow (root port)

1. `poll-ports!` detects `PORTSC.CCS` set (connect) → calls `port-connected p`.
2. `port-connected` writes `PR` to `PORTSC` (port reset), waits for `PRC`
   (reset complete) using `wait-until` up to 100 ms, then writes `PRC | CSC`
   to clear the status-change bits.
3. If `PED` (port enabled) is set: reads the speed from `PORTSC[13:10]`,
   issues **Enable Slot** command → gets `slot-id`.
4. Allocates `devctx` (DMA), `ring` (EP0 transfer ring), builds a slot record,
   calls `add-slot!` and `set-ep-ring! slot-id 1 ring`.
5. Writes `devctx-phys` to `dcbaa[slot-id * 8]`.
6. Calls `(address-device sl 1)` (BSR=1 — sets up EP0 without sending
   `SET_ADDRESS` on the bus).
7. Records `enum-slot = slot-id` (used when coreusb later issues a `control`
   message to address 0).
8. Sends `('port-connected (self) p (xspeed->uspeed xspeed))` to coreusb.

On disconnect, `poll-ports!` sends `('port-disconnected (self) p)` to coreusb.

### `address-device`

```scheme
(address-device sl bsr)   ; → 0 on success, -1 on command failure
```

Builds a 4096-byte input context DMA buffer:

- Input Control Context at offset 0: `ICC add` bits = `0b11` (slot + EP0).
- Slot context at `ctx-size`: route string, speed, root-port, context entries.
- EP0 context at `ctx-size * 2`: type = `EP-TYPE-CONTROL`, max packet size from
  `speed-mps0` (512 for super, 64 for high, 8 otherwise), TR Dequeue Pointer.

TR Dequeue Pointer encoding (critical): set to the EP0 ring's current enqueue
position plus the producer cycle bit (`ring-phys + enq*16 | cyc`). For a fresh
ring (`enq=0, cyc=1`) this equals `ring-base | 1`. For the BSR=0 second call
(after `GET_DESCRIPTOR` TRBs have been pushed), this points past the consumed
TRBs so the controller does not re-execute them.

`bsr=1` sets bit 9 of the command TRB control word (BSR flag — Block Set
Address Request); the controller sets up EP0 without issuing `SET_ADDRESS`.
`bsr=0` is used when coreusb's enumerator sends the actual `SET_ADDRESS`
request — at that point the control transfer handler intercepts the setup
packet and issues `address-device(sl, 0)` before replying.

### SET_ADDRESS interception

When a `control` message arrives for address 0 and `setup[1] == USB-REQ-SET-ADDRESS`:

```scheme
; Intercept SET_ADDRESS: issue Address Device(BSR=0) instead of forwarding
; to the HC (the device does not actually receive this on-wire; the xHCI
; controller handles addressing internally).
(address-device sl 0)
; Record mapping: addr-to-slot[wValue] = slot-id
(bytes-u8-set! addr-to-slot (bytes-u16-ref setup 2) slot-id)
; Reply success to coreusb without doing a real transfer
(send reply (list 'complete 0 #f))
```

All subsequent transfer requests use `addr-to-slot` to look up the slot.

## Transfer implementation

### Control transfer (`do-control`)

```scheme
(do-control sl setup data data-len)
; → (list n data|#f)
;   n     — byte count transferred, or -1 on error
;   data  — fresh bytes copy of IN data, or #f for OUT / no-data
```

Pushes three TRBs onto EP0's ring:

1. **Setup TRB** (`TRB-SETUP`) — 8-byte setup packet in `param`, IDT bit set
   (inline data), `TRT` (Transfer Type) field: 0 = no-data, 2 = OUT-data,
   3 = IN-data.
2. **Data TRB** (`TRB-DATA`) — only if `data-len > 0`; uses `bounce-phys`; DIR
   bit set for IN transfers. OUT data is copied into `bounce` first.
3. **Status TRB** (`TRB-STATUS`) — IOC bit set; DIR inverted from the data
   stage for non-zero-length transfers.

Doorbell: `db-ring! slot-id 1`. Waits via `(xhci-wait TRB-EVENT-TRANSFER slot-id 400000000)`.

### Interrupt / Bulk transfer (`do-data`)

```scheme
(do-data sl dci ep-type mps data len dir-in? timeout)
; → (list n data|#f)
```

Calls `configure-endpoint` lazily (no-op if the ring already exists). Pushes a
single `TRB-NORMAL` TRB with IOC and ISP flags. Doorbell to `dci`. Waits via
`xhci-wait` with the given timeout.

- **Interrupt IN** timeout: 8 ms (`8000000` ns).
- **Bulk** timeout: 300 ms (`300000000` ns).
- For IN transfers: returns `(list got (copy-bytes bounce 0 got))` where `got =
  dlen - residual` (residual is bits[23:0] of the event's status word).
- For OUT transfers: returns `(list got #f)`.
- On timeout for IN: returns `(list 0 (make-bytes 0))` (empty, not error); on
  timeout for OUT: returns `(list -1 #f)`.

### Isochronous transfer (`do-isoch`)

```scheme
(do-isoch sl dci ep-type mps data len dir-in?)
; → (list n data|#f)
```

Calls `configure-iso-endpoint` lazily. Splits `len` (capped at
`XHCI-BOUNCE-MAX = 2048`) into `ceil(len/mps)` packets (minimum 1). Pushes one
`TRB-ISOCH` TRB per packet onto the endpoint ring, all with the SIA flag (Start
Isoch ASAP — scheduled in successive (micro)frames). IOC is set only on the
last TRB so one transfer event covers the whole submission.

Doorbell to `dci`. Waits via `(xhci-wait TRB-EVENT-TRANSFER slot-id
1000000000)` (1 s timeout).

The iso endpoint context is configured with Interval=3 (1 ms ESIT — the
standard USB audio service interval). `bInterval` from the interface descriptor
is NOT read; 1 ms is assumed for all iso endpoints.

### Endpoint configuration (`configure-endpoint`, `configure-iso-endpoint`)

```scheme
(configure-endpoint sl dci ep-type mps)       ; → 0 on success, -1 on failure
(configure-iso-endpoint sl dci ep-type mps)   ; → 0 on success, -1 on failure
```

Both are idempotent — if an endpoint ring already exists for `(slot-id, dci)`
they return 0 immediately. On first call:

1. `ring-make` allocates the transfer ring.
2. Builds a 4096-byte input context: ICC add bits = `1 | (1 << dci)`, slot
   context copied from the device context with updated Context Entries field,
   EP context at `ctx-size * (1 + dci)` with type, MPS, and TR Dequeue Pointer.
3. Issues Configure Endpoint command, awaits completion.

For iso endpoints: `CErr = 0` (isochronous never retries), Interval = 3,
Max ESIT Payload = `mps` (FS/HS no burst/mult).

## Hub support

### `prepare-downstream`

```scheme
(prepare-downstream parent-addr parent-port speed)   ; → 0 or -1
```

Looks up the parent hub's slot via `addr-to-slot`. Issues Enable Slot to get a
new `slot-id`. Builds the route string by OR-ing the parent's route string with
the downstream port number shifted left by `(parent-depth * 4)` bits (4 bits
per hub level). Creates a slot record with `depth = parent-depth + 1` and the
same root port as the parent. Issues `address-device(sl, 1)` to set up EP0.

### `mark-hub-dev`

```scheme
(mark-hub-dev addr nports)   ; → 0 or -1
```

Sets the Hub bit (bit 26) in the slot context and records `nports` in bits
`[31:24]` of slot-context word 1. Issues Configure Endpoint (slot only — ICC
add = 1) to update the live device context.

### `disconnect-dev`

```scheme
(disconnect-dev addr)   ; fire-and-forget (no return value used by caller)
```

Issues Disable Slot command, clears `dcbaa[slot-id * 8]`, removes the slot
and all its endpoint rings from the alists, clears `addr-to-slot[addr]`.

## Message protocol

The xHCI context serves as the host-controller handle passed to coreusb and,
transitively, to class drivers. Messages follow the standard
[coreusb transfer protocol](../servers/coreusb.md). Only the serve loop
(`handle`) is the entry point — there is no separate "message-to-xhci" API.

All requests carry a `reply` context as their last element. The driver replies
`('complete n data|#f)`.

### `control`

```scheme
(control addr speed mps setup data len reply)   ; → (complete n data|#f)
```

- **`addr`** — USB bus address (0 = the device currently being enumerated,
  looked up via `enum-slot`).
- **`speed`** — USB speed code (from coreusb, not used directly by xHCI for
  transfers but forwarded from `port-connected`).
- **`mps`** — EP0 max packet size.
- **`setup`** — 8-byte setup packet (bytes).
- **`data`** — OUT payload bytes, or `#f` for IN / no-data.
- **`len`** — `wLength` from the setup packet.
- **`reply`** — reply context handle.

Special case: if `setup[0] == 0x00` (host-to-device device-recipient) and
`setup[1] == USB-REQ-SET-ADDRESS` (5), the driver issues Address Device(BSR=0)
and records the address→slot mapping without executing a real transfer.

### `interrupt-in`

```scheme
(interrupt-in addr speed ep maxp len reply)   ; → (complete n data)
```

- **`ep`** — endpoint address (bits[3:0] used as endpoint number; DCI = `ep_num*2 + 1`).
- **`maxp`** — max packet size.
- **`len`** — byte count requested (capped at `XHCI-BOUNCE-MAX`).

### `bulk`

```scheme
(bulk addr ep maxp data len dir-in? reply)   ; → (complete n data|#f)
```

- **`dir-in?`** — `#t` for IN (read), `#f` for OUT (write).
- DCI = `ep_num*2 + (if dir-in? 1 0)`.

### `isoch`

```scheme
(isoch addr speed ep maxp data len dir-in? reply)   ; → (complete n data|#f)
```

Multi-packet isochronous submission. `data` is the source buffer for OUT;
`len` bytes are transferred (capped at `XHCI-BOUNCE-MAX`).

### `prepare-downstream`

```scheme
(prepare-downstream parent-addr port speed reply)   ; → (complete status #f)
```

`status`: 0 on success, -1 on failure.

### `mark-hub`

```scheme
(mark-hub addr nports reply)   ; → (complete status #f)
```

### `disconnect-dev`

```scheme
(disconnect-dev addr)   ; fire-and-forget; no reply sent
```

## Exported functions

The module exports exactly one public symbol:

### `(xhci-init usb)`

```scheme
(xhci-init usb)   ; → 'xhci-spawned | #f
```

- **`usb`** — the coreusb context handle (returned by `start-usb-service`).
- Returns `'xhci-spawned` after successfully spawning the bring-up context, or
  `#f` if no supported xHCI controller is found or BAR resolution fails.
- Logs `[xhci] no controller present` or `[xhci] no BAR` to COM1 on failure.
- On success logs `[xhci] bar=<phys>` and, later from inside the spawned
  context, `[xhci] init complete; slots=N ports=M ctxsize=C msi=<slot>`.

## Port polling

The serve loop polls ports at `XPOLL-INTERVAL = 250 ms`. It maintains a
`seen` byte array (indexed by 1-based port number) tracking the previous
connection state:

```scheme
; On connect (CCS set, was not set):
(port-connected p)                                ; handles reset + slot alloc
(send usb ('port-connected (self) p speed))       ; notify coreusb

; On disconnect (CCS clear, was set):
(send usb ('port-disconnected (self) p))          ; notify coreusb
```

Between polls the context sleeps `XIDLE-NS = 2 ms` per iteration.

## Notes / gotchas

**Event ring, not MSI-wait.** `xhci-wait` polls the event ring buffer directly
at 100 µs intervals. MSI is configured and `IMAN.IE` is enabled, but no code
calls `msi-wait` — the completions are read from DMA memory regardless of
interrupt delivery. This guarantees correctness even when MSI delivery is
missed or delayed (e.g. the `IF=0` hazard noted in the boot memory for earlier
drivers).

**Intervening events consumed on wait.** `xhci-wait` discards port-status-
change events (and any other non-matching event type) that arrive while waiting
for a transfer or command completion. This is correct for the single-context
model but means port-connect events that fire during a transfer are deferred
until the next `poll-ports!` pass (250 ms later).

**Two-phase Address Device (BSR=1 → BSR=0).** The initial `address-device(sl,
1)` is issued at connect time so EP0 is ready for coreusb's `GET_DESCRIPTOR`.
The real USB `SET_ADDRESS` is intercepted: the control handler issues
`address-device(sl, 0)` with a TR Dequeue Pointer that points past the already-
consumed BSR=1 TRBs. Pointing at the ring base would cause the controller to
re-execute stale TRBs.

**Context size is hardware-dependent.** The `ctx-size` variable (32 or 64) is
read from `HCCPARAMS1` bit 2 at startup and applied to every field offset
calculation in input and device contexts. A misidentified context size silently
misaligns all subsequent slot/endpoint setup and will cause Address Device /
Configure Endpoint commands to fail with a bad completion code.

**DMA leak on disconnect.** Slot device-context buffers, endpoint transfer
rings, and input contexts are allocated with `dma-alloc-32` and never freed.
On a long-running system with frequent connect/disconnect cycles this is a
bounded resource leak. No `dma-free` primitive exists in the current substrate.

**Iso endpoint interval is hardcoded.** `configure-iso-endpoint` always sets
Interval=3 (1 ms ESIT). The `bInterval` field of the endpoint descriptor is
not consulted. Devices requiring a different service interval will not work
correctly.

**Max transfer is 2048 bytes.** Both `bounce` and `iso-bounce` are 4096-byte
pages but `XHCI-BOUNCE-MAX = 2048` caps every transfer. Data beyond that limit
is silently truncated.

**`msi-wait` not used.** Unlike AHCI, the xHCI driver does not call `msi-wait`
at all. The sole purpose of `pci-setup-msi` here is to prevent the MSI
capability from generating legacy INTx interrupts; the actual completion
signaling path is the polled event ring.

**Speed translation.** xHCI speed codes (1–4) differ from coreusb's USB speed
constants (`USB-SPEED-LOW` through `USB-SPEED-SUPER`). `xspeed->uspeed`
converts xHCI→coreusb; `xspeed-of` converts coreusb→xHCI for hub downstream
slots.
