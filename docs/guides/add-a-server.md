# Add a Core* server and design a message protocol

*A step-by-step guide to building a new Cardinal; OS service: choosing the right spawn primitive, designing a tagged-message protocol, replying safely, and wiring everything into `init.clp`.*

This guide assumes you have read the [message passing & concurrency](../concepts/message-passing.md) concept article. All code patterns are drawn from the real servers in `lisp/servers/`.

---

## 1. What a server is

A Cardinal; `Core*` service is a long-lived Lisp context parked on `recv`. It owns some private state — a list of registered devices, a map of active sessions, whatever your service needs — and processes one message at a time. It never exports a C ABI or a function pointer: every interaction is a `send`.

This design eliminates the re-entrant self-deadlock that the original C servers had to work around. Because `send` only enqueues a message, a device that reacts to an event (and might talk back to the same service) cannot deadlock it — the service is not holding any lock while the device runs.

The canonical shape, taken directly from `lisp/lib/driver-util.clp` (the `serve` helper), is:

```scheme
(spawn-restricted '()           ; no import authority beyond what was already captured
  (lambda ()
    (let loop ((state initial-state))
      (loop (step state (recv))))))
```

`serve` from `driver-util` captures exactly this shape, so you rarely write the loop explicitly. `serve` returns the context handle: the value callers pass to `send`. Store it.

The capability grant is `'()` — an empty list. The service context cannot call `(import sys-pci)` or acquire any new authority at runtime. It only has what its defining module's `import` declarations captured *lexically* at load time. A wedged or compromised service loop therefore cannot reach hardware it was never given.

---

## 2. Starting a server

### 2a. Using the `serve` helper

For a pure routing service that needs no hardware authority, use `serve` from `driver-util`:

```scheme
(define-module myservice
  (export start-myservice)
  (import driver-util)

  (define (start-myservice)
    (serve '()                           ; initial state: empty list of registrations
      (lambda (state m)
        (cond
          ((eq? (car m) 'register) ...)  ; return updated state
          ((eq? (car m) 'query)   ...)   ; return state unchanged
          (else state))))))              ; unknown tag: ignore, keep state
```

`serve` calls `spawn-restricted '()` internally and runs your step function in a tail-recursive loop. Your step function receives the current state and the message; it returns the next state. The loop never exits.

This is exactly the pattern used by `lisp/servers/coreinput.clp` and `lisp/servers/corepower.clp` — the two simplest servers in the tree. `coreinput` threads a list of registered device names; `corepower` threads a list of `(name class-bits ctx)` triples.

### 2b. Using `spawn-restricted` directly

If your service needs to capture a hardware capability that it imported at load time, or if the state setup is complex, call `spawn-restricted` directly:

```scheme
(define (start-myservice hw-handle)
  (spawn-restricted '()
    (lambda ()
      (let ((table (make-hash-table))    ; private mutable state
            (hw    hw-handle))           ; captured from the enclosing scope
        (let loop ()
          (let ((m (recv)))
            (dispatch table hw m)
            (loop)))))))
```

### 2c. Keeping the handle

`start-*-service` returns the handle. The handle is what callers pass to `send`. Bind it and keep it:

```scheme
; in system-init (lisp/init.clp):
(let ((audio (start-audio-service)))
  (set! audio-service audio)   ; publish for the REPL
  (hdaudio-init audio ecam)    ; pass to drivers that register with it
  ...)
```

If you drop the handle — as `corepower` does in the current `init.clp` — the service runs but no one can reach it. Always capture and distribute the handle.

---

## 3. The dispatch loop

Messages are ordinary Lisp lists. The convention throughout the tree is: the first element is a symbol identifying the operation (the *tag*), and the remaining elements are arguments.

```scheme
; send side (any context):
(send myservice (list 'register 'mydevice device-ctx))
(send myservice (list 'query 'mydevice (self)))

; receive side (inside the loop):
(cond
  ((eq? (car m) 'register) ...)
  ((eq? (car m) 'query)    ...)
  (else state))             ; always have a fall-through — unknown tags must be ignored
```

The fall-through `else` branch is not optional. An untrusted client can send any message; an unguarded `cond` that falls off the end returns `#f`, which becomes the next state — almost certainly wrong. An explicit `else state` or `else 'ignore` keeps the service alive.

---

## 4. Request/reply correctly and safely

### 4a. The shape of a request/reply

For operations where the caller needs an answer, the client passes its own context handle as the reply address. The canonical idiom:

```scheme
; client:
(send storage (list 'read 'vol0 0 1 (self)))
(let ((reply (recv)))    ; blocks until the reply arrives
  (display (cadr reply)))

; server:
((eq? (car m) 'read)
 (let ((reply-ctx (nth m 4)))
   (reply-to reply-ctx (list 'complete 0 data))
   state))
```

`(self)` returns the running context's handle. Handles pass through `send` by identity — they are not deep-copied like data. That is why passing `(self)` inside a message delivers a live, sendable address to the server.

### 4b. Validate every reply handle with `ctx?`

**This is the critical safety rule.** Never `send` directly to a value that arrived in a message. The VM has no `try/catch`; a `send` to a non-context value aborts the calling context. For a long-lived server loop, that means a single bad client message permanently kills the service.

Always validate first:

```scheme
; WRONG — a forged handle from any client kills the service:
(send (nth m 4) (list 'complete result))

; CORRECT — use reply-to from driver-util:
(reply-to (nth m 4) (list 'complete result))
```

`reply-to` (from `lisp/lib/driver-util.clp`) is defined as:

```scheme
(define (reply-to target msg)
  (if (ctx? target) (begin (send target msg) #t) #f))
```

`ctx?` is a VM primitive that returns `#t` only for genuine context handles — not pairs, not integers, not fabricated data. If the target fails the check, `reply-to` silently drops the reply and the service loop continues unharmed.

`coreaudio` guards every forwarded reply handle in exactly this way:

```scheme
; from lisp/servers/coreaudio.clp:
((eq? (car m) 'cards)
 (reply-to (nth m 1) (map car cards)) cards)
((eq? (car m) 'get-volume)
 (let ((rec (card-find (nth m 1) cards)))
   (if rec
       (if (ctx? (nth m 3)) (send (card-ctx rec) (list 'get-volume (nth m 2) (nth m 3))))
       (reply-to (nth m 3) #f))) cards)
```

Note the two patterns: `reply-to` when the service replies directly; a bare `(ctx? ...)` guard with a manual `send` when the service *forwards* the reply address into another message. In the forwarding case it is the *eventual sender* (the card driver context) that must be protected, so the guard lives here at the point of forwarding.

`corestorage` follows the same rule, guarding the `reply` field of `read` and `write` before forwarding it to the block driver:

```scheme
; from lisp/servers/corestorage.clp:
((eq? (car m) 'read)
 (if (ctx? (nth m 4)) (do-read devs (cadr m) (caddr m) (cadddr m) (nth m 4)))
 state)
```

---

## 5. Registration protocols and fan-out

The standard way a driver announces itself is to `send` a `register` message to the service at bring-up time:

```scheme
; driver side:
(send audio-svc (list 'register 'hda0 (self) endpoints))

; service side:
((eq? (car m) 'register)
 (cons (cons (nth m 1) (nth m 2)) cards))  ; store (name . ctx)
```

The service stores a `(name . ctx)` (or richer) record. Later requests that name a specific device are forwarded to that driver's context:

```scheme
(define (to-card name cards msg)
  (let ((rec (card-find name cards)))
    (if rec (send (card-ctx rec) msg))))
```

For fan-out (deliver to all registered subscribers), iterate the list:

```scheme
; from corepower.clp:
(define (pwr-fanout devs class msg)
  (for-each (lambda (d)
              (if (not (= 0 (bitwise-and (cadr d) class)))
                  (send (caddr d) msg)))
            devs))
```

Each matching context receives the event message independently; the service does not wait for any of them to process it.

---

## 6. Idioms and gotchas

### Fire-and-forget vs. reply

`send` is asynchronous. If your protocol does not need acknowledgement, just `send` and move on — `corepower` uses this for power-state fan-out. If the caller needs a result, it includes `(self)` in the request and blocks on `recv`.

Do not `recv` inside a `serve` step unless you are prepared for the stash pattern below. Blocking in the step function stalls the entire service.

### Single-context stash serialization

Sometimes a service handler must do its own message-IO to answer a request — for example, a filesystem provider doing block reads inside a `get` handler. Because the context is single-threaded, any other message that arrives mid-IO would be processed out of order if you called `recv` directly.

`cardfs` (`lisp/servers/cardfs.clp`) solves this with a `stash` list: messages that arrive while a block IO is in flight are appended to `stash` and re-processed at the top of the next loop iteration, before calling `recv` for a fresh message:

```scheme
; from cardfs.clp (condensed):
(let ((stash '()))
  (define (io-recv)
    (let loop ()
      (let ((m (recv)))
        (if (eq? (car m) 'complete)
            m
            (begin (set! stash (append stash (list m))) (loop))))))

  (let loop ()
    (let ((m (if (null? stash)
                 (recv)
                 (let ((h (car stash))) (set! stash (cdr stash)) h))))
      ...
      (loop))))
```

`io-recv` waits only for the `'complete` message, deferring everything else. The main loop drains `stash` before blocking on `recv` again, so deferred requests are serialized in arrival order. Use this pattern any time your handler must do its own `recv`.

!!! warning "Do not hold the loop while a slow operation runs"
    If your handler takes a long time (a reset wait, a DMA flush), yield explicitly via `(sleep 200000)` in a poll loop (`wait-until` from `driver-util` does this for you), or offload the work to a helper context and reply asynchronously. A handler that busy-spins or calls `recv` unexpectedly will stall every other client of the service.

### The `sleep`-wakes-on-`send` hazard

`sleep` and `recv` share the same blocked flag inside the VM. A `send` to a sleeping context wakes it immediately — even if the sleep timer has not expired. This means a service that parks on a long `sleep` to implement a periodic poll cannot trust the sleep duration: any inbound message will cut it short.

The safe pattern for a port-bound context that also needs periodic work is to track the deadline explicitly with `(uptime-ns)` and drain the mailbox on each wake:

```scheme
(let loop ((deadline (+ (uptime-ns) interval-ns)))
  (let ((now (uptime-ns)))
    (if (>= now deadline)
        (begin (do-periodic-work) (loop (+ now interval-ns)))
        (begin
          (if (not (%mailbox-empty?)) (process-pending-messages))
          (sleep (- deadline now))
          (loop deadline)))))
```

See the [message passing & concurrency](../concepts/message-passing.md) article for the full explanation.

### Never call `recv` inside `(self)` registration

Passing `(self)` as a reply address and then calling `recv` in the same handler is safe only if nothing else will send to your context first. In practice a service loop receives from many senders; an unexpected message arriving before the expected reply will be processed as if it were the reply. Prefer the stash pattern above or a dedicated helper context for operations that need a synchronous round-trip.

---

## 7. Wiring into `init.clp`

`lisp/init.clp` is the sole boot policy file. Add your service to `system-init`:

```scheme
; 1. Import your module at the top of the define-module block:
(import myservice ...)

; 2. Inside system-init, start the service and keep the handle:
(let ((mysvc (start-myservice)))
  ; 3. Pass the handle to drivers that register with it:
  (mydriver-init mysvc ecam)
  ; 4. Optionally expose for the serial REPL or other services:
  mysvc)
```

If your service depends on hardware discovered via `pci-find`, gate the driver init on it:

```scheme
(let ((ecam (pci-find #xVVVV #xDDDD)))
  (if ecam (mydriver-init mysvc ecam)
      (begin (display "[init] no mydevice") (newline))))
```

You do not need to change any CMakeLists or boot script. All `.clp` files under `lisp/` are packaged into the initrd automatically; `(import myservice)` at the top of `init.clp` is sufficient to make it available.

### Giving clients a way to find the service

There is no global service registry right now. The convention is to pass the handle at construction time: `init.clp` passes `audio-service` to `hdaudio-init`, the audio driver keeps it and calls `(send audio-service ...)` whenever a card comes up or a request needs forwarding. Publish the handle by passing it as an argument to any driver or subsystem that needs it, or hold it in a module-level variable (as `init.clp` does with `audio-service`) and expose an accessor.

---

## Worked example: a minimal event-bus server

The following is a condensed version of the `coreinput` pattern — the simplest server in the tree:

```scheme
(define-module mybus
  (export start-mybus)
  (import driver-util)

  (define (start-mybus)
    (serve '()                            ; state: list of subscriber contexts
      (lambda (subs m)
        (cond
          ((eq? (car m) 'subscribe)       ; (subscribe ctx) — add a subscriber
           (cons (cadr m) subs))
          ((eq? (car m) 'event)           ; (event payload) — fan out
           (for-each (lambda (s) (send s (list 'mybus-event (cadr m)))) subs)
           subs)
          (else subs))))))
```

In `init.clp`:

```scheme
(import mybus ...)

(define (system-init)
  (let ((bus (start-mybus)))
    (mydriver-init bus)   ; driver calls (send bus (list 'subscribe (self)))
    ...))
```

Subscribers call `(recv)` in their own loops and handle `'mybus-event` messages. The bus itself never blocks.

---

## Next steps

- [Message passing & concurrency](../concepts/message-passing.md) — the mental model for contexts, the scheduler, and the gotchas this guide references
- [coreinput](../servers/coreinput.md) — the minimal worked example (register + event fan-out)
- [corepower](../servers/corepower.md) — register with class-bit fan-out
- [coreaudio](../servers/coreaudio.md) — request/reply with `ctx?`/`reply-to` hardening
- [corestorage](../servers/corestorage.md) — forwarding reply handles safely; the `probe`/`claim` pattern
- [cardfs](../servers/cardfs.md) — single-context stash serialization for handlers that do message-IO
- [Lisp VM API reference](../vm/api.md) — `spawn`, `spawn-restricted`, `send`, `recv`, `self`, `ctx?`, `capabilities`
