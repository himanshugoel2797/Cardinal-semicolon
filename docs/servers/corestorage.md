# corestorage

> Block-device and filesystem-provider registry: mediates all block I/O between hardware drivers and filesystem providers.

| | |
|---|---|
| **Source** | `lisp/servers/corestorage.clp` |
| **Kind** | server |
| **Bound by** | `lisp/init.clp` — always started (unconditionally); block drivers and fs providers are gated on their own `pci-find` / hardware presence |
| **Registers with** | n/a — nothing registers _with_ corestorage; corestorage is the registry that others register into |
| **Capabilities** | none — the service itself holds no `sys-*` capability imports; it only routes messages between driver and provider contexts |

## Overview

`corestorage` is a pure message-routing service.  It maintains two lists:

- **Block devices** — each entry records `(name bsize bcount claimed? driver-ctx)`.
  `driver-ctx` is the Lisp context handle that owns the real hardware.
- **Filesystem providers** — each entry records `(name provider-ctx)`.

When a block device is registered, corestorage immediately `probe`s every already-registered fs provider with it.  When an fs provider registers, corestorage immediately probes it against every unclaimed block device.  A provider claims a device by sending `(claim <name>)` back to the storage handle it received in the probe; once claimed the device is never offered to a later provider.

For read/write, corestorage acts as a bounds-checking proxy: it validates the LBA range against the registered `bcount` and then forwards the request directly to the driver context.  The driver context sends the `(complete ...)` reply back to the caller's nominated reply handle — the service itself is not in that data path after forwarding.

The service holds no capabilities and does no I/O itself.

### Who talks to it

| Participant | Direction | What they send |
|---|---|---|
| [ahci](../drivers/ahci.md) driver | → service | `register-blockdev` after IDENTIFY |
| [usb-storage](../drivers/usb-storage.md) driver | → service | `register-blockdev` after SCSI READ CAPACITY |
| [cardfs](cardfs.md) server | → service | `register-fsprovider` at start-up; `claim` in response to a probe |
| Any caller with the service handle | → service | `read` / `write` |
| Service | → provider | `probe` (sent at registration time) |
| Driver context | → caller's reply handle | `complete` (reply to read/write) |

## Initialization

`init.clp` calls `start-storage-service` with no arguments, captures the returned context handle, and passes it to every block driver and fs provider that needs it:

```scheme
(start-storage-service)   ; → context handle (the service mailbox)
```

The returned handle is opaque.  `init.clp` pattern:

```scheme
(let ((storage (start-storage-service)))
  (start-cardfs storage)      ; fs provider — registers immediately
  (ahci-init storage)         ; block driver — registers asynchronously (spawned)
  (usb-storage-init usb storage))  ; block driver via USB
```

`cardfs` registers as an fs provider before `ahci-init` is called so that when the AHCI bring-up completes and sends `register-blockdev`, the provider is already present and receives the probe immediately.

## Message protocol

All messages are sent to the `storage` context handle.  Each is a list whose `car` is the tag symbol.

---

### `register-blockdev`

A block driver calls this once after its hardware is ready and it has determined the block geometry.

- **Request:** `(register-blockdev <name> <bsize> <bcount> <driver-ctx>)`
  - `name` — symbol identifying the device, e.g. `'ahci0` or `'usb0`.
  - `bsize` — block size in bytes (e.g. `512`).
  - `bcount` — total number of blocks on the device.
  - `driver-ctx` — context handle of the driver that will service `read`/`write` forwards.
- **Reply:** none — fire and forget.
- **Side effect:** the device is appended to the device list; each already-registered fs provider receives a `probe` message (see below).

```scheme
(send storage (list 'register-blockdev 'ahci0 512 sector-count driver-ctx))
```

---

### `register-fsprovider`

An fs provider calls this once at start-up to announce itself.

- **Request:** `(register-fsprovider <name> <provider-ctx>)`
  - `name` — symbol identifying the provider, e.g. `'cardfs`.
  - `provider-ctx` — context handle that will receive `probe` messages.
- **Reply:** none — fire and forget.
- **Side effect:** the provider is appended to the provider list; each unclaimed block device already present has a `probe` message sent to `provider-ctx`.

```scheme
(send storage (list 'register-fsprovider 'cardfs prov-ctx))
```

---

### `claim`

Sent by an fs provider back to the storage service (via the `storage` handle it received in a `probe`) to declare that it has recognised and mounted a device.  Once claimed, a device is never offered again to a subsequent provider.

- **Request:** `(claim <name>)`
  - `name` — symbol, must match an existing device name.
- **Reply:** none.

```scheme
(send storage (list 'claim 'ahci0))
```

---

### `read`

Bounds-checked read forwarded to the owning driver context.

- **Request:** `(read <name> <lba> <count> <reply>)`
  - `name` — device name symbol.
  - `lba` — first logical block address (0-based).
  - `count` — number of blocks to read.
  - `reply` — context handle that will receive the `complete` response.
- **Reply:** none from the service itself; the driver context sends `(complete <status> <bytes>)` to `reply` (see [Driver reply: `complete`](#driver-reply-complete) below).
- **Guard:** `reply` must satisfy `(ctx? reply)`; if not, the request is silently dropped.
- **Errors:** if `name` is unknown or `(+ lba count) > bcount`, the service sends `(complete -1 #f)` to `reply` directly (without forwarding to the driver).

```scheme
(send storage (list 'read 'ahci0 0 1 my-ctx))
; ... later recv:
; (complete 0 <bytes-object>)   ; success
; (complete -1 #f)              ; bounds error or unknown device
```

---

### `write`

Bounds-checked write forwarded to the owning driver context.

- **Request:** `(write <name> <lba> <count> <data> <reply>)`
  - `name` — device name symbol.
  - `lba` — first logical block address.
  - `count` — number of blocks to write.
  - `data` — `bytes` object containing the data to write (`count * bsize` bytes).
  - `reply` — context handle that will receive the `complete` response.
- **Reply:** none from the service; the driver sends `(complete <status>)` to `reply`.
- **Guard:** `reply` must satisfy `(ctx? reply)`; if not, the request is silently dropped.
- **Errors:** if `name` is unknown or `(+ lba count) > bcount`, the service sends `(complete -1)` to `reply` directly.

```scheme
(send storage (list 'write 'ahci0 lba 1 data-bytes my-ctx))
; ... later recv:
; (complete 0)    ; success
; (complete -1)   ; bounds error or unknown device
```

---

### Probe message (sent by the service to providers)

When corestorage offers a block device to a registered fs provider, it sends:

- **Message:** `(probe <name> <bsize> <bcount> <driver-ctx> <storage>)`
  - `name` — device name symbol.
  - `bsize` — block size in bytes.
  - `bcount` — total block count.
  - `driver-ctx` — the driver context handle (the provider may use this for raw block I/O while probing, but should go through the storage handle for normal use).
  - `storage` — the corestorage service handle, so the provider can send `(claim name)` back.

The provider is free to do block I/O against `driver-ctx` directly (or via `storage`) to inspect the volume before deciding.  Since `probe` is a message and not a synchronous re-entrant call, a provider that reads blocks while handling a probe cannot deadlock the service.

```scheme
;; Inside the fs provider's serve loop:
((eq? (car m) 'probe)
 (let ((name (cadr m)) (bcount (cadddr m)) (stor (nth m 5)))
   (if (volume-recognised? ...)
       (send stor (list 'claim name))
       'ignore)))
```

---

### Driver reply: `complete`

This is NOT sent to the storage service.  It is sent by the driver context (or by corestorage on a bounds error) directly to the `reply` handle the caller supplied in `read` or `write`.

- **For `read`:** `(complete <status> <bytes-or-#f>)`
  - `status` — `0` on success, `-1` on error.
  - second element — a `bytes` object containing the read data on success; `#f` on error.
- **For `write`:** `(complete <status>)`
  - `status` — `0` on success, `-1` on error.

## Exported functions

### `(start-storage-service)`

Spawns the storage service loop and returns the context handle (mailbox) for the service.  Called once from `init.clp`.  Takes no arguments.

## Notes / gotchas

**No capabilities held.** The service context imports only `driver-util`; it holds no `sys-*` capabilities.  All sensitive I/O stays in the driver contexts.

**Serialisation through the driver mailbox.** Because block requests are forwarded to a single driver context's mailbox, I/O to a given device is naturally serialised by the driver's message queue.  The AHCI driver's `make-driver-ctx` and the USB storage server both rely on this — they use a single DMA data buffer per device, which would race if two requests ran concurrently.  corestorage itself does not add any additional lock.

**`count` cap is driver-side, not service-side.** The service only bounds-checks `(+ lba count) <= bcount`.  It does NOT cap `count` to a maximum transfer size.  The AHCI driver additionally enforces a `DATA-SECTORS` (8 sector / 4 KB) per-request cap at the driver level and returns `(complete -1 #f)` for oversized requests.

**Probe is fire-and-forget.** The service sends `probe` and immediately returns to its event loop without waiting for a `claim`.  There is no timeout and no negative-acknowledgement path: if a provider inspects the volume and declines, it simply does not send `(claim name)`, and the device remains unclaimed (available to subsequent providers).

**Claimed flag is immutable list rebuild.** Device entries are stored as plain lists (immutable in this Lisp dialect).  Marking a device claimed replaces its entry with a new list via `map`; there is no in-place mutation.

**Single name per device.** Device and provider names are plain symbols compared with `eq?`.  There is no deduplication check; registering two devices with the same name results in two entries, but `find-dev` returns the first match, so the second is silently shadowed.

**Only two block drivers exist today.** As of the current tree, `ahci` (SATA) and `usb-storage` (USB mass storage) are the only drivers that call `register-blockdev`.  Only [cardfs](cardfs.md) calls `register-fsprovider`.

**`ctx?` guard on read/write.** The service uses `(ctx? (nth m 4))` / `(ctx? (nth m 5))` to validate the reply handle before forwarding.  A message with a non-context reply argument is silently discarded (state is returned unchanged).
