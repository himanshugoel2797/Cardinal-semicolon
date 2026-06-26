# Message passing & concurrency

*How Lisp contexts run, communicate, and the failure modes that surprise everyone once.*

## The model

The OS above the `Sys*` kernel modules is a collection of independent **Lisp
contexts** — the runtime's term for a green thread with its own heap, its own
execution state, and its own capability grant. No two contexts share mutable
memory. If you want to coordinate two contexts, you send a message.

This is not a metaphor. The isolation is structural: each context owns a
private heap (`gc.c`), and the GC's marking phase stops at the boundary. An
object in context A's heap is never reachable from context B's collector, and
vice versa. The rule is enforced by `mark_push`, which gates on a heap-local
address set and silently stops rather than tracing into a foreign heap. Nothing
in the language lets you reach across.

What crosses the boundary is a **message** — a deep copy of a Lisp value,
deposited into the receiver's heap. From that point, the receiver owns the copy
and can do whatever it wants with it; the sender's copy is unchanged.

Contexts are scheduled cooperatively *and* preemptively. Each gets a reduction
budget (a slice of evaluation steps); when the budget runs out the scheduler
suspends the context at the next safe point and resumes the next runnable one.
An infinite loop therefore cannot wedge the system — it is preempted. Calling
`yield` surrenders the remaining budget immediately. The scheduler is per-core:
each CPU core runs its own scheduler loop over its own run queue
(`g_sched[lisp_rt_core()]` in `libs/lisp/src/sched.c`).

The Lisp context IS userspace in Cardinal;. It is a capability-gated execution
environment, not a hardware ring-3 process. The hardware privilege boundary is
between kernel modules (`Sys*`) and everything else; the context model adds a
software capability layer on top of that. See
[Capabilities & sandbox](capabilities-and-sandbox.md) for how `import` gates
access to system primitives.

---

## Primitives

### Spawning contexts

```scheme
(spawn thunk)              ; → context handle
(spawn-restricted caps thunk)  ; → context handle, with narrowed authority
```

`spawn` creates a new context that calls `thunk` with no arguments, enqueues
it on the current core's scheduler, and returns a handle. The handle is how
you address the context for `send`. A spawned context **inherits** the
spawner's capability grant: if the spawner is restricted to `(net-module
storage-module)`, the child gets the same list. You cannot escalate through
spawn.

`spawn-restricted` narrows authority: the new context's `caps` list is exactly
what you pass in, which must be a subset of the spawner's own grant. A
restricted spawner cannot grant a capability it doesn't hold — the runtime
enforces this at spawn time.

The `serve` helper in `lisp/lib/driver-util.clp` formalises the standard
server shape: it calls `spawn-restricted '()` (no capabilities at all), so the
spawned service loop can only do what the closures it was handed can already do.
A wedged or compromised service therefore cannot acquire new authority.

```scheme
;; from driver-util.clp
(define (serve init step)
  (spawn-restricted '()
    (lambda ()
      (let loop ((state init))
        (loop (step state (recv)))))))
```

### Sending a message

```scheme
(send target message)   ; → unspecified
```

`send` deep-copies `message` into `target`'s heap and wakes the receiver if it
was parked. It never blocks the sender.

The copy is exhaustive: strings, byte buffers, pairs, vectors, and hash tables
are all rebuilt in the receiver's heap. This means **you cannot share a mutable
structure between two contexts** — what you send is a snapshot at the time of
the call, not a shared reference.

There are two value types that are **not** deep-copied and are instead passed
by identity:

- **Context handles** (what `spawn` and `self` return). These are actor
  identities — like Erlang PIDs. Copying one would produce a dead duplicate
  that no scheduler ever runs; the runtime passes the same system-heap pointer
  straight through. This is what lets a client embed its own handle in a request
  so the server can reply.

- **Grant objects** (shared-memory capability tokens from `libs/lisp/src/grant.c`).
  A grant IS the authority; duplicating its bits without duplicating the table
  entry would be meaningless and would break revocation.

Procedures, closures, and environments **cannot** be sent. A `send` with a
procedure payload is an error that aborts the sending context.

The copy also has a nesting-depth limit (128 levels) to protect the kernel
stack. Pathologically nested lists fail at send time, not at receipt.

Cross-core delivery — sending to a context whose scheduler runs on a different
core — requires locking the receiver's heap for the duration of the copy, since
the receiver's allocator/collector must not see a half-built message or a torn
mailbox spine. `prim_send` in `sched.c` handles this via
`lisp_gc_xfer_lock`/`lisp_gc_xfer_unlock`. Per-context heaps are otherwise
lock-free from the perspective of their owning core.

### Receiving a message

```scheme
(recv)   ; → message (blocks until one arrives)
```

`recv` is a blocking receive. It is defined entirely in Lisp on top of three
lower-level primitives:

```scheme
;; from sched.c lisp_install_sched
(define (recv)
  (if (%mailbox-empty?)
      (begin (%block) (recv))
      (%mailbox-pop)))
```

When the mailbox is empty, `%block` sets the context's `blocked` flag and
zeroes its budget so the scheduler skips it. When a `send` deposits a message,
it atomically clears `blocked` under the receiver's heap lock, closing the
check-then-park race that would otherwise lose a message arriving in that
window. On the next scheduler pass the context is runnable again and
`recv` returns the queued message.

The mailbox is a FIFO. Messages arrive in send order (per sender) but there is
no ordering guarantee across multiple senders.

### The running context's identity

```scheme
(self)   ; → context handle, or #f outside the scheduler
```

`self` returns the current context's own handle. Pass it inside a request
message so the receiver knows where to reply. The handle is a system-heap
object and survives `send`'s copy intact (identity pass-through, as above).

### Type-checking a handle

```scheme
(ctx? v)   ; → #t iff v is a live context handle
```

`ctx?` tests whether a value is a context object. Use it before sending to a
handle you received inside a message from an untrusted caller. The VM has no
try/catch; a `send` to a non-context value aborts the calling context — and for
a `serve` loop that means the service dies permanently, as there is no recovery
path.

### Yielding and sleeping

```scheme
(yield)       ; give up the rest of this scheduler slice
(sleep ns)    ; deschedule for approximately ns nanoseconds
```

`yield` zeroes the budget so the context suspends at the next safe point. It is
the polite thing to do in a busy-spin loop that has real work coming soon.

`sleep` parks the context for a duration. The resolution is the per-core timer
tick (~50 µs); sub-tick sleeps round up to one tick. Outside a scheduler
context (e.g. a driver running bring-up before `spawn`), `sleep` falls back to
a TSC busy-wait.

---

## Request/reply

The standard pattern for command-style interactions:

```scheme
;; Client side
(send service-handle (list 'do-work arg1 arg2 (self)))
(let ((reply (recv)))
  ;; use reply
  ...)

;; Server side (inside a `serve` loop)
(lambda (state m)
  (cond
    ((eq? (car m) 'do-work)
     (let ((reply-ctx (nth m 3)))
       (reply-to reply-ctx (list 'result (compute (cadr m) (caddr m))))
       state))
    ...))
```

`reply-to` (from `driver-util.clp`) is the canonical helper:

```scheme
(define (reply-to target msg)
  (if (ctx? target) (begin (send target msg) #t) #f))
```

The `ctx?` guard is not optional. Any message field that arrives from an
external sender could be anything — a fixnum, a string, `#f` — not the context
handle the protocol specifies. A bare `(send target msg)` in that position
would abort the service context if `target` is not a context. `reply-to` drops
the reply silently instead. For a reply handle forwarded into a third context's
message (rather than sent to immediately), guard the forward with `(ctx?
handle)` at the point of forwarding.

You can see this pattern in `lisp/servers/corestorage.clp`:

```scheme
((eq? (car m) 'read)
 (if (ctx? (nth m 4)) (do-read devs (cadr m) (caddr m) (cadddr m) (nth m 4)))
 state)
```

`corestorage` validates the reply address before passing it down to
`do-read`, which eventually sends the `(complete ...)` response.

---

## Gotchas

### `sleep` shares the blocked flag with `recv`

!!! warning "An incoming `send` wakes a sleeping context early"
    Both `%block` (called internally by `recv`) and `sleep` use the same
    `blocked` flag on the context object. When `send` delivers a message, it
    clears `blocked` to wake the receiver — it does not distinguish between
    "parked on `recv`" and "parked on `sleep`". A context that calls
    `(sleep 60000000000)` to wait one minute but also has messages arriving in
    its mailbox will wake up immediately on the first message, return from
    `sleep`, and proceed as if the sleep completed normally.

    **A port-bound context — one that receives messages — cannot trust a long
    `sleep` for timing.** The DHCP renewal loop hit this exactly: a bare
    `(sleep lease-duration)` kept being woken by unrelated network messages
    and triggered premature renewals.

The correct pattern is to poll to an absolute deadline using `uptime-ns`, with
short sleeps between checks:

```scheme
;; The dhcp-poll / dns-poll pattern from corenetwork
(define (wait-for-reply deadline)
  (let loop ()
    (cond ((%mailbox-empty?)              ; nothing yet
           (if (> (uptime-ns) deadline)
               #f                         ; timeout
               (begin (sleep 50000000)    ; 50ms nap, then re-check
                      (loop))))
          (else (%mailbox-pop)))))        ; got something
```

This works because each short `sleep` may be cut short by an incoming message,
but the outer loop immediately checks `%mailbox-empty?` and either pops the
message or continues waiting. The deadline comparison is the real timeout.
Drain the mailbox of unrelated messages rather than assuming the first message
is the one you want.

### Cross-core send serialises on the receiver's heap

On an SMP kernel each context's heap is normally lock-free from its owning
core's perspective. Cross-core delivery is the exception: `prim_send` holds
the receiver's heap spinlock (`lisp_gc_xfer_lock`) for the entire copy. This
blocks the receiver's core from collecting that heap while a message deposit is
in flight, but only momentarily — the lock is uncontended unless a send
happens to race an allocation. The runtime handles this automatically; the
design implication for you is that a large message payload sent cross-core is
not free.

Once a second core is live, the shared system heap is frozen: it grows only
(never collected), because the conservative system-heap collector cannot see
other cores' C stacks. Per-context heaps remain collectable precisely from
their CEK registers at scheduler safe points.

### SMP count amplifies or hides races

Concurrency bugs in the messaging layer often become visible only under SMP
(`SMP=2` or higher in `run-qemu.sh`). A race between `send` and `%block`
(the lost-wakeup window) is closed by taking the heap lock across both the
mailbox append and the `blocked` flag clear — but only a multi-core run will
actually exercise two cores concurrently hitting that window. Single-core runs
(`SMP=1`, the default in most quick smoke tests) serialize everything through
the scheduler loop and make such races invisible.

If a server or driver works under SMP=1 but hangs or misbehaves under SMP=2,
suspect a check-then-act race on the mailbox or the `blocked` flag.

### Driver RX handlers may re-enter the TX path

When a NIC driver's receive handler calls into the network stack with a raw
frame, the stack may need to reply immediately — an ARP response, an ICMP echo
reply, a UDP echo handler's answer. That reply travels back down through the
same driver's TX path. If the driver holds a lock across the network-stack call,
it will self-deadlock the moment such a frame arrives.

In Lisp this is structural rather than a locking discipline: the NIC driver
**sends** frames to `corenetwork` and **receives** TX requests back. The
corenetwork service context runs independently; there is no re-entrancy because
neither side calls the other synchronously. The old C network stack had to
manually copy handlers out from under their table locks before invoking them
(`CLAUDE.md` §Inter-module APIs) to break the same cycle. The message model
eliminates that class of bug entirely.

The lesson extends to any context: never hold a resource while blocking on a
`recv`, if an incoming message could need you to release that resource before it
completes.

---

## How this underpins servers and drivers

Every `Core*` service is a context running a `serve` loop. Every Lisp driver is
one or more contexts talking to those services by message. No C ABI crosses the
boundary. Fan-out notifications (`corepower`'s event broadcast), probe/claim
handshakes (`corestorage`), and streaming events (`coreinput`) are all special
cases of the same `send`/`recv` model described here.

The `serve` helper in `driver-util.clp` captures the idiom once. If you are
writing a new service, start from `serve` and add `ctx?` guards wherever your
handler handles a client-supplied reply address. See
[Adding a server](../guides/add-a-server.md) for the step-by-step guide.

Drivers that need hardware events use `irq-wait` (ISA) or `msi-wait` (PCIe
MSI) to park on an interrupt rather than `recv`, but the blocked/wake mechanism
is identical: a hardware ISR calls `lisp_ctx_wake`, the same single word-write
that `send` uses to unblock a receiver.

---

## See also

- [Capabilities & sandbox](capabilities-and-sandbox.md) — how `import` and
  `spawn-restricted` gate a context's authority
- [VM API reference](../vm/api.md) — full primitive reference including
  `spawn`, `send`, `recv`, `self`, `ctx?`, `yield`, `sleep`, and the
  low-level `%mailbox-*` / `%block` primitives
- [Adding a server](../guides/add-a-server.md) — step-by-step guide: `serve`,
  `reply-to`, `ctx?`, and wiring a new service into `lisp/init.clp`
- [CoreNetwork](../servers/corenetwork.md) — the network stack: the most
  complete example of fan-out, request/reply, and the deadline-based receive
  pattern (`dhcp-poll`, `dns-poll`)
- [CoreStorage](../servers/corestorage.md) — probe/claim and bounds-checked
  I/O forwarding using `ctx?` on reply addresses
- [CorePower](../servers/corepower.md) — event fan-out to registered device
  contexts: the simplest fire-and-forget broadcast example
