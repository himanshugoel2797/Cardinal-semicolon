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
;;   (register <name> <ctx> <endpoints>)   a driver announces a card, its context,
;;                                          and its endpoint descriptors
;;   (tone <name>)                  play the card's default bring-up tone
;;   (play <name> <freq> <amp> <frames>)   play a square-wave tone
;;   (cards <reply>)                reply with the list of registered card names
;;   (endpoints <name> <reply>)     reply with a card's endpoint descriptors
;;                                  ((id dir dev present) ...), or () if no card
;;   (set-volume <name> <ep-id> <vol>)     set an endpoint's volume (0..100)
;;   (get-volume <name> <ep-id> <reply>)   reply an endpoint's volume, or #f
;;   (mute <name> <ep-id> <on?>)           mute/unmute an endpoint
;;   (capture-start <name> <ep-id>)        begin capturing on an input endpoint
;;   (capture-stop <name>)                 stop capturing
;;   (capture-read <name> <reply>)         reply a bytes copy of the capture ring, or #f
;;   (capture-pos <name> <reply>)          reply the capture DMA position, or #f
;;   (rescan <name>)                       force a codec re-scan (hotplug reconcile)
;;   (poll-jacks <name>)                   force a jack-presence re-poll
;; Unknown messages are ignored (a wedged client can't crash the service).
;;
;; An endpoint descriptor names one jack/converter on a card -- a speaker, a
;; headphone jack, a mic, a line-in -- so a client can see what a card can play to
;; or capture from. coreaudio never touches hardware; it routes messages to the
;; owning driver context, which holds the live codec handle.

(define-module coreaudio
  (export start-audio-service)
  (import driver-util)

  ;; A card record is (name . ctx). `cards` is a list of them. Endpoints are NOT
  ;; cached here -- the driver context is the source of truth (presence is live),
  ;; so the endpoints query forwards to it.
  (define (card-find name cards)
    (cond ((null? cards) #f)
          ((eq? (car (car cards)) name) (car cards))
          (else (card-find name (cdr cards)))))
  (define (card-ctx rec) (cdr rec))

  ;; Forward a message to a named card's driver context, if registered.
  (define (to-card name cards msg)
    (let ((rec (card-find name cards)))
      (if rec (send (card-ctx rec) msg))))

  (define (start-audio-service)
    (serve '()
      (lambda (cards m)
        (cond
          ((eq? (car m) 'register)             ; (register name ctx endpoints)
           (display "[coreaudio] card registered: ") (display (nth m 1))
           (display " (") (display (length (nth m 3))) (display " endpoints)") (newline)
           (cons (cons (nth m 1) (nth m 2)) cards))
          ((eq? (car m) 'tone)                 ; (tone name)
           (to-card (nth m 1) cards (list 'tone)) cards)
          ((eq? (car m) 'play)                 ; (play name freq amp frames)
           (to-card (nth m 1) cards (list 'play (nth m 2) (nth m 3) (nth m 4))) cards)
          ((eq? (car m) 'cards)                ; (cards reply)
           (reply-to (nth m 1) (map car cards)) cards)
          ((eq? (car m) 'endpoints)            ; (endpoints name reply)
           ;; Forward to the driver context for LIVE endpoint state (presence can
           ;; change), rather than answering from the registration-time snapshot.
           ;; Guard the forward: (nth m 2) is the CLIENT's reply handle from an
           ;; untrusted message -- validate it before handing it to the driver.
           (let ((rec (card-find (nth m 1) cards)))
             (if rec
                 (if (ctx? (nth m 2)) (send (card-ctx rec) (list 'endpoints (nth m 2))))
                 (reply-to (nth m 2) '()))) cards)
          ((eq? (car m) 'set-volume)           ; (set-volume name ep-id vol)
           (to-card (nth m 1) cards (list 'set-volume (nth m 2) (nth m 3))) cards)
          ((eq? (car m) 'get-volume)           ; (get-volume name ep-id reply)
           ;; Guard the forward: (nth m 3) is the CLIENT's reply handle from an
           ;; untrusted message -- validate it before handing it to the driver.
           (let ((rec (card-find (nth m 1) cards)))
             (if rec
                 (if (ctx? (nth m 3)) (send (card-ctx rec) (list 'get-volume (nth m 2) (nth m 3))))
                 (reply-to (nth m 3) #f))) cards)
          ((eq? (car m) 'mute)                 ; (mute name ep-id on?)
           (to-card (nth m 1) cards (list 'mute (nth m 2) (nth m 3))) cards)
          ((eq? (car m) 'capture-start)        ; (capture-start name ep-id)
           (to-card (nth m 1) cards (list 'capture-start (nth m 2))) cards)
          ((eq? (car m) 'capture-stop)         ; (capture-stop name)
           (to-card (nth m 1) cards (list 'capture-stop)) cards)
          ((eq? (car m) 'capture-read)         ; (capture-read name reply)
           ;; Guard the forward: (nth m 2) is the CLIENT's reply handle from an
           ;; untrusted message -- validate it before handing it to the driver.
           (let ((rec (card-find (nth m 1) cards)))
             (if rec
                 (if (ctx? (nth m 2)) (send (card-ctx rec) (list 'capture-read (nth m 2))))
                 (reply-to (nth m 2) #f))) cards)
          ((eq? (car m) 'capture-pos)          ; (capture-pos name reply)
           ;; Guard the forward: (nth m 2) is the CLIENT's reply handle from an
           ;; untrusted message -- validate it before handing it to the driver.
           (let ((rec (card-find (nth m 1) cards)))
             (if rec
                 (if (ctx? (nth m 2)) (send (card-ctx rec) (list 'capture-pos (nth m 2))))
                 (reply-to (nth m 2) #f))) cards)
          ((eq? (car m) 'rescan)               ; (rescan name) -- force a codec re-scan
           (to-card (nth m 1) cards (list 'codec-change)) cards)
          ((eq? (car m) 'poll-jacks)           ; (poll-jacks name) -- force a jack re-poll
           (to-card (nth m 1) cards (list 'jack-poll)) cards)
          (else cards))))))
