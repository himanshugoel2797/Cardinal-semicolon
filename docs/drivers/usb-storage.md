# usb-storage

> USB Mass Storage class driver: implements the Bulk-Only Transport (BOT/BBB)
> protocol + a SCSI-2 command set to expose USB flash drives and hard disks as
> block devices registered with [corestorage](../servers/corestorage.md).

| | |
|---|---|
| **Source** | `lisp/drivers/usb-storage.clp` |
| **Kind** | driver |
| **Bound by** | `lisp/init.clp` — registered with [coreusb](../servers/coreusb.md) at start-up; one block-server context per enumerated mass-storage device |
| **Registers with** | [coreusb](../servers/coreusb.md) as the `USB-CLASS-MASS-STORAGE` (`#x08`) class driver; [corestorage](../servers/corestorage.md) via `register-blockdev` once the medium is ready |
| **Capabilities** | `spawn-restricted '()` — the block-server context holds **no** `sys-*` capabilities; all MMIO/DMA authority stays in the host-controller context |

## Overview

`usb-storage` splits into two layers.

**Dispatcher context** (`usb-storage-init`): a long-lived `serve` loop that
receives `probe` and `remove` messages from coreusb and maintains an alist of
`(usb-address . block-server-ctx)` pairs for every live device.

**Block-server context** (`start-block-server`): spawned per device on `probe`.
It owns the SCSI/BBB state machine, the stash FIFO (see [Notes](#notes-gotchas)),
the per-device block size and block count, and answers `read`/`write` messages
forwarded by [corestorage](../servers/corestorage.md).

The driver operates over the USB Bulk-Only Transport (BBB) protocol.  Every SCSI
command is wrapped in a 31-byte Command Block Wrapper (CBW) sent out-of-band
over the bulk-OUT endpoint; the response is a 13-byte Command Status Wrapper
(CSW) read from the bulk-IN endpoint, with an optional data phase in between.
LUN 0 is always used; multi-LUN devices are not supported.

Only the mass-storage **Bulk-Only** subclass (`bInterfaceProtocol == #x50`) is
matched — Control/Bulk/Interrupt (CBI) devices are not handled.

## Initialization

`init.clp` calls `usb-storage-init` before bringing up any host controller, so
that the class driver is registered before the first `port-connected` can arrive:

```scheme
(usb-storage-init usb storage)   ; → dispatcher context handle
```

- `usb` — the [coreusb](../servers/coreusb.md) context handle returned by
  `(start-usb-service)`.
- `storage` — the [corestorage](../servers/corestorage.md) context handle
  returned by `(start-storage-service)`.

Internally this spawns the dispatcher `serve` loop and sends:

```scheme
(send usb (list 'register-class USB-CLASS-MASS-STORAGE dispatcher-ctx))
```

No reply is expected.  The dispatcher context handle is returned to `init.clp`
(it is not usually needed after registration).

### Per-device bring-up (on `probe`)

When coreusb sends `('probe dev)` to the dispatcher, `stor-on-probe` runs:

1. Calls `(usb-find-endpoint dev USB-XFER-BULK #t)` and
   `(usb-find-endpoint dev USB-XFER-BULK #f)` to locate the bulk-IN and
   bulk-OUT endpoints.  If either is absent the device is not claimed and
   `devs` is returned unchanged.
2. Spawns `start-block-server` with the bulk endpoint addresses and max-packet
   sizes (defaulting to 64 if the descriptor reports 0), and the `storage`
   handle.
3. Appends `(usb-address . block-server-ctx)` to the dispatcher's device list.

The block-server context then executes its bring-up sequence synchronously
before entering its serve loop:

```
INQUIRY → wait-ready (TEST UNIT READY × 10) → READ CAPACITY(10) → MODE SENSE(6) → register-blockdev
```

If `bcount` is zero after READ CAPACITY (no medium or unreadable), the context
enters a park loop waiting for `('stop)` and does not register with corestorage.

### Device removal (on `remove`)

When coreusb sends `('remove addr)` to the dispatcher, `stor-on-remove` walks
the device list, sends `('stop)` to the matching block-server context (which
causes its main loop to exit), logs `[usb-storage] device removed`, and removes
the entry from the list.

## Bulk-Only Transport (BBB) protocol

### Constants

```scheme
CBW-SIG  ; #x43425355  ("USBC") — dTag field of the CBW
CSW-SIG  ; #x53425355  ("USBS") — dSignature field of the CSW
MAX-BLK  ; 4           — maximum blocks per BBB transfer (4 × 512 = 2048 bytes)
```

### `bbb` — one Bulk-Only command transaction

```scheme
(bbb cmd cmdlen data datalen dir-in?)   ; → (cons csw-status result-data)
```

Executes a single BBB transaction:

1. Builds a 31-byte CBW at the current `tag` (auto-incremented), setting
   `dDataTransferLength`, the direction flag, LUN 0, and copying `cmd`.
2. Sends the CBW on the bulk-OUT endpoint.
3. If `datalen > 0`, performs the data-phase bulk transfer on the appropriate
   endpoint.
4. Reads the 13-byte CSW on the bulk-IN endpoint via `read-csw`.

Return value is `(cons csw-status result-data)`:

- `csw-status` — `0` (good), `1` (SCSI command failed, sense available), or
  `-1` (transport failure: CBW not fully sent, data stalled and not recovered,
  or CSW unreadable after one retry).
- `result-data` — bytevector containing the IN data, `#f` for OUT/no-data
  transfers, or `#f` on error.

### `read-csw` — CSW retrieval with BOT stall recovery

```scheme
(read-csw mytag)   ; → bCSWStatus byte, or #f if unreadable
```

Attempts to read the 13-byte CSW.  If the bulk-IN endpoint STALLs, clears the
halt via `CLEAR_FEATURE(ENDPOINT_HALT)` and retries the CSW read once.  If the
retry also fails or produces an invalid signature/tag, returns `#f` (transport
failure).

### `clear-halt` — stash-aware `CLEAR_FEATURE`

```scheme
(clear-halt ep)   ; → completion message (discarded by callers)
```

Issues a `CLEAR_FEATURE(ENDPOINT_HALT)` control transfer to endpoint `ep`
through the block-server's own stash-aware `await` loop (not via
`usb-clear-halt` from coreusb).  This is critical: coreusb's `usb-clear-halt`
internally calls `await-complete`, which silently discards any non-`complete`
message from the mailbox.  Using it here would permanently lose a block request
or `stop` message that arrived during halt recovery.

### `cmd-retry` — command with automatic sense + retry

```scheme
(cmd-retry thunk where)   ; → last (cons csw-status data)
```

Calls `thunk` (a zero-argument lambda returning a `bbb` result) up to 3 times.
On a non-zero `csw-status`, issues REQUEST SENSE (logging the sense key,
ASC, and ASCQ) to clear the device's CHECK CONDITION before retrying.  The
name `where` (a string) is included in the log line for diagnosis.  Returns the
last result once all tries are exhausted or a zero-status result is obtained.

### `wait-ready` — medium spin-up polling

```scheme
(wait-ready tries)   ; → #t when ready, #f if never ready
```

Polls TEST UNIT READY up to `tries` times (called with 10 at bring-up).  On
each failure issues REQUEST SENSE (which also clears the NOT READY condition)
and sleeps 100 ms before retrying.  Removable media and slow spinning disks
commonly report NOT READY during spin-up.

## SCSI command set

The driver issues SCSI-2 commands over BBB.  All CDBs target LUN 0.
All fields wider than one byte are big-endian.

### INQUIRY (#x12)

```scheme
; 6-byte CDB: [#x12 0 0 0 36 0]
; Data IN: 36 bytes (standard inquiry data)
```

Sent once at bring-up.  The 36-byte response is logged but not parsed beyond
confirming status 0.  Used to wake the device and confirm it responds.

### TEST UNIT READY (#x00)

```scheme
; 6-byte CDB: [#x00 0 0 0 0 0]
; No data phase
; Return: CSW status (0 = ready)
```

Sent by `scsi-tur`; result is the raw `csw-status` (not the `bbb` cons).
Called in a poll loop by `wait-ready` and available for ad-hoc checks.

### REQUEST SENSE (#x03)

```scheme
; 6-byte CDB: [#x03 0 0 0 18 0]
; Data IN: 18 bytes (fixed-format sense data)
; Returns: sense bytevector or #f on failure
```

`scsi-request-sense` returns the raw 18-byte sense blob or `#f`.
`log-sense` wraps it: extracts sense key (byte 2 bits 3:0), ASC (byte 12),
ASCQ (byte 13), logs them to COM1, and returns the sense key (or -1 on parse
failure).  Calling `log-sense` also clears the device's pending CHECK CONDITION,
making it a prerequisite for retrying a failed command.

### READ CAPACITY(10) (#x25)

```scheme
; 10-byte CDB: [#x25 0 0 0 0 0 0 0 0 0]
; Data IN: 8 bytes ([be32 last-lba] [be32 block-length])
```

Sent once at bring-up via `cmd-retry`.  Sets the block-server's `bcount`
(= last-LBA + 1) and `bsize` (block length in bytes, defaulting to 512 if the
device reports 0).  If status is non-zero or data is absent, `bcount` is set to
0 and the device is not registered with corestorage.

### MODE SENSE(6) (#x1A)

```scheme
; 6-byte CDB: [#x1A 0 #x3F 0 192 0]  (all pages, 192-byte allocation)
; Data IN: up to 192 bytes (mode parameter header + page data)
```

Sent once at bring-up via `scsi-mode-sense6`.  Checks bit 7 of byte 2 of the
mode parameter header (the device-specific parameter / WP flag).  Returns `#t`
if write-protected, `#f` if not, or `#f` if the command failed or the response
was too short.  The result is logged but not enforced — the driver does not
gate WRITE(10) on this flag.

### READ(10) (#x28)

```scheme
; 10-byte CDB: [#x28 0 lba[3] lba[2] lba[1] lba[0] 0 count[1] count[0] 0]
; Data IN: count × bsize bytes
```

Issued by `scsi-read10` and called in `MAX-BLK`-block chunks from `do-read`.
`do-read` assembles multi-chunk reads into a single output buffer.

### WRITE(10) (#x2A)

```scheme
; 10-byte CDB: [#x2A 0 lba[3] lba[2] lba[1] lba[0] 0 count[1] count[0] 0]
; Data OUT: count × bsize bytes
```

Issued by `scsi-write10` and called in `MAX-BLK`-block chunks from `do-write`.
Each chunk copies `chunk × bsize` bytes out of the caller-supplied data buffer.

## Block-server message protocol

The block-server context receives messages forwarded by
[corestorage](../servers/corestorage.md) after `register-blockdev`.  All
messages are plain lists so `(car req)` is always valid.

### `read`

- **Request:** `('read lba count reply)`
  - `lba` — first logical block address (0-based).
  - `count` — number of blocks to read.
  - `reply` — context handle that will receive the `complete` response.
- **Reply (sent to `reply`):** `('complete 0 bytes)` on success; `('complete -1 #f)` on error.
- **Implementation:** `do-read` allocates a `count × bsize` output buffer and
  issues chunked READ(10) commands (up to `MAX-BLK` = 4 blocks each) via
  `cmd-retry`.  The first chunk failure aborts and sends `('complete -1 #f)`.

### `write`

- **Request:** `('write lba count data reply)`
  - `lba` — first logical block address.
  - `count` — number of blocks to write.
  - `data` — bytevector of `count × bsize` bytes.
  - `reply` — context handle that will receive the `complete` response.
- **Reply (sent to `reply`):** `('complete 0)` on success; `('complete -1)` on error.
- **Implementation:** `do-write` issues chunked WRITE(10) commands (up to 4
  blocks each) via `cmd-retry`, copying the appropriate slice of `data` into
  each chunk.  The first chunk failure aborts and sends `('complete -1)`.

### `stop`

- **Request:** `('stop)`
- **Reply:** none.
- **Effect:** Causes the main loop to exit (returns `'stopped`), ending the
  block-server context cleanly.  Sent by the dispatcher's `stor-on-remove` on
  USB device disconnect.  The message is always a list, so `(car req)` is safe.

## Exported functions

### `(usb-storage-init usb storage)`

The sole exported function.  Spawns the dispatcher context, registers it with
[coreusb](../servers/coreusb.md) as the `USB-CLASS-MASS-STORAGE` handler, and
returns the dispatcher context handle.

- `usb` — coreusb context handle.
- `storage` — corestorage context handle.
- Returns: the dispatcher context handle (a `serve` loop answering `probe` and
  `remove`).

All other internal functions (`start-block-server`, `stor-on-probe`,
`stor-on-remove`, `bbb`, `read-csw`, `cmd-retry`, `do-read`, `do-write`, etc.)
are private to the module and not exported.

## Notes / gotchas

**The stash — mandatory serialization for the block server.** The block-server
context multiplexes two independent message streams on its own mailbox: block
requests forwarded by corestorage and `('complete n data)` transfer completions
sent back by the host controller.  A BBB transaction issues up to three
sequential bulk transfers, each blocking in `await` until the controller replies.
If a `read` or `write` block request arrives from corestorage while `await` is
waiting for a transfer completion, a naive `recv` in `await` would consume and
lose it.

The fix: `await` stashes any non-`complete` message (i.e. any block request or
`stop`) into a FIFO (`stash`).  The main serve loop drains the stash before
calling `recv` for a fresh message.  Because corestorage serializes by
forwarding one request at a time, the stash rarely holds more than one entry.
The same pattern was used in the [cardfs](../servers/cardfs.md) port.

**`clear-halt` uses its own `await`, not `usb-clear-halt`.** coreusb's
`usb-clear-halt` internally calls `await-complete`, which silently drops any
non-`complete` message it sees.  If a block request arrived in the mailbox
between a STALL and the CLEAR_FEATURE, `usb-clear-halt` would permanently lose
it.  The block server therefore implements its own `clear-halt` that routes
through the stash-aware `await`, preserving all non-completion messages for the
main loop to drain.  See the coreusb docs for the general caveat.

**Only one device registered as `'usb0`.** The current implementation always
registers the block device under the symbol `'usb0` regardless of how many
mass-storage devices are present.  If two USB mass-storage devices are plugged
in simultaneously, both will try to register as `'usb0` with corestorage;
`find-dev` in corestorage returns the first match and the second device's
registration is silently shadowed.  Multi-device support (e.g. `'usb0`,
`'usb1`, …) is not implemented.

**Only LUN 0 is used.** CBW byte 13 is always 0.  Multi-LUN devices (e.g. card
readers with several slots) expose only their first logical unit.

**Write-protect flag is advisory only.** `scsi-mode-sense6` detects and logs
the write-protect status but the driver does not enforce it: WRITE(10) is
issued unconditionally.  The device itself will reject writes and return a
CHECK CONDITION if the medium is truly write-protected.

**MODE SENSE is fire-and-forget at bring-up.** The write-protect result is
logged but is not plumbed through to corestorage or callers.  There is no
mechanism for a filesystem provider to query the write-protect state of a USB
device after it has been registered.

**No medium-change / attention handling.** If the medium is changed (e.g. a
card removed and reinserted in a reader) after initial bring-up, the driver will
encounter a UNIT ATTENTION CHECK CONDITION on the next I/O.  `cmd-retry` will
sense and retry up to 3 times, but there is no higher-level re-enumeration
path: the block-server context was already registered with corestorage using the
old capacity and continues to serve that capacity until the USB device is
physically disconnected.

**Chunked I/O cap is `MAX-BLK` = 4 blocks.** Each BBB data phase is limited to
4 × 512 = 2048 bytes.  Callers may request arbitrarily many blocks in a single
`read` or `write` message; `do-read` / `do-write` loop internally, so the
limit is transparent to corestorage but increases round-trips for large
transfers.  The 2 KB cap exists to stay well within the data-buffer sizing
assumptions in the current implementation.

**No timeout on BBB transfers.** `await` blocks indefinitely for a
`('complete …)` reply from the host controller.  A device that stalls
permanently will wedge the block-server context.  Host controllers (UHCI, xHCI,
EHCI) are expected to time out their own transfers and return a negative `n` in
the completion.

**Bring-up sequence is synchronous in the spawned context.** The INQUIRY, TEST
UNIT READY polling, READ CAPACITY, and MODE SENSE all run in the block-server
context before it sends `register-blockdev`.  coreusb delivers `probe` before
any host controller is fully initialized (not quite — initialization is
sequential in `init.clp`), but the block-server bring-up can take up to ~1 s
for slow media due to `wait-ready`'s 10 × 100 ms sleep.  This delay is confined
to the spawned context and does not block the dispatcher or coreusb.
