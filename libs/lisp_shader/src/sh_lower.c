// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

// S3 UNIT 1 -- the lowerer: verified AST -> flat register bytecode (sh_chunk).
// A mechanical post-verification pass (on the verified side of the moat). STUB
// until implemented. See notes/scratch/shader-s3-decision.md and the deferred
// opcode set in notes/scratch/shader-proposal-minimalist.md section 7.

#include <stdlib.h>

#include "sh_bytecode.h"

sh_status sh_lower(const sh_program *p, sh_chunk **out, sh_error *err) {
    if (out) *out = NULL;
    (void)p;
    return sh_set_error(err, SH_ERR_INTERNAL, -1, -1, "lowerer not implemented");
}

void sh_chunk_free(sh_chunk *c) {
    if (!c) return;
    free(c->code);
    free(c->aux);
    free(c);
}
