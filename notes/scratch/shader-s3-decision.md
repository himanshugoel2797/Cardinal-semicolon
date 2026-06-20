<!--
 Copyright (c) 2026 Himanshu Goel

 This software is released under the MIT License.
 https://opensource.org/licenses/MIT
-->

# Shader tier S3 — backend decision (bytecode + SSE/AVX VM, host-first)

S3 is the first ISA layer (notes/core/lisp-shaders.md). Key scoping decision:
**not a machine-code JIT.** A true codegen (emit x86 bytes + W^X + relocation) is
the "native codegen first" road the minimalist convergence deliberately did NOT
take — a large trusted surface for a backend we can get most of the win from more
cheaply. Instead S3 is:

1. **`sh_lower.c`** — verified AST → a flat typed **register bytecode**
   (`sh_bytecode.h`, the deferred minimalist-spec §7 opcode set, now built). A
   mechanical post-verification lowering: it sits on the VERIFIED side of the moat,
   so a lowering bug is a *miscompile* caught by the differential oracle, never a
   safety hole — PROVIDED the VM stays memory-safe on its own (see below).
2. **`sh_vm.c`** — a register bytecode executor. Faster dispatch than the
   tree-walker (linear stream, no arena pointer-chasing), and its **vector opcodes
   use SSE/AVX intrinsics** (`<immintrin.h>`), with a scalar lane-loop fallback.
   This is the actual SIMD payoff.

The **reference interpreter (`sh_interp.c`) stays the canonical oracle.** The VM is
an *alternative* executor, exercised by a differential harness (VM == interpreter,
bit-for-bit) and a benchmark. (S4 may later route `sh_invoke` through the VM; not
now.) A true machine-code JIT remains a later option, consuming this same bytecode.

## Decisions

- **Register/slot bytecode**, not stack: each instruction writes a result vreg and
  reads operand vregs; maps ~1:1 to AST nodes, is SIMD-friendly (a vreg can hold a
  vector), and is the natural input for a future regalloc JIT. Loop induction vars
  are vregs.
- **Structured-ish control flow:** forward `JMP_IFNOT` for `if`/short-circuit, a
  backward `JMP` for the bounded-loop back-edge. Reducible, bound-annotated — no
  general CFG needed yet (the verifier already proved loop structure).
- **The VM re-validates the chunk before running** (`sh_chunk_validate`: every vreg
  index < nvregs, every jump target in range, every aux range in bounds, region
  accesses runtime-bounds-checked like the interpreter). This preserves the "no OOB
  ever" property even against a buggy lowerer — mirroring the note's "verify on use,
  wherever compiled" (cf. `VerifyModule`). So lowering need not be in the trusted
  surface; the chunk validator + the VM's runtime checks are.
- **VM vector-vreg layout is internal to `sh_vm.c`** (not part of the frozen
  bytecode): store vector lanes PACKED at native element width, 16-byte aligned, so
  S3b can `_mm_load_*` them directly; scalar fallback indexes by element size;
  convert to/from the public `sh_value` only at the invoke boundary.

## Float bit-exactness (the correctness landmine the differential test guards)

The oracle holds f32 as double and narrows per op. The SSE path must match it
bit-for-bit:
- `_mm_add_ps/sub/mul/div_ps` are correctly-rounded single precision → match scalar
  `float` ops per-op. OK.
- **Reductions MUST stay sequential left-to-right** (the spec mandates it). A SIMD
  horizontal/tree reduction reassociates and diverges for floats — do float
  reductions in lane order (or scalar). This is the #1 place exactness breaks.
- **min/max NaN + signed-zero semantics** must match the oracle's scalar min/max;
  `_mm_min_ps`/`_mm_max_ps` have specific NaN/-0 behavior — match the oracle or do
  min/max scalar.
- Integer vector ops are exact by construction; mind saturating vs wrapping
  (a blit wants `_mm_adds_epu8` saturating — the oracle's op must agree).

## Files (additive; the moat — frontend/verifier — is untouched)

```
libs/lisp_shader/
  src/sh_bytecode.h   # FROZEN: sh_bc_op, sh_instr, sh_chunk; lower/vm/validate entrypoints
  src/sh_lower.c      # Unit S3-1: AST -> bytecode
  src/sh_vm.c         # Unit S3-2 (scalar exec + chunk validate) then S3-3 (SSE/AVX)
  test/test_lower.c   # lowering structure tests
  test/test_vm.c      # differential VM==interpreter over a corpus + property cases
  test/bench_blit.c   # SIMD framebuffer-blit benchmark (scalar interp vs SSE VM)
```

Sequencing (a pipeline, like S0-S2): scaffold the frozen bytecode header → S3-1
lowering → S3-2 scalar VM + chunk validator + differential harness → S3-3 SSE/AVX
fast path + benchmark. Each stage keeps all suites green; the differential test is
the gate.
