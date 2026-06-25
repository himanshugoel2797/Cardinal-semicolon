;; coreaudio: the audio service, ported from servers/CoreAudio.
;;
;; The C CoreAudio was a stub -- its module_init only registered its own tests, and
;; the original hdaudio driver never produced a stream, so there was nothing to
;; mix. Now that the Lisp hdaudio driver brings a codec all the way to a playing
;; output stream, this service is the front door to it: a card registers its driver
;; CONTEXT here, and clients ask coreaudio to play through a named card. coreaudio
;; never touches hardware -- it just routes messages to the owning driver context,
;; the same least-privilege posture as the other Core* services.
;;
;; Protocol (send to the service handle returned by start-audio-service):
;;   (register <name> <ctx>)        a driver announces a card + its context
;;   (tone <name>)                  play the card's default bring-up tone
;;   (play <name> <freq> <amp> <frames>)   play a square-wave tone
;;   (cards <reply>)                reply with the list of registered card names
;; Unknown messages are ignored (a wedged client can't crash the service).

(define-module coreaudio
  (export start-audio-service)
  (import driver-util)

  ;; Look up a registered card's context by name; #f if absent. `cards` is an
  ;; association list of (name . ctx).
  (define (card-ctx name cards)
    (cond ((null? cards) #f)
          ((eq? (car (car cards)) name) (cdr (car cards)))
          (else (card-ctx name (cdr cards)))))

  ;; Forward a message to a named card's driver context, if registered.
  (define (to-card name cards msg)
    (let ((c (card-ctx name cards)))
      (if c (send c msg))))

  (define (start-audio-service)
    (serve '()
      (lambda (cards m)
        (cond
          ((eq? (car m) 'register)
           (display "[coreaudio] card registered: ") (display (nth m 1)) (newline)
           (cons (cons (nth m 1) (nth m 2)) cards))
          ((eq? (car m) 'tone)                 ; (tone name)
           (to-card (nth m 1) cards (list 'tone)) cards)
          ((eq? (car m) 'play)                 ; (play name freq amp frames)
           (to-card (nth m 1) cards (list 'play (nth m 2) (nth m 3) (nth m 4))) cards)
          ((eq? (car m) 'cards)                ; (cards reply)
           (send (nth m 1) (map car cards)) cards)
          (else cards))))))
