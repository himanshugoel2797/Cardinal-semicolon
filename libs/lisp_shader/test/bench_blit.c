// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

// S3.5 SLICE 2 -- saturating-add blit benchmark.
//
// Benchmarks a u8 saturating-add blit over a >= 1MB buffer using:
//   1. The SIMD VM path
//   2. The scalar-VM path (SH_VM_FORCE_SCALAR)
//   3. The oracle (sh_invoke)
//
// First verifies all three produce identical output, then times each.

#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "lisp.h"
#include "sh_bytecode.h"
#include "sh_internal.h"

#define BUF_SIZE (1024 * 1024)  // 1 MB
#define BENCH_ITERS 10

static double now_sec(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

// Thread lane[0] of the stored vector through the accumulator so the
// effect node (vregion-set!) is reachable from the tree root (verifier
// constraint: all nodes must be typed, which requires reachability from root).
static const char *blit_src =
  "(defshader blit ((buf (bytes-mut u8))(delta u8x16)) -> u32"
  "  (let loop ((i 0)(acc (u32 0)))"
  "    (if (>= i (region-len buf))"
  "        acc"
  "        (loop (+ i 16)"
  "              (+ acc (u32 (lane (vregion-set! buf i (sat+ (vregion-ref buf i 16) delta)) 0)))))))";

int main(int argc, char **argv) {
  (void)argc; (void)argv;
  printf("=== bench_blit (1 MB u8 saturating-add blit) ===\n");

  // Compile
  sh_program *p = NULL;
  sh_error err;
  memset(&err, 0, sizeof(err));
  sh_status s = sh_compile_string(blit_src, NULL, 0, &p, &err);
  if (s != SH_OK) {
    fprintf(stderr, "compile FAILED: %s\n", err.msg);
    return 1;
  }

  sh_chunk *c = NULL;
  memset(&err, 0, sizeof(err));
  s = sh_lower(p, &c, &err);
  if (s != SH_OK) {
    fprintf(stderr, "lower FAILED: %s\n", err.msg);
    sh_free(p);
    return 1;
  }
  printf("Compile+lower: ok\n");

  // Allocate buffers
  uint8_t *buf_oracle  = (uint8_t *)malloc(BUF_SIZE);
  uint8_t *buf_vm      = (uint8_t *)malloc(BUF_SIZE);
  uint8_t *buf_scalar  = (uint8_t *)malloc(BUF_SIZE);
  if (!buf_oracle || !buf_vm || !buf_scalar) {
    fprintf(stderr, "OOM\n");
    return 1;
  }

  // Fill with data near 200 so saturation is visible
  for (int i = 0; i < BUF_SIZE; i++) {
    buf_oracle[i] = (uint8_t)(100 + (i & 127));
    buf_vm[i]     = buf_oracle[i];
    buf_scalar[i] = buf_oracle[i];
  }

  // Build delta: add 100 to each lane (will saturate many values)
  sh_value delta;
  memset(&delta, 0, sizeof(delta));
  delta.kind = SH_K_VEC;
  delta.lanes = 16;
  delta.lane_kind = (uint8_t)SH_K_U8;
  for (int i = 0; i < 16; i++) delta.lane[i] = 100;

  sh_value out;

  // --- Correctness check ---
  memset(&out, 0, sizeof(out));
  sh_value args[2];
  args[0] = sh_val_region_raw(buf_oracle, BUF_SIZE, SH_K_U8, true);
  args[1] = delta;
  memset(&err, 0, sizeof(err));
  sh_status so = sh_invoke(p, args, 2, &out, &err);
  if (so != SH_OK) { fprintf(stderr, "oracle FAILED: %s\n", err.msg); return 1; }

  memset(&out, 0, sizeof(out));
  args[0] = sh_val_region_raw(buf_vm, BUF_SIZE, SH_K_U8, true);
  memset(&err, 0, sizeof(err));
  sh_status sv = sh_vm_run(c, args, 2, 0, &out, &err);
  if (sv != SH_OK) { fprintf(stderr, "vm FAILED: %s\n", err.msg); return 1; }

  memset(&out, 0, sizeof(out));
  args[0] = sh_val_region_raw(buf_scalar, BUF_SIZE, SH_K_U8, true);
  memset(&err, 0, sizeof(err));
  sh_status ss = sh_vm_run(c, args, 2, SH_VM_FORCE_SCALAR, &out, &err);
  if (ss != SH_OK) { fprintf(stderr, "scalar-vm FAILED: %s\n", err.msg); return 1; }

  if (memcmp(buf_oracle, buf_vm, BUF_SIZE) != 0) {
    fprintf(stderr, "CORRECTNESS FAIL: oracle != vm\n");
    return 1;
  }
  if (memcmp(buf_oracle, buf_scalar, BUF_SIZE) != 0) {
    fprintf(stderr, "CORRECTNESS FAIL: oracle != scalar-vm\n");
    return 1;
  }
  printf("Correctness: oracle == vm == scalar-vm  OK\n\n");

  // --- Benchmarks ---
  // Reset buffers for timing
  for (int i = 0; i < BUF_SIZE; i++) {
    buf_oracle[i] = (uint8_t)(100 + (i & 127));
    buf_vm[i]     = buf_oracle[i];
    buf_scalar[i] = buf_oracle[i];
  }

  double t_vm = 0, t_scalar = 0, t_oracle = 0;

  // SIMD VM
  double t0 = now_sec();
  for (int iter = 0; iter < BENCH_ITERS; iter++) {
    memset(&out, 0, sizeof(out));
    args[0] = sh_val_region_raw(buf_vm, BUF_SIZE, SH_K_U8, true);
    sh_vm_run(c, args, 2, 0, &out, NULL);
  }
  t_vm = (now_sec() - t0) / BENCH_ITERS;

  // Scalar VM
  t0 = now_sec();
  for (int iter = 0; iter < BENCH_ITERS; iter++) {
    memset(&out, 0, sizeof(out));
    args[0] = sh_val_region_raw(buf_scalar, BUF_SIZE, SH_K_U8, true);
    sh_vm_run(c, args, 2, SH_VM_FORCE_SCALAR, &out, NULL);
  }
  t_scalar = (now_sec() - t0) / BENCH_ITERS;

  // Oracle
  t0 = now_sec();
  for (int iter = 0; iter < BENCH_ITERS; iter++) {
    memset(&out, 0, sizeof(out));
    args[0] = sh_val_region_raw(buf_oracle, BUF_SIZE, SH_K_U8, true);
    sh_invoke(p, args, 2, &out, NULL);
  }
  t_oracle = (now_sec() - t0) / BENCH_ITERS;

  double mb = (double)BUF_SIZE / (1024.0 * 1024.0);
  printf("Buffer size: %.1f MB, iterations: %d\n", mb, BENCH_ITERS);
  printf("\n%-16s %10s %12s %8s\n", "Path", "Time(ms)", "Throughput", "Speedup");
  printf("%-16s %10s %12s %8s\n", "----", "--------", "----------", "-------");
  printf("%-16s %10.2f %12.1f %8.2fx\n",
         "SIMD-VM",
         t_vm * 1000.0,
         mb / t_vm,
         t_scalar / t_vm);
  printf("%-16s %10.2f %12.1f %8.2fx\n",
         "Scalar-VM",
         t_scalar * 1000.0,
         mb / t_scalar,
         1.0);
  printf("%-16s %10.2f %12.1f %8.2fx\n",
         "Oracle",
         t_oracle * 1000.0,
         mb / t_oracle,
         t_scalar / t_oracle);
  printf("  (throughput in MB/s; speedup vs scalar-VM)\n");

  free(buf_oracle);
  free(buf_vm);
  free(buf_scalar);
  sh_chunk_free(c);
  sh_free(p);
  return 0;
}
