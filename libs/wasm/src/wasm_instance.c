// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

// Instance spine for the Cardinal; Wasm interpreter: the shared helpers (LEB
// readers, value stack, linear memory), import resolution, and the
// instantiate/call/resume lifecycle. The decode (wasm_module.c) and execution
// (wasm_exec.c) layers build on the helpers and entry points declared in
// wasm_internal.h.

#include "wasm_internal.h"
#include <stdlib.h>
#include <string.h>

// ---- LEB128 -------------------------------------------------------------

bool wasm_read_byte(const uint8_t *buf, uint32_t len, uint32_t *pc, uint8_t *out) {
    if (*pc >= len) return false;
    *out = buf[(*pc)++];
    return true;
}

bool wasm_read_u64(const uint8_t *buf, uint32_t len, uint32_t *pc, uint64_t *out) {
    uint64_t result = 0;
    int shift = 0;
    uint8_t b;
    do {
        if (*pc >= len || shift >= 64) return false;
        b = buf[(*pc)++];
        result |= (uint64_t)(b & 0x7F) << shift;
        shift += 7;
    } while (b & 0x80);
    *out = result;
    return true;
}

bool wasm_read_u32(const uint8_t *buf, uint32_t len, uint32_t *pc, uint32_t *out) {
    uint64_t v;
    if (!wasm_read_u64(buf, len, pc, &v)) return false;
    if (v > 0xFFFFFFFFu) return false;   // reject overlong / over-32-bit LEB
    *out = (uint32_t)v;
    return true;
}

bool wasm_read_s64(const uint8_t *buf, uint32_t len, uint32_t *pc, int64_t *out) {
    int64_t result = 0;
    int shift = 0;
    uint8_t b;
    do {
        if (*pc >= len || shift >= 64) return false;
        b = buf[(*pc)++];
        result |= (int64_t)(b & 0x7F) << shift;
        shift += 7;
    } while (b & 0x80);
    if (shift < 64 && (b & 0x40))
        result |= -((int64_t)1 << shift);   // sign-extend
    *out = result;
    return true;
}

bool wasm_read_s32(const uint8_t *buf, uint32_t len, uint32_t *pc, int32_t *out) {
    int64_t v;
    if (!wasm_read_s64(buf, len, pc, &v)) return false;
    if (v < INT32_MIN || v > INT32_MAX) return false;  // must fit a 32-bit int
    *out = (int32_t)v;
    return true;
}

// ---- Value stack --------------------------------------------------------

void wasm_push(wasm_instance_t *inst, wasm_value_t v) {
    if (inst->vsp >= WASM_VSTACK_CAP) {
        inst->trap = WASM_TRAP_STACK_OVERFLOW;
        inst->status = WASM_RUN_TRAPPED;
        return;
    }
    inst->vstack[inst->vsp++] = v;
}

wasm_value_t wasm_pop(wasm_instance_t *inst) {
    if (inst->vsp == 0) {
        inst->trap = WASM_TRAP_STACK_UNDERFLOW;
        inst->status = WASM_RUN_TRAPPED;
        wasm_value_t z = {0};
        return z;
    }
    return inst->vstack[--inst->vsp];
}

// ---- Linear memory ------------------------------------------------------

uint8_t *wasm_mem_ea(wasm_instance_t *inst, uint32_t addr, uint32_t size) {
    // 64-bit math avoids addr+size overflow
    uint64_t end = (uint64_t)addr + (uint64_t)size;
    if (!inst->mem || end > inst->mem_size) {
        inst->trap = WASM_TRAP_OOB_MEMORY;
        inst->status = WASM_RUN_TRAPPED;
        return NULL;
    }
    return inst->mem + addr;
}

uint32_t wasm_mem_grow(wasm_instance_t *inst, uint32_t delta) {
    uint32_t prev = inst->cur_pages;
    uint64_t want = (uint64_t)prev + (uint64_t)delta;
    if (want > inst->max_pages)
        return 0xFFFFFFFFu;          // memory is reserved at max; cannot exceed it
    inst->cur_pages = (uint32_t)want;
    inst->mem_size = (size_t)inst->cur_pages * WASM_PAGE_SIZE;
    return prev;                     // grown region is already zeroed (calloc)
}

// ---- Module lifecycle (public) -----------------------------------------

wasm_module_t *wasm_module_decode(const uint8_t *buf, size_t len, wasm_result_t *err) {
    wasm_result_t e = WASM_OK;
    wasm_module_t *m = wasm_decode_impl(buf, len, &e);
    if (err) *err = e;
    return m;
}

wasm_result_t wasm_module_validate(wasm_module_t *m) {
    if (!m) return WASM_ERR_VALIDATE;
    return wasm_validate_impl(m);
}

void wasm_module_free(wasm_module_t *m) {
    if (m) wasm_module_free_impl(m);
}

// ---- Instance lifecycle -------------------------------------------------

static const wasm_import_def_t *find_import(const wasm_import_def_t *defs, uint32_t n,
                                            const char *mod, const char *field) {
    for (uint32_t i = 0; i < n; i++)
        if (strcmp(defs[i].module_name, mod) == 0 &&
            strcmp(defs[i].field_name, field) == 0)
            return &defs[i];
    return NULL;
}

wasm_instance_t *wasm_instantiate(wasm_module_t *m,
                                  const wasm_import_def_t *imports, uint32_t n_imports,
                                  wasm_result_t *err) {
    wasm_result_t e = WASM_OK;
    wasm_instance_t *inst = calloc(1, sizeof(*inst));
    if (!inst) { e = WASM_ERR_OOM; goto fail; }
    inst->module = m;

    // Resolve function imports (v1: function imports only).
    inst->n_imports = m->n_imports;
    if (m->n_imports) {
        inst->imports = calloc(m->n_imports, sizeof(wasm_resolved_import_t));
        if (!inst->imports) { e = WASM_ERR_OOM; goto fail; }
        for (uint32_t i = 0; i < m->n_imports; i++) {
            wasm_importdecl_t *d = &m->imports[i];
            if (d->kind != WASM_EXTERN_FUNC) { e = WASM_ERR_LINK; goto fail; }
            const wasm_import_def_t *def =
                find_import(imports, n_imports, d->module_name, d->field_name);
            if (!def) { e = WASM_ERR_LINK; goto fail; }
            inst->imports[i].fn = def->fn;
            inst->imports[i].user = def->user;
            inst->imports[i].host_id = def->host_id;
            inst->imports[i].module_name = def->module_name;
            inst->imports[i].field_name = def->field_name;
            inst->imports[i].type_index = d->type_index;
        }
    }

    // Linear memory: reserve max so the base never moves.
    if (m->has_mem) {
        uint32_t maxp = m->mem_max_pages;
        if (maxp < m->mem_min_pages) maxp = m->mem_min_pages;
        if (maxp == 0) maxp = m->mem_min_pages;
        inst->max_pages = maxp;
        inst->cur_pages = m->mem_min_pages;
        inst->mem_size = (size_t)inst->cur_pages * WASM_PAGE_SIZE;
        if (maxp) {
            inst->mem = calloc((size_t)maxp * WASM_PAGE_SIZE, 1);
            if (!inst->mem) { e = WASM_ERR_OOM; goto fail; }
        }
    }

    // Globals.
    if (m->n_globals) {
        inst->globals = calloc(m->n_globals, sizeof(wasm_value_t));
        if (!inst->globals) { e = WASM_ERR_OOM; goto fail; }
        for (uint32_t i = 0; i < m->n_globals; i++)
            inst->globals[i] = m->globals[i].init;
        inst->n_globals = m->n_globals;
    }

    // Table + element segments.
    if (m->has_table) {
        inst->table_len = m->table_max >= m->table_min ? m->table_max : m->table_min;
        if (inst->table_len == 0) inst->table_len = m->table_min;
        inst->table = malloc(sizeof(uint32_t) * (inst->table_len ? inst->table_len : 1));
        if (!inst->table) { e = WASM_ERR_OOM; goto fail; }
        for (uint32_t i = 0; i < inst->table_len; i++) inst->table[i] = 0xFFFFFFFFu;
        for (uint32_t s = 0; s < m->n_elem; s++) {
            wasm_elem_seg_t *seg = &m->elem[s];
            for (uint32_t k = 0; k < seg->n; k++) {
                uint64_t idx = (uint64_t)seg->table_offset + k;
                if (idx >= inst->table_len) { e = WASM_ERR_LINK; goto fail; }
                inst->table[idx] = seg->func_indices[k];
            }
        }
    }

    // Active data segments -> linear memory.
    for (uint32_t s = 0; s < m->n_data; s++) {
        wasm_data_seg_t *seg = &m->data[s];
        uint64_t end = (uint64_t)seg->mem_offset + seg->len;
        if (end > inst->mem_size) { e = WASM_ERR_LINK; goto fail; }
        if (seg->len) memcpy(inst->mem + seg->mem_offset, seg->bytes, seg->len);
    }

    // Execution scratch.
    inst->vstack = malloc(sizeof(wasm_value_t) * WASM_VSTACK_CAP);
    inst->frames = malloc(sizeof(wasm_frame_t) * WASM_FRAME_CAP);
    inst->ctrl = malloc(sizeof(wasm_label_t) * WASM_CTRL_CAP);
    if (!inst->vstack || !inst->frames || !inst->ctrl) { e = WASM_ERR_OOM; goto fail; }

    inst->status = WASM_RUN_DONE;   // idle until wasm_call()
    if (err) *err = WASM_OK;
    return inst;

fail:
    if (err) *err = e;
    wasm_instance_free(inst);
    return NULL;
}

void wasm_instance_free(wasm_instance_t *inst) {
    if (!inst) return;
    free(inst->imports);
    free(inst->mem);
    free(inst->globals);
    free(inst->table);
    free(inst->vstack);
    free(inst->frames);
    free(inst->ctrl);
    free(inst);
}

// ---- Execution drive ----------------------------------------------------

static const wasm_functype_t *func_signature(wasm_module_t *m, uint32_t func_index) {
    uint32_t ti;
    if (func_index < m->n_imported_funcs) {
        // imported function: signature carried on the import decl
        uint32_t seen = 0;
        for (uint32_t i = 0; i < m->n_imports; i++) {
            if (m->imports[i].kind != WASM_EXTERN_FUNC) continue;
            if (seen == func_index) { ti = m->imports[i].type_index; goto got; }
            seen++;
        }
        return NULL;
    }
    ti = m->funcs[func_index - m->n_imported_funcs].type_index;
got:
    if (ti >= m->n_types) return NULL;
    return &m->types[ti];
}

wasm_result_t wasm_call(wasm_instance_t *inst, const char *export_name,
                        const wasm_value_t *args, uint32_t n_args) {
    wasm_module_t *m = inst->module;
    // Reject re-entry while a run is in progress (suspended/fuel-paused). Only an
    // idle instance (never started, or a finished/trapped run) may be re-staged;
    // clobbering a live run's stacks would corrupt the suspended guest state.
    if (inst->started && inst->status != WASM_RUN_DONE &&
        inst->status != WASM_RUN_TRAPPED)
        return WASM_ERR_RUNTIME;
    // Locate the exported function.
    uint32_t func_index = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < m->n_exports; i++)
        if (m->exports[i].kind == WASM_EXTERN_FUNC &&
            strcmp(m->exports[i].name, export_name) == 0) {
            func_index = m->exports[i].index;
            break;
        }
    if (func_index == 0xFFFFFFFFu) return WASM_ERR_RUNTIME;
    if (func_index < m->n_imported_funcs) return WASM_ERR_RUNTIME; // no body to enter
    // Defense in depth: even if decode/validation missed it, never enter a
    // function index outside the defined-function range.
    if (func_index >= m->n_imported_funcs + m->n_funcs) return WASM_ERR_RUNTIME;

    const wasm_functype_t *sig = func_signature(m, func_index);
    if (!sig || sig->n_params != n_args || n_args > 16) return WASM_ERR_RUNTIME;

    // Reset execution state and stage the args.
    inst->vsp = inst->fsp = inst->csp = 0;
    inst->trap = WASM_TRAP_NONE;
    inst->has_provided = false;
    inst->n_provided = 0;
    for (uint32_t i = 0; i < n_args; i++) inst->vstack[inst->vsp++] = args[i];

    inst->entry_func = func_index;
    inst->entry_nargs = n_args;
    inst->entry_pending = true;
    inst->started = true;
    inst->status = WASM_RUN_FUEL;   // ready to run
    inst->n_results = 0;
    return WASM_OK;
}

wasm_run_status_t wasm_resume(wasm_instance_t *inst, int64_t fuel) {
    if (!inst->started) return WASM_RUN_TRAPPED;
    if (inst->status == WASM_RUN_DONE || inst->status == WASM_RUN_TRAPPED)
        return inst->status;
    inst->fuel = fuel;
    wasm_exec_run(inst, fuel);
    return inst->status;
}

const wasm_pending_t *wasm_pending(const wasm_instance_t *inst) {
    return &inst->pending;
}

wasm_result_t wasm_provide(wasm_instance_t *inst, const wasm_value_t *results, uint32_t n) {
    if (n > 16) return WASM_ERR_RUNTIME;
    // The guest expects exactly pending.n_results values pushed; a mismatch would
    // leave the operand stack short (guest reads stale/uninit slots) or long.
    if (n != inst->pending.n_results) return WASM_ERR_RUNTIME;
    for (uint32_t i = 0; i < n; i++) inst->provided[i] = results[i];
    inst->n_provided = n;
    inst->has_provided = true;
    return WASM_OK;
}

uint32_t wasm_results(const wasm_instance_t *inst, wasm_value_t *out, uint32_t cap) {
    uint32_t n = inst->n_results < cap ? inst->n_results : cap;
    for (uint32_t i = 0; i < n; i++) out[i] = inst->results[i];
    return n;
}

wasm_trap_t wasm_trap(const wasm_instance_t *inst) { return inst->trap; }

wasm_module_t *wasm_instance_module(const wasm_instance_t *inst) { return inst->module; }

uint8_t *wasm_memory(wasm_instance_t *inst, size_t *len_out) {
    if (len_out) *len_out = inst->mem_size;
    return inst->mem;
}
