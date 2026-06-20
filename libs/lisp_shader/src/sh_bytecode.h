// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#ifndef CARDINAL_LISP_SHADER_BYTECODE_H
#define CARDINAL_LISP_SHADER_BYTECODE_H

// S3 -- the flat typed REGISTER bytecode the backend consumes. This is the
// FROZEN cross-unit contract between the lowerer (sh_lower.c, the producer) and
// the VM (sh_vm.c, the consumer); see notes/scratch/shader-s3-decision.md.
//
// The bytecode is a MECHANICAL post-verification lowering of the verified AST
// (libs/lisp_shader/src/sh_internal.h). It sits on the VERIFIED side of the moat,
// so a lowering bug is a miscompile caught by the differential oracle (the
// reference interpreter), not a safety hole -- PROVIDED the VM re-validates the
// chunk (sh_chunk_validate) and runtime-bounds-checks region access. The bytecode
// is also the input a future machine-code JIT would consume.
//
// Register machine: each instruction writes a result vreg (`dst`) and reads
// operand vregs (`a`,`b`,`c`). vregs index the VM's slot file [0, nvregs).
// Control flow: forward SHB_JMP_IFNOT for if/short-circuit, a backward SHB_JMP for
// the bounded-loop back-edge (reducible, bound-annotated -- no general CFG).

#include "sh_internal.h"

typedef uint32_t sh_vreg;  // index into the VM value slot file
#define SH_VREG_NONE 0xFFFFFFFFu

typedef enum {
    SHB_CONST,       // dst = imm64 (read per `kind`: int / float bits / bool)
    SHB_MOV,         // dst = a (copy; binds params / loop inits / phi-join arms)
    SHB_PARAM,       // dst = args[imm]
    SHB_UNOP,        // dst = unop(a)         sub = sh_unop; result kind = `kind`
    SHB_BINOP,       // dst = binop(a,b)      sub = sh_binop; kind = operand kind
    SHB_CMP,         // dst = cmp(a,b)->bool  sub = sh_cmp;  kind = operand kind
    SHB_RLOAD,       // dst = region[b] of region-vreg a; elem = `kind`
    SHB_RSTORE,      // region-vreg a [b] = c (region must be mutable); dst optional = c
    SHB_RLEN,        // dst = len(region-vreg a) -> u32
    SHB_CALL,        // dst = prims[imm](aux vregs...); aux_off/aux_len select args
    SHB_JMP,         // pc = imm (unconditional; the loop back-edge)
    SHB_JMP_IFNOT,   // if a is false: pc = imm
    SHB_RET,         // return a
    // --- vectors: abstract ops; the VM picks scalar lane-loop or SSE/AVX ---
    SHB_VSPLAT,      // dst = broadcast(a) over `lanes` of `kind`
    SHB_VBINOP,      // dst = lane-wise binop(a,b)   sub = sh_binop
    SHB_VCMP,        // dst = lane-wise cmp(a,b)->mask  sub = sh_cmp
    SHB_VSELECT,     // dst = lane-wise (mask a) ? b : c
    SHB_VSHUFFLE,    // dst = { a.lane[aux[k]] } ; constant indices in aux
    SHB_VREDUCE,     // dst = reduce(a) -> scalar  sub = sh_reduce (DOT uses b)
    SHB_VLANE,       // dst = a.lane[imm] -> scalar
} sh_bc_op;

// One fixed-size instruction. Variadic operands (CALL args, VSHUFFLE indices)
// live in sh_chunk.aux addressed by (aux_off, aux_len): for CALL they are vregs,
// for VSHUFFLE they are plain constant lane indices.
typedef struct {
    uint16_t op;      // sh_bc_op
    uint16_t sub;     // sh_unop / sh_binop / sh_cmp / sh_reduce, else 0
    uint8_t  kind;    // sh_kind: result/operand scalar (or vector lane) kind
    uint8_t  lanes;   // vector lane count (0 for scalar ops)
    uint8_t  pad[2];
    sh_vreg  dst;     // result slot (SH_VREG_NONE if none, e.g. bare RSTORE/JMP/RET)
    sh_vreg  a, b, c; // operand slots (SH_VREG_NONE if unused)
    uint32_t imm;     // PARAM index / CALL prim index / jump target pc / VLANE index
    uint32_t aux_off; // start in sh_chunk.aux
    uint32_t aux_len; // count there
    int64_t  imm64;   // CONST payload: int value, or the double's bit pattern, or bool
} sh_instr;

// A lowered program: instruction stream + the slot file size + signature. One
// owned allocation group (freed by sh_chunk_free). Pointer-free indices only, so
// it is relocatable/serializable (a future in-OS cache or JIT input).
typedef struct {
    char     name[64];
    uint32_t nparams;
    sh_type  params[SH_MAX_PARAMS];
    sh_type  ret;

    sh_instr *code;  uint32_t ncode;   // the instruction stream
    uint32_t *aux;   uint32_t naux;     // CALL args / VSHUFFLE indices
    uint32_t  nvregs;                   // slot-file size the VM must allocate
    sh_vreg   result;                   // vreg holding the program result (if no RET)

    const sh_prim_set *prims;           // carried from the source program
} sh_chunk;

// --- VM run flags ------------------------------------------------------------
enum {
    SH_VM_FORCE_SCALAR = 1u << 0,  // disable the SSE/AVX path (force the scalar
                                   // lane-loop fallback) -- for differential tests
};

// =========================  S3 CROSS-UNIT SEAMS  ============================
// UNIT S3-1 (lowerer): verified sh_program -> bytecode chunk. `p` MUST be verified
// (p->verified). Returns SH_OK + *out (free with sh_chunk_free), else fills *err.
sh_status sh_lower(const sh_program *p, sh_chunk **out, sh_error *err);
void      sh_chunk_free(sh_chunk *c);

// UNIT S3-2 (VM): re-validate a chunk's internal consistency (vreg indices < nvregs,
// jump targets in range, aux ranges in bounds, prim indices valid). Defense in
// depth so a buggy lowerer cannot make the VM read/write OOB. Cheap, total.
sh_status sh_chunk_validate(const sh_chunk *c, sh_error *err);

// UNIT S3-2/3 (VM): execute a (validated) chunk on typed args. Runtime-bounds-checks
// every region access (a trap -> SH_ERR_BOUNDS), so a verified+lowered program can
// never corrupt memory. flags: SH_VM_FORCE_SCALAR to bypass SIMD. On SH_OK, *out
// holds the typed result.
sh_status sh_vm_run(const sh_chunk *c, const sh_value *args, uint32_t argc,
                    uint32_t flags, sh_value *out, sh_error *err);

#endif  // CARDINAL_LISP_SHADER_BYTECODE_H
