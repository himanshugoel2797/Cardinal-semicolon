;; corecompositor: the multi-client window compositor (notes/servers/CoreCompositor.md).
;;
;; PHASES 2-3. Phase 2 stood up the IPC root: the well-known PRIMARY mailbox
;; (`start-compositor-service`) + the `connect` handshake that spawns a dedicated
;; PER-CLIENT HANDLER context (the "secondary channel"). Phase 3 adds the SURFACE
;; PROTOCOL + the composite loop:
;;
;;   client                handler (relay)            root / instance (this serve loop)
;;   ------                --------------             ---------------------------------
;;   (connect a? reply) -> [spawns me] ------------->  reply (connected handler)
;;   (create-surface w h reply) -> (op ...) -------->  alloc 2 backings, mint 2 grants,
;;   <----------------------------- (surface id g0 g1 stride)   record the surface
;;   (map-grant g0/g1) ; zero-copy: draw into the back buffer
;;   (configure id x y vis) -> (op ...) ------------>  place + show; recomposite
;;   (commit id buf rects) -> (op ...) ------------->  flip front to `buf`; recomposite
;;   (destroy-surface id reply) -> (op ...) -------->  revoke grants, drop, recomposite
;;
;; OWNERSHIP (v1 = the N=1 case of the sharded model). The ROOT owns the GLOBAL,
;; z-ordered surface table + the screen back-buffer + all compositing, so it is the
;; single serialiser of the screen and cross-client occlusion is correct by one
;; painter's pass over one window list. The per-client HANDLER is a thin relay +
;; isolation boundary (its own mailbox, its client identity for cleanup, the future
;; home of phase-6 input). Phase 7 promotes "root" to a per-core INSTANCE that owns
;; its shard's clients and composites into a layer the owner merges -- at which
;; point this surface table moves onto the instance; the protocol is unchanged.
;;
;; CAPABILITIES ARE INJECTED, NOT IMPORTED. Allocating DMA backings and minting
;; grants need kernel authority (sys-mmio / sys-shm-mint), but importing those
;; kernel-only modules here would make the module unloadable in the host test
;; harness. Instead `init` -- which already holds that authority -- passes the
;; specific prims (dma-alloc-wb / grant-mint / grant-revoke) into
;; `start-compositor-service` by closure. That is exactly Cardinal's capability
;; delegation (init hands a service the narrow prims it needs), and it keeps the
;; whole module host-testable. The injected prims survive the `spawn-restricted '()`
;; root context because they are captured lexically, not acquired by `import`.

(define-module corecompositor
  (export start-compositor-service make-compositor-caps paint-windows)
  (import driver-util graphics)

  ;; --- injected capabilities --------------------------------------------------
  ;; (alloc nbytes) -> a phys-backed, zeroed DMA buffer (init: dma-alloc-wb);
  ;; (mint buf perms) -> a grant over buf (init: grant-mint);
  ;; (revoke g) -> invalidate a grant (init: grant-revoke).
  (define (make-compositor-caps alloc mint revoke) (list alloc mint revoke))
  (define (caps-alloc c)  (nth c 0))
  (define (caps-mint c)   (nth c 1))
  (define (caps-revoke c) (nth c 2))

  ;; --- the surface record (a mutable vector; lives only in the root) ----------
  ;; Two backings (double-buffered): the client draws into the buffer it is NOT
  ;; presenting, then `commit` flips `front`, so the compositor never reads a
  ;; half-drawn frame. g0/g1 are the grants handed to the client for those buffers.
  (define SF-ID 0) (define SF-CLIENT 1) (define SF-G0 2) (define SF-G1 3)
  (define SF-B0 4) (define SF-B1 5) (define SF-W 6) (define SF-H 7)
  (define SF-STRIDE 8) (define SF-X 9) (define SF-Y 10) (define SF-VIS 11)
  (define SF-FRONT 12) (define SF-ALPHA 13)
  (define (make-surf id client g0 g1 b0 b1 w h stride alpha?)
    (let ((v (make-vector 14 0)))
      (vector-set! v SF-ID id)       (vector-set! v SF-CLIENT client)
      (vector-set! v SF-G0 g0)       (vector-set! v SF-G1 g1)
      (vector-set! v SF-B0 b0)       (vector-set! v SF-B1 b1)
      (vector-set! v SF-W w)         (vector-set! v SF-H h)
      (vector-set! v SF-STRIDE stride)
      (vector-set! v SF-X 0)         (vector-set! v SF-Y 0)
      (vector-set! v SF-VIS #f)      (vector-set! v SF-FRONT 0)
      (vector-set! v SF-ALPHA alpha?)
      v))
  (define (sf r i) (vector-ref r i))
  ;; The committed-front backing as a source surface for blitting.
  (define (surf-src r)
    (make-surface (if (= (sf r SF-FRONT) 0) (sf r SF-B0) (sf r SF-B1))
                  (sf r SF-W) (sf r SF-H) (sf r SF-STRIDE)))

  ;; --- the painter (pure: no caps, no IPC -> host-testable) -------------------
  ;; `windows` is a back-to-front list of (src x y alpha?) specs (only the visible
  ;; ones). Clear to the background, then blit each window in order: painter's
  ;; algorithm, so a higher window correctly occludes a lower one. blit/blit-alpha
  ;; place the WHOLE source clipped to the screen bounds; alpha windows compose over
  ;; what is already there. (Rect-bounded recompositing is a later optimisation; v1
  ;; recomposites the whole screen on any change -- simplest and obviously correct,
  ;; and QEMU's framebuffer path is the bottleneck regardless.)
  (define (paint-windows screen bg windows)
    (clear screen bg)
    (for-each (lambda (w)
                (let ((src (nth w 0)) (x (nth w 1)) (y (nth w 2)) (a? (nth w 3)))
                  (if a? (blit-alpha screen src x y) (blit screen src x y))))
              windows))

  ;; Recomposite the whole screen from the surface table. `surfaces` is stored
  ;; top-first (newest on top), so reverse it to paint back-to-front, drop the
  ;; hidden ones, and project each record to a painter spec.
  (define (recomposite screen bg surfaces)
    (paint-windows screen bg
      (map (lambda (r) (list (surf-src r) (sf r SF-X) (sf r SF-Y) (sf r SF-ALPHA)))
           (filter (lambda (r) (sf r SF-VIS)) (reverse surfaces)))))

  ;; --- surface-table helpers --------------------------------------------------
  (define (find-surf id surfaces)
    (cond ((null? surfaces) #f)
          ((= (sf (car surfaces) SF-ID) id) (car surfaces))
          (else (find-surf id (cdr surfaces)))))
  (define (drop-surf id surfaces)
    (filter (lambda (r) (not (= (sf r SF-ID) id))) surfaces))
  ;; Move a surface to the top (front of the list) for `raise`.
  (define (raise-surf id surfaces)
    (let ((r (find-surf id surfaces)))
      (if r (cons r (drop-surf id surfaces)) surfaces)))

  ;; --- the per-client handler (secondary channel; a relay) --------------------
  ;; Forwards the client's surface ops to the root tagged with the client identity
  ;; and the transparency flag it declared at connect (so the root marks the
  ;; client's surfaces alpha and, in phase 7, routes it). A relay -- not a
  ;; compositor -- because cross-client occlusion needs ONE window list in the root.
  ;; The arity guard means a malformed message can't crash the handler.
  (define (make-handler root client transparency?)
    (spawn-restricted '()
      (lambda ()
        (let loop ()
          (let ((m (recv)))
            (if (pair? m) (send root (list 'op client transparency? m)))
            (loop))))))

  ;; Largest surface dimension we will allocate, in pixels per side. Clients are
  ;; SEMI-TRUSTED, so create-surface must bound the request: the VM has no
  ;; try/catch, and an unvalidated `w`/`h` would let a client crash the root serve
  ;; context for good -- a non-integer reaches `(* w 4)` (type error) or a huge
  ;; value makes `dma-alloc-wb` fail (returns no buffer, error propagates). Bounding
  ;; the dimensions defeats both; a genuine OOM under valid bounds is a system-wide
  ;; condition (and a shared limitation of every `serve` loop, not specific here).
  (define MAX-DIM 4096)

  ;; --- the root: surface ops over the global table ----------------------------
  ;; Threaded state is (next-id . surfaces). screen/bg/caps are captured constants.
  ;; `client` is the SENDER identity the handler stamped on (trusted: captured at
  ;; connect, unforgeable by message content). Replies go to `client`, never to a
  ;; reply field in the message, so a client cannot make the root `send` to an
  ;; arbitrary (or non-context) target. Every op validates arity AND ownership and
  ;; falls through harmlessly rather than killing the serve context.
  (define (handle-op st client transparency? m screen bg caps)
    (let ((next-id (car st)) (surfaces (cdr st))
          (verb (if (pair? m) (car m) #f)))
      ;; A surface this client is allowed to act on, or #f. Ownership is by the
      ;; sender identity, not just the id -- ids are sequential and guessable, so
      ;; without this a client could configure/commit/destroy another's window.
      (define (owned id)
        (let ((r (find-surf id surfaces)))
          (if (and r (eq? (sf r SF-CLIENT) client)) r #f)))
      (cond
        ;; (create-surface w h): validate dims, alloc 2 backings, mint 2 grants,
        ;; record (invisible until configured), reply with the grants. stride = w*4.
        ((eq? verb 'create-surface)
         (if (not (and (pair? (cdr m)) (pair? (cddr m))
                       (integer? (cadr m)) (integer? (caddr m))
                       (> (cadr m) 0) (> (caddr m) 0)
                       (<= (cadr m) MAX-DIM) (<= (caddr m) MAX-DIM)))
             (begin (send client (list 'surface-error 'bad-dimensions)) st)
             (let* ((w (cadr m)) (h (caddr m))
                    (stride (* w 4)) (sz (* stride h))
                    (b0 ((caps-alloc caps) sz)) (b1 ((caps-alloc caps) sz))
                    (g0 ((caps-mint caps) b0 'rw)) (g1 ((caps-mint caps) b1 'rw))
                    (r (make-surf next-id client g0 g1 b0 b1 w h stride transparency?)))
               (send client (list 'surface next-id g0 g1 stride))
               (cons (+ next-id 1) (cons r surfaces)))))   ; prepend = top of stack
        ;; (configure id x y visible): place + show/hide, then recomposite. Fire-
        ;; and-forget; only the owner's surface is touched.
        ((and (eq? verb 'configure) (pair? (cdr (cdddr m))))
         (let ((r (owned (cadr m))))
           (if r (begin (vector-set! r SF-X (caddr m))
                        (vector-set! r SF-Y (cadddr m))
                        (vector-set! r SF-VIS (nth m 4))
                        (recomposite screen bg surfaces))))
         st)
        ;; (commit id buf rects): flip the presented buffer, recomposite. Fire-and-
        ;; forget -- a client never blocks presenting a frame. The guard requires
        ;; `rects` to be present (phase 4 reads it to bound the flush; v1 ignores it
        ;; and recomposites whole-screen).
        ((and (eq? verb 'commit) (pair? (cdddr m)))
         (let ((r (owned (cadr m))))
           (if r (begin (vector-set! r SF-FRONT (caddr m))
                        (recomposite screen bg surfaces))))
         st)
        ;; (raise id): move the owner's surface to the top of the z-stack. Only
        ;; recomposite if it actually moved.
        ((and (eq? verb 'raise) (pair? (cdr m)))
         (if (owned (cadr m))
             (let ((s2 (raise-surf (cadr m) surfaces)))
               (recomposite screen bg s2)
               (cons next-id s2))
             st))
        ;; (destroy-surface id): revoke both grants, drop, recomposite, ack. Only
        ;; the owner can destroy; a bad/foreign id gets an error, not a spurious ok.
        ((and (eq? verb 'destroy-surface) (pair? (cdr m)))
         (let ((r (owned (cadr m))))
           (if (not r)
               (begin (send client (list 'destroy-error 'no-such-surface)) st)
               (begin ((caps-revoke caps) (sf r SF-G0))
                      ((caps-revoke caps) (sf r SF-G1))
                      (let ((s2 (drop-surf (cadr m) surfaces)))
                        (recomposite screen bg s2)
                        (send client 'ok)
                        (cons next-id s2))))))
        ;; (probe-pixel x y): the composited screen pixel. A test/debug hook,
        ;; relayed through the handler (not sent to the root directly) so it is
        ;; FIFO-ordered AFTER the client's preceding commit on the same channel --
        ;; a direct-to-root probe could overtake the relayed commit and read the
        ;; screen before compositing.
        ((and (eq? verb 'probe-pixel) (pair? (cddr m)))
         (send client (get-pixel screen (cadr m) (caddr m)))
         st)
        (else
         (display "[corecompositor] handler op ignored: ") (display verb) (newline)
         st))))

  ;; The PRIMARY mailbox loop. Demuxes the connect handshake, the handler-relayed
  ;; surface ops (`op`), and a test/debug `probe-pixel` (reads the composited
  ;; screen so an end-to-end test can assert what landed there).
  (define (start-compositor-service screen caps)
    (let ((bg (rgb screen 28 30 44)))
      (serve (cons 1 '())                       ; (next-id . surfaces)
        (lambda (st m)
          (cond
            ((not (pair? m)) st)
            ;; (connect transparency? reply) -> spawn this client's handler. `reply`
            ;; must be a context: it becomes the client's authenticated identity and
            ;; the address every later reply is sent to, so a non-context here (which
            ;; would abort the serve loop on the `send` below, since the VM has no
            ;; try/catch) is rejected outright.
            ((and (eq? (car m) 'connect) (pair? (cddr m)) (ctx? (caddr m)))
             (let ((h (make-handler (self) (caddr m) (cadr m))))
               (send (caddr m) (list 'connected h))
               st))
            ;; (op client transparency? msg) -> a relayed surface op (incl. the
            ;; test-only probe-pixel; see handle-op for why it goes via the handler).
            ((and (eq? (car m) 'op) (pair? (cdddr m)))
             (handle-op st (cadr m) (caddr m) (cadddr m) screen bg caps))
            (else
             (display "[corecompositor] primary: ignoring malformed ")
             (display (car m)) (newline)
             st)))))))
