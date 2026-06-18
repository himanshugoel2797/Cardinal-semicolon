// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

// Phase 1: a tree-walking Scheme evaluator with lexical environments and proper
// tail calls (via an internal loop, not C recursion) for if/begin/let and
// procedure application. Special forms: quote, if, define, lambda, let, begin,
// set!. Everything else is procedure application. Full first-class continuations
// and a bytecode VM are Phase 3; GC is Phase 4 (this phase leaks via malloc).

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "lisp.h"

#define MAX_ARGS 64  // Phase 1 cap on call arity; raised when the VM lands.

// --- Heap layouts (private to the evaluator) --------------------------------

typedef struct {
    lisp_header h;
    lisp_value parent;    // enclosing env, or LISP_EMPTY at the top level
    lisp_value bindings;  // assoc list ((sym . val) ...), mutated in place
} lisp_env_t;

typedef struct {
    lisp_header h;
    lisp_value params;  // list of parameter symbols
    lisp_value body;    // list of body expressions
    lisp_value env;     // captured definition environment
} lisp_closure_t;

typedef struct {
    lisp_header h;
    lisp_primitive_fn fn;
    lisp_value name;  // symbol, for diagnostics
} lisp_prim_t;

// --- Small helpers ----------------------------------------------------------

static lisp_value fail(const char **err, const char *msg) {
    if (err != NULL)
        *err = msg;
    return LISP_UNDEF;
}

// Mutate a pair's car/cdr. Used only on evaluator-owned structures (env
// bindings), never on user data -- the language model keeps pairs immutable.
static void set_cdr(lisp_value pair, lisp_value v) { ((lisp_pair *)lisp_obj(pair))->cdr = v; }

static bool symbol_eq(lisp_value a, lisp_value b) {
    if (a == b)
        return true;
    if (!lisp_is_symbol(a) || !lisp_is_symbol(b))
        return false;
    size_t la = lisp_named_len(a);
    if (la != lisp_named_len(b))
        return false;
    return memcmp(lisp_named_name(a), lisp_named_name(b), la) == 0;
}

// --- Constructors -----------------------------------------------------------

lisp_value lisp_make_closure(lisp_value params, lisp_value body, lisp_value env) {
    lisp_closure_t *c = (lisp_closure_t *)malloc(sizeof(lisp_closure_t));
    if (c == NULL)
        return LISP_UNDEF;
    c->h.header = LISP_MK_HEADER(LISP_OBJ_CLOSURE, 0);
    c->params = params;
    c->body = body;
    c->env = env;
    return lisp_from_obj(c);
}

lisp_value lisp_make_primitive(lisp_primitive_fn fn, const char *name) {
    lisp_prim_t *p = (lisp_prim_t *)malloc(sizeof(lisp_prim_t));
    if (p == NULL)
        return LISP_UNDEF;
    p->h.header = LISP_MK_HEADER(LISP_OBJ_PRIMITIVE, 0);
    p->fn = fn;
    p->name = lisp_make_symbol(name, strlen(name));
    if (p->name == LISP_UNDEF) {
        free(p);
        return LISP_UNDEF;
    }
    return lisp_from_obj(p);
}

// --- Environments -----------------------------------------------------------

lisp_value lisp_make_env(lisp_value parent) {
    lisp_env_t *e = (lisp_env_t *)malloc(sizeof(lisp_env_t));
    if (e == NULL)
        return LISP_UNDEF;
    e->h.header = LISP_MK_HEADER(LISP_OBJ_ENV, 0);
    e->parent = parent;
    e->bindings = LISP_EMPTY;
    return lisp_from_obj(e);
}

// Find the (sym . val) binding cell for `sym` in this single frame, or LISP_EMPTY.
static lisp_value frame_find(lisp_value env, lisp_value sym) {
    lisp_value b = ((lisp_env_t *)lisp_obj(env))->bindings;
    while (lisp_is_pair(b)) {
        lisp_value cell = lisp_car(b);
        if (lisp_is_pair(cell) && symbol_eq(lisp_car(cell), sym))
            return cell;
        b = lisp_cdr(b);
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
    e->bindings = lisp_cons(lisp_cons(sym, val), e->bindings);
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

// --- Symbol cache for special-form dispatch ---------------------------------
// Compared by name via symbol_eq, so freshly-read symbols match without interning.

static bool is_form(lisp_value sym, const char *name) {
    if (!lisp_is_symbol(sym))
        return false;
    size_t len = lisp_named_len(sym);
    return len == strlen(name) && memcmp(lisp_named_name(sym), name, len) == 0;
}

// --- Evaluator --------------------------------------------------------------

// Bind a closure's parameter list to evaluated args in a fresh child env.
// Returns the new env, or LISP_UNDEF (+*err) on arity mismatch / OOM.
// (On an arity/OOM error this abandons the freshly-allocated env; that leak is
// intentional for Phase 1 -- the GC arrives in Phase 4.)
static lisp_value bind_params(lisp_value params, lisp_value *args, int argc,
                              lisp_value parent_env, const char **err) {
    lisp_value env = lisp_make_env(parent_env);
    if (env == LISP_UNDEF)
        return fail(err, "out of memory");
    int i = 0;
    while (lisp_is_pair(params)) {
        if (i >= argc)
            return fail(err, "too few arguments");
        lisp_env_define(env, lisp_car(params), args[i]);
        params = lisp_cdr(params);
        i++;
    }
    if (!lisp_is_empty(params))
        return fail(err, "malformed parameter list");
    if (i != argc)
        return fail(err, "too many arguments");
    return env;
}

lisp_value lisp_eval(lisp_value expr, lisp_value env, const char **err) {
    if (err != NULL)
        *err = NULL;

tail:
    // Symbols: variable reference.
    if (lisp_is_symbol(expr)) {
        lisp_value v;
        if (!lisp_env_lookup(env, expr, &v))
            return fail(err, "unbound variable");
        return v;
    }
    // Self-evaluating: fixnums, booleans, chars, strings, keywords, eof.
    if (!lisp_is_pair(expr)) {
        if (lisp_is_empty(expr))
            return fail(err, "cannot evaluate empty application ()");
        return expr;
    }

    // Combination.
    lisp_value head = lisp_car(expr);
    lisp_value rest = lisp_cdr(expr);

    if (lisp_is_symbol(head)) {
        if (is_form(head, "quote"))
            return lisp_is_pair(rest) ? lisp_car(rest) : fail(err, "malformed quote");

        if (is_form(head, "if")) {
            if (!lisp_is_pair(rest) || !lisp_is_pair(lisp_cdr(rest)))
                return fail(err, "malformed if");
            lisp_value test = lisp_eval(lisp_car(rest), env, err);
            if (test == LISP_UNDEF && err != NULL && *err != NULL)
                return LISP_UNDEF;
            lisp_value branches = lisp_cdr(rest);
            if (lisp_truthy(test)) {
                expr = lisp_car(branches);
            } else {
                lisp_value elsebr = lisp_cdr(branches);
                if (!lisp_is_pair(elsebr))
                    return LISP_UNDEF;  // (if #f x) with no else -> unspecified
                expr = lisp_car(elsebr);
            }
            goto tail;
        }

        if (is_form(head, "define")) {
            if (!lisp_is_pair(rest))
                return fail(err, "malformed define");
            lisp_value target = lisp_car(rest);
            if (lisp_is_symbol(target)) {
                if (!lisp_is_pair(lisp_cdr(rest)))
                    return fail(err, "malformed define: missing value expression");
                lisp_value val = lisp_eval(lisp_car(lisp_cdr(rest)), env, err);
                if (val == LISP_UNDEF && err != NULL && *err != NULL)
                    return LISP_UNDEF;
                lisp_env_define(env, target, val);
                return target;
            }
            if (lisp_is_pair(target)) {  // (define (f args...) body...)
                lisp_value name = lisp_car(target);
                if (!lisp_is_symbol(name))
                    return fail(err, "define: function name must be a symbol");
                lisp_value params = lisp_cdr(target);
                lisp_value fn = lisp_make_closure(params, lisp_cdr(rest), env);
                if (fn == LISP_UNDEF)
                    return fail(err, "out of memory");
                lisp_env_define(env, name, fn);
                return name;
            }
            return fail(err, "malformed define");
        }

        if (is_form(head, "lambda")) {
            if (!lisp_is_pair(rest))
                return fail(err, "malformed lambda");
            lisp_value cl = lisp_make_closure(lisp_car(rest), lisp_cdr(rest), env);
            if (cl == LISP_UNDEF)
                return fail(err, "out of memory");
            return cl;
        }

        if (is_form(head, "set!")) {
            if (!lisp_is_pair(rest) || !lisp_is_pair(lisp_cdr(rest)))
                return fail(err, "malformed set!");
            lisp_value val = lisp_eval(lisp_car(lisp_cdr(rest)), env, err);
            if (val == LISP_UNDEF && err != NULL && *err != NULL)
                return LISP_UNDEF;
            if (!lisp_env_set(env, lisp_car(rest), val))
                return fail(err, "set! on unbound variable");
            return LISP_UNDEF;
        }

        if (is_form(head, "begin")) {
            if (!lisp_is_pair(rest))
                return LISP_UNDEF;  // (begin) -> unspecified
            while (lisp_is_pair(lisp_cdr(rest))) {
                lisp_eval(lisp_car(rest), env, err);
                if (err != NULL && *err != NULL)
                    return LISP_UNDEF;
                rest = lisp_cdr(rest);
            }
            expr = lisp_car(rest);
            goto tail;
        }

        if (is_form(head, "let")) {
            // (let ((x v) ...) body...) -- bindings evaluated in the outer env.
            if (!lisp_is_pair(rest))
                return fail(err, "malformed let");
            lisp_value newenv = lisp_make_env(env);
            if (newenv == LISP_UNDEF)
                return fail(err, "out of memory");
            lisp_value binds = lisp_car(rest);
            while (lisp_is_pair(binds)) {
                lisp_value b = lisp_car(binds);
                if (!lisp_is_pair(b) || !lisp_is_pair(lisp_cdr(b)))
                    return fail(err, "malformed let binding");
                lisp_value v = lisp_eval(lisp_car(lisp_cdr(b)), env, err);
                if (v == LISP_UNDEF && err != NULL && *err != NULL)
                    return LISP_UNDEF;
                lisp_env_define(newenv, lisp_car(b), v);
                binds = lisp_cdr(binds);
            }
            env = newenv;
            rest = lisp_cdr(rest);  // body
            if (!lisp_is_pair(rest))
                return LISP_UNDEF;
            while (lisp_is_pair(lisp_cdr(rest))) {
                lisp_eval(lisp_car(rest), env, err);
                if (err != NULL && *err != NULL)
                    return LISP_UNDEF;
                rest = lisp_cdr(rest);
            }
            expr = lisp_car(rest);
            goto tail;
        }

        if (is_form(head, "and")) {
            // (and) => #t; short-circuit on the first #f; last is in tail pos.
            if (!lisp_is_pair(rest))
                return LISP_TRUE;
            while (lisp_is_pair(lisp_cdr(rest))) {
                lisp_value v = lisp_eval(lisp_car(rest), env, err);
                if (v == LISP_UNDEF && err != NULL && *err != NULL)
                    return LISP_UNDEF;
                if (!lisp_truthy(v))
                    return LISP_FALSE;
                rest = lisp_cdr(rest);
            }
            expr = lisp_car(rest);
            goto tail;
        }

        if (is_form(head, "or")) {
            // (or) => #f; return the first truthy value; last is in tail pos.
            if (!lisp_is_pair(rest))
                return LISP_FALSE;
            while (lisp_is_pair(lisp_cdr(rest))) {
                lisp_value v = lisp_eval(lisp_car(rest), env, err);
                if (v == LISP_UNDEF && err != NULL && *err != NULL)
                    return LISP_UNDEF;
                if (lisp_truthy(v))
                    return v;
                rest = lisp_cdr(rest);
            }
            expr = lisp_car(rest);
            goto tail;
        }

        if (is_form(head, "cond")) {
            // (cond (test body...) ... (else body...)). A clause with no body
            // returns its test value; the chosen body's last form is tail.
            lisp_value clauses = rest;
            while (lisp_is_pair(clauses)) {
                lisp_value clause = lisp_car(clauses);
                if (!lisp_is_pair(clause))
                    return fail(err, "malformed cond clause");
                lisp_value test = lisp_car(clause);
                lisp_value body = lisp_cdr(clause);
                lisp_value testval = LISP_TRUE;
                bool take = is_form(test, "else");
                if (!take) {
                    testval = lisp_eval(test, env, err);
                    if (testval == LISP_UNDEF && err != NULL && *err != NULL)
                        return LISP_UNDEF;
                    take = lisp_truthy(testval);
                }
                if (take) {
                    if (!lisp_is_pair(body))
                        return testval;
                    while (lisp_is_pair(lisp_cdr(body))) {
                        lisp_eval(lisp_car(body), env, err);
                        if (err != NULL && *err != NULL)
                            return LISP_UNDEF;
                        body = lisp_cdr(body);
                    }
                    expr = lisp_car(body);
                    goto tail;
                }
                clauses = lisp_cdr(clauses);
            }
            return LISP_UNDEF;  // no clause matched
        }
    }

    // Procedure application: evaluate operator and operands.
    lisp_value op = lisp_eval(head, env, err);
    if (op == LISP_UNDEF && err != NULL && *err != NULL)
        return LISP_UNDEF;

    lisp_value args[MAX_ARGS];
    int argc = 0;
    lisp_value a = rest;
    while (lisp_is_pair(a)) {
        if (argc >= MAX_ARGS)
            return fail(err, "too many arguments (Phase 1 cap)");
        lisp_value v = lisp_eval(lisp_car(a), env, err);
        if (v == LISP_UNDEF && err != NULL && *err != NULL)
            return LISP_UNDEF;
        args[argc++] = v;
        a = lisp_cdr(a);
    }
    if (!lisp_is_empty(a))
        return fail(err, "improper argument list");

    if (lisp_is_objtype(op, LISP_OBJ_PRIMITIVE)) {
        lisp_prim_t *p = (lisp_prim_t *)lisp_obj(op);
        return p->fn(args, argc, err);
    }
    if (lisp_is_objtype(op, LISP_OBJ_CLOSURE)) {
        lisp_closure_t *c = (lisp_closure_t *)lisp_obj(op);
        lisp_value newenv = bind_params(c->params, args, argc, c->env, err);
        if (newenv == LISP_UNDEF)
            return LISP_UNDEF;
        lisp_value body = c->body;
        if (!lisp_is_pair(body))
            return LISP_UNDEF;  // empty body -> unspecified
        env = newenv;
        while (lisp_is_pair(lisp_cdr(body))) {
            lisp_eval(lisp_car(body), env, err);
            if (err != NULL && *err != NULL)
                return LISP_UNDEF;
            body = lisp_cdr(body);
        }
        expr = lisp_car(body);  // tail call: loop instead of recursing
        goto tail;
    }
    return fail(err, "attempt to call a non-procedure");
}

lisp_value lisp_apply(lisp_value proc, lisp_value *args, int argc, const char **err) {
    if (err != NULL)
        *err = NULL;
    if (lisp_is_objtype(proc, LISP_OBJ_PRIMITIVE))
        return ((lisp_prim_t *)lisp_obj(proc))->fn(args, argc, err);
    if (lisp_is_objtype(proc, LISP_OBJ_CLOSURE)) {
        lisp_closure_t *c = (lisp_closure_t *)lisp_obj(proc);
        lisp_value newenv = bind_params(c->params, args, argc, c->env, err);
        if (newenv == LISP_UNDEF)
            return LISP_UNDEF;
        lisp_value result = LISP_UNDEF;  // empty body -> unspecified
        for (lisp_value body = c->body; lisp_is_pair(body); body = lisp_cdr(body)) {
            result = lisp_eval(lisp_car(body), newenv, err);
            if (result == LISP_UNDEF && err != NULL && *err != NULL)
                return LISP_UNDEF;
        }
        return result;
    }
    return fail(err, "attempt to call a non-procedure");
}

lisp_value lisp_default_env(void) {
    lisp_value env = lisp_make_env(LISP_EMPTY);
    if (env != LISP_UNDEF)
        lisp_install_primitives(env);
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
