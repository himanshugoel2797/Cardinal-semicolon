// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

// S3 UNIT 3 -- SIMD benchmark: measure the speedup of the SIMD VM path
// vs the scalar-fallback VM path vs the tree-walking reference interpreter.
//
// Shader: a Horner-scheme polynomial evaluation over f32x4 vectors.
// The kernel runs a bounded loop that repeats many mul+add operations per
// iteration. Each call computes:
//
//   (loop (i from 0 to ITERS) (acc = acc * x + c))
//
// returning a f32x4 result, exercising VBINOP add and mul on every iteration.
//
// Method:
//   Compile+lower once.
//   (a) sh_vm_run with flags=0              -> SIMD path (SSE2, AVX2 if available)
//   (b) sh_vm_run with SH_VM_FORCE_SCALAR  -> scalar lane-loop path
//   (c) sh_invoke (reference interpreter)  -> tree-walker oracle
//
// Each variant is called REPS times. Wall-clock time is measured with
// clock_gettime(CLOCK_MONOTONIC) and reported in milliseconds. The ratio
// (b)/(a) isolates the SIMD-vs-scalar speedup within the VM (same dispatch
// overhead, only the vector op kernel differs). The ratio (c)/(a) shows the
// combined win of bytecode VM + SIMD over the tree-walker.
//
// Build and run:
//   clang -std=c11 -O2 -mavx2 \
//     -I libs/lisp_shader/inc -I libs/lisp_shader/src -I libs/lisp/inc \
//     libs/lisp_shader/src/*.c libs/lisp/src/*.c \
//     libs/lisp_shader/test/bench_compute.c -o /tmp/bench -lm && /tmp/bench
//
// (Also compile WITHOUT -mavx2 to exercise the SSE2-only path.)

// POSIX: needed for clock_gettime / CLOCK_MONOTONIC
#define _POSIX_C_SOURCE 200112L

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "lisp.h"
#include "sh_bytecode.h"
#include "sh_internal.h"

// ---------------------------------------------------------------------------
// Timer
// ---------------------------------------------------------------------------

static double now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

// ---------------------------------------------------------------------------
// Benchmark parameters
// ---------------------------------------------------------------------------

// Horner loop iteration count (more iterations = more VBINOP per call).
#define HORNER_ITERS 256

// Number of repetitions of each variant per benchmark round.
// Tuned so total time per variant is ~0.3-1 second on a typical 2026 host.
#define REPS 8000

// ---------------------------------------------------------------------------
// Shader source: Horner evaluation over f32x4.
//
//   (defshader horner ((x f32x4)(c f32x4)) -> f32x4
//     (let loop ((i 0) (acc (splat (f32 0.0))))
//       (if (>= i <ITERS>)
//         acc
//         (loop (+ i 1) (+ (* acc x) c)))))
//
// This does 2 x VBINOP per iteration (mul + add), exercising the f32x4
// SIMD path on every step.
// ---------------------------------------------------------------------------

static char *build_horner_src(int iters) {
  char *buf = (char *)malloc(512);
  if (!buf) return NULL;
  snprintf(buf, 512,
    "(defshader horner ((x f32x4)(c f32x4)) -> f32x4"
    " (let loop ((i 0) (acc (splat (f32 0.0))))"
    "   (if (>= i %d)"
    "     acc"
    "     (loop (+ i 1) (+ (* acc x) c)))))",
    iters);
  return buf;
}

// ---------------------------------------------------------------------------
// Helper: make an f32x4 sh_value.
// ---------------------------------------------------------------------------

static sh_value make_f32x4(float a, float b, float c, float d) {
  sh_value v;
  memset(&v, 0, sizeof(v));
  v.kind = SH_K_VEC;
  v.lanes = 4;
  v.lane_kind = (uint8_t)SH_K_F32;
  float vals[4] = {a, b, c, d};
  for (int i = 0; i < 4; i++) {
    uint32_t bits;
    memcpy(&bits, &vals[i], 4);
    v.lane[i] = (uint64_t)bits;
  }
  return v;
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(void) {
  (void)lisp_default_env();

  printf("S3 SIMD benchmark -- Horner f32x4 (%d iters/call, %d reps)\n\n",
         HORNER_ITERS, REPS);

  // Build shader source.
  char *src = build_horner_src(HORNER_ITERS);
  if (!src) { fprintf(stderr, "OOM\n"); return 1; }

  // Compile the program.
  sh_program *p = NULL;
  sh_error err;
  memset(&err, 0, sizeof(err));
  sh_status s = sh_compile_string(src, NULL, 0, &p, &err);
  free(src);
  if (s != SH_OK) {
    fprintf(stderr, "compile failed: %s\n", err.msg);
    return 1;
  }

  // Lower to bytecode.
  sh_chunk *c = NULL;
  memset(&err, 0, sizeof(err));
  s = sh_lower(p, &c, &err);
  if (s != SH_OK) {
    fprintf(stderr, "lower failed: %s\n", err.msg);
    sh_free(p);
    return 1;
  }

  // Args: x = (0.5, 0.6, 0.7, 0.8), c = (1.0, 2.0, 3.0, 4.0).
  sh_value args[2];
  args[0] = make_f32x4(0.5f, 0.6f, 0.7f, 0.8f);
  args[1] = make_f32x4(1.0f, 2.0f, 3.0f, 4.0f);

  // Verify all three paths produce the same result.
  sh_value out_simd, out_scalar, out_interp;
  memset(&out_simd,   0, sizeof(out_simd));
  memset(&out_scalar, 0, sizeof(out_scalar));
  memset(&out_interp, 0, sizeof(out_interp));
  sh_vm_run(c, args, 2, 0,                  &out_simd,   &err);
  sh_vm_run(c, args, 2, SH_VM_FORCE_SCALAR, &out_scalar, &err);
  sh_invoke(p, args, 2, &out_interp, &err);

  printf("Correctness check (all results should be bit-identical):\n");
  int ok = 1;
  for (int li = 0; li < 4; li++) {
    uint64_t vs = out_simd.lane[li];
    uint64_t vc = out_scalar.lane[li];
    uint64_t vi = out_interp.lane[li];
    float fv; uint32_t b32 = (uint32_t)vs; memcpy(&fv, &b32, 4);
    int match = (vs == vc && vs == vi);
    printf("  lane[%d]: simd=0x%08X  scalar=0x%08X  interp=0x%08X  "
           "f=%g  %s\n",
           li, (unsigned)vs, (unsigned)vc, (unsigned)vi,
           fv, match ? "OK" : "MISMATCH");
    if (!match) ok = 0;
  }
  if (!ok) {
    fprintf(stderr, "\nERROR: correctness mismatch -- aborting benchmark.\n");
    sh_chunk_free(c); sh_free(p);
    return 1;
  }
  printf("\n");

  // --- Benchmark: SIMD VM ---
  double t_simd_start = now_ms();
  for (int r = 0; r < REPS; r++) {
    sh_value out;
    memset(&out, 0, sizeof(out));
    sh_vm_run(c, args, 2, 0, &out, &err);
    // Prevent dead-code elimination: consume the result.
    __asm__ volatile("" : : "r"(out.lane[0]) : "memory");
  }
  double t_simd = now_ms() - t_simd_start;

  // --- Benchmark: scalar-fallback VM ---
  double t_scalar_start = now_ms();
  for (int r = 0; r < REPS; r++) {
    sh_value out;
    memset(&out, 0, sizeof(out));
    sh_vm_run(c, args, 2, SH_VM_FORCE_SCALAR, &out, &err);
    __asm__ volatile("" : : "r"(out.lane[0]) : "memory");
  }
  double t_scalar = now_ms() - t_scalar_start;

  // --- Benchmark: reference interpreter ---
  double t_interp_start = now_ms();
  for (int r = 0; r < REPS; r++) {
    sh_value out;
    memset(&out, 0, sizeof(out));
    sh_invoke(p, args, 2, &out, &err);
    __asm__ volatile("" : : "r"(out.lane[0]) : "memory");
  }
  double t_interp = now_ms() - t_interp_start;

  // Report.
  printf("Timing (%d reps, %d iters/call = %d VBINOP/call):\n",
         REPS, HORNER_ITERS, HORNER_ITERS * 2);
  printf("  %-30s  %8.2f ms   (%6.2f us/call)\n",
         "SIMD VM (default)",
         t_simd, t_simd * 1000.0 / REPS);
  printf("  %-30s  %8.2f ms   (%6.2f us/call)\n",
         "Scalar-fallback VM",
         t_scalar, t_scalar * 1000.0 / REPS);
  printf("  %-30s  %8.2f ms   (%6.2f us/call)\n",
         "Reference interpreter",
         t_interp, t_interp * 1000.0 / REPS);
  printf("\n");
  printf("Speedups:\n");
  printf("  scalar-VM / SIMD-VM  = %.2fx  (SIMD effect; same dispatch cost)\n",
         t_scalar / t_simd);
  printf("  interp    / SIMD-VM  = %.2fx  (SIMD VM vs tree-walker)\n",
         t_interp / t_simd);
  printf("  interp    / scalar-VM= %.2fx  (bytecode VM vs tree-walker)\n",
         t_interp / t_scalar);
  printf("\n");
  printf("Note: the VM has per-instruction dispatch overhead.\n");
  printf("SIMD/scalar ratio isolates the vector-op SIMD effect.\n");
  printf("A true JIT would eliminate dispatch and show a larger SIMD win.\n");

  sh_chunk_free(c);
  sh_free(p);
  return 0;
}
