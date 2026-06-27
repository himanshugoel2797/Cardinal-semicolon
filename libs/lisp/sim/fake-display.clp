;; fake-display: a host-backed display driver for the simulator.
;;
;; Stands in for lfb / virtio-gpu when the servers run on the host. A real driver
;; mmio-maps a scanout and the hardware scans it out; here the "scanout" is an
;; X11 window (or a PPM file) behind the host-present! prim. The driver:
;;
;;   * registers with coredisplay exactly as a real driver does -- so the real
;;     registry server is exercised unchanged;
;;   * answers (get-info reply) with the geometry, the discovery handshake an app
;;     uses to size its surface;
;;   * answers (present <bytes> [reply]) by pushing that frame to the host window.
;;
;; Because IPC deep-copies the message, the app composes into its OWN surface and
;; SENDS the finished frame here to be presented (a framebuffer-protocol model,
;; like a remote display) rather than sharing a mapped scanout -- the host has no
;; shared physical memory to map. host-present! / host-screen-size are the only
;; host prims; everything else is ordinary Lisp, so this file needs no caps.

(define-module fake-display
  (export start-fake-display)
  (import driver-util)

  ;; Spawn the driver. `coredisplay` is the registry handle; `w`/`h` the screen
  ;; size. Returns the driver handle an app sends frames to.
  (define (start-fake-display coredisplay w h)
    (let ((stride (* w 4)))
      (let ((svc
             (serve '()
               (lambda (st m)
                 (cond ((not (pair? m)) st)
                       ;; (present <bytes> [reply]) -> push the frame to the host.
                       ((eq? (car m) 'present)
                        (if (pair? (cdr m))
                            (host-present! (cadr m) w h stride))
                        (if (and (pair? (cdr m)) (pair? (cddr m)) (ctx? (caddr m)))
                            (send (caddr m) 'flushed))
                        st)
                       ;; (get-info <reply>) -> (w h stride): the geometry handshake.
                       ((eq? (car m) 'get-info)
                        (if (and (pair? (cdr m)) (ctx? (cadr m)))
                            (send (cadr m) (list w h stride)))
                        st)
                       (else st))))))
        (send coredisplay
              (list 'register "Fake Display" 'host svc (list w h stride 32)))
        svc))))
