<!--
 Copyright (c) 2026 Himanshu Goel

 This software is released under the MIT License.
 https://opensource.org/licenses/MIT
-->

# Cardinal; substrate direction: a kernel-resident Lisp

This is a whole-system direction note, not just storage — it started as a
filesystem-design review and converged somewhere much larger. It records the
**destination**, the **reasoning**, and crucially the **roads not taken and
why**, so the trail survives.

## The arc (how we got here)

The filesystem review asked: robustness, efficiency, multi-device/removable,
permissions, special regions, and how to expose descriptors across services
without locking everything into a read/write straightjacket. Successive
simplifications:

1. **Custom on-disk FS (the `Main.md` log-structured/COW/object-store spec)** —
   rejected as *undifferentiated, high-risk* work: re-deriving crash
   consistency, free-space, and B-trees on bare metal, where the value isn't.
2. **Off-the-shelf FS (ext4)** — rejected: a POSIX byte-stream tree is the exact
   read/write model we wanted to escape; correct RW-ext4 (jbd2 journal) is the
   *hard* part and is what you'd adopt it *for*; semantic mismatch with an object
   model.
3. **FAT container layer, each file = a COW B+tree store** — good
   (host-mountable boundary + rich format inside, two-tier capabilities,
   devices-as-pools) but judged too complex for a transparent-persistence
   microkernel.
4. **Single-level store (Multics/KeyKOS-EROS/Twizzler/Phantom)** — storage
   stops being a subsystem and falls out of the VM: persistence is orthogonal,
   objects are *mapped* not read/written, NV tiers are caches with page-table-like
   translation. read/write disappears. Foreign filesystems become ordinary
   userspace programs over block devices.
5. **The root cause: opacity.** Every complication (persistent pointers,
   fine-grained permissions, self-documentation, checkpoint granularity) is
   downstream of the OS handling bytes it cannot interpret. The cure is a
   **typed, introspectable object model** as the system lingua franca — a
   language-based OS (Lisp machines, Singularity/Midori).
6. **A small Clojure-flavored Lisp resident in the kernel** is the chosen
   substrate, because Clojure's *data philosophy* fits the established goals
   unusually well (see below). Native code is still allowed but only inside a
   heavily restricted sandbox (WASM-shaped) with a translation shim.

## Why a Lisp, and why Clojure-flavored specifically

- **Immutable persistent data structures (HAMT/RRB) are the COW idea as the
  native value model.** Structural sharing ⇒ cheap snapshots ⇒ cheap
  checkpoints; "versioned data banks" (`Main.md`) become retained old versions
  for free.
- **EDN is the self-documenting on-disk/wire format** — the language's data
  literals *are* the schema-bearing IDL. The "design an IDL" task evaporates.
- **Keyword-keyed maps subsume kvs / SysReg / SysObj** into one persistent-map
  abstraction — the "one coherent storage model" the docs kept asking for.
- **Homoiconicity** makes self-documentation and "apps stored as objects"
  native; the object browser is `pr-str`/inspect on any value.

This is **Clojure-flavored, NOT Clojure**: a small Scheme-ish core that borrows
the data philosophy (persistent structures, keyword maps, EDN, data-first), not
the JVM host, full numeric tower, STM, or stdlib. The `-mno-sse` (no-FP) kernel
constraint forces integer/fixnum-centric numerics (soft bignums optional).

## Security model: lexical scope = capabilities (W7)

Jonathan Rees's W7 ("A Security Kernel Based on the Lambda-Calculus", 1996):
lexical environment *is* the capability list — a computation can only invoke
what is bound in its environment; references are unforgeable. Maps exactly onto
"object-capabilities as the one permission model": a task's root environment is
its capability set; the per-task descriptor table holds its initial bindings.
**Reflection is itself a capability** (delightful for the trusted object
browser, withheld from untrusted code).

## The native-vs-managed split (resolves the WASM question)

- **Lisp = trusted, managed, first-class tier.** Safe *by construction* (no raw
  pointers exposed); needs no adversarial verifier — trust comes from "the only
  way to run is through the runtime we own."
- **WASM (or a hardware-ring jail) = untrusted foreign-native tier.** *That's*
  where a verifier is needed — use WASM's spec'd one, not a bespoke one. Ported
  native code runs there, surfaced to Lisp as opaque capability-wrapped objects.
- Existing C servers/drivers are exposed as **host functions** into the Lisp
  environment and migrated opportunistically — never a big-bang rewrite.

## Drivers

Default to Lisp; keep a **native escape hatch for performance**. The realistic
split is per-driver **control-plane-Lisp / data-plane-native**: enumeration,
config, policy, error handling in Lisp; line-rate hot paths (rtl8169, xHCI,
AHCI) native, surfaced to the Lisp half as a fast primitive. Simple/slow devices
(PS/2, GPIO, sensors, low-rate NIC) can be entirely Lisp.

**Interrupt model (makes Lisp drivers safe vs. the GC-context rule):** a Lisp
driver is never an ISR. A minimal native ISR stub acks the hardware and posts an
event; that resumes a scheduled Lisp task (a captured continuation — see TCO
below) in normal context where GC is permissible. This is the L4-family
userspace-driver shape.

## Concurrency: immutable data + atomic-swap-of-root

The unsafe-concurrency bugs already hit in C (network RX re-entrancy deadlock,
global interrupt-dispatch lock starving APs) are *manual shared-mutable-state*
bugs. Pushing logic into Lisp **concentrates** unsafe concurrency into one
heavily-tested runtime instead of smearing it across every driver/server (it
does not abolish it — the runtime is native and remains concurrency-critical).

**The universal mutation primitive: the only mutable thing is a single aligned
pointer; never mutate in place — build a new immutable version (structural
sharing) and atomically swap the reference (CAS).** This is Clojure
`atom`/`swap!` and is structurally identical to RCU.

- Reads are lock-free/wait-free (grab root, traverse immutable structure).
- Writes are read-copy-CAS, retry on contention.
- **Single-reference** updates are solved by word CAS. **Coordinated
  multi-reference** updates are NOT — design rule: *if two things must change
  atomically, put them under one root* so a single CAS covers them (avoid STM).
- Implementation: never mutate car/cdr in place; allocate a fresh immutable pair
  and single-word-CAS the pointer. (`CMPXCHG16B` available if in-place
  double-word ever wanted.)
- **GC solves lock-free reclamation for free** — old versions live exactly as
  long as a reader references them. So in this model GC is the mechanism that
  makes lock-free *correct*, not merely a tax. (It causes *context-safety*
  issues — never collect in ISR/spinlock — solved by the task model.)

**One mechanism, four payoffs:** atomic-swap-of-immutable-root serves
concurrency, versioning, the persistence checkpoint (atomic on-disk root flip),
and capability environments (immutable binding sets).

## Performance: primitives so the interpreted engine is fast (pre-JIT)

Two cost centers: **dispatch** and **allocation** (functional code allocates
furiously). Most perf primitives fall out of decisions already made for
safety/persistence/concurrency.

Dispatch:
- Compile to **bytecode** once; **threaded dispatch** (computed-goto), not
  tree-walking; superinstructions later.
- **Lexical addressing (De Bruijn) + flat closures** — variable access becomes
  array indexing. This is the perf payoff of the W7 lexical-scope choice.
- **Open-code primitives** (`+`, `car`, `=`, accessors) to dedicated opcodes.
- **Inline caches** for protocol/multimethod/keyword-map dispatch (invalidate on
  redef via an epoch counter).

Allocation:
- **Tagged immediates / fixnums** — no boxing for integer arithmetic (highest-
  leverage rep decision; no-FP makes it cleaner — spare low bits on aligned
  pointers, no NaN-boxing).
- **TLAB bump allocation** (lock-free fast path, SMP-friendly). v1 collector is
  **non-moving mark-sweep**; design the object header to allow a generational
  moving nursery later.
- **Transients** (thread-confined locally-mutable build-then-freeze) — kills the
  O(n log n) intermediate-version garbage in build-up loops.
- **Transducers + chunked seqs** — fuse map/filter into one pass, no
  intermediate sequences.
- **Small-collection specializations** (array-map/array-vector before HAMT/RRB).

Hot Lisp approaching native (the driver story):
- **Optional type hints + `unchecked-` ops** for unboxed inner loops.
- **Unboxed typed arrays / bytevectors + MMIO register-block primitives** that
  open-code to a single load/store (descriptor/packet parsing, register poking).

Kernel-specific:
- Interpret on an **explicit VM stack**, not the tiny in-kernel C stack.
- **AOT-compile the boot image** (no boot-time compile cost; same image
  mechanism later feeds persistence).
- The **explicit-stack/trampoline for TCO** is the same machinery that gives
  cheap **first-class continuations** — which is how a Lisp driver parks on an
  interrupt without a native thread. TCO + driver-events + lightweight task
  suspension are one mechanism.

## Deferred (planned-for, not built yet)

- **Persistent GC** — not needed yet. Plan-ahead constraints: keep every heap
  object **serializable/relocatable** (no embedded raw native pointers in Lisp
  values — native handles go through capability/foreign-object indirection); a
  future moving/persistent collector needs read/write barriers **co-designed
  with the CAS mutation model** (concurrent relocation vs. a CAS on a pointer to
  the moved object is the one real hazard). v1 non-moving mark-sweep sidesteps
  this.
- **JIT** — bytecode + hint-annotated IR is built to consume it later; deferring
  costs nothing structurally.
- **Persistent pointers / swizzling** — objects are opaque/value blobs for v1.

## Build strategy (de-risking)

**Prototype the runtime in isolation, host-first**, before betting the kernel on
it. The runtime (allocator/GC/scheduler glue) is upstream of everything; iterate
where a crash isn't a kernel panic. Fold into the kernel only once GC
context-safety and (later) persistence crash-consistency are proven.

Within the Lisp choice, **data-model first**: make Lisp data the storage/IPC
lingua franca and get a correct interpreter running before investing in the JIT
or persistent GC.

## Phased plan

- **Phase 0 — Core values + reader/printer (host-built).** Tagged value
  representation (fixnums/immediates/heap header), s-expr/EDN reader, printer.
  Milestone: read→print round-trip. *(in progress)*
- **Phase 1 — Tree-walk evaluator + environments.** Lexical environments,
  special forms (`quote`/`if`/`fn`/`let`/`def`), closures, primitive ops. Pure,
  immutable. Milestone: evaluate non-trivial programs; arithmetic; recursion.
- **Phase 2 — Persistent data structures.** Cons/list, then HAMT map + keyword
  interning + array-map specialization; transients. Milestone: Clojure-style
  map/vector ops with structural sharing.
- **Phase 3 — Bytecode compiler + threaded VM.** Lexical addressing, open-coded
  primitives, explicit VM stack, proper tail calls + continuations. Milestone:
  measurable speedup over the tree-walker; deep recursion safe.
- **Phase 4 — GC.** Non-moving mark-sweep + TLAB allocation. Milestone: runs
  under sustained allocation without leaking; header reserves room for a future
  moving nursery.
- **Phase 5 — Concurrency primitives.** `atom`/`swap!` (CAS-of-root), the
  one-root coordinated-update discipline; integrate the lock-free reclamation
  story with the GC.
- **Phase 6 — Kernelization.** Wrap as a signed `Sys*`/`Core*` module against
  `common/`; host-function FFI to expose existing C servers; the native-ISR →
  Lisp-task event/continuation bridge.
- **Phase 7+ — Capabilities (W7 environments), driver MMIO/bytevector
  primitives, the single-level-store persistence layer, WASM jail.**

Each phase is independently reviewable. Persistent GC, JIT, and persistence are
explicitly later phases the earlier ones are designed not to preclude.
