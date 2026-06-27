# Modules, capabilities & the `sys-*` surface

*Part of the [Lisp VM Reference](index.md).*

> How code is packaged (`define-module` / `import` / `include`), how the
> capability sandbox gates authority, and the `sys-*` modules that grant
> hardware access.

## Modules and capabilities

### `define-module`

```scheme
(define-module name
  (export sym ...)
  body ...)
```

Evaluates `body ...` in a fresh private environment parented on the global env,
then publishes the listed symbols as the module's exports.  **Root-context
only** — a restricted context cannot define modules (preventing authority
escalation).

```scheme
(define-module my-lib
  (export add square)
  (define (add a b) (+ a b))
  (define (square x) (* x x)))
```

### `import`

```scheme
(import name ...)
(import (name (only sym ...)) ...)
(import (name (prefix pfx)) ...)
```

Loads `name`'s source (via the module loader, which maps `name` to
`./lisp/<name>.clp` in the initrd) if not already loaded, then binds the
exports into the current environment.

- `(only sym ...)` — import only the listed names.
- `(prefix pfx)` — bind each export as `<pfx><name>`.

**Capability gating.** A restricted context (spawned with `spawn-restricted`)
may only import modules explicitly listed in its grant, and only ones already
loaded.  It cannot cause new source to be evaluated.  An unrestricted (root)
context may import anything.

```scheme
; Unrestricted (driver init):
(import driver-util)
(import sys-mmio)

; Restricted (sandboxed service):
; may only import whatever was in (spawn-restricted '(corenetwork ...) thunk)
```

### `include`

```scheme
(include part ...)
```

Inside a `define-module` body: splices sibling `.clp` files into the module's
private environment.  Each `part` resolves to `<module>/<part>.clp` in the
initrd.  Part files are NOT modules and cannot be imported directly — they
share the parent module's namespace.

### Module loader

The kernel module loader searches these directories in order:

1. `./lisp/`
2. `./lisp/lib/`
3. `./lisp/servers/`
4. `./lisp/drivers/`

Module names cannot contain `/` or `..` (path-escape is blocked).

### Capabilities and the sandbox boundary

The Lisp VM sandbox is Cardinal;'s userspace: a capability-gated Lisp context,
not an architectural ring-3.  The VM enforces the boundary in software:

- `spawn-restricted` mints a context with a fixed import list.
- `import` checks the running context's capability set at runtime.
- Read-only grant enforcement is done in the bytes mutators.
- `define-module` is root-only.

There is no hardware page-level enforcement between Lisp contexts; the VM and
the `sys-*` capability modules provide the boundary.

---

## The sys-* capability modules

These modules are C-level built-ins registered by `SysLisp` at boot.  They are
gated: a context must name them in its `import` list AND the spawn grant must
include them.  Import without the grant is an error.

### `sys-io` — legacy x86 port I/O

| Export | Description |
|--------|-------------|
| `(in-u8 port)` | Read 1 byte from I/O port |
| `(in-u16 port)` | Read 2 bytes |
| `(in-u32 port)` | Read 4 bytes |
| `(out-u8 port val)` | Write 1 byte |
| `(out-u16 port val)` | Write 2 bytes |
| `(out-u32 port val)` | Write 4 bytes |

### `sys-mmio` — MMIO mapping and DMA allocation

| Export | Description |
|--------|-------------|
| `(mmio-map phys size)` | Map a physical region uncached (UC) — volatile MMIO registers |
| `(mmio-map-wc phys size)` | Map write-combining (WC) — framebuffer scanout fronts; fast streaming writes, slow reads |
| `(mmio-map-wb phys size)` | Map write-back cached — compositing into a WB DMA backing; requires `phys > 0` |
| `(dma-alloc size)` | Physically-contiguous, zeroed, UC DMA buffer; `(bytes-phys b)` gives the physical address |
| `(dma-alloc-wb size)` | Same but WB-cached — for CPU-write, device-read buffers (e.g. framebuffer backing); do not use when the device writes it |
| `(dma-alloc-32 size)` | DMA buffer with a physical address below 4 GiB — for 32-bit-only devices (RTL8139, USB host controllers) |

### `sys-pci` — PCI device discovery and MSI

| Export | Description |
|--------|-------------|
| `(pci-find vid did)` | Physical ECAM address of first matching PCI device, or `#f` |
| `(pci-find-class class sub)` | First PCI device with given class/subclass |
| `(pci-find-all vid did)` | List of ECAM addresses of every matching device |
| `(pci-find-class-all class sub)` | List of ECAM addresses of every matching class |
| `(pci-setup-msi ecam-phys)` | Configure MSI/MSI-X on the device; return an opaque MSI handle (fixnum), or `#f` |
| `(msi-count handle)` | MSI interrupt counter (advances on each interrupt) |
| `(msi-wait handle seen [timeout-ns])` | Park until MSI counter passes `seen`; returns `#f` immediately if already passed or on timeout |
| `(pci-assign-bars ecam-phys)` | Assign BARs and open bridge windows for firmware-unconfigured devices; returns first BAR base or `#f` |

### `sys-irq` — ISA/IOAPIC interrupt lines

| Export | Description |
|--------|-------------|
| `(irq-register gsi)` | Claim ISA IRQ line `gsi`; return opaque handle (fixnum) or `#f` |
| `(irq-count handle)` | IRQ counter for this line |
| `(irq-wait handle seen [timeout-ns])` | Park until IRQ counter passes `seen`; `#f` if already passed or on timeout |

### `sys-cmdline` — kernel command line

| Export | Description |
|--------|-------------|
| `(cmdline-has? "substr")` | `#t` if the substring occurs in the kernel command line |
| `(cmdline-get "key=")` | String value of the first `key=VALUE` token, or `#f` |

### `sys-reg` — hardware registry

| Export | Description |
|--------|-------------|
| `(reg-read-uint "path" "key")` | Read an unsigned integer from the SysReg key/value store; `#f` if absent |

The registry is populated by the boot enumerators (PCI, ACPI, multiboot).
Paths like `"HW/PCI/0"`, `"HW/BOOTINFO/FRAMEBUFFER"` are examples.

### `sys-initrd` — initrd file access

| Export | Description |
|--------|-------------|
| `(initrd-file "name")` | Bytes of a file from the boot initrd tar, copied into a fresh owned buffer; `#f` if not found |

### `sys-ttf` — TrueType glyph rasterization

| Export | Description |
|--------|-------------|
| `(ttf-rasterize font-bytes cp px)` | Rasterize codepoint `cp` at pixel size `px`; returns `(coverage w h xoff yoff advance)` where `coverage` is an 8-bit alpha bitmap or `#f` for empty glyphs |
| `(ttf-vmetrics font-bytes px)` | Returns `(ascent descent linegap)` in pixels (descent is negative) |

These are the raw kernel-side rasterizer calls; drivers use the higher-level
[`ttf` library module](graphics.md#ttf-module-truetype-antialiased-text) which memoizes results.

### `sys-shm` — shared-memory grant (grantee side)

| Export | Description |
|--------|-------------|
| `(map-grant g)` | Map the granted region as a WB-cached `bytes`; returns `#f` if the grant was revoked |

### `sys-shm-mint` — shared-memory grant (owner/compositor side)

| Export | Description |
|--------|-------------|
| `(grant-mint buffer ['ro \| 'rw])` | Mint an unforgeable grant over `buffer`'s physical region; default is `'ro` (least privilege) |
| `(grant-revoke g)` | Invalidate the grant; future `map-grant` returns `#f`; idempotent |

The split `sys-shm` / `sys-shm-mint` enforces that only the compositor (the
owner) can create and revoke grants; a surface client gets `sys-shm` (read or
write the mapped region only) and never gets `sys-shm-mint` (no ability to
grant arbitrary physical memory to another context).

### `sys-debug` — reflective debugger capability

**Gated** — this module is POWERFUL.  Only a context granted `sys-debug` in
its spawn grant may import it.

| Export | Description |
|--------|-------------|
| `(ctx-make thunk)` | Create a PAUSED context applying `(thunk)`; not enqueued; drive with `ctx-step` |
| `(ctx-step c [n])` | Advance context `c` up to `n` reductions (default 1); returns status symbol: `eval`, `apply`, `done`, `error`, or `suspended` |
| `(ctx-status c)` | Stored status register: `eval`, `apply`, `done`, or `error` |
| `(ctx-control c)` | The expression `c` is about to evaluate (useful in `eval` state) |
| `(ctx-value c)` | Result accumulator (meaningful when `done`) |
| `(ctx-error c)` | Error message string, or `#f` |
| `(ctx-list)` | List of all live context handles on this core's scheduler queue |
| `(ctx-blocked? c)` | `#t` if `c` is parked waiting for a message |
| `(ctx-pause c)` | Mark a live scheduler-owned context blocked (attach for cooperative pause) |
| `(ctx-unpause c)` | Clear the blocked flag (return to scheduler) |

### `sys-console` — interactive serial REPL

Only available when `cardinal.repl` is on the kernel command line.

| Export | Description |
|--------|-------------|
| `(console-poll)` | Non-blocking: bytes waiting on the REPL channel, or `#f` |
| `(console-write str)` | Emit bytes on the REPL channel |
| `(repl-eval str)` | Evaluate `str` in the persistent REPL environment; return transcript |
| `(console-arm-rx)` | Enable COM1 receive interrupt |
| `(console-flush)` | Flush the coalesced debug log buffer |

The `sys-console` module is registered only when the kernel boots with
`cardinal.repl`; see [Debug the OS](../guides/debugging.md) for the REPL access
path (raw serial, driven by `scripts/serial-repl.py`); see [Debug the OS](../guides/debugging.md).
