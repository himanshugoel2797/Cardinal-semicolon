<!--
 Copyright (c) 2026 Himanshu Goel

 This software is released under the MIT License.
 https://opensource.org/licenses/MIT
-->

# Cardinal; shaders: a typed, compiled, restricted Lisp tier

A companion to [`lisp-substrate.md`](lisp-substrate.md). That note establishes a
kernel-resident Scheme as the system substrate and leaves two holes open:
"optional type hints + `unchecked-` ops for unboxed inner loops" and "unboxed
typed arrays / bytevectors + MMIO register-block primitives that open-code to a
single load/store" (its *Performance → Hot Lisp approaching native* section),
plus the *Drivers* split of "control-plane-Lisp / data-plane-native." This note
fills those holes with one mechanism: **shaders** — small, statically-typed,
allocation-free Lisp kernels compiled to a verified bytecode and run in a
heavily restricted environment, in the spirit of early GPU shader models.

This records the **destination**, the **reasoning**, and the **roads not taken**,
before any code — the same discipline the substrate note followed.

## The thesis: a third managed-but-restricted tier

Today the model is two tiers:

- **Trusted managed Lisp** — safe by construction, but cannot enter contexts with
  no GC / no allocation / no FP-save: ISR context, code holding `cli()`, line-rate
  hot paths.
- **Untrusted native C** — goes anywhere, trusted only by its signature (`.celf` +
  `VerifyModule`).

A shader is the **missing third tier in between: trusted, managed, restricted,
compiled, allocation-free.** It can go where full Lisp cannot — *precisely
because it removes the features that made full Lisp unsafe there.* No heap
allocation ⇒ no GC ⇒ safe under `cli()` and in ISR context. No boxed flonums in a
register-typed loop ⇒ no `-mno-sse` violation. Statically-bounded loops ⇒ a
computable worst-case cost ⇒ safe to run non-preemptibly.

Speed is the obvious benefit; the larger one is that this **dissolves the "data
plane must be C" rule** the substrate note accepts for drivers. There are three
use shapes, not one:

1. **Driver data-plane hot loops** — descriptor/packet parsing, register poking
   at line rate (rtl8169, xHCI, AHCI), surfaced to the control-plane Lisp half as
   a fast compiled primitive.
2. **ISR-context micro-ops** — the small, bounded, allocation-free work a native
   ISR stub does today, now expressible in the managed tier (constant or
   caller-clamped cost bound; see *Cost model*).
3. **Data-parallel compute kernels** — framebuffer blits, checksums, codecs,
   crypto, pixel ops. This is where the explicit SIMD vector types earn their
   keep, and the most compelling demo: a SIMD framebuffer blit written in Lisp.

### Why this is *not* "the deferred JIT, done early"

The substrate note's deferred bytecode-VM/JIT is "make all of dynamic Scheme
transparently faster" — closures, deopt, inline-cache invalidation on
redefinition, full dynamism. Large, subtle, and it grows the trusted surface.
Shaders are the opposite, complementary bet: carve out a tiny **monomorphic,
closed, non-dynamic** sublanguage that is *easy to compile well and easy to prove
safe*, and make the boundary explicit — you cross it by **calling** a compiled
shader with typed buffers. This is the Terra-vs-Lua / typed-vs-untyped-Racket /
`@numba.jit(nopython=True)` seam. It respects the project's load-bearing value —
*keep the trusted runtime small* — in a way a general JIT does not, and it unlocks
the ISR-safe managed data plane the general JIT would not give for free. The two
can later share an IR/backend, but they are distinct tiers with distinct goals.

## The language

A shader is a closed, typed term: a fixed typed parameter list, a typed return,
and a restricted body. It is defined alongside ordinary code and named like any
other binding; the dynamic Lisp side holds the compiled shader as an opaque
capability value and invokes it by application.

```scheme
(defshader ip-checksum ((buf (bytes u16)) (n u32)) -> u32
  (let loop ((i 0) (acc 0))            ; named-let is the one bounded-loop form
    (if (>= i n)
        (u32 (fold-carry acc))
        (loop (+ i 1) (+ acc (u16-ref buf i))))))   ; i provably < n
```

### Type system

Monomorphic, unboxed, statically annotated on parameters and the return; locals
are inferred.

- **Scalars:** `u8 u16 u32 u64 i64 f32 f64 bool`.
- **Typed regions:** a `bytes` viewed with a known element type and an
  SSA-tracked length — `(bytes u16)`, `(bytes f32)`, etc. Every region access is
  bounds-checkable to a single (statically-elided where provable) load/store.
- **Vectors:** a single fixed-width `<N×T>` lane family. Geometric vectors
  (`vec2`/`vec3`/`vec4` of `f32`) and packed SIMD lanes (`f32x4`, `u32x4`,
  `u8x16`, …) are **the same machinery** — `vecN` is named sugar over the packed
  family. Fixed width only; see *Roads not taken* for scalable vectors.

The type system's job is not ergonomics — it is to make the safety proof cheap.

### Banned in a shader body

Anything that allocates, dispatches dynamically, or names ambient authority:
`lambda`-capture, `define`, `cons` / `make-bytes` / any allocation, `import` /
`spawn` / `send`, `eval`, strings, variadics, recursion, unbounded loops. The
permitted forms are `let` / `let*` / `if` / `cond` / `and` / `or` / `when` /
`unless`, named-`let` as the single bounded-loop form, arithmetic, region
accessors, vector ops, and calls to other shaders.

### Bounded loops + cost model

The only loop form is a named-`let` whose trip count is statically bounded:
either a compile-time constant or a function of an input parameter. **Early exit
is allowed; it is not specially accounted — the computed cost is the worst case.**

The cost ceiling therefore comes in two regimes, and the verifier picks by where
the shader is invoked:

- **Cooperative-scheduler / compute use** — an *arg-dependent* bound is fine; the
  scheduler charges the worst case for *this* call's arguments.
- **ISR / `cli()` use** — requires a *compile-time-constant* ceiling or a
  caller-supplied clamp, because interrupt context cannot tolerate "however many
  iterations the argument implies."

## Architecture: four layers, ISA opcodes quarantined in one

The central design constraint is **SIMD without baking architecture-specific
opcodes into the IR or the trusted core.** This is the SPIR-V / WASM-SIMD /
LLVM `<N×T>` / .NET `Vector128<T>` playbook, scoped down to early-shader
functionality. The discipline is a strict layering:

1. **Frontend** (host *or* VM): restricted Lisp → typed IR with *abstract* vector
   ops. Portable.
2. **Verifier**: types, region bounds, bounded-loops, the capability whitelist —
   on the *abstract* IR. Portable. **This is the trusted moat** (it stands in for
   the absent MMU, exactly as the substrate note assigns that role to the GC and
   primitive bounds-checks). Runs identically host-side and in-OS.
3. **Reference interpreter**: executes the abstract bytecode directly, with a
   **scalar lowering for every vector op**. Portable, always available, and
   doubles as the **semantic oracle**.
4. **Backend(s)**: the *only* ISA-specific layer. Lowers abstract vector ops to
   SSE/AVX on x86, or a scalar unroll. Optional, per-ISA, behind a clean
   interface.

**Architecture-specific opcodes live only in layer 4. Layers 1–3 — the trusted
*and* the portable parts — never name `VADDPS` or `PSHUFB`.** The IR says
`add.f32x4`, `splat`, `dot`, `shuffle-const`, `reduce-add`, `cmp→mask`,
`select` / `blend`; the backend pattern-matches those to whatever the host has.

Three invariants make this hold:

- **Every abstract vector op has a mandatory scalar lowering** (a lane loop with
  an obvious definition). A shader is therefore *always runnable* — on a target
  with no SIMD, in the plain interpreter, or on the host — just slower. **SIMD is
  a performance property, never a correctness or availability one.** This is what
  makes "run in the VM too" and "do as much as possible on the host" true, and it
  yields a free differential test the codebase already leans on:
  `scalar-interpreter result == backend result`, bit-for-bit.
- **The op set is restricted to clean scalar semantics:** lane-wise arithmetic,
  splat/broadcast, **compile-time-constant** shuffles/swizzles (`.xyzw`),
  reductions (sum/min/max/dot), comparisons→masks, select/blend. Constant-index
  shuffles are the sweet spot — portable meaning, and a backend may still emit
  `PSHUFB` for them. No host-exotic ops in v1.
- **Fixed-width lanes; the backend legalizes width.** `f32x4` lowers to one
  `VADDPS` on x86, two narrow ops on a 64-bit-SIMD target, or a 4-iteration
  scalar loop with no SIMD at all. A wider packed type (`u32x8`, `u8x16`) is
  split by the backend on smaller targets. Width-in-the-type ≠ ISA-in-the-bytecode.

## Capabilities: the closed typed signature is W7 at its limit

A shader is a closed term over an explicit typed parameter list. It can touch only
the buffers **handed to it** and call only primitives **on its whitelist** — it
has **zero ambient authority by construction.** No `import`, no `mmio-map` of
arbitrary physical memory, no `spawn`. This is the W7 lexical-capability model
(see the substrate note) taken to its limit: the parameter list *is* the
capability list, and it is *static*. A driver's hot path is a shader its
control-plane module imports and invokes; the module boundary stays the capability
boundary, unchanged.

## Compilation, trust, and where it runs

- **Compiled at first execution.** A shader compiles to bytecode lazily on first
  invocation, not AOT at build and not eagerly at module load. The bytecode is an
  **internal cache, not a distribution artifact** — there is no shipped `.clpb`
  format to keep stable (see *Roads not taken*).
- **Verify on use, wherever compiled.** Whether a shader was compiled on the host
  (for tests) or in the VM, the **verifier runs before the bytecode executes.**
  This mirrors the existing `.celf` trust boundary exactly: `sign_exec` builds the
  artifact on the host, and the kernel still runs `VerifyModule` before loading.
  Compile anywhere; *verify before run, always.*
- **Host-first + VM.** The frontend, verifier, and reference interpreter are
  freestanding C that builds **both** host-side (under `libs/lisp/test/`-style
  harnesses) and into the in-OS runtime — the substrate note's "prototype the
  runtime host-first" discipline. The backend is the per-target addition.

### Placement

A **sibling library, `libs/lisp_shader`, depending on `libs/lisp`.** It reuses the
reader, the value/type representation, and the host test harness, but the
**verifier is the security boundary and deserves to be a separately-reviewable
unit** — the same instinct that makes `module_lib` (the `.celf` header
build/verify) its own lib rather than code scattered through the loader.

## Documentation: separate, cold, and hash-guarded against drift

Documentation is paired with shaders deliberately: in a homoiconic, reflective
system the **typed contract and the documentation are the same artifact**, and the
typed tier makes the machine-checkable half automatic.

Two doc layers with different drift-resistance:

- **The type signature / contract is *derived* from the code.** It cannot drift;
  it is free. The system renders it, checks it, and can drive the object-browser UI
  from it.
- **The prose docstring is *written* alongside the code.** It can drift — so it is
  the only thing that needs guarding.

Mechanism for the prose half:

- **Authored inline** with the code in the source `.clp`.
- **A build step extracts docs** into a separate, **cold** s-expr sidecar
  (`lisp/<module>.doc.clp`), each record keyed `{symbol, source-hash, prose}`. The
  sidecar is never faulted into the heap unless the object browser asks — so docs
  cost no resident kernel RAM (the reason for a separate file at all).
- **The hash is per definition, over its reader-canonical form** (`read`→`write`
  then hash), not the raw text and not the whole file. Reformatting / whitespace /
  comment-only edits (e.g. an `astyle` pass) do not trip it; any *structural*
  change to a single def trips only that def's record.
- **Two gates, two severities.** The **build step is strict** — it fails the build
  if a doc record's hash no longer matches its definition. The **interpreter is the
  safety net** — it recomputes the hash at module load and **warns very visibly** on
  mismatch, catching hand-edited sources and the initrd overlays this project uses
  for swap-free iteration. Build = error; runtime = loud warning.

This mechanism is not shader-specific — it answers the general "documentation
within the OS" question for *all* Lisp definitions. Shaders are simply the case
where the derived half (the type signature) is richest.

## Decisions locked (2026-06)

- **Scalars** include both `f32` and `f64`.
- **Geometric and packed vectors are unified** as one fixed-width `<N×T>` family.
- **Fixed-width vectors only** — the IR is not designed for scalable vectors.
- **First compile target is typed bytecode + a verifier**, not native codegen.
- **Bounded loops**, early exit allowed, **cost = worst case** (no early-exit
  accounting).
- **`libs/lisp_shader`, a sibling depending on `libs/lisp`.**
- **Bytecode is an internal cache**, compiled at first execution — not a stable
  shipped format.

## Roads not taken (and why)

- **Scalable / width-agnostic vectors** (ARM SVE, RISC-V V, .NET `Vector<T>`) —
  one kernel that optimally fills SSE/AVX/AVX-512. Real power-up, but a heavier
  authoring and verification model, and it does not match the "early shader model"
  feel. Deferred; the fixed-width IR does not preclude adding it. The accepted cost:
  a fixed `f32x4` will not auto-exploit AVX-512's wider lanes — reach for a wider
  packed type for throughput, and let the backend split it on smaller targets.
- **Native codegen first.** Faster, but a much larger trusted surface (codegen +
  W^X memory + relocation) with no portable reference to differentially test
  against. Bytecode + a scalar reference interpreter is the small, reviewable, and
  *checkable* core; native JIT is a later layer over the *verified* bytecode.
- **Bytecode as a stable, signed distribution format** (host-compiled shaders as a
  shipping artifact). Rejected for v1: shaders compile at first execution, so the
  bytecode is an internal cache. No format-stability or artifact-signing burden
  until an in-VM compiler and the runtime are proven; verify-on-use already covers
  trust regardless of where compilation happened.
- **Separate "shader vectors" and "SIMD lanes" concepts.** Unified instead — both
  are `<N×T>`; one IR concept, `vecN` as sugar.
- **Docstrings resident in the heap.** Wasteful kernel RAM; the cold hash-keyed
  sidecar gives queryability without the resident cost, and is the right shape for
  the eventual single-level store (docs as cold objects, versioned with the code
  they describe via structural sharing).

## Phased plan (sketch)

Independently reviewable phases, earlier ones designed not to preclude later ones —
the substrate note's pattern.

- **S0 — Frontend + types + verifier (host-first).** `defshader` reader, the
  monomorphic type checker, region-bounds and bounded-loop analysis, the
  capability whitelist. No execution yet beyond type-checking. Host test harness.
- **S1 — Typed bytecode + reference interpreter.** The portable, scalar-lowered
  executor; the semantic oracle. Differential tests vs. equivalent dynamic Lisp.
- **S2 — Vectors (abstract ops, scalar lowering).** The `<N×T>` family end-to-end
  through verifier + reference interpreter, still scalar underneath.
- **S3 — x86 SSE/AVX backend.** The first ISA layer; differential-tested
  bit-for-bit against the reference interpreter. SIMD framebuffer-blit demo.
- **S4 — In-OS integration.** `libs/lisp_shader` wired into `SysLisp`;
  compile-at-first-execution; a driver data-plane hot loop and/or an ISR micro-op
  ported to a shader through the existing ISR→event→wake bridge.
- **S5 — Documentation pipeline.** Inline-doc extraction, the canonical-hash build
  gate, the cold sidecar, and the load-time mismatch warning — applied to all Lisp
  defs, not only shaders.
