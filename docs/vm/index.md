# Lisp VM Reference

> Reference for the Cardinal; kernel-resident Scheme-like bytecode VM (`libs/lisp/`):
> value types, reader syntax, special forms, the built-in primitives, the
> module/capability system, the concurrency model, the `sys-*` capability modules,
> and the 2D graphics library.

| | |
|---|---|
| **Source** | `libs/lisp/src/` |
| **Kind** | Kernel-resident Lisp bytecode VM |
| **Entry point** | `SysLisp.celf` — `lisp_scheduler_enter` (per-core loop) |
| **Evaluator** | Bytecode compiler + threaded register VM (`lbc.c`), driven through `lisp_ctx_resume` in `eval.c` |
| **Language** | Scheme-like; R7RS-inspired but deliberately smaller |

## Reference map

| Page | Covers |
|------|--------|
| **This page** | Overview, value types, the memory model, reader syntax, and cross-cutting notes |
| [Special forms](special-forms.md) | The forms the compiler knows: `define`, `lambda`, `let`, `cond`, `case`, … |
| [Core primitives](primitives.md) | The ambient (no-import) builtins: arithmetic, lists, strings, bytes, vectors, hash tables |
| [Concurrency & messaging](concurrency.md) | `spawn`, `send`, `recv`, ports, `sleep` |
| [Modules & capabilities](capabilities.md) | `define-module`, `import`, the sandbox gate, and the `sys-*` capability modules |
| [Graphics & driver libraries](graphics.md) | The `graphics` / `font` / `ttf` 2D library and `driver-util` |

For the *why* behind this surface, read the concept articles
[Capabilities & the sandbox](../concepts/capabilities-and-sandbox.md) and
[Message passing & concurrency](../concepts/message-passing.md).

---

## Overview

The Cardinal; Lisp VM is the OS layer above the `Sys*` kernel modules.  Every
server and driver is a `.clp` (Lisp source) file loaded from the initrd.  The
VM is:

- **Kernel-resident** — `libs/lisp` is compiled into `SysLisp.celf`, which
  loads before any server or driver.  There is no ring-3 userspace; the Lisp
  sandbox IS the userspace boundary (a capability-gated context, not an
  architectural privilege level).
- **A register-bytecode VM** — the compiler (`lbc.c`) lowers each context's
  source to bytecode the first time it is resumed, then runs it on a threaded
  register VM with proper tail calls. A context's execution state lives in a
  heap object (`lisp_ctx_t`), so a computation can be suspended at a safe point,
  resumed later, and traced precisely by the GC. `eval.c` drives this through
  `lisp_ctx_resume`.
- **Cooperative round-robin** — each context (green thread) runs for a
  reduction budget, then the scheduler advances.  Tail calls are O(1);
  non-tail recursion grows a heap-linked continuation chain rather than the C
  stack.
- **Shared-nothing message-passing** — `send` deep-copies every message
  (copy-on-send); context handles and grant objects are passed by identity.

### Value types

| Type | Description | Predicate |
|------|-------------|-----------|
| **fixnum** | 62-bit signed integer (exact); unboxed | `integer?`, `exact?` |
| **flonum** | IEEE 754 `double` (inexact) | `inexact?` |
| **boolean** | `#t` / `#f` | `boolean?` |
| **empty list** | `'()` — the proper-list terminator | `null?` |
| **pair** | Immutable cons cell; the list spine | `pair?` |
| **symbol** | Interned, case-sensitive atom | `symbol?` |
| **keyword** | Interned `:name` (colon-prefixed symbol) | n/a |
| **char** | Unicode codepoint (0..0x10FFFF) | `char?` |
| **string** | Immutable byte sequence | `string?` |
| **bytes** | Mutable byte buffer; the MMIO/DMA primitive | `bytes?` (no pred yet) |
| **vector** | Mutable, fixed-length, heterogeneous array | `vector?` |
| **hash-table** | Mutable `equal?`-keyed hash map | `hash-table?` |
| **procedure** | Closure or C primitive | `procedure?` |
| **context** | Scheduler context handle (green thread identity) | `ctx?` |
| **grant** | Unforgeable shared-memory capability | n/a |
| `#<undef>` | The "unspecified" result of mutating operations | — |
| `#<eof>` | End-of-input sentinel from the reader | — |

**Numeric tower.** All exact integers are 62-bit fixnums.  Flonum (inexact)
arithmetic is available: the usual contagion applies — any operation mixing an
inexact operand produces an inexact result.  Bignums are not yet implemented;
an integer literal outside the fixnum range is treated as a symbol.

**Immutability.** Pairs and strings are immutable.  Vectors and bytes are
mutable in place; both are deep-copied on `send` (shared-nothing IPC
contract).  Symbols and keywords are interned: `eq?` on two symbols with the
same name is always `#t`.

### Memory model and GC

Each context has its own precisely-collected per-context heap (mark-sweep).
The shared system heap (interned symbols, the module registry, context objects,
grant table) is conservative-collected and frozen once the secondary cores are
released (grow-only afterwards).  `bytes` wrapping foreign MMIO/DMA memory
(`lisp_make_bytes_foreign`) are GC leaves; the GC does not free their backing.

---

## Reader syntax

The reader (`libs/lisp/src/reader.c`) recognizes the following literal forms:

### Integers

```scheme
42     -7     +100    ; decimal, signed
#x1F   #xDEAD         ; hexadecimal (lowercase x; uppercase X also accepted)
#b1010 #B0101          ; binary
```

An integer literal outside the 62-bit fixnum range is silently read as a
symbol (bignums are a future phase).

### Flonums

```scheme
3.14    -0.5    1.0e10    2.5E-3
```

Requires at least one of `.` or `e/E` (so `1` is a fixnum, `1.0` is a
flonum).  Conversion is not perfectly rounded; adequate for literals.

### Booleans

```scheme
#t   #true    ; true
#f   #false   ; false
```

### Characters

```scheme
#\a   #\Z   #\0   #\space   #\newline
```

Single character after `#\`.  Named characters `#\space`, `#\newline`,
`#\tab`, `#\return`, `#\nul` are recognized.

### Strings

```scheme
"hello"   "line1\nline2"   "tab\there"   "embedded\"quote"
```

Escape sequences: `\n`, `\t`, `\r`, `\0`, `\\`, `\"`.  Strings may contain
embedded NUL bytes; length is tracked separately.

### Lists and dotted pairs

```scheme
(1 2 3)           ; proper list
(a . b)           ; dotted pair
(1 2 . 3)         ; improper list
()                ; empty list
```

### Vectors

```scheme
#(1 "two" #t)     ; literal vector
```

Parsed as an immutable vector (contents are the same forms as list elements).

### Quote shorthands

| Sugar | Expands to |
|-------|------------|
| `'expr` | `(quote expr)` |
| `` `expr `` | `(quasiquote expr)` |
| `,expr` | `(unquote expr)` |
| `,@expr` | `(unquote-splicing expr)` |

### Comments

`;` begins a line comment (to end-of-line).  Block comments (`#|...|#`) are
not supported.

---

## Notes and gotchas

- **`sleep` wakes early on `send`.** `sleep` parks the context using the same
  blocked flag as `recv`; a `send` clears it, waking the context before the
  deadline.  A port-bound context (one that also receives messages) cannot
  rely on `sleep` for a precise delay.  Use an uptime deadline with
  `uptime-ns` instead.

- **Bytes mutators reject read-only grants.** Every `bytes-*-set!`,
  `bytes-fill32!`, `bytes-copy!`, and `gfx-*!` operation checks the
  destination's read-only flag.  This is the complete enforcement boundary in
  the sandbox (no ring protection — but the Lisp sandbox cannot remap
  addresses, so the software check is airtight).

- **`send` cannot carry procedures or environments.** Only data (pairs, strings,
  vectors, etc.) and handle types (contexts, grants) survive `send`.  Passing a
  closure across a context boundary is a programming error.

- **Bytecode compilation happens at first resume.** A form that the compiler
  cannot lower (e.g. a malformed special form) surfaces as a `LISP_CTX_ERROR`
  with a string error message at first `ctx-resume`, not at read time.

- **No `call/cc`, no `syntax-rules`.** These were intentionally omitted to
  keep the substrate small.  The `case` / `cond` / `while` / `when` / `unless`
  derived forms are recognized by the compiler directly (not macro-expanded).

- **No `try/catch`.** `error` aborts the current evaluation.  For servers that
  must survive bad input, validate all client data before processing.  `reply-to`
  guards the reply path; broader error isolation is by context boundary (a
  misbehaving client context dies; the server is unaffected).

- **Fixnum width is 62 bits, not 64.** The top 2 bits are used for the
  immediate-value tag.  Physical addresses and register values up to ~4 EiB are
  fine; a `uint64_t` value with the top bits set will not fit.

- **`(import sys-debug)` is for debugging only.** Stepping a scheduler-owned
  context from outside the scheduler is a data race; only use `ctx-step` on
  contexts you created with `ctx-make`, or on contexts you have first
  `ctx-pause`d.

- **Module source is loaded synchronously at boot.** `import` runs the module
  body to completion (no suspend/resume) during the single-core boot window,
  before the system heap is frozen.  Drivers may safely import each other
  (as long as there are no circular imports — those are caught and reported).
