// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

// Stackless, fuel-metered Wasm interpreter: the control-flow prepass and the
// dispatch loop. STUB -- implemented in the execution workstream against the
// frozen wasm_internal.h. See notes/core/wasm-guests.md.

#include "wasm_internal.h"

bool wasm_exec_prepare_func(wasm_func_t *f, wasm_result_t *err) {
    (void)f;
    if (err) *err = WASM_ERR_VALIDATE;
    return false;
}

void wasm_exec_run(wasm_instance_t *inst, int64_t fuel) {
    (void)fuel;
    inst->trap = WASM_TRAP_UNREACHABLE;
    inst->status = WASM_RUN_TRAPPED;
}
