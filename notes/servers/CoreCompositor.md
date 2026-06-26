# CoreCompositor — multi-client window compositor

Status: **phases 1–4 implemented** (grant substrate + IPC root + surface protocol
& compositing + the driver seam: the compositor now owns the real scanout and a
window appears on the display). Branch `claude/corecompositor-design`.

A new `Core*` Lisp server that owns the screen and composites off-screen
surfaces owned by independent client contexts, using **zero-copy shared-memory
surfaces**. This is the "real display server" layer above `CoreDisplay`/the
display drivers.

## Why this is mostly already possible

Cardinal contexts are shared-nothing with copy-on-send `bytes` (`sched.c`,
`LISP_OBJ_BYTES` deep-copies into the receiver's heap). That would make a naive
"send the framebuffer" compositor copy every surface every frame. But the
display stack already sidesteps this by sharing pixel memory **by physical
address**:

- A producer allocates a WB/WC DMA buffer; its **phys** is just an integer and
  crosses a `send` fine (the `bytes` shadow is a useless copy, the phys is not).
- The consumer re-maps that phys in *its own* context with `mmio-map-wb`/`-wc`
  and now both contexts touch the **same physical pages**.
  - `lisp/init.clp:315` maps the virtio-gpu scanout backing WB to compose into.
  - `lisp/init.clp:221`,`:246`,`:257` map the same scanout WC + UC for flush.
- `mmio-map*` is **already capability-gated**: a context that wasn't granted the
  authority cannot even *name* it (`libs/lisp/src/module.c:417`,
  `lisp/init.clp:205` — "init holds the sys-mmio/sys-reg authority to map it").

So a multi-client compositor needs **no** new address-space machinery, no
page-table sharing, no shared-heap value type. It needs the
*allocate → share-phys → map-on-both-sides* pattern generalized to N clients,
plus **one** security refinement (below).

## The one interpreter/VM change: a scoped shm grant

`mmio-map*` maps **arbitrary** physical addresses — correct for trusted drivers
and `init`, wrong for a semi-trusted window client (it could map all of RAM).
The substrate change is therefore not "add shared memory" but **"add a *scoped*
mapping capability"**:

- **New value type — `grant`.** Holds an index into a small kernel-resident
  grant table; the grant object is passed **by identity** on `send` (like
  `LISP_OBJ_CTX`, *not* deep-copied), so it cannot be forged from integers — only
  a context that was explicitly handed the grant holds a reference.
- **Kernel grant table entry:** `{ id, owner_ctx, phys, len, perms, refcount,
  revoked }`, allocated in the system heap.
- **New prim — `(map-grant g)`** (capability `sys-shm`): validates `g` is live
  and not revoked, then returns a foreign (`owned=0`) `bytes` view over exactly
  `[phys, phys+len)` with `perms`. Nothing else is mappable.
- **`(grant-revoke g)`** (compositor only): marks the entry revoked so future
  `map-grant` fails.

A window client's capability set gets `sys-shm` (just `map-grant`) and **not**
`sys-mmio` (arbitrary phys). It can map only what it was granted. This is the
whole VM-touching surface: one value type, two prims (`map-grant`,
`grant-revoke`; `grant-mint` is compositor-internal), one kernel table.

### Lifetime / revocation / the sharp edge

The surface backing is `dma-alloc-wb`'d and **owned by the compositor**; the
client's mapped view is a foreign `bytes` (never GC-frees the backing). The grant
table holds a reference that keeps the backing alive while `refcount > 0`
(compositor + each mapper).

**Sharp edge — use-after-revoke (HARDENED, phase 6).** On `destroy-surface` the
compositor calls `grant-revoke`. A client that already mapped the view still holds
the `bytes` object, but it is now neutralized: `map-grant` stamps the grant's
`(index, generation)` onto the view, and every bytes/`gfx-*` accessor re-validates
it against the grant table on use, so a revoked view **reads as a zero page and
refuses writes**. A late use-after-revoke therefore can neither read the (reused)
RAM nor corrupt it — the actual UAF danger is closed in software (the page table
can't do it: `map-grant` returns the shared physmap window, not a private mapping,
which is also why read-only is software-enforced). The v1 ordering contract
(*client should stop touching a surface after it acks `destroy`*) still holds as
hygiene, but is no longer load-bearing for safety. See `notes/AUDIT.md`
("[DONE] Use-after-revoke neutralized in software").

## Architecture

```
 client ctx (e.g. mana)        corecompositor ctx           display driver ctx
 ─────────────────────         ──────────────────           ──────────────────
 graphics.clp draws  ──┐       owns screen back-buffer       virtio-gpu / lfb
 into mapped surface   │       window list (z, geom,         (flush-rects — the
 (zero-copy)           │        grant, damage, dbl-buf)        already-merged path)
                       │       composites visible rects
 (commit id rects) ───►│──────► blit/blend into back ───────► (flush-rects screen-damage)
 ◄─── grant ───────────┘        via gfx-blit!/gfx-blend!
```

The compositor is mostly **orchestration over code that already exists**:
`lisp/lib/graphics.clp`'s `gfx-blit!` / `gfx-blend!` / double-buffer / damage
machinery, and the driver's `flush-rects` (one IPC round-trip for N rects). The
novel logic is z-ordered occlusion and folding per-client damage into screen
damage. `lisp/init.clp:200-330` is effectively a single-owner proto-compositor
(WC/WB scanout mapping + double-buffer + damage) to generalize.

## IPC model & wiring

Cardinal's VM is a **shared-nothing actor model** (`libs/lisp/src/sched.c`).
Every context is an actor with a single **FIFO mailbox**; the whole kernel IPC
surface is five primitives:

| prim | semantics |
|---|---|
| `(self)` | this context's handle |
| `(send target msg)` | **deep-copies** `msg` into `target`'s mailbox + wakes it. Cross-core safe (serialises on the receiver's heap lock). |
| `(recv)` | pop oldest message; `%block` until one arrives if empty. FIFO. |
| `(spawn t)` / `(spawn-restricted caps t)` | create a context, **return its handle** |
| `(yield)` | cooperative reschedule |

Two semantics drive the protocol design:

- **Messages deep-copy on send — except context handles (and our `grant`), which
  pass by *identity*.** So a request embeds `(self)` as its reply address, and a
  `grant` / a channel handle can be handed to a peer without being copied. This is
  exactly why `grant` is specced as an identity-passed value type.
- **One FIFO mailbox per context, no selective receive.** A context that
  multiplexes several conversations on its primary mailbox must demux them
  itself. We avoid that with secondary channels (below) and a tagged envelope.
- **No name service.** A server handle is the return value of
  `spawn`/`start-X-service`; `init.clp` holds it and hands it to clients by
  lexical closure. The compositor handle is distributed the same way — `init`
  passes it into the contexts it spawns. There is no global lookup.

### Message envelope

Every message is a list whose **head is a tag symbol**, so a context can demux a
mailbox that carries more than one kind of traffic (compositor replies *and*,
later, input events):

```
(tag . args)            ; e.g. (commit id buf rects), (input 'key ...), (connected ch)
```

Request/reply embeds the caller's handle as the last element:
`(verb args… reply-handle)`; the server replies with `(send reply-handle …)`.
Fire-and-forget omits the reply handle (used for the per-frame `commit`, so a
client never blocks presenting a frame).

### Secondary channels = per-connection handler contexts

A **mailbox is inseparable from a context**, so "open a separate mailbox after an
initial negotiation" is realised as "spawn a dedicated handler context and hand
the peer its handle." This single mechanism also *is* the per-client compositor
thread (see Threading). The handshake runs once over the well-known **primary**
mailbox; all subsequent per-client traffic flows on the **secondary** channel:

```
client                         compositor (primary)         per-client handler (secondary)
──────                         ────────────────────         ──────────────────────────────
(send comp (list 'connect (self)))
                               spawn handler ctx ──────────► owns this client's window list,
                               reply (connected handler)      grants, damage; its own mailbox
◄── (connected handler) ──────
;; from here the client talks ONLY to `handler`:
(send handler (list 'create-surface W H (self)))
◄── (surface id grant stride) ─────────────────────────────  mint grant, reply
(map-grant grant) ; zero-copy
(send handler (list 'commit id buf rects))  ; fire-and-forget per frame ──►
```

The handler owns one client's surfaces/grants/damage and forwards composited
damage to the single screen-owner (the compositor root, which serialises access
to the scanout + driver). This gives per-client isolation (a wedged client only
backs up its own handler's mailbox) and a clean home for per-client input
routing in v2 — input events arrive tagged on the same secondary mailbox the
client already drives.

## Surface protocol (on the secondary channel)

Per-surface **double-buffered** (chosen): a surface has *two* backings; `commit`
names the ready one (`buf` 0/1), the compositor reads only the committed-front, so
a client drawing into the back buffer never produces a torn frame. Costs 2×
surface RAM.

**Implemented as two grants (`grant0`/`grant1`), one per backing**, rather than one
grant over a contiguous 2× region. The latter would need the compositor and the
client to make a `graphics.clp` surface over the *back half* of a buffer, i.e. a
`bytes` sub-view at an offset — and no such slice primitive exists. Two separate
`dma-alloc-wb` backings are each a whole `bytes` that is directly a surface, so
double-buffering needs **no interpreter change**. The client maps both and draws
into the one it will commit.

Replies go to the client's **authenticated identity** (the context it `connect`ed
with, which the handler stamps on every relayed op), never to a reply field in the
message — so a client can't make the compositor `send` to an arbitrary or
non-context target. The `connect` `reply` is the one bootstrap handle, and it is
`ctx?`-validated before use.

| message | reply (→ connecting client) | meaning |
|---|---|---|
| `(connect transparency? reply)` *(primary)* | `(connected handler fmt)` | `reply` must be `ctx?`; route by `transparency?` (→ owner if set, else a shard), return the assigned instance's handle + the screen pixel `fmt` = `(r-off g-off b-off)` so the client draws into its granted backing in the display's channel layout (`gfx-blit!` is a raw copy — see phase 4) |
| `(create-surface w h)` *(via handler)* | `(surface id grant0 grant1 stride)` / `(surface-error …)` | validate `w,h ∈ [1,MAX-DIM]`, alloc 2 backings, mint a grant over each |
| `(configure id x y visible)` *(via handler)* | — | placement + show/hide → recomposite. (z is list-order; `raise` re-stacks — see below) |
| `(commit id buf rects)` *(via handler)* | — | `buf` (0/1) is the ready front → recomposite. Fire-and-forget |
| `(raise id)` *(via handler)* | — | move to top of the z-stack → recomposite |
| `(destroy-surface id)` *(via handler)* | `'ok` / `(destroy-error …)` | revoke both grants, drop, repaint exposed area |
| `(input …)` *(v2, server→client)* | — | tagged input event delivered on the secondary mailbox |
| `(layer-update shard rects)` *(shard→owner, multi-core)* | — | shard's layer dirtied these rects → owner bounds its merge+flush |

Every id-bearing op (`configure`/`commit`/`raise`/`destroy-surface`) acts **only on
a surface owned by the requesting client** (ownership is the stamped identity, since
ids are sequential and guessable); a foreign/unknown id is refused, not actioned.

Client flow: `connect` *(declares transparency)* → `create-surface` → `map-grant`
(both) → draw with `graphics.clp` into the back buffer → `commit`. Wayland-ish
`wl_surface.commit` shape. **All per-surface ops go via the client's handler**, so
they are FIFO-ordered on one channel (e.g. a `commit` is always processed before a
later op from the same client — a direct-to-root message could overtake it).

v1 deviations from the headline protocol, noted for honesty:
- **z** is implicit (surface-table order, newest on top) with an explicit `raise`,
  rather than an absolute `z` field in `configure`. The owner-authority z-stamp
  arrives with the sharded model (phase 7).
- **`commit` recomposites the whole screen** (clear + painter's pass over all
  visible surfaces) into the back-buffer, ignoring the client's `rects`. Simplest
  and obviously correct; the **flush** to the display *is* now bounded to the op's
  damage rect (phase 4), and bounding the recomposite (and honouring the client's
  per-commit `rects`) is a later refinement. The QEMU framebuffer path dominates
  cost regardless (see Open/deferred).

## Compositing

On each `commit`, for the union of dirty screen rects:
1. Walk the window list **top→bottom**; for each visible window intersecting the
   rect, `gfx-blit!` (opaque) or `gfx-blend!` (alpha) its committed-front into the
   screen back-buffer, clipped to the rect and to higher windows' occlusion.
2. Accumulate the touched screen region into the screen damage list.
3. `sfence` + `flush-rects` the screen damage to the driver, then clear it.

Reuses the screen-side double-buffer + damage helpers from `graphics.clp`.

## Threading & parallelism

A single context is **strictly single-threaded** — one CEK machine, cooperative
switching at safe points, no native preemption (this is *why* SSE/FP is safe in
the runtime). "Threads" are therefore separate **contexts**, and the runtime has
two layers (`modules/SysLisp/src/main.c`):

- **Per-core scheduler, true cross-core parallelism.** Every core runs its own
  `lisp_core_loop` over its own run queue simultaneously; `send` is cross-core
  safe (locks the receiver's heap, `sched.c:379`).
- **…but `spawn` only enqueues onto the *current* core** (`cur_sched()`). There
  is **no migration, work-stealing, or spawn-on-core** today — main.c:3001 is
  explicit: *"Long-lived OS services live on the BSP for now (cross-core
  messaging is a later step, so a service + its drivers must share one core)."*
  APs come up as live Lisp cores but currently run only a local proof-of-life
  context. **In practice all services run on the BSP.**

Implications for this design:

- **Per-client handler contexts work *now*** (the secondary-channel model above)
  — cooperative concurrency on the BSP. This is the structure we build to;
  isolation and clean per-client state are real even without parallelism.
- **Multi-core render fan-out is *data-ready but not schedule-ready*.** A render
  worker on any core can map the shared back-buffer **by phys grant** (mapping is
  core-agnostic), so tiling a frame across cores (each worker composites a
  disjoint scanline band into the shared back-buffer, then a barrier `send` to
  the screen owner) is embarrassingly parallel. The *only* missing piece is a
  scheduler-placement primitive.

The scaling model avoids scheduler migration entirely: instead of moving work to
other cores, run **one compositor instance per core**, born locally on that core,
and shard clients across them — see the next section. v1 is the single-instance
(N=1) degenerate case, so nothing is thrown away.

## Sharded per-core compositor (two-level layer merge)

Post-v1, the compositor scales as **one instance per core**, clients sharded
across them, the single screen produced by a **two-level layer merge**. Chosen
over region-partitioning because the shards stay independent (no cross-core
shared mutable window list). v1 is exactly N=1 of this model — build v1 to
generalize.

### Bootstrap: instances are born on their core (no migration)

`spawn` enqueues onto the current core, so each instance is spawned *by code
already running on that core*: the AP proof-of-life spawn (`main.c:3033`) becomes
"spawn this core's compositor instance." The only substrate addition is a
**per-core bring-up hook** (run a thunk as each core enters `lisp_core_loop`) —
far smaller than general migration / `spawn-on-core`. The scanout-owning instance
(core 0) additionally runs the screen setup in `(system-init)`.

### Client sharding & transparency routing

A client `connect`s to the well-known **owner** and declares in that initial
negotiation **whether it will use transparency** (`transparency?`). The owner
makes the routing decision from that flag:

- **Opaque clients → sharded** across instances (least-loaded). Reply carries the
  assigned instance's handle (the per-client secondary channel, possibly on
  another core).
- **Translucent clients → the owner itself.** All translucency is hosted on one
  instance; the reply hands back the owner's own handle.

The owner owns each assigned client's mailbox, surfaces, grants, and damage, and
composites that client's windows into **its instance's layer**. This is the
per-connection-handler model above, promoted to per-core. Per-client granularity
keeps the secondary channel simple (a client lives on exactly one instance); a
client that wants transparency on any window declares it and is owner-hosted.

### Layers + the merge

- Each instance composites its clients' windows into a **layer**: a grant-shared
  buffer carrying per pixel `(color, z)` — `z` is the window's global z-key
  (empty where the layer has no content). Because translucent clients are routed
  to the owner, **every shard layer is pure-opaque**.
- A shard tells the owner what changed with **`(layer-update shard rects)`** — the
  rects of its layer dirtied since the last update. The owner **unions these
  across shards** (plus its own translucent-window damage) to bound the merge +
  flush to actual damage; without it the owner would re-merge the whole screen
  each frame.
- The **owner** maps all N layers (read, WB, via grant — the same substrate as
  client surfaces) and produces the scanout in two passes over the union-damage
  region (next section).
- Cost: N opaque layer buffers + a z-plane each; merge is O(damage · N) on one
  core, not O(clients · screen).

### The merge is correct by construction (transparency centralised)

Routing all translucency to the owner removes the cross-shard ordering problem
entirely — there is no interleaved-alpha case across shards because shards never
hold alpha. The owner's per-frame merge, over the union-damage rects:

1. **Opaque pass — z-buffer pick.** Merge the N opaque shard layers (plus the
   owner's own opaque windows, if any) by taking the **max-z contributor per
   pixel**, yielding a flattened opaque image and a per-pixel topmost-opaque z
   `Zop`. Correct regardless of how opaque windows are sharded or how their z
   interleaves — a pick has no ordering dependence.
2. **Translucent pass — ordered alpha-over.** For each pixel, alpha-over the
   owner's translucent windows that cover it **with z > `Zop`** (those below the
   nearest opaque surface are occluded and skipped), back-to-front by z, onto the
   pass-1 color.

The owner holds *every* translucent window, so the one place ordering matters has
full z information in a single serial pass — the result is exactly correct. Cost
note: the owner carries all translucent windows + the merge + flush; translucent
windows are few on a normal desktop (menus, notifications, shadows), so this is
acceptable; rebalancing if the owner saturates is future work.

### Global z authority

Window z is a global value, so the **owner is the z authority**: raise/lower asks
the owner for a fresh top z-stamp (a monotonic counter). Light cross-instance
messaging, no shared mutable z-list.

### Why this composes

- Reuses **grant** shared memory (phase 1) for the layer buffers — no new sharing
  mechanism.
- Reuses the **secondary-channel / per-connection-handler** model — the handler
  just lives on the assigned core.
- Needs only a **per-core bring-up hook** + the layer/merge/z-authority protocol —
  no scheduler migration, no `spawn-on-core`.
- **v1 (N=1) is the same code with the merge as a no-op** — the single instance is
  both owner and the only shard.

## Placement

A new `corecompositor.clp` under `lisp/servers/`, brought up in `lisp/init.clp`
after `coredisplay` + the display driver bind (it needs the driver's
`get-framebuffer` phys to own the screen). It holds `sys-mmio` (map the scanout)
+ the new `sys-shm` mint authority; clients it spawns get only `sys-shm`.

## Phasing

1. **VM substrate** — ✅ **DONE.** `LISP_OBJ_GRANT` value type (`libs/lisp`,
   identity-passed on send, system-heap, GC leaf), portable grant table
   (`grant.c`: mint/resolve/revoke + generation invalidation), kernel prims
   `grant-mint`/`map-grant`/`grant-revoke`, split capabilities `sys-shm-mint`
   (owner) vs `sys-shm` (grantee). Host test (`test_grant.c`, 13 checks) + in-OS
   self-test (full vmem round-trip + capability-split + strict-perms + RO-write
   refused). `'ro` is the least-privilege **default** and is **enforced in
   software**: `map-grant` marks a `'ro` view read-only and every bytes/`gfx-*`
   mutator refuses to write it — airtight in the sandbox (a grantee reaches the
   region only through those prims). One tracked contract in `notes/AUDIT.md`:
   use-after-revoke (revoke invalidates future maps but doesn't tear down an
   existing mapping).
2. **corecompositor.clp root** — ✅ **DONE.** `lisp/servers/corecompositor.clp`:
   the well-known primary mailbox (`start-compositor-service`) + the `connect`
   handshake that spawns a **per-client handler context** (the secondary channel),
   replying `(connected handler)`. The handler threads per-client state
   (surfaces alist + next-id, empty in phase 2) over a FIFO loop and answers a
   liveness `(ping reply)` → `(pong)`; surface verbs are stubbed for phase 3. The
   handler is `spawn-restricted '()` yet closes over the module's lexical imports;
   phase 3 will add `(import sys-shm-mint sys-mmio)` to the module clause so the
   handler captures the minting authority (those imports are absent in phase 2 — it
   mints nothing yet, and adding them would break the host test, since
   `sys-shm-mint` is kernel-only). Both `connect` and `ping` arity-guard their
   payload so a malformed message can't crash the serve/handler context. Brought
   up unconditionally in `init.clp`'s display block (owns no scanout yet, so it is
   inert until a client connects). Host test (`test_compositor.c`: connect → distinct
   handler, ping/pong, two clients isolated on independent handlers) + in-OS smoke
   (`cardinal.compositortest`).
3. **Surface ops + composite loop** — ✅ **DONE.** create-surface (2 backings + 2
   grants) / configure / commit / destroy-surface / raise on the secondary channel,
   recompositing into a screen back-buffer over `graphics.clp`'s `blit`/`blit-alpha`
   (a `paint-windows` painter's pass — back-to-front, so opaque occlusion is correct
   by construction). Two design decisions, both to avoid an interpreter change and
   keep the module host-testable:
   - **The root owns the global surface table + screen + compositing** (the single
     serialiser; cross-client occlusion is one painter's pass over one z-ordered
     list); the **per-client handler is a thin relay + isolation boundary** (its
     mailbox, its client identity for cleanup, the phase-6 input home). Phase 7
     moves the surface table onto each per-core instance — the protocol is unchanged.
   - **Capabilities are injected, not imported.** `corecompositor` imports only
     `driver-util graphics`; `init` (which holds the authority) passes
     `dma-alloc-wb` / `grant-mint` / `grant-revoke` into `start-compositor-service`
     by closure. This *is* Cardinal capability delegation and keeps the whole module
     loadable in the host harness. The injected prims survive the
     `spawn-restricted '()` root because they are captured lexically, not imported.

   Hardening (clients are semi-trusted from phase 5 on, and the VM has no
   try/catch, so an unvalidated message can permanently kill the root serve loop):
   create-surface bounds `w,h ∈ [1,MAX-DIM]` (a non-integer or huge value would
   else crash via `*`/`dma-alloc-wb`); id-bearing ops enforce per-client ownership;
   replies target the authenticated client, not a message field; and `connect`'s
   reply is `ctx?`-validated. The one new VM primitive — **`ctx?`** (`sched.c`, a
   type predicate like `pair?`) — lets a server validate a reply handle before
   `send`, since a send to a non-context aborts the caller. The residual
   valid-bounds OOM that still kills a serve loop is a *systemic* property of every
   `serve` service (no try/catch), not specific to the compositor.

   Tests: host `test_compositor` (paint-windows occlusion/position/background; the
   full create/configure/commit/destroy/raise protocol mechanics under fake caps;
   bad-dimension rejection; cross-client-authorization refusal via a separate
   attacker context; `ctx?` + non-context connect rejected while the service
   survives); in-OS `cardinal.compositortest` (the *real* grant path — a
   `sys-shm`-restricted client maps a granted backing, draws, commits, and the
   composited pixel is probed: `OK surface created, drawn, composited`).
4. **Driver seam** — ✅ **DONE.** The compositor now owns the **real scanout** and
   pushes composited damage to the display, so a window actually appears on screen
   (validated: `build/ISO/os-compositordemo.iso` boot, screenshot shows a titled
   window on the compositor desktop). Decisions:
   - **The driver seam is one injected `present` closure**, not a driver import.
     `make-compositor-caps` gains a 4th cap `(present rects)`; the compositor calls
     it after every recomposite with the changed `(x y w h)` rects. `init` builds it
     per backend: **virtio-gpu** → `mmio-map-wb` the driver's `get-framebuffer` phys
     (the coherent cached view the device reads — the modern path) and present =
     `sfence` + `(flush-rects rects)` over the controlq; **boot framebuffer** (lfb /
     `-vga std`) → compose into a `make-double-buffer` back-buffer and present =
     `sfence` + `db-flush-rect` (back→front WC copy) per rect; **headless** → `#f`
     (composite into RAM, display nothing — the phase-3 posture, kept for the
     self-test). The compositor stays import-only-`driver-util graphics` (host-loadable).
   - **Bounded flush.** `recomposite` still repaints the whole back-buffer (cheap
     cached RAM), but `present` flushes only the op's damage rect(s): configure flushes
     old∪new (a moved/hidden/shown window), commit/raise the surface rect, destroy the
     vacated rect. The whole-screen background is flushed once at startup. Everything
     not in a damage rect is already correct on the display (nothing else moved), so a
     bounded flush is correct.
   - **The compositor advertises the screen pixel format** (`(connected handler fmt)`,
     `fmt = (r-off g-off b-off)`). `gfx-blit!` is a RAW 32-bit pixel copy — it does not
     repack channels — so the client's granted backing, the compositor's `surf-src`,
     and the screen must share one layout. The boot framebuffer is 0xRRGGBB (16/8/0);
     the virtio-gpu **X8R8G8B8** scanout is 8/16/24. Clients draw with the advertised
     `fmt`; `surf-src` stamps the screen's offsets on every source. (Alpha windows on a
     non-0xRRGGBB scanout are a follow-up — `gfx-blend!` reads alpha from the top byte,
     which is `B` under X8R8G8B8; phase-4 windows are opaque.)
   - **Bring-up runs in a spawned context** holding `'(sys-shm)`: `get-framebuffer`
     blocks until the driver's async bring-up has a scanout (so `system-init` must not
     call it directly), and the context must **hold** `sys-shm` to *delegate* it to the
     window clients it spawns — `spawn-restricted` refuses to grant a capability the
     spawner lacks (the bring-up's own scanout-map / grant-mint are captured prims,
     which work in any context regardless of caps).
   - Demo: `cardinal.compositordemo` + the `compositordemo-image` CMake target
     (`grub_compositordemo.cfg`); a `'(sys-shm)` client connects, creates a 360×240
     surface, maps `g0`, draws a titled window, configures + commits → it composites
     onto the owned scanout. Capture with
     `ISO=build/ISO/os-compositordemo.iso GPU=virtio-vga SCREENSHOT=out.ppm ./scripts/run-qemu.sh`
     (`virtio-vga`, not `virtio`, so the virtio-gpu scanout is the *primary* display
     the monitor `screendump` captures).
5. **Two-client demo** — generalize `init.clp`'s proto-compositor into two client
   contexts (two handlers) drawing independent windows; screenshot validation.
6. **(v2)** input routing (tagged `(input …)` on the secondary mailbox; compositor
   as `coreinput`→focused-window router), move/resize, zero-page revoke hardening.
7. **(scaling, separable) Sharded per-core instances** — a per-core bring-up hook
   spawning one instance per core; `connect`-time transparency routing (opaque →
   shard, translucent → owner) + global z authority on the owner; grant-shared
   opaque `(color,z)` layer buffers with `layer-update` damage reports; two-pass
   merge on the owner (opaque z-buffer pick, then ordered alpha-over) bounded to
   union damage. v1's single instance is the N=1 case of this code.

## Open / deferred

- Input routing deferred to v2 (compositor is the natural focus owner). Events
  arrive tagged on the secondary mailbox the client already drives — no third
  channel needed.
- Multi-core scaling uses **per-core-born instances + two-level layer merge**
  (phase 7) — no scheduler migration. v1 is the single-instance N=1 case and does
  not depend on parallelism.
- Transparency is declared at `connect`; translucent clients are hosted on the
  owner so every shard layer is pure-opaque and the z-buffer merge is correct by
  construction (the owner alpha-overs its translucent windows above the merged
  opaque z in a final pass). No cross-shard alpha case exists.
- Shards report their dirtied rects to the owner (`layer-update`) so the merge +
  flush stay bounded to actual damage rather than re-merging the whole screen.
- No vsync on virtio-gpu (explicit flush) — fine; pacing is per-commit.
- QEMU can't show the WC win (trapped-VRAM dirty-tracking, see
  `notes/AUDIT.md` / the gfx bench in `init.clp`); virtio-gpu path is the
  representative one under the emulator.
