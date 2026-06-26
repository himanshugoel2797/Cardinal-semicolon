# System overview

*The mental model for how Cardinal; is structured: a microkernel that does almost nothing, a thin layer of C modules that set up the hardware, and a Lisp OS that runs on top — all wired together by a chain of signed trust.*

---

## The thesis: a kernel that refuses to grow

Cardinal; is built around one discipline: the kernel is not allowed to accumulate policy. Its job is exactly three things — find ELF modules in the initrd, verify each one's cryptographic signature, and link it into the running address space. Memory management, interrupt routing, scheduling, drivers, and every OS service are **separate, separately-signed modules** that the kernel loads but does not govern. If a piece of functionality can live outside the kernel without sacrificing correctness, it must.

That discipline is not just about cleanliness. Every line in the kernel is trusted code that runs before any verification is complete. Keeping it minimal keeps the unauditable surface small. The rest of the system earns its trust by carrying a valid signature; the kernel's only job is to check it.

---

## The four layers

```
  ┌─────────────────────────────────────────────────────────┐
  │  Core* servers: coreinput / coredisplay / corenetwork …  │
  │  Lisp drivers: virtio-gpu / ahci / rtl8139 / ps2 / …    │
  │  (Lisp contexts — each with its own heap, capability set) │
  ├──────────────────────── message protocol ────────────────┤
  │  Kernel Lisp bytecode VM  (libs/lisp → SysLisp module)   │
  │  scheduler loop, per-context GC, sys-* capability prims  │
  ├─────────────────────────────────────────────────────────┤
  │  Sys* core modules                                        │
  │  SysPhysicalMemory / SysVirtualMemory / SysMemory         │
  │  SysInterrupts / SysMP / SysTimer / SysFP                │
  │  SysObj / SysReg / SysUser / SysTaskMgr                  │
  │  SysDebug / SysGdb                                        │
  ├─────────────────────────────────────────────────────────┤
  │  Microkernel                                              │
  │  ELF loader + module verifier + initrd parser             │
  │  bootstrap allocator + symbol DB                          │
  └─────────────────────────────────────────────────────────┘
```

### Layer 1 — the microkernel

The kernel (`kernel/`) links at a fixed high virtual address and never grows. At boot it parses the initrd (a tar archive), runs `loadscript.txt` line by line, and for each `LOAD:` directive: reads the `.celf` module, runs `VerifyModule` (HMAC-SHA256 over the header and payload), relocates the ELF against the symbol table it has built so far, and calls `module_init`. A `CALL:` directive invokes an already-loaded exported symbol by name. That is the entire boot mechanism — no dynamic loader, no device enumeration, no policy.

### Layer 2 — the Sys\* modules

The `Sys*` modules (`modules/`) are **kernel-privileged**: they link against and extend the kernel's own symbol table, running at the same privilege level. They bring up the hardware substrate in a fixed order driven by `loadscript.txt`:

- **SysPhysicalMemory / SysVirtualMemory / SysMemory** — the physical frame allocator, page-table management, and the slab/malloc layer on top.
- **SysInterrupts** — GDT, IDT, APIC, IOAPIC; interrupt routing and exception handling.
- **SysMP** — symmetric multiprocessing bring-up; per-core TLS; the AP boot sequence driven by `apscript.txt`.
- **SysTimer** — local APIC timer calibration.
- **SysObj / SysReg** — the object model and the hierarchical key/value registry (ACPI tables, PCI device tree, runtime configuration).
- **SysUser** — the syscall-table mechanism (register-based `syscallq`).
- **SysTaskMgr** — per-core scheduler infrastructure: brings up the per-core scheduler threads, then returns so `SysLisp` can take over.
- **SysDebug / SysGdb** — COM1 debug output; GDB remote-serial-protocol stub over COM2.

Once this layer completes, `loadscript.txt` ends with `LOAD:./SysLisp.celf` then `CALL:lisp_scheduler_enter`. The boot thread becomes the per-core Lisp scheduler loop and never returns.

### Layer 3 — the Lisp bytecode VM

`SysLisp` wraps `libs/lisp`, a kernel-resident Scheme-inspired Lisp runtime. It is a Scheme-*inspired* dialect — not R7RS-conformant by design: `call/cc` and `syntax-rules` macros were implemented and removed as not yet earning their complexity. The runtime provides:

- A **CEK abstract machine** with an explicit value/continuation stack (not C-stack recursion). This gives cheap, safe suspension: a context's entire execution state is a GC root on the heap, so it can be parked at any safe point and resumed later.
- A **cooperative round-robin scheduler** over Lisp *contexts* — independent root environments, each with its own private heap. A reduction budget is charged at every call and loop back-edge; when it hits zero the context yields and the scheduler picks the next. An infinite loop cannot wedge a core.
- **Per-context garbage collection**: each context's heap is collected independently (precise, from the CEK registers and continuation stack), so a GC pause is bounded by one context's heap, not the whole OS.
- **Copy-on-send IPC**: a value sent between contexts is deep-copied into the receiver's heap. Interned symbols are the only shared-immutable region, so there are no cross-heap pointer hazards.
- A **`sys-*` capability module system** (see below): hardware primitives are grouped into named built-in modules (`sys-io`, `sys-mmio`, `sys-pci`, `sys-irq`, …) and are not ambient.

Each core runs its own scheduler instance over its own pool of contexts. Cross-core messaging is not yet implemented; contexts do not migrate between cores. The runtime runs only in **task context** (never in ISRs or early `module_init`), which is why flonum arithmetic is safe: `SysTaskMgr` saves and restores full SSE/FP state per native task, and once the Lisp scheduler loop is running it is the only thing on its core.

### Layer 4 — the Lisp OS

Everything above `SysLisp` is Lisp: `.clp` source files packed into the initrd and loaded by the VM at boot. There is no C server or driver binder; the old `CoreDriver`/`devices.txt`/`servicescript.txt` mechanism was deleted.

**`Core*` servers** (`lisp/servers/`) are Lisp contexts that own a hardware abstraction and speak a message protocol:

- `coreinput` — keyboard/pointer event routing
- `coredisplay` / `corecompositor` — display surface registration and window compositing
- `corenetwork` — ARP/ICMP/IPv4/UDP networking and the reliable `RDT` transport; DHCP
- `coreaudio` — HD Audio endpoint model: playback, capture, volume, jack-sense polling
- `corestorage` — block-device registry; `cardfs` object store
- `corepower` — power state management
- `coreusb` — USB host-controller abstraction (UHCI/xHCI/EHCI)

**Lisp drivers** (`lisp/drivers/`) handle specific hardware: `virtio-gpu`, `lfb`, `ahci`, `virtio-net`, `rtl8139`, `rtl8169`, `ps2`, `hdaudio`, `uhci`, `xhci`, `ehci`, `usb-hid`, `usb-hub`, `usb-storage`, `usb-audio`. A driver is a Lisp module: it imports the `sys-*` capability prims it needs, initializes hardware once, then loops on a mailbox or waits for an ISR-wake event.

---

## Why Lisp on top

The original motivation was a **typed, introspectable object model as the system lingua franca** — an OS that handles data it can *understand*, not an opaque byte stream. Successive simplifications of the storage and IPC design converged on a language-based OS (in the tradition of Lisp machines and Singularity/Midori): when the runtime owns every value, you get structured IPC, fine-grained permissions, self-documenting data, and cheap checkpointing as consequences of the language model, not extra features to build.

The concrete technical payoffs are:

- **The Lisp context *is* the isolation unit.** A context can only name what is bound in its lexical environment; it cannot forge a reference to something not handed to it. This is Jonathan Rees's W7 model — lexical scope equals capability list. No page tables, no ring transitions, no separate address space required.
- **Drivers are safe by construction** (for the control plane): a Lisp driver cannot do an out-of-bounds write, cannot corrupt another context's heap, cannot access hardware not granted to it at load time. The data-plane hot path for line-rate devices (where a pure-Lisp loop would be too slow) is a C primitive wrapped in a Lisp interface — the unsafe surface is contained to that leaf.
- **Message passing replaces shared state.** Contexts communicate only through `send`/`recv` with copy-on-send semantics. There are no global mutable tables shared between contexts, which eliminates the whole class of lock-ordering and re-entrancy bugs that have plagued the C driver layer (see `notes/AUDIT.md`).
- **Boot policy is itself Lisp.** All decisions about what comes up and with which authority live in `lisp/init.clp`, not in C. A reader can understand the entire boot policy by reading one file.

!!! note "The sandbox is not ring-3"
    Lisp context isolation is *language-level*, not hardware-ring isolation. A capability-gated context is not running at CPL=3; it is running inside the same kernel address space as everything else, with the VM enforcing the boundary. There are no page-table switches and no `syscall`/`sysret` crossings between a Lisp driver and the kernel. See [Capabilities & the sandbox](capabilities-and-sandbox.md) for the full picture, including the accepted risks.

---

## How components talk

**Lisp-to-Lisp**: contexts communicate through **`send`/`recv`**. A server publishes a context handle (via a registry key or by passing it at spawn time); clients `send` it a message (a list) and optionally `recv` a reply. The value is deep-copied on send; the sender cannot observe or mutate what it sent after the fact. See [Message passing & concurrency](message-passing.md) for the scheduling semantics and server-design gotchas.

**Lisp-to-hardware**: drivers use the `sys-*` capability primitives — `(mmio-map phys size)`, `(out-u8 port val)`, `(msi-wait slot)`, `(dma-alloc-32 size)`, and so on. These are C functions registered as built-in modules; `(import sys-mmio)` makes them available in the importing module's lexical scope and *nowhere else*. A context that did not import `sys-mmio` cannot name `mmio-map`; there is no ambient pollution.

**`lisp/init.clp` — the sole boot policy and device binder**: the kernel loads `init` at boot with full (root) authority and calls `(system-init)`. `init` imports every server and driver module at root (so each driver's `sys-*` prims are captured into closures at load time), then spawns the long-lived service loops as *restricted* contexts with an empty capability grant. A spawned driver loop can no longer `(import sys-pci)` to reach new hardware — it already has exactly what it was handed. This is the knob `init` turns to configure what each component is allowed to do; changing `lisp/init.clp` changes the system's security posture without touching C.

---

## The security spine

The trust chain has two links. The **first** is the signed module format. Every loadable file is a `.celf` (Cardinal ELF): an ELF payload wrapped in a `ModuleHeader` containing a name, a 64-bit NID (derived from the name and a hash of the module itself), an HMAC-SHA256 over header and payload, and a truncated key hash. Two HMAC keys are in play: `KMOD_HMAC_Key.txt` for `Sys*` modules, `SERV_HMAC_Key.txt` for drivers and servers. `VerifyModule` checks the HMAC before the first byte of ELF is touched; a tampered or unsigned module never loads.

The **second** link is the Lisp capability model. Once the OS is running, a context's authority is exactly what is bound in its lexical environment; `import` of a `sys-*` module is gated on the context's capability grant; a restricted context cannot escalate. Reflection — the ability to inspect or manipulate other contexts — is itself a capability withheld from untrusted code. The trusted surface is the runtime itself (`libs/lisp`) plus the C primitives it exposes; everything else is Lisp that cannot escape its grant.

!!! warning "Known limitations and stubs"
    The system is in active development. `notes/AUDIT.md` tracks known stubs and incomplete areas. As of mid-2026: TCP and the userspace socket API are unimplemented (UDP and the `RDT` reliable transport work); `tarfs`'s `module_init` is a stub; cross-core context messaging is not yet built; the hardware-ring sandbox for truly untrusted (non-Lisp) native code is a planned future feature. Check `notes/AUDIT.md` before assuming something is broken rather than intentionally unfinished.

---

## Where to go next

If this is your first time in the codebase, the recommended path is:

1. **[Build & boot](../tutorials/index.md)** — get a running QEMU image and a serial REPL.
2. **[Write a Lisp driver](../guides/add-a-pci-driver.md)** — the fastest path to something running.
3. **[Capabilities & the sandbox](capabilities-and-sandbox.md)** — understand how `import` and grants work before writing anything that touches hardware.
4. **[Message passing & concurrency](message-passing.md)** — the server design model and the copy-on-send gotchas.
5. **[Lisp VM reference](../vm/api.md)** — the full surface of special forms, builtins, scheduling primitives, and the `sys-*` capability modules.

The server and driver references document the concrete message protocols:

- [`Core*` servers](../servers/index.md) — coreinput, coreaudio, corepower, corestorage, coredisplay, corecompositor, corenetwork, coreusb.
- [Drivers](../drivers/index.md) — the hardware drivers bound by `lisp/init.clp`.

---

## See also

- [Capabilities & the sandbox](capabilities-and-sandbox.md) — the W7 capability model, `spawn-restricted`, and why the sandbox is not ring-3.
- [Message passing & concurrency](message-passing.md) — `send`/`recv`, copy-on-send, the cooperative scheduler, and the per-context GC.
- [`lisp/init.clp`](https://github.com/himanshu-goel0/Cardinal-semicolon/blob/master/lisp/init.clp) — the single boot-policy file; reading it gives the complete picture of what the OS brings up and in what order.
- `notes/AUDIT.md` — tracked known bugs, stubs, and intentionally unfinished areas.
- `notes/core/lisp-substrate.md` — the full internal design note on why Lisp and what was rejected.
- [Lisp VM reference](../vm/api.md) — the concrete language and capability-primitive surface.
