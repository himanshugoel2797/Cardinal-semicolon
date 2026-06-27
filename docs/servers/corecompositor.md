# corecompositor

> Multi-client window compositor: owns the screen, arbitrates global z-order, routes input, and pushes composited damage to the real scanout — with one compositor instance per core.

| | |
|---|---|
| **Source** | `lisp/servers/corecompositor.clp` |
| **Kind** | server |
| **Bound by** | `lisp/init.clp` — always (no `pci-find` gate). The **owner** is started in a `spawn-restricted '(sys-shm)` context after the display service is live; a per-core **shard** is spawned on every application processor by the per-core bring-up hook |
| **Registers with** | `coreinput` (as an input subscriber); owns the scanout directly via an injected `present` closure |
| **Capabilities** | No kernel `sys-*` modules are `import`ed directly. DMA allocation (`dma-alloc-wb`), grant operations (`grant-mint`, `grant-revoke`, `map-grant`), scanout flushing (`present`), and the shard-mesh key are all **injected** by `init` as closures via `make-compositor-caps` |

## Overview

`corecompositor` is the display server above `coredisplay` / the display drivers.
Independent client contexts own off-screen, double-buffered surfaces in
**zero-copy shared memory** (grant-backed DMA buffers); the compositor composites
the visible ones into the scanout, arbitrates z-order, and routes keyboard and
pointer input to the focused window. All phases (the IPC root, the surface
protocol, the driver seam, input routing + drag-move, and cross-core sharded
instances with a two-level layer merge) are implemented.

**One instance per core.** The compositor runs as an **owner** on core 0 plus one
**shard** per application processor (always on — `init`'s per-core hook spawns a
shard on every AP). The owner holds the scanout, the global z authority, and input
routing; each shard composites the opaque clients routed to it into a grant-shared
layer the owner folds into the scanout. On a single-core boot there are no APs, so
the owner runs the degenerate N=1 path alone — the same code, no shard merge.

**Capabilities are injected, not imported.** Allocating DMA-backed buffers, minting
grants, mapping a shard's layer, and flushing the scanout all require kernel
authority (`sys-mmio` / `sys-shm-mint`), but importing those modules inside
`corecompositor.clp` would make it unloadable in the host test harness. Instead
`init` — which already holds that authority — wraps the specific primitives as
closures and passes them in via `make-compositor-caps`. This is Cardinal's standard
capability-delegation pattern: a supervisor hands a service the narrow prims it
needs rather than a broad module import. The injected closures survive the
`spawn-restricted '()` root because they are captured lexically.

## Architecture

**Owner + per-client handlers.** Each instance (owner or shard) owns a single
global, z-ordered surface table and is the sole compositing path for its clients.
A client `connect`s once on the well-known **primary mailbox**; the instance spawns
a dedicated per-client **handler** context (the "secondary channel") and replies
with its mailbox. The handler is a thin relay + isolation boundary: it forwards the
client's surface ops to the root tagged with the client's authenticated identity, so
a wedged client backs up only its own handler.

**Compositing is a z-buffer merge, not list order.** Every window carries a global
z-key (`SF-Z`) stamped by the owner's monotonic counter. Recompositing is two
passes: opaque windows build a `(colour, z)` **layer**; the merge `gfx-zpick!`s the
layer(s) into the scanout taking the max-z contributor per pixel (yielding the
per-pixel topmost-opaque z, `Zop`); translucent windows are then `gfx-blend-z!`
alpha-overed only where their z is above `Zop`. A max-z pick is order-independent,
so folding it over the owner's own layer plus every shard's layer is correct
regardless of how windows are sharded across cores.

**Input.** The compositor subscribes to `coreinput` and is the focus authority.
A key routes to the globally focused window's client; a pointer press in a window's
title strip starts a compositor-internal move (drag), a body press is routed to the
client in window-local coordinates.

## Initialization

`init.clp` starts the **owner** inside a `spawn-restricted '(sys-shm)` context (the
spawner holds `sys-shm` so it can later delegate it to window-client contexts):

```scheme
;; Build the injected-capability bundle (six closures/values).
(make-compositor-caps alloc mint revoke present map key)
;;   alloc   : (nbytes)      -> DMA-backed zeroed buffer        (dma-alloc-wb)
;;   mint    : (buf perms)   -> grant handle                    (grant-mint)
;;   revoke  : (g)           -> invalidate grant                (grant-revoke)
;;   present : (rects)       -> push (x y w h) list to display, or #f for a RAM screen
;;   map     : (g)           -> bytes view over a granted region (map-grant), or #f
;;   key     : the shard-mesh capability token (the rendezvous ctx), or #f (host harness)

;; Start the PRIMARY mailbox and return the instance context.
(start-compositor-service screen caps shard-cfg)
;;   screen    : back-buffer surface (owner: the scanout; shard: its own layer)
;;   caps      : a bundle from make-compositor-caps
;;   shard-cfg : #f for the OWNER; a make-shard-cfg for a SHARD
;;   -> the instance context (a mailbox address)
```

Two display targets are built by `init` for the owner's `present`:

- **virtio-gpu** (`compositor-gpu-target gpu`): maps the scanout backing WB; present
  calls `flush-rects` on the controlq and blocks for the ack (frames pace to the
  device). Channel layout: X8R8G8B8 → offsets R=8 G=16 B=24.
- **Boot framebuffer** (`compositor-fb-target`): `make-double-buffer` over the
  WC-mapped MMIO front-buffer; present copies dirty rects back→front after an
  `sfence`. Channel layout: 0xRRGGBB → offsets R=16 G=8 B=0.

With no live display target a 256×256 RAM back-buffer is used (composites but
pushes nothing — the headless `cardinal.compositortest` posture).

**Shards.** `init`'s per-core hook spawns a shard on every AP. A shard rendezvouses
for the owner handle, asks the screen geometry, `dma-alloc-wb`s a matching
`(colour, z)` layer, mints `rw` grants over it, starts a shard-role
`start-compositor-service` (its `screen` *is* the layer; `present` is `#f` — the
owner flushes), and registers its handle + layer grants with the owner.

### Pixel format advertisement

At `connect` the compositor returns a `fmt` triple `(r-off g-off b-off)` — the
channel bit-offsets of the screen surface. Clients **must** pack pixels using these
offsets: `gfx-blit!` is a raw 32-bit copy with no channel repacking, so a surface
drawn in the wrong layout shows swapped colours.

## Message protocol

Two mailboxes: the **primary mailbox** (the instance context) handles `connect`, the
internal `op` relay, input events, and the privileged inter-instance verbs; every
surface operation flows on the **per-client handler** mailbox from the `connected`
reply.

### `connect` (primary mailbox)

```scheme
(send comp (list 'connect transparency? reply))
```

- **`transparency?`** — `#t` if this client's surfaces need alpha blending over
  lower windows; `#f` for fully-opaque windows.
- **`reply`** — **must be a context** (`ctx?` validated). Used both as the client's
  authenticated identity and as the destination for all future replies. A
  non-context reaching `send` would abort the serve loop, so it is rejected outright.

**Routing.** The owner routes an **opaque** client round-robin to a **shard** (so
its windows composite on that core); the shard spawns *its* handler and replies to
the client directly. A **translucent** client (alpha must be composed centrally),
and any client when there are no shards, stays on the owner. Either way the reply is:

```scheme
(list 'connected handler fmt)
;;  handler : the per-client mailbox (all surface ops go here)
;;  fmt     : (r-off g-off b-off) — screen pixel-channel offsets
```

### Surface operations (handler mailbox)

| Message | Reply (→ client) | Meaning |
|---|---|---|
| `(create-surface w h)` | `(surface id g0 g1 stride)` / `(surface-error bad-dimensions)` | `w,h` positive integers ≤ `MAX-DIM` (4096); allocate 2 zeroed backings, mint a grant over each, record invisible. `stride = w*4` |
| `(configure id x y visible)` | — (fire-and-forget) | place + show/hide → recomposite; flush the new rect when shown and the old rect when it was visible there before |
| `(commit id buf rects)` | — (fire-and-forget) | `buf` (0/1) becomes the committed front; recomposite; flush the surface rect. `rects` is advisory in v1 |
| `(raise id)` | — (fire-and-forget) | stamp a fresh top z, restack, recomposite, flush the surface rect |
| `(destroy-surface id)` | `'ok` / `(destroy-error no-such-surface)` | revoke both grants, drop, recomposite, flush the vacated rect |
| `(probe-pixel x y)` | the composited 32-bit pixel | test/debug; **must** go via the handler so it is FIFO-ordered after a preceding `commit` |

Every id-bearing op acts **only** on a surface owned by the requesting client
(ownership is the stamped sender identity, since ids are sequential and guessable);
a foreign or unknown id is refused, not actioned. Surfaces are created **invisible**;
both backings are zeroed DMA buffers with `stride = w*4` (32-bit pixels). On
`commit`, `SF-FRONT` selects which backing the compositor reads — the client draws
into the *other* one — so a half-drawn frame is never composited.

### Input events (primary mailbox)

`coreinput` forwards every event to the subscribed compositor as `(input ev)`. The
compositor routes by policy (each event arity/type-guarded so a malformed event
falls through rather than killing the root):

```scheme
(input (key code pressed?))      ; -> delivered to the globally focused window's
                                 ;    client as (input (key code pressed?))
(input (pointer x y down?))      ; title-strip press (< TITLE-H = 28px) -> MOVE (drag);
                                 ;    body press -> client, in window-local coords
```

`down?` must be a boolean (`#t`/`#f`): Scheme treats integer `0` as true, so a
numeric button state would misread a release as a press. Focus follows the click
(an owner window raises). Keyboard routes to the max-z visible window across the
owner's own surfaces **and** every shard's reported windows, so a window hosted on
another core still receives keys.

## Multi-core sharding

The owner and shards form a **mesh**. The privileged inter-instance verbs all live
on an instance's primary mailbox — the same handle a semi-trusted client holds — so
each is authenticated by a **shard-mesh key**: the rendezvous ctx, an unforgeable,
identity-passed value injected (as the `key` cap) into the owner and every shard but
**no** client. Each verb carries the key as its first field and the receiver checks
it **fail-closed** (a `#f` key — the host harness with no mesh — rejects every keyed
verb rather than matching a forged `#f`).

| Verb | Direction | Meaning |
|---|---|---|
| `(register-shard key shard colour-grant z-grant)` | shard → owner | owner maps the layer grants and folds them into the merge |
| `(layer-update key shard window-list damage)` | shard → owner | shard repainted: record its window manifest (for input routing) and flush only the reported `damage` rects |
| `(alloc-z key reply surf-id)` | shard → owner | request a fresh global z for `surf-id` (the owner is the z authority) |
| `(z key N surf-id)` | owner → shard | apply global z `N` to `surf-id` and re-merge |
| `(move-window key id x y)` | owner → shard | relay a cross-shard title-bar drag: reposition + re-merge |

**Global z authority.** The owner holds the single monotonic z counter; every window
(its own + every shard's) draws from it, so z is globally comparable — true global
stacking, not static per-shard bands. The owner stamps its own windows
synchronously; a shard stamps a temporary local z, asks `alloc-z`, and snaps to the
returned global z (the surface id is the async correlator).

**Merge + damage bounding.** A shard builds its opaque `(colour, z)` layer then sends
`layer-update`; the owner folds every shard layer plus its own into the scanout with
the order-independent z-pick, alpha-overs its translucent windows above `Zop`, and
flushes only the union of the shards' reported damage rects (sanitised + clamped).
Translucent clients are all owner-hosted, so every shard layer is pure-opaque and the
merge is correct by construction.

**Cross-shard input.** Each shard reports its visible windows' geometry+client+z+id
in every `layer-update`; the owner builds one global window list (its own surfaces +
every shard's manifest) and hit-tests/focuses over it, relaying a title-bar drag on a
shard window back to the hosting shard via `move-window`.

## Exported functions

### `(make-compositor-caps alloc mint revoke present map key)`

Packs the six injected capabilities into a bundle consumed by
`start-compositor-service`. `present` and `map` may be `#f` (a RAM/headless owner, or
a shard whose owner does the flush/map); `key` is `#f` only in the host harness (no
mesh). See [Initialization](#initialization) for each argument.

### `(start-compositor-service screen caps shard-cfg)`

Starts the PRIMARY mailbox loop and returns the instance context. `shard-cfg` `#f`
makes it the **owner** (owns the scanout, the merge, routing, and the z authority);
a `make-shard-cfg` makes it a **shard**. The two share the whole serve loop and
surface machinery and differ only in how `recomposite` finalises (a shard builds its
grant-shared layer then `layer-update`s the owner; the owner merges + flushes) and
how `connect` routes. On entry the owner paints the desktop background
(`rgb 28 30 44`) and flushes the full screen so the backdrop shows before any client.

### `(make-shard-cfg owner lc lz z-base)`

Builds the shard configuration passed as `start-compositor-service`'s `shard-cfg`:
the `owner` handle, the grant-shared `(colour, z)` layer surfaces (`lc`/`lz`) the
shard composites into and the owner maps, and a `z-base` seed (0 — real z comes from
the owner's global counter via `alloc-z`).

### `(paint-windows screen bg windows)`

Pure (no IPC, no caps): clears `screen` to `bg`, then blits each `(src x y alpha?)`
spec back-to-front (painter's algorithm), `blit-alpha` for alpha windows else `blit`.
Retained as the host-testable compositing primitive; the live `recomposite` uses the
z-buffer merge above.

## Notes / gotchas

**Ownership by sender identity, not message content.** Every id-bearing op checks the
surface belongs to the *sender context* (`SF-CLIENT eq? client`), not just that the
id exists — ids are sequential and guessable.

**Arity guards (`len>=`).** Every op and inter-instance verb validates field count
with a safe `len>=` walk before touching fields (never `(cdr '())`, which would abort
the loop). A truncated or malformed message falls through to the `else` branch rather
than killing the root — the VM has no try/catch. Cross-core message data (a shard's
window manifest, its damage rects) is additionally **sanitised** on the way in.

**Fail-closed mesh-key check (`keyed?`).** The single guard shared by every privileged
verb is `(and (mrg-key mrg) (eq? (cadr m) (mrg-key mrg)))` — the truthy check precedes
the `eq?`, so a `#f` key cannot be matched by a forged `#f` field. Owner-only verbs
additionally gate on the role, mirroring the z guard.

**`MAX-DIM = 4096`.** `create-surface` rejects `w`/`h` outside `(0, 4096]`: a
non-integer would reach `(* w 4)` (type error, kills the root) and a huge integer
would fail `dma-alloc-wb` in an unguarded path.

**Use-after-revoke is neutralised in software.** `destroy-surface` revokes both
grants; a client's still-mapped view then reads as a zero page and refuses writes
(the grant table re-validates every bytes/`gfx-*` access). See the grant substrate
notes.

**Full recomposite, bounded present.** Recompositing repaints the whole back-buffer
(cheap cached RAM); only the damaged rects are pushed via `present` (everything else
on the display is already correct), so the flush to hardware is bounded.

**Channel layout must agree end-to-end.** The advertised `(r-off g-off b-off)` must be
used by the client (packing), the compositor's `surf-src` (the source it blits), and
the screen. `gfx-blit!` is a raw copy; a mismatch swaps red/blue.

**virtio-gpu present is blocking.** The owner's virtio-gpu `present` sends
`flush-rects` then `recv`s the ack — backpressure that paces frames to the device, at
the cost of one flush round-trip per compositing pass inside the serve loop. The boot
framebuffer present is non-blocking (WC copy, no ack).

**`probe-pixel` ordering.** Routing it through the handler (not the primary mailbox)
is a correctness requirement: the handler mailbox is FIFO, so a `probe-pixel` after a
`commit` is processed after that commit reaches the root.
