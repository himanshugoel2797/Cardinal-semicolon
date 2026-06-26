;; corecompositor: the multi-client window compositor (notes/servers/CoreCompositor.md).
;;
;; PHASE 2 -- the IPC skeleton: the well-known PRIMARY mailbox + the `connect`
;; handshake that spawns a dedicated PER-CLIENT HANDLER context (the "secondary
;; channel"). The handler owns exactly one client's window list, grants, and
;; damage; after the handshake the client talks only to its handler, so the
;; primary mailbox never carries per-surface traffic and one noisy client cannot
;; wedge another's handshake. The surface protocol (create/configure/commit/
;; destroy) + the composite loop land on the handler in phase 3; phase 2 stands up
;; the wiring and proves it end to end (a liveness `ping`).
;;
;; Cardinal IPC is a shared-nothing actor model: a context owns a single FIFO
;; mailbox, `send` deep-copies a message EXCEPT context handles (and grants), which
;; pass by identity. So a request carries the caller's own handle as its reply
;; address, and "open a separate mailbox" is realised as "spawn a handler context
;; and hand the peer its handle" -- there is no selective receive and no name
;; service (init distributes the primary handle by closure).
;;
;; Capability posture: the handler is spawned `spawn-restricted '()` like every
;; service (it can acquire NO new import authority), yet it still closes over
;; whatever this module imported AT LOAD. Phase 3 adds `(import sys-shm-mint
;; sys-mmio)` to the clause below; because the handler lambda is defined in this
;; module, it then closes over `grant-mint`/`mmio-map*` and can mint grants even
;; though spawned restricted -- the empty grant only forbids acquiring MORE. Those
;; imports are deliberately NOT present in phase 2: nothing mints yet, and adding
;; them now would break the host `test_compositor` (the sys-shm-mint module is
;; kernel-only, not registered in the host Lisp env).

(define-module corecompositor
  (export start-compositor-service)
  (import driver-util)

  ;; A per-client handler context. Threads this client's state -- `surfaces` (an
  ;; (id . surface-rec) alist) and `next-id` -- across a FIFO message loop. Phase 2
  ;; carries no surfaces yet; it answers the liveness `ping` and ignores (logs)
  ;; anything else, leaving a labelled home for the phase-3 verbs. `client` is the
  ;; connecting context (its reply handle), `transparency?` the routing flag it
  ;; declared at connect (phase 7 routes translucent clients to the owner).
  (define (make-handler client transparency?)
    (spawn-restricted '()
      (lambda ()
        (let loop ((surfaces '()) (next-id 1))
          (let ((m (recv)))
            (cond
              ((not (pair? m)) (loop surfaces next-id))     ; not a tagged envelope
              ;; (ping reply) -> (pong): handshake-complete / liveness probe. Proves
              ;; the secondary channel round-trips before any surface exists. The
              ;; arity guard keeps a malformed (ping) from crashing the handler --
              ;; a client (phases 3-5 are semi-trusted) must not be able to kill it.
              ((eq? (car m) 'ping)
               (if (pair? (cdr m)) (send (cadr m) (list 'pong)))
               (loop surfaces next-id))
              ;; PHASE 3 lands here: (create-surface w h reply), (configure ...),
              ;; (commit id buf rects), (destroy-surface id reply).
              (else
               (display "[corecompositor] handler: unhandled ")
               (display (car m)) (newline)
               (loop surfaces next-id))))))))

  ;; The well-known PRIMARY mailbox. Its only verb is the connect handshake:
  ;; a client sends (connect transparency? reply); we spawn that client's dedicated
  ;; handler and reply (connected handler). State is the (client . handler) roster
  ;; -- unused in phase 2 beyond bookkeeping, it becomes the shard's client table
  ;; in phase 7. `serve` gives the long-lived restricted-context message loop.
  (define (start-compositor-service)
    (serve '()
      (lambda (clients m)
        (cond
          ((not (pair? m)) clients)
          ;; (connect transparency? reply). The arity guard (cddr is a pair => at
          ;; least 3 elements) means a malformed connect falls through to `else`
          ;; and the primary mailbox SURVIVES, rather than a `caddr`-on-'() error
          ;; killing the serve context and wedging all future handshakes -- the
          ;; clients here are semi-trusted from phase 3 on.
          ((and (eq? (car m) 'connect) (pair? (cdr m)) (pair? (cddr m)))
           (let* ((transparency? (cadr m))
                  (reply (caddr m))
                  (h (make-handler reply transparency?)))
             (send reply (list 'connected h))
             (cons (cons reply h) clients)))
          (else
           (display "[corecompositor] primary: ignoring malformed ")
           (display (car m)) (newline)
           clients))))))
