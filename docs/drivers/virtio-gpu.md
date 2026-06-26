# virtio-gpu

> 2D paravirtualised display driver: brings a VirtIO 1.0 GPU device to `DRIVER_OK`, enumerates and initialises scanouts, and registers a framebuffer-backed display with [coredisplay](../servers/coredisplay.md).

| | |
|---|---|
| **Source** | `lisp/drivers/virtio-gpu.clp` (+ `lisp/drivers/virtio-gpu/cmds.clp`, `bringup.clp`, `driver.clp`) |
| **Kind** | driver |
| **Bound by** | `lisp/init.clp` — unconditionally called; `gpu-bringup` does an internal `pci-find` and returns `#f` if the device is absent (boot continues unaffected) |
| **Registers with** | [coredisplay](../servers/coredisplay.md) via `display-register`-style `(register name conn ctx scanouts)` send |
| **Capabilities** | `sys-mmio sys-pci driver-util virtio` (imports declared in `define-module`) |

## Overview

`virtio-gpu` is a pure-Lisp 2D virtio-gpu driver (PCI device 0x1AF4:0x1050). It is built on the shared `virtio` transport library (`lisp/lib/virtio.clp`), which handles the PCI capability walk, device-status handshake, feature negotiation, and split virtqueue setup. Everything GPU-specific — command framing, the ordered bring-up sequence, and the long-lived recv loop — lives in the three `include`d sub-files.

The driver only negotiates `VIRTIO_F_VERSION_1`; it does **not** request VirGL or any 3D feature. All command structs use **little-endian** byte accessors (`bytes-u32-set!`/`bytes-u64-set!`) — never the big-endian `put-be*!` helpers (those are for on-the-wire network headers).

The control queue is strictly serial (one command in flight at a time). Rather than taking an MSI per response the driver **polls** the used-ring index after each command, yielding via `wait-until-spin` with a 500 µs busy-spin window before descending into the scheduler. MSI-driven resize events are a future TODO.

The framebuffer backing each scanout is allocated as **write-back cached** (`dma-alloc-wb`), not uncached. The CPU composes into it at cache speed; x86 DMA is cache-coherent, so the device reads it correctly. An `sfence` before a `flush` message is the caller's responsibility (see Gotchas).

## Initialization

`init.clp` calls `virtio-gpu-init` once, passing the coredisplay service handle. The function spawns a restricted context (no capabilities) and returns the spawned handle immediately; the caller is never blocked.

```scheme
(virtio-gpu-init display-svc)   ; → spawned context handle (or #f if spawn fails)
```

Inside the spawned context `gpu-bringup` runs the ordered bring-up sequence (see below). If any enabled scanout successfully initialises the context registers with coredisplay and enters the driver recv loop. If `gpu-bringup` returns `#f`, or no scanout comes up, the context logs and exits without registering.

### `gpu-bringup` — internal bring-up sequence

```scheme
(gpu-bringup)   ; → (ctrlq notify mult scanouts) | #f
```

Steps, in order:

1. `(pci-find VIRTIO-GPU-VID VIRTIO-GPU-DID)` — aborts with `#f` if the device is absent.
2. `(virtio-bringup ecam 0 (arithmetic-shift 1 VIRTIO-F-VERSION-1-BIT))` — walks PCI capabilities, maps COMMON/NOTIFY/DEVICE regions, drives the ACK→DRIVER→FEATURES_OK→DRIVER_OK status machine, negotiates `VERSION_1` only.
3. `(virtio-setup-queue common GPU-CTRLQ)` (queue 0) — allocates descriptor/avail/used rings via `dma-alloc` and hands the device their physical addresses.
4. `(virtio-setup-queue common GPU-CURSORQ)` (queue 1) — allocated for parity with the C driver; **not used** by the Lisp driver.
5. Sets `VIRTIO_STATUS_DRIVER_OK`.
6. Issues `GET_DISPLAY_INFO` and iterates the 16 possible scanouts. For each enabled scanout, calls `init-scanout`.

### `init-scanout` — per-scanout bring-up

```scheme
(init-scanout ctrlq notify mult scanout-idx res-id w h)
; → (scanout-idx res-id w h fb) | #f
```

Sends five control-queue commands in strict order:

| Command | Effect |
|---------|--------|
| `RESOURCE_CREATE_2D` | Creates a host resource with format `X8R8G8B8` and the reported dimensions |
| `RESOURCE_ATTACH_BACKING` | Attaches a single `dma-alloc-wb` guest buffer as the resource backing (single `mem_entry`; scatter-gather chunking for very large modes is a TODO) |
| `SET_SCANOUT` | Points scanout `scanout-idx` at the resource |
| `TRANSFER_TO_HOST_2D` | Pushes the initial (white-filled) frame to the host resource |
| `RESOURCE_FLUSH` | Signals the host to display the resource on the monitor |

The framebuffer is pre-filled white via `bytes-fill32!` (a single vectorized C call). Resource IDs are assigned sequentially starting at 1, one per enabled scanout.

Returns `#f` if `RESOURCE_CREATE_2D` fails (device NACK or timeout); subsequent commands are skipped to avoid confusing the host.

## Message protocol

The driver recv loop (entered after successful registration) handles the following messages serially. The control queue is single-command-in-flight, so messages are processed one at a time.

### `(flush)`

- **Request:** `(flush)` — fire-and-forget full-frame push.
- **Effect:** issues `TRANSFER_TO_HOST_2D` + `RESOURCE_FLUSH` for scanout 0 (the full scanout rectangle).
- **Reply:** none.

### `(flush reply-ctx)`

- **Request:** `(flush reply-ctx)` — synchronous full-frame push.
- **Effect:** same as `(flush)`, then sends `flushed` to `reply-ctx` after the control-queue round-trip completes.
- **Reply:** `flushed` sent to `reply-ctx`.
- **Usage:** the caller can use this to time a frame (block until the GPU has received it). Example from `init.clp`:

```scheme
(sfence)                              ; fence composed stores before notifying GPU
(send gpu (list 'flush (self)))
(recv)                                ; blocks until 'flushed
```

### `(flush-rects rects)`

- **Request:** `(flush-rects rects)` — dirty-rect push, fire-and-forget.
- **Effect:** for each rect in `rects` (a list of `(x y w h)` lists) issues a `TRANSFER_TO_HOST_2D` + `RESOURCE_FLUSH` covering only that rectangle. Rects are clamped to scanout bounds; a rect with zero area after clamping is silently dropped.
- **Reply:** none.

### `(flush-rects rects reply-ctx)`

- **Request:** `(flush-rects rects reply-ctx)` — dirty-rect push with completion ack.
- **Effect:** same as `(flush-rects rects)`, then sends `flushed` to `reply-ctx`.
- **Reply:** `flushed` sent to `reply-ctx`.

### `(get-framebuffer reply-ctx)`

- **Request:** `(get-framebuffer reply-ctx)`.
- **Effect:** reads scanout 0's record and sends the caller its geometry and backing physical address.
- **Reply:** `(w h phys)` sent to `reply-ctx`, where `phys` is the physical base address of the write-back cached framebuffer. Returns `#f` if no scanout is registered.
- **Critical:** the reply is NOT the framebuffer bytes object. Sending the bytes would invoke copy-on-send (a several-megabyte copy whose writes would go to the copy, not the device-visible backing). Callers must map the physical address themselves:

```scheme
(send gpu (list 'get-framebuffer (self)))
(let ((r (recv)))           ; → (w h phys) or #f
  (let* ((w (car r)) (h (cadr r)) (phys (caddr r))
         (fb (mmio-map-wb phys (* w h 4))))
    ;; draw into fb — writes go to the device-coherent backing
    ...))
```

### `(display-info)`

- **Request:** `(display-info)`.
- **Effect:** **stub** — intended as the future resize-event path (re-read `GET_DISPLAY_INFO` and update scanout geometry). Currently a no-op (`'todo`).
- **Reply:** none.

## Exported functions

The following symbols are exported from `define-module virtio-gpu` and are available to importers (primarily the in-OS self-test, which uses them to validate struct offsets without a device).

### Entry point

#### `(virtio-gpu-init display-svc)`

Driver entry point. Spawns a restricted context that runs the full bring-up and, on success, registers with coredisplay and enters the recv loop. Returns the spawned context handle immediately; the caller is never blocked.

### Command builders

All builders return a `bytes` object containing a fully-formed little-endian control-queue command buffer, ready to copy into a DMA buffer via `gpu-cmd!`.

#### `(make-display-info-cmd)`

Builds a 24-byte `GET_DISPLAY_INFO` command (header only).

#### `(make-create-2d res-id fmt w h)`

Builds a 40-byte `RESOURCE_CREATE_2D` command. `fmt` should be `GPU-FORMAT-X8R8G8B8` (= 4).

#### `(make-attach-backing res-id addr length)`

Builds a 48-byte `RESOURCE_ATTACH_BACKING` command with a single `mem_entry`. `addr` is the **physical** address of the guest backing buffer (obtain with `bytes-phys`).

#### `(make-set-scanout scanout-id res-id x y w h)`

Builds a 48-byte `SET_SCANOUT` command pointing scanout `scanout-id` at resource `res-id` with the given source rectangle.

#### `(make-transfer-2d res-id offset x y w h)`

Builds a 56-byte `TRANSFER_TO_HOST_2D` command. `offset` is the byte offset within the resource's linear backing (use `y * stride + x * 4` for a sub-rectangle; 0 for a full-frame transfer).

#### `(make-flush res-id x y w h)`

Builds a 48-byte `RESOURCE_FLUSH` command flushing a rectangle of resource `res-id` to the scanout.

### Response accessors

#### `(gpu-resp-type resp)`

Returns the `u32` type field at offset 0 of a response buffer. Compare against `GPU-RESP-OK-NODATA` (#x1100) or `GPU-RESP-OK-DISPLAY-INFO` (#x1101).

#### `(resp-display-enabled? resp i)`

Returns `#t` if scanout `i` (0–15) is marked enabled in a `GET_DISPLAY_INFO` response.

#### `(resp-display-width resp i)` / `(resp-display-height resp i)`

Return the width and height of scanout `i` from a `GET_DISPLAY_INFO` response.

### Constants

| Constant | Value | Meaning |
|----------|-------|---------|
| `GPU-RESP-MAX` | 408 | Largest possible GPU response (header + 16 × `pmode`); allocate response buffers at least this large |
| `GPU-RESP-OK-NODATA` | #x1100 | Success response with no payload |
| `GPU-RESP-OK-DISPLAY-INFO` | #x1101 | Success response carrying `GET_DISPLAY_INFO` data |

## Notes / gotchas

**`get-framebuffer` returns a physical address, not a bytes object.**
The framebuffer backing is guest RAM. If the driver were to return the `bytes` handle, the VM's copy-on-send semantics would make a full-size copy (4 MB for a 1024×1024 display) and writes into the copy would never reach the device-visible backing. The driver therefore returns only `(w h phys)`. Callers must map the physical address with `mmio-map-wb` in their own context and draw there directly.

**`sfence` before a synchronous flush.**
The compositing path writes to the WB-cached framebuffer and then sends a `(flush reply-ctx)` to the driver. The store fence (`sfence`) must be issued **before** the send, not after, so the driver's `TRANSFER_TO_HOST_2D` command does not race against stores still in the CPU write buffer. Without the fence the host may read a partially-updated frame.

**Bring-up is asynchronous.**
`virtio-gpu-init` returns the spawned context handle immediately. The device is not yet at `DRIVER_OK` and the recv loop is not yet running. Code that wants to call `(get-framebuffer ...)` or send `(flush ...)` must first allow the scheduler to run `gpu-bringup` to completion. In `init.clp` the compositor demo sleeps 800 ms before attempting `get-framebuffer`.

**Control queue is strictly serial.**
`ctrlq-cmd!` (from `lisp/lib/virtio.clp`) posts a fixed descriptor pair (0, 1) and waits for the used ring to advance before returning. Issuing a second command while one is in-flight is not supported. The driver's recv loop processes one message at a time, so concurrent senders are serialised by the message queue, not by the driver explicitly.

**Cursor queue is allocated but unused.**
`GPU-CURSORQ` (queue 1) is set up during bring-up for parity with the C driver. No cursor commands are ever issued.

**Scatter-gather backing is not implemented.**
`make-attach-backing` always sends `nr_entries = 1`. For modes where `w * h * 4` exceeds a single contiguous DMA allocation's practical limit, chunked attach-backing is needed but is currently a TODO.

**Display resize is a stub.**
The `(display-info)` message does nothing. Hotplug/resize events from the host are silently ignored.

**Format is always `X8R8G8B8` (value 4).**
Byte layout in memory (little-endian): byte 0 = B, byte 1 = G, byte 2 = R, byte 3 = X (ignored). When interpreting pixel values numerically as a 32-bit little-endian word: R occupies bits 8–15, G bits 16–23, B bits 24–31 (i.e., `0x00RRGGBB` in memory order becomes `color = R<<8 | G<<16 | B<<24`). This differs from the more common `ARGB` or `RGBA` conventions used in higher-level graphics code — consult `init.clp`'s `gpu-bench` comment for the channel-offset table.

**Command timeout is 1 second.**
`GPU-CMD-TIMEOUT-NS` is 1 000 000 000 ns. A timeout causes `gpu-cmd!` to return `#f`; `gpu-cmd-ok!` logs a warning and the bring-up continues. A timeout during `create-2d` is treated as fatal for that scanout (the resource was never created, so subsequent commands would reference an invalid resource-id and NACK continuously).
