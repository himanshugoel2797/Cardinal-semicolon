// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

// S3 UNIT 1 -- the lowerer: verified AST -> flat register bytecode (sh_chunk).
// A mechanical post-verification pass (on the verified side of the moat).
// See notes/scratch/shader-s3-decision.md and sh_bytecode.h for the frozen
// cross-unit contract.
//
// Control flow:
//   IF   -> SHB_JMP_IFNOT to else-label; then-arm writes result vreg via MOV;
//            SHB_JMP to end-label; else-label; else-arm writes same result vreg;
//            end-label. Both arms MOV into the same pre-allocated join vreg.
//   LOOP -> emit induction inits (MOV into induction vregs), record header_pc,
//            lower body; RECUR evaluates new induction args into temporaries first
//            (parallel-assignment: compute ALL new values BEFORE overwriting any
//            induction vreg), then MOVs each temp into its induction slot vreg,
//            then JMP back to header_pc. The non-recur arm writes loop_result_vreg.
//
// Jump targets are backpatched: JMP/JMP_IFNOT are emitted with imm=0; the index
// is recorded and the actual pc is written back once known.
//
// The chunk does NOT end with SHB_RET -- chunk->result holds the final vreg.
// The VM reads chunk->result to pick the return value.

#include <stdlib.h>
#include <string.h>

#include "sh_bytecode.h"

// ---------------------------------------------------------------------------
// Builder context
// ---------------------------------------------------------------------------

#define INIT_CODE_CAP 64u
#define INIT_AUX_CAP  32u

typedef struct {
  const sh_program *p;
  sh_chunk         *c;
  sh_error         *err;

  // Capacities (sh_chunk only tracks count, not capacity)
  uint32_t code_cap;
  uint32_t aux_cap;

  // vreg high-water mark (next fresh vreg index)
  uint32_t next_vreg;

  // local slot -> vreg mapping; length = p->nlocals
  sh_vreg  *slot_vreg;

  // per-loop metadata; length = p->nloops
  sh_vreg   *loop_result;     // vreg written by the non-recur (exit) arm
  uint32_t  *loop_header_pc;  // pc of the loop body start (back-edge target)
} lower_ctx;

// ---------------------------------------------------------------------------
// Vreg allocator
// ---------------------------------------------------------------------------

static sh_vreg alloc_vreg(lower_ctx *ctx) {
  return ctx->next_vreg++;
}

// ---------------------------------------------------------------------------
// Grow-on-demand emit helpers
// ---------------------------------------------------------------------------

// Append one instruction; returns its pc, or UINT32_MAX on OOM.
static uint32_t emit_instr(lower_ctx *ctx, sh_instr instr) {
  sh_chunk *c = ctx->c;
  if (c->ncode == ctx->code_cap) {
    uint32_t new_cap = ctx->code_cap ? ctx->code_cap * 2 : INIT_CODE_CAP;
    sh_instr *nb = (sh_instr *)realloc(c->code, new_cap * sizeof(sh_instr));
    if (!nb) {
      sh_set_error(ctx->err, SH_ERR_OOM, -1, -1, "lowerer: OOM growing code");
      return UINT32_MAX;
    }
    c->code = nb;
    ctx->code_cap = new_cap;
  }
  uint32_t pc = c->ncode;
  c->code[pc] = instr;
  c->ncode++;
  return pc;
}

// Append one uint32 to aux; returns its index, or UINT32_MAX on OOM.
static uint32_t emit_aux(lower_ctx *ctx, uint32_t val) {
  sh_chunk *c = ctx->c;
  if (c->naux == ctx->aux_cap) {
    uint32_t new_cap = ctx->aux_cap ? ctx->aux_cap * 2 : INIT_AUX_CAP;
    uint32_t *nb = (uint32_t *)realloc(c->aux, new_cap * sizeof(uint32_t));
    if (!nb) {
      sh_set_error(ctx->err, SH_ERR_OOM, -1, -1, "lowerer: OOM growing aux");
      return UINT32_MAX;
    }
    c->aux = nb;
    ctx->aux_cap = new_cap;
  }
  uint32_t off = c->naux;
  c->aux[off] = val;
  c->naux++;
  return off;
}

// Return a zeroed instruction with the given op and all operands set to NONE.
static sh_instr blank(sh_bc_op op) {
  sh_instr i;
  memset(&i, 0, sizeof(i));
  i.op  = (uint16_t)op;
  i.dst = SH_VREG_NONE;
  i.a   = SH_VREG_NONE;
  i.b   = SH_VREG_NONE;
  i.c   = SH_VREG_NONE;
  return i;
}

// ---------------------------------------------------------------------------
// Forward declaration
// ---------------------------------------------------------------------------

static sh_status lower_node(lower_ctx *ctx, sh_nref ref, sh_vreg *out_vreg);

// ---------------------------------------------------------------------------
// lower_node: recursive descent over the verified AST
// ---------------------------------------------------------------------------

static sh_status lower_node(lower_ctx *ctx, sh_nref ref, sh_vreg *out_vreg) {
  const sh_program *p = ctx->p;

  if (ref == SH_NREF_NONE || ref >= p->nnodes)
    return sh_set_error(ctx->err, SH_ERR_INTERNAL, -1, -1,
                        "lower_node: invalid ref %u", ref);

  const sh_node *n = &p->nodes[ref];
  sh_op op = (sh_op)n->op;

  switch (op) {

    // -------------------------------------------------------------------------
    // CONST: literal -> SHB_CONST carrying the sub-flag and imm64 payload.
    // -------------------------------------------------------------------------
    case SH_OP_CONST: {
      sh_vreg dst = alloc_vreg(ctx);
      sh_instr ins = blank(SHB_CONST);
      ins.dst   = dst;
      ins.sub   = n->sub;          // 0=int, 1=float bits, 2=bool
      ins.kind  = n->type.kind;
      ins.imm64 = n->imm;
      if (emit_instr(ctx, ins) == UINT32_MAX) return SH_ERR_OOM;
      *out_vreg = dst;
      return SH_OK;
    }

    // -------------------------------------------------------------------------
    // PARAM: parameter index in `a` -> SHB_PARAM with imm = param index.
    // -------------------------------------------------------------------------
    case SH_OP_PARAM: {
      sh_vreg dst = alloc_vreg(ctx);
      sh_instr ins = blank(SHB_PARAM);
      ins.dst  = dst;
      ins.kind = n->type.kind;
      ins.imm  = n->a;
      if (emit_instr(ctx, ins) == UINT32_MAX) return SH_ERR_OOM;
      *out_vreg = dst;
      return SH_OK;
    }

    // -------------------------------------------------------------------------
    // LOCAL: return the vreg that was pre-assigned to this slot.
    // -------------------------------------------------------------------------
    case SH_OP_LOCAL: {
      uint32_t slot = n->a;
      if (slot >= p->nlocals)
        return sh_set_error(ctx->err, SH_ERR_INTERNAL, -1, -1,
                            "LOCAL: slot %u >= nlocals %u", slot, p->nlocals);
      *out_vreg = ctx->slot_vreg[slot];
      return SH_OK;
    }

    // -------------------------------------------------------------------------
    // UNOP: SHB_UNOP; sub = sh_unop; CVT carries the target kind in node.type.
    // -------------------------------------------------------------------------
    case SH_OP_UNOP: {
      sh_vreg va;
      sh_status s = lower_node(ctx, n->a, &va);
      if (s != SH_OK) return s;
      sh_vreg dst = alloc_vreg(ctx);
      sh_instr ins = blank(SHB_UNOP);
      ins.dst  = dst;
      ins.sub  = n->sub;
      ins.kind = n->type.kind;
      ins.a    = va;
      if (emit_instr(ctx, ins) == UINT32_MAX) return SH_ERR_OOM;
      *out_vreg = dst;
      return SH_OK;
    }

    // -------------------------------------------------------------------------
    // BINOP: SHB_BINOP; sub = sh_binop; kind = result/operand scalar kind.
    // -------------------------------------------------------------------------
    case SH_OP_BINOP: {
      sh_vreg va, vb;
      sh_status s = lower_node(ctx, n->a, &va);
      if (s != SH_OK) return s;
      s = lower_node(ctx, n->b, &vb);
      if (s != SH_OK) return s;
      sh_vreg dst = alloc_vreg(ctx);
      sh_instr ins = blank(SHB_BINOP);
      ins.dst  = dst;
      ins.sub  = n->sub;
      ins.kind = n->type.kind;
      ins.a    = va;
      ins.b    = vb;
      if (emit_instr(ctx, ins) == UINT32_MAX) return SH_ERR_OOM;
      *out_vreg = dst;
      return SH_OK;
    }

    // -------------------------------------------------------------------------
    // CMP: SHB_CMP; sub = sh_cmp; kind = OPERAND kind (result is always bool).
    // The interpreter reads operand kind from p->nodes[n->a].type.kind; we carry
    // that same value in instr.kind.
    // -------------------------------------------------------------------------
    case SH_OP_CMP: {
      sh_vreg va, vb;
      sh_status s = lower_node(ctx, n->a, &va);
      if (s != SH_OK) return s;
      s = lower_node(ctx, n->b, &vb);
      if (s != SH_OK) return s;
      sh_kind operand_kind = (sh_kind)p->nodes[n->a].type.kind;
      sh_vreg dst = alloc_vreg(ctx);
      sh_instr ins = blank(SHB_CMP);
      ins.dst  = dst;
      ins.sub  = n->sub;
      ins.kind = (uint8_t)operand_kind;
      ins.a    = va;
      ins.b    = vb;
      if (emit_instr(ctx, ins) == UINT32_MAX) return SH_ERR_OOM;
      *out_vreg = dst;
      return SH_OK;
    }

    // -------------------------------------------------------------------------
    // IF: emit cond; JMP_IFNOT [else]; then-arm -> join vreg; JMP [end];
    //     [else]: else-arm -> join vreg; [end].  Both arms MOV into join vreg.
    // -------------------------------------------------------------------------
    case SH_OP_IF: {
      // Pre-allocate the join (phi) vreg.
      sh_vreg join = alloc_vreg(ctx);

      // Evaluate condition.
      sh_vreg cond_v;
      sh_status s = lower_node(ctx, n->a, &cond_v);
      if (s != SH_OK) return s;

      // Emit JMP_IFNOT; target will be backpatched.
      sh_instr jif = blank(SHB_JMP_IFNOT);
      jif.a = cond_v;
      uint32_t jif_pc = emit_instr(ctx, jif);
      if (jif_pc == UINT32_MAX) return SH_ERR_OOM;

      // Then-arm.
      sh_vreg then_v;
      s = lower_node(ctx, n->b, &then_v);
      if (s != SH_OK) return s;
      // MOV then_v -> join.
      sh_instr mov_then = blank(SHB_MOV);
      mov_then.dst  = join;
      mov_then.a    = then_v;
      mov_then.kind = n->type.kind;
      if (emit_instr(ctx, mov_then) == UINT32_MAX) return SH_ERR_OOM;

      // Unconditional jump to end; target will be backpatched.
      sh_instr jmp = blank(SHB_JMP);
      uint32_t jmp_pc = emit_instr(ctx, jmp);
      if (jmp_pc == UINT32_MAX) return SH_ERR_OOM;

      // Else label: backpatch JMP_IFNOT.
      uint32_t else_pc = ctx->c->ncode;
      ctx->c->code[jif_pc].imm = else_pc;

      // Else-arm.
      sh_vreg else_v;
      s = lower_node(ctx, n->c, &else_v);
      if (s != SH_OK) return s;
      // MOV else_v -> join.
      sh_instr mov_else = blank(SHB_MOV);
      mov_else.dst  = join;
      mov_else.a    = else_v;
      mov_else.kind = n->type.kind;
      if (emit_instr(ctx, mov_else) == UINT32_MAX) return SH_ERR_OOM;

      // End label: backpatch JMP.
      uint32_t end_pc = ctx->c->ncode;
      ctx->c->code[jmp_pc].imm = end_pc;

      *out_vreg = join;
      return SH_OK;
    }

    // -------------------------------------------------------------------------
    // LET: evaluate each init expr, MOV result into the slot vreg, then
    // recurse into the body.
    // -------------------------------------------------------------------------
    case SH_OP_LET: {
      uint32_t first_slot = n->a;
      for (uint32_t i = 0; i < n->aux_len; i++) {
        sh_nref init_ref = p->aux[n->aux_off + i];
        sh_vreg init_v;
        sh_status s = lower_node(ctx, init_ref, &init_v);
        if (s != SH_OK) return s;
        // MOV into the pre-allocated slot vreg.
        sh_instr mov = blank(SHB_MOV);
        mov.dst  = ctx->slot_vreg[first_slot + i];
        mov.a    = init_v;
        mov.kind = p->nodes[init_ref].type.kind;
        if (emit_instr(ctx, mov) == UINT32_MAX) return SH_ERR_OOM;
      }
      return lower_node(ctx, n->b, out_vreg);
    }

    // -------------------------------------------------------------------------
    // LOOP (named-let):
    //   1. Evaluate each init expr -> MOV into its induction slot vreg.
    //   2. Record header_pc.
    //   3. Allocate loop_result vreg.
    //   4. Lower body (which contains an IF whose exit arm writes loop_result
    //      and whose recur arm emits new-val-temps + MOVs + JMP back).
    //   5. The body's returned vreg IS loop_result (the exit path of the
    //      body's IF sets it; the recur path jumps back before reaching the
    //      join MOV, making the join dead on that path -- harmless).
    // -------------------------------------------------------------------------
    case SH_OP_LOOP: {
      uint32_t loop_idx = n->a;
      const sh_loop *lp = &p->loops[loop_idx];

      // Evaluate induction var inits -> MOV into their slot vregs.
      for (uint32_t vi = 0; vi < lp->nvars; vi++) {
        sh_nref init_ref = p->aux[lp->init_off + vi];
        sh_vreg init_v;
        sh_status s = lower_node(ctx, init_ref, &init_v);
        if (s != SH_OK) return s;
        sh_instr mov = blank(SHB_MOV);
        mov.dst  = ctx->slot_vreg[lp->var_slot0 + vi];
        mov.a    = init_v;
        mov.kind = p->nodes[init_ref].type.kind;
        if (emit_instr(ctx, mov) == UINT32_MAX) return SH_ERR_OOM;
      }

      // Record header pc (back-edge target).
      ctx->loop_header_pc[loop_idx] = ctx->c->ncode;

      // Allocate the loop result vreg.
      ctx->loop_result[loop_idx] = alloc_vreg(ctx);

      // Lower the body.
      sh_vreg body_v;
      sh_status s = lower_node(ctx, lp->body, &body_v);
      if (s != SH_OK) return s;

      // body_v is the join vreg of the body's IF (allocated by the IF case).
      // That vreg is the same as loop_result[loop_idx] because the IF case's
      // exit arm MOVed into the join and the RECUR arm MOVed into loop_result
      // then jumped back -- both paths write loop_result via that join. We
      // set the loop result explicitly here for clarity.
      sh_vreg loop_res = ctx->loop_result[loop_idx];
      if (body_v != loop_res) {
        // Shouldn't happen for well-formed bounded loops, but guard anyway.
        sh_instr mov = blank(SHB_MOV);
        mov.dst  = loop_res;
        mov.a    = body_v;
        if (emit_instr(ctx, mov) == UINT32_MAX) return SH_ERR_OOM;
      }

      *out_vreg = loop_res;
      return SH_OK;
    }

    // -------------------------------------------------------------------------
    // RECUR: parallel-assignment -> JMP back to loop header.
    //   1. Evaluate ALL new induction-arg exprs into fresh temporaries.
    //   2. MOV each temp into its induction slot vreg (complete the parallel
    //      assignment only after all temps are computed).
    //   3. SHB_JMP back to loop header.
    // Returns loop_result[loop_idx] so the IF join MOV is syntactically valid
    // (that MOV is dead -- the JMP preceeds it -- but nvregs still covers it).
    // -------------------------------------------------------------------------
    case SH_OP_RECUR: {
      uint32_t loop_idx = n->a;
      const sh_loop *lp = &p->loops[loop_idx];
      uint32_t nargs = n->aux_len;

      // Allocate temporary vregs for the new induction values.
      sh_vreg *tmps = (sh_vreg *)malloc(nargs * sizeof(sh_vreg));
      if (!tmps)
        return sh_set_error(ctx->err, SH_ERR_OOM, -1, -1,
                            "RECUR: OOM for temporaries");

      // Phase 1: evaluate all new induction args, then COPY each into a FRESH
      // temp vreg. The copy is essential: lower_node for a bare LOCAL returns
      // that slot's own vreg, so without the copy a temp could alias an
      // induction slot that Phase 2 overwrites -- breaking a parallel swap like
      // (loop ... b a). Materializing into fresh temps makes Phase 2's writes
      // unable to clobber a still-unread new value.
      for (uint32_t i = 0; i < nargs; i++) {
        sh_nref arg_ref = p->aux[n->aux_off + i];
        sh_vreg v;
        sh_status s = lower_node(ctx, arg_ref, &v);
        if (s != SH_OK) { free(tmps); return s; }
        sh_nref init_ref = p->aux[lp->init_off + i];
        sh_kind kind = (sh_kind)p->nodes[init_ref].type.kind;
        sh_vreg tmp = alloc_vreg(ctx);
        sh_instr mov = blank(SHB_MOV);
        mov.dst = tmp;
        mov.a = v;
        mov.kind = (uint8_t)kind;
        if (emit_instr(ctx, mov) == UINT32_MAX) { free(tmps); return SH_ERR_OOM; }
        tmps[i] = tmp;
      }

      // Phase 2: MOV each temp into its induction slot vreg.
      for (uint32_t i = 0; i < nargs; i++) {
        sh_nref init_ref = p->aux[lp->init_off + i];
        sh_kind kind = (sh_kind)p->nodes[init_ref].type.kind;
        sh_instr mov = blank(SHB_MOV);
        mov.dst  = ctx->slot_vreg[lp->var_slot0 + i];
        mov.a    = tmps[i];
        mov.kind = (uint8_t)kind;
        if (emit_instr(ctx, mov) == UINT32_MAX) { free(tmps); return SH_ERR_OOM; }
      }
      free(tmps);

      // Phase 3: JMP back to loop header.
      sh_instr jmp = blank(SHB_JMP);
      jmp.imm = ctx->loop_header_pc[loop_idx];
      if (emit_instr(ctx, jmp) == UINT32_MAX) return SH_ERR_OOM;

      // Return loop_result so the enclosing IF join MOV has a valid source vreg.
      // That MOV is dead (JMP above skips it), but the vreg index is within
      // nvregs so the chunk validator will accept it.
      *out_vreg = ctx->loop_result[loop_idx];
      return SH_OK;
    }

    // -------------------------------------------------------------------------
    // CALL: evaluate args -> collect vreg indices in aux -> SHB_CALL.
    // -------------------------------------------------------------------------
    case SH_OP_CALL: {
      uint32_t prim_idx = n->a;
      uint32_t nargs    = n->aux_len;

      uint32_t aux_off = ctx->c->naux;
      for (uint32_t i = 0; i < nargs; i++) {
        sh_nref arg_ref = p->aux[n->aux_off + i];
        sh_vreg av;
        sh_status s = lower_node(ctx, arg_ref, &av);
        if (s != SH_OK) return s;
        if (emit_aux(ctx, (uint32_t)av) == UINT32_MAX) return SH_ERR_OOM;
      }

      sh_vreg dst = alloc_vreg(ctx);
      sh_instr ins = blank(SHB_CALL);
      ins.dst     = dst;
      ins.kind    = n->type.kind;
      ins.imm     = prim_idx;
      ins.aux_off = aux_off;
      ins.aux_len = nargs;
      if (emit_instr(ctx, ins) == UINT32_MAX) return SH_ERR_OOM;
      *out_vreg = dst;
      return SH_OK;
    }

    // -------------------------------------------------------------------------
    // REGION_LOAD: SHB_RLOAD; kind = elem kind from region operand's lane_kind.
    // -------------------------------------------------------------------------
    case SH_OP_REGION_LOAD: {
      sh_vreg va, vb;
      sh_status s = lower_node(ctx, n->a, &va);
      if (s != SH_OK) return s;
      s = lower_node(ctx, n->b, &vb);
      if (s != SH_OK) return s;
      sh_kind elem_kind = (sh_kind)p->nodes[n->a].type.lane_kind;
      sh_vreg dst = alloc_vreg(ctx);
      sh_instr ins = blank(SHB_RLOAD);
      ins.dst  = dst;
      ins.kind = (uint8_t)elem_kind;
      ins.a    = va;
      ins.b    = vb;
      if (emit_instr(ctx, ins) == UINT32_MAX) return SH_ERR_OOM;
      *out_vreg = dst;
      return SH_OK;
    }

    // -------------------------------------------------------------------------
    // REGION_STORE: SHB_RSTORE; dst = c (the stored value, matching interp).
    // -------------------------------------------------------------------------
    case SH_OP_REGION_STORE: {
      sh_vreg va, vb, vc;
      sh_status s = lower_node(ctx, n->a, &va);
      if (s != SH_OK) return s;
      s = lower_node(ctx, n->b, &vb);
      if (s != SH_OK) return s;
      s = lower_node(ctx, n->c, &vc);
      if (s != SH_OK) return s;
      sh_kind elem_kind = (sh_kind)p->nodes[n->a].type.lane_kind;
      sh_vreg dst = alloc_vreg(ctx);
      sh_instr ins = blank(SHB_RSTORE);
      ins.dst  = dst;
      ins.kind = (uint8_t)elem_kind;
      ins.a    = va;
      ins.b    = vb;
      ins.c    = vc;
      if (emit_instr(ctx, ins) == UINT32_MAX) return SH_ERR_OOM;
      *out_vreg = dst;
      return SH_OK;
    }

    // -------------------------------------------------------------------------
    // REGION_LEN: SHB_RLEN; result is always u32.
    // -------------------------------------------------------------------------
    case SH_OP_REGION_LEN: {
      sh_vreg va;
      sh_status s = lower_node(ctx, n->a, &va);
      if (s != SH_OK) return s;
      sh_vreg dst = alloc_vreg(ctx);
      sh_instr ins = blank(SHB_RLEN);
      ins.dst  = dst;
      ins.kind = (uint8_t)SH_K_U32;
      ins.a    = va;
      if (emit_instr(ctx, ins) == UINT32_MAX) return SH_ERR_OOM;
      *out_vreg = dst;
      return SH_OK;
    }

    // -------------------------------------------------------------------------
    // VSPLAT: broadcast scalar -> vector; kind = lane_kind, lanes from type.
    // -------------------------------------------------------------------------
    case SH_OP_VSPLAT: {
      sh_vreg va;
      sh_status s = lower_node(ctx, n->a, &va);
      if (s != SH_OK) return s;
      sh_vreg dst = alloc_vreg(ctx);
      sh_instr ins = blank(SHB_VSPLAT);
      ins.dst   = dst;
      ins.kind  = n->type.lane_kind;
      ins.lanes = n->type.lanes;
      ins.a     = va;
      if (emit_instr(ctx, ins) == UINT32_MAX) return SH_ERR_OOM;
      *out_vreg = dst;
      return SH_OK;
    }

    // -------------------------------------------------------------------------
    // VBINOP: lane-wise binop; kind = lane_kind of operands/result.
    // -------------------------------------------------------------------------
    case SH_OP_VBINOP: {
      sh_vreg va, vb;
      sh_status s = lower_node(ctx, n->a, &va);
      if (s != SH_OK) return s;
      s = lower_node(ctx, n->b, &vb);
      if (s != SH_OK) return s;
      sh_vreg dst = alloc_vreg(ctx);
      sh_instr ins = blank(SHB_VBINOP);
      ins.dst   = dst;
      ins.sub   = n->sub;
      ins.kind  = n->type.lane_kind;
      ins.lanes = n->type.lanes;
      ins.a     = va;
      ins.b     = vb;
      if (emit_instr(ctx, ins) == UINT32_MAX) return SH_ERR_OOM;
      *out_vreg = dst;
      return SH_OK;
    }

    // -------------------------------------------------------------------------
    // VCMP: lane-wise compare; kind = operand lane_kind (result is bool-mask).
    // The interpreter gets operand kind from p->nodes[n->a].type.lane_kind.
    // -------------------------------------------------------------------------
    case SH_OP_VCMP: {
      sh_vreg va, vb;
      sh_status s = lower_node(ctx, n->a, &va);
      if (s != SH_OK) return s;
      s = lower_node(ctx, n->b, &vb);
      if (s != SH_OK) return s;
      sh_kind operand_lk = (sh_kind)p->nodes[n->a].type.lane_kind;
      sh_vreg dst = alloc_vreg(ctx);
      sh_instr ins = blank(SHB_VCMP);
      ins.dst   = dst;
      ins.sub   = n->sub;
      ins.kind  = (uint8_t)operand_lk;
      ins.lanes = n->type.lanes;
      ins.a     = va;
      ins.b     = vb;
      if (emit_instr(ctx, ins) == UINT32_MAX) return SH_ERR_OOM;
      *out_vreg = dst;
      return SH_OK;
    }

    // -------------------------------------------------------------------------
    // VSELECT: mask ? then : else lane-wise.
    // -------------------------------------------------------------------------
    case SH_OP_VSELECT: {
      sh_vreg va, vb, vc;
      sh_status s = lower_node(ctx, n->a, &va);
      if (s != SH_OK) return s;
      s = lower_node(ctx, n->b, &vb);
      if (s != SH_OK) return s;
      s = lower_node(ctx, n->c, &vc);
      if (s != SH_OK) return s;
      sh_vreg dst = alloc_vreg(ctx);
      sh_instr ins = blank(SHB_VSELECT);
      ins.dst   = dst;
      ins.kind  = n->type.lane_kind;
      ins.lanes = n->type.lanes;
      ins.a     = va;
      ins.b     = vb;
      ins.c     = vc;
      if (emit_instr(ctx, ins) == UINT32_MAX) return SH_ERR_OOM;
      *out_vreg = dst;
      return SH_OK;
    }

    // -------------------------------------------------------------------------
    // VSHUFFLE: constant lane indices -> aux; kind = lane_kind, lanes = result
    // lane count = aux_len.
    // -------------------------------------------------------------------------
    case SH_OP_VSHUFFLE: {
      sh_vreg va;
      sh_status s = lower_node(ctx, n->a, &va);
      if (s != SH_OK) return s;

      uint32_t aux_off = ctx->c->naux;
      for (uint32_t i = 0; i < n->aux_len; i++) {
        if (emit_aux(ctx, p->aux[n->aux_off + i]) == UINT32_MAX)
          return SH_ERR_OOM;
      }

      sh_vreg dst = alloc_vreg(ctx);
      sh_instr ins = blank(SHB_VSHUFFLE);
      ins.dst     = dst;
      ins.kind    = n->type.lane_kind;
      ins.lanes   = n->type.lanes;
      ins.a       = va;
      ins.aux_off = aux_off;
      ins.aux_len = n->aux_len;
      if (emit_instr(ctx, ins) == UINT32_MAX) return SH_ERR_OOM;
      *out_vreg = dst;
      return SH_OK;
    }

    // -------------------------------------------------------------------------
    // VREDUCE: reduce vector to scalar; kind = lane_kind of input vector.
    // DOT (SH_RED_DOT) reads a second vector operand via n->b.
    // -------------------------------------------------------------------------
    case SH_OP_VREDUCE: {
      sh_vreg va;
      sh_status s = lower_node(ctx, n->a, &va);
      if (s != SH_OK) return s;
      sh_vreg vb = SH_VREG_NONE;
      if ((sh_reduce)n->sub == SH_RED_DOT) {
        s = lower_node(ctx, n->b, &vb);
        if (s != SH_OK) return s;
      }
      sh_kind src_lk    = (sh_kind)p->nodes[n->a].type.lane_kind;
      uint8_t src_lanes = p->nodes[n->a].type.lanes;
      sh_vreg dst = alloc_vreg(ctx);
      sh_instr ins = blank(SHB_VREDUCE);
      ins.dst   = dst;
      ins.sub   = n->sub;
      ins.kind  = (uint8_t)src_lk;
      ins.lanes = src_lanes;
      ins.a     = va;
      ins.b     = vb;
      if (emit_instr(ctx, ins) == UINT32_MAX) return SH_ERR_OOM;
      *out_vreg = dst;
      return SH_OK;
    }

    // -------------------------------------------------------------------------
    // VLANE: scalar lane extraction; kind = lane_kind of input; imm = lane idx.
    // -------------------------------------------------------------------------
    case SH_OP_VLANE: {
      sh_vreg va;
      sh_status s = lower_node(ctx, n->a, &va);
      if (s != SH_OK) return s;
      sh_kind src_lk = (sh_kind)p->nodes[n->a].type.lane_kind;
      sh_vreg dst = alloc_vreg(ctx);
      sh_instr ins = blank(SHB_VLANE);
      ins.dst  = dst;
      ins.kind = (uint8_t)src_lk;
      ins.a    = va;
      ins.imm  = (uint32_t)n->imm;
      if (emit_instr(ctx, ins) == UINT32_MAX) return SH_ERR_OOM;
      *out_vreg = dst;
      return SH_OK;
    }

    // -------------------------------------------------------------------------
    // VREGION_LOAD: SHB_VRLOAD; imm=N, kind=elem, lanes=N.
    // -------------------------------------------------------------------------
    case SH_OP_VREGION_LOAD: {
      sh_vreg va_r, vb_r;
      sh_status s = lower_node(ctx, n->a, &va_r);
      if (s != SH_OK) return s;
      s = lower_node(ctx, n->b, &vb_r);
      if (s != SH_OK) return s;
      sh_kind elem_kind = (sh_kind)p->nodes[n->a].type.lane_kind;
      uint8_t N = (uint8_t)n->imm;
      sh_vreg dst = alloc_vreg(ctx);
      sh_instr ins = blank(SHB_VRLOAD);
      ins.dst   = dst;
      ins.kind  = (uint8_t)elem_kind;
      ins.lanes = N;
      ins.imm   = (uint32_t)N;
      ins.a     = va_r;
      ins.b     = vb_r;
      if (emit_instr(ctx, ins) == UINT32_MAX) return SH_ERR_OOM;
      *out_vreg = dst;
      return SH_OK;
    }

    // -------------------------------------------------------------------------
    // VREGION_STORE: SHB_VRSTORE; kind=elem, lanes=vector's lane count.
    // Result = the stored vector (c).
    // -------------------------------------------------------------------------
    case SH_OP_VREGION_STORE: {
      sh_vreg va_r, vb_r, vc_r;
      sh_status s = lower_node(ctx, n->a, &va_r);
      if (s != SH_OK) return s;
      s = lower_node(ctx, n->b, &vb_r);
      if (s != SH_OK) return s;
      s = lower_node(ctx, n->c, &vc_r);
      if (s != SH_OK) return s;
      sh_kind elem_kind = (sh_kind)p->nodes[n->a].type.lane_kind;
      uint8_t nl = p->nodes[n->c].type.lanes;
      sh_vreg dst = alloc_vreg(ctx);
      sh_instr ins = blank(SHB_VRSTORE);
      ins.dst   = dst;
      ins.kind  = (uint8_t)elem_kind;
      ins.lanes = nl;
      ins.a     = va_r;
      ins.b     = vb_r;
      ins.c     = vc_r;
      if (emit_instr(ctx, ins) == UINT32_MAX) return SH_ERR_OOM;
      *out_vreg = dst;
      return SH_OK;
    }

    default:
      return sh_set_error(ctx->err, SH_ERR_INTERNAL, -1, -1,
                          "lower_node: unknown op %u at node %u", n->op, ref);
  }
}

// ---------------------------------------------------------------------------
// sh_lower: public entry point
// ---------------------------------------------------------------------------

sh_status sh_lower(const sh_program *p, sh_chunk **out, sh_error *err) {
  if (out) *out = NULL;
  if (!p || !p->verified)
    return sh_set_error(err, SH_ERR_INTERNAL, -1, -1,
                        "sh_lower: NULL or unverified program");

  sh_chunk *c = (sh_chunk *)calloc(1, sizeof(sh_chunk));
  if (!c)
    return sh_set_error(err, SH_ERR_OOM, -1, -1,
                        "sh_lower: OOM allocating chunk");

  // Copy program metadata.
  memcpy(c->name, p->name, sizeof(c->name));
  c->nparams = p->nparams;
  memcpy(c->params, p->params, p->nparams * sizeof(sh_type));
  c->ret    = p->ret;
  c->prims  = p->prims;
  c->result = SH_VREG_NONE;

  // Initial code/aux allocations (will grow as needed).
  c->code = (sh_instr *)malloc(INIT_CODE_CAP * sizeof(sh_instr));
  c->aux  = (uint32_t *)malloc(INIT_AUX_CAP  * sizeof(uint32_t));
  if (!c->code || !c->aux) {
    sh_chunk_free(c);
    return sh_set_error(err, SH_ERR_OOM, -1, -1,
                        "sh_lower: OOM for code/aux arrays");
  }

  lower_ctx ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.p          = p;
  ctx.c          = c;
  ctx.err        = err;
  ctx.code_cap   = INIT_CODE_CAP;
  ctx.aux_cap    = INIT_AUX_CAP;
  ctx.next_vreg  = 0;

  // Pre-allocate one vreg per local slot so LOCAL reads land in the right vreg
  // regardless of evaluation order within let/loop inits.
  if (p->nlocals > 0) {
    ctx.slot_vreg = (sh_vreg *)malloc(p->nlocals * sizeof(sh_vreg));
    if (!ctx.slot_vreg) {
      sh_chunk_free(c);
      return sh_set_error(err, SH_ERR_OOM, -1, -1,
                          "sh_lower: OOM for slot_vreg");
    }
    for (uint32_t i = 0; i < p->nlocals; i++)
      ctx.slot_vreg[i] = ctx.next_vreg++;
  }

  // Per-loop scratch arrays.
  if (p->nloops > 0) {
    ctx.loop_result    = (sh_vreg *)  malloc(p->nloops * sizeof(sh_vreg));
    ctx.loop_header_pc = (uint32_t *) malloc(p->nloops * sizeof(uint32_t));
    if (!ctx.loop_result || !ctx.loop_header_pc) {
      free(ctx.slot_vreg);
      free(ctx.loop_result);
      free(ctx.loop_header_pc);
      sh_chunk_free(c);
      return sh_set_error(err, SH_ERR_OOM, -1, -1,
                          "sh_lower: OOM for loop metadata");
    }
    memset(ctx.loop_result,    0, p->nloops * sizeof(sh_vreg));
    memset(ctx.loop_header_pc, 0, p->nloops * sizeof(uint32_t));
  }

  // Lower the program.
  sh_vreg result_vreg = SH_VREG_NONE;
  sh_status s = lower_node(&ctx, p->root, &result_vreg);

  // Clean up context scratch; chunk data is now owned by *out (or freed below).
  free(ctx.slot_vreg);
  free(ctx.loop_result);
  free(ctx.loop_header_pc);

  if (s != SH_OK) {
    sh_chunk_free(c);
    return s;
  }

  c->nvregs = ctx.next_vreg;
  c->result = result_vreg;
  *out = c;
  return SH_OK;
}

// ---------------------------------------------------------------------------
// sh_chunk_free: release the owned allocation group.
// ---------------------------------------------------------------------------

void sh_chunk_free(sh_chunk *c) {
  if (!c) return;
  free(c->code);
  free(c->aux);
  free(c);
}
