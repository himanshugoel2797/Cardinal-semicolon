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

Note its role *narrows* under the process model below: within a context,
execution is single-threaded and cooperative (one reduction at a time → no
intra-context data races), and *between* contexts the model is shared-nothing +
copy-on-send (no shared mutable structure at all). So the atomic box is reserved
for the rare genuinely-shared mutable cell across scheduler cores (e.g. a global
registry), not the common case.

## Process model & scheduling: the interpreter IS the scheduler

The endpoint of the language-OS direction (Singularity/SIP + Erlang/BEAM): when
isolation is enforced by the *language* (no raw pointers; capabilities = lexical
scope, per W7), hardware memory protection is redundant, and the traditional
per-process OS machinery collapses. This is a **designed-but-unbuilt** model
(it lands around/after kernelization, task #10) and it pulls back in the
explicit-execution-state work the bytecode VM was deferring.

### Contexts = processes (works at the language level today)

A "process" is just an **independent root environment** + the capabilities bound
into it. `lisp_default_env()` already gives a fresh one; `define` mutates only
that env; closures capture their defining env; you cannot *name* what is not in
your env. Isolation is lexical, not address-space-based — **no page tables
required**. (This part exists now; what follows is the runtime that schedules and
collects them.)

### The interpreter is the scheduler — what collapses

- **No page-table switch / no per-context address space.** One address space;
  contexts isolated by shared-nothing heaps + capabilities. A switch costs no
  `cr3` reload, no TLB flush.
- **No per-context CPU register save/restore.** A context's "registers" *are* its
  explicit value/continuation stack (see below). The CPU registers belong to the
  scheduler loop, not to any context, so a switch saves nothing CPU-level — it is
  a pointer swap + resume of the context's explicit stack. Even FP state is not
  per-context: a flonum is a heap object, and switches happen at safe points
  where no SSE temp is live, so no `fxsave`/`xrstor` per switch.
- **No ring transition for "syscalls".** A context calling an OS service is a
  primitive/function call inside the interpreter — no `syscall`, no kernel/user
  crossing. The SysUser syscall-table mechanism becomes function application
  (closing the loop with "invoke is a call, not read/write").
- **Switch trigger:** at quantum-end or `yield`, the running reduction returns to
  the scheduler, which picks the next context and resumes it.

What stays (deliberately thin): **N real OS threads, one per core**, each running
the interpreter scheduler loop, set up once at boot (SMP parallelism still needs
real CPUs). A timer survives only as a *backstop* to detect a wedged scheduler
thread, not as the preemption mechanism. SysTaskMgr shrinks from "manage every
process (cr3 + fxsave + per-task stack)" to "bring up the per-CPU scheduler
threads + the timer backstop."

### Safe points & preemption

Every **call and loop back-edge is a safe point.** A reduction counter is
decremented at each, and the context yields when it hits zero. This requires the
interpreter to own its execution state **explicitly** (a context-owned
value/continuation stack, not C-stack recursion) so a context can be suspended,
resumed, and walked precisely — that is the trampolined/explicit-stack execution
model (the bytecode VM, if built, sits on top as a perf layer).

Expensive **primitives** are the one gap in "every call + back-edge": a C builtin
runs to completion without passing a Lisp safe point. They must charge reductions
too, and if the cost exceeds the remaining quantum, **split** (BEAM "trapping"):
process a chunk, then return a *trap* carrying partial state (accumulator +
cursor) so the scheduler parks the context and re-enters the primitive next
quantum. Two clean ways to make this automatic:

- **(a) Define bulk ops in Lisp** (`map`/`filter`/`fold`): every element step is
  already a safe point → auto-preemptible, zero trap machinery. This is the
  default; the prelude already does much of it.
- **(b) For the few that must be C-fast**, route them through *one* budget-aware
  iteration combinator that owns the chunk → charge → trap → resume logic, so the
  fragile resumable convention exists in exactly one place.

A trapped primitive's partial state is a per-context heap value held in the
context's execution state → it survives the yield and is a precise GC root.

### Per-context GC (no global stop-the-world)

Each context owns its **own heap and collector**; a collection scans only that
context's roots and objects, pausing only that context — pause time is bounded by
*one* context's heap, never the whole OS. Soundness requires the cross-heap
discipline:

- **Shared-nothing between contexts, except a shared *immutable* region.**
  Cross-heap pointers go one way only, into shared-immutable, which never points
  back. A context GC treats a shared-region pointer as an external root
  (mark-and-stop; don't trace/sweep into it).
- **Interned symbols are the first shared-immutable region** (already global +
  immutable); interning is the one piece needing a lock (or per-context caches
  over a shared table).
- **Copy-on-send messaging.** Sending a value into another context deep-copies it
  — safe and cheap *because data is immutable* (the Erlang model). **Large
  immutable blobs** can live in a shared refcounted region to avoid copying.

Because execution within a context is single-threaded and GC runs at
scheduler-chosen safe points, roots are precisely enumerable and collection never
races — the per-context GC is the precise collector the conservative single-stack
one is not.

### Long native ops: async-yield is the universal interface

Native escapes reintroduce real registers/stack, but they are **wrapped in Lisp
interfaces**, so the fragile machinery is mostly Lisp and the native leaf is a
single atomic step between two safe points. For a native op that is *long*, the
interface is **always async-yield** (start it, yield via the interpreter's own
context switch, resume on a completion event) — never "block the scheduler
thread". The implementation of the completion source differs by what the op is
bound on, not the interface:

- **Device / hardware-offloaded** (DMA, disk, NIC, crypto/compression
  accelerator, GPU) — *pure async, no extra thread*: program the device, yield,
  and the completion **interrupt** wakes the context (a minimal native ISR posts
  an event → mark runnable). This is the same path as blocking I/O (blocking I/O
  *is* a yield) and is most "long native ops" in an OS.
- **Opaque + CPU-bound + synchronous** foreign code (can't chunk, can't be made
  natively async) — the one irreducible case: async can't conjure a CPU, so it is
  *run on a worker OS thread* (a "dirty scheduler") that signals completion. The
  context still just yields/resumes; a thread is consumed only because CPU work
  must occupy a CPU. **Deferred** — bring in dirty schedulers (or similar) later;
  the common hardware-offloaded path needs none.

This yields **one unified completion path** — *event → wake context* — serving
I/O, DMA, timers, inter-context messages, and the rare CPU-worker done-signal.
The driver ISR→event→wake primitive is universal.

### Accepted risk

**No hardware memory firewall** (accepted for now). Without page tables, a bug in
the runtime itself (interpreter/GC) or in native escape code can corrupt any
context — isolation is only as strong as the runtime's correctness. The safety
burden therefore concentrates in **(1) the per-context GC's correctness and (2)
the primitives' bounds checks** — those stand in for the MMU and deserve the
disproportionate share of review/verification effort. Keeping the runtime small
(e.g. the macro/`call/cc` cut) directly shrinks this trusted surface. An optional
hardware-ring boundary for untrusted contexts can be added later without changing
the common path.

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
- **Explicit per-context execution state** (trampolined / explicit value +
  continuation stack, not C-stack recursion) — *prerequisite* for the process
  model (safe-point preemption, green-thread suspend/resume, precise per-context
  GC). *(done — "K1")* `eval.c` is now an explicit-stack **CEK abstract machine**:
  the execution state (control expr, env, accumulator, and a heap-linked chain of
  continuation frames) lives in a `lisp_ctx_t` heap object, not on the C stack.
  Tail calls keep O(1) frames (the analogue of the old `goto tail`); deep non-tail
  recursion grows the heap, not the C stack. A per-slice **reduction budget**
  (charged at every call + loop back-edge) lets a context **suspend at a safe
  point and resume** (`lisp_ctx_make`/`lisp_ctx_resume`), and its state is a
  precise GC root (survives collection while parked). `lisp_eval`/`lisp_apply`
  are now thin wrappers that drive a context to completion, so all existing
  semantics + the host suite/conformance are unchanged. A bytecode VM, if built,
  sits on top as a pure-perf layer; the bytecode itself stays deferred until
  profiled-necessary.
  - **Tagged special-form dispatch** *(done; `eval.c`)* — a first, cheap step in
    that direction that needs no IR. Form heads were classified by a linear
    `is_form` name-compare cascade, so an ordinary application fell through *every*
    special-form check before reaching the apply path. Each form's interned head
    symbol now carries a small `form_id` (a byte in `lisp_named`'s padding, tagged
    once at startup by `lisp_init_special_forms`, read-only after the boot freeze),
    and `step_eval` dispatches via a `switch` jump table. Relies on symbols being
    interned (a form head is always the one canonical tagged object); `SF_NONE`
    falls through to application. ~11% on a worst-case application-bound loop that
    is otherwise GC-bound — i.e. the dispatch slice itself got materially cheaper,
    confirming the bigger remaining levers are allocation/GC and the assoc-list
    env walk (lexical addressing), not dispatch. `test_dispatch.c` differentially
    checks the id classification against a name oracle and covers the
    form-name-as-variable edge (special only in head position).
  - **Profile-driven optimization pass** *(done; `eval.c`/`gc.c`/`prims.c`)* — a
    sweep of measure → edit → test (host bench + suite + memcheck + QEMU boot)
    iterations against the allocator and the call path, the parts profiling showed
    actually cost cycles (memory traffic), as opposed to instruction-count hot
    spots that are cycle-cheap (tag/null checks, a vectorized memset) and were
    tried and reverted as wall-clock-neutral. Landed, biggest first:
    (1) **adaptive GC threshold** — scale the next collection to the live-set
    size instead of a fixed 256KB, so a large live structure isn't re-scanned on
    every 256KB (a 500k list build went 49s → 3s; a 200k map/fold 13s → 1.4s,
    ending a near-quadratic blow-up); (2) **all-simple-argument calls evaluated
    inline** with no continuation frame — when every argument is a symbol or
    literal (the bulk of calls) there is nothing to suspend, so no `K_EVAL_OP`,
    `K_EVAL_ARGS`, or "done" list is allocated; (3) a **2-argument fast path**
    (`K_ARGS2`) that keeps the first evaluated arg in the frame's own slot rather
    than a heap "done" cons; (4) **skip the operator-eval frame** for symbol-headed
    calls; (5) **bind parameters directly** (no redefine-check on a fresh frame);
    (6) **reuse one execution context** across `map`/`for-each` elements instead
    of one per element. Net on the compute/cons benches: ~2.2× (and the GC-bound
    workloads far more). The dominant remaining costs are the assoc-list **env
    lookup** (wants lexical addressing) and **GC marking of live data** (wants a
    generational collector) — both deliberately deferred as larger changes.
- **Cooperative scheduler over contexts** *(done — "K2", host-first; `sched.c`)*.
  A `lisp_sched_t` round-robin scheduler runs contexts as green-thread
  "processes", each resumed for a reduction slice and preempted at a safe point —
  so an infinite loop cannot wedge the scheduler. Primitives `spawn`/`yield`/
  `send`/`recv` give the language-level process model; **copy-on-send** deep-copies
  messages (shared-nothing IPC; interned symbols are the shared-immutable region,
  not copied), and `recv` blocks via a Lisp loop over a non-blocking mailbox +
  `%block`/wake. Kept out of `lisp_default_env` (no scheduler dependency in the
  base language). The globals (current scheduler/context) become per-core in the
  kernel.
- **Per-context GC + shared-immutable region** *(done — "K3", host-first; `gc.c`)*.
  The collector is de-globalized into a `lisp_heap_t`. One shared **system heap**
  holds the shared-immutable region (interned symbols), the global env/prelude, the
  scheduler structures, and the context objects; it is collected conservatively
  (C stack + intern table). A scheduler context may own a heap holding only its
  transient working data, collected **precisely** from its CEK registers and only
  at a safe point (the interpreter loop drives it between reductions, where no root
  is stranded in a C temporary) — so a collection pauses only that one context. The
  cross-heap firewall: `mark_push` marks only objects of the heap being collected
  (an external pointer is marked-and-stopped), so a collection never traces or
  sweeps another heap. Soundness rests on the one-way discipline — a context heap
  may point into the immutable system heap but never the reverse: symbols and the
  context/scheduler objects allocate into the system heap, and messages are
  deep-copied **into the receiver's heap** on send. A context with its own heap
  must therefore treat shared environments as read-only (mutating a shared binding
  is the one unsound operation; the frozen-shared-env of the full process model
  removes that footgun).
- **Kernelization — the runtime runs in the OS** *(done — "K4"; `modules/SysLisp`)*.
  The host-proven runtime is wrapped as a signed `Sys*` module that links the
  `lisp` static library, points its output sink at the COM1 debug log (`print_str`),
  and runs an in-OS self-test at load. It loads from **`servicescript`** (which runs
  as a task) rather than `loadscript`: the boot thread never returns past
  `SysTaskMgr`'s scheduler handoff, and — per the FP rule — the runtime's hardware
  doubles are only legal in task context. The self-test exercises the whole stack
  in the kernel: eval/recursion/higher-order, flonums (proving SSE works in task
  context), `display`→serial, GC survival, a foreign-function `(uptime-ns)` calling
  the kernel timer, and the cooperative scheduler + copy-on-send + per-context GC
  (producer/consumer). Two bare-metal fixes were needed: the SSE TUs build with
  `-mstackrealign` (the `-mno-sse` kernel call path doesn't keep the 16-byte stack
  alignment SSE assumes) and avoid aligned `.rodata` SSE constants (the module
  loader doesn't 16-align sections — kept the flonum path scalar, e.g. a signed
  `int64` integer-part cast in the printer). **Deferred to K5:** the native-ISR →
  event → wake-context bridge, which needs the scheduler to be the persistent
  per-core loop (the blocking `recv`/wake software path it rides on is already
  proven in K2).
- **Persistent Lisp scheduler + ISR-wake bridge** *(in progress — "K5a"; `modules/SysLisp`)*.
  The runtime is now a **long-lived kernel task** (created by `module_init`) that
  owns the env + scheduler and runs the Lisp scheduler loop persistently, rather
  than a one-shot self-test. It runs as a task **alongside** SysTaskMgr (which
  still schedules the OS's native tasks) — proven by its self-test output
  interleaving with the boot's driver loads — so the system stays fully bootable
  while the native task model is dismantled incrementally. The native-ISR → event
  → wake-context **bridge** is wired: a minimal native ISR (interrupt context, no
  allocation) bumps an event counter and calls the ISR-safe `lisp_ctx_wake`
  (a single word write) to mark a parked context runnable; the scheduler task
  sleeps on the counter via `task_monitor` and re-runs on the event. (The
  synthetic timer source for the in-OS demo isn't acquirable under QEMU here — the
  HPET interrupt sub-timers aren't free and the APIC local timer is the OS
  scheduler's — so the bridge's first real exercise will be a migrated driver's
  own IRQ.) **Plan (chosen 2026-06): incremental, stays bootable** — turn the
  drivers off, then migrate them to Lisp contexts one at a time (each driver's IRQ
  driving its Lisp context through the bridge), and delete SysTaskMgr's native
  scheduler LAST, once nothing native remains.
- **Drivers off → minimal boot** *(done — "K5b")*. `servicescript` stripped to the
  Lisp runtime; boots on 15 modules (Sys* infra + SysLisp), no native driver tasks.
- **The interpreter IS the per-core scheduler (single core)** *(done — "K5c")*. The
  flip: `SysTaskMgr.module_init` does the per-core bring-up (TLS + interrupt stack)
  then **returns** instead of arming the 50µs preemption timer and handing off to
  `task_switch_handler`. The boot script then `LOAD`s SysLisp and `CALL`s
  `lisp_scheduler_enter`, which **never returns** — the boot thread itself becomes
  the per-core scheduler loop. There is no native task switcher underneath: this
  one native thread runs Lisp contexts, which context-switch among themselves at
  safe points via the explicit-stack machine, and FP is safe because nothing
  preempts to clobber SSE state (exactly the design's claim). SysLisp dropped its
  dependency on the native task API entirely. The self-test runs in this loop and
  passes (11/11); the system then idles awaiting events. (The native scheduler code
  — `select_next`/`task_switch_handler`/`task_yield_stage2`/the per-switch
  `fxsave`/`cr3` — is now dormant and deleted once nothing native remains.)
- **One Lisp scheduler loop per core (multi-core)** *(done — "K5d")*. The BSP does
  the one-time global runtime init (concurrency hooks + GC + shared env) **single-
  core**, runs the self-test while the system collector is still live, then goes
  multi-core: it **freezes** the shared system heap (`lisp_gc_set_multicore(1)` —
  its conservative collector can't see another core's stack, so it becomes grow-
  only; interned symbols are permanent anyway and post-boot system-heap churn is
  negligible) and releases the APs via `mp_set_ap_entry` into a Lisp entry that
  sets an interrupt stack and runs the **same** per-core scheduler loop. Each core
  runs its own scheduler over its own contexts in their own **precisely-collected
  per-context heaps**; the only cross-core shared state — the system heap's object
  list/counters, the intern table, and the GC mark scratch — is guarded by one
  runtime lock (a plain spinlock: only the task-context scheduler loops take it,
  never the event ISR). The lock + core-id are injected into the lib via
  `lisp_set_concurrency(lock, unlock, core_id)`, keeping the lib freestanding and
  host-testable (`test_smp.c`); `g_sched`/`g_current`/`g_alloc_heap` became per-core
  arrays indexed by the core id (APIC id in the kernel, 0 host-side). Cores are
  independent islands — cross-core messaging is deliberately out of scope here
  (a context handle never crosses cores, so `send` can't target another core's
  context). Verified booting 2 and 4 cores under QEMU: every core comes online,
  runs a real GC-exercising computation on its own scheduler to the correct result,
  and idles cleanly; the self-test still passes 11/11 in the single-core phase.
  **Known first-cut limitations** (correctness is solid; these are performance, per
  the plan's "global lock first, revisit if contention shows"): (1) the one runtime
  lock also guards the GC scratch, so per-context collections *serialise* across
  cores though the heaps are disjoint — per-core scratch would parallelise them;
  (2) the kernel allocator (`SysMemory`) is O(n) best-fit with no free-list
  coalescing (`mem_compact` is a TODO), so heavy multi-core GC churn degrades
  super-linearly. *(Next: migrate drivers to Lisp one by one, each driver's IRQ
  driving its context through the ISR-wake bridge; delete the dormant native
  scheduler; later, per-core GC scratch + cross-core messaging.)*
- **Namespaced modules — multi-file programs + libraries** *(done; `module.c`)*.
  Two special forms layer over the flat global namespace. `(define-module NAME
  (export …) body…)` evaluates its body in a *fresh* env parented on the global
  env — so its internal `define`s stay private — then publishes the listed
  bindings as NAME's exports. `(import SPEC …)`, where a SPEC is `name`,
  `(name (prefix p:))`, or `(name (only a b))`, loads each module **once**
  (idempotent) and binds its chosen exports into the caller's env, optionally
  renamed so same-named exports from different libraries coexist. Source is
  fetched by name through a pluggable loader hook (`lisp_set_module_loader`):
  the kernel maps a name to `./lisp/<name>.clp` in the initrd, host tests to an
  in-memory table; the returned byte range need not be NUL-terminated (the
  reader is bounded). The registry of loaded modules lives **inside the global
  env** (a hidden `%modules` binding), not a C static — the system collector
  roots conservatively from the C stack + intern table, so anything hung off the
  always-reachable global env survives for free while a static `lisp_value`
  would be collected; the price is that loading must run in the **single-core
  boot window** (before `lisp_gc_set_multicore` freezes the system heap), the
  same rule top-level `load_clp` already follows. A `%loading` sentinel detects
  circular imports; a load that fails partway tombstones its registry entry so a
  retry reports the real error. `test_modules.c` covers the semantics on the
  host (20/20); in the OS, `virtio_net.clp` is the first multi-file program —
  it pulls its generic helpers (`nth`, the mutable word `cell`) from a
  `driver-util` **library** via `(import driver-util)`, and still completes the
  ARP round-trip end-to-end.
- **Macros + call/cc — implemented then CUT** (see Scope above). Not on the
  roadmap unless a concrete need (driver/IPC DSL; coroutines) brings them back.
- **Next — Kernelization.** Wrap as a signed `Sys*`/`Core*` module against
  `common/`; wire the output sink to DEBUG_PRINT; host-function FFI to expose
  existing C servers; the native-ISR → event → wake-context bridge.
- **Then — the process model** (interpreter-as-scheduler, contexts as processes,
  per-context GC, async-yield for long native ops — see *Process model &
  scheduling* above), **capabilities** (W7-style environments), the atomic box
  (for the rare cross-core shared cell), **driver MMIO/bytevector primitives**,
  the **single-level-store persistence layer**, the **foreign-native jail**.

Each phase is independently reviewable. Moving/persistent GC, JIT, and persistence are
explicitly later phases the earlier ones are designed not to preclude.

## Testing

Per-phase host harnesses (`libs/lisp/test/`) plus a thunk-based
`(test expr => expected)` corpus (`conformance.clp`, ~70 cases) exercise the
whole language host-side. Full R7RS conformance (the chibi `r7rs-tests.scm`
suite) is **not** a goal — this is a Scheme-*inspired* Lisp, not R7RS; the suite
was only ever a correctness check, which the host harnesses provide. (It would
also require `define-syntax`/`call/cc`, which were cut, and the full numeric
tower.)
