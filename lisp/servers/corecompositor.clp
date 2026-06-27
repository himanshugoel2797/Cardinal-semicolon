;; corecompositor: the multi-client window compositor (notes/servers/CoreCompositor.md).
;;
;; PHASES 2-4. Phase 2 stood up the IPC root: the well-known PRIMARY mailbox
;; (`start-compositor-service`) + the `connect` handshake that spawns a dedicated
;; PER-CLIENT HANDLER context (the "secondary channel"). Phase 3 added the SURFACE
;; PROTOCOL + the composite loop. Phase 4 is the DRIVER SEAM: the compositor owns
;; the real scanout and pushes composited damage to the display through an injected
;; `present` capability (virtio-gpu flush-rects, or a WC-framebuffer copy) -- so
;; windows now actually appear on screen, not just in a RAM back-buffer:
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
  (export start-compositor-service make-compositor-caps paint-windows make-shard-cfg Z-BAND)
  (import driver-util graphics)

  ;; --- injected capabilities --------------------------------------------------
  ;; (alloc nbytes) -> a phys-backed, zeroed DMA buffer (init: dma-alloc-wb);
  ;; (mint buf perms) -> a grant over buf (init: grant-mint);
  ;; (revoke g) -> invalidate a grant (init: grant-revoke);
  ;; (present rects) -> push the listed (x y w h) screen rects to the real display
  ;;   (phase 4: virtio-gpu flush-rects, or a WC-framebuffer back->front copy), or
  ;;   #f for an off-screen RAM screen (the compositor composites but displays
  ;;   nothing -- the phase-3 posture, kept for the headless self-test).
  ;; (map grant) -> map a grant the OWNER was handed (init: map-grant) to a `bytes`
  ;;   view of its physical region. Used to map a per-core shard's grant-shared layer
  ;;   so the merge can read it. Injected (not imported) for the same host-loadable
  ;;   reason as the rest; #f on an instance that maps nothing (a pure shard).
  (define (make-compositor-caps alloc mint revoke present map)
    (list alloc mint revoke present map))
  (define (caps-alloc c)   (nth c 0))
  (define (caps-mint c)    (nth c 1))
  (define (caps-revoke c)  (nth c 2))
  (define (caps-present c) (nth c 3))
  (define (caps-map c)     (nth c 4))

  ;; --- the surface record (a mutable vector; lives only in the root) ----------
  ;; Two backings (double-buffered): the client draws into the buffer it is NOT
  ;; presenting, then `commit` flips `front`, so the compositor never reads a
  ;; half-drawn frame. g0/g1 are the grants handed to the client for those buffers.
  ;; SF-Z is the surface's GLOBAL z-key (phase 7): a monotonic stamp the z authority
  ;; assigns on create and bumps on raise. It is what the layer's z-plane is stamped
  ;; with and what the two-pass merge picks/gates on -- so occlusion is by z-buffer
  ;; (opaque) and z-test (translucent), not list order. z==0 is reserved for "empty"
  ;; in the layer planes, so real z starts at 1 (see fresh-z).
  (define SF-ID 0) (define SF-CLIENT 1) (define SF-G0 2) (define SF-G1 3)
  (define SF-B0 4) (define SF-B1 5) (define SF-W 6) (define SF-H 7)
  (define SF-STRIDE 8) (define SF-X 9) (define SF-Y 10) (define SF-VIS 11)
  (define SF-FRONT 12) (define SF-ALPHA 13) (define SF-Z 14)
  (define (make-surf id client g0 g1 b0 b1 w h stride alpha?)
    (let ((v (make-vector 15 0)))
      (vector-set! v SF-ID id)       (vector-set! v SF-CLIENT client)
      (vector-set! v SF-G0 g0)       (vector-set! v SF-G1 g1)
      (vector-set! v SF-B0 b0)       (vector-set! v SF-B1 b1)
      (vector-set! v SF-W w)         (vector-set! v SF-H h)
      (vector-set! v SF-STRIDE stride)
      (vector-set! v SF-X 0)         (vector-set! v SF-Y 0)
      (vector-set! v SF-VIS #f)      (vector-set! v SF-FRONT 0)
      (vector-set! v SF-ALPHA alpha?) (vector-set! v SF-Z 0)
      v))
  (define (sf r i) (vector-ref r i))
  ;; The committed-front backing as a source surface for blitting. The screen's
  ;; channel offsets (ro/go/bo) are stamped on it because `gfx-blit!` is a RAW
  ;; 32-bit pixel copy -- it does not repack channels -- so a source must share the
  ;; screen's pixel layout or red/blue would swap. The client draws into the same
  ;; backing using the format the compositor advertised at connect, so all three
  ;; (client draw, this source, the screen) agree. (The boot framebuffer is
  ;; 0xRRGGBB = 16/8/0; the virtio-gpu X8R8G8B8 scanout is 8/16/24.)
  (define (surf-src r ro go bo)
    (make-surface* (if (= (sf r SF-FRONT) 0) (sf r SF-B0) (sf r SF-B1))
                   (sf r SF-W) (sf r SF-H) (sf r SF-STRIDE) ro go bo '()))
  ;; The surface's on-screen bounding rect (x y w h) -- the damage a change to it
  ;; contributes to the flush.
  (define (surf-rect r) (list (sf r SF-X) (sf r SF-Y) (sf r SF-W) (sf r SF-H)))

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

  ;; --- the merge buffers + z authority (phase 7) ------------------------------
  ;; `mrg` bundles this instance's compositing scratch: the opaque LAYER (a colour
  ;; plane + a z plane, both screen-sized), the owner's Zop plane, and the monotonic
  ;; z counter. All planes share the screen's stride so the z-pick can run flat over
  ;; the whole buffer (padding columns stay z==0 and are skipped). This is the N=1
  ;; case of the sharded model: one instance owns one layer and merges it (a 1-layer
  ;; z-pick) into the scanout -- phase 7B-wiring promotes the layer to a grant-shared
  ;; per-core buffer the owner maps, with the same merge.
  (define (make-mrg lc lz zop zctr shards owner focus)
    (vector lc lz zop zctr shards owner focus))
  (define (mrg-lc m)     (vector-ref m 0))   ; layer colour plane (a surface)
  (define (mrg-lz m)     (vector-ref m 1))   ; layer z plane (a surface; fill-rect stamps z)
  (define (mrg-zop m)    (vector-ref m 2))   ; the merged per-pixel topmost-opaque z (raw bytes)
  (define (mrg-zctr m)   (vector-ref m 3))   ; monotonic z counter (1-slot vector)
  ;; Cross-shard input (phase 7): a 1-slot cell holding the per-shard FOCUS
  ;; CANDIDATES the owner uses to route keyboard input to the globally-focused
  ;; window even when it is hosted on another core. Each shard reports its
  ;; top-most visible window's (client . z) with every layer-update; the owner
  ;; keeps a list of (shard-handle client z) and routes a key to the max-z client
  ;; across all shards + its own focus. (The shard knows nothing of global focus;
  ;; the owner is the focus authority, as for z.) Owner instance only.
  (define (mrg-focus m)  (vector-ref m 6))   ; 1-slot cell: list of (shard-handle client z)
  ;; The OWNER handle if this instance is a SHARD (its recomposite builds its layer
  ;; then notifies the owner, which re-merges); #f if this IS the owner (its
  ;; recomposite folds all shard layers + its own into the scanout). The one knob
  ;; that makes start-compositor-service dual-role: a shard and the owner share the
  ;; whole surface-table / handle-op / opaque-layer-build machinery and differ only
  ;; in how recomposite finalises (notify vs merge) and how `connect` routes.
  (define (mrg-owner m)  (vector-ref m 5))   ; owner handle (shard) or #f (owner)
  ;; Phase 7 cross-core: a 1-slot cell holding the list of REMOTE shard layers this
  ;; owner merges in. Each entry is (colour-view . z-view) -- the owner's mapped
  ;; views of a per-core shard's grant-shared (colour,z) layer planes (same
  ;; stride/np as the scanout). The opaque merge folds the z-pick over the owner's
  ;; own layer plus every shard layer; an empty list is the single-instance (N=1)
  ;; case (the SMP=1 / no-AP boot), so the owner-only path is unchanged.
  (define (mrg-shards m) (vector-ref m 4))   ; 1-slot cell: list of shard records
  ;; A shard record is (handle colour-view z-view): the shard's primary handle (the
  ;; owner routes opaque clients to it) and the owner's mapped views of the shard's
  ;; grant-shared (colour,z) layer planes (the merge folds them in).
  (define (shard-handle s) (car s))
  (define (shard-color s)  (cadr s))
  (define (shard-z s)      (caddr s))
  ;; The z authority: a fresh top z-stamp. Real z starts at 1 (z==0 = empty layer px).
  (define (fresh-z mrg)
    (let ((c (mrg-zctr mrg)))
      (vector-set! c 0 (+ (vector-ref c 0) 1))
      (vector-ref c 0)))

  ;; A SHARD config passed to start-compositor-service to put it in shard role: the
  ;; owner handle, the grant-shared layer planes (colour + z surfaces) the shard
  ;; composites its clients into, and a z BASE so shards occupy disjoint z bands
  ;; (core i starts at i*Z-BAND) -- a deterministic cross-shard stacking without a
  ;; per-window round-trip to the owner's z authority (true global z is a later
  ;; refinement). #f for the owner instance (it allocates its own scratch layer).
  (define Z-BAND 1000000)
  (define (make-shard-cfg owner lc lz z-base) (vector owner lc lz z-base))
  (define (scfg-owner c)  (vector-ref c 0))
  (define (scfg-lc c)     (vector-ref c 1))
  (define (scfg-lz c)     (vector-ref c 2))
  (define (scfg-zbase c)  (vector-ref c 3))

  ;; Recomposite the whole scanout from the surface table via the phase-7 two-pass
  ;; merge. `surfaces` is stored top-first; reverse for back-to-front. Opaque windows
  ;; build the layer (colour + z); the merge z-picks the layer into the scanout
  ;; (yielding Zop); translucent windows alpha-over only where they are in front of
  ;; Zop. At N=1 the layer is this instance's own; the structure is identical for N
  ;; shards (fold the z-pick over each shard's layer).
  (define (recomposite screen bg surfaces mrg)
    (let* ((lc (mrg-lc mrg)) (lz (mrg-lz mrg)) (zop (mrg-zop mrg))
           (sw (surface-width screen)) (sh (surface-height screen))
           ;; np = pixels in the WHOLE plane incl. any row padding, so the flat
           ;; gfx-zpick! addressing (i -> byte i*4) agrees with fill-rect/blit's 2D
           ;; addressing (y*stride + x*4). This requires stride to be a multiple of 4
           ;; -- always true for a 32-bpp surface (a row is N*4 bytes, padding too) and
           ;; the same precondition gfx-blit!/-zpick! already assume. Padding columns
           ;; stay layer-z==0 (fill-rect only touches window rects within `sw`), so the
           ;; z-pick skips them and the scanout padding is left untouched.
           (stride (surface-stride screen)) (np (* (quotient stride 4) sh))
           (ro (surface-r-off screen)) (go (surface-g-off screen)) (bo (surface-b-off screen))
           (vis (filter (lambda (r) (sf r SF-VIS)) (reverse surfaces)))   ; back-to-front
           (opaque (filter (lambda (r) (not (sf r SF-ALPHA))) vis))
           (translu (filter (lambda (r) (sf r SF-ALPHA)) vis)))
      ;; 1. opaque layer: clear the z plane (z==0 = empty), then composite the opaque
      ;; windows back-to-front -- blit colour AND stamp the window's z over its rect;
      ;; the topmost opaque window wins both colour and z at each covered pixel. The
      ;; colour plane `lc` is intentionally NOT cleared: gfx-zpick! reads lc[i] only
      ;; where lz[i] > 0, which is freshly stamped this frame, so stale colour under a
      ;; z==0 pixel is never read (clearing it would be wasted work).
      (bytes-fill32! (surface-fb lz) 0 np 0)
      (for-each (lambda (r)
                  (blit lc (surf-src r ro go bo) (sf r SF-X) (sf r SF-Y))
                  (fill-rect lz (sf r SF-X) (sf r SF-Y) (sf r SF-W) (sf r SF-H) (sf r SF-Z)))
                opaque)
      ;; A SHARD stops here: its lc/lz ARE the grant-shared layer the owner maps, so
      ;; once the opaque layer is built it just notifies the owner, which re-merges
      ;; every shard layer into the scanout. (Shards host only opaque clients --
      ;; translucent is routed to the owner -- so there is nothing else to do.) The
      ;; caller's present! is a no-op for a shard (its present cap is #f). It also
      ;; reports its FOCUS CANDIDATE -- its top-visible window's client+z -- so the
      ;; owner can route keyboard input to a window hosted on this core (`(self)` is
      ;; this shard's serve handle = the one it registered).
      (if (mrg-owner mrg)
          (let ((f (focused surfaces)))
            (send (mrg-owner mrg)
                  (list 'layer-update (self)
                        (if f (sf f SF-CLIENT) #f)
                        (if f (sf f SF-Z) 0))))
          (begin
      ;; 2. merge (OWNER): clear the scanout to the desktop bg and Zop to 0, z-pick the
      ;; owner's opaque layer in, followed by every REMOTE shard's layer. A z-pick is
      ;; max-z per pixel and order-independent, so folding it over (own + shard0 +
      ;; shard1 + ...) yields the correct global opaque image + Zop regardless of how
      ;; windows are sharded across cores. Shard layers are the owner's mapped views
      ;; of grant-shared buffers (a revoked one reads as a zero page -> contributes
      ;; nothing). With no shards this is exactly the single-layer N=1 merge.
      (clear screen bg)
      (bytes-fill32! zop 0 np 0)
      (gfx-zpick! (surface-fb screen) zop (surface-fb lc) (surface-fb lz) np)
      (for-each (lambda (s) (gfx-zpick! (surface-fb screen) zop (shard-color s) (shard-z s) np))
                (vector-ref (mrg-shards mrg) 0))
      ;; 3. translucent pass: alpha-over each translucent window onto the scanout, but
      ;; only where its z is above Zop (in front of the nearest opaque surface there).
      (for-each (lambda (r)
                  (let ((src (surf-src r ro go bo)))
                    (gfx-blend-z! (surface-fb screen) stride sw sh (sf r SF-X) (sf r SF-Y)
                                  (surface-fb src) (surface-stride src)
                                  (surface-width src) (surface-height src)
                                  zop (sf r SF-Z))))
                translu)))))

  ;; --- surface-table helpers --------------------------------------------------
  ;; Does proper list `lst` have at least `n` elements? Walks safely (checks pair?
  ;; before cdr), so it never errors on a short/improper message -- unlike `cdddr`,
  ;; whose `(cdr '())` aborts the context. Every per-op arity guard uses this: a
  ;; client is semi-trusted and the serve loop has no try/catch, so a truncated
  ;; message must fall through, not kill the root.
  (define (len>= lst n)
    (cond ((<= n 0) #t)
          ((pair? lst) (len>= (cdr lst) (- n 1)))
          (else #f)))
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

  ;; --- input hit-testing (phase 6) --------------------------------------------
  ;; The grabbable strip at the top of every window, in pixels: a pointer press
  ;; here begins a compositor-internal MOVE (title-bar drag); a press below it is
  ;; routed to the client. A fixed compositor policy -- independent of whatever a
  ;; client happens to paint as its title bar (cosmetic), so the compositor needs
  ;; no per-window chrome metadata.
  (define TITLE-H 28)
  ;; The top-most VISIBLE surface (the list is stored top-first), or #f. This is
  ;; the focus target: keyboard events route here.
  (define (focused surfaces)
    (cond ((null? surfaces) #f)
          ((sf (car surfaces) SF-VIS) (car surfaces))
          (else (focused (cdr surfaces)))))
  ;; The top-most visible surface whose on-screen rect contains (x,y), or #f --
  ;; the window a pointer press lands on (walk top-first so the upper window wins
  ;; an overlap, matching the painter's order).
  (define (surf-at surfaces x y)
    (cond ((null? surfaces) #f)
          ((let ((r (car surfaces)))
             (and (sf r SF-VIS)
                  (>= x (sf r SF-X)) (< x (+ (sf r SF-X) (sf r SF-W)))
                  (>= y (sf r SF-Y)) (< y (+ (sf r SF-Y) (sf r SF-H)))))
           (car surfaces))
          (else (surf-at (cdr surfaces) x y))))

  ;; --- cross-shard keyboard focus (phase 7) -----------------------------------
  ;; Record a shard's reported focus candidate (its top-visible window's client+z,
  ;; or client #f if it has none): replace this shard's prior entry. Owner only.
  (define (update-focus mrg shard-handle client z)
    (let* ((cell (mrg-focus mrg))
           (without (filter (lambda (e) (not (eq? (car e) shard-handle)))
                            (vector-ref cell 0))))
      (vector-set! cell 0 (cons (list shard-handle client z) without))))
  ;; The GLOBALLY focused window's client (max z across the owner's own top-visible
  ;; window and every shard's focus candidate), or #f. This is where a key routes,
  ;; whichever instance hosts it. (Shard windows live in high z bands, so a visible
  ;; shard window outranks the owner's -- consistent with the layer merge.)
  (define (global-focus-client surfaces mrg)
    (let ((own (focused surfaces)))
      (let loop ((cands (vector-ref (mrg-focus mrg) 0))
                 (bc (if own (sf own SF-CLIENT) #f))
                 (bz (if own (sf own SF-Z) -1)))
        (if (null? cands) bc
            (let ((c (cadr (car cands))) (z (caddr (car cands))))
              (if (and c (> z bz))
                  (loop (cdr cands) c z)
                  (loop (cdr cands) bc bz)))))))

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
  (define (handle-op st client transparency? m screen bg caps drag-state mrg)
    (let ((next-id (car st)) (surfaces (cdr st))
          (verb (if (pair? m) (car m) #f)))
      ;; A surface this client is allowed to act on, or #f. Ownership is by the
      ;; sender identity, not just the id -- ids are sequential and guessable, so
      ;; without this a client could configure/commit/destroy another's window.
      (define (owned id)
        (let ((r (find-surf id surfaces)))
          (if (and r (eq? (sf r SF-CLIENT) client)) r #f)))
      ;; Phase 4 driver seam: after recompositing into the screen back-buffer, push
      ;; the changed `rects` to the real display via the injected present cap. The
      ;; whole back-buffer is repainted (v1), but only the damaged rects are flushed
      ;; -- everything else on the display is already correct (nothing else moved),
      ;; so a bounded flush is correct and cheap. present is #f for a RAM screen.
      (define (present! rects)
        (let ((p (caps-present caps))) (if p (p rects))))
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
               (vector-set! r SF-Z (fresh-z mrg))   ; a fresh top z-stamp (z authority)
               (send client (list 'surface next-id g0 g1 stride))
               (cons (+ next-id 1) (cons r surfaces)))))   ; prepend = top of stack
        ;; (configure id x y visible): place + show/hide, then recomposite. Fire-
        ;; and-forget; only the owner's surface is touched. Damage = the old rect
        ;; (vacated -> repaint to whatever is now under it) plus, when the surface is
        ;; now visible, the new rect (where it landed) -- so a moved/shown window
        ;; flushes both; a hide flushes only the vacated rect. id/x/y must be integers
        ;; (a non-integer would reach `=`/`gfx-blit!` and kill the root).
        ((and (eq? verb 'configure) (len>= m 5)
              (integer? (cadr m)) (integer? (caddr m)) (integer? (cadddr m)))
         (let ((r (owned (cadr m))))
           (if r (let ((old (surf-rect r)))
                   (vector-set! r SF-X (caddr m))
                   (vector-set! r SF-Y (cadddr m))
                   (vector-set! r SF-VIS (nth m 4))
                   (recomposite screen bg surfaces mrg)
                   (present! (if (sf r SF-VIS) (list old (surf-rect r)) (list old))))))
         st)
        ;; (commit id buf rects): flip the presented buffer, recomposite, flush the
        ;; surface's rect (its content changed in place). Fire-and-forget -- a client
        ;; never blocks presenting a frame. id/buf must be integers (buf reaches `=`
        ;; in surf-src); a non-integer would kill the root. `rects` (position 3) is
        ;; required present (v1 flushes the whole surface rect; bounding to the
        ;; client's `rects` is a later refinement).
        ((and (eq? verb 'commit) (len>= m 4) (integer? (cadr m)) (integer? (caddr m)))
         (let ((r (owned (cadr m))))
           (if r (begin (vector-set! r SF-FRONT (caddr m))
                        (recomposite screen bg surfaces mrg)
                        (if (sf r SF-VIS) (present! (list (surf-rect r)))))))
         st)
        ;; (raise id): move the owner's surface to the top of the z-stack, then flush
        ;; its rect (occlusion within it changed). Only recomposite if it owns the id.
        ((and (eq? verb 'raise) (pair? (cdr m)) (integer? (cadr m)))
         (let ((r0 (owned (cadr m))))
           (if r0
             (let ((s2 (raise-surf (cadr m) surfaces)))
               (vector-set! r0 SF-Z (fresh-z mrg))   ; fresh top z so the z-buffer lifts it
               (recomposite screen bg s2 mrg)
               (if (sf r0 SF-VIS) (present! (list (surf-rect r0))))
               (cons next-id s2))
             st)))
        ;; (destroy-surface id): revoke both grants, drop, recomposite, flush the
        ;; vacated rect, ack. Only the owner can destroy; a bad/foreign id gets an
        ;; error, not a spurious ok.
        ((and (eq? verb 'destroy-surface) (pair? (cdr m)) (integer? (cadr m)))
         (let ((r (owned (cadr m))))
           (if (not r)
               (begin (send client (list 'destroy-error 'no-such-surface)) st)
               (let ((old (surf-rect r)) (was-vis (sf r SF-VIS)))
                 ((caps-revoke caps) (sf r SF-G0))
                 ((caps-revoke caps) (sf r SF-G1))
                 ;; if this surface is the one being dragged, abandon the drag --
                 ;; its id is about to be invalid, and a pointer-up may never come.
                 (let ((d (vector-ref drag-state 0)))
                   (if (and d (= (car d) (cadr m))) (vector-set! drag-state 0 #f)))
                 (let ((s2 (drop-surf (cadr m) surfaces)))
                   (recomposite screen bg s2 mrg)
                   (if was-vis (present! (list old)))
                   (send client 'ok)
                   (cons next-id s2))))))
        ;; (probe-pixel x y): the composited screen pixel. A test/debug hook,
        ;; relayed through the handler (not sent to the root directly) so it is
        ;; FIFO-ordered AFTER the client's preceding commit on the same channel --
        ;; a direct-to-root probe could overtake the relayed commit and read the
        ;; screen before compositing.
        ((and (eq? verb 'probe-pixel) (len>= m 3)
              (integer? (cadr m)) (integer? (caddr m)))
         (send client (get-pixel screen (cadr m) (caddr m)))
         st)
        (else
         (display "[corecompositor] handler op ignored: ") (display verb) (newline)
         st))))

  ;; --- input routing + drag-move (phase 6) ------------------------------------
  ;; The compositor is `coreinput`'s subscriber and the focus owner: every input
  ;; event arrives here as `(input ev)` on the PRIMARY mailbox and is routed by
  ;; policy. `drag-state` is a 1-slot mutable cell holding #f or `(id offx offy)`
  ;; while a title-bar drag is in progress (offx/offy = grab point relative to the
  ;; window origin, so the window tracks the pointer without jumping). Threaded
  ;; `st` is `(next-id . surfaces)`, returned (possibly re-stacked by a focus
  ;; raise) like handle-op. Two event shapes, both arity/type-guarded so a
  ;; malformed event falls through rather than killing the root:
  ;;   (key code pressed?)   -> the focused (top-most visible) window's client
  ;;   (pointer x y down?)   -> title-bar press = MOVE; body press = route to client
  ;; Resize is deferred: the surface backing is a fixed-size DMA buffer, so a true
  ;; resize needs a client-cooperative realloc (re-create-surface) -- move only here.
  (define (handle-input st ev screen bg caps drag-state mrg)
    (let ((next-id (car st)) (surfaces (cdr st))
          (tag (if (pair? ev) (car ev) #f)))
      (define (present! rects) (let ((p (caps-present caps))) (if p (p rects))))
      (cond
        ;; keyboard -> the GLOBALLY focused window's client, wherever it is hosted:
        ;; the max-z visible window across the owner's own surfaces AND every shard's
        ;; reported focus candidate. So a window on another core still receives keys.
        ;; No focused window anywhere -> dropped.
        ((and (eq? tag 'key) (len>= ev 3))
         (let ((target (global-focus-client surfaces mrg)))
           (if target (send target (list 'input ev))))
         st)
        ;; pointer: drag in progress -> move/end; else a press hit-tests the stack.
        ;; `down?` MUST be a boolean: Scheme treats integer 0 as TRUE, so a driver
        ;; that mirrored ps2's 1/0 key convention for the button would read a release
        ;; (0) as a press. Require #t/#f so the button state is unambiguous.
        ((and (eq? tag 'pointer) (len>= ev 4)
              (integer? (cadr ev)) (integer? (caddr ev)) (boolean? (cadddr ev)))
         (let ((x (cadr ev)) (y (caddr ev)) (down? (cadddr ev))
               (d (vector-ref drag-state 0)))
           (cond
             ;; an active drag: a button-down event moves the window to follow the
             ;; pointer (recomposite + flush old∪new); a button-up ends the drag. If
             ;; the dragged surface has vanished (destroyed mid-drag), abandon the
             ;; drag -- destroy-surface clears it proactively, but a stale id here
             ;; (any path that drops a surface) must not pin drag-state forever.
             (d
              (if down?
                  (let ((r (find-surf (car d) surfaces)))
                    (if r (let ((old (surf-rect r)))
                            (vector-set! r SF-X (- x (cadr d)))
                            (vector-set! r SF-Y (- y (caddr d)))
                            (recomposite screen bg surfaces mrg)
                            (present! (list old (surf-rect r))))
                        (vector-set! drag-state 0 #f)))
                  (vector-set! drag-state 0 #f))
              st)
             ;; a fresh press: focus-follows-click (raise the hit window), then a
             ;; title-bar press starts a drag, a body press routes to the client in
             ;; window-local coordinates. A press on no window is ignored.
             (down?
              (let ((r (surf-at surfaces x y)))
                (if (not r) st
                    ;; raise-surf re-lists the SAME surface vector `r` (no copy), so
                    ;; mutating r's z here is seen by the recomposite over s2.
                    (let ((s2 (raise-surf (sf r SF-ID) surfaces)))
                      (vector-set! r SF-Z (fresh-z mrg))   ; focus-follows-click lifts it in z
                      (recomposite screen bg s2 mrg)
                      (present! (list (surf-rect r)))
                      (if (< (- y (sf r SF-Y)) TITLE-H)
                          (vector-set! drag-state 0
                            (list (sf r SF-ID) (- x (sf r SF-X)) (- y (sf r SF-Y))))
                          (send (sf r SF-CLIENT)
                                (list 'input (list 'pointer (- x (sf r SF-X))
                                                   (- y (sf r SF-Y)) #t))))
                      (cons next-id s2)))))
             ;; pointer motion with no button and no drag: no hover routing in v1.
             (else st))))
        (else st))))

  ;; The PRIMARY mailbox loop. Demuxes the connect handshake, the handler-relayed
  ;; surface ops (`op`), input events from coreinput (`input`), and a test/debug
  ;; `probe-pixel` (reads the composited screen so an end-to-end test can assert).
  ;; Start a compositor instance. `shard-cfg` #f => the OWNER (owns the scanout +
  ;; the merge + routing + the z authority); a make-shard-cfg => a SHARD (composites
  ;; its routed clients into the grant-shared layer it was handed and notifies the
  ;; owner). The two share this whole serve loop and surface machinery.
  (define (start-compositor-service screen caps shard-cfg)
    (let ((bg (rgb screen 28 30 44))
          ;; the screen's pixel format, advertised to clients so they draw into
          ;; their granted backing with the SAME channel layout the screen uses
          ;; (gfx-blit! is a raw copy -- see surf-src). (r-off g-off b-off).
          (fmt (list (surface-r-off screen) (surface-g-off screen) (surface-b-off screen)))
          ;; ephemeral title-bar-drag state: #f, or (id offx offy) mid-drag. A
          ;; captured 1-slot cell rather than threaded `st` -- it is interaction
          ;; state, not part of the surface table, and the root is single-threaded.
          (drag-state (make-vector 1 #f))
          ;; round-robin cursor for routing opaque clients across shards (owner only).
          (rr (make-vector 1 0)))
      ;; Phase 7: this instance's compositing buffers. The opaque layer (colour + z
      ;; planes) and the Zop plane are screen-sized and share the screen's stride, so
      ;; the z-pick can run flat over the whole buffer. The OWNER allocates scratch
      ;; planes; a SHARD reuses the grant-shared planes it was handed (so the owner
      ;; maps the very buffers the shard composites into). z counter starts at the
      ;; shard's z-base (0 for the owner); fresh-z increments from there.
      (let* ((sw (surface-width screen)) (sh (surface-height screen))
             (stride (surface-stride screen)) (plane (* stride sh))
             (lc (if shard-cfg (scfg-lc shard-cfg)
                     (make-surface* (make-bytes plane) sw sh stride
                                    (surface-r-off screen) (surface-g-off screen)
                                    (surface-b-off screen) '())))
             (lz (if shard-cfg (scfg-lz shard-cfg)
                     (make-surface (make-bytes plane) sw sh stride)))
             (mrg (make-mrg lc lz (make-bytes plane)
                            (make-vector 1 (if shard-cfg (scfg-zbase shard-cfg) 0))
                            (make-vector 1 '())
                            (if shard-cfg (scfg-owner shard-cfg) #f)
                            (make-vector 1 '()))))
      ;; Phase 4: paint the desktop background and push the whole screen once, so the
      ;; display shows the compositor's backdrop immediately (before any client) and
      ;; every later op only needs to flush its own damage rect.
      (clear screen bg)
      (let ((p (caps-present caps)))
        (if p (p (list (list 0 0 (surface-width screen) (surface-height screen))))))
      (serve (cons 1 '())                       ; (next-id . surfaces)
        (lambda (st m)
          (cond
            ((not (pair? m)) st)
            ;; (connect transparency? reply) -> establish the client. `reply` must be a
            ;; context: it is the client's authenticated identity and the address every
            ;; later reply goes to, so a non-context (which would abort the loop on the
            ;; `send`) is rejected. ROUTING (phase 7): the OWNER sends an OPAQUE client
            ;; to a shard (round-robin) so its windows composite on that shard's core;
            ;; the shard spawns ITS handler and replies to the client directly. A
            ;; TRANSLUCENT client (alpha must be composed centrally) and any client when
            ;; there are no shards stay LOCAL -- spawn a handler here, reply `(connected
            ;; handler fmt)`. A shard instance has no shards of its own, so it always
            ;; hosts locally; thus the same code routes for the owner and hosts for a
            ;; shard. The shard's reply carries ITS fmt (identical -- same scanout).
            ((and (eq? (car m) 'connect) (len>= m 3) (ctx? (caddr m)))
             (let ((shards (vector-ref (mrg-shards mrg) 0)))
               (if (and (not (cadr m)) (pair? shards))
                   (let* ((n (length shards))
                          (i (modulo (vector-ref rr 0) n))
                          (sh (shard-handle (list-ref shards i))))
                     (vector-set! rr 0 (+ (vector-ref rr 0) 1))
                     (send sh (list 'connect (cadr m) (caddr m))))
                   (let ((h (make-handler (self) (caddr m) (cadr m))))
                     (send (caddr m) (list 'connected h fmt)))))
             st)
            ;; (op client transparency? msg) -> a relayed surface op (incl. the
            ;; test-only probe-pixel; see handle-op for why it goes via the handler).
            ;; Sent only by a (trusted) handler, but length-guarded all the same.
            ((and (eq? (car m) 'op) (len>= m 4))
             (handle-op st (cadr m) (caddr m) (cadddr m) screen bg caps drag-state mrg))
            ;; (input ev) -> a coreinput event (the compositor is subscribed to the
            ;; input service in init); route it by focus/hit-test. ev is validated
            ;; inside handle-input, so a malformed event can't crash the root.
            ((and (eq? (car m) 'input) (pair? (cdr m)))
             (handle-input st (cadr m) screen bg caps drag-state mrg))
            ;; (alloc-z reply) -> the owner is the global z authority; hand a shard a
            ;; fresh top z-stamp so its windows order correctly against everyone's.
            ((and (eq? (car m) 'alloc-z) (len>= m 2) (ctx? (cadr m)))
             (send (cadr m) (list 'z (fresh-z mrg)))
             st)
            ;; (shard-geom reply) -> a per-core shard asks the screen geometry so it
            ;; can allocate a layer matching the scanout (same stride/np). ctx?-guard
            ;; the reply (an unguarded send would abort the loop). Cross-core safe.
            ((and (eq? (car m) 'shard-geom) (len>= m 2) (ctx? (cadr m)))
             (send (cadr m) (list 'geom (surface-width screen) (surface-height screen)
                                  (surface-stride screen) fmt))
             st)
            ;; (register-shard shard-handle colour-grant z-grant) -> record the shard:
            ;; its primary handle (the owner routes opaque clients to it) and the owner's
            ;; mapped views of its grant-shared layer planes (folded into the merge). The
            ;; grants come from another core and are message data, so BOTH must be real
            ;; grants (grant?) before map-grant -- map-grant errors on a non-grant, which
            ;; would kill this try/catch-less serve loop. A live grant maps to a view; a
            ;; revoked one maps to #f -> skip it. No recomposite here: the shard sends
            ;; `layer-update` right after, which merges+flushes once.
            ((and (eq? (car m) 'register-shard) (len>= m 4) (caps-map caps)
                  (ctx? (cadr m)) (grant? (caddr m)) (grant? (cadddr m)))
             (let ((cv ((caps-map caps) (caddr m))) (zv ((caps-map caps) (cadddr m))))
               (if (and cv zv)
                   (vector-set! (mrg-shards mrg) 0
                                (cons (list (cadr m) cv zv) (vector-ref (mrg-shards mrg) 0)))
                   ;; a grant mapped to #f (revoked between the grant? check and here):
                   ;; the shard is NOT registered (its content/clients won't appear).
                   (begin (display "[corecompositor] register-shard: grant map failed; shard dropped")
                          (newline))))
             st)
            ;; (layer-update shard-handle focus-client focus-z) -> a registered shard
            ;; repainted its layer: record its focus candidate (for cross-shard
            ;; keyboard routing), then re-merge and flush. v1 flushes the whole screen
            ;; (bounding to the shard's damage is a later refinement). Sent cross-core.
            ;; v1 trust: every holder of `comp` is a boot-time-trusted context, so the
            ;; sender is not authenticated here (a future widely-shared `comp` would
            ;; check the sender against the registered shards before recompositing).
            ((eq? (car m) 'layer-update)
             ;; integer? on the z: global-focus-client does (> z bz), so a non-integer
             ;; z from a malformed layer-update would kill this try/catch-less loop.
             (if (and (len>= m 4) (ctx? (cadr m)) (integer? (cadddr m)))
                 (update-focus mrg (cadr m) (caddr m) (cadddr m)))
             (recomposite screen bg (cdr st) mrg)
             (let ((p (caps-present caps)))
               (if p (p (list (list 0 0 (surface-width screen) (surface-height screen))))))
             st)
            (else
             (display "[corecompositor] primary: ignoring malformed ")
             (display (car m)) (newline)
             st))))))))
