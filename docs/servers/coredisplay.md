# coredisplay

> Display device registry: maintains the list of registered display drivers and exposes a pure EDID parser used by drivers to decode monitor capabilities.

| | |
|---|---|
| **Source** | `lisp/servers/coredisplay.clp` |
| **Kind** | server |
| **Bound by** | `lisp/init.clp` — always (unconditionally at display bring-up) |
| **Registers with** | n/a — drivers register *with* coredisplay, not the other way around |
| **Capabilities** | `driver-util` only — no hardware capability; the EDID parser is pure bit-twiddling |

## Overview

coredisplay is a thin, stateful display-device registry. It runs as a `serve` loop that accumulates display-driver registrations in a list. Drivers — currently `lfb` and `virtio-gpu` — call `send` with a `register` message once their hardware is up; from then on, consumers (notably [corecompositor](corecompositor.md)) hold the per-driver context handle received in that registration and communicate directly with the driver context, not with coredisplay.

The second responsibility is `parse-edid`: a pure, capability-free parser, implemented entirely in Lisp byte operations, that converts a 128-byte EDID binary blob into a Lisp alist of monitor capabilities.

coredisplay does **not** itself perform any compositing, scanout, or mode-setting. It is an address-book / bring-up helper, not a display pipeline.

## Initialization

`init.clp` calls `start-display-service` unconditionally during the display bring-up sequence, before any driver `init` is invoked:

```scheme
(start-display-service)   ; → a context handle (the running registry loop)
```

The returned handle is passed to each driver init function (`lfb-init`, `virtio-gpu-init`) as their `display-svc` argument so they can send `register` messages.

```scheme
(define (start-display-service)
  (serve '()
    (lambda (disps m)
      (cond ((eq? (car m) 'register)   ; (register name conn ctx info)
             …
             (cons (cdr m) disps))
            (else disps)))))
```

The `serve` state is the accumulated list of registered display descriptors (`disps`). Each entry is the `cdr` of the received message — a `(name conn ctx info)` tuple stored verbatim.

## Message protocol

coredisplay itself handles only one message tag. All other display interaction goes directly to the driver context handle (`ctx`) obtained from the registry.

### `register`

Sent by a display driver context when its hardware is live and ready to serve display requests. coredisplay stores the descriptor and logs the name.

- **Request:** `(register name conn ctx info)`
  - `name` — human-readable string, e.g. `"Linear Framebuffer"` or `"Virtio GPU Display"`.
  - `conn` — connection state atom. Currently always `'unknown` (EDID read is not yet wired at registration time).
  - `ctx` — the driver's own context handle (obtained via `(self)` inside the driver loop). Consumers use this to send display requests directly to the driver.
  - `info` — driver-specific geometry / state. For `lfb`: `(width height pitch 32)`. For `virtio-gpu`: the full `scanouts` list (one scanout record per enabled display, each `(idx res-id w h fb)`).
- **Reply:** none — fire-and-forget.
- **Errors:** unrecognized tags are silently ignored; the registry state is unchanged.

### Driver context messages (lfb)

Once registered, clients communicate directly with the `ctx` handle. The `lfb` driver loop handles:

#### `get-framebuffer`

- **Request:** `(get-framebuffer reply)`
  - `reply` — the caller's own context handle to send the response to.
- **Reply:** `(fb width height pitch)` — `fb` is the mapped bytes object (uncached MMIO), dimensions in pixels, `pitch` in bytes.

#### `get-displayinfo`

- **Request:** `(get-displayinfo reply)`
- **Reply:** `(width height pitch bpp refresh-hz)` — `bpp` is always `32`; `refresh-hz` is always `60` (no EDID query).

#### `get-status`

- **Request:** `(get-status reply)`
- **Reply:** `'connected`

#### `flush`

- **Request:** `(flush)`
- **Reply:** none — no-op. The linear framebuffer is scanned out directly by the display controller; there is no host push step.

#### `fill`

- **Request:** `(fill color)`
  - `color` — a 32-bit packed pixel value in the framebuffer's native layout (use `pack-rgb` to construct).
- **Reply:** none. Fills the entire framebuffer with `color` using `bytes-fill32!` (vectorised, row-by-row to handle padded rows correctly).

### Driver context messages (virtio-gpu)

#### `flush`

- **Request:** `(flush)` or `(flush reply)`
  - With no `reply`: fire-and-forget — re-transfer and flush scanout 0 to the host via the virtqueue.
  - With `reply`: synchronous — sends `'flushed` to `reply` after the control-queue round-trip completes.
- **Reply:** `'flushed` (only when `reply` is supplied).

#### `flush-rects`

Dirty-rectangle flush path. Only the listed screen regions are transferred and presented to the host, rather than the whole scanout. Rects are clamped to scanout bounds; zero-area rects after clamping are dropped.

- **Request:** `(flush-rects rects)` or `(flush-rects rects reply)`
  - `rects` — a list of `(x y w h)` tuples (pixel coordinates, integer).
  - `reply` — optional; receives `'flushed` on completion.
- **Reply:** `'flushed` (only when `reply` is supplied).

#### `get-framebuffer`

- **Request:** `(get-framebuffer reply)`
- **Reply:** `(w h phys)` for scanout 0, or `#f` if no scanout is active.
  - `phys` is the **physical address** of the DMA backing buffer (not the bytes object). Callers must map it themselves via `mmio-map` / `mmio-map-wb` in their own context — sending the bytes object would copy-on-send (a multi-MB memcpy that would never reach the device).

#### `display-info`

- **Request:** `(display-info)`
- **Reply:** none — **stub** (`'todo`). Intended for the future display-resize / GET_DISPLAY_INFO path.

## Exported functions

### `(start-display-service)`

Spawns the registry `serve` loop and returns its context handle. Called once by `init.clp`; the handle is threaded into every driver `init`.

### `(parse-edid b)`

Pure EDID parser. No capabilities required; no device access.

- **`b`** — a `bytes` object of at least 128 bytes containing a raw EDID block.
- **Returns** an alist on success:

```scheme
((bit-depth    . N)           ; colour bit depth: 0 (analog/undefined), 6, 8, 10, 12, 14, or 16
 (gamma        . N)           ; raw gamma byte (byte 23 of EDID)
 (established  . N)           ; 24-bit established-timings bitmap (bytes 35-37)
 (standard-timings . ((h-res v-res v-freq aspect-num aspect-denom) …))
 (display-name . "string")    ; from the 0xFC display-descriptor, "" if absent
 (detailed-modes . (<mode-alist> …)))
```

Each `<mode-alist>` in `detailed-modes` is itself an alist with keys:

```scheme
pixel-clock   ; raw 16-bit pixel-clock value (EDID units: 10 kHz)
hactive       ; horizontal active pixels
hblank        ; horizontal blanking pixels
vactive       ; vertical active lines
vblank        ; vertical blanking lines
hsync-porch   ; horizontal sync front-porch
hsync-pulse   ; horizontal sync pulse width
vsync-porch   ; vertical sync front-porch
vsync-pulse   ; vertical sync pulse width
hsize-mm      ; horizontal physical size (mm)
vsize-mm      ; vertical physical size (mm)
hborder       ; horizontal border pixels
vborder       ; vertical border pixels
```

- **Returns `#f`** in any of these cases:
  - The 8-byte EDID magic (`00 FF FF FF FF FF FF 00`) does not match.
  - Byte 20 bit 7 is set (digital input) and the colour-depth code is `7` (reserved).
  - Any detailed timing descriptor is an analog timing (byte+17 bit 4 clear) — the entire parse is rejected.

## Notes / gotchas

**Registry is append-only.** The `serve` loop only handles `register`; there is no deregister message. If a driver context exits, its entry remains in the list with a stale handle. No driver currently hot-unplugs, so this is not an active issue.

**coredisplay stores `(cdr m)` verbatim.** When a driver sends `(register name conn ctx info)`, coredisplay stores `(name conn ctx info)` as one opaque list element. The registry does not inspect or validate the `info` field, so the format is driver-defined and callers must know which driver type they are talking to before interpreting it.

**lfb is the fallback.** `init.clp` gates `lfb-init` on the *absence* of a virtio-gpu device (`(pci-find #x1af4 #x1050)` returning `#f`). Only one of the two drivers registers at boot.

**Framebuffer bytes are copy-on-send.** The `lfb` `get-framebuffer` reply includes the `fb` bytes object directly. The virtio-gpu `get-framebuffer` reply intentionally sends only the physical address (`bytes-phys`), because sending the bytes object itself would trigger a copy-on-send that would produce a private copy disconnected from the scanned-out DMA buffer. Callers that need to draw into the GPU scanout must map the physical address in their own context.

**`lfb` test pattern runs in a spawned context.** The bring-up gradient (`fb-test-pattern!`) is millions of uncached MMIO writes; painting in the root `init` context (which is never GC-collected) would exhaust the system heap. `lfb-init` spawns a `spawn-restricted` context whose per-context heap is GC-managed. Pass `paint? = #f` (as `init.clp` does when `cardinal.gfxdemo` is active) to skip the paint and register immediately.

**EDID analog-mode bail-out.** If *any* of the four 18-byte detailed descriptors is an analog timing (byte+17 bit 4 = 0), `parse-edid` returns `#f` for the whole block. Mixed analog/digital descriptor blocks are unsupported.

**`conn` is always `'unknown`.** No driver currently reads EDID at registration time; the connection-status field in the registry entry is a placeholder for a future plug-detect / EDID-query path.

**corecompositor relationship.** [corecompositor](corecompositor.md) acquires the display driver's context handle (the `ctx` field of a registry entry) and wraps it in a `present` closure that calls `flush-rects` on the driver. coredisplay itself is not involved after registration; the compositor communicates with the driver directly.
