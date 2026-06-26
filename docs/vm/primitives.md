# Core primitives

*Part of the [Lisp VM Reference](index.md).*

All primitives below are available in the default environment without any
`import`.

## Arithmetic

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

## Numeric comparison

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

## Numeric predicates and coercions

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

## Numeric extras (prelude)

| Signature | Description |
|-----------|-------------|
| `(add1 n)` | `(+ n 1)` |
| `(sub1 n)` | `(- n 1)` |
| `(max x xs ...)` | Maximum |
| `(min x xs ...)` | Minimum |
| `(expt base e)` | Integer exponentiation (exact, iterative) |
| `(gcd n ...)` | Greatest common divisor |

## Bitwise (driver substrate)

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

## Booleans

| Signature | Description |
|-----------|-------------|
| `(not x)` | Logical negation; only `#f` is false |
| `(boolean? x)` | `#t` for `#t` or `#f` |
| `(boolean=? a b)` | `#t` if both are the same boolean (prelude; same as `eq?`) |

## Equality

| Signature | Description |
|-----------|-------------|
| `(eq? a b)` | Identity: pointer equality for heap objects; value equality for fixnums, booleans, chars, symbols (all unboxed or interned) |
| `(equal? a b)` | Deep structural equality over pairs, vectors, strings, bytes, flonums; `eq?` for everything else |

`eqv?` is not a distinct primitive; it is aliased appropriately in the prelude.

## Pairs and lists

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

## Symbols

| Signature | Description |
|-----------|-------------|
| `(symbol? x)` | `#t` for symbols |
| `(symbol->string sym)` | Symbol name as a fresh string |
| `(string->symbol s)` | Intern a string as a symbol |

## Strings

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

## Characters

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

## Bytes

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

## Vectors

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

## Hash tables

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

## Higher-order

| Signature | Description |
|-----------|-------------|
| `(apply proc arg ... lst)` | Apply `proc` to `arg ...` followed by elements of `lst` |
| `(map proc lst)` | Apply `proc` to each element; return list of results |
| `(for-each proc lst)` | Apply `proc` to each element for side effects; return `#<undef>` |

`map` and `for-each` accept exactly one list argument (multi-list map is not
implemented).

## Identity and miscellaneous

| Signature | Description |
|-----------|-------------|
| `(procedure? x)` | `#t` for any callable |
| `(identity x)` | Returns `x` (prelude) |

## Output and errors

| Signature | Description |
|-----------|-------------|
| `(display x)` | Print `x` without quotes/escapes (human-readable) to the debug sink (COM1) |
| `(write x)` | Print `x` in reader-faithful form (strings quoted, chars as `#\a`) |
| `(newline)` | Print a newline |
| `(error msg irritants ...)` | Abort the current evaluation with `msg` (a string); irritants are currently unused |

Output goes to the COM1 debug log through `lisp_set_output`; in OS context
that is `print_str` routing to SysDebug.
