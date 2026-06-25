// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#ifndef CARDINAL_LISP_H
#define CARDINAL_LISP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Cardinal; kernel-resident Scheme -- core value representation.
//
// See notes/core/lisp-substrate.md for the why. This is a Scheme dialect (the
// reader uses #t/#f, #\char, () with no nil, and Scheme truthiness: only #f is
// false). It deliberately deviates from classic Scheme in being immutable by
// default with persistent data structures -- that is load-bearing for the OS
// (persistence, checkpoints, lock-free concurrency), not a stylistic choice.
// The exceptions are a small set of explicitly MUTABLE container types -- bytes,
// vectors, and hash tables -- meant for in-place algorithmic state (driver
// buffers, the network stack's connection tables, etc.). They stay compatible
// with the shared-nothing model by being per-context and DEEP-COPIED on send, so
// no mutable object is ever shared across contexts.
//
// This is Phase 0: the tagged value representation plus reader/printer. The
// representation is the one decision that is painful to retrofit, so it is
// fixed here deliberately:
//
//   - A `lisp_value` is one machine word. The low 2 bits are a tag:
//       00  heap pointer (objects are >= 8-byte aligned, low 3 bits clear)
//       01  fixnum       (62-bit signed, value = (int64_t)v >> 2)
//       10  immediate    (#t/#f, empty list, char, eof; subtype in bits [7:2])
//       11  reserved      (second immediate space / future use)
//   - Fixnums are unboxed so integer arithmetic never allocates. Floats are
//     supported as heap-boxed flonums (a double does not fit beside the tag
//     bits). The runtime runs in task context, where SysTaskMgr saves/restores
//     FP state, so it uses native hardware doubles (its TUs are built with SSE,
//     overriding the kernel's blanket -mno-sse); no float ops in non-task
//     (ISR/early-boot) context.
//   - Every heap object begins with a `lisp_header` word whose layout reserves
//     space for a future GC (mark bits now; forwarding for a moving collector
//     later). v1 is a non-moving mark-sweep; nothing here precludes a nursery.

typedef uintptr_t lisp_value;

#define LISP_TAG_MASK 0x3u
#define LISP_TAG_PTR 0x0u
#define LISP_TAG_FIXNUM 0x1u
#define LISP_TAG_IMM 0x2u

// Immediate subtypes live in bits [7:2]; payload (e.g. a char codepoint) above.
// There is no nil (this is Scheme): booleans #t/#f and the empty list () are
// distinct values.
#define LISP_IMM_FALSE 0u
#define LISP_IMM_TRUE 1u
#define LISP_IMM_EMPTY 2u  // the empty list ()
#define LISP_IMM_CHAR 3u
#define LISP_IMM_EOF 4u    // reader end-of-input sentinel
#define LISP_IMM_UNDEF 5u  // unspecified / void

#define LISP_MK_IMM(subtype, payload) \
    ((lisp_value)(((uintptr_t)(payload) << 8) | ((uintptr_t)(subtype) << 2) | LISP_TAG_IMM))

#define LISP_FALSE LISP_MK_IMM(LISP_IMM_FALSE, 0)
#define LISP_TRUE LISP_MK_IMM(LISP_IMM_TRUE, 0)
#define LISP_EMPTY LISP_MK_IMM(LISP_IMM_EMPTY, 0)
#define LISP_EOF LISP_MK_IMM(LISP_IMM_EOF, 0)
#define LISP_UNDEF LISP_MK_IMM(LISP_IMM_UNDEF, 0)

// Heap object type tags (low byte of the header word).
typedef enum {
    LISP_OBJ_PAIR = 1,
    LISP_OBJ_SYMBOL = 2,
    LISP_OBJ_KEYWORD = 3,
    LISP_OBJ_STRING = 4,
    LISP_OBJ_ENV = 5,        // lexical environment frame (runtime plumbing)
    LISP_OBJ_CLOSURE = 6,    // lambda: params + body + captured env
    LISP_OBJ_PRIMITIVE = 7,  // built-in procedure backed by a C function
    LISP_OBJ_VECTOR = 8,     // immutable flat vector
    LISP_OBJ_FLONUM = 9,     // heap-boxed double (inexact real)
    LISP_OBJ_KONT = 10,      // a continuation frame (explicit-stack evaluator)
    LISP_OBJ_CTX = 11,       // an execution context (the CEK machine state)
    LISP_OBJ_BYTES = 12,     // a mutable byte buffer (driver MMIO/DMA + bulk IPC)
    LISP_OBJ_BCCLOSURE = 13, // a compiled closure: a bytecode chunk + captured cells (lbc.c)
    LISP_OBJ_HASHTABLE = 14, // a mutable equal?-keyed hash table (chained buckets)
    // reserved for later phases: BYTEVECTOR, BOX, ...
} lisp_objtype;

// Header bit layout: [7:0] type, [15:8] gc flags, [63:16] aux (type-specific,
// e.g. string/symbol length). A moving collector can later steal the whole word
// for a forwarding pointer (objects it forwards are distinguished by a gc flag).
#define LISP_GC_MARK 0x01u

typedef struct {
    uint64_t header;
} lisp_header;

#define LISP_HDR_TYPE(h) ((lisp_objtype)((h)->header & 0xffu))
#define LISP_HDR_AUX(h) ((h)->header >> 16)
#define LISP_MK_HEADER(type, aux) (((uint64_t)(aux) << 16) | ((uint64_t)(type) & 0xffu))

typedef struct {
    lisp_header h;
    lisp_value car;
    lisp_value cdr;
} lisp_pair;

// Symbols and keywords share a layout; aux holds the name length. (Interning
// comes in a later phase; for now each read produces a fresh object.)
typedef struct {
    lisp_header h;
    uint32_t hash;
    char name[];  // NUL-terminated; length in header aux
} lisp_named;

typedef struct {
    lisp_header h;
    char data[];  // length in header aux (NOT NUL-reliant; may contain NULs)
} lisp_string;

// A mutable byte buffer: the driver substrate's MMIO/DMA region + the bulk-data
// IPC message type. `data` points at inline storage trailing this header
// (owned == 1, freed with the object) or at FOREIGN memory -- an MMIO mapping or
// a DMA buffer (owned == 0, never freed by the GC; `phys` is its physical address,
// 0 for a non-DMA region). Accessors are volatile so MMIO reads/writes are not
// elided or reordered. Raw bytes have no Lisp children, so it is a GC leaf.
typedef struct {
    lisp_header h;
    uint8_t *data;
    size_t len;
    uint64_t phys;
    uint32_t owned;
} lisp_bytes;

typedef struct {
    lisp_header h;
    lisp_value items[];  // length in header aux
} lisp_vector;

// A mutable equal?-keyed hash table. `buckets` is a Lisp VECTOR of bucket lists;
// each bucket is a list of (key . value) pairs. Storing the buckets as a Lisp
// object (not a raw malloc) makes the whole table GC-traceable through a single
// child and frees with the mark-sweep -- no finalizer. Like bytes it is mutable
// per-context and DEEP-COPIED on send, so no mutable state is shared across
// contexts (shared-nothing IPC is preserved). `count` is the live entry count.
typedef struct {
    lisp_header h;
    lisp_value buckets;
    size_t count;
} lisp_hashtable;

typedef struct {
    lisp_header h;
    double val;
} lisp_flonum;

// --- Tag predicates ---------------------------------------------------------

static inline unsigned lisp_tag(lisp_value v) { return (unsigned)(v & LISP_TAG_MASK); }
static inline bool lisp_is_ptr(lisp_value v) { return lisp_tag(v) == LISP_TAG_PTR && v != 0; }
static inline bool lisp_is_fixnum(lisp_value v) { return lisp_tag(v) == LISP_TAG_FIXNUM; }
static inline bool lisp_is_imm(lisp_value v) { return lisp_tag(v) == LISP_TAG_IMM; }
static inline unsigned lisp_imm_subtype(lisp_value v) { return (unsigned)((v >> 2) & 0x3fu); }

static inline bool lisp_is_empty(lisp_value v) { return v == LISP_EMPTY; }
static inline bool lisp_is_eof(lisp_value v) { return v == LISP_EOF; }

// Scheme truthiness: only #f is false; everything else (incl. () and 0) is true.
static inline bool lisp_truthy(lisp_value v) { return v != LISP_FALSE; }

// --- Fixnums ----------------------------------------------------------------

#define LISP_FIXNUM_MAX ((int64_t)(((uint64_t)1 << 61) - 1))
#define LISP_FIXNUM_MIN (-((int64_t)1 << 61))

static inline lisp_value lisp_fixnum(int64_t x) {
    return (lisp_value)(((uintptr_t)x << 2) | LISP_TAG_FIXNUM);
}
static inline int64_t lisp_fixnum_val(lisp_value v) { return (int64_t)v >> 2; }

// --- Char -------------------------------------------------------------------

static inline lisp_value lisp_char(uint32_t cp) { return LISP_MK_IMM(LISP_IMM_CHAR, cp); }
static inline bool lisp_is_char(lisp_value v) {
    return lisp_is_imm(v) && lisp_imm_subtype(v) == LISP_IMM_CHAR;
}
static inline uint32_t lisp_char_val(lisp_value v) { return (uint32_t)(v >> 8); }

// --- Heap object access -----------------------------------------------------

static inline lisp_header *lisp_obj(lisp_value v) { return (lisp_header *)(uintptr_t)v; }
static inline lisp_value lisp_from_obj(void *p) { return (lisp_value)(uintptr_t)p; }

static inline bool lisp_is_objtype(lisp_value v, lisp_objtype t) {
    return lisp_is_ptr(v) && LISP_HDR_TYPE(lisp_obj(v)) == t;
}
static inline bool lisp_is_pair(lisp_value v) { return lisp_is_objtype(v, LISP_OBJ_PAIR); }
static inline bool lisp_is_symbol(lisp_value v) { return lisp_is_objtype(v, LISP_OBJ_SYMBOL); }
static inline bool lisp_is_keyword(lisp_value v) { return lisp_is_objtype(v, LISP_OBJ_KEYWORD); }
static inline bool lisp_is_string(lisp_value v) { return lisp_is_objtype(v, LISP_OBJ_STRING); }
static inline bool lisp_is_vector(lisp_value v) { return lisp_is_objtype(v, LISP_OBJ_VECTOR); }
static inline bool lisp_is_hashtable(lisp_value v) { return lisp_is_objtype(v, LISP_OBJ_HASHTABLE); }
static inline bool lisp_is_flonum(lisp_value v) { return lisp_is_objtype(v, LISP_OBJ_FLONUM); }
static inline bool lisp_is_bytes(lisp_value v) { return lisp_is_objtype(v, LISP_OBJ_BYTES); }
// A callable: a C primitive, a tree-walker closure, or a compiled (bytecode)
// closure. Anything that checks "is this a procedure" must accept all three.
static inline bool lisp_is_procedure(lisp_value v) {
    return lisp_is_objtype(v, LISP_OBJ_PRIMITIVE) ||
           lisp_is_objtype(v, LISP_OBJ_CLOSURE) ||
           lisp_is_objtype(v, LISP_OBJ_BCCLOSURE);
}

// A number is an exact fixnum or an inexact flonum.
static inline bool lisp_is_number(lisp_value v) { return lisp_is_fixnum(v) || lisp_is_flonum(v); }
static inline double lisp_flonum_val(lisp_value v) { return ((lisp_flonum *)(uintptr_t)v)->val; }
// Widen any number to double (for mixed arithmetic / comparison).
static inline double lisp_number_to_double(lisp_value v) {
    return lisp_is_fixnum(v) ? (double)lisp_fixnum_val(v) : lisp_flonum_val(v);
}

static inline lisp_value lisp_car(lisp_value v) { return ((lisp_pair *)lisp_obj(v))->car; }
static inline lisp_value lisp_cdr(lisp_value v) { return ((lisp_pair *)lisp_obj(v))->cdr; }

// --- Constructors (value.c) -------------------------------------------------

lisp_value lisp_cons(lisp_value car, lisp_value cdr);
lisp_value lisp_make_string(const char *data, size_t len);

// Symbols and keywords are interned (intern.c): equal names => identical object,
// so eq? is a pointer compare and dispatch is cheap.
lisp_value lisp_make_symbol(const char *name, size_t len);
lisp_value lisp_make_keyword(const char *name, size_t len);

// Mutable vectors. Like bytes, a vector is a per-context mutable object that is
// deep-copied on send, so shared-nothing IPC is preserved. lisp_vector_set is the
// in-place mutator backing vector-set!; lisp_vector_set_init is the identical
// store used by constructors building a fresh vector (clearer intent at the call
// site). The reader's #(...) literals and make-vector both yield mutable vectors.
lisp_value lisp_make_vector(size_t len, lisp_value fill);
size_t lisp_vector_length(lisp_value v);
lisp_value lisp_vector_ref(lisp_value v, size_t i);
void lisp_vector_set(lisp_value v, size_t i, lisp_value x);
void lisp_vector_set_init(lisp_value v, size_t i, lisp_value x);

// Mutable equal?-keyed hash tables. lisp_make_hashtable allocates a table with
// `nbuckets` empty buckets (rounded up to a power of two by the caller; the
// primitive layer picks a default). The buckets/count accessors are runtime
// plumbing for the primitive layer (prims.c) and the deep-copy path (sched.c).
lisp_value lisp_make_hashtable(size_t nbuckets);
lisp_value lisp_hashtable_buckets(lisp_value ht);
size_t lisp_hashtable_count(lisp_value ht);
void lisp_hashtable_set_buckets(lisp_value ht, lisp_value buckets);
void lisp_hashtable_set_count(lisp_value ht, size_t count);

lisp_value lisp_make_flonum(double x);

// Mutable byte buffers (driver substrate + bulk IPC). lisp_make_bytes allocates
// `len` zeroed, GC-owned bytes. lisp_make_bytes_foreign wraps EXTERNAL storage
// (an MMIO mapping or DMA buffer) the GC must not free; `phys` is its physical
// address (0 if none). The kernel mints foreign buffers from mmio-map / dma-alloc.
lisp_value lisp_make_bytes(size_t len);
lisp_value lisp_make_bytes_foreign(void *ptr, size_t len, uint64_t phys);
size_t lisp_bytes_len(lisp_value v);
void *lisp_bytes_data(lisp_value v);
uint64_t lisp_bytes_phys(lisp_value v);

const char *lisp_named_name(lisp_value v);  // symbol/keyword name (NUL-terminated)
size_t lisp_named_len(lisp_value v);
const char *lisp_string_data(lisp_value v);
size_t lisp_string_len(lisp_value v);

// --- Evaluator (eval.c / prims.c) -------------------------------------------

// A primitive procedure: receives an evaluated argument array. On error it
// returns LISP_UNDEF and, if `err` is non-NULL, points it at a static message.
typedef lisp_value (*lisp_primitive_fn)(lisp_value *args, int argc, const char **err);

lisp_value lisp_make_primitive(lisp_primitive_fn fn, const char *name);

// Lexical environments. A frame holds an assoc list of (symbol . value) bindings
// and a parent; lookup walks the chain. Environments are deliberately mutable
// runtime plumbing (define/set! update them in place) -- distinct from the
// immutable value model the language exposes to programs.
lisp_value lisp_make_env(lisp_value parent);
void lisp_env_define(lisp_value env, lisp_value sym, lisp_value val);
bool lisp_env_lookup(lisp_value env, lisp_value sym, lisp_value *out);
bool lisp_env_set(lisp_value env, lisp_value sym, lisp_value val);

// Evaluate `expr` in `env`. Returns LISP_UNDEF and sets *err on error. This is a
// thin synchronous wrapper that drives an explicit-stack CEK machine (see the
// execution-context API below) to completion. Proper tail calls use O(1)
// continuation frames; deep NON-tail recursion grows the heap, not the C stack.
lisp_value lisp_eval(lisp_value expr, lisp_value env, const char **err);

// Apply an already-evaluated procedure to an argument array. Used by eval and by
// higher-order primitives (map/for-each/apply). Returns LISP_UNDEF + *err on
// error. Like lisp_eval it runs a transient context to completion (atomic from a
// scheduler's view -- re-entrant primitives nest one of these).
lisp_value lisp_apply(lisp_value proc, lisp_value *args, int argc, const char **err);

// A garbage-collected heap (gc.c). Opaque: the shared system heap and each
// context's own heap are both lisp_heap_t. See the GC section below.
typedef struct lisp_heap lisp_heap_t;

// --- Execution contexts (the CEK machine; eval.c) ---------------------------
//
// A context is the interpreter's execution state made explicit and heap-resident
// (control expr, environment, accumulator, and a continuation-frame chain) so it
// can be suspended at a safe point, resumed, and precisely GC'd. This is the
// substrate the process model is built on: a "process" is a context. lisp_eval /
// lisp_apply above are completion-driven wrappers over this same machine.
typedef enum {
    LISP_CTX_EVAL = 0,       // internal: about to evaluate `control`
    LISP_CTX_APPLY = 1,      // internal: feed `accum` to the top frame
    LISP_CTX_DONE = 2,       // finished; result available via lisp_ctx_value
    LISP_CTX_ERROR = 3,      // aborted; message via lisp_ctx_error
    LISP_CTX_SUSPENDED = 4,  // reduction budget exhausted at a safe point; resumable
} lisp_ctx_status;

// Make a context that will evaluate `expr` in `env`. Returns a context value (a
// GC-managed object) or LISP_UNDEF on OOM. Hold the returned value as a root for
// as long as the context is live (it keeps its whole execution state alive).
lisp_value lisp_ctx_make(lisp_value expr, lisp_value env);

// Run the context for up to `budget` reductions. A reduction is one call or one
// loop back-edge -- the safe points where the machine can suspend; `budget` thus
// bounds the calls/loops before suspension, NOT every evaluation step. Call-free
// work between two safe points (e.g. building a literal, walking a let's bindings)
// runs uninterrupted, so a slice can exceed `budget` evaluation steps. Returns
// LISP_CTX_SUSPENDED if the budget ran out mid-computation (call again to
// continue), or LISP_CTX_DONE / LISP_CTX_ERROR when finished.
lisp_ctx_status lisp_ctx_resume(lisp_value ctx, int64_t budget);

// Result of a DONE context (LISP_UNDEF otherwise).
lisp_value lisp_ctx_value(lisp_value ctx);

// Static error message of an ERROR context (NULL otherwise).
const char *lisp_ctx_error(lisp_value ctx);

// Current status of a context (DONE/ERROR once finished, otherwise an internal
// EVAL/APPLY step). Lets a scheduler tell finished contexts from runnable ones.
lisp_ctx_status lisp_ctx_state(lisp_value ctx);

// 1 if the context is parked (blocked) waiting for a message, else 0. Read-only;
// sys-debug's (ctx-blocked? c) exposes it so a debugger can tell a parked context
// from a runnable one.
int lisp_ctx_is_blocked(lisp_value ctx);

// The context currently being resumed by the scheduler (LISP_EMPTY when none).
lisp_value lisp_current_ctx(void);

// This core's scheduler run queue (a list of live context values), or LISP_EMPTY
// if no scheduler is current. sys-debug's (ctx-list) exposes it for live
// enumeration + read-only inspection of scheduler-owned contexts.
lisp_value lisp_sched_queue(void);

// A context's capability set: LISP_UNDEF (unrestricted/root) or a list of the
// module-name symbols it may (import …). A restricted context can import only
// those modules, only ones already loaded, and cannot define-module. Set caps to
// launch a sandboxed context (e.g. a non-root serial REPL) from C; the spine is
// copied into the system heap. lisp_ctx_set_caps returns 0, or -1 on OOM (in
// which case it fails CLOSED -- caps is left granting NOTHING, never root). From
// Lisp, `(spawn-restricted caps thunk)` grants a subset of the spawner's own set
// and `(capabilities)` reports the running context's grant (#t when root).
lisp_value lisp_ctx_caps(lisp_value ctx);
int lisp_ctx_set_caps(lisp_value ctx, lisp_value caps);

// The control register: the expression a context (in the EVAL state) is about to
// evaluate next. Surfaced by the sys-debug capability for a Lisp single-stepper.
lisp_value lisp_ctx_control(lisp_value ctx);

// Wake / park a context. lisp_ctx_wake is ISR-SAFE (a single word write, no
// allocation or lock), so a native interrupt handler can wake a Lisp context
// parked on a hardware event -- the native-ISR -> event -> wake-context bridge.
void lisp_ctx_wake(lisp_value ctx);
void lisp_ctx_block(lisp_value ctx);

// Give a context its OWN heap, so its transient working data is collected
// independently and precisely (K3). Without this the context allocates into the
// shared system heap. Its environment may still reference shared data (the global
// env, interned symbols) -- those are external roots, never copied or collected by
// the context's GC. Returns 0 on success, <0 on OOM.
//
// CONTRACT (the shared region is immutable): a context with its own heap must
// treat any environment it did not itself create -- the shared global env and any
// enclosing frame from another heap -- as READ-ONLY. Its own `define`s land in its
// own (context-heap) frames, which is fine; but mutating a binding that lives in a
// shared/system env frame (`set!` of a global, or redefining one) makes a
// system-heap pair point into the context heap, which the context's GC will later
// free underneath it. That is unsound and unsupported in this stage; the frozen-
// shared-env of the full process model removes the footgun. Cross-context data is
// exchanged only by message passing (deep-copied into the receiver's heap), never
// by mutating shared state.
int lisp_ctx_attach_heap(lisp_value ctx);

// Live object count / collections-run of a context's own heap (0 if it has none).
size_t lisp_ctx_heap_live(lisp_value ctx);
size_t lisp_ctx_heap_collections(lisp_value ctx);

// --- Cooperative scheduler (sched.c) ----------------------------------------
//
// The contexts above become green-thread "processes" scheduled round-robin: each
// is resumed for a reduction slice, suspends at a safe point, and is requeued.
// This is the language-level process model (one scheduler per core in the kernel;
// a single global one host-side). Isolation is shared-nothing: messages between
// contexts are deep-copied (copy-on-send), cheap because values are immutable.
typedef struct {
    lisp_value queue;       // FIFO list of context values (the run set); a GC root
    int64_t slice;          // reductions granted per resume
    int per_context_heaps;  // if nonzero, spawn gives each context its own heap (K3)
} lisp_sched_t;

// Initialize a scheduler with the given per-resume reduction slice (<=0 -> a
// default) and make it the current one (spawn/send/yield/recv act on it). The
// struct must outlive the run and be reachable by the GC (e.g. a stack local).
void lisp_sched_init(lisp_sched_t *s, int64_t slice);

// Add an existing context to the run set (FIFO). spawn does this from Lisp.
// Returns false on OOM (the context was NOT enqueued) so the caller can react.
bool lisp_sched_add(lisp_sched_t *s, lisp_value ctx);

// Run the scheduler. Each pass resumes every runnable context once. Stops when
// all contexts have finished, when the remaining ones are all blocked (deadlock),
// or after `max_passes` passes (<=0 = unbounded). Returns the passes run.
int lisp_sched_run(lisp_sched_t *s, int max_passes);

// Install the scheduler primitives (spawn, yield, send, recv, ...) into `env`.
// Kept out of lisp_default_env so the base language has no scheduler dependency.
void lisp_install_sched(lisp_value env);

// A fresh global environment with the built-in primitives installed.
lisp_value lisp_default_env(void);

// Read and evaluate every form in `src`, returning the last result (or
// LISP_UNDEF + *err on the first failure). Convenience for tests/REPL.
lisp_value lisp_eval_string(const char *src, lisp_value env, const char **err);

// Install the built-in primitives into `env` (called by lisp_default_env).
void lisp_install_primitives(lisp_value env);

// Evaluate the Scheme-defined standard prelude into `env` (called by
// lisp_default_env, after the primitives). Returns 0 on success, <0 if the
// prelude itself failed to evaluate (a build-time bug).
int lisp_load_prelude(lisp_value env);

// --- Modules (module.c) -----------------------------------------------------
//
// Multi-file programs use two special forms (always recognized by the
// evaluator):
//
//   (define-module NAME (export a b ...) body...)
//       Evaluate `body` in a FRESH environment parented on the global env, so
//       its internal defines are private; then publish the listed bindings as
//       NAME's exports. NAME is a symbol; a file is normally exactly one such
//       form.
//
//   (import SPEC ...)
//       Load each module once (idempotent) and bind its chosen exports into the
//       calling environment. A SPEC is `name`, `(name (prefix p:))`, or
//       `(name (only a b ...))`. A module is located by NAME through the loader
//       hook below; loading a not-yet-seen module evaluates its source, which is
//       expected to (define-module NAME ...).
//
// The module registry lives inside the global environment (a hidden binding),
// so it is rooted by the GC for free; loading MUST therefore happen in the
// single-core boot window (before lisp_gc_set_multicore freezes the system
// heap), the same constraint top-level driver loading already has. A partially
// loaded module is marked so a circular import is reported rather than looping.

// Resolve a module NAME (as written in import/define-module) to its source
// bytes [*src, *src + *len). Return true if found. The bytes must remain valid
// for the duration of the load (embedder-owned, e.g. a pointer into the
// persistent initrd); NUL-termination is NOT required (the reader is bounded).
// The kernel maps NAME -> an initrd path; host tests map it to a file.
typedef bool (*lisp_module_loader_fn)(const char *name, const char **src,
                                      size_t *len, void *ctx);
void lisp_set_module_loader(lisp_module_loader_fn fn, void *ctx);

// --- Built-in (C-provided) modules ------------------------------------------

// One export of a built-in module: a Lisp name and the C primitive bound to it.
typedef struct {
    const char *name;
    lisp_primitive_fn fn;
} lisp_builtin_export;

// Register a built-in module: a subsequent (import NAME) binds these primitives
// into the importer's environment WITHOUT consulting the source loader (a
// built-in is just a pre-populated registry entry). This is how the embedder
// exposes capability-bearing C primitives -- port I/O, MMIO, PCI, IRQ -- as
// NAMED, importable modules rather than ambient globals: a context can only name
// a primitive whose module it imported, so withholding the import is withholding
// the authority (the W7-style lexical capability model; see
// notes/core/lisp-substrate.md). `env` may be any environment in the target
// global env's chain. Returns 0 on success, non-zero on out-of-memory. Like all
// module-registry mutation it must run single-core, before the system heap is
// frozen by lisp_gc_set_multicore.
int lisp_register_builtin_module(lisp_value env, const char *name,
                                 const lisp_builtin_export *exports,
                                 size_t count);

// Register the built-in `sys-debug` module (debug.c): the reflective debugging
// capability -- ctx-make/ctx-step/ctx-status/ctx-control/ctx-value/ctx-error,
// enough to write a single-stepping debugger in Lisp. Gated like any module, so
// only a context granted `sys-debug` can import it (driving/inspecting another
// context is authority a sandbox must not hold). Call during single-core boot.
void lisp_register_debug_module(lisp_value env);

// --- Reader (reader.c) ------------------------------------------------------

// Parse one datum from `*cursor`, advancing it past the consumed text. Returns
// LISP_EOF at end of input. On a parse error returns LISP_UNDEF and, if `err`
// is non-NULL, points it at a static message; `*cursor` is left at the offending
// byte (for an unterminated list/string, at the '(' / '"' that was never closed)
// so the caller can report where. `end` bounds the buffer.
lisp_value lisp_read(const char **cursor, const char *end, const char **err);

// Compute the 1-based line and column of `pos` within the buffer that starts at
// `base` (counting '\n' as the line break). Pair with lisp_read's error cursor to
// turn a parse failure into a "line L, column C" diagnostic. `line`/`col` may be
// NULL. If `base`/`pos` are NULL or pos<=base, reports line 1, column 1.
void lisp_source_location(const char *base, const char *pos, int *line, int *col);

// --- REPL engine (repl.c) ---------------------------------------------------

// Read every datum in [in, in+len), evaluate each in `env` (a persistent REPL
// environment, so definitions carry across calls), and write a human-readable
// transcript -- each value on its own line, or "error: <msg>" (a reader error
// also carrying "(line L, column C)") -- into `out` (NUL-terminated, capacity
// `cap`; truncates rather than overflowing). Returns the number of forms
// evaluated, or -1 if a reader error stopped the chunk. The serial shell is this
// engine wired to a CSMUX channel; it is also unit-tested standalone.
int lisp_repl_eval(const char *in, size_t len, lisp_value env, char *out, size_t cap);

// Like lisp_repl_eval but directs all evaluation allocation at the SYSTEM heap,
// so a persistent REPL env and its definitions survive across calls even when
// invoked from a context with its own per-context heap (which would otherwise
// reclaim them -- they are reachable only through `env`, not the caller's
// registers). This is the entry the in-OS serial shell uses; `env` should be a
// long-lived child of the global env kept reachable as a GC root.
int lisp_repl_serve(const char *in, size_t len, lisp_value env, char *out, size_t cap);

// --- Printer (print.c) ------------------------------------------------------

// Write a canonical (reader-faithful) textual form of `v` into `buf` (capacity
// `cap`). Returns the number of bytes that would be written (excluding the NUL),
// like snprintf, so truncation is detectable. Always NUL-terminates if cap > 0.
size_t lisp_print(lisp_value v, char *buf, size_t cap);

// Like lisp_print but in human (display) form: strings are unquoted and chars
// are bare. Same snprintf-style return.
size_t lisp_display(lisp_value v, char *buf, size_t cap);

// Output sink used by the display/write/newline procedures. The host test sets
// it to stdout; the kernel sets it to DEBUG_PRINT. If unset, output is dropped.
typedef void (*lisp_output_fn)(const char *s, size_t len, void *ctx);
void lisp_set_output(lisp_output_fn fn, void *ctx);

// --- Multi-core concurrency (gc.c) ------------------------------------------
//
// The runtime is single-threaded by default (host tests, the single-core
// kernel): the lock hooks are no-ops and every core resolves to slot 0. A
// multi-core embedder installs (1) a lock guarding the three pieces of shared
// mutable state -- the system heap's object list/counters, the intern table,
// and the GC mark scratch -- and (2) a function returning a stable small core
// index (0 <= id < LISP_MAX_CORES), so each core gets its own scheduler slot
// and allocation-target heap. Per-context heaps need NO lock: each is touched
// only by the one core running that context (collection aside, which takes the
// lock for the shared scratch).
#define LISP_MAX_CORES 256
void lisp_set_concurrency(void (*lock)(void), void (*unlock)(void), int (*core_id)(void));

// Switch the shared system heap to grow-only. Its collector roots conservatively
// from the *calling* core's C stack + registers, so once a second core is live
// it cannot see that core's stack roots and collecting it would free live data.
// After this call the system heap is never collected (interned symbols are
// permanent anyway, and post-boot system-heap churn is negligible); per-context
// heaps keep collecting precisely, per core. Call once, after self-test and
// before releasing the secondary cores. See notes/core/lisp-substrate.md (K5d).
void lisp_gc_set_multicore(int grow_only_system_heap);

// --- Garbage collection (gc.c) ----------------------------------------------
//
// Non-moving mark-sweep with PER-CONTEXT heaps. There is one shared SYSTEM heap
// (interned symbols, the global env/prelude, scheduler structures, and the
// context objects) collected CONSERVATIVELY (C stack + registers + the intern
// table); the calls below operate on it. A context may additionally own a heap
// for its transient data, collected PRECISELY from its CEK registers -- see
// lisp_ctx_attach_heap above and the lisp_heap_* helpers.
//
// The system collector is DISABLED until lisp_gc_init records a stack base;
// before that, allocation just grows the heap (so non-GC clients/tests are
// unaffected). `stack_base` must be an address in the OUTERMOST frame of the
// thread/task that runs the interpreter (e.g. the address of a local in main).
// The runtime must only allocate from that same thread, and float/GC must not run
// in ISR/early-boot context (see notes/core/lisp-substrate.md).
void lisp_gc_init(void *stack_base);

// Force a collection of the system heap now (no-op if uninitialized). Mainly for
// tests; normally collection is triggered automatically by allocation pressure.
void lisp_gc_collect(void);

size_t lisp_gc_live_count(void);    // system-heap live object count
size_t lisp_gc_collections(void);   // system-heap collections run so far

// Per-context heap helpers. A heap created here is owned by the runtime; it is
// freed when its context is freed, or explicitly via lisp_heap_free.
lisp_heap_t *lisp_heap_new(lisp_value owner_ctx);
void lisp_heap_free(lisp_heap_t *h);
void lisp_heap_collect(lisp_heap_t *h);   // precise collection from the owner's roots
size_t lisp_heap_live(lisp_heap_t *h);
size_t lisp_heap_collections(lisp_heap_t *h);

#endif
