# Cardinal; OS

**Cardinal;** is an extremely modular, security-oriented **microkernel operating
system** for `x86_64`. The kernel itself does almost nothing: it loads, verifies,
and relocates signed ELF modules. Everything above the tiny `Sys*` core —
physical/virtual memory aside — is written in **Lisp**, run by the kernel-resident
bytecode VM.

This site is the **API reference** for that Lisp layer:

- **[Servers](servers/index.md)** — the `Core*` OS services (input, audio, power,
  storage, display, network, USB, compositor). Each is a Lisp context that speaks a
  message protocol.
- **[Drivers](drivers/index.md)** — the hardware drivers bound to PCI/legacy devices
  by `lisp/init.clp` (AHCI, virtio, USB controllers, NICs, HD Audio, PS/2, …).
- **[Lisp VM](vm/api.md)** — the language and primitive (`prim`) surface exposed by
  the kernel bytecode VM: special forms, builtins, capabilities, and the messaging
  and scheduling model that servers and drivers are built on.

## How the pieces fit

```
        ┌─────────────────────────────────────────────┐
        │  Lisp servers (Core*)   ── message protocol ─┤
        │  Lisp drivers           ── sys-* prims ──────┤
        ├─────────────────────────────────────────────┤
        │  Kernel Lisp bytecode VM  (libs/lisp)        │
        ├─────────────────────────────────────────────┤
        │  Sys* core: memory / interrupts / scheduler  │
        │             object model / registry / syscalls│
        ├─────────────────────────────────────────────┤
        │  Microkernel: ELF loader + verifier          │
        └─────────────────────────────────────────────┘
```

`lisp/init.clp` is the single boot-policy file: it brings up the `Core*` services
and binds each driver to hardware (each gated on `pci-find`). There is no C service
or driver binder anymore.

## Building & viewing these docs

```bash
pip install mkdocs-material
mkdocs serve        # live preview at http://127.0.0.1:8000
mkdocs build        # static site → ./site
```

The published site is built from `master` by `.github/workflows/docs.yml` and served
from GitHub Pages.
