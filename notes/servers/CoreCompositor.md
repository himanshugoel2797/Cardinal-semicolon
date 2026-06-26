# CoreCompositor — multi-client window compositor

Status: **design** (no code yet). Branch `claude/corecompositor-design`.

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

**Sharp edge — use-after-revoke.** On `destroy-surface` the compositor calls
`grant-revoke` (future maps fail) but a client that already mapped the view still
has a raw pointer into that RAM. If the backing is freed, a late client write is
a UAF into reused RAM. v1 contract: **client must not touch a surface after it
acks `destroy`**, and the compositor frees the backing only after the client acks
(or after a bounded grace quantum). A hardening follow-up can remap revoked
client views onto a shared zero page so a late access faults harmlessly. Track in
`notes/AUDIT.md` when implemented.

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

Per-surface **double-buffered** (chosen): the grant covers *two* backing buffers;
`commit` names the ready one, the compositor reads only the committed-front, so a
client drawing into the back buffer never produces a torn frame. Costs 2×
surface RAM.

| message | reply | meaning |
|---|---|---|
| `(connect reply)` *(primary)* | `(connected handler)` | spawn per-client handler, return its handle |
| `(create-surface w h reply)` | `(surface id grant stride)` | alloc 2 backings, mint grant over both |
| `(configure id x y z visible)` | — | placement + z-order |
| `(commit id buf rects)` | — | `buf` (0/1) is ready; these rects changed → recomposite |
| `(destroy-surface id reply)` | `'ok` | revoke grant, free backings, repaint exposed area |
| `(input …)` *(v2, server→client)* | — | tagged input event delivered on the secondary mailbox |

Client flow: `connect` (once, on primary) → `create-surface` → `map-grant` →
draw with `graphics.clp` into the back buffer → `commit`. Wayland-ish
`wl_surface.commit` shape.

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

### Client sharding

A client `connect`s to the well-known **owner**; the owner assigns it to the
least-loaded instance and replies with that instance's handle (the per-client
secondary channel, now possibly on another core). That instance owns the client's
mailbox, surfaces, grants, and damage, and composites the client's windows into
**its own layer**. This is the per-connection-handler model above, promoted to
per-core.

### Layers + the merge

- Each instance composites its clients' windows into a **layer**: a grant-shared
  buffer carrying per pixel `(color, z)` — `z` is the window's global z-key
  (empty where the layer has no content). Damage-bounded.
- The **owner** maps all N layers (read, WB, via grant — the same substrate as
  client surfaces) and merges them into the scanout by **per-pixel topmost-z
  pick** (a z-buffer composite), over the union of the layers' damaged rects, then
  flushes those rects through the driver.
- Cost: N layer buffers + a z-plane each; merge is O(damaged-area · N) on one
  core, not O(clients · screen).

### The z-correctness constraint (honest tradeoff)

A flat "alpha-over the layers in order" merge is **wrong** when windows from two
shards overlap with interleaved z. The z-buffer pick fixes this **for opaque
windows** (pick the max-z contributor per pixel — correct regardless of
cross-shard z interleaving). It does **not** generalize to translucency, because
alpha-over is order-dependent, not a pick. So the rule: **translucent windows
that overlap a window owned by another shard must be co-assigned to the same
shard** (or accept artifacts). Most windows are opaque; translucency is the
menu/shadow edge case, and co-assignment is a cheap assignment constraint.

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

1. **VM substrate** — `grant` value type, grant table, `map-grant` /
   `grant-mint` / `grant-revoke` prims, `sys-shm` capability module. Host + SysTest
   coverage for mint/map/revoke/UAF-contract. *(touches `libs/lisp` — gated on
   sign-off, done.)*
2. **corecompositor.clp root** — the primary mailbox + `connect` handshake that
   spawns a **per-client handler context**; the handler holds that client's
   window list, grants, and damage. Single client first.
3. **Surface ops + composite loop** — create/configure/commit/destroy on the
   secondary channel, the composite-on-commit loop over `graphics.clp`, screen
   owner serialising the scanout.
4. **Driver seam** — own the scanout via `get-framebuffer`, drive `flush-rects`.
5. **Two-client demo** — generalize `init.clp`'s proto-compositor into two client
   contexts (two handlers) drawing independent windows; screenshot validation.
6. **(v2)** input routing (tagged `(input …)` on the secondary mailbox; compositor
   as `coreinput`→focused-window router), move/resize, zero-page revoke hardening.
7. **(scaling, separable) Sharded per-core instances** — a per-core bring-up hook
   spawning one instance per core; client assignment + global z authority on the
   owner; grant-shared `(color,z)` layer buffers; z-buffer merge on the owner over
   union damage. v1's single instance is the N=1 case of this code.

## Open / deferred

- Input routing deferred to v2 (compositor is the natural focus owner). Events
  arrive tagged on the secondary mailbox the client already drives — no third
  channel needed.
- Multi-core scaling uses **per-core-born instances + two-level layer merge**
  (phase 7) — no scheduler migration. v1 is the single-instance N=1 case and does
  not depend on parallelism.
- Translucent windows overlapping another shard's window must be co-assigned to
  one shard (z-buffer merge is opaque-correct only); pure-opaque overlap is
  always correct.
- No vsync on virtio-gpu (explicit flush) — fine; pacing is per-commit.
- QEMU can't show the WC win (trapped-VRAM dirty-tracking, see
  `notes/AUDIT.md` / the gfx bench in `init.clp`); virtio-gpu path is the
  representative one under the emulator.
