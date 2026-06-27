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
  (export start-compositor-service make-compositor-caps paint-windows make-shard-cfg)
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
  ;; key -> the SHARD-MESH CAPABILITY: an unforgeable token shared by the owner and
  ;;   every shard (init injects the SAME value into both), and given to NO client.
  ;;   The privileged inter-instance verbs (register-shard / layer-update / alloc-z /
  ;;   move-window / z) all live on the owner's primary mailbox, which a semi-trusted
  ;;   CLIENT also holds (it sends `connect` there) -- so without this any client
  ;;   could forge a shard registration, a window manifest (stealing input focus), or
  ;;   a move of another client's window. Each such verb carries `key` as its first
  ;;   field and the receiver checks `(eq? key (caps-key caps))`; a client cannot
  ;;   supply it. init uses the compositor rendezvous ctx as the token -- it is
  ;;   root-created, handed only to mesh members, and a ctx handle passes by IDENTITY
  ;;   (un-fabricable by a restricted context), so possessing it proves membership.
  ;;   #f in the host harness (no shard mesh; the keyed verbs are never sent there).
  (define (make-compositor-caps alloc mint revoke present map key)
    (vector alloc mint revoke present map key))
  (define (caps-alloc c)   (vector-ref c 0))
  (define (caps-mint c)    (vector-ref c 1))
  (define (caps-revoke c)  (vector-ref c 2))
  (define (caps-present c) (vector-ref c 3))
  (define (caps-map c)     (vector-ref c 4))
  (define (caps-key c)     (vector-ref c 5))
  ;; Push `rects` to the real display via the injected present cap -- a no-op when
  ;; the cap is #f (a RAM/headless screen, or a shard whose OWNER does the flush).
  (define (present-rects caps rects)
    (let ((p (caps-present caps))) (if p (p rects))))
  ;; The whole-screen rect list: the flush fallback (the startup backdrop, or a
  ;; shard layer-update that carried no usable damage rects).
  (define (full-screen-rect screen)
    (list (list 0 0 (surface-width screen) (surface-height screen))))

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
  ;; `mrg` is itself the mutable vector -- the four fields that change over the
  ;; instance's life (zctr/shards/windows/prev-rects) are mutated in place with
  ;; vector-set! on `mrg`, so they are plain slots, not nested 1-slot cells.
  (define MRG-LC 0) (define MRG-LZ 1) (define MRG-ZOP 2) (define MRG-ZCTR 3)
  (define MRG-SHARDS 4) (define MRG-OWNER 5) (define MRG-WINDOWS 6)
  (define MRG-PREV-RECTS 7) (define MRG-KEY 8)
  (define (make-mrg lc lz zop zctr shards owner windows prev-rects key)
    (vector lc lz zop zctr shards owner windows prev-rects key))
  (define (mrg-lc m)     (vector-ref m MRG-LC))   ; layer colour plane (a surface)
  (define (mrg-lz m)     (vector-ref m MRG-LZ))   ; layer z plane (a surface; fill-rect stamps z)
  (define (mrg-zop m)    (vector-ref m MRG-ZOP))  ; the merged per-pixel topmost-opaque z (raw bytes)
  (define (mrg-zctr m)   (vector-ref m MRG-ZCTR)) ; monotonic z counter (an integer)
  (define (mrg-owner m)  (vector-ref m MRG-OWNER)); owner handle (shard) or #f (owner)
  ;; The shard-mesh capability token (= caps-key); stamped on every privileged
  ;; inter-instance message this instance SENDS and checked on every one it receives
  ;; (see `keyed?` / make-compositor-caps). Shared by the owner and all shards; never
  ;; a client's. In the OS the key is always the rendezvous ctx (truthy + unforgeable).
  (define (mrg-key m)    (vector-ref m MRG-KEY))
  ;; A SHARD's previous-frame visible window rects (list of (x y w h)). Its
  ;; recomposite reports DAMAGE = the union of these (the old positions) and the
  ;; current rects (the new positions) with each layer-update, so the owner can flush
  ;; only the changed area -- bounding the per-frame scanout push -- instead of the
  ;; whole screen, and a moved/closed window's vacated area is still repainted. (The
  ;; merge into the cached back-buffer stays whole-screen; the FLUSH is what is bounded.)
  (define (mrg-prev-rects m) (vector-ref m MRG-PREV-RECTS))
  (define (mrg-prev-rects-set! m v) (vector-set! m MRG-PREV-RECTS v))
  ;; Cross-shard input (phase 7): the per-shard WINDOW MANIFEST the owner uses to
  ;; route input to a window hosted on another core -- an alist (shard-handle .
  ;; window-list) where each window is (client x y w h z id). Each shard reports its
  ;; visible windows with every layer-update; the owner builds one GLOBAL window list
  ;; (global-windows) from this + its own surface table and routes keyboard to the
  ;; max-z window's client and pointer to the window under the cursor -- the owner is
  ;; the input/focus authority, as for z. Owner instance only.
  (define (mrg-windows m) (vector-ref m MRG-WINDOWS))
  (define (mrg-windows-set! m v) (vector-set! m MRG-WINDOWS v))
  ;; Phase 7 cross-core: the list of REMOTE shard layers this owner merges in. Each
  ;; entry is (handle colour-view z-view) -- the owner's mapped views of a per-core
  ;; shard's grant-shared (colour,z) layer planes (same stride/np as the scanout). The
  ;; opaque merge folds the z-pick over the owner's own layer plus every shard layer;
  ;; an empty list is the single-instance (N=1) case (SMP=1 / no-AP boot), so the
  ;; owner-only path is unchanged.
  (define (mrg-shards m) (vector-ref m MRG-SHARDS))
  (define (mrg-shards-set! m v) (vector-set! m MRG-SHARDS v))
  ;; A shard record is (handle colour-view z-view): the shard's primary handle (the
  ;; owner routes opaque clients to it) and the owner's mapped views of the shard's
  ;; grant-shared (colour,z) layer planes (the merge folds them in).
  (define (shard-handle s) (car s))
  (define (shard-color s)  (cadr s))
  (define (shard-z s)      (caddr s))
  ;; The z authority: a fresh top z-stamp. Real z starts at 1 (z==0 = empty layer px).
  ;; The OWNER holds the single global counter; ALL windows (its own + every shard's)
  ;; draw from it, so z is globally comparable -- true global stacking, not static
  ;; per-shard bands.
  (define (fresh-z mrg)
    (vector-set! mrg MRG-ZCTR (+ (mrg-zctr mrg) 1))
    (mrg-zctr mrg))
  ;; The fail-closed shard-mesh guard every privileged inter-instance verb shares:
  ;; true iff message `m` carries this instance's (truthy) key as its first arg.
  ;; FAIL-CLOSED: a #f key (the host harness, no mesh) makes this #f for ALL keyed
  ;; verbs, so a forged `#f`-keyed message is rejected rather than matched by
  ;; `(eq? #f #f)`. Callers pre-check arity (len>=), so (cadr m) is safe here.
  (define (keyed? m mrg)
    (and (mrg-key mrg) (eq? (cadr m) (mrg-key mrg))))
  ;; Assign a window its GLOBAL z on create/raise. The OWNER stamps it synchronously
  ;; (it owns the counter). A SHARD can't request z synchronously without the
  ;; single-mailbox demux problem, so it asks ASYNC: it stamps a temporary local z if
  ;; the window has none yet (so a new window renders immediately) -- a raise keeps
  ;; its current z so it doesn't drop -- then sends (alloc-z self surf-id); the owner
  ;; replies (z N surf-id), which the shard applies by id and re-merges. So a shard
  ;; window briefly holds a local z, then snaps to its real global z (a round-trip).
  (define (stamp-z r mrg)
    (if (mrg-owner mrg)
        (begin
          (if (= (sf r SF-Z) 0) (vector-set! r SF-Z (fresh-z mrg)))
          (send (mrg-owner mrg) (list 'alloc-z (mrg-key mrg) (self) (sf r SF-ID))))
        (vector-set! r SF-Z (fresh-z mrg))))

  ;; A SHARD config passed to start-compositor-service to put it in shard role: the
  ;; owner handle, the grant-shared layer planes (colour + z surfaces) the shard
  ;; composites its clients into, and a z-base that seeds the shard's LOCAL temporary
  ;; z counter (now 0 -- the real z is the owner's global counter, requested via
  ;; alloc-z; see stamp-z). #f for the owner instance (it allocates its own scratch
  ;; layer).
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
      ;; reports its WINDOW MANIFEST -- its visible windows' (client x y w h z id) -- so
      ;; the owner can hit-test/focus/route input to a window hosted on this core
      ;; (`(self)` is this shard's serve handle = the one it registered).
      (if (mrg-owner mrg)
          ;; report the window MANIFEST + the DAMAGE rects (this frame's visible rects
          ;; unioned with last frame's, so the owner flushes only what changed -- incl.
          ;; the vacated area of a moved/closed window). Update prev-rects for next time.
          (let* ((vis (filter (lambda (r) (sf r SF-VIS)) surfaces))
                 (cur (map surf-rect vis))
                 (damage (append (mrg-prev-rects mrg) cur)))
            (mrg-prev-rects-set! mrg cur)
            (send (mrg-owner mrg)
                  (list 'layer-update (mrg-key mrg) (self)
                        (map (lambda (r) (list (sf r SF-CLIENT) (sf r SF-X) (sf r SF-Y)
                                               (sf r SF-W) (sf r SF-H) (sf r SF-Z) (sf r SF-ID)))
                             vis)
                        damage)))
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
                (mrg-shards mrg))
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
  ;; (Single-instance focus/hit-test were folded into the GLOBAL manifest helpers
  ;; below -- global-focus-client / window-at over global-windows -- which subsume
  ;; the N=1 case, so the old per-table `focused`/`surf-at` were removed.)

  ;; --- cross-shard input: the global window manifest (phase 7) ----------------
  ;; A global window ENTRY is (client x y w h z surf shard id): surf is the owner
  ;; SURFACE record for an owner-hosted window (so a press can drag it locally), or
  ;; #f for a shard-hosted one -- in which case `shard` is the hosting shard's handle
  ;; and `id` its surface id there, so the owner can RELAY a drag-move to it. Accessors:
  (define (we-client e) (nth e 0)) (define (we-x e) (nth e 1)) (define (we-y e) (nth e 2))
  (define (we-w e) (nth e 3))      (define (we-h e) (nth e 4)) (define (we-z e) (nth e 5))
  (define (we-surf e)   (nth e 6)) (define (we-shard e) (nth e 7)) (define (we-id e) (nth e 8))
  ;; Record a shard's reported window manifest (its visible windows, each
  ;; (client x y w h z id)): replace this shard's prior entry. The list is cross-core
  ;; MESSAGE DATA the owner later does arithmetic on (hit-test), and the serve loop
  ;; has no try/catch, so SANITIZE it on the way in: walk safely (pair? before cdr),
  ;; keep only well-formed entries (a ctx client + integer geometry), drop the rest.
  ;; Owner only.
  (define (sanitize-wins wins acc)
    (cond ((not (pair? wins)) (reverse acc))
          ((let ((w (car wins)))
             (and (len>= w 7) (ctx? (nth w 0))
                  (integer? (nth w 1)) (integer? (nth w 2)) (integer? (nth w 3))
                  (integer? (nth w 4))
                  ;; z must be a POSITIVE integer: z==0 is "empty" and would also beat
                  ;; top-window's -1 sentinel, letting a z=0 entry steal focus/hit.
                  (integer? (nth w 5)) (> (nth w 5) 0)
                  (integer? (nth w 6))))   ; the shard's window id (for move relay)
           (sanitize-wins (cdr wins) (cons (car wins) acc)))
          (else (sanitize-wins (cdr wins) acc))))
  (define (update-windows mrg shard-handle wins)
    (mrg-windows-set! mrg
      (cons (cons shard-handle (sanitize-wins wins '()))
            (filter (lambda (e) (not (eq? (car e) shard-handle))) (mrg-windows mrg)))))
  ;; The one GLOBAL list of visible window entries: the owner's own surfaces (carry
  ;; their record for local drag) followed by every shard's reported windows (surf
  ;; #f). Input routing/hit-testing/focus all read this, so a window is treated the
  ;; same wherever it is hosted.
  (define (global-windows surfaces mrg)
    (append
      (map (lambda (r) (list (sf r SF-CLIENT) (sf r SF-X) (sf r SF-Y)
                             (sf r SF-W) (sf r SF-H) (sf r SF-Z) r #f (sf r SF-ID)))
           (filter (lambda (r) (sf r SF-VIS)) surfaces))
      (apply append
        (map (lambda (se)               ; se = (shard-handle . window-list)
               (map (lambda (w)         ; w  = (client x y w h z id)
                      (list (nth w 0) (nth w 1) (nth w 2) (nth w 3) (nth w 4) (nth w 5)
                            #f (car se) (nth w 6)))
                    (cdr se)))
             (mrg-windows mrg)))))
  ;; Sanitise a shard's reported DAMAGE rects (cross-core message data the owner
  ;; passes to `present`): keep only well-formed (x y w h) integer rects, clamped to
  ;; the screen, dropping anything malformed or empty. Guards on (pair? rects) before
  ;; (car/cdr rects); an improper tail is treated as end-of-list.
  (define (sane-rects rects sw sh)
    (cond ((not (pair? rects)) '())
          (else
           (let ((r (car rects)) (rest (sane-rects (cdr rects) sw sh)))
             (if (and (len>= r 4) (integer? (nth r 0)) (integer? (nth r 1))
                      (integer? (nth r 2)) (integer? (nth r 3)))
                 (let* ((rx (nth r 0)) (ry (nth r 1))
                        (x  (if (< rx 0) 0 rx)) (y (if (< ry 0) 0 ry))
                        (xe (+ rx (nth r 2))) (ye (+ ry (nth r 3)))
                        (x2 (if (> xe sw) sw xe)) (y2 (if (> ye sh) sh ye)))
                   (if (and (< x x2) (< y y2))
                       (cons (list x y (- x2 x) (- y2 y)) rest)
                       rest))
                 rest)))))
  ;; The max-z entry of `wins` for which (pick? e) holds, or #f. The single hit/focus
  ;; primitive: keyboard passes a const-#t pick; pointer passes a contains-(x,y) pick.
  (define (top-window wins pick?)
    (let loop ((ws wins) (best #f) (bz -1))
      (if (null? ws) best
          (let ((e (car ws)))
            (if (and (pick? e) (> (we-z e) bz))
                (loop (cdr ws) e (we-z e))
                (loop (cdr ws) best bz))))))
  ;; The globally focused window's client (max z over all windows), or #f -- where a
  ;; key routes, whichever core hosts it.
  (define (global-focus-client surfaces mrg)
    (let ((e (top-window (global-windows surfaces mrg) (lambda (e) #t))))
      (if e (we-client e) #f)))
  ;; The top-most visible window under (x,y) across all instances, or #f.
  (define (window-at surfaces mrg x y)
    (top-window (global-windows surfaces mrg)
                (lambda (e) (and (>= x (we-x e)) (< x (+ (we-x e) (we-w e)))
                                 (>= y (we-y e)) (< y (+ (we-y e) (we-h e)))))))

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
      (define (present! rects) (present-rects caps rects))
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
               (stamp-z r mrg)   ; a fresh GLOBAL top z (sync on owner, async on a shard)
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
           (if r (let ((was-vis (sf r SF-VIS)) (old (surf-rect r)))
                   (vector-set! r SF-X (caddr m))
                   (vector-set! r SF-Y (cadddr m))
                   (vector-set! r SF-VIS (nth m 4))
                   (recomposite screen bg surfaces mrg)
                   ;; flush the OLD rect only if the surface was actually showing
                   ;; there before (else it covered nothing -- e.g. a first show,
                   ;; whose pre-configure rect is a spurious (0,0,w,h)), and the NEW
                   ;; rect only if it is visible now. So a first show flushes just the
                   ;; new rect, a move flushes old + new, a hide just the old.
                   (let ((dmg (append (if was-vis (list old) '())
                                      (if (sf r SF-VIS) (list (surf-rect r)) '()))))
                     (if (pair? dmg) (present! dmg))))))
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
               (stamp-z r0 mrg)   ; a fresh GLOBAL top z so the z-buffer lifts it (cross-shard)
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
  ;; policy. `drag-state` is a 1-slot mutable cell holding #f or `(id offx offy shard)`
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
      (define (present! rects) (present-rects caps rects))
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
           ;; A press in a window's title strip begins a drag (owner-local if the
           ;; entry's shard is #f, else relayed to the hosting shard); a body press is
           ;; routed to the client in window-local coords. Read via the `we-*`
           ;; manifest accessors so it works identically for an owner or a shard
           ;; window (an owner entry carries shard=#f and its own client/geometry).
           (define (title-drag-or-route e)
             (if (< (- y (we-y e)) TITLE-H)
                 (vector-set! drag-state 0
                   (list (we-id e) (- x (we-x e)) (- y (we-y e)) (we-shard e)))
                 (send (we-client e)
                       (list 'input (list 'pointer (- x (we-x e)) (- y (we-y e)) #t)))))
           (cond
             ;; an active drag: a button-down event moves the window to follow the
             ;; pointer (recomposite + flush old∪new); a button-up ends the drag. If
             ;; the dragged surface has vanished (destroyed mid-drag), abandon the
             ;; drag -- destroy-surface clears it proactively, but a stale id here
             ;; (any path that drops a surface) must not pin drag-state forever.
             (d
              (if down?
                  ;; d is (id offx offy shard): shard #f -> the window is the owner's,
                  ;; move it locally; otherwise relay the new position to the hosting
                  ;; shard, which repositions it and re-merges (no local move).
                  (let ((shard (cadddr d)))
                    (if shard
                        (send shard (list 'move-window (mrg-key mrg) (car d) (- x (cadr d)) (- y (caddr d))))
                        (let ((r (find-surf (car d) surfaces)))
                          (if r (let ((old (surf-rect r)))
                                  (vector-set! r SF-X (- x (cadr d)))
                                  (vector-set! r SF-Y (- y (caddr d)))
                                  (recomposite screen bg surfaces mrg)
                                  (present! (list old (surf-rect r))))
                              (vector-set! drag-state 0 #f)))))
                  (vector-set! drag-state 0 #f))
              st)
             ;; a fresh press: hit-test the GLOBAL window list (owner + shards). On an
             ;; OWNER window, focus-follows-click (raise) then title-bar = local drag /
             ;; body = route to the client. On a SHARD window, a title-bar press starts
             ;; a CROSS-SHARD drag (the owner relays each move to the hosting shard) and
             ;; a body press routes to the client -- both in window-local coords. No
             ;; window -> ignored. (drag-state is (id offx offy shard): shard #f for an
             ;; owner-local drag, the shard handle for a relayed one.)
             (down?
              (let ((e (window-at surfaces mrg x y)))
                (cond
                  ((not e) st)
                  ((we-surf e)
                   ;; an OWNER window: focus-follows-click raises it (raise-surf
                   ;; re-lists the SAME surface vector, so the z bump is seen by the
                   ;; recomposite over s2), then title-bar drag / body route as usual.
                   (let* ((r (we-surf e))
                          (s2 (raise-surf (sf r SF-ID) surfaces)))
                     (vector-set! r SF-Z (fresh-z mrg))   ; focus-follows-click lifts it
                     (recomposite screen bg s2 mrg)
                     (present! (list (surf-rect r)))
                     (title-drag-or-route e)
                     (cons next-id s2)))
                  (else                       ; a SHARD window: route/relay, no local raise
                   (title-drag-or-route e)
                   st))))
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
          ;; ephemeral title-bar-drag state: #f, or (id offx offy shard) mid-drag. A
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
                            (if shard-cfg (scfg-zbase shard-cfg) 0)   ; zctr
                            '()                                       ; shards
                            (if shard-cfg (scfg-owner shard-cfg) #f)  ; owner
                            '()                                       ; windows
                            '()                                       ; prev-rects
                            (caps-key caps))))
      ;; Phase 4: paint the desktop background and push the whole screen once, so the
      ;; display shows the compositor's backdrop immediately (before any client) and
      ;; every later op only needs to flush its own damage rect.
      (clear screen bg)
      (present-rects caps (full-screen-rect screen))
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
             (let ((shards (mrg-shards mrg)))
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
            ;; (move-window KEY id x y) -> the OWNER relays a cross-shard drag to this
            ;; SHARD: reposition the window and re-merge (recomposite -> layer-update,
            ;; so the owner re-merges + sees the new geometry). Gated TWO ways: the
            ;; mesh KEY must match (a client holding `comp` can't forge it -- see
            ;; caps-key) and only a SHARD acts (`mrg-owner` set), since the owner never
            ;; receives a legitimate move-window (it moves its own windows locally).
            ((and (eq? (car m) 'move-window) (len>= m 5) (keyed? m mrg)
                  (mrg-owner mrg)
                  (integer? (caddr m)) (integer? (cadddr m)) (integer? (nth m 4)))
             (let ((r (find-surf (caddr m) (cdr st))))
               (if r (begin (vector-set! r SF-X (cadddr m))
                            (vector-set! r SF-Y (nth m 4))
                            (recomposite screen bg (cdr st) mrg))))
             st)
            ;; (input ev) -> a coreinput event (the compositor is subscribed to the
            ;; input service in init); route it by focus/hit-test. ev is validated
            ;; inside handle-input, so a malformed event can't crash the root.
            ((and (eq? (car m) 'input) (pair? (cdr m)))
             (handle-input st (cadr m) screen bg caps drag-state mrg))
            ;; (alloc-z KEY reply surf-id) -> the owner is the global z authority; hand a
            ;; shard a fresh global z for window `surf-id`, echoing the id back so the
            ;; shard applies it to the right surface (the request/reply is async, so the
            ;; id is the correlator -- no FIFO assumption, and a since-destroyed window
            ;; is a safe no-op on the shard). KEY-gated: only a mesh member can request
            ;; z, so a client can't pump the counter or make the owner reply to an
            ;; arbitrary ctx.
            ((and (eq? (car m) 'alloc-z) (len>= m 4) (keyed? m mrg)
                  (not (mrg-owner mrg)) (ctx? (caddr m)) (integer? (cadddr m)))
             (send (caddr m) (list 'z (mrg-key mrg) (fresh-z mrg) (cadddr m)))
             st)
            ;; (z KEY N surf-id) -> the owner's reply to a SHARD's alloc-z: set window
            ;; surf-id's global z and re-merge. Gated on (mrg-owner mrg) so ONLY a
            ;; shard ever processes it: otherwise a stray (z N id) sent to the owner
            ;; would find-surf the id over the OWNER's own table and corrupt that
            ;; window's z (ids are small integers shared by both) -- the owner never
            ;; asks for z, so it must ignore the reply, not act on it. Also KEY-gated:
            ;; a client can't forge a z reply to skew a shard window's stacking.
            ((and (eq? (car m) 'z) (len>= m 4) (keyed? m mrg)
                  (mrg-owner mrg) (integer? (caddr m)) (integer? (cadddr m)))
             (let ((r (find-surf (cadddr m) (cdr st))))
               (if r (begin (vector-set! r SF-Z (caddr m))
                            (recomposite screen bg (cdr st) mrg))))
             st)
            ;; (shard-count reply) -> how many shards have registered. A client uses
            ;; this to wait until shards exist before connecting, so an opaque client
            ;; is deterministically ROUTED to a shard rather than racing the per-core
            ;; bring-up; the cross-shard tests rely on it (else they could silently
            ;; run owner-hosted). 0 on the single-instance (no-AP) owner.
            ((and (eq? (car m) 'shard-count) (len>= m 2) (ctx? (cadr m)))
             (send (cadr m) (length (mrg-shards mrg)))
             st)
            ;; (shard-geom reply) -> a per-core shard asks the screen geometry so it
            ;; can allocate a layer matching the scanout (same stride/np). ctx?-guard
            ;; the reply (an unguarded send would abort the loop). Cross-core safe.
            ((and (eq? (car m) 'shard-geom) (len>= m 2) (ctx? (cadr m)))
             (send (cadr m) (list 'geom (surface-width screen) (surface-height screen)
                                  (surface-stride screen) fmt))
             st)
            ;; (register-shard KEY shard-handle colour-grant z-grant) -> record the shard:
            ;; its primary handle (the owner routes opaque clients to it) and the owner's
            ;; mapped views of its grant-shared layer planes (folded into the merge). The
            ;; grants come from another core and are message data, so BOTH must be real
            ;; grants (grant?) before map-grant -- map-grant errors on a non-grant, which
            ;; would kill this try/catch-less serve loop. A live grant maps to a view; a
            ;; revoked one maps to #f -> skip it. No recomposite here: the shard sends
            ;; `layer-update` right after, which merges+flushes once. KEY-gated FIRST:
            ;; this is the highest-value forgery target (a fake shard injects arbitrary
            ;; pixels into the merge), so a client lacking the mesh key is rejected here.
            ((and (eq? (car m) 'register-shard) (len>= m 5) (keyed? m mrg)
                  (not (mrg-owner mrg)) (caps-map caps) (ctx? (caddr m)) (grant? (cadddr m)) (grant? (nth m 4)))
             (let ((cv ((caps-map caps) (cadddr m))) (zv ((caps-map caps) (nth m 4))))
               (if (and cv zv)
                   (mrg-shards-set! mrg
                                (cons (list (caddr m) cv zv) (mrg-shards mrg)))
                   ;; a grant mapped to #f (revoked between the grant? check and here):
                   ;; the shard is NOT registered (its content/clients won't appear).
                   (begin (display "[corecompositor] register-shard: grant map failed; shard dropped")
                          (newline))))
             st)
            ;; (layer-update KEY shard-handle window-list damage) -> a registered shard
            ;; repainted its layer: record its window manifest (for cross-shard input
            ;; routing), re-merge the whole back-buffer (cheap cached RAM), then FLUSH
            ;; only the shard's reported DAMAGE rects to the scanout (bounded; the
            ;; expensive part). With no/empty damage, fall back to a whole-screen flush.
            ;; KEY-gated: a forged layer-update could steal input focus (its manifest
            ;; drives keyboard routing) or drive an arbitrary flush, so a sender lacking
            ;; the mesh key is rejected -- only a real shard reaches the merge here. The
            ;; full 5 fields (key + ctx shard-handle + manifest + damage) are required:
            ;; a real shard always sends all of them, so a short message is a protocol
            ;; error and falls through (no spurious recomposite/flush) rather than being
            ;; half-processed. update-windows sanitises the manifest internally.
            ((and (eq? (car m) 'layer-update) (len>= m 5) (keyed? m mrg)
                  (not (mrg-owner mrg)) (ctx? (caddr m)))
             (update-windows mrg (caddr m) (cadddr m))
             (recomposite screen bg (cdr st) mrg)
             (let ((dmg (sane-rects (nth m 4) (surface-width screen) (surface-height screen))))
               (present-rects caps (if (pair? dmg) dmg (full-screen-rect screen))))
             st)
            (else
             (display "[corecompositor] primary: ignoring malformed ")
             (display (car m)) (newline)
             st))))))))
