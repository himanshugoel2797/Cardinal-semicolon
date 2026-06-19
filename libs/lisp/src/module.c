// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

// Multi-file programs: the `define-module` / `import` special forms and the
// module registry. The base language is a single flat global namespace; a
// module is a file that evaluates its body in a PRIVATE environment and
// publishes a chosen set of bindings. Importers pull those exports into their
// own environment, optionally under a prefix or restricted to a few names, so
// independently-written libraries do not clobber each other's globals.
//
// Rooting / lifetime. The registry of loaded modules is kept INSIDE the global
// environment (a hidden `%modules` binding), not in a C static: the system-heap
// collector roots conservatively from the C stack + the intern table, and a
// static `lisp_value` is neither -- it would be collected. The global env, by
// contrast, is always reachable while the evaluator runs, so anything hung off
// it (the registry, each module's exports, and the private env a closure
// captured) survives for free. The consequence is that loading must happen in
// the single-core boot window, before lisp_gc_set_multicore freezes the system
// heap -- the same rule top-level driver loading already follows.
//
// A module's source is fetched by NAME through the loader hook (the kernel maps
// a name to an initrd path; host tests map it to a file). Loading is idempotent
// and a partially-loaded module is tagged with a `%loading` sentinel so a
// circular import is reported rather than recursing forever. The forms run
// synchronously (nested lisp_eval to completion); they are a boot-time
// configuration step, not a concurrent path, so they need no suspend/resume.

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "internal.h"  // lisp_env_t layout + lisp_module_* prototypes
#include "lisp.h"

// --- embedder loader hook ---------------------------------------------------

static lisp_module_loader_fn g_loader = NULL;
static void *g_loader_ctx = NULL;

void lisp_set_module_loader(lisp_module_loader_fn fn, void *ctx) {
    g_loader = fn;
    g_loader_ctx = ctx;
}

// --- small helpers ----------------------------------------------------------

static lisp_value fail(const char **err, const char *msg) {
    if (err != NULL)
        *err = msg;
    return LISP_UNDEF;
}

// Same, for the bool-returning helpers (set the error, report failure).
static bool fail_b(const char **err, const char *msg) {
    if (err != NULL)
        *err = msg;
    return false;
}

// Mutate a registry cell in place (evaluator-owned structure, never user data).
static void set_car(lisp_value pair, lisp_value v) {
    ((lisp_pair *)lisp_obj(pair))->car = v;
}
static void set_cdr(lisp_value pair, lisp_value v) {
    ((lisp_pair *)lisp_obj(pair))->cdr = v;
}

// Symbol equality by name -- matches eval.c's is_form discipline so freshly-read
// (uninterned) symbols compare equal to interned ones.
static bool sym_name_eq(lisp_value a, lisp_value b) {
    if (a == b)
        return true;
    if (!lisp_is_symbol(a) || !lisp_is_symbol(b))
        return false;
    size_t la = lisp_named_len(a);
    return la == lisp_named_len(b) &&
           memcmp(lisp_named_name(a), lisp_named_name(b), la) == 0;
}

static bool sym_is(lisp_value s, const char *name) {
    if (!lisp_is_symbol(s))
        return false;
    size_t n = lisp_named_len(s);
    return n == strlen(name) && memcmp(lisp_named_name(s), name, n) == 0;
}

// The topmost (global) environment frame: where primitives, the prelude, and
// the module registry live. A module body parents off this so it sees the base
// language but keeps its own defines private.
static lisp_value global_env(lisp_value env) {
    while (lisp_is_objtype(env, LISP_OBJ_ENV)) {
        lisp_value p = ((lisp_env_t *)lisp_obj(env))->parent;
        if (!lisp_is_objtype(p, LISP_OBJ_ENV))
            break;
        env = p;
    }
    return env;
}

// Look up `sym` in ONE frame only (not the parent chain). Exports must be the
// module's own top-level defines, not names merely visible from the global env.
static bool frame_lookup(lisp_value env, lisp_value sym, lisp_value *out) {
    lisp_value b = ((lisp_env_t *)lisp_obj(env))->bindings;
    while (lisp_is_pair(b)) {
        lisp_value cell = lisp_car(b);
        if (lisp_is_pair(cell) && sym_name_eq(lisp_car(cell), sym)) {
            if (out != NULL)
                *out = lisp_cdr(cell);
            return true;
        }
        b = lisp_cdr(b);
    }
    return false;
}

// --- registry (an assoc list ((name . record) ...) in the global env) -------
//
// record is either the `%loading` sentinel (load in progress) or an exports
// assoc list ((export-name . value) ...).

static lisp_value reg_key(void) { return lisp_make_symbol("%modules", 8); }
static lisp_value loading_sentinel(void) { return lisp_make_symbol("%loading", 8); }
static bool is_loading(lisp_value rec) { return sym_is(rec, "%loading"); }

static lisp_value reg_list(lisp_value genv) {
    lisp_value v;
    if (lisp_env_lookup(genv, reg_key(), &v))
        return v;
    return LISP_EMPTY;
}

// The (name . record) cell for `name`, or LISP_EMPTY if not present.
static lisp_value reg_find(lisp_value genv, lisp_value name) {
    for (lisp_value l = reg_list(genv); lisp_is_pair(l); l = lisp_cdr(l)) {
        lisp_value cell = lisp_car(l);
        if (lisp_is_pair(cell) && sym_name_eq(lisp_car(cell), name))
            return cell;
    }
    return LISP_EMPTY;
}

// Drop name's registry entry by tombstoning the cell (its car can no longer
// match any symbol name), so a load that failed after marking %loading does not
// wedge the module as permanently "loading". Allocation-free, hence GC-safe and
// usable on an error path. The dead cons is harmless (matched by nothing).
static void reg_remove(lisp_value genv, lisp_value name) {
    lisp_value cell = reg_find(genv, name);
    if (lisp_is_pair(cell))
        set_car(cell, LISP_EMPTY);
}

// Insert or overwrite name's record. Returns false (+*err) on OOM.
static bool reg_set(lisp_value genv, lisp_value name, lisp_value record,
                    const char **err) {
    lisp_value cell = reg_find(genv, name);
    if (lisp_is_pair(cell)) {
        set_cdr(cell, record);
        return true;
    }
    lisp_value newcell = lisp_cons(name, record);
    if (newcell == LISP_UNDEF)
        return fail_b(err, "out of memory");
    lisp_value l = lisp_cons(newcell, reg_list(genv));
    if (l == LISP_UNDEF)
        return fail_b(err, "out of memory");
    lisp_env_define(genv, reg_key(), l);  // define overwrites in the same frame
    return true;
}

// --- define-module ----------------------------------------------------------

// (define-module NAME (export a b ...) body...)
lisp_value lisp_module_define(lisp_value form, lisp_value env, const char **err) {
    lisp_value rest = lisp_cdr(form);
    if (!lisp_is_pair(rest))
        return fail(err, "define-module: missing name");
    lisp_value name = lisp_car(rest);
    if (!lisp_is_symbol(name))
        return fail(err, "define-module: module name must be a symbol");

    lisp_value rest2 = lisp_cdr(rest);
    if (!lisp_is_pair(rest2))
        return fail(err, "define-module: missing (export ...) clause");
    lisp_value exportspec = lisp_car(rest2);
    lisp_value body = lisp_cdr(rest2);
    if (!lisp_is_pair(exportspec) || !sym_is(lisp_car(exportspec), "export"))
        return fail(err, "define-module: second form must be (export ...)");
    lisp_value exports = lisp_cdr(exportspec);

    // A fresh environment parented on the global env: the module's top-level
    // defines land here (private), while it still sees primitives/prelude.
    lisp_value genv = global_env(env);
    lisp_value modenv = lisp_make_env(genv);
    if (modenv == LISP_UNDEF)
        return fail(err, "out of memory");

    for (lisp_value b = body; lisp_is_pair(b); b = lisp_cdr(b)) {
        const char *e = NULL;
        lisp_eval(lisp_car(b), modenv, &e);
        if (e != NULL)
            return fail(err, e);
    }

    // Harvest the exported bindings (must be the module's own top-level defines).
    lisp_value alist = LISP_EMPTY;
    for (lisp_value ex = exports; lisp_is_pair(ex); ex = lisp_cdr(ex)) {
        lisp_value sym = lisp_car(ex);
        if (!lisp_is_symbol(sym))
            return fail(err, "define-module: export names must be symbols");
        lisp_value val;
        if (!frame_lookup(modenv, sym, &val))
            return fail(err, "define-module: exported name is not defined");
        lisp_value cell = lisp_cons(sym, val);
        if (cell == LISP_UNDEF)
            return fail(err, "out of memory");
        lisp_value na = lisp_cons(cell, alist);
        if (na == LISP_UNDEF)
            return fail(err, "out of memory");
        alist = na;
    }

    if (!reg_set(genv, name, alist, err))
        return LISP_UNDEF;
    return name;
}

// --- import -----------------------------------------------------------------

// Ensure module `name` is loaded; return its exports assoc list (or LISP_UNDEF
// +*err). Idempotent: a module already in the registry is not re-evaluated.
static lisp_value ensure_loaded(lisp_value genv, lisp_value name,
                                const char **err) {
    lisp_value cell = reg_find(genv, name);
    if (lisp_is_pair(cell)) {
        lisp_value rec = lisp_cdr(cell);
        if (is_loading(rec))
            return fail(err, "import: circular module dependency");
        return rec;
    }
    if (g_loader == NULL)
        return fail(err, "import: no module loader installed");

    const char *src = NULL;
    size_t len = 0;
    if (!g_loader(lisp_named_name(name), &src, &len, g_loader_ctx))
        return fail(err, "import: module not found");

    // Mark loading first, so a (direct or indirect) self-import is caught.
    if (!reg_set(genv, name, loading_sentinel(), err))
        return LISP_UNDEF;

    // Evaluate the source into the global env; its (define-module NAME ...)
    // replaces the sentinel with the real record. On any failure past this
    // point, tombstone the entry so the half-loaded module does not stay stuck
    // as "loading" (a same-env retry would otherwise be misreported as a cycle).
    const char *cur = src;
    const char *end = src + len;
    for (;;) {
        const char *e = NULL;
        lisp_value f = lisp_read(&cur, end, &e);
        if (f == LISP_EOF)
            break;
        if (f == LISP_UNDEF) {
            reg_remove(genv, name);
            return fail(err, e != NULL ? e : "import: source read error");
        }
        lisp_eval(f, genv, &e);
        if (e != NULL) {
            reg_remove(genv, name);
            return fail(err, e);
        }
    }

    cell = reg_find(genv, name);
    if (!lisp_is_pair(cell) || is_loading(lisp_cdr(cell))) {
        reg_remove(genv, name);  // never registered itself -> allow a clean retry
        return fail(err, "import: source did not define the requested module");
    }
    return lisp_cdr(cell);
}

// Build a symbol from `prefix` (plen bytes) concatenated with `sym`'s name.
static lisp_value prefixed_symbol(const char *prefix, size_t plen, lisp_value sym,
                                  const char **err) {
    const char *nm = lisp_named_name(sym);
    size_t nlen = lisp_named_len(sym);
    char buf[128];
    // Bound each operand before summing so a pathological length from untrusted
    // initrd source can't wrap size_t and slip past the buffer check.
    if (plen >= sizeof buf || nlen >= sizeof buf || plen + nlen >= sizeof buf)
        return fail(err, "import: prefixed name too long");
    memcpy(buf, prefix, plen);
    memcpy(buf + plen, nm, nlen);
    return lisp_make_symbol(buf, plen + nlen);
}

static bool name_in_list(lisp_value sym, lisp_value list) {
    for (lisp_value l = list; lisp_is_pair(l); l = lisp_cdr(l))
        if (sym_name_eq(lisp_car(l), sym))
            return true;
    return false;
}

// Bind `exports` into `env` per the clauses of one import spec:
//   (prefix p)   -- bind each export as <p><name>
//   (only a ...) -- bind only the listed exports
static bool bind_exports(lisp_value env, lisp_value exports, lisp_value clauses,
                         const char **err) {
    const char *prefix = "";
    size_t prefixlen = 0;
    lisp_value only = LISP_EMPTY;
    bool has_only = false;

    for (lisp_value c = clauses; lisp_is_pair(c); c = lisp_cdr(c)) {
        lisp_value cl = lisp_car(c);
        if (!lisp_is_pair(cl))
            return fail_b(err, "import: malformed clause");
        lisp_value tag = lisp_car(cl);
        if (sym_is(tag, "prefix")) {
            lisp_value p = lisp_cdr(cl);
            if (!lisp_is_pair(p) || !lisp_is_symbol(lisp_car(p)))
                return fail_b(err, "import: (prefix sym) expected");
            prefix = lisp_named_name(lisp_car(p));
            prefixlen = lisp_named_len(lisp_car(p));
        } else if (sym_is(tag, "only")) {
            only = lisp_cdr(cl);
            has_only = true;
        } else {
            return fail_b(err, "import: unknown clause (want prefix or only)");
        }
    }

    for (lisp_value e = exports; lisp_is_pair(e); e = lisp_cdr(e)) {
        lisp_value pairc = lisp_car(e);
        lisp_value sym = lisp_car(pairc);
        lisp_value val = lisp_cdr(pairc);
        if (has_only && !name_in_list(sym, only))
            continue;
        lisp_value target = sym;
        if (prefixlen != 0) {
            target = prefixed_symbol(prefix, prefixlen, sym, err);
            if (target == LISP_UNDEF)
                return false;
        }
        lisp_env_define(env, target, val);
    }
    return true;
}

// (import SPEC ...) where SPEC is `name` or `(name CLAUSE ...)`.
lisp_value lisp_module_import(lisp_value form, lisp_value env, const char **err) {
    lisp_value genv = global_env(env);
    for (lisp_value specs = lisp_cdr(form); lisp_is_pair(specs);
         specs = lisp_cdr(specs)) {
        lisp_value spec = lisp_car(specs);
        lisp_value name;
        lisp_value clauses = LISP_EMPTY;
        if (lisp_is_symbol(spec)) {
            name = spec;
        } else if (lisp_is_pair(spec)) {
            name = lisp_car(spec);
            if (!lisp_is_symbol(name))
                return fail(err, "import: module name must be a symbol");
            clauses = lisp_cdr(spec);
        } else {
            return fail(err, "import: bad import spec");
        }
        lisp_value exports = ensure_loaded(genv, name, err);
        if (exports == LISP_UNDEF && err != NULL && *err != NULL)
            return LISP_UNDEF;
        if (!bind_exports(env, exports, clauses, err))
            return LISP_UNDEF;
    }
    return LISP_UNDEF;  // unspecified
}

// --- built-in (C-provided) modules ------------------------------------------

// Register a module whose exports are C primitives, by pre-populating the same
// registry that source modules land in. A later (import NAME) finds it there
// (ensure_loaded returns the record without touching the loader) and binds the
// primitives like any other module's exports. This is the embedder's hook for
// exposing capability-bearing primitives (MMIO/PCI/port-I/O/IRQ) as named
// modules instead of ambient globals -- see lisp.h. The bindings live wherever
// the current allocation heap points; the embedder calls this during single-core
// boot, so they land in the (soon-frozen) permanent system heap.
int lisp_register_builtin_module(lisp_value env, const char *name,
                                 const lisp_builtin_export *exports,
                                 size_t count) {
    lisp_value genv = global_env(env);
    lisp_value nm = lisp_make_symbol(name, strlen(name));
    if (nm == LISP_UNDEF)
        return 1;
    // Build the exports assoc list ((sym . prim) ...) the importer machinery
    // already knows how to bind.
    lisp_value alist = LISP_EMPTY;
    for (size_t i = 0; i < count; i++) {
        lisp_value sym = lisp_make_symbol(exports[i].name, strlen(exports[i].name));
        lisp_value prim = lisp_make_primitive(exports[i].fn, exports[i].name);
        if (sym == LISP_UNDEF || prim == LISP_UNDEF)
            return 1;
        lisp_value cell = lisp_cons(sym, prim);
        if (cell == LISP_UNDEF)
            return 1;
        lisp_value na = lisp_cons(cell, alist);
        if (na == LISP_UNDEF)
            return 1;
        alist = na;
    }
    const char *err = NULL;
    return reg_set(genv, nm, alist, &err) ? 0 : 1;
}
