// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

// Wasm binary decoder: raw .wasm bytes -> wasm_module_t (sections). Implements
// the MVP binary format (https://webassembly.github.io/spec/core/binary/)
// against the frozen wasm_internal.h. The decoder copies the module bytes into
// m->image and points all code/data spans into that owned copy, so the caller's
// buffer need not outlive the call. Every read is bounds-checked; any malformed
// input yields NULL with *err = WASM_ERR_DECODE (partial state freed).

#include "wasm_internal.h"
#include <stdlib.h>
#include <string.h>

// ---- Section ids --------------------------------------------------------
enum {
    SEC_CUSTOM = 0,
    SEC_TYPE = 1,
    SEC_IMPORT = 2,
    SEC_FUNC = 3,
    SEC_TABLE = 4,
    SEC_MEMORY = 5,
    SEC_GLOBAL = 6,
    SEC_EXPORT = 7,
    SEC_START = 8,
    SEC_ELEM = 9,
    SEC_CODE = 10,
    SEC_DATA = 11,
};

#define WASM_MEM_MAX_DEFAULT 65536u   // cap when a memtype is unbounded

// ---- small allocation helper -------------------------------------------

// calloc that maps a zero-count to a NULL pointer (so free() over an empty
// vector is harmless and we never calloc(0)).
static void *xcalloc(size_t n, size_t sz) {
    if (n == 0) return NULL;
    return calloc(n, sz);
}

// ---- forward decls ------------------------------------------------------

static bool valid_valtype(uint8_t v) {
    switch (v) {
        case WASM_I32:
        case WASM_I64:
        case WASM_F32:
        case WASM_F64:
        case WASM_FUNCREF:
        case WASM_EXTERNREF:
            return true;
        default:
            return false;
    }
}

// Read a vec(valtype) into a freshly-allocated array. *out_arr may be NULL when
// the count is zero. Returns false on malformed input or OOM (caller decides
// which error).
static bool read_valtype_vec(const uint8_t *buf, uint32_t len, uint32_t *pc,
                             wasm_valtype_t **out_arr, uint32_t *out_n,
                             bool *oom) {
    *out_arr = NULL;
    *out_n = 0;
    *oom = false;
    uint32_t n;
    if (!wasm_read_u32(buf, len, pc, &n)) return false;
    wasm_valtype_t *arr = (wasm_valtype_t *)xcalloc(n, sizeof(wasm_valtype_t));
    if (n != 0 && arr == NULL) {
        *oom = true;
        return false;
    }
    for (uint32_t i = 0; i < n; i++) {
        uint8_t b;
        if (!wasm_read_byte(buf, len, pc, &b) || !valid_valtype(b)) {
            free(arr);
            return false;
        }
        arr[i] = (wasm_valtype_t)b;
    }
    *out_arr = arr;
    *out_n = n;
    return true;
}

// Read a name = vec(byte) and store as a malloc'd NUL-terminated C string.
// Returns false on malformed input; sets *oom on allocation failure.
static bool read_name(const uint8_t *buf, uint32_t len, uint32_t *pc,
                      char **out, bool *oom) {
    *out = NULL;
    *oom = false;
    uint32_t n;
    if (!wasm_read_u32(buf, len, pc, &n)) return false;
    // bounds-check the byte span before allocating
    if ((uint64_t)*pc + n > len) return false;
    char *s = (char *)malloc((size_t)n + 1);
    if (!s) {
        *oom = true;
        return false;
    }
    memcpy(s, buf + *pc, n);
    s[n] = '\0';
    *pc += n;
    *out = s;
    return true;
}

// Read a limits structure: flag byte (0 -> min only; 1 -> min,max). Returns the
// min/max (has_max indicates whether a max was present).
static bool read_limits(const uint8_t *buf, uint32_t len, uint32_t *pc,
                        uint32_t *min, uint32_t *max, bool *has_max) {
    uint8_t flag;
    if (!wasm_read_byte(buf, len, pc, &flag)) return false;
    if (flag != 0 && flag != 1) return false;
    if (!wasm_read_u32(buf, len, pc, min)) return false;
    if (flag == 1) {
        if (!wasm_read_u32(buf, len, pc, max)) return false;
        *has_max = true;
    } else {
        *max = 0;
        *has_max = false;
    }
    return true;
}

// Evaluate a constant init expression that ends in END (0x0B). Supports only the
// numeric *.const forms (the MVP forms produced by toolchains for globals /
// active-segment offsets). global.get of an imported global is not supported in
// v1 -> decode error. On success *out holds the constant; *out_type its valtype.
static bool eval_const_expr(const uint8_t *buf, uint32_t len, uint32_t *pc,
                            wasm_value_t *out, wasm_valtype_t *out_type) {
    uint8_t op;
    if (!wasm_read_byte(buf, len, pc, &op)) return false;
    wasm_value_t v;
    v.bits = 0;
    wasm_valtype_t ty;
    switch (op) {
        case OP_I32_CONST: {
            int32_t x;
            if (!wasm_read_s32(buf, len, pc, &x)) return false;
            v.i32 = x;
            ty = WASM_I32;
            break;
        }
        case OP_I64_CONST: {
            int64_t x;
            if (!wasm_read_s64(buf, len, pc, &x)) return false;
            v.i64 = x;
            ty = WASM_I64;
            break;
        }
        case OP_F32_CONST: {
            if ((uint64_t)*pc + 4 > len) return false;
            uint32_t bits;
            memcpy(&bits, buf + *pc, 4);
            *pc += 4;
            v.bits = bits;       // store raw f32 bits in low 32
            ty = WASM_F32;
            break;
        }
        case OP_F64_CONST: {
            if ((uint64_t)*pc + 8 > len) return false;
            uint64_t bits;
            memcpy(&bits, buf + *pc, 8);
            *pc += 8;
            v.bits = bits;
            ty = WASM_F64;
            break;
        }
        default:
            // global.get (0x23) of an imported global or anything else: not
            // supported in v1.
            return false;
    }
    // Expect the terminating END.
    uint8_t end;
    if (!wasm_read_byte(buf, len, pc, &end) || end != OP_END) return false;
    *out = v;
    *out_type = ty;
    return true;
}

// ---- section decoders ---------------------------------------------------
// Each returns false on malformed input (caller sets WASM_ERR_DECODE) and may
// set *oom for an allocation failure (caller sets WASM_ERR_OOM). The cursor
// (buf,len,*pc) is positioned at the section payload start and must be fully
// consumed; the caller verifies it lands exactly on the section end.

static bool decode_type_section(wasm_module_t *m, const uint8_t *buf,
                                uint32_t len, uint32_t *pc, bool *oom) {
    uint32_t n;
    if (!wasm_read_u32(buf, len, pc, &n)) return false;
    m->types = (wasm_functype_t *)xcalloc(n, sizeof(wasm_functype_t));
    if (n != 0 && !m->types) {
        *oom = true;
        return false;
    }
    m->n_types = n;
    for (uint32_t i = 0; i < n; i++) {
        uint8_t form;
        if (!wasm_read_byte(buf, len, pc, &form) || form != 0x60) return false;
        wasm_functype_t *ft = &m->types[i];
        if (!read_valtype_vec(buf, len, pc, &ft->params, &ft->n_params, oom))
            return false;
        if (!read_valtype_vec(buf, len, pc, &ft->results, &ft->n_results, oom))
            return false;
    }
    return true;
}

static bool decode_import_section(wasm_module_t *m, const uint8_t *buf,
                                  uint32_t len, uint32_t *pc, bool *oom) {
    uint32_t n;
    if (!wasm_read_u32(buf, len, pc, &n)) return false;
    m->imports = (wasm_importdecl_t *)xcalloc(n, sizeof(wasm_importdecl_t));
    if (n != 0 && !m->imports) {
        *oom = true;
        return false;
    }
    m->n_imports = n;
    for (uint32_t i = 0; i < n; i++) {
        wasm_importdecl_t *im = &m->imports[i];
        if (!read_name(buf, len, pc, &im->module_name, oom)) return false;
        if (!read_name(buf, len, pc, &im->field_name, oom)) return false;
        uint8_t kind;
        if (!wasm_read_byte(buf, len, pc, &kind)) return false;
        im->kind = (wasm_externkind_t)kind;
        switch (kind) {
            case WASM_EXTERN_FUNC: {
                if (!wasm_read_u32(buf, len, pc, &im->type_index)) return false;
                m->n_imported_funcs++;
                break;
            }
            case WASM_EXTERN_TABLE: {
                // tabletype = elemtype byte + limits
                uint8_t et;
                if (!wasm_read_byte(buf, len, pc, &et)) return false;
                if (et != WASM_FUNCREF && et != WASM_EXTERNREF) return false;
                uint32_t mn, mx;
                bool hm;
                if (!read_limits(buf, len, pc, &mn, &mx, &hm)) return false;
                break;
            }
            case WASM_EXTERN_MEM: {
                uint32_t mn, mx;
                bool hm;
                if (!read_limits(buf, len, pc, &mn, &mx, &hm)) return false;
                break;
            }
            case WASM_EXTERN_GLOBAL: {
                // globaltype = valtype + mut byte
                uint8_t vt, mut;
                if (!wasm_read_byte(buf, len, pc, &vt) || !valid_valtype(vt))
                    return false;
                if (!wasm_read_byte(buf, len, pc, &mut) || mut > 1) return false;
                break;
            }
            default:
                return false;
        }
    }
    return true;
}

// The func section's typeidx vec is stashed here; pairs with code bodies.
static bool decode_func_section(const uint8_t *buf, uint32_t len, uint32_t *pc,
                                uint32_t **out_typeidx, uint32_t *out_n,
                                bool *oom) {
    uint32_t n;
    if (!wasm_read_u32(buf, len, pc, &n)) return false;
    uint32_t *idx = (uint32_t *)xcalloc(n, sizeof(uint32_t));
    if (n != 0 && !idx) {
        *oom = true;
        return false;
    }
    for (uint32_t i = 0; i < n; i++) {
        if (!wasm_read_u32(buf, len, pc, &idx[i])) {
            free(idx);
            return false;
        }
    }
    *out_typeidx = idx;
    *out_n = n;
    return true;
}

static bool decode_table_section(wasm_module_t *m, const uint8_t *buf,
                                 uint32_t len, uint32_t *pc) {
    uint32_t n;
    if (!wasm_read_u32(buf, len, pc, &n)) return false;
    for (uint32_t i = 0; i < n; i++) {
        uint8_t et;
        if (!wasm_read_byte(buf, len, pc, &et)) return false;
        if (et != WASM_FUNCREF && et != WASM_EXTERNREF) return false;
        uint32_t mn, mx;
        bool hm;
        if (!read_limits(buf, len, pc, &mn, &mx, &hm)) return false;
        // v1 tracks only a single table.
        if (i == 0) {
            m->has_table = true;
            m->table_min = mn;
            m->table_max = hm ? mx : mn;
        }
    }
    return true;
}

static bool decode_memory_section(wasm_module_t *m, const uint8_t *buf,
                                  uint32_t len, uint32_t *pc) {
    uint32_t n;
    if (!wasm_read_u32(buf, len, pc, &n)) return false;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t mn, mx;
        bool hm;
        if (!read_limits(buf, len, pc, &mn, &mx, &hm)) return false;
        if (i == 0) {
            m->has_mem = true;
            m->mem_min_pages = mn;
            m->mem_max_pages = hm ? mx : WASM_MEM_MAX_DEFAULT;
        }
    }
    return true;
}

static bool decode_global_section(wasm_module_t *m, const uint8_t *buf,
                                  uint32_t len, uint32_t *pc, bool *oom) {
    uint32_t n;
    if (!wasm_read_u32(buf, len, pc, &n)) return false;
    m->globals = (wasm_global_t *)xcalloc(n, sizeof(wasm_global_t));
    if (n != 0 && !m->globals) {
        *oom = true;
        return false;
    }
    m->n_globals = n;
    for (uint32_t i = 0; i < n; i++) {
        wasm_global_t *g = &m->globals[i];
        uint8_t vt, mut;
        if (!wasm_read_byte(buf, len, pc, &vt) || !valid_valtype(vt))
            return false;
        if (!wasm_read_byte(buf, len, pc, &mut) || mut > 1) return false;
        g->type = (wasm_valtype_t)vt;
        g->mutable_ = (mut == 1);
        wasm_valtype_t init_ty;
        if (!eval_const_expr(buf, len, pc, &g->init, &init_ty)) return false;
        // The constexpr's type must match the declared global type.
        if (init_ty != g->type) return false;
    }
    return true;
}

static bool decode_export_section(wasm_module_t *m, const uint8_t *buf,
                                  uint32_t len, uint32_t *pc, bool *oom) {
    uint32_t n;
    if (!wasm_read_u32(buf, len, pc, &n)) return false;
    m->exports = (wasm_export_t *)xcalloc(n, sizeof(wasm_export_t));
    if (n != 0 && !m->exports) {
        *oom = true;
        return false;
    }
    m->n_exports = n;
    for (uint32_t i = 0; i < n; i++) {
        wasm_export_t *ex = &m->exports[i];
        if (!read_name(buf, len, pc, &ex->name, oom)) return false;
        uint8_t kind;
        if (!wasm_read_byte(buf, len, pc, &kind) || kind > 3) return false;
        ex->kind = (wasm_externkind_t)kind;
        if (!wasm_read_u32(buf, len, pc, &ex->index)) return false;
    }
    return true;
}

static bool decode_elem_section(wasm_module_t *m, const uint8_t *buf,
                                uint32_t len, uint32_t *pc, bool *oom) {
    uint32_t n;
    if (!wasm_read_u32(buf, len, pc, &n)) return false;
    m->elem = (wasm_elem_seg_t *)xcalloc(n, sizeof(wasm_elem_seg_t));
    if (n != 0 && !m->elem) {
        *oom = true;
        return false;
    }
    m->n_elem = n;
    for (uint32_t i = 0; i < n; i++) {
        wasm_elem_seg_t *e = &m->elem[i];
        uint32_t flag;
        if (!wasm_read_u32(buf, len, pc, &flag)) return false;
        // v1 supports only the MVP active form (flag 0): tableidx 0, an i32
        // offset constexpr, then vec(funcidx).
        if (flag != 0) return false;
        wasm_value_t off;
        wasm_valtype_t off_ty;
        if (!eval_const_expr(buf, len, pc, &off, &off_ty)) return false;
        if (off_ty != WASM_I32) return false;
        e->table_offset = off.u32;
        uint32_t cnt;
        if (!wasm_read_u32(buf, len, pc, &cnt)) return false;
        e->func_indices = (uint32_t *)xcalloc(cnt, sizeof(uint32_t));
        if (cnt != 0 && !e->func_indices) {
            *oom = true;
            return false;
        }
        e->n = cnt;
        for (uint32_t j = 0; j < cnt; j++) {
            if (!wasm_read_u32(buf, len, pc, &e->func_indices[j])) return false;
        }
    }
    return true;
}

// Code section: pairs with the previously-stashed func typeidx vec. `image` is
// the owned module copy and `image_base` is the payload's offset within it, so
// f->code can point into m->image rather than the transient section cursor.
static bool decode_code_section(wasm_module_t *m, const uint8_t *buf,
                                uint32_t len, uint32_t *pc,
                                const uint8_t *image, uint32_t image_base,
                                const uint32_t *func_typeidx,
                                uint32_t n_func_typeidx, bool *oom) {
    uint32_t n;
    if (!wasm_read_u32(buf, len, pc, &n)) return false;
    // The code count must match the func-section count.
    if (n != n_func_typeidx) return false;
    m->funcs = (wasm_func_t *)xcalloc(n, sizeof(wasm_func_t));
    if (n != 0 && !m->funcs) {
        *oom = true;
        return false;
    }
    m->n_funcs = n;
    for (uint32_t i = 0; i < n; i++) {
        wasm_func_t *f = &m->funcs[i];
        uint32_t tyidx = func_typeidx[i];
        if (tyidx >= m->n_types) return false;
        f->type_index = tyidx;
        f->n_params = m->types[tyidx].n_params;
        f->cf = NULL;

        uint32_t body_size;
        if (!wasm_read_u32(buf, len, pc, &body_size)) return false;
        uint32_t body_start = *pc;
        if ((uint64_t)body_start + body_size > len) return false;
        uint32_t body_end = body_start + body_size;

        // locals declarations: vec of (count u32, valtype byte)
        uint32_t n_decls;
        if (!wasm_read_u32(buf, len, pc, &n_decls)) return false;
        uint64_t total_locals = f->n_params;
        // First pass: count locals (and validate decl valtypes).
        uint32_t decls_pc = *pc;  // remember to re-read types into local_types
        for (uint32_t d = 0; d < n_decls; d++) {
            uint32_t cnt;
            uint8_t vt;
            if (!wasm_read_u32(buf, len, pc, &cnt)) return false;
            if (!wasm_read_byte(buf, len, pc, &vt) || !valid_valtype(vt))
                return false;
            total_locals += cnt;
            if (total_locals > 0xFFFFFFFFu) return false;  // sane bound
        }
        f->n_locals = (uint32_t)total_locals;

        // Allocate and fill local_types (params first, then declared locals).
        f->local_types =
            (wasm_valtype_t *)xcalloc(f->n_locals, sizeof(wasm_valtype_t));
        if (f->n_locals != 0 && !f->local_types) {
            *oom = true;
            return false;
        }
        for (uint32_t p = 0; p < f->n_params; p++)
            f->local_types[p] = m->types[tyidx].params[p];
        {
            uint32_t fill = f->n_params;
            uint32_t rp = decls_pc;
            for (uint32_t d = 0; d < n_decls; d++) {
                uint32_t cnt;
                uint8_t vt;
                // These were already validated above; just re-read to fill.
                if (!wasm_read_u32(buf, len, &rp, &cnt)) return false;
                if (!wasm_read_byte(buf, len, &rp, &vt)) return false;
                for (uint32_t c = 0; c < cnt; c++)
                    f->local_types[fill++] = (wasm_valtype_t)vt;
            }
        }

        // Instruction bytes run from here to (and including) the trailing END
        // at body_end - 1. *pc currently sits just past the locals decls.
        if (*pc > body_end) return false;
        uint32_t code_off = *pc;
        if (body_end == 0 || code_off > body_end) return false;
        // The function body must end with END (0x0B).
        if (body_end < 1 || buf[body_end - 1] != OP_END) return false;
        f->code = image + image_base + code_off;
        f->code_len = body_end - code_off;

        // Advance to the next code entry.
        *pc = body_end;
    }
    return true;
}

static bool decode_data_section(wasm_module_t *m, const uint8_t *buf,
                                uint32_t len, uint32_t *pc,
                                const uint8_t *image, uint32_t image_base,
                                bool *oom) {
    uint32_t n;
    if (!wasm_read_u32(buf, len, pc, &n)) return false;
    m->data = (wasm_data_seg_t *)xcalloc(n, sizeof(wasm_data_seg_t));
    if (n != 0 && !m->data) {
        *oom = true;
        return false;
    }
    m->n_data = n;
    for (uint32_t i = 0; i < n; i++) {
        wasm_data_seg_t *d = &m->data[i];
        uint32_t memidx;
        if (!wasm_read_u32(buf, len, pc, &memidx)) return false;
        if (memidx != 0) return false;  // MVP: single memory
        wasm_value_t off;
        wasm_valtype_t off_ty;
        if (!eval_const_expr(buf, len, pc, &off, &off_ty)) return false;
        if (off_ty != WASM_I32) return false;
        d->mem_offset = off.u32;
        uint32_t cnt;
        if (!wasm_read_u32(buf, len, pc, &cnt)) return false;
        if ((uint64_t)*pc + cnt > len) return false;
        d->len = cnt;
        d->bytes = image + image_base + *pc;  // point into the owned image
        *pc += cnt;
    }
    return true;
}

// ---- top-level decode ---------------------------------------------------

wasm_module_t *wasm_decode_impl(const uint8_t *buf, size_t len, wasm_result_t *err) {
    wasm_result_t local_err = WASM_ERR_DECODE;
    if (err) *err = WASM_ERR_DECODE;

    // The LEB readers index with uint32_t; reject oversized inputs up front.
    if (buf == NULL || len < 8 || len > 0xFFFFFFFFu) return NULL;

    // Header: magic \0asm, version 1.
    static const uint8_t kMagic[4] = {0x00, 0x61, 0x73, 0x6D};
    static const uint8_t kVersion[4] = {0x01, 0x00, 0x00, 0x00};
    if (memcmp(buf, kMagic, 4) != 0 || memcmp(buf + 4, kVersion, 4) != 0)
        return NULL;

    // All locals that the `fail:` cleanup touches are declared (and zeroed)
    // before any `goto fail`, so the cleanup never reads an indeterminate value.
    wasm_module_t *m = NULL;
    uint32_t *func_typeidx = NULL;   // stashed func-section typeidx vec
    uint32_t n_func_typeidx = 0;
    bool have_func_section = false;
    const uint32_t ulen = (uint32_t)len;
    uint32_t pc = 8;
    int last_id = -1;                // enforce ascending non-custom section ids
    bool oom = false;

    m = (wasm_module_t *)calloc(1, sizeof(wasm_module_t));
    if (!m) {
        local_err = WASM_ERR_OOM;
        goto fail;
    }

    // Owned copy of the bytes; all code/data spans point into this.
    m->image = (uint8_t *)malloc(len);
    if (!m->image) {
        local_err = WASM_ERR_OOM;
        goto fail;
    }
    memcpy(m->image, buf, len);
    m->image_len = len;

    m->mem_max_pages = WASM_MEM_MAX_DEFAULT;

    while (pc < ulen) {
        uint8_t id;
        if (!wasm_read_byte(buf, ulen, &pc, &id)) goto fail;
        uint32_t size;
        if (!wasm_read_u32(buf, ulen, &pc, &size)) goto fail;
        uint32_t payload_start = pc;
        if ((uint64_t)payload_start + size > ulen) goto fail;
        uint32_t payload_end = payload_start + size;

        // Section-relative cursor; section decoders consume exactly `size`.
        uint32_t spc = payload_start;

        if (id == SEC_CUSTOM) {
            // Skip custom sections wholesale. (Order constraints do not apply.)
            pc = payload_end;
            continue;
        }

        // Non-custom sections must appear in strictly ascending id order.
        if ((int)id <= last_id) goto fail;
        last_id = (int)id;

        bool ok = true;
        switch (id) {
            case SEC_TYPE:
                ok = decode_type_section(m, buf, ulen, &spc, &oom);
                break;
            case SEC_IMPORT:
                ok = decode_import_section(m, buf, ulen, &spc, &oom);
                break;
            case SEC_FUNC:
                ok = decode_func_section(buf, ulen, &spc, &func_typeidx,
                                         &n_func_typeidx, &oom);
                have_func_section = ok;
                break;
            case SEC_TABLE:
                ok = decode_table_section(m, buf, ulen, &spc);
                break;
            case SEC_MEMORY:
                ok = decode_memory_section(m, buf, ulen, &spc);
                break;
            case SEC_GLOBAL:
                ok = decode_global_section(m, buf, ulen, &spc, &oom);
                break;
            case SEC_EXPORT:
                ok = decode_export_section(m, buf, ulen, &spc, &oom);
                break;
            case SEC_START: {
                ok = wasm_read_u32(buf, ulen, &spc, &m->start_func);
                if (ok) m->has_start = true;
                break;
            }
            case SEC_ELEM:
                ok = decode_elem_section(m, buf, ulen, &spc, &oom);
                break;
            case SEC_CODE:
                ok = decode_code_section(m, buf, ulen, &spc, m->image, 0,
                                         func_typeidx, n_func_typeidx, &oom);
                break;
            case SEC_DATA:
                ok = decode_data_section(m, buf, ulen, &spc, m->image, 0, &oom);
                break;
            default:
                // Unknown id with a known-section value range: malformed.
                ok = false;
                break;
        }
        if (!ok) {
            if (oom) local_err = WASM_ERR_OOM;
            goto fail;
        }
        // The decoder must consume exactly the section payload.
        if (spc != payload_end) goto fail;

        pc = payload_end;
    }

    // A func section without a matching code section (or vice-versa) is invalid.
    if (have_func_section && m->funcs == NULL && n_func_typeidx != 0) goto fail;
    if (!have_func_section && m->funcs != NULL) goto fail;

    free(func_typeidx);
    if (err) *err = WASM_OK;
    return m;

fail:
    free(func_typeidx);
    wasm_module_free_impl(m);
    if (err) *err = local_err;
    return NULL;
}

void wasm_module_free_impl(wasm_module_t *m) {
    if (!m) return;

    if (m->types) {
        for (uint32_t i = 0; i < m->n_types; i++) {
            free(m->types[i].params);
            free(m->types[i].results);
        }
        free(m->types);
    }

    if (m->imports) {
        for (uint32_t i = 0; i < m->n_imports; i++) {
            free(m->imports[i].module_name);
            free(m->imports[i].field_name);
        }
        free(m->imports);
    }

    if (m->funcs) {
        for (uint32_t i = 0; i < m->n_funcs; i++) {
            free(m->funcs[i].local_types);
            // funcs[i].code points into m->image (not separately owned).
            wasm_exec_free_func(&m->funcs[i]);  // cf is owned by wasm_exec.c
        }
        free(m->funcs);
    }

    free(m->globals);

    if (m->exports) {
        for (uint32_t i = 0; i < m->n_exports; i++) free(m->exports[i].name);
        free(m->exports);
    }

    // data[i].bytes points into m->image; not separately owned.
    free(m->data);

    if (m->elem) {
        for (uint32_t i = 0; i < m->n_elem; i++) free(m->elem[i].func_indices);
        free(m->elem);
    }

    free(m->image);
    free(m);
}
