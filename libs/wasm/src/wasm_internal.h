// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

// Internal contract for the Cardinal; Wasm interpreter. This header is the
// FROZEN interface between the three implementation files:
//
//   wasm_module.c   -- wasm_module_decode(): bytes -> wasm_module_t (sections)
//   wasm_exec.c     -- control-flow prepass + the stackless dispatch loop
//   wasm_instance.c -- instantiate/resume spine, linear memory, import dispatch,
//                      LEB helpers, value-stack helpers (the SHARED utilities)
//
// Decode produces the immutable module (raw function bodies as code+locals).
// Exec consumes the module and drives behaviour, owning its own control-flow
// analysis (stored via wasm_func_t.cf). Keep this header authoritative: add
// fields here, not in the .c files.

#ifndef CARDINAL_WASM_INTERNAL_H
#define CARDINAL_WASM_INTERNAL_H

#include "wasm.h"
#include <stdbool.h>

// ---- Capacity limits (allocated per instance; overflow -> trap) ---------
#define WASM_VSTACK_CAP   (64 * 1024)   // value slots (locals + operands)
#define WASM_CTRL_CAP     4096          // active control labels
#define WASM_FRAME_CAP    1024          // call depth
#define WASM_PAGE_SIZE    (64 * 1024)   // Wasm linear-memory page
// Cap on linear memory RESERVED per instance (base-never-moves => grow fits in
// the reserve). An unbounded memory decodes to a 4 GiB max; this bounds the
// in-kernel calloc. 1024 pages = 64 MiB (enough for a Doom-class guest).
#define WASM_MAX_RESERVE_PAGES 1024

// ---- Section / extern kinds --------------------------------------------
typedef enum {
    WASM_EXTERN_FUNC = 0,
    WASM_EXTERN_TABLE = 1,
    WASM_EXTERN_MEM = 2,
    WASM_EXTERN_GLOBAL = 3,
} wasm_externkind_t;

// ---- Decoded module pieces ---------------------------------------------

typedef struct {
    wasm_valtype_t *params;
    uint32_t n_params;
    wasm_valtype_t *results;
    uint32_t n_results;
} wasm_functype_t;

typedef struct {
    char *module_name;
    char *field_name;
    wasm_externkind_t kind;
    uint32_t type_index;   // for WASM_EXTERN_FUNC: index into module->types
    // (table/mem/global import descriptors omitted in v1: Doom imports funcs)
} wasm_importdecl_t;

typedef struct {
    uint32_t type_index;       // signature, into module->types
    const uint8_t *code;       // body bytes (after the locals declaration)
    uint32_t code_len;
    uint32_t n_locals;         // total slots = params + declared locals
    uint32_t n_params;         // first n_params slots are the params
    wasm_valtype_t *local_types; // length n_locals (optional; for validation)
    void *cf;                  // control-flow side table, owned by wasm_exec.c
} wasm_func_t;

typedef struct {
    wasm_valtype_t type;
    bool mutable_;
    wasm_value_t init;         // constant init value (from a const-expr)
} wasm_global_t;

typedef struct {
    char *name;
    wasm_externkind_t kind;
    uint32_t index;            // into the relevant index space
} wasm_export_t;

typedef struct {
    uint32_t mem_offset;       // active-segment offset (const-expr result)
    const uint8_t *bytes;
    uint32_t len;
} wasm_data_seg_t;

typedef struct {
    uint32_t table_offset;     // active-segment offset
    uint32_t *func_indices;    // entries (function index space)
    uint32_t n;
} wasm_elem_seg_t;

struct wasm_module {
    uint8_t *image;            // owned copy of the module bytes (code points into it)
    size_t image_len;

    wasm_functype_t *types;     uint32_t n_types;
    wasm_importdecl_t *imports; uint32_t n_imports;
    uint32_t n_imported_funcs;  // function-index-space prefix (imported funcs)

    wasm_func_t *funcs;        uint32_t n_funcs;     // DEFINED functions only

    bool has_mem;
    uint32_t mem_min_pages, mem_max_pages;  // max defaults to a cap if unbounded

    bool has_table;
    uint32_t table_min, table_max;

    wasm_global_t *globals;    uint32_t n_globals;   // defined globals
    wasm_export_t *exports;    uint32_t n_exports;
    wasm_data_seg_t *data;     uint32_t n_data;
    wasm_elem_seg_t *elem;     uint32_t n_elem;

    bool has_start;
    uint32_t start_func;       // function index
};

// Map a function index (import-space) to: imported (idx < n_imported_funcs) or
// defined (module->funcs[idx - n_imported_funcs]). Helpers in wasm_instance.c.

// ---- Runtime instance ---------------------------------------------------

typedef struct {
    uint32_t func_index;       // function-index-space index of the running func
    const uint8_t *code;
    uint32_t code_len;
    uint32_t pc;               // byte offset into code
    uint32_t locals_base;      // vstack index of this frame's local 0
    uint32_t operand_base;     // vstack index where operands begin (= locals_base + n_locals)
    uint32_t ctrl_base;        // ctrl-stack index at frame entry
    uint32_t result_arity;
} wasm_frame_t;

// A resolved import: either a synchronous C handler or a host-serviced suspend.
typedef struct {
    wasm_host_fn fn;           // NULL => host-serviced (suspend)
    void *user;
    uint32_t host_id;
    const char *module_name;   // borrowed from the wasm_import_def_t
    const char *field_name;
    uint32_t type_index;       // signature, from the module import decl
} wasm_resolved_import_t;

struct wasm_instance {
    wasm_module_t *module;

    // resolved imports, parallel to module->imports (function imports only in v1)
    wasm_resolved_import_t *imports;
    uint32_t n_imports;

    // linear memory: reserved at max_pages so `base` never moves
    uint8_t *mem;
    size_t mem_size;           // current size in bytes (= cur_pages * PAGE)
    uint32_t cur_pages, max_pages;

    wasm_value_t *globals;     uint32_t n_globals;
    uint32_t *table;           uint32_t table_len;   // function indices (or ~0u)

    // execution state (stackless => resumable)
    wasm_value_t *vstack;      uint32_t vsp;          // value stack + pointer
    wasm_frame_t *frames;      uint32_t fsp;          // call frames + depth
    void *ctrl;                uint32_t csp;          // control stack (owned by exec; wasm_label_t[])

    int64_t fuel;

    wasm_run_status_t status;
    wasm_trap_t trap;

    // suspended-import bookkeeping
    wasm_pending_t pending;
    uint32_t pending_func_index;  // func-index of the suspended import (for its
                                  // signature: pending arg/result valtypes)
    wasm_value_t pending_args[16];
    wasm_value_t provided[16];
    uint32_t n_provided;
    bool has_provided;

    // results of a completed top-level call
    wasm_value_t results[16];
    uint32_t n_results;

    // pending top-level entry: wasm_call() stages this; wasm_exec_run() builds
    // frame 0 from it on the first slice.
    uint32_t entry_func;       // function-index-space index
    uint32_t entry_nargs;
    bool entry_pending;

    bool started;              // a wasm_call() has been set up
};

// `ctrl`/`csp` are owned by wasm_exec.c; instance.ctrl is allocated (by the
// instance spine) as wasm_label_t[WASM_CTRL_CAP] and managed by exec.

// Control label, used by wasm_exec.c.
typedef struct {
    uint8_t kind;              // 0=block 1=loop 2=if
    uint32_t target_pc;        // br target: loop=>header, block/if=>after end
    uint32_t else_pc;          // if-else target (0 if none)
    uint32_t end_pc;
    uint32_t arity;            // result values of the block
    uint32_t height;           // vsp at block entry (for unwinding operands)
} wasm_label_t;

// ---- Shared helpers (implemented in wasm_instance.c) --------------------

// LEB128 readers over a bounds-checked cursor. Advance *pc; return false on
// truncation/overflow.
bool wasm_read_u32(const uint8_t *buf, uint32_t len, uint32_t *pc, uint32_t *out);
bool wasm_read_u64(const uint8_t *buf, uint32_t len, uint32_t *pc, uint64_t *out);
bool wasm_read_s32(const uint8_t *buf, uint32_t len, uint32_t *pc, int32_t *out);
bool wasm_read_s64(const uint8_t *buf, uint32_t len, uint32_t *pc, int64_t *out);
bool wasm_read_byte(const uint8_t *buf, uint32_t len, uint32_t *pc, uint8_t *out);

// Value-stack helpers (trap on overflow/underflow by setting inst->trap).
void wasm_push(wasm_instance_t *inst, wasm_value_t v);
wasm_value_t wasm_pop(wasm_instance_t *inst);

// Bounds-checked linear-memory effective address. Returns NULL (and sets the
// OOB trap) if [addr, addr+size) is out of range.
uint8_t *wasm_mem_ea(wasm_instance_t *inst, uint32_t addr, uint32_t size);

// memory.grow by `delta` pages; returns previous size in pages, or 0xFFFFFFFF on
// failure (no allocation/realloc needed: memory is reserved at max).
uint32_t wasm_mem_grow(wasm_instance_t *inst, uint32_t delta);

// ---- Cross-file entry points -------------------------------------------

// Implemented in wasm_module.c:
wasm_module_t *wasm_decode_impl(const uint8_t *buf, size_t len, wasm_result_t *err);
void wasm_module_free_impl(wasm_module_t *m);   // frees what decode allocated

// Implemented in wasm_validate.c:
wasm_result_t wasm_validate_impl(wasm_module_t *m);   // type-checks the module

// Implemented in wasm_exec.c. Runs the current frames for up to `fuel`
// reductions, updating inst->status/trap/results. Called by wasm_resume().
void wasm_exec_run(wasm_instance_t *inst, int64_t fuel);

// Implemented in wasm_exec.c. Build the control-flow side table (wasm_func_t.cf)
// for one function; called lazily at first entry. Returns false on malformed
// control structure.
bool wasm_exec_prepare_func(wasm_module_t *m, wasm_func_t *f, wasm_result_t *err);

// Implemented in wasm_exec.c. Free the control-flow side table owned by exec
// (wasm_func_t.cf). Called by wasm_module_free_impl for each function.
void wasm_exec_free_func(wasm_func_t *f);

// ---- MVP opcodes --------------------------------------------------------
enum {
    OP_UNREACHABLE = 0x00, OP_NOP = 0x01,
    OP_BLOCK = 0x02, OP_LOOP = 0x03, OP_IF = 0x04, OP_ELSE = 0x05,
    OP_END = 0x0B, OP_BR = 0x0C, OP_BR_IF = 0x0D, OP_BR_TABLE = 0x0E,
    OP_RETURN = 0x0F, OP_CALL = 0x10, OP_CALL_INDIRECT = 0x11,

    OP_DROP = 0x1A, OP_SELECT = 0x1B,

    OP_LOCAL_GET = 0x20, OP_LOCAL_SET = 0x21, OP_LOCAL_TEE = 0x22,
    OP_GLOBAL_GET = 0x23, OP_GLOBAL_SET = 0x24,

    OP_I32_LOAD = 0x28, OP_I64_LOAD = 0x29, OP_F32_LOAD = 0x2A, OP_F64_LOAD = 0x2B,
    OP_I32_LOAD8_S = 0x2C, OP_I32_LOAD8_U = 0x2D, OP_I32_LOAD16_S = 0x2E, OP_I32_LOAD16_U = 0x2F,
    OP_I64_LOAD8_S = 0x30, OP_I64_LOAD8_U = 0x31, OP_I64_LOAD16_S = 0x32, OP_I64_LOAD16_U = 0x33,
    OP_I64_LOAD32_S = 0x34, OP_I64_LOAD32_U = 0x35,
    OP_I32_STORE = 0x36, OP_I64_STORE = 0x37, OP_F32_STORE = 0x38, OP_F64_STORE = 0x39,
    OP_I32_STORE8 = 0x3A, OP_I32_STORE16 = 0x3B,
    OP_I64_STORE8 = 0x3C, OP_I64_STORE16 = 0x3D, OP_I64_STORE32 = 0x3E,
    OP_MEMORY_SIZE = 0x3F, OP_MEMORY_GROW = 0x40,

    OP_I32_CONST = 0x41, OP_I64_CONST = 0x42, OP_F32_CONST = 0x43, OP_F64_CONST = 0x44,

    OP_I32_EQZ = 0x45, OP_I32_EQ = 0x46, OP_I32_NE = 0x47,
    OP_I32_LT_S = 0x48, OP_I32_LT_U = 0x49, OP_I32_GT_S = 0x4A, OP_I32_GT_U = 0x4B,
    OP_I32_LE_S = 0x4C, OP_I32_LE_U = 0x4D, OP_I32_GE_S = 0x4E, OP_I32_GE_U = 0x4F,
    OP_I64_EQZ = 0x50, OP_I64_EQ = 0x51, OP_I64_NE = 0x52,
    OP_I64_LT_S = 0x53, OP_I64_LT_U = 0x54, OP_I64_GT_S = 0x55, OP_I64_GT_U = 0x56,
    OP_I64_LE_S = 0x57, OP_I64_LE_U = 0x58, OP_I64_GE_S = 0x59, OP_I64_GE_U = 0x5A,
    OP_F32_EQ = 0x5B, OP_F32_NE = 0x5C, OP_F32_LT = 0x5D, OP_F32_GT = 0x5E, OP_F32_LE = 0x5F, OP_F32_GE = 0x60,
    OP_F64_EQ = 0x61, OP_F64_NE = 0x62, OP_F64_LT = 0x63, OP_F64_GT = 0x64, OP_F64_LE = 0x65, OP_F64_GE = 0x66,

    OP_I32_CLZ = 0x67, OP_I32_CTZ = 0x68, OP_I32_POPCNT = 0x69,
    OP_I32_ADD = 0x6A, OP_I32_SUB = 0x6B, OP_I32_MUL = 0x6C,
    OP_I32_DIV_S = 0x6D, OP_I32_DIV_U = 0x6E, OP_I32_REM_S = 0x6F, OP_I32_REM_U = 0x70,
    OP_I32_AND = 0x71, OP_I32_OR = 0x72, OP_I32_XOR = 0x73,
    OP_I32_SHL = 0x74, OP_I32_SHR_S = 0x75, OP_I32_SHR_U = 0x76, OP_I32_ROTL = 0x77, OP_I32_ROTR = 0x78,
    OP_I64_CLZ = 0x79, OP_I64_CTZ = 0x7A, OP_I64_POPCNT = 0x7B,
    OP_I64_ADD = 0x7C, OP_I64_SUB = 0x7D, OP_I64_MUL = 0x7E,
    OP_I64_DIV_S = 0x7F, OP_I64_DIV_U = 0x80, OP_I64_REM_S = 0x81, OP_I64_REM_U = 0x82,
    OP_I64_AND = 0x83, OP_I64_OR = 0x84, OP_I64_XOR = 0x85,
    OP_I64_SHL = 0x86, OP_I64_SHR_S = 0x87, OP_I64_SHR_U = 0x88, OP_I64_ROTL = 0x89, OP_I64_ROTR = 0x8A,

    OP_F32_ABS = 0x8B, OP_F32_NEG = 0x8C, OP_F32_CEIL = 0x8D, OP_F32_FLOOR = 0x8E,
    OP_F32_TRUNC = 0x8F, OP_F32_NEAREST = 0x90, OP_F32_SQRT = 0x91,
    OP_F32_ADD = 0x92, OP_F32_SUB = 0x93, OP_F32_MUL = 0x94, OP_F32_DIV = 0x95,
    OP_F32_MIN = 0x96, OP_F32_MAX = 0x97, OP_F32_COPYSIGN = 0x98,
    OP_F64_ABS = 0x99, OP_F64_NEG = 0x9A, OP_F64_CEIL = 0x9B, OP_F64_FLOOR = 0x9C,
    OP_F64_TRUNC = 0x9D, OP_F64_NEAREST = 0x9E, OP_F64_SQRT = 0x9F,
    OP_F64_ADD = 0xA0, OP_F64_SUB = 0xA1, OP_F64_MUL = 0xA2, OP_F64_DIV = 0xA3,
    OP_F64_MIN = 0xA4, OP_F64_MAX = 0xA5, OP_F64_COPYSIGN = 0xA6,

    OP_I32_WRAP_I64 = 0xA7,
    OP_I32_TRUNC_F32_S = 0xA8, OP_I32_TRUNC_F32_U = 0xA9, OP_I32_TRUNC_F64_S = 0xAA, OP_I32_TRUNC_F64_U = 0xAB,
    OP_I64_EXTEND_I32_S = 0xAC, OP_I64_EXTEND_I32_U = 0xAD,
    OP_I64_TRUNC_F32_S = 0xAE, OP_I64_TRUNC_F32_U = 0xAF, OP_I64_TRUNC_F64_S = 0xB0, OP_I64_TRUNC_F64_U = 0xB1,
    OP_F32_CONVERT_I32_S = 0xB2, OP_F32_CONVERT_I32_U = 0xB3, OP_F32_CONVERT_I64_S = 0xB4, OP_F32_CONVERT_I64_U = 0xB5,
    OP_F32_DEMOTE_F64 = 0xB6,
    OP_F64_CONVERT_I32_S = 0xB7, OP_F64_CONVERT_I32_U = 0xB8, OP_F64_CONVERT_I64_S = 0xB9, OP_F64_CONVERT_I64_U = 0xBA,
    OP_F64_PROMOTE_F32 = 0xBB,
    OP_I32_REINTERPRET_F32 = 0xBC, OP_I64_REINTERPRET_F64 = 0xBD,
    OP_F32_REINTERPRET_I32 = 0xBE, OP_F64_REINTERPRET_I64 = 0xBF,

    OP_I32_EXTEND8_S = 0xC0, OP_I32_EXTEND16_S = 0xC1,
    OP_I64_EXTEND8_S = 0xC2, OP_I64_EXTEND16_S = 0xC3, OP_I64_EXTEND32_S = 0xC4,
};

#endif // CARDINAL_WASM_INTERNAL_H
