# Lisp VM API Reference

> Complete reference for the Cardinal; kernel-resident Scheme-like bytecode VM
> (`libs/lisp/`), covering value types, reader syntax, special forms, all
> built-in primitives, the module/capability system, the concurrency model, the
> sys-* driver capability modules, and the 2D graphics library.

| | |
|---|---|
| **Source** | `libs/lisp/src/` |
| **Kind** | Kernel-resident Lisp bytecode VM |
| **Entry point** | `SysLisp.celf` — `lisp_scheduler_enter` (per-core loop) |
| **Evaluator** | Bytecode compiler + threaded register VM (`lbc.c`), driven through `lisp_ctx_resume` in `eval.c` |
| **Language** | Scheme-like; R7RS-inspired but deliberately smaller |

---

## Contents

1. [Overview](#1-overview) — what the VM is, value types, memory model
2. [Reader syntax](#2-reader-syntax) — literals the reader recognizes
3. [Special forms](#3-special-forms) — the forms the compiler knows
4. [Core primitives](#4-core-primitives) — the ambient (no-import) builtins
5. [Modules and capabilities](#5-modules-and-capabilities) — `define-module`, `import`, the sandbox gate
6. [Concurrency and messaging](#6-concurrency-and-messaging) — `spawn`, `send`, `recv`, `sleep`
7. [The sys-* capability modules](#7-the-sys-capability-modules) — hardware authority by import
8. [2D graphics and font library](#8-2d-graphics-and-font-library) — `graphics`, `font`, `ttf`
9. [`driver-util` module](#9-driver-util-module-shared-driver-utilities) — shared driver helpers
10. [Notes and gotchas](#10-notes-and-gotchas)

For the *why* behind this surface, read the concept articles:
[Capabilities & the sandbox](../concepts/capabilities-and-sandbox.md) and
[Message passing & concurrency](../concepts/message-passing.md).

---

## 1. Overview

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

### 1.1 Value types

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

### 1.2 Memory model and GC

Each context has its own precisely-collected per-context heap (mark-sweep).
The shared system heap (interned symbols, the module registry, context objects,
grant table) is conservative-collected and frozen once the secondary cores are
released (grow-only afterwards).  `bytes` wrapping foreign MMIO/DMA memory
(`lisp_make_bytes_foreign`) are GC leaves; the GC does not free their backing.

---

## 2. Reader syntax

The reader (`libs/lisp/src/reader.c`) recognizes the following literal forms:

### 2.1 Integers

```scheme
42     -7     +100    ; decimal, signed
#x1F   #xDEAD         ; hexadecimal (lowercase x; uppercase X also accepted)
#b1010 #B0101          ; binary
```

An integer literal outside the 62-bit fixnum range is silently read as a
symbol (bignums are a future phase).

### 2.2 Flonums

```scheme
3.14    -0.5    1.0e10    2.5E-3
```

Requires at least one of `.` or `e/E` (so `1` is a fixnum, `1.0` is a
flonum).  Conversion is not perfectly rounded; adequate for literals.

### 2.3 Booleans

```scheme
#t   #true    ; true
#f   #false   ; false
```

### 2.4 Characters

```scheme
#\a   #\Z   #\0   #\space   #\newline
```

Single character after `#\`.  Named characters `#\space`, `#\newline`,
`#\tab`, `#\return`, `#\nul` are recognized.

### 2.5 Strings

```scheme
"hello"   "line1\nline2"   "tab\there"   "embedded\"quote"
```

Escape sequences: `\n`, `\t`, `\r`, `\0`, `\\`, `\"`.  Strings may contain
embedded NUL bytes; length is tracked separately.

### 2.6 Lists and dotted pairs

```scheme
(1 2 3)           ; proper list
(a . b)           ; dotted pair
(1 2 . 3)         ; improper list
()                ; empty list
```

### 2.7 Vectors

```scheme
#(1 "two" #t)     ; literal vector
```

Parsed as an immutable vector (contents are the same forms as list elements).

### 2.8 Quote shorthands

| Sugar | Expands to |
|-------|------------|
| `'expr` | `(quote expr)` |
| `` `expr `` | `(quasiquote expr)` |
| `,expr` | `(unquote expr)` |
| `,@expr` | `(unquote-splicing expr)` |

### 2.9 Comments

`;` begins a line comment (to end-of-line).  Block comments (`#|...|#`) are
not supported.

---

## 3. Special forms

The bytecode compiler (`lbc.c`) recognizes the following special forms.
Everything else is a procedure call.

### `(quote datum)` / `'datum`

Returns `datum` unevaluated.

```scheme
'(1 2 3)     ; => (1 2 3)
'foo         ; => foo (the symbol)
```

### `(quasiquote template)` / `` `template ``

Backquote template: nested `unquote` and `unquote-splicing` are evaluated;
everything else is quoted.

```scheme
(let ((x 5)) `(the value is ,x))    ; => (the value is 5)
(let ((xs '(2 3))) `(1 ,@xs 4))     ; => (1 2 3 4)
```

### `(if test consequent [alternative])`

Evaluates `test`; if truthy, evaluates and returns `consequent`, else
`alternative` (or `#<undef>` if omitted).  Only `#f` is falsy.

### `(cond (test expr ...) ... [(else expr ...)])`

Evaluates each `test` in order; the first truthy clause's body is evaluated.
`(test => proc)` applies `proc` to the test value.  The `else` clause is a
catch-all.

### `(when test body ...)`

Evaluates `body ...` as a `begin` when `test` is truthy; returns `#<undef>`
when false.

### `(unless test body ...)`

Like `when` but evaluates body when `test` is falsy.

### `(and expr ...)`

Evaluates left-to-right; returns the first falsy value or the last value.
`(and)` returns `#t`.

### `(or expr ...)`

Returns the first truthy value or the last value.  `(or)` returns `#f`.

### `(begin expr ...)`

Evaluates expressions left-to-right; returns the value of the last one.

### `(define name expr)` / `(define (name params ...) body ...)`

Binds `name` in the current environment.  The procedure shorthand desugars to
`(define name (lambda (params ...) body ...))`.  Inside a `lambda` or `let`
body, `define` forms create local bindings visible throughout the body (internal
defines).

### `(set! name expr)`

Mutates an existing binding (must already be defined); errors if `name` is
unbound.

### `(lambda (params ...) body ...)` / `(lambda (params ... . rest) body ...)`

Creates a closure.  A trailing `. rest` parameter collects excess arguments
into a list.  `(lambda args body ...)` collects all arguments.

### `(let ((var init) ...) body ...)`

Bind variables (all `init`s evaluated before any binding is visible), then
evaluate `body ...` in the extended environment.

### `(let* ((var init) ...) body ...)`

Like `let` but each `init` can refer to the preceding bindings.

### `(letrec ((var init) ...) body ...)`

Mutual-recursion binding: all `var`s are bound before any `init` is evaluated
(init expressions may reference each other).

### `(let loop-name ((var init) ...) body ...)` — named let

Named-let / looping construct.  `loop-name` is bound to a procedure that
re-enters the loop with new values.

```scheme
(let loop ((i 0) (acc 0))
  (if (> i 5) acc (loop (+ i 1) (+ acc i))))   ; => 15
```

### `(while test body ...)`

Repeatedly evaluates `body ...` as long as `test` is truthy; returns
`#<undef>`.

```scheme
(let ((i (make-vector 1 0)))
  (while (< (vector-ref i 0) 5)
    (vector-set! i 0 (+ (vector-ref i 0) 1))))
```

### `(case key (datums expr ...) ... [(else expr ...)])`

Evaluates `key`; compares with each clause's datum list using `eqv?`; evaluates
the first matching clause's body.  `else` is a catch-all.

```scheme
(case x
  ((1 2) "small")
  ((3 4) "medium")
  (else  "large"))
```

### `(define-module name (export sym ...) body ...)`

Defines a module (see Section 5).  Root-context only.

### `(import spec ...)` / `(include part ...)`

Loads and binds a module's exports (see Section 5).

---

## 4. Core primitives

All primitives below are available in the default environment without any
`import`.

### 4.1 Arithmetic

| Signature | Description |
|-----------|-------------|
| `(+ n ...)` | Sum; exact if all args are fixnums, else inexact |
| `(- n ...)` | Difference; `(- n)` negates |
| `(* n ...)` | Product |
| `(/ n ...)` | Division; exact only when all fixnums and all divisions are even; otherwise inexact |
| `(modulo n d)` | Remainder with sign of divisor (R7RS) |
| `(remainder n d)` | Remainder with sign of dividend (C `%`) |
| `(quotient n d)` | Truncating integer division |

All arithmetic is variadic (zero or more args for `+`/`*`; at least one for
`-`/`/`).  Integer overflow wraps (bignums are not yet implemented).

### 4.2 Numeric comparison

All comparison operators are variadic and support chaining: `(< 1 2 3)` is
`#t`.

| Primitive | Description |
|-----------|-------------|
| `(= n ...)` | Numeric equality |
| `(< n ...)` | Strictly less than |
| `(> n ...)` | Strictly greater than |
| `(<= n ...)` | Less than or equal |
| `(>= n ...)` | Greater than or equal |

Mixed exact/inexact comparison converts the exact operand to `double` (a
fixnum magnitude > 2^53 may lose low bits — spec-permitted).

### 4.3 Numeric predicates and coercions

| Signature | Description |
|-----------|-------------|
| `(zero? n)` | `#t` if `n` is zero (exact or inexact) |
| `(positive? x)` | `#t` if `x > 0` (prelude) |
| `(negative? x)` | `#t` if `x < 0` (prelude) |
| `(even? n)` | `#t` if `n` is even (prelude) |
| `(odd? n)` | `#t` if `n` is odd (prelude) |
| `(abs x)` | Absolute value (prelude) |
| `(number? x)` | `#t` for any number |
| `(real? x)` | Alias for `number?` (all numbers are real) |
| `(integer? x)` | `#t` for fixnums and integral flonums |
| `(exact? n)` | `#t` for fixnums |
| `(inexact? n)` | `#t` for flonums |
| `(exact n)` / `(inexact->exact n)` | Convert flonum to fixnum (errors if not integral or out of range) |
| `(inexact n)` / `(exact->inexact n)` | Convert fixnum to flonum |
| `(number->string n)` | Render number to a string |
| `(string->number s)` | Parse a number string; returns `#f` if not a number |

### 4.4 Numeric extras (prelude)

| Signature | Description |
|-----------|-------------|
| `(add1 n)` | `(+ n 1)` |
| `(sub1 n)` | `(- n 1)` |
| `(max x xs ...)` | Maximum |
| `(min x xs ...)` | Minimum |
| `(expt base e)` | Integer exponentiation (exact, iterative) |
| `(gcd n ...)` | Greatest common divisor |

### 4.5 Bitwise (driver substrate)

All bitwise operations work on fixnums (62 signed bits).

| Signature | Description |
|-----------|-------------|
| `(bitwise-and n ...)` | Bitwise AND; identity is `-1` (all ones) |
| `(bitwise-or n ...)` | Bitwise OR; identity is `0` |
| `(bitwise-xor n ...)` | Bitwise XOR; identity is `0` |
| `(bitwise-not n)` | Bitwise complement |
| `(arithmetic-shift v count)` | Shift left (count > 0) or right (count < 0); arithmetic (sign-preserving) |
| `(bit-extract v lo width)` | Extract `width` bits of `v` starting at bit `lo`, right-justified |
| `(bit-insert v lo width field)` | Return `v` with its `width` bits at `lo` replaced by the low `width` bits of `field` |

```scheme
(bit-extract #xFF 4 4)       ; => 15 (bits 7..4)
(bit-insert  #x0F 4 4 #xA)  ; => #xAF
(arithmetic-shift 1 8)       ; => 256
```

### 4.6 Booleans

| Signature | Description |
|-----------|-------------|
| `(not x)` | Logical negation; only `#f` is false |
| `(boolean? x)` | `#t` for `#t` or `#f` |
| `(boolean=? a b)` | `#t` if both are the same boolean (prelude; same as `eq?`) |

### 4.7 Equality

| Signature | Description |
|-----------|-------------|
| `(eq? a b)` | Identity: pointer equality for heap objects; value equality for fixnums, booleans, chars, symbols (all unboxed or interned) |
| `(equal? a b)` | Deep structural equality over pairs, vectors, strings, bytes, flonums; `eq?` for everything else |

`eqv?` is not a distinct primitive; it is aliased appropriately in the prelude.

### 4.8 Pairs and lists

| Signature | Description |
|-----------|-------------|
| `(cons a d)` | Create an immutable pair |
| `(car p)` | First element |
| `(cdr p)` | Rest |
| `(list x ...)` | Build a proper list |
| `(pair? x)` | `#t` for pairs |
| `(null? x)` | `#t` for `'()` |
| `(length lst)` | Length of a proper list |
| `(reverse lst)` | Reverse a proper list |
| `(append lst ...)` | Concatenate lists; last argument used as-is (may be improper) |
| `(list-ref lst k)` | Element at zero-based index `k` |

**Prelude list utilities:**

| Signature | Description |
|-----------|-------------|
| `(list? x)` | `#t` iff `x` is a proper list (nil-terminated) |
| `(list-tail lst k)` | Tail starting at index `k` |
| `(list-copy lst)` | Shallow copy |
| `(fold-left f acc lst)` | Left fold |
| `(fold-right f acc lst)` | Right fold (via left + reverse; stack-safe) |
| `(reduce f ridentity lst)` | `fold-left` with `(car lst)` as initial accumulator |
| `(filter pred lst)` | List of elements satisfying `pred` |
| `(assq key lst)` | Find `(key . val)` in an association list using `eq?` |
| `(assv key lst)` | Alias for `assq` (correct for fixnum/symbol keys) |
| `(assoc key lst)` | Find using `equal?` |
| `(memq x lst)` | Find `x` using `eq?`; returns the tail |
| `(memv x lst)` | Alias for `memq` |
| `(member x lst)` | Find `x` using `equal?` |

**Prelude c[ad]+r accessors:**
`caar`, `cadr`, `cdar`, `cddr`, `caddr`, `cdddr`, `cadddr`,
`first`, `second`, `third`.

### 4.9 Symbols

| Signature | Description |
|-----------|-------------|
| `(symbol? x)` | `#t` for symbols |
| `(symbol->string sym)` | Symbol name as a fresh string |
| `(string->symbol s)` | Intern a string as a symbol |

### 4.10 Strings

Strings are immutable sequences of bytes.

| Signature | Description |
|-----------|-------------|
| `(string? x)` | `#t` for strings |
| `(string-length s)` | Byte length |
| `(string-ref s i)` | Char at byte index `i` |
| `(substring s start end)` | Fresh string of bytes `[start, end)` |
| `(string-append s ...)` | Concatenate strings |
| `(string=? s ...)` | Lexicographic equality (chained) |
| `(string<? s ...)` | Lexicographic less-than (chained) |
| `(string->list s)` | List of chars |
| `(list->string chars)` | String from a list of chars |
| `(string ch ...)` | String from char arguments |
| `(make-string n [ch])` | String of length `n`, filled with `ch` (default space) |

### 4.11 Characters

Characters are Unicode codepoints stored as 21-bit unsigned values.

| Signature | Description |
|-----------|-------------|
| `(char? x)` | `#t` for characters |
| `(char->integer ch)` | Codepoint value |
| `(integer->char n)` | Character from codepoint (0..0x10FFFF) |
| `(char=? ch ...)` | Equality (chained) |
| `(char<? ch ...)` | Less-than (chained) |
| `(char>? ch ...)` | Greater-than (chained) |
| `(char<=? ch ...)` | Less-or-equal (chained) |
| `(char>=? ch ...)` | Greater-or-equal (chained) |
| `(char-upcase ch)` | ASCII uppercase |
| `(char-downcase ch)` | ASCII lowercase |
| `(char-alphabetic? ch)` | ASCII letter |
| `(char-numeric? ch)` | ASCII digit |
| `(char-whitespace? ch)` | Space, tab, newline, return, form-feed |

### 4.12 Bytes

`bytes` is the VM's mutable byte buffer type — the foundation for MMIO
registers, DMA buffers, framebuffers, and network packets.

**Construction:**

| Signature | Description |
|-----------|-------------|
| `(make-bytes n)` | Fresh zeroed mutable buffer of `n` bytes |
| `(bytes-length b)` | Byte count |
| `(bytes-phys b)` | Physical address (DMA/MMIO regions only; `0` for heap buffers) |

**Width-specific volatile accessors** (use for MMIO registers; generates a
single natural-width volatile load or store — correct for memory-mapped I/O):

| Signature | Description |
|-----------|-------------|
| `(bytes-u8-ref b i)` | Read 1 byte at byte offset `i` |
| `(bytes-u16-ref b i)` | Read 2 bytes (LE) at `i` |
| `(bytes-u32-ref b i)` | Read 4 bytes (LE) at `i` |
| `(bytes-u64-ref b i)` | Read 8 bytes (LE) at `i` |
| `(bytes-u8-set! b i v)` | Write 1 byte |
| `(bytes-u16-set! b i v)` | Write 2 bytes (LE) |
| `(bytes-u32-set! b i v)` | Write 4 bytes (LE) |
| `(bytes-u64-set! b i v)` | Write 8 bytes (LE) |

**Bulk non-volatile operations** (use for data — framebuffers, DMA buffers,
network packets — NOT MMIO registers; compiler may vectorize):

| Signature | Description |
|-----------|-------------|
| `(bytes-fill32! b byte-off count color)` | Fill `count` 32-bit words at `byte-off` with `color`; requires 4-alignment |
| `(bytes-copy! dst doff src soff len)` | `memmove` semantics — safe for overlapping in-buffer shifts |
| `(sfence)` | x86 `sfence` — drain write-combining buffers before scanout; required after compositing to a WC-mapped framebuffer |

> **Warning:** A per-element Lisp loop over `bytes-u32-set!` is ~200–650x
> slower than `bytes-fill32!`/`bytes-copy!` for bulk data.  Always use the
> bulk primitive for framebuffers and packet buffers.

**Read-only grants.** A `bytes` obtained via `map-grant` with `'ro`
permissions is marked read-only; any attempt to write it via the bytes mutators
raises an error.  This enforces software read-only boundaries in the Lisp
sandbox (no ring separation, but airtight within the VM).

**Prelude region / register DSL:**

The prelude defines a tiny DSL for hardware registers over a `bytes` buffer:

```scheme
; Returns an accessor closure: (acc) reads, (acc v) writes
(define (register region offset size) ...)
; Returns an accessor closure over a bit field
(define (field reg lo width) ...)

; Example — map a PCI BAR and define a command register:
(let* ((cfg (mmio-map ecam-phys 4096))
       (cmd (register cfg #x04 2))
       (bus-master (field cmd 2 1)))
  (bus-master 1)      ; set bit 2
  (cmd))              ; read back
```

`region-ref` and `region-set!` are the underlying primitives (dispatch to
`bytes-u{8,16,32,64}-{ref,set!}` by size).

### 4.13 Vectors

Vectors are mutable, fixed-length, heterogeneous arrays.  All elements are
initialized to `fill` (default `#f`) at construction.

| Signature | Description |
|-----------|-------------|
| `(vector x ...)` | Construct a vector from arguments |
| `(make-vector n [fill])` | Allocate a vector of length `n` |
| `(vector? x)` | `#t` for vectors |
| `(vector-length v)` | Number of elements |
| `(vector-ref v i)` | Element at index `i` |
| `(vector-set! v i x)` | Mutate element `i` |
| `(vector-fill! v x)` | Set all elements to `x` |
| `(vector-copy! dst doff src soff len)` | Bulk element copy (handles overlaps) |
| `(vector->list v)` | Convert to a list |
| `(list->vector lst)` | Convert from a list |

Vectors are deep-copied on `send` (shared-nothing IPC).

**Mutable box idiom** (from `driver-util`):

```scheme
(define (make-cell v) (make-vector 1 v))
(define (cell-ref  c) (vector-ref c 0))
(define (cell-set! c v) (vector-set! c 0 v))
```

This is the idiomatic mutable single-value cell — safer than a bytes-based
approach because a vector slot is GC-traced.

### 4.14 Hash tables

Hash tables are mutable, `equal?`-keyed, and deep-copied on `send`.  The hash
function is content-based (FNV-64 over structure) so lists, strings, bytes,
numbers, and vectors all work as keys.  The load factor triggers rehashing at
1.0 (doubles bucket count).

| Signature | Description |
|-----------|-------------|
| `(make-hash-table)` | Create an empty hash table |
| `(hash-table? x)` | `#t` for hash tables |
| `(hash-set! ht key val)` | Insert or overwrite `key -> val` |
| `(hash-ref ht key [default])` | Look up `key`; returns `default` or errors if absent |
| `(hash-has-key? ht key)` | `#t` if `key` is present |
| `(hash-remove! ht key)` | Remove `key`; returns `#t` if found |
| `(hash-count ht)` | Number of entries |
| `(hash-keys ht)` | List of all keys (unspecified order) |
| `(hash-values ht)` | List of all values |
| `(hash->list ht)` | List of `(key . val)` pairs |
| `(hash-for-each ht proc)` | Apply `(proc key val)` to every entry over a snapshot |

```scheme
(let ((h (make-hash-table)))
  (hash-set! h "x" 42)
  (hash-ref  h "x"))    ; => 42
```

### 4.15 Higher-order

| Signature | Description |
|-----------|-------------|
| `(apply proc arg ... lst)` | Apply `proc` to `arg ...` followed by elements of `lst` |
| `(map proc lst)` | Apply `proc` to each element; return list of results |
| `(for-each proc lst)` | Apply `proc` to each element for side effects; return `#<undef>` |

`map` and `for-each` accept exactly one list argument (multi-list map is not
implemented).

### 4.16 Identity and miscellaneous

| Signature | Description |
|-----------|-------------|
| `(procedure? x)` | `#t` for any callable |
| `(identity x)` | Returns `x` (prelude) |

### 4.17 Output and errors

| Signature | Description |
|-----------|-------------|
| `(display x)` | Print `x` without quotes/escapes (human-readable) to the debug sink (COM1) |
| `(write x)` | Print `x` in reader-faithful form (strings quoted, chars as `#\a`) |
| `(newline)` | Print a newline |
| `(error msg irritants ...)` | Abort the current evaluation with `msg` (a string); irritants are currently unused |

Output goes to the COM1 debug log through `lisp_set_output`; in OS context
that is `print_str` routing to SysDebug.

---

## 5. Modules and capabilities

### 5.1 `define-module`

```scheme
(define-module name
  (export sym ...)
  body ...)
```

Evaluates `body ...` in a fresh private environment parented on the global env,
then publishes the listed symbols as the module's exports.  **Root-context
only** — a restricted context cannot define modules (preventing authority
escalation).

```scheme
(define-module my-lib
  (export add square)
  (define (add a b) (+ a b))
  (define (square x) (* x x)))
```

### 5.2 `import`

```scheme
(import name ...)
(import (name (only sym ...)) ...)
(import (name (prefix pfx)) ...)
```

Loads `name`'s source (via the module loader, which maps `name` to
`./lisp/<name>.clp` in the initrd) if not already loaded, then binds the
exports into the current environment.

- `(only sym ...)` — import only the listed names.
- `(prefix pfx)` — bind each export as `<pfx><name>`.

**Capability gating.** A restricted context (spawned with `spawn-restricted`)
may only import modules explicitly listed in its grant, and only ones already
loaded.  It cannot cause new source to be evaluated.  An unrestricted (root)
context may import anything.

```scheme
; Unrestricted (driver init):
(import driver-util)
(import sys-mmio)

; Restricted (sandboxed service):
; may only import whatever was in (spawn-restricted '(corenetwork ...) thunk)
```

### 5.3 `include`

```scheme
(include part ...)
```

Inside a `define-module` body: splices sibling `.clp` files into the module's
private environment.  Each `part` resolves to `<module>/<part>.clp` in the
initrd.  Part files are NOT modules and cannot be imported directly — they
share the parent module's namespace.

### 5.4 Module loader

The kernel module loader searches these directories in order:

1. `./lisp/`
2. `./lisp/lib/`
3. `./lisp/servers/`
4. `./lisp/drivers/`

Module names cannot contain `/` or `..` (path-escape is blocked).

### 5.5 Capabilities and the sandbox boundary

The Lisp VM sandbox is Cardinal;'s userspace: a capability-gated Lisp context,
not an architectural ring-3.  The VM enforces the boundary in software:

- `spawn-restricted` mints a context with a fixed import list.
- `import` checks the running context's capability set at runtime.
- Read-only grant enforcement is done in the bytes mutators.
- `define-module` is root-only.

There is no hardware page-level enforcement between Lisp contexts; the VM and
the `sys-*` capability modules provide the boundary.

---

## 6. Concurrency and messaging

### 6.1 Contexts (green threads)

| Signature | Description |
|-----------|-------------|
| `(spawn thunk)` | Create and enqueue a new context running `(thunk)`; returns its handle |
| `(spawn-restricted caps thunk)` | Like `spawn` but the new context can only import the listed module names; an unrestricted spawner may grant any; a restricted spawner may not grant more than it has |
| `(self)` | The handle of the running context; `#f` outside the scheduler |
| `(ctx? x)` | `#t` if `x` is a context handle |
| `(yield)` | Surrender the remainder of this time slice; resume at the next scheduler pass |
| `(capabilities)` | `#t` if the current context is unrestricted (root); otherwise the list of module-name symbols it may import |

```scheme
(define worker
  (spawn (lambda ()
    (display "worker running")
    (newline))))
```

### 6.2 Send and receive

Contexts communicate by message passing.  `send` deep-copies the message into
the receiver's mailbox (shared-nothing).  Context handles and grant objects are
passed by identity (not copied).

| Signature | Description |
|-----------|-------------|
| `(send target msg)` | Deep-copy `msg` into `target`'s mailbox; wake it if blocked; returns `#<undef>` |
| `(recv)` | Block until a message arrives; return the oldest message |

`recv` is defined in Lisp as:
```scheme
(define (recv)
  (if (%mailbox-empty?)
      (begin (%block) (recv))
      (%mailbox-pop)))
```

The low-level mailbox primitives (`%mailbox-empty?`, `%mailbox-pop`, `%block`)
are available but rarely needed directly.

**Copy-on-send rules:**
- Fixnums, booleans, chars, `'()` — passed as values.
- Symbols, keywords — shared immutable; not copied.
- Strings, flonums, pairs, vectors, bytes, hash-tables — deep-copied.
- Context handles, grants — passed by identity (not copied).
- Procedures and environments — **cannot be sent** (error).

```scheme
; A simple echo server:
(spawn (lambda ()
  (let loop ()
    (let ((msg (recv)))
      (send (car msg) (cdr msg))   ; reply to sender
      (loop)))))
```

### 6.3 Ports and the server pattern

The `driver-util` module exports a `serve` helper:

```scheme
(serve init step)
```

Spawns a zero-capability restricted context running a message loop.  Each
incoming message `m` transforms `state` via `(step state m)`.  Returns the
context handle.

Use `reply-to` to safely send to a handle that arrived in a message:

```scheme
(define (reply-to target msg)
  (if (ctx? target) (begin (send target msg) #t) #f))
```

This guard prevents a bad client-supplied address from killing the service
context (the VM has no `try/catch` — a `send` to a non-context aborts the
calling context).

### 6.4 Sleep

```scheme
(sleep nanoseconds)
```

Deschedules the current context for approximately `nanoseconds`; the timer tick
(~50 µs) is the resolution floor.  `sleep` shares the blocked flag with
`recv`: a `send` arriving while sleeping wakes the context early.  A
port-bound server that needs to distinguish sleep-expiry from an early message
should use an uptime deadline with `uptime-ns` instead of a bare `sleep`.

```scheme
(uptime-ns)   ; -> nanoseconds since boot (exact integer)
```

---

## 7. The sys-* capability modules

These modules are C-level built-ins registered by `SysLisp` at boot.  They are
gated: a context must name them in its `import` list AND the spawn grant must
include them.  Import without the grant is an error.

### `sys-io` — legacy x86 port I/O

| Export | Description |
|--------|-------------|
| `(in-u8 port)` | Read 1 byte from I/O port |
| `(in-u16 port)` | Read 2 bytes |
| `(in-u32 port)` | Read 4 bytes |
| `(out-u8 port val)` | Write 1 byte |
| `(out-u16 port val)` | Write 2 bytes |
| `(out-u32 port val)` | Write 4 bytes |

### `sys-mmio` — MMIO mapping and DMA allocation

| Export | Description |
|--------|-------------|
| `(mmio-map phys size)` | Map a physical region uncached (UC) — volatile MMIO registers |
| `(mmio-map-wc phys size)` | Map write-combining (WC) — framebuffer scanout fronts; fast streaming writes, slow reads |
| `(mmio-map-wb phys size)` | Map write-back cached — compositing into a WB DMA backing; requires `phys > 0` |
| `(dma-alloc size)` | Physically-contiguous, zeroed, UC DMA buffer; `(bytes-phys b)` gives the physical address |
| `(dma-alloc-wb size)` | Same but WB-cached — for CPU-write, device-read buffers (e.g. framebuffer backing); do not use when the device writes it |
| `(dma-alloc-32 size)` | DMA buffer with a physical address below 4 GiB — for 32-bit-only devices (RTL8139, USB host controllers) |

### `sys-pci` — PCI device discovery and MSI

| Export | Description |
|--------|-------------|
| `(pci-find vid did)` | Physical ECAM address of first matching PCI device, or `#f` |
| `(pci-find-class class sub)` | First PCI device with given class/subclass |
| `(pci-find-all vid did)` | List of ECAM addresses of every matching device |
| `(pci-find-class-all class sub)` | List of ECAM addresses of every matching class |
| `(pci-setup-msi ecam-phys)` | Configure MSI/MSI-X on the device; return an opaque MSI handle (fixnum), or `#f` |
| `(msi-count handle)` | MSI interrupt counter (advances on each interrupt) |
| `(msi-wait handle seen [timeout-ns])` | Park until MSI counter passes `seen`; returns `#f` immediately if already passed or on timeout |
| `(pci-assign-bars ecam-phys)` | Assign BARs and open bridge windows for firmware-unconfigured devices; returns first BAR base or `#f` |

### `sys-irq` — ISA/IOAPIC interrupt lines

| Export | Description |
|--------|-------------|
| `(irq-register gsi)` | Claim ISA IRQ line `gsi`; return opaque handle (fixnum) or `#f` |
| `(irq-count handle)` | IRQ counter for this line |
| `(irq-wait handle seen [timeout-ns])` | Park until IRQ counter passes `seen`; `#f` if already passed or on timeout |

### `sys-cmdline` — kernel command line

| Export | Description |
|--------|-------------|
| `(cmdline-has? "substr")` | `#t` if the substring occurs in the kernel command line |
| `(cmdline-get "key=")` | String value of the first `key=VALUE` token, or `#f` |

### `sys-reg` — hardware registry

| Export | Description |
|--------|-------------|
| `(reg-read-uint "path" "key")` | Read an unsigned integer from the SysReg key/value store; `#f` if absent |

The registry is populated by the boot enumerators (PCI, ACPI, multiboot).
Paths like `"HW/PCI/0"`, `"HW/BOOTINFO/FRAMEBUFFER"` are examples.

### `sys-initrd` — initrd file access

| Export | Description |
|--------|-------------|
| `(initrd-file "name")` | Bytes of a file from the boot initrd tar, copied into a fresh owned buffer; `#f` if not found |

### `sys-ttf` — TrueType glyph rasterization

| Export | Description |
|--------|-------------|
| `(ttf-rasterize font-bytes cp px)` | Rasterize codepoint `cp` at pixel size `px`; returns `(coverage w h xoff yoff advance)` where `coverage` is an 8-bit alpha bitmap or `#f` for empty glyphs |
| `(ttf-vmetrics font-bytes px)` | Returns `(ascent descent linegap)` in pixels (descent is negative) |

These are the raw kernel-side rasterizer calls; drivers use the higher-level
`ttf` library module (Section 8.3) which memoizes results.

### `sys-shm` — shared-memory grant (grantee side)

| Export | Description |
|--------|-------------|
| `(map-grant g)` | Map the granted region as a WB-cached `bytes`; returns `#f` if the grant was revoked |

### `sys-shm-mint` — shared-memory grant (owner/compositor side)

| Export | Description |
|--------|-------------|
| `(grant-mint buffer ['ro \| 'rw])` | Mint an unforgeable grant over `buffer`'s physical region; default is `'ro` (least privilege) |
| `(grant-revoke g)` | Invalidate the grant; future `map-grant` returns `#f`; idempotent |

The split `sys-shm` / `sys-shm-mint` enforces that only the compositor (the
owner) can create and revoke grants; a surface client gets `sys-shm` (read or
write the mapped region only) and never gets `sys-shm-mint` (no ability to
grant arbitrary physical memory to another context).

### `sys-debug` — reflective debugger capability

**Gated** — this module is POWERFUL.  Only a context granted `sys-debug` in
its spawn grant may import it.

| Export | Description |
|--------|-------------|
| `(ctx-make thunk)` | Create a PAUSED context applying `(thunk)`; not enqueued; drive with `ctx-step` |
| `(ctx-step c [n])` | Advance context `c` up to `n` reductions (default 1); returns status symbol: `eval`, `apply`, `done`, `error`, or `suspended` |
| `(ctx-status c)` | Stored status register: `eval`, `apply`, `done`, or `error` |
| `(ctx-control c)` | The expression `c` is about to evaluate (useful in `eval` state) |
| `(ctx-value c)` | Result accumulator (meaningful when `done`) |
| `(ctx-error c)` | Error message string, or `#f` |
| `(ctx-list)` | List of all live context handles on this core's scheduler queue |
| `(ctx-blocked? c)` | `#t` if `c` is parked waiting for a message |
| `(ctx-pause c)` | Mark a live scheduler-owned context blocked (attach for cooperative pause) |
| `(ctx-unpause c)` | Clear the blocked flag (return to scheduler) |

### `sys-console` — interactive serial REPL

Only available when `cardinal.repl` is on the kernel command line.

| Export | Description |
|--------|-------------|
| `(console-poll)` | Non-blocking: bytes waiting on the REPL channel, or `#f` |
| `(console-write str)` | Emit bytes on the REPL channel |
| `(repl-eval str)` | Evaluate `str` in the persistent REPL environment; return transcript |
| `(console-arm-rx)` | Enable COM1 receive interrupt |
| `(console-flush)` | Flush the coalesced debug log buffer |

The `sys-console` module is registered only when the kernel boots with
`cardinal.repl`; see [Debug the OS](../guides/debugging.md) for the REPL access
path (the link is framed CSMUX, driven by `scripts/csmux-repl.py`).

---

## 8. 2D Graphics and font library

These are Lisp-level modules in `lisp/lib/`.  They require no system
capabilities (only `bytes` and the ambient `gfx-*!` C primitives) and are
fully host-testable.

### 8.1 Low-level 2D primitives (C, ambient)

These are available without import — they are registered in the default
environment by `lisp_install_primitives`.

| Signature | Description |
|-----------|-------------|
| `(gfx-fill-rect! dst stride dw dh x y w h color)` | Fill a clipped rectangle in a 32-bit-per-pixel `bytes` |
| `(gfx-blit! dst dstride dw dh dx dy src sstride sw sh)` | Opaque blit of a `src` image to `(dx,dy)` in `dst`, clipped |
| `(gfx-blend! dst dstride dw dh dx dy src sstride sw sh)` | Alpha-composite an ARGB `src` (alpha in top byte) over `dst`, clipped |
| `(gfx-glyph! dst stride dw dh dx dy bitmap boff gw gh fg bg draw-bg scale)` | Blit a 1-bpp glyph (MSB-left, `ceil(gw/8)` bytes per row) scaled `scale×scale` |
| `(gfx-cover! dst stride dw dh dx dy cover cstride cw ch fg)` | Composite solid color `fg` through an 8-bit coverage mask (antialiased text path) |

All `gfx-*!` primitives clip to `(0,0,dw,dh)` (off-screen draws are safe),
require 4-aligned data and stride for 32-bit stores, and are NON-volatile (not
for MMIO registers).

### 8.2 `graphics` module — surface-based 2D drawing

```scheme
(import graphics)
```

#### Surface construction

```scheme
; Software surface over a plain 0x00RRGGBB framebuffer:
(make-surface fb width height stride)

; Full control over channel offsets and HW backend:
(make-surface* fb width height stride r-off g-off b-off backend)
```

Accessors: `surface-fb`, `surface-width`, `surface-height`,
`surface-stride`, `surface-r-off`, `surface-g-off`, `surface-b-off`,
`surface-backend`.

The `backend` is an alist of `(op-symbol . proc)` hardware acceleration
overrides.  Every drawing operation checks the backend first; the software
fallback (a `gfx-*!` call) runs if the op is absent.

#### Colour packing

```scheme
(rgb  surf r g b)       ; => packed pixel in surface's channel layout
(argb surf a r g b)     ; => packed pixel with 8-bit alpha in top byte
```

#### Drawing operations

| Signature | Description |
|-----------|-------------|
| `(clear surf color)` | Fill the entire surface |
| `(fill-rect surf x y w h color)` | Filled rectangle (hardware-acceleratable) |
| `(draw-rect surf x y w h t color)` | Rectangle outline of thickness `t` |
| `(draw-hline surf x y w color)` | Horizontal line |
| `(draw-vline surf x y h color)` | Vertical line |
| `(draw-line surf x0 y0 x1 y1 color)` | Bresenham line |
| `(draw-circle surf cx cy r color)` | Midpoint circle outline |
| `(fill-circle surf cx cy r color)` | Filled circle |
| `(put-pixel surf x y color)` | Write one pixel (bounds-checked) |
| `(get-pixel surf x y)` | Read one pixel; `0` if out of bounds |
| `(blit dst src dx dy)` | Opaque blit of `src` surface to `(dx,dy)` in `dst` |
| `(blit-alpha dst src dx dy)` | Alpha-composite `src` (ARGB) over `dst` |
| `(draw-glyph surf x y bitmap boff gw gh fg bg draw-bg? scale)` | Bitmap glyph blit |

#### Double buffering

```scheme
(make-double-buffer front-surface)   ; => double-buffer pair (back . front)
(db-back db)                         ; the cached (WB) compositing surface
(db-front db)                        ; the scanout (WC) surface
(db-flush db)                        ; bulk-copy entire back -> front + sfence
(db-flush-rect db x y w h)          ; copy only the dirty region
```

#### Damage tracking

```scheme
(make-damage)              ; mutable damage list
(damage-add! d x y w h)   ; record a dirtied rectangle
(damage-rects d)           ; list of (x y w h) rects
(damage-clear! d)          ; reset for next frame
(damage-empty? d)          ; #t if no damage recorded
(db-flush-damage db d)     ; flush only damaged rects, then clear the list
```

### 8.3 `font` module — bitmap font text rendering

```scheme
(import font)
```

Renders fixed-cell bitmap fonts (1-bpp, MSB-left row encoding) via
`draw-glyph`.  The included 8×16 VGA-style font covers ASCII 32–126.

```scheme
(define FONT8X16-PATH "./lisp/data/font8x16.bin")  ; initrd path
(define FONT8X16-W 8)
(define FONT8X16-H 16)

; Construction (the caller supplies bytes from sys-initrd):
(make-font bitmap glyph-w glyph-h)

; Accessors:
(font-glyph-w f)    (font-glyph-h f)    (font-cellbytes f)
```

| Signature | Description |
|-----------|-------------|
| `(draw-char surf font x y ch fg bg draw-bg? scale)` | Draw one character |
| `(draw-text surf font x y str fg bg draw-bg? scale)` | Draw a string; newlines wrap; returns next y |
| `(text-width font str scale)` | Pixel width of the widest line |

### 8.4 `ttf` module — TrueType antialiased text

```scheme
(import ttf)   ; requires sys-ttf and graphics in the grant
```

Wraps the `ttf-rasterize` kernel primitive with a per-font glyph cache and
vertical-metrics cache (both `equal?`-keyed hash tables).  Uses `gfx-cover!`
to composite glyphs as antialiased coverage bitmaps.

```scheme
(define TTF-FONT-PATH "./lisp/data/DejaVuSans-subset.ttf")

; Construction (caller supplies font bytes from sys-initrd):
(make-ttf-font bytes)
```

| Signature | Description |
|-----------|-------------|
| `(ttf-draw-text surf font x y str color px)` | Draw string antialiased at pixel size `px`; newlines wrap; returns baseline y of last line |
| `(ttf-text-width font str px)` | Pixel width of widest line (sum of cached glyph advances) |
| `(ttf-line-height font px)` | Line pitch in pixels (`ascent - descent + linegap`) |
| `(ttf-ascent font px)` | Distance from top-of-line to baseline |

Pen model: `(x, y)` is the top-left of the text block; the baseline sits
`ascent` pixels below `y`.  Each glyph's coverage box is placed at
`(x + xoff, baseline + yoff)` (yoff is negative — the box top is above the
baseline).

---

## 9. `driver-util` module — shared driver utilities

```scheme
(import driver-util)
```

A general-purpose library imported by most drivers and servers.

### List helpers

| Signature | Description |
|-----------|-------------|
| `(nth lst k)` | Zero-based list index (like `list-ref`) |

### Mutable cell

```scheme
(make-cell v)    ; make a mutable 1-element box holding v
(cell-ref c)     ; read current value
(cell-set! c v)  ; write new value
```

### Big-endian byte access

Network protocol headers are big-endian; these operate on a `bytes` buffer:

| Signature | Description |
|-----------|-------------|
| `(put-be16! b off v)` | Write 16-bit big-endian |
| `(get-be16 b off)` | Read 16-bit big-endian |
| `(put-be32! b off v)` | Write 32-bit big-endian |
| `(get-be32 b off)` | Read 32-bit big-endian |

### Buffer utilities

| Signature | Description |
|-----------|-------------|
| `(copy-bytes src off len)` | Copy `len` bytes from `src[off..)` into a fresh owned buffer |
| `(bytes-copy-into! dst off src len)` | Copy `len` bytes from `src[0..)` into `dst[off..)` |
| `(put-list! b off lst)` | Write a list of byte values into `b` starting at `off` |

### PCI helpers

| Signature | Description |
|-----------|-------------|
| `PCI-COMMAND` | Constant `#x04` (PCI command register offset) |
| `(bar-base cfg bar-idx)` | Decode a BAR's physical base address (handles 64-bit BARs) |
| `(pci-enable-mem-bus-master! cfg)` | Set COMMAND bits 1 (mem-space) and 2 (bus-master) |

### Polling

```scheme
; Poll pred with a spin budget before falling back to sleep-based polling:
(wait-until-spin pred timeout-ns spin-ns)

; Poll without a spin budget (for ms-scale waits):
(wait-until pred timeout-ns)
```

Returns `#t` when `pred` became true, `#f` on timeout.

### Server pattern

```scheme
(serve init step)
```

Spawns a zero-capability restricted context running:

```scheme
(let loop ((state init))
  (loop (step state (recv))))
```

Returns the context handle.

```scheme
(reply-to target msg)
```

Safely sends `msg` to `target` only if `(ctx? target)`; returns `#t` on
success.  Use this instead of bare `send` for reply addresses that arrived in
a client message.

---

## 10. Notes and gotchas

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
