# corecompositor

> Multi-client window compositor: owns the screen back-buffer, arbitrates z-order, and pushes composited damage to the real scanout.

| | |
|---|---|
| **Source** | `lisp/servers/corecompositor.clp` |
| **Kind** | server |
| **Bound by** | `lisp/init.clp` — always (no `pci-find` gate); started in a `spawn-restricted '(sys-shm)` context after the display service is live |
| **Registers with** | n/a — owns scanout directly via an injected `present` closure |
| **Capabilities** | No kernel `sys-*` modules are `import`ed directly; DMA allocation (`dma-alloc-wb`), grant operations (`grant-mint`, `grant-revoke`), and scanout flushing (`present`) are **injected** by `init` as closures |

## Overview

`corecompositor` is a multi-client window compositor spanning phases 2–4. Phase 2
established the well-known PRIMARY mailbox and the `connect` handshake that spawns
a dedicated per-client HANDLER context. Phase 3 added the surface protocol and the
compositor's back-buffer painter loop. Phase 4 made the compositor own the real
scanout: after each compositing pass it calls an injected `present` closure that
either flushes dirty rects over the virtio-gpu controlq or copies them from the
cached WB back-buffer to the WC-mapped boot framebuffer.

**Architecture — one root, many handlers.** The ROOT context owns the single global
z-ordered surface table, the screen back-buffer, and all compositing. This ensures
cross-client occlusion is always correct (one painter pass over one ordered list).
Each per-client HANDLER is a thin relay + isolation boundary: it forwards the
client's surface ops to the root tagged with the client's authenticated identity,
and is the future home of phase-6 per-client input routing.

**Capabilities are injected, not imported.** Allocating DMA-backed surface buffers
and minting grants require kernel authority (`sys-mmio` / `sys-shm-mint`), but
importing those modules inside `corecompositor.clp` would make the module
unloadable in the host test harness. Instead `init` — which already holds that
authority — wraps the specific primitives as closures and passes them into
`start-compositor-service` via `make-compositor-caps`. This is Cardinal's standard
capability-delegation pattern: a supervisor hands a service the narrow prims it
needs rather than a broad module import. The injected closures survive the
`spawn-restricted '()`  root context because they are captured lexically.

**Phase status.** Phases 1–4 are merged. Phases 5–7 (cross-client window stacking
policy, per-client input delivery, sharded per-core instances) are future work.

## Initialization

`init.clp` starts the compositor inside a `spawn-restricted '(sys-shm)` context
(the spawner holds `sys-shm` so it can later delegate that capability to
window-client contexts via `spawn-restricted`):

```scheme
;; Build the injected-capability bundle.
(make-compositor-caps alloc mint revoke present)
;;   alloc   : (lambda (nbytes) ...) -> DMA-backed zeroed buffer (dma-alloc-wb)
;;   mint    : (lambda (buf perms) ...) -> grant handle (grant-mint)
;;   revoke  : (lambda (g) ...) -> invalidate grant (grant-revoke)
;;   present : (lambda (rects) ...) -> push (x y w h) list to display, or #f for RAM screen

;; Start the PRIMARY mailbox and return the compositor context.
(start-compositor-service screen caps)
;;   screen : a surface record (make-surface / make-surface*) for the back-buffer
;;   caps   : a caps bundle from make-compositor-caps
;;   -> the compositor context (a mailbox address)
```

Two display targets are built by `init`:

- **virtio-gpu** (`compositor-gpu-target gpu`): maps the scanout backing WB; present
  calls `flush-rects` on the controlq and blocks for the ack (frames pace to the
  device). Channel layout: X8R8G8B8 → offsets R=8 G=16 B=24.
- **Boot framebuffer** (`compositor-fb-target`): `make-double-buffer` over the
  WC-mapped MMIO front-buffer; present copies dirty rects back→front after an
  `sfence`. Channel layout: 0xRRGGBB → offsets R=16 G=8 B=0.

When neither `cardinal.compositordemo` nor a live display target is available, a
256×256 RAM back-buffer is used (the phase-3 posture: composites but pushes
nothing to the screen, useful for headless `cardinal.compositortest`).

### Pixel format advertisement

At `connect` the compositor returns a `fmt` triple `(r-off g-off b-off)` —
the channel bit-offsets of the screen surface. Clients **must** pack pixels
using these offsets. `gfx-blit!` is a raw 32-bit copy with no channel repacking,
so a surface drawn in the wrong layout will show swapped colours.

## Message protocol

There are two mailboxes: the **primary mailbox** (the compositor context returned
by `start-compositor-service`) handles only `connect` and internal `op` relay
messages. Every surface operation is sent to the **per-client handler** mailbox
returned in the `connected` reply.

### `connect` (primary mailbox)

```scheme
(send comp '(connect transparency? reply))
```

- **`transparency?`** — `#t` if this client's surfaces need alpha blending over
  lower windows; `#f` for fully-opaque windows.
- **`reply`** — **must be a context** (`ctx?` validated). Used both as the client's
  authenticated identity and as the destination for all future replies. The primary
  mailbox rejects non-context values outright; a non-context reaching `send` would
  abort the serve loop.

**Reply** (sent to `reply`):

```scheme
'(connected handler fmt)
;;  handler : the per-client mailbox (all surface ops go here)
;;  fmt     : (r-off g-off b-off) — screen pixel-channel offsets
```

### `create-surface` (handler)

```scheme
(send handler (list 'create-surface w h))
```

- **`w`**, **`h`** — surface dimensions in pixels; both must be positive integers
  ≤ 4096 (`MAX-DIM`). Non-integer or out-of-range values are rejected.

**Reply** (sent to the client's `reply` context):

```scheme
'(surface id g0 g1 stride)
;;  id     : integer surface identifier (sequential, owned by this client)
;;  g0, g1 : grant handles for the two backings (both RW); map with map-grant
;;  stride : bytes per row = w * 4
```

The surface is created **invisible** (not shown until `configure` sets `visible #t`).
Both backings are zeroed DMA-backed buffers; stride is always `w * 4` (32-bit pixels).

**Error reply** (sent to the client):

```scheme
'(surface-error bad-dimensions)
```

### `configure` (handler)

```scheme
(send handler (list 'configure id x y visible))
```

- **`id`** — integer surface id; must be owned by this client.
- **`x`**, **`y`** — integer screen coordinates of the surface's top-left corner.
- **`visible`** — `#t` to show, `#f` to hide.

**Reply:** none (fire-and-forget). The compositor recomposites immediately and
calls `present!` with the union of the vacated rect (old position) and, when
`visible` is now `#t`, the new rect (new position). A hide flushes only the
vacated rect.

### `commit` (handler)

```scheme
(send handler (list 'commit id buf rects))
```

- **`id`** — integer surface id; must be owned by this client.
- **`buf`** — `0` or `1`; selects which backing (`b0` or `b1`) is now the
  front (committed) buffer. The client should draw into the *other* backing.
- **`rects`** — client-supplied damage list (list of `(x y w h)`). In v1 this
  is advisory: the compositor always flushes the surface's full bounding rect.
  Per-rect flush is a planned refinement.

**Reply:** none (fire-and-forget). The compositor sets `SF-FRONT` to `buf`,
recomposites, and calls `present!` with the surface's bounding rect (if visible).

### `raise` (handler)

```scheme
(send handler (list 'raise id))
```

- **`id`** — integer surface id; must be owned by this client.

**Reply:** none (fire-and-forget). Moves the surface to the top of the z-stack
(front of the window list), recomposites, and flushes the surface's bounding rect.

### `destroy-surface` (handler)

```scheme
(send handler (list 'destroy-surface id))
```

- **`id`** — integer surface id; must be owned by this client.

**Reply on success** (sent to the client):

```scheme
'ok
```

**Reply on failure** (bad or foreign id):

```scheme
'(destroy-error no-such-surface)
```

On success both grants (`g0`, `g1`) are revoked, the surface is dropped from the
table, the compositor recomposites, and the vacated rect is flushed if the surface
was visible.

### `probe-pixel` (handler — test/debug)

```scheme
(send handler (list 'probe-pixel x y))
```

- **`x`**, **`y`** — integer screen coordinates.

**Reply** (sent to the client): the raw 32-bit pixel value at `(x, y)` in the
composited back-buffer.

This message **must go through the handler**, not directly to the primary mailbox.
Routing it via the handler ensures it is FIFO-ordered after any preceding `commit`
on the same channel — a direct-to-root probe could overtake a relayed commit and
read stale pixel data.

## Exported functions

### `(make-compositor-caps alloc mint revoke present)`

Packs the four injected capability closures into a list. Called by `init` before
`start-compositor-service`. The four arguments are:

| Arg | Signature | Source in `init` |
|-----|-----------|-----------------|
| `alloc` | `(nbytes) -> buf` | `dma-alloc-wb` |
| `mint` | `(buf perms) -> grant` | `grant-mint` |
| `revoke` | `(g) -> #f` | `grant-revoke` |
| `present` | `(rects) -> #f` or blocking | see display-target builders |

Returns an opaque list consumed by `start-compositor-service`.

### `(start-compositor-service screen caps)`

Starts the PRIMARY mailbox loop and returns the compositor context (a mailbox
address clients `send` to). `screen` is the back-buffer surface (must have valid
width, height, stride, and channel offsets). `caps` is the bundle from
`make-compositor-caps`.

On entry, paints the desktop background (`rgb 28 30 44`) into `screen` and calls
`present!` over the full screen rect so the display shows the compositor's backdrop
before any client connects.

### `(paint-windows screen bg windows)`

Pure (no IPC, no caps). Clears `screen` to `bg`, then blits each window spec in
order (back-to-front painter's algorithm). `windows` is a list of
`(src x y alpha?)` tuples — only visible surfaces. `alpha?` selects `blit-alpha`
(over-composition) vs `blit` (opaque copy).

Host-testable: this function has no kernel dependencies and can be called from the
host test harness directly.

## Notes / gotchas

**Root-owns-table + handler-relay.** The root context is the single owner of the
global surface table and the sole compositing path. Per-client handlers are pure
relays: they stamp the client's identity onto every message and forward it to the
root as `(op client transparency? m)`. Cross-client occlusion is trivially correct
because there is exactly one window list and one painter pass. Phase 7 will promote
the root to a per-core instance owning a shard; the protocol is unchanged.

**Ownership by sender identity, not by message content.** All ops that touch a
surface (`configure`, `commit`, `raise`, `destroy-surface`) check that the surface
id belongs to the *sender context* (`SF-CLIENT eq? client`), not just that the id
exists. Surface ids are sequential and guessable; without this check a client could
manipulate another client's windows.

**Arity guards (`len>=`).** Every op validates field count with `len>=` before
accessing message fields. A truncated or malformed message from a semi-trusted
client falls through to the `else` branch (logged, ignored) rather than reaching
`(cdr '())` and killing the root serve loop. The VM has no try/catch.

**`MAX-DIM = 4096`.** `create-surface` rejects `w` or `h` outside `(0, 4096]`.
A non-integer `w` would reach `(* w 4)` (type error, kills the root); a huge
integer would cause `dma-alloc-wb` to fail in an unguarded path. Both are defeated
by bounding the dimensions first.

**Double-buffering and `commit buf`.** Each surface has two backings (`b0`, `b1`).
`SF-FRONT` selects which backing the compositor reads during compositing. The
client draws into the *other* one (`1 - SF-FRONT`), then calls `commit` with the
backing it just finished. The compositor never reads a half-drawn frame.

**Phase-4 flush model — full recomposite, bounded present.** v1 repaints the whole
screen back-buffer on every change (simplest and obviously correct). Only the
damaged rects are pushed via `present!`, however — the rest of the display is
already correct — so the flush to hardware (virtio-gpu controlq or WC copy) is
bounded. Per-rect recompositing is a planned optimisation.

**Channel layout must agree end-to-end.** The compositor advertises `(r-off g-off
b-off)` at connect. Clients must use these offsets when packing pixels into their
backing buffers. `gfx-blit!` is a raw 32-bit copy; a mismatch causes red/blue
channel swap. `surf-src` stamps the same offsets onto the source surface passed to
`gfx-blit!` so the compositor→screen copy is also layout-correct.

**`spawn-restricted` capability delegation.** The spawning context in `init` holds
`sys-shm` not to use it itself (the scanout map and grant mint are captured
closures that work in any context), but so it can pass `sys-shm` to window-client
contexts via `spawn-restricted`. `spawn-restricted` refuses to grant a capability
the spawner does not hold.

**virtio-gpu present is blocking.** The `present` closure for virtio-gpu sends
`flush-rects` and then `recv`s the ack. This paces frames to the device and
prevents the compositor from queuing an unbounded number of flushes, but it means
the root serve loop blocks for one flush round-trip per compositing pass. The boot
framebuffer present is non-blocking (WC copy, no ack).

**`probe-pixel` ordering.** Routing `probe-pixel` through the handler (rather than
directly to the primary mailbox) is a correctness requirement, not a convention.
The handler's mailbox is a FIFO: a `probe-pixel` sent after a `commit` will always
be processed after that `commit` has been relayed to and processed by the root.
