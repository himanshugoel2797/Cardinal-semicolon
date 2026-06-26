# coreusb

> USB enumeration and class-driver dispatch service: drives the USB 2.0 chapter-9 bring-up sequence for every attached device and routes enumerated devices to the appropriate class driver (HID, hub, mass storage, audio).

| | |
|---|---|
| **Source** | `lisp/servers/coreusb.clp` (+ `lisp/servers/coreusb/proto.clp`, `lisp/servers/coreusb/enum.clp`) |
| **Kind** | server |
| **Bound by** | `lisp/init.clp` — always (unconditional; host controllers are gated on `pci-find` individually) |
| **Registers with** | n/a — class drivers register *with* coreusb; coreusb does not register with any higher-level service |
| **Capabilities** | none — coreusb holds no `sys-*` hardware capability; all MMIO/DMA authority stays in the controller contexts |

## Overview

coreusb is a pure message-routing registry. It owns three pieces of shared state:

- **Class-driver table** — an alist mapping USB class byte to a class-driver context handle (HID `#x03`, hub `#x09`, mass-storage `#x08`, audio `#x01`).
- **Bus-address pool** — a list of in-use USB bus addresses (1–127); address 0 is the USB default address and is never claimed.
- **Enumerated-device records** — a list of `(address hci port parent class)` tuples used to correlate a later port-disconnect back to the correct device record.

A *host controller* (UHCI, xHCI, EHCI) is a long-lived context that owns its own hardware and DMA buffers and answers transfer-request messages. It never calls into coreusb directly; instead it sends `port-connected` / `port-disconnected` messages to the coreusb handle it received at init time.

When a `port-connected` (or `enumerate-downstream`) message arrives, coreusb allocates a USB bus address and spawns a *transient enumerator context* to drive the USB chapter-9 sequence: 8-byte device descriptor → `SET_ADDRESS` → full device descriptor → config header → full config descriptor → `SET_CONFIGURATION`. The enumerator sends transfer messages to the controller and reports its outcome back to coreusb with `enum-done` or `enum-failed`. On success, the device is handed to the matching class driver as a `probe` message.

coreusb itself never touches MMIO, DMA, or PCI. The controllers it communicates with retain their own `sys-*` authority.

## Initialization

`init.clp` calls `(start-usb-service)` before bringing up any host controller:

```scheme
(start-usb-service)   ; → a coreusb context handle (`usb`)
```

The returned handle (`usb`) is passed to every class driver init and to each host controller init. Class drivers must be registered **before** host controllers are initialized; the `register-class` messages are synchronous and the class table must be populated before the first `port-connected` can arrive.

Initialization order in `lisp/init.clp`:

```scheme
(let ((usb (start-usb-service)))
  (usb-hid-init     usb input)        ; register HID class driver
  (usb-hub-init     usb)              ; register hub class driver
  (usb-storage-init usb storage)      ; register mass-storage class driver
  (usb-audio-init   usb audio-service); register audio class driver
  (uhci-init usb)                     ; bring up UHCI (pci-find gated)
  (xhci-init usb)                     ; bring up xHCI (pci-find gated)
  (ehci-init usb))                    ; bring up EHCI (pci-find gated)
```

## Message protocol

Messages are sent to the coreusb context handle. All messages are fire-and-forget from the sender's perspective (no reply is sent back to the caller); the enumerator's internal outcome is reported via `enum-done` / `enum-failed` messages that the service sends to itself.

### `register-class`

Registers a class driver context for a given USB class byte.

- **Request:** `('register-class class-byte class-ctx)` — `class-byte` is one of `USB-CLASS-HID` (`#x03`), `USB-CLASS-HUB` (`#x09`), `USB-CLASS-MASS-STORAGE` (`#x08`), `USB-CLASS-AUDIO` (`#x01`), etc. `class-ctx` is the class driver's context handle.
- **Reply:** none.
- **Notes:** Must be sent before the corresponding host controller is initialized, or a device of that class that connects during enumeration will be silently discarded (logged as "no class driver for class N").

### `port-connected`

Notifies coreusb that a device has been attached to a root port of a host controller.

- **Request:** `('port-connected hci-ctx port speed)` — `hci-ctx` is the controller context handle; `port` is the 0-based root-port number; `speed` is one of `USB-SPEED-LOW` (0), `USB-SPEED-FULL` (1), `USB-SPEED-HIGH` (2), `USB-SPEED-SUPER` (3).
- **Reply:** none. The enumerator context reports outcome asynchronously to coreusb as `enum-done` or `enum-failed`.
- **Errors:** If the bus-address pool is exhausted (all 127 addresses allocated), the connection is silently dropped with a log message.

### `port-disconnected`

Notifies coreusb that a device on a root port has been removed.

- **Request:** `('port-disconnected hci-ctx port)` — `hci-ctx` is the controller handle; `port` is the root-port number.
- **Reply:** none. coreusb finds all records matching `(hci, parent=0, port)`, notifies each class driver with `('remove addr)`, sends `('disconnect-dev addr)` to the controller, and frees the bus addresses.

### `enumerate-downstream`

Requests enumeration of a device on a downstream hub port. Sent by the hub class driver (via `usb-enumerate-downstream`).

- **Request:** `('enumerate-downstream hci-ctx parent-addr port speed)` — `parent-addr` is the hub's USB bus address; `port` is the hub port number.
- **Reply:** none. Same async outcome as `port-connected`.

### `disconnect-downstream`

Tears down a device on a downstream hub port. Sent by the hub class driver (via `usb-disconnect-downstream`).

- **Request:** `('disconnect-downstream hci-ctx parent-addr port)` — `parent-addr` is the hub's USB bus address.
- **Reply:** none. Same teardown logic as `port-disconnected` but matches on `parent-addr` instead of parent=0.

### `enum-done` (internal)

Sent by the enumerator context to coreusb on successful enumeration.

- **Request:** `('enum-done addr hci-ctx port parent-addr class-byte)`.
- **Reply:** none. Adds a record to the enumerated-device list. The bus address was already reserved in `start-enum`.

### `enum-failed` (internal)

Sent by the enumerator context to coreusb when enumeration fails at any stage.

- **Request:** `('enum-failed addr)`.
- **Reply:** none. Frees the previously reserved bus address.

## Host-controller transfer protocol

A host controller context answers these transfer-request messages. Every request carries the caller's context as its last element (the reply target). The controller replies with `('complete n data)` where `n` is the byte count transferred (negative on error, timeout, or STALL) and `data` is a fresh bytevector for IN transfers or `#f` for OUT/no-data transfers.

```scheme
('control       addr speed mps setup data len reply)  → ('complete n data|#f)
('interrupt-in  addr speed ep maxp len reply)          → ('complete n data)
('bulk          addr ep maxp data len dir-in? reply)   → ('complete n data|#f)
('isoch         addr speed ep maxp data len dir-in? reply) → ('complete n data|#f)
('prepare-downstream parent-addr port speed reply)     → ('complete status #f)
('mark-hub      addr nports reply)                     → ('complete status #f)
('disconnect-dev addr)                                 ;; fire-and-forget
```

**Field notes:**

- `addr` — USB bus address (1–127); use 0 for the default address during enumeration before `SET_ADDRESS`.
- `speed` — one of `USB-SPEED-*` constants.
- `mps` — max packet size of endpoint 0 (control).
- `setup` — 8-byte bytevector produced by `make-setup`.
- `data` — OUT payload bytevector, or `#f` for no-data / IN transfers.
- `dir-in?` — `#t` for device-to-host, `#f` for host-to-device.
- `reply` — the context handle that will receive `('complete n data)`.

**`isoch` semantics:** the `len` bytes of `data` are split into `ceil(len/maxp)` packets, one per (micro)frame, scheduled back-to-back with no handshake or retry. A dropped packet is lost without retransmission. The controller replies once the whole submission has been clocked out. `prepare-downstream` and `mark-hub` are optional; UHCI replies `('complete 0 #f)` as a no-op.

**`disconnect-dev`** is fire-and-forget: no reply is expected. The controller uses this to reclaim any per-device state (toggle bits, slot IDs on xHCI, etc.).

## Class-driver dispatch protocol

After a successful enumeration, the enumerator sends a `probe` message to the class driver whose class byte matches the device's interface class (or device class if no interface descriptor is present):

```scheme
('probe dev)   ; dev is an enumerated-device record (see below)
```

On disconnect, coreusb sends:

```scheme
('remove addr)  ; addr is the USB bus address of the removed device
```

No reply is expected for either message.

## Enumerated-device record

The `dev` value handed to a class driver is an opaque list. Use the exported accessors:

```scheme
(usb-dev-hci    dev)        ; → controller context handle
(usb-dev-address dev)       ; → USB bus address (1–127)
(usb-dev-speed  dev)        ; → USB-SPEED-* integer
(usb-dev-mps0   dev)        ; → max packet size of endpoint 0
(usb-dev-config dev)        ; → raw configuration descriptor bytevector
(usb-dev-config-len dev)    ; → byte count of config descriptor
```

The raw config bytes are the full configuration descriptor blob (capped at 512 bytes). All descriptor parsing functions work from this blob.

## Exported functions

### Transfer functions (class driver API)

These run in the class driver's own context and block until the transfer completes (via `await-complete`). All send to `(usb-dev-hci dev)`.

```scheme
(usb-control-in  dev bmReq bReq wValue wIndex len)
```
Issues a control IN transfer. OR's `USB-REQ-DIR-IN` into `bmReq` automatically. Returns the data bytevector on success, `#f` on error (negative `n`).

```scheme
(usb-control-out dev bmReq bReq wValue wIndex data len)
```
Issues a control OUT transfer. OR's `USB-REQ-DIR-OUT` into `bmReq`. Returns the byte count (≥ 0) or −1.

```scheme
(usb-interrupt-in dev endpoint max-packet len)
```
Issues an interrupt IN transfer. Returns a bytevector or `#f` on error.

```scheme
(usb-bulk-in  dev endpoint max-packet len)
(usb-bulk-out dev endpoint max-packet data len)
```
Bulk IN/OUT. `usb-bulk-in` returns a bytevector or `#f`; `usb-bulk-out` returns the byte count or −1.

```scheme
(usb-isoch-out dev endpoint max-packet data len)
(usb-isoch-in  dev endpoint max-packet len)
```
Isochronous OUT/IN. No retry; a dropped packet is permanently lost. Used by the USB audio class driver. `usb-isoch-out` returns the byte count or −1; `usb-isoch-in` returns a bytevector or `#f`.

### Descriptor accessors

#### Simple (first-interface) accessors

Sufficient for single-interface devices (HID, mass storage).

```scheme
(usb-iface-class    dev)   ; → bInterfaceClass of first interface, or -1
(usb-iface-subclass dev)   ; → bInterfaceSubClass, or -1
(usb-iface-protocol dev)   ; → bInterfaceProtocol, or -1
(usb-iface-number   dev)   ; → bInterfaceNumber, or -1
```

```scheme
(usb-find-endpoint dev type dir-in?)
```
Searches the config descriptor for the first endpoint of transfer type `type` (use `USB-XFER-BULK` / `USB-XFER-INTERRUPT` / `USB-XFER-ISOCH`) and direction `dir-in?`. Returns `(list ep-address max-packet)` or `#f`.

#### Full multi-interface descriptor model

For multi-interface / multi-alternate-setting devices (audio, CDC):

```scheme
(usb-interfaces dev)
```
Returns a list of interface records, one per alternate setting, in descriptor order. Each record is accessed via:

```scheme
(iface-number   iface)   ; → bInterfaceNumber
(iface-alt      iface)   ; → bAlternateSetting
(iface-class    iface)   ; → bInterfaceClass
(iface-subclass iface)   ; → bInterfaceSubClass
(iface-protocol iface)   ; → bInterfaceProtocol
(iface-num-eps  iface)   ; → bNumEndpoints
(iface-offset   iface)   ; → byte offset of this descriptor in the config blob
```

```scheme
(usb-iface-endpoints dev num alt)
```
Returns a list of endpoint records belonging to interface `num` alternate setting `alt` (the descriptors between that interface descriptor and the next one). An audio streaming interface's alt 0 returns an empty list (zero-bandwidth). Each endpoint record:

```scheme
(ep-address    ep)   ; → bEndpointAddress (direction bit included)
(ep-attributes ep)   ; → bmAttributes raw byte
(ep-type       ep)   ; → transfer type: 0=ctl 1=iso 2=bulk 3=int
(ep-sync-type  ep)   ; → ISO sync type (bits 3:2 of bmAttributes)
(ep-dir-in?    ep)   ; → #t if bit7 of bEndpointAddress is set (IN)
(ep-number     ep)   ; → endpoint number (bits 3:0 of bEndpointAddress)
(ep-max-packet ep)   ; → wMaxPacketSize
(ep-interval   ep)   ; → bInterval
```

```scheme
(usb-find-ep-in eps type dir-in?)
```
Returns the first endpoint record from a list `eps` (as returned by `usb-iface-endpoints`) matching transfer type `type` and direction `dir-in?`, or `#f`.

### Standard requests

```scheme
(usb-get-descriptor dev recip dtype index windex len)
```
Issues `GET_DESCRIPTOR` with explicit recipient and `wIndex`. Returns the data bytevector or `#f`. Use `USB-REQ-RECIP-INTERFACE` and the interface number as `windex` for HID report descriptors (device-recipient would STALL).

```scheme
(usb-set-interface dev iface alt)
```
Issues `SET_INTERFACE` to select alternate setting `alt` of interface `iface`. Returns byte count ≥ 0 or −1. Used by the audio class driver to switch a streaming interface from its zero-bandwidth alt 0 to an alt that exposes the isochronous endpoint.

### String descriptors

```scheme
(usb-string-raw dev index langid)   ; → raw bytes or #f
(usb-langid dev)                    ; → first supported LANGID (e.g. #x0409), or 0
(usb-string-decode bytes)           ; → ASCII string (UTF-16LE, non-ASCII → '?')
(usb-string dev index langid)       ; → decoded string, "" on failure or index 0
```

### Robustness helpers

```scheme
(with-retries tries gap-ns ok? thunk)
```
Runs `thunk` up to `tries` times, returning the first result satisfying predicate `ok?`. On failure, sleeps `gap-ns` nanoseconds before retrying. Returns the last result once tries are exhausted. The enumerator wraps each control transfer in this (3 tries, 10 ms gap).

```scheme
(usb-clear-halt dev ep-addr)
```
Issues `CLEAR_FEATURE(ENDPOINT_HALT)` to clear a stalled bulk/interrupt endpoint. `ep-addr` is the full endpoint address (direction bit included). Returns byte count ≥ 0 or −1.

**Caveat:** `usb-clear-halt` internally calls `usb-control-out` → `await-complete`, which drops any non-`complete` message from the mailbox. A context that multiplexes other messages (e.g. a poll loop watching for a `stop` message) must implement its own CLEAR_FEATURE using its own receive loop, or the dropped message is lost permanently. See `usb-hid`'s `clear-halt` for the safe pattern.

### Hub helpers

```scheme
(usb-mark-hub dev nports)
```
Tells the host controller that `dev` is a hub with `nports` downstream ports. Needed so the controller can perform split transactions (for full/low-speed devices behind a high-speed hub). UHCI treats this as a no-op.

```scheme
(usb-enumerate-downstream usb dev port speed)
(usb-disconnect-downstream usb dev port)
```
Sends `enumerate-downstream` / `disconnect-downstream` to the coreusb handle `usb`. `dev` supplies the `hci-ctx` and `parent-addr`; `port` is the hub's downstream port number. These are fire-and-forget.

### Setup-packet builder

```scheme
(make-setup bmRequestType bRequest wValue wIndex wLength)   ; → 8-byte bytevector
```
Builds a USB setup packet (little-endian). Used internally by all transfer functions and exposed for class drivers that need to craft custom requests.

### Completion accessors

```scheme
(complete-n    c)   ; → byte count (negative on error)
(complete-data c)   ; → bytevector (IN transfers) or #f
```
Accessors for the raw `('complete n data)` reply from a host controller. Normally class drivers use the higher-level `usb-control-in` etc. instead.

## Constants

### Request direction

```scheme
USB-REQ-DIR-IN   ; #x80  (bit7 of bmRequestType)
USB-REQ-DIR-OUT  ; #x00
```

### Request type

```scheme
USB-REQ-TYPE-CLASS   ; #x20
```

### Request recipient

```scheme
USB-REQ-RECIP-DEVICE     ; #x00
USB-REQ-RECIP-INTERFACE  ; #x01
USB-REQ-RECIP-ENDPOINT   ; #x02
USB-REQ-RECIP-OTHER      ; #x03
```

### Standard requests

```scheme
USB-REQ-GET-STATUS        ; 0
USB-REQ-CLEAR-FEATURE     ; 1
USB-REQ-SET-FEATURE       ; 3
USB-REQ-SET-ADDRESS       ; 5
USB-REQ-GET-DESCRIPTOR    ; 6
USB-REQ-SET-CONFIGURATION ; 9
USB-REQ-SET-INTERFACE     ; 11
USB-FEATURE-ENDPOINT-HALT ; 0  (CLEAR_FEATURE / SET_FEATURE selector)
```

### Descriptor types

```scheme
USB-DESC-DEVICE      ; 1
USB-DESC-CONFIG      ; 2
USB-DESC-STRING      ; 3
USB-DESC-INTERFACE   ; 4
USB-DESC-ENDPOINT    ; 5
USB-DESC-IFACE-ASSOC ; 11  (Interface Association Descriptor)
```

### Class codes

```scheme
USB-CLASS-AUDIO        ; #x01
USB-CLASS-CDC          ; #x02
USB-CLASS-HID          ; #x03
USB-CLASS-MASS-STORAGE ; #x08
USB-CLASS-HUB          ; #x09
```

### Transfer types (bmAttributes bits 1:0)

```scheme
USB-XFER-CONTROL   ; 0
USB-XFER-ISOCH     ; 1
USB-XFER-BULK      ; 2
USB-XFER-INTERRUPT ; 3
```

### Speed codes

```scheme
USB-SPEED-LOW   ; 0
USB-SPEED-FULL  ; 1
USB-SPEED-HIGH  ; 2
USB-SPEED-SUPER ; 3
```

## Notes / gotchas

**Unexported names silently kill a spawned context.** coreusb's `define-module` block controls which names are exported. Any symbol used in a class driver that is not in coreusb's export list will produce an unbound-variable error at runtime in the importing context, which terminates that context silently (no panic, no log unless the VM's error handler prints it). If a class driver appears to probe but immediately disappears, check that every coreusb function it calls is in the `export` list in `coreusb.clp`.

**Class drivers must be registered before host controllers.** The `register-class` messages in `init.clp` are processed in-order before any `uhci-init` / `xhci-init` / `ehci-init` call, which is what guarantees a populated class table when the first `port-connected` arrives. Reversing this order is a race: a device present at power-on could enumerate and be dropped ("no class driver for class N") before the class driver registers.

**xHCI event-ring polling, not `msi-wait`.** xHCI completes transfers by posting to an event ring in memory and firing an MSI. The xHCI driver's poll loop reads the event ring directly rather than using the generic `msi-wait` primitive; `msi-wait` waits for a slot to be written but does not drain the ring correctly for doorbell-based controllers. UHCI and EHCI use `msi-wait` normally.

**`await-complete` drops non-completion messages.** The internal `await-complete` loop (used by `hci-control`, `hci-interrupt-in`, `hci-bulk`, `hci-isoch`) discards any message whose car is not `complete`. This is correct for the enumerator and for simple poll-loop class drivers that only receive transfer replies. Any class driver that expects other messages on its mailbox (e.g. a `stop` or a coreusb `remove`) while a transfer is in flight must implement its own receive loop with a stash, as `usb-storage` does.

**Config descriptor capped at 512 bytes.** The enumerator clips `wTotalLength` to 512 before requesting the full config descriptor. Devices with an unusually large descriptor blob (many interfaces/endpoints) will be silently truncated; descriptors past byte 512 are invisible to `usb-interfaces` / `usb-iface-endpoints`.

**`SET_ADDRESS` recovery delay.** The enumerator sleeps 50 ms after `SET_ADDRESS` before issuing any transfer at the new address. The USB 2.0 spec requires ≥ 2 ms; xHCI (and some real devices) want more. Do not reduce this delay.

**Multi-configuration devices always use configuration 0.** The enumerator reads `bNumConfigurations` and logs a warning when more than one configuration is present, but always selects configuration index 0 (`bConfigurationValue` from offset 5 of the config descriptor). There is no user-visible mechanism to select an alternate configuration.

**Address reuse after disconnect.** The bus-address pool uses a lowest-free-address scan; a freed address is immediately eligible for reuse on the next connect. There is no quarantine period. On real hardware, a device that briefly disconnects and reconnects may receive a different address.

**No hot-plug for class drivers.** Once a host controller sends its first `port-connected`, any `register-class` message that arrives afterward will update the class table, but any device already enumerated will not be re-dispatched to the newly registered driver. Class drivers must be registered before hardware bring-up.
