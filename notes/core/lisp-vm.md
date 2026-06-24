<!--
 Copyright (c) 2026 Himanshu Goel

 This software is released under the MIT License.
 https://opensource.org/licenses/MIT
-->

# Base Lisp: compile-to-VM redesign (design + plan)

Status: **in progress** (branch `claude/lisp-vm`). This note records the design
decided in discussion before the implementation lands, so the interpreter change
is reviewable up front. It supersedes the earlier "bytecode reorganization" note;
the host-only prototype (`libs/lisp/test/lbc.inc` + `test/test_bytecode.c`) that
proved the architecture end-to-end is the starting point.

## Why

The base Lisp interpreter (`libs/lisp/`) is a well-built tree-walking CEK
machine — tagged unboxed fixnums, interned symbols, a hash-backed global frame,
proper tail calls, a slab allocator — yet still ~1000x off C. Profiling and a
corpus survey agree the gap is **structural to the tree-walk**, not sloppy
interpretation:

- every closure call allocates a fresh `lisp_env_t` **plus one cons per
  parameter**, then GCs them (`bind_params`, `eval.c`);
- local variable reference is a **linear assoc-list scan per frame**
  (`frame_find`); only the global frame is hashed;
- non-tail calls heap-allocate a continuation frame (`kont_push`);
- argument lists are cons-chained, reversed, reversed back into an array.

We reorganize the base language's **execution representation** so it maps to the
CPU and is a clean JIT target, while keeping full dynamic typing and the Lisp
feel. The prototype already measured **~18x** (stack VM) and **~27x** (register
VM) over the tree-walker on a tail loop, with no machine-code emission.

## Two decisions that shape this redesign

These were decided explicitly and **diverge from the earlier prototype's design**;
they are the spine of everything below.

### Decision 1 — Replace the evaluator outright (no fallback)

The compiler+VM becomes the **only** evaluator. There is **no decline-to-oracle**
path and the tree-walking CEK step machine in `eval.c` (`step_eval` /
`step_applyk` / the `K_*` continuation frames) is **deleted** once the VM is live.

Consequences:

- **100% form coverage is mandatory** up front: every special form the reader can
  produce must compile. The full set (all currently in `eval.c`):
  `quote quasiquote unquote unquote-splicing if define lambda set! begin let let*
  letrec and or cond when unless while case define-module import include`, plus
  closures, primitive calls, and `apply`. There are **no macros, no `call/cc`, no
  `eval` primitive** in this Lisp (confirmed by audit), so the grammar is closed
  and finite — this is what makes "replace outright" tractable.
- The differential test (new VM vs. the current tree-walker) is the bring-up gate,
  but the tree-walker is a **scaffold that is removed at the end**, not a runtime
  fallback. After P3 the only oracle is the prior git revision.
- The module loader stops doing nested `lisp_eval` of source; it **compiles each
  top-level form to a chunk and runs the chunk**.
- `sys-debug` stepping (`ctx-step`/`ctx-control`/…) is re-expressed against VM
  **instruction boundaries** instead of CEK reductions.

### Decision 2 — Do NOT freeze core ops

The earlier prototype froze `+ - * < car cons …` into non-overridable opcodes.
We **reject** that: every binding, including the arithmetic/comparison/pair core,
stays an ordinary, **redefinable** global. Overriding `+` must work.

So speed for the hot ops cannot come from the *compiler* emitting a hardcoded
`ADD`. It comes from the **VM**, via an **inline cache guarded on the binding
cell** (see "Inline caches" below): the fast path runs only while the operator
still resolves to its original builtin and the operands are the fast type; a
redefinition or an off-type operand deopts to a normal call. This keeps full
dynamism and recovers most of the inlining win.

This decision also pushes us off the prototype's by-value-upvalue shortcut: with
redefinable, mutable, and mutually-recursive bindings all on the table, captured
variables need **boxed cells** (see "Bindings"). That, in turn, *fixes* the two
things the prototype could not do — `set!` on a captured variable, and
forward/mutual **local** recursion (`letrec`, internal `define`).

## Execution model (target)

Replace the tree-walk over the cons AST with a **compile step** to a flat,
register-based bytecode, run by a threaded VM.

### Chunks and closures

A `lambda`/`define` compiles to a `chunk`: a flat instruction array over virtual
registers, with locals resolved to **lexical addresses** at compile time — no
`frame_find` scan at runtime. A closure value is `chunk + captured-cell vector`
instead of `(params, body, env-chain)`.

### Register bytecode

3-operand register instructions (dst, src/a, src/b), mirroring the prototype's
`rop` set: `LOADK MOVE LOADUP LOADG(slot) SETG(slot) CLOSURE JMP JMPF JMPT CALL
TAILCALL RET` plus cell ops `MKCELL CELLGET CELLSET` and the inline-cache call
sites `OPCALL` (see below). A local read is *free* (the register **is** the
local), so `(f x y)` needs no load instructions for `x`/`y`.

### Explicit, heap-resident frame + register stack (the load-bearing invariant)

The CEK machine's superpower is that the *entire* continuation is a heap object,
so suspend/resume, precise GC, and context migration are trivial. The VM keeps
this: the per-context call stack is an **explicit, heap-allocated, GC-scannable**
register array with frame windows — **not** the C stack. A Lisp call never
recurses in C. This is what preserves:

- **Suspend/resume at safe points** (`lisp_ctx_resume(budget)`): an instruction
  boundary is a cleaner safe point than a CEK reduction. The VM decrements the
  budget per call/back-edge and returns `SUSPENDED` at an instruction boundary.
- **Precise GC roots:** the VM's register stack + frame metadata + chunk
  constants are the entire root set of a suspended context (replacing
  control/env/accum/kont). No conservative scan of a C call stack.
- **Allocation-free frames:** register windows in one contiguous per-context
  array; a non-tail call places the callee window right after the operator slot
  (args **are** the callee's parameter registers, zero copy); a tail call moves
  args to the window base and reuses the frame. No per-call `malloc`, ever (a hard
  requirement for the kernel).

### Bindings: flat registers + boxed cells (no escape analysis hand-waving)

- A local that is **never captured** by an inner `lambda` stays a flat register
  (the common, fast case).
- A local that **is captured** becomes a heap **cell** (`MKCELL`): the register
  holds the cell, reads/writes go through `CELLGET`/`CELLSET`, and a closure
  captures the **cell by reference** (shared mutable state across the closure
  boundary, the correct semantics).
- `letrec` and internal `define` allocate the group's cells **up front**
  (initialized to `undef`) and then compile the initializers, so the initializers
  can reference each other → **mutual and forward local recursion work**.
- `set!` on a local forces that local to a cell (it is captured-or-mutated). On a
  global it patches the global slot (below).

The capture analysis is a single pre-pass over each lambda body: a variable is
"boxed" iff some lexically-inner lambda references it, or it is `set!`. Simple and
correct; tightening it (capture-by-value when provably never mutated) is a later
optimization, not correctness.

### Globals as repatchable slots

Each global binding is a stable `(sym . val)` **cell**; `define`/`set!` mutate the
cell's cdr in place and never replace the cell. A global reference compiles to the
cell pointer (`LOADG slot` = one `cdr`), resolved once at compile time. **Live
redefinition is just a cdr write** — visible at every call site with no
de-optimization and no per-call hash lookup. (Measured ~1.7x over per-call hash.)

### Inline caches (how "no frozen ops" stays fast)

For the hot operator sites — arithmetic (`+ - * …`), comparison (`< <= > >= =`),
and core pair/predicate ops (`cons car cdr null? pair? not`) — the compiler emits
an `OPCALL` that carries: the operator's global **cell**, a **fast-op selector**,
and an **inline-cache** slot (initially empty). At runtime `OPCALL`:

1. `v = cdr(cell)` (the current binding of the operator);
2. **guard:** if `v` is identical to the cached builtin primitive *and* the
   operands are the fast type (e.g. both fixnums) → run the **inlined** fast op
   (e.g. fixnum add with overflow check). The cache is filled lazily on first
   execution: the first run records `v` if it is the expected builtin primitive;
3. **deopt:** otherwise (operator was redefined, or operand is a flonum / not a
   number / wrong arity) → fall through to an ordinary `CALL` of `v` with the
   args. This is also where flonum arithmetic and the n-ary forms go.

So redefining `+` simply makes the guard fail forever after (the new value is not
the cached primitive), and every call site transparently calls the new `+`. The
guard is a pointer compare plus operand tag checks — cheap relative to a call. For
non-`OPCALL` calls, a monomorphic inline cache on the `CALL` site (cache the last
callee + a thin direct `->fn` dispatch with zero-copy args) recovers the
"bypass the C ABI" win (~1.5x) for repeated calls to the same primitive.

### Threaded VM loop

Computed-goto dispatch where the compiler supports it (the classic 2-3x over a
`switch`), falling back to `switch`. Fully maintainable, no machine-code emission.
The chunk is left as a documented input for a future x86 JIT (a register bytecode
→ x86 emitter is small and testable), but the JIT is **out of scope** here.

## Invariants preserved (the process model rides on these)

The CEK machine is the OS process model, not just an evaluator. The VM keeps every
contract, unchanged above the evaluator:

- **Per-context heaps, capabilities, copy-on-send isolation, killability** — live
  above the evaluator; untouched.
- **`send`/`recv` blocking** and the scheduler slice — unchanged; the VM yields at
  instruction boundaries instead of CEK reductions.
- **Reflection / liveness (the crown jewel).** The compiler is **resident**: the
  REPL and the `sys-debug` capability compile a fresh expression against a live
  context's environment and run it (there is no separate "compiled vs.
  interpreted" world once the tree-walker is gone — everything is compiled,
  including REPL input). Per-chunk debug metadata (`slot ↔ name`, `pc ↔ source`)
  kept **off the hot path** lets a paused frame show/evaluate locals by name.
  `ctx-list / ctx-pause / ctx-step / ctx-control / ctx-value` keep working;
  stepping is now per-instruction (or per-source-form, using the `pc ↔ source`
  table).

## Form coverage (the closed grammar the VM must compile)

| form | lowering |
|------|----------|
| literals, `quote` | `LOADK` of the datum |
| variable ref | register / `LOADUP` cell / `LOADG` slot |
| `if` `when` `unless` `cond` `case` `and` `or` | `JMPF`/`JMPT` over compiled arms |
| `begin`, body sequences | compile in order, value of last |
| `while` | back-edge `JMP` + budget charge |
| `lambda` | child chunk + `CLOSURE` capturing cells |
| `define` (top / internal), `set!` | global slot patch / local cell / `MKCELL` |
| `let` `let*` `letrec` | cells (letrec: pre-allocated) + body |
| application | `CALL`/`TAILCALL`; hot ops via `OPCALL` |
| `apply` | VM-level spread into the callee window |
| `quasiquote`/`unquote`/`unquote-splicing` | compile template; `LOADK` constant parts, evaluate unquoted parts, splice |
| `define-module` `import` `include` | loader compiles each top-level form to a chunk; imports resolve to global slots |

## Phased plan (each phase independently reviewable + tested)

The correctness gate throughout: a **differential test** runs the VM and the
(soon-to-be-deleted) tree-walker on the same forms and asserts identical results —
the same discipline that guarded the prototype (39-case corpus + 40000-case
fuzzer, both green). The existing `libs/lisp/test` suite and the in-OS boot are
the regression gate.

- **P0 — Remove the shader tier + write this note.** `libs/lisp_shader/` was an
  unwired, host-only compute-kernel compiler (zero call sites in the OS); deleted
  wholesale. *Done.*
- **P1 — Full-coverage compiler: cons AST → register chunk.** Lexical addressing;
  flat registers + boxed cells (capture/`set!`/`letrec` analysis); **every** form
  in the table above, including the ones the prototype declined (`letrec`, `case`,
  `while`, `quasiquote`, mutual local recursion). No frozen ops — operators lower
  to `CALL`/`OPCALL` through global slots. Differential-test the compiler by
  running chunks under a reference chunk-walker and against `lisp_eval`; extend the
  fuzzer to the full grammar.
- **P2 — Threaded register VM** executing chunks under the `lisp_ctx` contract
  (budget/suspend/precise roots/heap-resident stack). Inline caches: `OPCALL` fast
  paths + monomorphic `CALL` caches; globals-as-slots. Differential + fuzz green.
- **P3 — Wire into live eval; delete the tree-walker.** Promote `lbc.inc` to real
  `libs/lisp` source. Switch `lisp_eval`/`lisp_ctx_resume`/`lisp_apply` to
  compile-then-run. Compile the module system. Re-express `sys-debug` stepping on
  instruction boundaries. **Delete** `step_eval`/`step_applyk`/`K_*` and the
  unreachable env/kont machinery.
- **P4 — In-OS boot validation + measure.** Boot the full OS (all servers/drivers/
  USB now run on the VM); confirm clean boot + `sys-debug` stepping + live
  redefinition in-OS; measure speedup; update `notes/AUDIT.md`.

## Prototype results carried forward (evidence the architecture works)

From the host-only prototype (`lbc.inc` + `test_bytecode.c`), the basis we build
on. These used frozen ops; with Decision 2 the inline-cache guard adds a small
constant per hot op, so expect a slightly smaller but comparable win:

- 39-case curated differential corpus + a 40000-case random-expression fuzzer
  against the tree-walker: **0 failures** (every case value-matches or
  error-matches; the fuzzer found a real `and`/`or` double-pop bug, since fixed).
- Microbench (2M-iter tail loop): stack VM **~18x**, register VM **~27x** over the
  tree-walker, no machine-code emission.
- Call-path churn (per add): inlined op ~57ns; `lisp_apply`+global-hash ~262ns;
  thin `->fn`+global-**slot** ~103ns. → inlining dominates; globals-as-slots ~1.7x;
  thin direct `->fn` ~1.5x. All three are kept; the inline-cache guard is what lets
  the inlined path coexist with redefinable operators.

## Non-goals

- Not statically typed. Not a machine-code JIT yet (the chunk is its future input).
- Not a surface-syntax change (s-expressions stay; a C/Python-like surface that
  compiles to the same chunk is a separate, optional project).
- Not beating a mature C compiler — target is a small multiple of the current
  interpreter while staying fully dynamic and live.
