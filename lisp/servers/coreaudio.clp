;; coreaudio: the audio service, ported from servers/CoreAudio.
;;
;; The C CoreAudio was a stub -- its module_init did nothing but register its own
;; tests. This is the equivalent placeholder: a long-lived service that accepts
;; (register <name>) from a future audio driver and otherwise idles. It exists so
;; the audio endpoint is present (drivers have something to attach to) while the
;; actual mixing/stream plumbing is still TODO, exactly as in the C tree. When a
;; real audio driver (hdaudio) is ported, its stream protocol grows here.

(define-module coreaudio
  (export start-audio-service)
  (import driver-util)

  (define (start-audio-service)
    (serve '()
      (lambda (cards m)
        (cond ((eq? (car m) 'register)
               (display "[coreaudio] card registered: ")
               (display (cadr m)) (newline)
               (cons (cadr m) cards))
              (else cards))))))
