;; usb-audio: USB Audio Class (UAC1) playback, the class handler for an Audio
;; interface (bInterfaceClass == 1). On (probe dev) it finds an AudioStreaming
;; interface alternate setting that exposes an isochronous OUT endpoint, activates
;; that alt, sets the sampling rate, registers a card with coreaudio, and serves
;; tone/play requests by synthesizing PCM and streaming it over the iso OUT
;; endpoint (the iso transfer primitive added to the controllers).
;;
;; SCOPE (matches the hdaudio path and the iso engine's simplifications): playback
;; only, 48 kHz stereo 16-bit, square-wave tones. The PCM synthesis mirrors
;; hdaudio's fill-tone! exactly so the two cards sound identical. A play request
;; streams the whole tone chunk-by-chunk over the iso OUT endpoint, which paces one
;; packet per (micro)frame; the small gap between chunks is a benign under-run
;; (inaudible / invisible on QEMU's sink). Volume/mute (a UAC Feature Unit) is not
;; wired yet -- those messages are accepted and ignored so a client never wedges.
;;
;; STOP HANDLING. The card runs in one CONTEXT that both streams transfers and
;; receives coreaudio requests + the (stop) sent on hot-remove. Like usb-hid, its
;; completion wait is STOP-AWARE: a (stop) arriving mid-transfer sets a flag (rather
;; than being dropped by proto's await-complete) so a long tone aborts promptly and
;; the context exits cleanly instead of leaking. All of the card's USB transfers go
;; through that local wait, never proto's usb-isoch-out/usb-control-out wrappers.
(define-module usb-audio
  (export usb-audio-init)
  (import coreusb driver-util)

  ;; The format we synthesize + advertise (QEMU usb-audio's default; the universal
  ;; PC audio format). FRAME-SZ = 2 channels * 2 bytes.
  (define SAMPLE-RATE 48000)
  (define FRAME-SZ    4)
  (define TONE-HZ     500)
  (define TONE-FRAMES 4800)     ; 0.1 s, matching hdaudio's bring-up tone
  (define TONE-AMP    8000)     ; ~1/4 full scale

  ;; USB Audio class constants.
  (define AUDIO-SUBCLASS-STREAMING 2)
  (define UAC-SET-CUR              #x01)
  (define UAC-SAMPLING-FREQ-CONTROL #x01)

  ;; A target submission is ~1.5 KiB (an UPPER bound, under both controllers' iso
  ;; caps), split into whole packets: bytes/chunk = packets-per-chunk * mps.
  (define CHUNK-BYTES-TARGET 1536)

  ;; Find an AudioStreaming interface alt that has an isochronous OUT endpoint.
  ;; Returns (interface-number alt endpoint-address max-packet) or #f. alt 0 of a
  ;; streaming interface is the zero-bandwidth setting (no endpoints); the active
  ;; alt carries the iso endpoint -- that's the one we want.
  (define (find-iso-out dev)
    (let loop ((ifs (usb-interfaces dev)))
      (if (null? ifs) #f
          (let ((i (car ifs)))
            (if (= (iface-subclass i) AUDIO-SUBCLASS-STREAMING)
                (let ((ep (usb-find-ep-in
                            (usb-iface-endpoints dev (iface-number i) (iface-alt i))
                            USB-XFER-ISOCH #f)))
                  (if ep
                      (list (iface-number i) (iface-alt i)
                            (ep-address ep)
                            (let ((m (ep-max-packet ep))) (if (> m 0) m 192)))
                      (loop (cdr ifs))))
                (loop (cdr ifs)))))))

  ;; The card's endpoint descriptor for coreaudio: one output endpoint (the
  ;; (id dir dev present) plain-data shape hdaudio uses). Always present.
  (define (audio-endpoint-descs) (list (list 0 'out 'speaker #t)))

  ;; Spawn the card context: activate the streaming alt, set the rate, register with
  ;; coreaudio, play a bring-up tone (proving the iso path end-to-end), then serve.
  ;; All USB transfers ride the local stop-aware `await`, so a hot-remove mid-stream
  ;; aborts the tone and exits the loop instead of orphaning the context.
  (define (start-audio-card dev audio iface alt ep-addr mps name)
    (spawn-restricted '()
      (lambda ()
        (let ((stopped #f))
          ;; Wait for a transfer completion; note a (stop) (don't drop it like
          ;; proto's await-complete would) and keep waiting for the completion of
          ;; the in-flight transfer; drop any other stray message.
          (define (await)
            (let ((m (recv)))
              (cond ((eq? (car m) 'complete) m)
                    ((eq? (car m) 'stop) (set! stopped #t) (await))
                    (else (await)))))
          ;; Iso OUT one buffer, through the stop-aware await.
          (define (isoch-out data len)
            (send (usb-dev-hci dev) (list 'isoch (usb-dev-address dev) (usb-dev-speed dev)
                                          ep-addr mps data len #f (self)))
            (await))
          ;; A no-data class/standard control OUT, through the same await.
          (define (ctl-out bmreq breq wval widx data len)
            (send (usb-dev-hci dev)
                  (list 'control (usb-dev-address dev) (usb-dev-speed dev) (usb-dev-mps0 dev)
                        (make-setup (bitwise-or bmreq USB-REQ-DIR-OUT) breq wval widx len)
                        data len (self)))
            (await))
          (define (set-iface alt*)
            (ctl-out USB-REQ-RECIP-INTERFACE USB-REQ-SET-INTERFACE alt* iface #f 0))
          ;; Best-effort UAC1 SET_CUR(SAMPLING_FREQ) on the endpoint (3-byte rate,
          ;; LE). A device that lacks the control STALLs -- harmless, it keeps its
          ;; default rate (48 kHz on QEMU).
          (define (set-rate rate)
            (let ((d (make-bytes 3)))
              (bytes-u8-set! d 0 (bitwise-and rate #xFF))
              (bytes-u8-set! d 1 (bitwise-and (arithmetic-shift rate -8) #xFF))
              (bytes-u8-set! d 2 (bitwise-and (arithmetic-shift rate -16) #xFF))
              (ctl-out (bitwise-or USB-REQ-TYPE-CLASS USB-REQ-RECIP-ENDPOINT)
                       UAC-SET-CUR (arithmetic-shift UAC-SAMPLING-FREQ-CONTROL 8)
                       ep-addr d 3)))
          ;; Synthesize a square-wave tone and stream it over the iso OUT endpoint.
          ;; Mirrors hdaudio's fill-tone! (+amp/-amp per half period) but fills and
          ;; ships one chunk at a time -- no multi-100 KiB allocation. `frames` is
          ;; samples-per-channel; phase is preserved across chunks via `done`. Aborts
          ;; early if a (stop) set the flag while waiting on a chunk.
          (define (play freq amp frames)
            (let* ((f    (if (> freq 0) freq 1))
                   (half (let ((h (quotient SAMPLE-RATE (* 2 f)))) (if (< h 1) 1 h)))
                   (neg  (- 65536 amp))
                   (ppc  (let ((p (quotient CHUNK-BYTES-TARGET mps))) (if (< p 1) 1 p)))
                   (cfr  (let ((c (quotient (* ppc mps) FRAME-SZ))) (if (< c 1) 1 c))))
              (let loop ((done 0))
                (if (or stopped (>= done frames)) done
                    (let* ((n   (let ((r (- frames done))) (if (> r cfr) cfr r)))
                           (buf (make-bytes (* n FRAME-SZ))))
                      (let fill ((i 0))
                        (if (< i n)
                            (let ((v   (if (= 0 (modulo (quotient (+ done i) half) 2)) amp neg))
                                  (off (* i FRAME-SZ)))
                              (bytes-u16-set! buf off v)              ; left
                              (bytes-u16-set! buf (+ off 2) v)        ; right
                              (fill (+ i 1)))))
                      (isoch-out buf (* n FRAME-SZ))
                      (loop (+ done n)))))))

          ;; --- bring-up ---
          (set-iface alt)
          (set-rate SAMPLE-RATE)
          (send audio (list 'register name (self) (audio-endpoint-descs)))
          (display "[usb-audio] registered ") (display name)
          (display " (iface=") (display iface) (display " alt=") (display alt)
          (display " ep=") (display ep-addr) (display " mps=") (display mps) (display ")") (newline)
          (play TONE-HZ TONE-AMP TONE-FRAMES)
          (display "[usb-audio] bring-up tone done") (newline)

          ;; --- serve coreaudio (the subset a UAC1 speaker supports) ---
          ;; `stopped` is checked at the top of each turn so a (stop) noted during a
          ;; transfer exits promptly; an idle (stop) is handled by its clause.
          (let serve-loop ()
            (if stopped
                (begin (set-iface 0) (display "[usb-audio] stopped") (newline) 'stopped)
                (let ((m (recv)))
                  (cond
                    ((eq? (car m) 'tone) (play TONE-HZ TONE-AMP TONE-FRAMES) (serve-loop))
                    ((eq? (car m) 'play)            ; (play freq amp frames)
                     (let ((freq (nth m 1)) (amp (nth m 2)) (frames (nth m 3)))
                       (if (and (> freq 0) (> amp 0) (< amp 32768) (> frames 0))
                           (play freq amp frames)
                           (begin (display "[usb-audio] play: bad params, ignored") (newline))))
                     (serve-loop))
                    ((eq? (car m) 'endpoints)       ; (endpoints reply)
                     (send (nth m 1) (audio-endpoint-descs)) (serve-loop))
                    ((eq? (car m) 'get-volume)      ; (get-volume ep-id reply) -- no Feature Unit yet
                     (send (nth m 2) #f) (serve-loop))
                    ((eq? (car m) 'set-volume) (serve-loop))  ; accepted + ignored (UAC FU TODO)
                    ((eq? (car m) 'mute)       (serve-loop))
                    ((eq? (car m) 'get-status) (send (nth m 1) 'playing) (serve-loop))
                    ((eq? (car m) 'stop)            ; hot-remove while idle
                     (set-iface 0) (display "[usb-audio] stopped") (newline) 'stopped)
                    (else (serve-loop))))))))))

  ;; A claimed device's name is usbaudioN (N from a monotonic counter, like the
  ;; hda0/hda1 scheme), so two USB audio devices register distinct coreaudio cards.
  (define (audio-on-probe dev audio devs n)
    (let ((found (find-iso-out dev)))
      (if (not found)
          (begin (display "[usb-audio] no iso OUT streaming endpoint; not claiming") (newline) devs)
          (let* ((name (string->symbol (string-append "usbaudio" (number->string n))))
                 (ctx (start-audio-card dev audio (nth found 0) (nth found 1)
                                        (nth found 2) (nth found 3) name)))
            (display "[usb-audio] claimed audio device") (newline)
            (cons (cons (usb-dev-address dev) ctx) devs)))))

  (define (audio-on-remove addr devs)
    (let loop ((ds devs) (keep '()))
      (cond ((null? ds) keep)
            ((= (caar ds) addr) (send (cdar ds) (list 'stop))
                                (display "[usb-audio] device removed") (newline)
                                (loop (cdr ds) keep))
            (else (loop (cdr ds) (cons (car ds) keep))))))

  ;; init.clp calls (usb-audio-init usb audio) with the coreusb + coreaudio handles.
  ;; Service state is (next-id devs): next-id advances only when a device is actually
  ;; claimed, so card names stay unique without leaking ids on non-audio probes.
  (define (usb-audio-init usb audio)
    (let ((ctx (serve (list 0 '())
                 (lambda (state m)
                   (let ((n (car state)) (devs (cadr state)))
                     (cond
                       ((eq? (car m) 'probe)
                        (let ((nd (audio-on-probe (cadr m) audio devs n)))
                          (if (eq? nd devs) (list n devs) (list (+ n 1) nd))))
                       ((eq? (car m) 'remove) (list n (audio-on-remove (cadr m) devs)))
                       (else state)))))))
      (send usb (list 'register-class USB-CLASS-AUDIO ctx))
      ctx)))
