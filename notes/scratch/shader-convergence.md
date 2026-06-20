<!--
 Copyright (c) 2026 Himanshu Goel

 This software is released under the MIT License.
 https://opensource.org/licenses/MIT
-->

# Shader tier — converged implementation decision (S0+S1+S2-scalar)

Two independent Opus design proposals were produced (a debate on the central S0
fork the note flags as painful to retrofit): `shader-proposal-minimalist.md`
("smallest trusted surface") vs. `shader-proposal-forward.md` ("IR the S3 native
backend retrofits onto cheaply"). They **agree** on every painful-to-retrofit
surface — the flat 4-byte `sh_type` POD, length-as-provenance (not in the type),
the capability whitelist = closed typed signature, the mandatory scalar lowering
of vector ops, and the `sh_compile`/`sh_invoke` public API. The **only** real
divergence is the executed representation: typed-AST-arena + tree-walker
(minimalist) vs. mini-SSA-over-basic-blocks + register file (forward).

**Decision: MINIMALIST, with its own forward hooks.** Rationale matches the
project's deepest value (the verifier stands in for an absent MMU — keep the
trusted surface small; cf. the substrate note cutting macros/`call/cc` and doing
conservative-GC-before-precise). The minimalist design keeps every
painful-to-retrofit surface stable and defers only the *cheap-to-retrofit* part
(AST→bytecode), which sits on the **verified** side of the moat and is
oracle-tested — so a wrong lowering is a caught miscompile, not a safety hole.
The forward-compat SSA/blocks buys a cleaner S3 retrofit at the cost of ~300-500
LOC of CFG/SSA bookkeeping *in the trusted path today*, for a backend two phases
out and explicitly out of scope. Adopted forward hooks: the deferred opcode set
is documented (§7 of the minimalist spec), region nodes carry an
`SH_NF_BOUNDS_PROVEN` bit for later elision, and the type/value/API surfaces are
frozen.

The contract is now scaffolded and host-green: `libs/lisp_shader/` with the
public header `inc/lisp_shader.h`, the frozen private IR `src/sh_internal.h`, the
real plumbing (arena builders, error helper, compile pipeline, introspection,
value helpers), the three stubbed seams, and a passing scaffold smoke test.

## The three implementation units (disjoint file ownership)

- **Unit A — Frontend** (`src/sh_frontend.c`, `test/test_frontend.c`):
  `(defshader …)` datum → structured untyped AST; desugaring (named-let →
  LOOP+RECUR, `vecN`/swizzle → `SH_K_VEC`+VSHUFFLE); banned-form rejection.
  Implements `shf_parse`; the arena builders + pipeline glue are already real.
- **Unit B — Verifier (the moat)** (`src/sh_verify.c`, `test/test_verify.c`):
  type-check the AST in place, derive the bounded-loop trip bound + worst-case
  cost, discharge static region bounds, enforce the capability whitelist.
  Implements `shv_verify`. Tests on hand-built IR (frontend is a stub in its
  isolated tree).
- **Unit C — Interpreter + values** (`src/sh_interp.c`, `src/sh_value.c`,
  `test/test_interp.c`): tree-walking reference interpreter; mandatory scalar
  lane-loop lowering of every vector op; runtime bounds traps. Implements
  `shi_invoke`/`shi_cost_for_args`. Tests on hand-built typed IR.

Integration (end-to-end `compile_string → verify → invoke` + the differential
corpus against dynamic Lisp) is the orchestrator's merge-time job, since it spans
all three units.
