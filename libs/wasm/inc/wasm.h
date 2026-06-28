// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

// Public C API for the Cardinal; WebAssembly guest interpreter.
//
// This library is intentionally Lisp-agnostic and host-compilable: it decodes a
// Wasm MVP module from a byte buffer, instantiates it, and runs it cooperatively
// with a fuel budget, suspending to the host on imported calls. The SysLisp
// `sys-wasm` glue layer (separate) adapts this to Lisp values; the differential
// host test harness drives this API directly.
//
// Design: notes/core/wasm-guests.md. The interpreter is stackless (operand and
// call state live in heap arrays on the instance), so wasm_resume() returns to
// its caller on fuel exhaustion or an import suspend, and can be re-entered to
// continue exactly where it left off -- the property that lets a Lisp context
// host a guest without any ring-3 / scheduler machinery.

#ifndef CARDINAL_WASM_H
#define CARDINAL_WASM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---- Values -------------------------------------------------------------

// Value types use the Wasm binary encoding bytes so decode/validate can store
// them directly.
typedef enum {
    WASM_I32 = 0x7F,
    WASM_I64 = 0x7E,
    WASM_F32 = 0x7D,
    WASM_F64 = 0x7C,
    WASM_FUNCREF = 0x70,
    WASM_EXTERNREF = 0x6F,
} wasm_valtype_t;

// A single operand-stack / argument / result slot. Untagged: the module's types
// (and, once implemented, validation) determine which field is live.
typedef union {
    int32_t i32;
    uint32_t u32;
    int64_t i64;
    uint64_t u64;
    float f32;
    double f64;
    uint64_t bits;
} wasm_value_t;

// ---- Status / result codes ---------------------------------------------

typedef enum {
    WASM_OK = 0,          // generic success (decode/instantiate)
    WASM_ERR_DECODE,      // malformed module bytes
    WASM_ERR_VALIDATE,    // module failed validation
    WASM_ERR_LINK,        // an import could not be satisfied at instantiate
    WASM_ERR_OOM,         // allocation failed
    WASM_ERR_RUNTIME,     // misuse of the API (bad args, wrong state)
} wasm_result_t;

// Outcome of a wasm_resume() slice.
typedef enum {
    WASM_RUN_DONE = 0,    // top-level call returned; results available
    WASM_RUN_FUEL,        // fuel exhausted mid-execution; resume to continue
    WASM_RUN_SUSPENDED,   // an imported function asked the host to service it;
                          // read wasm_pending(), wasm_provide() results, resume
    WASM_RUN_TRAPPED,     // a trap occurred (OOB, div0, unreachable, ...)
} wasm_run_status_t;

// Trap kinds (valid when wasm_resume returns WASM_RUN_TRAPPED).
typedef enum {
    WASM_TRAP_NONE = 0,
    WASM_TRAP_UNREACHABLE,
    WASM_TRAP_OOB_MEMORY,
    WASM_TRAP_OOB_TABLE,
    WASM_TRAP_DIV_BY_ZERO,
    WASM_TRAP_INT_OVERFLOW,
    WASM_TRAP_INVALID_CONVERSION,
    WASM_TRAP_INDIRECT_TYPE_MISMATCH,
    WASM_TRAP_UNINIT_ELEMENT,
    WASM_TRAP_CALL_STACK_EXHAUSTED,
    WASM_TRAP_STACK_OVERFLOW,
    WASM_TRAP_HOST,       // a host import signalled an error
} wasm_trap_t;

// ---- Opaque handles -----------------------------------------------------

typedef struct wasm_module wasm_module_t;       // decoded, immutable
typedef struct wasm_instance wasm_instance_t;   // runtime state (mutable)

// ---- Host imports -------------------------------------------------------

// A synchronous host function: fills `results` from `args` and returns. Used for
// cheap, no-policy imports (clock, etc.). For imports that must be serviced by
// the Lisp host (display/input/file), register them as host-serviced (NULL fn)
// so the call suspends to the driver instead -- see wasm_pending().
typedef wasm_trap_t (*wasm_host_fn)(wasm_instance_t *inst, void *user,
                                    const wasm_value_t *args, wasm_value_t *results);

// One import resolution supplied by the host at instantiate time. Match against
// the module's declared (module_name, field_name). `fn == NULL` marks the import
// as host-serviced: a call to it suspends the VM (WASM_RUN_SUSPENDED) carrying
// `host_id` in the pending record, rather than calling into C.
typedef struct {
    const char *module_name;
    const char *field_name;
    wasm_host_fn fn;     // synchronous handler, or NULL for host-serviced
    void *user;          // opaque, passed back to fn
    uint32_t host_id;    // identifies this import in the pending record
} wasm_import_def_t;

// Describes the imported call the guest is blocked on (WASM_RUN_SUSPENDED).
typedef struct {
    uint32_t host_id;             // from the matching wasm_import_def_t
    const char *module_name;
    const char *field_name;
    const wasm_value_t *args;     // arguments the guest passed
    uint32_t n_args;
    uint32_t n_results;           // results the host must wasm_provide()
} wasm_pending_t;

// ---- Module lifecycle ---------------------------------------------------

// Decode a module from raw .wasm bytes. The module copies what it needs; `buf`
// need not outlive the call. Returns NULL and sets *err on failure.
wasm_module_t *wasm_module_decode(const uint8_t *buf, size_t len, wasm_result_t *err);

// Validate a decoded module (type-checks bodies, control flow). v1 may implement
// only structural checks; full type validation is the hardening pass. Memory
// safety does not depend on this -- the interpreter bounds-checks at runtime.
wasm_result_t wasm_module_validate(wasm_module_t *m);

void wasm_module_free(wasm_module_t *m);

// ---- Instance lifecycle -------------------------------------------------

// Instantiate: resolve imports, allocate linear memory (reserving the declared
// max so it never moves), init globals/tables, run the start function's setup if
// present (does not run to completion -- use wasm_resume). `imports` is matched
// by (module_name, field_name); a missing required import -> WASM_ERR_LINK.
wasm_instance_t *wasm_instantiate(wasm_module_t *m,
                                  const wasm_import_def_t *imports, uint32_t n_imports,
                                  wasm_result_t *err);

void wasm_instance_free(wasm_instance_t *inst);

// The module an instance was created from (the instance does not own it; free
// the module separately, after the instance).
wasm_module_t *wasm_instance_module(const wasm_instance_t *inst);

// ---- Execution ----------------------------------------------------------

// Begin a call to an exported function. Arguments are taken from args[0..n_args).
// Does not run; call wasm_resume() to drive it. Returns WASM_ERR_* on bad setup.
wasm_result_t wasm_call(wasm_instance_t *inst, const char *export_name,
                        const wasm_value_t *args, uint32_t n_args);

// Run up to `fuel` reductions (calls + loop back-edges, like the Lisp VM budget).
// Returns the slice outcome; re-enter to continue.
wasm_run_status_t wasm_resume(wasm_instance_t *inst, int64_t fuel);

// When WASM_RUN_SUSPENDED: the call the guest is blocked on.
const wasm_pending_t *wasm_pending(const wasm_instance_t *inst);

// Supply the results for a host-serviced import before the next wasm_resume().
wasm_result_t wasm_provide(wasm_instance_t *inst, const wasm_value_t *results, uint32_t n);

// When WASM_RUN_DONE: copy out the top-level results (up to `cap`); returns count.
uint32_t wasm_results(const wasm_instance_t *inst, wasm_value_t *out, uint32_t cap);

// When WASM_RUN_TRAPPED: the trap kind.
wasm_trap_t wasm_trap(const wasm_instance_t *inst);

// ---- Linear memory (zero-copy host access) ------------------------------

// Current linear memory base + length in bytes. The pointer is stable for the
// instance's lifetime (memory is reserved at max), so the host can cache it.
// Returns NULL/0 if the module declares no memory.
uint8_t *wasm_memory(wasm_instance_t *inst, size_t *len_out);

#ifdef __cplusplus
}
#endif

#endif // CARDINAL_WASM_H
