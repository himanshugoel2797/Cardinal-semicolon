# Capabilities & the sandbox

*How isolation and authority work in Cardinal; — the mental model behind `sys-*` modules, `spawn-restricted`, and grants.*

---

## Userspace is a Lisp context, not a ring-3 process

The first thing to correct before going further: Cardinal; has no ring-3. There
is no `syscall` instruction boundary, no user/kernel privilege switch, and no
per-process page table. This is not a gap in the design — it is the design.

The **Lisp VM** (`libs/lisp`) is itself the userspace boundary. A "userspace"
program is a capability-gated Lisp context: an independent execution unit with
its own heap and its own grant set, run cooperatively by the kernel-resident
scheduler. The Lisp process model *replaces* hardware rings for everything above
the `Sys*` core — so a context is never a "ring-3 process," and the absence of
ring-3 is a deliberate substitution, not a missing feature.

Why does this work? Because the VM enforces what the MMU normally would:

- **No raw pointers.** Lisp values carry type tags; the runtime is the only
  code that converts to native pointers, and it does so only for trusted,
  registered operations. A context running Lisp code simply cannot fabricate
  an address.
- **Shared-nothing heaps.** Each context allocates from its own `lisp_heap_t`.
  Sending a value to another context deep-copies the data into the receiver's
  heap. No mutable structure crosses the boundary.
- **Capability gating at the language surface.** Dangerous operations — MMIO
  mapping, port I/O, PCI enumeration, IRQ registration — are not global
  builtins. They are exported by named built-in modules (`sys-mmio`, `sys-io`,
  …) that a context can access only if it was granted permission to import them.

The trade-off is explicit in the design notes (`notes/core/lisp-substrate.md`):
*"isolation is only as strong as the runtime's correctness."* The hardware MMU
is not a second line of defence for Lisp contexts; the runtime itself is the
trusted computing base for isolation. The `Sys*` native modules — where bugs
could corrupt anything — are signed and kernel-verified precisely because they
are outside that managed layer.

---

## Capabilities as importable modules

The hardware-access primitives in `modules/SysLisp/src/main.c` are registered as
named **built-in modules** via `lisp_register_builtin_module`. The full set, as
of the current codebase:

| Module | Authority |
|---|---|
| `sys-io` | x86 port I/O (`in-u8`, `out-u8`, …) |
| `sys-mmio` | MMIO and DMA (`mmio-map`, `dma-alloc`, `bytes-phys`, …) |
| `sys-pci` | PCI enumeration (`pci-find`, `pci-find-all`, …) |
| `sys-irq` | Interrupt registration (`irq-register`, `irq-wait`, …) |
| `sys-reg` | Hardware/config registry reads |
| `sys-initrd` | Read files from the boot initrd |
| `sys-cmdline` | Kernel command line query |
| `sys-shm` | Map a shared-memory grant (`map-grant`) |
| `sys-shm-mint` | Mint and revoke grants (`grant-mint`, `grant-revoke`) |
| `sys-debug` | Reflective context inspection (`ctx-list`, `ctx-pause`, …) |
| `sys-console` | Serial REPL I/O + `repl-eval` |

Each of these is an ordinary module entry in the registry. `(import sys-mmio)`
resolves it from the registry and binds its exports — `mmio-map`, `dma-alloc`,
and so on — into the calling environment. A context that never imports `sys-mmio`
cannot name `mmio-map` at all. The name does not exist in that environment.

### The import gate

The capability gate lives in `libs/lisp/src/module.c`, in `lisp_module_import`.
When the running context is **restricted** (its `caps` field is a list rather
than `LISP_UNDEF`), `import` checks two things before binding anything:

1. The requested module name must appear in the context's `caps` list.
2. The module must already be in the registry. A restricted context cannot
   trigger loading new source code — that would evaluate an arbitrary module
   body under the sandbox.

A restricted context also cannot execute `define-module` at all: creating a new
module could shadow a `sys-*` built-in in the registry, which is a root-only
operation.

An unrestricted context (one that booted from the kernel's root path, such as
`init`) behaves exactly as before — no gate applies, no change.

---

## `spawn-restricted` and the grant operation

A new context is created with `spawn` or `spawn-restricted`. The difference is
the capability set that travels with the child.

```scheme
;; A root spawner; the child inherits root (no restriction):
(spawn thunk)

;; A restricted child with an explicit grant:
(spawn-restricted '(sys-mmio sys-irq) thunk)

;; A child with an empty grant — can compute and message, no hardware access:
(spawn-restricted '() thunk)
```

The no-escalation rule is enforced in `sched.c` (`prim_spawn_restricted`): every
capability in the requested list must appear in the *spawner's own* grant. An
unrestricted root spawner may grant anything; a restricted spawner is bounded by
its own set. A sandbox cannot promote itself by spawning an unconstrained worker.

`spawn` (without `restricted`) propagates the parent's restriction to the child
unchanged. A restricted context calling `spawn` yields a child with the same
grant — not an unrestricted one. The only way to *narrow* the grant is
`spawn-restricted`.

### The boot posture in `lisp/init.clp`

`init` is loaded at root authority. Its top-level `(import ps2 virtio-net
sys-mmio sys-pci …)` succeeds because the boot context is unrestricted; each
imported driver module captures exactly the `sys-*` primitives it needs into
its own closures at load time.

The long-lived service contexts `init` then spawns get an empty grant:

```scheme
(define (setup-input)
  (let ((input (start-input-service)))
    (ps2-init)
    (spawn-restricted '()          ; keyboard pump needs no import authority
      (lambda () (ps2-keyboard-driver input)))
    input))
```

The driver lambda already has everything it needs because the `ps2` module
captured `sys-io` and `sys-irq` primitives lexically at the time `(import ps2)`
ran in the root context. The empty grant on the spawned context just means that
if the driver loop is ever wedged or compromised, it cannot call `(import
sys-pci)` to reach new hardware — the gate would reject it. The work it was
originally authorised to do is already in its closure.

---

## Capability injection: closures instead of raw capabilities

The cleanest expression of least-privilege in this system is **handing a context
a closure over a capability**, rather than granting the capability itself. The
compositor bring-up in `lisp/init.clp` is the concrete example:

```scheme
(spawn-restricted '(sys-shm)
  (lambda ()
    (let* ((target (compositor-gpu-target gpu))     ; maps the scanout; init holds sys-mmio
           (screen (car target))
           (present (cdr target))                   ; a closure: (lambda (rects) ...) wrapping
                                                    ; the virtio-gpu flush + ack recv
           (caps (make-compositor-caps dma-alloc-wb  ; these are VALUES, captured from init's
                                       grant-mint     ; scope -- not module names the
                                       grant-revoke   ; compositor could re-import
                                       present))
           (comp (start-compositor-service screen caps)))
      ...)))
```

The compositor context is `spawn-restricted '(sys-shm)`. It does not hold
`sys-mmio` or `sys-shm-mint` in its grant. It operates exclusively through the
specific function values `init` injected:

- `dma-alloc-wb` — allocate write-back DMA buffers for surface backings.
- `grant-mint` / `grant-revoke` — create and tear down shared-memory grants.
- `present` — push composited damage to the display (a closure wrapping either
  a virtio-gpu `flush-rects` round-trip or a WC-framebuffer copy, depending on
  which display is present).

The compositor can call these functions but cannot look inside them, cannot call
`mmio-map` with an arbitrary address, and cannot mint a grant outside what `init`
delegated. The injected closure is a capability at the function level.

The `'(sys-shm)` in the compositor's grant serves a different purpose: not for
the compositor to use `(import sys-shm)` itself, but so it can *delegate* `sys-shm`
to the window client contexts it spawns. `spawn-restricted` refuses to grant a
capability the spawner lacks, so the compositor must hold `sys-shm` in order to
hand it to clients:

```scheme
;; A window client: can only map the specific memory it was granted.
(spawn-restricted '(sys-shm)
  (lambda ()
    (import sys-shm)          ; map-grant only — no mmio-map, no dma-alloc
    (send comp (list 'connect #f (self)))
    (let ((r (recv)))
      (let* ((g0 (caddr (cadr r))))         ; grant handle from the compositor
        (make-surface (map-grant g0) sw sh stride)))))
```

A client with only `sys-shm` can map the exact physical region the compositor
granted it, and nothing else. It never names a physical address.

---

## Grants: shared memory without shared authority

The compositor needs to give a client zero-copy access to a surface backing
buffer without handing the client raw MMIO authority. Grants are the mechanism
(`libs/lisp/src/grant.c`).

A grant is an unforgeable, revocable token: an opaque object holding an index
into a fixed table (`LISP_GRANT_MAX = 1024` slots) and a generation counter.
The operations are:

```c
// Mint a grant over a physical region (perms: 0 = read-only, 1 = read-write).
lisp_value lisp_grant_mint(uint64_t phys, size_t len, uint32_t perms);

// Resolve a grant handle to its region (fails if revoked or stale).
int lisp_grant_resolve(lisp_value g, uint64_t *phys, size_t *len, uint32_t *perms);

// Revoke a grant; future resolves fail.
int lisp_grant_revoke(lisp_value g);
```

In Lisp these surface as `grant-mint`, `grant-revoke` (in `sys-shm-mint`), and
`map-grant` (in `sys-shm`, which calls `prim_map_grant` → `lisp_grant_resolve`
→ maps the region into the caller's address space).

**Why a table indexed by `(index, generation)` rather than embedding
`phys`/`len` in the grant object?** Revocation. The grantee holds its own
reference to the grant object, so revoking it cannot reach into the grantee's
heap to null it out. The table is the single authority for liveness: `revoke`
sets the slot dead, and any subsequent `resolve` fails the generation check. A
later `mint` that reuses the slot bumps the generation, so a stale handle cannot
map a new occupant.

**Grants pass by identity, not by copy.** The deep-copy performed by `send`
(see `sched.c`, `deep_copy`) treats a grant object the same as a context handle:
it returns the grant unchanged, passing the reference. Copying it would be
meaningless — the table slot is the authority, not the object's bytes — and
would break revocation (a copy would hold the same index and generation but
would not be reachable from the revoke call).

### The split capability

The compositor's write authority (`sys-shm-mint`, holding `grant-mint` and
`grant-revoke`) is a separate module from the client's read-only map authority
(`sys-shm`, holding `map-grant`). A context spawned with only `sys-shm` cannot
mint new grants or revoke existing ones. This split is enforced at the
`import` gate: a context granted `sys-shm` but not `sys-shm-mint` is refused
`(import sys-shm-mint)` outright.

---

## The signing boundary: where the sandbox ends

Everything described so far — capability gating, grants, copy-on-send isolation
— operates inside the Lisp VM layer. The `Sys*` native modules that provide the
underlying kernel services (`SysMemory`, `SysInterrupts`, `SysTaskMgr`,
`SysLisp` itself) are a different tier entirely.

Each `Sys*` module is a **signed ELF** wrapped with an HMAC-SHA256
`ModuleHeader` by the `sign_exec` host tool (using `KMOD_HMAC_Key.txt`). The
kernel verifies that HMAC before loading. A module that fails verification is
not loaded. This is the trust boundary for kernel-privilege code.

The Lisp OS runs **on top of** this verified base. The `SysLisp` module links
the `libs/lisp` runtime, registers the `sys-*` built-in modules, and enters the
per-core scheduler loop. From that point on, everything above is Lisp: the
`Core*` services, the hardware drivers, the window compositor — each a
capability-gated context, isolated by the VM.

The chain of trust therefore has two distinct links:
1. **Module signing**: the kernel only loads signed `Sys*` modules; a tampered
   module is rejected before it runs.
2. **Capability gating**: once inside the Lisp OS, a context reaches hardware
   only through the `sys-*` modules in its grant; the VM enforces this at every
   `import`.

See [system-overview.md](system-overview.md) for the module loading and signing
story in detail.

---

## Gotchas

### An unexported name kills a spawned context silently

When a context tries to call a name that was not exported by its imported
module — or was never imported at all — the lookup fails and the context
terminates with an error, silently if nothing is watching for it. There is no
try/catch. This is the most common way a new driver or server dies unexpectedly
after a `spawn-restricted`. Verify exports and import lists carefully; use
the serial REPL (`sys-debug`'s `ctx-list`) to inspect the scheduler queue for
missing contexts.

### Data is copied on send; handles travel by identity

`send` deep-copies Lisp data into the receiver's heap. Strings, pairs, vectors,
bytes, and hash tables are all rebuilt in the target context. Two things are
*not* copied and instead pass by identity:

- **Context handles** (`spawn` and `self` return these): they are actor
  identities, akin to Erlang PIDs. A client passing `(self)` in a request so
  the server can reply back is exactly this pattern.
- **Grant handles** (`grant-mint` returns these): the table slot is the
  authority; copying the object would be meaningless.

An important consequence for framebuffer or DMA handles: sending `bytes` backed
by an MMIO region copies the *current contents* of that memory into the
receiver's heap — not the MMIO mapping. Device handles are not shared across
`send`. If a context needs to compose into a live framebuffer, either the mapping
must live in that context's own closure (captured from `init`, which held the
`sys-mmio` authority at mapping time), or the compositor surface grant pattern
(above) is the right tool.

### `spawn-restricted '()` is the normal long-lived service posture

The empty grant is not a deprivation — it is the intended steady-state for a
driver or server loop that already captured its authority at load time. The
`ps2-keyboard-driver`, the audio encoder context, the USB class driver loops:
all run with `'()`. They cannot import anything new, which is exactly the
point. The authority they need was baked into their closures when `init` ran
`(import ps2)` as root.

---

## See also

- [system-overview.md](system-overview.md) — the microkernel, signed modules,
  and the Lisp OS layer.
- [message-passing.md](message-passing.md) — `send`/`recv`, the scheduler, and
  the copy-on-send contract in depth.
- [Lisp VM reference](../vm/index.md) — the full Lisp VM primitive surface, including
  `spawn`, `spawn-restricted`, `capabilities`, `send`, `recv`, and `self`.
- [../servers/corecompositor.md](../servers/corecompositor.md) — how the
  compositor uses grants, capability injection, and the `sys-shm` / `sys-shm-mint`
  split in practice.
