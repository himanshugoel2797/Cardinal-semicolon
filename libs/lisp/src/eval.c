// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

// A Scheme-inspired Lisp evaluator built as an explicit-stack CEK abstract
// machine: the execution state (control expr, environment, accumulator, and a
// heap-linked chain of continuation frames) lives in a lisp_ctx_t heap object
// rather than on the C stack. This lets a computation be suspended at a safe
// point (when a per-slice reduction budget runs out), resumed later, and traced
// precisely by the GC -- the substrate the process model needs (a "process" is a
// context). lisp_eval / lisp_apply are thin wrappers that drive a context to
// completion, preserving their original synchronous contract.
//
// Tail calls cost O(1) continuation frames (the analogue of the old `goto tail`):
// a call in tail position pushes no frame. Deep NON-tail recursion grows the heap
// chain, not the C stack. Special forms: quote, quasiquote, if, define, lambda,
// let/let*/letrec/named-let, begin, set!, cond, and, or, when, unless, while,
// case. The common derived forms are kept as interpreter special cases (cheap,
// and a future JIT can recognize them directly) rather than as a macro engine.
// Objects are GC-allocated (gc.c).
//
// Deliberately NOT included (cut to keep the substrate small -- see
// notes/core/lisp-substrate.md): syntax-rules macros and call/cc / the
// exception system. They can return if a concrete need (e.g. a driver DSL)
// arises.

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "internal.h"  // env/closure/primitive/kont/ctx layouts + lisp_gc_alloc
#include "lisp.h"

#define MAX_ARGS 64  // cap on call arity (and rest-arg spread); raised with the VM.

// An effectively-unbounded reduction budget for the synchronous wrappers (which
// run to completion and never suspend). Not INT64_MAX: the freestanding stdint.h
// in common/ does not define the limit macros.
#define CTX_BUDGET_UNBOUNDED ((int64_t)0x7fffffffffffffffLL)

// --- Small helpers ----------------------------------------------------------

static lisp_value fail(const char **err, const char *msg) {
    if (err != NULL)
        *err = msg;
    return LISP_UNDEF;
}

// Mutate a pair's car/cdr. Used only on evaluator-owned structures (env
// bindings), never on user data -- the language model keeps pairs immutable.
static void set_cdr(lisp_value pair, lisp_value v) { ((lisp_pair *)lisp_obj(pair))->cdr = v; }

// --- Constructors -----------------------------------------------------------

lisp_value lisp_make_closure(lisp_value params, lisp_value body, lisp_value env) {
    lisp_closure_t *c = (lisp_closure_t *)lisp_gc_alloc(sizeof(lisp_closure_t));
    if (c == NULL)
        return LISP_UNDEF;
    c->h.header = LISP_MK_HEADER(LISP_OBJ_CLOSURE, 0);
    c->params = params;
    c->body = body;
    c->env = env;
    return lisp_from_obj(c);
}

lisp_value lisp_make_primitive(lisp_primitive_fn fn, const char *name) {
    lisp_prim_t *p = (lisp_prim_t *)lisp_gc_alloc(sizeof(lisp_prim_t));
    if (p == NULL)
        return LISP_UNDEF;
    p->h.header = LISP_MK_HEADER(LISP_OBJ_PRIMITIVE, 0);
    p->fn = fn;
    // Initialize name to a non-pointer BEFORE the nested allocation below: that
    // alloc can trigger a GC which would trace this (already-listed) object and
    // read p->name. LISP_UNDEF is an immediate, so trace() ignores it.
    p->name = LISP_UNDEF;
    p->name = lisp_make_symbol(name, strlen(name));
    if (p->name == LISP_UNDEF)
        return LISP_UNDEF;  // p is GC-tracked; the collector reclaims it
    return lisp_from_obj(p);
}

// --- Environments -----------------------------------------------------------

// Buckets in the top-level frame's hash table (power of two). The global frame
// holds ~150 bindings; 256 buckets keep chains near length 1.
#define GLOBAL_ENV_BUCKETS 256

// The hash bucket for `sym` in a `table`-backed frame. Symbols are interned and
// carry their name hash, so this is a field read + mask.
static inline size_t env_bucket(lisp_value table, lisp_value sym) {
    return (size_t)((lisp_named *)lisp_obj(sym))->hash & (lisp_vector_length(table) - 1);
}

lisp_value lisp_make_env(lisp_value parent) {
    lisp_env_t *e = (lisp_env_t *)lisp_gc_alloc(sizeof(lisp_env_t));
    if (e == NULL)
        return LISP_UNDEF;
    e->h.header = LISP_MK_HEADER(LISP_OBJ_ENV, 0);
    e->parent = parent;
    e->bindings = LISP_EMPTY;
    e->table = LISP_EMPTY;  // set before the nested alloc below can trigger a GC
    // The top-level frame (no parent) gathers every primitive/prelude/global
    // define -- hundreds of bindings looked up on every global reference -- so
    // back it with a hash table for O(1) lookup. Per-call frames are tiny and
    // stay assoc lists (no per-call hash allocation).
    if (parent == LISP_EMPTY) {
        lisp_value t = lisp_make_vector(GLOBAL_ENV_BUCKETS, LISP_EMPTY);
        if (t != LISP_UNDEF)
            e->table = t;  // non-moving GC: e stays valid across the alloc; OOM -> assoc list
    }
    return lisp_from_obj(e);
}

// Find the (sym . val) binding cell for `sym` in this single frame, or LISP_EMPTY.
// Both the binding key and `sym` are interned symbols (every symbol-construction
// path goes through intern()), so equality is pointer identity -- no need to
// compare names. This is the interpreter's hottest loop (profiling: variable
// lookup dominates), so a general name compare's length+memcmp per non-matching
// binding is pure waste and is avoided. A table-backed (top-level) frame scans
// only one hash bucket; a plain frame scans its (short) assoc list.
static lisp_value frame_find(lisp_value env, lisp_value sym) {
    lisp_env_t *e = (lisp_env_t *)lisp_obj(env);
    lisp_value chain = e->table != LISP_EMPTY
                           ? lisp_vector_ref(e->table, env_bucket(e->table, sym))
                           : e->bindings;
    while (lisp_is_pair(chain)) {
        lisp_value cell = lisp_car(chain);
        if (lisp_is_pair(cell) && lisp_car(cell) == sym)
            return cell;
        chain = lisp_cdr(chain);
    }
    return LISP_EMPTY;
}

void lisp_env_define(lisp_value env, lisp_value sym, lisp_value val) {
    lisp_value cell = frame_find(env, sym);
    if (lisp_is_pair(cell)) {
        set_cdr(cell, val);  // redefining in the same frame overwrites
        return;
    }
    lisp_env_t *e = (lisp_env_t *)lisp_obj(env);
    lisp_value entry = lisp_cons(sym, val);  // (sym . val); held live across the next cons
    if (e->table != LISP_EMPTY) {
        size_t b = env_bucket(e->table, sym);
        // Prepend the entry to bucket b. The table is evaluator-owned plumbing, so
        // its slot is mutated in place (the language never sees this vector).
        ((lisp_vector *)lisp_obj(e->table))->items[b] =
            lisp_cons(entry, lisp_vector_ref(e->table, b));
    } else {
        e->bindings = lisp_cons(entry, e->bindings);
    }
}

bool lisp_env_lookup(lisp_value env, lisp_value sym, lisp_value *out) {
    while (lisp_is_objtype(env, LISP_OBJ_ENV)) {
        lisp_value cell = frame_find(env, sym);
        if (lisp_is_pair(cell)) {
            if (out != NULL)
                *out = lisp_cdr(cell);
            return true;
        }
        env = ((lisp_env_t *)lisp_obj(env))->parent;
    }
    return false;
}

bool lisp_env_set(lisp_value env, lisp_value sym, lisp_value val) {
    while (lisp_is_objtype(env, LISP_OBJ_ENV)) {
        lisp_value cell = frame_find(env, sym);
        if (lisp_is_pair(cell)) {
            set_cdr(cell, val);
            return true;
        }
        env = ((lisp_env_t *)lisp_obj(env))->parent;
    }
    return false;
}

// --- Keyword matching by name -----------------------------------------------
// A by-name symbol test for the syntactic keywords that are NOT top-level forms
// and so carry no form_id: else (cond/case) and unquote/unquote-splicing
// (quasiquote). The hot form dispatch uses the cached form_id instead.

static bool is_form(lisp_value sym, const char *name) {
    if (!lisp_is_symbol(sym))
        return false;
    size_t len = lisp_named_len(sym);
    return len == strlen(name) && memcmp(lisp_named_name(sym), name, len) == 0;
}

// --- Special-form dispatch ids ----------------------------------------------
// The head of a form is dispatched by a small integer cached on its interned
// symbol (lisp_named.form_id) rather than a linear is_form name cascade: an
// ordinary application used to fall through *every* special-form check before
// reaching the apply path. Symbols are interned, so a given form name is always
// the one canonical object; tagging it once makes the head -> id lookup O(1) and
// the dispatch a jump table. form_id 0 (SF_NONE) means "ordinary symbol".
typedef enum {
    SF_NONE = 0,
    SF_QUOTE, SF_QUASIQUOTE, SF_IF, SF_DEFINE, SF_LAMBDA, SF_SET, SF_BEGIN,
    SF_LET, SF_LET_STAR, SF_LETREC, SF_AND, SF_OR, SF_COND, SF_WHEN, SF_UNLESS,
    SF_WHILE, SF_CASE, SF_DEFINE_MODULE, SF_IMPORT,
} special_form_id;

static const struct {
    const char *name;
    uint8_t id;
} SPECIAL_FORMS[] = {
    {"quote", SF_QUOTE},     {"quasiquote", SF_QUASIQUOTE},
    {"if", SF_IF},           {"define", SF_DEFINE},
    {"lambda", SF_LAMBDA},   {"set!", SF_SET},
    {"begin", SF_BEGIN},     {"let", SF_LET},
    {"let*", SF_LET_STAR},   {"letrec", SF_LETREC},
    {"and", SF_AND},         {"or", SF_OR},
    {"cond", SF_COND},       {"when", SF_WHEN},
    {"unless", SF_UNLESS},   {"while", SF_WHILE},
    {"case", SF_CASE},       {"define-module", SF_DEFINE_MODULE},
    {"import", SF_IMPORT},
};

// Tag every special form's interned symbol with its dispatch id. Called from
// lisp_default_env before the prelude is evaluated, so the tags are in place
// before any form is dispatched. Idempotent (re-tagging writes the same id) and
// single-core at boot, so it never races the post-boot frozen shared heap.
static void lisp_init_special_forms(void) {
    for (size_t i = 0; i < sizeof(SPECIAL_FORMS) / sizeof(SPECIAL_FORMS[0]); i++) {
        lisp_value s =
            lisp_make_symbol(SPECIAL_FORMS[i].name, strlen(SPECIAL_FORMS[i].name));
        if (s != LISP_UNDEF)
            ((lisp_named *)lisp_obj(s))->form_id = SPECIAL_FORMS[i].id;
    }
}

// The special-form id of a head symbol (caller ensures lisp_is_symbol).
static inline special_form_id head_form_id(lisp_value head) {
    return (special_form_id)((lisp_named *)lisp_obj(head))->form_id;
}

// --- Parameter binding ------------------------------------------------------

// Bind a closure's parameter list to evaluated args in a fresh child env.
// Returns the new env, or LISP_UNDEF (+*err) on arity mismatch / OOM.
static lisp_value bind_params(lisp_value params, lisp_value *args, int argc,
                              lisp_value parent_env, const char **err) {
    lisp_value env = lisp_make_env(parent_env);
    if (env == LISP_UNDEF)
        return fail(err, "out of memory");
    // A fresh call frame parents on the closure's env, so it is never the
    // table-backed top-level frame -- prepend bindings straight onto its assoc
    // list. This skips lisp_env_define's redefine-check (a frame_find scan that,
    // on a frame being filled left-to-right, is wasted work growing with arity).
    lisp_env_t *e = (lisp_env_t *)lisp_obj(env);
    int i = 0;
    while (lisp_is_pair(params)) {
        if (i >= argc)
            return fail(err, "too few arguments");
        lisp_value cell = lisp_cons(lisp_car(params), args[i]);
        if (cell == LISP_UNDEF)
            return fail(err, "out of memory");
        lisp_value nb = lisp_cons(cell, e->bindings);
        if (nb == LISP_UNDEF)
            return fail(err, "out of memory");
        e->bindings = nb;
        params = lisp_cdr(params);
        i++;
    }
    // A trailing symbol (dotted tail, or a bare symbol param list) is a rest
    // parameter: it captures the remaining args as a list. (lambda args ...) and
    // (lambda (a b . rest) ...) both land here.
    if (lisp_is_symbol(params)) {
        lisp_value rest = LISP_EMPTY;
        for (int j = argc - 1; j >= i; j--) {
            lisp_value cell = lisp_cons(args[j], rest);
            if (cell == LISP_UNDEF)
                return fail(err, "out of memory");
            rest = cell;
        }
        lisp_value cell = lisp_cons(params, rest);
        if (cell == LISP_UNDEF)
            return fail(err, "out of memory");
        lisp_value nb = lisp_cons(cell, e->bindings);
        if (nb == LISP_UNDEF)
            return fail(err, "out of memory");
        e->bindings = nb;
        return env;
    }
    if (!lisp_is_empty(params))
        return fail(err, "malformed parameter list");
    if (i != argc)
        return fail(err, "too many arguments");
    return env;
}

// --- Quasiquote -------------------------------------------------------------
// qq_expand stays recursive and atomic: a template's nesting depth is bounded by
// source text (tiny), and its embedded evaluations go through lisp_eval (which
// spins a nested completion-driven machine). A future driver DSL could lift it
// into continuation frames; there is no need yet.

// Build (name x) for the nested-quasiquote rebuild cases.
static lisp_value qq_wrap(const char *name, lisp_value x, const char **err) {
    lisp_value sym = lisp_make_symbol(name, strlen(name));
    lisp_value rest = lisp_cons(x, LISP_EMPTY);
    if (sym == LISP_UNDEF || rest == LISP_UNDEF)
        return fail(err, "out of memory");
    lisp_value f = lisp_cons(sym, rest);
    return f == LISP_UNDEF ? fail(err, "out of memory") : f;
}

// Copy list `lst`, appending `tail` as the final cdr (for unquote-splicing).
static lisp_value qq_append(lisp_value lst, lisp_value tail, const char **err) {
    lisp_value head = LISP_EMPTY, last = LISP_EMPTY;
    while (lisp_is_pair(lst)) {
        lisp_value cell = lisp_cons(lisp_car(lst), LISP_EMPTY);
        if (cell == LISP_UNDEF)
            return fail(err, "out of memory");
        if (head == LISP_EMPTY)
            head = cell;
        else
            set_cdr(last, cell);
        last = cell;
        lst = lisp_cdr(lst);
    }
    if (!lisp_is_empty(lst))
        return fail(err, "unquote-splicing: not a proper list");
    if (head == LISP_EMPTY)
        return tail;
    set_cdr(last, tail);
    return head;
}

// Expand a quasiquote template at the given nesting depth. depth 1 means an
// unquote here is live; deeper unquotes are rebuilt with their depth reduced.
static lisp_value qq_expand(lisp_value t, int depth, lisp_value env, const char **err) {
    if (!lisp_is_pair(t))
        return t;  // atoms (and, for now, vectors) are literal
    lisp_value head = lisp_car(t);

    if (is_form(head, "unquote")) {
        if (!lisp_is_pair(lisp_cdr(t)) || !lisp_is_empty(lisp_cdr(lisp_cdr(t))))
            return fail(err, "malformed unquote");
        lisp_value x = lisp_car(lisp_cdr(t));
        if (depth == 1)
            return lisp_eval(x, env, err);
        lisp_value inner = qq_expand(x, depth - 1, env, err);
        if (inner == LISP_UNDEF && err != NULL && *err != NULL)
            return LISP_UNDEF;
        return qq_wrap("unquote", inner, err);
    }
    if (is_form(head, "quasiquote")) {
        if (!lisp_is_pair(lisp_cdr(t)))
            return fail(err, "malformed quasiquote");
        lisp_value inner = qq_expand(lisp_car(lisp_cdr(t)), depth + 1, env, err);
        if (inner == LISP_UNDEF && err != NULL && *err != NULL)
            return LISP_UNDEF;
        return qq_wrap("quasiquote", inner, err);
    }
    // (unquote-splicing x) in car position splices into the result list.
    if (lisp_is_pair(head) && is_form(lisp_car(head), "unquote-splicing")) {
        if (!lisp_is_pair(lisp_cdr(head)) || !lisp_is_empty(lisp_cdr(lisp_cdr(head))))
            return fail(err, "malformed unquote-splicing");
        lisp_value x = lisp_car(lisp_cdr(head));
        lisp_value rest = qq_expand(lisp_cdr(t), depth, env, err);
        if (rest == LISP_UNDEF && err != NULL && *err != NULL)
            return LISP_UNDEF;
        if (depth == 1) {
            lisp_value spliced = lisp_eval(x, env, err);
            if (spliced == LISP_UNDEF && err != NULL && *err != NULL)
                return LISP_UNDEF;
            return qq_append(spliced, rest, err);
        }
        lisp_value inner = qq_expand(x, depth - 1, env, err);
        if (inner == LISP_UNDEF && err != NULL && *err != NULL)
            return LISP_UNDEF;
        lisp_value newhead = qq_wrap("unquote-splicing", inner, err);
        if (newhead == LISP_UNDEF && err != NULL && *err != NULL)
            return LISP_UNDEF;
        lisp_value cell = lisp_cons(newhead, rest);
        return cell == LISP_UNDEF ? fail(err, "out of memory") : cell;
    }
    // Default: rebuild car and cdr.
    lisp_value a = qq_expand(head, depth, env, err);
    if (a == LISP_UNDEF && err != NULL && *err != NULL)
        return LISP_UNDEF;
    lisp_value d = qq_expand(lisp_cdr(t), depth, env, err);
    if (d == LISP_UNDEF && err != NULL && *err != NULL)
        return LISP_UNDEF;
    lisp_value cell = lisp_cons(a, d);
    return cell == LISP_UNDEF ? fail(err, "out of memory") : cell;
}

// --- The CEK machine --------------------------------------------------------
//
// Continuation-frame kinds. The kind is stored in the kont header's aux byte and
// selects how the frame's a/b/c slots are read (see each step_applyk case).
enum {
    K_EVAL_OP,     // a=arg-exprs, env=eval env. Awaiting the operator value.
    K_EVAL_ARGS,   // a=operator, b=reversed evaluated args, c=remaining arg-exprs.
    K_IF,          // a=then-expr, b=else-branch tail (list, maybe empty).
    K_SEQ,         // a=remaining body forms (>=1; the last is in tail position).
    K_DEFINE,      // a=target symbol, env=defining env.
    K_SET,         // a=target symbol, env=env to mutate.
    K_AND,         // a=remaining conjuncts (>=1).
    K_OR,          // a=remaining disjuncts (>=1).
    K_COND,        // a=clause list (its first clause's test is being evaluated).
    K_CASE,        // a=clause list. Awaiting the key value.
    K_WHEN,        // a=body forms, b=wanted truthiness (#t for when, #f for unless).
    K_WHILE_TEST,  // a=test expr, b=body forms. Awaiting the test value.
    K_WHILE_BODY,  // a=test expr, b=body forms, c=remaining body forms this pass.
    K_BIND,        // a=remaining bindings, b=new env, c=body, env=init-eval env.
};

// let-family kinds for let_start.
enum { LET_PLAIN, LET_STAR, LET_REC };

static lisp_kont_t *kont(lisp_value v) { return (lisp_kont_t *)lisp_obj(v); }

static void ctx_error(lisp_ctx_t *cx, const char *msg) {
    cx->err = msg;
    cx->status = LISP_CTX_ERROR;
}

// Push a fresh continuation frame onto cx->kont. Returns false (and signals an
// error on cx) on OOM. The a/b/c/env args are kept alive across the allocation by
// the conservative stack scan (they are live C locals in the caller).
static bool kont_push(lisp_ctx_t *cx, int kind, lisp_value env,
                      lisp_value a, lisp_value b, lisp_value c) {
    lisp_kont_t *k = (lisp_kont_t *)lisp_gc_alloc(sizeof(lisp_kont_t));
    if (k == NULL) {
        ctx_error(cx, "out of memory");
        return false;
    }
    k->h.header = LISP_MK_HEADER(LISP_OBJ_KONT, kind);
    k->next = cx->kont;
    k->env = env;
    k->a = a;
    k->b = b;
    k->c = c;
    cx->kont = lisp_from_obj(k);
    return true;
}

// Begin evaluating a body (a list of forms) in cx->env: all but the last for
// effect, the last in tail position (no frame). An empty body yields unspecified.
static void start_body(lisp_ctx_t *cx, lisp_value forms) {
    if (!lisp_is_pair(forms)) {
        cx->accum = LISP_UNDEF;
        cx->status = LISP_CTX_APPLY;
        return;
    }
    if (lisp_is_pair(lisp_cdr(forms))) {
        if (!kont_push(cx, K_SEQ, cx->env, lisp_cdr(forms), LISP_EMPTY, LISP_EMPTY))
            return;
    }
    cx->control = lisp_car(forms);
    cx->status = LISP_CTX_EVAL;
}

// Apply an operator to an evaluated argument array. Charges one reduction. A
// closure's body runs in tail position (the K_EVAL_* frames are already popped),
// so a tail self-call keeps the continuation depth flat.
static void do_call(lisp_ctx_t *cx, lisp_value op, lisp_value *args, int argc) {
    cx->budget--;  // a call is a reduction (a safe point / budget charge)
    if (lisp_is_objtype(op, LISP_OBJ_PRIMITIVE)) {
        lisp_prim_t *p = (lisp_prim_t *)lisp_obj(op);
        const char *e = NULL;
        lisp_value r = p->fn(args, argc, &e);
        if (r == LISP_UNDEF && e != NULL) {
            ctx_error(cx, e);
            return;
        }
        cx->accum = r;
        cx->status = LISP_CTX_APPLY;
        return;
    }
    if (lisp_is_objtype(op, LISP_OBJ_CLOSURE)) {
        lisp_closure_t *c = (lisp_closure_t *)lisp_obj(op);
        const char *e = NULL;
        lisp_value newenv = bind_params(c->params, args, argc, c->env, &e);
        if (newenv == LISP_UNDEF) {
            ctx_error(cx, e != NULL ? e : "bad application");
            return;
        }
        cx->env = newenv;
        start_body(cx, c->body);  // tail
        return;
    }
    ctx_error(cx, "attempt to call a non-procedure");
}

// --- Special-form starters (the EVAL-state expansions) ----------------------

static void and_start(lisp_ctx_t *cx, lisp_value forms) {
    if (!lisp_is_pair(forms)) {  // (and) => #t
        cx->accum = LISP_TRUE;
        cx->status = LISP_CTX_APPLY;
        return;
    }
    if (lisp_is_pair(lisp_cdr(forms))) {
        if (!kont_push(cx, K_AND, cx->env, lisp_cdr(forms), LISP_EMPTY, LISP_EMPTY))
            return;
    }
    cx->control = lisp_car(forms);  // single conjunct is in tail position
    cx->status = LISP_CTX_EVAL;
}

static void or_start(lisp_ctx_t *cx, lisp_value forms) {
    if (!lisp_is_pair(forms)) {  // (or) => #f
        cx->accum = LISP_FALSE;
        cx->status = LISP_CTX_APPLY;
        return;
    }
    if (lisp_is_pair(lisp_cdr(forms))) {
        if (!kont_push(cx, K_OR, cx->env, lisp_cdr(forms), LISP_EMPTY, LISP_EMPTY))
            return;
    }
    cx->control = lisp_car(forms);  // single disjunct is in tail position
    cx->status = LISP_CTX_EVAL;
}

// Process cond clauses until one's test must be evaluated (or an else/no clause
// resolves immediately). Does not recurse over clauses in C -- a clause whose
// test needs evaluating parks in a K_COND frame and the machine resumes here.
static void cond_start(lisp_ctx_t *cx, lisp_value clauses) {
    if (!lisp_is_pair(clauses)) {
        cx->accum = LISP_UNDEF;  // no clause matched
        cx->status = LISP_CTX_APPLY;
        return;
    }
    lisp_value clause = lisp_car(clauses);
    if (!lisp_is_pair(clause)) {
        ctx_error(cx, "malformed cond clause");
        return;
    }
    lisp_value test = lisp_car(clause);
    if (is_form(test, "else")) {
        lisp_value body = lisp_cdr(clause);
        if (!lisp_is_pair(body)) {  // (else) with no body -> #t (matches old testval)
            cx->accum = LISP_TRUE;
            cx->status = LISP_CTX_APPLY;
            return;
        }
        start_body(cx, body);  // tail
        return;
    }
    if (!kont_push(cx, K_COND, cx->env, clauses, LISP_EMPTY, LISP_EMPTY))
        return;
    cx->control = test;
    cx->status = LISP_CTX_EVAL;
}

static void when_start(lisp_ctx_t *cx, lisp_value rest, bool want) {
    if (!lisp_is_pair(rest)) {
        ctx_error(cx, "malformed when/unless");
        return;
    }
    if (!kont_push(cx, K_WHEN, cx->env, lisp_cdr(rest), want ? LISP_TRUE : LISP_FALSE,
                   LISP_EMPTY))
        return;
    cx->control = lisp_car(rest);  // the test
    cx->status = LISP_CTX_EVAL;
}

static void while_start(lisp_ctx_t *cx, lisp_value rest) {
    if (!lisp_is_pair(rest)) {
        ctx_error(cx, "malformed while");
        return;
    }
    lisp_value test = lisp_car(rest);
    if (!kont_push(cx, K_WHILE_TEST, cx->env, test, lisp_cdr(rest), LISP_EMPTY))
        return;
    cx->control = test;
    cx->status = LISP_CTX_EVAL;
}

static void case_start(lisp_ctx_t *cx, lisp_value rest) {
    if (!lisp_is_pair(rest)) {
        ctx_error(cx, "malformed case");
        return;
    }
    if (!kont_push(cx, K_CASE, cx->env, lisp_cdr(rest), LISP_EMPTY, LISP_EMPTY))
        return;
    cx->control = lisp_car(rest);  // the key
    cx->status = LISP_CTX_EVAL;
}

// let / let* / letrec share one frame (K_BIND). They differ only in which env the
// inits evaluate in: outer for let, the new env for let*/letrec; letrec also
// pre-binds every name to unspecified first (so mutual recursion works).
static void let_start(lisp_ctx_t *cx, lisp_value rest, int kind) {
    if (!lisp_is_pair(rest)) {
        ctx_error(cx, "malformed let / let* / letrec");
        return;
    }
    lisp_value outer = cx->env;
    lisp_value binds = lisp_car(rest);
    lisp_value body = lisp_cdr(rest);
    if (!lisp_is_pair(binds) && !lisp_is_empty(binds)) {
        ctx_error(cx, "malformed let: bindings must be a list");
        return;
    }
    lisp_value newenv = lisp_make_env(outer);
    if (newenv == LISP_UNDEF) {
        ctx_error(cx, "out of memory");
        return;
    }
    if (kind == LET_REC) {
        for (lisp_value b = binds; lisp_is_pair(b); b = lisp_cdr(b)) {
            lisp_value bind = lisp_car(b);
            if (!lisp_is_pair(bind)) {
                ctx_error(cx, "malformed letrec binding");
                return;
            }
            lisp_env_define(newenv, lisp_car(bind), LISP_UNDEF);
        }
    }
    if (!lisp_is_pair(binds)) {  // no bindings: straight to the body
        cx->env = newenv;
        start_body(cx, body);
        return;
    }
    lisp_value b0 = lisp_car(binds);
    if (!lisp_is_pair(b0) || !lisp_is_pair(lisp_cdr(b0))) {
        ctx_error(cx, "malformed let binding");
        return;
    }
    lisp_value evalenv = (kind == LET_PLAIN) ? outer : newenv;
    if (!kont_push(cx, K_BIND, evalenv, binds, newenv, body))
        return;
    cx->env = evalenv;
    cx->control = lisp_car(lisp_cdr(b0));  // first init
    cx->status = LISP_CTX_EVAL;
}

// Named let: (let name ((v init)...) body...). Desugar to applying a fresh
// recursive closure (bound to `name` in its own env) to the evaluated inits --
// reusing the ordinary K_EVAL_ARGS argument machinery. Inits evaluate in the
// outer env.
static void namedlet_start(lisp_ctx_t *cx, lisp_value rest) {
    lisp_value name = lisp_car(rest);
    lisp_value rest2 = lisp_cdr(rest);
    if (!lisp_is_pair(rest2)) {
        ctx_error(cx, "malformed named let");
        return;
    }
    lisp_value binds = lisp_car(rest2);
    if (!lisp_is_pair(binds) && !lisp_is_empty(binds)) {
        ctx_error(cx, "malformed named let: bindings must be a list");
        return;
    }
    lisp_value body = lisp_cdr(rest2);
    lisp_value params = LISP_EMPTY, ptail = LISP_EMPTY;
    lisp_value inits = LISP_EMPTY, itail = LISP_EMPTY;
    for (lisp_value b = binds; lisp_is_pair(b); b = lisp_cdr(b)) {
        lisp_value bd = lisp_car(b);
        if (!lisp_is_pair(bd) || !lisp_is_pair(lisp_cdr(bd))) {
            ctx_error(cx, "malformed named let binding");
            return;
        }
        lisp_value pc = lisp_cons(lisp_car(bd), LISP_EMPTY);
        lisp_value ic = lisp_cons(lisp_car(lisp_cdr(bd)), LISP_EMPTY);
        if (pc == LISP_UNDEF || ic == LISP_UNDEF) {
            ctx_error(cx, "out of memory");
            return;
        }
        if (params == LISP_EMPTY)
            params = pc;
        else
            set_cdr(ptail, pc);
        ptail = pc;
        if (inits == LISP_EMPTY)
            inits = ic;
        else
            set_cdr(itail, ic);
        itail = ic;
    }
    lisp_value loopenv = lisp_make_env(cx->env);
    if (loopenv == LISP_UNDEF) {
        ctx_error(cx, "out of memory");
        return;
    }
    lisp_value clo = lisp_make_closure(params, body, loopenv);
    if (clo == LISP_UNDEF) {
        ctx_error(cx, "out of memory");
        return;
    }
    lisp_env_define(loopenv, name, clo);
    if (!lisp_is_pair(inits)) {  // no loop variables
        do_call(cx, clo, NULL, 0);
        return;
    }
    if (!kont_push(cx, K_EVAL_ARGS, cx->env, clo, LISP_EMPTY, lisp_cdr(inits)))
        return;
    cx->control = lisp_car(inits);  // inits evaluate in the (current) outer env
    cx->status = LISP_CTX_EVAL;
}

// --- The two machine states -------------------------------------------------

// EVAL state: decompose cx->control. Either it finishes immediately (a value),
// reassigns control for a tail step, or pushes one frame and descends into a
// subexpression.
static void step_eval(lisp_ctx_t *cx) {
    lisp_value e = cx->control;

    // Symbols: variable reference.
    if (lisp_is_symbol(e)) {
        lisp_value v;
        if (!lisp_env_lookup(cx->env, e, &v)) {
            ctx_error(cx, "unbound variable");
            return;
        }
        cx->accum = v;
        cx->status = LISP_CTX_APPLY;
        return;
    }
    // Self-evaluating: fixnums, booleans, chars, strings, keywords, eof, flonums.
    if (!lisp_is_pair(e)) {
        if (lisp_is_empty(e)) {
            ctx_error(cx, "cannot evaluate empty application ()");
            return;
        }
        cx->accum = e;
        cx->status = LISP_CTX_APPLY;
        return;
    }

    lisp_value head = lisp_car(e);
    lisp_value rest = lisp_cdr(e);

    if (lisp_is_symbol(head)) {
        switch (head_form_id(head)) {
        case SF_QUOTE:
            if (!lisp_is_pair(rest)) {
                ctx_error(cx, "malformed quote");
                return;
            }
            cx->accum = lisp_car(rest);
            cx->status = LISP_CTX_APPLY;
            return;
        case SF_QUASIQUOTE: {
            if (!lisp_is_pair(rest)) {
                ctx_error(cx, "malformed quasiquote");
                return;
            }
            const char *err = NULL;
            lisp_value r = qq_expand(lisp_car(rest), 1, cx->env, &err);
            if (r == LISP_UNDEF && err != NULL) {
                ctx_error(cx, err);
                return;
            }
            cx->accum = r;
            cx->status = LISP_CTX_APPLY;
            return;
        }
        case SF_IF: {
            if (!lisp_is_pair(rest) || !lisp_is_pair(lisp_cdr(rest))) {
                ctx_error(cx, "malformed if");
                return;
            }
            lisp_value branches = lisp_cdr(rest);
            if (!kont_push(cx, K_IF, cx->env, lisp_car(branches), lisp_cdr(branches),
                           LISP_EMPTY))
                return;
            cx->control = lisp_car(rest);  // the test
            cx->status = LISP_CTX_EVAL;
            return;
        }
        case SF_DEFINE: {
            if (!lisp_is_pair(rest)) {
                ctx_error(cx, "malformed define");
                return;
            }
            lisp_value target = lisp_car(rest);
            if (lisp_is_symbol(target)) {
                if (!lisp_is_pair(lisp_cdr(rest))) {
                    ctx_error(cx, "malformed define: missing value expression");
                    return;
                }
                if (!kont_push(cx, K_DEFINE, cx->env, target, LISP_EMPTY, LISP_EMPTY))
                    return;
                cx->control = lisp_car(lisp_cdr(rest));
                cx->status = LISP_CTX_EVAL;
                return;
            }
            if (lisp_is_pair(target)) {  // (define (f args...) body...)
                lisp_value name = lisp_car(target);
                if (!lisp_is_symbol(name)) {
                    ctx_error(cx, "define: function name must be a symbol");
                    return;
                }
                lisp_value fn = lisp_make_closure(lisp_cdr(target), lisp_cdr(rest), cx->env);
                if (fn == LISP_UNDEF) {
                    ctx_error(cx, "out of memory");
                    return;
                }
                lisp_env_define(cx->env, name, fn);
                cx->accum = name;
                cx->status = LISP_CTX_APPLY;
                return;
            }
            ctx_error(cx, "malformed define");
            return;
        }
        case SF_LAMBDA: {
            if (!lisp_is_pair(rest)) {
                ctx_error(cx, "malformed lambda");
                return;
            }
            lisp_value cl = lisp_make_closure(lisp_car(rest), lisp_cdr(rest), cx->env);
            if (cl == LISP_UNDEF) {
                ctx_error(cx, "out of memory");
                return;
            }
            cx->accum = cl;
            cx->status = LISP_CTX_APPLY;
            return;
        }
        case SF_SET:
            if (!lisp_is_pair(rest) || !lisp_is_pair(lisp_cdr(rest))) {
                ctx_error(cx, "malformed set!");
                return;
            }
            if (!kont_push(cx, K_SET, cx->env, lisp_car(rest), LISP_EMPTY, LISP_EMPTY))
                return;
            cx->control = lisp_car(lisp_cdr(rest));
            cx->status = LISP_CTX_EVAL;
            return;
        case SF_BEGIN:
            if (!lisp_is_pair(rest)) {  // (begin) -> unspecified
                cx->accum = LISP_UNDEF;
                cx->status = LISP_CTX_APPLY;
                return;
            }
            start_body(cx, rest);
            return;
        case SF_LET:
            if (lisp_is_pair(rest) && lisp_is_symbol(lisp_car(rest)))
                namedlet_start(cx, rest);
            else
                let_start(cx, rest, LET_PLAIN);
            return;
        case SF_LET_STAR:
            let_start(cx, rest, LET_STAR);
            return;
        case SF_LETREC:
            let_start(cx, rest, LET_REC);
            return;
        case SF_AND:
            and_start(cx, rest);
            return;
        case SF_OR:
            or_start(cx, rest);
            return;
        case SF_COND:
            cond_start(cx, rest);
            return;
        case SF_WHEN:
            when_start(cx, rest, true);
            return;
        case SF_UNLESS:
            when_start(cx, rest, false);
            return;
        case SF_WHILE:
            while_start(cx, rest);
            return;
        case SF_CASE:
            case_start(cx, rest);
            return;
        // Modules (module.c). These run synchronously to completion (they drive
        // nested evals while loading source), which is fine for a boot-time
        // configuration step; see notes/core/lisp-substrate.md.
        case SF_DEFINE_MODULE: {
            const char *err = NULL;
            lisp_value r = lisp_module_define(e, cx->env, &err);
            if (err != NULL) {
                ctx_error(cx, err);
                return;
            }
            cx->accum = r;
            cx->status = LISP_CTX_APPLY;
            return;
        }
        case SF_IMPORT: {
            const char *err = NULL;
            lisp_value r = lisp_module_import(e, cx->env, &err);
            if (err != NULL) {
                ctx_error(cx, err);
                return;
            }
            cx->accum = r;
            cx->status = LISP_CTX_APPLY;
            return;
        }
        case SF_NONE:
            break;  // ordinary symbol -> procedure application below
        }
    }

    // Procedure application. The operator is almost always a symbol (a variable
    // reference), which needs no sub-evaluation -- look it up inline and skip the
    // K_EVAL_OP continuation frame entirely (one fewer allocation per call). A
    // compound operator expression takes the general K_EVAL_OP path.
    if (lisp_is_symbol(head)) {
        lisp_value op;
        if (!lisp_env_lookup(cx->env, head, &op)) {
            ctx_error(cx, "unbound variable");
            return;
        }
        if (!lisp_is_pair(rest)) {
            if (!lisp_is_empty(rest)) {
                ctx_error(cx, "improper argument list");
                return;
            }
            do_call(cx, op, NULL, 0);  // zero-argument call
            return;
        }
        if (!kont_push(cx, K_EVAL_ARGS, cx->env, op, LISP_EMPTY, lisp_cdr(rest)))
            return;
        cx->control = lisp_car(rest);
        cx->status = LISP_CTX_EVAL;
        return;
    }
    // Compound operator: evaluate it first (K_EVAL_OP), then the operands.
    if (!kont_push(cx, K_EVAL_OP, cx->env, rest, LISP_EMPTY, LISP_EMPTY))
        return;
    cx->control = head;
    cx->status = LISP_CTX_EVAL;
}

// APPLY state: a value (cx->accum) has arrived; hand it to the top frame.
static void step_applyk(lisp_ctx_t *cx) {
    if (cx->kont == LISP_EMPTY) {
        cx->status = LISP_CTX_DONE;
        return;
    }
    lisp_kont_t *k = kont(cx->kont);
    lisp_value V = cx->accum;

    switch ((int)LISP_HDR_AUX(&k->h)) {
        case K_EVAL_OP: {
            lisp_value op = V;
            lisp_value argexprs = k->a;
            lisp_value evalenv = k->env;
            cx->kont = k->next;  // pop
            if (!lisp_is_pair(argexprs)) {
                if (!lisp_is_empty(argexprs)) {
                    ctx_error(cx, "improper argument list");
                    return;
                }
                do_call(cx, op, NULL, 0);  // zero-argument call
                return;
            }
            if (!kont_push(cx, K_EVAL_ARGS, evalenv, op, LISP_EMPTY, lisp_cdr(argexprs)))
                return;
            cx->env = evalenv;
            cx->control = lisp_car(argexprs);
            cx->status = LISP_CTX_EVAL;
            return;
        }
        case K_EVAL_ARGS: {
            lisp_value op = k->a;
            lisp_value done = k->b;  // already-evaluated args, in reverse order
            lisp_value todo = k->c;
            if (lisp_is_pair(todo)) {
                lisp_value nd = lisp_cons(V, done);
                if (nd == LISP_UNDEF) {
                    ctx_error(cx, "out of memory");
                    return;
                }
                k->b = nd;
                k->c = lisp_cdr(todo);
                cx->env = k->env;
                cx->control = lisp_car(todo);
                cx->status = LISP_CTX_EVAL;
                return;
            }
            if (!lisp_is_empty(todo)) {
                ctx_error(cx, "improper argument list");
                return;
            }
            // Last argument is V; flatten (done reversed) + V into an array.
            lisp_value args[MAX_ARGS];
            int n = 1;
            for (lisp_value p = done; lisp_is_pair(p); p = lisp_cdr(p))
                n++;
            if (n > MAX_ARGS) {
                ctx_error(cx, "too many arguments");
                return;
            }
            int argc = n;
            args[--n] = V;
            for (lisp_value p = done; lisp_is_pair(p); p = lisp_cdr(p))
                args[--n] = lisp_car(p);
            cx->kont = k->next;  // pop before the (possibly tail) call
            do_call(cx, op, args, argc);
            return;
        }
        case K_IF: {
            lisp_value thenb = k->a;
            lisp_value elseb = k->b;
            cx->kont = k->next;  // pop: the chosen branch runs in tail position
            cx->env = k->env;
            if (lisp_truthy(V)) {
                cx->control = thenb;
                cx->status = LISP_CTX_EVAL;
            } else if (lisp_is_pair(elseb)) {
                cx->control = lisp_car(elseb);
                cx->status = LISP_CTX_EVAL;
            } else {  // (if #f x) with no else -> unspecified
                cx->accum = LISP_UNDEF;
                cx->status = LISP_CTX_APPLY;
            }
            return;
        }
        case K_SEQ: {
            lisp_value forms = k->a;  // remaining forms, >=1; V (prev result) dropped
            cx->env = k->env;
            if (lisp_is_pair(lisp_cdr(forms))) {
                k->a = lisp_cdr(forms);  // keep frame, advance
            } else {
                cx->kont = k->next;  // pop before the last form (tail)
            }
            cx->control = lisp_car(forms);
            cx->status = LISP_CTX_EVAL;
            return;
        }
        case K_DEFINE: {
            lisp_value name = k->a;
            lisp_env_define(k->env, name, V);
            cx->kont = k->next;
            cx->accum = name;  // define returns the bound symbol
            cx->status = LISP_CTX_APPLY;
            return;
        }
        case K_SET: {
            if (!lisp_env_set(k->env, k->a, V)) {
                ctx_error(cx, "set! on unbound variable");
                return;
            }
            cx->kont = k->next;
            cx->accum = LISP_UNDEF;
            cx->status = LISP_CTX_APPLY;
            return;
        }
        case K_AND: {
            if (!lisp_truthy(V)) {  // short-circuit
                cx->kont = k->next;
                cx->accum = LISP_FALSE;
                cx->status = LISP_CTX_APPLY;
                return;
            }
            lisp_value forms = k->a;  // remaining, >=1
            cx->env = k->env;
            if (lisp_is_pair(lisp_cdr(forms))) {
                k->a = lisp_cdr(forms);
            } else {
                cx->kont = k->next;  // last conjunct is in tail position
            }
            cx->control = lisp_car(forms);
            cx->status = LISP_CTX_EVAL;
            return;
        }
        case K_OR: {
            if (lisp_truthy(V)) {  // return the first truthy value
                cx->kont = k->next;
                cx->accum = V;
                cx->status = LISP_CTX_APPLY;
                return;
            }
            lisp_value forms = k->a;
            cx->env = k->env;
            if (lisp_is_pair(lisp_cdr(forms))) {
                k->a = lisp_cdr(forms);
            } else {
                cx->kont = k->next;  // last disjunct is in tail position
            }
            cx->control = lisp_car(forms);
            cx->status = LISP_CTX_EVAL;
            return;
        }
        case K_COND: {
            lisp_value clauses = k->a;
            lisp_value clause = lisp_car(clauses);
            lisp_value body = lisp_cdr(clause);
            cx->kont = k->next;  // pop
            cx->env = k->env;
            if (lisp_truthy(V)) {
                if (!lisp_is_pair(body)) {  // no body -> the test value
                    cx->accum = V;
                    cx->status = LISP_CTX_APPLY;
                } else {
                    start_body(cx, body);  // tail
                }
            } else {
                cond_start(cx, lisp_cdr(clauses));  // try the next clause
            }
            return;
        }
        case K_CASE: {
            lisp_value key = V;
            cx->kont = k->next;  // pop
            cx->env = k->env;
            for (lisp_value cl = k->a; lisp_is_pair(cl); cl = lisp_cdr(cl)) {
                lisp_value clause = lisp_car(cl);
                if (!lisp_is_pair(clause)) {
                    ctx_error(cx, "malformed case clause");
                    return;
                }
                lisp_value datums = lisp_car(clause);
                bool take = is_form(datums, "else");
                for (lisp_value d = datums; !take && lisp_is_pair(d); d = lisp_cdr(d))
                    if (lisp_car(d) == key)
                        take = true;
                if (take) {
                    start_body(cx, lisp_cdr(clause));  // tail
                    return;
                }
            }
            cx->accum = LISP_UNDEF;  // no clause matched
            cx->status = LISP_CTX_APPLY;
            return;
        }
        case K_WHEN: {
            lisp_value body = k->a;
            bool want = (k->b == LISP_TRUE);
            cx->kont = k->next;  // pop
            cx->env = k->env;
            if (lisp_truthy(V) != want) {
                cx->accum = LISP_UNDEF;  // body not run
                cx->status = LISP_CTX_APPLY;
                return;
            }
            start_body(cx, body);  // tail
            return;
        }
        case K_WHILE_TEST: {
            lisp_value test = k->a;
            lisp_value body = k->b;
            if (!lisp_truthy(V)) {  // loop done
                cx->kont = k->next;
                cx->accum = LISP_UNDEF;
                cx->status = LISP_CTX_APPLY;
                return;
            }
            cx->env = k->env;
            if (!lisp_is_pair(body)) {  // empty body: straight back to the test
                cx->budget--;          // loop back-edge is a safe point
                cx->control = test;
                cx->status = LISP_CTX_EVAL;
                return;
            }
            k->h.header = LISP_MK_HEADER(LISP_OBJ_KONT, K_WHILE_BODY);
            k->c = lisp_cdr(body);  // forms remaining after the one we start now
            cx->control = lisp_car(body);
            cx->status = LISP_CTX_EVAL;
            return;
        }
        case K_WHILE_BODY: {
            lisp_value rem = k->c;  // V (the finished form's value) is discarded
            cx->env = k->env;
            if (lisp_is_pair(rem)) {
                k->c = lisp_cdr(rem);
                cx->control = lisp_car(rem);
                cx->status = LISP_CTX_EVAL;
                return;
            }
            // Body exhausted: loop back to the test (a back-edge / safe point).
            cx->budget--;
            k->h.header = LISP_MK_HEADER(LISP_OBJ_KONT, K_WHILE_TEST);
            cx->control = k->a;  // the test
            cx->status = LISP_CTX_EVAL;
            return;
        }
        case K_BIND: {
            lisp_value binds = k->a;
            lisp_value newenv = k->b;
            lisp_value bind = lisp_car(binds);
            lisp_env_define(newenv, lisp_car(bind), V);
            lisp_value restb = lisp_cdr(binds);
            if (!lisp_is_pair(restb)) {  // all bound: run the body in the new env
                cx->kont = k->next;
                cx->env = newenv;
                start_body(cx, k->c);
                return;
            }
            lisp_value b1 = lisp_car(restb);
            if (!lisp_is_pair(b1) || !lisp_is_pair(lisp_cdr(b1))) {
                ctx_error(cx, "malformed let binding");
                return;
            }
            k->a = restb;
            cx->env = k->env;  // init-eval env (outer for let, newenv for let*/letrec)
            cx->control = lisp_car(lisp_cdr(b1));
            cx->status = LISP_CTX_EVAL;
            return;
        }
        default:
            ctx_error(cx, "internal: bad continuation frame");
            return;
    }
}

// Drive the machine for up to its current budget. Returns a run result: DONE,
// ERROR, or SUSPENDED (budget exhausted at a safe point; cx->status retains the
// pending EVAL/APPLY step so a later call resumes exactly here).
//
// If the context owns a heap (K3), it allocates into that heap while running (the
// switch is restored on exit, so a primitive that nests a synchronous machine
// keeps allocating into this same heap). Its collection is run HERE, at the top of
// the loop -- a safe point between reductions where the context's CEK registers
// are its complete root set and no value is stranded in a C temporary.
static lisp_ctx_status ctx_run(lisp_ctx_t *cx) {
    lisp_heap_t *prev = (cx->heap != NULL) ? lisp_gc_set_alloc_heap(cx->heap) : NULL;
    lisp_ctx_status result;
    for (;;) {
        if (cx->heap != NULL && lisp_heap_wants_gc(cx->heap))
            lisp_heap_collect(cx->heap);
        if (cx->status == LISP_CTX_DONE) {
            result = LISP_CTX_DONE;
            break;
        }
        if (cx->status == LISP_CTX_ERROR) {
            result = LISP_CTX_ERROR;
            break;
        }
        if (cx->budget <= 0) {
            result = LISP_CTX_SUSPENDED;
            break;
        }
        if (cx->status == LISP_CTX_EVAL)
            step_eval(cx);
        else
            step_applyk(cx);
    }
    if (cx->heap != NULL)
        lisp_gc_set_alloc_heap(prev);
    return result;
}

// --- Context construction + public API --------------------------------------

static lisp_value ctx_alloc(lisp_value expr, lisp_value env) {
    // The context object lives in the shared system heap: it is referenced across
    // contexts (a scheduler queue, a send target handle) and outlives its own
    // working heap, so it must not live in any per-context heap.
    lisp_ctx_t *cx = (lisp_ctx_t *)lisp_gc_alloc_shared(sizeof(lisp_ctx_t));
    if (cx == NULL)
        return LISP_UNDEF;
    cx->h.header = LISP_MK_HEADER(LISP_OBJ_CTX, 0);
    cx->control = expr;
    cx->env = env;
    cx->accum = LISP_UNDEF;
    cx->kont = LISP_EMPTY;
    cx->mailbox = LISP_EMPTY;
    cx->status = LISP_CTX_EVAL;
    cx->blocked = 0;
    cx->err = NULL;
    cx->budget = 0;
    cx->heap = NULL;  // uses the system heap unless given its own (K3)
    return lisp_from_obj(cx);
}

lisp_value lisp_ctx_make(lisp_value expr, lisp_value env) { return ctx_alloc(expr, env); }

lisp_ctx_status lisp_ctx_state(lisp_value ctxv) {
    return (lisp_ctx_status)((lisp_ctx_t *)lisp_obj(ctxv))->status;
}

// Mark a parked context runnable again. Deliberately a SINGLE word write (no
// allocation, no lock) so a native interrupt handler can call it to wake a
// context parked on a hardware event -- the ISR -> wake-context bridge.
void lisp_ctx_wake(lisp_value ctxv) { ((lisp_ctx_t *)lisp_obj(ctxv))->blocked = 0; }

// Park a context: skip it in the scheduler and yield at the next safe point.
void lisp_ctx_block(lisp_value ctxv) {
    lisp_ctx_t *cx = (lisp_ctx_t *)lisp_obj(ctxv);
    cx->blocked = 1;
    cx->budget = 0;  // suspend at the next safe point
}

int lisp_ctx_attach_heap(lisp_value ctxv) {
    lisp_ctx_t *cx = (lisp_ctx_t *)lisp_obj(ctxv);
    if (cx->heap != NULL)
        return 0;  // already has one
    cx->heap = lisp_heap_new(ctxv);
    return cx->heap != NULL ? 0 : -1;
}

size_t lisp_ctx_heap_live(lisp_value ctxv) {
    lisp_ctx_t *cx = (lisp_ctx_t *)lisp_obj(ctxv);
    return cx->heap != NULL ? lisp_heap_live(cx->heap) : 0;
}

size_t lisp_ctx_heap_collections(lisp_value ctxv) {
    lisp_ctx_t *cx = (lisp_ctx_t *)lisp_obj(ctxv);
    return cx->heap != NULL ? lisp_heap_collections(cx->heap) : 0;
}

lisp_ctx_status lisp_ctx_resume(lisp_value ctxv, int64_t budget) {
    lisp_ctx_t *cx = (lisp_ctx_t *)lisp_obj(ctxv);
    cx->budget = budget;
    return ctx_run(cx);
}

lisp_value lisp_ctx_value(lisp_value ctxv) { return ((lisp_ctx_t *)lisp_obj(ctxv))->accum; }

const char *lisp_ctx_error(lisp_value ctxv) { return ((lisp_ctx_t *)lisp_obj(ctxv))->err; }

// Drive a transient context to completion with an effectively-unbounded budget.
// Used by the synchronous wrappers below; never suspends (the budget is re-armed
// in the unlikely event a single call evaluates ~2^63 reductions).
static lisp_ctx_status run_to_completion(lisp_ctx_t *cx) {
    lisp_ctx_status r;
    do {
        cx->budget = CTX_BUDGET_UNBOUNDED;
        r = ctx_run(cx);
    } while (r == LISP_CTX_SUSPENDED);
    return r;
}

lisp_value lisp_eval(lisp_value expr, lisp_value env, const char **err) {
    if (err != NULL)
        *err = NULL;
    lisp_value ctxv = ctx_alloc(expr, env);
    if (ctxv == LISP_UNDEF)
        return fail(err, "out of memory");
    lisp_ctx_t *cx = (lisp_ctx_t *)lisp_obj(ctxv);
    if (run_to_completion(cx) == LISP_CTX_ERROR)
        return fail(err, cx->err);
    return cx->accum;
}

lisp_value lisp_apply(lisp_value proc, lisp_value *args, int argc, const char **err) {
    if (err != NULL)
        *err = NULL;
    lisp_value ctxv = ctx_alloc(LISP_UNDEF, LISP_EMPTY);
    if (ctxv == LISP_UNDEF)
        return fail(err, "out of memory");
    lisp_ctx_t *cx = (lisp_ctx_t *)lisp_obj(ctxv);
    cx->budget = CTX_BUDGET_UNBOUNDED;
    do_call(cx, proc, args, argc);  // sets up control/frames (or signals an error)
    if (run_to_completion(cx) == LISP_CTX_ERROR)
        return fail(err, cx->err);
    return cx->accum;
}

lisp_value lisp_default_env(void) {
    lisp_value env = lisp_make_env(LISP_EMPTY);
    if (env != LISP_UNDEF) {
        lisp_init_special_forms();  // tag form symbols before any form is dispatched
        lisp_install_primitives(env);
        lisp_load_prelude(env);     // standard library, defined in Scheme
    }
    return env;
}

lisp_value lisp_eval_string(const char *src, lisp_value env, const char **err) {
    const char *cur = src;
    const char *end = src + strlen(src);
    lisp_value result = LISP_UNDEF;
    for (;;) {
        lisp_value form = lisp_read(&cur, end, err);
        if (form == LISP_EOF)
            return result;
        if (form == LISP_UNDEF)
            return LISP_UNDEF;  // reader error, *err already set
        result = lisp_eval(form, env, err);
        if (result == LISP_UNDEF && err != NULL && *err != NULL)
            return LISP_UNDEF;
    }
}
