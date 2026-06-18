// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

// Escape continuations (call/cc), exceptions (raise / error / guard /
// with-exception-handler), and multiple values.
//
// All nonlocal exits unwind the C stack using the evaluator's existing
// `*err != NULL` propagation; this module only records *what kind* of exit is in
// flight (a control state) so the catchers can decide whether to stop it.
//
// Intentional limitations (documented exceptions): call/cc is ESCAPE-ONLY -- a
// continuation may be invoked only while its call/cc is still on the stack;
// re-invoking it later is an error (full re-entrant continuations need a VM or
// CPS). dynamic-wind is not implemented.

#include <stdint.h>
#include <string.h>

#include "internal.h"
#include "lisp.h"

// --- control state ----------------------------------------------------------

static int g_kind = LISP_CTL_NONE;
static lisp_value g_value = 0;
static uint64_t g_cont_id = 0;
static const char *CTL_ESCAPE = "escape: continuation invoked";

// A plain argument/usage error from a control primitive: guard err (the API
// permits NULL) and clear any stale nonlocal-exit kind.
static lisp_value cfail(const char **err, const char *msg) {
    if (err != NULL)
        *err = msg;
    lisp_ctl_clear();
    return LISP_UNDEF;
}

int lisp_ctl_kind(void) { return g_kind; }
lisp_value lisp_ctl_value(void) { return g_value; }
uint64_t lisp_ctl_cont_id(void) { return g_cont_id; }
void lisp_ctl_clear(void) {
    g_kind = LISP_CTL_NONE;
    g_value = LISP_UNDEF;
}
void lisp_ctl_set_raise(lisp_value condition) {
    g_kind = LISP_CTL_RAISE;
    g_value = condition;
}
void lisp_ctl_set_cont(uint64_t id, lisp_value value) {
    g_kind = LISP_CTL_CONT;
    g_cont_id = id;
    g_value = value;
}

// --- continuations ----------------------------------------------------------

static uint64_t g_cont_counter = 0;

lisp_value lisp_make_cont(uint64_t id) {
    lisp_cont_t *c = (lisp_cont_t *)lisp_gc_alloc(sizeof(lisp_cont_t));
    if (c == NULL)
        return LISP_UNDEF;
    c->h.header = LISP_MK_HEADER(LISP_OBJ_CONT, 0);
    c->id = id;
    return lisp_from_obj(c);
}

lisp_value lisp_cont_invoke(lisp_value cont, lisp_value value, const char **err) {
    lisp_ctl_set_cont(((lisp_cont_t *)lisp_obj(cont))->id, value);
    if (err != NULL)
        *err = CTL_ESCAPE;
    return LISP_UNDEF;
}

static lisp_value prim_callcc(lisp_value *args, int argc, const char **err) {
    if (argc != 1)
        return cfail(err, "call/cc expects one argument");
    uint64_t id = ++g_cont_counter;
    lisp_value k = lisp_make_cont(id);
    if (k == LISP_UNDEF)
        return cfail(err, "out of memory");
    lisp_value r = lisp_apply(args[0], &k, 1, err);
    if (err == NULL || *err == NULL)
        return r;  // normal return (continuation not invoked)
    if (lisp_ctl_kind() == LISP_CTL_CONT && lisp_ctl_cont_id() == id) {
        lisp_value v = lisp_ctl_value();
        lisp_ctl_clear();
        *err = NULL;
        return v;  // our escape: caught
    }
    return LISP_UNDEF;  // some other exit -> keep propagating
}

// --- error objects ----------------------------------------------------------
// Represented as a 3-element vector tagged by an interned marker symbol.

// Cached interned tag symbols (set in lisp_install_control) so the predicates
// below never allocate -- otherwise a GC could fire inside lisp_is_error_object.
static lisp_value g_error_tag = 0;
static lisp_value g_values_tag = 0;

static lisp_value error_tag(void) {
    return g_error_tag != 0 ? g_error_tag : lisp_make_symbol("%error-object", 13);
}

lisp_value lisp_make_error_object(lisp_value message, lisp_value irritants) {
    lisp_value v = lisp_make_vector(3, LISP_UNDEF);
    if (v == LISP_UNDEF)
        return LISP_UNDEF;
    lisp_vector_set_init(v, 0, error_tag());
    lisp_vector_set_init(v, 1, message);
    lisp_vector_set_init(v, 2, irritants);
    return v;
}

bool lisp_is_error_object(lisp_value v) {
    return lisp_is_vector(v) && lisp_vector_length(v) == 3 &&
           lisp_vector_ref(v, 0) == error_tag();
}

// --- exceptions -------------------------------------------------------------

static lisp_value prim_raise(lisp_value *args, int argc, const char **err) {
    if (argc != 1)
        return cfail(err, "raise expects one argument");
    lisp_ctl_set_raise(args[0]);
    if (err != NULL)
        *err = "uncaught exception";
    return LISP_UNDEF;
}

static lisp_value prim_error(lisp_value *args, int argc, const char **err) {
    lisp_value msg = (argc >= 1 && lisp_is_string(args[0]))
                         ? args[0]
                         : lisp_make_string("error", 5);
    lisp_value irr = LISP_EMPTY;
    for (int i = argc - 1; i >= 1; i--)
        irr = lisp_cons(args[i], irr);
    lisp_value cond = lisp_make_error_object(msg, irr);
    if (cond == LISP_UNDEF)
        return cfail(err, "out of memory");
    lisp_ctl_set_raise(cond);
    if (err != NULL)  // surface the message if uncaught
        *err = lisp_string_data(msg);
    return LISP_UNDEF;
}

static lisp_value prim_error_objectp(lisp_value *a, int n, const char **e) {
    if (n != 1)
        return cfail(e, "error-object? expects one argument");
    return lisp_is_error_object(a[0]) ? LISP_TRUE : LISP_FALSE;
}
static lisp_value prim_error_message(lisp_value *a, int n, const char **e) {
    if (n != 1 || !lisp_is_error_object(a[0]))
        return cfail(e, "error-object-message expects an error object");
    return lisp_vector_ref(a[0], 1);
}
static lisp_value prim_error_irritants(lisp_value *a, int n, const char **e) {
    if (n != 1 || !lisp_is_error_object(a[0]))
        return cfail(e, "error-object-irritants expects an error object");
    return lisp_vector_ref(a[0], 2);
}

// (with-exception-handler handler thunk): run thunk; if an exception unwinds,
// call handler with the condition. (We catch raise only, not continuation
// escapes or plain errors; guard is the higher-level form.)
static lisp_value prim_weh(lisp_value *args, int argc, const char **err) {
    if (argc != 2)
        return cfail(err, "with-exception-handler expects a handler and a thunk");
    lisp_value r = lisp_apply(args[1], NULL, 0, err);
    if (err == NULL || *err == NULL)
        return r;
    if (lisp_ctl_kind() == LISP_CTL_RAISE) {
        lisp_value cond = lisp_ctl_value();
        lisp_ctl_clear();
        *err = NULL;
        return lisp_apply(args[0], &cond, 1, err);
    }
    return LISP_UNDEF;  // continuation escape or plain error -> propagate
}

// --- multiple values --------------------------------------------------------
// (values ...) of one value is that value; otherwise a vector tagged with a
// marker. call-with-values spreads it back to the consumer.

static lisp_value values_tag(void) {
    return g_values_tag != 0 ? g_values_tag : lisp_make_symbol("%values", 7);
}

static lisp_value prim_values(lisp_value *args, int argc, const char **err) {
    if (argc == 1)
        return args[0];
    lisp_value v = lisp_make_vector((size_t)argc + 1, LISP_UNDEF);
    if (v == LISP_UNDEF)
        return cfail(err, "out of memory");
    lisp_vector_set_init(v, 0, values_tag());
    for (int i = 0; i < argc; i++)
        lisp_vector_set_init(v, (size_t)i + 1, args[i]);
    return v;
}

static lisp_value prim_cwv(lisp_value *args, int argc, const char **err) {
    if (argc != 2)
        return cfail(err, "call-with-values expects a producer and a consumer");
    lisp_value r = lisp_apply(args[0], NULL, 0, err);
    if (err != NULL && *err != NULL)
        return LISP_UNDEF;
    if (lisp_is_vector(r) && lisp_vector_length(r) >= 1 &&
        lisp_vector_ref(r, 0) == values_tag()) {
        size_t n = lisp_vector_length(r) - 1;
        lisp_value buf[64];
        if (n > 64)
            return cfail(err, "call-with-values: too many values");
        for (size_t i = 0; i < n; i++)
            buf[i] = lisp_vector_ref(r, i + 1);
        return lisp_apply(args[1], buf, (int)n, err);
    }
    return lisp_apply(args[1], &r, 1, err);  // single value
}

// --- installation -----------------------------------------------------------

static void def(lisp_value env, const char *name, lisp_primitive_fn fn) {
    lisp_env_define(env, lisp_make_symbol(name, strlen(name)), lisp_make_primitive(fn, name));
}

void lisp_install_control(lisp_value env) {
    g_error_tag = lisp_make_symbol("%error-object", 13);  // cache before any use
    g_values_tag = lisp_make_symbol("%values", 7);
    def(env, "call/cc", prim_callcc);
    def(env, "call-with-current-continuation", prim_callcc);
    def(env, "raise", prim_raise);
    def(env, "raise-continuable", prim_raise);  // simplified: same as raise
    def(env, "error", prim_error);
    def(env, "error-object?", prim_error_objectp);
    def(env, "error-object-message", prim_error_message);
    def(env, "error-object-irritants", prim_error_irritants);
    def(env, "with-exception-handler", prim_weh);
    def(env, "values", prim_values);
    def(env, "call-with-values", prim_cwv);
}
