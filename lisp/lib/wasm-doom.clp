;; Copyright (c) 2026 Himanshu Goel
;;
;; This software is released under the MIT License.
;; https://opensource.org/licenses/MIT

;; wasm-doom: the host for the Doom WASM guest (Phase 5 of
;; notes/core/wasm-guests.md). It runs guests/doom/doom.wasm -- doomgeneric
;; compiled to wasm32-wasi -- on the in-OS interpreter, presenting through the
;; window compositor and feeding it input the compositor routes to the focused
;; window.
;;
;; The guest reaches the OS through two import modules, both host-serviced here:
;;   wasi_snapshot_preview1  -- console + a read-only FS (the WAD), via wasm-host.
;;   cardinal                -- the doomgeneric backend (guests/doom/
;;                              doomgeneric_cardinal.c):
;;       present(buf,w,h)  copy the w*h 32bpp frame at linear-memory offset `buf`
;;                         into a compositor surface and commit it.
;;       poll_key()        next input event, or -1: (pressed<<8)|ps2_scancode.
;;       ticks_ms()        monotonic milliseconds since the guest started.
;;       sleep_ms(ms)      yield to the host for ~ms.
;;
;; Doom's frame is XRGB8888 (0x00RRGGBB), the same byte layout the boot
;; framebuffer (and thus the compositor surface, when scanning out the std-vga
;; LFB) uses -- channel offsets (16,8,0). In that case present is a single bulk
;; bytes-copy! of the whole frame. A surface with different offsets (e.g.
;; virtio-gpu's (8,16,24)) takes the per-pixel repack path, which is far slower
;; in Lisp; run the Doom demo on the LFB path for full speed.

(define-module wasm-doom
  (export run-doom doom-cardinal-imports)
  (import sys-wasm sys-shm sys-console wasm-host)

  (define DOOM-W 640)
  (define DOOM-H 400)

  ;; cardinal import ids (>100 so they never collide with wasm-host's WASI ids).
  (define CARD-PRESENT  101)
  (define CARD-POLL-KEY 102)
  (define CARD-TICKS-MS 103)
  (define CARD-SLEEP-MS 104)

  (define doom-cardinal-imports
    (list (list "cardinal" "present"  CARD-PRESENT)
          (list "cardinal" "poll_key" CARD-POLL-KEY)
          (list "cardinal" "ticks_ms" CARD-TICKS-MS)
          (list "cardinal" "sleep_ms" CARD-SLEEP-MS)))

  ;; Per-pixel repack for a surface whose channel offsets differ from Doom's
  ;; 0x00RRGGBB. Slow (a Lisp iteration per pixel); only taken off the fast path.
  (define (repack-frame dst mem src-off npx ro go bo)
    (let loop ((i 0))
      (if (< i npx)
          (let* ((p (bytes-u32-ref mem (+ src-off (* i 4))))
                 (r (bitwise-and (arithmetic-shift p -16) 255))
                 (g (bitwise-and (arithmetic-shift p -8) 255))
                 (b (bitwise-and p 255)))
            (bytes-u32-set! dst (* i 4)
                            (bitwise-or (arithmetic-shift r ro)
                                        (arithmetic-shift g go)
                                        (arithmetic-shift b bo)))
            (loop (+ i 1))))))

  ;; Run the Doom guest to (effectively) forever. `rendezvous` is the compositor
  ;; owner rendezvous (init's compositor-rendezvous); `wad-bytes` is the IWAD
  ;; (a bytes object, e.g. from initrd-file); `guest-bytes` is doom.wasm.
  ;; argv is the guest's command line, e.g. ("doom" "-iwad" "/doom1.wad").
  (define (run-doom rendezvous wad-bytes guest-bytes argv)
    (send rendezvous (list 'get-owner (self)))
    (let ((or-reply (recv)))                       ; (owner comp)
      (if (not (and (pair? or-reply) (eq? (car or-reply) 'owner)))
          (log "doom" "FAIL no compositor owner")
          (let ((comp (cadr or-reply)))
            (send comp (list 'connect #f (self)))
            (let ((r (recv)))                      ; (connected handler fmt)
              (if (not (and (pair? r) (eq? (car r) 'connected)))
                  (log "doom" "FAIL no connected reply")
                  (let* ((handler (cadr r)) (fmt (caddr r))
                         (ro (car fmt)) (go (cadr fmt)) (bo (caddr fmt))
                         (fast? (and (= ro 16) (= go 8) (= bo 0))))
                    (send handler (list 'create-surface DOOM-W DOOM-H))
                    (let ((s (recv)))              ; (surface id g0 g1 stride)
                      (let* ((id   (cadr s))
                             (buf0 (map-grant (caddr s)))
                             (buf1 (map-grant (cadddr s)))
                             (frame (make-vector 1 0))       ; back-buffer index (mutable)
                             (t0   (quotient (uptime-ns) 1000000))
                             (wasi (make-wasi-state
                                    (list (cons "doom1.wad" wad-bytes)) argv))
                             (inst (wasm-instantiate
                                    guest-bytes
                                    (append wasi-import-list doom-cardinal-imports))))
                        ;; ---- cardinal import handlers ----
                        (define (do-present args)
                          (let* ((src-off (car args)) (w (cadr args)) (h (caddr args))
                                 (mem (wasm-mem inst))
                                 (idx (vector-ref frame 0))
                                 (dst (if (= idx 0) buf0 buf1)))
                            (if fast?
                                (bytes-copy! dst 0 mem src-off (* w h 4))
                                (repack-frame dst mem src-off (* w h) ro go bo))
                            (send handler (list 'commit id idx '()))
                            (vector-set! frame 0 (if (= idx 0) 1 0))
                            (list)))             ; void

                        ;; Return one queued input event, or -1 if none. The
                        ;; compositor routes the focused window's key events here
                        ;; as (input (key scancode pressed)); skip anything else.
                        (define (do-poll-key)
                          (let loop ()
                            (if (%mailbox-empty?)
                                (list -1)
                                (let ((m (%mailbox-pop)))
                                  (if (and (pair? m) (eq? (car m) 'input)
                                           (pair? (cadr m)) (eq? (car (cadr m)) 'key))
                                      (let ((ev (cadr m)))   ; (key scancode pressed)
                                        (list (bitwise-or
                                               (arithmetic-shift
                                                (if (= (caddr ev) 0) 0 1) 8)
                                               (bitwise-and (cadr ev) #xFF))))
                                      (loop))))))

                        (define (do-ticks)
                          (list (- (quotient (uptime-ns) 1000000) t0)))

                        (define (do-sleep args)
                          (let ((ms (car args)))
                            (if (> ms 0) (sleep (* ms 1000000)))
                            (list)))             ; void

                        (define (dispatch id args)
                          (cond ((= id CARD-PRESENT)  (do-present args))
                                ((= id CARD-POLL-KEY) (do-poll-key))
                                ((= id CARD-TICKS-MS) (do-ticks))
                                ((= id CARD-SLEEP-MS) (do-sleep args))
                                (else (wasi-dispatch wasi inst id args))))

                        (log "doom" (if fast? "fast blit path (XRGB match)"
                                        "SLOW per-pixel repack (surface fmt != XRGB)"))
                        ;; place + show the window once; frames just re-commit.
                        (send handler (list 'configure id 80 60 #t))
                        (wasm-call inst "_start" (list))
                        (let ((status (run-guest inst dispatch)))
                          (log "doom" "guest exited: " status)
                          (wasm-destroy inst))))))))))))
