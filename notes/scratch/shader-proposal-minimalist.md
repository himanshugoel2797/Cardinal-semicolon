<!--
 Copyright (c) 2026 Himanshu Goel

 This software is released under the MIT License.
 https://opensource.org/licenses/MIT
-->

# Shader tier — MINIMALIST implementation spec (S0 + S1 + S2-scalar, host-first)

Stance: **smallest trusted surface, simplest thing that correctly works.** The
verifier is the security boundary (no MMU firewall — see
`notes/core/lisp-shaders.md`), so the design optimizes for a verifier that is
*small, total, and obviously correct*. Where I cut a corner I note exactly what
S3 (the native backend) must retrofit and argue the retrofit is cheap.

This deliverable is **host-buildable + host-tested only**: no x86 SSE/AVX
backend, no in-OS `SysLisp` integration, no documentation-hash pipeline. The IR
and API must not *preclude* those, and the relevant hooks are called out inline.

---

## 0. The one big decision: IR = typed AST, NOT bytecode (for now)

The note's phased plan says "first compile target is typed bytecode + a
verifier." I am proposing a refinement, not a contradiction: **build a single
typed-AST IR; verify and interpret it directly; defer the flat bytecode encoding
to the moment something actually needs it (the native backend in S3, or an
in-VM cache that must survive a heap move).**

Rationale, in stance terms:

1. **The verifier is the trusted moat. The smallest correct verifier walks the
   thing it is given once.** A typed AST *is* the natural shape the verifier
   produces and consumes; there is no second lowering pass to review, no
   bytecode encoder/decoder pair to prove round-trips faithfully, and no risk
   that the bytecode the interpreter runs differs from the AST the verifier
   checked. With bytecode you must trust **two** representations and the lowering
   between them; with a typed AST you trust **one**.
2. **The reference interpreter is the semantic oracle.** A tree-walker over the
   typed AST is ~300 lines and *visibly* matches the language's evaluation rules.
   A bytecode VM with a value stack, jump targets, and a back-edge opcode is more
   code and a less-obvious correspondence to the source semantics — for zero
   benefit at S1/S2 where we never JIT.
3. **Monomorphic + unboxed means the AST nodes already carry their type.** After
   the verifier annotates every node with its `sh_type`, the interpreter never
   does dynamic dispatch — it switches on a small `sh_op` enum and a scalar
   `sh_kind`. That is exactly the dispatch a bytecode opcode would give you,
   minus the encoding.

**What S3 retrofits, and why it is cheap.** S3 needs (a) a linear instruction
stream to pattern-match for codegen and (b) basic blocks for the named-let loop.
Both are a *mechanical post-order flattening* of the already-verified, already-
typed AST — the classic "AST → three-address / stack bytecode" textbook pass,
operating on a tree the verifier has already proven safe. Crucially, **the
bytecode emitter is NOT in the trusted surface**: it runs *after* verification,
so a bug in it is a miscompile (caught by the differential test against the
reference interpreter), not a safety hole. We get to add the elaborate
representation exactly when it pays for itself, on the *verified* side of the
moat, with a free oracle to test it against. This is the note's own argument for
why bytecode-then-native is safe, applied one layer earlier: AST-then-bytecode.

So §7 below still specifies an opcode set — because S3 will want it and the API
must not preclude it — but it is **explicitly deferred**: S0/S1/S2 ship the
typed AST + tree-walker, and §7 is the forward note for the agent who later adds
`sh_lower.c`.

### IR node shape

A shader compiles to a flat **arena** of typed nodes addressed by 32-bit index
(`sh_nref`), not a pointer-linked tree. The arena is one `malloc`'d block per
compiled shader; child references are indices into it. This keeps the IR a
single contiguous allocation (cheap to free, no GC interaction, trivially
serializable later) while reading like a tree.

```c
typedef uint32_t sh_nref;            // index into sh_program.nodes; SH_NREF_NONE = no child
#define SH_NREF_NONE 0xFFFFFFFFu

typedef enum {
    SH_OP_CONST,        // literal: payload in node.imm, type in node.type
    SH_OP_PARAM,        // read parameter #node.a (a = param index)
    SH_OP_LOCAL,        // read a let-bound local: a = slot index
    SH_OP_UNOP,         // node.sub = sh_unop; a = operand
    SH_OP_BINOP,        // node.sub = sh_binop; a,b = operands
    SH_OP_CMP,          // node.sub = sh_cmp;   a,b = operands -> bool (or mask for vectors)
    SH_OP_IF,           // a = cond(bool), b = then, c = else  (both arms same type)
    SH_OP_LET,          // bind slots [a0..); body = last child. See sh_let side table.
    SH_OP_REGION_LOAD,  // a = region(param/local), b = index. Elem type = node.type.
    SH_OP_REGION_STORE, // a = region, b = index, c = value. (only on a mutable bytes view)
    SH_OP_REGION_LEN,   // a = region -> u32 length (provenance-tracked, see §4)
    SH_OP_LOOP,         // named-let loop header; see sh_loop side table (§3)
    SH_OP_RECUR,        // tail re-entry of the enclosing LOOP with new induction args
    SH_OP_CALL,         // call another shader / whitelisted prim: callee in node.a, args in side table
    // --- S2 vector ops (scalar-lowered in the reference interpreter, §8) ---
    SH_OP_VSPLAT,       // a = scalar -> <N×T> broadcast (N,T from node.type)
    SH_OP_VBINOP,       // lane-wise; node.sub = sh_binop; a,b = <N×T>
    SH_OP_VCMP,         // lane-wise compare -> mask<N>; node.sub = sh_cmp
    SH_OP_VSELECT,      // a = mask<N>, b = then<N×T>, c = else<N×T>
    SH_OP_VSHUFFLE,     // a = src<N×T>; constant lane indices in side table (compile-time only)
    SH_OP_VREDUCE,      // a = <N×T> -> scalar T; node.sub = sh_reduce (add/min/max/dot uses b)
    SH_OP_VLANE,        // a = <N×T>, lane const in node.imm -> scalar T (extract)
} sh_op;
```

Each node is fixed-size; variadic children (LET bindings, CALL args, LOOP
induction vars, SHUFFLE indices) live in a single shared **side-array**
(`uint32_t aux[]`) addressed by `(node.aux_off, node.aux_len)`. One arena, one
side-array; no per-node allocation.

```c
typedef struct {
    uint16_t op;        // sh_op
    uint16_t sub;       // sh_unop / sh_binop / sh_cmp / sh_reduce, else 0
    sh_type  type;      // result type, FILLED IN BY THE VERIFIER (zeroed at parse)
    uint32_t a, b, c;   // child sh_nref or small immediates; SH_NREF_NONE if unused
    uint32_t aux_off;   // start in sh_program.aux[] for variadic operands
    uint32_t aux_len;   // count there
    int64_t  imm;       // CONST integer payload / lane index; floats via the f64 bit pattern
} sh_node;
```

`sh_type` is 4 bytes (§2), so a node is 40 bytes — fine; we never have many.

---

## 1. Runtime value model: a tagged union, NOT a raw 64-bit slot

The note says shaders are monomorphic + unboxed. The *static* type is known for
every node, so in principle a runtime value could be a bare `uint64_t` with the
type carried separately. **I am choosing a small tagged union anyway**, for the
minimalist reason that it makes the reference interpreter *obviously correct and
self-checking* at trivial cost:

```c
typedef struct {
    sh_kind kind;       // SH_K_U8..SH_K_F64, SH_K_BOOL, SH_K_VEC, SH_K_REGION
    uint8_t lanes;      // 1 for scalars; N for a vector value
    union {
        uint64_t u;     // u8/u16/u32/u64 zero-extended; bool in {0,1}
        int64_t  i;     // i64
        double   f;     // f32 stored as a double, narrowed on store (see note)
        struct { uint8_t *base; uint32_t len; sh_kind elem; uint8_t mutable_; } region;
        uint64_t lane[SH_MAX_LANES];  // vector lanes, each a scalar bit pattern
    };
} sh_value;
```

- **Why a union and not a bare word:** the interpreter is the oracle and runs
  only host-side and (later) cold in-VM compile/verify paths — it is **not** the
  hot path the backend replaces. Carrying `kind`/`lanes` lets every interpreter
  step `assert()` that the value it popped matches the static type the verifier
  stamped on the node. That turns "the verifier and interpreter agree" from a
  hope into a checked invariant on every test run — the single most valuable
  property for a security-critical component. The cost (a few bytes, a branch)
  is irrelevant because this code is never the SIMD fast path.
- **`f32` is held as `double` and *narrowed* (`(double)(float)x`) at every store,
  region write, and CONST.** This keeps the value model to one float field while
  preserving `f32` rounding semantics bit-exactly — which matters because the
  S3 backend's `f32x4` *must* match the interpreter bit-for-bit (the note's
  mandatory differential test). The narrowing points are: CONST of `f32`,
  `REGION_STORE` to an `(bytes f32)`, `VSPLAT`/`VREDUCE` producing `f32`, and the
  result of every `f32` arithmetic op.
- **`SH_MAX_LANES`**: fix at 16 (covers `u8x16`, `f32x4`, `u32x8`). A wider type
  is a verifier error in v1. S3 can raise it; nothing in the value model assumes
  a particular SIMD width (that is the whole point of scalar lowering).

The **call ABI** for `sh_invoke` (§6) is *also* `sh_value` arrays, for the same
self-checking reason: the host hands typed args in, the interpreter validates
each against the shader's declared param types before running.

---

## 2. The C encoding of a shader type

One 32-bit POD, no allocation, comparable by `==`:

```c
typedef enum : uint8_t {
    SH_K_VOID = 0,
    SH_K_BOOL, SH_K_U8, SH_K_U16, SH_K_U32, SH_K_U64, SH_K_I64,
    SH_K_F32, SH_K_F64,
    SH_K_VEC,       // a fixed-width vector; lane kind + count in sh_type
    SH_K_REGION,    // a typed bytes view; element kind in sh_type
} sh_kind;

typedef struct {
    uint8_t  kind;      // sh_kind
    uint8_t  lane_kind; // for SH_K_VEC / SH_K_REGION: the element's sh_kind (a SCALAR kind)
    uint8_t  lanes;     // for SH_K_VEC: lane count (2,3,4,8,16); else 0
    uint8_t  flags;     // bit0 = region is mutable (store allowed); reserved otherwise
} sh_type;
```

- **Scalar:** `{kind, 0, 0, 0}`. `sh_type_scalar(SH_K_U32)`.
- **Vector `<N×T>`:** `{SH_K_VEC, T, N, 0}`. `vec3`/`vec4` are sugar the frontend
  maps to `{SH_K_VEC, SH_K_F32, 3|4}` — *unified*, per the note's locked decision.
- **Typed region `(bytes T)`:** `{SH_K_REGION, T, 0, mutable?1:0}`. The element
  width drives the byte-offset math (`index * sizeof(T)`); see §4.

Length is **not** in the type — it is *provenance*, tracked by the verifier as a
symbolic fact about a value, not a number in the type (§3/§4). This is the
minimalist choice: types stay a flat comparable POD; the *interesting* static
reasoning (bounds, trip counts) lives in one analysis pass over the AST, not
smeared into a dependent-type encoding. S3 needs nothing more from the type.

Helpers (all `static inline`, pure):
```c
static inline sh_type sh_type_scalar(sh_kind k);
static inline sh_type sh_type_vec(sh_kind lane, uint8_t n);
static inline sh_type sh_type_region(sh_kind elem, bool mut);
static inline bool     sh_type_eq(sh_type a, sh_type b);
static inline uint32_t sh_kind_size(sh_kind k);   // bytes: u8->1 ... f64->8, bool->1
static inline bool     sh_kind_is_int(sh_kind k);
static inline bool     sh_kind_is_float(sh_kind k);
```

---

## 3. Bounded-loop verification (named-let)

The single loop form. A `(let loop ((i init) (acc init) ...) body)` where the
body tail-calls `loop` is parsed to one `SH_OP_LOOP` node + a side table:

```c
typedef struct {
    uint32_t nvars;            // induction variables
    uint32_t var_slot0;        // first local slot they occupy
    sh_nref  body;             // loop body (an expr tree containing SH_OP_RECUR in tail position)
    // --- bound, derived by the verifier ---
    sh_bound bound;
} sh_loop;

typedef enum { SH_BOUND_CONST, SH_BOUND_PARAM, SH_BOUND_NONE } sh_bound_kind;
typedef struct {
    sh_bound_kind kind;
    uint64_t      konst;       // for CONST: the literal ceiling
    uint32_t      param_idx;   // for PARAM: trip count <= value of this u32 param
    uint64_t      per_iter_cost;  // worst-case cost of one body iteration (see below)
} sh_bound;
```

**How the bound is derived (deliberately narrow + total).** The verifier
recognizes exactly one safe loop shape and *rejects everything else* — rejection
is always safe, so a narrow recognizer is the minimalist-correct choice:

A loop is *bounded* iff there is an induction var `i` with:
1. `init` is a constant or a parameter,
2. the **first** `if` in the body tests `(>= i LIMIT)` (or `<`, `<=`, `>`,
   normalized) where `LIMIT` is a constant or a `u32`/`u64` **parameter**, and
   that branch returns without recurring (the exit),
3. every `RECUR` passes `i' = (+ i STEP)` with a **constant positive** `STEP`
   for that induction var.

Then trip count `<= ceil((LIMIT - init)/STEP)`. If `LIMIT` and `init` are
constants → `SH_BOUND_CONST`; if `LIMIT` is a parameter → `SH_BOUND_PARAM`
(ceiling is `param_value` at invoke time). Anything not matching this template →
`SH_BOUND_NONE` → **compile error** (`SH_ERR_UNBOUNDED_LOOP`). No fixpoint, no
abstract interpretation — a syntactic pattern match. This is plenty for the
note's examples and is *obviously total*.

**Worst-case cost** = `per_iter_cost * trip_count_ceiling`, computed bottom-up:
each node has a small fixed cost (`sh_node_cost(op)` — 1 for arith, K for a
region access, the callee's whole cost for a CALL, the inner loop's full cost for
a nested loop). Nested bounded loops multiply. The cost is attached to the
compiled program:

```c
typedef struct {
    sh_bound_kind kind;     // CONST => total is known now; PARAM => total needs invoke args
    uint64_t      const_cost;        // valid iff every bound on the path is CONST
    // PARAM-dependent cost is recomputed at invoke from the actual u32 args; the
    // program records the symbolic per-loop bound, sh_cost_for_args() evaluates it.
} sh_cost;
uint64_t sh_cost_for_args(const sh_program *p, const sh_value *args, uint32_t argc);
```

**Two regimes (note §"Bounded loops + cost model").** The compile entry takes a
flag `SH_REQUIRE_CONST_COST` (set it for the future ISR/`cli()` use). When set,
any `SH_BOUND_PARAM` on a path → `SH_ERR_NONCONST_COST`. Default (cooperative)
allows param-dependent cost; the scheduler later charges `sh_cost_for_args`.
S4 wiring only reads `sh_cost`; the analysis is done here, once.

---

## 4. Region bounds checking + later elision

`(bytes T)` is a `{base, len, elem}` triple at runtime (`sh_value.region`).
Every `REGION_LOAD`/`REGION_STORE` in the **reference interpreter** does a
checked access:

```c
// in the interpreter, for every region access:
if (idx >= reg.len) -> trap SH_TRAP_BOUNDS (abort this invocation, structured error)
byte_off = idx * sh_kind_size(reg.elem);   // elem width from the static type
// load/store sizeof(T) bytes, narrowing f32 on store
```

`len` is the runtime length of the bytes the host handed in (a `lisp_bytes`'s
`len`, or an explicit length param). `REGION_LEN` reads it.

**Static info that enables later elision (S3), recorded but not yet used.** The
verifier already proves, for the loop template in §3, that the induction index
`i` satisfies `i < LIMIT` on the body path, and the loop bound ties `LIMIT` to
either a constant or the region's `REGION_LEN`. So the verifier annotates a
region-access node with a `bounds_proven` bit when **both**:
- the index is a loop induction var whose loop's `LIMIT` is `== REGION_LEN` of
  the same region (the `(loop (+ i 1) ...)` over `n = (region-len buf)` idiom),
  **or** the index is a constant `< a known constant length`.

```c
// in sh_node, repurpose two bits of `sub` for region nodes, or add:
#define SH_FLAG_BOUNDS_PROVEN 0x1   // set on REGION_LOAD/STORE the verifier discharged
```

The **reference interpreter ignores this bit and always checks** (minimalist:
the oracle is maximally safe). The S3 backend reads it to *skip* the runtime
compare. Because the interpreter still checks, the differential test catches any
case where the bit was set wrongly — i.e. an over-eager elision is caught as a
divergence, not exploited. **That is the whole minimalist bet: prove-then-elide
is opt-in and validated by the always-checking oracle.** S3 retrofit cost:
read one bit. If S3 ships before the elision analysis is trusted, it simply
ignores the bit and checks too — still correct, just slower.

---

## 5. Capability whitelist (W7, zero ambient authority)

The host registers the *closed set* of primitives a shader may name, as an
explicit table passed into `sh_compile`. A shader that names anything not in the
table (or any banned form) fails to compile. There is **no ambient global
environment** for shaders — unlike the dynamic Lisp, the shader frontend does
**not** consult `lisp_default_env`; the only names in scope are parameters,
locals, the fixed special forms (§ banned/permitted in the note), and this table.

```c
typedef struct {
    const char *name;        // the symbol a shader writes to call it, e.g. "fold-carry"
    sh_type     ret;
    uint8_t     nparams;
    sh_type     params[SH_MAX_PRIM_PARAMS];   // closed typed signature == the capability
    sh_prim_fn  fn;          // host C impl, scalar; NULL for an as-yet-unimplemented stub
} sh_prim;

// A primitive's C implementation in the reference interpreter: typed args in,
// one typed result out. Pure, allocation-free, total (no traps of its own in v1).
typedef sh_value (*sh_prim_fn)(const sh_value *args, uint32_t argc);

typedef struct {
    const sh_prim *prims;
    uint32_t       count;
} sh_prim_set;
```

- The signature **is** the capability: a shader can call `fold-carry` only with
  the exact arg types declared, and gets exactly the declared return type. The
  verifier type-checks every `CALL` against this. Zero authority is the default —
  an empty `sh_prim_set` means a shader can only do arithmetic on its params.
- **Other shaders as callees:** a compiled `sh_program` can be added to a
  `sh_prim_set` as a callee too (an adapter prim whose `fn` invokes the inner
  program). This is how "calls to other shaders" works without a second
  mechanism. For S0–S2 we can defer inter-shader calls (single-shader is enough
  to demo); the CALL node + prim table already subsumes it, so no API change.
- This mirrors `lisp_register_builtin_module` exactly in spirit (named,
  withhold-to-deny) but is *static and per-compile*, not a runtime registry —
  the minimalist tightening the note calls "W7 at its limit."

---

## 6. Public C API (`inc/lisp_shader.h`)

```c
// ---- error reporting -------------------------------------------------------
typedef enum {
    SH_OK = 0,
    SH_ERR_PARSE,            // not a well-formed (defshader ...) / reader gave junk
    SH_ERR_BAD_FORM,         // a banned/unknown form in the body
    SH_ERR_TYPE,             // a type mismatch
    SH_ERR_UNKNOWN_NAME,     // a free identifier (not param/local/prim)
    SH_ERR_NOT_WHITELISTED,  // a call to a name not in the prim set
    SH_ERR_UNBOUNDED_LOOP,   // a loop the §3 template did not recognize
    SH_ERR_NONCONST_COST,    // SH_REQUIRE_CONST_COST set but cost is arg-dependent
    SH_ERR_ARITY,            // wrong number of args at a call or at invoke
    SH_ERR_OOM,
    SH_ERR_INTERNAL,
} sh_status;

typedef struct {
    sh_status status;
    char      msg[160];      // human-readable; static-ish, snprintf'd
    int       line, col;     // source location (from lisp_source_location) when known
} sh_error;

// ---- compile (frontend + verifier; S0/S1/S2) -------------------------------
typedef struct sh_program sh_program;   // opaque compiled handle (one arena alloc)

enum { SH_REQUIRE_CONST_COST = 1u << 0 };

// Compile ONE shader. `form` is an s-expr the existing reader produced
// (a (defshader ...) datum). `prims` is the closed capability set. On success
// returns SH_OK and *out_prog (caller frees with sh_free); on failure returns
// the error and *err is filled. Never allocates after returning. Total: every
// input either compiles or yields a structured error, never UB.
sh_status sh_compile(lisp_value form, const sh_prim_set *prims,
                     uint32_t flags, sh_program **out_prog, sh_error *err);

// Convenience: read the first datum from `src` and compile it. (Wraps lisp_read.)
sh_status sh_compile_string(const char *src, const sh_prim_set *prims,
                            uint32_t flags, sh_program **out_prog, sh_error *err);

void sh_free(sh_program *p);

// ---- introspection (the derived contract; free, drift-proof) ---------------
uint32_t  sh_param_count(const sh_program *p);
sh_type   sh_param_type(const sh_program *p, uint32_t i);
sh_type   sh_return_type(const sh_program *p);
const char *sh_name(const sh_program *p);
sh_cost   sh_static_cost(const sh_program *p);          // §3
uint64_t  sh_cost_for_args(const sh_program *p, const sh_value *args, uint32_t argc);

// ---- invoke (the reference interpreter; S1/S2) -----------------------------
// Run the verified program on typed args. Validates argc and each arg's sh_type
// against the declared params (SH_ERR_ARITY / SH_ERR_TYPE). A runtime trap
// (region out-of-bounds, etc.) returns SH_ERR_* with a trap message; the program
// itself can never corrupt memory because every access is checked. On SH_OK,
// *out holds the typed result.
sh_status sh_invoke(const sh_program *p, const sh_value *args, uint32_t argc,
                    sh_value *out, sh_error *err);

// ---- value helpers for the host (build args / read results) ----------------
static inline sh_value sh_val_u32(uint32_t x);
static inline sh_value sh_val_i64(int64_t x);
static inline sh_value sh_val_f32(float x);
static inline sh_value sh_val_f64(double x);
static inline sh_value sh_val_bool(bool b);
sh_value sh_val_region(lisp_value bytes, sh_kind elem, bool mutable_); // wraps a lisp_bytes
sh_value sh_val_region_raw(void *base, uint32_t len, sh_kind elem, bool mutable_);
```

`sh_compile` is the trusted entry. `sh_invoke` runs only *verified* programs.
The two are separate so the in-OS path (S4) can compile/verify once and invoke
many times, and so the verifier can be tested in isolation (compile-only, never
invoke). Note the deliberate absence of any "load pre-compiled bytecode" entry —
bytecode is an internal cache (note's locked decision), so there is no public
deserialize that would have to be in the trusted surface.

---

## 7. Bytecode opcode set — DEFERRED to S3 (specified so the API doesn't preclude it)

Per §0 we ship the typed AST, not bytecode. This section is the forward
contract for the agent who later writes `sh_lower.c` (AST → bytecode) and the
S3 backend. It is **not** built in this deliverable.

A flat, typed, stack-or-register bytecode is a *mechanical* lowering of the
post-verification AST. Proposed op set (one-to-one with the §0 `sh_op`s plus
explicit control flow):

```
; scalar arith / convert       SHB_CONST, SHB_ADD/SUB/MUL/DIV/MOD (kind-suffixed),
                               SHB_AND/OR/XOR/SHL/SHR, SHB_NEG, SHB_CVT (k->k)
; compare                       SHB_CMP_LT/LE/EQ/NE/GT/GE  -> bool/mask
; region                        SHB_RLOAD (elem-kind imm), SHB_RSTORE, SHB_RLEN
; control flow                  SHB_JMP, SHB_JMP_IF, SHB_JMP_IFNOT, SHB_RET
; bounded-loop back-edge        SHB_LOOP_HDR (carries the verified trip bound),
                               SHB_RECUR (update induction regs, jump to header)
; call                          SHB_CALL (callee index, fixed arity)
; vector (abstract, ISA-free)   SHB_VSPLAT, SHB_VADD/... , SHB_VCMP, SHB_VSEL,
                               SHB_VSHUF (const indices), SHB_VREDUCE, SHB_VLANE
```

Invariants the lowering must preserve and the verifier guarantees upstream:
back-edges only via `SHB_LOOP_HDR`/`SHB_RECUR` (no general jumps → control flow
stays reducible and bound-annotated); every `SHB_VxxX` keeps its abstract
meaning (the backend, layer 4, is the only place ISA opcodes appear). Because
lowering runs **after** verification, a bug in it is a miscompile caught by the
differential oracle, never a safety hole. **Retrofit cost: one new file
(`sh_lower.c`) + the backend; zero change to types, value model, or API.**

---

## 8. Vector value + op representation, and the MANDATORY scalar lowering

A vector value is `sh_value{ kind=SH_K_VEC, lanes=N, lane[0..N) = scalar bit
patterns of the lane kind }`. There is **no SIMD anywhere in this deliverable**:
every vector op is defined by a lane loop in the reference interpreter, and that
lane loop **is the semantics** (S3's SSE/AVX must match it bit-for-bit).

```c
// the ONLY definition of vector semantics in S0-S2 — a lane loop:
static sh_value vbinop(sh_binop op, sh_value x, sh_value y) {
    sh_value r = x;                       // copies kind/lanes
    for (uint8_t k = 0; k < x.lanes; k++)
        r.lane[k] = scalar_binop(op, x.lane_kind_at(k), x.lane[k], y.lane[k]);
    return r;                             // f32 lanes narrowed inside scalar_binop
}
```

- **`VSPLAT`**: `for k: r.lane[k] = s` (broadcast).
- **`VBINOP` / `VCMP`**: lane-wise `scalar_binop` / `scalar_cmp`; `VCMP`
  produces a **mask** value (`lanes` booleans, each lane all-ones/zero
  conceptually, stored as 0/1 — the interpreter only needs truthiness; the S3
  backend materializes real all-ones masks, still differential-tested via
  `VSELECT` results).
- **`VSELECT`**: `for k: r.lane[k] = mask.lane[k] ? then.lane[k] : else.lane[k]`.
- **`VSHUFFLE`**: indices are **compile-time constants** (verifier rejects a
  non-constant index → keeps the op portable and the backend free to emit
  `PSHUFB`); `for k: r.lane[k] = src.lane[idx[k]]`.
- **`VREDUCE`** (add/min/max/dot): a sequential fold over lanes to a scalar;
  `dot` reduces `x*y`. **Reduction order is fixed left-to-right and is part of
  the spec** so float reductions are deterministic and the backend must match it
  (a tree-reduction backend would diverge — so the contract is sequential, and
  S3 either matches it or we accept a documented `f32` reduction tolerance; the
  minimalist call is *exact sequential, no tolerance*, since correctness-as-
  bit-equality is the note's stated test).
- `VLANE` extracts one lane to a scalar (constant index).

Geometric sugar (`vec3`, `.xyz`) is frontend-only: it desugars to the same
`SH_K_VEC` nodes + a `VSHUFFLE` for swizzles before the verifier ever runs, so
the trusted core sees only `<N×T>`.

---

## 9. File-by-file decomposition + the 3 disjoint units

```
libs/lisp_shader/
  CMakeLists.txt                 # mirrors libs/lisp: STATIC lib `lisp_shader`,
                                 #   depends on `lisp`, same -msse/-mstackrealign
                                 #   /-fno-vectorize flags (it links beside lisp).
  inc/
    lisp_shader.h                # THE shared public header (§2,§5,§6): sh_kind,
                                 #   sh_type(+inline helpers), sh_value, sh_prim,
                                 #   sh_prim_set, sh_program(opaque), sh_error,
                                 #   sh_status, all public fn signatures.
  src/
    sh_internal.h                # SHARED internal header: sh_node, sh_op, sh_unop,
                                 #   sh_binop, sh_cmp, sh_reduce, sh_loop, sh_bound,
                                 #   sh_cost, sh_program (full def: nodes[], aux[],
                                 #   params[], prim_set ref, root nref, cost), and
                                 #   the cross-unit fn prototypes below. The ONE
                                 #   file all three units include; frozen first.
    sh_frontend.c                # UNIT A: reader s-expr -> typed AST (untyped types)
    sh_verify.c                  # UNIT B: type-check + bounds + loops + caps; the MOAT
    sh_interp.c                  # UNIT C: reference interpreter + scalar lowering
    sh_value.c                   # UNIT C: sh_value helpers, region wrap, narrowing
    sh_error.c                   # UNIT A: sh_error formatting helpers (shared, tiny)
  test/
    build-and-run.sh             # mirrors libs/lisp/test: each test_*.c its own main,
                                 #   host clang against ../src/*.c + ../../lisp/src/*.c
                                 #   + -I both inc dirs.
    test_frontend.c              # UNIT A: parse/desugar; banned-form rejection
    test_types.c                 # UNIT B: sh_type helpers + type-check accept/reject
    test_verify.c                # UNIT B: bounds, loop template, cost, caps, OOB-static
    test_interp_scalar.c         # UNIT C: scalar arith / region / loop execution
    test_interp_vector.c         # UNIT C: every vector op vs hand-written lane loop
    test_diff.c                  # UNIT C: differential — shader result == equivalent
                                 #   dynamic-Lisp result (uses libs/lisp eval) for a
                                 #   corpus (ip-checksum, a blit, a dot product).
```

### The shared contract (`sh_internal.h`) — frozen before fan-out

Defines the AST node types, the `sh_program` layout, and these **cross-unit
function prototypes** that are the seams between units:

```c
// UNIT A produces this (no types yet; sh_node.type left zeroed):
sh_status shf_parse(lisp_value form, sh_program *p, sh_error *err);   // sh_frontend.c

// UNIT B consumes the parsed program in place, filling sh_node.type, the
// sh_loop bounds, sh_cost, and validating caps. After this returns SH_OK the
// program is VERIFIED and safe to interpret:
sh_status shv_verify(sh_program *p, const sh_prim_set *prims, uint32_t flags,
                     sh_error *err);                                  // sh_verify.c

// UNIT C runs a VERIFIED program:
sh_status shi_invoke(const sh_program *p, const sh_value *args, uint32_t argc,
                     sh_value *out, sh_error *err);                   // sh_interp.c
uint64_t  shi_cost_for_args(const sh_program *p, const sh_value *a, uint32_t n);
```

`sh_compile` (the public entry) lives in `sh_frontend.c` and is just:
`alloc program arena -> shf_parse -> shv_verify -> return`. That three-call body
is the entire trusted pipeline, readable at a glance.

### The 3 disjoint units (by file ownership — minimal conflict)

- **UNIT A — Frontend (`sh_frontend.c`, `sh_error.c`, `test_frontend.c`).**
  Owns: the reader-datum → AST walk, all desugaring (`defshader` shape, named-let
  recognition into a LOOP+RECUR skeleton, `vec`/swizzle sugar → `SH_K_VEC` +
  VSHUFFLE), banned-form rejection, the arena/aux allocators, `sh_error`
  formatting, and the public `sh_compile`/`sh_compile_string`/`sh_free`/`sh_name`/
  `sh_param_*` glue. Produces an untyped-but-structured `sh_program`. Depends
  only on `lisp.h` (reader) + `sh_internal.h`.
- **UNIT B — Verifier (`sh_verify.c`, `test_types.c`, `test_verify.c`).** Owns:
  type inference/checking over the AST, the §3 loop-bound template + cost, the §4
  static bounds-proven annotation, the §5 capability check, and the `sh_type`
  inline helpers' tests. The security moat — separately reviewable, touches no
  I/O, no allocation (annotates in place). Depends only on `sh_internal.h`.
- **UNIT C — Interpreter + values (`sh_interp.c`, `sh_value.c`,
  `test_interp_scalar.c`, `test_interp_vector.c`, `test_diff.c`).** Owns: the
  tree-walking reference interpreter, the mandatory scalar lowering for every
  vector op (§8), the `sh_value` model + f32 narrowing + region wrapping (§1),
  runtime bounds traps, `sh_invoke`, and the differential oracle tests. Depends
  on `sh_internal.h` + `lisp.h` (to wrap `lisp_bytes` and, in `test_diff.c`, to
  evaluate the reference dynamic-Lisp version).

Conflict surface: only `sh_internal.h` and `inc/lisp_shader.h` are shared, and
both are frozen up front. Each unit owns its `.c` files and its `test_*.c`
outright. The seams are the three `shf_/shv_/shi_` prototypes above — agreed
before fan-out, so the agents never edit the same file.

---

## Where I'd push back on the note / the forward-compat stance

1. **"First compile target is typed bytecode."** I push the bytecode one phase
   later (§0): ship a typed AST + tree-walker for S0–S2, add bytecode in S3 where
   the native backend actually consumes it. The note's own safety argument
   (verify the small core, build native over the *verified* IR) applies one layer
   up: AST-then-bytecode keeps the trusted surface to *one* representation instead
   of two-plus-a-lowering, and the lowering, when it comes, sits on the verified
   side with a free oracle. A forward-compat proposal will likely want bytecode +
   maybe SSA *now* "so S3 is easy." I'd argue that is premature: it enlarges the
   thing we must prove total today to save a mechanical, post-verification,
   differentially-tested pass later. Cut now, retrofit cheap.

2. **Length-as-dependent-type vs length-as-provenance.** The note says regions
   have an "SSA-tracked length." A forward-compat design may encode that length
   into the type (dependent/refinement types) so bounds elision falls out of type
   equality. I keep `sh_type` a flat 4-byte POD and put all length/bound reasoning
   in *one* analysis pass over the AST (§3/§4), with a single `bounds_proven` bit
   the always-checking interpreter validates. Smaller trusted type system, and the
   oracle catches a wrong elision instead of a subtle type-rule bug enabling one.

3. **SSA / a forward-looking IR + a tagged-1-word value.** I'd resist both an SSA
   IR (vs the direct typed AST) and a bare-`uint64_t` runtime value (vs the
   self-checking tagged union in §1). SSA buys optimization we don't do until S3;
   the tagged union costs nothing on this never-hot oracle and turns
   "verifier and interpreter agree" into a per-run assertion. The minimalist
   principle: pay for the elaborate representation when a consumer exists, and
   never weaken the oracle's self-checking to save bytes on a cold path.
