;; virtio-gpu/driver: the long-lived driver context + the entry point.
;;
;; NOT a module -- spliced into virtio-gpu by `include`. virtio-gpu-init spawns
;; the bring-up context (closing over the display-service handle) and returns
;; immediately, mirroring virtio-net-init. The spawned context runs the bring-up
;; (which yields on wait-until/sleep), registers with coredisplay, then parks in a
;; recv loop answering display requests serially -- the controlq is single-
;; command-in-flight, so a server-style serial loop is the right shape.

;; scanout-record accessors: (idx res-id w h fb).
(define (sc-idx s)    (nth s 0))
(define (sc-res-id s) (nth s 1))
(define (sc-w s)      (nth s 2))
(define (sc-h s)      (nth s 3))
(define (sc-fb s)     (nth s 4))

;; Re-push scanout 0 to the host (transfer its backing + flush). Used by (flush).
(define (gpu-flush-scanout0 ctrlq notify mult scanouts)
  (if (not (null? scanouts))
      (let ((s (car scanouts)))
        (gpu-cmd! ctrlq notify mult
                  (make-transfer-2d (sc-res-id s) 0 0 0 (sc-w s) (sc-h s)) 56)
        (gpu-cmd! ctrlq notify mult
                  (make-flush (sc-res-id s) 0 0 (sc-w s) (sc-h s)) 48))))

;; The driver recv loop. Messages:
;;   (flush)        -- re-transfer + flush scanout 0 to the host (fire-and-forget)
;;   (flush reply)  -- same, then ack `reply` once the controlq round-trip completes
;;                     (a synchronous flush: the caller learns when the frame is up)
;;   (get-framebuffer reply) -- send the caller (w h phys) for scanout 0. NOT the fb
;;                     bytes: those are copy-on-send (a 4 MB copy whose writes never
;;                     reach the device), so a consumer maps the backing PHYS itself
;;                     (mmio-map / mmio-map-wb) in its own context and draws there.
;;   (display-info) -- stub for the future resize-event path
(define (gpu-driver-loop ctrlq notify mult scanouts)
  (let loop ()
    (let ((m (recv)))
      (cond
        ((eq? (car m) 'flush)
         (gpu-flush-scanout0 ctrlq notify mult scanouts)
         (if (pair? (cdr m)) (send (cadr m) 'flushed)))   ; optional completion ack
        ((eq? (car m) 'get-framebuffer)
         (if (not (null? scanouts))
             (let ((s (car scanouts)))
               (send (cadr m) (list (sc-w s) (sc-h s) (bytes-phys (sc-fb s)))))
             (send (cadr m) #f)))
        ((eq? (car m) 'display-info)
         'todo)                          ; resize path: re-read GET_DISPLAY_INFO
        (else 'ignored))
      (loop))))

;; The entry point. `display` is the coredisplay service handle. Spawn a context
;; that brings the device up and, if a scanout came up, registers with coredisplay
;; and runs the driver loop. coredisplay's `register` handler stores (cdr m)
;; verbatim, so we hand it (register name conn ctx info) and no coredisplay change
;; is needed. Returns immediately (the spawned handle).
(define (virtio-gpu-init display-svc)
  (spawn-restricted '()
    (lambda ()
      (let ((dev (gpu-bringup)))
        (if (not dev)
            (begin (display "[virtio-gpu] bring-up failed; not registering") (newline) #f)
            (let ((ctrlq (nth dev 0)) (notify (nth dev 1))
                  (mult (nth dev 2)) (scanouts (nth dev 3)))
              (if (null? scanouts)
                  (begin (display "[virtio-gpu] no enabled scanout; not registering") (newline) #f)
                  (begin
                    ;; scanout-info carries the geometry + framebuffer for each
                    ;; scanout; coredisplay stores it verbatim for consumers.
                    (send display-svc (list 'register "Virtio GPU Display" 'unknown
                                            (self) scanouts))
                    (display "[virtio-gpu] registered with display service") (newline)
                    (gpu-driver-loop ctrlq notify mult scanouts)))))))))
