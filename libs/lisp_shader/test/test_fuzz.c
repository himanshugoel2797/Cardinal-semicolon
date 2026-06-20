// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

// Randomized differential fuzzer for the shader tier. For every case it runs the
// SAME program through THREE executors and asserts they agree bit-for-bit:
//   (1) sh_invoke           -- the tree-walking reference interpreter (the ORACLE)
//   (2) sh_vm_run SCALAR    -- the bytecode VM, scalar lane loops
//   (3) sh_vm_run (default) -- the bytecode VM, SSE/AVX fast path
// Data is randomized with a deterministic PRNG (fixed seed -> reproducible), so a
// failure is a real divergence, not flakiness. This is the high-confidence
// cross-check the whole "the VM matches the oracle" design rests on, stressed
// across the saturating-arith and vector-region (S3.5) paths plus the broader
// scalar/vector/loop surface, on inputs the hand-written corpus does not enumerate.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sh_bytecode.h"  // pulls in sh_internal.h + the public API

// ---------------------------------------------------------------------------
// Deterministic PRNG (xorshift64*)
// ---------------------------------------------------------------------------
static uint64_t g_rng = 0x9E3779B97F4A7C15ull;
static uint64_t xs(void) {
  uint64_t x = g_rng;
  x ^= x >> 12;
  x ^= x << 25;
  x ^= x >> 27;
  g_rng = x;
  return x * 0x2545F4914F6CDD1Dull;
}
static uint32_t rnd_u32(void) { return (uint32_t)(xs() >> 11); }
static uint64_t rnd_u64(void) { return xs(); }
// A value biased toward saturation/overflow boundaries to stress clamp/wrap.
static uint64_t rnd_boundary(uint32_t bytes) {
  uint64_t mask = (bytes >= 8) ? ~0ull : ((1ull << (bytes * 8)) - 1);
  uint64_t v = xs();
  switch (v & 7) {           // bias toward 0, max, near-max
    case 0: return 0;
    case 1: return mask;
    case 2: return mask - (v >> 8 & 3);
    case 3: return (v >> 8 & 3);
    default: return v & mask;
  }
}
static float rnd_f32(void) {
  // A spread of small/large/negative floats (avoid NaN/Inf: differential of
  // the few non-IEEE-associative ops is covered elsewhere; here we want clean
  // per-op bit-equality which holds for finite +-*/).
  int32_t n = (int32_t)(rnd_u32() % 20001) - 10000;  // [-10000, 10000]
  return (float)n * 0.013f;
}

// ---------------------------------------------------------------------------
// Bit-exact value comparison (scalar + vector aware)
// ---------------------------------------------------------------------------
static int val_eq(sh_value a, sh_value b) {
  if (a.kind != b.kind) return 0;
  if (a.kind == SH_K_VEC) {
    if (a.lanes != b.lanes || a.lane_kind != b.lane_kind) return 0;
    uint32_t esz = sh_kind_size((sh_kind)a.lane_kind);
    if (esz == 0) esz = 8;
    uint64_t mask = (esz >= 8) ? ~0ull : ((1ull << (esz * 8)) - 1);
    for (uint8_t k = 0; k < a.lanes; k++)
      if ((a.lane[k] & mask) != (b.lane[k] & mask)) return 0;
    return 1;
  }
  switch (a.kind) {
    case SH_K_F32: {
      float fa = (float)a.f, fb = (float)b.f;
      uint32_t xa, xb;
      memcpy(&xa, &fa, 4);
      memcpy(&xb, &fb, 4);
      return xa == xb;
    }
    case SH_K_F64: {
      uint64_t xa, xb;
      memcpy(&xa, &a.f, 8);
      memcpy(&xb, &b.f, 8);
      return xa == xb;
    }
    case SH_K_I64:
      return a.i == b.i;
    default: {
      uint32_t esz = sh_kind_size(a.kind);
      uint64_t mask = (esz == 0 || esz >= 8) ? ~0ull : ((1ull << (esz * 8)) - 1);
      return (a.u & mask) == (b.u & mask);
    }
  }
}

// ---------------------------------------------------------------------------
// Harness state
// ---------------------------------------------------------------------------
static long g_cases = 0;
static long g_fail = 0;

// Compile + lower a shader; both handles are required for the three-way run.
static int build(const char *src, const sh_prim_set *prims,
                 sh_program **out_p, sh_chunk **out_c) {
  sh_error err;
  memset(&err, 0, sizeof(err));
  sh_program *p = NULL;
  if (sh_compile_string(src, prims, 0, &p, &err) != SH_OK || !p) {
    printf("  [fuzz] COMPILE FAILED: %s\n    %s\n", src, err.msg);
    return 0;
  }
  sh_chunk *c = NULL;
  memset(&err, 0, sizeof(err));
  if (sh_lower(p, &c, &err) != SH_OK || !c) {
    printf("  [fuzz] LOWER FAILED: %s\n    %s\n", src, err.msg);
    sh_free(p);
    return 0;
  }
  *out_p = p;
  *out_c = c;
  return 1;
}

// Run args through oracle + scalar-VM + SIMD-VM; assert all three agree on
// status and (when SH_OK) on the result value, bit-for-bit.
static void three_way(sh_program *p, sh_chunk *c, const sh_value *args,
                      uint32_t argc, const char *tag) {
  sh_value o, s, m;
  sh_error eo, es, em;
  memset(&o, 0, sizeof(o));
  memset(&s, 0, sizeof(s));
  memset(&m, 0, sizeof(m));
  memset(&eo, 0, sizeof(eo));
  memset(&es, 0, sizeof(es));
  memset(&em, 0, sizeof(em));

  sh_status so = sh_invoke(p, args, argc, &o, &eo);
  sh_status ss = sh_vm_run(c, args, argc, SH_VM_FORCE_SCALAR, &s, &es);
  sh_status sm = sh_vm_run(c, args, argc, 0, &m, &em);
  g_cases++;

  if (so != ss || so != sm) {
    if (g_fail < 12)
      printf("  [fuzz] %s STATUS mismatch: oracle=%d scalar=%d simd=%d\n",
             tag, (int)so, (int)ss, (int)sm);
    g_fail++;
    return;
  }
  if (so != SH_OK) return;  // all three trapped identically: good
  if (!val_eq(o, s) || !val_eq(o, m)) {
    if (g_fail < 12)
      printf("  [fuzz] %s RESULT mismatch (oracle.u=%llu scalar.u=%llu simd.u=%llu)\n",
             tag, (unsigned long long)o.u, (unsigned long long)s.u,
             (unsigned long long)m.u);
    g_fail++;
  }
}

// ---------------------------------------------------------------------------
// Vector value builders
// ---------------------------------------------------------------------------
static sh_value mkvec(sh_kind lane, uint8_t n) {
  sh_value v;
  memset(&v, 0, sizeof(v));
  v.kind = SH_K_VEC;
  v.lanes = n;
  v.lane_kind = (uint8_t)lane;
  uint32_t esz = sh_kind_size(lane);
  for (uint8_t k = 0; k < n; k++) {
    if (lane == SH_K_F32) {
      float f = rnd_f32();
      uint32_t b;
      memcpy(&b, &f, 4);
      v.lane[k] = b;
    } else {
      v.lane[k] = rnd_boundary(esz);
    }
  }
  return v;
}

// ===========================================================================
// Fuzz kernels
// ===========================================================================

// 1. Scalar integer arithmetic mix (u32 / i64) over random operands.
static void fuzz_scalar_int(int iters) {
  sh_program *p32, *pi64;
  sh_chunk *c32, *ci64;
  if (!build("(defshader f ((a u32)(b u32)(c u32)) -> u32"
             " (+ (* a b) (- c (bit-xor a b))))", NULL, &p32, &c32)) { g_fail++; return; }
  if (!build("(defshader f ((a i64)(b i64)(c i64)) -> i64"
             " (- (+ a (* b c)) (mod c (bit-or a 1))))", NULL, &pi64, &ci64)) {
    g_fail++; sh_free(p32); sh_chunk_free(c32); return;
  }
  for (int i = 0; i < iters; i++) {
    sh_value a32[3] = {sh_val_u32(rnd_u32()), sh_val_u32(rnd_u32()), sh_val_u32(rnd_u32())};
    three_way(p32, c32, a32, 3, "scalar-u32");
    sh_value ai[3] = {sh_val_i64((int64_t)rnd_u64()), sh_val_i64((int64_t)rnd_u64()),
                      sh_val_i64((int64_t)rnd_u64())};
    three_way(pi64, ci64, ai, 3, "scalar-i64");
  }
  sh_free(p32); sh_chunk_free(c32);
  sh_free(pi64); sh_chunk_free(ci64);
}

// 2. Scalar float arithmetic (f32) -- per-op bit equality.
static void fuzz_scalar_float(int iters) {
  sh_program *p; sh_chunk *c;
  if (!build("(defshader f ((a f32)(b f32)(c f32)) -> f32"
             " (+ (* a b) (- c a)))", NULL, &p, &c)) { g_fail++; return; }
  for (int i = 0; i < iters; i++) {
    sh_value a[3] = {sh_val_f32(rnd_f32()), sh_val_f32(rnd_f32()), sh_val_f32(rnd_f32())};
    three_way(p, c, a, 3, "scalar-f32");
  }
  sh_free(p); sh_chunk_free(c);
}

// 3. Saturating add/sub on scalars and vectors (u8/u16/u32), boundary-biased.
static void fuzz_saturate(int iters) {
  struct { const char *src; sh_kind k; uint8_t lanes; } K[] = {
    {"(defshader f ((a u8)(b u8)(c u8)) -> u8 (sat- (sat+ a b) c))", SH_K_U8, 0},
    {"(defshader f ((a u16)(b u16)(c u16)) -> u16 (sat- (sat+ a b) c))", SH_K_U16, 0},
    {"(defshader f ((a u32)(b u32)(c u32)) -> u32 (sat- (sat+ a b) c))", SH_K_U32, 0},
    {"(defshader f ((a u8x16)(b u8x16)) -> u8x16 (sat+ a b))", SH_K_U8, 16},
    {"(defshader f ((a u16x8)(b u16x8)) -> u16x8 (sat- a b))", SH_K_U16, 8},
  };
  for (size_t ki = 0; ki < sizeof(K) / sizeof(K[0]); ki++) {
    sh_program *p; sh_chunk *c;
    if (!build(K[ki].src, NULL, &p, &c)) { g_fail++; continue; }
    uint32_t esz = sh_kind_size(K[ki].k);
    for (int i = 0; i < iters; i++) {
      if (K[ki].lanes == 0) {
        sh_value a[3];
        for (int j = 0; j < 3; j++) {
          a[j].kind = K[ki].k; a[j].lanes = 1; a[j].lane_kind = 0;
          a[j].u = rnd_boundary(esz);
        }
        three_way(p, c, a, 3, "sat-scalar");
      } else {
        sh_value a[2] = {mkvec(K[ki].k, K[ki].lanes), mkvec(K[ki].k, K[ki].lanes)};
        three_way(p, c, a, 2, "sat-vector");
      }
    }
    sh_free(p); sh_chunk_free(c);
  }
}

// 4. Vector lane-wise arithmetic + reduction (f32x4, u32x4).
static void fuzz_vector(int iters) {
  sh_program *pf, *pu, *pr;
  sh_chunk *cf, *cu, *cr;
  if (!build("(defshader f ((a f32x4)(b f32x4)) -> f32x4 (+ (* a b) b))", NULL, &pf, &cf)) { g_fail++; return; }
  if (!build("(defshader f ((a u32x4)(b u32x4)) -> u32x4 (- (+ a b) a))", NULL, &pu, &cu)) {
    g_fail++; sh_free(pf); sh_chunk_free(cf); return;
  }
  if (!build("(defshader f ((a f32x4)(b f32x4)) -> f32 (dot a b))", NULL, &pr, &cr)) {
    g_fail++; sh_free(pf); sh_chunk_free(cf); sh_free(pu); sh_chunk_free(cu); return;
  }
  for (int i = 0; i < iters; i++) {
    sh_value af[2] = {mkvec(SH_K_F32, 4), mkvec(SH_K_F32, 4)};
    three_way(pf, cf, af, 2, "vec-f32x4");
    sh_value au[2] = {mkvec(SH_K_U32, 4), mkvec(SH_K_U32, 4)};
    three_way(pu, cu, au, 2, "vec-u32x4");
    sh_value ar[2] = {mkvec(SH_K_F32, 4), mkvec(SH_K_F32, 4)};
    three_way(pr, cr, ar, 2, "vec-dot");
  }
  sh_free(pf); sh_chunk_free(cf);
  sh_free(pu); sh_chunk_free(cu);
  sh_free(pr); sh_chunk_free(cr);
}

// 5. Region-sum loop over a random u32 buffer (non-mutating, scalar result).
static void fuzz_region_sum(int iters) {
  sh_program *p; sh_chunk *c;
  if (!build("(defshader f ((buf (bytes u32))) -> u32"
             " (let loop ((i 0)(acc (u32 0)))"
             "   (if (>= i (region-len buf)) acc"
             "     (loop (+ i 1) (+ acc (region-ref buf i))))))", NULL, &p, &c)) { g_fail++; return; }
  for (int i = 0; i < iters; i++) {
    uint32_t n = 1 + (rnd_u32() % 64);
    uint32_t *buf = (uint32_t *)malloc(n * sizeof(uint32_t));
    for (uint32_t j = 0; j < n; j++) buf[j] = rnd_u32();
    sh_value arg = sh_val_region_raw(buf, n, SH_K_U32, false);
    three_way(p, c, &arg, 1, "region-sum");
    free(buf);
  }
  sh_free(p); sh_chunk_free(c);
}

// 6. The SIMD saturating blit -- the S3.5 flagship. MUTATES the buffer, so each
// executor gets its own copy and we compare the resulting buffers byte-for-byte.
static void fuzz_blit(int iters) {
  sh_program *p; sh_chunk *c;
  if (!build("(defshader blit ((buf (bytes-mut u8))(d u8x16)) -> u32"
             " (let loop ((i 0))"
             "   (if (>= i (region-len buf)) (u32 0)"
             "     (begin (vregion-set! buf i (sat+ (vregion-ref buf i 16) d))"
             "            (loop (+ i 16))))))", NULL, &p, &c)) { g_fail++; return; }
  for (int it = 0; it < iters; it++) {
    uint32_t strips = 1 + (rnd_u32() % 64);
    uint32_t n = strips * 16;
    uint8_t *base = (uint8_t *)malloc(n);
    for (uint32_t j = 0; j < n; j++) base[j] = (uint8_t)rnd_u32();
    uint8_t *bo = (uint8_t *)malloc(n), *bs = (uint8_t *)malloc(n), *bm = (uint8_t *)malloc(n);
    memcpy(bo, base, n); memcpy(bs, base, n); memcpy(bm, base, n);
    sh_value delta = mkvec(SH_K_U8, 16);

    sh_value ao[2] = {sh_val_region_raw(bo, n, SH_K_U8, true), delta};
    sh_value as[2] = {sh_val_region_raw(bs, n, SH_K_U8, true), delta};
    sh_value am[2] = {sh_val_region_raw(bm, n, SH_K_U8, true), delta};
    sh_value ro, rs, rm; sh_error eo, es, em;
    memset(&ro,0,sizeof ro); memset(&rs,0,sizeof rs); memset(&rm,0,sizeof rm);
    memset(&eo,0,sizeof eo); memset(&es,0,sizeof es); memset(&em,0,sizeof em);
    sh_status so = sh_invoke(p, ao, 2, &ro, &eo);
    sh_status ss = sh_vm_run(c, as, 2, SH_VM_FORCE_SCALAR, &rs, &es);
    sh_status sm = sh_vm_run(c, am, 2, 0, &rm, &em);
    g_cases++;
    if (so != SH_OK || ss != SH_OK || sm != SH_OK) {
      if (g_fail < 12) printf("  [fuzz] blit STATUS o=%d s=%d m=%d\n",(int)so,(int)ss,(int)sm);
      g_fail++;
    } else if (memcmp(bo, bs, n) != 0 || memcmp(bo, bm, n) != 0) {
      if (g_fail < 12) printf("  [fuzz] blit BUFFER mismatch (n=%u)\n", n);
      g_fail++;
    }
    free(base); free(bo); free(bs); free(bm);
  }
  sh_free(p); sh_chunk_free(c);
}

// 7. Bounds fuzz: a vregion load at a PARAM index that is sometimes out of range
// -> the oracle and both VM paths must agree on trap-vs-ok (and the value if ok).
static void fuzz_bounds(int iters) {
  sh_program *p; sh_chunk *c;
  // Sum lane 0 of the strip loaded at index i (a param), no loop -- isolates the
  // strip bounds check at an arbitrary index.
  if (!build("(defshader f ((buf (bytes u8))(i u32)) -> u8"
             " (lane (vregion-ref buf i 16) 0))", NULL, &p, &c)) { g_fail++; return; }
  for (int it = 0; it < iters; it++) {
    uint32_t n = 16 * (1 + (rnd_u32() % 8));   // a few full strips
    uint8_t *buf = (uint8_t *)malloc(n);
    for (uint32_t j = 0; j < n; j++) buf[j] = (uint8_t)rnd_u32();
    // index in [0, n+8): straddles the in-bounds boundary (n-16 is last valid).
    uint32_t idx = rnd_u32() % (n + 8);
    sh_value args[2] = {sh_val_region_raw(buf, n, SH_K_U8, false), sh_val_u32(idx)};
    three_way(p, c, args, 2, "vregion-bounds");
    free(buf);
  }
  sh_free(p); sh_chunk_free(c);
}

// 8. Multi-effect begin: TWO sequential vregion-set! per iteration (different
// strips), sequencing two side effects in one begin -- stresses begin's
// effect-ordering desugar beyond the single-store blit. Each executor gets its
// own buffer copy; the resulting buffers must be byte-identical.
static void fuzz_begin_multi(int iters) {
  sh_program *p; sh_chunk *c;
  // buf processed two strips (32 elems) per iteration: strip i gets +d1, the
  // next strip gets saturating +d2. Tests order + two distinct effects.
  if (!build("(defshader f ((buf (bytes-mut u8))(d1 u8x16)(d2 u8x16)) -> u32"
             " (let loop ((i 0))"
             "   (if (>= i (region-len buf)) (u32 0)"
             "     (begin"
             "       (vregion-set! buf i (sat+ (vregion-ref buf i 16) d1))"
             "       (vregion-set! buf (+ i 16) (sat+ (vregion-ref buf (+ i 16) 16) d2))"
             "       (loop (+ i 32))))))", NULL, &p, &c)) { g_fail++; return; }
  for (int it = 0; it < iters; it++) {
    uint32_t pairs = 1 + (rnd_u32() % 32);
    uint32_t n = pairs * 32;  // multiple of 32 so every iteration's two strips fit
    uint8_t *base = (uint8_t *)malloc(n);
    for (uint32_t j = 0; j < n; j++) base[j] = (uint8_t)rnd_u32();
    uint8_t *bo = (uint8_t *)malloc(n), *bs = (uint8_t *)malloc(n), *bm = (uint8_t *)malloc(n);
    memcpy(bo, base, n); memcpy(bs, base, n); memcpy(bm, base, n);
    sh_value d1 = mkvec(SH_K_U8, 16), d2 = mkvec(SH_K_U8, 16);
    sh_value ao[3] = {sh_val_region_raw(bo, n, SH_K_U8, true), d1, d2};
    sh_value as[3] = {sh_val_region_raw(bs, n, SH_K_U8, true), d1, d2};
    sh_value am[3] = {sh_val_region_raw(bm, n, SH_K_U8, true), d1, d2};
    sh_value ro, rs, rm; sh_error eo, es, em;
    memset(&ro,0,sizeof ro); memset(&rs,0,sizeof rs); memset(&rm,0,sizeof rm);
    memset(&eo,0,sizeof eo); memset(&es,0,sizeof es); memset(&em,0,sizeof em);
    sh_status so = sh_invoke(p, ao, 3, &ro, &eo);
    sh_status ss = sh_vm_run(c, as, 3, SH_VM_FORCE_SCALAR, &rs, &es);
    sh_status sm = sh_vm_run(c, am, 3, 0, &rm, &em);
    g_cases++;
    if (so != SH_OK || ss != SH_OK || sm != SH_OK) {
      if (g_fail < 12) printf("  [fuzz] begin-multi STATUS o=%d s=%d m=%d\n",(int)so,(int)ss,(int)sm);
      g_fail++;
    } else if (memcmp(bo, bs, n) != 0 || memcmp(bo, bm, n) != 0) {
      if (g_fail < 12) printf("  [fuzz] begin-multi BUFFER mismatch (n=%u)\n", n);
      g_fail++;
    }
    free(base); free(bo); free(bs); free(bm);
  }
  sh_free(p); sh_chunk_free(c);
}

int main(void) {
  (void)lisp_default_env();  // bring up interning for the reader

  const int N = 4000;
  printf("[fuzz] running randomized differential (oracle == scalar-VM == SIMD-VM)\n");
  fuzz_scalar_int(N);
  fuzz_scalar_float(N);
  fuzz_saturate(N);
  fuzz_vector(N);
  fuzz_region_sum(N / 4);
  fuzz_blit(N / 8);
  fuzz_begin_multi(N / 8);
  fuzz_bounds(N);

  printf("[fuzz] %ld differential cases, %ld failures\n", g_cases, g_fail);
  if (g_fail == 0) printf("[test_fuzz] ALL PASSED\n");
  return g_fail ? 1 : 0;
}
