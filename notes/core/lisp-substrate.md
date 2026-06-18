<!--
 Copyright (c) 2026 Himanshu Goel

 This software is released under the MIT License.
 https://opensource.org/licenses/MIT
-->

# Cardinal; substrate direction: a kernel-resident Scheme

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
6. **A small Scheme resident in the kernel** is the chosen substrate — a clean,
   minimal, well-specified Lisp that fits the established goals (see below).
   Native code is still allowed but only inside a heavily restricted sandbox; the
   sandbox *mechanism* is deferred (hardware-ring isolation is the fallback; a
   managed-bytecode jail such as WASM is a possible future option, not pursued
   now).

## Why Scheme

- **Homoiconicity + `read`/`write` give the self-documenting data format for
  free.** S-expressions *are* the schema-bearing serialization (no separate IDL,
  no EDN); the object browser is `write`/inspect on any value; "apps stored as
  objects" is native (code is data).
- **Immutability + persistent data structures are a deliberate Cardinal
  deviation from classic Scheme**, kept because they are *load-bearing for the
  OS*, not for style: structural sharing ⇒ cheap snapshots ⇒ cheap checkpoints
  ⇒ "versioned data banks" (`Main.md`) for free, and lock-free reads (see
  Concurrency). Classic Scheme's `set-car!`/mutable vectors are dropped in favour
  of persistent structures plus one explicit mutable cell (an *atomic box*).
- **Symbol-keyed persistent maps subsume kvs / SysReg / SysObj** into one
  abstraction — the "one coherent storage model" the docs kept asking for.
- **W7 is literally a capability-secure Scheme** (see below), so the security
  model is not bolted on — it is the language's own scoping discipline.

Scope: a small **Scheme-*inspired* Lisp** — not R7RS-conformant, and that is a
deliberate choice (2026-06). Scheme syntax/semantics (`#t`/`#f`, `#\char`, `()`
with no nil, only `#f` false) and special forms `quote`/`quasiquote`/`if`/
`define`/`lambda`/`let`/`let*`/`letrec`/named-`let`/`set!`/`begin`/`cond`/`and`/
`or`/`when`/`unless`/`while`/`case`, plus a Scheme-defined prelude. Numerics:
unboxed fixnums + heap-boxed flonums; bignums/rationals optional later.

**Cut (2026-06), to keep the substrate small:** `syntax-rules` **macros** and
**`call/cc`** + the exception/`values` system were implemented, validated, then
removed. The common derived forms (`when`/`unless`/`case`/`while`) are kept as
cheap interpreter special cases instead of macros (a future JIT recognizes them
directly; macros' real payoff — *program-defined* syntax — isn't needed yet, and
nothing in the prelude depended on the macro engine). `call/cc` was the
lowest-value / highest-subtlety piece. Both can return if a concrete need (a
driver/IPC DSL → macros; coroutines → continuations) arises. Consequently R7RS
conformance (the chibi `r7rs-tests.scm` suite) is **no longer a goal** — it was
only ever a way to validate correctness, which the per-feature host suites do.

### Floating point is in scope (corrected)

The kernel avoids FP only in *low-level native code* (`-mno-sse` is blanket
because boot/ISR/scheduler contexts don't manage FP register state). But the
**task manager already saves/restores full FP/SSE state per task** on every
context switch (`SysTaskMgr` `fpu_state` + `fp_platform_get/setstate`), and the
Scheme runtime always runs in **task context**. So the runtime can use **native
hardware `double`** — the runtime translation units are compiled with SSE
enabled (overriding the blanket `-mno-sse`), no soft-float needed. The single
caveat mirrors the GC-context rule: **no float ops in non-task context** (ISRs,
early `module_init` before the scheduler) — runtime float arithmetic runs during
evaluation, which is always task context. Flonums are heap-boxed (a `double`
does not fit beside the tag bits); int/float arithmetic follows the usual
contagion (any flonum operand ⇒ flonum result).

## Security model: lexical scope = capabilities (W7)

Jonathan Rees's W7 ("A Security Kernel Based on the Lambda-Calculus", 1996):
lexical environment *is* the capability list — a computation can only invoke
what is bound in its environment; references are unforgeable. Maps exactly onto
"object-capabilities as the one permission model": a task's root environment is
its capability set; the per-task descriptor table holds its initial bindings.
**Reflection is itself a capability** (delightful for the trusted object
browser, withheld from untrusted code).

## The native-vs-managed split

- **Scheme = trusted, managed, first-class tier.** Safe *by construction* (no raw
  pointers exposed); needs no adversarial verifier — trust comes from "the only
  way to run is through the runtime we own."
- **Untrusted foreign-native tier = a restricted sandbox.** Mechanism deferred:
  hardware-ring isolation is the fallback; a managed-bytecode jail (e.g. WASM,
  whose verifier we'd inherit rather than write) is a *possible future* option,
  not pursued now. Ported native code is surfaced to Scheme as opaque
  capability-wrapped objects.
- Existing C servers/drivers are exposed as **host functions** into the Scheme
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
sharing) and atomically swap the reference (CAS).** Scheme has no `atom`, so this
is an explicit **atomic box** (a one-slot mutable cell with a CAS update); it is
structurally identical to RCU.

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
- **Inline caches** for generic/record dispatch and map lookup (invalidate on
  redefinition via an epoch counter).

Allocation:
- **Tagged immediates / fixnums** — no boxing for integer arithmetic (highest-
  leverage rep decision; no-FP makes it cleaner — spare low bits on aligned
  pointers, no NaN-boxing).
- **TLAB bump allocation** (lock-free fast path, SMP-friendly). v1 collector is
  **non-moving mark-sweep**; design the object header to allow a generational
  moving nursery later.
- **Transient builders** (thread-confined locally-mutable build-then-freeze) —
  kill the O(n log n) intermediate-version garbage in build-up loops.
- **Fused iteration** (transducer-style) — map/filter compose into one pass with
  no intermediate sequences; chunked lazy sequences amortize per-element cost.
- **Small-collection specializations** (array-backed map/vector before HAMT/RRB).

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

Reordered in practice from the original sketch: the bytecode VM was deferred
(pure optimization; the tree-walker has proper tail calls), and a *conservative*
mark-sweep GC was done directly against the tree-walker instead of a precise GC
behind a VM.

- **Phase 0 — Core values + reader/printer.** *(done)*
- **Phase 1 — Tree-walk evaluator + environments.** Lexical envs, closures,
  special forms, proper tail calls via an internal loop. *(done)*
- **Phase 2 — Interning + vectors + core library.** Interned symbols (eq? =
  identity), immutable vectors, equal?, list library, higher-order, cond/and/or.
  *(done; the HAMT persistent map is deferred until the persistence layer needs
  it — kept small per guidance.)*
- **Phase 3 — Core language completion.** let*/letrec/named-let, quasiquote,
  floats (native hardware doubles), the string/char library, display/write,
  variadic lambda, error, and a Scheme-defined standard prelude. A curated
  R7RS-subset conformance suite runs green. *(done)*
- **Phase 4 — GC.** *Conservative, non-moving mark-sweep* (gc.c): conservative
  C-stack + register roots, precise heap tracing, threshold-triggered. Runs under
  sustained allocation without leaking. *(done)* A precise/moving collector would
  need a VM value stack and is not currently planned.
- **Bytecode VM — deferred.** Pure optimization; revisit only on a measured need
  or if a moving GC is wanted.
- **Macros + call/cc — implemented then CUT** (see Scope above). Not on the
  roadmap unless a concrete need (driver/IPC DSL; coroutines) brings them back.
- **Next — Kernelization.** Wrap as a signed `Sys*`/`Core*` module against
  `common/`; wire the output sink to DEBUG_PRINT; host-function FFI to expose
  existing C servers; the native-ISR → Scheme-task event bridge.
- **Then — concurrency primitives** (the atomic box / CAS-of-root), **capabilities**
  (W7-style environments), **driver MMIO/bytevector primitives**, the
  **single-level-store persistence layer**, the **foreign-native jail**.

Each phase is independently reviewable. Moving/persistent GC, JIT, and persistence are
explicitly later phases the earlier ones are designed not to preclude.

## Testing

Per-phase host harnesses (`libs/lisp/test/`) plus a thunk-based
`(test expr => expected)` corpus (`conformance.scm`, ~70 cases) exercise the
whole language host-side. Full R7RS conformance (the chibi `r7rs-tests.scm`
suite) is **not** a goal — this is a Scheme-*inspired* Lisp, not R7RS; the suite
was only ever a correctness check, which the host harnesses provide. (It would
also require `define-syntax`/`call/cc`, which were cut, and the full numeric
tower.)
