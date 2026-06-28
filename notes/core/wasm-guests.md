<!---
 Copyright (c) 2026 Himanshu Goel

 This software is released under the MIT License.
 https://opensource.org/licenses/MIT
-->

# Running non-Lisp guests as WebAssembly, hosted by Lisp

Status: **design proposal** (nothing built yet). The way to run foreign code
(a ported C program such as Doom, a language runtime, an untrusted blob) in
Cardinal; **without** reaching for ring-3 / the MMU / the native scheduler.
Supersedes [`native-sandbox.md`](native-sandbox.md), which used hardware
isolation and was rejected for dragging native-process machinery into an OS whose
whole point is that *the interpreter is the security boundary*.

## The thesis

Userspace in Cardinal; is a Lisp VM context precisely so we don't need ring-3.
The aligned way to run foreign code is therefore **not** "add native code
support" but "**add a second guest interpreter**" — do for foreign code exactly
what we already did for Lisp: run it in a VM we control, mediate every effect
through host-provided imports, gate it with capabilities, and yield cooperatively
via a fuel counter. WebAssembly is the purpose-built target for this, and LLVM
(the clang 21 we already build with) emits `wasm32` with no new toolchain.

## Why this deletes the hard parts of the ring-3 plan

The ring-3 design needed a return-to-host trampoline, CR3 switches, a watchdog
timer, and fault-vs-panic unwinding — all just to **suspend a running guest** and
hand control back to Lisp. A Wasm guest suspends by *returning from a C function*,
**if** the interpreter keeps its state (operand stack, call frames, pc) in heap
structures and runs a flat dispatch loop instead of using the C stack — i.e. the
same shape as the existing `lbc` Lisp VM.

| What ring-3 fought for | How the stackless Wasm VM gets it |
|---|---|
| periodic yield from the guest | decrement a **fuel** counter in the dispatch loop → return `OUT_OF_FUEL` (identical to Lisp's reduction `budget`) |
| return-to-host on an IO call | an import handler records a pending request and signals `SUSPEND` → the loop returns to the Lisp host |
| scheduler stays oblivious | the VM is driven by a host Lisp context via `(wasm-resume inst fuel)`; that host context is the only schedulable unit |
| guest fault ≠ kernel panic | a trap (OOB access, div0, bad indirect call) is an interpreter branch → returns `TRAPPED`; no CPU exception, no IDT, no panic |
| isolation | every memory access is `base + offset` bounds-checked against linear-memory size; a raw pointer cannot be *expressed* |

No MMU, no rings, no asm trampolines, no timers. The hard, risky pieces become
ordinary control flow inside an interpreter we own.

## Decisions

- **Engine: write our own** small, stackless, fuel-metered **MVP** interpreter
  modeled on `lbc`. Resumable by construction; smallest trusted surface;
  validation under our control; host-testable. (wasm3 was rejected: it runs on
  the C stack — not resumable mid-call without native stack-switching — and only
  partially validates, undercutting the security boundary.)
- **Guest ABI: WASI subset.** `clang --target=wasm32-wasi` gives the guest a
  standard libc; the host implements the ~dozen `wasi_snapshot_preview1` calls
  Doom actually uses, plus a tiny custom `cardinal` import module for
  display/input (which WASI does not cover).

## Architecture

```
   Lisp scheduler (per core, cooperative) — knows nothing below this line
        ▼
   ┌──────────── host Lisp context (caps: sys-display sys-input sys-initrd) ───┐
   │  inst = (wasm-load wad-host-module-bytes)                                  │
   │  loop:                                                                     │
   │    st = (wasm-resume inst FUEL)                                            │
   │    case st of                                                             │
   │      done            -> finish                                            │
   │      (suspend req...) -> service req via Core* servers, (wasm-resume ...)  │
   │      trapped(code)    -> log + tear down                                   │
   │    (yield)           ; hand the slice back when idle                       │
   └────────────────────────────────────────────────────────────────────────┘
        │ drives
        ▼
   Wasm interpreter (libs/wasm, linked into SysLisp like lbc/ttf)
     linear memory  = a Lisp bytes object  (framebuffer is a sub-range)
     operand stack  = heap array            (stackless → resumable)
     frame stack    = heap array
     fuel           = cooperative yield     (like lisp_ctx_t.budget)
```

### The interpreter (modeled on `lbc`)

Instance state, all on the heap so execution is suspend/resumable and GC-traced:

- `module`: decoded function bodies (rcode), type signatures, tables, globals,
  data/element segments, import descriptors.
- `mem`: a Lisp `bytes` (linear memory). Wasm pages are 64 KiB; reserve the
  module's declared **max** up front so `memory.grow` never moves it (simpler
  than realloc-and-refetch; bounded by the declared max). `(wasm-mem inst)`
  returns the current handle for zero-copy host reads.
- `ostack`: operand stack (raw 64-bit slots; validation guarantees types, so no
  per-slot tag needed once validated).
- `frames`: call frames `{func, pc, locals_base, operand_base, ret_arity}`.
- `fuel`, `status` (`RUN | DONE | OUT_OF_FUEL | SUSPEND | TRAPPED`),
  `pending` (the import request when `SUSPEND`).

Main loop: a flat `for(;;)` dispatch (computed-goto like `lbc`'s threaded VM).
**Fuel** is decremented on calls and loop back-edges (the `lbc` rule); on
`fuel <= 0` the loop returns `OUT_OF_FUEL`. Memory loads/stores bounds-check
`base + offset` against `mem` size and return `TRAPPED` on violation. Resume
re-enters the loop from saved `pc`/stacks; when resuming from a suspended import,
the host-supplied result is pushed first.

### Imports = capabilities, resolved at instantiate

A Wasm module declares its imports as `(module, name, signature)`. At
`wasm-load`/instantiate time a **Lisp resolver** maps each to a host function or
rejects it — an unsatisfied import fails instantiation. This is the capability
gate: a host context without `sys-display` simply cannot satisfy the
`cardinal.present` import, so a module needing it won't instantiate. Two kinds of
import handler, the same Tier split as the ring-3 plan but now declarative:

- **Inline-C (fast).** Serviced entirely in the interpreter against state the
  guest already has — e.g. `clock_time_get` (→ `timer_timestamp_ns`),
  bulk memory ops. No suspend. May read/write `mem` directly.
- **Suspend-to-host (policy/IO).** Records `pending = (tag args…)` and returns
  `SUSPEND`. The Lisp host reads it, services it with its caps + Core\* servers,
  and `(wasm-resume inst fuel result)`s. No re-entry into the Wasm VM from inside
  an import — the suspend/resume boundary keeps it clean.

### WASI subset + a `cardinal` import module

The guest links `wasi-libc` (standard libc) and we implement only the calls Doom
exercises, mostly trivial and mostly *inline-C*:

| WASI call | backed by | kind |
|---|---|---|
| `proc_exit` | tear down instance | inline |
| `fd_read` / `fd_seek` / `fd_close` | WAD via `sys-initrd` (read-only); savegame via RAM/`cardfs` | suspend (file) |
| `fd_write` | `(log …)` for stdout/stderr; savegame writes | suspend |
| `path_open` | stubbed: map `"doom1.wad"` → a synthetic fd over the initrd blob | suspend |
| `clock_time_get` | `timer_timestamp_ns` | inline |
| `args_*` / `environ_*` | argv = `("doom")`; environ empty | inline |
| `random_get` | virtio-rng or stub | suspend/inline |

Display and input are **not** WASI, so doomgeneric's platform shim calls a small
custom `cardinal` import module instead (a Wasm module can import from several
namespaces):

| `cardinal` import | meaning | host services it by |
|---|---|---|
| `present(off,len)` | frame ready at `mem[off..off+len]` | message `coredisplay` (needs `sys-display`) |
| `poll_input() -> i32` | next key event, packed | message `coreinput` |

The framebuffer never copies through the ABI: doomgeneric writes `DG_ScreenBuffer`
into linear memory, calls `present(off,len)`, and the host blits straight from
`(wasm-mem inst)` sub-range with the existing `bytes-copy!` bulk path.

### The `sys-wasm` capability module + GC handle

New capability-gated module in SysLisp (cf. `sys-mmio`/`sys-pci`):

- `(wasm-load bytes resolver)` → validate + instantiate → opaque **handle**.
- `(wasm-resume inst fuel [result])` → `done | (suspend tag args…) | (trapped code)`.
- `(wasm-mem inst)` → a `bytes` view of current linear memory.
- `(wasm-call inst "export" args…)` → invoke an exported function (for setup).
- `(wasm-destroy inst)` → free interpreter state + linear memory.

The instance is owned by a **finalizer-bearing GC handle**. That generic handle
type (`LISP_OBJ_HANDLE` / `lisp_make_handle(ptr, fin, tag)`) existed for the
shader tier but was **removed** with it — it must be **re-added** to the current
bytecode VM's GC (a per-type finalizer hook in mark-sweep), or replaced by an
explicit `wasm-destroy` with leak-on-drop for a first cut. Re-adding the handle
is the cleaner long-term choice and is generically useful.

## The one real cost: a new TCB component

The interpreter + validator become **trusted code** — the sandbox holds only if
they are correct. This is far smaller and more tractable than ring-3 + MMU +
trampoline + fault paths, and it has a strong, mechanical mitigation we have used
before: **differential-test on the host against the official WebAssembly spec
test suite** (the play the old shader tier used with its host harness). Memory
safety is the simplest invariant to get right and the most important: every
access is a bounds-checked `base + offset`, so a correct interpreter cannot be
escaped regardless of module validity. Full validation is then a hardening +
performance layer (typed bytecode lets the loop skip per-op type checks and lets
us cleanly *reject* malformed modules rather than trap mid-run).

## Performance

A naive interpreter is ~20–50× slower than native; wasm3-class is ~4–15×. Doom
ran on a 386, so even the naive end clears 35 fps on modern hardware under KVM
(under TCG/CI it is slower but functional). If it ever matters, the *same*
bytecode can later get a JIT — but JIT means native codegen + W^X, which both
reintroduces the machinery we deleted and widens the TCB, so it stays deferred
and arguably off-ethos. Interpret for now.

## Build / wiring

- `libs/wasm/` — the interpreter + validator, linked into `modules/SysLisp` like
  `lisp`/`ttf`. Compile the TU with SSE locally enabled (as `lisp`/`ttf` do) for
  Wasm `f32`/`f64`; the rest is integer. Freestanding against `common/`.
- Guest: `clang --target=wasm32-wasi` + `wasi-libc`, doomgeneric's own zone
  allocator over linear memory, a ~2-import platform shim. Output `.wasm` baked
  into the initrd (shareware `doom1.wad` alongside it).
- No kernel / scheduler / MMU / boot-script changes. This is a pure
  SysLisp-side library + a Lisp host app.

## Phases

1. **Stackless interpreter core**: decode + flat dispatch for the MVP opcode
   subset, linear memory as `bytes`, fuel/suspend/trap status. Host-driven, no
   Lisp yet (a C test harness feeds modules).
2. **Validator + host differential harness** against the Wasm spec test suite.
   This is where sandbox confidence is earned; mostly host work, pre-boot.
3. **`sys-wasm` prims** + re-add the finalizer GC handle; `(wasm-load/resume/
   mem/destroy)`; capability-gated import resolver.
4. **WASI subset + `cardinal` import module** (file→`sys-initrd`, clock→timer,
   present→`coredisplay`, input→`coreinput`).
5. **Doom**: compile doomgeneric to `wasm32-wasi`, the 2-import shim, drive it
   from a host Lisp app; blit from linear memory, forward input, pace with sleep.

Phases 1–2 are the bulk and are fully host-testable before anything boots.

## Open items

- **Re-add `LISP_OBJ_HANDLE` finalizer** vs explicit `wasm-destroy` only for v1.
  *(lean: re-add the handle.)*
- **Reserve-max vs grow-and-refetch** linear memory. *(lean: reserve max.)*
- **Validation depth in v1**: runtime-checks-only (memory-safe, trusts the
  producer) vs full validating loader. *(lean: structural validation in v1, full
  type validation as the hardening pass once the spec harness is up.)*
- **Multi-value / bulk-memory / reference-types proposals**: out of scope for v1
  (MVP only); `wasi-libc` + doomgeneric do not require them.
