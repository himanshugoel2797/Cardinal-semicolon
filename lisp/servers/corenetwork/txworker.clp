;; corenetwork/txworker: a per-interface transmit engine. register-nic wraps each
;; NIC's raw tx context in one of these and stores IT as the interface's tx, so
;; every eth-tx flows through the engine. The engine keeps at most ONE frame in
;; flight (it forwards a frame to the NIC, then waits for the NIC's tx-done ack
;; before sending the next) and a bounded FIFO of pending frames behind it; frames
;; that arrive when the FIFO is full are DROPPED and counted. This gives real
;; backpressure and bounds tx memory under a burst, and -- because a frame is only
;; handed over after the previous one's completion -- it serialises access to a
;; driver's single TX buffer (no overwrite-in-flight).
;;
;; Acks: the NIC sends (tx-done) once it has finished a frame (for virtio, after
;; the device consumes the descriptor). A NIC that never acks (e.g. a test mock)
;; would stall the one-in-flight gate, so the engine also accepts (tx-tick) from
;; the service's periodic tick and, if a frame has been outstanding for at least a
;; tick with no ack, treats it as done -- so such targets are paced at tick rate
;; rather than wedged. Acking NICs complete well within a tick, so the fallback
;; never double-fires on them.

(define TXE-MAX 64)        ; max frames held behind the in-flight one before dropping

;; Forward (frame len) to the NIC with ourselves as the ack target.
(define (txe-dispatch nic-tx fr) (send nic-tx (list 'tx (car fr) (cadr fr) (self))))

;; Spawn an engine wrapping `nic-tx`; `tick-ns` is the stale threshold for the
;; non-acking fallback. State is threaded through the loop (ring busy disp dropped):
;; `ring` is the pending FIFO, `busy` whether a frame is in flight, `disp` when it
;; was dispatched, `dropped` the overflow counter.
(define (start-tx-engine nic-tx tick-ns)
  (spawn-restricted '()
    (lambda ()
      (let loop ((ring '()) (busy #f) (disp 0) (dropped 0))
        ;; pump: send the next queued frame (if any), else go idle.
        (let ((pump (lambda (rg dr)
                      (if (null? rg)
                          (loop '() #f disp dr)
                          (begin (txe-dispatch nic-tx (car rg))
                                 (loop (cdr rg) #t (uptime-ns) dr))))))
          (let ((m (recv)))
            (cond
              ((eq? (car m) 'tx)
               (if busy
                   (if (< (length ring) TXE-MAX)
                       (loop (append ring (list (list (cadr m) (caddr m)))) #t disp dropped)
                       (loop ring #t disp (+ dropped 1)))        ; FIFO full: drop
                   (begin (txe-dispatch nic-tx (list (cadr m) (caddr m)))
                          (loop ring #t (uptime-ns) dropped))))
              ((eq? (car m) 'tx-done) (pump ring dropped))       ; NIC finished a frame
              ((eq? (car m) 'tx-tick)                            ; non-acking fallback
               (if (and busy (>= (- (uptime-ns) disp) tick-ns))
                   (pump ring dropped)
                   (loop ring busy disp dropped)))
              ((eq? (car m) 'tx-stats)                           ; (tx-stats reply)
               (send (cadr m) (list (length ring) dropped))
               (loop ring busy disp dropped))
              (else (loop ring busy disp dropped)))))))))
