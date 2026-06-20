// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

// S4 -- the VM facade. sh_run executes a verified program through the bytecode VM
// (the SIMD-accelerated path) instead of the scalar reference interpreter. The
// chunk is lowered + validated once, lazily, and cached on the program -- the
// "compiled at first execution" model from notes/core/lisp-shaders.md. Result
// semantics are identical to sh_invoke by construction; the host differential
// suite asserts VM == interpreter bit-for-bit.

#include "sh_internal.h"
#include "sh_bytecode.h"

sh_status sh_run(sh_program *p, const sh_value *args, uint32_t argc,
                 uint32_t flags, sh_value *out, sh_error *err) {
  if (!p || !p->verified)
    return sh_set_error(err, SH_ERR_INTERNAL, -1, -1,
                        "run of unverified program");
  if (argc != p->nparams)
    return sh_set_error(err, SH_ERR_ARITY, -1, -1,
                        "shader expects %u args, got %u", p->nparams, argc);

  // Same shallow arg-kind contract the interpreter enforces (shi_invoke), so the
  // two entry points reject identical inputs.
  for (uint32_t i = 0; i < argc; i++) {
    sh_kind ak = args[i].kind;
    sh_kind pk = (sh_kind)p->params[i].kind;
    if (pk != ak)
      return sh_set_error(err, SH_ERR_TYPE, -1, -1,
                          "arg %u: expected kind %u, got kind %u", i, pk, ak);
  }

  // Lower + validate on first use, then cache. The validator is defence in depth:
  // it re-checks the lowerer's output against the VM's structural contract so a
  // lowerer bug cannot drive the VM out of bounds (see sh_vm.c's preamble).
  if (p->chunk == NULL) {
    sh_chunk *c = NULL;
    sh_status s = sh_lower(p, &c, err);
    if (s != SH_OK)
      return s;
    s = sh_chunk_validate(c, err);
    if (s != SH_OK) {
      sh_chunk_free(c);
      return s;
    }
    p->chunk = c;
  }

  return sh_vm_run((const sh_chunk *)p->chunk, args, argc, flags, out, err);
}
