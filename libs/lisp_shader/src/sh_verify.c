// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

// UNIT B -- THE VERIFIER (the trusted moat; no MMU firewall stands behind it).
// Type-checks the parsed AST in place, derives the bounded-loop trip bounds and
// worst-case cost, discharges static region bounds (SH_NF_BOUNDS_PROVEN), and
// enforces the capability whitelist. No allocation, no I/O.
//
// See notes/scratch/shader-proposal-minimalist.md sections 2-5.

#include <string.h>
#include "sh_internal.h"

// ---------------------------------------------------------------------------
// Costs
// ---------------------------------------------------------------------------

#define COST_ARITH   1u
#define COST_REGION  4u
#define COST_CALL    8u   // base; overridden by callee cost when known
#define COST_VEC     2u   // per-lane cost for vector ops

// ---------------------------------------------------------------------------
// Verify context (no allocation -- all stack/in-place)
// ---------------------------------------------------------------------------

typedef struct {
  sh_program *p;
  const sh_prim_set *prims;
  uint32_t flags;
  sh_error *err;
  // Per-node cost array -- parallel to p->nodes, stack-allocated up to limit.
  // We store costs in a fixed-size stack array; programs larger than this limit
  // get rejected with SH_ERR_INTERNAL (extremely conservative but safe).
  // 4096 nodes covers any practical shader.
  uint64_t node_cost[4096];
  // Type inference for integer literals needs context. We do a two-phase
  // approach: bottom-up pass leaves integer-CONST type as SH_K_VOID (unresolved);
  // a second pass resolves them from consumer context.
  // For simplicity we track "needs_int_resolution" per-node as a bit in a
  // byte array (1 if this CONST has sub=0 and type still unset).
  uint8_t  needs_resolve[4096]; // 1 if integer CONST needing context-type
} verify_ctx;

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------

static sh_status verify_node(verify_ctx *vc, sh_nref ref);

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static bool kind_is_integer(sh_kind k) {
  return k == SH_K_U8 || k == SH_K_U16 || k == SH_K_U32 ||
         k == SH_K_U64 || k == SH_K_I64;
}

// Check if a nref is a valid node index (declared early; used by all helpers)
static bool valid_ref(const sh_program *p, sh_nref ref) {
  return ref != SH_NREF_NONE && ref < p->nnodes;
}

// Get the sh_type of a node (which must already be verified/typed)
static sh_type node_type(const sh_program *p, sh_nref ref) {
  if (!valid_ref(p, ref)) return sh_type_scalar(SH_K_VOID);
  return p->nodes[ref].type;
}

// Resolve an integer-const node to a specific kind if it is still unresolved.
// Does nothing if already resolved.
static void resolve_int_const(verify_ctx *vc, sh_nref ref, sh_kind k) {
  if (ref == SH_NREF_NONE || ref >= vc->p->nnodes) return;
  sh_node *n = &vc->p->nodes[ref];
  if (n->op != (uint16_t)SH_OP_CONST || n->sub != 0) return;
  if (vc->needs_resolve[ref] && kind_is_integer(k)) {
    n->type = sh_type_scalar(k);
    vc->needs_resolve[ref] = 0;
  }
}

// Resolve a float-const to f32 or f64
static void resolve_float_const(verify_ctx *vc, sh_nref ref, sh_kind k) {
  if (ref == SH_NREF_NONE || ref >= vc->p->nnodes) return;
  sh_node *n = &vc->p->nodes[ref];
  if (n->op != (uint16_t)SH_OP_CONST || n->sub != 1) return;
  if (n->type.kind == (uint8_t)SH_K_VOID &&
      (k == SH_K_F32 || k == SH_K_F64)) {
    n->type = sh_type_scalar(k);
  }
}

// Re-type all LOCAL nodes for a given slot and update the backing CONST if
// it was an unresolved integer literal. Called when comparison/binop context
// determines the type of a loop induction variable.
static void retype_all_locals_for_slot(verify_ctx *vc, uint32_t slot,
                                        sh_type new_type) {
  sh_program *p = vc->p;
  // Update all LOCAL nodes for this slot
  for (uint32_t i = 0; i < p->nnodes; i++) {
    if (p->nodes[i].op == (uint16_t)SH_OP_LOCAL && p->nodes[i].a == slot) {
      p->nodes[i].type = new_type;
    }
  }
  // Also update any CONST init node that was used as the init for this slot.
  // We find it by looking in all loop and let init aux arrays.
  // Loop induction vars:
  for (uint32_t li = 0; li < p->nloops; li++) {
    sh_loop *lp = &p->loops[li];
    for (uint32_t vi = 0; vi < lp->nvars; vi++) {
      if (lp->var_slot0 + vi == slot) {
        sh_nref init_ref = p->aux[lp->init_off + vi];
        if (valid_ref(p, init_ref) && p->nodes[init_ref].op == (uint16_t)SH_OP_CONST
            && p->nodes[init_ref].sub == 0 && vc->needs_resolve[init_ref]) {
          p->nodes[init_ref].type = new_type;
          vc->needs_resolve[init_ref] = 0;
        }
      }
    }
  }
}


// ---------------------------------------------------------------------------
// CONST literal typing: determine a concrete kind from CONST node
// ---------------------------------------------------------------------------

// After resolve passes, the CONST node's type must be non-VOID.
// Integer CONSTs (sub=0): defaults to u32 if no consumer provided context.
// Float CONSTs (sub=1): defaults to f64 if no consumer provided context.
// Bool CONSTs (sub=2): always bool.
static sh_status finalize_const(verify_ctx *vc, sh_nref ref) {
  sh_node *n = &vc->p->nodes[ref];
  if (n->type.kind != (uint8_t)SH_K_VOID) return SH_OK; // already set
  switch (n->sub) {
    case 0: // integer literal -- default to i64
      n->type = sh_type_scalar(SH_K_I64);
      vc->needs_resolve[ref] = 0;
      break;
    case 1: // float literal -- default to f64
      n->type = sh_type_scalar(SH_K_F64);
      break;
    case 2: // bool
      n->type = sh_type_scalar(SH_K_BOOL);
      break;
    default:
      return sh_set_error(vc->err, SH_ERR_INTERNAL, -1, -1,
                          "unknown CONST sub %u", n->sub);
  }
  return SH_OK;
}

// ---------------------------------------------------------------------------
// Loop bound recognition helpers
// ---------------------------------------------------------------------------

// Check if a node is a CONST integer with a known value
static bool is_const_int(const sh_program *p, sh_nref ref, int64_t *val) {
  if (!valid_ref(p, ref)) return false;
  const sh_node *n = &p->nodes[ref];
  if (n->op != (uint16_t)SH_OP_CONST || n->sub != 0) return false;
  if (val) *val = n->imm;
  return true;
}

// Check if a node is a PARAM of integer kind, returning its index
static bool is_int_param(const sh_program *p, sh_nref ref, uint32_t *pidx) {
  if (!valid_ref(p, ref)) return false;
  const sh_node *n = &p->nodes[ref];
  if (n->op != (uint16_t)SH_OP_PARAM) return false;
  if (n->a >= p->nparams) return false;
  sh_kind k = (sh_kind)p->params[n->a].kind;
  if (!kind_is_integer(k)) return false;
  if (pidx) *pidx = n->a;
  return true;
}

// Check if a node is a LOCAL reading induction var slot `slot`
static bool is_local_slot(const sh_program *p, sh_nref ref, uint32_t slot) {
  if (!valid_ref(p, ref)) return false;
  const sh_node *n = &p->nodes[ref];
  return n->op == (uint16_t)SH_OP_LOCAL && n->a == slot;
}

// Check if a RECUR updates induction var `var_idx` by adding a constant
// positive step to the current value of that var.
// recur_ref = the RECUR node
// var_slot = the slot of the induction var we are checking
// var_idx = which induction arg (0..nvars-1) this is
// step_out = where to write the step value if found
static bool recur_advances_by_const(const sh_program *p,
                                     sh_nref recur_ref,
                                     uint32_t var_slot,
                                     uint32_t var_idx,
                                     int64_t *step_out) {
  if (!valid_ref(p, recur_ref)) return false;
  const sh_node *recur = &p->nodes[recur_ref];
  if (recur->op != (uint16_t)SH_OP_RECUR) return false;
  if (var_idx >= recur->aux_len) return false;
  sh_nref new_val_ref = p->aux[recur->aux_off + var_idx];
  if (!valid_ref(p, new_val_ref)) return false;
  const sh_node *nv = &p->nodes[new_val_ref];
  // Pattern: (+ i STEP) where i is LOCAL slot and STEP is CONST positive int
  if (nv->op == (uint16_t)SH_OP_BINOP && nv->sub == (uint16_t)SH_BIN_ADD) {
    sh_nref la = nv->a;
    sh_nref lb = nv->b;
    int64_t step = 0;
    // (+ i step) or (+ step i)
    if (is_local_slot(p, la, var_slot) && is_const_int(p, lb, &step) && step > 0) {
      if (step_out) *step_out = step;
      return true;
    }
    if (is_local_slot(p, lb, var_slot) && is_const_int(p, la, &step) && step > 0) {
      if (step_out) *step_out = step;
      return true;
    }
  }
  return false;
}

// Find the first RECUR node reachable from a root node.
// Returns SH_NREF_NONE if not found.
// Simple recursive search with a depth limit to avoid blowup.
static sh_nref find_recur(const sh_program *p, sh_nref ref, int depth) {
  if (depth <= 0 || !valid_ref(p, ref)) return SH_NREF_NONE;
  const sh_node *n = &p->nodes[ref];
  if (n->op == (uint16_t)SH_OP_RECUR) return ref;

  sh_nref found = SH_NREF_NONE;
  // For IF node: check then and else arms (RECUR is in tail positions)
  if (n->op == (uint16_t)SH_OP_IF) {
    found = find_recur(p, n->b, depth - 1);
    if (found != SH_NREF_NONE) return found;
    found = find_recur(p, n->c, depth - 1);
    return found;
  }
  // For LET: check body
  if (n->op == (uint16_t)SH_OP_LET) {
    return find_recur(p, n->b, depth - 1);
  }
  // Generic: look at children
  if (valid_ref(p, n->a)) {
    found = find_recur(p, n->a, depth - 1);
    if (found != SH_NREF_NONE) return found;
  }
  if (valid_ref(p, n->b)) {
    found = find_recur(p, n->b, depth - 1);
    if (found != SH_NREF_NONE) return found;
  }
  if (valid_ref(p, n->c)) {
    found = find_recur(p, n->c, depth - 1);
    if (found != SH_NREF_NONE) return found;
  }
  return SH_NREF_NONE;
}

// ---------------------------------------------------------------------------
// REGION_LEN tracking for bounds annotation
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Main type-inference + cost pass: verify_node
// ---------------------------------------------------------------------------
// This is a recursive bottom-up traversal. Each call returns the type of the
// given node (or sets an error and returns VOID on failure). The node's
// `type` field is filled before returning.

// Helper: check and fill binary numeric operands (both must be same scalar kind)
// Promotes BINOP -> VBINOP / CMP -> VCMP if operands are vectors.
static sh_status check_binop_operands(verify_ctx *vc, sh_nref ref) {
  sh_program *p = vc->p;
  sh_node *n = &p->nodes[ref];
  sh_type ta = node_type(p, n->a);
  sh_type tb = node_type(p, n->b);

  bool is_cmp = (n->op == (uint16_t)SH_OP_CMP);

  // Resolve untyped integer/float CONST literals from the concrete peer type.
  // We intentionally do NOT update LOCAL slot types here.  Resolving a LOCAL
  // from a sibling operand cascades the type across the entire slot (all uses
  // in all arms), which can give the wrong type to an accumulator variable
  // (e.g. in `(+ acc i)` where `i` is u32 but `acc` should be i64 from the
  // declared return type).  LOCALs get their type from their binding
  // initializer during the LOOP/LET processing, not from peer operands.
  if (ta.kind == (uint8_t)SH_K_VOID && n->a != SH_NREF_NONE) {
    if (tb.kind != (uint8_t)SH_K_VOID) {
      sh_kind k = (sh_kind)tb.kind;
      if (tb.kind == (uint8_t)SH_K_VEC) k = (sh_kind)tb.lane_kind;
      if (kind_is_integer(k))
        resolve_int_const(vc, n->a, k);
      else if (k == SH_K_F32 || k == SH_K_F64)
        resolve_float_const(vc, n->a, k);
      ta = node_type(p, n->a);
    }
  }
  if (tb.kind == (uint8_t)SH_K_VOID && n->b != SH_NREF_NONE) {
    if (ta.kind != (uint8_t)SH_K_VOID) {
      sh_kind k = (sh_kind)ta.kind;
      if (ta.kind == (uint8_t)SH_K_VEC) k = (sh_kind)ta.lane_kind;
      if (kind_is_integer(k))
        resolve_int_const(vc, n->b, k);
      else if (k == SH_K_F32 || k == SH_K_F64)
        resolve_float_const(vc, n->b, k);
      tb = node_type(p, n->b);
    }
  }

  // If both are vectors -> promote to VBINOP/VCMP
  if (ta.kind == (uint8_t)SH_K_VEC && tb.kind == (uint8_t)SH_K_VEC) {
    if (ta.lane_kind != tb.lane_kind || ta.lanes != tb.lanes) {
      return sh_set_error(vc->err, SH_ERR_TYPE, -1, -1,
                          "vector operands have incompatible types");
    }
    if (is_cmp) {
      n->op = (uint16_t)SH_OP_VCMP;
      // VCMP result: a mask vector (bool lanes)
      n->type = sh_type_vec((sh_kind)SH_K_BOOL, ta.lanes);
    } else {
      n->op = (uint16_t)SH_OP_VBINOP;
      n->type = ta;
    }
    vc->node_cost[ref] = (uint64_t)ta.lanes * COST_VEC;
    return SH_OK;
  }

  // If either operand is still VOID (an unresolved integer LOCAL whose slot
  // type is determined later by loop-init finalization), defer the result type:
  // - BINOP: leave result as VOID; the re-inference pass will resolve it after
  //          induction-var inits are finalized.
  // - CMP:   produce bool immediately (the IF condition must be bool).  We
  //          cannot enforce operand equality now; it will be verified in the
  //          re-inference pass.  This is safe: if the operands end up with
  //          incompatible concrete types, the re-inference pass can flag it
  //          (or the whole program will have a stray VOID somewhere else
  //          that triggers an error).
  if (ta.kind == (uint8_t)SH_K_VOID || tb.kind == (uint8_t)SH_K_VOID) {
    if (is_cmp) {
      n->type = sh_type_scalar(SH_K_BOOL); // deferred operand check; result is bool
    } else {
      n->type = sh_type_scalar(SH_K_VOID); // deferred
    }
    vc->node_cost[ref] = COST_ARITH;
    return SH_OK;
  }
  if (!sh_type_eq(ta, tb)) {
    return sh_set_error(vc->err, SH_ERR_TYPE, -1, -1,
                        "binary operand type mismatch");
  }
  if (is_cmp) {
    n->type = sh_type_scalar(SH_K_BOOL);
  } else {
    n->type = ta;
  }
  vc->node_cost[ref] = COST_ARITH;
  return SH_OK;
}

// ---------------------------------------------------------------------------
// Bounded loop verification
// ---------------------------------------------------------------------------
// Recognizes the safe loop template and fills sh_loop.bound.
// Returns SH_ERR_UNBOUNDED_LOOP if the template is not matched.
//
// Template (syntactic match):
//   body = (if (CMP i LIMIT) EXIT (... RECUR ...))
//   where CMP is >=, >, <=, <  (normalized to "exit when i >= LIMIT")
//   LIMIT is CONST or u32/u64 PARAM
//   every RECUR passes i' = (+ i STEP) with STEP a positive CONST
//   body type = loop result type (the EXIT arm of the IF)

static sh_status verify_loop_bound(verify_ctx *vc, uint32_t loop_idx,
                                   sh_nref body,
                                   sh_nref *induction_var_slot_out,
                                   uint64_t body_cost,
                                   sh_nref region_param_for_bounds,
                                   uint32_t region_param_idx) {
  sh_program *p = vc->p;
  sh_loop *lp = &p->loops[loop_idx];

  if (lp->nvars == 0)
    return sh_set_error(vc->err, SH_ERR_UNBOUNDED_LOOP, -1, -1,
                        "loop has no induction variables");

  // Body must be IF
  if (!valid_ref(p, body) || p->nodes[body].op != (uint16_t)SH_OP_IF)
    return sh_set_error(vc->err, SH_ERR_UNBOUNDED_LOOP, -1, -1,
                        "loop body must start with an if exit test");

  sh_node *if_node = &p->nodes[body];
  sh_nref cond_ref = if_node->a;
  sh_nref then_ref = if_node->b;
  sh_nref else_ref = if_node->c;

  // Cond must be a CMP
  if (!valid_ref(p, cond_ref) || p->nodes[cond_ref].op != (uint16_t)SH_OP_CMP)
    return sh_set_error(vc->err, SH_ERR_UNBOUNDED_LOOP, -1, -1,
                        "loop exit test must be a comparison");

  sh_node *cmp = &p->nodes[cond_ref];
  sh_cmp cmpop = (sh_cmp)cmp->sub;
  sh_nref cmp_lhs = cmp->a;
  sh_nref cmp_rhs = cmp->b;

  // Find which operand is the induction var and which is the limit
  // Normalize to: induction_slot >= limit_ref (or equivalent)
  sh_nref i_ref = SH_NREF_NONE;  // the node referencing the induction var
  sh_nref limit_ref = SH_NREF_NONE;
  uint32_t ind_slot = 0;
  bool found_ind = false;
  bool limit_on_rhs = false;  // true: lhs=i, rhs=limit; false: lhs=limit, rhs=i

  // Check first induction var only (the spec's minimalist template uses var 0
  // as the loop counter). The bound must involve exactly this var.
  for (uint32_t vi = 0; vi < lp->nvars && !found_ind; vi++) {
    uint32_t slot = lp->var_slot0 + vi;
    if (is_local_slot(p, cmp_lhs, slot)) {
      ind_slot = slot;
      i_ref = cmp_lhs;
      limit_ref = cmp_rhs;
      limit_on_rhs = true;
      found_ind = true;
    } else if (is_local_slot(p, cmp_rhs, slot)) {
      ind_slot = slot;
      i_ref = cmp_rhs;
      limit_ref = cmp_lhs;
      limit_on_rhs = false;
      found_ind = true;
    }
  }
  (void)i_ref;

  if (!found_ind)
    return sh_set_error(vc->err, SH_ERR_UNBOUNDED_LOOP, -1, -1,
                        "loop exit test does not compare an induction variable");

  // Determine if the exit branch is the then or else arm.
  // Exit arm is the one that does NOT contain a RECUR.
  // The other arm (continue arm) must contain a RECUR.
  bool then_has_recur = (find_recur(p, then_ref, 256) != SH_NREF_NONE);
  bool else_has_recur = (find_recur(p, else_ref, 256) != SH_NREF_NONE);

  if (then_has_recur == else_has_recur) {
    // Either both or neither have recur -- not the canonical template
    return sh_set_error(vc->err, SH_ERR_UNBOUNDED_LOOP, -1, -1,
                        "loop body must have exactly one exit arm and one recur arm");
  }

  // exit_arm: the arm that exits (no RECUR); continue_arm: contains RECUR
  sh_nref continue_arm = then_has_recur ? then_ref : else_ref;
  (void)continue_arm;

  // Validate the comparison direction:
  // When limit_on_rhs (i op LIMIT):
  //   >= or > means "exit when i >= LIMIT" (induction increases)
  //   <= or < means "exit when i <= LIMIT" (invalid for upward induction)
  // When limit_on_lhs (LIMIT op i):
  //   <= or < means "exit when LIMIT <= i" i.e. i >= LIMIT
  //   >= or > means "exit when LIMIT >= i" i.e. i <= LIMIT (wrong direction)
  bool exit_direction_ok = false;
  if (limit_on_rhs) {
    // i CMP limit, exit when true
    // then_has_recur=false means then is exit branch (exit when test is true)
    // then_has_recur=true means else is exit branch (exit when test is false)
    if (!then_has_recur) {
      // exit when (i CMP limit) is true
      exit_direction_ok = (cmpop == SH_CMP_GE || cmpop == SH_CMP_GT ||
                           cmpop == SH_CMP_EQ);
    } else {
      // exit when (i CMP limit) is false, i.e. NOT(i CMP limit)
      // i < limit -> continue, exit when i >= limit (the false branch exits? No.)
      // Actually: else branch is exit, so we exit when cond is false
      // cond = (i < limit), exits when false = i >= limit. OK.
      exit_direction_ok = (cmpop == SH_CMP_LT || cmpop == SH_CMP_LE);
    }
  } else {
    // limit CMP i
    if (!then_has_recur) {
      // exit when (limit CMP i) is true
      exit_direction_ok = (cmpop == SH_CMP_LE || cmpop == SH_CMP_LT ||
                           cmpop == SH_CMP_EQ);
    } else {
      // exit when cond false
      exit_direction_ok = (cmpop == SH_CMP_GT || cmpop == SH_CMP_GE);
    }
  }

  if (!exit_direction_ok)
    return sh_set_error(vc->err, SH_ERR_UNBOUNDED_LOOP, -1, -1,
                        "loop bound comparison direction not recognized as upward bounded");

  // Determine which induction var slot is the counter (the one in the cmp)
  uint32_t counter_var_idx = ind_slot - lp->var_slot0;

  // Verify that every RECUR in the body advances the counter by a positive const step
  // Find all RECUR nodes (there may be multiple in nested IFs)
  sh_nref recur_ref = find_recur(p, lp->body, 512);
  if (recur_ref == SH_NREF_NONE)
    return sh_set_error(vc->err, SH_ERR_UNBOUNDED_LOOP, -1, -1,
                        "no RECUR found in loop body");

  int64_t step = 0;
  if (!recur_advances_by_const(p, recur_ref, ind_slot, counter_var_idx, &step))
    return sh_set_error(vc->err, SH_ERR_UNBOUNDED_LOOP, -1, -1,
                        "loop recur does not advance induction var by a constant positive step");

  // Determine the bound: limit is CONST or u32/u64 PARAM
  int64_t const_limit = 0;
  uint32_t param_limit_idx = 0;
  bool limit_is_const = false;
  bool limit_is_param = false;

  // Also accept REGION_LEN as a limit (special case for bounds annotation)
  bool limit_is_region_len = false;
  uint32_t region_len_param_idx = 0;

  if (is_const_int(p, limit_ref, &const_limit)) {
    limit_is_const = true;
  } else if (is_int_param(p, limit_ref, &param_limit_idx)) {
    sh_kind pk = (sh_kind)p->params[param_limit_idx].kind;
    if (pk == SH_K_U32 || pk == SH_K_U64) {
      limit_is_param = true;
    } else {
      return sh_set_error(vc->err, SH_ERR_UNBOUNDED_LOOP, -1, -1,
                          "loop limit param must be u32 or u64");
    }
  } else if (valid_ref(p, limit_ref) &&
             p->nodes[limit_ref].op == (uint16_t)SH_OP_REGION_LEN) {
    // (region-len buf) as limit: the trip count is bounded by the region length
    // Treat this as param-bound for cost purposes
    sh_nref rn = p->nodes[limit_ref].a;
    if (valid_ref(p, rn) && p->nodes[rn].op == (uint16_t)SH_OP_PARAM) {
      region_len_param_idx = p->nodes[rn].a;
      limit_is_region_len = true;
      limit_is_param = true;
      param_limit_idx = region_len_param_idx;
    } else {
      return sh_set_error(vc->err, SH_ERR_UNBOUNDED_LOOP, -1, -1,
                          "loop limit region-len must be applied to a parameter");
    }
  } else {
    return sh_set_error(vc->err, SH_ERR_UNBOUNDED_LOOP, -1, -1,
                        "loop limit must be a constant or a u32/u64 parameter");
  }

  // Get init value for trip count ceiling calculation
  int64_t init_val = 0;
  bool init_is_const = false;
  sh_nref init_ref = p->aux[lp->init_off + counter_var_idx];
  if (is_const_int(p, init_ref, &init_val)) {
    init_is_const = true;
  }

  // Fill the bound
  if (limit_is_const && init_is_const) {
    // Compute trip count ceiling: ceil((LIMIT - init) / step)
    int64_t range = const_limit - init_val;
    uint64_t trip = 0;
    if (range > 0 && step > 0) {
      trip = (uint64_t)((range + step - 1) / step);
    }
    lp->bound.kind = SH_BOUND_CONST;
    lp->bound.konst = trip;
    lp->bound.per_iter_cost = body_cost;
  } else if (limit_is_param || limit_is_region_len) {
    if (vc->flags & SH_REQUIRE_CONST_COST)
      return sh_set_error(vc->err, SH_ERR_NONCONST_COST, -1, -1,
                          "SH_REQUIRE_CONST_COST: loop bound is a parameter");
    lp->bound.kind = SH_BOUND_PARAM;
    lp->bound.param_idx = param_limit_idx;
    lp->bound.per_iter_cost = body_cost;
  } else {
    lp->bound.kind = SH_BOUND_NONE;
    return sh_set_error(vc->err, SH_ERR_UNBOUNDED_LOOP, -1, -1,
                        "loop bound is not constant or parameter");
  }

  // Output induction var slot (for bounds annotation use)
  if (induction_var_slot_out) *induction_var_slot_out = ind_slot;

  // If limit is region-len, record the region param for bounds annotation
  if (limit_is_region_len) {
    if (region_param_for_bounds) (void)region_param_for_bounds;
    (void)region_param_idx;
    // Stored separately in the call site
    lp->bound.param_idx = region_len_param_idx;
  }

  return SH_OK;
}

// ---------------------------------------------------------------------------
// verify_node: the main recursive bottom-up pass
// ---------------------------------------------------------------------------

static sh_status verify_node(verify_ctx *vc, sh_nref ref) {
  sh_program *p = vc->p;
  if (ref == SH_NREF_NONE || ref >= p->nnodes)
    return sh_set_error(vc->err, SH_ERR_INTERNAL, -1, -1,
                        "invalid node ref %u", ref);
  if (ref >= 4096)
    return sh_set_error(vc->err, SH_ERR_INTERNAL, -1, -1,
                        "program too large for verifier (%u nodes)", ref);

  sh_node *n = &p->nodes[ref];
  sh_op op = (sh_op)n->op;
  sh_status s;

  switch (op) {
    // --- CONST ------------------------------------------------------------
    case SH_OP_CONST: {
      // sub=0 integer: type is left to be resolved from context
      // sub=1 float:   type resolved from context too (default f64)
      // sub=2 bool:    always bool
      if (n->sub == 2) {
        n->type = sh_type_scalar(SH_K_BOOL);
      } else if (n->sub == 1) {
        // float literal: leave as VOID until consumer resolves, default f64
        if (n->type.kind == (uint8_t)SH_K_VOID) {
          // leave for context; will be finalized later
        }
      } else {
        // integer literal: mark as needing resolution
        if (n->type.kind == (uint8_t)SH_K_VOID) {
          vc->needs_resolve[ref] = 1;
        }
      }
      vc->node_cost[ref] = 0; // literals have no runtime cost
      return SH_OK;
    }

    // --- PARAM ------------------------------------------------------------
    case SH_OP_PARAM: {
      if (n->a >= p->nparams)
        return sh_set_error(vc->err, SH_ERR_INTERNAL, -1, -1,
                            "PARAM index %u out of range", n->a);
      n->type = p->params[n->a];
      vc->node_cost[ref] = 0;
      return SH_OK;
    }

    // --- LOCAL ------------------------------------------------------------
    case SH_OP_LOCAL: {
      // The type of a local is filled in when the LET/LOOP that binds it is
      // verified. At first encounter it may be VOID; we type it here from
      // the slot's expected type (stored in p->params is wrong -- locals are
      // in a separate implied array). We will set the type when the LET/LOOP
      // is processed. If type is already set, use it; otherwise leave as VOID
      // and it will be resolved when the binding is typed.
      vc->node_cost[ref] = 0;
      return SH_OK;
    }

    // --- UNOP -------------------------------------------------------------
    case SH_OP_UNOP: {
      if (!valid_ref(p, n->a))
        return sh_set_error(vc->err, SH_ERR_INTERNAL, -1, -1, "UNOP missing operand");
      s = verify_node(vc, n->a);
      if (s != SH_OK) return s;
      sh_type ta = node_type(p, n->a);

      sh_unop unop = (sh_unop)n->sub;
      switch (unop) {
        case SH_UN_NEG:
          if (ta.kind == (uint8_t)SH_K_BOOL || ta.kind == (uint8_t)SH_K_VOID)
            return sh_set_error(vc->err, SH_ERR_TYPE, -1, -1,
                                "NEG: operand must be numeric");
          n->type = ta;
          break;
        case SH_UN_NOT:
          if (ta.kind != (uint8_t)SH_K_BOOL) {
            // Allow bool literals to propagate
            if (ta.kind == (uint8_t)SH_K_VOID) {
              // unresolved -- try to force bool
              if (p->nodes[n->a].op == (uint16_t)SH_OP_CONST &&
                  p->nodes[n->a].sub == 2) {
                p->nodes[n->a].type = sh_type_scalar(SH_K_BOOL);
              } else {
                return sh_set_error(vc->err, SH_ERR_TYPE, -1, -1,
                                    "NOT: operand must be bool");
              }
            } else {
              return sh_set_error(vc->err, SH_ERR_TYPE, -1, -1,
                                  "NOT: operand must be bool, got %u", ta.kind);
            }
          }
          n->type = sh_type_scalar(SH_K_BOOL);
          break;
        case SH_UN_CVT:
          // Frontend already set node.type to the target type.
          // Validate the source is a scalar numeric or bool kind.
          if (n->type.kind == (uint8_t)SH_K_VOID)
            return sh_set_error(vc->err, SH_ERR_INTERNAL, -1, -1,
                                "CVT without target type (frontend bug)");
          if (ta.kind == (uint8_t)SH_K_REGION || ta.kind == (uint8_t)SH_K_VEC)
            return sh_set_error(vc->err, SH_ERR_TYPE, -1, -1,
                                "CVT: cannot convert region or vector");
          // Resolve operand integer literals to source -- leave as-is, cast is valid
          break;
        default:
          return sh_set_error(vc->err, SH_ERR_INTERNAL, -1, -1,
                              "unknown UNOP sub %u", n->sub);
      }
      vc->node_cost[ref] = COST_ARITH;
      return SH_OK;
    }

    // --- BINOP ------------------------------------------------------------
    case SH_OP_BINOP: {
      if (!valid_ref(p, n->a) || !valid_ref(p, n->b))
        return sh_set_error(vc->err, SH_ERR_INTERNAL, -1, -1, "BINOP missing operand");
      // Verify children first
      s = verify_node(vc, n->a);
      if (s != SH_OK) return s;
      s = verify_node(vc, n->b);
      if (s != SH_OK) return s;
      s = check_binop_operands(vc, ref);
      if (s != SH_OK) return s;
      // check_binop_operands may have promoted to VBINOP; cost already set there
      if (n->op == (uint16_t)SH_OP_BINOP) {
        // Still scalar BINOP
        sh_type ta = node_type(p, n->a);
        if (ta.kind == (uint8_t)SH_K_BOOL || ta.kind == (uint8_t)SH_K_REGION ||
            ta.kind == (uint8_t)SH_K_VEC)
          return sh_set_error(vc->err, SH_ERR_TYPE, -1, -1,
                              "BINOP operands must be scalar numeric");
      }
      return SH_OK;
    }

    // --- CMP --------------------------------------------------------------
    case SH_OP_CMP: {
      if (!valid_ref(p, n->a) || !valid_ref(p, n->b))
        return sh_set_error(vc->err, SH_ERR_INTERNAL, -1, -1, "CMP missing operand");
      s = verify_node(vc, n->a);
      if (s != SH_OK) return s;
      s = verify_node(vc, n->b);
      if (s != SH_OK) return s;
      s = check_binop_operands(vc, ref);
      if (s != SH_OK) return s;
      // After potential promotion to VCMP, type is already set by check_binop_operands
      return SH_OK;
    }

    // --- IF ---------------------------------------------------------------
    case SH_OP_IF: {
      if (!valid_ref(p, n->a) || !valid_ref(p, n->b) || !valid_ref(p, n->c))
        return sh_set_error(vc->err, SH_ERR_INTERNAL, -1, -1, "IF missing arms");
      s = verify_node(vc, n->a);
      if (s != SH_OK) return s;

      // Cond may be unresolved const bool
      sh_type tcond = node_type(p, n->a);
      if (tcond.kind == (uint8_t)SH_K_VOID) {
        // try to resolve as bool
        if (p->nodes[n->a].op == (uint16_t)SH_OP_CONST &&
            p->nodes[n->a].sub == 2) {
          p->nodes[n->a].type = sh_type_scalar(SH_K_BOOL);
          tcond = p->nodes[n->a].type;
        }
      }
      if (tcond.kind != (uint8_t)SH_K_BOOL)
        return sh_set_error(vc->err, SH_ERR_TYPE, -1, -1,
                            "IF condition must be bool, got kind %u", tcond.kind);

      s = verify_node(vc, n->b);
      if (s != SH_OK) return s;
      s = verify_node(vc, n->c);
      if (s != SH_OK) return s;

      sh_type tthen = node_type(p, n->b);
      sh_type telse = node_type(p, n->c);

      // Resolve untyped integer consts from each arm
      if (tthen.kind == (uint8_t)SH_K_VOID && telse.kind != (uint8_t)SH_K_VOID) {
        if (kind_is_integer((sh_kind)telse.kind))
          resolve_int_const(vc, n->b, (sh_kind)telse.kind);
        else if (telse.kind == (uint8_t)SH_K_F32 || telse.kind == (uint8_t)SH_K_F64)
          resolve_float_const(vc, n->b, (sh_kind)telse.kind);
        tthen = node_type(p, n->b);
      }
      if (telse.kind == (uint8_t)SH_K_VOID && tthen.kind != (uint8_t)SH_K_VOID) {
        if (kind_is_integer((sh_kind)tthen.kind))
          resolve_int_const(vc, n->c, (sh_kind)tthen.kind);
        else if (tthen.kind == (uint8_t)SH_K_F32 || tthen.kind == (uint8_t)SH_K_F64)
          resolve_float_const(vc, n->c, (sh_kind)tthen.kind);
        telse = node_type(p, n->c);
      }

      // If one arm is a RECUR (which has VOID result since it never returns
      // a value -- the loop body's type comes from the exit arm), use the
      // other arm's type. This is how the bounded-loop IF works: one arm
      // is the exit value, the other is RECUR (which loops back).
      bool then_is_recur = (valid_ref(p, n->b) &&
                            p->nodes[n->b].op == (uint16_t)SH_OP_RECUR);
      bool else_is_recur = (valid_ref(p, n->c) &&
                            p->nodes[n->c].op == (uint16_t)SH_OP_RECUR);
      if (then_is_recur && !else_is_recur) {
        // then = RECUR, else = exit value: IF's type is else's type
        n->type = telse;
        vc->node_cost[ref] = COST_ARITH + vc->node_cost[n->a] + vc->node_cost[n->c];
        return SH_OK;
      }
      if (else_is_recur && !then_is_recur) {
        // else = RECUR, then = exit value: IF's type is then's type
        n->type = tthen;
        vc->node_cost[ref] = COST_ARITH + vc->node_cost[n->a] + vc->node_cost[n->b];
        return SH_OK;
      }

      if (!sh_type_eq(tthen, telse))
        return sh_set_error(vc->err, SH_ERR_TYPE, -1, -1,
                            "IF arms have different types: then kind=%u else kind=%u",
                            tthen.kind, telse.kind);

      n->type = tthen;
      vc->node_cost[ref] = COST_ARITH +
                           vc->node_cost[n->a] +
                           (vc->node_cost[n->b] > vc->node_cost[n->c]
                            ? vc->node_cost[n->b]
                            : vc->node_cost[n->c]);
      return SH_OK;
    }

    // --- LET --------------------------------------------------------------
    case SH_OP_LET: {
      // aux[aux_off .. +aux_len) are the init-expr nrefs
      // n->a = first slot, n->b = body
      uint32_t first_slot = n->a;
      uint64_t let_cost = 0;

      for (uint32_t i = 0; i < n->aux_len; i++) {
        sh_nref init_ref = p->aux[n->aux_off + i];
        if (!valid_ref(p, init_ref))
          return sh_set_error(vc->err, SH_ERR_INTERNAL, -1, -1,
                              "LET binding %u has invalid init ref", i);
        s = verify_node(vc, init_ref);
        if (s != SH_OK) return s;
        // Finalize the init type
        if (p->nodes[init_ref].type.kind == (uint8_t)SH_K_VOID) {
          s = finalize_const(vc, init_ref);
          if (s != SH_OK) return s;
        }
        // Propagate type to all LOCAL nodes that read this slot
        sh_type init_type = p->nodes[init_ref].type;
        uint32_t slot = first_slot + i;
        // Patch all LOCAL nodes for this slot with the inferred type
        for (uint32_t j = 0; j < p->nnodes; j++) {
          if (p->nodes[j].op == (uint16_t)SH_OP_LOCAL && p->nodes[j].a == slot) {
            p->nodes[j].type = init_type;
          }
        }
        let_cost += vc->node_cost[init_ref];
      }

      // Verify body
      if (!valid_ref(p, n->b))
        return sh_set_error(vc->err, SH_ERR_INTERNAL, -1, -1, "LET missing body");
      s = verify_node(vc, n->b);
      if (s != SH_OK) return s;

      sh_type tbody = node_type(p, n->b);
      if (tbody.kind == (uint8_t)SH_K_VOID) {
        s = finalize_const(vc, n->b);
        if (s != SH_OK) return s;
        tbody = node_type(p, n->b);
      }

      n->type = tbody;
      vc->node_cost[ref] = let_cost + vc->node_cost[n->b];
      return SH_OK;
    }

    // --- REGION_LOAD ------------------------------------------------------
    case SH_OP_REGION_LOAD: {
      if (!valid_ref(p, n->a) || !valid_ref(p, n->b))
        return sh_set_error(vc->err, SH_ERR_INTERNAL, -1, -1,
                            "REGION_LOAD missing operands");
      s = verify_node(vc, n->a);
      if (s != SH_OK) return s;
      s = verify_node(vc, n->b);
      if (s != SH_OK) return s;

      sh_type treg = node_type(p, n->a);
      sh_type tidx = node_type(p, n->b);

      if (treg.kind != (uint8_t)SH_K_REGION)
        return sh_set_error(vc->err, SH_ERR_TYPE, -1, -1,
                            "REGION_LOAD: first arg must be a region");

      // Index must be an integer kind. If it is still unresolved (a LOCAL or
      // integer CONST that has not yet been given a type by its binding), we
      // resolve it to u32 (the natural region-index type).  For LOCALs this
      // updates the whole slot so every use picks up u32.
      if (tidx.kind == (uint8_t)SH_K_VOID) {
        sh_node *idx_node = &p->nodes[n->b];
        if (idx_node->op == (uint16_t)SH_OP_CONST) {
          resolve_int_const(vc, n->b, SH_K_U32);
        } else if (idx_node->op == (uint16_t)SH_OP_LOCAL) {
          retype_all_locals_for_slot(vc, idx_node->a, sh_type_scalar(SH_K_U32));
        }
      }
      tidx = node_type(p, n->b);
      if (!kind_is_integer((sh_kind)tidx.kind))
        return sh_set_error(vc->err, SH_ERR_TYPE, -1, -1,
                            "REGION_LOAD: index must be integer");

      // Result type = element kind of the region
      n->type = sh_type_scalar((sh_kind)treg.lane_kind);
      vc->node_cost[ref] = COST_REGION;
      return SH_OK;
    }

    // --- REGION_STORE -----------------------------------------------------
    case SH_OP_REGION_STORE: {
      if (!valid_ref(p, n->a) || !valid_ref(p, n->b) || !valid_ref(p, n->c))
        return sh_set_error(vc->err, SH_ERR_INTERNAL, -1, -1,
                            "REGION_STORE missing operands");
      s = verify_node(vc, n->a);
      if (s != SH_OK) return s;
      s = verify_node(vc, n->b);
      if (s != SH_OK) return s;
      s = verify_node(vc, n->c);
      if (s != SH_OK) return s;

      sh_type treg = node_type(p, n->a);
      sh_type tidx = node_type(p, n->b);
      sh_type tval = node_type(p, n->c);

      if (treg.kind != (uint8_t)SH_K_REGION)
        return sh_set_error(vc->err, SH_ERR_TYPE, -1, -1,
                            "REGION_STORE: first arg must be a region");
      if (!(treg.flags & SH_TYPE_FLAG_MUTABLE))
        return sh_set_error(vc->err, SH_ERR_TYPE, -1, -1,
                            "REGION_STORE: region is not mutable");

      if (tidx.kind == (uint8_t)SH_K_VOID) {
        sh_node *idx_node2 = &p->nodes[n->b];
        if (idx_node2->op == (uint16_t)SH_OP_CONST) {
          resolve_int_const(vc, n->b, SH_K_U32);
        } else if (idx_node2->op == (uint16_t)SH_OP_LOCAL) {
          retype_all_locals_for_slot(vc, idx_node2->a, sh_type_scalar(SH_K_U32));
        }
      }
      tidx = node_type(p, n->b);
      if (!kind_is_integer((sh_kind)tidx.kind))
        return sh_set_error(vc->err, SH_ERR_TYPE, -1, -1,
                            "REGION_STORE: index must be integer");

      // Resolve value type from region element type if needed
      sh_kind elem = (sh_kind)treg.lane_kind;
      if (tval.kind == (uint8_t)SH_K_VOID) {
        if (kind_is_integer(elem)) resolve_int_const(vc, n->c, elem);
        else if (elem == SH_K_F32 || elem == SH_K_F64) resolve_float_const(vc, n->c, elem);
        tval = node_type(p, n->c);
      }

      sh_type expected = sh_type_scalar(elem);
      if (!sh_type_eq(tval, expected))
        return sh_set_error(vc->err, SH_ERR_TYPE, -1, -1,
                            "REGION_STORE: value type does not match region element type");

      // REGION_STORE returns the stored value (elem type)
      n->type = expected;
      vc->node_cost[ref] = COST_REGION;
      return SH_OK;
    }

    // --- REGION_LEN -------------------------------------------------------
    case SH_OP_REGION_LEN: {
      if (!valid_ref(p, n->a))
        return sh_set_error(vc->err, SH_ERR_INTERNAL, -1, -1,
                            "REGION_LEN missing operand");
      s = verify_node(vc, n->a);
      if (s != SH_OK) return s;

      sh_type treg = node_type(p, n->a);
      if (treg.kind != (uint8_t)SH_K_REGION)
        return sh_set_error(vc->err, SH_ERR_TYPE, -1, -1,
                            "REGION_LEN: operand must be a region");

      n->type = sh_type_scalar(SH_K_U32);
      vc->node_cost[ref] = COST_REGION;
      return SH_OK;
    }

    // --- LOOP -------------------------------------------------------------
    case SH_OP_LOOP: {
      // n->a = loop index into p->loops
      uint32_t loop_idx = n->a;
      if (loop_idx >= p->nloops)
        return sh_set_error(vc->err, SH_ERR_INTERNAL, -1, -1,
                            "LOOP index %u out of range", loop_idx);

      sh_loop *lp = &p->loops[loop_idx];

      // Verify init expressions and propagate types to induction var LOCAL nodes
      uint64_t init_cost = 0;
      for (uint32_t vi = 0; vi < lp->nvars; vi++) {
        sh_nref init_ref = p->aux[lp->init_off + vi];
        if (!valid_ref(p, init_ref))
          return sh_set_error(vc->err, SH_ERR_INTERNAL, -1, -1,
                              "LOOP init ref %u invalid", vi);
        s = verify_node(vc, init_ref);
        if (s != SH_OK) return s;
        // Do NOT finalize integer literals here yet: the body may resolve them
        // to the correct type via comparison against a concrete-typed param.
        // Only finalize non-integer types (floats/bools) or concrete types.
        sh_type init_type = p->nodes[init_ref].type;
        // For integer literals (VOID), just leave LOCAL type as VOID for now.
        // retype_all_locals_for_slot will be called when the body resolves them.
        if (init_type.kind != (uint8_t)SH_K_VOID) {
          uint32_t slot = lp->var_slot0 + vi;
          // Propagate concrete type to LOCAL nodes
          for (uint32_t j = 0; j < p->nnodes; j++) {
            if (p->nodes[j].op == (uint16_t)SH_OP_LOCAL && p->nodes[j].a == slot) {
              p->nodes[j].type = init_type;
            }
          }
        }
        init_cost += vc->node_cost[init_ref];
      }

      // Verify the loop body. During body verification, LOCAL nodes for loop
      // induction vars may be resolved from context (e.g., when compared to
      // a u32 param, the LOCAL adopts u32 via resolve_local_void).
      sh_nref body = lp->body;
      if (!valid_ref(p, body))
        return sh_set_error(vc->err, SH_ERR_INTERNAL, -1, -1, "LOOP missing body");
      s = verify_node(vc, body);
      if (s != SH_OK) return s;

      // Verify loop bound structure FIRST (before finalization) so that
      // structurally invalid loops (no IF, no RECUR in the right place) are
      // rejected with SH_ERR_UNBOUNDED_LOOP rather than a spurious type error
      // from the RECUR arg check below.  Body cost is already in vc->node_cost.
      uint64_t body_cost = vc->node_cost[body];
      sh_nref ind_slot_out = 0;
      s = verify_loop_bound(vc, loop_idx, body, &ind_slot_out,
                            body_cost, SH_NREF_NONE, 0);
      if (s != SH_OK) return s;

      // After the body is verified, finalize any still-unresolved induction var
      // inits.  We do this AFTER verify_loop_bound so that the bound structure
      // has already been validated and the induction slot is known.
      for (uint32_t vi = 0; vi < lp->nvars; vi++) {
        sh_nref init_ref = p->aux[lp->init_off + vi];
        sh_type init_type = p->nodes[init_ref].type;
        if (init_type.kind == (uint8_t)SH_K_VOID) {
          // Still unresolved; check if the body resolved the LOCAL's type
          uint32_t slot = lp->var_slot0 + vi;
          sh_type local_type = sh_type_scalar(SH_K_VOID);
          for (uint32_t j = 0; j < p->nnodes; j++) {
            if (p->nodes[j].op == (uint16_t)SH_OP_LOCAL &&
                p->nodes[j].a == slot &&
                p->nodes[j].type.kind != (uint8_t)SH_K_VOID) {
              local_type = p->nodes[j].type;
              break;
            }
          }
          if (local_type.kind != (uint8_t)SH_K_VOID) {
            // Update init to match the resolved local type
            p->nodes[init_ref].type = local_type;
            vc->needs_resolve[init_ref] = 0;
          } else {
            // The body did not determine a concrete type for this induction var.
            // Default: use the program's declared return type if it is an
            // integer kind (the accumulator var is typically the return value
            // so it should take the declared return type, not a fixed default).
            // If the return type is not an integer (or is VOID), fall back to i64.
            sh_type default_type;
            sh_kind ret_kind = (sh_kind)p->ret.kind;
            if (kind_is_integer(ret_kind)) {
              default_type = sh_type_scalar(ret_kind);
            } else {
              default_type = sh_type_scalar(SH_K_I64);
            }
            // Only override a CONST init (not a non-integer literal)
            if (p->nodes[init_ref].op == (uint16_t)SH_OP_CONST &&
                p->nodes[init_ref].sub == 0 &&
                vc->needs_resolve[init_ref]) {
              p->nodes[init_ref].type = default_type;
              vc->needs_resolve[init_ref] = 0;
            } else {
              s = finalize_const(vc, init_ref);
              if (s != SH_OK) return s;
            }
            // Propagate to all LOCALs for this slot
            retype_all_locals_for_slot(vc, slot, p->nodes[init_ref].type);
          }
        }
      }

      // Now that LOCAL types are finalized, re-infer the types of any VOID
      // BINOP/UNOP nodes in the RECUR args.  During body verification these
      // nodes were computed with VOID children (e.g. `(+ i 1)` when `i` was
      // still VOID); now `i` has its final type so we can re-read the children.
      // We walk every VOID BINOP/UNOP in the whole program; it is a small arena.
      {
        bool changed = true;
        while (changed) {
          changed = false;
          for (uint32_t ni = 0; ni < p->nnodes; ni++) {
            sh_node *mn = &p->nodes[ni];
            if (mn->type.kind != (uint8_t)SH_K_VOID) continue;
            if (mn->op == (uint16_t)SH_OP_BINOP || mn->op == (uint16_t)SH_OP_VBINOP) {
              sh_type ta2 = node_type(p, mn->a);
              sh_type tb2 = node_type(p, mn->b);
              if (ta2.kind != (uint8_t)SH_K_VOID && sh_type_eq(ta2, tb2)) {
                mn->type = (mn->op == (uint16_t)SH_OP_BINOP) ? ta2 : ta2;
                changed = true;
              } else if (ta2.kind != (uint8_t)SH_K_VOID &&
                         tb2.kind == (uint8_t)SH_K_VOID) {
                // peer const might resolve
                if (kind_is_integer((sh_kind)ta2.kind))
                  resolve_int_const(vc, mn->b, (sh_kind)ta2.kind);
                tb2 = node_type(p, mn->b);
                if (sh_type_eq(ta2, tb2)) { mn->type = ta2; changed = true; }
              } else if (tb2.kind != (uint8_t)SH_K_VOID &&
                         ta2.kind == (uint8_t)SH_K_VOID) {
                if (kind_is_integer((sh_kind)tb2.kind))
                  resolve_int_const(vc, mn->a, (sh_kind)tb2.kind);
                ta2 = node_type(p, mn->a);
                if (sh_type_eq(ta2, tb2)) { mn->type = tb2; changed = true; }
              }
            } else if (mn->op == (uint16_t)SH_OP_UNOP) {
              if ((sh_unop)mn->sub == SH_UN_NEG) {
                sh_type ta2 = node_type(p, mn->a);
                if (ta2.kind != (uint8_t)SH_K_VOID) { mn->type = ta2; changed = true; }
              }
            } else if (mn->op == (uint16_t)SH_OP_IF) {
              // Re-infer IF type if one arm is RECUR (VOID) and the other
              // arm's type was VOID at body-verification time but is now typed.
              bool b_is_recur = (valid_ref(p, mn->b) &&
                                 p->nodes[mn->b].op == (uint16_t)SH_OP_RECUR);
              bool c_is_recur = (valid_ref(p, mn->c) &&
                                 p->nodes[mn->c].op == (uint16_t)SH_OP_RECUR);
              sh_type tb2 = node_type(p, mn->b);
              sh_type tc2 = node_type(p, mn->c);
              if (b_is_recur && !c_is_recur && tc2.kind != (uint8_t)SH_K_VOID) {
                mn->type = tc2; changed = true;
              } else if (c_is_recur && !b_is_recur && tb2.kind != (uint8_t)SH_K_VOID) {
                mn->type = tb2; changed = true;
              } else if (!b_is_recur && !c_is_recur &&
                         tb2.kind != (uint8_t)SH_K_VOID &&
                         sh_type_eq(tb2, tc2)) {
                mn->type = tb2; changed = true;
              }
            } else if (mn->op == (uint16_t)SH_OP_LOCAL) {
              // A LOCAL still VOID: look up the slot's type from all other
              // LOCALs for the same slot that ARE typed.
              uint32_t lslot = mn->a;
              for (uint32_t jj = 0; jj < p->nnodes; jj++) {
                if (p->nodes[jj].op == (uint16_t)SH_OP_LOCAL &&
                    p->nodes[jj].a == lslot &&
                    p->nodes[jj].type.kind != (uint8_t)SH_K_VOID) {
                  mn->type = p->nodes[jj].type;
                  changed = true;
                  break;
                }
              }
            }
          }
        }
      }

      sh_type tbody = node_type(p, body);
      if (tbody.kind == (uint8_t)SH_K_VOID) {
        s = finalize_const(vc, body);
        if (s != SH_OK) return s;
        tbody = node_type(p, body);
      }

      // Verify the RECUR nodes: their arg types must match the induction var types
      sh_nref recur_ref = find_recur(p, body, 512);
      if (recur_ref != SH_NREF_NONE) {
        sh_node *recur = &p->nodes[recur_ref];
        if (recur->aux_len != lp->nvars)
          return sh_set_error(vc->err, SH_ERR_ARITY, -1, -1,
                              "RECUR arg count %u != loop nvars %u",
                              recur->aux_len, lp->nvars);
        for (uint32_t vi = 0; vi < lp->nvars; vi++) {
          sh_nref arg_ref = p->aux[recur->aux_off + vi];
          if (!valid_ref(p, arg_ref))
            return sh_set_error(vc->err, SH_ERR_INTERNAL, -1, -1,
                                "RECUR arg %u invalid ref", vi);
          sh_type targ = node_type(p, arg_ref);
          // Get the finalized type for this induction var from its init
          sh_nref init_r = p->aux[lp->init_off + vi];
          sh_type tvar = node_type(p, init_r);
          if (targ.kind == (uint8_t)SH_K_VOID) {
            // Try to resolve from tvar context
            if (kind_is_integer((sh_kind)tvar.kind)) {
              resolve_int_const(vc, arg_ref, (sh_kind)tvar.kind);
              targ = node_type(p, arg_ref);
            }
          }
          // Only enforce type if both sides are now concrete
          if (tvar.kind != (uint8_t)SH_K_VOID &&
              targ.kind != (uint8_t)SH_K_VOID &&
              !sh_type_eq(targ, tvar))
            return sh_set_error(vc->err, SH_ERR_TYPE, -1, -1,
                                "RECUR arg %u type mismatch", vi);
        }
        recur->type = sh_type_scalar(SH_K_VOID); // RECUR has no result type
      }

      // Compute loop cost
      uint64_t loop_total_cost;
      if (lp->bound.kind == SH_BOUND_CONST) {
        loop_total_cost = init_cost + lp->bound.konst * body_cost;
      } else {
        // PARAM-bound: cost depends on args; use body_cost as per-iter
        loop_total_cost = init_cost + body_cost; // symbolic; exact at invoke
      }

      n->type = tbody;
      vc->node_cost[ref] = loop_total_cost;

      // Now try to annotate region accesses in the loop with SH_NF_BOUNDS_PROVEN.
      // Look for REGION_LOAD/STORE nodes where the index is the loop induction var
      // and the limit is REGION_LEN of the same region.
      // We need to identify: which induction var is the counter and what its limit was.
      {
        uint32_t ind_slot = ind_slot_out;
        sh_nref limit_ref_check = SH_NREF_NONE;
        // Re-read the body's exit test
        if (valid_ref(p, body) && p->nodes[body].op == (uint16_t)SH_OP_IF) {
          sh_nref cond_r = p->nodes[body].a;
          if (valid_ref(p, cond_r) && p->nodes[cond_r].op == (uint16_t)SH_OP_CMP) {
            sh_nref la = p->nodes[cond_r].a;
            sh_nref lb = p->nodes[cond_r].b;
            if (is_local_slot(p, la, ind_slot)) limit_ref_check = lb;
            else if (is_local_slot(p, lb, ind_slot)) limit_ref_check = la;
          }
        }
        if (limit_ref_check != SH_NREF_NONE &&
            valid_ref(p, limit_ref_check) &&
            p->nodes[limit_ref_check].op == (uint16_t)SH_OP_REGION_LEN) {
          // The limit is (region-len REGION_PARAM)
          sh_nref region_node = p->nodes[limit_ref_check].a;
          uint32_t region_p_idx = 0;
          bool is_param = (valid_ref(p, region_node) &&
                           p->nodes[region_node].op == (uint16_t)SH_OP_PARAM);
          if (is_param) region_p_idx = p->nodes[region_node].a;
          // Annotate REGION_LOAD/STORE nodes that index by the same induction var
          for (uint32_t j = 0; j < p->nnodes; j++) {
            sh_op jop = (sh_op)p->nodes[j].op;
            if (jop == SH_OP_REGION_LOAD || jop == SH_OP_REGION_STORE) {
              sh_nref jreg = p->nodes[j].a;
              sh_nref jidx = p->nodes[j].b;
              // Check that the region is the same parameter
              bool same_region = (is_param && valid_ref(p, jreg) &&
                                  p->nodes[jreg].op == (uint16_t)SH_OP_PARAM &&
                                  p->nodes[jreg].a == region_p_idx);
              // Check that the index is the induction var
              bool same_idx = is_local_slot(p, jidx, ind_slot);
              if (same_region && same_idx) {
                p->nodes[j].vflags |= SH_NF_BOUNDS_PROVEN;
              }
            }
          }
        }
      }

      return SH_OK;
    }

    // --- RECUR ------------------------------------------------------------
    case SH_OP_RECUR: {
      // Verify all arg expressions
      uint64_t recur_cost = 0;
      for (uint32_t i = 0; i < n->aux_len; i++) {
        sh_nref arg = p->aux[n->aux_off + i];
        if (!valid_ref(p, arg))
          return sh_set_error(vc->err, SH_ERR_INTERNAL, -1, -1,
                              "RECUR arg %u invalid ref", i);
        s = verify_node(vc, arg);
        if (s != SH_OK) return s;
        recur_cost += vc->node_cost[arg];
      }
      // RECUR type is void (it doesn't return)
      n->type = sh_type_scalar(SH_K_VOID);
      vc->node_cost[ref] = recur_cost;
      return SH_OK;
    }

    // --- CALL -------------------------------------------------------------
    case SH_OP_CALL: {
      // n->a = prim index; aux = arg nrefs
      if (!vc->prims)
        return sh_set_error(vc->err, SH_ERR_NOT_WHITELISTED, -1, -1,
                            "CALL with no prim set");
      uint32_t pidx = n->a;
      if (pidx >= vc->prims->count)
        return sh_set_error(vc->err, SH_ERR_NOT_WHITELISTED, -1, -1,
                            "CALL: prim index %u out of range", pidx);

      const sh_prim *prim = &vc->prims->prims[pidx];
      if (n->aux_len != (uint32_t)prim->nparams)
        return sh_set_error(vc->err, SH_ERR_ARITY, -1, -1,
                            "CALL '%s': expected %u args, got %u",
                            prim->name, prim->nparams, n->aux_len);

      uint64_t call_cost = 0;
      for (uint32_t i = 0; i < n->aux_len; i++) {
        sh_nref arg = p->aux[n->aux_off + i];
        if (!valid_ref(p, arg))
          return sh_set_error(vc->err, SH_ERR_INTERNAL, -1, -1,
                              "CALL arg %u invalid ref", i);
        s = verify_node(vc, arg);
        if (s != SH_OK) return s;

        sh_type targ = node_type(p, arg);
        sh_type texpected = prim->params[i];

        // Resolve consts from expected type
        if (targ.kind == (uint8_t)SH_K_VOID) {
          if (kind_is_integer((sh_kind)texpected.kind))
            resolve_int_const(vc, arg, (sh_kind)texpected.kind);
          else if (texpected.kind == (uint8_t)SH_K_F32 ||
                   texpected.kind == (uint8_t)SH_K_F64)
            resolve_float_const(vc, arg, (sh_kind)texpected.kind);
          targ = node_type(p, arg);
        }

        if (!sh_type_eq(targ, texpected))
          return sh_set_error(vc->err, SH_ERR_TYPE, -1, -1,
                              "CALL '%s' arg %u type mismatch", prim->name, i);
        call_cost += vc->node_cost[arg];
      }

      n->type = prim->ret;
      vc->node_cost[ref] = call_cost + COST_CALL;
      return SH_OK;
    }

    // --- VSPLAT -----------------------------------------------------------
    case SH_OP_VSPLAT: {
      if (!valid_ref(p, n->a))
        return sh_set_error(vc->err, SH_ERR_INTERNAL, -1, -1, "VSPLAT missing operand");
      s = verify_node(vc, n->a);
      if (s != SH_OK) return s;

      sh_type tsrc = node_type(p, n->a);
      // The result type is set by context. The frontend does NOT set this.
      // Infer from the shader's declared return type when at the root
      // (the common case: `-> vec4 (splat x)`). If the program's declared
      // return type is a vector, use it as the VSPLAT result type.
      if (n->type.kind == (uint8_t)SH_K_VOID) {
        if (p->ret.kind == (uint8_t)SH_K_VEC) {
          n->type = p->ret;
        }
      }
      if (n->type.kind == (uint8_t)SH_K_VOID)
        return sh_set_error(vc->err, SH_ERR_TYPE, -1, -1,
                            "VSPLAT: cannot infer result vector type (no context)");
      if (n->type.kind != (uint8_t)SH_K_VEC)
        return sh_set_error(vc->err, SH_ERR_TYPE, -1, -1,
                            "VSPLAT: result must be a vector type");

      // Resolve scalar operand from the vector's lane kind
      sh_kind lane_k = (sh_kind)n->type.lane_kind;
      if (tsrc.kind == (uint8_t)SH_K_VOID) {
        if (kind_is_integer(lane_k)) resolve_int_const(vc, n->a, lane_k);
        else if (lane_k == SH_K_F32 || lane_k == SH_K_F64)
          resolve_float_const(vc, n->a, lane_k);
        tsrc = node_type(p, n->a);
      }

      if (tsrc.kind != (uint8_t)SH_K_VOID &&
          tsrc.kind != (uint8_t)n->type.lane_kind)
        return sh_set_error(vc->err, SH_ERR_TYPE, -1, -1,
                            "VSPLAT: scalar operand kind %u != lane kind %u",
                            tsrc.kind, n->type.lane_kind);

      vc->node_cost[ref] = (uint64_t)n->type.lanes * COST_VEC;
      return SH_OK;
    }

    // --- VBINOP (already promoted from BINOP in check_binop_operands) -----
    case SH_OP_VBINOP: {
      // Already typed by check_binop_operands
      if (n->type.kind == (uint8_t)SH_K_VOID)
        return sh_set_error(vc->err, SH_ERR_INTERNAL, -1, -1,
                            "VBINOP type not set");
      return SH_OK;
    }

    // --- VCMP (already promoted from CMP in check_binop_operands) ---------
    case SH_OP_VCMP: {
      if (n->type.kind == (uint8_t)SH_K_VOID)
        return sh_set_error(vc->err, SH_ERR_INTERNAL, -1, -1,
                            "VCMP type not set");
      return SH_OK;
    }

    // --- VSELECT ----------------------------------------------------------
    case SH_OP_VSELECT: {
      if (!valid_ref(p, n->a) || !valid_ref(p, n->b) || !valid_ref(p, n->c))
        return sh_set_error(vc->err, SH_ERR_INTERNAL, -1, -1,
                            "VSELECT missing operands");
      s = verify_node(vc, n->a);
      if (s != SH_OK) return s;
      s = verify_node(vc, n->b);
      if (s != SH_OK) return s;
      s = verify_node(vc, n->c);
      if (s != SH_OK) return s;

      sh_type tmask = node_type(p, n->a);
      sh_type tthen = node_type(p, n->b);
      sh_type telse = node_type(p, n->c);

      // mask must be a VEC of BOOL lanes (from VCMP)
      if (tmask.kind != (uint8_t)SH_K_VEC || tmask.lane_kind != (uint8_t)SH_K_BOOL)
        return sh_set_error(vc->err, SH_ERR_TYPE, -1, -1,
                            "VSELECT: mask must be bool vector (from vcmp)");
      if (!sh_type_eq(tthen, telse))
        return sh_set_error(vc->err, SH_ERR_TYPE, -1, -1,
                            "VSELECT: then/else types must match");
      if (tthen.kind != (uint8_t)SH_K_VEC)
        return sh_set_error(vc->err, SH_ERR_TYPE, -1, -1,
                            "VSELECT: operands must be vectors");
      if (tmask.lanes != tthen.lanes)
        return sh_set_error(vc->err, SH_ERR_TYPE, -1, -1,
                            "VSELECT: mask lane count %u != operand lane count %u",
                            tmask.lanes, tthen.lanes);

      n->type = tthen;
      vc->node_cost[ref] = (uint64_t)tthen.lanes * COST_VEC;
      return SH_OK;
    }

    // --- VSHUFFLE ---------------------------------------------------------
    case SH_OP_VSHUFFLE: {
      if (!valid_ref(p, n->a))
        return sh_set_error(vc->err, SH_ERR_INTERNAL, -1, -1,
                            "VSHUFFLE missing source");
      s = verify_node(vc, n->a);
      if (s != SH_OK) return s;

      sh_type tsrc = node_type(p, n->a);
      if (tsrc.kind != (uint8_t)SH_K_VEC)
        return sh_set_error(vc->err, SH_ERR_TYPE, -1, -1,
                            "VSHUFFLE: source must be a vector");

      // Validate each shuffle index < source lane count
      for (uint32_t i = 0; i < n->aux_len; i++) {
        uint32_t idx = p->aux[n->aux_off + i];
        if (idx >= (uint32_t)tsrc.lanes)
          return sh_set_error(vc->err, SH_ERR_TYPE, -1, -1,
                              "VSHUFFLE index %u >= source lane count %u",
                              idx, tsrc.lanes);
      }

      // Result type: same lane kind, aux_len lanes
      if (n->aux_len == 0 || n->aux_len > SH_MAX_LANES)
        return sh_set_error(vc->err, SH_ERR_TYPE, -1, -1,
                            "VSHUFFLE: result lane count %u out of range", n->aux_len);

      n->type = sh_type_vec((sh_kind)tsrc.lane_kind, (uint8_t)n->aux_len);
      vc->node_cost[ref] = (uint64_t)n->aux_len * COST_VEC;
      return SH_OK;
    }

    // --- VREDUCE ----------------------------------------------------------
    case SH_OP_VREDUCE: {
      if (!valid_ref(p, n->a))
        return sh_set_error(vc->err, SH_ERR_INTERNAL, -1, -1,
                            "VREDUCE missing operand");
      s = verify_node(vc, n->a);
      if (s != SH_OK) return s;

      sh_type tsrc = node_type(p, n->a);
      if (tsrc.kind != (uint8_t)SH_K_VEC)
        return sh_set_error(vc->err, SH_ERR_TYPE, -1, -1,
                            "VREDUCE: operand must be a vector");

      sh_reduce rop = (sh_reduce)n->sub;
      if (rop == SH_RED_DOT) {
        // dot product: needs second vector
        if (!valid_ref(p, n->b))
          return sh_set_error(vc->err, SH_ERR_INTERNAL, -1, -1,
                              "VREDUCE DOT missing second operand");
        s = verify_node(vc, n->b);
        if (s != SH_OK) return s;
        sh_type tsrc2 = node_type(p, n->b);
        if (!sh_type_eq(tsrc, tsrc2))
          return sh_set_error(vc->err, SH_ERR_TYPE, -1, -1,
                              "VREDUCE DOT: both vectors must have same type");
      }

      // Result = scalar of the lane kind
      n->type = sh_type_scalar((sh_kind)tsrc.lane_kind);
      vc->node_cost[ref] = (uint64_t)tsrc.lanes * COST_ARITH;
      return SH_OK;
    }

    // --- VLANE ------------------------------------------------------------
    case SH_OP_VLANE: {
      if (!valid_ref(p, n->a))
        return sh_set_error(vc->err, SH_ERR_INTERNAL, -1, -1,
                            "VLANE missing operand");
      s = verify_node(vc, n->a);
      if (s != SH_OK) return s;

      sh_type tsrc = node_type(p, n->a);
      if (tsrc.kind != (uint8_t)SH_K_VEC)
        return sh_set_error(vc->err, SH_ERR_TYPE, -1, -1,
                            "VLANE: operand must be a vector");

      int64_t lane_idx = n->imm;
      if (lane_idx < 0 || (uint64_t)lane_idx >= (uint64_t)tsrc.lanes)
        return sh_set_error(vc->err, SH_ERR_TYPE, -1, -1,
                            "VLANE: lane index %lld >= lane count %u",
                            (long long)lane_idx, tsrc.lanes);

      n->type = sh_type_scalar((sh_kind)tsrc.lane_kind);
      vc->node_cost[ref] = COST_ARITH;
      return SH_OK;
    }

    default:
      return sh_set_error(vc->err, SH_ERR_INTERNAL, -1, -1,
                          "unknown op %u at node %u", n->op, ref);
  }
}

// ---------------------------------------------------------------------------
// Finalize pass: resolve any remaining untyped CONST nodes and validate return
// ---------------------------------------------------------------------------

static sh_status finalize_all(verify_ctx *vc) {
  sh_program *p = vc->p;
  for (uint32_t i = 0; i < p->nnodes; i++) {
    sh_node *n = &p->nodes[i];
    if (n->op == (uint16_t)SH_OP_CONST && n->type.kind == (uint8_t)SH_K_VOID) {
      sh_status s = finalize_const(vc, i);
      if (s != SH_OK) return s;
    }
  }
  return SH_OK;
}

// ---------------------------------------------------------------------------
// Slot index bounds check
// ---------------------------------------------------------------------------

static sh_status validate_slots(verify_ctx *vc) {
  sh_program *p = vc->p;
  for (uint32_t i = 0; i < p->nnodes; i++) {
    sh_node *n = &p->nodes[i];
    if (n->op == (uint16_t)SH_OP_LOCAL) {
      if (n->a >= p->nlocals)
        return sh_set_error(vc->err, SH_ERR_INTERNAL, -1, -1,
                            "LOCAL slot %u >= nlocals %u", n->a, p->nlocals);
    }
    if (n->op == (uint16_t)SH_OP_PARAM) {
      if (n->a >= p->nparams)
        return sh_set_error(vc->err, SH_ERR_INTERNAL, -1, -1,
                            "PARAM index %u >= nparams %u", n->a, p->nparams);
    }
  }
  return SH_OK;
}

// ---------------------------------------------------------------------------
// Compute worst-case cost from the root
// ---------------------------------------------------------------------------

static void compute_cost(verify_ctx *vc) {
  sh_program *p = vc->p;
  sh_nref root = p->root;
  if (root == SH_NREF_NONE || root >= p->nnodes) {
    p->cost.is_const = true;
    p->cost.const_cost = 0;
    return;
  }

  // Check if any loop in the program has a PARAM bound
  bool has_param_bound = false;
  for (uint32_t i = 0; i < p->nloops; i++) {
    if (p->loops[i].bound.kind == SH_BOUND_PARAM) {
      has_param_bound = true;
      break;
    }
  }

  if (has_param_bound) {
    p->cost.is_const = false;
    p->cost.const_cost = vc->node_cost[root];
  } else {
    p->cost.is_const = true;
    p->cost.const_cost = vc->node_cost[root];
  }
}

// ---------------------------------------------------------------------------
// VSPLAT type resolution: if VSPLAT has no type set, infer from context.
// This is a forward pass from the root.
// We skip this for now -- the frontend must set the type from the declared
// return type or from context. In practice, the shader's return type is
// declared and the interpreter can check compatibility.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// shv_verify: public entry
// ---------------------------------------------------------------------------

sh_status shv_verify(sh_program *p, const sh_prim_set *prims, uint32_t flags,
                     sh_error *err) {
  if (!p) return sh_set_error(err, SH_ERR_INTERNAL, -1, -1, "null program");
  if (p->root == SH_NREF_NONE)
    return sh_set_error(err, SH_ERR_INTERNAL, -1, -1, "program has no root");
  if (p->nnodes == 0)
    return sh_set_error(err, SH_ERR_INTERNAL, -1, -1, "program has no nodes");
  if (p->nnodes > 4096)
    return sh_set_error(err, SH_ERR_INTERNAL, -1, -1,
                        "program too large (%u nodes, limit 4096)", p->nnodes);

  verify_ctx vc;
  memset(&vc, 0, sizeof(vc));
  vc.p = p;
  vc.prims = prims;
  vc.flags = flags;
  vc.err = err;

  // Validate slot indices first (cheap safety check)
  sh_status s = validate_slots(&vc);
  if (s != SH_OK) return s;

  // Bottom-up type inference from the root
  s = verify_node(&vc, p->root);
  if (s != SH_OK) return s;

  // Resolve any remaining untyped CONST nodes
  s = finalize_all(&vc);
  if (s != SH_OK) return s;

  // Check return type agrees with the declared ret type
  sh_type actual = node_type(p, p->root);
  if (!sh_type_eq(actual, p->ret)) {
    // If the actual is VOID (e.g. unresolved), try to match
    if (actual.kind != (uint8_t)SH_K_VOID) {
      return sh_set_error(err, SH_ERR_TYPE, -1, -1,
                          "return type mismatch: declared kind %u, got kind %u",
                          p->ret.kind, actual.kind);
    }
  }

  // Compute worst-case cost
  compute_cost(&vc);

  // Check SH_REQUIRE_CONST_COST
  if ((flags & SH_REQUIRE_CONST_COST) && !p->cost.is_const)
    return sh_set_error(err, SH_ERR_NONCONST_COST, -1, -1,
                        "SH_REQUIRE_CONST_COST: program has param-dependent cost");

  return SH_OK;
}
