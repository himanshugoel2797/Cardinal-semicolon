<!--
 Copyright (c) 2026 Himanshu Goel

 This software is released under the MIT License.
 https://opensource.org/licenses/MIT
-->

# Shader tier S3.5 — vector region load/store + saturating ops

S3's SIMD win is on compute kernels over vector vregs, not memory bandwidth: today
`region-ref` returns ONE scalar element, so a real SIMD framebuffer blit (load a
`u8x16` strip, saturating-add, store) isn't expressible. S3.5 adds exactly the two
things a blit needs, end-to-end through all five units (frontend → verifier →
interpreter-oracle → lowerer → VM scalar+SSE):

1. **Vector region load/store** — read/write a contiguous STRIP of a region as a
   fixed-width vector.
2. **Saturating integer add/sub** — the clamp a brightness/alpha blit needs.

The target kernel this makes expressible and fast:

```scheme
(defshader blit ((buf (bytes-mut u8)) (delta u8x16)) -> u32
  (let loop ((i 0))
    (if (>= i (region-len buf))
        0
        (begin
          (vregion-set! buf i (sat+ (vregion-ref buf i 16) delta))
          (loop (+ i 16))))))   ; 16 elements per iteration
```

## Surface syntax

- `(vregion-ref buf i N)` — load `N` consecutive elements of `buf` starting at
  ELEMENT index `i`, as a `<N × buf.elem>` vector. `N` is a literal in
  `[2, SH_MAX_LANES]`. The loop strides `i` by `N`.
- `(vregion-set! buf i v)` — store vector `v`'s lanes to `buf[i .. i+lanes)`.
  `buf` must be mutable; `v`'s element kind must equal `buf.elem`.
- `(sat+ a b)`, `(sat- a b)` — saturating add/sub. Integer kinds only (reject on
  float). Promote to the vector form on vector operands, exactly like `+`/`-`.

## IR / bytecode additions (the contract)

- `sh_internal.h`: append `SH_OP_VREGION_LOAD` (a=region, b=index, imm=lane count N;
  result type vec<elem,N>) and `SH_OP_VREGION_STORE` (a=region, b=index, c=vector).
  Append `SH_BIN_SADD`, `SH_BIN_SSUB` to `sh_binop` (used by SH_OP_BINOP and, when
  promoted, SH_OP_VBINOP — same path as add/sub).
- `sh_bytecode.h`: append `SHB_VRLOAD` (a=region, b=index, imm=N, kind=elem) and
  `SHB_VRSTORE` (a=region, b=index, c=vec). Saturating binops flow through the
  existing `SHB_BINOP`/`SHB_VBINOP` with the new `sub`.

## Bounds checking — the safety-critical invariant (no MMU behind it)

A strip access touches elements `[idx, idx+N)`. Compute the check WITHOUT overflow,
with `idx` taken as `uint64_t` (an i64 index that went negative becomes huge and is
caught), `len` the region element count, `N` the lane count:

```c
if (idx > len || (uint64_t)(len - idx) < (uint64_t)N)
    -> trap SH_ERR_BOUNDS;       // never compute idx + N (could overflow)
```

The interpreter AND the VM must use this exact form, and ALWAYS check (the verifier
cannot statically prove `idx + N <= len` for the strided loop, so there is no
elision). The region is contiguous, so the in-bounds access is a single
`memcpy`/`_mm_loadu`/`_mm_storeu` of `N*esz` bytes.

## Saturating semantics (oracle definition)

- Unsigned kinds (u8/u16/u32/u64): `sat+` = `min(a+b, KMAX)`, `sat-` = `max(a-b, 0)`.
- `i64`: signed clamp to `[INT64_MIN, INT64_MAX]`.
- Float operands: a verifier error (`sat` is integer-only).

## SSE/AVX mapping (VM fast path; scalar fallback otherwise, bit-exact either way)

- `SHB_VRLOAD`/`VRSTORE`: `_mm_loadu_si128`/`_mm_storeu_si128` (or `_mm_loadu_ps`)
  straight from/to the region pointer for 128-bit strips; scalar `memcpy` fallback
  for other widths. THIS is the memory-bandwidth win.
- Saturating `VBINOP`: `_mm_adds_epu8`/`_mm_subs_epu8` (u8x16),
  `_mm_adds_epu16`/`_mm_subs_epu16` (u16x8). Wider kinds (u32/u64/i64): scalar
  (no SSE unsigned-saturating for those). The blit's `u8x16` is the SSE case.

## Differential bit-exactness

The VM (SIMD and forced-scalar) must match the interpreter oracle bit-for-bit, as
in S3. New landmines: the SSE saturating ops must clamp identically to the scalar
oracle (they do for u8/u16); vregion load/store must read/write the same bytes the
oracle does (endianness/packing identical — both use native packing).

## Test suite (high quality — the explicit ask)

- Per-unit: parse (new forms), verify (accept + reject: float sat+, immutable
  vregion-set!, bad N, element-kind mismatch), interp results.
- Differential: every new op through compile→lower→vm (SIMD AND forced-scalar) vs
  the oracle, across element kinds (u8/u16/u32/f32) and lane counts (2/4/8/16),
  many data sets.
- Bounds traps: `idx+N > len`, partial last strip, len not a multiple of N, a
  negative (i64) index, immutable-store rejection.
- Saturating boundaries: 255+1, 0-1, near-max wraparound-vs-clamp, negatives.
- A randomized property fuzzer: random buffers + the blit/transform kernels,
  asserting SIMD-VM == scalar-VM == oracle over many iterations.
- `bench_blit.c`: the real SIMD memory blit, scalar-VM vs SIMD-VM vs oracle.
- ASan/UBSan on all suites incl. the AVX2 build.

## Sequencing (autonomous)

Two vertical slices, each kept green + tested, then a safety/bit-exactness review
and fixes, then the blit benchmark and a randomized fuzzer capstone:
1. Saturating ops (`sat+`/`sat-`) through all units.
2. Vector region load/store through all units + the blit benchmark.
