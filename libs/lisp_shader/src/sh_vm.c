// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

// S3 UNIT 2/3 -- the bytecode VM. Re-validates a chunk (defense in depth, so a
// buggy lowerer can't drive an OOB) and executes it on typed args, runtime-
// bounds-checking every region access. Vector opcodes use SSE/AVX intrinsics with
// a scalar lane-loop fallback (SH_VM_FORCE_SCALAR). The reference interpreter
// (sh_interp.c) remains the canonical oracle this VM is differential-tested
// against. STUBS until implemented. See notes/scratch/shader-s3-decision.md.

#include "sh_bytecode.h"

sh_status sh_chunk_validate(const sh_chunk *c, sh_error *err) {
    (void)c;
    return sh_set_error(err, SH_ERR_INTERNAL, -1, -1, "chunk validator not implemented");
}

sh_status sh_vm_run(const sh_chunk *c, const sh_value *args, uint32_t argc,
                    uint32_t flags, sh_value *out, sh_error *err) {
    (void)c;
    (void)args;
    (void)argc;
    (void)flags;
    (void)out;
    return sh_set_error(err, SH_ERR_INTERNAL, -1, -1, "VM not implemented");
}
