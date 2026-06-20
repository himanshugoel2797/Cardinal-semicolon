// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

// UNIT A -- the frontend: a (defshader ...) reader-datum into a structured,
// untyped AST (the verifier fills the types). Also owns the arena builders, the
// public sh_compile pipeline glue, sh_free, and the introspection getters (the
// derived, drift-proof contract).
//
// The arena builders and the orchestration are REAL (stable plumbing). shf_parse
// -- the actual desugaring + banned-form rejection -- is a STUB to be implemented.
// See notes/scratch/shader-proposal-minimalist.md sections 0, 6, 9.

#include <stdlib.h>
#include <string.h>

#include "sh_internal.h"

// --- arena builders ---------------------------------------------------------

static bool grow(void **buf, uint32_t *cap, uint32_t need, size_t elem) {
    if (need <= *cap) return true;
    uint32_t ncap = *cap ? *cap * 2 : 8;
    while (ncap < need) ncap *= 2;
    void *nb = realloc(*buf, (size_t)ncap * elem);
    if (!nb) return false;
    *buf = nb;
    *cap = ncap;
    return true;
}

sh_nref sh_node_alloc(sh_program *p, sh_op op) {
    if (!grow((void **)&p->nodes, &p->cap_nodes, p->nnodes + 1, sizeof(sh_node)))
        return SH_NREF_NONE;
    sh_nref id = p->nnodes++;
    sh_node *n = &p->nodes[id];
    memset(n, 0, sizeof(*n));
    n->op = (uint16_t)op;
    n->a = n->b = n->c = SH_NREF_NONE;
    return id;
}

bool sh_aux_reserve(sh_program *p, uint32_t n, uint32_t *off) {
    if (!grow((void **)&p->aux, &p->cap_aux, p->naux + n, sizeof(uint32_t)))
        return false;
    *off = p->naux;
    for (uint32_t i = 0; i < n; i++) p->aux[p->naux++] = SH_NREF_NONE;
    return true;
}

sh_nref sh_loop_alloc(sh_program *p, uint32_t *out_index) {
    if (!grow((void **)&p->loops, &p->cap_loops, p->nloops + 1, sizeof(sh_loop)))
        return SH_NREF_NONE;
    uint32_t idx = p->nloops++;
    memset(&p->loops[idx], 0, sizeof(sh_loop));
    *out_index = idx;
    return idx;
}

// --- frontend: parse (STUB) -------------------------------------------------

sh_status shf_parse(lisp_value form, sh_program *p, sh_error *err) {
    (void)form;
    (void)p;
    return sh_set_error(err, SH_ERR_INTERNAL, -1, -1, "frontend not implemented");
}

// --- public compile pipeline ------------------------------------------------

sh_status sh_compile(lisp_value form, const sh_prim_set *prims, uint32_t flags,
                     sh_program **out_prog, sh_error *err) {
    if (!out_prog) return sh_set_error(err, SH_ERR_INTERNAL, -1, -1, "null out_prog");
    *out_prog = NULL;

    sh_program *p = calloc(1, sizeof(*p));
    if (!p) return sh_set_error(err, SH_ERR_OOM, -1, -1, "out of memory");
    p->root = SH_NREF_NONE;
    p->prims = prims;

    sh_status s = shf_parse(form, p, err);
    if (s != SH_OK) { sh_free(p); return s; }

    s = shv_verify(p, prims, flags, err);
    if (s != SH_OK) { sh_free(p); return s; }

    p->verified = true;
    *out_prog = p;
    return SH_OK;
}

sh_status sh_compile_string(const char *src, const sh_prim_set *prims, uint32_t flags,
                            sh_program **out_prog, sh_error *err) {
    if (out_prog) *out_prog = NULL;
    const char *cur = src;
    const char *end = src + strlen(src);
    const char *rerr = NULL;
    lisp_value form = lisp_read(&cur, end, &rerr);
    if (rerr) {
        int line = -1, col = -1;
        lisp_source_location(src, cur, &line, &col);
        return sh_set_error(err, SH_ERR_PARSE, line, col, "reader: %s", rerr);
    }
    return sh_compile(form, prims, flags, out_prog, err);
}

void sh_free(sh_program *p) {
    if (!p) return;
    free(p->nodes);
    free(p->aux);
    free(p->loops);
    free(p);
}

// --- introspection: the derived contract ------------------------------------

const char *sh_name(const sh_program *p) { return p ? p->name : ""; }
uint32_t sh_param_count(const sh_program *p) { return p ? p->nparams : 0; }
sh_type sh_param_type(const sh_program *p, uint32_t i) {
    if (p && i < p->nparams) return p->params[i];
    return sh_type_scalar(SH_K_VOID);
}
sh_type sh_return_type(const sh_program *p) { return p ? p->ret : sh_type_scalar(SH_K_VOID); }
uint64_t sh_static_cost(const sh_program *p) { return p ? p->cost.const_cost : 0; }
bool sh_cost_is_const(const sh_program *p) { return p ? p->cost.is_const : false; }
uint64_t sh_cost_for_args(const sh_program *p, const sh_value *args, uint32_t argc) {
    return shi_cost_for_args(p, args, argc);
}
