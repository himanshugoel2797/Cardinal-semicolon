<!--
 Copyright (c) 2026 Himanshu Goel

 This software is released under the MIT License.
 https://opensource.org/licenses/MIT
-->

# Base Lisp: bytecode reorganization (design + plan)

Status: **proposed**. This note records the design decided in discussion before
the implementation lands, so the interpreter change is reviewable up front.

## Why

The base Lisp interpreter (`libs/lisp/`) is a well-built tree-walking CEK
machine — tagged unboxed fixnums, interned symbols, a hash-backed global frame,
proper tail calls, a slab allocator — yet still ~1000x off C. Profiling and a
corpus survey agree the gap is **structural to the dynamic model**, not sloppy
interpretation:

- every closure call allocates a fresh `lisp_env_t` **plus one cons per
  parameter**, then GCs them (`bind_params`, `eval.c`);
- local variable reference is a **linear assoc-list scan per frame**
  (`frame_find`); only the global frame is hashed;
- non-tail calls heap-allocate continuation frames;
- argument lists are cons-chained, reversed, reversed back into an array.

The shader tier already proved the lever: it is fast *because it dropped
dynamism* (static types, flat regions, no GC, no closures). We are **not** making
the base language static — that is the shader's job. Instead we reorganize the
base language's **execution representation** so it maps to the CPU and is a clean
JIT target, while keeping full dynamic typing and the Lisp feel.

## What we are optimizing for (and what we are not)

The differentiating value of this OS's Lisp is **universal liveness/reflection**
— the debugger is a more-privileged REPL, every context is inspectable, code is
hot-swappable. That is the crown jewel and must survive. Homoiconic
metaprogramming is *not* what we are protecting:

- **Drop:** runtime macros (none exist in the corpus), `call/cc` (never exposed),
  runtime `eval`/in-body `define` for *compiled modules*. The REPL/debugger keep
  a resident compiler — see Reflection below.
- **Keep:** s-expression surface (for now — a C/Python-like surface is a separate,
  optional, surface-only project that compiles to the same bytecode), dynamic
  typing, closures, the context/capability/scheduler process model.

We are not trying to beat clang. Target: the base language goes from ~1000x off C
to a small multiple of the current interpreter, and becomes a flat IR a future
machine-code JIT can consume (the shader already shows a register-bytecode -> x86
emitter is small and testable). Hot numeric loops stay the shader's job.

## Language definition (the contract a linter enforces)

The discipline lives in the language definition, not in a clever compiler. A
checker pass (`the linter`) *is* the spec; complexity stays in the spec.

1. **Frozen primitives.** Arithmetic (`+ - * / modulo remainder quotient`),
   comparison (`< <= > >= = eq? eqv? equal?`), bitwise/shift
   (`bitwise-* arithmetic-shift bit-extract bit-insert`), core pair ops
   (`cons car cdr null? pair?`), and the boolean/`not` core are **non-overridable**
   and compile to dedicated opcodes. The special forms (already non-overridable:
   `quote if define lambda set! begin let let* letrec and or cond when unless
   while case define-module import include`) are syntax, as today.
   Corpus violations: **0**. Rationale: overriding `+`/`car` is poor design, and
   freezing them is what lets the compiler emit a direct op instead of a global
   lookup + indirect call.

   Frozen primitives remain **first-class values** (so `(map + xs)` could work):
   a literal operator in head position inlines to an opcode; used as a value, the
   real procedure object is still available.

2. **Immutable lexical bindings + explicit cells.** Bindings are single-assignment.
   Mutable state is an explicit heap object — already the universal idiom here:
   `cell` is a 3-line prelude wrapper over a mutable `bytes` buffer
   (`driver-util.clp`), and there is **zero** `set!`-on-a-lexical-variable in the
   corpus. Immutable bindings mean closures capture by value into a **flat upvalue
   array** with **no escape analysis** — the single biggest simplification, and it
   costs zero migration. `set!` stays valid on globals/REPL (needed for live
   redefinition); a `set!` on a local is permitted but de-opts that local to a
   boxed cell (no corpus code hits this).

3. **Dynamic typing stays.** Values remain the existing tagged `lisp_value`;
   frozen ops still type-check operands at runtime (or trap). Types are the
   shader's concern, not the base language's.

## Execution model (target)

Replace the tree-walk over the cons AST with a **compile step** to a flat,
register-based bytecode, run by a threaded VM:

- **Per-closure chunks.** A `lambda`/`define` compiles to a `chunk`: a flat
  instruction array over virtual registers, with locals resolved to lexical
  addresses (`depth`,`slot`) at compile time — no `frame_find` scan at runtime.
  A closure value becomes `chunk + flat upvalue array` instead of
  `(params, body, env-chain)`.
- **Register bytecode**, mirroring the shader's IR (shared mental model and
  tooling): `CONST/MOV/LOADL/LOADU/LOADG/CALL/TAILCALL/RET/JMP/JMP_IFNOT` plus a
  frozen-op family (`ADD/SUB/LT/EQ/CAR/CDR/CONS/...`). Lowering from the existing
  special forms is mechanical because none of them have a hairy lowering once
  bindings are immutable and macros/`call/cc` are gone.
- **Globals as repatchable slots (the "compiled jump table").** Each global is a
  table slot; a call to a user global is an indirect call through its slot, so
  **live redefinition just repatches the slot** without de-optimizing call sites.
  Frozen primitives need no slot (inlined).
- **Threaded VM loop** (computed-goto where available): the classic 2-3x over a
  switch VM, fully maintainable, no machine-code emission.

### Invariants that must be preserved (the process model rides on these)

The CEK machine is the OS process model, not just an evaluator. The bytecode VM
keeps every contract:

- **Suspend/resume at safe points** (`lisp_ctx_resume(budget)`): an instruction
  boundary is a cleaner safe point than a CEK reduction. `send`/`recv` blocking
  and the scheduler slice are unchanged.
- **Precise GC roots:** the VM's value stack + frame registers + chunk constants
  are the precise root set of a suspended context (today: control/env/accum/kont).
- **Per-context heaps, capabilities, copy-on-send isolation, killability** —
  unchanged; they live above the evaluator.

### Reflection (the crown jewel) — explicitly preserved

- **Resident compiler.** The REPL and the `sys-debug` capability invoke the same
  compiler at runtime to compile a fresh expression against a live context's
  environment and run it. Closed-world applies to *compiled modules*, not to the
  REPL/debugger side-door. So "the debugger is a privileged REPL" still holds.
- **Debug metadata kept off the hot path:** per-chunk `slot <-> name` and
  `pc <-> source` tables so a paused frame shows/evaluates locals by name. The
  global symbol table (already present) gives name->slot for globals. `ctx-list /
  ctx-pause / ctx-step / ctx-control / ctx-value` keep working; stepping is now
  per-instruction.

## Phased plan (each phase independently shippable + tested)

The correctness gate throughout: a **differential test** runs the new path and
the current tree-walker on the same forms and asserts identical results — the
same technique that guards the shader JIT. The existing `libs/lisp/test` suite
and the in-OS boot are the regression gate.

- **P0 — Spec + linter.** This note + a checker pass enforcing the contract
  (frozen-op rebinding, lexical `set!`, `call/cc`, in-body `define`/`eval` in a
  compiled module). Must flag 0 sites on today's corpus. *Additive, no behavior
  change.*
- **P1 — Compiler: cons AST -> register chunk** with lexical addressing and flat
  upvalue capture. Lower every existing special form. Differential-test the
  compiler output by interpreting chunks with a reference chunk-walker.
- **P2 — Threaded bytecode VM** executing chunks under the `lisp_ctx` contract
  (budget/suspend/precise roots). Switch `lisp_eval`/`lisp_ctx_resume` to compile
  then run. Tree-walker stays available behind a flag for differential testing.
- **P3 — Globals as repatchable slots + frozen-op inlining finalized + reflection
  metadata.** Verify `sys-debug` stepping and live redefinition in-OS.
- **P4 — Measure + tune**; document the speedup; leave the chunk as a documented
  JIT input for later (shares the shader emitter's approach).

## Prototype status (what's built)

`libs/lisp/test/lbc.inc` + `test/test_bytecode.c` are a first working cut,
host-only (the `.inc` is `#include`d by that one test, so it never enters the
kernel build or other tests). It implements P1+P2 for a covered subset and
proves the architecture end-to-end:

- **Covered:** literals, variable refs (local/upvalue/self/global), `if`,
  `begin`, `lambda` (incl. rest args), internal `define` (self-recursive),
  `let`/`let*`/named-`let`, `and`/`or`, `when`/`unless`, `cond`, `set!` on a
  non-captured local/global, the frozen ops (`+ - * < <= > >= =` with exact
  prim fallback for non-fixnums; `cons car cdr null? pair? not`), and calls to
  any existing primitive/prelude procedure via `lisp_apply`.
- **Declines to the oracle:** `letrec`, `case`, `while`, `quasiquote`,
  `define-module`/`import`/`include`, n-ary frozen ops (routed as exact calls),
  `set!` of a captured/`self` binding, and forward/mutual *local* recursion
  (top-level mutual recursion works -- globals resolve at call time).
- **The model in practice:** the prototype confirmed two design bets cost zero:
  immutable bindings make upvalue capture a by-value copy (no escape analysis,
  no cells), and self-recursion via an `OP_SELF` slot keeps named-`let` loops in
  O(1) frames without a recursive binding.

Validation (`test/build-and-run.sh`, picks it up automatically):

- 39-case curated differential corpus: all match the tree-walker (or both error),
  2 forms correctly decline, 0 failures.
- A random-expression **fuzzer**: 20000 exprs, every one either value-matches or
  error-matches the tree-walker. It found a real `and`/`or` double-pop bug.
- Microbench (2,000,000-iteration tail loop): the bytecode VM runs **~18x**
  faster than the tree-walker, with no machine-code emission (a threaded stack
  VM). NOTE: the prototype VM is **stack-based**; the register form the spec
  targets (to match the shader IR and ease a future machine-code JIT) is a
  backend follow-up -- stack->register is a known, local transform.

**Churn measurement (the C-ABI question).** A loop doing an inlined `(+ a b)`
opcode vs the same add forced through the call path (3-arg `+`, routed as
`LOADGLOBAL`+`CALL`), 2M iters x 2 adds:

| call path (per add) | ns/add |
|---------------------|-------:|
| inlined opcode (no call) | ~57 |
| `lisp_apply` + global hash lookup (original) | ~262 |
| thin direct-`->fn` + global hash | ~176 |
| thin direct-`->fn` + global **slot** | ~103 |

Conclusions: (1) **inlining dominates** -- a call is several times an inlined op,
so freezing+inlining the hot ops is the main lever; (2) for the calls that
remain, **bypassing the C ABI is worth ~1.5x** -- calling a primitive's C `fn`
directly with the operand-stack slice (zero-copy args, one err branch) instead of
routing through `lisp_apply` (the full CEK machine) saved ~86 ns/call; (3)
**globals-as-slots is worth ~1.7x** -- resolving a global to its binding cell once
at compile time and loading it with a single `cdr` (the cell is a stable slot:
define/set! mutate its cdr in place, never replace it, so it doubles as the
live-redefinition jump-table) instead of a per-call hash lookup saved ~73 ns/call.
Combined, thin `->fn` + slot is **2.54x** the original call path (262 -> 103
ns/call) and cuts the residual call overhead over an inlined op from ~101 to
~46 ns. Both are now the prototype defaults (all corpus + fuzz tests stay green).

Next (P3+): widen coverage (the declined forms), then the reviewed step of
wiring the compiler into the live `lisp_eval`/`lisp_ctx_resume` under the
suspend/precise-GC contract, globals-as-repatchable-slots, and the reflection
metadata. Until then the kernel keeps using the tree-walker.

## Non-goals

- Not statically typed (shader's job). Not a machine-code JIT yet (the bytecode is
  its future input). Not a surface-syntax change (separate optional project). Not
  beating a mature C compiler.
