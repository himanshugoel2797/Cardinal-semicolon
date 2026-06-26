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

## Surface model & protocol

Per-surface **double-buffered** (chosen): the grant covers *two* backing buffers;
`commit` names the ready one, the compositor reads only the committed-front, so a
client drawing into the back buffer never produces a torn frame. Costs 2×
surface RAM.

| message | reply | meaning |
|---|---|---|
| `(create-surface w h reply)` | `(id grant stride)` | alloc 2 backings, mint grant over both |
| `(configure id x y z visible)` | — | placement + z-order |
| `(commit id buf rects)` | — | `buf` (0/1) is ready; these rects changed → recomposite |
| `(destroy-surface id reply)` | `'ok` | revoke grant, free backings, repaint exposed area |

Client flow: `create-surface` → `map-grant` → draw with `graphics.clp` into the
back buffer → `commit`. Wayland-ish `wl_surface.commit` shape.

## Compositing

On each `commit`, for the union of dirty screen rects:
1. Walk the window list **top→bottom**; for each visible window intersecting the
   rect, `gfx-blit!` (opaque) or `gfx-blend!` (alpha) its committed-front into the
   screen back-buffer, clipped to the rect and to higher windows' occlusion.
2. Accumulate the touched screen region into the screen damage list.
3. `sfence` + `flush-rects` the screen damage to the driver, then clear it.

Reuses the screen-side double-buffer + damage helpers from `graphics.clp`.

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
2. **corecompositor.clp** — window list, create/configure/commit/destroy, the
   composite-on-commit loop over `graphics.clp`. Single client first.
3. **Driver seam** — own the scanout via `get-framebuffer`, drive `flush-rects`.
4. **Two-client demo** — generalize `init.clp`'s proto-compositor into two client
   contexts drawing independent windows; screenshot validation.
5. **(v2)** input routing (compositor as `coreinput`→focused-window router),
   move/resize, zero-page revoke hardening.

## Open / deferred

- Input routing deferred to v2 (compositor is the natural focus owner).
- No vsync on virtio-gpu (explicit flush) — fine; pacing is per-commit.
- QEMU can't show the WC win (trapped-VRAM dirty-tracking, see
  `notes/AUDIT.md` / the gfx bench in `init.clp`); virtio-gpu path is the
  representative one under the emulator.
