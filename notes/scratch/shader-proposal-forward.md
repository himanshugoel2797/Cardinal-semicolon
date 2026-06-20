<!--
 Copyright (c) 2026 Himanshu Goel

 This software is released under the MIT License.
 https://opensource.org/licenses/MIT
-->

# `libs/lisp_shader` implementation spec — FORWARD-COMPATIBLE stance

Implementation design for S0+S1+S2-scalar of the shader tier described in
[`notes/core/lisp-shaders.md`](../core/lisp-shaders.md). This spec is the
*implementation* beneath that settled design note. It is opinionated and
host-first: no x86 backend, no SysLisp integration, no doc-hash pipeline. The
governing constraint of this stance: **the IR shape chosen now must let S3 (an
x86 SSE/AVX backend) and an eventual JIT be *new backends over the same IR*, not
a rewrite.** Every bit of extra structure below is justified by the concrete
retrofit it buys, and the "Honest complexity cost" section names what it costs
today.

The note's own warning is the thesis of this stance:

> S0's exact IR shape is the decision everything downstream is painful to retrofit.

---

## 0. Executive decisions (the load-bearing choices)

| # | Decision | Why (the retrofit it buys) |
|---|----------|----------------------------|
| D1 | **IR = typed, three-address, mini-SSA over explicit basic blocks.** Values are SSA virtual registers (`vreg`), each with a static `shader_type`. Blocks end in an explicit terminator. | A code generator walks blocks in order and emits per-op machine code; SSA gives it use-def for free register allocation; no AST re-traversal. This is the LLVM/Cranelift/SPIR-V shape — the backends we are pre-paying for already assume it. |
| D2 | **Runtime value model = a flat typed register file.** One `uint64_t` slot per `vreg` (or a small inline lane array for vectors), no boxing, no tags. The static type table says how to interpret each slot. | An SSE backend assigns each `vreg` to a hardware register / spill slot using the *same* index space. The interpreter's "slot file" and the backend's "register file" are the same abstraction at two fidelities. |
| D3 | **Abstract vector ops carry lane-kind + lane-count in the op, not just the type.** Every vector op has a *mandatory scalar lane-loop* in the reference interpreter. | The backend pattern-matches `OP_VADD{f32,4}` → one `VADDPS`; the interpreter runs the same op as a 4-iteration scalar loop. Bit-for-bit differential test for free (the note's "scalar == backend" oracle). |
| D4 | **Region accesses are two ops: an explicit `BOUNDS_CHECK` op and a raw `LOAD`/`STORE`.** The check is a separate IR node carrying the SSA length value. | A backend elides the check by deleting the `BOUNDS_CHECK` node when range analysis proves it redundant — *without touching the load*. Check-elision becomes a peephole over the IR, the single most valuable region optimization, retrofittable with zero load/store rework. |
| D5 | **The bounded `named-let` lowers to real blocks with a back-edge** (header / body / exit), parameters become block params (phi-equivalent), trip bound is an attribute on the loop header. | A backend emits a counted loop directly. An unroller is a pass over the same blocks. The cost model reads the header attribute; it never re-derives structure from an AST. |

Everything else is mechanism in service of these five.

---

## 1. Library placement, build, and file layout

A sibling library `libs/lisp_shader/` depending on `libs/lisp`, mirroring the
existing lib exactly: freestanding C, host-buildable, host-tested by a
`build-and-run.sh` that clones `libs/lisp/test/build-and-run.sh`.

```
libs/lisp_shader/
  CMakeLists.txt              # mirrors libs/lisp/CMakeLists.txt; links lisp
  inc/
    lisp_shader.h             # THE shared public header (the contract all 3 agents code to)
    shader_ir.h               # IR + type structs (shared, internal-but-cross-unit)
  src/
    types.c          # shader_type encoding, equality, display
    frontend.c       # s-expr (from lisp_read) -> untyped AST -> typed IR builder
    verify.c         # the trusted moat: types, bounds, bounded-loops, caps
    ir.c             # IR construction helpers, block/vreg allocators, IR printer
    interp.c         # reference interpreter (scalar-lowered, the oracle)
    vecops.c         # the scalar lane-loop definitions for every abstract vector op
    prims.c          # capability registry: host registers allowed primitives
    api.c            # the public compile/invoke/error-reporting entry points
  test/
    build-and-run.sh          # clone of libs/lisp/test/build-and-run.sh, adds -I../../lisp/inc
    test_types.c
    test_frontend.c
    test_verify.c
    test_interp_scalar.c
    test_interp_vector.c
    test_api.c
    test_differential.c       # shader result == equivalent dynamic-lisp result
```

### `CMakeLists.txt` (sketch, mirrors `libs/lisp`)

```cmake
cmake_minimum_required(VERSION 3.20)
FILE(GLOB SHADER_SRCS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.c)
ADD_LIBRARY(lisp_shader STATIC ${SHADER_SRCS})
TARGET_INCLUDE_DIRECTORIES(lisp_shader PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/inc")
TARGET_LINK_LIBRARIES(lisp_shader PUBLIC lisp)
TARGET_INCLUDE_DIRECTORIES(lisp_shader SYSTEM PUBLIC "${KERN_STDLIB_INCLUDE_DIR}")
# Same SSE caveat as lisp/: the reference interpreter is SCALAR (no packed ops),
# so flonum/double math is allowed but auto-vectorization is OFF -- the actual
# SIMD lives only in a future backend TU, never here.
TARGET_COMPILE_OPTIONS(lisp_shader PRIVATE -fno-pic -msse -msse2 -mstackrealign
                                           -fno-vectorize -fno-slp-vectorize)
```

### `test/build-and-run.sh` (sketch)

```bash
#!/usr/bin/env bash
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
SH="$HERE/.."           # libs/lisp_shader
LISP="$SH/../lisp"      # libs/lisp
TMP="$(mktemp -d)"; status=0
for t in "$HERE"/test_*.c; do
  name="$(basename "$t" .c)"
  clang -std=c11 -Wall -Wextra -Werror -g \
    -I"$SH/inc" -I"$LISP/inc" \
    "$SH"/src/*.c "$LISP"/src/*.c "$t" -o "$TMP/$name"
  "$TMP/$name" "$HERE" || status=1; echo
done
exit "$status"
```

The shader lib reuses `libs/lisp`'s reader (`lisp_read`), interned symbols
(`lisp_make_symbol`, eq?-comparable), `lisp_bytes` for regions at the boundary,
and `lisp_print` for diagnostics. It adds no new dependency.

---

## 2. The type encoding (`shader_type`) — `types.c` / `shader_ir.h`

A shader type is a small POD value, passed by value, comparable by `==` on its
two words. No allocation, no interning needed (it is 8 bytes).

```c
// Scalar lane kinds. Order matters: width derivable, integer-ness derivable.
typedef enum {
    SK_BOOL = 0,   // 1 logical bit, stored in a byte/slot
    SK_U8, SK_U16, SK_U32, SK_U64,
    SK_I8, SK_I16, SK_I32, SK_I64,
    SK_F32, SK_F64,
    SK_KIND_COUNT
} shader_scalar_kind;

// Top-level type category.
typedef enum {
    TC_VOID = 0,   // statement / no value (e.g. a store)
    TC_SCALAR,     // one lane
    TC_VECTOR,     // fixed-width <lanes x kind>
    TC_REGION,     // a (bytes T) view: element kind + provenance, length is dynamic
} shader_type_class;

// 8-byte POD. Compared with shader_type_eq (a memcmp / two-word equality).
typedef struct {
    uint8_t  cls;      // shader_type_class
    uint8_t  kind;     // shader_scalar_kind  (the lane/element/scalar kind)
    uint8_t  lanes;    // TC_VECTOR: lane count (>=1). else 1.
    uint8_t  flags;    // reserved (e.g. CONST/uniform marker for a later phase)
    uint32_t _pad;     // reserved; keeps the struct exactly 8 bytes & 0 today
} shader_type;
```

Constructors / queries (all `static inline` or trivial, in `shader_ir.h`):

```c
static inline shader_type ty_scalar(shader_scalar_kind k);
static inline shader_type ty_vector(shader_scalar_kind k, uint8_t lanes);
static inline shader_type ty_region(shader_scalar_kind elem);  // (bytes elem)
static inline shader_type ty_void(void);
static inline bool        shader_type_eq(shader_type a, shader_type b);

size_t  sk_size(shader_scalar_kind k);     // bytes of one lane: 1,2,4,8
bool    sk_is_int(shader_scalar_kind k);
bool    sk_is_float(shader_scalar_kind k);
bool    sk_is_signed(shader_scalar_kind k);
// Human form: "u32", "f32x4", "(bytes u16)", "void". For diagnostics + the
// (future) derived doc signature.
size_t  shader_type_print(shader_type t, char *buf, size_t cap);
```

Key choices for the stance:

- **Region length is *not* in the type.** A `TC_REGION` carries only its element
  kind. The *length* is a runtime SSA value tracked alongside the region value
  (see §5) — that is the provenance the verifier and a future backend reason
  about. Putting length in the type would force monomorphization per-length;
  tracking it as an SSA value is exactly what lets a backend do range analysis.
- `vec2/vec3/vec4` are pure frontend sugar over `ty_vector(SK_F32, N)`; the type
  table never distinguishes them (the note's "unified `<N×T>`").
- `flags`/`_pad` are reserved-zero today. They cost nothing and give the
  backend phase a place to hang a `uniform`/`const`/alignment-known marker
  without re-encoding every type — a forward-compat hedge that is literally free.

---

## 3. The IR (`shader_ir.h` + `ir.c`) — typed mini-SSA over basic blocks

This is **D1**, the central decision. The IR is the persistent compiled artifact
(the bytecode the note calls an "internal cache"). It is a flat, index-based
structure — no pointers between nodes, only integer indices — so it is trivially
relocatable, serializable, and cache-friendly, and a backend indexes into the
same arrays.

```c
typedef uint32_t vreg_id;     // SSA value id; index into the value/type tables
typedef uint32_t block_id;    // basic block index
typedef uint32_t inst_id;     // instruction index (global, dense)

#define VREG_NONE  ((vreg_id)0xFFFFFFFFu)
#define BLOCK_NONE ((block_id)0xFFFFFFFFu)
```

### 3.1 Instruction

Each instruction defines at most one `vreg` (its result), reads up to a fixed
small number of operand `vreg`s, and carries one opcode plus an immediate union.
Three-address means: no nested expressions, every input is a named prior `vreg`.

```c
#define SHADER_MAX_OPERANDS 4   // covers select(mask,a,b), fma(a,b,c), call uses an arg list

typedef struct {
    uint16_t   op;          // shader_op
    uint16_t   _pad;
    shader_type type;       // type of the RESULT (8 bytes, the op's static type)
    vreg_id    result;      // VREG_NONE for void ops (store, bounds_check, terminators)
    vreg_id    in[SHADER_MAX_OPERANDS];
    // Immediate payload, interpreted per-op (see the op table in section 7):
    union {
        int64_t   i64;      // integer literal / const-index for shuffle/extract
        double    f64;      // float literal
        uint32_t  u32[2];   // {lane_kind, lane_count} echo, or shuffle indices, etc.
        struct { block_id t, f; } br;     // CBR targets
        block_id  jmp;                    // BR target
        struct { uint32_t prim; uint32_t argc; } call;  // index into the prim table
    } imm;
    uint32_t   arg_off;     // CALL/LOOP back-edge: offset into the shader's extra-args pool
} shader_inst;
```

Rationale for the stance: keeping the result `type` *on every instruction* (not
only on `vreg` definitions) is the redundancy a backend wants — it pattern-matches
on `(op, type.kind, type.lanes)` in one read, never chasing a separate type
table. The cost is 8 bytes/inst; trivial. Operands are a fixed array (not a
side list) so the backend's use-def walk is index arithmetic.

### 3.2 Basic block

A block is a half-open range of instructions ending in exactly one terminator,
plus its *block parameters* — the SSA story for loop-carried values and the
`if`/`cond` join. We use **block parameters (Cranelift/MLIR style) instead of
classic phi nodes**: a branch supplies argument `vreg`s, the target block names
them as parameters. This is strictly nicer to lower than phis (no
"phi at top, but evaluated on the edge" hazard) and is exactly how the
`named-let` loop-carried variables and the `if` join are represented.

```c
typedef struct {
    inst_id   first;        // first instruction index
    inst_id   last;         // index of the terminator (inclusive)
    uint32_t  nparams;      // block parameters (loop-carried / join values)
    uint32_t  param_off;    // offset into shader->block_params (vreg_id + type pairs)
    // Loop header annotation (BLOCK_NONE/0 when this block is not a loop header):
    uint8_t   is_loop_header;
    uint8_t   _pad[3];
    cost_bound bound;       // worst-case trip bound for the loop this header opens
} shader_block;
```

### 3.3 The compiled shader

```c
typedef struct {
    char       name[48];           // shader name (interned symbol's text, truncated)

    // Signature (the W7 capability list; see section 5/6):
    uint32_t   nparams;            // shader parameter count
    shader_type params[16];        // parameter types, in order
    shader_type ret;               // return type

    // SSA value space: type per vreg (result types are also on the inst, this is
    // the canonical table the interpreter indexes for its slot file).
    uint32_t   nvregs;
    shader_type *vreg_types;       // [nvregs]

    // Instructions (dense) and blocks:
    uint32_t   ninsts;  shader_inst  *insts;     // [ninsts]
    uint32_t   nblocks; shader_block *blocks;    // [nblocks]; block 0 is entry

    // Side pools (variable-length operand lists for call/loop-arg/block-params):
    uint32_t   nextra;  vreg_id      *extra_args;     // CALL args, loop back-edge args
    uint32_t   nbparams; struct { vreg_id v; shader_type t; } *block_params;

    // Capability binding resolved at compile time (section 5):
    uint32_t   nprims;  const shader_prim *prims;     // the allowed-prim table used

    // Worst-case cost of the whole shader (sum/product of block costs); section 3.4
    cost_bound total_cost;
} shader;
```

All arrays are owned by one bump arena (`shader_arena`) so freeing a compiled
shader is one `free`; an in-OS port swaps the arena for the kernel allocator with
no structural change. Index-based, contiguous, single-owner: this is the shape a
JIT serializes/maps with zero pointer-fixup.

### 3.4 Cost bound

```c
typedef enum { COST_CONST, COST_LINEAR_IN_PARAM, COST_UNBOUNDED } cost_class;
typedef struct {
    cost_class cls;
    uint64_t   konst;      // COST_CONST: the constant; COST_LINEAR: the coefficient
    uint32_t   param;      // COST_LINEAR_IN_PARAM: which shader param indexes the bound
} cost_bound;
```

The verifier computes a `cost_bound` per loop header and folds them into
`shader.total_cost`. `COST_UNBOUNDED` is a verification failure (the loop has no
derivable bound). The two-regime policy from the note (ISR needs `COST_CONST`;
cooperative tolerates `COST_LINEAR_IN_PARAM`) is a *consumer* check the
invoke-site/scheduler makes against `total_cost` — the verifier records the bound;
the policy is applied where the shader is used. (For S0–S2 host tests we just
assert the recorded class.)

### 3.5 How the interpreter executes it (the value model — D2)

```c
// A runtime value: a raw 64-bit slot, OR an inline lane array for a vector. The
// static type (shader->vreg_types[id]) says which union arm and how to read it.
typedef struct {
    union {
        uint64_t bits;            // scalar: holds u8..u64/i64 (zero/sign-extended),
                                  //   f32 in low 32 bits, f64 as bit pattern, bool 0/1
        uint64_t lane[16];        // vector: up to 16 lanes, each a scalar slot
        struct { void *base; uint64_t len; } region;  // region: pointer + length
    } u;
} shader_val;
```

The interpreter is a flat **slot file** `shader_val slots[nvregs]`, indexed by
`vreg_id`. Execution is a block walk:

1. Bind shader params into the slots of the entry block's parameters.
2. For the current block, execute instructions `[first, last)` straight-line
   (three-address: read `in[]` slots, write `result` slot).
3. Execute the terminator (`last`): `RET` ends; `BR`/`CBR` copies the branch's
   argument `vreg`s into the target block's parameter slots, then jumps; the loop
   back-edge is just a `BR` to the loop header with the next iteration's values.

Because values are unboxed and monomorphic, no GC runs, nothing allocates — which
is the entire safety claim for ISR/`cli()` context. The slot file is stack/arena
memory sized at compile time (`nvregs * sizeof(shader_val)`).

**The forward-compat win:** a backend assigns each `vreg_id` to a hardware
register or spill slot. The interpreter's `slots[id]` and the backend's
`regalloc(id)` are the same index space; the block walk becomes a code-emission
walk; the terminator copies become `mov`/`jmp`. Nothing in the IR mentions x86,
so layers 1–3 stay portable (the note's quarantine).

---

## 4. Region bounds checking (D4)

A region value carries `{base, len}` at runtime (the `region` arm above). The
frontend lowers `(u16-ref buf i)` into **two** IR instructions:

```
%c  = bounds_check buf, i        ; void op; traps if i >= len(buf). Carries the
                                 ; region vreg + the index vreg; reads len at run.
%v  = load.u16 buf, i            ; raw element load, no check
```

and `(u16-set! buf i x)` into `bounds_check` + `store.u16`.

- **Reference interpreter:** `bounds_check` reads `slots[buf].u.region.len`,
  compares the index, and on `idx >= len` aborts the invocation with a structured
  `SH_ERR_BOUNDS` (no UB, no OOB — this op *is* the absent MMU). The `load`/`store`
  then trusts the prior check.
- **Static info the IR carries for a future backend:** the `bounds_check` is a
  *distinct node* with explicit operands, so a range-analysis pass can prove
  `idx < len` (e.g. the loop induction var `i` is bounded by `n` and `len(buf) >=
  n` from a guard) and **delete the `bounds_check` node**, leaving the bare
  `load`. Elision is a node deletion, not a load rewrite. This is the single most
  valuable region optimization and D4 makes it a peephole. The note's "(bytes
  u16) with an SSA-tracked length, every access a single statically-elided-where-
  provable load/store" is *exactly* this representation.

A region's `len` enters the IR as a first-class `vreg` via a `REGION_LEN` op
(`%n = region_len buf`), so length participates in SSA range analysis like any
other integer. That is the "SSA-tracked length" the note names.

---

## 5. Capability whitelist (W7 — `prims.c`)

A shader may call only primitives the host explicitly hands the compiler, and may
name only those. Zero ambient authority: there is no global table the frontend
consults. The allowed-prim set is an *argument to compile* (§6).

```c
// A primitive a shader is permitted to call: a closed typed signature + a C impl
// the reference interpreter calls. The signature IS the capability.
typedef shader_val (*shader_prim_fn)(const shader_val *args, uint32_t argc);

typedef struct {
    const char  *name;          // the symbol a shader writes to call it
    shader_type  ret;
    uint8_t      nargs;
    shader_type  args[7];
    shader_prim_fn fn;          // NULL => intrinsic lowered to an op, not a call
} shader_prim;

// A frozen set the host assembles and passes to compile. Resolution is by name
// against THIS set only -- a name not in the set is a compile error
// (SH_ERR_UNKNOWN_PRIM), never a silent ambient binding.
typedef struct {
    const shader_prim *prims;
    uint32_t           count;
} shader_prim_set;
```

The frontend resolves every call-position symbol against the passed
`shader_prim_set`; the verifier re-checks that every `CALL` op's `imm.call.prim`
indexes a member of the set and that argument types match the prim's signature
exactly (monomorphic, no coercion). Because the parameter list and the prim set
are the only names a shader can reach, **the signature is the capability list**,
exactly as the note states. Built-in arithmetic/region/vector ops are *not*
prims — they are IR opcodes (D3/D4), so the prim set governs only host-exposed
authority (e.g. a future `mmio-read32`), keeping the trusted surface tiny.

The interpreter, on a `CALL`, gathers the arg slots from `extra_args[arg_off ..]`
and invokes `prim->fn`. (S0–S2 ship a tiny demo set; the real authority-bearing
prims arrive with S4.)

---

## 6. Public C API (`lisp_shader.h` + `api.c`)

```c
// ---- Error reporting (structured; no errno, no longjmp) --------------------
typedef enum {
    SH_OK = 0,
    SH_ERR_SYNTAX,        // not a well-formed (defshader ...)
    SH_ERR_TYPE,          // type mismatch / bad annotation
    SH_ERR_UNKNOWN_PRIM,  // call to a symbol not in the allowed set
    SH_ERR_BANNED_FORM,   // lambda/define/cons/recursion/unbounded loop/...
    SH_ERR_UNBOUNDED,     // a named-let with no derivable trip bound
    SH_ERR_BOUNDS,        // (invoke) region access out of range
    SH_ERR_ARITY,         // (invoke) wrong arg count/type
    SH_ERR_INTERNAL,
} shader_status;

typedef struct {
    shader_status status;
    char          msg[160];   // human-readable; includes the offending form printed
    int           line, col;  // source location when known (via lisp_source_location)
} shader_error;

// ---- Compile: s-expr + allowed-prim set -> compiled handle or error --------
// `form` is the (defshader ...) datum from lisp_read. `prims` is the capability
// set. On success returns a non-NULL `*out` (owns one arena; free with
// shader_free) and SH_OK. On failure returns the error and leaves *out NULL.
shader_status shader_compile(lisp_value form,
                             const shader_prim_set *prims,
                             shader **out,
                             shader_error *err);

// Convenience: read the first datum from `src` then compile it.
shader_status shader_compile_str(const char *src, size_t len,
                                 const shader_prim_set *prims,
                                 shader **out, shader_error *err);

void shader_free(shader *s);

// ---- Introspection (the derived contract; feeds S5 docs later) -------------
uint32_t    shader_param_count(const shader *s);
shader_type shader_param_type(const shader *s, uint32_t i);
shader_type shader_ret_type(const shader *s);
cost_bound  shader_cost(const shader *s);
size_t      shader_signature_print(const shader *s, char *buf, size_t cap);

// ---- Invoke: typed args -> result ------------------------------------------
// Args are a typed array the caller fills; arg.type MUST equal the shader's
// declared param type (checked -> SH_ERR_ARITY/SH_ERR_TYPE). Region args carry
// {base,len}; the interpreter bounds-checks every access against that len.
// `*result` receives the return value (its type is shader_ret_type). Returns
// SH_OK or a runtime error (SH_ERR_BOUNDS/SH_ERR_ARITY) with `err` filled.
typedef struct { shader_type type; shader_val val; } shader_arg;

shader_status shader_invoke(const shader *s,
                            const shader_arg *args, uint32_t argc,
                            shader_val *result,
                            shader_error *err);

// Helpers to build region args from a libs/lisp lisp_bytes at the boundary:
shader_arg shader_arg_region(shader_scalar_kind elem, void *base, uint64_t nbytes);
shader_arg shader_arg_u64(shader_scalar_kind k, uint64_t bits);
shader_arg shader_arg_f64(double x);
```

`shader_compile` is the only entry that builds IR; `shader_invoke` only runs the
verified IR. The verifier runs *inside* `shader_compile`, before it returns a
handle (the note's "verify before run, always") — there is no way to obtain a
`shader*` that was not verified.

---

## 7. The opcode set (`shader_op`) — `shader_ir.h`

Each op's result `type` (on the inst) carries the kind/lane info a backend needs;
the op identifies the *operation*, the type identifies the *width/lanes*. So
`OP_ADD` with `type = f32x4` is the vector add a backend turns into `VADDPS`, and
`OP_ADD` with `type = u32` is a scalar add. **One op family, type-parameterized** —
this is the LLVM model and the reason the op set stays tiny.

```c
typedef enum {
    // --- constants / moves ---
    OP_CONST,         // imm.i64/f64 -> result (type says how to read imm)
    OP_PARAM,         // materialize shader param #imm.i64 as a vreg (entry only)

    // --- scalar & lane-wise arithmetic (type = scalar OR vector) ---
    OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_REM,   // DIV/REM integer or float per kind
    OP_NEG, OP_ABS, OP_MIN, OP_MAX,
    OP_AND, OP_OR, OP_XOR, OP_NOT, OP_SHL, OP_SHR,  // integer bitwise/shift
    OP_CMP,           // imm.i64 = a compare predicate (EQ/NE/LT/LE/GT/GE);
                      //   result type bool (scalar) or boolNxK mask (vector)

    // --- conversions (monomorphic, explicit; no implicit coercion) ---
    OP_CONVERT,       // result type = dst; in[0] = src. int<->int (trunc/ext),
                      //   int<->float, f32<->f64. Backend = one cvt instruction.

    // --- regions (D4) ---
    OP_REGION_LEN,    // %n = region_len buf        (len -> a u64 vreg, for SSA range)
    OP_BOUNDS_CHECK,  // void; in[0]=region in[1]=index; traps if index>=len
    OP_LOAD,          // %v = load buf, index       (type = element scalar/vector)
    OP_STORE,         // void; in[0]=buf in[1]=index in[2]=value

    // --- abstract vector ops (D3; every one has a scalar lane-loop in vecops.c) -
    OP_SPLAT,         // scalar in[0] -> all lanes of result vector
    OP_EXTRACT,       // imm.i64 = lane index; vector in[0] -> scalar result
    OP_INSERT,        // imm.i64 = lane; (vector,scalar) -> vector
    OP_SHUFFLE,       // imm.u32[]=const lane indices (in arg pool); -> permuted vector
    OP_REDUCE,        // imm.i64 = reduction (ADD/MIN/MAX); vector -> scalar
    OP_DOT,           // (vector,vector) -> scalar  (sum of lane products)
    OP_SELECT,        // (mask, a, b) lane-wise blend -> vector (or scalar if mask scalar)

    // --- control flow / terminators (D5) ---
    OP_BR,            // unconditional; imm.jmp target; args via arg pool -> target params
    OP_CBR,           // in[0]=bool cond; imm.br.{t,f}; per-edge args via arg pool
    OP_RET,           // in[0] = return value (or none for void)

    // --- call (D-cap) ---
    OP_CALL,          // imm.call.{prim,argc}; args via extra_args[arg_off..]; result typed

    OP_COUNT
} shader_op;
```

Note there is **no opcode for the loop**. The bounded `named-let` is *structure*
(blocks + a back-edge `OP_BR` to a loop-header block), not an instruction (D5).
This is precisely what lets a backend emit a counted loop and an unroller run as a
block pass.

### How an abstract vector op carries backend info

`OP_ADD` / `type={cls:VECTOR, kind:F32, lanes:4}` is everything a pattern matcher
needs: "lane-wise add, f32, 4 lanes" → `VADDPS xmm,xmm`. `OP_SHUFFLE` with its
constant index array → `PSHUFB`/`SHUFPS` with the indices baked into the imm8.
`OP_REDUCE ADD f32x4` → a `haddps`/shuffle tree. No op names an instruction; the
backend owns that mapping (layer 4). For S2 the same ops run scalar (next
section).

---

## 8. Vector scalar-lowering (the mandatory oracle — `vecops.c`)

**Invariant (from the note): every abstract vector op has a scalar lane-loop, and
S0–S2 execute *only* that.** SIMD is perf, never semantics. `vecops.c` is a table
of lane-loop definitions the interpreter dispatches to; a vector value is the
`lane[16]` arm of `shader_val`, each lane a scalar slot.

Sketch of the lowerings (each is total and obvious):

```c
// OP_ADD, vector f32x4 -> lane loop:
for (i = 0; i < lanes; i++)
    r.u.lane[i] = f32_bits( f32(a.u.lane[i]) + f32(b.u.lane[i]) );

// OP_SPLAT: for i in lanes: r.lane[i] = scalar
// OP_EXTRACT: r.bits = v.lane[imm]
// OP_SHUFFLE: for i in lanes: r.lane[i] = v.lane[idx[i]]   (idx const, verified < lanes)
// OP_REDUCE ADD: acc = lane[0]; for i in 1..lanes: acc += lane[i]; r.bits = acc
// OP_DOT: acc = 0; for i: acc += a.lane[i]*b.lane[i]; r.bits = acc
// OP_CMP -> mask: for i: r.lane[i] = predicate(a.lane[i],b.lane[i]) ? all-ones : 0
// OP_SELECT: for i: r.lane[i] = mask.lane[i] ? a.lane[i] : b.lane[i]
```

Per-lane scalar arithmetic reuses the *same* scalar helpers the scalar ops use
(`sk_add(kind,a,b)`, etc. in `interp.c`), so a scalar `OP_ADD u32` and one lane of
a vector `OP_ADD u32x4` are bit-identical by construction. That guarantees the
differential test `scalar-interp == vector-interp-per-lane`, and later
`reference == SSE backend`, holds for free.

**The later SSE lowering, same op:** in S3, a backend visits `OP_ADD` with
`type=f32x4` and emits one `VADDPS` over the two xmm regs holding `in[0]`/`in[1]`,
writing `result`'s xmm. The IR node is unchanged; only layer 4 differs. The
differential harness then asserts the SSE result equals this `vecops.c` lane-loop
bit-for-bit — the note's free oracle. **Nothing in `vecops.c` or any of
layers 1–3 ever mentions `xmm` or `VADDPS`.**

---

## 9. Verification — `verify.c` (the trusted moat, small + total)

Runs inside `shader_compile`, on the abstract IR, after the frontend builds it.
It is the security boundary (stands in for the absent MMU), so it is total
(terminates on every input) and conservative (rejects anything it cannot prove).
Six passes over the flat IR:

1. **Well-formedness:** every `vreg` is defined before use (SSA dominance: with a
   single back-edge per loop header this is a linear scan + a dominator check that
   is cheap because the CFG is reducible by construction — the frontend only emits
   reducible CFGs from the structured forms); every block ends in exactly one
   terminator; branch targets in range.
2. **Type check:** each op's operand types match its rule and the result type on
   the inst is the rule's output (e.g. `OP_ADD` requires both inputs == result
   type; `OP_LOAD` result kind == region elem kind; `OP_CALL` args == prim sig).
   Monomorphic, no coercion — a mismatch is `SH_ERR_TYPE`.
3. **Capability check:** every `OP_CALL.prim` indexes the passed `shader_prim_set`
   (`SH_ERR_UNKNOWN_PRIM` otherwise). No other op reaches outside the shader.
4. **Banned-form residue:** the frontend already rejects `lambda`/`define`/`cons`/
   recursion/etc.; the verifier asserts no op that could only come from a banned
   form survived (defense in depth — e.g. no allocation op exists in the set at
   all, which is the structural guarantee).
5. **Bounded-loop analysis:** for each loop-header block, derive the trip
   `cost_bound` from the induction variable and the exit test (constant bound →
   `COST_CONST`; bound is a shader param → `COST_LINEAR_IN_PARAM`; otherwise
   `COST_UNBOUNDED` → `SH_ERR_UNBOUNDED`). Fold into `shader.total_cost`.
6. **Shuffle/extract index check:** constant lane indices on `OP_SHUFFLE`/
   `OP_EXTRACT`/`OP_INSERT` are `< lanes` (so the lane-loop and any backend are
   in-bounds by construction; no runtime check needed for these).

Region *element* bounds are the only check left to runtime (`OP_BOUNDS_CHECK`,
§4), because the length is dynamic. Everything else is proven at compile time.

---

## 10. The frontend — `frontend.c`

Consumes the s-expr from `lisp_read`. Pipeline:

1. **Parse `(defshader NAME ((p T) ...) -> RET body)`** into a small untyped AST
   (reusing `lisp_pair`/`lisp_symbol`; the AST is just validated s-expr structure
   — no new node types needed for the simple grammar). Parse type annotations
   (`u32`, `f32x4`, `(bytes u16)`) into `shader_type`.
2. **Reject banned forms eagerly** while walking the body (`lambda`, `define`,
   `cons`, `make-bytes`, `eval`, `import`, `spawn`, `send`, strings, variadic
   calls, any loop that is not a `named-let`, any self/mutual recursion) →
   `SH_ERR_BANNED_FORM` with the offending form printed into `err->msg` and its
   source location.
3. **Build typed IR** via the `ir.c` builder API, inferring local types bottom-up
   (operands' types determine the result; the annotated params/return are the
   fixed points). `if`/`cond` lower to `OP_CBR` + a join block with block params;
   `named-let` lowers to header/body/exit blocks with loop-carried block params and
   a back-edge `OP_BR` (D5); region accessors to `OP_BOUNDS_CHECK`+`OP_LOAD/STORE`;
   calls resolve against the prim set to `OP_CALL`.

`ir.c` exposes the builder the frontend drives (so the frontend never touches raw
arrays — clean unit boundary):

```c
typedef struct ir_builder ir_builder;
ir_builder *irb_new(const char *name, shader_arena *a);
block_id    irb_block(ir_builder *b);                       // new block
block_id    irb_loop_header(ir_builder *b, cost_bound bnd); // loop header block
vreg_id     irb_param(ir_builder *b, uint32_t idx, shader_type t);
vreg_id     irb_const_i(ir_builder *b, shader_type t, int64_t v);
vreg_id     irb_const_f(ir_builder *b, shader_type t, double v);
vreg_id     irb_binop(ir_builder *b, shader_op op, shader_type t, vreg_id x, vreg_id y);
vreg_id     irb_load(ir_builder *b, shader_type elem, vreg_id region, vreg_id idx);
void        irb_bounds_check(ir_builder *b, vreg_id region, vreg_id idx);
void        irb_store(ir_builder *b, vreg_id region, vreg_id idx, vreg_id val);
vreg_id     irb_vecop(ir_builder *b, shader_op op, shader_type t, const vreg_id *in, int n);
void        irb_br(ir_builder *b, block_id target, const vreg_id *args, int n);
void        irb_cbr(ir_builder *b, vreg_id cond, block_id t, const vreg_id *ta, int tn,
                                                 block_id f, const vreg_id *fa, int fn);
void        irb_ret(ir_builder *b, vreg_id v);
vreg_id     irb_call(ir_builder *b, uint32_t prim, shader_type ret, const vreg_id *args, int n);
shader     *irb_finish(ir_builder *b);   // seals arrays into a `shader`
```

---

## 11. The 3-unit split (DISJOINT file ownership)

All three agents code to the *shared headers* `inc/lisp_shader.h` and
`inc/shader_ir.h` (these two headers are written FIRST, jointly, and frozen
before parallel work — they are the only coupling). Then ownership is disjoint by
`.c` file:

| Unit | Owns (`.c`) | Owns (tests) | Responsibility |
|------|-------------|--------------|----------------|
| **Unit A — Types & IR core** | `types.c`, `ir.c` | `test_types.c` | The `shader_type` encoding + all its queries; the `ir.c` builder API + arena + IR printer + flat-array sealing (`irb_*`, `shader` assembly). Produces the substrate the other two consume. Owns `shader_ir.h` edits (coordinated). |
| **Unit B — Frontend & Verifier** | `frontend.c`, `verify.c` | `test_frontend.c`, `test_verify.c` | s-expr → typed IR via Unit A's `irb_*`; banned-form rejection; the six verifier passes incl. bounded-loop cost + region/shuffle static checks. The trusted moat. Consumes A; produces a verified `shader`. |
| **Unit C — Interpreter, Vecops, Prims, API** | `interp.c`, `vecops.c`, `prims.c`, `api.c` | `test_interp_scalar.c`, `test_interp_vector.c`, `test_api.c`, `test_differential.c` | The reference interpreter (block walk + slot file), the mandatory scalar lane-loops, the capability/prim registry, and the public `shader_compile`/`shader_invoke`/error API (calls into B for compile+verify, runs A's IR). The semantic oracle. |

Conflict surface is minimal: A defines structs + builder, B calls the builder, C
reads the sealed `shader`. The only shared mutable header is `shader_ir.h` (owned
by A); B and C only *read* it. The public `lisp_shader.h` is owned by C (the API)
but its types (`shader`, `shader_type`, `shader_prim*`) live in `shader_ir.h`. A
suggested order: A lands the headers + `ir.c`/`types.c` first (unblocks both); B
and C then proceed fully in parallel against the frozen builder and `shader`
struct. C's `test_differential.c` depends on B being able to compile, so it lands
last — but C's interpreter unit-tests can run against hand-built IR (via A's
builder) before B's frontend exists.

---

## 12. Where I push back on the note / the minimalist stance

Being honest, per the brief:

1. **Block-params/SSA is more than S0–S2 strictly need.** A minimalist S0 could
   keep a tree-walking typed AST interpreter and skip blocks entirely — for the
   *scalar reference interpreter alone*, an AST is less code today. I am
   deliberately paying that cost now because the note's own warning ("the IR shape
   is the thing painful to retrofit") makes the AST a trap: an S3 backend over an
   AST means inventing the SSA/CFG lowering *then*, under backend pressure, with no
   differential oracle stable across the change. I'd rather eat ~300–500 lines of
   `ir.c`/`verify.c` SSA bookkeeping now than rewrite the executor at S3. **This is
   the one place I most expect the minimalist proposal to disagree** — and the
   disagreement is the whole point of my assigned stance. If the team wants to
   defer the backend indefinitely, the minimalist AST wins; if S3 is real, mini-SSA
   pays for itself.

2. **Two-op region access (`BOUNDS_CHECK` + `LOAD`) vs. a fused checked-load.**
   Minimalism says one `CHECKED_LOAD` op is simpler and the interpreter does the
   check inline. I split them because *check elision is the highest-value backend
   optimization for the driver hot-loop use case*, and a fused op forces the
   backend to pattern-match-and-split before it can elide, whereas a separate node
   is a one-line deletion. The cost is two IR nodes per access and a verifier rule
   that a `LOAD` is dominated by a matching `BOUNDS_CHECK`. I think it's worth it;
   a reasonable reviewer could call it premature. **Second likely disagreement.**

3. **Result type stored on every instruction (8 bytes/inst) is redundant** with
   `vreg_types[]`. Minimalism would store it once per `vreg`. I duplicate it
   because the backend's pattern matcher reads `(op,type)` in one cache line and
   never indirects — but for the *interpreter alone* it's pure redundancy. Cheap,
   but genuinely unnecessary until S3. **Third, smaller disagreement.**

4. **Mild pushback on the note itself:** the note says bytecode is "an internal
   cache, not a distribution artifact," which is right — but it should not be read
   as license to make the IR *un*-serializable. My index-based, pointer-free,
   single-arena layout costs nothing today and keeps the door open to mapping a
   compiled shader from a cache file later without a redesign. I'm treating
   "internal cache" as "not a *stable* format," not "ephemeral in-RAM only."

### Honest complexity cost added today

- ~300–500 extra LOC in `ir.c`/`verify.c` for blocks, block-params, the SSA
  define-before-use scan, and the reducible-CFG/dominance check — none of which a
  scalar AST interpreter would need.
- The frontend must *lower* structured forms (`if`/`cond`/`named-let`) to blocks
  rather than interpret them directly — more frontend code than a tree-walker.
- A second IR node per region access, and the verifier rule binding them.
- The redundant per-inst type word.

In exchange, S3 is a **new file (`backend_x86.c`) that visits the existing IR**,
not a rewrite of the executor; the differential oracle (`scalar == backend`) is
stable across that addition; and the in-OS port is an arena swap. That trade is
the entire forward-compatible bet, and it is the bet my assigned stance is meant
to make.
