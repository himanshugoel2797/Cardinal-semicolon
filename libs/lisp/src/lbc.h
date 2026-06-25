// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

// lbc -- the bytecode compiler + threaded register VM for the base Lisp
// (notes/core/lisp-vm.md). Public surface only: the chunk/closure types stay
// opaque here (their layout lives in lbc.c); this header is what the kernel
// evaluator and the host differential test compile against.
//
// STATUS: the compiler + VM are the sole evaluator (lisp_eval / lisp_ctx_resume
// run them; the tree-walker and the earlier stack-bytecode form are gone). Still
// fuzz/corpus-tested on the host (test/test_bytecode.c) against the ctx VM oracle.

#ifndef CARDINAL_LISP_LBC_H
#define CARDINAL_LISP_LBC_H

#include <stdbool.h>

#include "lisp.h"

// A compiled chunk (one per lambda) and a compiled closure (chunk + captured
// cells). Opaque: only lbc.c constructs and inspects them.
typedef struct bcchunk bcchunk;
typedef struct bcclosure bcclosure;

typedef enum { LBC_OK, LBC_DECLINED, LBC_ERR } lbc_status;

// Tuning toggles (the host bench flips these to measure each lever). Default 1.
extern int g_thin_prim;     // call a primitive's C fn directly (zero-copy args)
extern int g_global_slots;  // resolve a global to its binding cell at compile time

// Compile `expr` (against global env `genv`) to a register-bytecode chunk.
// Returns false and sets *why on a malformed form / resource limit.
bool rlbc_compile(lisp_value genv, lisp_value expr, bcchunk **out, const char **why);

// Run a top-level closure to completion, returning its value via *out (false +
// *err on a runtime error).
bool rvm_run(bcclosure *top, lisp_value genv, lisp_value *out, const char **errout);

// Compile-then-run convenience wrapper.
lbc_status rlbc_eval(lisp_value genv, lisp_value expr, lisp_value *out, const char **msg);

// Wrap a chunk in a fresh top-level closure (no captured cells).
bcclosure *lbc_top(bcchunk *k);

// Static instruction count over a chunk tree (a dispatch-density proxy) and the
// number of inline-cache sites -- used by the bytecode test/bench.
int lbc_count_reg(bcchunk *k);
int lbc_chunk_nics(bcchunk *k);

#endif
