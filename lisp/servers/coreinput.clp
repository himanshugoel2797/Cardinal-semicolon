;; coreinput: the asynchronous input service, the canonical Cardinal Lisp server.
;;
;; The old C CoreInput (servers/CoreInput) was a device table plus a kernel poll
;; task that drained each device's has_pending/read into an event queue. Here it
;; is a long-lived restricted context owning a device list: input drivers `send`
;; it (register <name>) when they come up and (event <payload>) per input event.
;; There is no synchronous callback ABI, so the rx-handler-re-enters-tx
;; self-deadlock that the C servers had to tiptoe around cannot arise by
;; construction -- a `send` only enqueues.
;;
;; CONSUMERS subscribe: a context that wants the input stream sends
;; (subscribe <ctx>) and thereafter receives every (input <payload>) the service
;; gets. The compositor is the canonical subscriber -- it owns focus and routes
;; each event to the focused window's client (notes/servers/CoreCompositor.md
;; phase 6). The service stays pure mechanism: it neither interprets the payload
;; nor decides routing (that is the compositor's policy); it only fans events out.
;; Forwarding is a plain `send` (enqueue), so a subscriber that re-enters with its
;; own `send` cannot deadlock the service -- same property as the driver side.
;;
;; This is the mold every ported server follows: (import driver-util) for the
;; generic `serve` (spawn a restricted recv-loop threading some state), export a
;; single bring-up entry point, and dispatch messages by their head tag. The
;; service holds NO capabilities (serve grants '()): it only routes messages, so
;; it needs no hardware authority -- least privilege. The POLICY of which driver
;; feeds it lives in `init`, not here; this module is pure mechanism and is reused
;; unchanged by any input source (ps2 today, USB HID later).

(define-module coreinput
  (export start-input-service)
  (import driver-util)

  ;; Spawn the input service; returns the handle drivers `send` to. State is
  ;; (devs . subs): the registered-device list and the subscriber contexts the
  ;; event stream is fanned out to, threaded through the loop by `serve`.
  (define (start-input-service)
    (serve (cons '() '())
      (lambda (st m)
        (let ((devs (car st)) (subs (cdr st)))
          ;; Every arm reads (cadr m); guard arity up front so a truncated message
          ;; (e.g. a bare '(event) or a non-pair) falls through rather than aborting
          ;; on (car '())/(cadr '()) -- coreinput is the only path PS2/USB keys reach
          ;; the compositor, and the serve loop has no try/catch (killing it silences
          ;; all input for the session).
          (cond ((or (not (pair? m)) (not (pair? (cdr m)))) st)
                ((eq? (car m) 'register)
                 (display "[coreinput] device registered: ")
                 (display (cadr m)) (newline)
                 (cons (cons (cadr m) devs) subs))
                ;; (subscribe <ctx>): add a consumer of the event stream. ctx?-guard
                ;; it -- a non-context would abort the later forwarding `send`, and
                ;; the serve loop has no try/catch.
                ((eq? (car m) 'subscribe)
                 (if (ctx? (cadr m))
                     (cons devs (cons (cadr m) subs))
                     st))
                ;; (event <payload>): fan the event out to every subscriber tagged
                ;; (input <payload>), the envelope the compositor demuxes. Pure
                ;; relay -- the payload is opaque here.
                ((eq? (car m) 'event)
                 (for-each (lambda (s) (send s (list 'input (cadr m)))) subs)
                 st)
                (else st)))))))
