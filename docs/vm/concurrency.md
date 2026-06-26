# Concurrency and messaging

*Part of the [Lisp VM Reference](index.md).*

## Contexts (green threads)

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

## Send and receive

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

## Ports and the server pattern

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

## Sleep

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
